# DCI 工具

[English](README.md) · [文档目录](../../docs/README.md) · [DCI 规范](../../docs/DCI_SPEC_ZH.md) · [MOSP](../../docs/MOSP_ZH.md)

本目录包含生产端 Adapter、契约校验与编码、Active Adapter 会话和 Stub 后端。
统一命令为 `python tools/dci/dci.py`；`vyxc dci` 从 SDK 邻近目录或
`VYX_DCI_TOOLS` 查找同一套工具。

DCI 遵循 MOSP 模型：生产端负责源语言语义，消费端依据经过验证的事实执行特定目标上的操作。
契约承载闭合 ABI 事实；Active / Online Adapter 可以在构建期间请求新事实与实现产物。

## 统一 SDK 架构

SDK 1.1 使用同一套参数模型与注册接口组织内建和第三方实现，契约格式版本独立保持为 1.0。

```text
CLI / vyxc dci
    → 声明式参数表 → AdapterRequest
    → AdapterPlugin → 生产端事实
    → 校验 / 编码 / Stub / 产物交付
```

| 组成 | 作用 |
|---|---|
| `sdk_parameters.py` | 统一参数语义与标准拼写 |
| `OptionSpec` / `param()` | 声明参数类型、别名、支持语言与帮助文本 |
| `AdapterRequest` | `build_argv(request)`、`run(request)`、`doctor(request)` 共用的请求对象 |
| `AdapterPlugin` | 注册语言生产端及其参数和输入规则 |
| `CommandPlugin` | 注册 CLI 子命令 |
| `StubBackendPlugin` | 注册 Stub 编译策略 |

同一概念按 `dest` 合并为一个参数。例如 `--compiler` 是统一名，
`--cxx`、`--rustc`、`--zig` 是对应语言的兼容别名。
跨语言误用诊断由参数归属自动生成；新增语言通过注册声明接入，不需要另写核心 CLI 分支。

插件从内建注册、本地 `plugins/*.py` 和已安装包的 entry points 加载。
扩展点分别是 `vyx_dci.adapters`、`vyx_dci.commands`、`vyx_dci.backends`。

```sh
python tools/dci/dci.py plugins --json
```

该命令输出 SDK/契约版本、已注册实现，以及合并后的参数名称、形态和所属语言。
安装与完整扩展示例见 [EXTENDING.md](EXTENDING.md)。

## 环境与入口

需要 Python 3.10 或更新版本，以及所选语言的生产端工具链。
C++ 提取使用 Clang 作为结构化辅助工具，由选定的 Clang、GCC 或 MSVC 决定 ABI 事实；
Rust 使用选定的 rustc，Zig 使用 Zig。

下列命令在仓库根目录执行：

```sh
python tools/dci/dci.py --help
python tools/dci/dci.py doctor --language cpp --toolchain clang
python tools/dci/dci.py doctor --language rust
python tools/dci/dci.py plugins
```

其他入口包括 `vyxc dci ...`、Windows 下的 `tools\dci\dci.cmd`，
以及安装后的 `vyx-dci` 命令。安装与插件 API 见 [SDK 扩展指南](EXTENDING.md)。

## 生成与检查契约

以下示例使用仓库中的 C++ 开放泛型生产端：

```sh
python tools/dci/dci.py adapter --language cpp tests/projects/dci_opengeneric/native/lib.hpp --toolchain clang -o contracts/native.dcib --debug-json
python tools/dci/dci.py validate --strict contracts/native.dcib
python tools/dci/dci.py inspect contracts/native.dcib
python tools/dci/dci.py decode contracts/native.dcib contracts/native.dci.json
python tools/dci/dci.py encode contracts/native.dci.json contracts/native.dcib
```

契约描述实现；生成契约不等于完成实现的编译与链接。
开放泛型调用还需要下文的 Active Adapter 配置。

`.dcib` 是 Vyx 构建消费的规范二进制格式。JSON 用于检查、校验和显式转换。
经过当前 cJSON 模型的整数必须位于精确范围 `±2^53` 内。

