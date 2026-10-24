# Bootstrap string scan cost and bounds

```powershell
python probes/gates/runtime-string-scan/run.py --backend bootstrap_compiler/out/vyx_compiler_backend.dll --output .cache/runtime-string-scan
```

On Linux select the matching `.so`, export its `LD_LIBRARY_PATH`, and provide
`clang++` on PATH. The probe loads the real backend. Linux interposes `mincore`
to count the work done by short string queries and explicit-length searches;
these operations must issue zero page-residency probes. It also verifies
buffer mutation/address reuse, page-crossing text and a `PROT_NONE` guard.
Wall time is printed for comparison, without a machine-dependent time limit.

`--unit` compiles the production private scanner header without LLVM. This
is useful for checking POSIX behavior on a host without LLVM development files;
it does not certify a complete SDK. Release native builds additionally run
against the newly built backend before compiling Vyx targets.

Pointer-only bootstrap string compatibility retains its existing 1 MiB scan
cap. Explicit registered byte views keep their lengths and lifetime rules.
Unknown pointer scan results remain uncached. Linux checks mapping read
permissions when a scan or explicit span crosses a page; short inputs stop
at NUL without probing unrelated pages. This removes the previous fixed
256-page `mincore` walk on every short query and avoids reading mapped but
inaccessible guard pages.
