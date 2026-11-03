# MIR source map

MIR records explicit blocks, places, values, instructions and terminators.
The builder consumes HIR facts; analysis and optimization operate on MIR;
verification checks the invariants required by the native backend.

```text
mir/
├── model/
│   ├── mir_model.vyx       MirUnit storage and public forwarding API
│   ├── mir_ids.vyx         record kinds, flags and ID allocation
│   ├── records/           function/block/local/place/value/instruction records
│   └── lifetime.vyx       record storage release
├── builder/
│   ├── mir_builder.vyx    MirUnitBuilder state, construction and final release
│   ├── lower/             expression/statement dispatch and value operations
│   ├── flow/              branches, loops, match and control expressions
│   ├── calls/             arguments, candidate selection and call lowering
│   ├── functions/         HIR materialization, roots, queue and body streaming
│   ├── lifetime/          moves, drops, branch state and scope exit
│   ├── storage/           locals, places, temporaries and static initialization
│   ├── types/             equivalence, templates, trait and record layout
│   ├── debug/             diagnostics and retained source locations
│   └── support/           lookup indexes, text operations and profiling
├── analysis/              CFG/use indexes and local storage escape analysis
├── passes/                pass pipeline, copy propagation and SCCP
├── verify/
│   ├── mir_verify.vyx     verifier state and public entry points
│   ├── records/           record IDs, structure, values and instructions
│   ├── scope/             checks restricted to the current function's ranges
│   ├── strict/            whole-unit CFG, call, layout and lifetime checks
│   └── dci/               contract storage obligations
├── backend/               backend selection interface
└── debug/                 readable MIR dumps
```

## Where to change behavior

`builder/flow/iteration_protocol.vyx` constructs typed, ordinary iterator calls
and Option branches. Collection normalization uses verified Vec/String provider
flags. The returned element enters the iteration cleanup scope; None never
initializes an element owner. This preserves the normal ABI and ownership paths
for aggregate payloads instead of rebuilding them inside LLVM foreach lowering.

Fixed arrays use ordinary Clone calls for nontrivial elements and structural
loops for nested arrays. `builder/lifetime/arrays.vyx` emits reverse element
cleanup, including temporary construction guards. Array local/temporary drop
metadata names the homogeneous leaf destructor; it must never be called with
the array as its receiver. DCI shared-unwind registration currently rejects
owning arrays until structural EH callbacks exist.

Named owning enum payload bindings enter the selected match arm's drop and
storage scope after its guard succeeds. Returning a binding or using it as the
match expression result transfers that lifetime to the destination. Matching
a borrowed receiver keeps payload bindings borrowed.

| Operation | Entry point |
|---|---|
| Start HIR-to-MIR construction | [builder/mir_builder.vyx](builder/mir_builder.vyx): `MirUnitBuilder.create`, `create_for_codegen` |
| Expression and statement dispatch | [lower/expressions.vyx](builder/lower/expressions.vyx): `lower_expr`; [lower/statements.vyx](builder/lower/statements.vyx): `lower_stmt` |
| Calls and receiver selection | [calls/dispatch.vyx](builder/calls/dispatch.vyx): `lower_call_expr_value`; [method_resolution.vyx](builder/calls/method_resolution.vyx) |
| Value match and branch cleanup | [flow/match.vyx](builder/flow/match.vyx); [lifetime/branch_state.vyx](builder/lifetime/branch_state.vyx) |
| Moves, defer and drop | [lifetime/moves.vyx](builder/lifetime/moves.vyx), [drop.vyx](builder/lifetime/drop.vyx), [scope_exit.vyx](builder/lifetime/scope_exit.vyx) |
| Temporary owners and borrowed views | [storage/full_expression.vyx](builder/storage/full_expression.vyx): materialization, statement cleanup, conditional moves and view lifetime extension |
| Late generic bodies and reachability | [functions/hir_materialization.vyx](builder/functions/hir_materialization.vyx), [reachability.vyx](builder/functions/reachability.vyx) |
| Function body lowering | [functions/body.vyx](builder/functions/body.vyx): `build_function_body` |
| Streaming and verification | [functions/streaming.vyx](builder/functions/streaming.vyx); [verify/mir_verify.vyx](verify/mir_verify.vyx) |
| Record access and layout | [model/records/](model/records/); [builder/types/layout.vyx](builder/types/layout.vyx) |
| Optimization order | [passes/mir_pass.vyx](passes/mir_pass.vyx) |
| CFG and storage escape facts | [analysis/mir_analysis.vyx](analysis/mir_analysis.vyx) |
| DCI storage checks | [verify/dci/storage.vyx](verify/dci/storage.vyx) |

## Ownership and lifetime

