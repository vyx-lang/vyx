# Vyx 进阶教程

[English](INTERMEDIATE_TUTORIAL.md) · [文档目录](README.md) · [入门](入门指南_ZH.md) · [事实语义所有权系统](MOSP_ZH.md)

第 11 到 20 课，把入门课里的单文件小程序变成能用的程序。这一段的主线是**数据怎么组织**（容器、闭包、泛型）、**失败怎么表达**（`match`、`Result`、`?`）、**代码怎么分家**（多文件项目、C 互操作）。

每一课照旧先说问题、再说取舍。安装 SDK 的方式、以及 `vyxc --src=file main.vyx --run=aot` 这类命令，仍然沿用[入门教程](入门指南_ZH.md)。

## 第11课：集合容器

数组 `[T; N]` 的长度在编译期就定死了。但真实程序的数据量往往要跑起来才知道：读进来的行数、用户输入的数量。这就需要能在运行时增长的容器。

```vyx program
use std.collections;

fn main() -> i32 {
    // i32 是 Vec 的类型实参，new 是这个类型上的构造方法。
    var values = Vec::<i32>.new();
    values.push(20);
    values.push(22);
    print(values.get(0) + values.get(1));
    // 提前释放缓冲；Vec.drop() 也会在局部值离开作用域时清理。
    values.destroy();
    return 0;
}
```

输出 `42`。

### 作用域清理与提前释放

标准库的 `Vec`、`Dict` 和 `Set` 定义了 `drop()`，未转移的局部值离开作用域时会自动调用它。它们的 `destroy()` 负责释放容器的缓冲；需要提前释放时可以显式调用。

不要把缓冲清理等同于元素清理：当前 `Vec.destroy()` 不逐个调用元素的析构。元素持有独立资源时，按元素类型的 API 处理；释放或重新分配后，也不能继续使用此前取得的元素引用。

对于需要显式配对的资源，也可以用 `defer`（第十课）安排清理：

```vyx fragment
var values = Vec::<i32>.new();
defer { values.destroy(); }
```

这个写法在退出作用域时调用 `destroy()`。这里的容器会把自身状态清空，之后的 `drop()` 不再重复释放缓冲；不能将这种行为推广到所有资源 API。

### 三个容器的分工

| 容器 | 用来做什么 | 关键方法 |
| --- | --- | --- |
| `Vec<T>` | 有序序列，按下标访问 | `push` `get` `count` `pop` |
| `Dict<K, V>` | 按 key 找 value | `put` `get` `contains` |
| `Set<T>` | 只关心「在不在」 | `add` `contains` `remove` |

三个都通过 `use std.collections;` 导入，均提供 `destroy()` 和作用域清理。

```vyx fragment
var scores = Dict::<string, i32>.new();
scores.put("alice", 42);
if (scores.contains("alice")) { print(scores.get("alice")); }

var tags = Set::<string>.new();
tags.add("vyx");
if (tags.contains("vyx")) { print("member"); }

scores.destroy();
tags.destroy();
```

### 增长为什么会「摊销成 O(1)」

`push` 看起来是一次追加，但容量写满时必须换一块更大的存储、把现有元素搬过去。单看那一次，代价是 O(n)；均摊到之前每一次追加，仍是常数级。

理解这一点决定了两件事：

- 已知规模时先 `reserve`（或 `ensure_capacity`）能把搬移次数压到零。
- 不要假设「第 n 次 push 和前一次一样便宜」——它偶尔会贵得多。

### 常见错误

- 容器缓冲释放后继续使用元素引用，或把缓冲清理误认为元素资源也已清理。
- 混用其他语言的容器 API；本教程统一使用 `Set.add` 和 `Dict.put`，`Dict.set` 也有别名支持。

## 第12课：闭包

有时候你要传出去的不是一段固定的逻辑，而是「带上当前上下文的一段逻辑」。为此专门定义一个具名函数和一个结构体，代价太大。

