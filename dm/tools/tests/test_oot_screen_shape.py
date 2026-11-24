"""OOT aspect-stub safety. No game launch, compiler or proprietary fixture required.

ASCENSION_TEST_MAIN_NSO optionally checks the supplied original main read-only.
This does not establish rendered/HUD correctness; that needs isolated gameplay.
"""
import importlib.util
import os
from pathlib import Path
import sys
import unittest

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
spec = importlib.util.spec_from_file_location('oot_aspect_patches', TOOLS.parent / 'games/0400486024DEC000/patches.py')
patch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patch)


class Context:
    def __init__(self):
        keys = dict.fromkeys(tuple(k for k, _, _ in patch.SCREEN_SHAPE_SITES) + patch.SCREEN_FADE_DEPENDENCIES)
        self.others = {key: 0x100000 + i * 0x20000 for i, key in enumerate(keys)}
        self.original = {self.others[k] + off: bytes.fromhex(hx) for k, off, hx in patch.SCREEN_SHAPE_SITES}
        self.state = 0x5000000
        self.code = {}
        self.writes = []
        self.capabilities = set()

    def here(self):
        return 0x6000000 + 4 * len(self.code)

    def emit(self, word):
        self.code[self.here()] = word

    def site(self, at, orig):
        if self.original[at] != bytes.fromhex(orig):
            raise ValueError('original instruction mismatch')

    def put(self, at, words, why):
        self.writes.append((at, words, why))


def signed(value, bits):
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


def disabled_path(ctx, start, back):
    """Follow only the zero-state bypass: ADRP, its state load, CBZ and B.

    Record original instructions rather than emulating their effects. Fail if
    aspect math executes while disabled or a branch leaves the emitted stub.
    """
    pc, regs, original = start, {}, []
    for _ in range(32):
        if pc == back:
            return original
        w = ctx.code[pc]
        if w & 0x9f000000 == 0x90000000:  # ADRP
            imm = ((w >> 5) & 0x7ffff) << 2 | ((w >> 29) & 3)
            regs[w & 31] = (pc & ~0xfff) + (signed(imm, 21) << 12)
        elif w & 0xffc00000 == 0xb9400000 and ((w >> 5) & 31) == 16:
            address = regs[16] + ((w >> 10) & 0xfff) * 4
            if not ctx.state <= address < ctx.state + 0x1000:
                raise AssertionError('load does not read the isolated state page')
            regs[w & 31] = 0
        elif w & 0x7f000000 == 0x34000000:  # CBZ
            if regs[w & 31] == 0:
                pc += signed((w >> 5) & 0x7ffff, 19) * 4
                continue
        elif w & 0xfc000000 == 0x14000000:  # B
            pc += signed(w & 0x3ffffff, 26) * 4
            continue
        else:
            original.append(w)
        pc += 4
    raise AssertionError('disabled stub did not return')


