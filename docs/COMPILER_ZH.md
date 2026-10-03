# Vyx 编译器架构

[English](COMPILER.md) · [文档目录](README.md) · [源码与自举](../bootstrap_compiler/README.md) · [构建验证](TESTING_GUIDE_ZH.md)

本文说明当前 LLVM 后端的数据流与源码位置。语言规则见[语言设计](设计文档_ZH.md)；
本页关注规则在编译器里怎样变成可执行代码；源码与回归门说明实现及验证范围。
当前验收以原生 AOT 为基准。JIT 的能力对齐与性能属于后续任务，本文的 AOT
结果不代表 JIT 已通过相同验收。

## 从源码到原生程序

```text
.vyx / Vyx.toml
    ↓ 词法、语法、AST 与语义分析
HIR：类型、声明、调用目标、泛型环境、所有权事实
    ↓ 按可达函数物化、降低结构化控制流
MIR：基本块、存储位置、值、指令、终结器
    ↓ 验证与 MIR 优化
LLVM IR → 对象文件 → 链接成原生程序
```

`bootstrap_compiler/src/core/` 包含驱动、前端与构建系统；`src/hir/`、
`src/mir/` 分别实现两级中间表示；`src/codegen/llvm_lower.vyx` 将 MIR
降低到 LLVM。具体构建命令见[编译器 README](../bootstrap_compiler/README.md)。

## HIR：保留语言语义

[`HirUnit`](../bootstrap_compiler/src/hir/hir_model.vyx) 将类型、声明、函数、
局部变量、语句、表达式、泛型环境与类型列表保存在分开的记录表中。逻辑上的
语句与表达式关系使用数字 ID 连接；物理存储是带 `count`、`capacity` 的
扁平记录池。ID 是当前编译单元内的索引，不是跨构建持久的全局身份。

表达式记录保存类型、调用或成员目标、运算符、值类别与源码位置。所有权解析
还会写入借用、移动和析构相关标记。[`lower_pipeline_prepare_hir`](../bootstrap_compiler/src/codegen/lower_pipeline.vyx)
依次进行 HIR 语义解析、DCI 生命周期绑定、所有权解析与 HIR 验证，失败时
返回诊断。AST 阶段已有语义工作，因此不能将全部类型检查归于 HIR。

`src/hir/hir.vyx` 是 AST 语义事实审计模块；HIR 的数据模型以
`src/hir/hir_model.vyx` 为准。

## 接口、泛型与跨模块定义

`.vyi` 保留普通公开声明表面。包含泛型的用户模块还由
[`template_artifact.vyx`](../bootstrap_compiler/src/core/template_artifact.vyx)
追加 v1 AST 图，携带泛型 body 和私有依赖。消费者重建语义引用，保留定义模块，
供限定名称查找与私有 helper 可见性检查使用。显式与推导出的泛型参数先代入，
再确定调用返回类型。图中携带私有 helper 不等于将它公开给消费者调用。

导入器在语义查询前检查格式、标记顺序、公开表面和图的 SHA-256、模块身份、
节点数与各项边界。只要出现 artifact 标记，损坏的内容就导致导入失败，不能
静默退回无 body 声明。扁平声明和语句链使用迭代遍历，列表长度不消耗结构嵌套
深度。runtime、`bootstrap.*` 与 `std.*` 当前仍使用不带此 payload 的普通接口；
它是用户模板传输机制，尚不是可共享的完整前端会话快照。格式上限与 std 消费者
所需的源单元见[泛型接口门](../probes/gates/generic_interfaces/README.md)。

导入的枚举保留其声明 owner；全局变量链接名由声明模块与源码名称生成，不包含
当前编译单元的 HIR item ID，也不依赖消费者的类型拼写。MIR 静态记录的初始化
文本与模块元信息使用独立字段，避免非空字符串初始化数据在清理时破坏存储。
[跨模块门](../probes/gates/cross-module/README.md)故意改变两侧 item 排序来覆盖此契约。

重复泛型 ODR 定义保留规范符号名。native 后端为 COFF、ELF、Wasm 的
linkonce/weak ODR 定义附加 `Any` COMDAT，允许链接器合并重复实例。
两个独立消费者实例化相同泛型的 Windows COFF 链接/运行回归已经通过；
这不等于 ELF 与 Wasm 已完成相同平台验收。

