#!/bin/bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    printf 'Usage: bash %s <output folder>\n' "$0" >&2
    exit 1
fi
here="$(cd -- "$(dirname -- "$0")" && pwd -P)"
repo="$(git -C "$here" rev-parse --show-toplevel)"
revision="$(git -C "$repo" rev-parse HEAD)"
mkdir -p -- "$1"
output="$(cd -- "$1" && pwd -P)"
stage="$(mktemp -d "$output/.ascension-installer.XXXXXXXX")"
trap 'rm -rf -- "$stage"' EXIT
export TMPDIR="$stage/"

name='Ascension Installer.app'
app="$stage/$name"
resources="$app/Contents/Resources"
mkdir -p "$app/Contents/MacOS" "$resources"

version=1.3
app_build=4
printf '{"version":"%s","build":"%s","revision":"%s"}\n' "$version" "$app_build" "$revision" > "$resources/Release.json"
/bin/cp "$here/install.py" "$resources/"

# Bundle only the committed build tools, this game profile, and its source dependencies.
# Reading Git trees works in sparse checkouts and excludes local caches and app bundles.
git -C "$repo" archive --format=tar --prefix=payload/ "$revision" -- \
    LICENSE .gitignore scripts dm/.gitignore dm/gfx dm/host dm/ios dm/mac/Launcher.swift \
    dm/mac/launcher.c dm/mac/min_os.mk dm/mac/pack_app.py dm/tools dm/third_party \
    dm/games/README.md dm/games/game.py dm/games/profile_guess.py dm/games/settings.py \
    dm/games/settings.tsv dm/games/0400486024DEC000 \
    ':(exclude)dm/tools/nx_extract_bin' ':(exclude)dm/tools/tests' \
    ':(exclude)dm/gfx/tests' ':(exclude)dm/host/tests' |
    python3 -c 'import lzma, shutil, sys
with lzma.LZMAFile(sys.stdout.buffer, "wb", preset=6) as output:
    shutil.copyfileobj(sys.stdin.buffer, output)' > "$resources/ascension-source.tar.xz"

cat > "$app/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleIdentifier</key><string>org.nxnative.ascension.installer</string>
    <key>CFBundleDisplayName</key><string>Ascension Installer</string>
    <key>CFBundleName</key><string>Ascension Installer</string>
    <key>CFBundleExecutable</key><string>AscensionInstaller</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>$version</string>
    <key>CFBundleVersion</key><string>$app_build</string>
    <key>LSMinimumSystemVersion</key><string>26.6</string>
    <key>NSHighResolutionCapable</key><true/>
    <key>NSRemovableVolumesUsageDescription</key><string>Read your NUP and install Ascension on the drive you choose.</string>
</dict>
</plist>
PLIST
/usr/bin/xcrun --sdk macosx swiftc -swift-version 6 -O -parse-as-library \
    -target arm64-apple-macos26.6 -module-cache-path "$stage/module-cache" \
    "$here/Installer.swift" -o "$app/Contents/MacOS/AscensionInstaller"
/usr/bin/codesign --force --sign - "$app"
/usr/bin/codesign --verify --deep --strict "$app"
/usr/bin/ditto -c -k --keepParent "$app" "$stage/Ascension-Installer.zip"
/bin/mv -f "$stage/Ascension-Installer.zip" "$output/Ascension-Installer.zip"
printf 'Built %s\n' "$output/Ascension-Installer.zip"
