# Shared global storage across module interfaces

```powershell
vyxc --src=project . --run=aot -j1
```

Expected output: `global module contract OK`, exit code `0`.

The storage module has two source files and exports an initialized integer and
nonempty string. No producer function references either global. Two separately
compiled consumers read their initial values and update the same integer in
both directions. A second producer exports globals with the same source names;
its values and updates must remain independent.

The [cross-module storage gate](../../../probes/gates/cross-module/README.md)
also compiles these modules in isolated directories using only generated `.vyi`
interfaces and explicit object linking. It removes producer sources from the
import path before consumer compilation and checks cold, warm and incremental
manifest builds. The incremental variant changes the second producer source's
string initializer while preserving the interface surface.
