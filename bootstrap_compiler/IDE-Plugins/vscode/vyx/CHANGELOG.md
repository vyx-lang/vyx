# Change Log

## 0.2.1

- `use std.` / `import` completion lists registered packages with prefix match.
- Typing another character or `.` re-queries (`isIncomplete`) instead of freezing the first list.

## 0.2.0

- Wire `vyxc-lsp` / `vyxc-dap` from the self-host SDK, not host-era `vyxc --lsp`.
- Format Document / format on save through LSP (indent formatter).
- Code actions: similar-name, insert `use`, missing `;`, clone before move.
- Find references, rename, folding ranges.
- Debug current file compiles with `-g` then launches DAP.
- Resolve compiler/LSP/DAP from workspace `bootstrap_compiler/out` and `LLVM_ROOT`.
- Problem matcher keeps `Exxxx` codes. Status bar shows LSP health.

## 0.1.0

- Initial syntax highlighting, snippets, and LSP client shell.
