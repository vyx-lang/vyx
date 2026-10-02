# Vyx 快速迁移指南：Rust / C++ 开发者

[English: Moving from Rust or C++](MIGRATING_FROM_RUST_CPP.md) · [文档目录](README.md)

> 这是一张对照地图，不是 ABI 或所有权规范。要动手写项目，以[设计文档](设计文档_ZH.md)、
> `TESTING_GUIDE.md` 和实际编译结果为准；C / Rust / C++ 互操作以 `DCI_SPEC_ZH.md`、
> 当前 headers 与项目回归为准。下面每个结论都在本仓库的当前编译器上实测过，
> 但它是迁移指引，不是全功能清单。

你已经会 Rust 或 C++，所以这份材料不从「什么是变量」讲起。它只回答三个问题：

1. **这个写法在 Vyx 里怎么写？** 每节先给一张对照表，把词汇映射过去。
2. **换过去之后效果有什么不同？** 表格下面是解释：哪一条会改变你的代码结构，哪一条只是换个拼写。
3. **Vyx 为什么这么定？** 有些差异是刻意取舍（比如成员函数必须写在 `impl` 或 `class` 体内、`match` 兜底写 `default` 而不是 `_`）。知道动机，才能在新场景里自己判断，而不是背规则。

读法：先扫表格建立映射，再回头读「为什么」。想补齐语言本身，按顺序看入门章与进阶章，这里不重复它们的课程结构。

三句话先给你一个抓手：

- **值语义是默认。** `class` 和 `struct` 都是内联记录，赋值与传参按位拷贝；要共享堆对象，显式用 `Ref` / `Box`。
- **失败写进类型。** `Result<T, E>` + `?`，没有异常，也没有隐式展开。
- **没有 GC，也没有隐藏的析构。** 容器的堆内存要显式 `destroy()`；`defer` 与 `drop()` 决定的是**什么时候**调，不是**调什么**。

---

## 基础语法

| | Rust | C++ | Vyx |
|---|------|-----|-----|
| 不可变 | `let x = 42;` | `const int x = 42;` | `let x = 42;` |
| 可变 | `let mut x = 0;` | `int x = 0;` | `var x = 0;` |
| 类型标注 | `let x: i32 = 42;` | `int x = 42;` | `let x: i32 = 42;` |
| 函数 | `fn foo(x: i32) -> i32` | `int foo(int x)` | `fn foo(x: i32) -> i32` |
| 字符串 | `String` / `&str` | `std::string` / `string_view` | `String` / `str` |
| 打印 | `println!("{x}")` | `std::cout << x` | `print("${x}")` |
| 格式化 | `format!("{} {}", a, b)` | `sprintf(buf, "%d %d", a, b)` | 字符串插值：`"${a} ${b}"` |

### 为什么是 `let` / `var`，而不是 `let` / `let mut`

Rust 用修饰符表达可变（`let mut`），C++ 用类型限定（`const`）。Vyx 把它拆成两个关键字：`let` 绑定不可再赋值，`var` 声明可变。

效果是**可变性永远出现在声明的第一个词上**。读代码时不用回头找 `mut` 在哪，也不用分辨「这个 `const` 是约束接口还是约束实现」。写错了编译器直接给动作建议，而不是丢一句错误码：

```text
t10.vyx:3:7: error: E3000: cannot assign to immutable binding 'a'
help: change `let` to `var` / `let mut`, or write `mut` on the parameter
```

### 为什么没有 `format!` / `sprintf`

Vyx 把格式化收进字符串字面量本身：

```vyx
fn main() -> i32 {
    let a = 1;
    let b = 2;
    print("a=${a} b=${b}");
    return 0;
}
```

输出 `a=1 b=2`。

这一个设计同时消掉三类问题：C 的 `sprintf` 要自己管临时 buffer；Rust 的 `format!` 读起来要点括号数实参；C 的 `%d` 与实参类型不匹配是未定义行为。插值就是一个表达式，所以类型检查发生在编译期。

有两点和 `println!` 不一样，迁移时容易踩：

