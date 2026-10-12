# Primitive string methods

```powershell
python probes/gates/primitive-string-methods/run.py --compiler bootstrap_compiler/out/vyxc.exe
```

Use the current-tree SDK compiler and matching backend/runtime. AOT O0/O2 checks
that `to_string()` and `toString()` both produce `string`: signed integers,
maximum `u64`, booleans, floats, and text containing a NUL byte. User methods
with the same names retain their own dispatch. Commands and hashes are recorded
under `.cache/`.
