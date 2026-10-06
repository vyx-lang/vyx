# Vyx.toml 项目清单

[English](PACKAGE_MANIFEST.md) · [创建与配置项目](PROJECTS_ZH.md) · [文档目录](README.md)

`Vyx.toml` 描述包、源文件、构建目标、依赖和原生链接配置。本页是常用字段索引；
从新建项目到编译运行的步骤集中在[项目指南](PROJECTS_ZH.md)。完整字段、配置
合并与平台覆盖规则见[英文参考](PACKAGE_MANIFEST.md)。

## 目标选择

`--target app` 选择清单中的目标并构建其依赖闭包；仅构建时省略它会构建清单中的目标集合。
`--triplet` 指定目标平台，不是清单目标名。默认编译宿主平台。

`vyxc --src=project <目录>` 读取该目录的 `Vyx.toml`，使用与 `vyxc build` 相同的
项目构建器。`build` 是当前目录项目的简写；目标、依赖、钩子、DCI 契约、优化和
并行编译参数共用。`--src=file <文件>` 编译单个文件。

`vyxc --run=aot --src=project <目录>` 构建并运行一个可执行目标的依赖闭包。未指定 `--target` 时，
优先选择与包同名的可执行目标，否则选择唯一的可执行目标；有多个候选时报告错误。
库和交叉平台产物不能这样启动。`--` 后的参数只传给程序，不影响构建选项。

`--emit=ir` 将所选目标的 Vyx LLVM 模块合并到 `<输出目录>/<目标>.ll`；
`--emit=obj` 输出一个对象文件，有多个对象时输出 `<目标>.objects` 目录。
依赖照常构建，所选目标不链接为可执行文件。`-o <路径>` 覆盖产物位置；
`-o` 路径相对调用目录，清单路径相对项目目录。多个目标且没有包同名目标时需指定
`--target`。这两种输出不能与 `--run=aot` 或 `--artifact-file` 组合。

## 常用配置

```toml
[package]
name = "app"
version = "0.1.0"
entry = "src/main.vyx"

[build]
output_dir = "target"
cache_dir = ".cache"
threads = 0

[dependencies]
core = { path = "std:core", target = "std_core" }
collections = { path = "std:collections", target = "std_collections" }

[target.app]
type = "executable"
entry = "src/main.vyx"
auto_sources = false
sources = ["src/math.vyx"]
```

这个例子在新项目配置上增加了 `src/math.vyx`；先创建该文件，或移除 `sources` 这一行。

| 配置 | 含义 |
|---|---|
| `package.name`、`version` | 包名与版本 |
| `package.entry` | 默认入口 |
| `build.output_dir` | 产物目录；目标上的同名字段可覆盖 |
| `build.cache_dir` | 构建缓存目录 |
| `build.threads` | 默认并发任务数；0 使用硬件并发度，命令行 `-j` 可覆盖 |
| `dependencies` | 包内目标共享的依赖 |
| `target.<name>` | 一个命名构建目标 |
| `type` | `executable`、`static`、`shared` 或 `source` |
| `entry`、`sources` | 目标入口与其他源文件 |
| `auto_sources = false` | 显式管理源文件；新增文件后需加入配置 |

`source` 目标提供源码而不生成归档，使泛型函数体等内容可在消费目标中实例化。
编译后的用户泛型模块也可通过生成的 `.vyi` 版本化模板制品供消费方实例化，
并保留私有定义依赖；普通已编译定义仍需链接生产端原生库。完整验证见
[泛型接口回归门](../probes/gates/generic_interfaces/README.md)。
没有显式目标表时，构建器使用包名和默认入口生成可执行目标。

## Effect 属性 schema

```toml
[effect]
attr_files = ["attrs/common.attr", "../shared/layout.attr"]
auto_discover = true
```