- **`print` 的各实参之间不加分隔符，末尾补一个换行。** 所以一行里拼多段，靠插值而不是靠逗号。
- **插值内使用普通表达式语法。** 字符串字面量可以直接写在 `${...}` 中，内层引号不需要为外层字符串转义：

```vyx
print("has=${m.contains("a")}");
```

要输出字面量 `${...}`，写 `\${...}`。

### `str` 和 `String`

和 Rust 一样分成「借用的视图」与「拥有的缓冲」，但读法更松：

```vyx
var owned: String = "hi";   // 拥有
owned = owned + "!";        // 追加得到新的 String
let view: str = "abc";      // 只读视图
print("${view.len}");       // 长度是字段 len，不是方法 len()
```

从 C++ 迁移时注意最后一行：**`str` 的长度是字段 `.len`**，写成 `.length()` / `.size()` 会得到类型错误，而不是一个能跑但结果不对的程序。

---

## 控制流

```
// Vyx                          // Rust                         // C++
if (x > 0) {                   if x > 0 {                      if (x > 0) {
    print("pos");                   println!("pos");                cout << "pos";
} elif (x == 0) {              } else if x == 0 {              } else if (x == 0) {
    print("zero");                  println!("zero");               cout << "zero";
} else {                       } else {                        } else {
    print("neg");                   println!("neg");                cout << "neg";
}                              }                               }

// for-in range
for (i in 0..10) { }          for i in 0..10 { }              for (int i = 0; i < 10; i++) { }

// Unicode 字符遍历
for (c in text) { }           for c in text.chars() { }       for (char32_t c : decoded_text) { }

// UTF-8 字节遍历
for (b in text.bytes()) { }   for b in text.bytes() { }       for (unsigned char b : text) { }

// match
match (x) {                    match x {                       switch (x) {
    case 1 => { ... }              1 => { ... }                    case 1: ... break;
    case 2 => { ... }              2 => { ... }                    case 2: ... break;
    default => { ... }             _ => { ... }                    default: ...
}                              }                               }
```

### 圆括号不是可选的，`elif` 是刻意选的

`if` / `elif` / `while` / `for` 的条件**必须带圆括号**。漏掉得到的是解析错误，不是「碰巧编译过」：

```text
t12.vyx:3:8: error: E0002: expected '(', got `x`
help: insert '(' before `x`
```

这是刻意的一刀。条件加括号之后，`if (a) - b` 之类**悬垂 else / 一元运算符**的歧义从文法层面就不存在，不需要回溯，也不需要给用户一条「请加括号」的风格建议。

第二个差异是 `elif`：Vyx 写 `} elif (...) {`，不写 `else if`。效果是**嵌套层级不随分支数增长**——五个分支仍然是五个平级分支，而不是五层缩进。

### `match` 是表达式，兜底写 `default`

```vyx
let code = match state {
    case 0 => 10,
    default => 20,
};
```

`match` 直接产生值，所以能出现在 `let`、`return`、实参位置。分支可以是块，块尾表达式就是这一支的值（和 Rust 一样，块尾不加分号）。

带载荷的情况用 `case` 绑定，`if` 后缀是 **guard**：

```vyx
enum State { Idle; Busy(i32); Done }

fn label(s: State) -> string {
    return match s {
        case Idle => "idle",
        case Busy(n) if n > 5 => "busy-high",
        case Busy(_) => "busy",
        case Done => "done",
    };
}
```

guard 不成立就继续试后面的分支，所以顺序有意义——`case Busy(n) if n > 5` 必须写在 `case Busy(_)` 前面。

**兜底写 `default`；Rust 的裸 `_` 顶层分支目前不支持**（写了会得到后端错误而不是诊断）。但变体内部忽略载荷的 `_` 是正常的，比如 `case Busy(_)`。

### 字符遍历和字节遍历是两套迭代器

```text
for (c in text) { }           for c in text.chars() { }
for (b in text.bytes()) { }   for b in text.bytes() { }
```

`for (c in text)` 按 **Unicode 字符**走，`for (b in text.bytes())` 按 **UTF-8 字节**走，两者必须显式区分。C++ 的 `for (char c : s)` 会让你在不知不觉中按字节处理多字节文本；Vyx 把「我正在按什么单位遍历」写进了语法。

