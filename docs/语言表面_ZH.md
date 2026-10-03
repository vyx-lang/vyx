# Vyx 语言表面

[English: Language surface](LANGUAGE_SURFACE.md) · [文档目录](README.md)

用本页查找语言构造对应的课程；规则见[语言设计](设计文档_ZH.md)，处理它的
编译阶段见[编译器架构](COMPILER_ZH.md)。当前支持情况应以从当前源码新构建的
SDK 编译器和相关测试或 `probes/gates/` 程序核对。

## 怎么用

1. 查构造名。
2. 按 [测试指南](TESTING_GUIDE_ZH.md) 用 SDK 编译器跑那一课的完整 `.vyx`。
3. 表格中的范围说明表示当前实现边界。

编辑器插件（VS Code / IntelliJ）连的是 `vyxc-lsp` / `vyxc-dap`，不是额外语法。

## 第 1–10 课 — [入门](入门指南_ZH.md)

| 构造 | 课 |
|---|---|
| `fn main() -> i32`、`print`、`;` | 1 |
| `let` / `var`、`${}` / `f"…"`、`"""…"""`、`//` `/* */` `///` | 2 |
| `i8`…`i64`、`u8`…`u64`、`isize`/`usize`、`f32`/`f64`、`bool`、`char`、`string` | 3 |
| `0x`/`0o`/`0b`、整数后缀、`[e,…]` / `[elem; n]`、`(e, e)` / `.0` | 3 |
| `if` / `elif` / `else`、`? :`、`if` 表达式 | 4 |
| `while`、`for`、C 风格 `for`、`break`、`continue`、`'outer: while` / `break 'outer` | 5 |
| 函数、默认参数、`f(name: expr)`、`return` | 6 |
| `struct`、`Name { field: expr }` | 7 |
| `class`、`self`、`public` / `@[vis(...)]`、`ClassName(args)` 构造 | 8 |
| 记录体内的嵌套 `struct` / `class`，`Outer<i32>::Inner<f64> { … }` | 7 + 8 |
| `panic` | 9 |
| `defer` | 10 |

## 第 11–20 课 — [进阶](进阶教程_ZH.md)

