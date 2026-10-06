# Vyx 高级特性

[English: Advanced features](ADVANCED_FEATURES.md) · [文档目录](README.md)

## 关于这份教程

入门课教你写出能跑的程序，进阶课教你组织数据和处理失败。这一章处理的是另一类问题：**当你必须自己承担更多责任时，语言给了哪些工具，各自的边界在哪里**。

具体包括：堆对象的归属、异步、trait 与运行时多态、unsafe 与 C、`drop`、更复杂的泛型、编译期能力、继承、平台门控、布局查询，以及 Migrate、DCI、反射这几个元信息子系统。

示例都用 AOT 执行。Migrate、Reflection、DCI、DCE、Effect 的当前用法统一收在 [事实语义所有权系统](MOSP_ZH.md)，Effect 与自定义属性规则见 [Effect 模型规范](MOSP_EFFECT_ZH.md)，语言规则见[语言设计](设计文档_ZH.md)。

这一章有 28 节，主线可以按组来读：

```text
第 21–24 课   堆上的所有权、容器、闭包捕获、derive
第 25 课      异步：等的时候别把别人卡住
第 26–29 课   枚举、trait、dyn、管道
第 30–32 课   unsafe / C、drop、更复杂的泛型
第 33–38 课   comptime、move、继承、interface、别名、platform
第 39–43 课   布局查询、运算符、版本、DCI、反射
```

---

## 先建立一张安全地图

高级特性最容易混淆的一点，是**三种完全不同的边界被当成同一种东西**。先把它们分开，后面每一课都只是在这张地图上找一个位置。

```text
普通值、struct、Vec     作用域结束就该清理；defer / drop 帮你登记
Box / Ref / Weak        堆对象：独占、共享、或「看看还在不在」
unsafe、rawptr、FFI     你自己证明：非空、对齐、没人抢着放
```

对应到具体手段：

- **跨线程**改同一份数据，用 `Mutex<T>`。
- **跨 C** 用 `extern "C"`；**跨其它编译型语言**用 DCI 契约。
- **不要让 `Ref` 去当锁**。它管的是「什么时候拆房子」，不是「谁能同时进房间」。这是本机最容易踩的坑。

`&T` / `&mut T` 是借用。借用规则就两条：共享借用还活着时再取 `&mut T`，或搬走仍被借的局部，都是 E3101；堆所有权的赋值默认是 move（E3100），除非类型是 `Copy`。

成员访问跟拥有值一样写，不必先解引用——编译器替你处理：

```vyx fragment
fn bump(values: &mut Vec<i32>) {
    values[0] = values[0] + 1;
    values.push(9);
}
```

显式写 `*T` 时用 `->` 读成员，而且不会搬走所有权。`rawptr` 是无类型 unsafe 指针，上面这些规则它一概不享受。

```vyx fragment
class Pair { public value: i32; }
fn read_pair(p: *Pair) -> i32 { return p->value; }
```

记住这条分界线：**只要指针类型里出现了 `*` 或 `rawptr`，所有权检查就不再替你兜底**。

---

## 第21课：资源管理——Ref、Weak 与 Box

先看问题。堆上的对象由谁负责销毁？C++ 把这个决定交给文档和约定，Java 交给 GC，Python 交给引用计数。Vyx 的做法是把「归属方式」写进类型，让你在声明处就选好：

```text
Box   房间钥匙只有一把。搬走就不再属于你。
Ref   门卡可以复印。大家都指同一间房；最后一张卡收回去，房子才拆。
Weak  一张「过期作废」的复印件。upgrade() 时房子可能已经没了。
```

```vyx program
use std.ref;

fn main() -> i32 {
    let shared = Ref::<i32>.new(42);
    let copy = shared.clone();
    copy.set(100);
    print("两张卡看见同一个数：${shared.deref()}，强引用大约 ${shared.count()}");

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

### 为什么 `clone()` 是「加一张卡」而不是「复制一份值」

如果 `clone()` 真的复制出一份独立的 `i32`，那 `copy.set(100)` 就改不到 `shared`，`Ref` 也就没有存在意义了。**共享的定义就是：多条路径指向同一份数据，从任何一条路径写入，其它路径都看得见。**

理解这一点之后，`shared.deref()` 打印出 `100` 就不再是巧合，而是这条规则的直接结果。

### 为什么需要 `Weak`

`Ref` 有一个无法回避的问题：如果两个对象互相持有对方的 `Ref`，两边的计数都永远不会归零，谁也不会被拆掉——这就是循环引用。`Weak` 是为此准备的：它是**观察者**，不延长寿命，需要时通过 `upgrade()` 试着升级成强引用，拿到 `Option<Ref<T>>`，不成功就说明对象已经没了。

### 代价与边界

- `Ref` / `Weak` 是**单线程**引用计数，不是锁。要跨线程改同一份数据，用 `Mutex<T>`；用 `Ref` 代替锁是并发 bug 的经典来源。
- 引用计数本身有开销：每次拷贝要加一，每次销毁要减一。
- 忘记用 `Weak` 打散循环，泄漏仍然会发生——引用计数解决的是「谁来拆」，不是「能不能拆」。

### 小结

`Box` 独占，`Ref` 共享，`Weak` 可能升级失败。三者都是关于**归属**的，都不关于**并发**。

---

## 第22课：泛型容器与迭代

容器要能当参数传进函数，也要能被循环走一遍。

```vyx program
use std.collections;

fn sum(values: Vec<i32>) -> i32 {
    defer { values.destroy(); }
    var total = 0;
    for (value in values) {
        total = total + value;
    }
    return total;
}

fn main() -> i32 {
    var values = Vec::<i32>.new();
    values.push(10);
    values.push(32);
    print("10+32=${sum(values)}");
    return 0;
}
```

### 遍历期间不要改结构

这条纪律值得单独说：**`for (value in values)` 还在走的时候，不要 `push` / `pop` / `set` / `clear` / `destroy`。**

原因是迭代器记着「走到哪了」和「底层存储在哪」。`push` 触发的重新分配会让之前记住的位置失效，迭代器继续按旧位置读，行为就不再是你能依赖的了。

这和 C++ 里「边遍历边改容器」的坑是同一个，只是这里把它写成了一条明确的纪律：要改就先收集，改完再遍历。

### 按值传参会转移所有权

`Vec<i32>` 是拥有缓冲的类型。`sum(values)` 把所有权交给形参，调用后不能继续使用原绑定；这不是“编译器发现没有保存它，所以省掉一次复制”的优化。本例由 `sum` 用 `defer` 释放收到的缓冲。如果调用方还要保留容器，应改用借用参数或显式克隆，借用与移动见第 34 课。

只读取容器时，形参用 `&Vec<T>`。`for-in` 通过借用访问原容器，函数返回后调用方可以继续使用它：

```vyx program
use std.collections;

