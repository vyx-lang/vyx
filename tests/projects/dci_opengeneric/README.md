# dci_opengeneric — DCI Active Adapter 开放泛型演示

Vyx 消费方在源码里**直接声明开放泛型 DCI 函数**，编译器按调用点实参闭合，
构建期回呼 Active Adapter 让**生产端编译器**裁决边界并实时物化。

```vyx
extern "dci" {
    struct Vec2 { public x: f64; public y: f64; };

    // 非泛型工厂/访问器：闭合契约（.dcib symbol）。
    fn vyx_vec2_make(x: f64, y: f64) -> Vec2;
    // 开放泛型：调用点遇到什么 T，Active Adapter 就实时物化什么。
    fn twice<T>(v: T) -> T;
    fn max_of<T>(a: T, b: T) -> T;
    fn identity<T>(v: T) -> T;
    fn swap<T, U>(a: T, b: U) -> (U, T);
}

fn main() -> i32 {
    if (twice(21) != 42) { return 1; }        // twice__i32
    if (twice(1.5) != 3.0) { return 2; }      // twice__f64
    if (max_of(3, 9) != 9) { return 3; }      // max_of__i32（生产端裁决约束）
    let v = vyx_vec2_make(3.0, 4.0);
    let r = identity(v);                       // identity__Vec2（16B sret）
    let sw = swap(21, 1.5);                    // swap__i32_f64（16B sret）
    ...
}
```

## 两个生产端，两份点对点契约

`native/lib.rs`（rustc）与 `native/lib.hpp`（clang++）声明**同一套**开放泛型
——`twice` / `max_of` / `identity` / `swap` 加同一个 `Vec2` ——所以选哪个生产端
是构建配置，不是调用点的改动：

| target | backend 参数 | provider | 边界裁决者 | 契约 |
|---|---|---|---|---|
| `dci_opengeneric` | `--lang rust` | `native/lib.rs` | rustc（`Add` / `PartialOrd`） | `dci/open_generic.dcib` |
| `dci_opengeneric_cpp` | `--lang cpp` | `native/lib.hpp` | clang++（`operator+` / `operator>`） | `dci/open_generic.cpp.dcib` |

两者共用同一套**开放泛型**调用点：`src/open_generic_cpp.vyx` 是
`src/open_generic.vyx` 的副本，**逐行相同、只差 `@[dci_import]` 那一行**
（门里对此有断言）。成员函数也一样齐：两份源都调用 `Vec2::norm1`（非泛型
record）与 `Pair2<f64,i32>::{get_first,swapped}`（泛型 record 的实例方法）。
差别只在**符号怎么来** —— 由各自产物的工具链决定，不是调用点的选择：cpp 侧
`norm1` 的 `link_name` 就是 clang 的成员 mangling（`?norm1@Vec2@@QEBANXZ`），
实例方法走 semantic_id 物化请求、clang++ 现场发射真实成员符号；rust 侧
`norm1` 落在稳定链接名 `vyx_vec2_norm1`（rustc 不会替没人引用的函数发射符号，
所以它同样走 semantic_id），实例方法由 rustc UFCS 物化。
构建配置写在 `Vyx.toml` 的 `[target.*]` 里
（`dci_stub_backend_tool(_args/_dependencies/_version/_source_extension/_capabilities)`），
相对路径按项目目录解析，所以在项目目录里直接 `vyxc build` 即可（只需 `python`
与 `rustc` 在 PATH 上，clang++ 由 backend 从 `clang/bin` 自取）。

> `VYX_DCI_STUB_BACKEND_*` 环境变量一旦设置即覆盖 manifest —— 但它是**全局**
> 覆盖，会把两个 target 钉成同一个生产端。按 target 选生产端只能走 manifest，
> 所以 `run_vyx.sh` 不导出这些变量（脚本末尾另有一步专门验证 env 覆盖仍生效）。

### `.dcib` 是点对点的：换实现就换契约

`.dcib` 描述的是**一个产物**：谁编的、从哪个源文件、导出哪些符号、record 长什么
样。它不是接口 IDL，不追求跨实现通用 —— provider 换一门语言，契约就该跟着换。

| target | 契约 | 生产端 / 源 | 怎么来 |
|---|---|---|---|
| `dci_opengeneric` | `dci/open_generic.dcib` | rustc 1.92.0 / `native/lib.rs` | `dci_adapter_rust.py`（`--export-active-requests`；**不点名实例**）从 provider 源现场产出 |
| `dci_opengeneric_cpp` | `dci/open_generic.cpp.dcib` | clang++ / `native/lib.hpp` | `dci.py adapter --language cpp` 从 provider 头自动导出 |

> **实例需求由消费方驱动（2026-09-24 落地）**：契约只带不变事实（闭合符号、
> 非泛型方法、声明的实例布局），泛型实例**不预导出**。消费方降级时缺哪个实例就
> 往 `.dci_open` 写一行 `instance <type-text>`，构建内闭合循环派生
> `tools/dci/dci_close_instances.py`（dcib 溯源 → 对应语言 adapter）现场让生产端
> 闭合，产出补充 `.dcib` 后再降级，直到不动点。**脚本与契约里都不再有任何
> `--export-instance`**；下面各节叙述「门里点名实例」的段落记录的是该机制落地
> 前的取证路径，实例名现在由消费方发现。

