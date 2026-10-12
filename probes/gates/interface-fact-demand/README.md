# Interface body facts on demand

The runner checks a producer with 128 unused functions and 128 unused methods,
then emits its `.vyi`. A consumer calls one function, one method and one generic
function through normal `-L` SDK discovery. Sema's profiling trace must contain
no body resolution for the unused declarations. A wrongly typed call must fail.

This gate deliberately omits `--interface-root`: that partition option already
suppresses eager imported facts, while ordinary SDK consumers also need bounded
work. Source imports and producer body validation continue to run their checks.

```powershell
./probes/gates/interface-fact-demand/run.ps1 -Compiler ./bootstrap_compiler/out/vyxc.exe
```

The larger consumer is `samples/projects/zyn_recomposition_smoke`. Build it with
the same compiler and run `target/zyn_recomposition_smoke.exe --self-test`.
