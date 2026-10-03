# MOSP Effect AOT 验收门

本目录定义 MOSP Effect 的验收范围和后续编排入口。它以当前源码构建的
**SDK 编译器**和原生 AOT 为唯一基线；JIT 不属于本门。此门验证的是已经存在的
Migrate、Reflection、DCI、DCE 行为及其失败边界，并把增量失效等尚未具备稳定证据的
部分明确标记为 `skip` 或 `expected_fail`。当前编译器已执行 EffectPlan
归一化、handler 校验、义务诊断、manifest 导入/导出，以及 Reflection、DCE、Migrate、
platform、derive、comptime、async/task 和 DCI import 的真实 lowering/驱动边界接线；
`dci_export` 已连接到 AOT 导出根和显式 `VYX_DCI_EXPORT_OUT` 数据侧车；侧车只记录已
校验的声明事实，不冒充 C++/Rust/Zig ABI 合约。未接线的 handler 仍不会被写成“已完成”。

## 编译器身份

运行器必须显式接收 `-Compiler`（Windows）或 `--compiler`（跨平台），并在结果中
记录：

```text
compiler_path
compiler_sha256
compiler_version
target_triple
abi_profile
runtime_backend
```

默认值不得指向 `boot`、`seed`、`deprecated_seeds` 或旧 `out` 产物。若参数的文件名
包含 `boot`，或路径落在 `bootstrap_compiler/seed*`、`deprecated_seeds/`，运行器应
立即失败并给出修复提示；不能自动改用另一个编译器。普通 SDK 的 LLVM 后端来自本次
构建配套的 SDK；从源码重建时使用仓库 LLVM 22。

## 验收层级

每个夹具都必须完成 **构建、运行、证据检查** 三步。只生成 exe、只通过链接或只看
stdout 都不是成功。推荐运行器按以下顺序执行并输出一份 JSONL/JSON 摘要：

### A. Migrate（时间维度）

| 夹具 | 真实覆盖 | 证据 |
|---|---|---|
| `tests/projects/tutorial_migrate` | 当前版本和 `@1.0.0` 历史函数、字段、构造、方法调用 | AOT exit 0；历史调用结果与源码断言一致 |
| `tests/projects/versioned_module_import` | 字段别名、显式版本调用、重载和新增字段 | AOT 项目运行；生成的 `.vyi` 保留 `@[migrate]` |
| `tests/projects/versioned_module_discard_diagnostic` | 已丢弃历史入口被调用时的拒绝 | 必须 exit 1；包含 `E2400`、源文件位置和历史调用提示；不得到达链接阶段 |
| `tests/projects/versioned_field_migrate_diagnostic` | 缺失历史字段的定位诊断 | 必须 exit 1；包含 `E2400`、`add_v2.vyx` 和缺失字段；不得出现无关级联错误 |

这些夹具证明迁移关系和诊断；它们尚不能证明完整 EffectPlan（依赖边、输出事实、
义务）或自动数据格式迁移。Manifest 行和依赖现在按规范化字节序计算稳定 fingerprint，
门内覆盖了插入顺序无关、依赖变更失效和身份冲突拒绝；
尚未纳入本门的固定点比较。后两项不能被静默降级为警告。

### B. Reflection（编译期/运行时元数据）

至少运行：

- `tests/cases/tutorial_reflect.vyx`：登记名、别名、`getField`、绑定实例、写字段、
  绑定方法；使用 `--emit=exe` 后运行。
- `tests/cases/reflection_full_model.vyx`：继承接口、字段和方法别名、`@[hidden]`、
  `create/bind/write`、bound/raw 方法调用；使用 `--emit=exe` 后运行。
- `tests/cases/reflection_*_smoke.vyx`：按测试名分层运行，失败时保留独立诊断。

验收必须检查 `@[hidden]` 成员不能由运行时名称查询得到，登记别名能够解析到唯一
实体，绑定实例不转移所有权。`retain_effect.vyx` 还用 MIR 函数数、符号和 AOT
运行断言了未调用入口的保留；这条证据不替代 Reflection manifest 的独立 inspect
命令。

### C. DCI（空间、生命周期和异常维度）

按成本从小到大运行：

