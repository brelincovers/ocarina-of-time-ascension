"""CPU-only synthetic container/transaction tests; never invokes Metal tools."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location('pso_repack', Path(__file__).parents[1] / 'tools/pso_repack.py')
repack = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(repack)


def archive(metallib=True, pipelines=37, vertex=b'compiled vertex bytes', cpu=0x01000013, platform=1):
    sections = [('__reflection', b'preserved reflection'), ('__vertex', vertex),
                ('__fragment', b'compiled fragment bytes'), ('__compute', b'compiled compute bytes')]
    if metallib:
        sections.append(('__metallib', b'embedded library' * 200))
    segment_length = 72 + len(sections) * 80
    commands_length = segment_length + 40 + 24
    note_offset = 32 + commands_length
    table_offset = note_offset + 16
    content_offset = (table_offset + pipelines + 15) & ~15
    file_size = content_offset + sum(len(data) for _, data in sections)
    header = struct.pack('<8I', repack.MH_MAGIC_64, cpu, 0, repack.MH_GPU_EXECUTE, 3, commands_length, 0, 0)
    segment = struct.pack('<II16sQQQQiiII', repack.LC_SEGMENT_64, segment_length, b'__TEXT',
                          content_offset, file_size - content_offset, content_offset, file_size - content_offset, 7, 5, len(sections), 0)
    offset = content_offset
    for name, data in sections:
        segment += struct.pack('<16s16sQQIIIIIIII', name.encode(), b'__TEXT', offset, len(data), offset, 0, 0, 0, 0, 0, 0, 0)
        offset += len(data)
    note = struct.pack('<II16sQQ', repack.LC_NOTE, 40, b'AIR_PIPELINE', note_offset, 16)
    target = struct.pack('<6I', repack.LC_BUILD_VERSION, 24, platform, 27 << 16, 27 << 16, 0)
    prefix = header + segment + note + target + struct.pack('<QQ', table_offset, pipelines)
    return prefix.ljust(content_offset, b'\0') + b''.join(data for _, data in sections)


class RepackTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.source = self.root / 'original.mtl4a'
        self.source.write_bytes(archive())
        self.output = self.root / 'experimental.mtl4a'

    def tearDown(self):
        self.tmp.cleanup()

    def info(self, data):
        file = self.root / 'parse.mtl4a'
        file.write_bytes(data)
        return repack.archive_info(file, hashes=True)

    def test_exact_gpu_code_and_pipeline_count_survive_removing_metallib(self):
        before, after = self.info(archive()), self.info(archive(metallib=False))
        repack.verify_repack(before, after)
        self.assertEqual(before['advertised_pipeline_count'], 37)
        self.assertEqual(after['embedded_metallib_bytes'], 0)

    def test_changed_code_target_count_or_remaining_metallib_is_rejected(self):
        before = self.info(archive())
        for kwargs in ({'vertex': b'wrong code'}, {'pipelines': 36}, {'cpu': 0x01000014},
                       {'platform': 2}, {'metallib': True}):
            with self.subTest(kwargs=kwargs), self.assertRaises(ValueError):
                repack.verify_repack(before, self.info(archive(**dict({'metallib': False}, **kwargs))))

    def test_no_hash_is_not_proof(self):
        before = repack.archive_info(self.source)
        with self.assertRaises(ValueError):
            repack.verify_repack(before, self.info(archive(metallib=False)))

    def test_malformed_container_is_rejected(self):
        for kind in ('short', 'fat', 'commands', 'section', 'note', 'pipeline'):
            bad = bytearray(archive())
            if kind == 'short': bad = bad[:15]
            elif kind == 'fat': struct.pack_into('<I', bad, 0, 0xCAFEBABE)
            elif kind == 'commands': struct.pack_into('<I', bad, 20, 0xffffffff)
            elif kind == 'section': struct.pack_into('<Q', bad, 32 + 72 + 40, len(bad) + 1)
            elif kind == 'note': struct.pack_into('<Q', bad, 32 + 72 + 5 * 80 + 24, len(bad) + 1)
            elif kind == 'pipeline': struct.pack_into('<Q', bad, 32 + 72 + 5 * 80 + 40 + 24 + 8, 0)
            with self.subTest(kind=kind), self.assertRaises((ValueError, struct.error)):
                self.info(bad)

    def runner(self, candidate=None, code=0):
        calls = []

        def run(command, **kwargs):
            calls.append(command)
            if command[-1] == '-help':
                return subprocess.CompletedProcess(command, 0, '-pack-metallibs <none|internal|external|universal>')
            if command[-1] == '--version':
                return subprocess.CompletedProcess(command, 0, 'test-only tool; no Metal invoked')
            self.assertIn('-pack-metallibs', command)
            self.assertEqual(command[command.index('-pack-metallibs') + 1], 'none')
            self.assertEqual(command[command.index('-nthreads') + 1], '1')
            Path(command[-1]).write_bytes(candidate if candidate is not None else archive(metallib=False))
            return subprocess.CompletedProcess(command, code)
        return run, calls

    def test_success_publishes_separate_archive_and_honest_manifest(self):
        run, calls = self.runner()
        original = self.source.read_bytes()
        result = repack.repack(self.source, self.output, '8d9afacc48e2', runner=run)
        self.assertEqual(len(calls), 3)
        self.assertEqual(self.source.read_bytes(), original)
        manifest = json.loads(Path(str(self.output) + '.manifest.json').read_text())
        self.assertEqual(manifest, result)
        self.assertTrue(manifest['compatibility_gate']['exact_non_metallib_section_bytes'])
        self.assertFalse(manifest['compatibility_gate']['archive_only_pipeline_lookup_coverage_tested'])
        self.assertFalse(manifest['compatibility_gate']['release_qualified'])

    def test_failed_tool_or_changed_code_never_publishes_candidate(self):
        for name, candidate, code in (('tool', None, 1), ('code', archive(metallib=False, vertex=b'wrong'), 0)):
            output = self.root / (name + '.mtl4a')
            run, _ = self.runner(candidate, code)
            with self.subTest(name=name), self.assertRaises((RuntimeError, ValueError)):
                repack.repack(self.source, output, '8d9afacc48e2', runner=run)
            self.assertFalse(output.exists())
            self.assertFalse(Path(str(output) + '.manifest.json').exists())
            self.assertEqual(self.source.read_bytes(), archive())

    def test_refuses_overwrite_and_any_app_destination_before_running_tool(self):
        self.output.write_bytes(b'keep me')
        for output in (self.source, self.output, self.root / 'Working.app/Contents/Resources/new.mtl4a'):
            run, calls = self.runner()
            with self.subTest(output=output), self.assertRaises((ValueError, FileExistsError)):
                repack.repack(self.source, output, '8d9afacc48e2', runner=run)
            self.assertFalse(calls)
        self.assertEqual(self.output.read_bytes(), b'keep me')

    def test_advertised_but_rejected_option_records_failure_without_retry(self):
        base, calls = self.runner()

        def rejected(command, **kwargs):
            if command[-1] in ('-help', '--version'):
                return base(command, **kwargs)
            calls.append(command)
            kwargs['stdout'].write("metal-pack: error: invalid packing 'none'\n")
            return subprocess.CompletedProcess(command, 1)

        with self.assertRaisesRegex(RuntimeError, 'advertises but rejects'):
            repack.repack(self.source, self.output, '8d9afacc48e2', runner=rejected)
        self.assertEqual(len(calls), 3)  # Help, version, exactly one packing attempt.
        report = json.loads(Path(str(self.output) + '.failure.json').read_text())
        self.assertEqual(report['status'], 'unsupported_pack_metallibs_none')
        self.assertEqual(report['tool_exit_code'], 1)
        self.assertTrue(report['source_unchanged'])
        self.assertFalse(report['archive_published'])
        self.assertFalse(report['runtime_lookup_tested'])
        self.assertFalse(self.output.exists())
        self.assertFalse(Path(str(self.output) + '.manifest.json').exists())
        self.assertEqual(self.source.read_bytes(), archive())

    def test_default_is_inspection_only(self):
        with contextlib.redirect_stdout(io.StringIO()) as out:
            repack.main([str(self.source), str(self.output), '--shader-chain', '8d9afacc48e2'])
        self.assertEqual(json.loads(out.getvalue())['action'], 'plan_only_no_output_written')
        self.assertFalse(self.output.exists())


if __name__ == '__main__':
    unittest.main()