`bootstrap.mir_builder`, `bootstrap.mir_model` and `bootstrap.mir_verify` each
remain a single logical module. Implementation files share their module's
private helpers. Public class and method signatures remain the import contract.
The facades own state; moving an accessor does not change the record layout.
Public implementation blocks use the complete owner name (for example,
`impl bootstrap.mir_model.MirUnit`) so the shallow interface binds them to the
qualified class declaration when imported.

`MirUnitBuilder` borrows the HIR codegen handle. The driver/LLVM pipeline owns
its release and free. Function materialization can recursively discover more
functions, so verification uses append-range marks rather than scanning all
resident bodies. Do not replace the scoped checks with the whole-unit verifier
inside the streaming path. Body record IDs cannot survive record discard.

The trees separate source responsibilities. `MirUnit` still owns raw record
tables, and the builder shares lookup caches and queues; they are not safe for
concurrent mutation. `analysis/mir_analysis.vyx` and `passes/mir_pass.vyx` still
contain concentrated implementations to decompose separately.

## Resolved call boundary

A source direct call enters MIR with a selected HIR FunctionId and a closed
signature. Mapping that identity to a MIR function must preserve its HIR ID.
Missing IDs are errors; expression names cannot recover them. A concrete
receiver cannot retain an interface requirement as its executable target;
dynamic receivers retain the selected interface slot for vtable dispatch.
The call lowering path must not search the receiver's method family again.

Source operators and class indexing follow the same identity rule. Overload
scoring and generic method materialization belong to HIR. Typed `*T` indexing
selects T storage, regardless of any operator declared on T. Fixed arrays and
primitive text keep their intrinsic indexing rules. Newly materialized HIR
bodies check function bindings over their own append range before MIR lowering;
this does not retain or rescan whole-unit bodies.

The build driver distinguishes the complete unity input list from the authored
source list (`--project-owned-sources`). Source dependencies provide bodies,
but do not become the consumer's export roots. Package discovery stops at
nested `Vyx.toml` boundaries; compiled dependencies supply their published VYI
through their resolved artifact directory instead of recursive source scans.
The source-dependency marker authorizes lazy body materialization without
granting export ownership. MIR builder diagnostics restore the source file of
the materialized function before using its expression's line and column.

Iteration stores selected `iter`, `next`, optional `has_next` and array leaf
`clone` FunctionIds on its HIR statement. HIR closes their receiver-only
signatures before MIR. MIR maps those identities directly, without searching
method families again. The bindings are checked over the materialized statement
range and survive body streaming; they do not retain function bodies.
Protocol instantiation also receives the collection's HIR expression identity
for constraint diagnostics. Resolve its location when emitting the diagnostic;
do not retain a pointer into a growing HIR table or reuse Sema's last location.
Dynamic conversions bind each interface prototype to its concrete HIR method
using the complete parameter/return signature and declared inheritance. MIR
retains `(payload TypeId, prototype FunctionId, implementation FunctionId)`
bindings across body discard and CGU partitioning. LLVM consumes these bindings;
it does not select implementations from a method-name index or saved local
chains. The MIR unit owns the binding records; streaming releases them after
module finalization. Async support still has separate lookup entry points to
audit and does not authorize a source-call fallback.
CGU membership comes from the executable reachability walk. A retained function
shell can describe an external library symbol or an interface prototype; its
presence cannot promote it into a definition or another export root.
Verified imported template artifacts can materialize private helper bodies.
When those helpers cross CGUs, they retain ODR linkage so a separately linked
library's canonical export does not collide with a new strong definition.
Ordinary public free functions loaded from a compiled interface remain owned
by the producer library even when the AST artifact transports their bodies.
Source-package bodies and closed generic instances retain their distinct
materialization paths; body availability does not grant export ownership.
For native linkage diagnostics, `VYX_LLVM_LINKAGE_TRACE=1` prints the function
flags, CGU identity and chosen internal/ODR linkage at definition emission.

## Validation

Build the SDK compiler from this tree, then run hello `--dump-mir2`,
[match expressions](../../../probes/gates/match-expression/README.md),
[compiler usability](../../../probes/gates/compiler-usability/README.md),
[cross-module definitions](../../../probes/gates/cross-module/README.md) and
[module contracts](../../../probes/gates/compiler-modules/README.md).
DCI/class changes also require the DCI projects named in the root `AGENTS.md`.
The [testing guide](../../../docs/TESTING_GUIDE.md) defines the self-host gate.

## 中文导航

表达式和语句降低在 `builder/lower/`，分支、循环和 match 在 `builder/flow/`，
参数与调用目标在 `builder/calls/`。排查释放问题时从 `builder/lifetime/` 开始；
排查按函数编译时从 `builder/functions/` 开始。`model/records/` 只处理记录存储，
不应塞入类型推导或优化策略。`verify/scope/` 用于流式函数范围，
`verify/strict/` 用于完整单元检查，两者的扫描边界不可混用。