def run_fade_guard(names, pane_index, null_name=None):
    """Interpret the production emitted integer gate, not a duplicate predicate.

    Deliberately excludes geometry/FP arithmetic; checks identity admission and
    scratch restoration. Unknown opcodes or out-of-range reads fail the test.
    """
    start, state, back = 0x6000000, 0x5000000, 0x100004
    words, labels = patch.screen_fade_stub(start, state, back)
    code = {start + 4 * i: w for i, w in enumerate(words)}
    mem = {}
    def put(a, v, size):
        for i in range(size): mem[a + i] = (v >> (8 * i)) & 255
    def get(a, size): return sum(mem[a + i] << (8 * i) for i in range(size))
    for a, w in code.items(): put(a, w, 4)
    owner, panes, resource, table = 0x200000, 0x300000, 0x400000, 0x410000
    pane = panes + pane_index * 0xb0
    put(pane + 0x18, owner, 8); put(owner + 0x100, panes, 8)
    put(owner + 0x13c, len(names), 2); put(owner + 0xf8, resource, 8); put(resource + 0x20, table, 8)
    for i, name in enumerate(names):
        ptr = 0x420000 + i * 128
        put(table + i * 8, 0 if i == null_name else ptr, 8)
        for j, v in enumerate(name.encode() + b'\0'): put(ptr + j, v, 1)
    regs = [0xabc000 + i for i in range(32)]
    regs[21], regs[31] = pane, 0x800000
    saved = regs.copy(); pc = labels['guard']; cmp = (0, 0); scaled = False
    for _ in range(12000):
        if pc == labels['original']:
            if any(regs[i] != saved[i] for i in range(9, 15)) or regs[8] != saved[8] or regs[31] != saved[31]:
                raise AssertionError('live/scratch registers or stack not restored')
            return scaled
        w = code[pc]; rd, rn = w & 31, (w >> 5) & 31
        if w & 0xffc00000 in (0xa9800000, 0xa9000000, 0xa9400000, 0xa8c00000):
            kind = w & 0xffc00000; off = signed((w >> 15) & 127, 7) * 8; rt2 = (w >> 10) & 31
            addr = regs[rn] if kind == 0xa8c00000 else regs[rn] + off
            if kind in (0xa9800000, 0xa9000000): put(addr, regs[rd], 8); put(addr + 8, regs[rt2], 8)
            else: regs[rd], regs[rt2] = get(addr, 8), get(addr + 8, 8)
            if kind in (0xa9800000, 0xa8c00000): regs[rn] += off
        elif w & 0xffc00000 == 0xf9400000: regs[rd] = get(regs[rn] + ((w >> 10) & 4095) * 8, 8)
        elif w & 0xffc00000 == 0x79400000: regs[rd] = get(regs[rn] + ((w >> 10) & 4095) * 2, 2)
        elif w & 0xffe0fc00 == 0xf8607800: regs[rd] = get(regs[rn] + regs[(w >> 16) & 31] * 8, 8)
        elif w & 0xffe00c00 == 0x38400400:
            regs[rd] = get(regs[rn], 1); regs[rn] += signed((w >> 12) & 511, 9)
        elif w & 0xffe0fc00 == 0xcb000000: regs[rd] = (regs[rn] - regs[(w >> 16) & 31]) & ((1 << 64) - 1)
        elif w & 0x7f000000 == 0x71000000:
            mask = (1 << (64 if w >> 31 else 32)) - 1; cmp = (regs[rn] & mask, (w >> 10) & 4095)
        elif w & 0x7fe0fc00 == 0x6b000000: cmp = (regs[rn] & 0xffffffff, regs[(w >> 16) & 31] & 0xffffffff)
        elif w & 0xff800000 == 0x52800000: regs[rd] = (w >> 5) & 65535
        elif w & 0xffc00000 == 0x91000000: regs[rd] = regs[rn] + ((w >> 10) & 4095)
        elif w & 0x9f000000 == 0x10000000:
            regs[rd] = pc + signed((((w >> 5) & 0x7ffff) << 2) | ((w >> 29) & 3), 21)
        elif w & 0x9f000000 == 0x90000000:
            regs[rd] = (pc & ~4095) + (signed((((w >> 5) & 0x7ffff) << 2) | ((w >> 29) & 3), 21) << 12)
        elif w & 0x7e000000 == 0x34000000:
            value = regs[rd] & ((1 << (64 if w >> 31 else 32)) - 1)
            take = (value != 0) if w & 0x1000000 else (value == 0)
            if take: pc += signed((w >> 5) & 0x7ffff, 19) * 4; continue
        elif w & 0xff000010 == 0x54000000:
            take = {0: cmp[0] == cmp[1], 1: cmp[0] != cmp[1], 3: cmp[0] < cmp[1]}[w & 15]
            if take: pc += signed((w >> 5) & 0x7ffff, 19) * 4; continue
        elif w & 0xfc000000 == 0x14000000: pc += signed(w & 0x3ffffff, 26) * 4; continue
        elif w == 0xbd405e02: pass  # scale load (not part of the identity gate)
        elif w == 0x4f829000: scaled = True
        else: raise AssertionError('Unknown integer-gate opcode ' + hex(w))
        pc += 4
    raise AssertionError('guard did not terminate')


