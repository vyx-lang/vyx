# Effect 模型规范

[English](MOSP_EFFECT.md) · [事实语义所有权系统](MOSP_ZH.md) · [文档目录](README.md)

状态：设计与当前实现并列。下文区分规范要求和已经由 AOT 编译器执行的路径。

当前实现状态：Sema 会把 `@[name(...)]` 与 `@[name]` 归一化为
`EffectDecl`/`EffectFact`，校验 handler 的目标和参数策略，记录确定性的计划结果与
编译义务，并输出 `[MOSP effect]` 摘要。`VYX_EFFECT_MANIFEST_OUT` 导出带版本和转义的
单元 manifest；`VYX_EFFECT_MANIFEST_IN` 在校验 target、ABI、scope、source、compiler
戳记后把生产端事实合并到消费端计划。`reflect` handler 会授权 Reflection 元数据，
`retain` handler 会授权 HIR 的代码生成根（`dce.retain`）；`migrate.collect` 会授权
字段别名物化，`async.lower`/`task.lower` 会授权协程任务标记。这些 lowering 只消费
编译器生成的 marker，不直接信任原始属性文本。`platform.select` 也会在语义解析前
授权声明筛选；`derive.expand` 和 `comptime.evaluate` 分别控制 trait synthesis 与
常量折叠。`dci.import` 会把授权 marker 写入完整声明树；源码自动发现的 `.dcib` 只有
在 import marker 封存后才能进入生命周期绑定器。`dci.export` 已完成归一化并写入声明树
marker；它会把函数保留为 AOT 导出根，且可通过 `VYX_DCI_EXPORT_OUT` 选择稳定的
`MOSP-DCI-EXPORT` 数据侧车供桥接工具消费。该侧车不是外部 ABI 合约，C++/Rust/Zig ABI
合约仍由 DCI 适配器生成。显式 `--dci` 与源码自动发现的
`@[dci_import]` 都会在 Sema 封存计划前解析；其中的生命周期、异常、符号、依赖边和
`shared_abi` 清理义务会进入同一个 EffectPlan。即使没有桥接 manifest，契约格式错误或
义务不完整也会让 Consumer 失败。

当前 AOT 完成证据还包括：`myAttr` 的 compiler-owned 声明式 schema handler（
`level="abi"|"layout"`、可选 `retain=bool`）已经完成参数、目标和跨 manifest 导入校验；
合法属性产生 `handled` 结果，`retain=true` 才能授权未调用声明进入 MIR。
`tools/effect_manifest.py inspect/validate` 与 SDK 使用同一 canonical 行排序、转义和
fingerprint 规则，可独立检查产物；DCI 源文件内容 stamp 也进入 manifest freshness。
`probes/gates/mosp-effect/run.ps1`、`dci-exceptions`、`dci-vector` 和 9 个真实 DCI
项目均以当前构建的 SDK 编译器执行。跨包构建编排仍必须显式传递 producer 的 source/compiler
stamp；缺失时消费端拒绝 manifest。

包提供的声明式 schema 也已经接入同一条路径。项目构建会在编译 Vyx 源码前加载包根目录的
`.attr` 文件，以及 `Vyx.toml` 中 `[effect]` 指定的文件。一个文件可以定义多个 `attribute`
块，使用 Vyx 标量类型名，并通过 `extends` 复用其他 schema 的参数。构建会合并依赖包的
schema、校验完整集合，并把文件内容纳入 object/interface 缓存键，无需每次设置输入环境变量。

兼容入口仍可通过 `VYX_EFFECT_SCHEMAS_IN` 提供并先验
证的 `MOSP-EFFECT-SCHEMAS\t1` 数据表可以注册带限定名的 `package.attribute` handler，
兼容 bool/int/string/enum 参数、规范默认值、声明目标以及 retain/reflect 后果；它只加载
数据，不执行 compiler plugin。schema 内容摘要会写入 manifest 依赖并由 consumer 比对；
schema 文件内容变化会让原本其他字段匹配的 manifest 失效。格式错误、未知目标或类型、
非法参数以及 schema 与内建 handler 冲突都会 fail closed。
侧车通过 `VYX_DCI_EXPORT_OUT=/path/to/file` 选择，字段使用与 Effect manifest 相同的转义
制表格式：首行是 `H\tMOSP-DCI-EXPORT\t1\t<unit>\t<plan-fingerprint>`，后续是
`R\t<subject>\tdci_export\t<arguments>\t<phase>\t<scope>\tregistered`。
首行 fingerprint 是封存 EffectPlan 中实际授权导出的指纹。桥接器或构建缓存消费生成的
声明和 native 对象前必须先检查侧车；`python tools/effect_manifest.py
validate-dci-export <sidecar> --manifest <同次构建manifest>` 会检查 header、转义、规范行序、
重复身份和 compiler-authorized 状态，再与同次 manifest 的 plan 依赖指纹和完整导出事实集合
比对。这项检查不替代外部 ABI 校验，而是把 ABI 校验绑定到明确的 EffectPlan 来源。

