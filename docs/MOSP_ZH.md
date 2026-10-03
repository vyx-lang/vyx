# 事实语义所有权系统

[English](MOSP.md) · [文档目录](README.md) · [入门教程](入门指南_ZH.md)

Vyx 的事实语义所有权系统把程序事实的来源、意义、消费权限和有效条件连接起来。
执行一个语义操作，需要对应的已验证事实与授权规则。事实提供方、语义规则的定义方、
消费方可以属于不同模块或不同语言；目标架构、ABI、工具链、配置和版本限定事实的适用范围。

| 问题 | 系统中的位置 |
|---|---|
| 事实怎么来 | 源码声明、编译器推导、DCI 生产端；记录来源与证据 |
| 事实怎么用 | 验证参数、实体身份和有效条件，由对应规则消费 |
| 谁用 | 编译器阶段、已注册的 handler、外部契约消费端 |
| 谁管理 | 提供方负责来源；编译器维护依赖、有效范围与失效 |
| 有什么作用 | 选择实现、查询类型、生成调用、建立编译义务和决定代码保留 |

这里的所有权指对事实语义的权限与责任。内存对象的归属和清理也可以由其中的事实约束。
五大核心特性是 **Migrate、Reflection、DCI、DCE、Effect**：Migrate 关联事实随版本的变化，
Reflection 提供查询和访问，DCI 跨语言传递事实，DCE 据事实决定保留，Effect 定义消费后的语义后果。

本文给出五项特性的用法，详细规则见 [Effect 规范](MOSP_EFFECT_ZH.md)。
原名称 MOSP 的文件路径与数据格式标识保留兼容。

## Migrate：模块版本与历史调用

版本和变体标识一个模块版本；迁移标注关联当前声明与历史声明。
下面的项目保留两个运费规则版本。

目录：

```text
shipping/
  Vyx.toml
  Vyx.lock
  src/
    main.vyx
    shipping_v1.vyx
    shipping_v2.vyx
```

`Vyx.toml`：

```toml
[package]
name = "shipping"
version = "0.1.0"
entry = "src/main.vyx"

[build]
output_dir = "target"

[target.shipping]
type = "executable"
entry = "src/main.vyx"
```

`Vyx.lock` 指定默认模块版本：

```text
Shipping:2.0.0["standard"]
```

`src/shipping_v1.vyx`：

```vyx
@[version("1.0.0")]
@[variant("standard")]
module Shipping;

public fn quote(weight_kg: i32) -> i32 {
    return 8 + weight_kg * 2;
}
```

`src/shipping_v2.vyx`：

```vyx
@[version("2.0.0")]
@[variant("standard")]
module Shipping;

@[migrate(fromVer="1.0.0", fromSig=fn quote(i32)->i32)]
public fn quote(weight_kg: i32) -> i32 {
    return 5 + weight_kg * 2;
}
```

`src/main.vyx`：

```vyx
use Shipping;

fn main() -> i32 {
    print(quote(3));
    print(quote@1.0.0["standard"](3));
    return 0;
}
```

在项目目录执行 `vyxc --src=project . --run=aot`，输出 11 和 14。
默认调用使用锁文件选定的版本，显式版本调用使用历史实现。

字段改名可使用 `@[migrate(fromVer="1.0.0", fromField="旧字段名")]`。
编译器检查历史字段及映射关系；这描述的是程序定义的演进，不是磁盘数据的自动格式转换。
完整的字段、构造函数与历史方法示例见
[tutorial_migrate](../tests/projects/tutorial_migrate/src/main.vyx)。

## Reflection：发现、绑定、调用

`@[reflect]` 声明反射名称与别名，`@[hidden]` 排除成员。
`std.reflect` 读取编译器生成的元数据，提供类型查询和实例操作。

保存为 `reflection.vyx`：

```vyx
use std.reflect;

@[reflect("Counter")]
public class Counter {
    public value: i32;

    @[reflect(alias="read")]
    public fn current() -> i32 {
        return self.value;
    }

    @[hidden]
    public fn reset() {
        self.value = 0;
    }
}

fn main() -> i32 {
    var counter = Counter { value: 42 };
    let counter_type = getType("Counter");
    var instance = counter_type.bind(&counter);
    let method = instance.getMethod("read");
    let read = method.as::<fn()->i32>();
    print(read());
    return 0;
}
```

运行 `vyxc --src=file reflection.vyx --run=aot`，输出 42。

