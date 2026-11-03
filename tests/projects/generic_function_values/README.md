# Generic function values across modules

Run with the compiler being tested and its matching runtime:

```powershell
vyxc --src=project tests/projects/generic_function_values --run=aot -j2
```

Expected output: `generic function values ok`, exit 0.

The application imports generic callbacks under aliases, receives an instantiated
callback from another module's factory, and passes a callback to `Iterator.map`.
Both modules declare `measure`; the library factory must preserve its own
declaration when HIR restores targets from template artifacts. Run at O0/O2 and
with `VYX_CODEGEN_UNITS=1` and `4` to check both compilation paths.