fn sum(values: &Vec<i32>) -> i32 {
    var total = 0;
    for (value in values) { total += value; }
    return total;
}

fn main() -> i32 {
    var values = Vec::<i32>.new();
    values.push(10);
    values.push(32);
    print(sum(&values));  // 42
    print(values.count());  // 2，容器仍由调用方持有
    return 0;
}
```

### 小结

把 `Vec` 传进函数、用 `for-in` 累加；**遍历期间别拆袋子**。

---

## 第23课：闭包捕获与高阶调用

第 12 课介绍了竖线形式的闭包。这里补上另一种写法，它把「依赖哪些外部名字」摆到明面上：

```vyx program
fn main() -> i32 {
    let plus7 = |x: i32| { return x + 7; };
    print("35+7=${plus7(35)}");

    let offset = 5;
    let add_offset = [offset](x: i32) => x + offset;
    print("37+5=${add_offset(37)}");
    return 0;
}
```

### 为什么要有显式的捕获列表

`[offset](x: i32) => x + offset` 把捕获的东西写在方括号里。好处是读代码的人不必往函数体里翻，就能看到**这个闭包依赖外层哪些变量**。

闭包越是被传来传去，这一点越重要：调用点看到的是 `add_offset`，看不到它背后还拴着 `offset`。捕获列表把这个隐式依赖显式化了。

### 两种写法怎么选

| 写法 | 适合 |
| --- | --- |
| `\|参数\| { 语句 }` | 函数体有多条语句，或需要 `return` |
| `[捕获](参数) => 表达式` | 单个表达式，想把捕获面写清楚 |

闭包可以像普通函数值一样当参数传给别的函数，这两条路都通。

### 小结

当场写小函数；要带外层的值，写进 `[…]`。

---

## 第24课：`derive` 生成方法

相等、克隆、哈希、排序、打印——这些方法的实现完全由字段决定，却必须手写一遍。更麻烦的是**它们之间要保持一致**：加了字段却忘了更新 `operator_eq`，就会得到一个"看起来相等、实际不等"的类型。

`@[derive(...)]` 把这件事交给编译器：

```vyx program
use std.hash;
use std.clone;
use std.fmt;

@[derive(Eq, Clone, Hashable, Ord, Debug, Display)]
class Pair {
    public x: i64;
    public y: i64;
}

fn main() -> i32 {
    let a = Pair { x: 7, y: 9 };
    let b = a.clone();
    print("相等？${a.operator_eq(b)}");
    print("hash=${a.hash()} compare=${a.compare(b)}");
    return 0;
}
```

你会看到相等为真、一个非零哈希、比较结果 0。

### `Clone` 按字段的实现克隆

`@[derive(Clone)]` 组合各字段的克隆行为。`Vec<T>.clone()` 复制缓冲和元素；`Ref<T>.clone()` 增加强引用计数，与原值共享同一对象。不能把所有 `clone()` 都理解成深拷贝。

克隆后的共享关系由字段类型决定。设计自己的资源类型时，应明确克隆是复制资源还是增加共享引用，而不是只复制一个未经管理的指针。

enum 按变体 `match` 重建；泛型字段视为隐含对应 trait bound。

### 为什么指针字段会被拒绝

`@[derive(Clone)]` 遇到含 `rawptr` / `*T` 字段的类型会直接拒绝。原因很直接：**编译器无法证明复制这个指针意味着什么**——是共用同一个目标，还是需要深拷贝目标？没有正确答案，所以要求你手写 `clone()`，把语义说清楚。

### 什么时候不该用 derive

derive 给的是**结构相等**：字段全等则相等。如果业务上「同一个 id 就是同一个对象」，而其它字段允许不同，那就必须手写——derive 生成的版本会得出错误的结论。

`@[derive(Copy)]` 还有一个附加作用：对纯标量 class 抑制赋值时的 E3100（见第 34 课）。

### 小结

derive 按字段合成；克隆是否共享由字段的实现决定；裸指针字段需要手写。

---

## 第25课：async、Task、Promise 与 VIO

到现在为止，函数都是一条道走到黑。写 `vio_sleep(1000)`，整台程序一起睡一秒——没有别人能往前跑。

想要的是另一种等法：让**当前这个任务**歇着，队列里别的任务继续走。

```text
时间 →

slow:  [叫起来]====睡眠====[醒来写 1]
fast:  [叫起来][立刻写 2]

先启动两个，再 await ──► log 变成 21（先 2 后 1）
先 await slow 再叫 fast ──► log 变成 12（根本没人可插队）
```

```vyx program
use std.vio;

var log: i32 = 0;

@[async]
fn slow() -> i32 {
    await Task::<i32>.sleep(20);
    log = log * 10 + 1;
    print("slow 醒了");
    return 0;
}

@[async]
fn fast() -> i32 {
    log = log * 10 + 2;
    print("fast 做完了");
    return 0;
}

fn main() -> i32 {
    let a = slow();
    let b = fast();
    await a;
    await b;
    print("log=${log}");
    return 0;
}
```

先看到 `fast 做完了`，再看到 `slow 醒了`，最后 `log=21`。

### 为什么先启动再 `await` 才会交错

关键在时序，不在语法：

1. `@[async]` 的函数，**调用时不会在调用点执行**，而是立刻得到一个任务（future）。
2. `let a = slow(); let b = fast();` 把两个任务都排进队列。
3. `await a` 才开始等 slow 结束。slow 在 `Task.sleep` 上歇着的时候，队列里的 fast 就能跑完。

所以把 `main` 改成 `await slow(); await fast();`，`log` 会变成 `12`——fast 在 slow 结束前根本没入队，没有人可以插队。**这不是执行器的行为差异，是你自己把并发窗口关掉了。**

这也解释了 `@[async] fn f() -> i32` 那个看起来别扭的地方：签名写的是 `i32`，调用处拿到的却是任务；`await` 之后才是 `i32`。

```vyx program
use std.vio;

@[async]
fn delayed_add(a: i32, b: i32) -> i32 {
    await Task::<i32>.sleep(1);
    return a + b;
}