EffectPlan 现在还拥有类型化的依赖边表。已注册 Effect 会记录“声明 → handler”边，导入的
manifest 可以继续加入跨模块边；边检查使用确定性的字典序 DFS，回边会生成
`EFFECT-CYCLE` 编译义务并阻止计划封存。DCI lifecycle/exception 义务也走同一个 manifest
边界：依赖检查使用完整的确定性 DFS，会遍历每个出边分支并报告字典序最小的完整环路；
回边会生成 `EFFECT-CYCLE` 义务并阻止计划封存。DCI 校验器先合并生产模块，桥接器再把
`dci.lifecycle`、`dci.exception` 事实和 `edge:` 依赖写入 `VYX_EFFECT_MANIFEST_IN`
可消费的格式。消费端在 HIR 之前把这些行导入当前 EffectPlan，因此清理-再传播边、未解析
节点、冲突和依赖环由同一事实图校验。导入后的规范节点 ID 是
`dci:exception:<subject>` 与 `dci:lifecycle:<subject>`。

## 1. 范围与目标

事实语义所有权系统把经过验证的程序事实作为显式编译对象。Migrate、Reflection、DCI、DCE、Effect 是五大核心特性。Effect 定义事实的消费规则和语义后果，与 Migrate 的版本关系共同支撑有效范围、依赖与失效管理。

Effect 不是运行时 IO、内存写入、线程调度或异常本身的类型系统。它描述的是元信息对事实图、编译阶段、诊断和产物的影响。一个运行时纯函数可以产生 Effect；一个运行时会写内存的函数也可以没有 Effect。

本规范的目标是：

1. 让 Migrate、Reflection、DCI、DCE、derive、comptime、async 和平台选择使用同一套事实传播模型。
2. 让每次元信息传播都能追溯到输入事实、处理器版本和输出产物。
3. 让布局、所有权、生命周期、异常模型和编译期 lowering 可以跨模块验证。
4. 让增量构建根据事实指纹失效，而不是依赖隐藏的全局编译器状态。
5. 让自举编译器对自身的元信息影响可复现、可审查、可固定点验证。

本规范不授予普通源代码任意修改编译器的能力，也不要求 Consumer 解析 C++、Rust 或其他生产端源语言。

## 2. 规范性词汇

本文使用以下词语：

- **必须（MUST）**：不满足时实现不能声称符合本规范。
- **不得（MUST NOT）**：实现禁止执行该行为。
- **应（SHOULD）**：默认要求；有充分理由时可以偏离，但必须记录原因。
- **可以（MAY）**：实现可选能力。

## 3. 事实语义所有权系统 五个维度

| 维度 | 要回答的问题 | 典型事实或机制 |
|---|---|---|
| 时间 | 实体在哪个版本、历史区间或迁移阶段有效 | `version`、`variant`、Migrate |
| 空间 | 事实在哪个目标、ABI、布局、模块或分配域有效 | target、layout、DCI、allocator domain |
| 运行时 | 生成的程序怎样调用、拥有、销毁和传播错误 | 生命周期、异常、async、调用约定 |
| 编译期 | 编译器在什么阶段发现、保留、生成或降低代码 | Reflection、DCE、derive、comptime、MIR lowering |
| 事实 | 编译器凭什么知道、事实来自哪里、改变后影响什么 | Fact、provenance、Effect |

事实维度与其他四个维度交叉。它不是又一个运行时区域，而是程序事实的知识、来源、验证状态和因果关系。Effect 是事实维度的执行语义：事实说明“知道了什么”，Effect 说明“知道后必须发生什么”。

## 4. 核心定义

### 4.1 实体

实体是 Effect 可以观察或影响的稳定对象，例如模块、类型、字段、函数、方法、泛型实例、DCI 操作、Reflection 入口和编译器阶段。实体必须有稳定身份；显示名称不能单独作为身份。

实体身份至少包含：

```text
package identity
module identity
version / variant
declaration kind
declaration identity
generic arguments when closed
```

### 4.2 事实

事实是经验证、可引用、带作用域的程序知识。逻辑模型如下：

