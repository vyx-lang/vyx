# Contract checks

[Documentation](../../docs/README.md) · [Verification](../../docs/TESTING_GUIDE.md)

`tests/checks/` contains explicit PowerShell checks that are intentionally
separate from the recursive module/project sweep in `../run_all_modules.ps1`.
They are grouped by the compiler contract they validate:

- `bootstrap/` — bootstrap-only parser/module-registry checks; source fixtures
  stay under `tests/bootstrap/` and are not loose modules.
- `diagnostics/` — diagnostic and self-host consistency matrices.
- `llvm/` — LLVM lowering and ABI contracts.
- `mir/` — MIR analysis and optimization passes.
- `memory/` — allocation, ARC, and automatic-storage contracts.
- `runtime/` — runtime ownership and iterator behavior.
- `language/` — tutorial and language-level integration checks.
- `dci/` — native DCI adapter contract checks.
- `vyi/` — interface/layout contract checks whose fixture owns a project.
- `performance/` — manually requested comparison benchmarks.

Discover canonical names:

```powershell
powershell -File tests/checks/run_checks.ps1 -ListOnly
```

Run one check while forwarding its native arguments:

```powershell
powershell -File tests/checks/run_checks.ps1 `
  -Check class_auto_stack_local `
  -BootstrapCompiler $compiler
```

`$compiler` must refer to the SDK compiler freshly produced from the current
checkout at `bootstrap_compiler/out/vyxc.exe`, using a compatible Release SDK
as Stage 0 in the [verification guide](../../docs/TESTING_GUIDE.md). Use
`vyx_compiler_backend` / `vyx_runtime` from that same build.

The reference receiver contract is registered as
`reference_receiver_ergonomics`; it covers `&T`/`&mut T` member and index
access, typed-pointer `->`, and explicit `new`/`delete` across LLVM and
MIR2CPP.

Run one suite:

```powershell
powershell -File tests/checks/run_checks.ps1 -Suite mir
```

`-All` runs the `targeted` tier; `-IncludeManual` additionally includes long
performance and diagnostic matrices.

## Separate AOT compiler gates

This dispatcher does not include every repository regression. Invoke the
[match-expression](../../probes/gates/match-expression/README.md),
[cross-module](../../probes/gates/cross-module/README.md),
[generic interface](../../probes/gates/generic_interfaces/README.md), and
[compiler pressure](../../probes/gates/compiler-industrial/README.md) entry points
directly with `-Compiler $compiler` after relevant frontend, HIR/MIR, symbol, or
build-system changes. Generic gates include tampered artifact rejection, private
helper access rejection, long flat lists, and independently compiled consumers
linked into one executable.

DCI/class ABI/lowerer acceptance also needs the real `tests/projects/dci_*`
fixtures and the affected `probes/gates/dci-*` contracts. IR verification and an
FFI-only program do not substitute for that compile/link/run path. See the
[current Gate A/B/C commands](../../docs/TESTING_GUIDE.md#aot-compiler-regression-gates).
JIT parity remains separate future work; current AOT passes do not certify it.
