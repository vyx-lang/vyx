# Vyx for IntelliJ IDEA and CLion

Vyx support backed by `vyxc-lsp` and `vyxc-dap`: completion, navigation,
diagnostics, rename, formatting, project creation, running, and debugging.

[Installation and debugging](../../../../docs/TOOLING.md) ·
[中文安装与调试指南](../../../../docs/TOOLING_ZH.md)

## Build and install

Requirements: JDK 25 and a local IDE based on platform build 262 (2026.2)
or later with the native DAP module. Build against the IDE you intend to use.

From this directory on Windows:

```powershell
.\gradlew.bat buildPlugin "-PvyxIdePath=C:/Tools/CLion"
```

On Linux:

```sh
./gradlew buildPlugin -PvyxIdePath=/path/to/compatible/ide
```

Install the ZIP from `build/distributions` with **Settings → Plugins →
Install Plugin from Disk**. For plugin development, replace `buildPlugin` with
`runIde` to launch the development sandbox.

For a self-contained tool bundle, pass `-PvyxSdkPath=/path/to/packaged/sdk` to
`buildPlugin`. It preserves the SDK's compiler, LSP, DAP, static runtime,
standard packages, debugger assets and LLVM driver layout. Without this option,
the plugin bundles checkout tools and standard packages; configure the compiler
path to a complete SDK for compilation outside the source checkout.

## Configure Vyx

Open **Settings → Languages & Frameworks → Vyx** and configure the compiler,
language server, and debug adapter paths from the same Vyx SDK:

- `vyxc`: project builds and program compilation.
- `vyxc-lsp`: language service.
- `vyxc-dap`: native debugging.

Use the New Project wizard for a Vyx executable, static library, or shared library.
For existing projects, open the directory containing `Vyx.toml`.
Choose **Build & Run Project** to invoke `vyxc --run=aot --src=project <dir> --target <name>`.
The working directory selects the manifest; Arguments are passed to the program
after `--`, with quoted words and empty arguments preserved. `(auto)` follows
the compiler's executable target selection.

Debug builds use `-g -O0 --artifact-file <temporary-file>` and launch the exact
executable path reported by the compiler, including custom output directories.

Debugging uses CodeLLDB and its matching LLDB from the Vyx SDK on Windows and Linux.
Override it with `VYX_DAP_ADAPTER`. See the linked guide for capabilities
and current limits.
