# IDE plugins

Plugin source is tracked in the main repository. Dependencies, IDE state,
build output, and packaged extensions are ignored. Keep the Gradle wrapper,
package lockfile, and packaging configuration with the source. Repository
development rules are in the [root agent guide](../../AGENTS.md).

Self-host Vyx editor support. Both plugins talk to `vyxc-lsp` and `vyxc-dap` from `bootstrap_compiler/out` (or a bundled toolchain).

| Folder | IDE |
| --- | --- |
| `vscode/vyx` | VS Code / Cursor |
| `IDEA/vyx` | IntelliJ / CLion (2026.2+ native DAP) |

Installation, tool paths, and debugging: [English](../../docs/TOOLING.md) · [简体中文](../../docs/TOOLING_ZH.md).

See each folder’s README for install and settings. Language-server behavior lives in `../src/core/tooling/lsp_main.vyx`; the debug adapter in `../src/core/tooling/dap_main.vyx`.

`use std.` completion lists registered `std.*` (and other) modules with prefix match; further characters and `.` re-query the list.
