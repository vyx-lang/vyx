# Platform guarded interfaces

This gate checks the actual `.vyi` boundary used by split standard-library and
compiler modules. It emits shallow, full, and project-private interfaces, then
consumes them for Windows, Linux, Android, and both macOS architectures.

```powershell
python probes/gates/platform-interfaces/run.py `
  --compiler bootstrap_compiler/out/vyxc.exe `
  --out .cache/platform-interfaces
```

The source includes nested `extern "C"` guards, repeated platform clauses,
overloads with one platform alternative per declaration, C ABI link names, the
`ffi_returns_independent` foreign-storage contract, and an opaque attribute that
contains lookalike text. The runner requires:

- `GetFileAttributesA` only in the Windows IR;
- `opendir` only in non-Windows IR;
- Linux, Android, and macOS alternatives selected independently;
- an outer `windows` guard combined with an inner `linux` guard to remain
  impossible rather than becoming a union;
- repeated platform clauses to retain their source union;
- exactly one independent-return contract, with quoted opaque text inert;
- C wrapper declarations to remain `extern "C"` members in every interface;
- non-Windows object generation to complete without a Win32 runtime binding.

The filesystem sub-gate uses the real `bootstrap_compiler/std_packages/io/src/fs.vyx`
source and repeats the IR check for every target. It does not execute a foreign
executable on the host. This is an AOT code-generation and interface-selection
gate; it does not certify JIT behavior.
