# LLVM lowering source map

The entry point and `LlvmMirLowerer` storage live in
[`llvm_lower.vyx`](llvm_lower.vyx), module `bootstrap.llvm_mir_lower`.
This directory implements that same type with `impl` blocks. These source files
all declare the logical module `bootstrap.llvm_mir_lower`; the project builder
groups them with the entry file into one module object.
Start with the dispatch method for the MIR operation being changed, then follow
its calls into the files below.

```text
llvm/
├── llvm_lower.vyx   context storage, construction and public API
├── lower/          values, instructions and language operations
├── calls/          call dispatch, argument lowering and target resolution
├── functions/      signatures, frames and per-function streaming
├── storage/        places, globals, handles and ownership operations
├── lifetime/       local lifetime, unwind helpers and release
├── abi/            native representation and runtime declarations
├── strings/        string representation, operations and output
├── dci/            contracts, measured layout, ABI and exception boundaries
├── emit/           CGU partitioning and native worker support
└── debug/          source locations and diagnostics
```

## Dispatch and function compilation

| File | Responsibility / useful entry point |
|---|---|
| [values.vyx](lower/values.vyx) | MIR value dispatch: `lower_value`, `lower_value_as` |
| [instructions.vyx](lower/instructions.vyx) | MIR instruction dispatch: `lower_instr` |
| [control_flow.vyx](lower/control_flow.vyx) | Blocks, branches, returns and function bodies: `lower_terminator`, `lower_function` |
| [calls.vyx](calls/calls.vyx) | Call dispatch: `lower_call_value` |
| [call_resolution.vyx](calls/call_resolution.vyx) | Candidate indexes, executable call targets and effective receiver types |
| [call_arguments.vyx](calls/call_arguments.vyx) | Source arguments, receivers and self storage |
| [function_declarations.vyx](functions/function_declarations.vyx) | Function types, symbols, linkage and declarations |
| [function_frame.vyx](functions/function_frame.vyx) | Local slots, captured values and incoming parameters |
| [local_lifetimes.vyx](lifetime/local_lifetimes.vyx) | Lifetime markers, slot reuse and frame-invariant borrow handles |
| [streaming.vyx](functions/streaming.vyx) | Per-function materialization, optimization, lowering and release |
| [cgu.vyx](emit/cgu.vyx) | CGU collection, partitioning, retained bodies and object emission |
| [debug_info.vyx](debug/debug_info.vyx) | Source locations and local variable debug records |
| [diagnostics.vyx](debug/diagnostics.vyx) | Backend errors and candidate/signature diagnostics |

## Values, storage and language operations

| File | Responsibility |
|---|---|
| [type_layout.vyx](abi/type_layout.vyx) | Canonical types, record/tuple layout and LLVM types |
| [c_abi.vyx](abi/c_abi.vyx) | Native C aggregate classification, coercion, sret and byval |
| [pointer_handles.vyx](storage/pointer_handles.vyx) | Pointer handles, regions, checked access and alias scopes |
| [places.vyx](storage/places.vyx) | Local, field, index and dereference addresses |
| [global_storage.vyx](storage/global_storage.vyx) | Static initializers and HIR global storage |
| [aggregates.vyx](lower/aggregates.vyx) | Arrays, tuples, class aggregates and explicit allocation |
| [scalar_values.vyx](lower/scalar_values.vyx) | Casts, arithmetic, truthiness and scalar values |
| [enum_values.vyx](lower/enum_values.vyx) | Enum, Option and Result layout, construction and `?` |
| [pattern_matching.vyx](lower/pattern_matching.vyx) | Pattern conditions, payload binding and match branches |
| [collections.vyx](lower/collections.vyx) | Vec element operations and indexed/method iterator lowering |
| [ownership.vyx](storage/ownership.vyx) | Assignment retain/release, replacement storage and drop |
| [closures.vyx](lower/closures.vyx) | Environments, function values and callback adapters |
| [trait_dispatch.vyx](lower/trait_dispatch.vyx) | Dynamic trait layout, vtables, adapters and calls |
| [async_tasks.vyx](lower/async_tasks.vyx) | Task frames, workers, parameters, returns and yield |
| [reflection.vyx](lower/reflection.vyx) | Type/field queries, manifests, method pointers and hashing |
| [assertions.vyx](lower/assertions.vyx) | Runtime and static assertions |
| [memory_intrinsics.vyx](lower/memory_intrinsics.vyx) | Allocation, raw memory operations and atomics |
| [runtime_builtins.vyx](abi/runtime_builtins.vyx) | Runtime function declarations and ABI attributes |

