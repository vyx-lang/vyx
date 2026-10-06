# Editor service pressure gates

These gates use real compiler and Zyn source trees over framed stdio. They do
not compile the workspace inside the LSP. Measurements use private memory on
Windows and RSS where private memory is unavailable. Install Python `psutil`
in the interpreter used for the gates.

## Build the current tools

Build the SDK compiler and matching backend/runtime as described in
[AGENTS.md](../../../AGENTS.md). Then use that compiler, which resolves this
tree's standard-library packages, to build editor tools:

```powershell
$env:LLVM_ROOT = (Resolve-Path clang).Path
Push-Location bootstrap_compiler
try {
    & ./out/boot.exe build --target vyxc-lsp -j4
    if ($LASTEXITCODE) { throw 'LSP build failed' }
    & ./out/boot.exe build --target vyxc-dap -j4
    if ($LASTEXITCODE) { throw 'DAP build failed' }
} finally { Pop-Location }
python bootstrap_compiler/scripts/prepare_debug_adapter.py --cxx clang/bin/clang++.exe
```

On Linux use `out/boot`, `LD_LIBRARY_PATH=out`, and the host clang++ path.
Do not substitute an older SDK's standard-library sources when measuring new
allocation behavior. Keep the compiler, backend and runtime from the same build.

## LSP

```powershell
python probes/gates/editor-industrial/measure_lsp.py --lsp bootstrap_compiler/out/vyxc-lsp.exe --workspace bootstrap_compiler --document bootstrap_compiler/src/core/parser.vyx --iterations 100 --semantic-tokens --burst 100 --result .runs/editor-industrial/lsp-compiler.json
python probes/gates/editor-industrial/measure_lsp.py --lsp bootstrap_compiler/out/vyxc-lsp.exe --workspace Zyn --document Zyn/src/ui/Ui.vyx --query Zyn --iterations 100 --semantic-tokens --result .runs/editor-industrial/lsp-zyn.json
python probes/gates/editor-industrial/measure_lsp.py --lsp bootstrap_compiler/out/vyxc-lsp.exe --workspace probes/gates/dci-qt-counter --document probes/gates/dci-qt-counter/src/main.vyx --query Counter --iterations 100 --semantic-tokens --burst 100 --result .runs/editor-industrial/lsp-qt.json
```

The client validates nonempty workspace and document indexes, semantic-token
encoding, diagnostic framing, repeated edits and clean shutdown. It samples
memory every 50 ms while awaiting responses, fails above 1,536 MiB, and limits
growth after ten warm-up edits to 64 MiB. Full per-edit measurements go in the
JSON result. Thresholds are failure guards, not language performance guarantees.
`--burst 100` also sends 100 changes without awaiting individual diagnostics;
the subsequent request must see the last edit within 20 seconds. The server
applies every incremental change in order, coalesces queued analysis, and
flushes pending analysis before a request or after 20 ms of quiet input.

`python probes/gates/editor-industrial/check_lsp.py --lsp bootstrap_compiler/out/vyxc-lsp.exe --workspace Zyn`
checks escaped JSON, a Unicode/space/plus URI, UTF-16 incremental changes and
definition ranges, signature help, stale-version rejection, empty buffers and repeated pull
diagnostics. Existing completion and formatting gates remain under `../lsp/`.
The Qt edit workload uses the [Qt counter](../dci-qt-counter/README.md) source
and its regenerated contract. It measures editor requests separately from the
counter's native build and event-loop checks.

Closed workspace files use declaration-only parsing. Open documents additionally
run Sema. Index text belongs to the server; ASTs, tokens, temporary strings,
container storage and checked-pointer metadata belong to a request scope.
Generated directories are excluded, saved unchanged buffers reuse diagnostics,
and closing a document releases its editor text and restores its disk index.

### Cross-module bindings