---

## 结构体和方法

### 方法写在 `impl` 或 `class` 体内

```vyx
struct Point { x: f64; y: f64; }

impl Point {
    fn distance(self: Point, other: Point) -> f64 {
        let dx = self.x - other.x;
        let dy = self.y - other.y;
        return (dx * dx + dy * dy);
    }
}

fn main() -> i32 {
    let a = Point { x: 0.0, y: 0.0 };
    let b = Point { x: 3.0, y: 4.0 };
    print("${a.distance(b)}");
    return 0;
}
```

输出 `25`。

**注意：`fn Point.distance(...)` 这种点号挂载写法当前会解析失败。** 正确形式只有两种：`impl Point { fn ... }`，或者把方法直接写进 `class Point { ... }` 体内。从 C++ 过来的人容易顺手写出 `fn Type.method`，请改用上面这两种。

### 为什么 `self` 是写在参数表里的第一个参数

`self: Point` 是普通的第一个参数，不是预先绑定好的关键字。这条规则换来一件对迁移者很实际的事：**值语义和按引用传递在签名上一眼看得出**：

```text
self: Point        按值（拷贝）
self: &Point       借用，只读
self: &mut Point   借用，可写
```

C++ 里 `void f() const` 只告诉你方法不改对象，成员是值还是引用要看字段声明；Rust 里 `&self` 只说是借用，不说拷贝。Vyx 把「传的时候发生了多少次拷贝」提到了签名最前面——而这个信息在值语义语言里恰恰最贵。

另外，`struct` 声明用**分号**分隔字段，字面量用**逗号**：

```vyx
struct Point { x: f64; y: f64; }              // 声明：分号
let p = Point { x: 1.0, y: 2.0 };             // 字面量：逗号
```

## 类和 OOP

| | Rust | C++ | Vyx |
|---|------|-----|-----|
| 类 | 无（用 struct + impl） | `class Foo { }` | `class Foo { public fn method() {} }` |
| 继承 | 无（用 trait） | `class B : A { }` | `class B : A { }` |
| 接口 | `trait Drawable { }` | `class I { virtual ... }` | `trait Drawable { fn draw(); }` |
| 虚分发 | dyn Trait (vtable) | virtual (vtable) | `dyn Trait` → vtable |

```vyx
class Base {
    public x: i32;
    public fn get(self) -> i32 { return self.x; }
}

class Child : Base {
    public y: i32;
    public override fn get(self) -> i32 { return self.x + self.y; }
}

class Left {
    public a: i32;
    public fn fa(self) -> i32 { return self.a; }
}

class Right {
    public c: i32;
    public fn fc(self) -> i32 { return self.c; }
}

class Mix : Left, Right {
    public b: i32;
    public fn sum(self) -> i32 { return self.fa() + self.b + self.fc(); }
}

fn main() -> i32 {
    let c = Child { x: 3, y: 4 };
    print("3+4=${c.get()}，字段 x 仍是 ${c.x}");
    let m = Mix { a: 1, c: 2, b: 4 };
    print("1+4+2=${m.sum()}");
    return 0;
}
```

输出 `3+4=7，字段 x 仍是 3` 和 `1+4+2=7`。

### 这里的 `class B : A` 不是 C++ 的继承

**它做的是布局拼接。** `A` 的字段按声明顺序铺在前面，`B` 自己的字段接在后面，仍然是一块连续内存，赋值和传参仍然按位拷贝。所以 `Child { x: 3, y: 4 }` 可以用一个字面量把父子字段一次给全；`Mix { a: 1, c: 2, b: 4 }` 的顺序是「先 Left 整段、再 Right 整段、再 Mix 自己的字段」。

这一点决定了迁移时要改的心智模型：

- 没有虚表、没有对象头、没有隐式指针间接。**值类型的 `c.get()` 是静态确定的直接调用，零额外开销。**
- `override` 必须对准一个已有的父方法，否则被拒绝——声明必须能被验证，不能只是给读者的注释。
- 棱形继承（两个父类共享同一祖先）是硬错误：那时 `self.a` 指哪一段没有不歧义的答案。编译器选择在编译期拒绝，而不是定一条难以预料的选择规则。

