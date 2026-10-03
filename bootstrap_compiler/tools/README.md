# Bootstrap pipeline probes

Small drivers compiled **with host `vyxc`** to test each compiler stage in isolation.

| Executable | Stages | Links `vyx_codegen`? |
|------------|--------|----------------------|
| `probe_lex.exe` | read file → `lex_source` | **No** |
| `probe_parse.exe` | lex → `parse_unit` | **No** |
| `probe_sema.exe` | lex → parse → `analyze_unit` + `check_program` | **No** |
| `probe_emit.exe` | full pipeline → `emit_unit` / `emit_to_file` | **Yes** (LLVM FFI only) |

Build (from repo root, same `vyxc` / `vyx_codegen` discovery as `scripts/build.ps1`):

```powershell
.\bootstrap_compiler\scripts\build_probes.ps1
```

Run all samples through lex → parse → sema → (emit if built):

```powershell
.\bootstrap_compiler\scripts\test_pipeline.ps1
```

Manual:

```powershell
.\bootstrap_compiler\out\probe_lex.exe   .\samples\bootstrap\hello.vyx
.\bootstrap_compiler\out\probe_parse.exe .\samples\bootstrap\hello.vyx
.\bootstrap_compiler\out\probe_sema.exe   .\samples\bootstrap\hello.vyx
.\bootstrap_compiler\out\probe_emit.exe   .\samples\bootstrap\hello.vyx .\bootstrap_compiler\out\hello.probe.ll
```

Use these to see **which stage** fails without running the full SDK compiler or loading the C++ host-codegen bridge.
