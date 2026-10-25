# Foreign return storage

Run with a compiler built from the current checkout and its matching runtime:

```powershell
python probes/gates/foreign-return-storage/run.py --compiler bootstrap_compiler/out/vyxc.exe --cxx clang/bin/clang++.exe --out .cache/foreign-return-storage
```

Each process has a 90-second timeout and a 2 GiB memory bound. The runner records
commands, sources and compiler/backend/runtime identities. It checks native AOT
at O0/O2, with a separately compiled C++ producer.

The decoder reproduction passes a local length output slot to the native string
decoder, then returns a view of the independently allocated buffer. The index
reproduction passes a temporary path and a local length slot, then returns a view
of storage owned by the native producer. Both match the Linux LSP build failure.

`@[ffi_returns_independent]` is a trusted C ABI contract: returned storage
does not borrow any argument. It is valid only on bodyless foreign declarations
returning `rawptr` or a typed pointer, with no attribute arguments. It does not
promise static lifetime, transfer ownership or choose a deallocator. The native
producer must actually satisfy the contract. DCI declarations continue using their
descriptors and cannot use this attribute. For example, an index may invalidate
its returned buffer on a later edit; a newly allocated buffer still needs release.

Effect validates the contract and sets a compiler-owned declaration fact. HIR
retains it on the exact selected foreign signature. MIR continues evaluating all
arguments and their side effects, but does not attach their local lifetimes to
this independent result. Exported `.vyi` interfaces preserve the contract.

Unknown external calls remain conservative. Negative cases check an actual
native alias of an owning local, an opaque attribute containing a forged marker,
an annotation on a Vyx body, an invalid scalar result, attribute arguments and a
same-named local function returning a dangling view. No symbol-name exemption or
general waiver for raw pointers is used.

Also rebuild the `vyxc-lsp` target with the new compiler and current
`bootstrap_compiler/std_packages`; small fixtures alone do not certify the actual
LSP self-build path. Linux execution and release packaging belong to the GitHub
runner, not to a Windows-only result from this gate.
