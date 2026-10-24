# Host runtime target selection

An explicit macOS deployment triple such as `aarch64-apple-macosx15.0`
must select the matching host runtime when LLVM reports `arm64-apple-darwin24.*`.
The gate tests the actual backend's parsed ABI comparison, rejects different
architectures, Apple platforms, environments and object formats, then compiles
and runs a StringBuilder program with both implicit and explicit host targets,
through the single-file and project paths. A foreign CPU project must be
rejected before execution. Neither path manually adds a runtime archive.

```sh
python probes/gates/host-runtime-targets/run.py --backend /path/to/libvyx_compiler_backend.dylib --compiler /path/to/vyxc --triplet aarch64-apple-macosx15.0 --output /tmp/runtime-target-gate
```

Use the matching `.dll` / `.so` on Windows / Linux. The triple defaults to the
backend's host triple when omitted. Deployment minimums are enforced by the
runtime objects and native linker; ABI compatibility does not promise support
for an older macOS version than the SDK's documented minimum.
