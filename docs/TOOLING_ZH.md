# 编辑器、语言服务与调试

[English](TOOLING.md) · [创建项目](PROJECTS_ZH.md) · [文档目录](README.md)

Vyx 提供编译器与项目构建工具 `vyxc`、语言服务器 `vyxc-lsp` 和调试适配器
`vyxc-dap`。IDE 插件通过这些程序提供语言与调试功能。三个工具应来自同一套 SDK 或编译器构建。

插件源码位于 `bootstrap_compiler/IDE-Plugins/`，随仓库一起维护。
[忽略规则](../.gitignore)仅排除依赖、IDE 缓存、构建输出和插件安装包；
Gradle wrapper 与扩展打包配置保留在源码中。

## 语言服务器

`vyxc-lsp --stdio` 为 LSP 客户端提供诊断、补全、悬浮提示、定义跳转、引用查找、
重命名、文档与工作区符号、签名提示、语义高亮、格式化、代码操作和折叠范围。
模块补全包含 `std.collections` 等已注册的包。

引用查找、文档高亮和重命名使用编译器解析出的声明与绑定。跨文件、跨模块查询
区分同名局部变量、参数和不同类型的成员，包含导入别名、泛型类型参数、枚举、
全局变量、闭包捕获与字符串插值。重载函数按同一模块和所属类型的声明族重命名。
重命名导出的声明会更新导入目标并保留别名；重命名别名只更新别名及其使用位置。

重命名先在临时源码覆盖层中重新分析，确认已有绑定没有被捕获、冲突或丢失后
才返回编辑。未保存缓冲区参与分析；支持 `documentChanges` 的客户端收到文档版本。
无法解析、存在语义错误、涉及 SDK 只读声明或外部 ABI 名称时拒绝重命名。
项目夹具见[跨模块编辑器项目](../tests/projects/editor_semantic_rename/README.md)。

在编辑器中打开包含 `Vyx.toml` 的项目目录，以便语言服务获取项目上下文。
插件无法自动发现 SDK 时，手动配置工具路径。

关闭的源码文件先建立声明索引；引用与重命名按候选文件逐个进行语义分析，
只缓存源码位置和绑定事实。依赖提供声明签名，需要推导返回类型时保留函数体。
标准库按显式 `VYX_STD_PACKAGES`、工作区规范包、SDK 的顺序选择一个完整来源；
每个来源都需要 `std_packages/registry`，不再回退到旧 `std/`。其他规范包来源保留导航索引，不合并进语义分析，
SDK 源码保持只读。相对路径与绝对路径共享同一文件身份。
打开的缓冲区另外提供完整诊断。每次请求和单个文件
索引完成后释放临时分析存储。替换、关闭文档时回收旧文本、符号和诊断；拉取诊断
复用当前结果。发现源码时跳过输出、缓存、依赖、IDE 产物和本地 seed 工程。
大型符号和语义高亮响应使用可增长缓冲区构造，避免反复复制整份源码与 JSON 前缀。
请求存储还覆盖编译器生成的子串、隐式复制与检查指针元数据。编辑器的 UTF-16
位置在协议边界转换为源码字节位置。
排队的修改在输入暂停后合并分析，请求处理前同步此前的修改，诊断携带文档版本。
压力验证入口见[编辑器服务压力门](../probes/gates/editor-industrial/README.md)。

## 格式化

`vyxc fmt` 原地格式化 `.vyx` 文件。未指定路径时，扫描当前工作目录下存在的
`src/`、`tests/` 和 `benches/`。显式路径可以是文件或目录，重复和重叠的输入会去重。
递归扫描跳过 `.git`、`.cache`、`target`、`out`、`build` 和 `snapshots` 子目录。
该命令不按清单 target 筛选源码。

```sh
vyxc fmt
vyxc fmt src tests
vyxc fmt --check src
vyxc fmt --stdout src/main.vyx
vyxc fmt --indent-width 2 src
vyxc fmt --tabs src
vyxc fmt -- -example.vyx
```

