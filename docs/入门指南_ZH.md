# Vyx 入门教程

[English](TUTORIAL.md) · [文档目录](README.md) · [事实语义所有权系统](MOSP_ZH.md)

十节课，从第一个可执行文件走到「类」与「作用域清理」。每一课都按同一条线推进：先说清要解决什么问题，再看最朴素的写法为什么不够用，然后给出 Vyx 的写法和它背后的取舍。

学完之后，你应该能不查资料写出几十行的单文件程序，并且读懂 `tests/cases/tutorial_beginner_core.vyx` 与 `tests/cases/tutorial_beginner_surface.vyx` 里的每一行。

> **提示** 本教程假设你已经知道变量、分支、循环、函数大致是什么样子。不需要事先了解 Vyx，也不需要 C、Rust 或 C++ 的背景。

## 安装 SDK

先把「能跑起来」这件事解决掉。Vyx 的 SDK 是一个自包含的压缩包，解压即用，不需要安装器，也不需要先克隆仓库。

Windows x86_64：[下载 ZIP](https://github.com/vyx-lang/vyx/releases/latest/download/vyx-sdk-windows-x86_64-llvm22.zip)

```powershell
Expand-Archive .\vyx-sdk-windows-x86_64-llvm22.zip -DestinationPath .\vyx
$sdk = (Resolve-Path .\vyx\vyx-sdk-windows-x86_64-llvm22).Path
$env:PATH = "$sdk\bin;" + $env:PATH
vyxc --version
```

Linux x86_64：[下载 tar.gz](https://github.com/vyx-lang/vyx/releases/latest/download/vyx-sdk-linux-x86_64-llvm22.tar.gz)

```bash
tar -xzf vyx-sdk-linux-x86_64-llvm22.tar.gz
export PATH="$PWD/vyx-sdk-linux-x86_64-llvm22/bin:$PATH"
vyxc --version
```

Windows 与 Linux 两个 SDK 都已经内置 LLVM 后端，解压完就能编译，不需要另外准备工具链。

### 为什么是「改 PATH」而不是「跑安装程序」

`vyxc` 只是 SDK 目录里的一个可执行文件，它要靠自己所在的路径反推出同级的 `std`、`std_packages` 与运行时。把 `bin` 加进 PATH 之后，这层相对位置才是完整的。由此有两条纪律：

- 不要把 `vyxc.exe` 单独拷到别处，那样它找不到标准库。
- 解压出来的目录结构保持原样，别拆开搬运。

上面两条命令只对当前终端生效，新开终端要重新执行一遍，或者写进 shell 的启动配置。`vyxc --version` 能打印出编译器版本，就说明位置对了——这是往下走的前置条件。

## 编译与运行

把后面每一课的完整示例保存成一个 `.vyx` 文件，然后让编译器编译并立即运行：

```sh
vyxc --src=file hello.vyx --run=aot
```

`--run=aot` 的含义是「编译成本机程序，然后马上执行」。要留下可执行文件、之后再运行：

```powershell
# Windows
vyxc --src=file hello.vyx --emit=exe -o hello.exe
.\hello.exe
```

```bash
# Linux
vyxc --src=file hello.vyx --emit=exe -o hello
./hello
```

### 为什么入门阶段先只用一个文件

单文件编译把「构建系统」这一整层变量排除掉了：没有清单、没有目标、没有依赖解析，输入只有一个 `.vyx`，输出要么是程序要么是编译器报错。等语言本身熟了再去碰多文件项目（第十九课），排查问题时能少一半怀疑对象。

真正写东西时用 `--emit=exe` 留下可执行文件更划算。`--run=aot` 的价值在于把「改一行、看一眼结果」的循环压到最短。

## 第一课：程序入口与输出

程序总要回答两个问题：从哪一行开始跑，以及怎么把结果交出去。Vyx 的答案分别是 `main` 和 `print`。

```vyx
// 可执行程序从 main 开始执行。-> i32 声明这个函数交回一个 32 位整数。
fn main() -> i32 {
    // print 把实参依次写成文本，并在最后一个实参后补一个换行。
    print("Hello, Vyx!");
    // return 0 表示正常结束；这个值会成为进程的退出码。
    return 0;
}
```

拆开看这个函数：

- `fn` 开始一个函数定义，后面跟函数名。
- `main` 后面的 `()` 表示它不接收参数。
- `-> i32` 是返回类型。**声明了什么类型，就必须真的返回那个类型。**
- 函数体用 `{ }` 包起来，每条语句以 `;` 结尾。

### 为什么返回值是 `i32`，而不是「什么都不返回」

因为 `main` 的返回值不会被丢掉：它变成进程的**退出码**。在 shell 里用 `echo $?`（PowerShell 里用 `$LASTEXITCODE`）看到的就是这个数字，`0` 表示成功，非 0 表示失败。

这条约定让程序能跟外部世界对话。脚本、CI、构建系统判断你的程序有没有出错，看的就是这个数字，而不是去解析你打印出来的文本。

### `print` 的两条行为

- 它把每个实参依次转成文本写出去，中间**不加任何分隔符**。`print("a", 1)` 输出的是 `a1`。
- 它在最后一个实参后面补一个**换行**，所以两次 `print` 各自占一行。

要拼内容就交给字符串插值（第二课），要分行就分两次调用。

### 常见错误

- **漏掉 `return`**。函数声明了 `-> i32` 就必须交出一个 `i32`，这不是警告而是编译错误。
- 字符串用双引号 `"..."`，不是单引号。
- 每条语句结尾的 `;` 不能省。

## 第二课：变量与可变性

程序里大多数值会变。真正的问题是**哪些允许变**：如果所有名字都可能被任意改写，读代码时你就无法在某一行的位置确定它的值。

```vyx
fn main() -> i32 {
    // let 声明一个绑定：这个名字不能再指向别的值。
    let name = "Vyx";
    // var 声明一个可重新赋值的绑定。
    var score: i32 = 20;
    score = score + 22;
    // ${...} 里的表达式先求值，再转成文本。
    print("${name}: ${score}");
    return 0;
}
```

输出 `Vyx: 42`。

### 为什么默认是 `let`

把「不可变」做成默认，收益在读代码的那一侧：看到 `let`，你就知道这个名字在整个作用域里始终指向同一个值，不必回头翻找有没有被改过。需要改的时候必须显式写 `var`，于是**所有可变点都在源码里留了记号**。

写错的时候编译器会把出路直接给你：

```text
error: E3000: cannot assign to immutable binding 'a'
note: N3000: change `let` to `var` / `let mut`, or write `mut` on the parameter
```

### `let` 不等于「对象不可改」

这是最容易误解的一条。`let counter = Counter(41)` 说的是**名字 `counter` 不能再指向别处**；它指向的那个对象，状态照样能改：

```vyx
let counter = Counter(41);
counter.increment();   // 合法：改的是对象，不是绑定
```

按 C++ 的词汇，它更接近 `T* const` 而不是 `const T*`。绑定本身是只读的，绑定到的东西不是。

### 类型什么时候可以省

`var score: i32 = 20` 里的 `: i32` 常常能省——初值是 `20`，类型推导就够用了。需要写出来的场合有两类：

- 初值看不出类型，比如先声明、稍后再赋值。
- 你想要的类型和推导结果不一致。

第三课接着讲类型。

## 第三课：基本类型与类型推导

「数字」在机器里从来不是一种东西。位宽、有没有符号、是不是浮点，决定了能表示多大范围、以及运算要付多少代价。Vyx 要求这件事在源码里是明确的——由你写出来，或者由初值推出来。

```vyx
fn main() -> i32 {
    let count: i32 = 42;
    // as 是显式的数值转换。
    let total: i64 = count as i64;
    let ratio: f64 = 1.5;
    let ready: bool = true;
    // [T; N] 是固定长度数组，索引从 0 开始。
    let values: [i32; 3] = [10, 20, 30];
    print("${total}, ${ratio}, ${ready}, ${values[1]}");
    return 0;
}
```

输出 `42, 1.5, true, 20`。

### 为什么转换必须写 `as`

`let total: i64 = count;` 不合法，哪怕 `i32` 加宽到 `i64` 明明不会丢数据。原因是规则要覆盖所有情况：`i64` 缩到 `i32` 会截断，浮点转整数会丢小数。如果只对「安全的那部分」允许隐式转换，读代码的人就得在脑子里维护一张「哪几种能省、哪几种不能省」的清单。

写 `as` 的成本是三个字符，换来的是**每一个可能丢信息的转换点都在源码里可见**。

### 字面量怎么写

| 写法 | 含义 |
| --- | --- |
| `42` | 十进制 |
| `0x2A` / `0b101010` / `0o52` | 十六 / 二 / 八进制，都等于 42 |
| `100i64` | 带类型后缀的字面量，这里明确是 64 位 |
| `255u8` | 无符号 8 位，范围正好卡满 |

### 常用类型

| 类型 | 说明 |
| --- | --- |
| `i32` / `i64` | 有符号整数，日常默认用 `i32` |
| `u32` / `u64` / `u8` | 无符号整数，适合位运算与字节 |
| `f32` / `f64` | 浮点数；没特殊理由就选 `f64` |
| `bool` | 只有 `true` 和 `false` |
| `string` | 文本 |

### 推导的边界在哪里

`let x = 20;` 会推导成 `i32`，因为整数默认就是 32 位。所以在需要 64 位运算的地方，要么写 `20i64`，要么写 `let x: i64 = 20`。整数溢出不会自动升级宽度——这不是 Vyx 的疏漏，而是绝大多数系统语言共同的选择：宽度在编译期定死，运算才能直接映射到机器指令。

## 第四课：条件分支

分支是「同一个函数在不同输入下做不同事」的最基本手段。

```vyx
fn main() -> i32 {
    let temperature = 18;
    // 条件必须写在括号里。
    if (temperature < 20) {
        print("cool");
    } else {
        print("warm");
    }
    return 0;
}
```

输出 `cool`。

### 为什么条件非要加括号

把括号去掉会直接编译不过：

```text
error: E0002: expected '(', got `temperature`
```

两个字符换来的是一致性：`if`、`while`、`for` 的条件形式完全相同，解析器不需要在这里区分「表达式」和「块」的边界，你也不必在脑内补一层优先级。模板化的写法看着啰嗦，扫读时反而更快。

### 比较与逻辑运算

| 用途 | 运算符 |
| --- | --- |
| 相等 / 不等 | `==` `!=` |
| 大小 | `<` `<=` `>` `>=` |
| 逻辑与 / 或 / 非 | `&&` `||` `!` |

### 分支也能产生值

`if` 是表达式，可以把它当作值交出去：

```vyx
fn choose(flag: i32) -> i32 {
    return if (flag == 1) { 10 } else { 20 };
}
```

只判断「二选一」时，`? :` 更紧凑：

```vyx
let mood = age > 20 ? "adult" : "child";
```

两条分支的类型必须相容，否则编译器没法给整个表达式定类型。

### 常见错误

- 条件漏括号，触发 `E0002`。
- 把比较写成赋值：`if (a = 1)` 不是比较，`==` 才是。

## 第五课：循环、`break` 与 `continue`

循环要处理的核心问题是边界：从哪儿开始、到哪儿停、哪一轮跳过。Vyx 用左闭右开的区间把「到哪儿停」写清楚。

```vyx
fn main() -> i32 {
    var total: i64 = 0;
    // 0..6 是左闭右开区间：i 依次取 0、1、2、3、4、5。
    for (i in 0..6) {
        if (i == 2) { continue; }   // 跳过本轮剩下的语句
        if (i == 5) { break; }      // 直接结束整个循环
        total = total + i;
    }
    print(total);
    return 0;
}
```

输出 `8`。

### 为什么区间不包含右端点

`0..6` 的长度正好是 6，`0..n` 的长度正好是 `n`。这一个性质带来两个好处：

- 长度不用再 ±1 推算，`n` 是多少就是多少。
- 相邻区间可以无缝拼接：`0..3` 和 `3..6` 不重不漏，正好覆盖 `0..6`。

### 8 是怎么来的

把每一轮摊开看：

| `i` | 这一轮做什么 | `total` |
| --- | --- | --- |
| 0 | 累加 | 0 |
| 1 | 累加 | 1 |
| 2 | `continue`，跳过累加 | 1 |
| 3 | 累加 | 4 |
| 4 | 累加 | 8 |
| 5 | `break`，循环结束 | 8 |

`continue` 只跳掉本轮剩下的语句，`break` 结束整个循环——差别就在于「下一轮还进不进来」。

### 条件循环与带标签的 `break`

条件不定次数时用 `while (条件) { ... }`。嵌套循环里想一次跳出外层，给外层贴个标签：

```vyx
'outer: while (labeled < 10) {
    while (true) {
        break 'outer;   // 不是只跳出内层
    }
}
```

不加标签的 `break` 只管最近的一层，这在两层以上嵌套时很难用缩进看出意图，标签把这个意图写明。

## 第六课：函数

当同一段逻辑出现第二次，就该把它变成函数。函数同时是**给未来的自己看的边界**：里面怎么变都行，只要签名不变。

```vyx
// 参数必须写类型；b 带默认值 1。
fn add(a: i32, b: i32 = 1) -> i32 {
    return a + b;
}

fn main() -> i32 {
    print(add(41));              // 用默认值 -> 42
    print(add(b: 22, a: 20));    // 具名传参，顺序随意 -> 42
    return 0;
}
```

### 为什么参数一定要写类型

函数签名是模块之间唯一的契约面。类型写在签名里，调用方读签名就知道该传什么、会拿到什么，不必去看函数体。返回类型同理：`-> i32` 是一句承诺。

没有返回值的函数省略 `->`，例如 `fn increment() { ... }`。

### 默认值与具名传参各解决什么

- **默认值**让最常见的调用最短：`add(41)`，不必每次都重复那个显然的值。
- **具名传参**把「位置」这个负担拿掉。参数一多，`f(w, h, x, y)` 到底谁在前谁在后全靠记；写成 `make_rect(w: 3, h: 4, x: 1, y: 2)` 之后，调用点自己就把意思说清楚了，顺序也不再要紧。

两者可以组合使用。

### 常见错误

- 参数漏写类型。
- 声明了返回类型却有没有返回值的分支——和第一课的漏 `return` 是同一种错误。

## 第七课：结构体

几个总是同时出现的数据，应该绑成一个类型，而不是一路用平行变量传下去。

```vyx
struct Point {
    x: i32;
    y: i32;
}

fn main() -> i32 {
    // 用 类型名 { 字段: 值 } 构造一个值。
    let point = Point { x: 3, y: 4 };
    // 用 . 读字段。
    print(point.x + point.y);
    return 0;
}
```

输出 `7`。

### 为什么要有 `struct`

对比一下不用它的时候：一个点要有 `point_x`、`point_y` 两个变量；一个矩形就是四个。一旦把它们作为参数传递，签名会膨胀成 `make_rect(x, y, w, h)` 这种「顺序错了编译器也拦不住」的形式。

`struct` 把「这几个字段是一伙的」变成类型系统里的事实。函数签名收缩成 `fn area(rect: Rect)`，字段顺序错误不再可能发生。

### 和 `class` 的分工

`struct` 只打包数据：没有方法和构造函数，字段默认可访问，也可以用 `@[vis(...)]` 限定范围。`class` 在此基础上加上行为与封装。判断标准很简单：

- 只是想**把几个值一起搬运**，用 `struct`。
- 这个类型有**自己的行为和不变量**，或者要跟 C++ / Rust 的类对接，用 `class`（第八课）。

### 字段声明以 `;` 结尾

注意这里和 C / C++ 的习惯不同：字段后面是分号，不是逗号。整套语言里「声明」都以 `;` 收尾，成员声明也不例外。

## 第八课：类与方法

当数据和行为必须绑在一起时，用 `class`。

```vyx
class Counter {
    // public 说明这个字段对外可见。
    public value: i32;

    // 构造函数与类同名，通过 return 一个实例来产生对象。
    public Counter(start: i32) {
        return Counter { value: start };
    }

    public fn increment() {
        // self 指当前实例。
        self.value = self.value + 1;
    }
}

fn main() -> i32 {
    let counter = Counter(41);
    counter.increment();
    print(counter.value);
    return 0;
}
```

输出 `42`。

### 构造函数为什么写成 `return` 一个实例

构造函数体里没有隐含的「初始化 `self`」魔法，而是显式地交出一个实例：`return Counter { value: start };`。

这样做的好处是「对象从哪来」在代码里是看得见的。同一个类型也可以不写构造函数，直接用字段初始化：`Counter { value: 0 }`——两者都用同一套构造语法，没有第二套隐藏规则。

### 可见性：@[vis]

Vyx 用 `@[vis(范围)]` 指定声明允许哪些调用方访问。`public` 是
`@[vis(world)]` 的简写；更细的范围也写在声明上。Vyx 的可见性写法不使用
`protected`、`private` 或 `internal` 访问级别。

```vyx
module counter.api;

@[vis(world)]
fn initial_value() -> i32 { return 41; }

@[vis(package)]
fn normalize(value: i32) -> i32 { return value < 0 ? 0 : value; }

@[vis(in(counter.ui) + friend(test_tools))]
fn reset_value() -> i32 { return 0; }
```

| 范围 | 允许访问的调用方 |
|---|---|
| `world` | 所有调用方，等同于 `public` |
| `self` | 当前编译单元中的声明，不向导入方开放 |
| `mod` | 声明所在的模块 |
| `super` | 声明所在模块和它的直接父模块 |
| `tree` | 声明所在模块及其子模块 |
| `package` | 同一包；当前按模块路径的首段判断包身份 |
| `in(a.b, c.d)` | 指定模块及其子模块 |
| `friend(a, b)` | 指定包；参数是模块路径的首段 |
| `none` | 不允许访问 |

范围可以组合：`+` 合并，`&` 取交集，`-` 排除。
例如 `@[vis(world - in(counter.generated))]` 对外开放，但排除该模块树。
组合较长时用括号明确分组。

未标注的普通声明默认使用 `self`，类自身可以访问自己的成员；它不是
C++ 的类级 `private`。`struct` 字段默认可访问，显式 `@[vis(...)]` 可以收紧。
跨模块的例子见[第十九课](进阶教程_ZH.md#第19课多文件项目与-vyxtoml)。

### 方法能改状态，绑定仍然是 `let`

`counter.increment()` 会改掉 `counter.value`，而 `counter` 是用 `let` 声明的。两条规则并不冲突：

- `let` 约束的是**绑定**，不是绑定指向的对象。
- 要改的是对象内部的状态，那属于对象自己的事。

需要「连绑定一起换掉」时才用 `var`。

### 常见错误

- 忘了写 `public`，然后从外部访问字段失败。
- 构造函数里忘了 `return` 实例。

## 第九课：前置条件与 `panic`

不是所有错误都能「返回给调用方解决」。有些是调用方从逻辑上就不该那么调用——比如传了 0 当除数。

```vyx
fn divide(a: i32, b: i32) -> i32 {
    // 前置条件不满足，继续算下去没有意义。
    if (b == 0) {
        panic("divisor must not be zero");
    }
    return a / b;
}

fn main() -> i32 {
    print(divide(42, 2));
    return 0;
}
```

输出 `21`。把调用换成 `divide(42, 0)`，程序会打印 `divisor must not be zero`，然后以**非零退出码**结束——`panic` 之后的代码不会再执行。

### `panic` 和「返回错误」怎么分工

| 情况 | 用什么 |
| --- | --- |
| 调用方可以合理地处理：文件不存在、输入格式不对 | 返回 `Result` / `Option`，见第十六课 |
| 继续执行没有意义：内部不变量被破坏、参数越界 | `panic` |

判断标准是「**调用方有没有可能写出正确的处理代码**」。文件打不开，调用方可以重试或者换路径，那就该返回错误；除数是 0，调用方除了改代码没有别的办法，那就不该把负担转给它。

### 一个必须知道的坑

`panic` 需要你**显式写出来**。整数除以零本身不会触发 `divide` 里那个 `panic`：

```vyx
print(1 / 0);   // 结果未定义，不要指望它替你报错
```

所以「除数为零」这类前置条件必须自己检查——这正是 `divide` 开头那个 `if` 存在的全部理由。

### 常见错误

- 拿 `panic` 当错误处理用，让调用方无法挽救。
- 以为 `panic` 之后还能兜底：它直接终止进程。

## 第十课：`defer` 与作用域清理

资源的释放点如果靠人肉记忆，早晚会漏。Vyx 的做法是把「退出这个作用域时要做什么」写在申请资源的旁边。

```vyx
fn main() -> i32 {
    defer { print("finished"); }
    print("working");
    return 0;
}
```

先输出 `working`，再输出 `finished`。

### 为什么是「后进先出」

登记两个看看顺序：

```vyx
defer { print("first registered"); }
defer { print("second registered"); }
print("body");
```

输出顺序是 `body`、`second registered`、`first registered`——**最后登记的跑在最前**。

这不是随便定的。资源天然是「后来的依赖先来的」：你先打开文件、再在它上面建锁，那么必须先放锁、后关文件。`defer` 的 LIFO 顺序正好让「释放顺序 = 申请顺序的镜像」，写代码时按申请顺序写下去即可，不用倒着组织。

### 提前 `return` 也不会漏

`defer` 挂在**作用域退出**这个事件上，而不是挂在某条语句后面。函数从哪个出口离开都一样会执行，所以清理逻辑不必在每个 `return` 前面复制一遍。

### 注意事项

- `defer` 块里用到的东西，在执行时必须仍然有效。
- `defer` 只负责「在正确的时机调用」，具体怎么释放仍然要遵守对应 API 的约定。比如容器通常要求显式调用 `destroy()`——`defer` 帮你决定**什么时候**调，不改变**要调什么**。

## 创建第一个项目

单文件跑通之后，下一步是把它变成一个真正的项目。

```sh
vyxc new hello_app
cd hello_app
vyxc build --target hello_app
```

Windows 运行 `target/hello_app.exe`，Linux 运行 `./target/hello_app`。

生成的 `Vyx.toml` 描述入口、输出目录、目标和依赖——也就是「构建系统需要知道的全部」。从这个模板起步，比从空目录手写清单要少踩很多坑。

继续阅读[创建与配置项目](PROJECTS_ZH.md)，学习添加源文件、调整构建设置和增加目标；插件安装、LSP 与 DAP 配置见[编辑器与调试](TOOLING_ZH.md)。

## 接下来

- [进阶教程](进阶教程_ZH.md)：集合容器、闭包、泛型、`match`、错误处理和多文件项目。
- [高级特性](高级特性_ZH.md)：所有权与借用、异步、trait、comptime、继承与运行时反射。
- [从 Rust / C++ 迁移](快速迁移_Rust_CPP.md)：把已经会的写法逐条对照过来。
- [事实语义所有权系统](MOSP_ZH.md)：Migrate、Reflection、DCI、DCE 与 Effect 的用法。
- [Effect](MOSP_EFFECT_ZH.md)：事实消费、编译后果与自定义属性。
- [语言表面](语言表面_ZH.md)：已实现语法的索引。

对应语法用例位于 [tutorial_beginner_core.vyx](../tests/cases/tutorial_beginner_core.vyx)
与 [tutorial_beginner_surface.vyx](../tests/cases/tutorial_beginner_surface.vyx)。