### 运行时分发走 `dyn Trait`，不走继承

「一个变量装多种类型」是另一条路：

```vyx
trait Shape { fn area(self) -> f64; }

struct Circle { radius: f64; }

impl Shape for Circle {
    fn area(self) -> f64 {
        return self.radius * self.radius * 3.14159;
    }
}
```

这里的 `circle.area()` 是**静态分发**，编译期就确定实现；真正的运行时多态要显式写成 trait 对象（`dyn Shape`），那时才引入 vtable。迁移者要注意的是：**能力（trait）和布局（继承）在 Vyx 里是两套机制**，不像 C++ 那样混在同一个类层级里——「圆和方都能算面积」不会逼你造一个假的共同祖先。

---

## 错误处理

```
// Vyx                          // Rust                         // C++
error MyErr { NotFound }       // 选择或定义一个错误类型        // 无标准错误类型

fn risky() -> Result<i32, MyErr> { fn risky() -> Result<i32,E> { int risky() {
    if (bad) {                     if bad {                        if (bad)
        fail MyErr.NotFound;           return Err(E::Nf);              throw runtime_error("nf");
    }                              }                               return 42;
    return 42;                     Ok(42)                       }
}                              }

let v = risky()?;              let v = risky()?;               try { auto v = risky(); }
                                                               catch (...) { }
```

### 可运行的最小例子

```vyx
error MyErr { NotFound }

fn risky(bad: bool) -> Result<i32, MyErr> {
    if (bad) { fail MyErr.NotFound; }
    return 42;
}

fn caller(bad: bool) -> Result<i32, MyErr> {
    let v = risky(bad)?;
    return v + 1;
}

fn main() -> i32 {
    match (caller(false)) {
        case Ok(v) => { print("ok=${v}"); }
        case Err(_) => { print("err"); }
    }
    match (caller(true)) {
        case Ok(v) => { print("ok=${v}"); }
        case Err(_) => { print("err"); }
    }
    return 0;
}
```

输出 `ok=43` 和 `err`。

### `fail` 是语句，成功路径不写 `Ok`

和 Rust 的对应关系是逐条的：

| | Rust | Vyx |
|---|---|---|
| 错误类型 | `enum E { Nf }` | `error MyErr { NotFound }` |
| 返回失败 | `return Err(E::Nf)` | `fail MyErr.NotFound;` |
| 返回成功 | `Ok(42)` | `return 42;` |
| 传播 | `?` | `?` |
| 消费 | `match` | `match`（`case Ok(v)` / `case Err(_)`） |

两个最容易写错的地方：

- **`fail` 是语句，不是表达式。** 它当场结束当前函数并返回失败，所以 `if (bad) { fail ...; }` 这一支结尾不需要再写 `return`。照着 Rust 写 `return Err(...)` 会得到类型错误。
- **成功路径不需要 `Ok(...)`。** `fn -> Result<i32, MyErr>` 里的 `return 42` 会被自动装进 `Ok`。写 `return Ok(42)` 得到的是「`Result` 的 `Result`」。

### 为什么不用异常

失败出现在签名里：`-> Result<i32, MyErr>`。任何调用点都能看到这个函数可能失败，不需要猜某个函数背后会不会抛。代价同样明确：**错误必须被处理，或者继续上抛**，不能悄悄忽略——这正是目的。

C++ 的 `throw` 可以在任意深度逃逸，调用方只能靠文档和 `noexcept` 注解推测；Vyx 把这份推测变成了类型检查。判断「返回 `Result` 还是直接 `panic`」的标准和 Rust 一样：**调用方有没有可能写出正确的处理代码**——输入格式不对，调用方能选择报错或换输入，用 `Result`；内部不变量被破坏，调用方无能为力，用 `panic`。

---

## 内存管理

| | Rust | C++ | Vyx |
|---|------|-----|-----|
| 所有权 | 编译器强制 | 无（需手动） | 显式写进类型（`Ref` / `Box` / 裸指针） |
| 引用计数 | `Arc<T>` | `shared_ptr<T>` | `Ref::<T>.new(val)` / `clone()` |
| 独占 | `Box<T>` | `unique_ptr<T>` | `Box::<T>.new(val)` |
| 弱引用 | `Weak<T>` | `weak_ptr<T>` | `Weak::<T>.of(strong)`（`std.ref`） |
| RAII | `Drop` trait | 析构函数 | `fn drop()` 自动调用 |
| defer | 无 | 无（RAII 替代） | `defer { cleanup(); }` |