**两份契约都是 adapter 产出（生成即 provenance），不存在手写契约。**

两份**不做逐字段比对**（门里也不断）：它们描述的是两个不同产物、两个不同编译
器，provenance、mangling、布局记录天然不同，要求「ABI 面等价」等于把点对点契约
重新读成通用契约。点对点契约的验收标准只有一条 —— **它服务的那个 target 能编、
能跑**，其余交给生产端自己的编译器。

唯一一条跨两份的断言是「开放泛型两边都不在契约里」：`twice` / `max_of` /
`identity` / `swap` 由调用点实参在编译期闭合，走 `<obj>.dci_open` sidecar 交给
生产端，所以两份 `.dcib` 里**一个实例符号都没有**（门里 `grep 'twice__'` 必须为 0）。
两边都有的是：3 个闭合符号（`vyx_vec2_make` / `vyx_vec2_x` / `vyx_vec2_y`）、
`Vec2` 与全部 `Pair2<...>` 实例的布局（12 条，标量宽度对 + record 作实参，见
`dci/instance_layouts.json`），以及各实例的**成员函数**符号 ——
但符号长什么样由各自的工具链决定：cpp 是 clang 的 mangling
（`?get_first@?$Pair2@NH@@QEBANXZ`），rust 的 link 名是 adapter 合成的
`vyx_pair2_<args>_<method>`，符号本体由 rustc 在构建期物化。

两个 target 的 sidecar 逐字相同（门里 `cmp` 断言，行数 ≥10）：闭合请求只由调用点
实参决定，与谁去物化无关 —— 这条断的是「两份 Vyx 源是同一套调用点」这句话本身。

### cpp 契约里 16 字节聚合参数为什么是 `indirect`

clang 把 `vyx_vec2_x(Vec2 v)`（16 字节 POD 按值）编成

```
define dso_local double @vyx_vec2_x(ptr dead_on_return noundef %0)
```

—— **传指针**：调用者提供存储、被调者不得持有，正是 DCI 的 `indirect`（借用
指针）。adapter 早先把这里的 `ptr` 机械地翻成 `coerce` 并指向 `ptr`，但 `coerce`
要求载体与 storage **等宽**（`llvm_lower.vyx` 的 `dci_coerce_contract_supported`），
16 ≠ 8，于是构建期只管报：

```
I0100: DCI coerce size mismatch for parameter #0 of `vyx_vec2_x`: storage=16 coerce=8
```

这个形态本来就是**非法契约**，不是编译器缺功能：`dci_rust_generic/run.ps1` 的
`coerce_parameter_width` 变体（把 `flags_priority` 的载体 `u32` 改成 `u64`）正是
把它当成「必须失败」的负例。所以修的是 adapter 的分类
（`tools/dci/dci_adapter_msvc.py` 的 `_aggregate_lowered_to_bare_pointer`），
而不是去放宽编译器校验。

`run_vyx.sh` 现在在契约产出后立刻用 `tools/check_cpp_contract.py` 断言这一点
（`coerce` 载体必须与 storage 等宽；`vyx_vec2_x` / `vyx_vec2_y` 必须是
`indirect`/`byval` 且 `size=16`，并打印真正检查过的条目数），免得它只在构建期
以「看着像编译器 bug」的 I0100 现身。

### 泛型类型（泛型 record）怎么过 DCI 边界

`swap<T, U>(a: T, b: U) -> Pair2<U, T>` 返回的是一个**泛型类型**，既不是
primitive 也不是像 `Vec2` 那样具体的 record。它和自由泛型函数走两条不同的路，
这个区别值得钉住：

- **泛型函数不进契约**。实例在编译期由调用点闭合，经 `<obj>.dci_open` sidecar
  交给 external backend，生产端编译器在 `resolve_batch` 探针里裁决。所以契约里
  `grep 'twice__'` 必须为 0（门里有这条断言）。
- **泛型类型的实例布局必须进契约**。codegen 在生成 IR 的那一刻就要知道
  `Pair2<f64,i32>` 的 size / alignment / 字段偏移，而那时 backend 还没跑 ——
  没有第二个时机能告诉它。匹配键是**实例化名**：Vyx 侧的类型文本
  `mir.struct.Pair2::<f64,i32>` 会被归一成 `Pair2<f64,i32>`（`.<` → `<`，
  见 `dci_canonical_type_name_text`），契约里就写这个字符串。

  写成定义名（`Pair2`）会失败，而且是两条不同的失败：

  ```
  I0100: DCI Descriptor has no matching symbol for external declaration `swap__i32_f64`
  I0100: missing DCI ABI lowering for return of `swap__i32_f64`
  ```

**这条布局现在由 adapter 实测，项目声明只做剩下两件事。** 布局闭包
（`expand_layout_closure`）原本从「头文件里声明的记录名」出发：模板定义本身没有
ABI，特化 `Pair2<double, int>` 也不是被声明的记录名，所以实例永远进不来，谁引用它
谁就被「没有 verified layout」拒掉 —— 绕成一个环。破环的是把**实例名**（现在由
消费方发现、闭合器点名给生产端；该机制落地前是门里
`--export-instance "Pair2<double, int>"` 点名，rust/cpp 同构；provider 源码里
不再需要任何 `template struct ...;` 显式实例化）收进 sizeof 探针与布局闭包：
clang 的 `-fdump-record-layouts` 因此 dump 出真实的 size / align / offset。
**clang 只为「布局被 requires」的记录 dump** —— 实测过：探针里不写
`static_assert(sizeof(<实例>))`，dump 里连该条目都不存在。
`Pair2<i32,f64>` 就是这么进来的（`swapped() -> Pair2<B, A>` 的按值返回类型）。

