# Nominal identities and borrowed values

```powershell
python probes/gates/nominal-identity/run.py --compiler bootstrap_compiler/out/vyxc.exe
```

Use a compiler and matching backend/runtime built from the current tree.
The positive project defines same-named records in two modules with different
layouts. Build and run it at O0 and O2, checking actual field values, qualified
inferred types, aliases, reassignment, generic records, and mutable-to-shared
borrows.

Negative fixtures cover value binding, assignment, parameters, reference returns,
typed pointers, nested pointers, generic base identities, generic arguments,
shared-to-mutable borrows, and integer pointee widths. Each must produce a located
E1000 and no executable. A pointer preserves its pointee's type and layout;
numeric value widening does not widen the object behind an address.

Results, commands, diagnostics and compiler hashes are saved under `.cache/`.