fn main() -> i32 {
    let sum = await delayed_add(20, 22);
    print("sum=${sum}");
    return 0;
}
```

参数与局部变量在 `await` 前的修改会保留到恢复执行之后。可变参数写作
`mut value: i32`。异步函数也可以返回 `string` 或聚合类型，调用方通过
`await` 获得声明的返回值。

### 让出时间：三种等法的区别

| 你想做的事 | 写法 | 别人还能跑吗 |
| --- | --- | --- |
| 等一会儿但不卡住调度器 | `await Task::<i32>.sleep(ms)` | 能 |
| 只让出当前这一拍 | `await Task::<i32>.yield_now()` | 能 |
| 线程整段睡死 | `vio_sleep(ms)` | 不能 |

这张表值得记住：**在有任务的环境里用 `vio_sleep`，等于把整个执行器一起按停**。

`Task::<i32>.ready(2)` 是已经算完的任务；`Promise::<i32>.failed(7001)` 是已经失败的结果，先问 `is_done`、`is_failed`、`error_code`。`poll()` 只回答「现在好了没有」，不泵队列。

`main` 不必写成 `@[async]`——顶层由执行器负责推进。

### 另一条老路：`StartCoroutine`

`StartCoroutine` + `yield WaitForNextTick()` 用的是操作系统 fiber，和上面这套 `@[async]` 是两回事。**不要在同一个函数里混用**。只有这条路需要 `vio_start()` / `vio_stop()`。

```vyx program
use std.vio;

var steps: i32 = 0;

fn worker(ctx: rawptr) -> i32 {
    steps = 1;
    yield WaitForNextTick();
    steps = steps + 1;
    return 0;
}

fn main() -> i32 {
    vio_start();
    let handle = StartCoroutineHandle(worker as i64);
    if (!handle.is_valid()) {
        vio_stop();
        print("句柄无效");
        return 1;
    }
    StopCoroutineHandle(handle);
    vio_stop();
    print("steps=${steps}");
    return 0;
}
```

选它的理由只有一个：需要**切换栈**的语义。代价是可移植性——Android 的 Bionic 没有 `getcontext` / `swapcontext`，`StartCoroutine` 在那里切不了 fiber 栈（退化成 no-op）。`@[async]` 是无栈泵，不依赖 fiber，所以没有这个问题。

### 实现层面

编译器在 `@[async]` 的 `await` 处做 rustc 式 **MIR 拆分**：一次 `poll()`，没就绪就 `mir_term_yield`，回来再 poll。执行器是无栈泵——Pending 时兄弟任务能跑完。`Task.sleep` / `Task.yield_now()` 是定时 Future。

### 小结

先启动再 `await` 才会交错；让出时间用 `Task.sleep`，不要用 `vio_sleep`；`StartCoroutine` 是另一套机制。

---

## 第26课：枚举状态建模

有限几种形状的数据，用「整数 + 约定」表达是最容易写错的：约定写在文档里，编译器看不见，漏掉一种情况也不会有人拦你。

```text
Signal::Stop          没有额外数据
Signal::Go(42)        这一支带着一个 i32
```

```vyx program
enum Signal {
    Stop,
    Go(i32)
}

fn describe(signal: Signal) -> i32 {
    match (signal) {
        case Stop => { return 0; }
        case Go(value) => { return value; }
        default => { return -1; }
    }
}

fn main() -> i32 {
    print("Go(42) → ${describe(Signal::Go(42))}");
    print("Stop → ${describe(Signal::Stop)}");
    return 0;
}
```

### 为什么带载荷的变体更好

`Go(i32)` 把数据**装在分支里面**。这带来的不是语法糖，而是不可能表达错误状态：不存在「说是 `Stop` 却还带着一个数」的对象，也不存在「从 `Stop` 里取出了数值」的代码——编译器根本不允许那么写。

用整数 tag 加一个平行字段就没有这层保护：tag 和字段不同步是随时可能发生的。

### 为什么仍然要写 `default`

当前 `match` 需要兜底分支（第 14 课讲过，裸 `_` 还不支持），所以 `describe` 里加了 `default`。它的作用不是语义需要，而是满足语言的当前要求。

业务代码请优先用带载荷变体和命名模式，**不要依赖变体的声明顺序**——顺序是实现的细节，不是接口的一部分。

### 小结

变体用 `Type::Variant(value)` 构造，用 `case Variant(value)` 拆开。数据装在分支里，错误状态就写不出来。

---

## 第27课：trait 与 impl

「能算面积」是一种能力，圆和正方形都可以有。可是它们没有共同的父类。用继承来表达这种关系，会逼你造一个并不存在的祖先。

`trait` 直接描述能力，`impl Trait for Type` 为某个类型补上这份答卷：

```vyx program
trait Shape { fn area(self) -> f64; }

struct Circle { radius: f64; }

impl Shape for Circle {
    fn area(self) -> f64 {
        return self.radius * self.radius * 3.14159;
    }
}

fn main() -> i32 {
    let circle = Circle { radius: 2.0 };
    print("半径 2 的圆面积约 ${circle.area()}");
    return 0;
}
```

你会看到大约 12.57。

### 为什么 trait 比继承更合适

`Circle` 和 `Square` 之间没有「是一种」的关系，只有「都能算面积」。用继承表达能力，代价是引入一个假的类型层级：真正的父子关系（比如「按钮是一种控件」）和纯粹的能力共享（「都能算面积」）被混在同一套机制里，改起来互相牵扯。

trait 把这两件事分开：**继承管布局和数据，trait 管能力**。一个类型可以实现任意多个 trait，不增加任何布局开销；但只能有一个父类（第 35 课）。

### 现在的形态

这里还是静态分发：`circle.area()` 在编译期就确定调用哪个实现，等于直接调用，没有额外开销。下一课讲袋子里装的是圆还是方，编译器也说不清时怎么办。

### 小结

trait 是能力清单；impl 是某类型的答卷。

---

## 第28课：动态 trait 对象

上一课的前提是「编译期知道类型」。当一袋子里既有圆又有正方形时，这个前提就不成立了，需要 `dyn Shape`：运行时再找 `area`。

```vyx program
use std.collections;

trait Shape { fn area(self) -> f64; }

struct Circle { radius: f64; }
struct Square { side: f64; }