所以项目声明只剩两件事：provider→consumer 的**拼写映射**，以及 provider 没实例化
的实例的**声明**（声明会被 merge 与实测结果逐字段交叉核对，对不上即失败）。

| 文件 | 谁产出的 |
|---|---|
| `dci/instance_layouts.json` | 手写——拼写映射 + 声明（声明会被实测逐字段交叉核对） |
| `dci/open_generic.dcib` / `dci/open_generic.cpp.dcib` | rust / cpp adapter 从 provider 源码导出 → `tools/dci/merge_instance_layouts.py` 重拼名字并交叉核对 |

`type_name` 必须用**消费者拼写**（`Pair2<f64,i32>`），它是布局查找的匹配键；
provider 侧拼写写在 `provider_spelling` 里（cpp `Pair2<double, int>`、
rust `Pair2<f64, i32>`），merge 用它把**实测**布局的名字重拼过来再合并 ——
顺序反了会因键对不上把同一条实例当成缺失、重复加一遍。

**人工声明不等于凭幻觉。** 门里紧接着用生产端编译器把它兑现：
`tools/dci/verify_instance_layouts.py` 按**契约里的数字**生成探针，断言
`sizeof` / `alignof` / `offsetof` —— rust 走 `std::mem::size_of` / `offset_of!`，
cpp 走 `static_assert(offsetof(...))`。声明与产物漂移就红。负例实测：把 `second`
的偏移从 8 谎报成 4，两侧都失败（rust `E0080: evaluation panicked`、
cpp `static assertion failed`）。

### 泛型 record 的**成员函数**：两侧都通了

字段能过边界（上一节），成员函数也能 —— 但这条路要**两个半边各自补齐**，缺一个
就死在半路。三条实测把位置钉住：

| 情况 | 结果 |
|---|---|
| `Vec2::norm1`：**非泛型** record 的成员函数 | ✅ 契约里由 adapter **自动**导出（`kind=method`、`owner=Vec2`）；两端各按自己产物的 ABI 命名（cpp `link_name=?norm1@Vec2@@QEBANXZ`、rust `link_name=vyx_vec2_norm1`），编得动、跑得动 |
| `TupBox::<i32>.get()`：**原生**泛型类的方法 | ✅ mono 物化成 `mir2$TupBox::<i32>::get:T,Self=i32,TupBox::<i32>`（`owner_type=TupBox::<i32>`） |
| `Pair2<f64,i32>.get_first()`：**DCI 泛型** record 的方法 | ✅ mono 物化成 `mir2$Pair2::<f64,i32>::get_first`（`flags=172`），codegen 解析到契约里 `owner=Pair2<f64,i32>`、`member=get_first` 的符号 |

**消费方：`ensure_function_instance` 的早返回。** 它原来的条件是「这个 DCI 声明
**自己**有没有模板参数」—— 而泛型 record 的方法自己没有，模板是它的**所有者**。
于是它被当成普通 DCI 声明原样返回（开放的 `Pair2`），调用点选不出 callee：

```
unresolved call `get_first` (recv=Pair2::<f64,i32>): callee is not a function,
builtin, variant constructor, or lowered method
```

修法是给早返回补一个 `owner_needs_instance`：所有者是模板**且**接收者已带上具体
实参时不再早返回，继续走实例化。物化出来的实例是**无体声明**（`declaration_only`），
flags 从非泛型 DCI 方法的 `168` 变成 `172` —— 差的那个 `4` 就是 `closed_generic_env`，
即「所有者是模板实例」这件事本身。

**codegen：`function_method_name` 里的一个字符。** 实例名形如

```
mir2$Pair2_3A_3A_3Cf64_2Ci32_3E_3A_3Aget_5Ffirst_3A_3AA_2CB_2CSelf_3D_3Ef64_2Ci32_2CPair2_3A_3A_3Cf64_2Ci32_3E
```

按 `_3A_3A`（`::`）切段是 `mir2$Pair2` / `_3Cf64_2Ci32_3E` / `get_5Ffirst` / …，
第二段是泛型实参，要跳过。原来的跳过判断写成 `segment.substring(0, 2) == "3C"`，
而 `<` 的转义是 **`_3C`** —— 判断永远不成立，方法名于是解析成了**实参列表**
（`<f64,i32>`）。契约里那条 `member=get_first` 的符号因此一辈子匹配不上：

```
DCI Descriptor has no matching symbol for external declaration `mir2$Pair2...`
(kind=method, member=<f64,i32>, owner=Pair2::<f64,i32>)
```

