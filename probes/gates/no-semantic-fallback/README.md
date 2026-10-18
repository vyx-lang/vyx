# Standard-library calls and missing semantic facts

Run with the SDK compiler built from the current source tree and its matching
runtime/backend:

```powershell
.\probes\gates\no-semantic-fallback\run.ps1 -Compiler <current-sdk-compiler.exe>
```

The gate checks file and project compilation at O0/O2, LLVM IR and executable
results. Console coverage includes primitive and user `Display` implementations,
references and `Vec` indices, owned strings borrowed across printing, Unicode,
embedded NUL, assertions, process arguments, hashing, ordering and Option/Result
methods. The shadow case gives common library names different signatures and
aggregate returns; those declarations must retain their own ABI.
Unsigned method arguments exercise literal range and suffix checking. A custom
primitive `clone()` returns a different value to prove the compiler calls its
implementation instead of substituting an identity copy.
Independent generic parameters keep independent trait bounds. A Clone-only
record passed as T must not acquire U's Hashable/Eq requirements.
A where-clause naming an undeclared trait must be diagnosed before codegen,
even when the generic body never invokes a trait method.

A separate C producer defines `zero`, `one`, `default_value` and
`string_sso_capacity` with values different from the old compiler substitutions.
The linked application must call those definitions. Negative cases require a
located error and no executable for an unavailable print provider, an untyped
raw-pointer dereference, an unknown primitive member and duplicate lang-item
providers. A trait merely named `Clone` cannot satisfy a primitive bound without
an implementation. JSON results record the compiler path, hash, commands and exits.
Record indexing checks the declared `operator[]` return type with `Vec<str>` and
a user record. Atomic primitives require an exact explicit ABI; a declaration
with a different return type must fail before an executable is published.
Pack indexing keeps the selected element type. A static library depends on the
real core package and builds with one and two LLVM emitter threads, checking
that open generic declarations do not acquire an invented ABI.
Container cloning covers derived records containing Vec<String> and Dict<str,
String>, nested Vecs, and mutation/destruction of the independent copy. A Vec
whose element lacks Clone must not satisfy a generic Clone bound.
Narrow signed and unsigned globals widen at their consumers: IR loads must
retain the declared storage width, then extend the scalar. This also covers
the i32 offsets consumed by the compiler's i64 pointer-arithmetic API.

This is an AOT gate. It does not verify JIT or replace the DCI and self-host gates.
