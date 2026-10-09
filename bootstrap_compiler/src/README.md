# Compiler source tree

Start in the subsystem that owns the behavior. Directories describe compiler
responsibilities; `module bootstrap.*` names describe the public import boundary.
A logical module can span several files listed together in `Vyx.toml`.

```text
src/
├── core/
│   ├── driver/       CLI dispatch and command tools
│   ├── syntax/       tokens, lexer, parser, AST and diagnostics
│   ├── sema/         name/type checking, traits and template artifacts
│   ├── facts/        Effect, attributes and compiler facts
│   ├── dci/          contract decoding and adapter requests
│   ├── project/      manifests, dependencies, actions and scheduling
│   ├── tooling/      LSP and DAP entry points
│   └── runtime/      compiler host declarations
├── hir/
│   ├── model/        HIR records, IDs and type table
│   ├── builder/      declarations, generics, resolution and body construction
│   ├── resolve/      semantic, ownership and DCI facts
│   ├── verify/       HIR and AST fact checks
│   └── debug/        HIR dumps
├── mir/
│   ├── model/        MIR records, IDs and storage lifetime
│   ├── builder/      HIR lowering and reachable body materialization
│   ├── analysis/     CFG, uses and storage escape facts
│   ├── passes/       optimization pipeline, copy propagation and SCCP
│   ├── verify/       record, function scope and strict checks
│   ├── backend/      MIR backend interface
│   └── debug/        MIR dumps
└── codegen/
    ├── lower_pipeline.vyx  HIR preparation shared by backends
    ├── abi/          shared native/DCI ABI definitions
    ├── llvm/         LLVM lowering, calls, storage, DCI and emission
    └── cpp/          experimental C++ emitter
```

Use the [HIR builder map](hir/builder/README.md), [MIR map](mir/README.md),
and [LLVM map](codegen/llvm/README.md) for method-level entry points.
The [architecture guide](../../docs/COMPILER.md) explains the data flow.

## Source and build boundaries

- Keep new behavior beside the operation that owns it. Do not place unrelated
  implementations in a facade or create a general `utils` directory.
- A split class keeps its fields, construction and final release in its facade.
  Implementation files extend that class inside the same logical module.
  The split does not create independent compiler sessions or synchronization.
- The compiler target explicitly lists its source files; the LSP target lists
  its frontend dependency closure. Add new files to the relevant target groups.
  Type definitions precede their implementation files. Directory discovery is
  not a substitute for those module groups.
- Preserve existing signatures and visibility when moving methods. Other
  modules consume `.vyi`; their exported methods must remain available.
- HIR/MIR body records are materialized, checked, lowered and released per
  function. Keep IDs and caches within their documented lifetime.
- Use responsibility names such as `project/` and `builder/` for source directories.
  Reserve `build/`, `target/` and cache directories for generated artifacts.

`core/sema/sema.vyx`, `core/project/build_system.vyx` and the parser/driver still
contain large implementations. This tree establishes their ownership boundary;
it does not claim those files or their shared state have already been decomposed.

## 中文导航

先按阶段找目录，再按操作找实现。`core/syntax` 处理语法，`core/sema` 处理语义，
`core/project` 负责项目构建；HIR 保存语义事实，MIR 降低控制流并进行验证与优化，
`codegen/llvm` 负责原生代码生成。目录层级与逻辑模块名是两回事，移动文件不改变
`bootstrap.*` 导入接口。新增分文件实现时必须更新 `Vyx.toml` 中对应的模块组。