两处修好之后，那句 `member=` 就是最好的回归探针：它从 `<f64,i32>` 变成 `get_first`。
门里那条负例（provider 不提供的方法）断言的是 `member=tag, owner=Pair2::<f64,i32>)`
—— 失败必须**已经不在 mono**（不得再出现 `unresolved call`），且点名的成员是 `tag`
而不是实参段。少了「不在 mono」这一条，「修好了」和「又退回 mono 了」不可区分。

**契约侧的符号从哪来 —— 两个生产端现在完全同构。**

*cpp：consumer 发现实例，adapter 产出物化请求，clang++ 现场裁决。* 消费方在
`.dci_open` 请求的实例名（此前由门里 `--export-instance "Pair2<double, int>"`
点名，现由构建闭合循环自动派生）喂给 sizeof
探针与布局测量（见上一节），实例方法导出成带 `semantic_id`
（`dci.active.cpp.Pair2::get_first(i32,f64)`，实参是 **DCI 名空间**的拼写）的
**Active 物化请求**，`link_name` 仍是最终 MSVC mangled 成员符号。构建期
stub backend 的 shim TU 发射 force-use 体：构造接收者、调用成员、并把成员
**地址存进 volatile 全局**（`static auto volatile keep = &Pair2<int,double>::get_first;`）
—— 这一步是 load-bearing 的：裸的 `(void)&f` 会被 clang 当常量折叠，-O2 还会
把对零初始化接收者的调用整个内联掉，COMDAT 定义就被丢了；volatile 存储不可
折叠，成员自己的定义必然被发射，消费方的未定义引用由**真实成员符号**兑现
（this/sret 顺序由 clang 自己摆，ABI 不经人手）。shim 因此用 `-O0` 编译。

*rust：同一形状，rustc 裁决。* rustc 也不会为一个没人引用的单态化发射符号 ——
**产物里有哪些实例由消费方的实例请求决定**：消费方在 `.dci_open` 里请求哪些实例，
闭合循环就闭合哪些（此前由门里 `--export-instance "Pair2<f64, i32>"` 点名全部组合，
现由 `dci_close_instances.py` 自动派生），adapter 解析 `impl<A, B> Pair2<A, B>` 的
方法（泛型 impl 方法曾是解析盲区），对每个实例导出带 `semantic_id`
（`dci.active.rust.Pair2::get_first(f64,i32)`）的**Active 物化请求**。构建期
Active Adapter 生成 UFCS shim（`crate_shim::Pair2::<f64,i32>::get_first(&v0)`），
rustc 接受单态化才放行 —— `A: Copy` 这类约束不满足就以 producer 诊断拒绝
（record 实参因此要求 `Copy`，这正是 DCI 按值边界的 trivial-copy 语义）。
`--export-active-requests` 同时把 3 个闭合符号与 `Vec2::norm1`（**非泛型** record
的方法）也变成物化请求：`lib.rs` 零 `#[no_mangle]`，provider 是**纯泛型源码**。
非泛型方法这一条走的是**方法查询**（按 `owner` + `member` 定位），接收者不进
semantic_id 的实参列表 —— 它是方法查询的隐式部分，spell 成实参会让生产端去找一个
名叫 `&Vec2` 的 record。

方法返回类型引用的**第二个实例**（`swapped() -> Pair2<B, A>`）由 adapter 从方法
签名自动物化并实测布局（以前要靠门里显式多传一个 `--export-instance`，现在不用了）。

两端符号名不同是**应该的**，不是需要抹平的差异：cpp 走类成员符号（`this` 是隐式
接收者），rust 走自由函数入口（接收者是显式首参），本来就不是同一种 ABI。

**门怎么钉住两侧。** provider 侧与消费方侧**分开**断言，谁也不靠「链接恰好成功」
证明对方：

- provider（cpp）：`llvm-nm` 在构建产物（后端生成的 shim 对象）里找到成员符号
  （`?norm1@Vec2@@QEBANXZ` + 各实例的 `get_first`/`swapped`），并断言「检查了几个
  对象、读了几行符号」—— 0 匹配必须是「看过并且没有」，不是空断言（这条以前吃过
  亏：MSYS 路径喂给 Windows 的 `llvm-nm` 会静默返回空，两个空输入比出「无重复」）；
- provider（rust）：对称的一条 —— 消费方引用的入口在 rust 那份产物里必须是
  **已定义**（`T vyx_pair2_*`，外加**非泛型** record 的 `T vyx_vec2_norm1`），同样
  先断「看过几个对象、读了几行符号」；
- 消费方（cpp）：单文件编到 MIR 断言实例被物化（`owner_type`、`block_count=0`、
  `flags=172`），再编到**目标文件**、`llvm-nm` 断言留下的是对同一 mangled 名的
  **未定义**引用（`U`）—— 未定义才说明这个名字要由 provider 兑现，而不是消费方
  自己编了一个实现；
- 消费方（rust）：上面那两条换 rust 那份契约再走一遍 —— MIR 实例名**逐字相同**
  （MIR 命名只由 Vyx 侧类型决定），目标文件留下的未定义引用换成
  `U vyx_pair2_f64_i32_{get_first,swapped}` 与 `U vyx_vec2_norm1`；
- 端到端：两个 exe 都必须跑出 `dci_opengeneric OK`，并在**各自 target 那份目标
  文件**上再查一次未定义引用（cpp 查 `?get_first@?$Pair2@NH@@QEBANXZ`、rust 查
  `vyx_pair2_f64_i32_*`）—— 这条断言比「跑通了」强：它排除「调用点被优化没了」。

