#!/usr/bin/env python3
"""Build and install Ascension from a locally provided Switch 2 NUP."""

import argparse
import ctypes
import fcntl
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import signal
import subprocess
import sys
import tarfile
import tempfile

TITLE_ID = "0400486024DEC000"
PACKAGE_VERSION = 131072
BUNDLE_ID = "org.nxnative.oot.ascension.framegen.latest20261115"
APP_NAME = "Ocarina of Time"
GIB = 1024 ** 3
REQUIREMENTS = [
    "lz4==4.4.5", "capstone==5.0.6", "fonttools==4.68.0", "zbic==1.0.1.post1",
]


class InstallError(Exception):
    pass


def event(kind, **fields):
    print(json.dumps({"ev": kind, **fields}), flush=True)


def run(args, env, log=None):
    if log is None:
        value = subprocess.run(args, env=env, capture_output=True, text=True, errors="replace")
        if value.returncode:
            raise InstallError((value.stderr or value.stdout).strip() or f"{args[0]} exited {value.returncode}.")
        return value.stdout.strip()

    with log.open("a", encoding="utf-8") as output:
        child = subprocess.Popen(
            args, env=env, start_new_session=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, errors="replace",
        )
        assert child.stdout is not None
        try:
            for line in child.stdout:
                output.write(line)
                output.flush()
                try:
                    payload = json.loads(line)
                except ValueError:
                    continue
                if isinstance(payload, dict) and "ev" in payload:
                    print(line.rstrip(), flush=True)
            if child.wait():
                raise InstallError("A build tool stopped. Open the build log for details.")
        finally:
            if child.poll() is None:
                os.killpg(child.pid, signal.SIGTERM)
                try:
                    child.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    os.killpg(child.pid, signal.SIGKILL)
                    child.wait()


def plist(bundle):
    with (bundle / "Contents/Info.plist").open("rb") as data:
        return plistlib.load(data)


def existing_app(bundle):
    if bundle.is_symlink():
        raise InstallError("Choose a folder with a real Ascension app or no app at that name.")
    if not bundle.exists():
        return None
    try:
        info = plist(bundle)
    except (OSError, ValueError, plistlib.InvalidFileException) as error:
        raise InstallError("This folder already contains an app at the destination name.") from error
    if info.get("CFBundleIdentifier") != BUNDLE_ID or info.get("DMTitleID") != TITLE_ID:
        raise InstallError("This folder already contains a different Ocarina of Time app. Choose another folder.")
    state = bundle.stat()
    return state.st_dev, state.st_ino, info.get("DMCommit")


def prepare_source(work, resources):
    source = work / "source"
    if not source.is_dir():
        event("stage", name="Preparing build files")
        with tempfile.TemporaryDirectory(prefix="source-", dir=work) as temporary:
            with tarfile.open(resources / "ascension-source.tar.xz", "r:xz") as archive:
                archive.extractall(temporary, filter="data")
            # The source is only exposed under its stable path after full extraction.
            contents = Path(temporary) / "payload"
            contents.rename(source)
    return source


def check_nup(nup, source):
    sys.path[:0] = [str(source / "scripts"), str(source / "dm/games")]
    from game import load
    from pack_nup import Error, Nup, NO_DIR, scan

    event("stage", name="Checking your NUP")
    try:
        container = Nup(nup)
        try:
            title, base, updates, _ = scan(container)
            if f"{title:016X}" != TITLE_ID:
                raise InstallError("Select a Switch 2 Ocarina of Time NUP.")
            update = updates.get(PACKAGE_VERSION)
            if update is None:
                raise InstallError(f"The NUP needs the complete v{PACKAGE_VERSION} update.")
            registered = load(TITLE_ID).builds()[f"v{PACKAGE_VERSION}"]
            exefs = update.exefs if update.exefs != NO_DIR else base.exefs if base else NO_DIR
            if exefs == NO_DIR:
                raise InstallError("This NUP is missing its game program files.")
            for module, expected in registered.items():
                if module == "display":
                    continue
                index = container.child(exefs, module.encode("ascii"))
                header = container.head(index, 0x50) if index is not None else b""
                if header[:4] != b"NSO0" or header[0x40:0x50].hex() != expected:
                    raise InstallError(f"The NUP’s {module} does not match the supported Ascension build.")
        finally:
            container.close()
    except (Error, OSError) as error:
        raise InstallError(str(error)) from error
    event("say", msg=f"Verified title {TITLE_ID}, update v{PACKAGE_VERSION}, and registered module IDs.")


