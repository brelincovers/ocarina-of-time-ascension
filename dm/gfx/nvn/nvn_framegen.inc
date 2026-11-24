// ---------- scratch (2026-11-10): frame generation for Ocarina of Time, DM_FRAMEGEN=1. An evaluation build, not for main as it is. ----------
// A game held at 30 fps shown at 60: MetalFX's frame interpolator makes the frame between the previous scene and this one, from the scene before the HUD (the upscaler's
// input, 1440x810 linear), the scene depth and motion vectors reprojected from the game's camera block (zero where the depth says a pixel is not still scenery).
// The generated scene is upscaled by the same MetalFX spatial scaler (a second run), and the HUD is not interpolated: at the present, a pixel of the canvas that changed
// after the upscale (the HUD passes) keeps the real frame's picture, every other pixel takes the generated scene. The present thread (DM_PRESENT_LINK=0) shows the
// generated frame, then the real one, each for half the present's period; generated frames pause while frames come late (as seedds/nxnative#2241's FG_STEADY).
// Scratch limits: OoT only (the camera block is vertex uniform 0 of 13,568 bytes, the scene camera the binding with far plane 25000; the canvas is presented 1:1 inside the
// window texture's crop); DM_UPSCALER=metalfx, DM_NVN_ENC_THREAD=0 and DM_PRESENT_LINK=0 only; the constants belong in a profile if this goes further.
// Pacing and accounting borrow from seedds/nxnative#2241 (pause while late, own present allocators) and from framegen-stage1-v4.patch (the held frame's presented time shifted
// back by its hold; the first engagement delayed past boot).
// Switches: DM_FRAMEGEN=0|1 (0 still logs the shown-frame statistics, for comparison); DM_FRAMEGEN_PAUSE=0 shows every generated frame, late or not;
// DM_FRAMEGEN_VIEW=1 the generated scene without the HUD split, 2 = HUD pixels in magenta; DM_FRAMEGEN_FLIP=1 flips the canvas rows against the window's;
// DM_FRAMEGEN_SHOT=<dir>:<a>-<b> saves the generated and the real picture of frames a..b (slow: it waits for the GPU).
#if FG_PART == 0
extern int dm_focus_now(void);
extern int front_hdr_active(void);
#include "nvn_fg_policy.h"
extern int front_framegen_timing(double *, double *);
extern int front_framegen_timing_epoch(double *, double *, uint64_t *);
static _Atomic uint64_t g_fg_presented, g_fg_accepted, g_fg_budget_skips;
static _Atomic uint64_t g_fg_present_ns, g_fg_interval_ns;
static _Atomic int g_fg_queue_depth;
static _Atomic uint64_t g_fg_outcomes[DM_FG_OUTCOME_COUNT], g_fg_attempts[128];
static _Atomic uint64_t g_fg_generated_frame[128];
static _Atomic int g_fg_fallback;
static uint64_t g_fg_reserved_epoch, g_fg_reserved_real_ns; // g_pq_mu; protects pair order before presented callbacks
enum { FG_COST_INTERPOLATION, FG_COST_RECONSTRUCTION, FG_COST_COMPOSITION, FG_COST_PRESENTATION, FG_COST_COUNT };
static dm_fg_cost g_fg_cost[FG_COST_COUNT]; // g_pq_mu; unmeasured scopes remain zero
static void fg_cost_note(unsigned scope, uint64_t attempt, double start, double end) {
    if (scope>=FG_COST_COUNT || !isfinite(start) || !isfinite(end) || !(start>0 && end>start) || end>(double)UINT64_MAX/1e9) return;
    pthread_mutex_lock(&g_pq_mu);
    dm_fg_cost_record(&g_fg_cost[scope],attempt,(uint64_t)(start*1e9),(uint64_t)(end*1e9));
    pthread_mutex_unlock(&g_pq_mu);
}
static void fg_finish(uint64_t id, dm_fg_outcome outcome) {
    if (dm_fg_finish_once(&g_fg_attempts[id % 128], id, outcome)) atomic_fetch_add(&g_fg_outcomes[outcome], 1);
}
uint64_t mtl_generated_frame_count(void) { return atomic_load(&g_fg_presented); }
static _Atomic int g_fg_mode = -1;   // -1 unread, 0 off, 1 on
const char *mtl_framegen_status(void) {
    if (atomic_load(&g_fg_mode) < 0) return "Awaiting renderer";
    if (atomic_load(&g_fg_fallback) == 101) return "Inactive: expanded aspect camera metadata not verified";
    if (!atomic_load(&g_fg_mode)) return "Off or unavailable; see requested setting and log";
    switch (atomic_load(&g_fg_fallback)) {
        case DM_FG_NO_CAPABILITY: return "Inactive: requires a 30 FPS target";
        case DM_FG_NO_CADENCE: return "Inactive: waiting for display timing";
        case DM_FG_NO_BASE_TIMING: case DM_FG_STALE_TIMING: return "Inactive: waiting for completed GPU timing";
        case DM_FG_GPU_BUDGET: return "Inactive: insufficient GPU headroom";
        case DM_FG_QUEUE_BUSY: return "Inactive: presentation queue busy";
        case DM_FG_NO_DEADLINE: case DM_FG_MISSED_DEADLINE: return "Inactive: no timely display opportunity";
        case 100: return "Inactive: HDR composition contract not verified";
        default: return "Eligible; activity requires actual generated presentations";
    }
}
static int fg_on(void); static void fg_cam_note(const float *c); static void fg_depth_note(MTL4RenderPassDescriptor *pd);
static void fg_shown(int gen, double tt, double tin);
static void fg_hdr_note(const char *key, int geometry_ok, int flip);
#elif FG_PART == 1
#include "nvn_framegen_color.h"
#define FG_WIN 3
static _Atomic int g_fg_seen;
static int g_fg_view, g_fg_flip, g_fg_pause = 1;
static int g_fg_upto = 9;   // DM_FRAMEGEN_UPTO=<n>, to split the cost in play: 0 nothing drawn, 1 the three copies, 2 + motion pass, 3 + interpolation, 4 + second upscale, 5 + window picture (not shown), 9 all
static float g_fg_soft;   // the real frame's upscale blends this share of a bilinear picture in (up_mfx_encode's soft); the generated frame's does the same (fg_comp)
static _Atomic int g_fg_hooked;   // the hook was reached: an upscale of the game's was drawn by MetalFX (up_mfx_encode_from -> fg_frame)
static struct {
    id<MTL4FXFrameInterpolator> fi; NSUInteger iw, ih, ow, oh; MTLPixelFormat cvf; int bad;
    id<MTLTexture> c[2], d[2], mv, gen, gen_up, cv0, cv, depth_seen[4], win[FG_WIN];
    uint64_t win_serial[FG_WIN], win_ticket[FG_WIN], win_next, win_id[FG_WIN];
    dm_fg_slot slot[FG_WIN]; double win_deadline[FG_WIN];
    dm_fg_pair pair, win_pair[FG_WIN];
    uint64_t attempt; double deadline; int reserved_slot;
    id<MTLTexture> match_cv; NSUInteger match_w, match_h;
    float hdr_rows[12]; long hdr_frame; uint64_t hdr_target;
    int hdr_matched, hdr_flip;
    id<MTL4CommandAllocator> walloc[FG_WIN]; id<MTL4CommandBuffer> wcb[FG_WIN]; id<MTLBuffer> wpar[FG_WIN];
    id<MTL4ArgumentTable> margs, wargs; id<MTLRenderPipelineState> mv_pso, comp_pso; MTLPixelFormat comp_fmt; id<MTLFunction> vf, f_mv, f_comp;
    int cur, warm, prev_ok, gen_ready; long last_frame, last_encoded_frame, cam_frame, depth_frame[4]; float cam[96];
    _Atomic uint64_t made, resets, nocam, nodepth, busy, late, behind, settling;
} FG;
static const char *g_fg_shot_dir; static long g_fg_shot_a = -1, g_fg_shot_b = -1;
static int fg_on(void) {
    if (g_fg_mode >= 0) return g_fg_mode;
    const char *e = getenv("DM_FRAMEGEN"), *x; int v = e && atoi(e) > 0; g_fg_seen = e != NULL;
    if ((x = getenv("DM_FRAMEGEN_VIEW"))) g_fg_view = atoi(x);
    if ((x = getenv("DM_FRAMEGEN_FLIP"))) g_fg_flip = atoi(x);
    if ((x = getenv("DM_FRAMEGEN_PAUSE"))) g_fg_pause = atoi(x);
    if ((x = getenv("DM_FRAMEGEN_UPTO"))) g_fg_upto = atoi(x);
    if ((x = getenv("DM_FRAMEGEN_SHOT"))) { const char *c = strrchr(x, ':'); if (c && sscanf(c + 1, "%ld-%ld", &g_fg_shot_a, &g_fg_shot_b) == 2) g_fg_shot_dir = strndup(x, (size_t)(c - x)); }
    FG.last_frame = FG.last_encoded_frame = -2; FG.cam_frame = -1;
    // loader.c publishes the actual rounded guest output size before rendering.
    // The tall-screen patch changes projection, but the captured scalar camera
    // metadata has not yet been proved equivalent. Do not allocate/encode FG.
    uint64_t shape = atomic_load(&g_screen_shape);
    if (v && shape && !dm_fg_camera_shape_verified((uint32_t)shape, shape >> 32)) {
        atomic_store(&g_fg_fallback, 101); v = 0;
        MLOG("frame generation inactive: expanded aspect camera metadata not verified; original HDR/SDR frames retained");
    }
    if (v && !(g_q4 && [MTLFXFrameInterpolatorDescriptor supportsMetal4FX:g_dev])) { MLOG("[RED] frame generation (DM_FRAMEGEN): this GPU has no Metal 4 MetalFX frame interpolator; frames are shown as the game draws them"); v = 0; }
    if (v && (strcmp(DM_GAME_ID, "0400486024DEC000") ||
              strcmp(dm_main_build(), "e0f3ccfaf50065ae4ea01eb5aeb13b1a"))) v = 0;
    if (v && g_dm_present_link) {
        MLOG("frame generation inactive: classic presentation is required");
        v = 0;
    }
    if (v && rec_on()) { MLOG("[RED] frame generation (scratch build): not with DM_NVN_ENC_THREAD=1; frames are shown as the game draws them"); v = 0; }
    if (v) MLOG("frame generation (DM_FRAMEGEN=1, scratch build): the scene before the HUD is interpolated from depth and the camera, upscaled a second time, and the HUD is kept from the real frame; "
                "pause while late %d, view %d, flip %d, stages up to %d", g_fg_pause, g_fg_view, g_fg_flip, g_fg_upto);
    else if (g_fg_seen) MLOG("frame generation %s: the shown-frame statistics are still logged, for comparison",
                              e && atoi(e) > 0 ? "requested On but inactive" : "off (DM_FRAMEGEN=0)");
    return g_fg_mode = v;
}
// The camera block as the game binds it (encode_args_): the scene camera's binding has far plane 25000; the environment probe's (far 10000, one cube face a frame) is not taken
static void fg_cam_note(const float *c) {
    if (!fg_on()) return;
    if (!c) { FG.cam_frame=-1; return; } // the currently bound camera cannot be read safely
    if (isfinite(c[57]) && c[57]>0 && c[57]<=20000.0f) return; // a readable environment-probe camera is not the scene camera
    FG.cam_frame=-1; // a later malformed scene binding must not leave an earlier camera valid this frame
    if (!(c[56]>0 && c[57]>20000.0f && c[57]>c[56] && c[62]>0 && c[70]>0 && c[70]<3.141593f)) return;
    for (int i=12;i<44;i++) if (!isfinite(c[i])) return;
    for (int i=84;i<96;i++) if (!isfinite(c[i])) return;
    if (!isfinite(c[56]) || !isfinite(c[57]) || !isfinite(c[62]) || !isfinite(c[70])) return;
    memcpy(FG.cam,c,sizeof FG.cam); FG.cam_frame=(long)atomic_load(&g_cur_frame);
}
// Each render pass (td_note): the last few D32F depth attachments, newest first. The scene depth is the newest of them that holds the scene being upscaled
// (fg_frame); with dynamic resolution the scene is narrower than a shadow map, so "the last large one" is not it
static void fg_depth_note(MTL4RenderPassDescriptor *pd) {
    if (!g_fg_mode || !pd || !fg_on()) return;
    id<MTLTexture> d = pd.depthAttachment.texture;
    if (!d || d.pixelFormat != MTLPixelFormatDepth32Float || d.textureType != MTLTextureType2D) return;
    long frame=(long)atomic_load(&g_cur_frame);
    if (d==FG.depth_seen[0]) { FG.depth_frame[0]=frame; return; }
    int n = 3; for (int i = 1; i < 4; i++) if (FG.depth_seen[i] == d) { n = i; break; }
    for (int i = n; i > 0; i--) { FG.depth_seen[i] = FG.depth_seen[i - 1]; FG.depth_frame[i]=FG.depth_frame[i-1]; }
    FG.depth_seen[0] = d; FG.depth_frame[0]=frame;
}
static int fg_inv4(const double a_[16], double r[16]) {
    double a[16]; memcpy(a, a_, sizeof a); for (int i = 0; i < 16; i++) r[i] = i % 5 == 0;
    for (int c = 0; c < 4; c++) { int p = c; for (int k = c + 1; k < 4; k++) if (fabs(a[k * 4 + c]) > fabs(a[p * 4 + c])) p = k;
        if (fabs(a[p * 4 + c]) < 1e-12) return 0;
        if (p != c) for (int k = 0; k < 4; k++) { double t = a[c * 4 + k]; a[c * 4 + k] = a[p * 4 + k]; a[p * 4 + k] = t; t = r[c * 4 + k]; r[c * 4 + k] = r[p * 4 + k]; r[p * 4 + k] = t; }
        double s = 1.0 / a[c * 4 + c]; for (int k = 0; k < 4; k++) { a[c * 4 + k] *= s; r[c * 4 + k] *= s; }
        for (int q = 0; q < 4; q++) if (q != c) { double f = a[q * 4 + c]; if (f != 0) for (int k = 0; k < 4; k++) { a[q * 4 + k] -= f * a[c * 4 + k]; r[q * 4 + k] -= f * r[c * 4 + k]; } } }
    return 1;
}
static void fg_mul4(const double a[16], const double b[16], double r[16]) { for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { double s = 0; for (int k = 0; k < 4; k++) s += a[i * 4 + k] * b[k * 4 + j]; r[i * 4 + j] = s; } }
// Capture only the known terminal HDR pass, including the bound canvas and
// immutable matrix of this frame. Unknown postprocessing retains the real picture.
static void fg_hdr_note(const char *key, int geometry_ok, int flip) {
    if (g_fg_mode <= 0 || !FG.match_cv || !R.has_rt || !R.rt.n || !R.rt.color[0]) return;
    uint64_t target = R.rt.color[0]->serial;
    if (strncmp(key, "a4200b53be47212c", 16)) {
        if (FG.hdr_matched && target == FG.hdr_target) FG.hdr_matched = 0;
        return;
    }
    FG.hdr_matched = 0;
    if (!geometry_ok) return;
    if (g_pass.cfmt[0] != MTLPixelFormatRGB10A2Unorm || R.rt.n != 1 ||
        (S.color.blend_en & 1u) || S.color.logic_op != NVN_LOGIC_OP_COPY) return;
    int tid = (int)(tex_slot_h(S.tex[1], S.img[1], 0) & 0xfffff);
    os_unfair_lock_lock(&g_rmu);
    id<MTLTexture> sampled = tid > 0 && tid < g_ntreg && g_treg[tid].ok ? tex_by_id(tid) : nil;
    int sid = (int)(S.tex[1][0] >> 20);
    int sampler_ok = sid > 0 && sid < g_nsreg && g_sreg_ok[sid];
    samp_par sampler = sampler_ok ? g_sreg[sid] : (samp_par){0};
    os_unfair_lock_unlock(&g_rmu);
    if (!sampler_ok || sampler.cmp_mode || sampler.reduction ||
        (sampled.mipmapLevelCount > 1 && sampler.minf >= 2 && (sampler.lod_min != 0 || sampler.lod_max != 0))) return;
    id<MTLTexture> base = sampled.parentTexture ?: sampled, canvas = FG.match_cv.parentTexture ?: FG.match_cv;
    if (!sampled || base != canvas || sampled.width != FG.match_w || sampled.height != FG.match_h ||
        sampled.parentRelativeLevel != FG.match_cv.parentRelativeLevel || sampled.parentRelativeSlice != FG.match_cv.parentRelativeSlice ||
        sampled.pixelFormat != FG.match_cv.pixelFormat) return;
    if (!S.has_vp && (g_pass.tw != FG.match_w || g_pass.th != FG.match_h)) return;
    MTLTextureSwizzleChannels swz = sampled.swizzle;
    if (swz.red != MTLTextureSwizzleRed || swz.green != MTLTextureSwizzleGreen || swz.blue != MTLTextureSwizzleBlue) return;
    if (S.has_vp && (S.vp[0][0] != 0 || S.vp[0][1] != 0 || S.vp[0][2] != FG.match_w || S.vp[0][3] != FG.match_h)) return;
    if (R.has_sc && (R.sc.x != 0 || R.sc.y != 0 || R.sc.w != FG.match_w || R.sc.h != FG.match_h)) return;
    float rows[12] = {0};
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++)
        if (!ubo_float(1, ((18+r) << 2) | c, &rows[4*r+c]) || !isfinite(rows[4*r+c])) return;
    memcpy(FG.hdr_rows, rows, sizeof rows); FG.hdr_frame = (long)atomic_load(&g_cur_frame);
    FG.hdr_target = target; FG.hdr_matched = 1; FG.hdr_flip = flip;
    static int once; if (!once++) MLOG("frame generation: matched native HDR terminal a4200b53be47212c, canvas %lux%lu, exact guest matrix + PQ; no additional tone map",
                                     (unsigned long)FG.match_w, (unsigned long)FG.match_h);
}
static NSString *const FG_MSL = @"#include <metal_stdlib>\nusing namespace metal;\n"
    "struct V { float4 p [[position]]; };\n"
    "vertex V fg_vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); V o; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
    "struct MP { float4x4 reproj; float2 size; float2 inv_size; };\n"
    "static float vz(float d) { return 1.0 / max(1.0 - d, 1e-7); }\n"
    // Motion in pixels toward the previous position: this pixel's depth reprojected through the previous view-projection; zero where the previous depth there disagrees (a moving
    // object, or a disocclusion): the test of MacNativeGames' mfx_mv (MIT), as the offline test used it
    "fragment float4 fg_mv(V in [[stage_in]], depth2d<float> d [[texture(0)]], depth2d<float> pd [[texture(1)]], constant MP *p [[buffer(0)]]) {\n"
    "  uint2 g = uint2(in.p.xy); float2 uv = (float2(g) + 0.5) * p->inv_size; float d0 = d.read(g); float2 m = 0.0; int still = -1;\n"
    "  float4 c = p->reproj * float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, d0 * 2.0 - 1.0, 1.0);\n"
    "  if (c.w > 1e-7) { float2 n = c.xy / c.w; float2 prev = float2(n.x * 0.5 + 0.5, 0.5 - 0.5 * n.y); m = (prev - uv) * p->size; int2 qq = int2(floor(prev * p->size));\n"
    "    if (d0 < 1.0 && all(qq >= int2(1)) && all(qq < int2(p->size) - int2(1))) { float ze = vz(c.z / c.w * 0.5 + 0.5), zmin = 3.4e38, zmax = 0.0; bool nr = false;\n"
    "      for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) { float z = vz(pd.read(uint2(qq + int2(x, y)))); zmin = min(zmin, z); zmax = max(zmax, z); nr = nr || fabs(z - ze) <= ze * 0.01; }\n"
    "      still = (zmax <= zmin * 1.05 ? (ze >= zmin * 0.99 && ze <= zmax * 1.01) : nr) ? 1 : 0; } }\n"
    "  if (still == 0) m = 0.0; if (any(isnan(m)) || any(isinf(m))) m = 0.0; return float4(m, 0.0, 0.0); }\n"
    FG_HDR_COLOR_MSL
    "struct CP { int2 org; int2 csize; int flip; int view; int encode; float soft; float4 hdr_rows[3]; };\n"
    "static float3 oetf(float3 c) { c = saturate(c); return mix(12.92 * c, 1.055 * pow(c, float3(1.0 / 2.4)) - 0.055, step(float3(0.0031308), c)); }\n"
    // The generated frame's window picture: win = the real frame's window texture (encoded bytes), gen = the generated scene upscaled (linear), cv = the canvas now (with the HUD),
    // cv0 = the canvas right after the real frame's upscale. A canvas pixel that changed is HUD: it keeps the real frame's picture. Any other takes the real picture plus the
    // difference between the generated and the real scene, both encoded, so a still scene comes out exactly as the real frame whatever the game's last pass does to the colour
    "fragment float4 fg_comp(V in [[stage_in]], texture2d<float> win [[texture(0)]], texture2d<float> gen [[texture(1)]], texture2d<float> cv [[texture(2)]], texture2d<float> cv0 [[texture(3)]], texture2d<float> gs [[texture(4)]], constant CP *p [[buffer(0)]]) {\n"
    "  uint2 g = uint2(in.p.xy); float4 w = win.read(g); int2 q = int2(g) - p->org; if (p->flip != 0) q.y = p->csize.y - 1 - q.y;\n"
    "  if (any(q < int2(0)) || any(q >= p->csize)) return w;\n"
    "  float3 a = cv.read(uint2(q)).rgb; float3 b = cv0.read(uint2(q)).rgb; bool hud = any(a != b);\n"
    "  if (p->view == 2 && hud) return float4(1.0, 0.0, 1.0, 1.0);\n"
    "  if (hud && p->view != 1) return w;\n"
    // soft: the share of a bilinear picture of the scaler's own input in the real frame's upscale (the profile's upscaler_sharpness, seedds/nxnative#2366): the generated
    // scene (gs, before its upscale) is blended in the same way, or generated frames would be crisper than real ones
    "  float3 c = gen.read(uint2(q)).rgb;\n"
    "  if (p->soft > 0.0) { constexpr sampler ls(coord::normalized, address::clamp_to_edge, filter::linear); c = mix(c, gs.sample(ls, (float2(q) + 0.5) / float2(p->csize)).rgb, p->soft); }\n"
    "  if (p->encode == 2) { if (all(c == b)) return w; return float4(fghdr_encode(c, p->hdr_rows), w.a); }\n"
    "  if (p->encode == 0) return float4(c, w.a);\n"
    "  if (p->view == 1) return float4(oetf(c), w.a);\n"
    "  return float4(saturate(w.rgb + oetf(c) - oetf(b)), w.a); }\n";
