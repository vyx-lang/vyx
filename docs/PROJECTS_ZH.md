# 创建与配置 Vyx 项目

[English](PROJECTS.md) · [文档目录](README.md) · [入门教程](入门指南_ZH.md) · [Vyx.toml 参考](PACKAGE_MANIFEST_ZH.md)

先安装 SDK，并确认 `vyxc --version` 可运行。
以下步骤从新建项目开始，逐步添加文件和修改构建配置。

## 1. 创建、构建、运行

```sh
vyxc new hello_app
cd hello_app
vyxc build --target hello_app
vyxc --run=aot --src=project . --target hello_app
```

第二条命令构建后直接运行所选目标，自动使用清单中配置的输出路径。
也可以单独运行产物。Windows：

```powershell
.\target\hello_app.exe
```

Linux：

```sh
./target/hello_app
```

程序输出 `Hello, Vyx!`。`new` 创建：

```text
hello_app/
  Vyx.toml
  src/
    main.vyx
```

构建后生成 `target/` 和 `.cache/`。
普通应用使用默认模板即可；`--bare` 不添加默认标准库依赖。

## 2. 认识生成的配置

`Vyx.toml`：

```toml
[package]
name = "hello_app"
version = "0.1.0"
entry = "src/main.vyx"

[build]
cache_dir = ".cache"
output_dir = "target"
threads = 0

[dependencies]
core = { path = "std:core", target = "std_core" }
collections = { path = "std:collections", target = "std_collections" }

[target.hello_app]
type = "executable"
entry = "src/main.vyx"
auto_sources = false
```

| 设置 | 作用 |
|---|---|
| `package.name` / `version` | 包名称与版本 |
| `package.entry` | 默认入口文件 |
| `build.output_dir` | 输出目录 |
| `build.cache_dir` | 编译缓存目录 |
| `build.threads` | 并发任务数；0 使用硬件并发数 |
| `dependencies` | 包内目标共享的依赖 |
| `target.hello_app` | 名为 `hello_app` 的构建目标 |
| `target.entry` | 该目标的入口文件 |
| `auto_sources = false` | 使用显式源文件配置 |

`--target hello_app` 选择构建目标。
`--triplet` 选择目标平台，是另一个参数；不指定时构建本机程序。
省略 `--target` 会按清单构建目标集合，而不只是选一个入口。

`--run=aot --src=project .` 构建并运行一个可执行目标：优先选择与包同名的可执行目标，
否则选择唯一的可执行目标。有多个候选时必须指定 `--target`；库目标不能运行。
依赖、DCI stub、缓存和构建钩子与 `build` 共用，构建失败不会启动旧产物。
程序退出码作为命令退出码返回。交叉编译产物不在本机自动启动。

程序参数放在 `--` 后面：

```sh
vyxc --run=aot --src=project . --target hello_app -- "two words" --config app.json
```

`vyxc build` 是 `vyxc --src=project .` 的简写。所有项目输入都通过清单构建流程，
包括 `vyxc --emit=ir --src=project . --target hello_app`，输出到目标配置的目录，
并加载同一组依赖和 DCI 契约。`--src=file` 只编译单个文件。
清单脚本仍使用 `vyxc run <脚本名>`。
IDE 调试可用 `vyxc build --target hello_app -g -O0 --artifact-file launch.txt`，
构建成功后 `launch.txt` 以 UTF-8 写入可执行产物的绝对路径。

## 3. 添加源文件

新建 `src/math.vyx`：

```vyx
module hello_app;

public fn square(value: i32) -> i32 {
    return value * value;
}
```

将 `src/main.vyx` 改为：

```vyx
module hello_app;

fn main() -> i32 {
    print(square(6));
    return 0;
}
```

修改已有的目标段，添加 `sources`：

```toml
[target.hello_app]
type = "executable"
entry = "src/main.vyx"
auto_sources = false
sources = ["src/math.vyx"]
```

