# Vyx documentation · Vyx 文档

This is the entry point for the language, tools, and compiler documentation.
Choose a reading path below; each topic has one primary owner and English
and Simplified Chinese routes. The DCI English page summarizes the full
Chinese Core specification. The repository
[README](../README.md) / [中文首页](../README.zh-CN.md) introduces the project.

这里是语言、工具与编译器文档的共同入口。按阅读路径进入主题；每个主题有明确的
事实归属。DCI 英文页是中文核心规范的概览，详尽程度不同。

## Learn the language · 学习语言

Start with the tutorial, then move to the project guide when you need more than
one source file. The standard-library tutorial teaches APIs; the reference
records package routes and implementation boundaries.

从入门教程开始；需要多个源文件时转到项目指南。标准库教程教用法，参考手册查
包入口和实现边界。

| Read in order / 建议顺序 | English | 简体中文 |
|---|---|---|
| 1. Syntax and first program / 语法与第一个程序 | [Getting started](TUTORIAL.md) | [入门教程](入门指南_ZH.md) |
| 2. Collections, generics, errors / 集合、泛型与错误 | [Intermediate tutorial](INTERMEDIATE_TUTORIAL.md) | [进阶教程](进阶教程_ZH.md) |
| 3. Ownership, traits, async / 所有权、trait 与异步 | [Advanced features](ADVANCED_FEATURES.md) | [高级特性](高级特性_ZH.md) |
| 4. Library examples / 标准库示例 | [Standard-library tutorial](STANDARD_LIBRARY_TUTORIAL.md) | [标准库教程](标准库教程_ZH.md) |
| Coming from Rust or C++ / 从 Rust 或 C++ 迁移 | [Migration guide](MIGRATING_FROM_RUST_CPP.md) | [迁移指南](快速迁移_Rust_CPP.md) |

## Build a project · 创建项目

The project guide owns the step-by-step workflow. Use the manifest reference
for exact fields, the library reference for packages, and the tooling guide
for editor and debugger setup.

项目指南负责操作步骤；清单参考负责字段，标准库参考负责包，工具指南负责编辑器
和调试器配置。

| Topic / 主题 | English | 简体中文 |
|---|---|---|
| Create, build, link / 创建、构建、链接 | [Project guide](PROJECTS.md) | [项目指南](PROJECTS_ZH.md) |
| `Vyx.toml` fields / 配置字段 | [Manifest reference](PACKAGE_MANIFEST.md) | [清单参考](PACKAGE_MANIFEST_ZH.md) |
| Packages and APIs / 包与 API | [Standard-library reference](STD_LIBRARY.md) | [标准库参考](STD_LIBRARY.zh-CN.md) |
| LSP, DAP, IDE / 编辑器与调试 | [Tooling](TOOLING.md) | [工具指南](TOOLING_ZH.md) |
| Runnable projects / 可运行项目 | [Samples](../samples/README.md) | [示例项目](../samples/README.md) |

## Understand the model · 理解设计

Fact Semantic Ownership System connects fact provenance, semantic rules, and consumption authority across five core features. Language design
describes rules from the current compiler; the surface index helps locate a
construct. DCI has a separate interface reference and an SDK guide for
generating contracts.

事实语义所有权系统连接事实来源、语义规则与消费权限，包含五大核心特性。语言设计按当前编译器解释规则，语言表面
用于查找构造；DCI 的契约规则与生成工具分别维护。

| Topic / 主题 | English | 简体中文 |
|---|---|---|
| Migrate, Reflection, DCI, DCE, Effect | [Fact Semantic Ownership System](MOSP.md) | [事实语义所有权系统](MOSP_ZH.md) |
| Effect model / Effect 模型规范 | [Effect](MOSP_EFFECT.md) | [Effect 规范](MOSP_EFFECT_ZH.md) |
| Implemented language rules / 已实现语言规则 | [Language design](LANGUAGE_DESIGN.md) | [语言设计](设计文档_ZH.md) |
| Construct-to-lesson index / 构造与课程索引 | [Language surface](LANGUAGE_SURFACE.md) | [语言表面](语言表面_ZH.md) |
| DCI facts and validity / DCI 契约与有效性 | [DCI reference](DCI_SPEC.md) | [DCI 规范](DCI_SPEC_ZH.md) |
| DCI commands and adapters / DCI 工具与 Adapter | [SDK guide](../tools/dci/README.md) | [SDK 指南](../tools/dci/README.zh-CN.md) |
| HIR, MIR, LLVM / 编译器结构 | [Compiler architecture](COMPILER.md) | [编译器架构](COMPILER_ZH.md) |