impl Shape for Circle {
    fn area(self) -> f64 { return self.radius * self.radius * 3.14159; }
}
impl Shape for Square {
    fn area(self) -> f64 { return self.side * self.side; }
}

fn main() -> i32 {
    var shapes = Vec::<dyn Shape>.new();
    shapes.push(Circle { radius: 2.0 });
    shapes.push(Square { side: 3.0 });

    var total: f64 = 0.0;
    var index: i64 = 0;
    while (index < shapes.count()) {
        total = total + shapes.get(index).area();
        index = index + 1;
    }
    print("一圆一方，面积大约 ${total}");
    return 0;
}
```

大约 21.57。

### 为什么需要 `dyn`

`Vec<Circle>` 只能装圆。要在同一个容器里装不同实现，元素类型必须退化成「某个实现了 `Shape` 的东西」——这就是 `dyn Shape` 表达的意思。此时每个元素携带的不只是数据，还有「该调哪个 `area`」，所以调用走运行时分发。

### 代价

| 维度 | 静态（`Circle`） | 动态（`dyn Shape`） |
| --- | --- | --- |
| 调用 | 直接调用，可内联 | 间接调用，通常无法内联 |
| 元素大小 | 就是数据本身 | 数据 + 类型信息 |
| 何时确定 | 编译期 | 运行时 |

结论是**先试静态**。只有在类型确实编译期不可知时才引入 `dyn`。

### 一个容易踩的细节

`Vec.count()` 是元素个数。`Vec.size()` 是同义别名，**不是容量**；容量要用 `capacity()`。名字相近但含义不同，这里值得多看一眼。

### 小结

`dyn Trait` 把不同实现装进同一容器，调用走运行时分发。

---

## 第29课：管道操作符

`|>` 把左边的值塞进右边调用的**第一个**参数。适合把一串「只吃一个值」的变换按阅读顺序排下来。

```vyx program
fn double(value: i64) -> i64 { return value * 2; }
fn add_ten(value: i64) -> i64 { return value + 10; }

fn main() -> i32 {
    let result = 16 |> double() |> add_ten();
    print("16 加倍再加十：${result}");
    return 0;
}
```

你会看到 `42`。

### 为什么值得写

`16 |> double() |> add_ten()` 和 `add_ten(double(16))` 的语义完全相同，区别只在阅读顺序：管道从**左到右**按数据流动的方向读，嵌套调用从**内到外**倒着读。

数据变换链越长，这个差别越明显。三四个步骤之后，嵌套调用的括号已经很难数清，而管道仍然是一行一步。

### 代价

参数被隐式固定为第一个位置。如果函数的主要参数不在第一位，管道就会读得很别扭——这时候老老实实写嵌套调用更好。

### 小结

管道只是换一种阅读顺序，语义仍是嵌套调用。

---

## 第30课：unsafe 与 C FFI

语言能直接对接的仍然是 C。这一课把和 C 打交道需要的工具集中起来：

- `extern "C"` 声明函数
- `cfn(...)` 表示 C 函数指针
- `@[repr(C)]` / `packed` / `align(N)` 控制布局
- `@[no_mangle]` / `@[link_name]` / `@[export_name]` / `@[link(name=)]` 控制符号

比 C 更富的编译型语言互操作走[第 42 课](#第42课)的 DCI，不是再写一种 `extern`。

```vyx linked
@[repr(C)]
class Point {
    public x: i32;
    public y: i32;
}

extern "C" {
    fn point_sum(p: Point) -> i32;
    fn call_cb(cb: cfn(i32) -> i32, x: i32) -> i32;
}

@[no_mangle]
public fn bump_c(x: i32) -> i32 { return x + 1; }

fn main() -> i32 {
    print("20+22=${point_sum(Point { x: 20, y: 22 })}");
    print("回调 +1：${call_cb(bump_c, 41)}");
    return 0;
}
```

这两步需要链接提供 `point_sum` / `call_cb` 的 C 侧实现。

### 为什么 `@[repr(C)]` 不能省

把 `Point` 按值传给 C 函数时，两边必须约定相同的布局与 ABI。`@[repr(C)]` 声明按 C 的规则排布；仅仅把字段拼写成相同的类型，不能替代这个边界约定。

这类错误编译器不会报，因为从 C ABI 的角度看签名完全合法。这正是 unsafe 边界的典型形态：**正确性由你证明，不由编译器证明**。

### C 函数指针与函数值

`cfn(i32) -> i32` 描述 C 函数指针；`fn(...)` 类型的函数值可以携带上下文，不能把这种值直接当作 C 函数指针。上例传入的是具名函数 `bump_c`，并用 `@[no_mangle]` 保留导出符号；它与一个带捕获的函数值不同。C 回调还必须匹配参数、返回类型和调用约定。

C 的可变参数 `...` 写在 `extern "C"` 声明的最后一个形参上。

### 谁负责什么

调用方负责边界、null、对齐、生命周期——一句话：**跨过这条线的每个假设都要你自己保证**。这也是它叫 unsafe 的原因。

### 小结

C 是语言 FFI；布局用 `repr(C)`；回调用 `cfn`。

---

## 第31课：`drop` 与作用域清理

第 10 课的 `defer` 登记一块语句。类还可以更进一步：定义 `drop()`，实例离开作用域时自动调用。适合文件句柄、连接、堆对象这类「有确定生命周期」的资源。

```vyx program
var resource_drops: i32 = 0;

class Resource {
    public id: i32;
    public fn drop() {
        resource_drops = resource_drops + 1;
        print("drop id=${self.id}");
    }
}

fn main() -> i32 {
    {
        let resource = Resource { id: 1 };
        print("还在用 ${resource.id}");
    }
    print("离开内层以后，drop 次数 ${resource_drops}");
    return 0;
}
```

先看到「还在用 1」，再看到 drop，次数为 1。

### 为什么是「离开作用域」而不是「函数返回」

注意这里用的是内层的 `{ }` 块。`drop()` 挂在**作用域**上，所以只要出了那对大括号就会触发，不必等到整个函数结束。这让作用域成了资源生命周期的最小单位——想让资源早点释放，就用一对括号把它的使用范围圈出来。

### 和 `Vec.destroy()` 的关系

两者不冲突，但职责不同：

- `drop` 是**语言钩子**：什么时候调用由作用域决定。
- `Vec.drop()` 调用 `destroy()` 释放缓冲，因此未转移的局部容器会在作用域退出时清理；显式 `destroy()` 可以提前释放。

`Vec.destroy()` 会清空自身状态，后续自动 `drop()` 不重复释放；它不逐个析构元素。其他资源类型是否允许重复销毁，要看各自的 API，不能套用容器的约定。

### 小结

`drop()` 是作用域清理入口；容器的 `destroy()` 是具体的缓冲释放操作。

---

## 第32课：泛型枚举与可变参数泛型

`Either<L, R>` 表达「不是左就是右，两边类型可以不同」。`<...Ts>` 是类型包，`args...+` 对所有实参做加法折叠——运算符必须对每一个实参都成立。

```vyx program
use std.core;

