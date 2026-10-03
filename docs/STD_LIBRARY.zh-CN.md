# Vyx 标准库参考手册

[English: standard-library reference](STD_LIBRARY.md) · [标准库教程](标准库教程_ZH.md) · [文档目录](README.md)

> 本文描述当前自举标准库 package 入口与边界。涉及自举、基准或运行时正确性的
> 结论，以 `docs/TESTING_GUIDE.md` 和最新命令输出为准；旧数字仅作历史背景。

> `bootstrap_compiler/std_packages/` 是 std 源码的唯一权威。`--src=file` 经该目录
> 根上的 `registry` 转发到各 package 的 `.vyx`。扁平的 `bootstrap_compiler/std/`
> 与根目录 `std/` 是冻结的历史快照，待整体删除；registry 存在时不参与扫描，
> 也没有任何同步要求。
>
> 权威门：`probes/gates/h/std_single_truth.sh` 要求非 FFI 模块 basename 不得
> 出现在两个 package，并探测 SDK 编译器能导入真实 std 模块。

> 下表是源码与 package 入口索引，不是 Release 可用性承诺。使用某个 package
> 前，请按 `TESTING_GUIDE.md` 构建当前 compiler，并运行该
> package 对应的 project 或 contract check。

## Package 入口

源码泛型标准库使用 `type = "source"`，由消费 target 完成 MIR/Mono；原生
bridge（JSON/miniz）继续使用 static package。当前入口：

| 路径 | target | 内容 |
|---|---|---|
| `std:core` | `std_core` | defaults、Option/Result、String/Clone、Ref/Box、hash、ops、num |
| `std:collections` | `std_collections` | Vec/Dict/Set/List/Deque/Stack/Iterator 等 |
| `std:algorithm` | `std_algorithm` | 容器算法 |
| `std:io` | `std_io` | io/fs/path/process/args/env/ffi/vio |
| `std:sync` | `std_sync` | sync/channel/threading/thread_pool/concurrent |
| `std:format` | `std_format` | fmt/color |
| `std:testing` | `std_testing` | testing/mock |
| `std:toml` | `std_toml` | TOML |
| `std:json` | `std_json` | JSON + cJSON bridge |
| `std:miniz` | `std_miniz` | 压缩 + miniz bridge |
| `std:std` | `std` | 已 package 化的通用标准库聚合包（含 JSON/miniz） |
| `std:cacao` | `std_cacao` | Cacao 图形/GPU C ABI 绑定（按需，自行链接 Cacao） |
| `std:dll` | `std_dll` | 动态库加载 |
| `std:llvm` | `std_llvm` | LLVM C API 绑定 |
| `std:network` | `std_network` | curl 绑定、HTTP 客户端、WebSocket、gRPC（模块 `std.network.*`） |
| `std:security` | `std_security` | OpenSSL 绑定 + 纯 Vyx 哈希（模块 `std.security.*`） |
| `std:sqlite` | `std_sqlite` | SQLite3 驱动 |
| `std:stb_image` | `std_stb_image` | `std.image`：stb_image + write + resize2（stub C 实现宏） |
| `std:win32` | `std_win32` | Win32 API 辅助（`@[platform("windows")]`） |
| `std:logger` | `std_logger` | `std.log`：DCI + 编译后的 spdlog（按需；package prebuild） |

例如：

```toml
[dependencies]
core = { path = "std:core", target = "std_core" }
collections = { path = "std:collections", target = "std_collections" }
```

`vyxc new app` 默认生成上述两个常用全局依赖。多 target 项目也可放到
`[target.app.dependencies]`，使依赖只归属于 `app`。

每个分块 package 的规范源码位于自身 `src/` 下；`std:std` 通过 manifest
依赖组合这些 package，而不是再次平铺同一批文件。LLVM、Cacao、curl、
OpenSSL、SQLite、stb_image、Win32 等需要独立 native SDK/实现的绑定不进入
通用聚合包，避免一个普通 `std` 应用被迫链接所有第三方库；它们应由对应的
显式 native package 提供完整 link closure 后再导入。

