# C++ DCI ABI regression

The default gate validates native AOT on Windows x86_64: measured C++ contracts,
reverse DCI stubs, MIR branch and lifetime behavior, LLVM ABI signatures, object
emission, executable linking, and the program's output and exit status.

```powershell
.\run.ps1 -BootstrapCompiler ..\..\..\bootstrap_compiler\out\vyxc.exe
```

Use `-ExperimentalBackends` to additionally run the retained JIT and MIR2CPP
checks. These backends are outside the AOT acceptance gate. The reported JIT
resolution failure for `vyx_runtime_malloc` / `vyx_runtime_free` remains open;
a default gate pass does not validate JIT or MIR2CPP.

The runner copies source inputs to its own directory under `tests/.cache` and
records the compiler and backend hashes in `toolchain.txt`. Other platforms
return 77 because this fixture checks the MSVC ABI.