### 一套参数

`dci adapter` 一个概念只有一个参数。规范名与该语言的别名选中的是同一个东西，
统一名称用于共享概念；语言专用别名和选项只能在所属语言下使用：

| 规范名 | 各语言别名 | 含义 |
|---|---|---|
| `--compiler` | `--cxx`（cpp）· `--rustc`（rust）· `--zig`（zig） | 要运行的产出方编译器 |
| `--compiler-arg` | `--cxx-arg`/`--frontend-arg`（cpp）· `--rustc-arg`（rust）· `--zig-arg`（zig） | 给它的一个额外参数；可重复 |
| `--extractor` | `--clang`（cpp） | 结构化事实提取器（clang++/clang-cl） |
| `--extractor-arg` | `--clang-arg`（cpp） | 给提取器的一个额外参数；可重复 |
| `--namespace` | `--crate-name`（rust、zig） | 契约里记录的 DCI 命名空间 |
| `-o`/`--output`、`--triplet`/`--target`、`--debug-json`、`--stub-out`、`--artifact` | — | 所有语言都接受 |

各语言另外还声明只有自己懂的参数：C++ 有 `--toolchain`、`--std`、`-j`、
`--include`、`-I`、`--project-root`、`--scan-public-root`、`--cmake-build-path`、
`--compile-flags`；Rust 有 `--edition`、`--deny-rejected`、
`--export-active-requests`、`--export-instance`；Zig 有 `--deny-rejected`。
用错别的语言的参数会直接报错并指出归属，不会被静默忽略：

```console
$ dci adapter --language rust lib.rs --extractor clang++
dci: error: adapter option(s) belong to another language adapter:
--extractor (--language cpp); this run is --language rust
```

`dci plugins --json` 会打印合并后的参数表（`parameters`），它就是插件声明的东西，
写法见 [EXTENDING.md](EXTENDING.md)。

### C++ 配置

| 选项 | 作用 |
|---|---|
| `--toolchain clang\|gcc\|msvc` | 选择决定 ABI 事实的编译器 |
| `--compiler`（`--cxx`） | 指定该编译器的可执行文件 |
| `--extractor`（`--clang`） | 指定结构化提取辅助工具 |
| `--triplet` | 指定目标；`--target` 是兼容别名 |
| `-I`、`--std` | 包含目录与语言标准 |
| `--extractor-arg`、`--compiler-arg` | 辅助工具或选定编译器的额外参数 |
| `--cmake-build-path` | 从 `compile_commands.json` 读取构建参数 |
| `--compile-flags` | 读取参数文件或追加单个参数 |

Linux GCC 生产端示例，头文件和构建目录应替换为实际库的路径：

```sh
python tools/dci/dci.py adapter --language cpp include/Api.hpp --toolchain gcc --compiler g++ --triplet linux_x64 --cmake-build-path cmake-build-release -o contracts/Api.dcib
```

以 `-` 开头的内联参数使用 `--compile-flags=-DFEATURE_ON=1`。
下划线形式 `--cmake_build_path` 与 `--compile_flags` 也可使用。

布局、符号身份与聚合传参方式必须对应选定的生产端和产物。
编译器版本字符串用于记录来源，不要求消费端编译器与生产端版本相同。

### Rust 配置

```sh
python tools/dci/dci.py adapter --language rust tests/projects/dci_opengeneric/native/lib.rs --namespace native_api --compiler rustc -o contracts/native_rust.dcib --debug-json
```

`--triplet` 指定目标，`--edition` 指定语言版本；重复的 `--compiler-arg`（`--rustc-arg`）
传递编译参数，`--artifact` 记录契约描述的对象文件或库。`--deny-rejected` 使存在拒绝导出的
实体时生成失败，`--export-instance TYPE<ARGS>` 点名宿主引用的闭合泛型实例
（在 Vyx 构建内实例由消费方自动发现，见「开放泛型与 Active Adapter」）。