### 先说清没有的东西

**Vyx 没有借用检查器。** 从 Rust 过来的人最容易先找这个：没有别名规则在编译期替你兜底，没有 "borrowed value does not live long enough"。所有权在这里是**写在类型里的选择**，不是全程序推导出来的结论。

**Vyx 也没有 GC，没有隐式析构。** 从 C++ / Java 过来的人最容易先找这个：`class` 实例是按位拷贝的普通值，没有「析构时机」；容器的堆缓冲不会有人替你释放。

这两件事合起来决定了迁移时的动作：**先想清楚「这块堆内存归谁」，然后把它写出来。**

### 三种归属

```text
Box   房间钥匙只有一把。搬走就不再属于你。
Ref   门卡可以复印。大家指同一间房；最后一张卡收回去，房子才拆。
Weak  一张「过期作废」的复印件。upgrade() 时房子可能已经没了。
```

```vyx
use std.ref;

fn main() -> i32 {
    let shared = Ref::<i32>.new(42);
    let copy = shared.clone();
    copy.set(100);
    print("两张卡看见同一个数：${shared.deref()}");

    let weak = Weak::<i32>.of(shared);
    match (weak.upgrade()) {
        case Some(strong) => { print("房子还在：${strong.deref()}"); }
        case None => { print("房子没了"); }
    }

    let unique = Box::<i32>.new(7);
    print("独占盒里：${unique.deref()}");
    return 0;
}
```

输出 `两张卡看见同一个数：100`、`房子还在：100`、`独占盒里：7`。

最需要调整预期的是 `clone()`：**它是「再加一张门卡」，不是「复制一份值」**。所以 `copy.set(100)` 之后 `shared.deref()` 也是 `100`——这不是巧合，而是这条规则的直接结果。C++ 的 `shared_ptr` 拷贝同样只加计数，但拷过去的新名字不会让你觉得「能改到原对象」；`Ref<T>` 的 `clone()` 是可见的、要显式写的动作。

`Weak` 的存在理由和 Rust 一样：两个对象互持 `Ref` 会让计数永不归零，必须有一边持 `Weak` 来断开环。

### `defer`：把「什么时候释放」和「释放什么」分开

`defer` 登记一块语句，挂在**作用域退出**这个事件上，不挂在某条语句后面。所以函数从哪个出口离开都会执行，清理逻辑不必在每个 `return` 前面复制一遍。

**顺序是后进先出（LIFO）**：

```vyx
fn main() -> i32 {
    defer { print("first registered"); }
    defer { print("second registered"); }
    print("body");
    return 0;
}
```

输出 `body`、`second registered`、`first registered`。

这不是随便定的：资源天然是「后来的依赖先来的」。你先打开文件、再在它上面加锁，就必须先放锁、后关文件。按申请顺序写下去，「释放顺序 = 申请顺序的镜像」自动成立。

和 C++ 的 RAII 相比，`defer` 的差异是**它不拥有资源**——只决定**什么时候**调清理函数，调什么仍然由你写。所以「必须显式 `destroy()`」的容器 API，配 `defer` 才是完整用法：

```vyx
let mut values = Vec::<i32>.new();
defer { values.destroy(); }
// ... 正常路径和提前 return 都会走到这里
```

### 与裸指针的边界

**只要类型里出现了 `*` 或 `rawptr`，所有权就不再替你兜底。** 从 C/C++ 过来的人对这一点会感到熟悉；从 Rust 过来的人需要把它当成一条「必须自己记住的安全边界」。

---

## 泛型

### 内建运算符不需要约束

```
// Vyx                                      // Rust
fn max_of<T>(a: T, b: T) -> T               fn max_of<T: PartialOrd>(a: T, b: T) -> T
{                                            {
    if (a > b) { return a; }                     if a > b { a } else { b }
    return b;                                }
}
```

