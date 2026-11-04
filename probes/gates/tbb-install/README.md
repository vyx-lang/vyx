# Loaded oneTBB installation

Run on Linux or macOS with the current source-built compiler, its matching
runtime/backend and the oneTBB library from that build:

```sh
python3 probes/gates/tbb-install/run.py --compiler bootstrap_compiler/out/boot \
  --library bootstrap_compiler/out/libtbb.so --output .cache/tbb-install
```

On macOS, select `out/libtbb.12.dylib`. Set `VYX_RUNTIME_ROOT`,
`VYX_STD_PACKAGES` and the loader paths for the selected compiler as for a
normal source build. The output directory must be new.

The gate compiles the actual `runtime/scripts/prepare_tbb.vyx` hook at O0/O2.
Each isolated parent loads a real oneTBB library, holds its original inode open,
executes the hook twice, calls the loaded ABI again, and unloads the library.
The compiler invocation cases run the hook through `--run=aot --src=file`,
reproducing the process nesting used by project prebuild hooks. A valid binary
with an appended marker exercises changed bytes without a substitute ABI.

The 16 checks cover identical content, changed content, destination/source
symlinks, a missing output and an obstructed output. Identical files retain their
inode and modification time. Updates publish a new inode, preserving bytes
visible through the old descriptor and any destination symlink's referent.
Installation failure preserves the existing output and reports an error.
Temporary files must be removed. Commands, worker exits, hook/compiler/library
hashes and results are saved in `verification.json` and per-case logs.

Native Linux and macOS release jobs run this gate before tool builds. The
Windows DLL deployment contract is covered by the
[runtime companion gate](../runtime-companions/README.md).
