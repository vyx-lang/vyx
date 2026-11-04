# Vyx 标准库参考手册

[English: standard-library reference](STD_LIBRARY.md) · [标准库教程](标准库教程_ZH.md) · [文档目录](README.md)

> 本文描述当前自举标准库 package 入口与边界。涉及自举、基准或运行时正确性的
> 结论，以 `docs/TESTING_GUIDE.md` 和最新命令输出为准；旧数字仅作历史背景。

> `bootstrap_compiler/std_packages/` 是 std 源码的唯一权威。`--src=file` 经该目录
> 根上的 `registry` 转发到各 package 的 `.vyx`。标准库修改只落在 `std_packages/`；
> 展平的 `bootstrap_compiler/std/` 和根目录 `std/` 都是旧源码，不作为实现入口。
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
| `std:serde` | `serde_toml` | TOML |
| `std:serde` | `serde_json` | JSON + cJSON bridge |
| `std:miniz` | `std_miniz` | 压缩 + miniz bridge |
| `std:std` | `std` | 已 package 化的通用标准库聚合包（含 JSON/miniz） |
| `std:cacao` | `std_cacao` | Cacao 图形/GPU C ABI 绑定（按需，自行链接 Cacao） |
| `std:dll` | `std_dll` | 动态库加载 |
| `std:llvm` | `std_llvm` | LLVM C API 绑定 |
| `std:network` | `std_network` | [HTTP、文本 WebSocket、无压缩一元 gRPC](../bootstrap_compiler/std_packages/network/README.md)；gRPC 需要启用 HTTP/2 的 libcurl |
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

## 资源与值语义

[标准库回归门](../probes/gates/stdlib-contracts/README.md) 包含 O0/O2 项目运行、
语义拒绝检查，以及使用 Python 列表计算预期结果的组合操作测试。Windows、Linux、
macOS 的原生 Release 任务在打包前执行。验证报告中的平台、编译器和配套库哈希
才是本次验证的身份；目录清单和构建成功本身不能证明运行正确。

`Vec<T>` 的查找和相等比较要求 `T: Eq`，哈希要求 `T: Hashable`，排序要求
`T: Ord`。这些操作使用元素值，不比较指针地址或结构体填充字节。排序不稳定，
使用有界堆排序，并使已有迭代器失效。哈希值不作为持久化格式。
兼容方法 `Vec.sum()`、`Iterator.sum()` 将每个元素转换为 i64 后累加，浮点数
逐项截断；需要保留元素类型时使用 `std.algorithm.sum_iter`。

`PriorityQueue<T>` 的入队、出队和堆序切换要求 `T: Ord`。`peek` 要求 `Clone`，
返回独立值；`pop` 转移元素所有权。`clear` 析构所有存活元素并保留容量。切换
最小堆/最大堆会以 O(n) 重建非空队列、使迭代器失效，并返回可变借用。
构造拥有存储的最大堆用 `new_max()`；初始分配和扩容均遵守 `alignof<T>`。

`BTreeMap`、`BTreeSet` 当前使用有序平坦数组：查询 O(log n)，插入、删除 O(n)。
查询和删除借用查询键，不要求键实现 Clone。返回独立的值、键或克隆整个容器时，
才要求相应的 Clone 约束。有序键的 Eq 必须与 `compare(other) == 0` 一致。

`Iterator.next` 和返回独立元素值的读取操作要求 `Clone`。`map`、`filter`、
`take`、`skip` 立即执行并持有独立存储，并非惰性管线。`map`、`filter` 耗尽
输入游标；`take(n)` 只前进选中的数量；`skip(n)` 耗尽游标并返回剩余范围。
`fn(T)` 回调接收所有权，`fn(&T)` 回调借用参数。适配器销毁时也清理未读取元素。
容器迭代器借用源容器；源必须存活到迭代器使用结束，修改计数检查不能延长其寿命。

`Channel<T>` 是有界、非阻塞队列。`try_send` 通过 `ChannelSendError<T>` 返回
拒收值的所有权，`into_inner()` 可以取回一次。`try_recv` 区分空与关闭；关闭后
仍可取完已有消息。销毁时清理未读取消息；销毁前必须等待所有使用者退出。