## 当前状态

- `std.math` 是源码标准库，直接封装 libm 常见函数。
- `std.vio` 是源码标准库，包含 `Promise<T>` / `Task<T>`、平台门控、线程/纤程/IO 辅助。
- `@[platform]` 已被 bootstrap sema 识别并过滤。`posix` 同时匹配 Linux、
  Android、macOS。OS 相关 API（`os` / `fs` / `path` / `sync` / `threading` /
  `vio`）用 `windows` vs `posix`；`io_uring` 与 x86_64 `syscallN` 仍仅 Linux。
- `async` / `await` 走 Future（`Promise`/`Task`）。`poll()` 只看 `is_done`（`true` = Ready，`false` = Pending）。`@[async] fn` 里的 `await` 是 MIR 拆分（`poll` + `mir_term_yield`）。非 async 调用方的 `await_value()` 等到就绪：无栈泵 + `Task.sleep` 定时器。`StartCoroutine` 仍走 OS fiber。`vio_sleep` 仍会卡住调度器。
- `@[comptime]` 会把带常量实参的 `@[comptime] fn` 调用折成字面量；解释器跑 `while`/`for`、`break`/`continue`、`print`/`println`、`ct_file_size`。不解释堆。
- 赋值是 move-by-default（见 `std.clone`）。满足 `Copy` 的类型（标量、
  `impl Copy` / `@[derive(Copy)]`）赋值不触发 E3100 / V-MOVE-001。类
  `string` / `String` 不是 Copy；需要第二份所有权时用 `.clone()`。
- `std.ref` 只保留 class `Ref<T>` / `Box<T>`，已删除遗留 `_Ref` / `_Box` 函数 API。
- 自举 runtime 的 Dict 仍是 C ABI 后的 C++ STL。`vyx_bootstrap_mkdir_p` /
  `sleep_ms` / `file_mtime` / `file_stamp` / `cwd` 已改为 Win32/POSIX。
  目录树遍历仍用 `std::filesystem`。用户程序 `i64` sort 在
  `runtime/src/vyx_runtime.vyx`。
- 语言 FFI 只有 C：`std.ffi` 提供 `c_int` / `c_char` / `c_size_t` 以及
  `CStr` / `CString`。`c_long` 跟随目标 C ABI（Windows LLP64 = i32；POSIX
  LP64 = i64）。C++ 类/vtable 走 DCI，不走语言 FFI。
- Android 上 `vyx_fiber_posix_*` 是 no-op stub（Bionic 无 `getcontext` /
  `swapcontext` / `makecontext`）。`@[async]` 走无栈泵，不依赖 fiber。
  `StartCoroutine` 仍需要。
- `std.time` 权威实现是 POSIX `current_timestamp`/`clock_ms`/`monotonic_nanos`/`sleep_ms`（i64；按值结构体 API 等 class ABI）；旧的 Windows QPC/`Sleep` 扁平副本不再作为公开 API。

## 外部库 package

`bootstrap_compiler/std_packages/logger` 是 `std.log` 的家。`[build] prebuild`
走编译器自带的 `vyx:` 脚本：克隆 spdlog、CMake 编静态库、跑仓库内 DCI
adapter 写出 `contracts/spdlog.dcib`。Vyx 通过契约调用
`spdlog::logger::log`；C++ 助手只覆盖默认 logger 的 lifetime。消费方用
`log_info` / `set_log_level`，级别走 `log_level_*()`（package `.vyi` 不重导出
`let` 初值）。需要显式依赖：

```toml
[dependencies]
logger = { path = "std:logger", target = "std_logger" }
```

`bootstrap_compiler/std_packages/json` 把 Vyx API、cJSON 源码和 native bridge
放在同一个 `Vyx.toml` 项目中。消费项目可声明：

```toml
[dependencies]
json = { path = "std:json", target = "std_json" }
```

