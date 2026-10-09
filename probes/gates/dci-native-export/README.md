# Native Vyx DCI export gate

This gate requires a SDK compiler built from the current source tree, its matching
backend/runtime, and Python 3.11+ with `jsonschema` for the independent validator.
Python is used to inspect the result and run the Converter; the compiler's native
DCIB emission runs with external Adapter/tool discovery deliberately disabled.

From the repository root on Windows:

```powershell
$env:LLVM_ROOT = (Resolve-Path .\clang).Path
$env:PATH = "$env:LLVM_ROOT\bin;$env:PATH"
python .\probes\gates\dci-native-export\run.py `
    --compiler .\bootstrap_compiler\out\vyxc.exe `
    --triplet x86_64-pc-windows-msvc
```

On Linux use the compiler built on that host and the matching LLVM target triple:

```bash
python3 probes/gates/dci-native-export/run.py \
  --compiler bootstrap_compiler/out/vyxc \
  --triplet x86_64-unknown-linux-gnu
```

The gate checks:

- Native `--emit=dcib --src=file` produces the canonical DCIB v1 binary container,
  byte-identical to the independent codec, and passes strict schema/ABI validation.
- Only explicit `@[dci_export]` declarations appear in the published contract;
  each measured link symbol is checked against its exact `symbol="..."` override.
  An export with no `symbol` argument preserves and executes its original name.
- A separately emitted native object is consumed by generated `extern "dci"`
  definitions and an independently compiled AOT executable. The executable calls
  i32, i64, f32, f64, bool, void and non-owning raw pointer exports
  and checks their results.
- Project emission collects exports from all selected target source modules.
  Decorated declarations from a source dependency are available for compilation
  but are excluded from the target's published contract.
- Project default output, explicit `-o` output and the `build --emit=dcib` shorthand
  agree byte for byte. Project `--run` rejection preserves the previous contract. A separately
  built static library is consumed and executed through the project contract.
- Missing selection, unsupported string/tracked reference signatures, unproved external unwind
  behavior, a recursive call graph and an incompatible `--run=aot` combination
  are rejected. A rejected
  export preserves the previous output file.
- A supported free function in the same unit does not permit silently omitting a
  selected class method or module. Both mixed selection cases must fail with a
  normal diagnostic and preserve the previous output.

Compiler identity and results are recorded in `.cache/verification.json` only
after all checks pass. A failure keeps its generated workspace under `.cache/failed/`.
This runner records evidence for the actual host; it does not imply that another
platform, all Vyx types or all DCI Core capability levels have been validated.