```vyx program
fn main() -> i32 {
    let offset = 7;
    // |参数列表| { 函数体 }；函数体里可以直接用 offset。
    let add_offset = |value: i32| { return value + offset; };
    print(add_offset(35));
    return 0;
}
```

输出 `42`。

### 闭包比函数指针多了什么

普通的函数值只能靠参数拿到数据。闭包不一样：它把定义处能看到的名字**捕获**进来，于是 `add_offset` 记住了 `offset = 7`，调用方不需要知道这件事。

这就是为什么「给一批数据加上当前的偏移」这类需求，用闭包写只需要几行；用函数就得额外传一个上下文参数，一路传到底。

### 捕获是有代价的

捕获的东西必须在**闭包使用期间一直有效**。捕获的是引用时尤其要注意：如果被引用的值先一步失效了，闭包再去读它就是错的。

写闭包时的实用规则是：**如果捕获的上下文只在当前作用域里短暂存在，就不要把一个还活着的闭包存到外面去。**

### 什么时候用闭包

- 回调：把「拿到结果之后怎么办」作为参数传进去。
- 小段局部逻辑：只用一次、不值得单独命名。
- 需要携带上下文的高阶调用。

反过来，如果一段逻辑要复用、要单独测试，或者要被很多地方调用，写成具名函数更清楚——闭包的价值在于「顺手」，不在于「通用」。

## 第13课：泛型

同一个操作，类型不同，实现完全一样。为每种类型各抄一遍，是纯粹的重复；而且以后改一处就得改一串。

```vyx program
fn identity<T>(value: T) -> T {
    return value;
}

// where 声明 T 必须满足的约束。
fn add<T>(left: T, right: T) -> T where T: Add {
    return left + right;
}

fn main() -> i32 {
    // 显式给出类型实参。
    print(identity::<i32>(42));
    print(add::<i32>(20, 22));
    return 0;
}
```

两次都输出 `42`。

### `where` 声明的是什么

`add` 的函数体里写了 `left + right`。`where T: Add` 把「`T` 支持相加」这个前提写进了签名：调用方一眼能看到约束，函数体里也能放心用 `+`。

值得说清楚的是，约束并不是「不写就编不过」的开关——当前的实现允许不带约束就使用这些运算。写出来的价值在于**把前提留在签名里**：读代码的人不必翻进函数体去推断这个泛型到底要求什么，将来实例化出错时也更容易定位。

对关联常量这类能力（第 37 课的 `T::MAX`）约束则是必需的：不声明 `T: Bounded`，`T::MAX` 根本解析不到。

### `::<...>` 什么时候要写

大多数时候类型实参能从参数推出来，`identity::<i32>(42)` 里的 `::<i32>` 其实可以省。需要显式写的场合是**推不出来**的时候，典型是泛型容器：`Vec::<i32>.new()` 没有任何参数能告诉你里面装什么，只能由你指定。

| 写法 | 含义 |
|---|---|
| `fn identity<T>(value: T) -> T` | 声明泛型形参 `T` |
| `let values: Vec<i32> = Vec::<i32>.new();` | 类型标注用 `<...>`，表达式显式给类型实参用 `::<...>` |
| `identity::<i32>(42)` | 给函数的泛型形参传入 `i32` |
| `values.push(42)` | 调用实例方法，`T` 已由容器类型确定 |
| `T::MAX` | 读取类型的关联常量；这里的 `::` 不是实例方法调用 |

### 代价：单态化

泛型不是运行时机制。每用一组具体类型实例化一次，就生成一份对应的机器码——`add::<i32>` 和 `add::<i64>` 是两份独立的函数。

好处是没有间接跳转、没有装箱，跑起来和手写版本一样快。代价是二进制体积随实例数量增长。所以泛型适合「少量类型、大量调用」的场景。

### 常见错误

- 该写 `::<T>` 的构造点忘了写，编译器推不出容器元素类型。
- 用了关联常量却没有在约束里声明对应的 trait。

## 第14课：`match`

