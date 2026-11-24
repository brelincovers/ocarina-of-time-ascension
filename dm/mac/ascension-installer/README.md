# Ascension Installer

The installer builds Ocarina of Time Ascension 1.3 for Apple silicon. It includes a snapshot of the matching nxnative build tools and the Ascension game profile. A complete Ocarina of Time Switch 2 NUP with title ID `0400486024DEC000` and update `v131072` supplies the game files.

## First build

1. Install full Xcode and open it once to complete setup. In **Xcode → Settings → Components**, install the **Metal Toolchain**. Use the Metal toolchain recorded in the source snapshot (Xcode 27.2) or the Ascension Xcode 27.3 compiler.
2. Install [Homebrew](https://brew.sh) if needed, then run the one-time tools setup. The installer uses native Python 3.14 or later for the NUP’s Zstandard reader.

   ```sh
   brew install python zstd glslang
   ```

3. Open **Ascension Installer.app**, choose your NUP and destination, and click **Build & Install**. The installer prepares its Python environment automatically. Set aside roughly twice the size of your NUP plus 10 GiB, with more room available for the compiler if needed. Internet access is used for the Python build packages.

During a build, **Open Build Log** opens the local tool output. Retrying the same NUP in the same folder reuses completed work. The installer builds to a staging folder, verifies the app signature, and installs it in the selected folder. Updates swap a completed app into place and discard the displaced bundle after the swap. Existing saves and preferences stay in their normal locations.

The download is ad-hoc signed. macOS may require first-open approval. The resulting game app is also signed locally.

## Build the installer from the checkout

```sh
bash dm/mac/ascension-installer/build.sh ./installer-output
```

The ZIP packages the Swift installer, the Python worker, and the recorded Git revision of the portable build sources. Its inputs are tracked files from a restricted set of source directories. The game app and game files are generated from the NUP on the user's Mac.
