# C++ project export preparation

`vyxc build --run=aot` reads `native/session.hpp` through the target's `dci_imports`
configuration. The compiler adds generated sources to the build graph; API
producer facts are published to `contracts/session.dcib`. The generated Vyx
module and native materialization live in `.cache/dci/session/` and can be
recreated by the next build after deleting that cache. Set `definitions` in
`[dci.import.session]` to use a maintained Converter file instead.

The manifest explicitly exports `NativeSession` and configures the module,
original headers and native toolchain. Application usage cannot expand exports.
There is no binding selection JSON or application-maintained C wrapper.
`NativeSession()` and `session.add(1)` use the defaults declared by C++.
The native implementation remains a normal source in the build target.
The Converter includes `session.hpp` and emits the original constructor/method
signatures with their measured defaults. There are no default-argument wrappers.

The import pipeline uses Python 3.11+, or Python 3.10 with the SDK's `tomli` dependency.
Set `LLVM_ROOT` to the repository's LLVM 22 directory. A packaged SDK can set
`VYX_DCI_TOOLS` to its installed tools directory and select its native compiler
in the target's `cxx` setting.

Header dependencies, author-declared export scope, native compilers and DCI tool
content participate in the import cache identity. Missing or changed generated
outputs trigger regeneration. Unsupported overloads and lifecycle requirements
are recorded in `.cache/dci/session/report.json`.