- `getType` 根据登记名查找类型；外部输入的名称应检查返回对象的 `valid`。
- `bind` 绑定现有实例，不转移该实例的所有权；在使用绑定期间保持实例有效。
- `getMethod` 支持登记的别名，`as::<fn(...) -> R>()` 指定调用签名。
- 字段与属性操作使用 `getField`、`getProperty`、`view::<T>()`、`write::<T>()`。
- 编译期类型查询如 `T::name`、`T::fields` 与运行时按名查询是不同的使用路径。

运行时反射需要保留元数据与相应代码。反射入口会参与 DCE 的根集合。
更多操作见 [tutorial_reflect.vyx](../tests/cases/tutorial_reflect.vyx)
和 [reflection_full_model.vyx](../tests/cases/reflection_full_model.vyx)。

## DCI：生产端事实与开放泛型

### 分工

```text
生产端源代码
    ↓ 生产端编译器：布局、约束检查、实例化
Adapter
    ↓ 闭合 ABI 事实与实现产物
.dcib + 对象文件 / 库
    ↓ 验证、生成调用、链接
Vyx 程序
```

C++ 语义由 C++ 编译器处理，Rust 语义由 rustc 处理。
Vyx 使用声明和契约；不需要在消费端实现另一套 C++ 或 Rust 类型系统。

Active / Online Adapter 可以在构建期间接收实例请求。
开放请求位于控制面，canonical `.dcib` 描述闭合实体的事实。
生产端负责约束检查与实例化，返回的产物还必须进入最终链接。

Rust Adapter 既有 C/system 边界，也有受支持的 rustc 实测原生 ABI 路径；
这些事实与具体生产端环境绑定。当前支持范围见 [DCI 规范](DCI_SPEC_ZH.md)，
契约生成命令见 [工具指南](../tools/dci/README.zh-CN.md)。

### 原始定义与 Vyx 调用

以下 Rust 定义选自 [native/lib.rs](../tests/projects/dci_opengeneric/native/lib.rs)：

```rust
pub fn twice<T: std::ops::Add<Output = T> + Copy>(v: T) -> T {
    v + v
}
```

对应的 C++ 定义见 [native/lib.hpp](../tests/projects/dci_opengeneric/native/lib.hpp)：

```cpp
template <typename T>
T twice(T v) {
    return v + v;
}
```

Vyx 的调用形式：

```vyx
@[dci_import("../dci/open_generic.dcib")]
extern "dci" {
    fn twice<T>(v: T) -> T;
}

fn main() -> i32 {
    print(twice(21));
    print(twice(1.5));
    return 0;
}
```

这段代码需要配套契约、生产端源码和 Active Adapter 构建配置。
C++ 路径使用自己的 `open_generic.cpp.dcib`；不能用 Rust 的契约描述 C++ 产物。

### 构建已有项目

[开放泛型项目](../tests/projects/dci_opengeneric/README.md) 包含完整配置：

| 目标 | 生产端 | 声明与调用 |
|---|---|---|
| `dci_opengeneric` | Rust，`native/lib.rs` | `src/open_generic.vyx` |
| `dci_opengeneric_cpp` | C++，`native/lib.hpp` | `src/open_generic_cpp.vyx` |

准备 Python、rustc，以及项目所用的 C++ 工具链，在项目目录运行：

```sh
vyxc build --target dci_opengeneric
vyxc build --target dci_opengeneric_cpp
```

构建配置通过 `dci_stub_backend = "external"` 和
`dci_stub_backend_tool_args` 指定生产端与后端。
契约生成、工具链设置及端到端运行步骤见该项目 README。

当前项目区分两种信息需求：

- 泛型函数按调用点闭合请求，经 `.dci_open` 交给生产端物化。
- `Pair2<A, B>` 等泛型记录在 Vyx 生成代码时就需要大小、对齐和字段偏移，
  因而使用的实例布局必须已进入契约；成员调用由对应生产端提供实现。

兼容的基础类型、DCI 导入类型和受支持的 Vyx C 布局类型可以作为接入候选。
是否可用取决于具体操作要求：布局、传参、生命周期和生产端泛型约束必须分别满足。
不做通用对象布局转换，也不承诺任意类型、任意泛型均可跨语言使用。

### 原始模板与真实生态项目

[原始 vector 门](../probes/gates/dci-vector/README.md) 导入开放 C++
`std::vector<T>` 模板，Vyx 消费方选择 `std.vector<i32>` 并使用 brace 初始化。
[C++ 生态门](../probes/gates/dci-cpp-ecosystem/README.md) 使用 ICU 的原始
`UnicodeString` API；[Rust 生态门](../probes/gates/dci-rust-ecosystem/README.md)
通过实测 view 和生成的 native bridge 消费 locked Cargo crates。
这些是具名操作与目标的 AOT 回归。Adapter 命令、bridge 边界、存储释放与共享
异常清理见 [DCI SDK 指南](../tools/dci/README.zh-CN.md)和[DCI 规范](DCI_SPEC_ZH.md)。
JIT 能力对齐留待后续。

