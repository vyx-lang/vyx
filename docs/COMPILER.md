# Vyx compiler architecture

[简体中文](COMPILER_ZH.md) · [Documentation](README.md) · [Compiler source and bootstrap](../bootstrap_compiler/README.md) · [Verification](TESTING_GUIDE.md)

This page maps the current LLVM compilation path to its source. For language
rules, use [language design](LANGUAGE_DESIGN.md). The source and regression
gates linked below describe the implementation and its verification scope.
The current verification baseline is native AOT. JIT parity and JIT performance
are deferred work and are not covered by the AOT results below.

## From source to native code

```text
.vyx / Vyx.toml
    ↓ lexing, parsing, AST, semantic analysis
HIR: types, declarations, call targets, generic environments, ownership facts
    ↓ reachable-function materialization and control-flow lowering
MIR: basic blocks, places, values, instructions, terminators
    ↓ verification and MIR optimization
LLVM IR → object files → native linking
```

`bootstrap_compiler/src/core/` contains the driver, frontend, and build
system. `src/hir/` and `src/mir/` implement the two intermediate
representations. `src/codegen/llvm/llvm_lower.vyx` owns the LLVM lowering context and
public entry points. Its method implementations live in
[`src/codegen/llvm/`](../bootstrap_compiler/src/codegen/llvm/README.md), grouped by
value/storage operations, native ABI, DCI, and function/CGU compilation. See the
[compiler README](../bootstrap_compiler/README.md) for build commands.

For a lowering bug, start with `values.vyx`, `instructions.vyx` or `calls.vyx`
and follow the selected operation. The source map lists the corresponding
implementation and its entry points. These files extend the same qualified
`LlvmMirLowerer` type within one logical module. `Vyx.toml` supplies the module's
source list, and the facade retains its storage and lifetime. Semantic
analysis and project building still have large central units. HIR construction
now has a [source map](../bootstrap_compiler/src/hir/builder/README.md) for
declarations, instances, expression resolution and body lifetime. HIR and LLVM
implementations still share their respective builder/lowering state.
The [source tree](../bootstrap_compiler/src/README.md) groups the frontend by
driver, syntax, semantics, facts, DCI, build scheduling and editor services.

## HIR: language meaning

[`HirUnit`](../bootstrap_compiler/src/hir/model/hir_model.vyx) stores types, items,
functions, locals, statements, expressions, generic environments, and type
lists in separate tables. Numeric IDs connect the logical expression and
statement structures. The physical representation uses flat record pools with
counts and capacities. These IDs index a compilation unit; they are not
persistent global identities across builds.

An expression records its type, resolved call or member target, operator,
value category, and source position. Ownership resolution adds borrow, move,
and drop-related facts. [`lower_pipeline_prepare_hir`](../bootstrap_compiler/src/codegen/lower_pipeline.vyx)
runs HIR semantic resolution, DCI lifecycle binding, ownership resolution,
and HIR verification in that order, reporting errors before lowering. The AST
and semantic-analysis stages already do part of this work; HIR does not own
all type checking.

`src/hir/verify/hir.vyx` audits AST semantic facts. The HIR data model lives in
`src/hir/model/hir_model.vyx`.

`src/hir/builder/hir_builder.vyx` holds builder storage, construction and native-handle
entry points. Its implementations in `src/hir/builder/` form the same logical
module. `function_bodies.vyx` handles reachable body requests;
`body_storage.vyx` discards generated records after codegen. Keep this bounded
lifetime when changing generic instances, closures or deferred typing.

## Interfaces and cross-module definitions

An emitted `.vyi` has an ordinary public declaration surface. For user modules
containing generics or public functions/methods with inferred return types,
[`template_artifact.vyx`](../bootstrap_compiler/src/core/sema/template_artifact.vyx)
also appends a version 1 AST graph with the required bodies and their private
dependencies. A shallow interface cannot infer a return annotation from syntax
alone; the checked graph preserves this inference in the consumer instead of
silently changing a value-returning function to `void`.
The importer rebuilds semantic references in the consumer and
retains the defining module for qualified lookup and helper visibility. Explicit
and inferred generic arguments are substituted before typing the return value.
Carrying a private helper in the graph does not make it publicly callable.

