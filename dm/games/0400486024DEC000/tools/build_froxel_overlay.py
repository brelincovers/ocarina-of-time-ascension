#!/usr/bin/env python3
"""Recompile the v131072 froxel kernel with its 512-thread launch constraint."""
import subprocess
import sys
import tempfile
from pathlib import Path

dm = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(dm / "tools"), str(dm / "gfx/tools")]
from romfs_image import read_index
from dmsh_chain import MAC_METAL, version

key = "9ed48d1011f1b48b_b"
image, output = map(Path, sys.argv[1:])
if image.name != f"b-{version()}.img":
    raise SystemExit("Expected a single-shader source image from the current chain")
is_dir, offset, size = read_index(image)[f"{key}.metal"]
assert not is_dir
with image.open("rb") as stream:
    stream.seek(offset)
    source = stream.read(size).decode()
entry = "kernel void main0("
assert source.count(entry) == 1
output.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(dir=output) as temp:
    work = Path(temp)
    msl = work / "froxel.metal"
    air = work / "froxel.air"
    lib = work / f"{key}.metallib"
    msl.write_text(source.replace(entry, "[[max_total_threads_per_threadgroup(512)]] " + entry))
    subprocess.run([*MAC_METAL, "-fbracket-depth=1024", str(msl), "-o", str(air)], check=True)
    subprocess.run(["xcrun", "-sdk", "macosx", "metallib", str(air), "-o", str(lib)], check=True)
    lib.replace(output / lib.name)
