# DCI 工具

[English](README.md) · [文档目录](../../docs/README.md) · [DCI 规范](../../docs/DCI_SPEC_ZH.md) · [事实语义所有权系统](../../docs/MOSP_ZH.md)

本目录包含生产端 Adapter、契约到 Vyx 的 Converter、契约校验与编码、Active Adapter 会话和 Stub 后端。
统一命令为 `python tools/dci/dci.py`；`vyxc dci` 从 SDK 邻近目录或
`VYX_DCI_TOOLS` 查找同一套工具。

DCI 遵循 事实语义所有权系统 模型：生产端负责源语言语义，消费端依据经过验证的事实执行特定目标上的操作。
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
`--compile-flags`、`--boundary`；Rust 有 `--edition`、`--deny-rejected`、
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

#### 独立 Adapter 与 Converter

DCI 使用独立工具链。先由生产端 Adapter 测量并验证契约，再显式转换成可查看的 Vyx 定义：

```sh
vyx-dci adapter --language cpp include/Api.hpp --toolchain clang -o contracts/Api.dcib
vyx-dci convert contracts/Api.dcib --module native.api --header Api.hpp -o src/native_api.vyx
```

未安装工具时，使用 `python tools/dci/dci.py` 的相同命令。
`converter` 和 `convertor` 是 `convert` 的别名。输出是普通 Vyx 模块，使用
`@[dci_import(...)]` 和 `extern "dci"`，保留类型身份、公开基类、构造/析构函数、
方法、const 和 virtual 签名。Vyx 派生类依据这些签名实现 `override`；Consumer
将人工维护的声明与原始契约交叉验证。转换不测量或改变 ABI、ownership、lifecycle。
已有定义文件只有显式传入 `--force` 才能覆写。`--report` 保存不能表达的声明诊断；
`--deny-rejected` 使转换在这些诊断存在时失败。当前自动声明覆盖全局 C++ 类名和
可表达的方法/自由函数签名，未覆盖全部 C++ 语法。

ownership 的提取和规范化属于 Adapter。标量、引用和实测生命周期操作可以自动提供
部分事实；裸指针的保留、转移与释放需要生产端语义或库协议的权威声明。
事实缺失由 Adapter 拒绝，Converter 只消费契约，不推断所有权。
Qt 协议由 Adapter 的 SDK profile 提供。布局、继承偏移、虚表和 ABI 符号继续由
生产端编译器实测。导出范围由用户声明：`--export-type` / `--export-function`
选择精确实体；不带筛选的 Adapter 调用表示用户显式选择输入的公共头文件范围。
消费端调用、inline 定义不会选择或扩张范围。范围内可表达的实体全部输出，不能
表达的实体记录诊断；布局和基类依赖由 Adapter 测量以验证所选 API。

标量/null 默认参数是原始签名上的声明事实。Converter 生成可支持的 Vyx 默认参数，
Consumer 在调用处补齐参数，仍直接调用原生符号。内置常量表达式和枚举常量交由
生产端编译器求值。未知或动态默认值记录诊断，要求调用者显式提供参数，不生成
省略参数的包装重载。`cpp_include` 引用原始 API 头文件。
提取器与生产端编译器或配置不一致时，也不采用其展开的默认值；原生完整签名仍可
通过显式参数调用。

[Qt 门](../../probes/gates/dci-qt-counter/README.md) 在 `src/qt_widgets.vyx` 维护可见定义，
在项目构建时准备契约。`cpp-import Vyx.toml --target app --import widgets`
可以独立准备一个库，不要求目标启用自动源码导入。

#### 可选的声明范围构建准备

目标中的可选 `dci_imports` 将生产端原始头文件接入普通 `vyxc build`。
每个库只配置模块、头文件和工具链：

```toml
[target.app]
type = "executable"
entry = "src/main.vyx"
dci_imports = ["widgets"]
dci_stub_backend = "clang-cpp"
# cxx、cxxflags 和 include_paths 使用正常的原生工具链配置。

[dci.import.widgets]
module = "qt.widgets"
contract = "contracts/qt_widgets.dcib"
# 可选：使用已维护的 Converter 定义文件，不生成第二份缓存模块。
definitions = "src/qt_widgets.vyx"
headers = ["native/widgets.hpp"]
export_types = ["QWidget", "QPushButton"]
ownership_headers = ["native/widgets_ownership.hpp"]
# 可选 Qt 连接操作，需要显式选择：
qt_connections = ["QAbstractButton::clicked(bool)"]
profile = "qt"
boundary = "shared_abi"
```

编译器在收集源码之前运行 `dci cpp-import Vyx.toml --target app`，把选定的
Vyx 定义文件和准备好的原生源码纳入构建图。`contract` 默认输出到项目的
`contracts/<导入名>.dcib`。设置 `definitions` 时，这个 Vyx 文件必须已存在，
Adapter 不覆写它，也不生成第二份模块；省略时才使用缓存生成的 Converter 模块，
其中引用的仍是项目契约。类型身份、签名、基类和重载来自生产端实测事实。
构建系统传入已解析的模板与平台配置，适配器复用原生编译的 C++ 参数和头文件路径。
用户必须声明 `export_types` / `export_functions`，或显式选择 `export_all = true`
覆盖输入公共头文件的范围。不扫描应用源码决定导出。所选类输出所有可表达成员，
包括未被应用使用的成员，并测量基类/布局依赖。原始声明提供签名、重载及可支持
的默认参数。Qt 协议只为 `qt_connections` 明确列出的原生信号生成有类型的
`on_<signal>` 连接操作；看到信号本身不会触发导出。这是可选适配操作，与原生
信号方法分开。状态地址必须存活到
连接结束；sender/context 析构会断开连接。未知载荷生命周期会拒绝适配。
`ownership_headers` 显式选择可复用的库事实声明，例如 SDK 的
`profiles/qt_widgets.hpp`，不会仅因 profile 名称而自动启用。也可以在公共头文件
的 `dci-ownership` 中手写一次。未知指针生命周期记录诊断并拒绝，不猜测补全。