重新执行 `vyxc build --target hello_app` 并运行程序，输出 36。
两个文件属于同一模块；跨文件调用的函数标为 `public`。
在显式源文件模式下，仅把文件放进 `src/` 不等于将它加入目标。
修改已有段即可，不要在同一 TOML 文件中重复声明同名段。

### 导入独立模块

用 `module` 命名各模块，用 `use` 导入。消费方需要的函数、枚举、常量和可变
全局量须标为 `public`。例如，枚举可以写作
`use Status = Cross.GlobalEnum.Status;`，再用 `Status::Ready()` 构造。
[跨模块项目回归门](../probes/gates/cross-module/README.md) 提供完整生产端、
消费端、接口生成和原生链接步骤，覆盖枚举与整数/字符串全局量。

## 4. 修改输出、并发与优化

例如，将已有 `[build]` 段改为：

```toml
[build]
cache_dir = ".cache"
output_dir = "out"
threads = 4
```

```sh
vyxc build --target hello_app -j4
vyxc build --target hello_app -O2
```

此时运行 `out/hello_app.exe`（Windows）或 `out/hello_app`（Linux）。
目标自己的 `output_dir` 可以覆盖全局值；命令行 `-j` 覆盖默认并发数。

调试构建使用：

```sh
vyxc build --target hello_app -g -O0
```

IDE 的调试配置应指向本次生成的可执行文件，见 [编辑器与调试](TOOLING_ZH.md)。

## 5. 添加另一个目标

新建 `src/report.vyx`：

```vyx
module report;

fn main() -> i32 {
    print("report");
    return 0;
}
```

在清单末尾添加一个名称不同的目标：

```toml
[target.report]
type = "executable"
entry = "src/report.vyx"
auto_sources = false
```

```sh
vyxc build --target report
```

该目标生成 `out/report.exe` 或 `out/report`，使用前一节配置的输出目录。
`type` 还可为 `static`、`shared`、`source`；
`source` 向消费方提供源码，适合需要在消费方实例化的泛型。
编译后的用户泛型模块也可以通过生成的 `.vyi` 模板制品提供函数体；普通已编译
定义仍需链接生产端原生库。[泛型接口回归门](../probes/gates/generic_interfaces/README.md)
展示隐藏生产端源码后的消费、限定名泛型调用、私有辅助依赖，以及两个消费模块
对同一实例的链接。

## 6. 修改依赖与原生链接

默认项目已声明 `std:core`、`std:collections`，例如：

```vyx
use std.collections;
```

依赖可以放在全局 `[dependencies]`，也可以只提供给某个目标：

```toml
[target.hello_app.dependencies]
support = { path = "../support", target = "support" }
```

该例要求 `../support/Vyx.toml` 存在，并定义 `support` 目标。
`path` 相对项目目录解析；`target` 选择依赖包中的具体目标。
同一清单内的目标依赖使用 `support = { target = "support", usage = "private" }`。

预编译原生库使用链接表，替换为实际库名与目录：

```toml
[target.hello_app.link]
libs = ["native_support"]
lib_paths = ["vendor/lib"]
```

`libs` 填原生库名，不填写 Vyx 目标名称；Vyx 目标通过依赖表连接。
DCI 契约和实现产物也需要匹配，配置见 [DCI 工具指南](../tools/dci/README.zh-CN.md)。

当前注册表操作面向本地 `file://` registry。包获取与版本锁定规则见
[清单参考](PACKAGE_MANIFEST.md)，不应假定会从远程包服务自动下载依赖。

## 7. 需要提交哪些文件

提交 `Vyx.toml`、源码，以及项目使用的 `Vyx.lock`。
将可重新生成的 `target/`、`out/`、`.cache/` 写入项目的 `.gitignore`。
如果项目使用 DCI，同时维护契约生成输入、生产端源码和构建配置。

更多配置见 [Vyx.toml 参考](PACKAGE_MANIFEST_ZH.md)；
编辑器安装、LSP 和 DAP 配置见 [工具与 IDE](TOOLING_ZH.md)。