顺带记住两个真实的坑：类内定义的成员函数隐式 `inline`，没有调用点就会被丢掉 ——
消费方引用的 DCI 符号会在**链接期**以 `undefined symbol: public: double __cdecl
Vec2::norm1(void) const` 炸掉（`norm1` 最早就是这么发现的）。所以非模板成员要
**类外定义**；而实例方法的 force-use 物化在 -O2 下会被 clang 折叠干净（调用被
内进、`(void)&f` 是常量、连 `static volatile` 全局指针都会被常量初始化吃掉），
shim 必须 **-O0** 编译，调用才活下来、成员定义才被发射。

### 消费方声明的 record 作类型实参（`TestS` / `TestF`）

上面所有实例的类型实参都是**生产端侧**的类型（primitive、或 provider 自己
`#[repr(C)]` 的 `Vec2`），契约里有它们的布局。`TestS` 不是：它只存在于消费方
源码里，契约里**没有**它，预导出名单里也没有 `Pair2<i32,TestS>`
—— 这个实例完全是调用点自己闭合出来的（消费方引用到它，才产生实例请求）。

```vyx
@[repr(C)] struct TestS { width: u32; height: u32; }   // 8 字节，C 布局
@[repr(C)] struct TestF { v: Vec2; }                    // 16 字节，嵌一个 DCI record

let structT: TestS = TestS{width: 1, height: 1};
let structF: TestF = TestF{v: v};
let ss  = swap(structT, n32);      // -> Pair2<i32, TestS>
let sff = swap(structF, n32);      // -> Pair2<i32, TestF>
```

要让这件事成立，请求文件里必须同时带**两行**（不是一行）：

```
record	TestS	8	4	width:u32:0,height:u32:4
layout	Pair2<i32,TestS>	12	4
```

- `record` 让生产端**真的定义**这个类型。没有它，rustc / clang++ 连实例的名字
  `Pair2<i32,TestS>` 都拼不出来（会报 `record 'TestS' does not exist in the
  producer crate`）。所以这一行必须落到**生产端源码**里，而不是只进一张元数据表。
- `layout` 把消费方**推导**出来的实例布局交回生产端。只有消费方能推：
  `@[repr(C)]` 让记录的布局就是 C 布局（不带 Vyx class 的 8 字节对齐下限），
  于是按字段序列算出 `first@0`、`second@4`、size 12、align 4。

**推导为什么诚实**：生产端会把同一个数字**实测**回来。生成的 shim 里带断言 ——

```rust
#[repr(C)] pub struct TestS { pub r#width: u32, pub r#height: u32 }
const _: () = assert!(::core::mem::size_of::<Pair2<i32,TestS>>() == 12);
const _: () = assert!(::core::mem::align_of::<Pair2<i32,TestS>>() == 4);
```

对不上就是 rustc / clang++ 自己的编译错误（实测把推导的 `align 4` 改成 `8`，
rustc 立刻以 `E0080` 拒绝）。所以这不是「消费方说了算」，是「消费方陈述、
生产端复核」。

**fail-closed**：只有 `@[repr(C)]` 的记录可推。普通 Vyx class 的布局带着 8 字节
对齐下限，而生产端的 `struct` 没有，推出来的数字根本不是同一个布局 —— 宁可直接
拒绝。同理，实例的实参若全是生产端**本来就看得见**的东西（标量、已实测的
record），缺布局仍然是一个响亮的契约错误，不会被悄悄「推导」掉。

**ABI 上还有一个坑（Win64）**：8 字节的 `TestS` 是 **direct** 实参，但
「direct」不等于「把 LLVM struct 原样传过去」。clang / rustc 对 1/2/4/8 字节的
聚合统一强制成**单个整数寄存器**（`struct {float,float}`、`struct {double}` 也是
`i64`，**不是** XMM；3/6 字节那种则按引用传）。裸 `{i32,i32}` 在 LLVM 里会占
**两个**寄存器，于是 8 字节的 `TestS` 会把后面的 `n32` 挤到下一个寄存器 ——
症状是「**前面**那个值读错了」（`ss.first` 读出 `structT.width`），而不是类型
错误。修复见下面 `## 符号与 ABI 约定` 里「direct 的聚合要按目标 C 规则」那条。

## 两条验证路径

### `run_vyx.sh` — Vyx 消费方真路径（主编译器门）

`.dci` → `.dcib` 编码 + 严格校验 → `vyxc.exe build`：编译器在 mono 期把每个
调用点的闭合实例记录到 `<obj>.dci_open` sidecar（`open <符号> <路径> <返回>
<类型实参…>`），build_system 合并所有 sidecar 为 requests 文件，emit 阶段以
`--requests` 连同 `--descriptor` 一起交给 external DCI stub backend
（`tools/dci/stub_backend.py` —— 协议只有这一份实现；项目在 `Vyx.toml` 里用
`--provider` 声明自己链接哪个生产端，语言跟着文件扩展名走）。

