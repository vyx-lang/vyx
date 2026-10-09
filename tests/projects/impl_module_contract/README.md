# Multi-file inherent implementation contract

```powershell
vyxc --src=project . --run=aot -j1
```

Expected output: `42`, exit code `0`.

The application and counter are compiled into separate objects. The counter
module consists of three source files: its type/constructor, arithmetic and
inspection methods. Both `Counter` implementation blocks use public methods
without an explicit visibility attribute on the block. `read` also calls a
private helper implemented in the other file.

The private helper uses a private `PrivateCounter` class with public inherent
methods. Its ordinary implementation and a second `@[vis(world)]` implementation
must both stay out of the public interface because their owner is private.

This exercises shallow interface publication, owner identity and export-root
collection for every source file in a logical module. Cold, warm, secondary-file
incremental builds and negative visibility checks are in the
[compiler module gate](../../../probes/gates/compiler-modules/README.md).