| 构造 | 课 |
|---|---|
| `Vec` / `Dict` / `Set` | 11 |
| 闭包 `\|x\| { … }` 与 `[cap](x) =>` | 12 |
| `fn f<T>(…) where T: Trait`、`const N` | 13 |
| `enum`、`match` / `case`（语句或值表达式） | 14 |
| `string` 方法、`f"…"` / `"""…"""` | 15 |
| `Option` / `Result` / `?` / `fail Type.Variant` | 16 |
| `+=` 与位运算 | 17 |
| `for (x in xs)` | 18 |
| `module`、`Vyx.toml`、`sources`、`vyxc build` / `new` / `repl` / `install` / `test` / `bench` / `fmt` / `doc` | 19 |
| `extern "C"`、`cfn(…) -> R` | 20 |
| DCI `extern "dci"` / `"cpp"`、`@[dci_import]` | 20 + [42](高级特性_ZH.md#第42课dci--c-abi-互操作) |

本地 `file://` registry（`vyx publish` / `install` / `lock`）见
[PACKAGE_MANIFEST_ZH.md](PACKAGE_MANIFEST_ZH.md)，不是语法课。

## 第 21–43 课 — [高级](高级特性_ZH.md)

| 构造 | 课 | 当前范围 |
|---|---|---|
| `&T` / `&mut T`、`*T`、`->` | 安全模型 | 冲突借用为 E3101 |
| `Ref` / `Weak` / `Box` | 21 | 单线程引用计数 |
| 泛型 `Vec` 迭代 | 22 | 结构突变会使迭代器失效 |
| 闭包捕获 | 23 | |
| `@[derive(Clone, Hashable, Eq, Ord, Debug, Display, Copy)]` | 24 | 逐字段 `.clone()`；`rawptr`/`*T` 字段拒绝 |
| `@[async]` / `await` / `Task` / `Promise` | 25 | `poll()` 是 Future 表面；MIR await 拆分 + 无栈泵 + `Task.sleep`；StartCoroutine 仍走 OS fiber；用 AOT 跑 |
| 带载荷 enum | 26 | |
| `trait` / `impl` | 27 | solver + blanket `where` |
| `dyn Trait` | 28 | |
| `\|>` 管道 | 29 | |
| `unsafe`、`rawptr`、`@[repr(C)]`、`@[no_mangle]`、`@[link_name]` | 30 | 语言 FFI 只有 C；C++ / Rust 等编译型语言走 [DCI 第 42 课](高级特性_ZH.md#第42课dci--c-abi-互操作) |
| `drop()` | 31 | 不替代 `Vec.destroy()` |
| 泛型 enum、`<...Ts>` pack | 32 | |
| `@[comptime]` 折叠 | 33 | 整数局部量、`while`/`for`、`break`/`continue`、`print`/`println`、`ct_file_size`；不解释堆 |
| move E3100、`Copy`、先 clone 再分享 | 34 | 类 `string`/`String` 不是 Copy；标量是 |
| `class Child : Base`、`class Mix : Left, Right`、`override` | 35 | 棱形继承报错 |
| `interface` 默认方法、超接口 | 36 | `interface` / `trait` / `protocol` 同一关键字槽 |
| `type Alias = …`、关联 `const` | 37 | |
| `@[platform("windows"\|"posix"\|"linux"\|"macos"\|"android")]` | 38 | `posix` = Linux + Android + macOS |
| `sizeof::<T>()` / `alignof::<T>()` / `static_assert` | 39 | 折成 `i64` |
| `operator +` / `operator[]` 方法 | 40 | |
| `@[version]` / `@[variant]` / `@[migrate]` / `@[discard]` / `name@ver["tag"]` | 41 | `fromSig` 不是匹配器；错误 migrate/discard 为 E2400 |
| `@[dci_import]`、`extern "dci"` / `"cpp"` | 42 | Adapter `.dcib`；Direct vs Stub；[当前 AOT 覆盖](DCI_SPEC_ZH.md)；JSON 整数 ±2^53 |
| `std.reflect` `getType` / `Type.of` / `Instance` | 43 | intern 哈希表；`@[reflect]` 别名；不是 `reflect_*` 内建 |

## 标准库（不是新语法）

模块用法见 [标准库教程](标准库教程_ZH.md) 和 [参考](STD_LIBRARY.zh-CN.md)：
`std.collections`、`std.sync`、`std.vio`、`std.reflect`（`Type.of` / `getType` / `Instance`；intern 哈希查找，第 43 课）、
`std.ffi`（`c_long` 跟随 LLP64/LP64）、
`os`/`fs`/`path`（`windows` vs `posix`）。Windows 交叉编 Android：
[测试指南](TESTING_GUIDE_ZH.md#交叉编译windows-宿主)。

## 已解析、但支持范围受限

这些写法的实现范围比其语法可能暗示的范围更窄：

| 构造 | 说明 |
|---|---|
| `T?` 类型 | 解析成 `try` / `?` 节点，类型本身被**拒绝**；传播错误用 `?` / `try`（第 16 课） |
| `new T(...)` / `delete` | 正常的堆分配/释放写法：`let a = new i32(1)`、`delete a`；容器另有 `Vec::<T>.new()`（第 9 课） |
| 表达式位置的 `match` | 分支值类型相容，支持布尔 guard、枚举载荷和块尾值；兜底用 `default`；不承诺一般穷尽性证明（[第 14 课](进阶教程_ZH.md#第14课match)） |

## 有关键字、但本版不教的

它们会按关键字分词，但 **1.0.0-alpha.1 ea 没有用户课**。不要因为在编译器词表里见到就当成已教特性：

| 记号 | 改用 |
|---|---|
| `asm`、`macro`、`bench`、`volatile` | 不教，无用户契约 |
| `concept`、`requires` | `where T: Trait`（第 13 课） |
| `foreach` | `for (x in xs)`（第 18 课） |
| `error`（关键字） | `Result` / `?`（第 16 课） |
| `newtype` | `class` 或 `type` 别名（第 8、37 课） |
| `task` / `fail` 关键字 | `@[async]` + `std.vio` 的 `Task`（第 25 课）；`fail Type.Variant` 在第 16 课 |
| 语言级 `extern "C++"` | [DCI 第 42 课](高级特性_ZH.md#第42课dci--c-abi-互操作) |
| `--verify-hir2` / `--verify-mir2` | 编译器 IR 检查，不是用户类型诊断 |

`new` 是普通标识符（如 `Vec::<T>.new()`），不是关键字。

## 课内片段和 SDK 编译器不一致时

以从当前源码新构建的 SDK 编译器和对应 `probes/gates/` 为准。改本页和那一课，
不要给编译器加特例。
