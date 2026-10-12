# DCI translated failure payload — Windows AOT

```powershell
$compiler = (Resolve-Path ./bootstrap_compiler/out/vyxc.exe).Path
powershell -NoProfile -ExecutionPolicy Bypass -File ./probes/gates/dci-failure/run.ps1 -Compiler $compiler
```

Use the SDK compiler freshly built from current source, with its matching
backend/runtime. `-OutputDir <new-directory>` selects a result directory;
`-Iterations N` increases the native repetitions from the minimum of 32 per
optimization level. Contracts, translator sources, IR, executables and logs stay
inside that directory. The tracked `generated/` file is not overwritten.

The C++ adapter publishes `dci.Failure` as a stable 64-byte, alignment-8 record
with its nontrivial copy/destroy lifecycle. The native translator contains this
record inline in its tagged result. Vyx calls the translator and reads the
corresponding `Result<i32, dci.Failure>` fields; the gate rejects IR calling the
original throwing symbol or registering Vyx exception-handling personalities.

Each executable checks successful division, the exact 37-byte exception message
`abi::checked_divide: division by zero`, the complete MSVC RTTI identity and its
length, the producer tag, and the retained exception payload's size, alignment
and `what()` value. The same checks run after the Result crosses two noinline
Vyx return frames and an 8 KiB native stack overwrite.

The fixture declaration does not request automatic destruction. Its native
helper receives the address of the Err record, validates it, calls
`dci_failure_destroy` once, and checks that the contents were cleared. Vyx reads
the cleared fields back. Each process checks 16 successful destructions, and
the script runs 32 independent processes at each of O0 and O2. It never frees
an outer Result or Failure wrapper.

`results.json` records the compiler/backend/runtime identities, exact commands,
contract and IR checks, executable hashes and every native exit status. These
results cover the Windows MSVC translated boundary; they do not establish
automatic drop for this declaration or another platform's exception ABI.