真实 Cargo package 当前使用独立 Rust Adapter 的 `--manifest-path` 与 `--package`。
Cargo 专用参数尚未注册到统一 SDK CLI 的 Rust 参数表。示例：

```sh
python tools/dci/dci_adapter_rust.py --manifest-path probes/gates/dci-rust-ecosystem/native/Cargo.toml --package adler2 --item adler32_slice --locked --offline --target x86_64-pc-windows-msvc --emit-views --native-lib-out contracts/adler2.lib --artifact contracts/adler2.lib -o contracts/adler2.dcib
```

`--features`、`--no-default-features`、`--locked`、`--offline` 交由 Cargo 执行。
Adapter 复放 Cargo 实际选择的依赖图、build-script 环境和原生产端编译参数，
不手工猜测 `--extern` 路径。`--item` 按 public 路径选择操作；`--opaque-type`
请求 rustc 测量的闭合记录及显式 drop；`--emit-views` 测量 fat reference 的具名 view；
`--native-lib-out` 为受支持的普通 public 函数生成可链接的 native bridge staticlib。
[真实 Rust 生态门](../../probes/gates/dci-rust-ecosystem/README.md) 使用 registry 中的
`crc32fast`、`adler2`，O0/O2 与独立 Cargo oracle 对比。该门不需要手写 C wrapper，
但生成的 bridge 是明确的实现制品，不代表任意 crate ABI 或无需桥接的 Rust 调用。

Adapter 使用选定的 rustc 探测布局和 ABI lowering。已有路径包括 C/system 边界，
以及实测的 Rust 原生表示、引用、胖指针与 trait 派发。这些事实属于该生产端环境，
不代表跨 rustc 版本稳定的 ABI。生产端环境改变后，应重新生成契约与匹配产物。

未闭合泛型定义不能作为具体 ABI 实体导出。带数据的枚举、缺少生命周期证据的 `Drop` 类型，
以及 Adapter 无法验证的签名仍可能被拒绝。开放泛型请求走下面的构建期路径。

## 开放泛型与 Active Adapter

Active / Online Adapter 是 Adapter 的一种事实生产模式。
“Online”表示构建期间可调用，不要求联网。

```text
Vyx 调用点 → 闭合请求 → 生产端约束检查与实例化
           → 已验证事实 + 实现产物 → 原生链接
```

重载选择、trait/concept 检查、模板实例化和单态化由生产端执行；
Vyx 不重新实现这些源语言规则。表示兼容的类型可以参与受支持的操作，
该路径不做通用对象布局转换。

### 函数请求与记录布局

当前[开放泛型项目](../../tests/projects/dci_opengeneric/README.md)区分两种要求：

- 泛型函数调用生成闭合的 `.dci_open` 请求，交给 external 后端。
- 泛型记录在 Vyx 发射代码前需要大小、对齐和字段偏移；
  所用实例布局必须已经进入契约。

实例需求由消费方驱动：lowering 把它实际物化的每个泛型实例（包括只作为被调方法
返回类型出现的实例）写成 `.dci_open` 的 `instance <类型文本>` 行，
[dci_close_instances.py](dci_close_instances.py) 让生产端 adapter 闭合恰好这些实例，
产出补充 `.dcib`，失败的编译 job 再对新契约重新 lowering，每个 job 最多三轮实例闭合。
这不是完整的全局语义与制品闭包。Vyx 构建内不需要手写
`--export-instance` 名单；该旗标仍是显式请求路径（生产端在 Vyx 之外使用时，
或闭合器把生产端以「无实测布局」拒绝的实例回喂重试时）。同一实例可能以消费方拼写、
生产端拼写或 C++ 原语拼写出现，闭合器先规范化（折叠空白、模块前缀、
`unsigned int` -> `u32`）再匹配。多根请求以换行拼接的单个 `--descriptor` 传入，
各根分别解码。

