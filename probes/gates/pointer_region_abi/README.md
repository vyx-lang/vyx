# Pointer region ABI regression

This native probe compiles the actual `vyx_codegen/src/vyx_pointer_handle_rt.inc`
at `-O2`. It independently reads the 64-bit region layout at byte offsets
`base=0`, `bytes=8`, `owned=16`, `alive=24`, `rank=32`, `dimensions=40`, and
`next=48`, with a total size of 56 bytes. The offsets are literal in the probe
so a shared mistaken constant cannot make both sides pass.

Run from the repository root with PowerShell 7:

```powershell
pwsh -NoProfile -File probes/gates/pointer_region_abi/run.ps1
```

The runner uses the repository's `clang/bin/clang++.exe`. It builds only this
small native executable and does not rebuild the backend or Vyx compiler.
Each run preserves source hashes, compiler hash, build arguments, stdout,
stderr, and test results under `.runs/` without overwriting earlier runs.

Coverage:

- Three simultaneously live regions without shape metadata, including shared
  and mutable tags. The second and third regions have non-null registry links,
  so a layout that confuses those links with rank or dimensions cannot pass.
- Rank-2 dimensions, native and canonical-offset checked addresses, and reads.
- Owned deletion returns the original allocation once; later deletion returns
  null, with base/bytes cleared and alive set to zero.
- Range unregistration invalidates original and interior alias regions, clears
  base/bytes/alive, and preserves an unrelated region.
- Separate processes must fail with the expected runtime diagnostic for shared
  or mutable borrowed deletion, dangling access, access after unregistration,
  shape bounds, and flat byte bounds. Shape bounds use an address still inside
  the allocation so a missing dimension check cannot pass accidentally.

The probe checks the native implementation against the compiler's expected
byte layout. It is not a substitute for executing generated Vyx code through
the rebuilt compiler or for self-host and DCI gates.
