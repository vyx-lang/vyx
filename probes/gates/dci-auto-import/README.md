# DCI preparation for declared exports

Run from the repository root with a compiler built from this checkout:

```powershell
python probes/gates/dci-auto-import/run.py --compiler bootstrap_compiler/out/vyxc.exe --result-dir .runs/dci-auto-import/cache
```

The gate copies the ordinary C++ session project into a fresh owned result
directory and uses only `vyxc build --run=aot`. It checks cold and warm builds,
source-only edits, same-size/same-mtime native default changes, missing or
tampered generated files, rejection of a newly used but unexported producer
type, explicitly adding that type to the export scope, and
rejection of malformed producer source before an old executable can run.
It also verifies that inherited templates, platform flags/include paths and
build defaults reach both producer extraction and native compilation.
It also selects an authored Converter file, deletes the entire cache, removes
the project contract, and checks regeneration without rewriting definitions.
Missing or invalid authored definitions must fail before launching an executable.
It generates a separate contract with the standalone Adapter, deletes its cache,
and checks consumption without AST extraction or modifying the offline contract.
A corrupt supplied contract is rejected and preserved for diagnosis.
Native constructor/method defaults remain on original signatures, with no
generated default wrappers. The Vyx definitions include the original header.
Export preparation never scans application code to choose an export surface.

The real Qt application and signal/context destruction checks live in
[dci-qt-counter](../dci-qt-counter/README.md). Callback pointer identity,
ownership and unsupported overloads remain fail-closed. This gate is Windows
AOT validation; it does not certify Linux Qt or JIT.