```text
Fact {
    key: FactKey
    subject: EntityId
    value: CanonicalValue
    scope: ValidityScope
    provenance: Provenance
    dependencies: [FactRef]
    status: observed | verified | derived | external
    fingerprint: Hash
}
```

事实的 `key`、实体、值和作用域共同决定语义身份。来源和依赖决定它能否被信任和失效。事实值必须是规范化表示，不能把未解析的外部源代码片段当作已验证事实。

### 4.3 有效范围

`ValidityScope` 可以包含：

```text
target triple / operating system
ABI profile and calling convention
toolchain identity and version
build mode and feature flags
package / module version and variant
allocator or runtime domain
source artifact identity
```

消费端只有在自己的作用域包含事实作用域时，才可以使用该事实。作用域不匹配必须报错或明确降级，不能通过名称相同或布局相似来放宽。

### 4.4 Effect

Effect 是一个声明式、阶段受限、可验证的事实变换。它读取一组事实，检查约束，并产生新的事实、编译义务、失效请求、声明或产物。

```text
Effect(subject, phase, read_facts)
    -> add_facts
    -> invalidations
    -> obligations
    -> declarations
    -> artifacts
    -> rerun_requests
```

Effect 的写入目标是事实图和已注册的编译阶段能力，不是任意的编译器全局变量。每个 Effect 必须有稳定 ID、处理器版本、输入指纹和来源。

### 4.5 Effect handler

目标模型中的 Effect handler 是编译器注册的确定性处理器。它定义 Effect 的输入
schema、读取事实、输出 schema、可用阶段和能力。当前代码已经有 schema 和确定性的
dispatch 结果表；`reflect.collect`、`dce.retain`、`migrate.collect`、async/task
lowering、`platform.select`、`derive.expand` 和 `comptime.evaluate` 已连接真实消费者；
内建 schema 带有确定性的版本和 capability 标签，schema digest 会写入导出 manifest 的
依赖项；
`dci.import` 已连接到驱动器和 codegen 的授权边界：源码自动发现的 `.dcib` 必须带
compiler-owned import marker，显式 `--dci` 仍可服务 inline/direct 合约。`dci.export`
已连接到 AOT 导出根和显式 `VYX_DCI_EXPORT_OUT` 侧车边界；它不会猜测外部 ABI 布局。
只有 SDK 编译器 handler 可以执行，
普通包不能向编译器进程注入宿主代码。

### 4.6 Effect plan

`EffectPlan` 是目标模型中在完成版本解析和 Migrate 规范化后生成的执行计划。完整
计划应包含排序后的 Effect 实例、输入事实指纹、依赖边、阶段边界、输出事实和待验证
义务。当前实现保存规范化 `EffectFact` 行、handler 元数据、确定性的结果和义务表、类型化
依赖边表、计数器，以及冻结时折叠五张表的指纹。紧凑 manifest 编解码器已经
支持跨模块导出和导入；CLI 只有在显式 freshness 戳记通过后才会合并输入事实。

## 5. Effect 的结果类别

Effect 可以产生以下结果。一个 handler 可以同时产生多类结果，但每一类都必须显式列出。

### 5.1 派生事实

例如由布局事实派生字段偏移，由 Migrate 派生历史别名，由 DCI 契约派生生命周期能力。派生事实必须记录依赖，不得伪装成生产端直接观察到的事实。

### 5.2 编译义务

义务是必须在当前或后续阶段完成的验证，例如：

- owned 值必须存在匹配的 destroy、release 或 allocator domain；
- DCI 边界必须有双方共同支持的异常传播方式；
- Reflection 名称必须唯一且入口可调用；
- Migrate 字段必须存在于指定历史版本；
- 泛型实例必须满足生产端约束并具有闭合布局。

义务未完成时，构建必须失败。Effect 不得把未完成义务转换为空诊断。

### 5.3 声明和桥接产物

Effect 可以请求生成已经由编译器注册的声明、兼容 shim、Reflection manifest、DCI stub 或 drop glue。生成产物必须带来源 Effect 和输入 fingerprint，不能在源码树中隐式出现一个无法追踪的声明。

### 5.4 保留和裁剪

Effect 可以增加 DCE root、保留元数据、声明动态调用入口，或报告某些事实已失效。它不能直接删除仍被导出、反射、析构或外部契约需要的内容。

### 5.5 Lowering 和代码生成请求

Effect 可以选择已注册的 HIR、MIR 或 LLVM lowering，例如 async 状态机、comptime 折叠、derive 方法和 ABI 调用序列。它不能改变 lowering 的未声明输入，也不能通过名字匹配推断缺失布局。

### 5.6 失效和重跑