上面这段 Vyx 直接编译：

```vyx
fn max_of<T>(a: T, b: T) -> T {
    if (a > b) { return a; }
    return b;
}

fn main() -> i32 {
    print("${max_of(3, 9)}");
    return 0;
}
```

输出 `9`。

Rust 的 `T: PartialOrd` 是**必需的**：没有它，`a > b` 里的 `>` 解析不到 trait 实现，编译不过。Vyx 当前对**内建运算符做隐式实例化**——`+`、`>` 这类运算符在单态化时按实际类型展开，所以无约束泛型也能写。

这不是「Vyx 比 Rust 更强」，而是**取舍位置不同**：Rust 把接口完备性放在泛型定义处检查（调用方看到的签名就是全部前提），Vyx 把它推到实例化点（写起来短，但错误出现在单态化时）。迁移时知道这个差异，才不会在泛型报错里找错地方。

### `where` 什么时候真的必需

需要**关联常量或关联类型**这类「只有 trait 才知道」的能力时，`where` 就不是可选的：

```vyx
trait Bounded {
    const MAX: i32;
}

class U8Like {
    public v: i32;
}

impl Bounded for U8Like {
    const MAX: i32 = 255;
}

fn clamp_max<T>(v: i32) -> i32 where T: Bounded {
    if (v > T::MAX) { return T::MAX; }
    return v;
}

fn main() -> i32 {
    print("clamped=${clamp_max::<U8Like>(300)}");
    return 0;
}
```

输出 `clamped=255`。

`T::MAX` 的值只能来自约束，编译器无从推断。所以在 Vyx 里：

- 只用内建运算符 ⇒ 不写约束也能编译。**但写上更好**：前提留在签名里，读者和编译器都少猜一次。
- 要用 `T::MAX` / 关联类型 / trait 方法 ⇒ `where` 必需。

注意调用点的 `clamp_max::<U8Like>(300)`：和 `Vec::<i32>.new()` 一样，**类型参数可以显式给出**。C++ 的 `requires std::totally_ordered<T>` 对应的是 Rust 那一路：约束写在签名里，实例化前就检查。

## 容器

| | Rust | C++ | Vyx |
|---|------|-----|-----|
| 动态数组 | `Vec<T>` | `vector<T>` | `Vec::<T>.new()`（先 `use std.collections;`） |
| 哈希表 | `HashMap<K,V>` | `unordered_map<K,V>` | `Dict::<K, V>.new()` |
| 集合 | `HashSet<T>` | `unordered_set<T>` | `Set::<T>.new()` |
| 插入 | `v.push(x)` | `v.push_back(x)` | `v.push(x)` |
| 键值写入 | `m.insert(k, v)` | `m[k] = v` | `m.put(k, v)` |
| 索引 | `v[i]` | `v[i]` | `v[i]`，或 `v.get(i)` |
| 长度 | `v.len()` | `v.size()` | `v.count()`，或字段 `v.len` |

```vyx
use std.collections;

fn main() -> i32 {
    let mut v = Vec::<i32>.new();
    defer { v.destroy(); }
    v.push(3);
    v.push(9);
    print("count=${v.count()} first=${v.get(0)}");

    let mut m = Dict::<string, i32>.new();
    defer { m.destroy(); }
    m.put("a", 1);
    let has = m.contains("a");
    let value = m.get("a");
    print("has=${has} value=${value}");

    let mut s = Set::<i32>.new();
    defer { s.destroy(); }
    s.add(7);
    s.add(7);
    print("set size=${s.len}");
    return 0;
}
```

输出 `count=2 first=3`、`has=true value=1`、`set size=1`。

### 为什么名字和 Rust 不一样

`Vec::<T>.new()` 里的 `::<T>` 是**类型参数的显式给法**，因为 `new()` 没有任何参数能推断出 `T`——Rust 靠返回类型的位置推断，Vyx 要求你在这里直接写出来。同理，`Set` 用 `add`（不是 `insert`），`Dict` 用 `put`（`set` 是别名）。这类改名会在迁移时撞出编译错误，但都只是「改一次就好」，不影响代码结构。

### 三个会影响正确性的边界

