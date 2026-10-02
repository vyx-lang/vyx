# Match expression AOT gate

[Compiler architecture](../../../docs/COMPILER.md) · [Verification](../../../docs/TESTING_GUIDE.md)

`run.ps1` compiles and executes `match_expression.vyx`, then requires four
invalid sources to fail with an `E1000` diagnostic. It uses the freshly built
AOT SDK compiler and `bootstrap_compiler/out` runtime from the same source build.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ./probes/gates/match-expression/run.ps1 `
  -Compiler ./bootstrap_compiler/out/vyxc.exe
```

The executable checks value matches with and without subject parentheses,
enum payload binding, nested matches, block tail values, boolean guards,
returns from arms, and arithmetic using a match result. A side-effect counter
requires the subject to be evaluated exactly once.

| Invalid input | Required result |
|---|---|
| `negative_type_mismatch.vyx` | Incompatible arm result types are rejected |
| `negative_guard_type.vyx` | Non-boolean guard is rejected |
| `negative_missing_value.vyx` | A value-producing arm cannot silently have no result |
| `negative_empty.vyx` | An empty value match cannot acquire a synthetic `i32` result |

This gate passed in the Windows AOT 2026-10-01 batch (`ef86b928`, `61d31e51`).
Logs and executables are generated under `.runs/`. The gate does not establish
complete exhaustiveness analysis or JIT parity.