enum Either<L, R> {
    Left(L),
    Right(R),
}

fn unwrap_left<L, R>(value: Either<L, R>, fallback: L) -> L {
    match (value) {
        case Left(item) => { return item; }
        case Right(_) => { return fallback; }
    }
}

fn sum_all<...Ts>(...args: Ts) -> i64 {
    return args...+;
}

fn main() -> i32 {
    let left: Either<i32, string> = Either::<i32, string>.Left(42);
    let right: Either<i32, string> = Either::<i32, string>.Right("fallback");
    print("左值：${unwrap_left::<i32, string>(left, -1)}");
    match (right) {
        case Left(_) => { print("不该是左"); }
        case Right(_) => { print("右边是字符串"); }
    }
    print("10+20+12=${sum_all(10, 20, 12)}");
    return 0;
}
```

### 为什么要 `Either`

函数的失败路径和成功路径常常是不同类型——不是「同一种东西的两种状态」，而是**两种完全不同的东西**。硬塞进一个类型就要造一个只有两个字段、永远只用其中一个的结构体，还得自己维护「哪个字段有效」这个约定。`Either<L, R>` 把这个约定变成类型本身。

### 可变参数泛型解决什么

`sum_all(10, 20, 12)` 的参数个数不固定，类型也可能各不相同。`<...Ts>` 声明一个类型包，`args...+` 表示对包里所有实参做折叠。

约束是**运算符必须对每一个实参都成立**：`+` 要求所有 `Ts` 都支持相加。这条约束是编译期检查的，不满足就不通过。

嵌套泛型按 turbofish / 接收者实参实例化。

### 小结

泛型枚举用 match 拆；参数包用 `...` 折叠。

---

## 嵌套记录：`Outer<T>::Inner<U>`

把只在一个类型里用得到的小类型放进它内部，比摊平在模块作用域更好——读代码时「这个类型属于谁」一目了然。

`struct` / `class` 可以写在另一个记录体内，深度不限。嵌套记录按**点分名**提升到模块作用域（`FreqArray` 里的 `A` 就是记录 `FreqArray.A`），参数表是**父级参数表后面接自己的参数表**；两层参数表**完全相同**时视为只是在复述父级参数，不新增参数。

```vyx program
struct FreqArray<T> {
    struct A<T> { public a: T; }        // 与父级参数表相同：不新增参数
    struct B<U> { public b: U; }        // 新增 U
    struct C<T, V> { public c1: T; public c2: V; }   // 复述 T，新增 V
    struct D<K, V> { public d1: K; public d2: V; }   // 全为新名
    public data: *T = null;
}

fn main() -> i32 {
    let a = FreqArray<i32>::A { a: 11 };                 // 参数全部来自父级
    let b = FreqArray<i32>::B<f64> { b: 2.5 };           // 先父级实参，后自己的
    let c = FreqArray<i32>::C<i32, f64> { c1: 3, c2: 4.5 };
    let d = FreqArray<i32>::D<i64, f64> { d1: 5, d2: 6.5 };
    print("${a.a} ${b.b} ${c.c1} ${c.c2} ${d.d1} ${d.d2}");
    return 0;
}
```

### 为什么是「拼接」而不是「隐式继承」

使用处一律写成**「父级段的实参，接嵌套段的实参」**，编译器把它们拼成这个记录的实参列表。

所以第三种（`C<T, V>`）要写全 `<i32, f64>`——只写 `<f64>` 与参数个数不符，编译会失败（目前报的是后端诊断「no record layout」，不是源码级的元数错误，但确实是失败）。

规则看起来啰嗦，但它换来的是**不存在隐式捕获**：任何一处写出 `FreqArray<i32>::C<i32, f64>`，你就能数出这个类型的全部实参，不用回头找外层。

这条规则对类型位置同样成立：

```vyx fragment
struct Holder {
    c: FreqArray<i32>::C<i32, f64>;
    deep: L1<i32>::L2<f64>::L3<i8>;      // 任意深度，逐层实参依次拼接
}
```

嵌套 `class` 与普通 `class` 规则一致：可以有 `public` 成员、方法、构造函数。

```vyx program
class Envelope<T> {
    class Inner {
        public v: i64;
        Inner(n: i64) { self.v = n; }
        public fn get(&mut self) -> i64 { return self.v; }
    }
    public t: T;
}

fn main() -> i32 {
    var b = Envelope<i32>::Inner(42);
    print("${b.get()}");
    return 0;
}
```

### 当前的边界

嵌套记录在模块作用域里只是一个**名字**，由此带来三条限制：

1. 必须先声明后使用。
2. 写在嵌套记录上的静态成员暂时无法通过限定路径访问——该路径解析到记录本身。
3. 父记录自己的体内要写嵌套记录时，必须把父级实参写齐（`FreqArray<T>::Inner<…>`，或扁平写法 `FreqArray.Inner::<…>`），不做外层参数的隐式捕获。

证据：`probes/gates/lang/nested_records.sh`（五种参数表形态 + `class` 嵌套 + 深度链 + 参数个数不符必须报错）。

### 小结

嵌套记录提升为点分名；参数表父级在前、自己在后，两层相同则不新增。

---

## 第33课：`@[comptime]` 折叠

有些计算的结果在写代码时就已经确定了，放进运行时是纯粹的浪费：查表、常量表达式、按文件大小做的分支。

带**常量整数**实参的 `comptime` 调用，会在编译期跑完，折成字面量。

```vyx program
@[comptime]
fn triple(x: i32) -> i32 {
    let y = x + 1;
    if (y > 0) {
        return y * 3;
    }
    return 0;
}

@[comptime]
fn sum_to(n: i32) -> i32 {
    var i = 0;
    var acc = 0;
    while (i < n) {
        i += 1;
        acc += i;
    }
    return acc;
}

