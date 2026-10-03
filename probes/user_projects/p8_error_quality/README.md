# p8_error_quality

Intent: invalid programs must **fail closed** with a useful diagnostic.

- `bad_type.vyx`: assigns a string literal to an `i32` and does arithmetic on
  it. Expect a type-mismatch diagnostic, non-zero exit.
- `unknown_call.vyx`: calls an undeclared free function and a non-existent
  method. Expect unresolved-name / no-such-method diagnostics, non-zero exit.

Fail conditions: SIGSEGV, hang, or exit 0 (compiler accepted invalid code).