Effect 可以声明局部函数、模块、目标或全局事实需要重算。失效范围必须是最小可证明范围；未知范围必须扩大到安全范围或报错，不能静默保留旧产物。

## 6. Effect 声明模型

编译器现在接受顶层、上下文相关的 `effect` 声明。现有 `@[migrate]`、`@[reflect]`、`@[dci_import]`、`@[derive]`、`@[comptime]`、`@[async]`、`@[platform]`、布局属性以及 `@[name(...)]` 自定义形式仍会归一化为结构化 `EffectDecl` 和 `EffectFact`。声明是编译期数据：它进入 EffectPlan 和 manifest，但不会变成函数或运行时符号。

```text
effect reflect_alias {
    phase: metadata;
    reads: [migrate.field_alias, reflect.registration];
    provides: [reflect.name_alias];
    requires: [stable_identity];
    capability: [retain_metadata];
}
```

`effect` 只在声明位置按上下文识别，因此表达式位置原有的 `effect` 标识符和成员名保持有效。必填字段是 `phase`、`reads`、`provides`、`requires` 和 `capability`；`writes`、`handler`、`version`、`scope`、`determinism`、`diagnostics` 可选。字段不能重复，值只保留为数据 token 列表；未知、重复、空值和格式错误都会诊断。

没有 `handler` 的声明是纯声明式事实契约。显式 `handler` 必须解析到编译器已注册的 schema，可选 `version` 必须匹配。这样源码可以表达包契约，同时不会引入可执行的编译器插件。

EffectDecl 至少包含：

```text
effect_id
handler_id and handler_version
phase
read fact patterns
write fact keys
required capabilities
scope constraints
determinism declaration
diagnostic policy
```

`reads`、`writes` 和 `requires` 是审计边界。handler 读取未声明事实，或者写入未声明事实，必须失败。

## 7. 编译阶段与执行顺序

Effect 按以下逻辑阶段执行：

| 阶段 | 主要工作 | 可产生的 Effect 结果 |
|---|---|---|
| Parse | 收集属性和声明来源 | 原始 Effect 请求 |
| Identity | 解析模块、版本、变体和实体身份 | 身份事实 |
| Migrate | 规范化历史实体关系 | 迁移事实、兼容义务 |
| Semantic | 类型、泛型、可见性和约束检查 | 类型和约束事实 |
| Layout / ABI | 计算布局、调用约定、生命周期和分配域 | ABI、布局、drop 事实 |
| Metadata | 固定 Reflection、DCI、导出和保留信息 | manifest、根集合、桥接请求 |
| MIR | 执行已注册 lowering 和局部验证 | MIR 请求、函数级事实 |
| DCE / Codegen | 计算可达性并生成目标代码 | 机器代码、符号和失效摘要 |
| Link | 验证最终产物身份和依赖 | 链接事实、最终报告 |

版本解析和 Migrate 必须先于依赖它们的 Effect。Effect 只能写入当前或后续阶段，不能通过隐式回写破坏已经冻结的事实。

自举编译器仍然保持函数级 MIR streaming。全局 Effect 信息通过上面的紧凑 manifest 和
摘要路径跨单元传递。不得为了计算 Effect 恢复 whole-unit 二次扫描和大内存模型。

## 8. 计算语义

### 8.1 确定性

同一组源码、工具链、目标、配置、输入事实和 handler 版本，必须产生相同的 EffectPlan 和输出 fingerprint。Effect 不得读取时间、随机数、网络响应、未声明的环境变量或进程全局状态。

### 8.2 单调事实和显式失效

默认事实图采用单调增长模型：Effect 可以添加经过验证的新事实，但不能静默覆盖旧事实。需要替换或撤销时，必须产生显式 invalidation，并说明旧事实为何失效。

FactBase 冻结后，新的写入必须通过下一代 EffectPlan 进行。write-after-freeze 不能被静默吸收。

### 8.3 固定点

Effect 允许在有限单调事实格上迭代到固定点。每轮都必须：

1. 使用当前事实快照运行 handler；
2. 规范化并排序输出；
3. 检查新增、失效和冲突；
4. 对下一轮使用新的 fingerprint。

如果 Effect 产生无限新实体、非单调循环或无法确定的顺序，必须诊断失败。实现不得用最大迭代次数截断后报告成功。

### 8.4 冲突

两个 Effect 对同一事实键产生不同规范值，或者对同一符号产生不同生成声明时，构建必须报告冲突。冲突诊断至少指出：两个 Effect 的来源、输入事实、作用域和处理器版本。

### 8.5 能力检查