1. `probes/gates/dci-storage`：DCI 对象 storage、构造/析构/free 顺序和重复作用域。
2. `probes/gates/dci-exceptions`：C++ 异常传播、嵌套作用域清理、构造失败、共享
   ABI；Windows 与 Linux 路径分别记录目标和 ABI。
3. `probes/gates/dci-vector`：原始 C++ `std::vector<T>`，调用方选择 `i32/f64`，
   覆盖 brace 初始化、增长、借用访问、clear/pop、越界异常及存储释放。
4. `probes/gates/dci-cpp-ecosystem`：原始 ICU `UnicodeString`，由上游 Unicode 行为
   提供结果，不使用手写 wrapper。
5. `probes/gates/dci-rust-ecosystem`：locked Cargo 的 `crc32fast` / `adler2`，保留
   真实 `&[u8]` slice view 和生成的 native bridge。
6. `tests/projects/dci_multilang`、`dci_cpp_trait`、`dci_rust_trait`、
   `dci_abi_stress`、`probes/gates/dci-active`：回调、开放泛型、跨语言契约、离线
   bundle 重放和 corruption fail-closed。

所有 DCI 子门必须把契约解码、生产端工具链、目标、布局、所有权、分配域、析构和
异常模型作为事实证据。缺少其中任一项时应拒绝生成调用或返回 `expected_fail`；
不能用名称匹配、通用 raw pointer 或链接成功来填空。

### D. DCE（编译期可达性）

- `tests/cases/mir_dce.vyx` + `tests/checks/mir/mir_dce.ps1`：比较重复 O2 MIR dump，
  检查 effectful call、drop 和应清除的表达式；另运行 AOT 产物并核对 `mir_dce OK`。
- `tests/projects/extern_cpp_mir_abi`：检查 DCI 调用及 drop 在 MIR 中保留。
- Reflection 与 DCE 的联动：当前仅有行为夹具，没有稳定 manifest/symbol root 断言，
  该项必须输出 `skip (missing effect manifest/root evidence)`。

## Effect / Fact 证据缺口

编译器在 Sema 的 resolve 前阶段收集、冻结并释放 `EffectPlan`，
`VYX_PHASE_SUMMARY=1` 可看到事实计数和指纹；`VYX_EFFECT_MANIFEST_OUT/IN` 提供带
freshness 戳记的导出和消费边界。现有 debug 入口仍是 `--dump-hir2`、`--dump-mir2`
和 `--verify-hir-facts`。`tools/effect_manifest.py` 提供独立的 `inspect`/`validate`
命令，并使用与 SDK 相同的转义、字节排序和 fingerprint 规则。因此编排门不得把
尚未执行的项目写成已通过：

当前摘要已有独立的 AOT smoke 回归脚本：

```powershell
.\probes\gates\mosp-effect\run.ps1 -Compiler .\bootstrap_compiler\out\vyxc.exe
```

运行器必须显式传入本次 SDK 编译器，并把摘要、完整编译输出和 AOT 产物保存到
`.runs/<timestamp>/`。脚本拒绝 `boot`、`seed` 和 `deprecated_seeds` 路径，不会在
缺少 `-Compiler` 时替换成旧默认值。
脚本在成功和失败时都会恢复调用方的环境变量，避免 profiling 或测试契约影响后续构建。

该 smoke 当前有两组可执行证据：

- `fixtures/custom_attrs.vyx`：编译并运行带 `@[myAttr(...)]` 与 `@[reflect(...)]` 的
  AOT 程序，检查 `[MOSP effect]` 中的 `handled` 结果、`retain=true` 对未调用函数的
  MIR 保留、零 `unregistered` 和零 malformed，并核对程序输出。未注册属性仍只能
  作为 opaque fact 保留，不能触发 lowering。
- `fixtures/manifest_api.vyx`：把当前 `effect_manifest.vyx` 作为项目源编译，覆盖
  serialize/deserialize 的 fresh round-trip、target/ABI/scope/source/compiler/version
  不匹配、空行/未知行/未知转义拒绝、非负 flags、fact identity 冲突，以及事实和依赖
  反向插入仍产生相同 canonical fingerprint；依赖 digest 改变必须产生新的失效指纹。
  `flags` 是闭合状态枚举 `0/1/2/4`，组合值和未知位（例如 `3`、`8`）必须拒绝。
