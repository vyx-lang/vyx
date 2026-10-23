# SDK releases

`SDK release` runs on each push to `main`. It builds Windows x64, Linux x64,
Linux AArch64 and Android/Termux AArch64. Manual runs accept a compatible
Stage 0 release tag; the default resolves the latest published SDK. Pin a tag
for reproducing a previous bootstrap input.

The native builds use LLVM 22 and a checksum-verified Stage 0 SDK. Stage 0 uses
its own matching standard library while building the current compiler source:
the October Linux SDK cannot compile the newer typed container drop helpers.
The new SDK packages the checkout's `std_packages`, and the newly
built compiler builds LSP and, on x64, DAP. CodeLLDB comes from the verified
Stage 0 bundle. AArch64 Linux has no DAP bundle. Termux is cross-built using
the new Linux SDK and Android NDK r28c; its existing device LLVM 21 and oneTBB
payloads are retained, with private ELF dependency isolation verified by the
Termux packager. Android execution still requires a device.

Each compiler build permits one Vyx child process and two memory slots. Native
compilation uses four workers. Build caches are not shared between platforms.

All four archives, their SHA-256 sidecars and source identities must be present
before publishing. The tag reads `[package].version` from `Vyx.toml` and appends
the publication date in UTC+8: `1.0.0-alpha.1.26.10.23`. Further builds that day
use `.2`, `.3`, etc. Tag creation is atomic; existing tags and assets are not
replaced. Uploads first go to a draft Release. A failed upload leaves that draft
for inspection; rerunning allocates a new tag. A completed build of an older
commit does not replace the latest main SDK release.

Run locally on the corresponding native host (Python 3.12, PowerShell 7,
LLVM 22 development files, CMake, Ninja, Git; MSVC/Windows SDK on Windows):

```sh
python bootstrap_compiler/scripts/release/build_native.py --llvm-root /usr/lib/llvm-22 --work /tmp/vyx-build --output ./release-assets --jobs 4
python bootstrap_compiler/scripts/release/build_termux.py --host-sdk ./release-assets/vyx-sdk-linux-x86_64-llvm22 --ndk /opt/android-ndk-r28c --work /tmp/vyx-termux --output ./release-assets --jobs 4
python -m unittest discover -s bootstrap_compiler/scripts/release -p 'test_*.py'
```

Use new work/output directories for each build. `--bootstrap-sdk` and
`--base-package` allow predownloaded bootstrap inputs. `GH_TOKEN` is optional
for public downloads but avoids GitHub API rate limits. Publishing requires a
repository token with `contents: write`; it is only granted to the publish job.
SDK files are distributed through GitHub Releases.

Termux's Debian version preserves an epoch when needed to upgrade from the
historical `1.0.5~android21...` package to the actual `1.0.0-alpha.1` version.
Its control version includes a UTC build timestamp; the release tag uses UTC+8.