构建器会递归构建 package、导入其 `.vyi`、自动加入静态库并传播运行时资源。
`std.json` 提供 `Result` 错误、对象/数组/标量、深拷贝子值、类型检查、序列化
和确定性的 `drop()`；缺失键、类型错误、解析错误和分配错误不会被伪装成空值。

`bootstrap_compiler/std_packages/miniz` 提供自包含的 zlib-compatible 压缩、
解压、CRC32 和 Adler32。消费项目声明：

```toml
[dependencies]
miniz = { path = "std:miniz", target = "std_miniz" }
```

`CompressedData` 拥有 native 缓冲区并自动 `drop()`；解压必须给出最大输出长度，
容量不足、非法参数、分配失败和数据错误通过 `Result` 区分，不返回含混的空字符串。

`bootstrap_compiler/std_packages/stb_image` 内嵌 stb 图像家族，并用一个 stub C
做实现宏（与 `std:json` 相同的 C/H 混编）：

- `vendor/stb_image.h`
- `vendor/stb_image_write.h`
- `vendor/stb_image_resize2.h`
- `native/stb_image_stub.c` 里定义 `STB_IMAGE_IMPLEMENTATION` /
  `STB_IMAGE_WRITE_IMPLEMENTATION` / `STB_IMAGE_RESIZE_IMPLEMENTATION` 后再
  `#include` 上述头文件（单头库不经过这个翻译单元就不会导出符号）

```toml
[dependencies]
stb_image = { path = "std:stb_image", target = "std_stb_image" }
```

消费方不必再写 stb 的 `.c` / `.h` 或额外 `libs`。

Cacao / curl / OpenSSL / SQLite / LLVM / Win32 等按需 native 绑定现在也是
`type = "source"` package，为树内 Vyx 绑定，不内嵌第三方 SDK：

```toml
[dependencies]
cacao = { path = "std:cacao", target = "std_cacao" }
```

真正调用 GPU/网络 API 时，由消费 target 自己写 `libs` / `lib_paths`（与
`samples/projects/cacao` 链接 Cacao 的方式相同）。

`std.cjson` 平铺 binding 已移除。显式 source package 会关闭该 target 的隐式
flat-std 搜索；漏写依赖会在 import 阶段直接报错，而不是静默拾取兼容镜像。

---

## 内置函数 (Builtins)

### 输出 & 调试
| 函数 | 签名 | 说明 |
|------|------|------|
| `print` | `print(val)` | 打印任意类型（i64/f64/string/bool） |
| `assert` | `assert(cond: bool)` | 条件为 false 时终止并报告位置 |
| `panic` | `panic(msg: string)` | 立即终止程序并打印消息 |
| `format` | `format(fmt, args...)` | 字符串格式化 |

### 类型 & 内存
| 函数 | 签名 | 说明 |
|------|------|------|
| `sizeof::<T>()` | `-> i64` | 类型 T 的字节大小 |
| `alignof::<T>()` | `-> i64` | 类型 T 的对齐要求 |
| `type_name::<T>()` | `-> string` | 类型 T 的编译时名称 |
| `typeinfo(T)` | `.fields -> [string]` | 编译时类型字段名列表 |
| `alloc(size: i64)` | `-> rawptr` | 分配内存 |
| `dealloc(ptr: rawptr)` | | 释放内存 |
| `transmute` | `transmute::<T>(val)` | 比特级类型转换（unsafe） |

### 指针 & 转换
| 函数 | 签名 | 说明 |
|------|------|------|
| `to_rawptr(&val)` | `-> rawptr` | 获取变量的原始指针 |
| `from_cstr(ptr: rawptr)` | `-> string` | C 字符串转 Vyx 字符串 |

### 命令行
| 函数 | 签名 | 说明 |
|------|------|------|
| `argCount()` | `-> i64` | 命令行参数个数 |
| `getArg(i: i64)` | `-> string` | 获取第 i 个参数 |
| `getArgs()` | `-> Vec<string>` | 获取所有参数 |

---

## math — libm 封装 (`std.math`)

`std.math` 不是编译器内建的“假数学库”，而是直接面向 libm 的源码模块。

