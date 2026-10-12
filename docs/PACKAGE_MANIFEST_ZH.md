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
| `type` | `executable`、`static`、`shared`、`source` 或 `prebuilt` |
| `entry`、`sources` | 目标入口与其他源文件 |
| `auto_sources = false` | 显式管理源文件；新增文件后需加入配置 |

`source` 目标提供源码而不生成归档，使泛型函数体等内容可在消费目标中实例化。
库和源码目标可以省略 `entry`，由 `sources` 提供解析起点。默认的统一 crate 库构建
将源码依赖的实现编入消费方归档，VYI 与 DCIB 的公开选择仍属于目标自己的源码。
分模块或根分区库构建尚无源码包普通函数的唯一对象归属规划；
[原生导出门](../probes/gates/dci-native-export/README.md) 验收默认统一 crate 路径。
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

## 包源与锁文件

第三方依赖使用命名的 JSON 清单源。清单地址可以是 HTTP(S) URL、`file://` URL
或本地路径，清单与包归档可以分别存放在 GitHub、私有服务器或共享文件系统。
包管理引擎随 SDK 放在 `tools/packages/`，使用 Python 3.11 及以上版本的标准库。
只依赖本地 `path` 和 `std:` 包的项目，不需要为依赖解析安装 Python。

为当前用户配置源：

```sh
vyxc source add community https://packages.example.org/index.json
vyxc source add company https://packages.example.org/private/index.json --token-env VYX_PACKAGE_TOKEN
vyxc source list
vyxc source remove community
```

示例域名需替换成实际清单地址。用户配置保存在 `~/.vyx/sources.json`；
`VYX_PACKAGE_SOURCES` 可指定其他配置文件。格式如下：

```json
{
  "schema": 1,
  "sources": [
    {"name": "company", "url": "https://packages.example.org/private/index.json", "token_env": "VYX_PACKAGE_TOKEN"},
    {"name": "community", "url": "https://packages.example.org/index.json"}
  ]
}
```

项目自己的源写在 `Vyx.toml` 中：

```toml
[sources.company]
url = "https://packages.example.org/private/index.json"
token_env = "VYX_PACKAGE_TOKEN"

[dependencies]
codec = { version = "^1.2.0", source = "company", target = "codec" }
```

项目源按声明顺序排在前面，随后使用未被项目同名配置覆盖的用户源。
依赖指定 `source` 时只从该源解析；省略时，第一个包含该包的源负责解析，
即使其中没有满足版本要求的版本，也不会转向后面的源。源无法访问、JSON 无效
或包元数据无效都会报错。私有包建议显式指定来源。

清单格式如下：

```json
{
  "schema": 1,
  "packages": {
    "codec": {
      "description": "Binary encoding library",
      "versions": {
        "1.2.3": {
          "url": "archives/codec-1.2.3.tar.gz",
          "sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
          "strip_prefix": "codec-1.2.3"
        }
      }
    }
  }
}
```

示例 SHA256 必须替换为实际归档的哈希。归档相对 URL 按清单位置解析；
移除可选的 `strip_prefix` 后，包中必须有 `Vyx.toml` 及其声明的输入。
解包前验证 SHA256，并拒绝不安全的归档路径。源身份由配置的源名与规范化后的
清单 URL 共同确定，不包含 token 值。即使 URL 相同，不同命名源也保持独立，
避免不同私有认证视图共用依赖身份。缓存目录为
`.cache/packages/<source-identity-hash>/<name>/<version>/<archive-hash>/`。

版本要求支持精确 SemVer（`1.2.3`）、caret（`^1.2.0`）、tilde（`~1.2.0`）、
比较条件（`>=1.2.0 <2.0.0`）、通配符（`1.2.*` 或 `*`），以及用 `||` 连接的候选范围。
解析器在负责该包的源中选择兼容版本；同一命名源的同一个包在依赖图中统一为一个版本，
无法同时满足的要求会报告版本冲突。预发布版本需在要求中显式指定预发布版本；
`*` 不会自动选用预发布版。

```sh
vyxc search codec
vyxc install 'codec@^1.2.0' --source company
vyxc lock
vyxc build --locked
vyxc build --offline
vyxc update
```

带包名的 `install` 将版本依赖写入 `Vyx.toml`；不带包名时物化项目声明的依赖图。
`lock` 解析依赖图并写出 `Vyx.packages.lock`，`update` 显式重新选择版本。
解析覆盖项目所有目标的有效依赖，按全局、继承模板、目标的顺序应用覆写和移除，
再处理本地包与下载包的传递依赖；因此可能下载本次未选目标所需的包。
未使用的模板和已被覆写的默认项不额外施加版本约束。

