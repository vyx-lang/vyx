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

在编辑器中打开包含 `Vyx.toml` 的项目目录，以便语言服务获取项目上下文。
插件无法自动发现 SDK 时，手动配置工具路径。

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
Linux 使用内置的 ptrace/DWARF 后端；其他宿主需要显式配置 LLDB：
把 `VYX_LLDB` 设为 LLDB 可执行文件路径，或把 `LLVM_ROOT` 设为对应 LLVM 安装目录。
也可以在启动配置中设置 `debuggerPath`，例如 `"C:/LLVM/bin/lldb.exe"`。
VS Code 的 `vyx.llvmRoot` 设置可提供 LLVM 根目录。

当前适配器声明不支持条件断点、函数断点和变量赋值。调试时使用普通行断点；
需要改变源码中的值时，修改后重新构建。

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

在 **Settings → Plugins → Install Plugin from Disk** 中安装
`build/distributions` 下生成的 ZIP。随后进入
**Settings → Languages & Frameworks → Vyx**，配置 SDK 中的
`vyxc`、`vyxc-lsp`、`vyxc-dap`。
插件提供可执行程序、静态库和动态库项目创建、语言服务、Vyx 运行配置和 DAP 调试。
调试程序与 LLDB 的要求同上。

## 从仓库构建语言服务器与调试适配器

安装可用的 Vyx SDK 和编译器构建依赖后执行：

```sh
cd bootstrap_compiler
vyxc build --target vyxc-lsp
vyxc build --target vyxc-dap
```

构建与运行依赖见[编译器说明](../bootstrap_compiler/README.md)。当前实现位于
[lsp_main.vyx](../bootstrap_compiler/src/core/lsp_main.vyx) 和
[dap_main.vyx](../bootstrap_compiler/src/core/dap_main.vyx)。
