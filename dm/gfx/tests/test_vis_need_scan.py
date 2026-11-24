"""Compile the live counter-lookahead functions under ASan/UBSan, without Metal.

The cursor follows synthetic recorded operations, not a copied implementation.
GPU query values and image parity remain covered by t_render_enable_queue and
t_mesh_first_use; this fixture tests conservative replay/control-flow decisions.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1] / 'nvn/nvn_mtl.m'
PRELUDE = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { uint32_t op, size; } op_hdr;
enum { OP_END, OP_JUMP, OP_CALL, OP_DRAW, OP_MULTI_DRAW, OP_RESET_COUNTER, OP_REPORT_COUNTER };
enum { NVN_COUNTER_TYPE_SAMPLES_PASSED = 9 };
typedef struct { const uint8_t *p, *ret[8]; int ret_n, rest_i, bad; } fz_cursor;
static const uint8_t *g_ret[16]; static int g_ret_n;
static const uint64_t *g_rest; static int g_nrest, g_vis_need = 1, g_vis_need_mode = -1;
static int sw_mode(const char *name, int fallback) { (void)name; return fallback; }
static int sw_on(int mode) { return mode != 0; }
'''
HARNESS = r'''
typedef struct { uint8_t bytes[256]; size_t n; } chain;
static void emit(chain *c, uint32_t op, uint64_t a) {
    uint32_t size = op == OP_END || op == OP_DRAW ? 8 : op == OP_REPORT_COUNTER ? 24 : 16;
    assert(c->n + size <= sizeof c->bytes);
    op_hdr h = {op, size}; memcpy(c->bytes + c->n, &h, 8);
    if (size > 8) memcpy(c->bytes + c->n + 8, &a, 8);
    c->n += size;
}
static int checks;
static void check(const uint8_t *p, int want, const char *why) {
    vis_need_scan(p); checks++;
    if (g_vis_need != want) { fprintf(stderr, "FAIL %s: %d != %d\n", why, g_vis_need, want); exit(1); }
}
int main(void) {
    chain reset = {0}, report = {0}, end = {0}, draw_reset = {0}, other_reset = {0};
    emit(&reset, OP_RESET_COUNTER, NVN_COUNTER_TYPE_SAMPLES_PASSED); emit(&reset, OP_END, 0);
    emit(&report, OP_REPORT_COUNTER, NVN_COUNTER_TYPE_SAMPLES_PASSED); emit(&report, OP_END, 0);
    emit(&end, OP_END, 0);
    emit(&draw_reset, OP_MULTI_DRAW, UINT64_MAX); emit(&draw_reset, OP_DRAW, 0);
    emit(&draw_reset, OP_RESET_COUNTER, NVN_COUNTER_TYPE_SAMPLES_PASSED); emit(&draw_reset, OP_END, 0);
    emit(&other_reset, OP_REPORT_COUNTER, 1); emit(&other_reset, OP_RESET_COUNTER, NVN_COUNTER_TYPE_SAMPLES_PASSED); emit(&other_reset, OP_END, 0);
    check(reset.bytes, 1, "default is baseline until qualification");
    g_vis_need_mode = 1;
    check(reset.bytes, 0, "next reset discards the interval");
    check(report.bytes, 1, "next report observes the interval");
    check(end.bytes, 1, "unknown future beyond chunk retains counting");
    check(NULL, 1, "null continuation retains counting");
    check(draw_reset.bytes, 0, "indirect draws are crossed without reading their arguments");
    check(other_reset.bytes, 0, "other counter types do not consume sample totals");
    chain call = {0}; emit(&call, OP_CALL, (uintptr_t)report.bytes); emit(&call, OP_RESET_COUNTER, NVN_COUNTER_TYPE_SAMPLES_PASSED); emit(&call, OP_END, 0);
    check(call.bytes, 1, "callee report precedes caller reset");
    uint64_t ptr = (uintptr_t)end.bytes; memcpy(call.bytes + 8, &ptr, 8);
    check(call.bytes, 0, "callee return resumes caller reset");
    g_ret_n = 2; g_ret[0] = reset.bytes; g_ret[1] = report.bytes;
    check(end.bytes, 1, "innermost live caller report precedes outer reset");
    g_ret_n = 9; check(reset.bytes, 1, "over-deep live stack is never silently truncated");
    g_ret_n = 8; for (int i = 0; i < 8; i++) g_ret[i] = end.bytes;
    check(call.bytes, 1, "new call beyond cursor capacity retains counting");
    g_ret_n = 0;
    uint64_t rest[] = {(uintptr_t)reset.bytes}; g_rest = rest; g_nrest = 1;
    check(end.bytes, 0, "remaining handle in this submission has reset");
    g_nrest = 0; check(end.bytes, 1, "another queue or future chunk is never inspected");
    chain jump = {0}; emit(&jump, OP_JUMP, (uintptr_t)reset.bytes);
    check(jump.bytes, 0, "jump follows replay order");
    ptr = (uintptr_t)jump.bytes; memcpy(jump.bytes + 8, &ptr, 8);
    check(jump.bytes, 1, "cyclic jumps terminate conservatively");
    chain bad = {0}; emit(&bad, OP_RESET_COUNTER, NVN_COUNTER_TYPE_SAMPLES_PASSED);
    ((op_hdr *)bad.bytes)->size = 8; check(bad.bytes, 1, "short counter payload is unknown");
    ((op_hdr *)bad.bytes)->size = 0; check(bad.bytes, 1, "nonadvancing record is unknown");
    op_hdr *limit = calloc(20002, sizeof *limit); assert(limit);
    for (int i = 0; i < 20001; i++) limit[i] = (op_hdr){OP_DRAW, 8};
    limit[20001] = (op_hdr){OP_END, 8}; check((uint8_t *)limit, 1, "bounded search retains counting"); free(limit);
    g_vis_need_mode = 0; check(reset.bytes, 1, "explicit baseline counts every interval");
    printf("visibility lookahead: %d checks passed\n", checks); return 0;
}
'''


class VisibilityLookahead(unittest.TestCase):
    def test_current_source(self):
        source = SOURCE.read_text()
        start = source.index('static int vis_need_on(void) {')
        end = source.index('static void vis_draw_state(id<MTL4RenderCommandEncoder> en) {', start)
        compiler = shutil.which('clang')
        self.assertIsNotNone(compiler, 'clang is required')
        with tempfile.TemporaryDirectory(prefix='ascension-vis-need-') as folder:
            cfile, binary = Path(folder) / 'fixture.c', Path(folder) / 'fixture'
            cfile.write_text(PRELUDE + source[start:end] + HARNESS)
            built = subprocess.run([compiler, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                                    '-fsanitize=address,undefined', str(cfile), '-o', str(binary)],
                                   capture_output=True, text=True, timeout=30)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn('20 checks passed', ran.stdout)


if __name__ == '__main__':
    unittest.main()
