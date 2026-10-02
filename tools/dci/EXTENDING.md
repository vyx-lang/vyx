# DCI SDK — 架构与扩展指南

DCI 工具链以 SDK 形式组织：**一个插件注册表（`dci_plugin.py`）+ 一组通过同一
注册表接入的内建实现**。内建与第三方走完全相同的扩展 API，所以"内建怎么写的"
就是"扩展怎么写"的模板。

```
tools/dci/
├── dci.py              统一 CLI（薄壳）：组合注册表 → argparse → 分发
├── dci_plugin.py       SDK 核心：参数模型 + 三个注册表 + 插件发现
├── sdk_parameters.py   统一参数词汇表：一个概念一个 dest、规范名 + 语言别名
├── sdk_adapters.py     内建语言适配器插件（cpp / rust / zig）
├── sdk_commands.py     内建命令插件（validate / inspect / encode / decode /
│                       doctor / plugins）
├── plugins/            本地插件目录：放入即生效，无需改任何核心代码
├── stub_backend.py     Stub 后端（external 参考实现；编译策略走注册表）
├── dcib.py             DCIB 规范编解码（稳定格式层）
├── dci_validate.py     独立校验器
├── dci_adapter_msvc.py / dci_adapter_rust.py / dci_adapter_zig.py
│                       适配器实现本体（也保持可独立调用）
├── active_*.py         Active Adapter 控制面（开放泛型 semantic_id 物化）
├── rust_cargo.py       Cargo 实际 rustc 参数与环境捕获、复放
├── artifact_bundle.py  事实与实现制品校验、内容身份、原子发布
├── dci_close_instances.py 消费方实例需求的补充契约闭合
├── merge_instance_layouts.py / verify_instance_layouts.py
└── pyproject.toml      可安装为 vyx-dci-sdk（console script: vyx-dci）
```

## 稳定面（兼容性契约）

改动以下任何一项都属于破坏性变更，需要升 SDK 版本号：

* `python tools/dci/dci.py <cmd> ...` 的 CLI 表面（含别名 `adapt`/`check`/`info`、
  `--` 透传、`--target` 兼容别名，以及**全部历史参数拼写**：`--cxx`/`--rustc`/
  `--zig`/`--clang`/`--crate-name`/`--rustc-arg`/`--zig-arg`/`-I`/`-j` …）。
  统一参数只是加规范名，历史拼写一律保留为别名（见 README「One parameter set」）。
* `dci.py` 的路径与 `VYX_DCI_TOOLS` 发现方式（`vyxc dci` 依赖）。
* 适配器实现模块的独立可调用性：门脚本直接调
  `dci_adapter_msvc.py` / `merge_instance_layouts.py` / `verify_instance_layouts.py`。
* `.dcib` 二进制格式与 schema（`dcib.py` / `schema/dci-1.0.schema.json`）。
* 测试导入路径 `from tools.dci import X`。

## 扩展点 1：新增一个 CLI 子命令（最常见）

在 `tools/dci/plugins/` 放一个模块（`_` 开头的文件被跳过），导入时注册即可：

```python
# tools/dci/plugins/sign_contracts.py
import argparse
try:
    from dci_plugin import CommandPlugin, register_command
except ImportError:
    from tools.dci.dci_plugin import CommandPlugin, register_command

def _run(args):
    for c in args.contracts:
        print(f"sign {c}")
    return 0

register_command(CommandPlugin(
    name="sign",
    help="example: sign contracts",
    add_arguments=lambda sub: sub.add_argument("contracts", nargs="+"),
    handler=_run,
))
```

立刻生效：`dci sign a.dcib`、`dci -h`、`dci plugins` 里都会出现。坏插件只会在
stderr 上告警并被跳过。该行为仅隔离插件发现错误：选择不存在的 Adapter/backend
仍须失败，不能因此消费未验证契约、猜测布局或省略所需的实现制品。

## 扩展点 2：新增一门语言适配器

实现三个钩子并注册 `AdapterPlugin`（参考 `sdk_adapters.py` 里的 rust 实现）。
**所有钩子只收一个参数**：一个 `AdapterRequest`：

```python
from dci_plugin import AdapterPlugin, AdapterRequest, register_adapter
from sdk_parameters import OptionSpec, param

def build_argv(request: AdapterRequest) -> list[str]:
    # request.sources / .output / .target / .debug_json / .stub_out / .artifacts
    # request.compiler / .compiler_args / .namespace —— 统一名，与你选用的拼写无关
    return [str(request.sources[0]), "--output", str(request.output),
            "--swiftc", request.compiler or "swiftc"]

def run(request: AdapterRequest) -> int:
    # request.argv 已经由 build_argv 填好，这里通常转调你的适配器模块 main
    return swift_adapter.main(list(request.argv))

def doctor(request: AdapterRequest) -> int:
    print(f"Swift frontend: {request.compiler or '(discovered)'}")
    return 0

register_adapter(AdapterPlugin(
    language="swift",                       # 即 --language swift
    parameters=(
        param("compiler", "--swiftc"),      # 统一名 + 本语言别名
        OptionSpec(dest="sdk_path", flags=("--sdk-path",), metavar="DIR",
                   help="Swift SDK used for the measurement"),
    ),
    build_argv=build_argv,
    run=run,
    doctor=doctor,
    max_sources=1, input_noun="crate-root .swift",
    # validate=lambda request: "..."  # 声明表达不了的规则才用它
))
```

