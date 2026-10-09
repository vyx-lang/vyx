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

| Operation | Entry point |
|---|---|
| Start HIR-to-MIR construction | [builder/mir_builder.vyx](builder/mir_builder.vyx): `MirUnitBuilder.create`, `create_for_codegen` |
| Expression and statement dispatch | [lower/expressions.vyx](builder/lower/expressions.vyx): `lower_expr`; [lower/statements.vyx](builder/lower/statements.vyx): `lower_stmt` |
| Calls and receiver selection | [calls/dispatch.vyx](builder/calls/dispatch.vyx): `lower_call_expr_value`; [method_resolution.vyx](builder/calls/method_resolution.vyx) |
| Value match and branch cleanup | [flow/match.vyx](builder/flow/match.vyx); [lifetime/branch_state.vyx](builder/lifetime/branch_state.vyx) |
| Moves, defer and drop | [lifetime/moves.vyx](builder/lifetime/moves.vyx), [drop.vyx](builder/lifetime/drop.vyx), [scope_exit.vyx](builder/lifetime/scope_exit.vyx) |
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
