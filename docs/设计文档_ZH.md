# Vyx 语言设计规范

[English: Language design](LANGUAGE_DESIGN.md) · [文档目录](README.md) ·
[语言表面](语言表面_ZH.md) · [编译器架构](COMPILER_ZH.md)

**依据：** 当前 `bootstrap_compiler/` 自举编译器源码。
本文按 `src/core/lexer.vyx`、`src/core/policy.vyx`、`src/core/parser.vyx`、
`src/core/sema.vyx`、`src/core/diag.vyx`、`src/core/main.vyx`、
`src/hir/hir_builder.vyx` 整理。早期 C++ Host 的版本清单与示例不代表当前编译器行为。

教程提供可运行示例；[编译器架构](COMPILER_ZH.md)说明 HIR、MIR 与 LLVM 的处理位置。
若文档与从当前源码新构建的 SDK 编译器行为冲突，应按编译器与测试修正文档。

## 设计不变量

1. 可观察的 `drop` / `defer` / panic / FFI / DCI 调用不得当纯表达式删掉。
2. 布局、packed ABI、外部对齐优先于优化便利。
3. 更高优化级不得改变 `O0` 语义。
4. 未证明的 alias / lifetime / effect 一律保守。
5. MIR2LLVM 是验收后端；`--emit=cpp` 为实验。

## 关键字（`policy.vyx` `policy_keyword_kind`）

保留字：`fn` `if` `as` `is` `in` `let` `var` `use` `for` `mut` `asm`
`true` `enum` `impl` `elif` `else` `case` `null` `self` `task` `fail` `type`
`when` `bool` `char` `void` `false` `class` `error` `while` `match` `break`
`yield` `async` `await` `const` `defer` `bench` `trait` `where` `macro`
`isize` `usize` `struct` `import` `module` `export` `extern` `return`
`public` `static` `unsafe` `rawptr` `default` `private` `newtype` `concept`
`foreach` `continue` `internal` `override` `comptime` `protocol` `requires`
`volatile` `interface` `protected` `static_assert`，以及整型/浮点宽度
（`i8`…`u64`，`f32` `f64`）。

`new` **不是**关键字（kind 5 = 标识符）。`interface` / `trait` / `protocol`
共用 kind **11**。`string` / `str` 是原语**类型名**
（`policy_primitive_type_kind` → 13），不是关键字。

## 词法（`lexer.vyx`）

- 空白；`//` 行注释；嵌套 `/* … */`；`///` 文档行挂到下一个 token
  （`skip_whitespace_and_comments` / `flush_pending_doc_to_last`）。
- 整数：十进制、`0x`/`0X`、`0o`/`0O`、`0b`/`0B`。可选后缀 `i8`…`i64` /
  `u8`…`u64` / `isize` / `usize`（`consume_int_suffix`）。**拒绝**浮点后缀。
  数字里的下划线不是契约。
- 字符串：`"…"`，转义 `\n \t \r \\ \" \' \0 \$`。普通字符串里的 `${expr}`
  会变成插值（`parser_raw_string_has_interpolation`）。`f"…"` 是
  `tk_FSTRING_RAW`。`"""…"""` 是多行字符串。
- 字符字面量 `'x'`（ASCII 快路径）。`'ident`（后面不是 `'`）是标签 token，不是 char。
- `true` / `false` 在关键字查找之前就是 bool 字面量。

## 类型（`parser.vyx` `parse_type`）

原语：`i8` `i16` `i32` `i64` `isize`，`u8` `u16` `u32` `u64` `usize`，
`f32` `f64`，`bool`，`char`，`str` / `string`（同一 `ty_STRING()` kind），
`rawptr`，`void`。

类型构造：

| 语法 | 含义 |
|---|---|
| `&T` / `&mut T` | 共享 / 独占借用 |
| `*T` | 带类型指针；成员用 `p->field` |
| `[T; N]` 或 `[N; T]` | 定长数组 |
| `(T, U, …)` / `()` | 元组；`(T)` 只是分组，不是 1 元组 |
| `fn(…) -> R` | Vyx 函数类型 |
| `unsafe fn(…) -> R` / `cfn(…) -> R` | C 函数指针（`parse_fn_type_ref_after_kw(true)`） |
| `dyn Trait` | trait 对象（`dyn` 是标识符前缀，不是关键字） |
| `Path::<Args>` | 泛型类型；类型路径可用 `::` 或 `.` |
| 形参/类型上的 `…` | C 变参 / pack |