一串 `if (x == 0) ... else if (x == 1) ... else ...` 能表达同样的意思，但读代码的人看不出两件事：**是不是每个情况都覆盖了**，以及**这里到底在判断哪一个值**。

```vyx program
fn label(value: i32) -> string {
    return match value {
        case 0 => "idle",
        case 1 => "running",
        default => "unknown",
    };
}

fn main() -> i32 {
    print(label(1));
    return 0;
}
```

输出 `running`。

### `match` 是一个表达式

注意 `return match ... ;`——`match` 直接产生值，所以它既能当语句用，也能像上面这样把结果交出去。每个分支的值类型必须相容，否则整个表达式没有确定的类型。

分支体也可以是多条语句的块：

```vyx fragment
case 1 => { let text = "running"; text; }
```

当前 `match` 分支块把最后一个表达式语句的值作为分支值，上例要保留 `text;` 的分号；不写 `return`。它与 `if` 表达式分支的 `{ text }` 写法不同，也不改变普通函数必须用 `return` 返回值的规则。

### 处理带载荷的情况

`Result` 这类枚举带着数据，模式里可以把数据取出来：

```vyx fragment
match (parse_next("42")) {
    case Ok(value) => { print(value); }
    case Err(_) => { print("invalid number"); }
}
```

这是函数体内的片段，`parse_next` 的完整定义见第 16 课。`case Ok(value)` 把成功值绑到 `value` 上；`case Err(_)` 里的 `_` 表示“忽略载荷”，不是整个 `match` 的兜底分支。兜底写 `default`。

### guard：模式之外的附加条件

模式匹配完之后还可以再补一个布尔条件：

```vyx fragment
return match parse_number(input) {
    case Ok(v) if v > 0 => v,
    case Ok(_) => 0,
    case Err(_) => -1,
};
```

`case Ok(v) if v > 0` 读作「是 `Ok` 且里面的值大于 0」。guard 写在不满足时就会落到后面的分支上，所以顺序仍然有意义。

### 现状与边界

- 兜底请写 `default`。**裸 `_` 作为顶层分支目前还不支持**，写了会得到后端错误而不是优雅的诊断。
- 被匹配的值只求值一次，可以放心在里面调用有副作用的函数。
- 完整正例和诊断用例见 [match 表达式回归门](../probes/gates/match-expression/README.md)。

## 第15课：字符串操作

字符串的处理需求高度集中在几个动作上：找、切、比大小写。

```vyx program
fn main() -> i32 {
    let text = "Hello, Vyx!";
    print(text.contains("Vyx"));
    print(text.substring(0, 5));
    print(text.to_upper());
    return 0;
}
```

依次输出 `true`、`Hello`、`HELLO, VYX!`。

### 常用方法一览

| 方法 | 作用 | 返回 |
| --- | --- | --- |
| `contains(s)` | 是否包含子串 | `bool` |
| `index_of(s)` | 子串第一次出现的位置 | 下标；找不到时的取值按实现约定 |
| `starts_with(s)` / `ends_with(s)` | 前缀 / 后缀判断 | `bool` |
| `substring(start, end)` | 取一段 | 文本 |
| `to_upper()` / `to_lower()` | 大小写转换 | 文本 |
| `len` | 字节长度 | 整数，教程使用属性写法 |

### `substring` 也是左闭右开

`text.substring(0, 5)` 从 `"Hello, Vyx!"` 里取出 `"Hello"`——下标 0 到 4，共 5 个字符。这跟第五课的区间规则是同一条：**右端点不包含在内**。

一致的边界规则带来的好处和循环那里一样：要取「前 n 个」就写 `substring(0, n)`，长度就是 `n`，不用再算一次减一。

### 字符串长度的写法

本教程统一写 `text.len`，表示字节长度，不能据此计算 Unicode 字符数量。原生字符串还支持 `size()`、`count()`、`length()` 等既有写法；不要把“教程选用属性”理解成其他写法都会编译失败。

### 插值仍然是最常用的

需要把几个值拼成一句话时，插值比手工拼接更省事，也不用担心中间的类型转换：