| 函数 | 说明 |
|------|------|
| `abs_val(i32)` / `abs_f64(f64)` | 绝对值 |
| `sin` / `cos` / `tan` | 三角函数 |
| `asin` / `acos` / `atan` / `atan2` | 反三角函数 |
| `sinh` / `cosh` / `tanh` | 双曲函数 |
| `sqrt` / `cbrt` | 平方根 / 立方根 |
| `pow` | 幂 |
| `exp` / `exp2` | 指数 |
| `log` / `log2` / `log10` | 对数 |
| `floor` / `ceil` / `round` / `trunc` | 舍入 |
| `pi()` / `tau()` / `e()` | 常量 |

`math_libm_test.vyx` 现在覆盖了这些常见路径。

---

## vio — 标准异步运行时 (`std.vio`)

`std.vio` 是源码标准库，不是 C wrapper 兜底。它包含：

| 组件 | 说明 |
|------|------|
| `vio_start` / `vio_stop` | 运行时初始化 / 关闭 |
| `vio_sleep` / `vio_yield` / `vio_now` | 基础调度与时间 |
| `Promise<T>` / `Task<T>` | 源码状态对象，提供 `new` / `ready` / `failed` / `resolve` / `reject` / `poll` / `await_value` |
| `StartCoroutine` / `StopCoroutine` | 协程入口包装 |
| `@[platform]` | 平台门控声明，bootstrap sema 会过滤未命中的分支 |
| `vio_run` / `vio_run_wait` | 线程级异步执行 |
| `vio_fiber_*` | 纤程调度辅助 |

当前编译器层把 `@[async]` 调用 lower 到 `vio_sched_spawn`，把 `await` lower 到 `await_value()`。`poll()` 是用户 Future 表面。Android 没有 ucontext 时走无栈 invoke，不是 fiber 切换。

### `std.sync` / `std.threading` / `std.thread_pool`

`Mutex<T>`、`RwLock<T>`、`Condvar`、`Barrier`、`Once`、`atomic_*` 是 Vyx 类型。句柄来自 `runtime/src/vyx_sync.c` 的 OS 对象（Windows SRWLOCK / CONDITION_VARIABLE / CreateThread，POSIX pthread），不是对 C++ `std::mutex` / `std::thread` 的封装。