`attr_files` 相对于当前清单解析；`auto_discover` 默认为 `true`，自动加载包根目录
的 `.attr`，设为 `false` 后只使用显式列表。子目录文件需列入配置。
一个文件可包含多个 `attribute` 块，`extends` 在选中文件之间复用参数并支持前向引用。
参数使用 `i32`、`u64`、`f32`、`bool`、`string` 等 Vyx 标量类型名；`.attr` 不接受 `int`。
格式见 [Effect 自定义属性](MOSP_EFFECT_ZH.md#142-在-attr-中定义-schema)。

选中文件的路径和内容均参与 object/interface 缓存身份，包括包目录之外的显式文件。
`VYX_EFFECT_SCHEMAS_IN` 保留为规范 schema 数据表的兼容输入；与 `.attr` 重复的
schema 身份会报错。

## 目标依赖与链接

依赖另一个本地包中的库目标：

```toml
[target.app.dependencies]
support = { path = "../support", target = "support" }
```

`../support/Vyx.toml` 必须存在并定义 `support` 目标。
同一清单中的依赖可以写作 `support = { target = "support" }`。
Vyx 目标通过依赖表加入构建及链接，原生系统库或预编译库使用链接表：

```toml
[target.app.link]
libs = ["native_support"]
lib_paths = ["native/lib"]
```

链接表不会生成 `native_support`，必须事先提供可链接的库。
`libs_windows`、`libs_linux` 等平台字段与基础列表累加；
更具体的平台项不替换基础项。依赖缺失与环会在编译前诊断。

## 本地包注册表

`vyxc publish`、`install`、`search`、`lock` 使用本地 `file://` 注册表，
由 `VYX_REGISTRY` 指定，默认 `./.vyx-registry`。
安装的包写入 `.cache/registry/<name>/<version>/`，安装过程写入 `Vyx.lock`。
依赖可使用 `version = "1.2.3"` 或 `path = "registry:name@1.2.3"`。
当前没有远程 HTTP 注册表。

## DCI 配置

目标的 `dci_file` 或 `dci_files` 指定二进制契约；`dci_stub_backend`
选择调用桥接后端。它们与源码中的 `@[dci_import]` 共同提供契约输入。

当前 C++ / Rust 开放泛型项目使用 `dci_stub_backend = "external"`，
通过 `stub_backend.py --provider` 指定生产端源文件。函数请求在构建期间物化；
泛型记录实例的布局必须在 Vyx 代码生成前进入契约。
完整配置见[开放泛型项目清单](../tests/projects/dci_opengeneric/Vyx.toml)，
生成与验证命令见 [DCI SDK 指南](../tools/dci/README.zh-CN.md)。

## 调试与交叉编译

```sh
vyxc build --target app -g -O0
vyxc build --target app --triplet aarch64-linux-android23
```

调试配置见[编辑器与调试](TOOLING_ZH.md)。
交叉编译需要目标平台的工具链、运行时及依赖，具体要求见[测试与构建指南](TESTING_GUIDE_ZH.md)。

## 准备已声明的 C++ DCI 导出

构建目标设置 `dci_imports = ["name"]` 后，编译器在收集源码前准备导入。
`[dci.import.name]` 提供 `module`、`headers`，必须明确声明
`export_types` / `export_functions` 或 `export_all = true`，以及可选的 `profile`
（`cpp` 或 `qt`）、`project_roots`、`triplet`、`boundary`、`std`、`toolchain`、
`contract`、`definitions`。
原生 `cxx`、`cxxflags`、`include_paths` 使用目标与 build 默认配置。

编译器在收集源码前调用 SDK DCI 工具。`contract` 默认输出到项目的
`contracts/name.dcib`，放在缓存之外。可选的 `definitions` 指向已维护的 Vyx
文件，准备过程不覆写它；省略时才添加 `.cache/dci/name/import.vyx`。
两种模式都会添加准备好的原生 `producer.cpp`。已有离线契约经校验后直接使用，
不重新提取 AST 或覆写。删除缓存后普通构建会从该契约重建中间产物。契约缺失，
或 `source.preparation_inputs` 记录的生产端输入变化时才测量；无效或不兼容的
输入契约会报错。`cpp-import --force` 显式要求重新生成；未携带准备元信息的
离线契约由提供者管理版本。独立准备与可编辑的 Converter 定义仍可选用。
应用调用不会决定导出范围；所选类的全部可表达成员保持可用。
`ownership_headers` 显式选用用户编写的库事实声明，未知所有权拒绝适配。
`qt_connections` 列出精确的可选信号连接操作，不会仅因出现信号或 inline 就增加导出。
见 [DCI 工具说明](../tools/dci/README.zh-CN.md#可选的声明范围构建准备) 和
[Qt 工程](../probes/gates/dci-qt-counter/README.md)。
