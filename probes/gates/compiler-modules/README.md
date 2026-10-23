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

The current source baseline still has two failures in this older fixture:
template method bodies may be emitted in both the consumer and defining object,
causing duplicate symbols, and a private inherent helper may be callable from
the isolated consumer. The cold/warm/incremental and helper-negative checks
remain required checks and currently fail; a passing scale gate below does not
certify that these separate module-boundary defects are fixed.

Isolated consumers that call `hidden`, or construct `PrivateCounter` and call
`amplify`, must fail with a diagnostic naming the inaccessible member or type.
The current missing-method diagnostic is emitted by LLVM lowering (`I0100`),
after frontend analysis.

Export and emission root lists are also checked as build inputs: nonexistent
`--export-root-names-file` and `--emit-root-names-file` paths must fail without an
object artifact and name the missing file. Existing empty lists remain valid;
nonempty lists must emit the selected function body.

## Symbol-index growth

```powershell
python probes/gates/compiler-modules/check_scale.py --compiler bootstrap_compiler/out/vyxc.exe --out .cache/module-scale
```

This generated gate crosses the old 2048-entry function-index and 1024-entry
class-index scratch capacities, including a second growth. It checks parsing
separately from binding and native execution. Every generated function is called
and every generated class is constructed; checking only a few endpoints can
miss a prefix lost during growth. The largest cases also run at O2 with default
and four CGUs, and the function interface must retain every exported signature.

A project splits 4097 functions across three files of one named module and
calls all of them from another module. Same-named functions in a third module
check qualified lookup isolation. Cold, warm and incremental build/run paths
must succeed; the warm path must compile no tasks, while changing a function
in the secondary source must recompile the logical module and change the
expected result. Generated inputs, source hashes, compiler identity, process
limits and results remain in the selected local output directory. The runner
refuses to overwrite an existing result directory.

`--functions` and `--classes` select positive count lists for a focused
reproduction; `--skip-project` skips and does not certify the project path.
The default counts are test boundaries, not compiler limits.

The fixture has a type definition and two implementation files in one logical
module. Public method roots must include all three files. No generated C++ or
JIT path is used. Logs and compiler identity are written to ignored `.runs/`;
`-OutputDir <new-directory>` selects another result directory.
