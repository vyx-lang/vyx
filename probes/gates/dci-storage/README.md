# DCI local object storage regression

Run from the repository root after building the SDK compiler from current source
at `bootstrap_compiler/out/vyxc.exe` with its matching backend/runtime:

```powershell
./probes/gates/dci-storage/run.ps1 -Compiler bootstrap_compiler/out/vyxc.exe
```

The probe consumes the measured `BigState` contract from `dci_complex_abi`.
Its native producer preserves the four `i64` fields, 32-byte layout and C++
constructor/destructor symbols, and records every allocation, destruction and
release. Only allocation calls in the emitted program are intercepted; the
application runtime and native producer keep their own allocators. The tracker
also covers LLVM's conversion of `malloc` plus zeroing into `calloc` at `-O2`.

The executable checks repeated scopes, early returns, direct constructor
reassignment, borrowed pointer calls, and loop `continue`/`break` cleanup. A
release before destruction, a repeated destructor or an unknown/repeated release
aborts the executable. The live allocation count must return to zero.

## Measured results — 2026-09-30

| Compiler | Allocations | Destructors | Releases | Result |
|---|---:|---:|---:|---|
| Before fix, first repeated-scope check | 2048 | 2048 | 0 | Exit 2, 2048 live allocations |
| Fixed, `-O0`, complete probe | 2056 | 2056 | 2056 | Exit 0 |
| Fixed, `-O2`, complete probe | 2056 | 2056 | 2056 | Exit 0 |

`llvm_lifetime.vyx` owns the allocation/drop emission extracted from
`llvm_lower.vyx`. DCI complete objects use the generated module's `malloc/free`
pair. The implementation avoids the general class arena for DCI complete
objects; the acceptance baseline here is AOT, and JIT parity is deferred.
Storage is released after a DCI destructor only for pointer-represented owning
locals. Stable inline records, borrowed parameters, descriptor lifecycle
operations and native subclass factory destruction retain their existing paths.

## Historical build verification at the storage repair

The following hashes and project results identify the repair stages; they are
not hashes of the newest repository SDK compiler. Subsequent compiler changes
must rebuild and rerun their own Gate A/B/C.

The extraction was verified before the semantic fix:

- Hello IR SHA-256, unchanged:
  `7C98411BB094C939533A0942A785063B010D20D677585CE4FBDB7BCFA11F7BF0`.
- Complex DCI ABI `-O0` IR SHA-256, unchanged:
  `88A97E74C1CF6A6DED1A9BF4C9882BFF196B24CB252608100085A5CEB20AFBDB`.
- Extraction S2/S3 SDK compiler SHA-256:
  `4170D6EF828AAD66BC94E73EEA261A104DC521246CD55513244C0D84DAAD2D26`.

The storage repair's final S1/S2/S3 SDK compiler SHA-256 was identical:
`373236990161571796C6B230FF30009C71E58F8277F0F61895400A3373ED8115`.
Hello MIR remains `functions=1 blocks=1 locals=1 places=1 values=6 instrs=2
cases=0 types=5`, and its IR hash remains unchanged.

DCI project validation passed: `dci_cpp_trait`, `dci_rust_trait`,
`dci_multilang`, `dci_zig_abi`, `dci_complex_abi`,
`extern_cpp_overalign_abi`, and `extern_cpp_mir_abi` at `-O2`. Historical JIT
checks at this stage do not change the current AOT acceptance scope.
The C++ trait gate was rerun successfully after an initial subprocess startup
failure during concurrent SDK compiler replacement.

Additional gates passed: `dci-failure`, `dci-inalloca`, `dci-landingpad`,
and `dci-active`. Two existing Qt gates failed: `dci-qt` during Adapter
strict layout validation, before invoking the SDK compiler; `dci-qt-counter` at
`QApplication`/`QString` constructor binding. The saved pre-fix SDK compiler
reproduces the same Counter binding diagnostics against the freshly generated
contract.

Validation used the repository's Windows LLVM 22.1.1, the Release SDK compiler
solely as the initial bootstrap seed, and the freshly built SDK compiler as the
tested compiler.
The historical remote P0a ref is absent; the repair branch retains current
source and its local P0a ancestor `265a8533d0074f76dc77d752d7ef3b5ad418338c`.

For constructor failure, inline records, throwing destruction and shared
propagation, use the separate [AOT exception gate](../dci-exceptions/README.md).
Borrowed objects and producer allocator domains must not receive the consumer's
local storage release.