The importer validates the format, marker order, SHA-256 of both the public
surface and graph, module identity, node count, and bounds before semantic lookup.
If artifact markers are present, an invalid artifact fails import; it is not
silently reduced to bodyless declarations. Flat declaration and statement lists
are traversed iteratively, so list length does not consume the structural nesting
limit. Emission plans length prefixes and payload fragments before one final
buffer write. Hex encoding is linear; neither growing prefixes nor chain suffixes
are repeatedly copied into the string arena. Decoding borrows bounded field slices
from the live payload rather than copying each remaining chain. AST names, literals
and metadata receive their own decoded storage. The
[artifact scaling gate](../probes/gates/template_artifact_scaling/README.md) checks
v1 compatibility, large payload hashes, list handling and an AOT consumer under
a measured memory guard. Runtime, `bootstrap.*`, and `std.*` modules currently use their ordinary
interfaces without this payload; this is a user-template mechanism, not a
serialized shared frontend session. See the [generic interface gate](../probes/gates/generic_interfaces/README.md)
for the format limits and the std consumer's source-unit requirement.
Imported `.vyi` bodies are excluded from the eager whole-library HIR-fact pass.
Calls resolve the concrete nongeneric interface bodies they reach, including
private helper calls, before Sema's context is released. Signatures, inferred
returns and source imports retain their checks; template instantiation still
uses its generic environment. The [interface demand gate](../probes/gates/interface-fact-demand/README.md)
checks normal SDK discovery, transitive calls, unused bodies and invalid calls.
The [return inference gate](../probes/gates/return-inference/README.md) exercises
separate project objects at O0/O2, including nongeneric private helpers and
function-body diagnostics.

Imported enums retain their declaring owner. Global linker names use the
declaring module and source name, rather than compilation-local HIR IDs or
consumer type spellings. MIR static records keep initializer text and module
metadata in separate fields; this also prevents nonempty string initializers
from corrupting storage during cleanup. The [cross-module gate](../probes/gates/cross-module/README.md)
exercises different producer/consumer item ordering.

Repeated generic ODR definitions keep their canonical symbol names. The native
backend attaches `Any` COMDAT groups to linkonce/weak ODR definitions for COFF,
ELF, and Wasm. Two independent consumers instantiating the same generic have
passed a Windows COFF link/run regression; this does not establish equivalent
platform testing for ELF or Wasm.

## MIR: execution paths

[`MirUnit`](../bootstrap_compiler/src/mir/model/mir_model.vyx) retains the type
table and adds tables for functions, basic blocks, locals, fields, places,
values, instructions, and match cases. A block ends in a terminator such as
`return`, `goto`, `branch`, `match`, or `yield`.

| Concept | Question | Examples |
|---|---|---|
| Place | Where is data read or written? | Local, field, dereference, index, static storage |
| Value | What is computed? | Constant, read of a Place, operation, call, aggregate, cast |
| Instr | What happens? | Evaluation, assignment, drop, storage live/dead |
| Terminator | Where does control go? | Return, jump, branch, iteration |

The [MIR tree](../bootstrap_compiler/src/mir/README.md) separates the record
model, HIR lowering, analysis, optimization passes and verification.
`builder/mir_builder.vyx` retains builder state; expression, control-flow, call,
lifetime and reachable-function operations live in their respective subtrees.
`model/records/` owns record access; `verify/scope/` checks bounded function
ranges, while `verify/strict/` checks whole units.

HIR retains an `if` as a statement. [`lower_if_stmt`](../bootstrap_compiler/src/mir/builder/flow/branches.vyx)
creates then, else, and join blocks with explicit edges. The builder also
places `drop`, `defer`, and storage-end operations on scope-exit paths. MIR
allows mutable locals, `assign`, and `read_place`; a `ValueId` does not make
the entire representation SSA. See [`mir_ids.vyx`](../bootstrap_compiler/src/mir/model/mir_ids.vyx)
for the current kinds.

`match` also produces values. The builder evaluates its subject once, lowers
pattern tests and guards to control flow, and joins value-producing arms through
a result place. Nested matches, payload bindings, block tail values, and returns
from arms have an [AOT compile/run gate](../probes/gates/match-expression/README.md).
Incompatible arm types, non-boolean guards, missing arm values, and empty value
matches have negative diagnostic checks.

