# Borrowing an owning receiver

An owning value-class receiver is bound through a pointer slot. `&self` and
`&mut self` must refer to the object behind that slot. Ordinary inline locals
must continue to refer to their aggregate storage.

`positive.vyx` checks both borrow forms inside a fluent method, chained moves,
and mutation through a borrow of an ordinary local. It checks the Vec contents
and an adjacent field, so reading the receiver slot as object storage fails.
The Windows runner executes AOT binaries at O0/O2 with region hoisting off/on.
The hoist variant tests this receiver representation; it does not establish
the safety of repeatedly evaluated borrow sites under the optional hoist.

```powershell
./probes/gates/receiver-borrow/run.ps1 -Compiler ./bootstrap_compiler/out/vyxc.exe
```

The real consumer is `samples/projects/zyn_render_smoke`: its quadratic and
cubic path builders borrow an owning `Path` receiver before adding points.