注册后自动获得：

* `dci adapter --language swift ...` 分发，**参数表由你的声明组装**——`-h`、
  用法行、`dci doctor` 都跟着变，核心代码一行不动；
* 跨语言诊断：只有你声明的拼写在别的 `--language` 下会被拒绝（`--swiftc
  (--language swift)`），`--compiler` / `--namespace` 这类统一名自动共用；
* 输入规则（`max_sources` / `input_noun`）与 `dci plugins` 列表。

统一参数名在 `sdk_parameters.py` 的 `UNIFIED` 里，一个概念一行；用
`param("<dest>", *别名)` 声明支持它。语言私有参数直接写 `OptionSpec`。

适配器实现本体建议仍是一个可独立调用的模块（沿用 `dci_adapter_<lang>.py`
惯例），SDK 只负责分发。

### 真实项目接入要求（2026-10-01）

适配器应复放生产端实际构建环境，并保持公共协议与普通库名无关。
当前 Rust Cargo 路径在 `rust_cargo.py` 捕获选定 package 的真实 rustc 调用及环境，
包含依赖、features/cfg、proc macros、build scripts 与 `OUT_DIR`，供布局和 ABI 探针复用。
`dci_adapter_rust.py` 的 `--manifest-path`、`--package`、`--item`、`--opaque-type`、
`--emit-views`、`--native-lib-out` 是独立入口选项，尚未声明到统一 SDK 参数表。
后续接入统一 CLI 应继续使用 `OptionSpec`/`AdapterRequest`，并覆盖无 positional
source 的 Cargo 输入规则，不能只透传参数却仍访问 `request.sources[0]`。

扩展验收应包含：

- 生产端原始 public API 与真实依赖图；不为夹具库名添加 Consumer 分支。
- 可验证的表示、ownership/lifecycle、失败模型与目标 ABI；证据不足时拒绝。
- 契约及其实现制品、依赖和 provenance 一起更新；生成 bridge 明确计入制品。
- 消费方按需实例闭合及生产端约束拒绝；实例补充目前最多三轮 job 重试，
  不等同于完整全局闭包或 CGU 冻结协议。
- AOT O0/O2 与独立原生 oracle，以及缓存失效、符号身份和失败诊断。

可参考 [Rust Cargo 门](../../probes/gates/dci-rust-ecosystem/README.md)、
[C++ ICU 门](../../probes/gates/dci-cpp-ecosystem/README.md)、
[原始 vector 门](../../probes/gates/dci-vector/README.md) 和
[工业压测编排](../../probes/gates/dci-industrial/README.md)。这些结果覆盖具名场景，
不宣称任意语言库接入或工业级完整性。JIT 是后续任务，当前以 AOT 为验收基准。

> **1.1.0 破坏性变更**：`AdapterPlugin` 的钩子签名由
> `build_argv(args, headers, output, debug_json, target)` / `reject(args)` 改为
> 统一的 `build_argv(request)` / `run(request)` / `doctor(request)` 加声明式
> `parameters`。第三方适配器迁移：删掉 `reject`（把规则写成参数声明），
> 把 `args.xxx` 换成 `request.xxx`。

## 扩展点 3：新增 Stub 编译后端

`stub_backend.py` 的 `compile` 分发走注册表，后端名与 Vyx 构建系统一致
（内建：`clang-cpp`、`rustc`）：

```python
from dci_plugin import StubBackendPlugin, register_stub_backend

register_stub_backend(StubBackendPlugin(
    name="my-cc",
    compile=lambda args: subprocess.run([...]).returncode,
    languages=("cpp",),
))
```

## 分发式安装（pip）

```bash
pip install -e tools/dci          # 开发模式；console script: vyx-dci
pip install "vyx-dci-sdk[validate]"   # 附带 jsonschema 校验支持
vyx-dci doctor                     # 与 python tools/dci/dci.py doctor 等价
```

安装形态下，第三方包用 importlib entry points 接入（无需碰 plugins/ 目录）：

```toml
# 你的包的 pyproject.toml
[project.entry-points."vyx_dci.commands"]
sign = "my_pkg.dci:sign_command"

[project.entry-points."vyx_dci.adapters"]
swift = "my_pkg.dci:swift_adapter"
```

## 自省

* `dci plugins` — 当前注册的适配器 / 命令 / 后端 + 来源（builtin / plugins /
  entry-point）。
* `dci plugins --json` — 机器可读快照：`adapters[*].parameters` 是各语言声明的
  拼写，`parameters` 是**合并后的参数表**（每个 dest 的全部拼写、拥有它的语言、
  取值形态）。构建工具可据此生成补全脚本 —— 补全表与 CLI 不会各说各话。

## 版本

SDK 版本定义在 `dci_plugin.py` 的 `SDK_VERSION`（`dci --version` 输出），
`pyproject.toml` 的 `version` 需与其保持同步。契约格式版本独立演化
（`CONTRACT_FORMAT` / schema），两者互不捆绑。