[`mir_pass.vyx`](../bootstrap_compiler/src/mir/passes/mir_pass.vyx) runs constant
folding, constant-branch folding, SCCP, copy propagation, drop-aware dead-code
elimination, select formation, and CFG simplification. At optimization levels
above 0, these passes run for a bounded number of rounds. MIR is verified and
checked against the DCI storage contract before LLVM lowering.

## Per-function work and CGUs

The current LLVM streaming path materializes, optimizes, lowers, and releases
MIR per function; those phases appear in
[`streaming.vyx`](../bootstrap_compiler/src/codegen/llvm/functions/streaming.vyx). A code
generation unit (CGU) groups definitions for backend work and object emission in
[`cgu.vyx`](../bootstrap_compiler/src/codegen/llvm/emit/cgu.vyx).
This is distinct from running separate project build jobs concurrently.

The project scheduler uses typed actions, input-size estimates, observed memory,
and successful-task cost history. The default host reserve is one third of the
physical memory available at batch start, bounded between 256 MiB and 4 GiB.
Active workers' growth allowances do not reduce that reserve. Interface emission
also counts as frontend work for memory estimates; codegen slot accounting is
separate. Cold object estimates include the bytes of peer interfaces loaded by that
action, in addition to its primary source; splitting a file into root chunks does
not remove those inputs. Split units also account for their registered import
closure, including every file of an imported logical module and dependencies
introduced by primary peers. A visited set bounds cycles and counts each imported
source once. Successful measured history supersedes the cold estimate.
If running workers consume half of the host reserve (at most 2 GiB remaining),
the scheduler reports physical and commit headroom and cancels its own batch.
Unknown telemetry does not trigger this check. Cancellation is a last resort
for underestimated growth; it does not make estimates hard memory caps.
The reserve arithmetic uses `i64`. Task-time history is scanned once per
batch and matched by full output identity. These are scheduling estimates, not
hard per-process memory caps or proof that the frontend is shared across jobs.
The [compiler pressure gate](../probes/gates/compiler-industrial/README.md)
checks cold/warm/incremental work and runtime results at `-j1` and `-jN`.

Shared parsing and immutable semantic facts, complete monomorphization closure,
and concurrent LLVM lowering are not established by the current project gates.
Existing backend optimize/emit queues and multiple build processes do not
establish those stages.

External generic operations must obtain closed facts and implementation
artifacts from the producer before consumption. Their contract belongs to
the [DCI reference](DCI_SPEC.md); adapter operation and verification commands
are in the [DCI SDK guide](../tools/dci/README.md).

## Library calls and compiler primitives

`print`, `println`, `panic`, `assert` and `assert_eq` are Vyx declarations in
`std.console`. Formatting dispatches through `Display`, including user implementations.
The core package also provides allocation declarations, process arguments and
formatting helpers. `std.io` forwards its console API to these declarations.
With implicit standard-library loading disabled, an absent declaration is an
undefined name. A user declaration with the same name keeps its own signature.

The lowering boundary requires an explicit callable target and ABI. It does not
substitute a legacy StringBuilder factory, manufacture `zero`/`one`/string
constants for a missing body, or pick an unrelated same-named method. Class
identity includes its module and generic arguments. String, Option and Result
ABI roles come from declaration facts. Missing DCI storage layout is an error.
Multiple distinct `lang_item` providers for one slot are diagnosed.

Size/layout queries, reflection, typed-pointer operations and string representation
primitives still have explicit compiler lowering. Their recognized operations do
not authorize unknown member names or guessed layouts. Cache misses can scan a
declaration index again; the scan must obey the same identity and scope checks.
The [library-call gate](../probes/gates/no-semantic-fallback/README.md) verifies the
ordinary call path and negative diagnostics. The [StringBuilder gate](../probes/gates/stringbuilder-abi/README.md)
checks factory and storage ABI across file/project compilation.

## Continue reading

- Language behavior: [design](LANGUAGE_DESIGN.md) and [surface index](LANGUAGE_SURFACE.md).
- Compiler work: [bootstrap](../bootstrap_compiler/README.md) and [verification](TESTING_GUIDE.md).
- Metadata and interop: [Fact Semantic Ownership System](MOSP.md) and the [DCI reference](DCI_SPEC.md).