`T?` **会被拒绝**（`finish_type_suffix`：「nullable type suffix requires the
standard library option type」）。用 `Option<T>`。

`type Name = T;` 与 `newtype Name = T;` 都会解析（`parse_type_alias_decl`）。
关联 `const` 和关联 `type` 写在 `trait` / `interface` / `class` / `enum` 上。

内建类型查询（sema）：`sizeof::<T>()`、`repr_sizeof::<T>()`、
`alignof::<T>()` → `i64`。

## 声明（`parse_decl`）

项关键字之前的前缀标记：`export`、`async`、`comptime`、`unsafe`、`task`，
以及拼写 `coroutine` / `corountine`。可见性之后：`static`，然后
`override` / `virtual` / `final`。

| 形式 | 解析入口 |
|---|---|
| `module a.b;` / `module a.b { … }` | `parse_module` |
| `use path;` / `import path;` / `use Alias = path;` / `use path as Alias;` | `parse_use` |
| `use` / `import` 的版本后缀 `@1.2["variant"]` | `parse_version_selector_suffix` |
| `fn` / `public fn` / `static fn` | 自由函数与方法 |
| `struct` | `DE_CLASS`，**只有字段**（`parse_struct`；体内无方法） |
| `class` | 字段、`fn` 方法、`ClassName(…)` 构造、`const` / `type` 成员 |
| `enum` / `error` | 可选 `(payload)` 的变体；`error` 里也可以有 class 成员 |
| `trait` / `interface` / `protocol` | 同一关键字槽 |
| `concept` | 按 interface-like 解析（`\|kind:interface\|kind:concept`）；**不是**课内约束语言 |
| `impl Trait for T` / 固有 `impl T` | `parse_impl_decl`；可选 `<T: Bound>` 和 `where` |
| 模块或局部的 `let` / `var` / `const` | 绑定 |
| `type` / `newtype` | 别名 |
| `public` / `@[vis(范围)]` | 可见性；显式范围优先于 `public` 简写 |
| `override fn` | 替换父方法 |
| `class Child : Base` / `struct V3 : V2` | `parse_decl_suffix_markers` 的 `|impl:` |
| `extern "C" { … }` | C 函数、`type`/`newtype`、`let`/`var` |
| `extern "C++"` / `extern "dci" { class/struct/fn/… }` | **DCI 面**（`extern_abi_uses_dci_surface`），不是第二套语言 FFI |

函数与成员：

- 参数：`name: T`，默认值 `name: T = expr`（初值挂在形参 decl 上），变参
  `...name: T` / `name: ...T`，尾部 C 变参 `...`（`__c_varargs`）。
- 接收者：无标记 `self`（C++ `this` 逃逸）、`&self`、`&mut self`、`own self`（Rust `self`）、`mut self`（Rust `mut self`）。`own` 只作为连体 `own self`。
- 返回 `-> T`。函数**体**没有最后表达式隐式 return，必须写 `return`。
  `if` 表达式的块允许最后一条表达式不写 `;`（`parse_if_expr_block`）。
- `operator +` / `operator[]` / `operator++` / …（`parse_operator_decl_name`）。
- 泛型参数 `<T, const N>`（`N` 存成 `N^const`）。`where` 和 `requires` 在
  `fn` / 方法 / 构造上是同一段约束文本。
- interface / trait 方法可以有 `{ body }`（默认方法）或 `;`。
- 构造：class 体内的 `ClassName(args) { … }` 是名为 `constructor` 的
  **static** 方法（`parse_constructor`）。调用点不传 `self`；函数体仍绑定
  `self`（HIR 构造 ABI self）并可以写字段。落到函数末尾则返回该 `self`。
- 字段 `name: T [= expr]` — 下一 token 明显是下一个成员或 `}` 时 `;` 可省略。
- `@[…] { …decls… }` 把属性套到块内每个声明（`parse_attributed_decl_block`）。
- 仅 DCI：`~Owner();` 是析构声明，存成方法 `drop`。

## 表达式（`parse_expr` 链，低 → 高）

```
赋值            =  += -= *= /= %= <<= >>= &= |= ^=
三目            cond ? a : b          （右结合；C 分组）
空合并          ??
区间            ..  ..=
管道            |>
逻辑            ||  &&
位              |  ^  &
比较            == !=  < <= > >=
移位            << >>
加减乘除        + -   * / %
转换            as T     （`&n as *T` 是 `(&n) as *T`）
一元            !  -  ~  ++ --  *  &  &mut  own  try  await  comptime  new  delete
后缀            调用  .成员  ::路径  ->成员  [下标]  {字段}  ?  ++ --  ...折叠
主键            字面量  标识符  (expr)  if 表达式  闭包  插值串
```

