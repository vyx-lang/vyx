# p4_dispatch

Intent: method/overload selection and chained calls.

- `chained.vyx`: static factory `Accum.create()` plus fluent
  `.add().add().build()` chaining on a value-type class. The final result must
  accumulate every step. Expected exit 0.
- `overload.vyx`: free-function overloading by arity (`area(s)` vs
  `area(w,h)`) and by parameter type (`describe(i32)` vs `describe(i64)`).
  Either it resolves correctly, or it must be rejected with a clear diagnostic
  (recorded as a language gap). A wrong-overload miscompile is a failure.
