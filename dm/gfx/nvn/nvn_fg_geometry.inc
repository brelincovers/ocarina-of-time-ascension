#include <float.h>
// Include after attr_read/ubo_float and before draw_. This is an admission proof
enum { NVN_COVER_TRIANGLES = 4 };   // NVNdrawPrimitive (nvn_cover.h, removed upstream)
// for the captured terminal quad, not a replacement vertex renderer. An unknown
// primitive, pending GPU-written input or transform retains ordinary rendering.
static int fg_geometry_unavailable(unsigned reason, const char *why) {
    static uint32_t reported;
    if (reason < 32 && !(reported & (1u << reason))) {
        reported |= 1u << reason;
        MLOG("frame generation: terminal geometry not matched (%s); genuine frame retained", why);
    }
    return 0;
}

static int fg_terminal_geometry(NvProg *pr, const p_draw *dw, const p_draw_idx *di, int *flip) {
    if (!pr || !pr->sh[0] || strncmp(pr->sh[0]->key, "a5b1bd74e47458d7", 16) ||
        pr->sh[2] || pr->sh[3] || pr->sh[4])
        return fg_geometry_unavailable(0, "different vertex or geometry shader");
    if ((!dw && !di) || (dw && di)) return fg_geometry_unavailable(1, "no ordinary draw");
    int n = dw ? dw->count : di->count, prim = dw ? dw->prim : di->prim;
    int instances = dw ? dw->instances : di->instances;
    if (n != 6 || prim != NVN_COVER_TRIANGLES || instances < 0 || instances > 1 ||
        (dw ? (dw->ind || dw->mdi_cnt) : (di->ind || di->mdi_cnt)))
        return fg_geometry_unavailable(2, "expected one direct two-triangle quad");
    if (S.rast_discard || S.poly.mode != NVN_POLYGON_MODE_FILL || S.ms.a2c || atest_on() ||
        (S.chmask.mask & 15u) != 15u ||
        (R.rt.depth && ((S.ds.dt && S.ds.df != NVN_DEPTH_FUNC_ALWAYS) || S.ds.st)))
        return fg_geometry_unavailable(3, "partial coverage, alpha or depth/stencil state");
    uint32_t sw; memcpy(&sw, S.vp_sw[0], sizeof sw);
    if (sw != 0 && sw != 0x100) return fg_geometry_unavailable(4, "unsupported viewport swizzle");
    float input[64][4], rows[4][4], position[6][4];
    if (attr_read(0, dw, di, input)) return fg_geometry_unavailable(5, "vertex/index input unavailable or pending");
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++)
        if (!ubo_float(0, ((r + 2) << 2) | c, &rows[r][c]) || !isfinite(rows[r][c]))
            return fg_geometry_unavailable(6, "vertex transform unavailable or pending");
    const int raster_flip = atomic_load_explicit(&g_origin_ll, memory_order_relaxed) ^ (sw == 0x100);
    int yflip = -1, corner[6]; unsigned all = 0;
    for (int k = 0; k < 6; k++) {
        const float *v = input[k];
        if (!isfinite(v[0]) || !isfinite(v[1]) || !isfinite(v[2]) ||
            fabsf(v[0]) < FLT_MIN || fabsf(v[1]) < FLT_MIN)
            return fg_geometry_unavailable(7, "non-corner or non-finite vertex");
        // Exact source order of a5b1bd74e47458d7's four position components.
        for (int r = 0; r < 4; r++) {
            position[k][r] = fmaf(v[2], rows[r][2], fmaf(v[0], rows[r][0], v[1] * rows[r][1])) + rows[r][3];
            if (!isfinite(position[k][r])) return fg_geometry_unavailable(8, "non-finite transformed vertex");
        }
        float w = position[k][3];
        if (!(w > 0) || (k && w != position[0][3]))
            return fg_geometry_unavailable(9, "perspective transform");
        float x = position[k][0] / w, y = position[k][1] / w, z = position[k][2] / w;
        if ((x != -1 && x != 1) || (y != -1 && y != 1))
            return fg_geometry_unavailable(10, "quad does not reach the viewport corners");
        if (!atomic_load_explicit(&g_depth_zero, memory_order_relaxed)) z = fmaf(z, 0.5f, 0.5f);
        if (z < 0 || z > 1) return fg_geometry_unavailable(11, "terminal quad is depth clipped");
        float screen_u = (x + 1) * 0.5f, screen_v = (1 - y) * 0.5f;
        if (raster_flip) screen_v = 1 - screen_v;
        // This shader derives UV from the sign of its attribute, not from the
        // matrix-transformed position. Zero inputs were excluded above.
        float u = v[0] > 0 ? 1 : 0, vv = v[1] > 0 ? 0 : 1;
        if (u != screen_u) return fg_geometry_unavailable(12, "horizontal texture transform");
        int fy = vv == screen_v ? 0 : vv == 1 - screen_v ? 1 : -1;
        if (fy < 0 || (yflip >= 0 && yflip != fy))
            return fg_geometry_unavailable(13, "inconsistent vertical texture transform");
        yflip = fy; corner[k] = (x > 0 ? 1 : 0) | (y > 0 ? 2 : 0); all |= 1u << corner[k];
    }
    if (all != 15) return fg_geometry_unavailable(14, "quad misses a viewport corner");
    unsigned triangle[2] = {0, 0};
    int front_ccw = (S.poly.front == NVN_FRONT_FACE_CCW) ^ g_queue_origin_ll ^ raster_flip;
    for (int t = 0; t < 2; t++) {
        int a=corner[3*t], b=corner[3*t+1], c=corner[3*t+2];
        if (a==b || a==c || b==c) return fg_geometry_unavailable(15, "degenerate terminal triangle");
        triangle[t]=(1u<<a)|(1u<<b)|(1u<<c);
        int ax=a&1, ay=a>>1, bx=b&1, by=b>>1, cx=c&1, cy=c>>1;
        int cross=(bx-ax)*(cy-ay)-(by-ay)*(cx-ax), front=front_ccw ? cross>0 : cross<0;
        if (S.poly.cull==NVN_FACE_FRONT_AND_BACK || (S.poly.cull==NVN_FACE_FRONT && front) ||
            (S.poly.cull==NVN_FACE_BACK && !front))
            return fg_geometry_unavailable(16, "terminal triangle is culled");
    }
    // Two distinct halves must share the diagonal, not a side. Merely seeing
    // four corners permits overlapping triangles with an uncovered quarter.
    unsigned shared=triangle[0]&triangle[1];
    if (shared != ((1u<<0)|(1u<<3)) && shared != ((1u<<1)|(1u<<2)))
        return fg_geometry_unavailable(17, "terminal triangles do not cover the rectangle");
    *flip=yflip;
    return 1;
}
