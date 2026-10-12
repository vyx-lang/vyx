# String identity and layout

```powershell
python probes/gates/string-identity/run.py --compiler bootstrap_compiler/out/vyxc.exe
```

Use the SDK compiler and matching backend/runtime built from the current tree.
The native AOT fixtures run at O0 and O2 and compare stdout bytes, including NUL.
They cover standard String SSO and heap storage, qualified types, aliases, and
printing without consuming a named owner. A differently named class explicitly
declares the string language item and its complete seven-field ABI.

Negative fixtures must report a source location and produce no executable.
A class named String does not receive the standard string ABI from its name or
field names. An explicit language item must also have the correct rawptr/i64
field types and the complete ordered SSO layout.

Generated fixtures, logs and compiler/artifact hashes are saved in `.cache/`.
