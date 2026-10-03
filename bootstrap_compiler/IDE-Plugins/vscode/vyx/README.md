# Vyx VS Code Extension

Language support for the Vyx SDK toolchain (`vyxc`, `vyxc-lsp`, `vyxc-dap`).

## Features

- **LSP** via `vyxc-lsp` (stdio): diagnostics with `Exxxx` codes, completion (including `use std.` package registry, prefix match, differential on `.`), hover, go to definition, find references, rename, document symbols, signature help, semantic tokens, folding, document formatting, code actions (quick fixes: similar-name, `use` insert, missing `;`, clone-before-move).
- **DAP** via `vyxc-dap`: launch compiled programs, breakpoints, stack, variables, stepping. `Vyx: Debug Current File` compiles with `-g` then starts the adapter.
- **Formatting**: LSP indent formatter (4 spaces). Bound as the default formatter for `[vyx]` and Format Document / Format on Save.
- **Problems**: `$vyx` matcher for `file:line:col: error: E2000: …`.
- **Tasks**: build project, compile current file, compile with debug info, run current file.
- **Snippets**: `main`, `fn`, `class`, `trait`, `impl`, `match`, `use`, Option/Result.

Toolchain resolution, in order: setting → extension sibling/`bin`/`toolchain` → workspace `bootstrap_compiler/out` (and parents) → `VYX_*` env → PATH.

`LLVM_ROOT` is taken from `vyx.llvmRoot`, the process environment, or a workspace `clang/` directory, and is prepended onto PATH when spawning compiler/LSP/DAP so `vyx_compiler_backend` can load.

## Settings

| Setting | Meaning |
| --- | --- |
| `vyx.compilerPath` | `vyxc` / `vyxc.exe` |
| `vyx.lspPath` | `vyxc-lsp` |
| `vyx.dapPath` | `vyxc-dap` |
| `vyx.llvmRoot` | LLVM SDK root (`bin/clang`) |
| `vyx.trace.server` | LSP trace: off / messages / verbose |

## Commands

- `Vyx: Run Current File` → `vyxc --src=file <file> --run=aot`
- `Vyx: Build Project` → `vyxc build`
- `Vyx: Debug Current File` → compile `-g --emit=exe` then DAP
- `Vyx: Format File` → `editor.action.formatDocument` (LSP)
- `Vyx: Restart Language Server`

## Build this extension

```text
cd bootstrap_compiler/IDE-Plugins/vscode/vyx
npm install
npm run compile
```

Press F5 from this folder to launch an Extension Development Host.

## Source of truth

- Language server: `bootstrap_compiler/src/core/lsp_main.vyx`
- Debug adapter: `bootstrap_compiler/src/core/dap_main.vyx`
- Compiler CLI: `bootstrap_compiler/src/core/main.vyx`

Host-era `src/LSP` is archived and is not used.