默认使用四个空格缩进。`--indent-width` 接受 1–16 的整数；`--tabs` 每层使用一个
制表符。`--check` 只报告需要修改的文件。`--stdout` 要求最终只选中一个文件，
向标准输出写入格式化后的原始字节，不附加状态文本，且不能与 `--check` 同用。
`--` 结束选项解析，可用于传入以连字符开头的路径。

格式化器先解析输入，再核对输出与输入的 token 序列。注释、字面量内容及其写法
保持不变，包括多行字符串和插值；已有 UTF-8 BOM 也会保留。新增空白沿用源码的
换行符风格，字符串和注释内部的空白不变。词法、语法或 token 校验失败时，
报告错误并保留该文件原文。

目前调整缩进、语句与代码块换行，以及 token 间距；不重排导入，也不按最大行宽
折行。泛型尖括号和管道符等有多种语法含义的 token 保留已有相邻关系。
混合 LF/CRLF 文件保留每条行注释的终止符，以免改变文档注释数据。

开始格式化前会验证全部路径参数。需要修改的文件先写入同目录中的唯一临时文件，
再次核对原文件内容与身份后原子替换。写回拒绝符号链接、硬链接和只读文件；
需要查看这些文件的格式化结果时可使用 `--stdout`。已有备份文件不受影响。
原子替换以单个文件为单位：后续文件出错，不会撤销此前已完成的文件修改。

| 退出码 | 含义 |
|---|---|
| `0` | 格式化或标准输出成功；`--check` 下表示无需修改。 |
| `1` | `--check` 发现需要格式化的文件。 |
| `2` | 参数错误、输入缺失、语法/token 校验失败或 IO 错误。 |

LSP 的整篇文档格式化使用同一实现。客户端的 `tabSize` 指定缩进宽度（1–16），
`insertSpaces: false` 选择制表符。源码无效时返回请求错误，不返回编辑。
当前不支持范围格式化：`documentRangeFormattingProvider` 为 false，收到范围
格式化请求时明确拒绝，不会借此替换整篇文档。

## VS Code 与 Cursor

`bootstrap_compiler/IDE-Plugins/vscode/vyx/` 中的插件声明兼容 VS Code `^1.110.0`；
Cursor 也需要支持对应版本的扩展 API。如果已有 VSIX，通过
**Extensions: Install from VSIX** 安装。

从源码运行插件，需要先安装 Node.js 和 npm：

```sh
cd bootstrap_compiler/IDE-Plugins/vscode/vyx
npm ci
npm run compile
```

用 VS Code 打开这个插件目录，按 F5 启动扩展开发宿主，再在新窗口中打开 Vyx 项目。

在用户设置或 `.vscode/settings.json` 中配置以下路径，将示例路径换成实际 SDK 安装位置：

```json
{
  "vyx.compilerPath": "C:/Tools/vyx/bin/vyxc.exe",
  "vyx.lspPath": "C:/Tools/vyx/bin/vyxc-lsp.exe",
  "vyx.dapPath": "C:/Tools/vyx/bin/vyxc-dap.exe"
}
```

Linux 使用不带 `.exe` 的可执行文件路径。插件还会从随附工具目录、工作区编译器产物、
环境变量和 `PATH` 发现工具。

命令面板提供 **Vyx: Run Current File**、**Vyx: Build Project**、
**Vyx: Debug Current File**、**Vyx: Format File** 和 **Vyx: Restart Language Server**。
插件为 `.vyx`、`.vyi` 提供语法高亮和代码片段；构建任务可使用 `$vyx` 问题匹配器。
工具启动失败时查看 **Vyx** 输出频道；排查 LSP 通信时可把 `vyx.trace.server` 设为 `verbose`。

## 调试项目

生成带调试信息且关闭优化的程序：

```sh
vyxc build --target hello_app -g -O0
```