## MIR：显式执行路径

[`MirUnit`](../bootstrap_compiler/src/mir/mir_model.vyx) 保留类型表，另有
函数、基本块、局部变量、字段、存储位置（Place）、值（Value）、指令
（Instr）和匹配分支（Case）等记录表。每个基本块以终结器结束，例如
`return`、`goto`、`branch`、`match` 或 `yield`。

这四种概念最重要：

| 概念 | 问题 | 当前表示的例子 |
|---|---|---|
| Place | 写入或读取哪里？ | 局部变量、字段、解引用、索引、静态存储 |
| Value | 计算出什么？ | 常量、读取 Place、运算、调用、聚合、类型转换 |
| Instr | 做什么操作？ | 求值、赋值、析构、存储开始与结束 |
| Terminator | 下一步去哪？ | 返回、跳转、条件分支、迭代 |

例如 `if` 在 HIR 中仍是语句；[`lower_if_stmt`](../bootstrap_compiler/src/mir/mir_builder.vyx)
把它变为 then、else、join 基本块及显式分支。builder 也会在作用域退出路径
安排 `drop`、`defer` 和存储结束操作。MIR 有可变局部存储与 `assign`、
`read_place`；`ValueId` 不表示整个 MIR 已经是 SSA。种类定义见
[`mir_ids.vyx`](../bootstrap_compiler/src/mir/mir_ids.vyx)。

`match` 可以产生值。builder 将 subject 求值一次，把模式测试和 guard 降低为
控制流，再通过结果 place 汇合产生值的分支。嵌套 match、payload 绑定、block
尾值和 arm 内 return 均有 [AOT 编译/运行门](../probes/gates/match-expression/README.md)；
分支类型不一致、非 bool guard、缺少分支值与空值匹配有负向诊断检查。

[`mir_pass.vyx`](../bootstrap_compiler/src/mir/mir_pass.vyx) 接入了常量折叠、
常量分支折叠、SCCP、复制传播、考虑析构的死代码消除、select 形成及 CFG
简化。优化等级大于 0 时，这些 pass 以有限轮数运行。MIR 通过验证并检查
DCI 存储契约后交给 LLVM 后端。

## 按函数处理与 CGU

当前 LLVM 流式路径以函数为单位进行 MIR 物化、优化、LLVM 降低和记录释放；
相关阶段在 [`llvm_lower.vyx`](../bootstrap_compiler/src/codegen/llvm_lower.vyx)
中。CGU 把待生成的定义分到多个代码生成单元，后端可以并发发射对象文件。
这与项目构建系统按模块启动并行任务是两个不同层次。

项目调度器使用 typed action、输入大小估计、运行中内存观测和成功任务成本历史。
默认先保留 3 GiB，再决定可发射任务；保留量算术使用 `i64`。任务时间历史每批
只扫描一次，按完整输出身份匹配。这些是调度估计，不是进程内存硬上限，也不
证明多个任务共享一次前端分析。[编译器压力门](../probes/gates/compiler-industrial/README.md)
检查 `-j1`/`-jN` 的冷、暖、增量工作量与执行结果。

现有项目门尚不能证明共享解析与只读语义事实、完整单态化闭合、并发 LLVM lowering。
已有后端优化/emit 队列与多个构建进程不能代替这些阶段的验收。

外部泛型操作在消费前必须从生产端取得闭合事实及实现制品。用户契约以
[DCI 规范](DCI_SPEC_ZH.md)为准，Adapter 操作与验证命令见
[DCI SDK 指南](../tools/dci/README.zh-CN.md)。

## 接下来

- 查语言行为：[语言设计](设计文档_ZH.md)与[语言表面](语言表面_ZH.md)。
- 构建并验证编译器：[自举说明](../bootstrap_compiler/README.md)与[验证指南](TESTING_GUIDE_ZH.md)。
- 研究元信息和跨语言调用：[事实语义所有权系统](MOSP_ZH.md)与[DCI 规范](DCI_SPEC_ZH.md)。
