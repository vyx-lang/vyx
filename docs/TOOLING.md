# Editors, language server, and debugging

[简体中文](TOOLING_ZH.md) · [Create a project](PROJECTS.md) · [Documentation](README.md)

Vyx supplies a compiler and project builder (`vyxc`), a language server
(`vyxc-lsp`), and a debug adapter (`vyxc-dap`). Editor plugins connect to these
executables. Use tools from the same SDK or compiler build.

Plugin sources live under `bootstrap_compiler/IDE-Plugins/` and are tracked in
this repository. The [ignore rules](../.gitignore) exclude dependencies, IDE
caches, build output, and packaged plugins. The Gradle wrapper and extension
packaging configuration are kept with the source.

## Language server

`vyxc-lsp --stdio` serves LSP clients. It provides diagnostics, completion,
hover, definition and reference lookup, rename, document and workspace symbols,
signature help, semantic tokens, formatting, code actions, and folding ranges.
Module completion includes registered packages such as `std.collections`.

Open the project directory containing `Vyx.toml` so the editor has the project
context. Configure the executable path if the plugin cannot discover your SDK.

## VS Code and Cursor

The extension at `bootstrap_compiler/IDE-Plugins/vscode/vyx/` declares
VS Code `^1.110.0` compatibility. Cursor must support that extension API version.
For a supplied VSIX, use **Extensions: Install from VSIX**.

To run the extension from source, install Node.js and npm, then run:

```sh
cd bootstrap_compiler/IDE-Plugins/vscode/vyx
npm ci
npm run compile
```

Open this extension directory in VS Code and press F5 to start an Extension
Development Host. Open your Vyx project in that window.

Set these paths in user settings or `.vscode/settings.json`, replacing the
example SDK location with your installation:

```json
{
  "vyx.compilerPath": "C:/Tools/vyx/bin/vyxc.exe",
  "vyx.lspPath": "C:/Tools/vyx/bin/vyxc-lsp.exe",
  "vyx.dapPath": "C:/Tools/vyx/bin/vyxc-dap.exe"
}
```

On Linux, use paths to the executables without `.exe`. The extension also
discovers tools from its bundled directories, workspace compiler output,
environment variables, and `PATH`.

The command palette includes **Vyx: Run Current File**, **Vyx: Build Project**,
**Vyx: Debug Current File**, **Vyx: Format File**, and **Vyx: Restart Language
Server**. Syntax highlighting and snippets cover `.vyx` and `.vyi`; build tasks
can use the `$vyx` problem matcher. Check the **Vyx** output channel when tool
startup fails. Set `vyx.trace.server` to `verbose` for LSP troubleshooting.

## Debug a project

Build with debug information and without optimization:

```sh
vyxc build --target hello_app -g -O0
```

For the default project output directory, create `.vscode/launch.json`:

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

On Linux, remove `.exe`. If you changed `build.output_dir` to `out`, update
`program` accordingly. Rebuild after changing source; this launch configuration
does not itself invoke a build. **Debug Current File** is a separate command
that compiles the selected file with debug information before launching it.

`vyxc-dap --stdio` provides launch, line breakpoints, stepping, stack frames,
scopes, variables, and expression evaluation. On Linux it has a native
ptrace/DWARF backend. Other hosts use an explicitly configured LLDB:
set `VYX_LLDB` to the LLDB executable or `LLVM_ROOT` to its LLVM installation.
A launch configuration can also set `debuggerPath`, for example
`"C:/LLVM/bin/lldb.exe"`. VS Code's `vyx.llvmRoot` setting supplies an LLVM root.

The adapter currently declares conditional breakpoints, function breakpoints,
and variable assignment unsupported. Use ordinary line breakpoints and rebuild
the program when changing values in source.

## IntelliJ IDEA and CLion

The plugin at `bootstrap_compiler/IDE-Plugins/IDEA/vyx/`
targets platform build 262 (2026.2) or later with the native DAP module.
Its source build requires JDK 25 and a compatible local IDE installation.

From the plugin directory on Windows:

```powershell
cd bootstrap_compiler/IDE-Plugins/IDEA/vyx
.\gradlew.bat buildPlugin "-PvyxIdePath=C:/Tools/CLion"
```

On Linux:

```sh
cd bootstrap_compiler/IDE-Plugins/IDEA/vyx
./gradlew buildPlugin -PvyxIdePath=/path/to/compatible/ide
```

Install the resulting ZIP under `build/distributions` using **Settings → Plugins
→ Install Plugin from Disk**. In **Settings → Languages & Frameworks → Vyx**,
configure `vyxc`, `vyxc-lsp`, and `vyxc-dap` from your SDK. The plugin provides
project creation for executable, static-library, and shared-library projects,
language-server integration, Vyx run configurations, and DAP debugging.
The executable and LLDB requirements are the same as above.

## Building the servers from the repository

With a working Vyx SDK and the compiler build prerequisites installed:

```sh
cd bootstrap_compiler
vyxc build --target vyxc-lsp
vyxc build --target vyxc-dap
```

Build instructions and runtime requirements are in the
[compiler README](../bootstrap_compiler/README.md).
The active implementations are [lsp_main.vyx](../bootstrap_compiler/src/core/lsp_main.vyx)
and [dap_main.vyx](../bootstrap_compiler/src/core/dap_main.vyx).
