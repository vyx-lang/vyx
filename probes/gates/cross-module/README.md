# Cross-module definitions

[Compiler architecture](../../../docs/COMPILER.md) · [Generic interfaces](../generic_interfaces/README.md)

This gate emits a producer interface and native object, then compiles an
isolated consumer from the `.vyi` and links the producer object explicitly with
`--link-obj`. The producer exports a mutable integer global, a nonempty string
global, and an enum. The consumer puts a
local global before the imported symbols so HIR item ids differ across the two
compilations, then reads the imported global, constructs the imported enum,
and matches it in the consumer.

The executable must return zero. A successful run proves that static linker
names use the declaring module and source name, and that enum variants remain
qualified to their imported owner.

The declaring module owns native storage and its initializer. Interface-only
consumers refer to that storage, so independently compiled consumers observe the
same mutable value. The producer retains exported storage even when none of its
native functions references it. Linking a consumer therefore requires the
producer object; an omitted owner must produce an unresolved storage symbol.
Each run records the compiler SHA-256 and phase exits in `result.json`.

The string global also checks MIR static metadata storage: initializer text and
module identity must occupy independent fields. An earlier overlap corrupted
string cleanup and caused heap failures in compiler self-hosting.

`generic-direct-return.ps1` separately emits a free-generic producer and checks a
qualified generic call inside a direct return expression. Explicit `T=i32` must
be substituted before return checking; contextual comparison typing must not
hide an unresolved `T`.

`storage.ps1` uses the real
[`global_module_contract`](../../../tests/projects/global_module_contract/README.md)
manifest fixture for a stronger storage contract. The producer's initialized
integer and nonempty string are not referenced by any producer function; the
string lives in a second source file in the same logical module. Two isolated
consumers read the initial values and observe each other's integer updates. A
second producer exports globals with identical source names and independent
values. Each module's source is archived before later consumers compile, so
imports can resolve only through `.vyi`. The executable explicitly links all
objects; omitting the storage owner's object must fail with an unresolved
global. The same fixture also runs as a cold, warm and incremental manifest
project. The incremental variant changes the secondary producer source's string
initializer. Every phase records its command, compiler hash and exit status in
`results.json`.

`storage-paths.ps1` exercises the other storage ownership paths:

- An interface constant with no available initializer cannot initialize another
  static. IR compilation must fail with the specific missing-initializer
  diagnostic; an unrelated nonzero compiler exit does not satisfy the gate.
- A module-block fixture emits seven initialized globals with distinct source
  names, nested blocks and an explicit semicolon module inside a block. The IR
  contract checks each declaring module and initializer, including restoration
  of the outer module after a closing block. The parser's `__module_end` marker
  must never become a storage owner.
  Repeated short names are additionally checked by `block-modules.ps1` below.
- Generic associated constants arrive through a full `.vyi` artifact. Two
  classes have identically named `VALUE` members with different values. Two
  independent consumers each read both classes specialized with `i32` and
  `i64`, then link and run together without any producer native object. This
  checks member ownership, specialization and duplicate-consumer linking.
  Before object compilation, each consumer emits IR in 16 fresh processes with
  identical arguments and output path. Every hash must match, with four closed
  owner definitions, their correct values, and no unspecialized owner reference.
  Hashes and shape checks are recorded for every repetition; failing IR is kept.
- A unity compile importing two globals named `counter` rejects an unqualified
  reference with a located ambiguity diagnostic. A separate executable imports
  those same modules and defines its own `counter`; reads and writes must select
  the current module's declaration.
- A global and a function in different modules share the name `value`. Both
  source-list orders must preserve the global read and the qualified function
  call, so flat symbol insertion order cannot decide name resolution.
- A full `.vyi` carries a versioned generic AST artifact. Its producer owns a
  private mutable counter referenced only by a public generic body. Independent
  `i32` and `i64` consumers must observe the shared sequence `18`, `19`, `20`.
  The private counter stays out of the public declaration surface. Removing the
  producer object must produce an unresolved storage symbol.
- A source import through `--module-source` runs the same private generic
  counter contract without an interface, checking that source-owned definitions
  remain available.
- Two object compilations use `--export-root-names` and `--emit-root-names` to
  partition one module's read/update functions. Their linked executable must
  read `17`, update to `18`, and read `18` through the other partition.
- A project unity input uses `VYX_CODEGEN_UNITS=4`. The gate checks that the
  retained `.cgu.list` names at least two existing native objects and runs the
  shared-global fixture after linking them. A single-unit fallback fails the
  gate. The script records the actual unit count and restores the caller's
  environment.

`block-modules.ps1` uses the real
[`block_module_contract`](../../../tests/projects/block_module_contract/README.md)
project. Distinct block modules may declare the same global or constant short
name; exact lexical module identity determines lookup and storage ownership.
The gate checks nested and reopened blocks, an inner semicolon module, root
scope restoration, per-module aliases, and static initializers that read their
own constants. It runs single-file and unity programs at O0/O2, isolated shallow
and full interface consumers, and cold/warm unity and parallel manifest builds.
The interface consumers import the producer file's root module before using
its block namespaces. Separate negative cases require located `E2100` errors
for genuine duplicate definitions in the same logical module, including two
non-entry project files. A no-file-module case checks the anonymous root scope.

These storage scripts preserve generated interfaces, native objects, commands,
compiler identities and phase logs under their unique `.runs/` directory.
`-OutputDir <new-directory>` selects an explicit result directory.
`storage-paths.ps1 -CaseFilter <regex>` runs selected cases during iteration;
for example, `-CaseFilter 'associated|unity-'` selects the associated-constant
and name-resolution contracts. An unmatched filter fails instead of reporting
a successful empty run.

Run these gates with the SDK compiler freshly built from current source for AOT
and its matching runtime/backend:

```powershell
$compiler = (Resolve-Path ./bootstrap_compiler/out/vyxc.exe).Path
powershell -NoProfile -ExecutionPolicy Bypass -File ./probes/gates/cross-module/run.ps1 -Compiler $compiler
powershell -NoProfile -ExecutionPolicy Bypass -File ./probes/gates/cross-module/storage.ps1 -Compiler $compiler
powershell -NoProfile -ExecutionPolicy Bypass -File ./probes/gates/cross-module/storage-paths.ps1 -Compiler $compiler
powershell -NoProfile -ExecutionPolicy Bypass -File ./probes/gates/cross-module/block-modules.ps1 -Compiler $compiler
powershell -NoProfile -ExecutionPolicy Bypass -File ./probes/gates/cross-module/generic-direct-return.ps1 -Compiler $compiler
```

These contracts cover native AOT storage, enum ownership, generic imports and
the named scope rules. Block modules have both IR ownership and native runtime
contracts.
Results do not certify every import form, JIT execution or an untested platform.
