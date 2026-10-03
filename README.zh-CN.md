<div align="center">
  <img src="vyx.png" width="128" height="128" alt="Vyx 图标">
  <h1>Vyx</h1>
  <p>
    <a href="README.md">English</a> ·
    <a href="README.zh-CN.md">简体中文</a> ·
    <a href="https://www.vyxlang.com/">官网</a> ·
    <a href="https://github.com/vyx-lang/vyx/releases/latest">下载</a> ·
    <a href="docs/README.md">文档</a>
  </p>
</div>

Vyx 是一门编译成原生机器码的系统编程语言。编译器用 Vyx 编写，后端使用 LLVM 22，
当前构建和测试以原生 AOT 为基准。

语言支持结构体、类、枚举、泛型、trait 和闭包。内存管理提供值语义、借用、指针与
作用域清理；项目用 `Vyx.toml` 配置源文件、库和构建目标。

Vyx 的事实语义所有权系统将程序事实的来源、有效范围和消费规则连接起来。
Migrate、Reflection、DCI、DCE、Effect 是它的五大核心特性。

## 运行第一个程序

从 [Releases](https://github.com/vyx-lang/vyx/releases/latest) 下载 SDK，解压后
将 `bin` 目录加入 `PATH`。Windows 与 Linux SDK 均已内置 LLVM 后端和原生链接工具，
使用时保留完整的 SDK 目录，无需另装 LLVM。
安装步骤见[入门教程](docs/入门指南_ZH.md)。

保存为 `hello.vyx`：

```vyx
fn main() -> i32 {
    print("Hello, Vyx!");
    return 0;
}
```

编译并运行：

```sh
vyxc --src=file hello.vyx --run=aot
```

多文件项目、依赖和库链接见[项目指南](docs/PROJECTS_ZH.md)。
从源码构建编译器还需要开发依赖，见[编译器 README](bootstrap_compiler/README.md)。

## 学习 Vyx

| 指南 | 内容 |
|---|---|
| [入门教程](docs/入门指南_ZH.md) | 安装 SDK、变量、控制流、函数、结构体、类与可见性 |
| [进阶教程](docs/进阶教程_ZH.md) | 容器、闭包、泛型、`match`、错误处理与多文件项目 |
| [高级特性](docs/高级特性_ZH.md) | 所有权、trait、并发、反射与互操作 |
| [从 Rust 或 C++ 迁移](docs/快速迁移_Rust_CPP.md) | 语法与项目习惯对照 |
| [标准库参考](docs/STD_LIBRARY.zh-CN.md) | 包入口、API 与实现边界 |

[教程网站](https://www.vyxlang.com/tutorial/) 从这四篇语言教程生成正文，提供章节导航与中英文切换。

### 用 `@[vis]` 声明可见性

`@[vis(scope)]` 控制哪些调用方可以访问声明。`public` 是 `@[vis(world)]` 的简写；
模块和包的访问范围也通过这个属性声明。

```vyx
module counter.api;

@[vis(world)]
fn initial_value() -> i32 { return 41; }

@[vis(package)]
fn normalize(value: i32) -> i32 { return value < 0 ? 0 : value; }
```

`world` 允许所有调用方访问，`package` 允许模块路径首段相同的包内调用方访问。
更细的范围可用 `mod`、`tree`、`in(...)` 和 `friend(...)`，也可通过 `+`、`&`、`-` 组合。
完整范围表、默认规则与成员访问见[入门教程](docs/入门指南_ZH.md)。

## 使用 C++ 和 Rust 库

DCI（Declarative Code Interface）用于导入其他语言的类型和函数。适配器从对应语言的
编译器取得布局、调用约定和构造析构信息，写入 `.dcib` 契约；Vyx 根据契约生成调用，
再链接原生库。对受支持的泛型，构建时可通过适配器请求原语言编译器完成实例化。

例如，配置好 DCI 声明和构建后，可以在 Vyx 中使用 C++ 标准库的 `std::vector<T>`：

```vyx
var numbers = std.vector<i32>{1, 2, 3, 4, 5};
let value: i32 = 6;
numbers.push_back(&value);
```

类型实参由 Vyx 调用方选择。完整声明和构建步骤在
[vector 示例](probes/gates/dci-vector/README.md)。

仓库中的真实库测试还包括 ICU 的 `UnicodeString`，以及 Cargo 中的
`crc32fast` 和 `adler2`。这些 Rust 示例所需的桥接代码由适配器生成。
各平台和类型的支持范围见 [DCI 规范](docs/DCI_SPEC_ZH.md)，生成契约的命令见
[DCI 工具文档](tools/dci/README.zh-CN.md)。

## 事实语义所有权系统：五大核心特性

执行一个语义操作，需要对应的已验证事实和消费权限。事实可以来自源码声明、
编译器推导或外部语言的生产端；编译器检查它们的目标、ABI、版本与依赖，
再交给相应规则消费。事实语义所有权关注谁提供事实、谁定义其意义、谁使用，
以及有效条件变化后如何重新验证。

- **Migrate**：用 `@[version]`、`@[variant]` 和 `@[migrate]` 描述模块及 API 的
  版本关系。调用方可以选择历史实现，项目需要提供相应版本的模块。
- **Reflection**：编译期查询类型；用 `@[reflect]` 登记运行时可查找的类型与成员，
  再通过 `std.reflect` 访问属性、绑定方法和调用。
- **DCI**：由原语言生产端提供布局、调用 ABI、生命周期与泛型实例事实，
  Vyx 验证契约后生成调用并链接实现。
- **DCE**：编译器从入口、导出和反射登记追踪需要的代码，删除无用代码，同时保留
  副作用和必要的清理。
- **Effect**：规定事实如何被注册规则消费，产生事实记录、编译义务和代码生成后果。
  项目可以在 `.attr` 文件中定义有类型的属性，通过 `Vyx.toml` 自动加载。

例如，属性 schema 可以把一个函数声明接到 `retain` 规则，使它成为代码保留根。
参数使用 `i32`、`bool`、`string` 等类型；一个文件支持多个 schema，并可用 `extends`
复用参数。当前 `.attr` 的后果支持 `record`、`retain` 和 `reflect`。

完整说明见[事实语义所有权系统](docs/MOSP_ZH.md)、
[Effect 与属性定义](docs/MOSP_EFFECT_ZH.md)和[项目配置](docs/PACKAGE_MANIFEST_ZH.md)。
文档路径和契约格式标识保留兼容。

## 工具与界面开发

`vyxc` 提供项目创建、构建、测试和格式化命令。仓库还包含 `vyxc-lsp`、
`vyxc-dap`，以及 VS Code / Cursor 和 IntelliJ IDEA / CLion 插件。
安装与配置见[工具文档](docs/TOOLING_ZH.md)。

[Zyn](Zyn/README.md) 是用 Vyx 编写的 UI 框架，使用 View 组合界面，通过应用资源和
命令更新状态。已有布局容器、MD3 控件、主题、阴影和稳定 key 列表；文字排版和复杂
列表组件还在完善。可运行项目见[示例目录](samples/README.md)。

## 当前状态

Vyx 处于 Early Access，版本为 `1.0.0-alpha.1`。当前开发和测试以 LLVM AOT 为基准，
支持输出可执行文件、对象文件和库。编译速度与大工程稳定性仍在改进；
JIT 的能力对齐是后续任务。

编译器源码在 [bootstrap_compiler/](bootstrap_compiler/README.md)。
修改编译器后，用重新构建的 SDK 编译器跑 hello、自举固定点和相关项目测试。
具体命令见[验证指南](docs/TESTING_GUIDE_ZH.md)，并行构建测量见
[编译器压力测试](probes/gates/compiler-industrial/README.md)。

更多内容：[文档目录](docs/README.md) · [贡献指南](CONTRIBUTING.zh-CN.md)