```vyx fragment
let name = "Vyx";
let score = 42;
print("${name}: ${score}");
```

## 第16课：错误处理——Result、Option 和 `?`

「这个操作可能失败」是需要写进**类型**里的信息。写进类型之后，调用方在签名处就能看到，编译器也会盯着你别装作没看见。

```vyx fragment
use std.core;

// 声明一组命名的失败原因。
error ParseError {
    InvalidNumber
}

fn parse_number(input: string) -> Result<i32, ParseError> {
    if (input == "42") { return 42; }
    // fail 交出一个错误值。
    fail ParseError.InvalidNumber;
}

fn parse_next(input: string) -> Result<i32, ParseError> {
    // ? 成功就解包，失败就立刻把错误交回给调用方。
    let value = parse_number(input)?;
    return value + 1;
}
```

`parse_next("42")` 得到 `Ok(43)`；`parse_next("bad")` 得到 `Err(ParseError.InvalidNumber)`。

### `?` 到底做了什么

`parse_number(input)?` 展开成：调用它，如果是 `Ok(v)` 就取出 `v` 继续往下走；如果是 `Err(e)` 就**当场返回** `Err(e)`，函数剩下的部分不再执行。

这一行替代的是：

```vyx fragment
let value = match parse_number(input) {
    case Ok(v) => v,
    case Err(e) => { fail e; }
};
```

嵌套几层之后，`?` 省下的就不只是行数，而是「错误要往上穿几层」这件事在视觉上的噪音。

### 为什么用返回值而不是异常

失败在签名里：`-> Result<i32, ParseError>` 明明白白告诉你这个调用可能失败。任何调用点都能看到，不必去猜某个函数背后会不会抛出东西。

调用方用 `match` 检查结果，或用 `?` 传播。当前编译器允许直接丢弃 `Result`，这里是 API 使用约定，不是已实现的强制处理诊断。外部语言异常的传播与清理另见 [DCI 规范](DCI_SPEC_ZH.md)。

### `Option<T>`：只有「有」和「没有」

有些失败不需要原因，只关心结果在不在——查一个 key 是否命中、解析一个可选字段。这时用 `Option<T>`，比硬塞一个用不上的错误类型干净。

### `panic` 还是 `Result`

判断标准和第九课一致：**调用方有没有可能写出正确的处理代码**。字符串格式不对，调用方可以选择报错给用户或者换输入，那就返回 `Result`；内部不变量被破坏，调用方无能为力，那就 `panic`。

### 常见错误

- 拿到 `Result` 直接当值用，忘了先解包（`?` 或者 `match`）。
- 该上抛的地方用 `panic`，把可以挽救的失败变成崩溃。

## 第17课：复合赋值与位运算

`x = x + 5` 里，`x` 写了两次。左边一旦复杂起来（比如带下标），写两遍就是两遍出错的机会。

```vyx program
fn main() -> i32 {
    var count = 10;
    count += 5;      // 等价于 count = count + 5
    count *= 2;
    print(count);
    print(0xFF & 0x0F);
    return 0;
}
```

输出 `30`，然后 `15`。

### 为什么 `+=` 不只是「短一点」

左边的表达式只需要求值一次，也只需要写一次。对 `values[index] += 1` 这种写法，重复写 `values[index]` 意味着下标计算可能出现两次——既费事又容易出现两处不一致。复合赋值把这种重复从语言层面消掉。

常用的还有 `-=`、`*=`、`/=`、`%=`、`&=`、`|=`、`^=`、`<<=`、`>>=`。

### 位运算

| 运算符 | 含义 |
| --- | --- |
| `&` | 按位与 |
| `|` | 按位或 |
| `^` | 按位异或 |
| `~` | 按位取反 |
| `<<` / `>>` | 左移 / 右移 |

```vyx fragment
print(0xFF & 0x0F);   // 15   取低四位
print(1 << 4);        // 16   左移四位
print(0xFF ^ 0x0F);   // 240  异或会翻转低四位
```