Effect 只有在当前阶段获得所需 capability 时才能运行。能力包括 `retain_metadata`、`emit_stub`、`request_codegen`、`add_dce_root`、`rerun_phase` 等。能力不是普通字符串标签，必须由编译器注册并按作用域授予。

## 9. Migrate 与 Effect

Migrate 负责建立版本轴上的实体对应关系：

```text
old entity + mapping + version scope
    → current entity
```

Effect 负责读取这条关系并传播后果：

```text
Migrate relation
    → reflection alias
    → compatibility diagnostic
    → DCI schema check
    → generated shim
    → invalidation of dependent artifacts
```

必须遵守以下规则：

1. Effect 只能使用已规范化的 Migrate 关系。
2. 旧版本 Effect 不能因为函数签名相同就自动继承；必须有明确的实体映射。
3. 字段迁移描述 API/元信息对应关系，不自动转换磁盘数据或内存对象。
4. 布局、生命周期或异常模型发生不兼容变化时，Effect 必须产生义务或拒绝映射。
5. 历史入口的保留时间必须由引用、导出和版本作用域决定，不能永久保留未知内容。

## 10. 与现有 事实语义所有权系统 子系统的组合

### 10.1 Reflection

Reflection Effect 读取登记名、别名、成员可见事实和布局，产生类型表、成员表、方法入口和 DCE roots。`hidden` 成员不得由 Reflection Effect 重新暴露。

运行时名称查找的有效性、绑定对象的所有权和方法签名仍需独立验证；Effect 只负责把这些要求变成可检查事实。

### 10.2 DCI

DCI Effect 读取 `.dcib` 中的目标、ABI、布局、所有权、生命周期、异常和实现产物事实，产生：

- Consumer 调用检查；
- Direct 或 Stub 后端请求；
- 构造、复制、移动、析构和释放入口；
- 泛型闭合请求；
- 异常或错误转换义务；
- 最终链接依赖。

DCI Consumer 不得通过源语言名称猜测 Effect。`source.language` 只用于来源身份和诊断，不能成为选择布局或生命周期算法的条件。

### 10.3 DCE

DCE Effect 将导出、反射、动态调用、析构、外部契约和可观察行为转化为 roots 或保留义务。普通运行时读写分析属于 MIR 分析，不应和 Effect 的事实写入混为一谈。

### 10.4 derive、comptime、async 和 platform

这些现有属性可以逐步归一化为内建 Effect：

| 属性 | Effect 结果 |
|---|---|
| `derive` | 生成声明、trait 事实和约束诊断 |
| `comptime` | 确定性求值结果和输入 fingerprint |
| `async` | Future 事实、MIR await lowering 和清理义务 |
| `platform` | 目标过滤、作用域不匹配诊断 |
| `repr/align/packed` | 布局事实和 ABI 重新验证 |

归一化不改变已有语义；在缺少验证门之前，不得声称某项属性已经支持新的 Effect 行为。

## 11. 编译器自身的 Effect

编译器 Effect 分为三个作用域：

```text
program   → 影响生成程序的事实和代码
toolchain → 影响目标、ABI、链接和产物
compiler  → 影响已注册的编译阶段能力
```

`compiler` 作用域只能调用编译器注册的 handler。它可以：

- 添加或失效事实；
- 请求某个已注册 lowering；
- 请求阶段重跑；
- 添加诊断和 DCE roots；
- 生成可验证的声明或 manifest。

它不得：

- 修改 parser 语法表；
- 动态替换类型系统或 MIR 验证器；
- 访问未声明的文件、网络或进程状态；
- 静默修改全局编译器配置；
- 以失败开放的方式吞掉未实现能力。

Effect handler 的实现属于 SDK 编译器。普通包只提交声明和输入事实，不提交可以在编译器进程中任意执行的宿主代码。

## 12. 跨模块产物、指纹与缓存

每个可被消费的模块产物必须携带逻辑上的 Effect manifest。manifest 至少包含：

```text
entity identities
exported facts
effect declarations and handler versions
required capabilities
validity scopes
generated declarations and artifacts
input/output fingerprints
invalidation summary
```

物理编码可以嵌入 `.vyi`、`.dcib` 或其他模块制品，也可以使用配套 sidecar；编码格式不改变本规范的逻辑要求。

缓存键至少覆盖：

```text
source or artifact identity
compiler / handler version
target and ABI scope
build configuration
input fact fingerprints
EffectPlan fingerprint
```

效果范围未知时必须扩大失效范围。旧 manifest 不能在作用域或 handler 版本不匹配时复用。

## 13. 标准库约束

标准库中的每个公开类型和操作都应逐步声明其 Effect 事实，尤其是：