`Vyx.packages.lock` 是 JSON 文件，记录精确版本、源身份、归档哈希、已验证的 payload 摘要与依赖边，
应随 `Vyx.toml` 一起提交。它与选择 MOSP 模块版本和变体的 `Vyx.lock` 分开。
`--locked` 拒绝缺少锁文件或依赖图发生变化的构建。`--offline` 要求已有匹配的锁文件
和通过校验的包缓存，不会在网络失败后静默使用其他缓存包。普通构建保留兼容的
锁定版本，需要更新版本时运行 `update`。

私有源的 bearer token 从配置指定的环境变量读取，不写入项目文件或锁文件；
携带认证信息的跨域重定向会被拒绝。源的 token 只发送给同源归档 URL，不发送给
独立托管的跨域归档。归档哈希用于验证下载内容，正确的包元数据仍由配置的源负责发布。

`vyxc publish --source NAME --dry-run` 列出实际发布文件；
`vyxc publish --source NAME` 生成归档并更新本地 JSON 清单。
用 `[package]` 的 `include`、`exclude` 数组控制内容。默认排除隐藏文件、生成物、
日志与本地工具状态。发布用原生库应放在 `native/` 或 `lib/`，避免放进生成目录
`out/`、`target/`。模块版本选择文件 `Vyx.lock` 保留在包中，包管理的
`Vyx.packages.lock` 不作为依赖包的发布内容。
远程服务器或 GitHub 的上传使用对应平台自己的发布流程，不由 `publish` 自动完成。
完整版本约束、归档格式和命令选项见[包管理引擎说明](../tools/packages/README.md)。

### 源码、二进制与 DCI 包

版本条目的 `kind` 默认为 `source`，直接使用 `url` / `sha256`，包中的目标照常构建。
`binary` 和 `dci` 条目改用 `artifacts` 数组，明确列出目标 triplet 和 ABI：

```json
{
  "kind": "dci",
  "artifacts": [
    {
      "triplet": "x86_64-pc-windows-msvc",
      "abi": "msvc",
      "url": "archives/codec-1.2.3-windows-msvc.tar.gz",
      "sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
      "strip_prefix": "codec-1.2.3"
    }
  ]
}
```

这个对象放在上述清单的版本条目位置，哈希替换为实际归档哈希。
必须恰好有一个产物匹配构建的 triplet 和 ABI；缺少匹配或重复匹配都会报错。
二进制包的锁定结果包含目标身份，不会静默使用另一平台的归档。

归档内仍以 `Vyx.toml` 描述原生库及其公开输入。例如一个 Windows DCI 动态库包：

```toml
[package]
name = "codec"
version = "1.2.3"
kind = "dci"

[target.codec]
type = "prebuilt"
artifact = "native/codec.lib"
linkage = "shared"
triplet = "x86_64-pc-windows-msvc"
abi = "msvc"
sources = ["bindings/codec.vyx"]
dci_files = ["contracts/codec.dcib"]
runtime_files = ["native/codec.dll"]
```

`artifact` 指向现成的链接输入；Windows 动态库在这里填写导入库，DLL 放入
`runtime_files`。`linkage` 为 `static` 或 `shared`，所有文件路径都相对包根目录。
`sources` 提供 Vyx 定义，可选的 `interface_sources` 提供额外的 Vyx 或 `.vyi` 接口输入。
DCI 包必须同时在 `dci_files` 中提供契约，在 `sources` 中提供 Vyx 声明。
消费方通过依赖的 `target` 字段选择包内目标。

构建直接使用包内原生库、定义与契约，不重新运行生产端 Adapter。
发布前可以用 DCI Converter 生成定义，产物仍是开发者可检查的 Vyx 文件。
`.vyi` 必须通过消费编译器的格式与兼容性检查；triplet 和 ABI 相同，不代表任意
编译器版本或库版本都能兼容。

下载、校验、解包与解析依赖不会执行包清单的构建钩子。源码包中需要单独构建的目标
使用普通项目构建器，以当前用户权限执行声明的钩子；包完整性校验不提供构建沙箱。
预编译依赖直接消费已有产物，不运行生产端构建。

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

如果 `vyxc` 运行在 Android/Termux，直接使用 Termux 自带的 LLVM 工具链和
sysroot，不需要 NDK，也不会查找 Android SDK 的 NDK。SDK 安装脚本会提供
`LLVM_ROOT` / `LLVM_BIN`；手动安装时可设置这两个变量，或把 Termux 的 LLVM
目录加入 `PATH`。Windows/Linux 宿主把 Android 作为交叉目标时，仍需要 NDK
sysroot，并使用下面的 staged runtime 目录。

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