def check_tools(source, env):
    missing = [tool for tool in ("zstd", "glslangValidator") if not shutil.which(tool, path=env["PATH"])]
    if missing:
        raise InstallError("Install the missing Homebrew tools with: brew install zstd glslang")
    try:
        version = run(["/usr/bin/xcrun", "-sdk", "macosx", "metal", "--version"], env)
    except InstallError as error:
        raise InstallError(
            "Full Xcode and its Metal Toolchain are required. In Xcode, open Settings → Components, "
            "then install Metal Toolchain and try again."
        ) from error
    locked = dict(
        line.split("\t", 1) for line in (source / "dm/gfx/tools/dmsh_chain.lock").read_text().splitlines()
        if line and not line.startswith("#")
    )["metal_macosx"]
    if locked in version:
        return []
    if re.search(r"Apple metal version 32023\.925\b", version):
        event("say", msg="Using the Ascension Xcode 27.3 Metal toolchain.")
        return ["--allow-other-metal"]
    raise InstallError("Select the Xcode 27.2 Metal toolchain or Ascension’s Xcode 27.3 toolchain.")


def install(candidate, destination, previous, version, revision, env, log):
    event("stage", name="Signing and checking the new app")
    info = plist(candidate)
    if info.get("CFBundleIdentifier") != BUNDLE_ID or info.get("DMTitleID") != TITLE_ID or not info.get("DMChain"):
        raise InstallError("The generated app has an unexpected profile or shader chain.")
    info.update(CFBundleShortVersionString=version["version"], CFBundleVersion=version["build"], DMCommit=revision)
    (candidate / "Contents/Info.plist").write_bytes(plistlib.dumps(info))
    run(["/usr/bin/codesign", "--force", "--sign", "-", candidate], env, log)
    run(["/usr/bin/codesign", "--verify", "--deep", "--strict", candidate], env, log)

    if existing_app(destination) != previous:
        raise InstallError("The installed app changed while this build was running. Try again.")
    processes = run(["/bin/ps", "-Aww", "-o", "comm="], env).splitlines()
    installed_prefix = str(destination / "Contents/MacOS") + "/"
    if any(process.strip().startswith(installed_prefix) or process.strip() in {"nxnative", "dmhost"}
           for process in processes):
        raise InstallError("Quit Ocarina of Time, then retry to finish installing the prepared app.")

    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    event("installing", name="Installing Ascension")
    exchange = ctypes.CDLL(None, use_errno=True).renamex_np
    exchange.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint]
    exchange.restype = ctypes.c_int
    rename_swap, rename_exclusive = 0x00000002, 0x00000004
    flags = rename_swap if previous else rename_exclusive
    if exchange(os.fsencode(candidate), os.fsencode(destination), flags):
        error = ctypes.get_errno()
        raise InstallError(f"The install folder could not be updated: {os.strerror(error)}.")


