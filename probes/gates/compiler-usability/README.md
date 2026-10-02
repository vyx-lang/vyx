# Compiler usability regressions — native AOT

From the repository root, after building the tested SDK compiler from current source:

```powershell
./probes/gates/compiler-usability/run.ps1 -Compiler bootstrap_compiler/out/vyxc.exe
```

The Windows runner uses the compiler's own runtime directory, checks exit codes
and exact output, and rejects malformed interpolation with a source location.
It records compiler identity and logs in ignored `.runs/`.
`-RuntimeDir` can select the matching runtime explicitly.

The default matrix runs `-O0` and `-O2`, with `VYX_CODEGEN_UNITS=1` and `4`.
The latter exercises the collect/rebuild streaming path and requests up to two
emit workers (`-j 2`); the former exercises streaming without the collect walk.
The actual unit count depends on the partition size.

| Fixture | Covered path |
|---|---|
| `derived_trait.vyx` | Derived aggregate with inherited and own fields after `impl Trait for Child`; field reads and trait dispatch |
| `derived_multilevel.vyx` | Three inheritance levels and two trait implementations; integer and string field layout |
| `index_print.vyx` | Inferred Vec index binding and direct indexing; direct print and interpolation for signed integers, u64, bool, f32/f64 and borrowed strings |
| `interpolation.vyx` | Embedded string literals, nested interpolation, escaped quotes, literal `\${...}`, and braces inside character literals/comments |
| `async_stream.vyx` | Two interleaved tasks, nested async call, yield/sleep and named locals across multiple awaits, repeated for 32 rounds |
| `async_params.vyx` | Mutable integer, string and aggregate parameters across yield/sleep; native byval body calls and borrowed method receivers |
| `async_values.vyx` | Mutable object, Vec and string locals across awaits; `defer` executes once per completed task |
| `async_payload.vyx` | Nested async string and aggregate returns; native sret/byval in the task worker |
| `interpolation_invalid.vyx` | Trailing expression tokens must fail with `E0001` at line 2, column 20 |
| `interpolation_unterminated.vyx` | Unclosed interpolation must fail with `E0102` at line 2, column 11 |

The async regression must execute a native binary. Emitting LLVM IR alone uses
a different path and did not reproduce the collect/rebuild defect. Likewise,
the index regression checks both `print(y)` and `"${y}"`: direct integer printing
alone did not reproduce the reference-to-C-string conversion defect.

For a smaller reproduction:

```powershell
./probes/gates/compiler-usability/run.ps1 -Compiler bootstrap_compiler/out/vyxc.exe `
    -OptimizationLevels 0 -CodegenUnits 4
```

This runner targets Windows. Linux execution and JIT are outside this gate.
