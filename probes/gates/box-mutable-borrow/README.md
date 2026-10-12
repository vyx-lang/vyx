# Mutable Box payload borrows

```powershell
.\probes\gates\box-mutable-borrow\run.ps1 -Compiler <fresh SDK compiler>
```

Run with the current SDK standard packages. `Box.deref_mut(&mut self)` borrows
its uniquely owned payload without copying ownership. The AOT gate mutates
concrete and dynamic trait payloads at O0/O2, reads the concrete result through
`deref()`, and rejects calling `deref_mut` through `&Box<T>` with E3101.
It also mutates an element through `Vec.get_mut`, reads it through
`get_ref`, destroys the owning vector, and rejects `get_mut` through `&Vec<T>`.

The real consumer is Zyn's CustomRenderHandler path: rendering and releasing a
boxed handler require a mutable payload reference. Zyn's render and custom
render smoke applications cover that path separately.
