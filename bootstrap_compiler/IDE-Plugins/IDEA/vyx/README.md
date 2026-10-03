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

## Configure Vyx

Open **Settings → Languages & Frameworks → Vyx** and configure the compiler,
language server, and debug adapter paths from the same Vyx SDK:

- `vyxc`: project builds and program compilation.
- `vyxc-lsp`: language service.
- `vyxc-dap`: native debugging.

Use the New Project wizard for a Vyx executable, static library, or shared library.
For existing projects, open the directory containing `Vyx.toml`.
Build a debug executable with `vyxc build --target <name> -g -O0`, then point
the Vyx run/debug configuration at that executable.

Linux debugging has a native ptrace/DWARF backend. On other hosts, configure
LLDB through `VYX_LLDB` or `LLVM_ROOT`. See the linked guide for capabilities
and current limits.