另外会解析：

- 插值字符串：`"… ${expr} …"` 和 `f"…"`（`parse_fstring_inner`）。
  字符串里单独的 `{expr}` **不是**插值。
  插值表达式可包含字符串字面量，如 `"${s.contains("X")}"`；
  兼容旧写法 `"${s.contains(\"X\")}"`。字面量内的花括号不结束插值，
  字符串自己的转义仍按普通字符串处理。
- `if (cond) { e } elif … else { e }` 作为**表达式**（`parse_if_expr`）。
  条件**必须**写 `(…)`。
- 闭包：`fn (x: T) -> R { … }`、`|x: T| { … }`、`|| expr`，以及
  `[cap, &r](x) => { … }`（`parse_primary`）。
- 命名实参：`f(name: expr)`（`parse_call_args`）。
- 结构体/类字面量：`Name { field: expr, … }`（`parse_struct_init`）。
- 数组字面量：`[e, e, …]` 和重复 `[elem; n]`（`ex_ARRAY_LIT`）。
- 元组字面量：`()` 和 `(e, e, …)`。单独的 `(e)` 只是分组。
- turbofish：`Foo::<T>`；`Foo@1.2["v"]` 版本选择。
- 后缀 `expr?` 仅当下一 token 是终结符（`;`、`,`、`)`、`}`、`]`、EOF）；
  否则 `?` 是三目。
- 前缀 `try expr` 与 try-`?` 同一节点。
- 绑定：`let` 只读拥有，`var` / `let mut` 可变拥有。`&T` / `&mut T` 与方法接收者同一套借用神谕。`unsafe { }` 逃逸 E3100/E3101。
- 上下文 `new T(...)` / `new T { … }` / `new::<T>(…)` / 可选 allocator 组；
  上下文 `delete` / `delete::<T>(allocator) p`。
- 数组分配花括号初始化：`new [T; N] { … }`。
- ident 上的 pack 折叠：`xs...+` / `...-` / `...*` / `...&&` / `...|`。
- `fail Type.Variant` / `fail Type.Variant(payload)` /
  `fail Type.Variant.with(msg)`（`ex_FAIL`；sema 按当前返回类型给它类型）。
- `asm("template", …)` → `ex_INLINE_ASM`。Sema 给类型；MIR 降成
  **intrinsic**，不是有保证的汇编方言。不是课内契约。
- 主键位置的 `sizeof...()` 会被改写成整数 `1`（pack 占位）。

任一端是 string-like 时，`string + T` 走拼接（`resolve_bin_op` + `str_concat`）。

