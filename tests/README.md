# Test layout

[文档目录](../docs/README.md) · [编译器构建与验证](../docs/TESTING_GUIDE_ZH.md) · [English verification guide](../docs/TESTING_GUIDE.md)

> 验证当前源码时，必须显式传入以兼容 Release SDK 为 Stage 0、由当前 checkout
> 刚构建的 SDK 编译器（`bootstrap_compiler/out/vyxc.exe`），并使用同一次构建的
> `vyx_compiler_backend` / `vyx_runtime`。具体构建步骤见上方验证指南。

The test tree is organized by execution model instead of historical feature phase.

~~~text
tests/
+- run_all_modules.ps1       stable loose-module and manifest-project entry
+- README.md
+- cases/                    standalone Vyx programs
|  +- conformance/           strict, adversarial, generic, extension and stdlib suites
|  +- interpreters/          standalone interpreter programs
|  +- smoke/common/          compact baseline smoke programs
+- projects/                 integration fixtures rooted at Vyx.toml
+- checks/                   explicit PowerShell contract checks
|  +- bootstrap/             bootstrap-only conformance runners and fixtures
|  +- run_checks.ps1
|  +- diagnostics/
|  +- dci/
|  +- llvm/
|  +- memory/
|  +- mir/
|  +- runtime/
|  +- performance/
+- snapshots/ir/             host-era IR corpus retained for migration
~~~

## Execution contract

- cases contains loose Vyx programs. run_all_modules.ps1 discovers and executes
  every runnable case.
- projects contains manifest integration fixtures. A directory is a project
  test only when it is rooted at tests/projects/**/Vyx.toml; the runner builds
  it with the manifest instead of treating src/main.vyx as a loose module.
- checks contains assertion scripts requiring a bespoke protocol, compiler
  mode, diagnostic comparison, foreign tool, or multi-step setup. It is
  intentionally excluded from run_all_modules.ps1; use checks/run_checks.ps1.
- bootstrap contains compiler-stage fixtures that are intentionally excluded
  from the loose-module sweep; invoke their dedicated runners (or the
  bootstrap gate catalog) instead of compiling the fixture files directly.
- snapshots/ir contains historical golden IR, not executable tests. Its manifest
  can pin a stable snapshot ID separately from a source path, so suite moves do
  not rename historical golden files. These baselines predate the active
  self-hosted compiler and are not a current acceptance gate until regenerated
  and reviewed for it.

## Commands

DCI schema checks require the Python `jsonschema` package. Install the DCI SDK's
`validate` extra in your chosen interpreter and set `VYX_DCI_PYTHON` to its executable.
The `dci_rust_generic` runner also accepts `-Python` explicitly.

Native dependencies may need DLL/shared-library search directories at execution time.
Pass `-NativeRuntimeDirs <directory>` to the module/project runner; this is separate
from `-RuntimeDir`, which selects the matching Vyx SDK runtime. For a vcpkg build,
the native directory is typically `$env:VCPKG_ROOT/installed/x64-windows/bin`.
Missing native libraries remain test failures.

Run these from the repository root.

~~~powershell
# All loose modules and Vyx.toml projects after a fresh SDK compiler build.
$compiler = (Resolve-Path .\bootstrap_compiler\out\vyxc.exe).Path
$runtime = (Resolve-Path .\bootstrap_compiler\out).Path
powershell -ExecutionPolicy Bypass -File tests/run_all_modules.ps1 `
  -BootstrapOnly -BootstrapCompiler $compiler -RuntimeDir $runtime -RuntimeLib vyx_runtime

# Discover explicit contract checks.
powershell -ExecutionPolicy Bypass -File tests/checks/run_checks.ps1 -ListOnly

# Run one contract check or all non-manual checks.
powershell -ExecutionPolicy Bypass -File tests/checks/run_checks.ps1 -Check borrow_invalidation -BootstrapCompiler $compiler
powershell -ExecutionPolicy Bypass -File tests/checks/run_checks.ps1 -All -BootstrapCompiler $compiler
~~~

## AOT gates outside this tree

The current baseline is AOT. The recursive sweep and check dispatcher do not
automatically run the focused gates under `probes/gates/`:

| Gate | Coverage |
|---|---|
| [match-expression](../probes/gates/match-expression/README.md) | Value `match`, guards, payloads, one-time subject evaluation, negative diagnostics |
| [cross-module](../probes/gates/cross-module/README.md) | Imported enum/global/string storage identity and qualified generic return typing |
| [generic_interfaces](../probes/gates/generic_interfaces/README.md) | Versioned AST payload validation, private dependencies, duplicate consumers, long statement lists |
| [compiler-industrial](../probes/gates/compiler-industrial/README.md) | Deterministic cold/warm/incremental scheduling matrix and executable semantics |
| [DCI industrial](../probes/gates/dci-industrial/README.md) | DCI stress and native fixtures with explicit case/platform results |

DCI/class ABI/lowerer changes additionally require `projects/dci_cpp_trait`,
`dci_rust_trait`, `dci_multilang`, `dci_zig_abi`, and affected `probes/gates/dci-*`
contracts. Invoke each project's `run.ps1` with `-BootstrapCompiler $compiler` and
the matching runtime. The retired `dci_spdlog/run.sh` is not a current gate.
Exit 77 is a reported skip, never a pass. Compiler changes also require exact
hello MIR and self-host S2/S3 identity; commands and result boundaries are in
the [verification guide](../docs/TESTING_GUIDE.md#aot-compiler-regression-gates).

## Compatibility filters

run_all_modules.ps1 -PathFilter accepts both canonical paths and historic suite
names during the migration. For example, _strict, _adversarial2,
_generics_industrial, common, and build_system_smoke normalize to their new
locations. New scripts and docs must use canonical paths under cases and
projects.

Generated .cache and target directories are ignored and must not be committed.