- `run.ps1` 会把同一编译器导出的 manifest 通过 `VYX_EFFECT_MANIFEST_IN` 重新送入
  一个**没有任何 Effect 属性的 consumer fixture**，检查 `effect-manifest accepted`、
  consumer AOT 可执行文件以及重新导出的 manifest 至少包含两条 `R` 事实；随后用错误的
  `VYX_EFFECT_SOURCE_STAMP`、`VYX_EFFECT_COMPILER_STAMP` 各运行一次，必须以非零退出
  并包含 `Effect manifest rejected: source-stale|compiler-stale`。因此当前门已经覆盖
  编译器边界的正向消费和 stale 拒绝，不再把这部分记为 skip。manifest 的依赖边、
  producer 内容 stamp 和 fingerprint 已参与消费校验；构建编排器必须把 producer
  stamp 传给 consumer，缺失时会 fail-closed，不会猜测依赖。
- `fixtures/retain_effect.vyx` 与 `retain_none.vyx`：两个未调用的 `kept` 函数只差
  `@[retain]`。MIR dump 必须分别有两个和一个函数，且只有 retain 版本包含 `kept`；
  retain 版本还需构建并运行 AOT。该项证明验证过的 Effect dispatch result 真正进入
  codegen reachability，不以 raw 属性文本直接授权 HIR root。
- `fixtures/retain_invalid.vyx`：`@[retain(keep=true)]` 违反 `none` 参数策略，必须
  以 `EFFECT-ARGS` 失败并且不能进入 HIR/MIR lowering。
- `../async/await_split.vyx`：`@[async]` 经 `async.lower` 结果进入 HIR task 标记，MIR
  必须产生 await yield（`term=7`），并完成原生 AOT 运行；`fixtures/async_invalid.vyx`
  用额外参数验证 `async` 的 `none` 策略以 `EFFECT-ARGS` fail-closed。
- `fixtures/stdlib_effect.vyx`：通过 SDK 的真实 `std.vio` 包注册表编译，调用其
  Windows 平台实现，并在同一单元保留 `@[platform("windows")]`。门同时检查
  `std.vio` 符号解析、Effect 摘要中的 handled fact/dispatch 计数和原生 AOT 输出，
  证明标准库包路径使用与探针相同的 EffectPlan，而不是只在孤立源文件上通过。
- `../derive/enum_unit.vyx` 与 `../comptime/fold.vyx`：分别检查 `derive.expand` 进入
  trait synthesis、`comptime.evaluate` 进入常量折叠，并运行生成的 AOT 程序。
- `fixtures/dci_export.vyx`：使用显式 `VYX_DCI_EXPORT_OUT` 选择器，检查已授权
  `dci_export` 函数同时成为 AOT 根并写出 `MOSP-DCI-EXPORT` 侧车；对没有
  `dci_export` 事实的单元使用同一选择器必须 `expected_fail`，不能生成空侧车。
  门随后调用 `tools/effect_manifest.py validate-dci-export --manifest`，检查侧车的
  EffectPlan fingerprint、规范行序、重复身份和 `registered` 授权状态；只有通过
  同次 manifest 的 plan digest 和完整导出事实集合比对，才算可供桥接产物消费。
  `check_artifact_provenance.py` 再对真实产物改动计划指纹、导出参数、授权状态和重复行，
  每个变体都必须拒绝，不能只凭相同文件名接受生成结果。

脚本只在各组 fixture 的构建、AOT 运行和证据断言全部满足时输出 `status=pass`；任何
未分类的编译、链接、运行或摘要缺失都会以非零退出。生成的 `result.json` 保留编译器
路径、SHA-256、版本、证据摘要、manifest 接受结果和两条 stale 拒绝结果。

- Manifest 行按字节序规范化后参与 fingerprint；同一组 facts/dependencies 的发现顺序
  不影响 `fingerprint`、`dependency_summary` 或序列化结果。依赖 digest 属于失效输入，
  变化必须改变两者。完整 EffectPlan 的跨进程固定点比较仍由自举门负责；本门只验证
  manifest 层的规范化证据；
- Migrate/Reflection/DCI 属性修改后的最小增量失效范围；当前 gate 已证明 Migrate
  字段别名在 HIR 侧只消费 `migrate.collect` marker，尚未覆盖完整增量失效集合；
