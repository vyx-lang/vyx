# AST storage and semantic targets

Use a compiler built from the current tree with its matching backend/runtime:

```powershell
python probes/gates/ast-storage/run.py --compiler bootstrap_compiler/out/vyxc.exe
```

AOT O0/O2 checks the original `Vec<Ref<Item>>` fixture, indirect Vyx and C function
calls, iterator callbacks, inherited fields, trait implementations on derived
classes, and direct comptime evaluation. It asserts exit status and output.

AST text getters return borrowed views. Callers retaining text across a setter
on the same declaration must own a snapshot. A call's semantic target is tagged:
direct functions/methods contain a declaration; indirect calls contain a symbol.
Comptime folding must inspect that tag before accessing declaration metadata.

## Windows memory guard

Ordinary heap placement can hide out-of-bounds reads and stale text views.
Relink the same compiler objects with a diagnostic frontend allocator. Supply
the link response from that build; paths in it are relative to
`bootstrap_compiler/`. Choose a new output beside the matching backend DLL:

```powershell
python probes/gates/ast-storage/build_guard.py --link-response bootstrap_compiler/.cache/link.rsp --clang clang/bin/clang++.exe --output bootstrap_compiler/out/vyxc-guarded.exe
python probes/gates/ast-storage/run.py --compiler bootstrap_compiler/out/vyxc.exe --guarded-compiler bootstrap_compiler/out/vyxc-guarded.exe
```

The guard places frontend allocations at the end of writable pages, protects
released allocations, and retains their reservations until process exit.
It also handles HIR scratch strings released through `vyx_runtime_free`.
The allocator is for this single-process regression gate, not release builds
or compiler memory/performance measurements. Alignment leaves up to 15 bytes
of padding; this is not a substitute for a general memory sanitizer.

Build commands, hashes, diagnostics, maps and gate results remain in `.cache/`.
The memory guard uses Windows APIs; its results do not establish Linux behavior.
