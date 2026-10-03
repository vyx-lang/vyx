# Cross-module definitions

[Compiler architecture](../../../docs/COMPILER.md) · [Generic interfaces](../generic_interfaces/README.md)

This gate emits a producer interface and compiles a consumer from only the
`.vyi`. The producer exports a mutable integer global, a nonempty string global,
and an enum. The consumer puts a
local global before the imported symbols so HIR item ids differ across the two
compilations, then reads the imported global, constructs the imported enum,
and matches it in the consumer.

The executable must return zero. A successful run proves that static linker
names use the declaring module and source name, and that enum variants remain
qualified to their imported owner.

The string global also checks MIR static metadata storage: initializer text and
module identity must occupy independent fields. An earlier overlap corrupted
string cleanup and caused heap failures in compiler self-hosting.

`generic-direct-return.ps1` separately emits a free-generic producer and checks a
qualified generic call inside a direct return expression. Explicit `T=i32` must
be substituted before return checking; contextual comparison typing must not
hide an unresolved `T`.

Run both with the SDK compiler freshly built from current source for AOT
and its matching runtime/backend:

```powershell
$compiler = (Resolve-Path ./bootstrap_compiler/out/vyxc.exe).Path
powershell -NoProfile -ExecutionPolicy Bypass -File ./probes/gates/cross-module/run.ps1 -Compiler $compiler
powershell -NoProfile -ExecutionPolicy Bypass -File ./probes/gates/cross-module/generic-direct-return.ps1 -Compiler $compiler
```

The Windows gates passed in the 2026-10-01 `ef86b928`/`61d31e51` batch. Generated
interfaces, compiler logs, and executable logs stay under `.runs/`. Their scope
is the fixture's exported storage, enum owner, and free-generic return contracts;
they do not certify every import form or target platform.