使用项目默认输出目录时，创建 `.vscode/launch.json`：

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "type": "vyx",
      "request": "launch",
      "name": "Debug hello_app",
      "program": "${workspaceFolder}/target/hello_app.exe",
      "cwd": "${workspaceFolder}",
      "args": []
    }
  ]
}
```

Linux 去掉 `.exe`；如果已将 `build.output_dir` 改成 `out`，也要修改 `program`。
源码修改后先重新构建，这份启动配置本身不会执行构建。
**Debug Current File** 是另一个入口，会先为当前文件生成调试产物，再启动调试。

`vyxc-dap --stdio` 提供程序启动、行断点、单步执行、调用栈、作用域、变量和表达式求值。
Windows 和 Linux 均将协议直接交给 CodeLLDB。调试器负责异步执行、
调用栈分页、变量按需展开、输出流和会话清理；Vyx 入口不保存重复的作用域、变量或
程序输出。具体能力以实际调试器 `initialize` 返回的 capabilities 为准。

完整 SDK 提供 `bin/debugger/adapter/codelldb` 及其配套 LLDB 环境。从源码构建时，
在 `bootstrap_compiler/` 执行 `python scripts/prepare_debug_adapter.py` 准备固定版本、
SHA-256 校验的调试器源码与配套运行时。源码构建需要 Cargo/Rust、Git 与 clang++；
SDK 使用者不需要这些构建工具。Vyx 补丁补充变量分页，并分离 Windows 内部控制台
输出与 DAP 协议。打包检查补丁和可执行文件哈希，保留组件许可；详见
[调试器构建说明](../tools/debugger/README.md)。需要指定其他 CodeLLDB 或 LLDB-DAP
安装时，把 `VYX_DAP_ADAPTER` 设为适配器路径；旧的 `VYX_LLDB_DAP` 仍然接受。
VS Code/Cursor 的启动配置可用 `debuggerPath` 指定同样的覆盖路径。
旧配置中的 `lldb` CLI 路径会选择同目录的 `lldb-dap`。找不到适配器时明确报错。
程序应使用 `-g -O0` 构建，以保留源码和局部变量调试信息。
参见 [CodeLLDB](https://github.com/vadimcn/codelldb/blob/v1.12.3/MANUAL.md)。

## IntelliJ IDEA 与 CLion

`bootstrap_compiler/IDE-Plugins/IDEA/vyx/` 中的插件面向
平台构建号 262（2026.2）及以上、包含原生 DAP 模块的 IDE。
源码构建需要 JDK 25 和兼容的本地 IDE 安装。

Windows 下执行：

```powershell
cd bootstrap_compiler/IDE-Plugins/IDEA/vyx
.\gradlew.bat buildPlugin "-PvyxIdePath=C:/Tools/CLion"
```

Linux 下执行：

```sh
cd bootstrap_compiler/IDE-Plugins/IDEA/vyx
./gradlew buildPlugin -PvyxIdePath=/path/to/compatible/ide
```

给 `buildPlugin` 传入 `-PvyxSdkPath=/path/to/packaged/sdk`，可将完整 SDK 随插件
打包，包括编译器、语言服务、调试适配器、运行时、标准包和 LLVM 驱动。
未使用此选项时，在源码仓库外构建项目需要配置完整 SDK。

在 **Settings → Plugins → Install Plugin from Disk** 中安装
`build/distributions` 下生成的 ZIP。随后进入
**Settings → Languages & Frameworks → Vyx**，配置 SDK 中的
`vyxc`、`vyxc-lsp`、`vyxc-dap`。
插件提供可执行程序、静态库和动态库项目创建、语言服务、Vyx 运行配置和 DAP 调试。
调试程序与 LLDB 的要求同上。

**Build & Run Project** 执行 `vyxc --run=aot --src=project <工作目录>`，
并传入所选清单目标。程序参数放在 `--` 后面。
Debug 使用 `-g -O0 --artifact-file <临时文件>` 构建，再启动编译器返回的
可执行产物路径，支持目标独立设置的输出目录。

## 从仓库构建语言服务器与调试适配器

安装可用的 Vyx SDK 和编译器构建依赖后执行：

```sh
cd bootstrap_compiler
vyxc build --target vyxc-lsp
vyxc build --target vyxc-dap
```

构建与运行依赖见[编译器说明](../bootstrap_compiler/README.md)。当前实现位于
[lsp_main.vyx](../bootstrap_compiler/src/core/tooling/lsp_main.vyx) 和
[dap_main.vyx](../bootstrap_compiler/src/core/tooling/dap_main.vyx)。