**① 容器不会自动释放。** 没有 GC，也没有隐式析构，堆缓冲必须显式还回去——`v.destroy()`，或者 `defer { v.destroy(); }`。这是从 Rust / C++ 迁移到 Vyx 时**最容易漏的一步**。

**② 长度有方法也有字段，但都是元素个数。** `Vec` 上 `count()` / `size()` / `length()` 是同义方法，字段 `len` 是同一份数据；`Set` 只有字段 `len`。**它们都不是容量**——容量是 `capacity()`。Rust 的 `len()` 同样不指容量，但字段与方法混用是 Vyx 特有的，容易在 `Set` 上写出 `s.count()` 拿到「没有这个函数」的错误。

**③ 索引类型由容器的元素类型推断。** `Vec<T>` 的 `v[i]` 返回元素引用；需要值副本时使用 `get(i)` 或声明目标类型。

```vyx
let a: i32 = v[0];    // 读取 i32 值
v[0] = 99;            // 修改元素
let b = v.get(0);     // 读取值
let c = v[0];         // 推断为元素引用
print("c=${c}");     // 打印元素值
```

直接打印和插值打印都会读取引用指向的元素；容器的字符串元素按实际长度输出。

### 迭代

`Vec` / `Set` / `Dict` 都实现 `Iterable`，`for (x in container) { ... }` 直接可用。

---

## 并发

```vyx
use std.sync;
use std.vio;

@[async]
fn answer() -> i32 { vio_sleep(1); return 42; }

fn main() -> i32 {
    vio_start();
    let value = await answer();
    let mtx = Mutex::<i32>.new(value);
    mtx.lock();
    mtx.set(42);
    mtx.unlock();
    let result = mtx.get();
    mtx.destroy();
    vio_stop();
    print("result=${result}");
    return result - 42;
}
```

输出 `result=42`，进程退出码 0。

### `@[async]` 与 `await` 是语言特性，不是库

**异步是属性加关键字**，不是 `Future` + `.await` 的方法链。`@[async] fn` 标出可挂起的函数，`await answer()` 在调用点挂起当前任务。

好处是**不需要写运行时胶水**：没有 `async fn` 返回 `impl Future` 的类型体操，没有 executor 要挑，也没有 `Pin` 出现在错误信息里。代价是**控制粒度更粗**：你通过 `@[async]` 决定哪个函数可以挂起，而不是通过 `.await` 决定挂在哪一步。

`vio_start()` / `vio_stop()` 是任务运行时的起止边界，异步工作在这两者之间执行。

### 共享可变状态用 `Mutex<T>`，不是 `Ref<T>`

`Mutex::<T>.new(v)` 把值和锁装在一起，`lock()` / `unlock()` 围着临界区，`get()` / `set()` 读写。注意 **`Ref<T>` 是单线程引用计数，不是锁**——拿它当并发共享是经典的并发 bug 来源。

不想手写加解锁时，`std.sync` 还提供 `RwLock`、`Condvar`、`Barrier`、`Once`、`Channel<T>`，以及无锁的原子量：

```vyx
use std.sync;

fn main() -> i32 {
    let counter = atomic_new(0);
    atomic_add(counter, 3);
    print("now=${atomic_load(counter)}");
    atomic_destroy(counter);
    return 0;
}
```

输出 `now=3`。

### 和 Rust / C++ 的心理落差

Rust 的所有权系统让「跨线程共享」必须过 `Send` / `Sync` 两道编译期检查。Vyx 没有这道闸门——**共享是否安全由你选的类型保证**（`Mutex<T>` 安全，`Ref<T>` 不安全）。C++ 的 `std::thread` 同样把责任交给程序员，但类型系统里没有任何提示；Vyx 的提示是「你手里拿的是 `Mutex` 还是 `Ref`」。

---

## C 互操作

```vyx
extern "C" {
    fn abs(x: i32) -> i32;
}

fn main() -> i32 {
    print("${abs(-7)}");
    return 0;
}
```

输出 `7`。

### `cfn`：为什么 C 回调要单独一个类型

```vyx
extern "C" {
    fn call_cb(cb: cfn(i32) -> i32, x: i32) -> i32;
}
```

