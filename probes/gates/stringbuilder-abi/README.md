# StringBuilder native ABI regression

Run with the SDK compiler freshly built from the current source tree:

```powershell
./probes/gates/stringbuilder-abi/run.ps1 -Compiler bootstrap_compiler/out/vyxc.exe
```

The gate compiles and executes the independent `4429` reproducer and the
extended cases at O0 and O2 through both file and project compilation.
Projects explicitly depend on the real `std:core` and `std:collections` packages.
It checks that LLVM IR calls the typed Vyx factory and never substitutes the
raw-pointer `vyx_string_builder_*_abi` entry points.

Cases cover aggregate factory returns, inline/heap storage, signed and unsigned
integer appends, the MIR integer-append rewrite, build snapshots, transfer/reuse,
destruction and an unrelated class whose name contains `StringBuilder`.
Compiler identity, commands, output and results stay in the ignored `.runs/`.