位运算直接在整数的位上进行。整数的宽度在编译期就定了（第三课），所以这些操作能一对一映射到机器指令，没有额外的检查或转换。

也正因为宽度是固定的，`<<` 移出边界的位就是真的丢了——**不会自动扩展宽度**。需要更宽的结果时，先把值转成更宽的类型再移位。

### 常见错误

- 把 `&` 当成逻辑与。判断条件要用 `&&`；`&` 是按位运算。
- 移位量超出位宽时结果不由语言保证，别依赖它。

## 第18课：for-in 的各种用法

同一个循环结构，可以驱动好几种不同的东西。

```vyx program
fn main() -> i32 {
    let values: [i32; 3] = [10, 20, 30];
    var total: i32 = 0;
    // 按下标遍历。
    for (index in 0..3) {
        total += values[index];
    }
    print(total);
    return 0;
}
```

输出 `60`。

### 按索引，还是直接取元素

两种都能拿到数据，区别在于你要不要**位置**：

```vyx fragment
for (index in 0..3) { total += values[index]; }   // 需要下标时

for (value in values) { total += value; }         // 只关心元素本身
```

需要下标的情形：要写回数组、要按位置挑元素、要拿相邻元素。不需要位置时就别引入下标——少一个变量，也少一次越界的可能。

容器同样可以直接遍历：

```vyx fragment
use std.collections;

var values = Vec::<i32>.new();
values.push(10);
values.push(20);
for (v in values) { print(v); }
values.destroy();
```

### 区间仍然是左闭右开

`for (index in 0..3)` 走 0、1、2。长度 3 的数组刚好用 `0..3` 覆盖完——这就是第五课那条规则在数组上的自然结果，也是它最常被用到的地方。

### `while` 与 `break` / `continue` 照旧

轮数不确定时用 `while (条件)`；`break` 结束整个循环，`continue` 跳过本轮剩下的语句。嵌套里想跳出外层，用第五课讲过的标签。

## 第19课：多文件项目与 `Vyx.toml`

一个文件迟早装不下。这时候需要的不是「把代码拆开」这么简单，而是回答三个问题：**文件之间怎么互相看见**、**哪些东西可以对外**、**构建系统怎么知道要编哪些文件**。

先建目录：

```text
squares/
  Vyx.toml
  src/main.vyx
  src/math.vyx
```

`src/math.vyx`：

```vyx file=src/math.vyx
module squares;

public fn square(value: i32) -> i32 {
    return value * value;
}
```

`src/main.vyx`：

```vyx file=src/main.vyx
module squares;

public fn main() -> i32 {
    print(square(6));
    return 0;
}
```

`Vyx.toml`：

```toml
[package]
name = "squares"
version = "0.1.0"
entry = "src/main.vyx"

[build]
output_dir = "target"
auto_sources = false

[target.squares]
type = "executable"
entry = "src/main.vyx"
sources = ["src/math.vyx"]
```

在项目目录执行：

```sh
vyxc build --target squares
```

Windows 运行 `.\target\squares.exe`，Linux 运行 `./target/squares`，输出 `36`。

### 为什么两个文件写同一个 `module` 名

模块名相同的文件属于**同一个模块**：共享一套命名空间，互相引用不需要任何限定前缀，`main.vyx` 里直接写 `square(6)` 就行。

这是把「一个模块拆成几个文件」和「拆成几个模块」分开的两件事。前者只是物理上的分行，后者才有边界。按功能拆文件、但不急着拆模块，是绝大多数项目该有的起点。

### 用 @[vis] 声明模块边界

示例里的 `square` 用 `public` 对外开放，也可以写成 `@[vis(world)]`。
如果辅助函数只供本包使用，用 `@[vis(package)]`；只供特定模块使用，用
`@[vis(in(...))]`。示例中的 `main` 使用 `public`；可执行入口由清单选择，`public` 本身不决定它是不是入口。

