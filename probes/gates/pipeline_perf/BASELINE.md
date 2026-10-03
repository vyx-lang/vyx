# Initial measurement, 2026-09-27

Compiler source baseline: `8e44b87647c48584b89fdfd43b82fcdca882b7c7`.
Compiler executable SHA-256:

```text
9CEA76C4C8989C8B14E2E0D8592CFBBA292CFA88D37512A39E4DF8B8FCEEF4B3
```

Windows x64, Intel Core Ultra 7 265KF, 20 logical processors, 31.69 GiB physical
memory, repository LLVM 22.1.1. Measurements used the existing compiler/backend
before the subsequent native rebuild. Default compiler resource controls were
preserved. No Zyn project was built or cleaned.

These historical runs used metadata schema 1. Backend DLL, runtime library,
TBB DLL and compiler-source diff hashes were not collected, and have not been
backfilled from later files. Schema 2 collects those identities for new runs.

Two fresh source-only fixtures each contained 64 independent modules plus the
entry module. Each build emitted 65 object files. Each generated executable
returned `0` and checked that `compute000(0) == 136`. The second build of each
fixture changed no source or configuration; its cache label is `no-op`, which
describes the request, not a guarantee that the compiler did no work.

| Invocation | Exit | Wall (s) | Parent peak RSS (MiB) | Tree sampled peak RSS (MiB) | Tree sampled peak private (MiB) | Observed CPU (s) | Peak observed Vyx children |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `-j1`, cold | 0 | 31.220 | 257.4 | 379.6 | 558.2 | 12.172 | 1 |
| `-j1`, no source changes | 0 | 3.976 | 281.5 | 350.4 | 446.6 | 2.422 | 1 |
| `-j20`, cold | 0 | 6.190 | 259.6 | 735.3 | 1236.1 | 11.469 | 9 |
| `-j20`, no source changes | 0 | 3.594 | 281.9 | 400.4 | 550.1 | 2.484 | 2 |

The observed cold speedup was 5.04x on this small independent-unit fixture.
This is not a large-application benchmark or a comparison with Clang. The
100 ms observer can miss short workers. Sampling work took 4.324 s during the
serial cold run and 0.702 s during the parallel cold run (approximately 14% and
11% of build wall time). That is time spent observing, not a measured causal
slowdown; uninstrumented measurements are needed to quantify observer effects.
The final harness additionally validates creation times to avoid mistaking a
reused Windows process ID for an earlier child.

The unchanged-source runs still did work: the serial build regenerated some
`.vyi` files; the parallel build recompiled `unit000` and `unit016` objects and
relinked. Keep that cache behavior visible when comparing later improvements.

Artifacts remain in these ignored directories:

```text
.runs/baseline-independent-j1-cold/
.runs/baseline-independent-j1-noop/
.runs/baseline-independent-j20-cold/
.runs/baseline-independent-j20-noop/
```

An earlier fixture made `main` directly import all 64 modules. Its serial run
was manually stopped after 88.322 s as the sampled tree RSS reached 7992 MiB;
exit `-1` and the complete observations remain in `.runs/baseline-j1-cold/`.
That incomplete experiment is not included in the success table. It indicates
an import-fan-out workload worth investigating separately. The current
generator keeps imports shallow while still compiling all explicit sources.

Failure propagation was checked with target `missing_measurement_target`:
the compiler returned `1`, the harness returned `1`, and JSON/CSV/stdout/stderr
were retained in `.runs/harness-failure-check/`. No directories were deleted.