- effect cycle；EffectPlan 现在维护独立的依赖边表，注册 Effect 会记录声明到 handler
  的边，新增边会用遍历全部分支的确定性 DFS 检查回路，并报告字典序最小的完整路径。回边会生成
  `EFFECT-CYCLE` obligation、增加 cycle 计数并在冻结前拒绝；manifest 使用 `D` 记录的
  `edge:<source>-><target>` 命名空间保存跨声明/模块边。`effect_cycle.vyx` 覆盖边行往返、
  插入顺序稳定性和 expected-fail 环诊断。
- DCI lifecycle/exception obligation 已进入统一事实图：
  `probes/gates/dci-exceptions/check_obligations.py` 先合并两个 producer sidecar，
  `fixtures/emit_dci_effect_manifest.py` 再生成 `dci.lifecycle`、`dci.exception` 和
  `edge:` 记录；本门的 `dci-effect-bridge` consumer 在 HIR 前导入它们并重新导出，
  检查 cleanup-before-propagate 边仍然存在。DCI 专门门继续覆盖实际析构、释放和异常
  运行时行为，Effect 门覆盖事实合并与 fail-closed 边界。
  直接 `--dci` 与源码自动发现的 `@[dci_import]` 也在 Sema 封存前导入同一
  `EffectPlan`；消费端会拒绝缺失符号、未解析边、环路和 shared_abi 缺少
  cleanup-before-propagate 的契约。规范节点名为 `dci:exception:*` 与
  `dci:lifecycle:*`。

这些检查在结果中使用 `status: "skip"`，并填写稳定的 `reason`。当实现了相应
manifest 后，才把同一用例切换为 `pass`；若实现明确拒绝某输入，则使用
`expected_fail`，而不是把失败吞掉。

## 自定义属性 `@[...]`

### `.attr` schema 输入

项目级 schema 由 `vyxc build` 自动加载：包根目录的 `*.attr` 会被发现，其他路径在
`Vyx.toml` 中通过 `[effect].attr_files` 列出。`auto_discover = false` 时只加载显式列表。
`.attr` 文件可以包含多个 `attribute` 块；schema 使用独立的声明格式，但参数类型复用
Vyx 的标量命名（`bool`、`i8`、`i16`、`i32`、`i64`、`u8`、`u16`、`u32`、`u64`、`f32`、
`f64`、`string`），并支持 `enum("a", "b")`。`.attr` 不接受模糊的 `int`，必须选择位宽。
`extends: [base.schema]` 可以复用其他 schema 的参数；解析完全部文件后统一解析前向引用。
未知基 schema、重复 schema 名、继承环和参数冲突都必须在源文件语义分析前失败。

门中的 `fixtures/attribute_project/` 覆盖包根自动发现、`Vyx.toml` 显式文件、多个 schema、
跨文件 `extends`、`i32` 边界值、默认值物化以及 AOT retain/record 后果。门会用同一缓存目录
执行冷编译和热编译，修改 `attrs/common.attr` 后要求 schema 摘要和缓存输入指纹发生变化。
这条路径不再要求用户每次设置 `VYX_EFFECT_SCHEMAS_IN`；该环境变量只保留作旧数据表兼容入口。

`attribute_source_api.vyx` 直接以当前解析器源码构建 AOT，覆盖全部标量的边界默认值、
越界拒绝、前向复用、继承环/冲突、必填字段、重复字段与块外垃圾文本拒绝。
另有包外显式文件的热缓存回归：首次构建通过后把共享文件改为非法类型，第二次构建
必须重新验证并失败。`auto_discover = false` 夹具在根目录放置未选中的非法 `.attr`，
确认显式列表可以构建运行、schema 文件不会混入 Vyx 源码注册表。
`interface_cache` 覆盖库接口的冷/热一致性：stub 与 `emit_vyi=false` 都不生成 `.vyi`，
启用接口的库在接口文件缺失后，热构建必须恢复相同内容。

