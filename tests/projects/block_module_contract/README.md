# Block module storage contract

This native AOT project checks five logical module scopes in `src/scopes.vyx`:
the root, two siblings, a nested block, and a semicolon module inside a block.
Each scope declares its own `counter` variable and `LIMIT` constant with distinct
initial values. `src/main.vyx` checks qualified reads and writes, bare accesses
inside each module, independent storage, a reopened block, and restoration of
the parent scope after a block closes. Static initializers after the nested and
outer blocks also read the restored scope's `LIMIT`.

Use an SDK compiler freshly built from the current checkout, with its matching
backend and runtime:

```powershell
$compiler = (Resolve-Path ./bootstrap_compiler/out/vyxc.exe).Path
Push-Location ./tests/projects/block_module_contract
try {
    & $compiler --src=project . --run=aot -O2 -j1
    if ($LASTEXITCODE -ne 0) { throw 'block module contract failed' }
} finally { Pop-Location }
```

The executable must print `block module contract OK` and return zero.
The manifest uses project unity compilation. The
[`block-modules.ps1`](../../../probes/gates/cross-module/block-modules.ps1) gate
copies it to isolated result directories and checks both unity and parallel
manifest builds, including a warm build with zero compile tasks. It also checks
single-file and project-unit native compilation at `-O0` and `-O2`, and shallow
and full interface consumers with the producer source archived before import.
Duplicate declarations in one logical module are negative tests, including
reopened blocks and declarations in two non-entry project files. They require
`E2100` at the duplicate declaration, with its source name and line.