backend 按 provider 源开 `RustActiveSession` 或 `CppActiveSession`，把闭合请求
（契约里带 `semantic_id` 的实例方法符号在**两端**也一并交给会话）合成一次
`resolve_batch`——生产端探针裁决边界，不满足即以 producer 诊断拒绝构建——按
facts 生成单态化 stub（rust：`#[no_mangle] extern "system"`；cpp：自由函数
`extern "C" auto … -> decltype(…)`，方法则是 force-use 体 + volatile keep），
再由 rustc / clang++ 编译并自动链接。

门的断言（按生产端分别取 adapter 日志，靠日志里的 `[dci-active] <lang>` 定位）：

- rust：≥13 CLOSED（3 个闭合符号 + 全部开放泛型实例 + 实例方法物化，全部经 rustc
  裁决）；
- cpp：≥34 CLOSED（开放泛型模板 + 12 实例 × get_first/swapped 的物化请求，经
  clang++ 裁决）+ 3 CONTRACT（闭合契约符号，非模板，会话不裁决，ABI 由契约直接
  给定，存在性由 clang++ 编译时检查）；
- 两个日志里都必须逐个出现 `twice__i32` / `twice__f64` / `max_of__i32` /
  `identity__*` / 全部 `swap__*` 组合 —— 只数总数会放过「少闭合一个」；
- 两份 `.dci_open` sidecar 逐字节相同（两份源多的成员函数调用 —— 各实例的
  `get_first` 与双方的 `v.norm1()` —— 都不是开放泛型请求，所以不许改变请求面）、
  且行数 ≥10（5 个原有闭合 + 5 个新组合的 swap 闭合）；
- cpp 契约里必须有 `?norm1@Vec2@@QEBANXZ`（非泛型，adapter 自动导出）与各实例的
  `dci.active.cpp.Pair2::*` 物化请求（`link_name` 仍是 MSVC mangled 成员符号）；
- rust 契约里必须有 `vyx_vec2_norm1`（同一个非泛型方法，稳定链接名）与各实例的
  `dci.active.rust.Pair2::*` 物化请求；非泛型方法那条的 semantic_id 是
  `dci.active.rust.Vec2::norm1()` —— **实参列表里不许有接收者**，这条断言把
  「请求面」那半边钉死；
- rust 契约的 `rejected_symbols` 必须**恰好**是 4 个开放泛型函数（twice/max_of/
  identity/swap —— 它们由调用点闭合，不属于静态契约），cpp 的必须是空。
- **provider 侧就绪**：`llvm-nm` 在构建产物里找到成员符号（`norm1` + 各实例的
  `get_first`/`swapped`），并断言「检查了几个对象、读了几行符号」—— 0 匹配必须是
  「看过并且没有」，不是空断言；rust 侧还单独断 `vyx_vec2_norm1` 在（它不在
  provider 源码的契约符号里，靠构建期 UFCS shim 才活下来）；
- **消费方单态化**：两个端到端消费方源码（`src/open_generic_cpp.vyx` 与
  `src/open_generic.vyx`）单文件直编必须过，MIR 里要有
  `owner_type=Pair2::<f64,i32>`、`block_count=0`、`flags=172` 的无体实例；编到目标
  文件后 `llvm-nm` 必须看到对 `?get_first@?$Pair2@NH@@QEBANXZ`（cpp）/
  `vyx_pair2_f64_i32_get_first`（rust）的**未定义**引用，以及非泛型方法的
  `?norm1@Vec2@@QEBANXZ` / `vyx_vec2_norm1`（rust，`CONSUMER ok3`）；
- **契约边界负例**：`src/unsupported/missing_contract_method.vyx`（provider 不提供的
  `tag`）必须以 I0100 `no matching symbol ... member=tag, owner=Pair2::<f64,i32>)`
  失败 —— 且**恰好** 4 errors + 2 notes，并且**不得**再出现 mono 的 `unresolved call`。
  这一条**必须用 `--emit=obj`**：mono 修好之后 MIR 这一层见不到这个失败（实例照旧
  物化），用 `--emit=mir` 跑会拿到 rc=0，门就静默变绿了；
- 两个 exe 都要跑出 `dci_opengeneric OK`；cpp 那个 target 自己的 `crate_*.obj` 上还要
  再查一次对 `get_first` 的未定义引用（排除「调用点被优化没了」）。

### `run.sh` — Python 协议面（adapter 直驱）

`run_demo.py` 双语言各跑 open-session → resolve_batch → materialize → 单飞发布
→ 离线重放（只凭 bundle 链接 C driver）→ 失效，golden 逐字节比对
（`expected_output.txt`）。**请求面两边完全一致**（同样的 6 个闭合 + 1 个被拒），
只有 `op` 不同（`rust/call/1` 与 `cpp/call/1`）；golden 里两段的 CALL/REPLAY
行因此逐行对得上，可以直接看出「同一个开放泛型在两个生产端下的同一结果」。

被拒的那一项是 `twice<Vec2>`：

```
rust: error[E0277]: cannot add `Vec2` to `Vec2`
cpp:  error: invalid operands to binary expression ('Vec2' and 'Vec2')
```

同一个请求、两种生产端语言各自的诊断，证明裁决来自生产端编译器而不是请求面。

## 符号与 ABI 约定

- 实例符号 `<base>__<类型实参…>`（每实参 policy_mangle_part，`_` 连接），
  由编译器在 sidecar 里点名，backend 原样导出，无 `mir2$` 掩码。