`File.try_open`、`try_read_all_bytes`、`try_read_all_text` 返回 `Result`，空文件
是成功结果。旧便捷读取方法保留失败时返回空值的行为。Windows 路径按 UTF-8 转为
宽字符 API 输入。文件作用域清理自动关闭句柄，显式关闭可重复调用。

`std.security.openssl` 提供 `try_sha256`、`try_sha512`、`try_sha1`、`try_md5`、
`try_hmac_sha256`、`try_random_bytes`、`try_random_int`，通过 `Result` 返回
`CryptoError`（类别、原生错误码、消息）。摘要和 HMAC 的成功结果是拥有存储的
十六进制 `String`。`try_random_bytes` 接受 0..256 字节，也返回十六进制文本，
每字节两个字符；零长度是成功的空结果。原有文本便捷函数在原生调用失败时 panic。
真实 OpenSSL 已知答案和失败注入测试须显式启用并提供原生库，不代表整个 TLS API
已经验收。

严格 Base64/hex 解码提供返回 `Result` 的 `try_*` 方法。CSV 支持引号内换行、
双引号转义、CRLF 和内嵌 NUL，格式错误包含位置。现有项目回归不代表所有原生绑定、
数据格式、内存分配失败、异步调度和设备后端均已验收。

`std.semver` 按 [SemVer 2.0](https://semver.org/) 解析和比较版本。
`SemVer.try_parse` 返回 `Result<SemVer, SemVerError>`，包含错误类别、消息和字节位置。
三个核心数字范围为 0..2147483647，溢出明确报错；数字预发布标识符不受原生整数位宽限制。
不接受 `v` 前缀、空白、缺少字段的版本和范围表达式。`parse`、`create` 与原有驼峰命名的
递增方法在失败时 panic；`try_create`、`try_bump_major/minor/patch` 返回显式错误。
比较、Eq 和 Hashable 忽略构建元数据，序列化保留元数据。Clone 独立拥有标识符字符串，
版本值可用于 Vec 排序和 Dict 键。直接修改公开字段后，序列化、比较、哈希、递增仍会验证
字段有效性。实际消费示例见[版本目录项目](../tests/projects/std_semver_contracts/README.md)。

`std.url` 区分百分号编码与表单编码。`url_decode` 保留字面 `+`，需要把加号解为
空格时使用 `form_decode`。`try_url_decode`、`try_form_decode` 对错误转义返回带字节
位置的错误。这些是字节 API，保留 NUL 和非 UTF-8 数据，编码通过字节构建器完成。
`QueryPair` 与 `query_encode_pairs` / `try_query_decode` 保留重复键、顺序和空键值；
表单参数使用 `form_query_encode` / `try_form_query_decode`。字典版 `query_encode`
借用输入，按键的字节顺序序列化。

`Url.try_parse` 接受绝对 HTTP/HTTPS/WS/WSS URL，支持注册主机名、IPv4、括号 IPv6，
端口必须是非空的 0..65535 十进制数字。解析保留转义、大小写和空的 `?` / `#` 分隔符，
`host` 字段不含 IPv6 方括号。`try_to_string` 验证修改后的公开字段与分隔标志，Clone
独立拥有字段。此 API 不做相对 URL 解析、IDNA、DNS 查询、浏览器 WHATWG 规范化、
IPvFuture 或 IPv6 zone 解析。原始空格、控制字节、非 ASCII 字符和错误转义会被拒绝。
消费示例与边界见 [URL 项目](../tests/projects/std_url_contracts/README.md)。

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
- `std.time` 使用 Windows QPC 或 POSIX `clock_gettime` 读取单调时间；macOS 使用本机的 `CLOCK_MONOTONIC` 编号 6，Linux/Android 为 1。失败不退回 CPU 计时。`try_monotonic_nanos`、`try_current_timestamp`、`try_sleep_ms` 返回 `Result<_, TimeError>`，普通时钟函数在失败时 panic。POSIX 睡眠被 EINTR 中断后继续剩余时长；Windows 分段处理超过 DWORD 范围的时长。兼容 `sleep_ms` 的非正参数仍为空操作，`try_sleep_ms` 拒绝负数。
- `std.path.is_absolute` 按目标平台判断，不访问文件系统。Windows 的 `C:relative` 与 `\\rooted` 都不算绝对路径；`is_rooted` 可区分带驱动器或根的路径。绝对路径包括完整驱动器根、UNC 服务器/共享及 Win32 设备命名空间。POSIX 只把开头的 `/` 视为根，反斜杠与 `C:` 是普通路径字符。这不是文件名有效性或路径规范化检查。

## 外部库 package

`std:sqlite` 的 `std.sqlite` 模块提供拥有原生句柄的 `Database` 和 `Query`。
`try_open`、`try_exec`、`try_query` 及游标的 `try_*` 返回包含 SQLite 状态码和消息的
`Result`；便捷方法保留，同时增加显式错误路径。原来的 `error()` 是无法解析的关键字
方法名，现为 `lastError()`。包会传播 `sqlite3` 链接依赖，第三方原生库仍需独立安装。
Windows 可配置 `VCPKG_ROOT`，发布应用时部署相应 `sqlite3.dll`。

绑定文本和 BLOB 时复制输入，允许调用后销毁或修改原始缓冲。读取返回独立副本，
保留嵌入 NUL，区分 NULL 与空值。i64 读取不截断；i32 读取检查范围。
游标检查行状态、列索引和类型；`try_query` 拒绝空语句及多语句，批处理用 `try_exec`。
句柄在作用域退出时关闭，显式关闭可重复调用；数据库先关闭时，已有语句保持原生
连接存活到最后一次 finalize。`Query.close()` 即使返回上次执行错误也已经释放语句。
完整 API 和消费示例见 [SQLite package](../bootstrap_compiler/std_packages/sqlite/README.md)。

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

`bootstrap_compiler/std_packages/serde` 把 Vyx API、cJSON 源码和 native bridge
放在同一个 `Vyx.toml` 项目中。消费项目可声明：

```toml
[dependencies]
json = { path = "std:serde", target = "serde_json" }
```

构建器会递归构建 package、导入其 `.vyi`、自动加入静态库并传播运行时资源。
`std.serde.json` 提供 `Result` 错误、对象/数组/标量、深拷贝子值、类型检查、序列化
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
做实现宏（与 `std:serde` 相同的 C/H 混编）：

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

调用 Cacao GPU API 时，由消费 target 配置 `libs` / `lib_paths`（与
`samples/projects/cacao` 相同）。SQLite、OpenSSL 等已声明公开链接依赖的包会
传播库名和相应搜索路径；消费方仍须安装匹配目标的原生库并部署动态运行库。

`std.cjson` 平铺 binding 已移除。显式 source package 会关闭该 target 的隐式
flat-std 搜索；漏写依赖会在 import 阶段直接报错，而不是静默拾取兼容镜像。

---

## 常用入口与编译器内建

`print`、`assert`、`panic` 和 `format` 由标准库源码提供。下表同时列出
类型查询等编译器内建；不能把库函数的名字当作独立于库实现的编译器能力。

### 输出 & 调试
| 函数 | 签名 | 说明 |
|------|------|------|
| `print` | `print(val)` | 打印任意类型（i64/f64/string/bool） |
| `assert` | `assert(cond: bool, message: str = "assert failed")` | 条件为 false 时打印消息并终止 |
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

`resolve(value)` 接收结果的所有权；重复完成时，传入但未被接收的值仍会正常析构。
`await pending` 或 `pending.await_value()` 将结果转移给调用者，结果只能取一次。
`clone()` 只共享完成状态，不复制结果；通过任意句柄取过结果后，再次取值会 panic。
`poll()` / `is_done()` 在取值后仍保持完成状态。最后一个句柄销毁时，尚未取出的结果也会析构。
拥有值无需实现 `Clone`；如果应用需要多份独立结果，应在取值后显式克隆结果。

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
| `get(i: i64)` | `-> T where T: Clone` | 克隆元素（越界 panic） |
| `get_unchecked(i)` | `-> T where T: Clone` | 克隆元素，无边界检查 |
| `get_ref(i: i64)` | `-> &T` | 借用元素，不克隆（越界 panic） |
| `get_mut(i: i64)` | `-> &mut T` | 通过可变 Vec 借用原地修改元素（越界 panic） |
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
| `clear()` | | 析构存活元素并清空，保留容量 |
| `reserve(n)` | | 预留容量 |
| `shrink_to_fit()` | | 收缩容量 |
| `iter()` | `-> Iterator<T>` | 获取迭代器 |
| `destroy()` | | 析构存活元素并释放存储；重复调用无害 |

`push`、`set` 和 `pop` 转移元素所有权。`set` 会先析构被替换的元素；
缩小 `resize` 会析构移除的元素，扩大时分别克隆填充值。
用户定义的记录和枚举需要提供克隆实现，例如 `impl Clone for Type` 或 `@[derive(Clone)]`。
非泛型类型中匹配的 `clone() -> Self` 成员也可按结构化 trait 规则满足约束。

只读访问不能克隆的元素时用 `get_ref`；借用期间不要扩容、替换、移除或销毁该元素。
原地修改元素时用 `get_mut`，它要求可变借用 Vec，不复制元素所有权。
存入 `Vec`、`Box` 和 `Ref` 的原始 `str` 按分配器标记释放堆存储；字面量和借用视图不会被释放。
`Ref` 在最后一个强引用销毁时释放载荷，嵌套容器遵循相同规则。

`std.ref` 中 `Box<T>` 唯一拥有载荷，`Ref<T>` 共享载荷。两者的 `deref() -> &T`
都借用载荷，`get() -> T` 克隆载荷，因此 `get` 要求 `T: Clone`。
`Box.deref_mut(&mut self) -> &mut T` 提供唯一载荷的可变借用，也支持 `Box<dyn Trait>`。
共享借用的 `Box` 不能调用它；`Ref` 的共享载荷不提供这个操作。
`Box.clone()` 深拷贝载荷；`Ref.clone()` 增加强引用计数。载荷的借用不能比句柄活得更久。

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
| `get(key: K)` | `-> V where V: Default + Clone` | 克隆值；缺少 key 时返回默认值 |
| `try_get(key: K)` | `-> Option<V> where V: Clone` | 返回独立拥有的克隆，缺少 key 时返回 None |
| `get_ptr(key: K)` | `-> *V where K: Clone, V: Default` | 返回存储指针；缺少 key 时克隆 key 并插入默认值 |
| `contains(key: K)` | `-> bool` | 是否包含 key |
| `remove(key: K)` | `-> bool` | 删除 |
| `isEmpty()` | `-> bool` | 是否为空 |
| `count()` | `-> i64` | 元素数量 |
| `length()` | `-> i64` | 同 count() |
| `size()` | `-> i64` | 同 count() |
| `keys()` | `-> Vec<K> where K: Clone` | 所有 key 的独立克隆 |
| `values_vec()` | `-> Vec<V> where V: Clone` | 所有 value 的独立克隆 |
| `values()` | `-> DictValIterator` | 旧的 i64 值迭代接口；泛型值使用 values_vec() |
| `iter()` | `-> DictEntryIterator` | 迭代器 |
| `clear()` | | 清空 |
| `destroy()` | | 释放内存 |

Dict 拥有存入的键和值。覆盖值会析构旧值，删除会析构键和值，清空及作用域结束会
清理仍占用的条目。读取和快照返回的克隆由调用方清理，不能当作表内元素的借用。
`get_ptr` 借用传入的查询键，只在缺失时克隆它；返回指针会因删除、清空、析构或扩容失效。

`Vec<T>` 在 `T: Clone` 时实现 `Clone`；`Dict<K,V>` 在键和值都满足 `Clone`
且键满足 `Hashable + Eq` 时实现 `Clone`。这两个约束只限制克隆操作。
`Dict.get()` 返回独立的克隆值；`get_ptr()` 借用表内槽位，使用期间不得使表扩容或销毁表。

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
| `pop() -> T` | 出栈，转移元素所有权 |
| `peek() -> T where T: Clone` | 克隆栈顶，保留源元素 |
| `isEmpty() -> bool` | 是否为空 |
| `clear()` | 析构全部剩余元素并清空 |
| `destroy()` | 清空并释放存储，可重复调用 |

### Queue 方法

| 方法 | 说明 |
|------|------|
| `enqueue(val: T)` | 入队 |
| `dequeue() -> T` | 出队，转移元素所有权 |
| `peek() -> T where T: Clone` | 克隆队首，保留源元素 |
| `len() -> i64` | 长度 |
| `isEmpty() -> bool` | 是否为空 |
| `clear()` | 析构全部剩余元素并清空 |
| `destroy()` | 清空并释放存储，可重复调用 |

`push` / `enqueue` 接管传入值；`get`、`peek` 和 `clone` 返回独立副本，要求 `T: Clone`。
`set` 先析构旧值，再接管新值。只插入、移出和清理不可克隆元素时，不要求实现 Clone。
`get_ptr` 借用容器存储，容器必须比指针存活得更久，借用期间不得扩容或移除元素。
`Vec.from_array`、`Stack.from_array`、`Queue.from_array` 克隆调用方提供的裸指针范围，
由调用方保证存在 `n` 个有效元素；源元素保留所有权，在调用结束前必须保持有效。

原始 `str` 仍有编译器限制：普通方法的返回值未必取得自动作用域或临时值清理。
通过这些 API 取得拥有存储的 `str` 后，使用完须显式调用 `str_drop`；借用视图不得按拥有值释放。
需要自动作用域清理时使用 `String`。

---

## Iterator\<T\> (`std.iter`)

Vec 的 for-in 经 `iter()` 取得视图，`next()` 调用元素的 Clone 并返回独立值。
值形式、`&Vec<T>` 与 `&mut Vec<T>` 都保留源容器的元素；副本在每轮结束时清理，
移出循环时转移清理责任。元素缺少 Clone 时拒绝实例化。
`Vec.iter()` 和 `Iterator.from_raw()` 的源存储必须比迭代器活得更久。
自定义迭代器可从声明的父类继承 `next() -> Option<Item>`。元素推导沿父类声明查找；
当前类型声明的方法会遮蔽父类方法。字段布局相同不构成继承关系。

固定数组的 for-in 保留源元素，通过 Clone 取得非平凡元素；嵌套数组按结构递归复制。
每轮结束清理产生的拥有值，移出循环时转移清理责任。拥有元素的数组离开作用域时逆序清理。
`[owner; N]` 不允许重复非 Copy 的资源句柄，需分别构造各元素。
DCI shared_abi 展开帧目前拒绝拥有此类数组，因为数组的结构化异常清理尚未实现。

裸存储中的拥有值通过类型化赋值转移，例如 `unsafe { *(slot as *T) = value; }`。
调用者必须保证地址对齐、容量足够，并已清理旧值或使用未初始化的存储；接收方负责
新值的最终清理。C `memcpy` / `memmove` 只复制字节，即使复制完整对象也不会转移
源对象的所有权。不得把拥有值的字节副本再当成第二个独立拥有者。

`Display.display()` 可返回借用的 str 视图，或向调用方转移新建文本的所有权。
控制台输出会在写完后释放新建的格式化文本。自定义实现引用已有存储时，须返回真正的
借用视图，例如 `String.as_str()`，不能返回带拥有标记的别名。

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
| `from_raw(data, len)` | 从外部元素存储创建视图，初始位置为 0 |
| `next() -> Option<T>` | Clone 下一个元素；耗尽返回 None |
| `has_next() -> bool` | 是否有下一个 |
| `reset()` | 重置位置 |
| `collect() -> Vec<T>` | 收集为 Vec |
| `count() -> i64` | 剩余元素数 |
| `sum() -> i64` | 求和 |
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

`String.raw()` / `as_str()` 返回的 `str` 不拥有源存储；`String` 使用 SSO/堆存储并负责释放。`iter()`、`chars()` 和
`bytes()` 都返回借用游标，修改或销毁源 `String` 会使游标失效。

`String.fromBytes(ptr, len)` 会复制字节并返回拥有存储的 String。返回其视图时，源存储
必须在调用结束后仍然有效：可以返回 String 本身、克隆文本，或借用调用方持有的对象。
返回局部 String 的视图会报 `E3102`；局部别名、后续赋值、条件分支和循环也参与检查。

```vyx
fn view_of(owner: &String) -> str { return owner.as_str(); }
fn copy_of(owner: &String) -> str { return owner.as_str().clone(); }
```

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
| `std.serde` | Vyx 值序列化基础接口；完整 JSON 能力使用 `std.serde.json` package |
| `std.ffi` | C ABI 外部函数接口（`c_int`/`CStr`/`CString`；不是 C++） |
| `std.allocator` | 内存分配器 |
| `std.win32` | Windows API 绑定 |
| `std.sdl3` | SDL3 游戏开发绑定 |
| `std.openssl` | 加密库绑定 |
| `std.curl` | HTTP 客户端 |
| `std.stb_image` | 图像加载 |