- 所有权、借用、复制、移动和销毁；
- 分配器和运行时域；
- 布局与平台限制；
- 异常、panic、Result 或 error 传播；
- 反射登记与 DCE 保留；
- DCI 导入、导出和回调；
- comptime 和 async lowering 要求。

容器等泛型库必须在实例化点传播元素类型的 Effect。不能因为类型名称包含 `vector`、`string` 或 `result` 就假定布局、释放或异常能力。

## 14. 源码语法与兼容性

源码形式已经在 AOT 路径可用，并且必须满足：

1. 明确列出 `phase`、`reads`、`provides` 和 `requires`；
2. 只能请求已注册的 capability；
3. 不能直接执行任意宿主语言代码；
4. 未知 handler、未知输入或作用域不匹配必须诊断；
5. 旧版本客户端只能消费它理解的 manifest 版本；
6. Effect schema 变更必须有版本和兼容策略。

语义检查后，`effect` 声明会在 HIR lowering 入口被跳过，因此不会静默生成函数、全局变量或运行时副作用。JIT 对齐仍是后续工作；当前契约和门以原生 AOT 编译器为准。

### 14.1 自定义属性 `@[myAttr(...)]`

自定义属性是用户进入事实维度的入口。下面的写法应当被解析为声明元信息：

```vyx
@[myAttr(level="abi", retain=true)]
public class Buffer {
    // ...
}
```

属性本身只产生一个 `AttributeFact`；它不会因为名字叫 `myAttr` 就执行编译器代码。只有在属性被注册、参数通过 schema 验证并绑定到 Effect handler 后，才会产生编译后果。

自定义属性分为三类：

| 类别 | 行为 |
|---|---|
| 内建属性 | 编译器直接注册，例如 `migrate`、`reflect`、`async` |
| 声明式自定义属性 | 有 schema 和 Effect handler，可以派生事实、义务或产物 |
| 不透明属性 | 编译器保留原文供工具和 IDE 使用，不改变语义 |

### 14.2 在 `.attr` 中定义 schema

`.attr` 是独立的声明式输入，使用 Vyx 类型名和字面量写法，不定义 Vyx 函数、导入、
class 或可执行 handler。例如，一个 `acme.attr` 文件可以包含多个 schema：

```vyx
attribute acme.layout {
    version: 1;
    targets: [type, field];

    arguments {
        mode: enum("abi", "layout");
        align: i32 = 0;
        retain: bool = false;
    }

    consequence: retain;
}

attribute acme.note {
    version: 1;
    targets: [function];
    arguments {
        label: string = "startup";
    }
    consequence: record;
}
```

当前支持的字面量参数类型为 `bool`、`i8`、`i16`、`i32`、`i64`、`u8`、`u16`、`u32`、
`u64`、`f32`、`f64` 和 `string`。整数值和默认值必须符合声明的位宽与符号；
数值使用十进制字面量，浮点数可写指数；字符串使用双引号。
`enum("abi", "layout")` 定义封闭的字符串取值集合，取值须为标识符形式。
`.attr` 不接受 `int`，应明确选择
Vyx 整数类型。运行时对象和任意表达式不能作为 schema 值。

`version`、`targets`、`arguments` 和 `consequence` 为必填字段。目标可以是 `function`、
`type`、`field`、`module`、`variant`、`variable`、`use`、`declaration`；后果可以是 `record`、`retain`、`reflect`。
没有默认值的参数是必填参数。后两种后果授权现有编译器的保留或反射路径，不在 schema
中定义新的 handler 函数体。

加载 `acme.attr` 后，在 Vyx 源码中使用限定名：

```vyx
@[acme.layout(mode="abi", align=16, retain=true)]
class Packet {
    public id: i32;
}
```

参数名本身是数据：名为 `align` 的参数不会自动改变布局，声明的后果授权对应的
现有编译器路径。成员事实包含所属类型身份，不同类型的同名字段保持独立。

### 14.3 项目加载与参数复用

`vyxc build` 自动发现包根目录的 `.attr` 文件。其他目录的文件只需在 `Vyx.toml` 中配置一次：

```toml
[effect]
attr_files = ["attrs/common.attr", "attrs/storage.attr"]
auto_discover = true
```

路径相对于 manifest。自动发现只加载包根目录的 `.attr`；子目录中的 schema 不会
自动成为编译输入。
设为 `auto_discover = false` 时只使用显式列表。依赖包编译时加载自己的 schema；
另一包需要复用时，在自己的列表中显式列出共享文件。每个选中文件都是编译输入：
路径或内容变化会使 object/interface 缓存，以及 Effect
manifest 中的 schema 摘要失效。

多个 schema 和文件可以复用参数：