[原始 vector 门](../../probes/gates/dci-vector/README.md) 从仅包含 `<vector>` 的头文件
和开放 `std.vector<T>` 声明开始，消费方直接写
`var a = std.vector<i32>{1,2,3,4,5};`，没有预先导出具体 vector API。
`@[dci_list_init(true)]` 请求生产端检查 C++ brace 构造，包括 narrowing 拒绝；
生成的构造入口与标准库原始成员符号仍是边界的明确组成部分。

SDK 1.1 的统一参数注册表已接入 Rust 实例布局导出与 Active 请求选项。
选择 `--language rust` 后，可直接使用统一入口：

```sh
python tools/dci/dci.py adapter --language rust tests/projects/dci_opengeneric/native/lib.rs --namespace open_generic --export-instance "Pair2<i32,f64>" --export-active-requests -o contracts/open_generic.dcib
```

项目需要生产端到消费端的名称映射或补充实例布局声明时，先合并，再用生产端编译器复核：

```sh
python tools/dci/merge_instance_layouts.py contracts/open_generic.dcib tests/projects/dci_opengeneric/dci/instance_layouts.json
python tools/dci/verify_instance_layouts.py contracts/open_generic.dcib tests/projects/dci_opengeneric/dci/instance_layouts.json --lang rust --provider tests/projects/dci_opengeneric/native/lib.rs
```

合并工具会原地修改契约。写入布局声明本身不能证明生产端实际使用该布局，
后续验证负责完成这一检查。完整实例列表与生成命令见
[run_vyx.sh](../../tests/projects/dci_opengeneric/run_vyx.sh)。

### 构建配置

项目选择 `dci_stub_backend = "external"`，并提供：

| 清单字段 | 含义 |
|---|---|
| `dci_stub_backend_tool` | 后端可执行程序，例如 `python` |
| `dci_stub_backend_tool_args` | 后端脚本与 `--provider` 源文件 |
| `dci_stub_backend_source_extension` | 生成源码的后缀，例如 `rs` 或 `cpp` |
| `dci_stub_backend_dependencies` | 影响结果的脚本、生产端源码与配置 |
| `dci_stub_backend_version` | 后端版本或配置缓存盐 |
| `dci_stub_backend_capabilities` | 提供的能力，包括 `emit-source`、`compile-object` |

完整配置见项目的 [Vyx.toml](../../tests/projects/dci_opengeneric/Vyx.toml)。
在该项目目录构建 Rust 或 C++ 目标：

```sh
vyxc build --target dci_opengeneric
vyxc build --target dci_opengeneric_cpp
```

两个目标使用各自的生产端契约。调用形式可以相同，契约身份与产物仍需分别匹配。

### 协议与产物 API

| 模块 | 职责 |
|---|---|
| `active_protocol.py` | 查询、解析结果、环境身份、实体/请求/产物包键 |
| `active_cpp.py`、`active_rust.py` | 生产端会话与语义检查 |
| `artifact_bundle.py` | 内容验证、确定性清单与原子发布 |
| `stub_backend.py` | external 后端的 `emit` / `compile` 命令协议 |

Active Adapter 模块是 Python API，不是额外的 CLI 子命令。
产物包关联事实、实现文件和依赖；物化失败时不得发布不完整结果。

## 生命周期、异常与 Stub

- `dci-ownership` 注解补充无法直接推导的所有权事实。
- Rust 的 `dci-lifecycle` 注解将复制、移动和销毁操作绑定到符号。
  非平凡值除布局兼容外，还需要生命周期证据。
- `profile.lifecycle_binding = automatic` 启用 Vyx 已支持的局部复制、移动与清理改写，
  不代表完整的控制流所有权分析。
- 指针表示的自有 DCI 局部对象析构后释放模块分配的存储。内联记录只析构，
  不释放堆存储；借用参数遵循生产端 ownership，不由消费方释放。
