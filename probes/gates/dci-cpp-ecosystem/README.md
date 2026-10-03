# Real C++ ecosystem gate: ICU 78.3

Run from the repository root on Windows with the SDK compiler built from current source:

```powershell
./probes/gates/dci-cpp-ecosystem/run.ps1 -Compiler bootstrap_compiler/out/vyxc.exe
```

This builds
the Vyx project and independently links its `-O0` and `-O2` IR to the real ICU
import library. Each executable must agree with an ordinary C++ reference.

The dependency is the existing `Zyn/vendor/ICU` distribution: upstream headers,
`icuuc78.lib`, `icuuc78.dll`, and Unicode data in `icudt78.dll`. `upstream.json`
pins the version and hashes; this gate fails if they change. The library is not
rebuilt or replaced with a test implementation.

`icu.hpp` only includes ICU's original header and aliases its original
`icu_78::UnicodeString` type. It defines no classes or callable functions.
Vyx imports that C++ object through DCI and calls ICU's original mangled
constructors, destructor, and methods. The contract checker verifies a measured
64-byte layout, destructor binding, and native symbols. No generated function
wrappers or C handles are used.

The scenario covers 512 rounds of heap-backed supplementary UTF-16 strings,
surrogate/code-point iteration, copying, capacity, accented Latin and Greek
case folding, default versus Turkic folding, appending supplementary characters,
case conversion, copies retaining content after mutation, and `ßß` expanding
to `SSSS`. ICU supplies the Unicode behavior and data.

The three reference-returning mutators explicitly use the adapter's
`--borrow-return` ownership contract. Unannotated pointer/reference APIs remain
rejected. The entry alias is currently necessary because raw-source inventory
does not expand ICU's `U_NAMESPACE_BEGIN` macro; it preserves the exact original
class and symbols while making its public name visible to the adapter.

This fixture covers the Windows x86_64 MSVC ABI and AOT. Required headers,
libraries and DLLs must match `upstream.json`; missing Windows prerequisites are
not evidence about another target. It demonstrates real ICU operations, not
all C++ library compatibility or industrial readiness. The
[industrial runner](../dci-industrial/README.md) composes repeated executions
with the original vector, Rust Cargo and exception fixtures.
