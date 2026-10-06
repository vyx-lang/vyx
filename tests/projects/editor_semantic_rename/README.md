# Cross-module editor project

This six-file AOT project exercises binding-aware references and rename:
imports and aliases, shadowed locals, unrelated types with identical member
names, enum constructors, nominal and generic types, overloaded functions,
globals, closures, Unicode interpolation and multiple files in one module.

Build and run the unmodified project with the current SDK compiler:

```powershell
vyxc build -j2
./target/editor_semantic_rename.exe
```

From the repository root, run the protocol and applied-edit gate:

```powershell
python probes/gates/editor-industrial/check_lsp_semantic.py --lsp bootstrap_compiler/out/vyxc-lsp.exe --compiler bootstrap_compiler/out/boot.exe --result .runs/editor-industrial/semantic.json
```

The gate copies the project to a temporary Unicode path, checks exact reference
locations and unsafe rename refusals, applies successive renames, then builds
and runs the edited project. It also checks unsaved versions, stale changes,
syntax errors, SDK read-only files and source discovery/deletion. Failure sources
are retained beside the result JSON. Use compiler, LSP, backend and runtime from
the same build. Linux uses executables without `.exe`.
