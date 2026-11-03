# Runtime companion deployment

Windows native AOT regression:

```powershell
python probes/gates/runtime-companions/run.py --compiler bootstrap_compiler/out/vyxc.exe --runtime bootstrap_compiler/out --output .cache/runtime-companions
```

The explicit-runtime project stages its own oneTBB DLL before linking. A PE
overlay marker makes its file identity differ from the SDK DLL while retaining
the same callable library. The build must preserve that producer's DLL. An
ordinary project must still receive the implicit SDK runtime's DLL. Both
executables load their deployed DLL through the Windows loader, and the gate
checks the complete DLL bytes after deployment.
An obstructed deployment destination must also fail with a specific diagnostic;
a successful link alone cannot turn a failed DLL copy into a successful build.
An already loaded identical DLL must not be overwritten, while a different
loaded DLL must fail deployment rather than silently retain the wrong bytes.
A same-size COFF timestamp change also requires replacement, covering binary
differences after embedded NULs rather than accepting matching lengths alone.
The output directory must be new; commands, hashes and results are retained.

The native Windows release runs this gate before source verification and tools.
Before starting S1, it restores the current CMake-produced TBB DLL/import-library
pair: old Stage 0 compilers can copy their own SDK DLL over the new output.
This is a bootstrap handoff for older compilers, not permission to ignore a
different or unreadable loaded library. The preparation hook compares binary
files explicitly and reports failed reads/copies.
