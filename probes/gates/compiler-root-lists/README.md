# Compiler root list inputs

After building the current SDK compiler, run from the repository root:

```powershell
python probes/gates/compiler-root-lists/run.py --compiler bootstrap_compiler/out/vyxc.exe
```

`--export-root-names` selects export roots; `--emit-root-names` filters the
emission partition. Both accept comma-separated names and the project worker's
pipe-separated form. Their `-file` options accept the same syntax. Surrounding
whitespace is trimmed per name. Commas inside generic arguments are preserved:
`--export-root-names "choose<i32,i64>,third"` selects two roots.

The gate checks native LLVM function definitions and CUDA PTX `.entry` symbols,
including one, two and three roots, whitespace, pipe compatibility, mixed
separators, empty lists, generic arguments and file inputs. An unselected
`first_extra` function checks exact name matching. `--export-all` and
`--export-top-level-roots` remain control cases. A native AOT application calls
the selected functions and checks their results at O2.

PTX generation requires the SDK NVPTX backend; it does not require a CUDA device
or toolkit. The separate [CUDA symbol gate](../cuda-symbol-identity/README.md)
checks manifest builds, embedded host symbols, `ptxas` and actual GPU launches
when available. This gate does not certify GPU execution.

Inputs, compiler/runtime hashes, commands and results are written under the
ignored `.cache/<timestamp>/` directory. `--output-dir` selects a fresh directory;
existing evidence is not overwritten.

Release builds also pass `--build-root bootstrap_compiler`. This extracts the
actual root-normalization helper from the driver source, compiles it against the
bootstrap build's shallow standard-library interfaces, and executes it separately
through native AOT. Both inline and heap-sized results must survive local String
and StringBuilder cleanup. Source-only import tests do not exercise the shallow
interface ownership boundary. Native release jobs run this gate before packaging.
The imported primitive-str Clone provider must also retain its independent-copy
contract through those interfaces. A signature-only user clone returning a view
is checked separately and must be rejected when it escapes its local owner.
The check also introduces a caller type named `Clone` to verify provider identity,
and inspects IR for the two releases of copied results through shallow interfaces.
The same release check runs the local-owner/builder return fixture at O0/O2,
checks borrowed results from similarly named user functions, and verifies that
user `str_drop` functions cannot replace the std.string lifecycle binding. It
requires E3102 for dangling views, aliases, forwarding calls, a misleading clone
name, library String slices, reassignment, loop/branch exits and references to local scalars. This keeps
the repair paired with rejection tests instead of disabling return checking.
Generated generic interfaces retain their defining-module helpers and overloads;
incompatible calls must report E2200 without producing an artifact.