**Vyx 的普通 `fn` 值是「脂肪指针」（函数指针 + 捕获环境），C 函数指针是一个裸地址，两者调用约定不同，不能互相顶替。** 把 `fn` 塞进 `cfn` 槽位会被拒绝（`E1000`），转换必须显式写出来。

这条规则看起来只是「多写一个关键字」，实际防止的是最隐蔽的一类崩溃：C 侧按一个地址调用，Vyx 侧传过去的是两个机器字——多出来的那个字被当成第一个参数，或者反过来读到垃圾。**把差异写进类型，就等于让编译器替你抓住它。**

### `extern "dci"`：给 C++ / Rust 用的边界

`extern "C"` 覆盖面窄：POD 布局、C 调用约定、无生命周期。要用到 **C++ / Rust 的原生布局、生命周期与派发**时，走 `extern "dci"` + 生产端契约，由 Active Adapter 在构建期请求生产端实例化受支持的泛型。

契约模型见 [DCI 规范](DCI_SPEC_ZH.md)，原始定义、声明与构建配置见 [MOSP 示例](MOSP_ZH.md#dci生产端事实与开放泛型)。

## 构建系统

| | Rust | C++ | Vyx |
|---|------|-----|-----|
| 配置 | `Cargo.toml` | `CMakeLists.txt` | `Vyx.toml` |
| 新项目 | `cargo new` | 手动 | `vyxc new <dir>` |
| 构建 | `cargo build` | `cmake --build` | `vyxc build --target <name>` |
| 单文件运行 | `cargo run` | 手动 | `vyxc --src=file app.vyx --run=aot` |
| 测试 | `cargo test` | `ctest` | `vyxc test` |
| 依赖 | crates.io | vcpkg / conan | `Vyx.toml` + 本地 `file://` registry（`vyxc install` / `publish` / `search` / `lock`） |

### `vyxc new` 生成什么

```text
proj1/Vyx.toml
proj1/src/main.vyx
```

```toml
[package]
name = "proj1"
version = "0.1.0"
entry = "src/main.vyx"

[dependencies]
core = { path = "std:core", target = "std_core" }
collections = { path = "std:collections", target = "std_collections" }

[target.proj1]
type = "executable"
entry = "src/main.vyx"
auto_sources = false
```

`[target.<name>]` 是和 Cargo / CMake 最不一样的地方：**一个包可以同时产出多个目标**（可执行文件、静态库、动态库），每个目标自己声明源文件集合与类型。`vyxc build` 构建包里全部目标，`--target <name>` 只构建其中一个。多产物项目（库 + 示例 + 测试二进制）因此不需要拆成多个包。

### 依赖解析走本地 registry

`install` / `publish` / `search` / `lock` 对接的是**本地 `file://` registry**，不是网络上的公共索引。所以在内网或离线环境里工具链行为完全一致——这对「把 C++ 的 vcpkg 依赖迁过来」很重要：可以先把对应包放进本地 registry，再按名字引用。`lock` 写出 `Vyx.lock` 固定版本，定位和 `Cargo.lock` 相同。

### 排查顺序

迁移一个模块时，建议按这个顺序把问题分层：

```text
vyxc --src=file app.vyx --stop-after-parse   # 语法（秒级）
vyxc --src=file app.vyx --stop-after-sema    # 类型与语义
vyxc --src=file app.vyx --run=aot            # 端到端
```

前两步把语法错误、类型错误和后端错误分开，避免在同一个报错屏幕上混着看。

---

## 一句话总结

> 把它当作一份迁移地图，而不是 Rust、C++ 的替代性承诺。

迁移时要主动换掉的四个习惯：

- **可变性**：`let mut` / `const` → `let` / `var`。
- **失败**：`throw` / `Err(...)` → `fail` + `?`（成功路径不写 `Ok`）。
- **归属**：`shared_ptr` / `Arc` / GC → 显式的 `Ref` / `Box` / `destroy()` / `defer`。
- **方法挂载**：`fn Type.method` → `impl Type { fn ... }` 或写进 `class` 体内。

所有权、ABI 和工具链边界以当前编译器的构建与对应验证为准。
