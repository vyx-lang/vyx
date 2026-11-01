# Formatter regression gate

Run against the SDK compiler and language server built from the current tree:

```sh
python probes/gates/formatter/run.py --compiler /path/to/vyxc --lsp /path/to/vyxc-lsp --corpus --stress
```

The gate uses temporary copies. It checks comments and literal bytes, idempotence,
indentation settings, BOM/CRLF handling, invalid input without writes, directory
deduplication, atomic publication, and native AOT output before/after formatting.
The LSP checks use actual framed stdio requests and compare edits with CLI output,
including exact UTF-16 replacement ranges and invalid-buffer diagnostics.
`--corpus` adds real compiler/library files; `--stress` records elapsed times for
1,000 and 4,000 generated functions. These measurements do not claim a universal
performance threshold. Results and compiler hashes are written to `.cache/` or
`--output`; failed inputs are retained there.

Document formatting supports `tabSize` (1–16) and `insertSpaces`. Range formatting
is currently not advertised and returns an explicit unsupported-method error;
selecting a range must never rewrite the rest of the document.