## Strings and output

| File | Responsibility |
|---|---|
| [string_storage.vyx](strings/string_storage.vyx) | String representation, allocation and scalar conversion |
| [string_interpolation.vyx](strings/string_interpolation.vyx) | Interpolation segments and concatenation |
| [string_operations.vyx](strings/string_operations.vyx) | String views, conversion, cloning, slicing and search |
| [comparison.vyx](strings/comparison.vyx) | Scalar and string comparison |
| [printing.vyx](strings/printing.vyx) | Typed printing and formatted output |

## DCI contracts and calls

| File | Responsibility |
|---|---|
| [dci_contracts.vyx](dci/dci_contracts.vyx) | Contract loading, validation and indexes |
| [dci_symbols.vyx](dci/dci_symbols.vyx) | Type aliases, symbol matching, open instances and shared-link compatibility |
| [dci_layout.vyx](dci/dci_layout.vyx) | Contract/consumer layouts, field offsets and bitfields |
| [dci_casts.vyx](dci/dci_casts.vyx) | Base adjustment and contract-backed runtime casts |
| [dci_abi.vyx](dci/dci_abi.vyx) | Verified parameter/return lowering, inalloca and ABI attributes |
| [dci_arguments.vyx](dci/dci_arguments.vyx) | Argument materialization, coercion and return unmarshalling |
| [dci_dispatch.vyx](dci/dci_dispatch.vyx) | Direct receiver adjustment and virtual dispatch |
| [dci_unwind.vyx](dci/dci_unwind.vyx) | Exception boundaries, personality, invoke and translated results |
| [dci_bridges.vyx](dci/dci_bridges.vyx) | Contract-requested forwarding and override/stub factories |

## Module contract

- All implementations target `LlvmMirLowerer` in `bootstrap.llvm_mir_lower`.
  Moving a method must preserve its owner type, signature and visibility.
- `Vyx.toml` lists the facade and implementation files explicitly, with the
  type definition first. The compiler manifest uses `auto_sources = false`;
  a new implementation file must be added to this source list.
- The logical module shares its helper functions and state. Other compiler
  modules consume its shallow interface. Public methods in every implementation
  file must be included in the module object's export roots.
- Implementation blocks explicitly declare `@[vis(world)]` for SDK stages whose
  shallow emitter does not yet publish inherent implementations automatically.
  Method visibility is separate: the current emitter excludes non-public
  helpers from the public interface.
- Construction, auxiliary-storage disposal, shared helper declarations and
  final orchestration remain in the facade. Its state is still shared between
  these implementations; this split does not provide independent backend sessions.
- A MIR body is materialized, verified, lowered and released per function.
  Changes to `streaming.vyx` and `cgu.vyx` must preserve this bounded lifetime.
- Native ABI and DCI decisions require proven type/layout facts. Keep rejection
  diagnostics on paths that lack a usable contract.

## Global storage

`global_storage.vyx` consumes explicit MIR storage flags and the declaring
module identity. A loaded `.vyi` declares producer-owned mutable storage;
reading an interface must not create a zero-initialized consumer definition.
Public producer globals are emitted even when no producer function reads them.
Export-object compilation also retains private storage used by generic bodies
in the producer's template artifact.

The parser records each declaration's exact lexical module separately from
the importing file's transport identity. Sema and HIR retain that identity
through nested/reopened module blocks and interface serialization. Distinct
modules may reuse global and constant short names; `E2100` still rejects two
definitions in the same logical module. Shallow interfaces preserve module
scopes and scoped imports rather than hoisting declarations into the first
module of the file.

Verified template artifacts may materialize immutable constants and closed
associated storage in their consumers. These definitions use ODR coalescing,
with the canonical closed owner included in associated storage names. Root
partitions also coalesce storage, so updates remain visible across partitions.
Within one compilation, the first emitting CGU owns the definition and later
CGUs reference it externally. The static metadata scan does not retain MIR
function bodies.

Run the [cross-module gates](../../../../probes/gates/cross-module/README.md)
after changing these rules. They cover isolated interfaces, shared mutations,
same-named globals in distinct modules, template dependencies, root partitions,
multiple CGUs, and cold/warm/incremental project builds.

For source builds and the hello, fixed-point and DCI gates, use the
[compiler README](../../../README.md) and [verification guide](../../../../docs/TESTING_GUIDE.md).
