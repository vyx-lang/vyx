# Compiler native standard-library closure

This gate uses the current `bootstrap_compiler/Vyx.toml` boot target's native
stdlib C/C++ sources and include paths with its `parallel_vyx` and
`parallel_std_sources` settings. It does not use the normal application's
`std:serde` static-package link path. The fixture calls both the legacy manifest
helpers and the typed TOML/JSON APIs and runs the result at O0/O2.

```sh
python probes/gates/bootstrap-native-stdlib/run.py --compiler /path/to/current/vyxc --output .cache/bootstrap-native-stdlib
```

When boot's native inputs omit a serialization backend, this program fails
linking just like the compiler's whole standard-library module. This caught
the Android/Termux failure after TOML acquired a native parser: the Vyx module
was emitted, but its native sources were absent from boot's manifest. The
same manifest supplies Windows/Linux native and macOS cross/native builds.

`--build-root` selects an exported current-source bootstrap tree. Compiler
identity, manifest/native/std inputs, commands, output and drift checks are
retained in the output directory. Use the matching backend/runtime. Host runs
do not certify an Android executable; Android's full cross-link remains in
the SDK release workflow.

The interface checks retain imported public trait implementations for generic
owners, exclude local private owners/traits, and execute a Clone implementation
from the generated interface. Ref's shallow interface must carry its checked
generic AST artifact. Standard-library namespaces follow the same template
transport rules as application libraries; declaration-only Ref/Vec interfaces
are insufficient for independent module compilation.

A source-free named `std.*` artifact also exercises a non-reserved generic
record, an inferred method return and a private helper accepting an error
enum. Artifact decoding must apply the same type qualification as ordinary
source import; treating the enum variant as i32 fails this consumer.
Two registry orders combine that interface with a conflicting source under
a different filename. The published interface must win by exact logical
module identity. A module being compiled from source keeps its source and
ignores its own previous output interface.
An explicitly selected source also takes priority over an old interface
discovered through a library search directory. Explicit interface inputs
retain their existing priority over source transports of that same module.
