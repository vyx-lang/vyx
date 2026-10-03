# p2_class_value

Intent: class values behave like values, not shared pointers.

- `class_value.vyx`: copy independence, pass-by-value, return-by-value on a
  scalar-field class. Expected exit 0 / `class_value ok`.
- `class_value_collection.vyx`: the self-host **true-copy** bug class. A class
  owns a `Vec<i32>` (a heap buffer behind a `rawptr`). Copying the class and
  then editing the copy in place (`set(0,999)`) or growing it (`add`) must not
  be visible through the original. If the copy is a shallow memberwise copy,
  the two share one buffer and `a[0]` becomes `999` — that is the miscompile
  this probe hunts. Expected exit 0 / `class_value_collection ok`.