### 继承与原生回调

DCI 还承载对象模型事实。在
[dci_multilang](../tests/projects/dci_multilang/src/main.vyx) 中，`VyxSink`
继承 C++ 的 `abi_complex.AbstractSink` 并覆写 `consume`，原生
`NativeDriver` 经基类派发回 Vyx。同一工程里，`VyxHost` 继承 Rust
`native.Sink` 契约，Rust 分派也会调用 Vyx 覆写。反向调用依赖生产端实测的
虚表事实和匹配的 Stub 后端。开放泛型 trait 须先闭合为具体实例，见
[dci_rust_trait](../tests/projects/dci_rust_trait/)。哪些生产端与目标组合已验证，
以 [DCI 能力矩阵](DCI_SPEC_ZH.md#16-vyx首个实现与标准基线)为准。

## DCE：可达性与必要行为

DCE 不需要源代码标注。它使用编译器已经掌握的调用关系、导出要求、反射登记及副作用信息：

1. 确定入口与必须保留的根。
2. 追踪依赖，按需构建可达函数体。
3. 在 MIR 优化中消除可安全移除的代码。
4. 将保留的内容交给 LLVM 优化与生成机器码。

没有直接调用不等于可以删除：导出接口、反射入口、析构与可观察副作用都可能要求保留。
反射与 DCE 的配合体现了 事实语义所有权系统的作用——同一实体的元信息会影响发现、调用和代码生成。

## Effect：事实消费与语义后果

Effect 将声明事实连接到已注册的语义规则。规则检查附着目标、参数和能力，
再记录事实、产生义务或授权对应的编译行为。Migrate 管理版本关系；Effect 规定
这些关系以及其他事实如何被消费，并追踪依赖变化后的失效。

项目可以在 `.attr` 中定义有类型的属性。下面的项目声明两个 schema，用 `extends`
复用 `i32` 参数，再把 `acme.entry` 关联到 `retain`。

`attrs/acme.attr`:

```attr
attribute acme.meta {
    version: 1;
    targets: [function];

    arguments {
        priority: i32 = 0;
    }

    consequence: record;
}

attribute acme.entry {
    version: 1;
    targets: [function];
    extends: [acme.meta];

    arguments {
        reason: string = "native callback";
    }

    consequence: retain;
}
```

`Vyx.toml`:

```toml
[package]
name = "effect_demo"
version = "0.1.0"
entry = "main.vyx"

[effect]
attr_files = ["attrs/acme.attr"]
auto_discover = false
```

`main.vyx`:

```vyx
@[acme.entry(priority=10)]
fn native_entry() -> i32 {
    return 42;
}

fn main() -> i32 {
    print("effect-ready");
    return 0;
}
```

在项目目录执行 `vyxc build`，然后运行生成的程序：输出为 `effect-ready`。
`native_entry` 没有直接调用，仍因 `retain` 进入代码保留根。
`priority` 是有类型的属性数据，其名称本身不改变调度。

包根目录的 `.attr` 默认自动发现；其他文件由 `[effect].attr_files` 指定，路径相对于清单。
`auto_discover = false` 只加载显式列表。选定 schema 的内容参与缓存输入，修改后重新验证。

当前 `.attr` 后果支持 `record`、`retain`、`reflect`。参数类型复用 Vyx 标量名称，
支持默认值与 `enum("a", "b")`；重复身份、继承环、参数冲突和越界都会被拒绝。
任意可执行 handler、布局重写与事实所有权转交需要各自的编译器规则和验证。

配置见 [项目清单](PACKAGE_MANIFEST_ZH.md)，完整规则见 [Effect 规范](MOSP_EFFECT_ZH.md)，
可运行项目见 [attribute_project](../probes/gates/mosp-effect/fixtures/attribute_project/main.vyx)。

## 进一步阅读

- [DCI 规范](DCI_SPEC_ZH.md)：契约格式、能力与验证规则。
- [项目清单](PACKAGE_MANIFEST_ZH.md)：目标、版本与构建配置。
- [高级特性](高级特性_ZH.md)：所有权、泛型、继承及编译期能力。
- [编译器架构](COMPILER_ZH.md)：HIR、MIR、LLVM 与 CGU 如何消费这些事实。
- [Effect 模型规范](MOSP_EFFECT_ZH.md)：事实传播、编译器 Effect 与实现阶段。
