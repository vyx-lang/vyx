# Native linker symbol ownership

Two source definitions cannot share a native linker symbol merely because their
LLVM signatures match. Compatible extern declarations may refer to one
definition. Repeated requests for the same closed generic must retain the same
source identity and generic arguments.

Run with the SDK compiler and backend built from the current sources:

```powershell
python probes/gates/native-symbol-collision/run.py `
    --compiler bootstrap_compiler/out/vyxc.exe `
    --triplet x86_64-pc-windows-msvc
```

The gate executes independent AOT consumers of object and project static-library
producers. It checks declarations appearing before and after a definition,
multiple extern aliases, and repeated closed-generic calls. Distinct definitions
with matching `export_name`, `no_mangle`, or differing generic arguments must
fail with a definition-collision diagnostic before publishing an object.

A selected executable source has one process entry. Other modules may expose
ordinary functions named `main`, including functions with parameters; qualified
calls must use their own bodies and signatures. The gate calls two such modules
from the selected entry through source imports and through both whole-crate and
parallel project builds. Separately consumed `static` and `staticlib` targets
also call both module functions. `shared` and `dll` targets are built through
real manifest dependencies; the consumer executes using the deployed library.
Library module names do not select a process entry. Explicit
`no_mangle` and native symbol attributes keep their requested symbols and remain
subject to the duplicate-definition checks above.

Results and compiler identity are written to `.cache/verification.json`. Host
execution does not imply another platform was tested. DCI contract aliases and
native DCIB output preservation remain covered by the DCI gates.