解析器可以保留未知属性文本。现有子系统继续处理内建属性；Effect ledger 会把它们
归一化并按 schema 校验。当前已注册 `reflect`、`Reflect`、`REFLECT`、`retain` 和声明式 `myAttr`；此外，
`VYX_EFFECT_SCHEMAS_IN` 可加载经过严格校验的 `MOSP-EFFECT-SCHEMAS\t1` 数据表，把包提供的
typed `package.attribute` 绑定到 compiler-owned generic handler。前者授权
Reflection metadata，`retain` 授权 DCE codegen root，`myAttr` 由 compiler-owned
`record.myAttr.validate` schema handler 严格校验 `level`/`retain`，并可授权同一
retain 能力；schema 表还会产生规范化参数和 retain/reflect 后果。其他未知属性只进入
`unregistered` 事实，不会触发 lowering。schema 内容摘要进入 manifest dependency，schema
变化会触发 `schema-stale` 拒绝。Effect 门仍分两阶段：

1. **保留门（opaque 属性）**：用临时 `.vyx` 写入类似
   `@[demo.note(level="abi", retain=true)]` 的不透明属性，运行 AOT 并在
   `VYX_PHASE_SUMMARY=1` 中确认它进入 `unregistered` fact 计数、机器码语义不改变。
   规范化 `.vyi`/跨模块元数据保留还没有稳定 dump，应标为 `skip`；未注册属性不能
   触发 codegen。
2. **扩展注册门**：`myAttr` 和 schema 数据表是当前完整参考 handler；新增 handler 仍必须验证
   schema、目标类型、默认参数、重复规则、限定身份、handler 版本、capability、
   跨模块导出和显式 Migrate 映射。未知 handler、参数类型错误、目标错误和 capability
   缺失必须有可定位诊断；DCI import/export 除了生成计划记录，还把 source-discovered
   `.dcib` 交给 build/codegen 前的 compiler-owned marker 检查；错误目标必须在 DCI
   binder 之前失败。

## 结果格式和退出规则

建议每个测试写一条结构化记录（同时保留完整 stdout/stderr、契约和编译日志）：

```json
{
  "fixture": "dci-vector",
  "compiler_path": "E:/.../bootstrap_compiler/out/vyxc.exe",
  "compiler_sha256": "...",
  "target": "x86_64-pc-windows-msvc",
  "abi": "dci.eh.msvc-cxx.v1",
  "build_exit": 0,
  "run_exit": 0,
  "evidence": ["contract_digest", "allocation_balance", "exception_identity"],
  "status": "pass",
  "reason": ""
}
```

`status` 只有四种值：

- `pass`：构建、运行和全部该层证据均满足；
- `fail`：实际行为或证据不满足，退出码必须非零；
- `skip`：平台/工具/尚未提供的可观测证据使本项未执行；推荐进程退出 77；
- `expected_fail`：用例明确验证拒绝路径，诊断和锚点全部匹配。

`expected_fail` 不能用于掩盖崩溃、超时、空日志或未执行的正向夹具。任何未分类的
异常都应让编排器退出非零。结果中的路径必须指向本次构建产物；旧测量、seed、
临时 boot 和生成物目录不能成为验收依据。

## 推荐执行顺序

```powershell
$compiler = (Resolve-Path .\bootstrap_compiler\out\vyxc.exe).Path
if ([IO.Path]::GetFileName($compiler) -match '(?i)boot|seed') { throw 'SDK compiler required' }

# 先运行已有单项门；每个脚本都必须把 -Compiler 传入而不是使用旧默认值。
.\probes\gates\dci-vector\run.ps1 -Compiler $compiler
.\probes\gates\dci-cpp-ecosystem\run.ps1 -Compiler $compiler
.\probes\gates\dci-rust-ecosystem\run.ps1 -Compiler $compiler
.\probes\gates\dci-exceptions\run.ps1 -Compiler $compiler
```

上面只是夹具入口示例；真正的 Effect 编排器还必须验证契约和元数据摘要，不能把
四条命令的 exit 0 合并成“Effect 已完成”。跨平台缺少 rustc、C++ ABI、ICU 或
Linux 异常环境时，记录 `skip` 和具体工具缺口。

## 当前实现边界

本门包含上下文相关的顶层 `effect` 声明回归。它为统一 Fact/Effect manifest 保留稳定的 AOT 回归
基线：声明必须进入 EffectPlan，但不能生成 HIR item 或运行时符号。JIT 对齐、
任意 Rust panic 转换和未列入能力矩阵的 C++/Rust 类型不属于本门范围。
