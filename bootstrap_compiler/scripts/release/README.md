# SDK releases

`SDK release` runs on each push to `main`. It builds Windows x64, Linux x64,
Linux AArch64 and Android/Termux AArch64. Manual runs accept a compatible
Stage 0 release tag; the default resolves the latest published SDK. Pin a tag
for reproducing a previous bootstrap input.

The debugger patch uses LF on every host through `.gitattributes`. CodeLLDB's
platform, source patch, bundled patch and executable hashes are checked before
compilation; packaging repeats its source patch and executable checks. A hash
mismatch fails without rewriting the manifest or accepting different bytes.

The native builds use LLVM 22 and a checksum-verified Stage 0 SDK. Stage 0 uses
its own matching standard library while building the current compiler source:
the October Linux SDK cannot compile the newer typed container drop helpers.
The build uses an isolated committed source snapshot, and its bootstrap-only
manifest points `parallel_std_sources` at the Stage 0 SDK's `std`. Environment
variables alone do not override that explicit source list. The manifest is
restored before the current compiler builds tools or the SDK is packaged.
The native C++ bridge is built first. Stage 0 then loads that bridge from a
separate directory while keeping its own executable and application runtime.
This prevents old Stage 0 bridges from embedding the build runner's optional
CPU instructions in a portable SDK. A baseline-ISA emission gate runs before
the compiler build, and a packaged SDK must build and run a StringBuilder
project before its archive is accepted. The bootstrap bridge hash is recorded
in `SDK-BUILD.*.json`.
The new SDK packages the checkout's `std_packages`, and the newly
built compiler builds LSP and, on x64, DAP. CodeLLDB comes from the verified
Stage 0 bundle. AArch64 Linux has no DAP bundle. Termux is cross-built using
the new Linux SDK and Android NDK r28c; its existing device LLVM 21 and oneTBB
payloads are retained, with private ELF dependency isolation verified by the
Termux packager. The downloaded Linux host SDK must pass the same project
build/run gate on the Android build runner before cross-compilation starts.
Android execution still requires a device.

Each compiler build permits one Vyx child process and two memory slots. Native
compilation uses four workers. Build caches are not shared between platforms.
Windows LLVM preparation streams the XZ archive once and extracts only headers,
static libraries, Clang resources and required tools. Prepared development files
are cached by LLVM archive identity. Dependency preparation has a 15-minute
limit; compiler steps print elapsed time every 30 seconds and retain failure logs.
The compile step has a 150-minute limit within the 180-minute native job, leaving
time to upload diagnostics when compilation times out. Android depends on the
Linux x64 job directly; it does not wait for Windows or Linux AArch64.

The native bridge also runs the [string scan gate](../../../../probes/gates/runtime-string-scan/README.md)
before compiling Vyx. Its POSIX compatibility scanner stops at NUL and bounds
page checks by the requested input. A former fixed 1 MiB page walk caused
millions of `mincore` calls during short string operations on Linux. The gate
checks syscall count and read-protected page boundaries on the actual bridge.

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