## Work on the compiler · 开发编译器

Agents start with the repository-wide [handoff guide](../AGENTS.md), which
maps source owners, build commands, and verification gates.
Agent 接手先读根目录的[项目指南](../AGENTS.md)，查源码归属、构建入口和验收门。

The [compiler README](../bootstrap_compiler/README.md) covers source layout
and bootstrap. [Verification](TESTING_GUIDE.md) / [中文验证指南](TESTING_GUIDE_ZH.md)
covers the compiler under test, fixed point, and project gates.
[Contributing](../CONTRIBUTING.md) / [参与贡献](../CONTRIBUTING.zh-CN.md)
covers repository changes.

Current compiler performance and DCI gates use AOT; JIT parity is later work.
Focused gates cover [match expressions](../probes/gates/match-expression/README.md),
[cross-module enums/globals](../probes/gates/cross-module/README.md),
[generic interfaces without producer sources](../probes/gates/generic_interfaces/README.md),
and [cold/warm/incremental parallel builds](../probes/gates/compiler-industrial/README.md).

当前编译器性能与 DCI 验收以 AOT 为基准，JIT 对齐留待后续。以上回归门分别覆盖
match 表达式、跨模块枚举/全局量、隐藏生产端源码的泛型接口，以及冷构建、热缓存
和增量并发构建；单项验证结果不等于工业级编译器目标已经完成。

The [compiler architecture](COMPILER.md) / [编译器架构](COMPILER_ZH.md)
maps the implementation to current source and regression gates. Use those
entries for behavior and verification limits; historical plans remain in Git.

编译器架构按当前源码与回归门说明实现和边界；历史计划从 Git 查阅。

## Component notes · 各目录说明

These files stay beside the code or artifact they describe. Use the guides
above for language rules and the files below for local procedures and evidence.

这些说明保留在对应源码或产物旁边；语言规则从上面的主文档查，目录内文件负责
本地操作步骤和证据。

| Area / 目录 | Entry / 入口 |
|---|---|
| Compiler sources / 编译器源码 | [bootstrap_compiler](../bootstrap_compiler/README.md) · [scripts](../bootstrap_compiler/scripts/README.md) · [standard packages](../bootstrap_compiler/std_packages/README.md) |
| DCI SDK / 互操作工具 | [tools/dci](../tools/dci/README.md) · [中文](../tools/dci/README.zh-CN.md) |
| Tests and gates / 测试与验证门 | [tests](../tests/README.md) · [checks](../tests/checks/README.md) |
| Examples / 示例工程 | [samples](../samples/README.md) |
| Zyn UI / 界面框架 | [Zyn](../Zyn/README.md) · [组合布局](../Zyn/docs/COMPOSITION_ZH.md) · [smoke project](../samples/projects/zyn_recomposition_smoke/README.md) |
| Benchmark workloads / 基准负载 | [benchmarks](../benchmarks/workloads/README.md) |
| Showcase website / 展示网站 | [Website / 官网](https://www.vyxlang.com/) |

## Which page owns a fact · 事实归属

Use [language design](LANGUAGE_DESIGN.md) for syntax and semantics,
[the manifest reference](PACKAGE_MANIFEST.md) for build fields,
[the DCI reference](DCI_SPEC.md) for interoperability rules, and
[the SDK guide](../tools/dci/README.md) for Adapter commands. Tutorials show
how to use those rules; test guides describe how to reproduce verification.

用户可见行为以当前自举编译器及对应测试为准。修改行为时，同步更新所属主题的
中英文主文档，并修正引用该事实的教程；不要再复制一份独立规范。
