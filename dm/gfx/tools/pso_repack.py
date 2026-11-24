#!/usr/bin/env python3
"""Guarded experiment: remove embedded metallibs from an existing GPU archive.

This is a binary repack, never a shader compiler. The default is inspection and
planning only. --execute writes a NEW archive outside any app, using Apple's
installed metal-pack -pack-metallibs none. Exact non-metallib section hashes,
GPU target, platform and advertised pipeline count must survive before publish.
These checks do NOT prove Metal lookup compatibility; the manifest explicitly
requires archive-only lookup/coverage and startup tests before packaging.
Tool help alone is not a capability guarantee: installed versions may advertise
``none`` yet reject it. Such failures preserve the original and publish no archive.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile

MH_MAGIC_64, MH_GPU_EXECUTE = 0xFEEDFACF, 0xD
LC_SEGMENT_64, LC_NOTE, LC_BUILD_VERSION = 0x19, 0x31, 0x32


def archive_info(path, hashes=False):
    """Read the bounded thin-Mach-O container; never decode private GPU code."""
    path = Path(path)
    with path.open('rb') as stream:
        size = os.fstat(stream.fileno()).st_size

        def read(offset, count):
            if offset < 0 or count < 0 or offset > size or count > size - offset:
                raise ValueError('Archive range is outside the file')
            stream.seek(offset)
            data = stream.read(count)
            if len(data) != count:
                raise ValueError('Archive changed or is truncated')
            return data

        h = struct.unpack('<8I', read(0, 32))
        if h[0] != MH_MAGIC_64 or h[3] != MH_GPU_EXECUTE:
            raise ValueError('Only a thin little-endian MH_GPU_EXECUTE archive is supported')
        ncmds, cmd_bytes = h[4:6]
        if not ncmds or ncmds > cmd_bytes // 8 or cmd_bytes > 16 * 1024 * 1024:
            raise ValueError('Invalid or oversized Mach-O command table')
        commands = read(32, cmd_bytes)
        position, sections, count, platform = 0, {}, None, None
        for _ in range(ncmds):
            if len(commands) - position < 8:
                raise ValueError('Truncated load command')
            command, length = struct.unpack_from('<II', commands, position)
            if length < 8 or length > len(commands) - position:
                raise ValueError('Invalid load-command length')
            data = commands[position:position + length]
            if command == LC_SEGMENT_64:
                if length < 72:
                    raise ValueError('Truncated segment')
                seg = struct.unpack_from('<II16sQQQQiiII', data)
                if seg[5] > size or seg[6] > size - seg[5] or seg[9] > (length - 72) // 80:
                    raise ValueError('Invalid segment range or section count')
                for i in range(seg[9]):
                    sec = struct.unpack_from('<16s16sQQIIIIIIII', data, 72 + i * 80)
                    name = sec[1].rstrip(b'\0').decode('ascii') + '/' + sec[0].rstrip(b'\0').decode('ascii')
                    section_size, offset = sec[3], sec[4]
                    if name in sections or offset > size or section_size > size - offset:
                        raise ValueError('Duplicate or out-of-range section')
                    if (sec[8] & 255) in (1, 12, 18):
                        raise ValueError('Zero-fill sections are outside this repack contract')
                    sections[name] = dict(offset=offset, size=section_size)
            elif command == LC_NOTE:
                if length < 40:
                    raise ValueError('Truncated note')
                _, _, owner, offset, note_size = struct.unpack_from('<II16sQQ', data)
                if offset > size or note_size > size - offset:
                    raise ValueError('Invalid note range')
                if owner.rstrip(b'\0') == b'AIR_PIPELINE':
                    if note_size != 16 or count is not None:
                        raise ValueError('Unsupported AIR_PIPELINE index format')
                    table_offset, count = struct.unpack('<QQ', read(offset, 16))
                    if not count or table_offset < 32 + cmd_bytes or table_offset > size or count > size - table_offset:
                        raise ValueError('Invalid advertised pipeline table')
            elif command == LC_BUILD_VERSION:
                if length < 24 or platform is not None:
                    raise ValueError('Invalid build target')
                platform = list(struct.unpack_from('<III', data, 8))
            position += length
        if position != cmd_bytes or count is None or platform is None:
            raise ValueError('Incomplete GPU archive metadata')
        if not any(name in sections for name in ('__TEXT/__vertex', '__TEXT/__fragment', '__TEXT/__compute')):
            raise ValueError('Archive has no known GPU code sections')
        if hashes:
            for section in sections.values():
                stream.seek(section['offset'])
                digest, remaining = hashlib.sha256(), section['size']
                while remaining:
                    data = stream.read(min(1024 * 1024, remaining))
                    if not data:
                        raise ValueError('Section changed or truncated while hashing')
                    digest.update(data); remaining -= len(data)
                section['sha256'] = digest.hexdigest()
        return dict(bytes=size, cpu_type=h[1], cpu_subtype=h[2], file_type=h[3],
                    platform_minimum_sdk=platform, advertised_pipeline_count=count,
                    sections=sections, embedded_metallib_bytes=sum(s['size'] for n, s in sections.items() if n.endswith('/__metallib')))


def verify_repack(before, after):
    for key in ('cpu_type', 'cpu_subtype', 'file_type', 'platform_minimum_sdk', 'advertised_pipeline_count'):
        if before[key] != after[key]:
            raise ValueError('Repack changed ' + key)
    native = lambda info: {name: (section['size'], section.get('sha256')) for name, section in info['sections'].items()
                           if not name.endswith('/__metallib')}
    expected, observed = native(before), native(after)
    if any(value[1] is None for value in expected.values()) or expected != observed:
        raise ValueError('Non-metallib section bytes changed or hashes are missing')
    if after['embedded_metallib_bytes'] != 0:
        raise ValueError('Repack did not remove all embedded metallib payload')
    if not before['embedded_metallib_bytes'] or after['bytes'] >= before['bytes']:
        raise ValueError('Repack produced no archive-size reduction')


def fingerprint(path):
    stat = path.stat()
    return stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns, stat.st_ctime_ns


def file_hash(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for data in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(data)
    return result.hexdigest()


def repack(source, output, chain, runner=subprocess.run):
    source, output = Path(source).resolve(), Path(output).resolve()
    manifest = output.with_name(output.name + '.manifest.json')
    failure_manifest = output.with_name(output.name + '.failure.json')
    log = output.with_name(output.name + '.metal-pack.log')
    if source == output or any(p.suffix == '.app' for p in output.parents):
        raise ValueError('Output must be outside all applications and distinct from source')
    if any(p.exists() for p in (output, manifest, failure_manifest, log)):
        raise FileExistsError('Output, manifest or log already exists; refusing overwrite')
    if not re.fullmatch(r'[0-9a-f]{12}', chain):
        raise ValueError('Declare the exact 12-digit shader chain; it is not embedded in this archive')
    original = fingerprint(source)
    before = archive_info(source, hashes=True)
    before_sha = file_hash(source)
    if fingerprint(source) != original:
        raise ValueError('Source changed during validation')
    help_text = runner(['xcrun', '-sdk', 'macosx', 'metal-pack', '-help'], capture_output=True, text=True, check=True).stdout
    if '-pack-metallibs <none|internal|external|universal>' not in help_text:
        raise RuntimeError('Installed metal-pack does not advertise the required option')
    version = runner(['xcrun', '-sdk', 'macosx', 'metal-pack', '--version'], capture_output=True, text=True, check=True).stdout.strip()
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.pso-repack-', dir=output.parent) as temporary:
        temporary = Path(temporary)
        candidate = temporary / 'candidate.mtl4a'
        command = ['xcrun', '-sdk', 'macosx', 'metal-pack', '-nthreads', '1', '-pack-metallibs', 'none', str(source), '-o', str(candidate)]
        with log.open('x') as stream:
            process = runner(command, stdout=stream, stderr=subprocess.STDOUT, check=False,
                             env=dict(os.environ, TMPDIR=str(temporary)))
        if process.returncode:
            detail = log.read_text(errors='replace')
            unsupported = "invalid packing 'none'" in detail
            failure = dict(schema=1,
                status='unsupported_pack_metallibs_none' if unsupported else 'metal_pack_failed',
                source=str(source), output=str(output), declared_shader_chain=chain,
                source_sha256=before_sha, source_unchanged=fingerprint(source) == original,
                tool_version=version, command=command, tool_exit_code=process.returncode,
                log=str(log), archive_published=False, runtime_lookup_tested=False,
                release_qualified=False)
            with failure_manifest.open('x') as stream:
                json.dump(failure, stream, indent=2)
                stream.write('\n')
            reason = ('Installed metal-pack advertises but rejects -pack-metallibs none; '
                      'retain the original archive; no alternate packing or rebuild attempted'
                      if unsupported else 'metal-pack failed with exit ' + str(process.returncode))
            raise RuntimeError(reason + '; see ' + str(log))
        after = archive_info(candidate, hashes=True)
        verify_repack(before, after)
        if fingerprint(source) != original:
            raise ValueError('Source changed during repacking; refusing publication')
        report = dict(schema=1, status='binary_repack_verified_runtime_unqualified',
            source=str(source), output=str(output), declared_shader_chain=chain,
            shader_chain_verification='Caller declaration; no shader inputs are changed or regenerated',
            source_sha256=before_sha, output_sha256=file_hash(candidate), tool_version=version,
            command=command, tool_exit_code=process.returncode, before=before, after=after,
            bytes_removed=before['bytes'] - after['bytes'],
            compatibility_gate=dict(exact_non_metallib_section_bytes=True, advertised_pipeline_count_equal=True,
                metal_archive_open_tested=False, archive_only_pipeline_lookup_coverage_tested=False,
                startup_and_frame_pacing_tested=False, release_qualified=False),
            remaining='Require identical requested pipeline lookups without compiler fallback, then matched startup/gameplay; do not install on structural checks alone.')
        metadata = temporary / 'manifest.json'
        metadata.write_text(json.dumps(report, indent=2) + '\n')
        # Same-filesystem exclusive hard links publish complete files and never
        # replace an output created by another process between our checks.
        os.link(candidate, output)
        try:
            os.link(metadata, manifest)
        except BaseException:
            output.unlink()
            raise
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--shader-chain', required=True)
    parser.add_argument('--execute', action='store_true', help='Run the binary repack; default only inspects metadata')
    args = parser.parse_args(argv)
    if args.execute:
        print(json.dumps(repack(args.source, args.output, args.shader_chain), indent=2))
    else:
        print(json.dumps(dict(action='plan_only_no_output_written', source=str(args.source), output=str(args.output),
                              archive=archive_info(args.source), requested_pack_metallibs='none',
                              compatibility='Unproven until repack and runtime archive-only lookup checks pass'), indent=2))


if __name__ == '__main__':
    main()