项目契约保存在 `contracts/`；生成定义、原生物化源码、依赖戳和拒绝原因保存在
`.cache/dci/<导入名>/`。删除缓存不删除项目契约或用户定义，下一次普通构建会
从已有契约重建缺失产物，不重新提取 AST。离线 Adapter 契约会检查 schema、
目标 ABI、异常边界和所选导出，并保留原文件。缺少已测量的 Qt 连接操作会明确
报错；无效或不兼容的输入契约不会被擅自替换。

契约缺失、用户显式执行 `cpp-import --force`，或已生成契约记录的生产端输入
发生变化时才重新测量。生成契约携带可选的 `source.preparation_inputs`，记录
头文件内容哈希和原生配置，不依赖 `.cache`。编译器版本注释和 DCI 工具更新
不会让这些事实自动失效。普通离线契约可以没有此元信息，其库版本由提供者管理。
无法从 C++ 类型推断的指针所有权仍由库的权威事实声明提供。
ABI 缺失或不同原生重载坍缩为同一个消费端签名时记录诊断，不生成可执行绑定。
缓存身份包含传递头文件内容、用户声明的导出范围、生产端编译器与工具内容。
应用源码变化不会扩张范围，也不会使未变化的生产端事实失效。

普通 C++ 库默认使用 `profile = "cpp"`。Python 3.11 自带 TOML 读取器；
Python 3.10 使用 SDK 的 `tomli` 依赖。
缓存生成定义是可选的。维护显式 Converter 定义的项目通过 `definitions` 指定文件，
普通构建仍负责准备契约。独立 Adapter / Converter 命令继续可用。见
[普通 C++ 工程](../../tests/projects/dci_auto_import/README.md)。
另见[导出准备门](../../probes/gates/dci-auto-import/README.md)，覆盖内容失效、
生成产物修复和用户控制的导出范围。

`--boundary shared_abi` 声明共同的 C++ 异常传播 ABI，保留原异常身份，由 Vyx
生成传播与栈对象清理代码；该模式不生成 `translate_unwind.cpp`。
当前目标限于已实现的 x86_64 MSVC / System V ABI，调用双方必须使用兼容的
展开机制。默认 `--boundary no_unwind` 保留边界内捕获和显式错误转换路径。

该选项通用于 C++ 项目。输入库的原始公开头文件，例如：

```powershell
python tools/dci/dci.py adapter --language cpp include/Api.hpp --boundary shared_abi --triplet windows_x64 -o contracts/Api.dcib
```

Vyx 消费契约时生成异常传播、对象清理和所需的原生桥接。接入不要求手写
`watch`、`throw` 或 `call_visible` 等测试辅助函数，也不要求创建名为
`exception_probe.hpp` 的文件；Qt 回归夹具中的这些函数只用于制造异常和观测清理。
库的所有权事实、析构声明和构建/链接配置仍需正确提供。

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
- 跨展开边界的契约可以设置 `profile.obligation_mode = "required"`，并在根节点写入
  `obligations`（`version`、`lifecycle`、`exceptions`、`edges`）。生命周期事实绑定必须
  存在的清理符号，异常事实绑定传播 ABI 与清理策略，边把不同生产模块的事实连成图。
  `dci validate` 与 `dci_validate.py` 会合并全部输入契约，缺节点、冲突声明、依赖环，或
  没有 `cleanup-before-propagate` 边的 `shared_abi` 异常都会拒绝。

能力边界和源码、测试入口见 [DCI 当前实现说明](../../docs/DCI_SPEC_ZH.md#16-vyx首个实现与标准基线)。

## 工具与验证

| 工具 | 用途 |
|---|---|
| `dci.py` | Adapter 分发、校验、检查、编解码与诊断 |
| `dci_adapter_cpp.py` | C++ 入口；实现在 `dci_adapter_msvc.py` |
| `dci_adapter_rust.py`、`dci_adapter_zig.py` | Rust 与 Zig 生产端 |
| `dci_validate.py` | Schema 与契约语义检查 |
| `dci_obligations.py` | 跨模块生命周期/异常事实图及 fail-closed 校验 |
| `../effect_manifest.py` | Effect manifest 与 `MOSP-DCI-EXPORT` 侧车 provenance 检查 |
| `dcib.py` | 规范编码与解码 |
| `merge_instance_layouts.py`、`verify_instance_layouts.py` | 实例名称/布局合并与生产端验证 |
| `dci_close_instances.py` | 构建内置闭合器：把 `.dci_open` 的 `instance` 请求变成生产端闭合事实（补充 `.dcib`） |
| `dci_plugin.py` | 命令、Adapter 与后端注册 |

CLI 工具均提供 `--help`。协议与编码检查示例：

`VYX_DCI_EXPORT_OUT` 用于选择 SDK 编译器的数据导出侧车。生成桥接之前先执行
`python tools/effect_manifest.py validate-dci-export <sidecar> --manifest <同次构建manifest>`；
`inspect-dci-export <sidecar> --json` 会显示封存 EffectPlan fingerprint 和已授权声明行。
侧车本身不声明 C++/Rust/Zig ABI 或布局。

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