def main(args):
    resources = Path(__file__).resolve().parent
    release = json.loads((resources / "Release.json").read_text())
    revision = release["revision"]
    if not re.fullmatch(r"[a-f0-9]{40}", revision):
        raise InstallError("The installer’s source revision is invalid.")
    nup = args.nup.resolve(strict=True)
    folder = args.folder.resolve(strict=True)
    if not nup.is_file() or nup.suffix.lower() != ".nup" or not folder.is_dir():
        raise InstallError("Select a NUP file and an installation folder.")
    with nup.open("rb") as file:
        header = file.read(128)
    if len(header) != 128 or header[:4] != b"NUP\x01":
        raise InstallError("The selected file is not a complete NUP container.")
    key = hashlib.sha256(header).hexdigest()[:16]
    if args.check:
        with tempfile.TemporaryDirectory(prefix=".ascension-check-", dir=folder) as temporary:
            check_nup(nup, prepare_source(Path(temporary), resources))
        return

    destination = folder / f"{APP_NAME}.app"
    previous = existing_app(destination)
    workspace = folder / f".ascension-build-{revision[:12]}-{key}"
    if workspace.is_symlink():
        raise InstallError("Move the existing build folder link and retry.")
    try:
        workspace.mkdir()
    except FileExistsError:
        if not (workspace / ".ascension-installer").is_file():
            raise InstallError("The build folder already exists and belongs to another process. Choose another folder.") from None
    else:
        (workspace / ".ascension-installer").touch()
    tmp = workspace / "tmp"
    tmp.mkdir(exist_ok=True)
    if tmp.is_symlink():
        raise InstallError("The build workspace is invalid. Choose another folder.")
    log = workspace / "build.log"
    event("log", path=str(log))
    env = {k: v for k, v in os.environ.items() if not k.startswith(("DM_", "DMSH_"))}
    env.update(TMPDIR=str(tmp), PYTHONDONTWRITEBYTECODE="1", PIP_NO_CACHE_DIR="1")
    with (workspace / ".lock").open("a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise InstallError("This NUP is already being built in that installation folder.") from error
        source = prepare_source(workspace, resources)
        check_nup(nup, source)
        event("stage", name="Checking Xcode and build tools")
        metal = check_tools(source, env)
        extracted = workspace / "game/extract"
        minimum = nup.stat().st_size + 10 * GIB
        if not extracted.is_dir():
            minimum += nup.stat().st_size
        if shutil.disk_usage(folder).free < minimum:
            raise InstallError(f"This installation folder needs about {minimum / GIB:.0f} GiB of free space.")
        venv = workspace / "python"
        python = venv / "bin/python"
        if not (venv / ".ready").exists():
            event("stage", name="Installing Python build packages")
            run([sys.executable, "-m", "venv", venv], env, log)
            run([python, "-m", "pip", "install", "--disable-pip-version-check", "--no-input", *REQUIREMENTS], env, log)
            (venv / ".ready").touch()
        candidate = workspace / destination.name
        run([
            python, source / "scripts/build_ipa_from_nsp.py", nup, "--platform", "mac-app",
            "--title-id", TITLE_ID, "--package", f"v{PACKAGE_VERSION}", "--no-seen-shaders",
            "--bundle-id", BUNDLE_ID, "--app-name", APP_NAME, "--progress", "json",
            "--work-dir", workspace / "game", "--out", candidate, *metal,
        ], env, log)
        install(candidate, destination, previous, release, revision, env, log)
        # After the atomic exchange this folder owns both the intermediates and the displaced app.
        # Release the lock only after removing them, so a retry cannot reuse the old candidate.
        try:
            shutil.rmtree(workspace)
        except OSError as error:
            event("say", msg=f"Installed, but could not clear the build workspace: {error}")
    event("installed", path=str(destination), version=release["version"])


def cancelled(_signum, _frame):
    raise InstallError("Build cancelled. Select the same NUP and folder again to resume.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("nup", type=Path)
    parser.add_argument("folder", type=Path)
    parser.add_argument("--check", action="store_true", help="Validate NUP title and modules without building.")
    signal.signal(signal.SIGTERM, cancelled)
    try:
        if sys.version_info < (3, 14):
            raise InstallError("Use Homebrew Python 3.14 or later, which includes the NUP’s Zstandard reader.")
        main(parser.parse_args())
    except (OSError, ValueError, InstallError, tarfile.TarError) as issue:
        event("error", msg=str(issue))
        sys.exit(1)