class ScreenShapeTests(unittest.TestCase):
    def test_complete_patch_emits_sixteen_unique_sites_and_capability(self):
        c = Context()
        self.assertTrue(patch.screen_shape(c))
        self.assertEqual(len(c.writes), 16)
        self.assertEqual(len({w[0] for w in c.writes}), 16)
        self.assertEqual(c.capabilities, {'SCREEN_ASPECT'})

    def test_each_missing_function_leaves_all_aspect_code_unwritten(self):
        for key in Context().others:
            with self.subTest(key=key):
                c = Context()
                c.others[key] = None
                self.assertFalse(patch.screen_shape(c))
                self.assertFalse(c.code)
                self.assertFalse(c.writes)
                self.assertFalse(c.capabilities)

    def test_each_bad_instruction_fails_before_first_emit(self):
        for key, off, _ in patch.SCREEN_SHAPE_SITES:
            with self.subTest(key=key, offset=off):
                c = Context()
                c.original[c.others[key] + off] = b'\0' * 4
                with self.assertRaises(ValueError):
                    patch.screen_shape(c)
                self.assertFalse(c.code)
                self.assertFalse(c.writes)
                self.assertFalse(c.capabilities)

    def test_disabled_every_stub_executes_its_original_instruction_once(self):
        c = Context()
        patch.screen_shape(c)
        for at, words, _ in c.writes:
            with self.subTest(site=hex(at)):
                self.assertEqual(len(words), 1)
                branch = words[0]
                self.assertEqual(branch & 0xfc000000, 0x14000000)
                start = at + signed(branch & 0x3ffffff, 26) * 4
                self.assertEqual(disabled_path(c, start, at + 4), [int.from_bytes(c.original[at], 'little')])

    def test_emitted_guard_accepts_only_known_fade_panes(self):
        for names, allowed in zip(patch.SCREEN_FADE_LAYOUTS, ({1, 3, 5, 6, 8}, {2})):
            for index in range(len(names)):
                with self.subTest(pane=names[index]):
                    self.assertEqual(run_fade_guard(names, index), index in allowed)

    def test_emitted_guard_rejects_any_changed_name_or_order(self):
        for names, index in zip(patch.SCREEN_FADE_LAYOUTS, (3, 2)):
            for changed in range(len(names)):
                altered = list(names); altered[changed] += '_movie'
                with self.subTest(layout=len(names), changed=changed):
                    self.assertFalse(run_fade_guard(altered, index))
            swapped = list(names); swapped[0], swapped[1] = swapped[1], swapped[0]
            self.assertFalse(run_fade_guard(swapped, index))
            self.assertFalse(run_fade_guard(names[:-1], index))
            self.assertFalse(run_fade_guard(names, index, null_name=0))

    def test_emitted_guard_does_not_accept_movie_layout_with_named_black_pane(self):
        names = list(patch.SCREEN_FADE_LAYOUTS[0]); names[1] = 'MovieFrame_00'
        self.assertFalse(run_fade_guard(names, 3))

    @unittest.skipUnless(os.environ.get('ASCENSION_TEST_MAIN_NSO'), 'optional private original NSO not supplied')
    def test_original_game_identity_fingerprints_and_sixteen_site_bytes(self):
        import nso
        import nso_match
        m = nso.read(os.environ['ASCENSION_TEST_MAIN_NSO'])
        self.assertEqual(m.build_id[:16].hex(), 'e0f3ccfaf50065ae4ea01eb5aeb13b1a')
        image = nso_match.Image(m.image, m.text_end)
        rule = patch.MATCH_FRAME_STEP['main'][0]
        image.locate(rule['size'], rule['fingerprints'])
        found = {k: image.locate(v['size'], v['fingerprints']) for k, v in rule['others'].items()}
        for key, off, hx in patch.SCREEN_SHAPE_SITES:
            with self.subTest(key=key, offset=off):
                self.assertEqual(m.image[found[key] + off:found[key] + off + 4].hex(), hx)


if __name__ == '__main__':
    unittest.main()