static int fg_pipes(void) {
    static int st; if (st) return st > 0;
    st = -1; NSError *e = nil; uint64_t t0 = now_ns();
    MTLCompileOptions *options = [MTLCompileOptions new]; options.mathMode = MTLMathModeSafe;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:FG_MSL options:options error:&e];
    if (!lib) { MLOG("[RED] frame generation: its shaders do not compile: %s", e.localizedDescription.UTF8String); return 0; }
    FG.vf = [lib newFunctionWithName:@"fg_vs"]; FG.f_mv = [lib newFunctionWithName:@"fg_mv"]; FG.f_comp = [lib newFunctionWithName:@"fg_comp"];
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new]; pd.vertexFunction = FG.vf; pd.fragmentFunction = FG.f_mv; pd.colorAttachments[0].pixelFormat = MTLPixelFormatRG16Float;
    FG.mv_pso = FG.vf && FG.f_mv ? [g_dev newRenderPipelineStateWithDescriptor:pd error:&e] : nil;
    MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxTextureBindCount = 5; ad.maxBufferBindCount = 1;
    FG.margs = rec_table([g_dev newArgumentTableWithDescriptor:ad error:&e]); FG.wargs = [g_dev newArgumentTableWithDescriptor:ad error:&e];
    if (!FG.mv_pso || !FG.f_comp || !FG.margs || !FG.wargs) { MLOG("[RED] frame generation: no pipeline or argument table: %s", e.localizedDescription.UTF8String); return 0; }
    MLOG("frame generation: shaders compiled in %.1f ms", (now_ns() - t0) / 1e6);
    st = 1; return 1;
}
static id<MTLTexture> fg_tex(MTLPixelFormat f, NSUInteger w, NSUInteger h, MTLTextureUsage u, NSString *label) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:w height:h mipmapped:NO]; td.storageMode = MTLStorageModePrivate; td.usage = u;
    id<MTLTexture> t = [g_dev newTextureWithDescriptor:td]; if (t) { t.label = label; res_add(t); } return t;
}
// The interpolator and its textures for a scene of iw x ih upscaled to ow x oh (made again when a size changes: dynamic resolution)
static int fg_setup(NSUInteger iw, NSUInteger ih, NSUInteger ow, NSUInteger oh, MTLPixelFormat cvf, id<MTL4FXSpatialScaler> sc) {
    if (FG.iw == iw && FG.ih == ih && FG.ow == ow && FG.oh == oh && FG.cvf == cvf) return !FG.bad;
    id<MTLTexture> old[] = {FG.c[0], FG.c[1], FG.d[0], FG.d[1], FG.mv, FG.gen, FG.gen_up, FG.cv0};
    for (unsigned i = 0; i < sizeof old / sizeof *old; i++) if (old[i]) { res_remove(old[i]); defer_obj_release(old[i]); }
    FG.c[0] = FG.c[1] = FG.d[0] = FG.d[1] = FG.mv = FG.gen = FG.gen_up = FG.cv0 = nil; FG.fi = nil;
    FG.iw = iw; FG.ih = ih; FG.ow = ow; FG.oh = oh; FG.cvf = cvf; FG.bad = 1; FG.warm = 0; FG.prev_ok = 0; FG.gen_ready = 0; FG.last_frame = FG.last_encoded_frame = -2;
    NSError *err = nil; uint64_t t0 = now_ns();
    if (!g_fx_comp) g_fx_comp = [g_dev newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&err];
    MTLFXFrameInterpolatorDescriptor *d = [MTLFXFrameInterpolatorDescriptor new];
    d.colorTextureFormat = MTLPixelFormatRGBA16Float; d.outputTextureFormat = MTLPixelFormatRGBA16Float; d.depthTextureFormat = MTLPixelFormatDepth32Float; d.motionTextureFormat = MTLPixelFormatRG16Float;
    d.inputWidth = iw; d.inputHeight = ih; d.outputWidth = iw; d.outputHeight = ih;
    id<MTL4FXFrameInterpolator> fi = g_fx_comp ? [d newFrameInterpolatorWithDevice:g_dev compiler:g_fx_comp] : nil;
    if (!fi) { MLOG("[RED] frame generation: MetalFX makes no frame interpolator for %lux%lu RGBA16Float; frames are shown as the game draws them", (unsigned long)iw, (unsigned long)ih); return 0; }
    MTLTextureUsage rd = MTLTextureUsageShaderRead;
    for (int i = 0; i < 2; i++) { FG.c[i] = fg_tex(MTLPixelFormatRGBA16Float, iw, ih, fi.colorTextureUsage | rd, @"framegen colour"); FG.d[i] = fg_tex(MTLPixelFormatDepth32Float, iw, ih, fi.depthTextureUsage | rd, @"framegen depth"); }
    FG.mv = fg_tex(MTLPixelFormatRG16Float, iw, ih, fi.motionTextureUsage | MTLTextureUsageRenderTarget | rd, @"framegen motion");
    FG.gen = fg_tex(MTLPixelFormatRGBA16Float, iw, ih, fi.outputTextureUsage | sc.colorTextureUsage | rd, @"framegen generated scene");
    FG.gen_up = fg_tex(MTLPixelFormatRGBA16Float, ow, oh, sc.outputTextureUsage | rd, @"framegen generated scene, upscaled");
    FG.cv0 = fg_tex(cvf, ow, oh, rd, @"framegen canvas before the HUD");
    res_commit();
    if (!FG.c[0] || !FG.c[1] || !FG.d[0] || !FG.d[1] || !FG.mv || !FG.gen || !FG.gen_up || !FG.cv0) { MLOG("[RED] frame generation: a texture could not be made; frames are shown as the game draws them"); return 0; }
    FG.fi = fi; FG.bad = 0;
    MLOG("frame generation: MetalFX frame interpolator %lux%lu RGBA16Float, generated scene upscaled to %lux%lu (canvas format %lu), made in %.1f ms; about %.0f MB of textures",
         (unsigned long)iw, (unsigned long)ih, (unsigned long)ow, (unsigned long)oh, (unsigned long)cvf, (now_ns() - t0) / 1e6, (iw * ih * (8 * 3 + 4 * 3) + ow * oh * (8 + 4)) / 1048576.0);
    return 1;
}
static void fg_barrier(void) {   // as up_mfx_encode: MetalFX opens encoders of its own, which take no barrier
    id<MTL4ComputeCommandEncoder> ce = [g_cb computeCommandEncoder];
    [ce barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageBlit visibilityOptions:resource_vis()];
    [ce barrierAfterStages:MTLStageBlit beforeQueueStages:MTLStageAll visibilityOptions:resource_vis()];
    [ce endEncoding];
}
// After the game's upscale of this frame (up_mfx_encode): color = the scene, linear RGBA16F (the scaler's input), out = the canvas the upscaled scene was just drawn into
static void fg_frame(id<MTL4FXSpatialScaler> sc, id<MTLTexture> color, id<MTLTexture> out, const NVNcopyRegion *orr, NSUInteger cw, NSUInteger ch) {
    if (!fg_on()) return;
    if (FG.gen_ready) { fg_finish(FG.attempt, DM_FG_REJECTED_UNAVAILABLE); FG.gen_ready = 0; }
    if (dm_focus_now() != 1) { FG.prev_ok = FG.warm = 0; return; }
    FG.hdr_matched = 0;
    atomic_store(&g_fg_hooked, 1);
    long f = (long)atomic_load(&g_cur_frame); NSUInteger ow = (NSUInteger)orr->width, oh = (NSUInteger)orr->height;
    // Defense in depth: an unexpected non-16:9 scene cannot bypass the startup
    // gate through stale/unset screen state. No copies or interpolation precede
    // this check, and returning to the original shape starts fresh history.
    if (!dm_fg_camera_shape_verified(cw, ch) || !dm_fg_camera_shape_verified(ow, oh)) {
        FG.prev_ok = FG.gen_ready = FG.warm = 0;
        FG.last_frame = FG.last_encoded_frame = -2;
        atomic_store(&g_fg_fallback, 101);
        static int once; if (!once++) MLOG("frame generation inactive: non-16:9 scene/canvas has unverified camera metadata; genuine frame retained");
        return;
    }
    if (orr->xoffset || orr->yoffset || ow != out.width || oh != out.height || cw != color.width || ch != color.height) {
        static int once; if (!once++) MLOG("[RED] frame generation: the upscale is not whole textures (%lux%lu of %lux%lu into %lux%lu of %lux%lu); frames are shown as the game draws them",
                                           (unsigned long)cw, (unsigned long)ch, (unsigned long)color.width, (unsigned long)color.height, (unsigned long)ow, (unsigned long)oh, (unsigned long)out.width, (unsigned long)out.height);
        return; }
    FG.match_cv = out; FG.match_w = ow; FG.match_h = oh;
    // Not in the first 300 upscaled frames: its shaders and the interpolator are made at first use, and making them under the boot's compile load is what framegen-stage1-v4
    // blames a crash in the shader compiler's cache on (a base M5). A stopgap as that patch's: the pipelines belong in the prebuilt set if this goes further
    { static int seen; if (seen < 300) { seen++; return; } }
    // Admission precedes texture copies and interpolation. 8ms is an explicitly
    // conservative estimate from the prior paired run, not a measured deadline.
    // A sample already containing this path is compared directly, never charged twice.
    double tick = 0, refresh = 0, now_s = CACurrentMediaTime();
    uint64_t epoch=0;
    int cadence = front_framegen_timing_epoch(&tick, &refresh, &epoch);
    uint64_t now = (uint64_t)(now_s * 1e9), sf = f >= 2 ? (uint64_t)f - 2 : 0;
    uint64_t gs = 0, ge = 0; double gpu = mtl_gpu_ms(sf);
    int valid_gpu = gpu > 0 && mtl_gpu_span(sf, &gs, &ge);
    int generated = atomic_load(&g_fg_generated_frame[sf % 128]) == sf && sf != 0;
    double period = 1.0 / 30.0, extra_ms = generated ? 0 : 8.0;
    double deadline = cadence ? tick + floor((now_s + period - tick) / refresh) * refresh : 0;
    dm_fg_cadence observed={epoch,tick>0?(uint64_t)(tick*1e9):0,refresh>0?(uint64_t)(refresh*1e9):0};
    dm_fg_pair pair={0};
    uint64_t previous_real=atomic_load(&g_fg_present_ns);
    pthread_mutex_lock(&g_pq_mu);
    if (g_fg_reserved_epoch==epoch && g_fg_reserved_real_ns>previous_real) previous_real=g_fg_reserved_real_ns;
    pthread_mutex_unlock(&g_pq_mu);
    cadence = cadence && dm_fg_pair_plan(&observed,now,deadline>0?(uint64_t)(deadline*1e9):0,
                                         previous_real,(uint64_t)(period*1e9),500000,&pair);
    if (cadence) deadline=pair.generated_ns/1e9;
    dm_fg_admission admission = {
        .now_ns=now, .target_period_ns=(uint64_t)(period * 1e9),
        .display_interval_ns=(uint64_t)(refresh * 1e9), .cadence_tolerance_ns=500000,
        .sample_end_ns=valid_gpu ? ge : 0, .max_sample_age_ns=250000000,
        .sample_total_ns=gpu > 0 ? (uint64_t)(gpu * 1e6) : 0,
        .additional_gpu_ns=(uint64_t)(extra_ms * 1e6),
        // Conservative legacy upper bound, NOT measured remaining GPU work.
        // Replace only once phase/queue-frontier measurements populate it.
        .predicted_ready_ns=now + (uint64_t)(fmax(gpu, 0) * 1e6 + extra_ms * 1e6),
        .deadline_ns=deadline > 0 ? (uint64_t)(deadline * 1e9) : 0,
        .queued_real_frames=(unsigned)atomic_load(&g_fg_queue_depth), .max_queued_real_frames=1,
        .capable=mtl_fps_target() == 30, .cadence_valid=cadence, .deadline_valid=cadence,
        .sample_has_generation=generated, .sample_is_same_generated_path=generated,
    };
    dm_fg_admission_reason reason = dm_fg_admit(&admission);
    // Reserve a composition slot before paying the interpolation cost. Its
    // allocator is protected by actual producer/reader completion, not real-frame tickets.
    int k = (int)(FG.win_next % FG_WIN);
    pthread_mutex_lock(&g_pq_mu);
    int reusable = dm_fg_slot_reusable(&FG.slot[k]);
    pthread_mutex_unlock(&g_pq_mu);
    if (reason != DM_FG_ADMIT || !reusable) {
        atomic_store(&g_fg_fallback, reason != DM_FG_ADMIT ? reason : DM_FG_QUEUE_BUSY);
        FG.prev_ok = FG.gen_ready = FG.warm = 0;
        uint64_t skipped = atomic_fetch_add(&g_fg_budget_skips, 1) + 1;
        if (skipped == 1 || skipped % 300 == 0)
            MLOG("frame generation: %llu early skips, reason %d, observed refresh %.3f ms, GPU sample %.2f ms (%s), extra estimate %.2f ms",
                 (unsigned long long)skipped, atomic_load(&g_fg_fallback), refresh * 1000, gpu,
                 generated ? "includes generation" : "baseline", extra_ms);
        return;
    }
    atomic_store(&g_fg_fallback, 0); FG.deadline = deadline; FG.reserved_slot = k; FG.pair=pair;
    if (!fg_pipes() || !fg_setup(cw, ch, ow, oh, out.pixelFormat, sc)) return;
    if (g_fg_upto < 1) return;
    id<MTLTexture> dep = nil;   // the newest recent depth attachment of exactly the scene's size (devMlGUE, seedds/nxnative#2279: with dynamic resolution the scene is 953x536 or 854x480 and the last large one is a shadow map)
    for (int i = 0; i < 4 && !dep; i++) { id<MTLTexture> d = FG.depth_seen[i]; if (d && FG.depth_frame[i]==f && d.width == cw && d.height == ch) dep = d; }
    int have_d = dep != nil, have_cam = FG.cam_frame == f, cur = FG.cur, prev = cur ^ 1;
    if (!have_d) FG.nodepth++; if (!have_cam) FG.nocam++;
    { static NSUInteger lw, lh, ldw; static int n; NSUInteger dw = dep ? dep.width : 0;
      if ((lw != cw || lh != ch || ldw != dw) && n < 40) { n++; lw = cw; lh = ch; ldw = dw; char b[96] = ""; int k = 0;
          for (int i = 0; i < 4; i++) if (FG.depth_seen[i]) k += snprintf(b + k, sizeof b - k, " %lux%lu", (unsigned long)FG.depth_seen[i].width, (unsigned long)FG.depth_seen[i].height);
          MLOG("frame generation: scene %lux%lu, depth used %lux%lu, recent depth attachments:%s", (unsigned long)cw, (unsigned long)ch, (unsigned long)dw, (unsigned long)(dep ? dep.height : 0), b); } }
    MTLOrigin o0 = MTLOriginMake(0, 0, 0);
    id<MTL4ComputeCommandEncoder> b = blit();
    enc_b_copy_texture(b, color, 0, 0, o0, MTLSizeMake(cw, ch, 1), FG.c[cur], 0, 0, o0);
    if (have_d) enc_b_copy_texture(b, dep, 0, 0, o0, MTLSizeMake(cw, ch, 1), FG.d[cur], 0, 0, o0);
    enc_b_copy_texture(b, out, 0, 0, o0, MTLSizeMake(ow, oh, 1), FG.cv0, 0, 0, o0);
    FG.cv = out; FG.gen_ready = 0;
    int ok = have_d && have_cam && FG.prev_ok && FG.last_frame == f - 1; double m[16];
    if (ok) {   // reprojection = projection x previous view (floats 84-95 of this frame's block) x inverse(view-projection)
        double cam[96], ic[16], pvp[16]; for (int i = 0; i < 96; i++) cam[i] = FG.cam[i];
        double pv[16] = {cam[84], cam[85], cam[86], cam[87], cam[88], cam[89], cam[90], cam[91], cam[92], cam[93], cam[94], cam[95], 0, 0, 0, 1};
        if (fg_inv4(&cam[12], ic)) { fg_mul4(&cam[28], pv, pvp); fg_mul4(pvp, ic, m); } else ok = 0;
    }
    if (ok && g_fg_upto >= 2) {
        uint64_t next = atomic_load(&FG.made) + 1;
        if (!dm_fg_attempt_begin(&g_fg_attempts[next % 128], next)) {
            FG.prev_ok = FG.warm = 0; atomic_fetch_add(&g_fg_budget_skips, 1); return;
        }
        FG.attempt = next;
        rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
        id<MTLBuffer> pb; size_t po = arena_alloc(96, &pb); float *p = (float *)((uint8_t *)pb.contents + po);
        for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) p[c * 4 + r] = (float)m[r * 4 + c];
        p[16] = cw; p[17] = ch; p[18] = 1.0f / cw; p[19] = 1.0f / ch;
        MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new];
        rp.colorAttachments[0].texture = FG.mv; rp.colorAttachments[0].loadAction = MTLLoadActionDontCare; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        ser_gate(); id<MTL4RenderCommandEncoder> en = enc_r_begin(rp);
        [en setArgumentTable:FG.margs atStages:MTLRenderStageFragment];
        [FG.margs setTexture:FG.d[cur].gpuResourceID atIndex:0]; [FG.margs setTexture:FG.d[prev].gpuResourceID atIndex:1]; [FG.margs setAddress:pb.gpuAddress + po atIndex:0];
        [en setViewport:(MTLViewport){0, 0, cw, ch, 0, 1}]; [en setScissorRect:(MTLScissorRect){0, 0, cw, ch}];
        [en setRenderPipelineState:FG.mv_pso]; [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; enc_r_end(en);
        id<MTL4FXFrameInterpolator> fi = FG.fi; id<MTLTexture> c1 = FG.c[cur], c0 = FG.c[prev], d1 = FG.d[cur], mv = FG.mv, gen = FG.gen, gup = FG.gen_up;
        int reset = FG.warm == 0 || dm_fg_history_reset((uint64_t)f,(uint64_t)FG.last_frame,(uint64_t)FG.last_encoded_frame,FG.prev_ok);
        float nearp = FG.cam[56], farp = FG.cam[57], fov = FG.cam[70] * 57.29578f, aspect = FG.cam[62];
        if (g_fg_upto >= 3) {
        fg_barrier();
        rec_cb_block(g_cb, ^(id<MTL4CommandBuffer> cb) {
            fi.colorTexture = c1; fi.prevColorTexture = c0; fi.depthTexture = d1; fi.motionTexture = mv; fi.outputTexture = gen;
            fi.motionVectorScaleX = 1; fi.motionVectorScaleY = 1; fi.jitterOffsetX = 0; fi.jitterOffsetY = 0; fi.deltaTime = 1.0f / 30;
            fi.nearPlane = nearp; fi.farPlane = farp; fi.fieldOfView = fov; fi.aspectRatio = aspect; fi.depthReversed = NO; fi.shouldResetHistory = reset;
            [fi encodeToCommandBuffer:cb];
        });
        FG.last_encoded_frame=f;
        }
        if (g_fg_upto >= 4) {
        fg_barrier();
        rec_cb_block(g_cb, ^(id<MTL4CommandBuffer> cb) {   // the generated scene through the game's own upscale, a second time
            sc.colorTexture = gen; sc.outputTexture = gup; sc.inputContentWidth = cw; sc.inputContentHeight = ch;
            [sc encodeToCommandBuffer:cb];
        });
        }
        g_last_end_relaxed = 0; args_dirty_all(); g_pso_dirty = g_dss_dirty = 1;
        atomic_fetch_add(&FG.made, 1);
        atomic_store(&g_fg_generated_frame[(uint64_t)f % 128], (uint64_t)f);
        if (++FG.warm >= 3 && g_fg_upto >= 5) FG.gen_ready = 1;
        else fg_finish(FG.attempt, DM_FG_REJECTED_UNAVAILABLE);   // the first two outputs after a new history are the new frame unchanged
    } else if (!ok) { if (FG.warm) FG.resets++; FG.warm = 0; }
    FG.prev_ok = have_d && have_cam; FG.last_frame = f; FG.cur = prev;
}
// At the present (present_enqueue, the game's thread): the generated frame's window picture into a ring texture of the host's, in a command buffer of its own committed
// behind the frame's work. Returns the texture's serial (0 = no generated frame for this present) and its ring slot
static uint64_t fg_compose(const tex_t *t, const int crop[4], int *slot) {
    if (g_fg_mode <= 0 || !FG.gen_ready) return 0;
    FG.gen_ready = 0;
    #define FG_REJECT(why) do { fg_finish(FG.attempt, why); return 0; } while (0)
    if (g_dm_present_link) { static int once; if (!once++) MLOG("[RED] frame generation (scratch build): DM_PRESENT_LINK is not 0, generated frames are not shown (it paces on the present thread only)"); FG_REJECT(DM_FG_REJECTED_UNAVAILABLE); }
    id<MTLTexture> src = tex_mtl(t); if (!src) FG_REJECT(DM_FG_REJECTED_UNAVAILABLE);
    id<MTLTexture> raw = present_raw_src(src); NSUInteger r[4];
    if (raw.pixelFormat != MTLPixelFormatRGBA8Unorm && raw.pixelFormat != MTLPixelFormatBGRA8Unorm && raw.pixelFormat != MTLPixelFormatRGB10A2Unorm) FG_REJECT(DM_FG_REJECTED_UNAVAILABLE);
    int hdr = raw.pixelFormat == MTLPixelFormatRGB10A2Unorm;
    if (hdr && (!FG.hdr_matched || FG.hdr_frame != (long)atomic_load(&g_cur_frame) || FG.hdr_target != t->serial)) {
        atomic_store(&g_fg_fallback, 100);
        static int once; if (!once++) MLOG("frame generation: HDR terminal shader/canvas contract not matched; original HDR frame retained");
        FG_REJECT(DM_FG_REJECTED_UNAVAILABLE);
    }
    if (!present_crop_rect(src, crop, r)) FG_REJECT(DM_FG_REJECTED_UNAVAILABLE);
    if (hdr && (r[0] || r[1])) FG_REJECT(DM_FG_REJECTED_UNAVAILABLE);
    if (r[2] != FG.ow || r[3] != FG.oh) { static int once; if (!once++) MLOG("[RED] frame generation: the present shows %lux%lu of the window texture, not the %lux%lu canvas 1:1; frames are shown as the game draws them",
                                                                           (unsigned long)r[2], (unsigned long)r[3], (unsigned long)FG.ow, (unsigned long)FG.oh); FG_REJECT(DM_FG_REJECTED_UNAVAILABLE); }
    int k = FG.reserved_slot;
    pthread_mutex_lock(&g_pq_mu); int busy = !dm_fg_slot_reusable(&FG.slot[k]); pthread_mutex_unlock(&g_pq_mu);
    if (busy) { FG.busy++; FG_REJECT(DM_FG_REJECTED_QUEUE); }
    NSError *e = nil;
    if (!FG.comp_pso || FG.comp_fmt != raw.pixelFormat) {
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new]; pd.vertexFunction = FG.vf; pd.fragmentFunction = FG.f_comp; pd.colorAttachments[0].pixelFormat = raw.pixelFormat;
        FG.comp_pso = [g_dev newRenderPipelineStateWithDescriptor:pd error:&e]; FG.comp_fmt = raw.pixelFormat;
        if (!FG.comp_pso) { MLOG("[RED] frame generation: no pipeline for the window format %lu: %s", (unsigned long)raw.pixelFormat, e.localizedDescription.UTF8String); FG_REJECT(DM_FG_REJECTED_UNAVAILABLE); }
    }
    if (!FG.comp_pso) FG_REJECT(DM_FG_REJECTED_UNAVAILABLE);
    id<MTLTexture> dst = FG.win[k];
    if (!dst || dst.width != src.width || dst.height != src.height || dst.pixelFormat != raw.pixelFormat) {
        if (dst) { res_remove(dst); defer_obj_release(dst); }
        dst = fg_tex(raw.pixelFormat, src.width, src.height, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead, @"framegen window picture");
        if (!dst) { MLOG("[RED] frame generation: no texture for the generated frame's window picture"); FG_REJECT(DM_FG_REJECTED_UNAVAILABLE); }
        if (!FG.win_serial[k]) { static uint64_t n; FG.win_serial[k] = (1ull << 61) + (1ull << 40) + ++n; }
        serial_put(FG.win_serial[k], dst); FG.win[k] = dst;
        MLOG("frame generation: window picture ring slot %d %lux%lu format %lu", k, (unsigned long)dst.width, (unsigned long)dst.height, (unsigned long)dst.pixelFormat);
    }
    if (!FG.walloc[k]) { FG.walloc[k] = [g_dev newCommandAllocator]; FG.wcb[k] = [g_dev newCommandBuffer]; FG.wpar[k] = [g_dev newBufferWithLength:80 options:MTLResourceStorageModeShared];
        if (!FG.walloc[k] || !FG.wcb[k] || !FG.wpar[k]) { MLOG("[RED] frame generation: no command allocator or buffer for the window picture"); FG.walloc[k] = nil; FG_REJECT(DM_FG_REJECTED_UNAVAILABLE); }
        res_add(FG.wpar[k]); }
    res_commit();
    int32_t *p = (int32_t *)FG.wpar[k].contents; p[0] = (int32_t)r[0]; p[1] = (int32_t)r[1]; p[2] = (int32_t)FG.ow; p[3] = (int32_t)FG.oh; p[4] = hdr ? FG.hdr_flip : g_fg_flip; p[5] = g_fg_view;
    p[6] = hdr ? 2 : 1; ((float *)p)[7] = g_fg_soft; memcpy((float *)p + 8, FG.hdr_rows, sizeof FG.hdr_rows);
    [FG.walloc[k] reset];
    id<MTL4CommandBuffer> cb = FG.wcb[k];
    [cb beginCommandBufferWithAllocator:FG.walloc[k]];
    MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new];
    rp.colorAttachments[0].texture = dst; rp.colorAttachments[0].loadAction = MTLLoadActionDontCare; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTL4RenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
    if (!en) { cb_end(cb, nil); MLOG("[RED] frame generation: no render encoder for the window picture"); FG_REJECT(DM_FG_REJECTED_UNAVAILABLE); }
    [en barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:resource_vis()];
    [en setArgumentTable:FG.wargs atStages:MTLRenderStageFragment];
    [FG.wargs setTexture:raw.gpuResourceID atIndex:0]; [FG.wargs setTexture:FG.gen_up.gpuResourceID atIndex:1]; [FG.wargs setTexture:FG.cv.gpuResourceID atIndex:2]; [FG.wargs setTexture:FG.cv0.gpuResourceID atIndex:3]; [FG.wargs setTexture:FG.gen.gpuResourceID atIndex:4];
    [FG.wargs setAddress:FG.wpar[k].gpuAddress atIndex:0];
    [en setViewport:(MTLViewport){0, 0, dst.width, dst.height, 0, 1}]; [en setScissorRect:(MTLScissorRect){0, 0, dst.width, dst.height}];
    [en setRenderPipelineState:FG.comp_pso]; [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [en endEncoding];
    cb_end(cb, nil);
    uint64_t attempt = FG.attempt;
    FG.win_id[k] = attempt; FG.win_deadline[k] = FG.deadline; FG.win_pair[k]=FG.pair;
    pthread_mutex_lock(&g_pq_mu);
    FG.slot[k].composition_submitted = attempt; FG.slot[k].queued = 1;
    if (g_fg_reserved_epoch!=FG.pair.epoch || g_fg_reserved_real_ns<FG.pair.real_ns) {
        g_fg_reserved_epoch=FG.pair.epoch; g_fg_reserved_real_ns=FG.pair.real_ns;
    }
    pthread_mutex_unlock(&g_pq_mu);
    MTL4CommitOptions *opts = [MTL4CommitOptions new];
    [opts addFeedbackHandler:^(id<MTL4CommitFeedback> fb) {
        if (!fb.error) fg_cost_note(FG_COST_COMPOSITION,attempt,fb.GPUStartTime,fb.GPUEndTime);
        if (fb.error) { fg_finish(attempt, DM_FG_FAILED); gpu_lost_check(fb.error, "frame generation composition"); }
        pthread_mutex_lock(&g_pq_mu);
        if (FG.slot[k].composition_done < attempt) FG.slot[k].composition_done = attempt;
        pthread_cond_broadcast(&g_pq_cv); pthread_mutex_unlock(&g_pq_mu);
    }];
    pthread_mutex_t *commit_mu = res_submit_mutex();
    pthread_mutex_lock(commit_mu); [g_q4 commit:&cb count:1 options:opts]; pthread_mutex_unlock(commit_mu);
    *slot = k;
    #undef FG_REJECT
    return FG.win_serial[k];
}
// The present thread: whether this present's generated frame is shown. Not while frames come late or the queue is behind, until three presents in a row came in time
static int fg_pace(int slot, uint64_t id, double deadline, int waiting, double *target, double *real_target) {
    *target=*real_target=0;
    if (waiting > 2) { fg_finish(id, DM_FG_REJECTED_QUEUE); return 0; }
    // This waits only for work the real frame also follows on the same GPU queue.
    // The wait is bounded by the admitted deadline; failure immediately uses the real frame.
    pthread_mutex_lock(&g_pq_mu);
    while (FG.slot[slot].composition_done < id && !atomic_load(&g_gpu_lost)) {
        double remain = deadline - CACurrentMediaTime();
        if (remain <= 0) break;
        struct timespec ts = {(time_t)remain, (long)((remain - floor(remain)) * 1e9)};
        pthread_cond_timedwait_relative_np(&g_pq_cv, &g_pq_mu, &ts);
    }
    int complete = FG.slot[slot].composition_done >= id && atomic_load(&g_fg_attempts[id % 128]) == (id << 8);
    pthread_mutex_unlock(&g_pq_mu);
    double tick=0,refresh=0; uint64_t epoch=0;
    int cadence=front_framegen_timing_epoch(&tick,&refresh,&epoch);
    dm_fg_cadence c={epoch,tick>0?(uint64_t)(tick*1e9):0,refresh>0?(uint64_t)(refresh*1e9):0};
    dm_fg_pair p=FG.win_pair[slot]; uint64_t now=(uint64_t)(CACurrentMediaTime()*1e9);
    if (!complete || !cadence || !dm_fg_pair_valid(&p,&c,now,500000) || p.generated_ns>(uint64_t)(deadline*1e9)+1) {
        fg_finish(id, DM_FG_REJECTED_LATE); return 0;
    }
    *target=p.generated_ns/1e9; *real_target=p.real_ns/1e9;
    return 1;
}
// Each frame put on screen (the drawable's presented handler): the intervals between shown frames and, for real frames, the time from the game's enqueue to the screen
static void fg_shown(int gen, double tt, double tin) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER; static double last, first, iv[600], lat[600]; static int n, nl, ngen;
    if (!(tt > 0) || !g_fg_seen) return;
    pthread_mutex_lock(&mu);
    if (last > 0 && tt > last) { if (!n) first = last; iv[n++] = (tt - last) * 1e3; ngen += gen; }
    if (tt > last) {
        if (last > 0) atomic_store(&g_fg_interval_ns, (uint64_t)((tt - last) * 1e9));
        atomic_store(&g_fg_present_ns, (uint64_t)(tt * 1e9));
        last = tt;
    }
    if (!gen && tin > 0 && nl < 600) lat[nl++] = (tt - tin) * 1e3;
    if (n == 600) {
        for (int i = 1; i < n; i++) { double x = iv[i]; int j = i - 1; while (j >= 0 && iv[j] > x) { iv[j + 1] = iv[j]; j--; } iv[j + 1] = x; }
        for (int i = 1; i < nl; i++) { double x = lat[i]; int j = i - 1; while (j >= 0 && lat[j] > x) { lat[j + 1] = lat[j]; j--; } lat[j + 1] = x; }
        int over = 0; for (int i = 0; i < n; i++) over += iv[i] > 25.0;
        MLOG("frame generation %s: 600 shown frames at %.1f a second, %d generated; interval between shown frames p50 %.2f ms, p95 %.2f, max %.2f, over 25 ms %d; game enqueue to screen (not input latency; -1 unavailable) p50 %.1f ms, p95 %.1f; "
             "interpolations %llu, history restarts %llu, no camera %llu, no depth %llu, ring slot busy %llu; generated frames not shown: late %llu, queue behind %llu, settling %llu",
             g_fg_mode > 0 ? "on" : "off", 600.0 / (last - first), ngen, iv[299], iv[569], iv[599], over, nl ? lat[nl / 2] : -1.0, nl ? lat[nl * 95 / 100] : -1.0,
             (unsigned long long)FG.made, (unsigned long long)FG.resets, (unsigned long long)FG.nocam, (unsigned long long)FG.nodepth, (unsigned long long)FG.busy,
             (unsigned long long)FG.late, (unsigned long long)FG.behind, (unsigned long long)FG.settling);
        MLOG("frame generation totals: %llu early skips, %llu submitted, %llu presented",
             (unsigned long long)atomic_load(&g_fg_budget_skips),
             (unsigned long long)atomic_load(&g_fg_accepted),
             (unsigned long long)atomic_load(&g_fg_presented));
        MLOG("frame generation outcomes: presented %llu, dropped %llu, late %llu, queue %llu, unavailable/history %llu, failed %llu",
             (unsigned long long)atomic_load(&g_fg_outcomes[DM_FG_PRESENTED]), (unsigned long long)atomic_load(&g_fg_outcomes[DM_FG_DROPPED]),
             (unsigned long long)atomic_load(&g_fg_outcomes[DM_FG_REJECTED_LATE]), (unsigned long long)atomic_load(&g_fg_outcomes[DM_FG_REJECTED_QUEUE]),
             (unsigned long long)atomic_load(&g_fg_outcomes[DM_FG_REJECTED_UNAVAILABLE]), (unsigned long long)atomic_load(&g_fg_outcomes[DM_FG_FAILED]));
        dm_fg_cost cost[FG_COST_COUNT]; pthread_mutex_lock(&g_pq_mu); memcpy(cost,g_fg_cost,sizeof cost); pthread_mutex_unlock(&g_pq_mu);
        uint64_t now=(uint64_t)(CACurrentMediaTime()*1e9);
        MLOG("frame generation GPU scopes (latest completed, -1 unknown/stale): interpolation %.3f ms, reconstruction %.3f ms, composition %.3f ms, presentation %.3f ms; admission reserve remains an 8 ms estimate",
             dm_fg_cost_fresh(&cost[0],now,1000000000)?cost[0].duration_ns/1e6:-1.0,dm_fg_cost_fresh(&cost[1],now,1000000000)?cost[1].duration_ns/1e6:-1.0,
             dm_fg_cost_fresh(&cost[2],now,1000000000)?cost[2].duration_ns/1e6:-1.0,dm_fg_cost_fresh(&cost[3],now,1000000000)?cost[3].duration_ns/1e6:-1.0);
        n = nl = ngen = 0;
    }
    pthread_mutex_unlock(&mu);
}
#elif FG_PART == 2
// The generated frame onto the screen, ahead of its real frame (present_main; the present thread takes the drawable itself: DM_PRESENT_LINK=0). Command allocators of its
// own (two, in turn), as #2241 gives the picture after a generated frame one, so neither present waits for the other's GPU work
static id<MTL4CommandAllocator> g_fg_palloc[2]; static id<MTL4CommandBuffer> g_fg_pcb[2]; static id<MTLBuffer> g_fg_puv[2]; static uint64_t g_fg_pfence[2], g_fg_pdone[2], g_fg_pseq;
static int present_gen(const tex_t *t, double target, double deadline, int slot, uint64_t attempt, const int crop[4]) {
    if (!(target>0 && deadline>0) || CACurrentMediaTime()>=target) { fg_finish(attempt,DM_FG_REJECTED_LATE); return 0; }
    id<MTLTexture> src = tex_mtl(t);
    if (!src || dm_focus_now() != 1 || atomic_load(&g_gpu_lost)) return 0;
    CAMetalLayer *layer = (__bridge CAMetalLayer *)front_metal_layer();
    if (!layer || (layer.pixelFormat != MTLPixelFormatBGRA8Unorm && layer.pixelFormat != MTLPixelFormatBGR10A2Unorm)) return 0;
    if ((layer.pixelFormat == MTLPixelFormatBGR10A2Unorm) != (src.pixelFormat == MTLPixelFormatRGB10A2Unorm)) return 0;
    present_pipeline(layer.pixelFormat, src.pixelFormat);
    uint64_t seq = ++g_fg_pseq; int i = (int)(seq & 1);
    if (!g_fg_palloc[i]) { g_fg_palloc[i] = [g_dev newCommandAllocator]; g_fg_pcb[i] = [g_dev newCommandBuffer]; g_fg_puv[i] = [g_dev newBufferWithLength:sizeof(float) * 8 options:MTLResourceStorageModeShared];
        if (!g_fg_palloc[i] || !g_fg_pcb[i] || !g_fg_puv[i]) { g_fg_palloc[i] = nil; return 0; }
        res_add(g_fg_puv[i]); }
    res_commit();
    pthread_mutex_lock(&g_pq_mu);
    BOOL busy = g_fg_pdone[i] < g_fg_pfence[i];
    pthread_mutex_unlock(&g_pq_mu);
    if (busy) return 0;
    id<CAMetalDrawable> dr = [layer nextDrawable];
    if (!dr) return 0;
    double tick=0,refresh=0; uint64_t epoch=0;
    int cadence=front_framegen_timing_epoch(&tick,&refresh,&epoch);
    dm_fg_cadence c={epoch,tick>0?(uint64_t)(tick*1e9):0,refresh>0?(uint64_t)(refresh*1e9):0};
    if (!cadence || !dm_fg_pair_valid(&FG.win_pair[slot],&c,(uint64_t)(CACurrentMediaTime()*1e9),500000) || target>deadline+1e-9) {
        fg_finish(attempt,DM_FG_REJECTED_LATE); return 0;
    }

    [g_fg_palloc[i] reset];
    [g_q4 waitForDrawable:dr];
    id<MTL4CommandBuffer> cb = g_fg_pcb[i];
    [cb beginCommandBufferWithAllocator:g_fg_palloc[i]];
    int kind = present_encode_m4(cb, src, dr.texture, crop, g_fg_puv[i], &g_fx_present);
    cb_end(cb, layer.residencySet);
    if (!kind || CACurrentMediaTime()>=target) { [g_q4 signalDrawable:dr]; if (kind) fg_finish(attempt,DM_FG_REJECTED_LATE); return 0; }
    MTL4CommitOptions *o = [MTL4CommitOptions new];
    [o addFeedbackHandler:^(id<MTL4CommitFeedback> fb) {
        if (!fb.error) fg_cost_note(FG_COST_PRESENTATION,attempt,fb.GPUStartTime,fb.GPUEndTime);
        if (fb.error) { MLOG("[RED] frame generation: display command buffer error: %s", fb.error.description.UTF8String); gpu_lost_check(fb.error, "display (generated frame)"); }
        pthread_mutex_lock(&g_pq_mu); if (FG.slot[slot].reader_done < attempt) FG.slot[slot].reader_done = attempt; if (seq > g_fg_pdone[i]) g_fg_pdone[i] = seq; g_pq_inflight--; pthread_cond_broadcast(&g_pq_cv); pthread_mutex_unlock(&g_pq_mu);
    }];
    pthread_mutex_lock(&g_pq_mu); g_pq_inflight++; FG.slot[slot].reader_submitted = attempt; pthread_mutex_unlock(&g_pq_mu);
    pthread_mutex_lock(&g_res_commit_mu);
    [g_q4 commit:&cb count:1 options:o];
    pthread_mutex_unlock(&g_res_commit_mu);
    [g_q4 signalDrawable:dr];
    g_fg_pfence[i] = seq;
    atomic_fetch_add(&g_fg_accepted, 1);
    [dr addPresentedHandler:^(id<MTLDrawable> d) {
        if (d.presentedTime > 0) {
            atomic_fetch_add(&g_displayed_frames, 1);
            if (atomic_fetch_add(&g_fg_presented, 1) == 0) MLOG("frame generation: first generated frame presented");
        }
        fg_finish(attempt, d.presentedTime > 0 ? DM_FG_PRESENTED : DM_FG_DROPPED);
        fg_shown(1, d.presentedTime, 0);
    }];
    [dr presentAtTime:target];
    return 1;
}
// DM_FRAMEGEN_SHOT: the generated and the real picture of a frame as PNGs (waits for the GPU: a diagnostic)
static void fg_shots(uint64_t frame, const tex_t *gen, const tex_t *real, const int crop[4]) {
    if (!g_fg_shot_dir || (long)frame < g_fg_shot_a || (long)frame > g_fg_shot_b) return;
    char path[1024]; mtl_finish_gpu();
    id<MTLTexture> g = gen ? tex_mtl(gen) : nil, r = tex_mtl(real);
    if (g) { snprintf(path, sizeof path, "%s/F%llu_generated.png", g_fg_shot_dir, (unsigned long long)frame); shot(g, path, 1, crop); }
    if (r) { snprintf(path, sizeof path, "%s/F%llu_real.png", g_fg_shot_dir, (unsigned long long)frame); shot(r, path, 1, crop); }
}
#endif