`std.thread_pool` 的工作窃取调度走 oneTBB（`tbb::parallel_for` / `tbb::task_group` / `tbb::task_arena`），C ABI 在 `runtime/src/vyx_tbb.cpp`。`use std.thread_pool` 的程序自动链接 `tbb12`（Windows）或 `tbb`（POSIX）并自动拷贝运行库。`runtime/scripts/prepare_tbb.vyx`（`vyx:` hook）克隆 [oneTBB](https://github.com/uxlfoundation/oneTBB) `v2022.1.0` 并把导入库放到 SDK 编译器（`vyxc`）旁边。

`parallel_for(n, body, ctx)` 接收**带类型的 C ABI 回调** `cfn(rawptr, i64) -> void`：给循环体标 `@[no_mangle]`、按名字传入即可。Vyx `fn` 值是脂肪指针，不能跨 C 回调边界，所以不提供 `rawptr` 重载。循环体跑在线程池线程上——只写互不重叠的槽位（如 `ctx + i * elem_size`），库不做隐式同步。可运行示例见 `probes/gates/tbb_pool`。

---

### Option/Result（源码 std 模块）
`Option<T>` / `Result<T, E>` 来自 `std.option` / `std.result`，不是编译器内建表里的“假构造器”。
`Some`、`None`、`Ok`、`Err` 这类名字应当来源于 std 源码声明，而不是 sema/codegen 的硬编码分支。

### 容器构造（源码 std 模块）
`Vec` / `Dict` / `Set` / `Stack` / `Queue` / `Iterator` 也都应来自 std 源码模块；
如果这里还保留辅助构造函数，它们只是 std 层兼容入口，不是编译器内建。

---

## Vec\<T\> — 动态数组 (`std.vec`)

```vyx
var v = Vec::<i64>.new();        // 创建
v.push(42);                      // 添加
let x = v.get(0);                // 获取 → T
v.set(0, 100);                   // 设置
let n = v.pop();                 // 弹出最后 → T
```

### 方法
| 方法 | 签名 | 说明 |
|------|------|------|
| `new()` | `static -> Vec<T>` | 创建空 Vec |
| `with_capacity(n)` | `static -> Vec<T>` | 指定初始容量 |
| `push(val: T)` | | 追加元素 |
| `get(i: i64)` | `-> T` | 获取元素（越界 panic） |
| `get_unchecked(i)` | `-> T` | 无边界检查获取 |
| `set(i: i64, val: T)` | | 设置元素 |
| `pop()` | `-> T` | 弹出最后元素 |
| `first()` | `-> T` | 首元素 |
| `last()` | `-> T` | 末元素 |
| `count()` | `-> i64` | 元素数量 |
| `length()` | `-> i64` | 同 count() |
| `size()` | `-> i64` | 同 count() |
| `isEmpty()` | `-> bool` | 是否为空 |
| `capacity()` | `-> i64` | 当前容量 |
| `contains(val: T)` | `-> bool` | 是否包含值 |
| `indexOf(val: T)` | `-> i64` | 查找索引（-1=未找到） |
| `insert(i, val)` | | 在位置 i 插入 |
| `remove(i)` | `-> T` | 删除位置 i 的元素 |
| `removeAt(i)` | `-> T` | 同 remove() |
| `swap(i, j)` | | 交换两个位置 |
| `reverse()` | | 反转 |
| `sort()` | | 排序（快排） |
| `clear()` | | 清空 |
| `reserve(n)` | | 预留容量 |
| `shrink_to_fit()` | | 收缩容量 |
| `iter()` | `-> Iterator<T>` | 获取迭代器 |
| `destroy()` | | 释放内存 |

### 字段
- `len: i64` — 元素数量（只读使用）
- `data: rawptr` — 底层数据指针
- `cap: i64` — 容量
- `elem_size: i64` — 元素大小

---

## Dict\<K, V\> — 字典/哈希表 (`std.dict`)

```vyx
var d = Dict::<string, i64>.new();
d.put("age", 25);
let age = d.get("age");          // → V
if (d.contains("age")) { ... }
d.remove("age");
```

### 方法
| 方法 | 签名 | 说明 |
|------|------|------|
| `new()` | `static -> Dict<K,V>` | 创建空 Dict |
| `with_capacity(n)` | `static -> Dict<K,V>` | 指定初始容量 |
| `put(key: K, val: V)` | | 插入/更新 |
| `insert(key, val)` | | 同 put() |
| `get(key: K)` | `-> V` | 获取值 |
| `contains(key: K)` | `-> bool` | 是否包含 key |
| `remove(key: K)` | `-> bool` | 删除 |
| `isEmpty()` | `-> bool` | 是否为空 |
| `count()` | `-> i64` | 元素数量 |
| `length()` | `-> i64` | 同 count() |
| `size()` | `-> i64` | 同 count() |
| `keys()` | `-> Vec<K>` | 所有 key |
| `values()` | `-> Vec<V>` | 所有 value |
| `iter()` | `-> DictEntryIterator` | 迭代器 |
| `clear()` | | 清空 |
| `destroy()` | | 释放内存 |

> 注意: `Dict<string, V>` 自动使用 content-based hash/eq（按字符串内容，非指针值）

---

## Set\<T\> — 集合 (`std.set`)

```vyx
var s = Set::<i64>.new();
s.add(42);
if (s.contains(42)) { ... }
s.remove(42);
```

| 方法 | 说明 |
|------|------|
| `new()` | 创建空 Set |
| `add(val: T)` | 添加元素 |
| `contains(val: T) -> bool` | 是否包含 |
| `remove(val: T)` | 删除 |
| `isEmpty() -> bool` | 是否为空 |
| `clear()` | 清空 |
| `destroy()` | 释放 |

---

## Stack\<T\> / Queue\<T\> (`std.stack`)

```vyx
var st = Stack::<i64>.new();
st.push(1); st.push(2);
let top = st.pop();    // → 2

var q = Queue::<i64>.new();
q.enqueue(1); q.enqueue(2);
let front = q.dequeue(); // → 1
```

### Stack 方法
| 方法 | 说明 |
|------|------|
| `push(val: T)` | 入栈 |
| `pop() -> T` | 出栈 |
| `peek() -> T` | 查看栈顶 |
| `isEmpty() -> bool` | 是否为空 |
| `clear()` | 清空 |
| `destroy()` | 释放 |

### Queue 方法
| 方法 | 说明 |
|------|------|
| `enqueue(val: T)` | 入队 |
| `dequeue() -> T` | 出队 |
| `peek() -> T` | 查看队首 |
| `len() -> i64` | 长度 |
| `isEmpty() -> bool` | 是否为空 |
| `clear()` | 清空 |
| `destroy()` | 释放 |

---

## Iterator\<T\> (`std.iter`)

```vyx
let it = vec.iter();
while (it.has_next()) {
    let val = it.next();
}
// 或使用 for-in
for (item in vec) { ... }
```

| 方法 | 说明 |
|------|------|
| `from_raw(data, len, pos)` | 从原始数据创建 |
| `next() -> Result<T>` | 下一个元素 |
| `has_next() -> bool` | 是否有下一个 |
| `reset()` | 重置位置 |
| `collect() -> Vec<T>` | 收集为 Vec |
| `count() -> i64` | 剩余元素数 |
| `sum() -> T` | 求和 |
| `for_each(fn)` | 遍历 |
| `map(fn) -> Iterator` | 映射 |
| `filter(fn) -> Iterator` | 过滤 |
| `take(n) -> Iterator` | 取前 n 个 |
| `skip(n) -> Iterator` | 跳过前 n 个 |
| `min() / max()` | 最小/最大值 |

---

## string 操作 (`std.string`)

```vyx
let s = "Hello, World!";
let parts = str_split(s, ", ");     // → Vec<string>
let sub = str_substring(s, 0, 5);   // → "Hello"
if (str_contains(s, "World")) { ... }
```

### 自由函数
| 函数 | 签名 | 说明 |
|------|------|------|
| `str_length(s)` | `-> i64` | 字符串长度 |
| `str_is_empty(s)` | `-> bool` | 是否为空 |
| `str_equals(a, b)` | `-> bool` | 按内容比较 |
| `str_contains(s, sub)` | `-> bool` | 是否包含子串 |
| `str_starts_with(s, prefix)` | `-> bool` | 前缀匹配 |
| `str_ends_with(s, suffix)` | `-> bool` | 后缀匹配 |
| `str_index_of(s, sub)` | `-> i64` | 查找位置（-1=未找到） |
| `str_substring(s, start, end)` | `-> string` | 子串 |
| `str_split(s, delim)` | `-> Vec<string>` | 分割 |
| `str_to_upper(s)` | `-> string` | 转大写 |
| `str_to_lower(s)` | `-> string` | 转小写 |
| `str_trim(s)` | `-> string` | 去除首尾空白 |
| `str_replace(s, old, new)` | `-> string` | 替换 |
| `str_char_at(s, i)` | `-> i32` | 获取指定 UTF-8 字节 |
| `str_repeat(s, n)` | `-> string` | 重复 n 次 |
| `str_reverse(s)` | `-> string` | 反转 |
| `str_to_i64(s)` | `-> i64` | 解析整数 |
| `str_join(parts, sep)` | `-> string` | 连接 |

### str 与 String 迭代
```vyx
let view: str = "A¢€𐍈";
for (c in view) { ... }          // char：4 个 Unicode scalar
for (c in view.chars()) { ... }  // 与直接 for-in 相同
for (b in view.bytes()) { ... }  // u8：10 个 UTF-8 字节

var owned = String.from("hello");
let borrowed: str = owned.as_str();
for (c in owned) { ... }
owned.destroy();
```

`str` 不拥有源存储；`String` 使用 SSO/堆存储并负责释放。`iter()`、`chars()` 和
`bytes()` 都返回借用游标，修改或销毁源 `String` 会使游标失效。

---

## reflect — 运行时反射 (`std.reflect`)

课内 API 是 `Type` / `Field` / `Method` / `Instance`，不是 `TypeBuilder` / `TypeMeta`。
编译器 CSV 只 intern 一次；之后按名查找走哈希表。

```vyx
use std.reflect;

@[reflect("MeterType", alias=["MeterAlias"])]
public class Meter {
    @[reflect("value", alias=["reading"])]
    public value: i64;

    public fn bump(delta: i32) -> i32 {
        return (self.value as i32) + delta;
    }
}

fn main() -> i32 {
    var src = Meter { value: 7 };
    let t = Type.of::<Meter>();
    if (!t.valid || !t.matches::<Meter>()) { return 1; }
    let by_alias = getType("MeterAlias");
    if (!by_alias.valid || by_alias.name != "MeterType") { return 2; }
    var inst = t.bind(&src);
    let prop = inst.getProperty("reading");
    inst.write::<i64>(prop, 11);
    if (src.value != 11) { return 3; }
    return 0;
}
```

| API | 说明 |
|------|------|
| `Type.of::<T>()` / `getType(name)` | 按类型或名字（含 `@[reflect]` 别名）取 `Type` |
| `Type.getField` / `getFieldAt` / `getMethod` / `getMethodAt` | intern 后 O(1) |
| `Instance.bind` / `getProperty` / `write` / `getMethod` | 绑对象、读写字段、取绑定方法 |
| `@[reflect("…", alias=[…])]` / `@[hidden]` | 改公开名 / 从清单隐藏 |

契约：`tests/cases/tutorial_reflect.vyx`。`reflect_*` 内建不是课内表面。

---

## Option\<T\> (`std.option`)

```vyx
let maybe: Option<i64> = Some(42);
match (maybe) {
    case Some(val) => { print(val); }
    default => { print("none"); }
}
let x = maybe.unwrap();           // panic if None
let y = maybe.unwrap_or(0);       // 默认值
```

---

## hash & collections (`std.hash`, `std.collections`)

### Hashable trait
```vyx
trait Hashable {
    fn hash() -> i64;
}
```
内置: `hash_i64`, `hash_string`, `hash_any`, `hash_bytes`

### Iterable trait（collections.vyx）
| 方法 | 说明 |
|------|------|
| `sort()` | 排序 |
| `binary_search(val) -> i64` | 二分查找 |
| `find(pred) -> Option<T>` | 条件查找 |
| `count() -> i64` | 计数 |
| `sum() -> T` | 求和 |
| `min() / max()` | 最小/最大 |
| `reverse()` | 反转 |
| `unique() -> Vec<T>` | 去重 |

---

## 其他标准库模块

| 模块 | 说明 |
|------|------|
| `std.math` | libm wrappers: sin/cos/tan/asin/acos/atan/atan2/sinh/cosh/tanh/sqrt/pow/log/ceil/floor/round/trunc |
| `std.vio` | async/runtime source module: Promise/Task, threads, fibers, platform gates, TCP helpers |
| `std.io` | 文件读写 (read_file/write_file) |
| `std.fs` | 文件系统 (exists/mkdir/readdir/stat) |
| `std.os` | 操作系统 (env/exec/exit/sleep) |
| `std.error` | 错误处理 (Error trait/panic) |
| `std.serde` | Vyx 值序列化基础接口；完整 JSON 能力使用 `std.json` package |
| `std.ffi` | C ABI 外部函数接口（`c_int`/`CStr`/`CString`；不是 C++） |
| `std.allocator` | 内存分配器 |
| `std.win32` | Windows API 绑定 |
| `std.sdl3` | SDL3 游戏开发绑定 |
| `std.openssl` | 加密库绑定 |
| `std.curl` | HTTP 客户端 |
| `std.stb_image` | 图像加载 |
