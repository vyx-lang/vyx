# SDK debugger

`vyxc-dap --stdio` launches the SDK's CodeLLDB engine. LLDB owns breakpoints,
threads, stack frames and value handles; the Vyx launcher owns process cleanup.
There is no debugger CLI parser or second variable/output cache in Vyx.

## Build

Build `vyxc-dap` with the current SDK compiler, then run from the repository root:

```powershell
python bootstrap_compiler/scripts/prepare_debug_adapter.py --cxx clang/bin/clang++.exe
```

The source build requires Cargo/Rust, Git and clang++. SDK users need none of
these tools to debug. On Linux use `--cxx /usr/lib/llvm-22/bin/clang++`.
`--archive`, `--source-archive`, `--headers-archive` accept offline archives;
each still has to match its pinned SHA-256. `--target-dir` reuses a Cargo cache.

The script pins CodeLLDB 1.12.3, source commit
`62def434dc22c1d77d4837c4d99cddef7ac3338f`, and its matching
LLVM 22.1.8-codelldb headers/runtime. Downloads, extracted sources and Cargo
outputs stay in ignored build directories. The repository stores the complete
[patch](patches/codelldb-1.12.3-vyx.patch),
[CodeLLDB license](LICENSE-CodeLLDB.txt) and [LLVM license](LICENSE-LLVM.txt).

## Vyx patch

- `variables` respects `start`, `count` and `filter`. Indexed SBValue children
  use direct indexing: requesting element 50,000 does not expand earlier values.
  Container responses report indexed/named counts, including the raw view.
- Windows internal-console output goes through separate named pipes. Original
  DAP stdin/stdout handles are captured before launching the program; its output
  cannot become protocol frames. Readers use bounded buffers and await channel
  capacity, drain before termination, and close on session disposal.
- LLDB bindings use C++17 and the matching LLVM 22 API.
- Debug logging does not dump the launched program's environment.

The installed `MANIFEST.json` records archive, source, patch and executable
hashes. SDK packaging checks the patch and executable hashes and includes all
component notices. A failed build never installs the unpatched executable.

## Selection and verification

Default layout: `bin/debugger/adapter/codelldb[.exe]`, with its sibling LLDB,
Python and formatter assets. A source checkout uses `out/_debug_adapter`.
`VYX_DAP_ADAPTER` can select another CodeLLDB or LLDB-DAP executable;
`VYX_LLDB_DAP` remains compatible. An invalid explicit path fails immediately.

Run the [editor pressure gate](../../probes/gates/editor-industrial/README.md)
and `tests/bootstrap/dap_smoke/run.py` against the packaged entry point.
Upstream sources: [CodeLLDB](https://github.com/vadimcn/codelldb/tree/v1.12.3),
[matching LLDB release](https://github.com/vadimcn/lldb-build/releases/tag/codelldb%2F22.x-74).
Linux builds are supported by the installer; Windows pressure results do not
establish Linux runtime behavior.