```powershell
python probes/gates/editor-industrial/check_lsp_semantic.py --lsp bootstrap_compiler/out/vyxc-lsp.exe --compiler bootstrap_compiler/out/boot.exe --result .runs/editor-industrial/semantic.json
python probes/gates/editor-industrial/check_lsp_packages.py --lsp bootstrap_compiler/out/vyxc-lsp.exe --result .runs/editor-industrial/packages.json
python probes/gates/editor-industrial/measure_lsp_bindings.py --lsp bootstrap_compiler/out/vyxc-lsp.exe --workspace bootstrap_compiler --document bootstrap_compiler/src/core/parser.vyx --result .runs/editor-industrial/bindings-compiler.json
```

The first gate checks exact bindings in a six-file project, applies renames and
builds/runs the edited project using native AOT. The second queries `parse_unit`
in the real SDK compiler project and repeats the query 25 times. It checks that
rename refuses the obsolete `probe_emit.vyx` source with missing backend APIs,
then prepares a safe `parser_auto_qualify_std_modules` rename across at least
three compiler source files. It enforces the same memory guards
as sustained edits and records executable identity and cold/cached timings.

Binding queries type candidate files individually, cache compact facts keyed
by source revision and query coverage, and discard frontend ASTs between files.
Imported explicit signatures omit bodies; inferred signatures retain real
return-type analysis. Lexical inventories only select candidates. Binding
identity and rename validation come from Sema. `VYX_LSP_TRACE=<file>` records
analysis diagnostics; `VYX_LSP_BIND_TRACE=phases` writes file phases to stderr.
Use these only while diagnosing a query.

The package gate checks a multi-file standard module, excludes a competing
legacy copy, and verifies an explicit provider's read-only declarations.
Standard-library imports select one complete provider: `VYX_STD_PACKAGES`, then
the workspace's canonical packages, then the SDK and current-directory fallbacks.
Legacy `std/` is used only if no canonical provider has a registry. Relative and
absolute paths share one index entry. Installed SDK copies remain navigable but
are not merged with the workspace's standard library during semantic analysis.

## DAP

```powershell
python tests/bootstrap/dap_smoke/run.py bootstrap_compiler/out/boot.exe bootstrap_compiler/out/vyxc-dap.exe
python probes/gates/editor-industrial/measure_dap.py --adapter bootstrap_compiler/out/vyxc-dap.exe --cxx clang/bin/clang++.exe --result .runs/editor-industrial/dap.json
```

The Vyx smoke gate checks source breakpoints, arguments, typed hover/watch,
single stepping, output arriving while the program is still running, termination
and disconnect. Build the debuggee with `-g -O0`.

The native fixture has 100,000 STL records, a 32-level stack, 100 stops and about
4 MiB of output. It checks a page starting at element 50,000, nested values,
named/indexed filtering, out-of-range pages and globals. Adapter-family memory
excludes the debuggee, fails above 768 MiB and limits warmed-stop growth to
64 MiB. Output is consumed as a stream rather than retained in the test client.

After the pressure run, execute
`python probes/gates/editor-industrial/check_dap_cleanup.py --adapter bootstrap_compiler/out/vyxc-dap.exe --program .runs/editor-industrial/native_debuggee.exe`.
This checks invalid explicit adapter paths, normal disconnect, protocol EOF and
forced wrapper exit, including absence of surviving session descendants.

## Packaging

`bootstrap_compiler/scripts/package_sdk.ps1` includes the verified debugger
runtime. It uses the manifest's primary compiler artifact by default, so an
old `out/vyxc.exe` held open by an IDE cannot replace the fresh `out/boot.exe`.
`-CompilerPath` selects an explicit compiler. Run these gates against the SDK's
`bin/vyxc-lsp` and `bin/vyxc-dap` before publishing a package.

Compiler/runtime changes also require hello, self-host fixed point and relevant
DCI gates. Record actual hashes, commands and platform results in local result
JSON; a passing Windows run does not count as a Linux run.