```vyx fragment
module squares.math;

@[vis(package)]
fn square(value: i32) -> i32 { return value * value; }

@[vis(in(squares.ui) + friend(squares_tests))]
fn debug_square(value: i32) -> i32 { return square(value); }
```

这里 `squares` 是包身份，`squares.ui` 是被允许的模块树，`squares_tests`
是被允许的另一个包。`tree` 指模块的子树，和类继承没有关系。
`+` 合并访问范围，`&` 取交集，`-` 排除范围；显式 `@[vis(...)]`
优先于 `public` 简写。同模块拆文件与跨模块导入是不同的边界。
完整范围表见[第八课](入门指南_ZH.md#第八课类与方法)；项目用例见
[visibility_semantics](../tests/projects/visibility_semantics/src/api.vyx)。

### 为什么清单里要写 `sources`

这里用 `auto_sources = false` 关闭自动扫描，显式列出目标入口和额外源码。编译器仍会处理源码里的项目导入依赖；不要把显式清单、自动扫描和 `use` 导入混为一件事。

多写一行的代价，换来的是构建结果不随目录里意外多出来的文件而改变。

### 继续往下

- `vyxc new`、构建设置与依赖的完整步骤见[创建与配置项目](PROJECTS_ZH.md)。
- 清单每个字段的说明见[项目清单](PACKAGE_MANIFEST_ZH.md)。
- 独立模块、跨模块枚举与常量的完整流程见[项目指南](PROJECTS_ZH.md#导入独立模块)和[跨模块回归门](../probes/gates/cross-module/README.md)。
- 编译后的泛型库如何在 `.vyi` 接口里携带实例化函数体，见[泛型接口回归门](../probes/gates/generic_interfaces/README.md)。

## 第20课：C 语言互操作——extern

已经存在的 C 库不必重写。Vyx 能直接调用它们，代价是要把「这个函数的 ABI 长什么样」说清楚。

```vyx program
extern "C" {
    fn abs(x: i32) -> i32;
}

fn main() -> i32 {
    print(abs(-42));
    return 0;
}
```

输出 `42`。

### `extern "C"` 声明的是约定，不是实现

`extern "C"` 块里只有签名，没有函数体。它的作用是告诉编译器：**这个名字按 C 的调用约定去找**，参数和返回值怎么摆放，不要按 Vyx 自己的规则来。

实现来自链接进来的库。所以 `extern "C"` 是一份**契约**——写错了签名，编译器不会拦住你，出问题要等到运行时。这也是为什么跨语言边界的类型要格外小心。

### 回调要写 `cfn`

C 那边要求传函数指针时，用 `cfn(...) -> R` 类型。普通的 Vyx `fn` 值不能直接当 C 回调用，因为两者的调用约定不是一回事——`cfn` 就是把这个差异写在类型里。

### 什么时候该用 DCI 而不是 `extern "C"`

`extern "C"` 能表达的是 C 的 ABI：函数、指针、按值传的基本类型。

一旦需要下面这些，信息量就超出了 C ABI 能承载的范围：

- C++ 的类、继承、虚函数
- 带构造析构与生命周期语义的对象
- 跨语言的泛型实例

这些用 [DCI](MOSP_ZH.md#dci生产端事实与开放泛型) 表达：契约由生产端生成，包含真实的类型布局与符号，消费方按它生成 stub。生产端也不限于 C++——Rust 通过自己的 Adapter 提供同样的事实。

## 接下来

- [高级特性](高级特性_ZH.md)：所有权与借用、异步、trait、继承、编译期能力与运行时反射。
- [从 Rust / C++ 迁移](快速迁移_Rust_CPP.md)：把已经会的写法逐条对照过来。
- [事实语义所有权系统](MOSP_ZH.md)：版本迁移、反射、跨语言互操作、代码裁剪与事实语义后果。
- [入门教程](入门指南_ZH.md)：需要回头补基础时从这里开始。

相关用例：[tutorial_intermediate_core.vyx](../tests/cases/tutorial_intermediate_core.vyx)、
[tutorial_manifest](../tests/projects/tutorial_manifest/Vyx.toml)。
