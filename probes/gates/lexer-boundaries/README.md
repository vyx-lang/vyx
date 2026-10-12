# Lexer boundaries

Run with the SDK compiler and runtime built from the current source tree:

```powershell
python probes/gates/lexer-boundaries/run.py --compiler bootstrap_compiler/out/vyxc.exe
```

The positive fixture runs through native AOT at `O0` and `O2`. It checks
embedded NUL lengths and bytes (including actual stdout bytes), inline and
heap-backed `String` output without consuming their storage, numeric/string
output order, interpolation literals, nested comments,
triple-quoted expressions with newlines and braces, and legacy escaped quotes.
Generated 1,048,576-byte and 1,048,577-byte literals assert their full lengths,
an embedded NUL and the final byte after native compilation.

Malformed fixtures must exit `1`, identify the original source location, and
produce no executable. They cover unfinished nested comments, physical LF/CR
inside a character, CR inside a single-line string, an unfinished triple-quoted
interpolation expression, and invalid ordinary string escapes.

Generated sources, logs and compiler identity are kept in `.cache/`. The
runner writes the actual outcomes to `.cache/verification.json`.