`match` 也可以产生值：`let result = match value { case 0 => 10,
default => 20, };`。被匹配值的括号可省略；分支支持枚举载荷绑定、布尔 guard、
嵌套表达式，以及以表达式结尾的块（`{ let x = 1; x; }`）。被匹配值只求值一次。
产生值的分支类型必须相容；缺少值、空 match 或非布尔 guard 会报告 E1000。
兜底分支使用 `default`；裸 `_` 分支尚不支持，`Some(_)` 等载荷模式可用。
当前不承诺对所有模式完成穷尽性证明。用法见[第 14 课](进阶教程_ZH.md#第14课match)，
验证见 [AOT 回归门](../probes/gates/match-expression/README.md)。

## 语句（`parse_stmt`）

`let` / `var`（可选 `let volatile` / `var volatile` — 局部 flag，不是课内特性）、
`if (cond) { … }` / `elif (cond)` / `else`（**必须有括号**）、`while (cond) { … }`、
C 风格 `for (init; cond; incr)`、`for x in xs` / `for (x in xs)` / `foreach`、
`match` / `case` / `default`（可选 `if` 守卫；`=>` 或 `:`）、带可选 ident 标签的
`break`/`continue`、标签 `name: stmt`、`return`/`defer`（块或单条语句）、
`yield expr;`（存成 `ex_AWAIT`；`yield return` 就是 `return`）、
`unsafe { … }`（块 flag `stmt_flag_unsafe_block`）、
`static_assert(cond [, msg…]);`、块 `{ … }`。

没有 `{` 的 `unsafe` 走 `parse_skip_stmt`（吃掉后换成 null 表达式）——不是契约。

## 所有权（`sema.vyx`）

- 堆所有权按值赋值是 **move**（E3100）。之后再用源是错误。
  `type_is_heap_owning` 把 class `string`/`String`、`Vec`/`Dict`/`HashMap`/
  `HashSet`/`Set`/`Stack`/`Queue`/`Deque`/`List`/`PriorityQueue`/`BTreeMap`/
  `BTreeSet`、`Box`/`Ref`/`Scope`、带实例 `drop` 的类型、以及嵌套堆字段当成 owning。
- `type_is_copy`：整型/浮点/bool/char、`rawptr`、`cfn`/`fn`、以及原语
  `ty_STRING()` 是 Copy。`&T` / `&mut T` / `*T` 不是。类 `string` / `String`
  **不是** Copy。其它 class 需要 `impl Copy` / `@[derive(Copy)]`
  （`type_satisfies_trait`）。
- 共享借用存活时再取 `&mut T`、或移动仍被借的局部，是 **E3101**。
- `defer { }` 在作用域退出时倒序执行。类 `drop()` 是语言清理钩子；
  `Vec.destroy()` 是库的缓冲区 API，不被 `drop()` 替代。

`Ref` / `Weak` / `Box` 是 **std.ref** 类型，不是编译器内建 `makeRef`。

## 内建方法（`policy.vyx`）

原语 `string`：`len`/`size`/`count`、`ptr`、`c_str`、`clone`、
`charAt`/`char_at`/`charCodeAt`、`substr`/`substring`、`trim`、`toUpper` /
`toLower`、`replace`、`contains`、`isEmpty`、`startsWith`/`endsWith`、
`repeat`、`toString`、`equals` / `operator==`。

`*T` / `&T` / `&mut T` / `rawptr`：`offset`/`add`/`sub`（按元素步长）、
`byte_offset`/`byte_add`/`byte_sub`、`offset_from` / `byte_offset_from`。

sema 当 host builtin、不当未定义的自由名（`is_host_builtin`）：
`print`/`println`/`format`、`panic`/`assert`/`assert_eq`、
`sizeof`/`repr_sizeof`/`alignof`、`typeof`/`typeinfo`/`type_name`、
`field_count`/`field_name`/`field_type`、`hash`/`compare`/`operator==`、
`argCount`/`getArg`、`malloc`/`realloc`/`free`/`memcpy`/`memmove`/`memcmp`/
`memset`、`strlen`、`to_rawptr`/`ptr_offset`/`ptr_read_*`/`ptr_write_*`、
`from_cstr`/`from_cstr_len`/`from_cstr_view_len`/`from_raw_string_parts`、
`int_to_string`/`bool_to_string`、`construct`/`alloc`/`dealloc`/`transmute`、
`reflect_*`、`ct_file_size`。出现在这张表里不是教程承诺；课内内建是 `print` 和 `panic`。运行时反射课内表面是 `use std.reflect`（`Type.of` / `getType` / `Instance`），不是 `reflect_*`。

## 诊断（`diag.vyx`）

结构化码包括 `E0001`/`E0002`（语法）、`E0101`–`E0104`（词法）、
`E1000` 类型不符、`E1100` trait bound、`E1200` 控制流、`E2000` 未定义、
`E2100`/`E2101` 重复/重定义、`E2200` 无重载、`E2300` 歧义、`E2400` 版本策略、
`E3000` 泛型、`E3100` move、`E3101` 借用、`E3200` mono clone 深度、
`I0001`/`I0100` 内部/codegen。

## 驱动（`main.vyx` `show_usage` / `main`）

`main` 里接上的命令：`--src=file|project`、`build`、`new`、`dci`、`run`、
`repl`（`vyx_bootstrap_repl_run`）、`install`/`publish`/`search`/`lock`
（`pkg_dispatch`，`file://` registry）、`test`、`bench`、`fmt`、`doc`、
`version`、`help`。
`--emit=`：`ir obj exe dll lib vyi cpp dci-stubs hir mir hir-verify
mir-verify hir-facts`。LLVM 是验收后端；`cpp` 是实验。

`test` / `bench` / `fmt` / `doc`（`cli_tools.vyx`）：

- `test [--filter] [path…]` — 在 `tests/`（或显式路径）里找 `@[test]`
  函数；文件没有 `main` 时包一层；用当前 SDK 编译器 `--src=file --emit=exe`
  编译并运行。任一失败则非 0。
- `bench [--filter] [path…]` — 同样对 `benches/` 下的 `@[bench]`；打印
  墙钟毫秒。`bench` **关键字**仍未接线。
- `fmt [--check] [path…]` — 按 token 重排 `.vyx`。`//` 和 `/* */` 会被丢掉
  （词法器不保留）；`///` 会写回。`--check` 若文件会变则退出 1。不是 rustfmt。
- `doc [--out <dir>] [path…]` — 从紧挨着的 `fn`/`class`/`struct`/`interface`/`enum`
  上的 `///` 出 markdown。默认 stdout。

## 属性

解析器把 `@[…]` / `@ident` 存进声明 meta（`collect_attribute`）。Sema / HIR
至少认：

| 属性 | 作用 | 位置 |
|---|---|---|
| `@[vis(范围)]` | `world` / `self` / `mod` / `super` / `tree` / `package` / `in(...)` / `friend(...)` / `none`；`+` 合并、`&` 交集、`-` 排除；`public` 是 `world` 简写 | `sema_decl_visibility_expr` / `sema_vis_expr_allows`；[第八课](入门指南_ZH.md#第八课类与方法) |
| `@[platform("windows"\|"posix"\|"linux"\|"android"\|"macos")]` | 保留或丢掉声明；`posix` = linux\|android\|macos；宿主别名 `win32`/`msvc`、`darwin`/`apple` | `sema_platform_token_matches_current` |
| `@[derive(Eq, Ord, Clone, Copy, Hashable, Debug, Display)]` | 合成 impl；`Hash` 等同 Hashable；字段都满足 Copy 时 Copy 为标记 | `host_analyze_synthesize_derive_methods` |
| `@[async]` | spawn 无栈 Future worker（`vio_sched_spawn`）；MIR await 拆分（`poll` + `mir_term_yield`）；`Task.sleep` 定时器 | sema |
| `@[comptime]` | `sema_ct_eval_known` 成功才折叠（整/bool/char、一元二元三目、循环、`print`/`ct_file_size`、`@[comptime]` 调用） | `sema_try_fold_comptime_call` |
| `@[no_mangle]` `@[export]` `@[export_name]` `@[link_name]` | C ABI / 符号名 | `sema_decl_is_c_abi_fn` |
| `@[repr(C)]` `@[packed]` `@[align(N)]` / `repr(… packed … align(N))` | 布局标志 | `hir_builder_decl_has_repr_c` |
| `@[borrow_args]` | 编译器内部；跳过部分 move 检查 | 编译器源 |
| `@[version("x.y.z")]` `@[variant("tag")]` | 模块身份 `Name@x.y.z["tag"]` | `policy_module_identity_from_meta` |
| `@[migrate(fromVer, fromField?, fromSig?, desc?)]` | 版本化 API/字段迁移 | `sema.vyx`（见下节） |
| `@[discard(fromVer, desc?)]` | 当前拼写不可调用；要用 `@fromVer` | `report_discarded_callable` |

未知属性只是被存下来，不是第二套语言。没有探针就不要把 `@[inline]` 写成优化契约。

## 版本化模块与 migrate

模块可以带 `@[version("1.0.0")]` 和 `@[variant("_1")]`。身份是
`Base@version["variant"]`（`policy_module_identity`）。调用和成员选择器用
`parse_version_selector_suffix` 已经解析的 `@ver["variant"]` 后缀。

带 `fromVer` 的 `@[migrate(...)]` / `@[discard(...)]` 会排队导入那个历史模块
（`sema_queue_migration_sources_decl_chain`）。Sema 把同 base、同一 variant
的身份当成一个版本族（`sema_versioned_module_pair`），保留最新 class，并
合并历史（`sema_merge_versioned_class_members`）。

**字段**（`sema_field_migrate_*`，`hir_builder_field_migrate_from_field`）：

- 字段上 `@[migrate(fromVer="1.0.0", fromField="n")]`：`fromField` 是**同一存储槽**
  的查找别名。旧拼写不得变成第二个 HIR/MIR 字段。
- `fromField` 只对 `de_FIELD` 合法，必须同时有 `fromVer`。历史版本里必须存在
  那个源字段（否则 **E2400**）。两个当前字段不能从同一版本迁移同一个源字段。
- 没有 `fromField` 的 `@[migrate(fromVer="…")]` 表示**新增**字段，不会因为
  名字碰巧相同就吃掉历史槽。

**可调用项：** `fn` / 方法 / 构造上的 `@[migrate(fromVer=…)]` 会加载旧模块，
让历史重载仍然可见。默认查找偏向新版本；`name@1.0.0["_1"](…)` 选旧的。
`@[discard(fromVer=…)]` 让当前名字变成 **E2400**，除非调用方写历史选择器
（帮助：`name@fromVer["variant"](…)`）。

测试里会出现 `fromSig=`、`desc=` 属性文本。Sema **不读** `fromSig`
（`bootstrap_compiler/` 里没有 `policy_named_argument_value(..., "fromSig")`）。
重载身份是版本合并后的真实声明，不是签名字符串匹配器。

探针：`tests/projects/versioned_module_import`、
`versioned_field_migrate_diagnostic`、`versioned_module_discard_diagnostic`。

## 模块、包、FFI

- `public` 枚举、常量和可变全局量可以跨模块消费。导入定义模块即可；
  `use Status = Cross.GlobalEnum.Status;` 这样的别名仍保留枚举身份。
  [跨模块回归门](../probes/gates/cross-module/README.md) 覆盖独立编译生产模块的
  枚举变体，以及整数和字符串全局量。
- 用户泛型模块生成的 `.vyi` 接口携带版本化模板制品，消费方可以在没有生产端
  源文件时实例化函数体。定义模块身份和私有辅助依赖随制品保留；普通消费代码
  仍不能直接访问私有辅助项。损坏或不支持的制品会被拒绝，见
  [泛型接口回归门](../probes/gates/generic_interfaces/README.md)。
- 应用侧导入写 `use std.collections;`（`use` 也接受 `.` 分隔）。
  `import` 解析成同一个 `DE_USE`，路径用 `::`。
  `use path::*` / `import path::*` 在 `*` 处停下（`parse_import_path`）。
- 项目是 `Vyx.toml`（`PACKAGE_MANIFEST_ZH.md`）。有本地 `file://` registry，
  没有远程 HTTP registry。
- `extern "C"` 和 `cfn` 描述 C ABI 调用与回调。
  `extern "dci"` 导入原生事实契约，涵盖受支持的 C++ / Rust 布局、调用、生命周期与派发。
  Active Adapter 在构建期间请求生产端实例化泛型，Consumer 消费闭合事实。
  详见 [DCI](DCI_SPEC_ZH.md) 和 [事实语义所有权系统](MOSP_ZH.md)。
- `@[platform]` 选择 OS 面 std（`windows` vs `posix`）。

## async 与 comptime（子集）

- `@[async]` / `await`：`Promise`/`Task` Future（`poll` / `await_value`）。
  `await` 处 MIR 拆分（`poll` + `mir_term_yield`）；无栈泵在 Pending 时把 worker
  再入队。`Task.sleep` / `Task.yield_now` 是定时 Future。OS fiber 只留给
  `StartCoroutine`。阻塞 syscall 卡住调度器。
- `@[comptime]`：`sema_ct_eval_known` 成功才折叠——整数/bool/char 字面量、
  一元/二元/三目、`while`/`for`/`break`/`continue`、`print`/`println`、
  `ct_file_size`、以及对 `@[comptime]` 函数且实参已知的调用（函数递归深度上限 32）。
  不解释堆。

## 能分词、但不是用户契约

`macro` / `bench` 关键字、`concept`（按 interface-like 解析；改用
`where T: Trait`）、优先拼写的 `foreach`（改用 `for-in`）、关键字 `task`
（用 `std.vio` 的 `Task`）、`when` / `is`（有关键字 kind，解析器里没找到
产生式）、`volatile` 局部、`T?` 可空类型、作为用户
汇编的 `asm(…)`、`Delegate`/`Event`、不经 DCI 的语言级 C++ class 接管。

`fail Type.Variant` **会解析并给类型**。教程仍优先 `Result`/`?`。
`requires` 在函数上当作 `where` 同义词，不是 C++ concepts 语言。
`repl` 是 `main` 里真实接上的命令（经 runtime 的 LLVM JIT），不是缺失 stub。

## 实现边界

当前 DCI 和编译器性能验收以 AOT 为基准。LLVM JIT REPL 已存在；JIT 的能力
对齐和压力测试留待后续，AOT 验证结果不代表相同操作已经支持 JIT。

活动树：`bootstrap_compiler/`（lexer → parse → Sema → HIR → MIR → LLVM）。
已归档 C++ Host：仓库根 `src/`。冻结 host std：仓库根 `std/`。权威 std：
`bootstrap_compiler/std_packages/`。

构建与门： [TESTING_GUIDE_ZH.md](TESTING_GUIDE_ZH.md)。课表对照：
[语言表面](语言表面_ZH.md)。