- 实例 ABI 由编译器按 MIR 类型合成（契约等价 JSON）：指针/≤8 字节 direct；
  record >8 字节参数 indirect+by_value、返回 sret。与 closed 契约对同一
  类型声明的 lowering 一致。
- **direct 的聚合要按目标 C 规则**：Win64 只有**恰好** 1/2/4/8 字节的聚合走单个
  整数寄存器（`struct {float,float}` 与 `struct {double}` 都是 `i64`，**不是**
  XMM），其余尺寸（3/6 字节等）按引用传、返回走 sret。这个强制是**生产端的
  编译器**（clang / rustc）替它做的，消费方必须做同一个 —— 否则裸 LLVM
  `{i32,i32}` 参数会占**两个**寄存器，把后面所有实参挤位（症状是**前面**某个
  值读错，而不是类型错误）。所以开放泛型实例在 Win64 上也走 C 聚合规则：
  `c_sysv_coerce_llvm_type` 多了 Win64 分支（整数类，不走 SSE 分支），
  `dci_open_lowering_passing` 把非 1/2/4/8 字节的聚合改判 `indirect`，
  实参侧的强制在 `dci_open_direct_param_uses_c_coerce`。门里断言的正是
  这个寄存器布局：8 字节 `TestS` 在 RDX、`i32` 在 R8、sret 在 RCX。
- record 类型实参：provider 侧需 `#[repr(C)]`。具体 record（`Vec2`）两边同名就够；
  泛型类型的**实例**则写**消费者拼写**（`Pair2<f64,i32>`），不是 provider 的
  `Pair2<double, int>`（cpp）或 `lib.Pair2<f64, i32>`（rust，带 crate 前缀）——
  匹配键对不上就找不到布局（`missing DCI ABI lowering` /
  `requires a stable or transparent value representation`）。

## 已知边界（Phase 1）

- 类型实参支持 primitive、provider 内 `repr(C)` record（`Vec2` 已实测），以及
  **消费方自己声明的 `@[repr(C)]` record**（`TestS` / `TestF`，见上面「消费方声明
  的 record 作类型实参」一节）；引用、嵌套**泛型**实参
  （`Pair2<Pair2<i32,f64>,i32>`）待扩展。
- 消费方声明的 record 只覆盖 `@[repr(C)]`（布局即 C 布局）。带 8 字节对齐下限的
  普通 Vyx class 不在这一档 —— 它的布局和生产端的 `struct` 不是同一个，推导不出
  诚实数字，所以拒绝而不是猜。
- **泛型类型（泛型 record）可以过边界**，见上一节：`swap<T,U> -> Pair2<U,T>`
  闭合成 `Pair2<f64,i32>`，codegen 走
  `call void @swap__i32_f64(ptr sret(%"mir.struct.Pair2::<f64,i32>") align 8 %6, i32 21, double 1.5)`，
  `sw.first` / `sw.second` 按契约 layout 的 offset 读。前提是契约里有那条**实例**
  布局 —— 它现在由 adapter **实测**：消费方发现、闭合器点名给生产端的实例
  （cpp/rust 同构，provider 源码零显式实例化）会进 sizeof 探针与布局闭包，
  clang 的 `-fdump-record-layouts` 给出真实的 size/align/offset，所以**同一
  泛型类型的多个实例是自然覆盖的**。
  `dci/instance_layouts.json` 因此只剩两件事：provider→consumer 的**拼写映射**，
  以及跨语言布局契约（与 rustc / clang++ 各自实测逐字段交叉核对，对不上即失败）。
- **任意类型组合矩阵已实测**（2026-09-18）：f32/f64、i32/i64、u64/u32、char/i64，
  以及 **record 作类型实参**（`swap(v, 21) -> Pair2<i32,Vec2>`，24 字节嵌套聚合，
  Vec2 在 offset 8，`vyx_vec2_x(sv.second)` 把嵌套 record 字段原样带回来）。
  没有一张「支持类型」的硬表 —— 每个组合的布局由 rustc / clang++ 各自实测，
  约束（`Copy` / trivial copy）由 rustc 裁决，不满足就拒绝。char 的双端事实：
  DCI `char` 是 1 字节、C++ `char` 是它的拼写（semantic_id 实参也写 `char`），
  rustc 的 `char` 是 4 字节标量 ——
  merge/verify 只比对（字段名, offset），两侧 size/align/offset 一致即契约成立，
  ASCII 值语义一致。
  尚未覆盖：**嵌套泛型实参**（`Pair2<Pair2<i32,f64>,i32>`）。