```vyx
attribute acme.base {
    version: 1;
    targets: [type, field];
    arguments {
        align: i32 = 0;
    }
    consequence: record;
}

attribute acme.layout {
    version: 1;
    targets: [type, field];
    extends: [acme.base];
    arguments {
        mode: enum("abi", "layout");
    }
    consequence: record;
}
```

`extends` 复用参数；每个 schema 仍声明自己的版本、目标和后果。引用在全部选中文件解析后
统一解析，允许前向引用。未知基 schema、继承环、重复 schema 名，以及继承参数与本地参数
的名称冲突，都会在编译 Vyx 源码前诊断。
当前解析器的继承链上限为 64 条边，超过时报告独立的深度错误。

属性参数只能使用可规范化的字面量、标识符、路径、列表和嵌套属性值。不能把任意 Vyx 表达式、宿主语言闭包或未验证的源代码作为属性参数。

属性的源码短名可以是 `myAttr`，但事实身份必须包含注册包和 schema 版本。内建属性名属于编译器保留命名空间；跨包使用时应使用工具能够解析的限定身份，不能依赖两个包恰好拥有相同短名。

属性的默认作用域是声明本身。它不会自动传播到字段、方法、泛型实例或子模块；需要传播时，handler 必须声明传播规则和新的实体身份。属性也不会自动继承到 Migrate 的历史实体。

跨模块消费时，只有标记为可导出的属性及其规范化 `AttributeFact` 会进入 Effect manifest。不透明属性可以被工具保留，但不能在 Consumer 中触发编译语义。注册属性的模块被删除、版本不兼容或 schema 不匹配时，Consumer 必须报错或明确降级。

自定义属性和 Effect 的关系是：

```text
@[myAttr(...)]
    ↓ parse / canonicalize
AttributeFact(package.myAttr@schema, subject, arguments)
    ↓ registered handler
EffectPlan
    ↓
derived facts / obligations / artifacts / lowering
```

例如 `myAttr.validate` 可以读取属性参数、布局和 DCI 契约，产生 `stable_layout` 义务和保留事实；它不能直接调用 `malloc`、修改 parser 表或写入全局编译器状态。

如果自定义属性参与 Migrate，必须显式声明属性事实的映射：

```text
old package.myAttr@1 on old entity
    -- explicit migration mapping -->
new package.myAttr@2 on current entity
```

编译器不得因为属性短名和实体签名相同就自动把旧属性复制到新实体。参数变化、schema 变化或 Effect handler 变化都应进入兼容性检查。

未注册的 `@[myAttr(...)]` 仍可被 parser 作为不透明元信息保存，但不能产生语义效果。注册后参数错误、附着目标错误、handler 不可用或 capability 缺失都必须是可定位的编译错误。

## 15. 诊断与失败规则

实现至少应覆盖以下诊断类别：

```text
missing_fact              所需事实不存在
scope_mismatch            事实目标或 ABI 作用域不匹配
effect_conflict           两个 Effect 产生冲突结果
effect_cycle              Effect 依赖形成非法循环
write_after_freeze        冻结事实图后写入
nondeterministic_effect   输入或输出不可确定
unknown_handler           handler 未注册或版本不兼容
missing_capability        当前阶段没有所需能力
stale_artifact            manifest 或生成产物已失效
unfulfilled_obligation    编译义务未完成
```

诊断必须给出触发实体、Effect ID、输入事实、有效范围和修复方向。`exit=77` 只能表示明确跳过，不能表示 Effect 验证失败后继续构建。

## 16. 安全与信任边界

外部 `.dcib`、模块 manifest 和 adapter 输出都是未信任输入。Consumer 必须先验证格式、签名或工具链身份、作用域、fingerprint 和生命周期事实，再运行相关 Effect。

Effect handler 不得使用未声明的文件、网络、时间、随机数或环境变量。生产端源代码只由 Adapter 和生产端工具链解释；Consumer 不得因为 Effect 而重新解析 C++、Rust 或 Zig 语法。

## 17. AOT、JIT 与运行时边界

Effect 的首个实现和验证以原生 AOT 为基准。Effect 可以生成 AOT 所需的事实、桥接和 lowering 请求，不得以 JIT 的动态状态替代可验证 manifest。

将来 JIT 如果消费 Effect，必须先取得稳定、可哈希的原生实体和 ABI 事实。无法提供稳定事实的动态对象属于其他协议，不扩展 DCI Core 或本 Effect 模型。

## 18. 端到端示例

### 18.1 字段迁移与反射

```text
v1: User.name
v2: User.display_name @[migrate(fromVer="1.0.0", fromField="name")]
```

