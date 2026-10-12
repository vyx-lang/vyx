# Integer value conversions

```powershell
python probes/gates/integer-conversions/run.py --compiler bootstrap_compiler/out/vyxc.exe
```

Use a current-tree compiler and its matching backend/runtime. Positive AOT
fixtures run at O0 and O2 and check signed/unsigned boundaries, negative literals,
widening, explicit `as` truncation, and stores through records, arrays, references
and Vec indexing.

Negative fixtures include the existing industrial signed/unsigned narrowing and
signedness cases. They also exercise binding, assignment, return, record fields,
array elements, dereferences, Vec elements, array/tuple literals, and array length.
Every rejection must include a source location and E1000 and leave no executable.

Contextual literal typing requires the value to fit. Implicit conversion of an
integer value may widen within its signedness; narrowing needs an explicit cast.
Aggregate typing must check each element before adopting the destination type.

Commands, compiler hashes and diagnostics are recorded in ignored `.cache/`.