- 默认边界为 `no_unwind`。`translated` 使用生产端编译的转换函数。
  AOT `shared_abi` 实现 x86_64 Windows MSVC（`dci.eh.msvc-cxx.v1`）与
  x86_64 Linux（`dci.eh.itanium-cxx.v1`）的真实 C++ 异常传播。参与帧按逆构造顺序
  清理已完成的作用域对象，DCI 构造失败只释放存储，不调用析构；原生异常身份保持。
  展开时析构再次抛异常会终止。未知 ABI/版本/目标、未参与调用方与不支持的清理签名
  均拒绝。它不合成通用 catch/throw；Rust panic、Zig error、`longjmp` 仍需自己的
  受支持边界策略。JIT 验收暂缓。
- `exports.stub_requests` 描述所需桥接。受支持的 `forward_direct` 和
  `reverse_override` 请求有 Consumer 合成路径；其他操作需要具备相应能力的后端。

能力边界和源码、测试入口见 [DCI 当前实现说明](../../docs/DCI_SPEC_ZH.md#16-vyx首个实现与标准基线)。

## 工具与验证

| 工具 | 用途 |
|---|---|
| `dci.py` | Adapter 分发、校验、检查、编解码与诊断 |
| `dci_adapter_cpp.py` | C++ 入口；实现在 `dci_adapter_msvc.py` |
| `dci_adapter_rust.py`、`dci_adapter_zig.py` | Rust 与 Zig 生产端 |
| `dci_validate.py` | Schema 与契约语义检查 |
| `dcib.py` | 规范编码与解码 |
| `merge_instance_layouts.py`、`verify_instance_layouts.py` | 实例名称/布局合并与生产端验证 |
| `dci_close_instances.py` | 构建内置闭合器：把 `.dci_open` 的 `instance` 请求变成生产端闭合事实（补充 `.dcib`） |
| `dci_plugin.py` | 命令、Adapter 与后端注册 |

CLI 工具均提供 `--help`。协议与编码检查示例：

```sh
python -m unittest tools.dci.tests.test_dcib tools.dci.tests.test_active_protocol tools.dci.tests.test_artifact_bundle tools.dci.tests.test_dci_adapter_parameters
```

生产端和 Consumer 测试还需要对应工具链与编译器产物。参见
[tests/](tests/)、[开放泛型项目](../../tests/projects/dci_opengeneric/README.md)
和 [DCI 规范](../../docs/DCI_SPEC_ZH.md)。

构建缓存包含契约内容与后端依赖。经 `dci_stub_backend_tool_args` 传入的脚本，
也应列入 `dci_stub_backend_dependencies`；仅记录 `python` 不能代表脚本内容。

## AOT 生态与压测门

先从当前源码构建 SDK 编译器，再将 `bootstrap_compiler/out/vyxc.exe` 作为被测
Consumer，并使用同次构建的 backend/runtime。PATH 中的 Release SDK 编译器仅作为
Stage 0 完成初始自举。在 Windows 仓库根目录运行：

```powershell
./probes/gates/dci-vector/run.ps1 -Compiler bootstrap_compiler/out/vyxc.exe
./probes/gates/dci-cpp-ecosystem/run.ps1 -Compiler bootstrap_compiler/out/vyxc.exe
./probes/gates/dci-rust-ecosystem/run.ps1 -Compiler bootstrap_compiler/out/vyxc.exe
./probes/gates/dci-exceptions/run.ps1 -Compiler bootstrap_compiler/out/vyxc.exe
./probes/gates/dci-industrial/run.ps1 -Compiler bootstrap_compiler/out/vyxc.exe -Repeat 2 -Scale 2 -Parallel 3
```

[工业压测编排](../../probes/gates/dci-industrial/README.md) 记录状态、子进程耗时、日志和
进程树 RSS 采样；RSS 缺失会明确标记不可用，exit 77 记为跳过。它组合真实夹具与回归，
不等同于工业级已完成。Linux 共享传播有独立的
[WSL 门](../../probes/gates/dci-exceptions/run-linux.ps1)。DCI 必跑项目仍为
`dci_cpp_trait`、`dci_rust_trait`、`dci_multilang`、`dci_zig_abi`。
旧 `dci_spdlog` 项目与 `gate-c` 夹具已退役，不再作为验收命令。