@[comptime]
fn io_ok() -> i32 {
    print("ct");
    if (ct_file_size("probes/gates/comptime/loop.vyx") > 0) {
        return 1;
    }
    return 0;
}

fn main() -> i32 {
    print("triple(4)=${comptime triple(4)}");
    print("1+…+4=${comptime sum_to(4)}");
    print("文件看得到？${comptime io_ok()}");
    return 0;
}
```

`triple(4)` 是 15，`sum_to(4)` 是 10；能看到文件则打印 `ct` 且第三个数为 1。

### 为什么 `comptime` 值得单独存在

它不是「优化」那么简单。`ct_file_size(path)` 这类能力**只能在编译期用**——运行时程序已经不知道构建目录长什么样了。有了它，配置检查、按资源大小做的分支、生成期的校验都可以在编译时完成，失败就直接编不过，而不是等到部署之后才发现。

### 解释器能跑什么

会跑 `while` / C 风格 `for`、`break` / `continue`、`+=`、`print` / `println`，以及 `ct_file_size(path)`（相对当前工作目录的文件字节数，失败为 `-1`）。

**堆分配仍不解释**——这是当前的能力边界。

### 注意

从仓库根目录编译，课里的相对路径才找得到。

### 小结

`@[comptime] fn` + 常量实参 → 编译期结果；不能 new 堆。

---

## 第34课：move、`Copy` 与借用

堆所有权赋值是搬走，不是复印。这条规则让「同一块堆存储有两个主人」在类型层面就不可能发生。

```vyx program
use std.collections;

class Bag {
    public items: Vec<i32>;
}

fn main() -> i32 {
    var a = Bag { items: Vec::<i32>.new() };
    a.items.push(1);
    var b = a;
    print("袋子现在在 b 手里：${b.items.get(0)}");
    return 0;
}
```

若在 `var b = a` 之后写 `a.items.get(0)`，就是 E3100。

### 为什么要搬而不是拷

`Bag` 里装着一个堆上的 `Vec`。如果 `var b = a` 只做浅拷贝，两个 `Bag` 就会持有同一个 `Vec`；两个都 `destroy()` 就是双重释放。如果做深拷贝，每次赋值都可能是一次昂贵的分配——而多数时候你并不需要那份副本。

move 是第三条路：**把所有权交出去，源变成不可用**。代价立刻可见（编译错误），代价也立刻可修（真的需要两份就 `clone()`）。

### `Copy`：可以再读的类型

标量和 `@[derive(Copy)]` 的纯标量 class 可以再读源：

```vyx program
@[derive(Copy)]
class Tiny {
    public n: i32;
    public fn drop() {}
}

fn main() -> i32 {
    var a = Tiny { n: 1 };
    var b = a;
    print("两份都还在：${a.n} 和 ${b.n}");
    return 0;
}
```

判断标准是**按位复制是否等价于一份独立的值**。整数、指针宽度的小结构成立；持有堆存储的类型不成立。

原生 `string` / `str` 被视为 Copy；大写 `String` 拥有文本存储，赋值或按值传参会转移所有权。转移后读取源绑定会报告 `E3100`，`let` 和 `var` 都遵守这条规则。需要保留原值时用 `.clone()`，只读参数用 `&String`；已移动的 `var` 可以重新赋值后继续使用。

### 借用

`&T` / `&mut T` 是借用。冲突的 `&` / `&mut` 是 E3101——共享借用还活着的时候不能取可变借用，这样「读一半被改写」就不可能发生。

接收者的写法对齐 Rust：

| 写法 | 含义 |
| --- | --- |
| 无标记 `self` | C++ 的 `this` |
| `&self` / `&mut self` | 共享 / 可变借用 |
| `own self` | 取得所有权 |
| `mut self` | 可变绑定 |

`let mut` 等于 `var`。`unsafe { }` 不跑所有权诊断——这是 escape hatch，也是责任转移点。

### 小结

有堆就当搬走；要两份就 `clone` 或 `Copy`。

---

## 第35课：继承与 `override`

子类把父类字段**按声明顺序拼在前面**，再用 `override` 换掉父方法。`struct` 也可以这样拼字段（`struct Vec3 : Vec2`）。

```vyx program
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

### 先说清它不是什么

**这不是 C++ 虚表。** `class` 仍然是值记录：`Child { x: 3, y: 4 }` 就是一块连续内存，字段 `x` 在前、`y` 在后。继承在这里的含义是**布局拼接**，不是运行时分发。

运行时「一个袋子装多种类型」走 `dyn Trait`（第 28 课）。两件事不要混：**继承管布局，dyn 管分发**。

写 `class Mix : Left, Right` 只是把 Left 的整段布局、再 Right 的整段、再 Mix 自己的字段连起来。所以 `Mix { a: 1, c: 2, b: 4 }` 能同时满足两边的字段。

### 为什么棱形继承是硬错误

两个父类若共享同一个祖先（棱形），编译器直接报错。原因是布局上会出现同一段字段被铺两遍的问题——那时 `self.a` 到底指哪一段？没有不歧义的答案。

与其定一条难以预料的选择规则，不如在编译期拒绝。**这是"让错误的代码写不出来"的又一例。**

### `override` 必须对准已有父方法

没有对应父方法的 `override` 会被拒绝。这和 `@[platform]` 之外的可见性规则一样，都属于同一类设计：声明必须能被验证，不能只是给读者的注释。

### 小结

`Child : Base` 继承字段；`Mix : Left, Right` 按顺序拼两段布局。`override` 必须对准已有父方法。棱形继承是硬错误。

---

## 第36课：`interface` 默认方法与超接口

`interface`、`trait`、`protocol` 占用同一个关键字槽，是同一种东西的三个名字。

接口可以带默认方法体；`impl` 只补必选方法：

```vyx program
interface Greeter {
    fn greet(self) -> i32;
    fn hello(self) -> i32 { return self.greet() + 1; }
}

class En {
    public n: i32;
}

impl Greeter for En {
    fn greet(self) -> i32 { return self.n; }
}

fn main() -> i32 {
    let e = En { n: 6 };
    print("直接 hello=${e.hello()}");
    let d: dyn Greeter = e;
    print("dyn hello=${d.hello()}");
    return 0;
}
```

两行都是 `7`。

### 默认方法解决什么

没有默认方法时，每个实现者都要重复写一遍相同的逻辑——而且这些副本会各自漂移。默认方法把「共同做法」放进接口本身，实现者只需要回答**真正因类型而异的那部分**。