执行顺序：

1. Migrate 产生 `FieldAlias(User@1.0.0, name, User@2.0.0, display_name)`。
2. Reflection Effect 产生历史反射别名。
3. Serializer Effect 只有在显式注册时才产生数据字段映射。
4. DCE Effect 保留仍被导出的历史入口。
5. 如果两个当前字段映射同一历史字段，报告冲突。

### 18.2 DCI 拥有对象

```text
constructor → owned(T, allocator=A)
destructor  → destroy(T, allocator=A)
```

如果 Effect 只找到构造和布局，却找不到同一 allocator domain 的销毁事实，DCI Effect 必须拒绝生成 Consumer 代码。异常、提前返回和跨模块销毁路径都必须消费同一个所有权事实。

### 18.3 开放泛型

```text
std.vector<T>
```

实例化 `T = i32` 时，Effect 可以直接使用已验证布局。实例化为外部类型时，Effect 必须请求生产端闭合泛型，取得布局、调用和生命周期事实，再生成最终 contract 和调用代码。

### 18.4 编译器 lowering

```text
@[async] fn load() -> Result<Data, Error>
```

Async Effect 可以产生 Future 状态事实、await lowering 请求、取消和 drop 义务，并要求错误传播路径满足该函数的 Result contract。它不能因为函数名字叫 `load` 就猜测网络或 IO 能力。

## 19. 验收与验证

实现 Effect 时至少加入以下验证：

1. Migrate 后 Effect 映射、重复映射和旧 Effect 冲突。
2. Reflection Effect 对 DCE roots 和隐藏成员的影响。
3. DCI Effect 对布局、所有权、allocator、析构和异常模型的验证。
4. `comptime` 输入、输出和 handler fingerprint 的确定性。
5. write-after-freeze、作用域不匹配、能力缺失和未完成义务。
6. Effect 冲突、循环和非单调输出诊断。
7. 跨模块 manifest 消费、过期产物拒绝和增量失效。
8. Stage 0 → S1 → S2 → S3 的 Effect manifest 固定点。

验证仍以 AOT 为准。编译成功不等于 Effect 验证成功；每个项目必须运行最终产物，并记录实际编译器、目标和跳过项。

## 20. 实现路线

### Phase 0：规范和观测

定义 Fact、EffectDecl、EffectPlan、Scope、Provenance、Fingerprint 和诊断模型。加入 Effect dump，不改变现有源码语义。

### Phase 1：内部归一化

把 `migrate`、`reflect`、`dci_import`、`derive`、`comptime`、`async`、`platform` 和布局属性归一化为内建 Effect。保留现有各阶段实现作为兼容后端。

### Phase 2：核心联动

AOT 基线已经把 Migrate → Reflection、Migrate → DCE、DCI → lifecycle/exception 和布局 →
codegen/cache 接入 Effect 账本。跨模块 manifest 携带类型化依赖边；戳记过期、事实冲突、
未完成的 DCI 义务和依赖环都会在 lowering 前拒绝。

### Phase 3：编译器 Effect

当前 AOT 编译器已经把 derive、comptime、async/task、platform、Reflection、DCE、Migrate、
源码自动发现的 DCI import 授权和 DCI export 产物选择接入注册 handler。固定点门比较重建
编译器与确定性缓存；Effect 门检查 manifest freshness、依赖环诊断、DCI 义务导入和
lowering 证据。stub 生成仍由独立的 DCI consumer 路径负责。

### Phase 4：标准库迁移

逐个为容器、字符串、Result、异步、反射、资源管理和 DCI 适配器补充事实。每项公开能力进入真实项目夹具和 AOT 回归。

### Phase 5：受限用户 Effect

第一版受限用户 Effect 已按上面的 schema 数据表落地。它声明类型化输入、compiler-owned 后果以及
scope/capability 边界，不能提交可执行 compiler plugin。未来的源码级声明可以复用这条 wire contract，
前提是另行确定语法和包解析规则。

## 21. 设计结论

Effect 的规范对象不是普通函数副作用，而是：

```text
已验证事实
    → 声明式 Effect
    → 派生事实 / 编译义务 / 生成产物 / lowering / 失效
```

Migrate 负责时间轴上的实体对应，Effect 负责事实在其他维度中的传播。Reflection、DCI、DCE、标准库和编译器自举都通过同一个事实图获得可追踪的因果关系。

Effect 是事实语义所有权系统的第五大核心特性。当前 AOT 实现验证注册规则、依赖与义务；源码定义的 schema 可以选择 record、retain、reflect。其他语义扩展需要明确的授权与验证规则。
