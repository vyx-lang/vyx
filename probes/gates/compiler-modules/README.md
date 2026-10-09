# Compiler module contracts

After building the SDK compiler from the current source, run at the repository root:

```powershell
./probes/gates/compiler-modules/run.ps1 -Compiler bootstrap_compiler/out/vyxc.exe
```

The runner builds and executes `tests/projects/impl_module_contract` with separate
module objects. Cold and warm runs must print `42`; the warm build must compile
no tasks. Changing the secondary implementation file and expected result must
recompile the counter module and print `43`.

Public interfaces must contain both `Counter` implementation blocks and exclude
its private helper. Neither the private `PrivateCounter` type nor its inherent
implementations may appear, even when an implementation has `@[vis(world)]`.
The runner checks shallow and full `.vyi` emission, and verifies that
`--vyi-project-private` still includes these private declarations. Qualified
external owners and primitive owners retain their public method signatures in
shallow and full interfaces; shallow emission precedes import resolution.

Isolated consumers that call `hidden`, or construct `PrivateCounter` and call
`amplify`, must fail with a diagnostic naming the inaccessible member or type.
The current missing-method diagnostic is emitted by LLVM lowering (`I0100`),
after frontend analysis.

The fixture has a type definition and two implementation files in one logical
module. Public method roots must include all three files. No generated C++ or
JIT path is used. Logs and compiler identity are written to ignored `.runs/`;
`-OutputDir <new-directory>` selects another result directory.