这跟 `@[derive]` 是同一个思路的两种形态：derive 把完全由字段决定的实现交给编译器，默认方法把大部分由接口决定的实现交给接口。

### 超接口

`interface Drawable : Shape` 要求两边契约都满足。这样「可绘制」的约束里就已经包含了「有形状」，调用方拿到 `dyn Drawable` 就能用 `Shape` 的方法，不必再声明第二个约束。

### 默认方法也能走 `dyn`

上面例子里 `d.hello()` 就是通过 `dyn Greeter` 调用的默认方法——分发点在运行时，但默认实现本身仍然是编译好的那一份。

### 小结

默认方法写在接口里；impl 补缺口；dyn 也能调默认方法。

---

## 第37课：类型别名与关联 `const`

`type Ptr<T> = *T` 只是换个短名字。trait 上的 `const MAX` 是关联常量，用 `T::MAX` 读。

```vyx program
type Ptr<T> = *T;

trait Bounded {
    const MAX: i64;
    fn value(self) -> i64;
}

class U8Like {
    public v: i64;
    const MAX: i64 = 255;
    public fn value(self) -> i64 { return self.v; }
}

fn clamp<T>(x: T) -> i64 where T: Bounded {
    let v = x.value();
    if (v > T::MAX) { return T::MAX; }
    return v;
}

fn main() -> i32 {
    let n: i32 = 9;
    let p: Ptr<i32> = &n;
    print("别名指针看到 ${*p}");
    print("300 夹紧到 ${clamp::<U8Like>(U8Like { v: 300 })}");
    print("123 原样 ${clamp::<U8Like>(U8Like { v: 123 })}");
    return 0;
}
```

### 别名是零成本的

`Ptr<i32>` 和 `*i32` 在编译器眼里是同一个类型，别名只影响源码的可读性。所以取名的收益和代价都很纯粹：**读起来更清楚就值，为了短而短就不值。**

### 关联常量解决什么

`clamp` 需要对「这个类型的上限」做判断。如果把上限写成一个全局常量，那么每个类型都得配一个，还得自己保证它们互不混淆。

关联常量把上限**挂在类型上**：`T::MAX` 读的是 `T` 自己声明的那一份。加上 `where T: Bounded`，编译器就知道任何传进来的 `T` 一定有 `MAX`，可以放心使用。

这里必须写 `where T: Bounded`，让 `T::MAX` 能通过约束解析到关联常量。第 13 课的普通算术在当前编译器中可以没有显式约束；不要把两者的解析要求混为一条规则。

### 小结

`type` 起别名；`T::MAX` 读关联常量。

---

## 第38课：`@[platform]` 门控

同一件事在不同操作系统上做法不同：路径分隔符、系统调用、可用的 API。用运行时分支处理，意味着两边都要能编过——包括那些在目标平台上根本不存在的 API。

`@[platform]` 把选择提前到编译期：**同一名字按目标操作系统留一份实现，Sema 丢掉不匹配的那份**，因此不会重名。

```vyx program
@[platform("posix")]
fn path_sep() -> i32 { return 47; }

@[platform("windows")]
fn path_sep() -> i32 { return 92; }

fn main() -> i32 {
    print("路径分隔符的字节是 ${path_sep()}");
    return 0;
}
```

POSIX 上是 `47`（`/`），Windows 上是 `92`（`\`）。

### 可用的门

- `posix`：Linux、Android、macOS
- `linux`：仅 Linux（`io_uring`、x86_64 `syscallN` 走这条）
- `windows`：Windows

### 为什么在 Sema 阶段就丢掉

如果留到运行时判断，不匹配的那份代码仍然要**通过类型检查、参与编译**。而它引用的往往是目标平台上不存在的符号——这就直接编不过。

在 Sema 阶段丢弃不匹配的声明，等于说「这份代码在这个目标上不存在」，问题从根上消失。代价是**编译器只检查当前目标**：某个平台上的那份实现写错了，只有编译那个平台时才会发现。所以跨平台的门需要在 CI 上按目标各编一遍。

从 Windows 交叉链接 Android 见[测试指南](TESTING_GUIDE_ZH.md#交叉编译windows-宿主)。

### 小结

`@[platform]` 按目标挑一份声明，另一份不存在。

---

## 第39课：`sizeof`、`alignof` 与 `static_assert`

这些是**类型查询**，折成 `i64`，不是 C 预处理器。`repr_sizeof::<T>()` 是同一族。

```vyx program
fn main() -> i32 {
    static_assert(1 + 1 == 2);
    static_assert(sizeof::<i32>() == 4, "i32 is 4 bytes");
    print("i32 大小 ${sizeof::<i32>()} 对齐 ${alignof::<i32>()}");
    print("i64 大小 ${sizeof::<i64>()}");
    return 0;
}
```

### 为什么要 `static_assert` 而不是运行时断言

类型布局的假设一旦不成立，程序会以难以诊断的方式出错：读错字段、越界、ABI 不匹配。这类假设**在编译期就能验证**，把它留到运行时就等于把必然的失败推迟到最难查的时刻。

`static_assert` 在编译期检查，失败则编不过——和第二参数给出的说明一起，直接把「我对这块内存的假设是什么」写进代码。

顺带一提，`sizeof` 和 `alignof` 是查询而不是宏，所以它们遵循正常的类型系统规则，也能用在泛型里。

### 小结

`sizeof` / `alignof` / `static_assert` 都在编译期说话。

---

## 第40课：运算符方法

你可以给 class 写 `operator+`、`operator[]` 等方法，让自定义类型在语法上和内置类型一样好用。

```vyx program
class SymbolBox {
    public value: i32;

    public fn operator[](self, idx: i32) -> i32 {
        return self.value + idx;
    }

    public fn operator+(self, other: SymbolBox) -> SymbolBox {
        return SymbolBox { value: self.value + other.value };
    }
}

