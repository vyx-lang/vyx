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
representations. `src/codegen/llvm_lower.vyx` lowers MIR to LLVM. See the
[compiler README](../bootstrap_compiler/README.md) for build commands.

## HIR: language meaning

[`HirUnit`](../bootstrap_compiler/src/hir/hir_model.vyx) stores types, items,
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

`src/hir/hir.vyx` audits AST semantic facts. The HIR data model lives in
`src/hir/hir_model.vyx`.

## Interfaces and cross-module definitions

An emitted `.vyi` has an ordinary public declaration surface. For user modules
containing generics, [`template_artifact.vyx`](../bootstrap_compiler/src/core/template_artifact.vyx)
also appends a version 1 AST graph with generic bodies and their private
dependencies. The importer rebuilds semantic references in the consumer and
retains the defining module for qualified lookup and helper visibility. Explicit
and inferred generic arguments are substituted before typing the return value.
Carrying a private helper in the graph does not make it publicly callable.

The importer validates the format, marker order, SHA-256 of both the public
surface and graph, module identity, node count, and bounds before semantic lookup.
If artifact markers are present, an invalid artifact fails import; it is not
silently reduced to bodyless declarations. Flat declaration and statement lists
are traversed iteratively, so list length does not consume the structural nesting
limit. Runtime, `bootstrap.*`, and `std.*` modules currently use their ordinary
interfaces without this payload; this is a user-template mechanism, not a
serialized shared frontend session. See the [generic interface gate](../probes/gates/generic_interfaces/README.md)
for the format limits and the std consumer's source-unit requirement.

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

[`MirUnit`](../bootstrap_compiler/src/mir/mir_model.vyx) retains the type
table and adds tables for functions, basic blocks, locals, fields, places,
values, instructions, and match cases. A block ends in a terminator such as
`return`, `goto`, `branch`, `match`, or `yield`.

| Concept | Question | Examples |
|---|---|---|
| Place | Where is data read or written? | Local, field, dereference, index, static storage |
| Value | What is computed? | Constant, read of a Place, operation, call, aggregate, cast |
| Instr | What happens? | Evaluation, assignment, drop, storage live/dead |
| Terminator | Where does control go? | Return, jump, branch, iteration |

HIR retains an `if` as a statement. [`lower_if_stmt`](../bootstrap_compiler/src/mir/mir_builder.vyx)
creates then, else, and join blocks with explicit edges. The builder also
places `drop`, `defer`, and storage-end operations on scope-exit paths. MIR
allows mutable locals, `assign`, and `read_place`; a `ValueId` does not make
the entire representation SSA. See [`mir_ids.vyx`](../bootstrap_compiler/src/mir/mir_ids.vyx)
for the current kinds.

`match` also produces values. The builder evaluates its subject once, lowers
pattern tests and guards to control flow, and joins value-producing arms through
a result place. Nested matches, payload bindings, block tail values, and returns
from arms have an [AOT compile/run gate](../probes/gates/match-expression/README.md).
Incompatible arm types, non-boolean guards, missing arm values, and empty value
matches have negative diagnostic checks.

[`mir_pass.vyx`](../bootstrap_compiler/src/mir/mir_pass.vyx) runs constant
folding, constant-branch folding, SCCP, copy propagation, drop-aware dead-code
elimination, select formation, and CFG simplification. At optimization levels
above 0, these passes run for a bounded number of rounds. MIR is verified and
checked against the DCI storage contract before LLVM lowering.

## Per-function work and CGUs

The current LLVM streaming path materializes, optimizes, lowers, and releases
MIR per function; those phases appear in
[`llvm_lower.vyx`](../bootstrap_compiler/src/codegen/llvm_lower.vyx). A code
generation unit (CGU) groups definitions for backend work and object emission.
This is distinct from running separate project build jobs concurrently.

The project scheduler uses typed actions, input-size estimates, observed memory,
and successful-task cost history. It reserves 3 GiB by default before admitting
jobs; the reserve arithmetic uses `i64`. Task-time history is scanned once per
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

## Continue reading

- Language behavior: [design](LANGUAGE_DESIGN.md) and [surface index](LANGUAGE_SURFACE.md).
- Compiler work: [bootstrap](../bootstrap_compiler/README.md) and [verification](TESTING_GUIDE.md).
- Metadata and interop: [MOSP](MOSP.md) and the [DCI reference](DCI_SPEC.md).
