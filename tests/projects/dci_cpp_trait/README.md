# C++ DCI virtual callbacks

This Windows AOT fixture imports the original `SinkG<T>` producer template.
The project builder discovers and measures its `i32`, `i64`, consumer record,
and producer record instances. The initial contract contains no closed template
instances. Both cold builds and recovery after losing generated discovery
artifacts must compile and run.

`VyxHostI32` also owns a scalar call counter in its native subclass. `PointerSink`
checks the native callback ABI: C++ passes a borrowed `char*` into Vyx and receives
the identical address back, including `nullptr`, over 2000 calls. No byte is
dereferenced in that callback. Code that directly dereferences raw native
callback state uses `unsafe` and a lifetime guarantee supplied by its owner,
as demonstrated in the Qt counter fixture.

From the repository root, with a newly built SDK compiler:

```powershell
./tests/projects/dci_cpp_trait/run.ps1 -BootstrapCompiler ./bootstrap_compiler/out/vyxc.exe
```

The gate needs the repository's LLVM development tools and Python DCI tools.
An SDK compiler located outside the repository can select the current adapters
with `VYX_DCI_TOOLS` pointing to `tools/dci`. Generated contracts, native stubs,
logs, and executables stay in `contracts/`, `.cache/`, and `target/`.