fn main() -> i32 {
    let a = SymbolBox { value: 4 };
    let b = SymbolBox { value: 9 };
    let c = a + b;
    print("a[3]=${a[3]}，a+b 的 value=${c.value}");
    return 0;
}
```

你会看到 `7` 和 `13`。

### 运算符就是普通方法名

`operator+` 只是方法名，`a + b` 会被解析成对它的调用。这意味着规则和普通方法完全一致：能借用、能返回新值、能被 trait 约束。

**整数自己的 `+` 不走这条路径**——内置运算仍然直接映射到机器指令，不会被任何用户定义的东西劫持。这一点很重要：它保证了基础运算的性能和行为不会因为你实现了什么而改变。

### 什么时候该写

运算符适合**语义上没有歧义**的操作：`+` 表示合并、`[]` 表示按下标取值。如果含义需要解释（`+` 到底是相加还是拼接？），用普通方法名更清楚——自定义运算符的收益是可读性，含义模糊就把这个收益抵消掉了。

### 小结

运算符是普通方法名；只对你写了 overload 的类型生效。

---

## 第41课：版本化模块与 migrate

库的接口会变，但已经编译好的调用方不能跟着一起碎。

`@[version]` 与 `@[variant]` 标识模块版本，`Vyx.lock` 选择默认版本。`@[migrate]` 关联当前声明与历史声明，`fromSig` 指定历史函数签名，`fromField` 描述字段映射。显式历史调用写作 `quote@1.0.0["standard"](3)`。

### 为什么需要它

普通的做法是「改接口 → 所有调用方一起改」。这在库和调用方由不同人维护、或者有二进制交付物时行不通。

版本化把「旧签名」保留下来，让老调用方继续按老契约工作，新调用方用新契约。`@[migrate]` 的作用是**明确写出新旧之间的对应关系**，而不是让编译器去猜。

代价是维护面扩大：项目必须提供所引用的历史版本，历史声明也要一直存在。

### 继续阅读

完整多文件示例与构建命令见 [事实语义所有权系统：Migrate](MOSP_ZH.md#migrate模块版本与历史调用)。
字段、构造函数与方法示例见 [tutorial_migrate](../tests/projects/tutorial_migrate/src/main.vyx)。

---

## 第42课：DCI——编译型语言互操作

`extern "C"` 能表达的是 C 的 ABI。一旦对方是 C++ 的类、Rust 的 trait、或者带生命周期语义的对象，需要传递的信息就超出了一个函数签名能承载的范围：**类型布局、调用约定、生命周期、泛型实例**。

DCI 用生产端提供的事实契约来描述这些。Vyx 通过 `@[dci_import("path.dcib")]` 与 `extern "dci"` 使用外部声明；生产端的语义与泛型约束仍由生产端编译器处理。

### 为什么是「生产端出具事实」

把布局和符号的定义权交给**真正拥有这个类型的那一侧**，是 DCI 的核心选择。

如果让消费方自己猜（或者由人手工照抄头文件），一旦生产端的布局变了而消费方没跟上，错误会在运行时以最难看的方式出现。让生产端在构建期生成契约，消费方按契约生成 stub，两边用的是同一份事实。

C++ 与 Rust 均有 Active Adapter 开放泛型路径。泛型函数在调用点闭合请求，由生产端在构建期物化；代码生成需要的泛型记录实例布局必须已在契约中提供。

具体支持范围取决于 Adapter、目标和操作，**不能把旧的离线 Adapter 限制推广到整个 DCI**。

### 继续阅读

完整原始定义、Vyx 声明及配置说明见 [事实语义所有权系统：DCI](MOSP_ZH.md#dci生产端事实与开放泛型)。
可运行工程见 [dci_opengeneric](../tests/projects/dci_opengeneric/README.md)，协议见 [DCI 规范](DCI_SPEC_ZH.md)。

当前 AOT 回归也覆盖原始 C++ `std::vector<T>` 模板、Cargo 生态库、ICU 原生 DLL，以及 `shared_abi` 的传播与作用域清理。Adapter 命令和目标限制见 [SDK 指南](../tools/dci/README.zh-CN.md)，必跑回归见[验证指南](TESTING_GUIDE_ZH.md)。JIT 能力对齐留待后续。

---

## 第43课：运行时反射

编译期能确定的东西都不需要反射。反射处理的是另一半：**类型和成员在写代码时还不知道**——插件按名字查找、通用序列化、调试工具。

使用 `@[reflect]` 登记类型或成员，`@[hidden]` 排除成员。`std.reflect` 提供按名查询、绑定实例、属性访问和有类型的方法调用。

### 为什么要显式登记

如果所有类型都默认可反射，那就意味着**每个类型的元数据都必须保留**——包括那些只是为了在内部传递数据而存在的类型。这会直接顶住 DCE 的目标。

`@[reflect]` 把「这个类型需要在运行时被看见」变成一个显式的声明。编译器据此决定保留什么，`@[hidden]` 则在同一类型内部做减法。

由此有一条必须记住的联动关系：**运行时反射依赖保留的元数据和代码，相关入口会影响 DCE 的根集合**。选了反射，就要接受这部分代码不会被裁掉。

### 继续阅读

完整示例见 [事实语义所有权系统：Reflection](MOSP_ZH.md#reflection发现绑定调用)。
更多 API 见 [tutorial_reflect.vyx](../tests/cases/tutorial_reflect.vyx)。
代码裁剪规则见 [事实语义所有权系统：DCE](MOSP_ZH.md#dce可达性与必要行为)。

---

## 第44课：事实语义所有权、Effect 与自定义属性

前面已经使用了 Migrate、DCI 和 Reflection。它们与 DCE、Effect 共同构成 Vyx 的
事实语义所有权系统：事实有来源、有效条件和消费规则，编译器据此决定程序语义。

### 从事实到语义

DCI 的生产端提供布局和生命周期事实；消费端验证契约后生成调用与清理。
Reflection 登记的成员成为可查询事实，也影响 DCE 的代码保留。
Migrate 描述版本之间的关系；Effect 让这些事实进入对应的语义规则，并建立依赖与义务。

学习时依次追问：事实怎么来、怎么用、谁用、谁管理、有什么作用。
提供方、规则定义方与消费方各自承担责任。一个属性的名字不赋予它修改编译器的权限。

### 定义属性，纳入项目

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

```vyx program
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

## 接下来

高级课到此结束。想继续往深处走，这几份文档是随手的参考：

- [语言表面](语言表面_ZH.md)：已实现语法的索引。
- [语言设计](设计文档_ZH.md)：语言规则背后的设计取舍。
- [DCI 规范](DCI_SPEC_ZH.md)：跨语言契约的完整协议。
- [事实语义所有权系统](MOSP_ZH.md)：Migrate、Reflection、DCI、DCE、Effect 的当前用法。
- [Effect 模型规范](MOSP_EFFECT_ZH.md)：事实消费、编译后果与自定义属性。