- **record 的成员函数分两档**：
  - **非泛型** record 的成员函数已通，**两个生产端都是**：`Vec2::norm1` 的方法符号
    由 adapter 自动导出（`kind=method`、`owner=Vec2`，两端也都带 semantic_id 物化
    请求），消费方 `v.norm1()` 取接收者地址当 `this`。两端各按自己产物的 ABI 命名
    （cpp `?norm1@Vec2@@QEBANXZ`、rust `vyx_vec2_norm1`），门里两侧各有正例
    （契约侧 + provider 目标文件 + 消费方目标文件的**未定义**引用）。
    > rust 侧这条曾经根本走不通，根因是 **adapter 的两处缺陷**，与语言能力无关：
    > ①**表示形状**：active-request 分支把接收者当普通首参导出（`kind=function`、
    > 形参名叫 `&Vec2`、`abi.parameters[0]` 没有类型），于是一路撞
    > `I0100: DCI Descriptor has no matching symbol for external declaration
    > 'norm1' (kind=method, ...)` -> `missing DCI ABI lowering`。修法是照
    > stable-link 分支补齐 `abi.receiver`（`passing=direct`、`type.reference=
    > pointer`）、`owner`/`member_name`，并把 `is_const` 取成「接收者不是
    > `&mut self`」—— 编译器**按 const 性匹配方法**，`&self` 报成
    > `is_const=false` 一样匹配不上。②**请求面**：semantic_id 的实参列表把接收者
    > 也拼了进去（`dci.active.rust.Vec2::norm1(&Vec2)`），生产端于是去找一个名叫
    > `&Vec2` 的 record（`representation_incompatible: record '&Vec2' does not
    > exist in the producer crate`）。接收者是方法查询的**隐式**部分，必须从
    > 实参列表里去掉。两处修好后，非泛型 impl 方法经 `_find_generic_method`
    > 走方法查询、由 `_close_method_request` 生成 UFCS shim。
  - **泛型** record 的成员函数已通：`get_first()`（返回类型是泛型参数 `A`）按实例
    单态化，semantic_id 物化请求由 clang++ / rustc 现场发射 —— 见上面「契约侧的
    符号从哪来」一节。
  - 返回**另一个实例**的方法（`swapped() -> Pair2<B, A>`）也已通。它曾经被
    adapter 以 `value return type 'Pair2<int,double>' has no verified nonzero
    layout` 拒掉（契约里两条 `rejected_symbols` 就是这么来的）——注意**这不是
    名字有歧义**：那两条拒收条目各自带着完整精确的 mangled 名
    （`?swapped@?$Pair2@NH@@QEBA?AU?$Pair2@HN@@XZ`）。缺的是"按值返回的**第二个
    实例**没有布局记录"，而布局闭包刻意不含实例名。现在实例布局由 adapter 实测
    （`Pair2<i32,f64>` 因此进入契约），消费方 `sw.swapped()` 的 MIR 返回类型解析
    成第二个实例、目标文件留下 `U ?swapped@?$Pair2@NH@@QEBA?AU?$Pair2@HN@@XZ`，
    门里有正例（`CONSUMER ok2`）。rust 侧对称：
    `src/open_generic.vyx` 的目标文件留下 `U vyx_pair2_f64_i32_swapped`
    （`CONSUMER ok (rust fixture)`），且实例布局矩阵里每个 `swapped` 返回侧实例
    在两端都已实测。
- **tuple 返回也可用**（2026-09-18 修复），但真跨语言时具名 record 更结实：

  `fn swap<T,U>(a:T,b:U)->(U,T)` 闭合出 `swap__i32_f64`，codegen 走
  `call void @swap__i32_f64(ptr sret(%"mir.tuple.(f64,i32)") align 8 %6, i32, double)`，
  `sw.0` / `sw.1` 直接可读。字段顺序是**约定**而非巧合：`(f64, i32)` 要求
  f64@0、i32@8，而 Rust 裸 tuple 的布局是未规定的（`#[repr(Rust)]` 可以重排）。
  两个 provider 因此都不返回裸聚合，而是返回 `#[repr(C)] Pair2<A, B>` ——
  让顺序写进契约，而不是靠两边恰好一致，这正是它现在返回泛型类型的原因。

  > 修复前的实测：`open swap__i32_f64 swap (f64,i32) i32,f64` 能进 sidecar，
  > 但签名被降级成 `declare ptr @swap__i32_f64(i32, double)`，`sw.0` 报
  > `I0100: lower_value_as produced null`。根因不在 DCI：`hir_type_table.intern_name`
  > 把一切按 `ty_CLASS()` 登记，泛型实例化产生的 `(f64,i32)` 也成了 CLASS，
  > 而 tuple 的元素与布局全靠文本推导 —— 于是
  > `class aggregate has fields but no record layout for (f64,i32)`。
  > 纯 Vyx 的 `fn mk2<T,U>(a:T,b:U)->(U,T)` 同样失败，可证明这一点。
- C++ 会话只解析**函数模板**。闭合契约里的 `vyx_vec2_make` / `vyx_vec2_x` /
  `vyx_vec2_y` 不是模板，因此 cpp 目标下不交给会话裁决：契约已经钉住它们的
  ABI，provider 自己就以 `extern "C"` 导出这三个 link name，backend 只打一行
  `CONTRACT` 日志（早先版本会另发一层 `decltype` 转发壳，现在那层是重定义，
  已去掉）；rust 目标下它们照旧交给 `resolve_batch` 裁决。
- provider 函数的类型变量按「签名中首次出现顺序」与请求的实参对位
  （`max_of<T>(a:T,b:T)` + `[i32]` → `[i32,i32]`）。cpp 侧的形参类型文本由
  backend 的小型模板扫描器读出，只覆盖本项目 provider 用到的写法
  （`T v` / `double x` / `Vec2 v`；`const T&`、默认值、含逗号嵌套的形参类型未覆盖）。
