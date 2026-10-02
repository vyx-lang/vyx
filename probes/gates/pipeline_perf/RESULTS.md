# Pipeline measurements — 2026-09-27

The repeated measurements confirm that unchanged-source builds no longer
regenerate interfaces, objects or the executable. They do **not** show a
material cold-build speedup over the historical small-fixture observations.
The separate self-host measurements still show high memory use and limited
observed worker concurrency. These results do not establish industrial-scale
performance or parity with Clang.

## Reproduce the matrix

From the repository root, with PowerShell 7 and the SDK compiler built from
current source beside its matching backend/runtime. The example uses the current
SDK entry point; the figures below retain the dated historical observations:

```powershell
$env:LLVM_ROOT = Join-Path (Get-Location) 'clang'
./probes/gates/pipeline_perf/Run-Matrix.ps1 `
    -Compiler ./bootstrap_compiler/out/vyxc.exe `
    -Repetitions 3 -SourceCount 64 -SampleIntervalMs 100 `
    -OutputDir ./probes/gates/pipeline_perf/.runs/final-matrix-20260927
```

Use a new output directory when repeating this command; the runner refuses
to overwrite an existing matrix. Each worker count gets three fresh fixtures.
Each cold build is followed by two builds without source changes. Worker-count
order alternates between repetitions. No cache or output directory is deleted,
and scheduler resource limits are not overridden.

The fixture has 64 independent modules and one entry module. All 65 source
units are explicitly compiled, but the entry imports and calls only one unit.
It checks `compute000(0) == 136`. This deliberately measures independent source
units with shallow imports. It is not a representative Zyn application or an
equivalent C++ workload.

All 18 compiler invocations exited `0`. Each generated executable also exited
`0`, and every fixture contained at least 65 object files. Raw observations,
source fixtures and aggregate CSV/JSON remain in
`.runs/final-matrix-20260927/`. The runner itself returned `0`.

## Wall time and memory

Every row contains three measurements. RSS/private values are the largest
simultaneous sampled process-tree values among those three measurements;
they are not sums of independent per-process peaks.

| Workers requested | Build | Wall median (s) | Wall range (s) | Maximum tree RSS (MiB) | Maximum tree private commit (MiB) | Peak observed Vyx children |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | Cold | 30.470 | 30.414–30.912 | 382.0 | 482.6 | 1 |
| 1 | First no-op | 2.138 | 2.092–2.158 | 254.1 | 282.4 | 0 |
| 1 | Second no-op | 2.117 | 2.067–2.180 | 255.8 | 282.7 | 0 |
| 20 | Cold | 6.385 | 6.227–7.266 | 784.7 | 1267.4 | 10 |
| 20 | First no-op | 2.170 | 2.128–2.175 | 255.6 | 282.7 | 0 |
| 20 | Second no-op | 2.102 | 2.067–2.130 | 262.0 | 292.4 | 0 |

The ratio of serial to parallel cold medians is 4.77x. This is a comparison of
worker settings on this fixture, not an improvement over the previous compiler.
The observer saw a maximum of 7–10 concurrent Vyx children in the parallel
cold runs. Short-lived workers can fall between samples, so this does not prove
that the scheduler never briefly reached a higher count.

Observed cumulative process CPU time was 11.250–11.797 s for serial cold builds
and 11.375–15.797 s for parallel cold builds. These are lower bounds: exited
workers' unsampled CPU tails and unobserved short processes are missing.
Observer work occupied 10.3–16.5% of the measured build wall time across the
matrix. This records measurement cost, not a causal slowdown estimate.

## Incremental-build work

| Build kind | Observations | Interfaces compiled per run | Objects compiled per run | Link executions per run | Link cache hits per run |
| --- | --- | --- | --- | --- | --- |
| Cold | 6 | 65 | 65 | 1 | 0 |
| No source changes | 12 | 0 | 0 | 0 | 1 |

The earlier baseline's no-op requests compiled 9 interfaces and 1 object at
`-j1`, and 12 interfaces and 2 objects at `-j20`; both relinked. The native string
length defect that corrupted record separators was fixed; a separate
before/after cache-record comparison found identical records after two
unchanged-source builds. Cache comparison instructions are in the
[measurement guide](README.md).

Combining both no-op positions gives six observations per worker count. Their
medians are 2.127 s at `-j1` and 2.129 s at `-j20`. Cache checking still costs
about two seconds for this small project even after redundant compilation is
removed.

## Historical comparison: exploratory only

The [baseline](BASELINE.md) contains one observation per configuration, while
the final matrix contains three cold and six no-op observations per worker
count. Both used the same source generator, machine and nominal 100 ms sampling
interval. They ran at different times; operating-system caches and system load
were not controlled. Historical metadata did not collect native-library hashes
or the compiler-source diff, and the final observer also validates process
creation times to avoid PID reuse errors. Do not treat this as a controlled
before/after benchmark of an individual optimization.

| Configuration | Historical single wall (s) | Final median (s) | Arithmetic change |
| --- | --- | --- | --- |
| `-j1` cold | 31.220 | 30.470 | −2.4% |
| `-j20` cold | 6.190 | 6.385 | +3.1% |
| `-j1` no-op | 3.976 | 2.127 | −46.5% |
| `-j20` no-op | 3.594 | 2.129 | −40.8% |

The cold figures are of the same order, with the parallel final median slightly
slower. The no-op work counts provide stronger evidence than the percentage
changes: all twelve final no-op runs avoided frontend/codegen/link invocations.

## Separate self-host observations

The parent worker measured SDK compiler target builds with `-j20`
using this harness with a **500 ms** interval. These are different workloads
and sampling settings from the fixture matrix.

| Stage | Wall (s) | Tree RSS (MiB) | Tree private commit (MiB) | Peak observed Vyx children | Observed CPU (s) |
| --- | --- | --- | --- | --- | --- |
| S2 | 197.974 | 10899.9 | 11550.7 | 3 | 274.391 |
| S3 | 196.144 | 10898.6 | 11595.5 | 3 | 269.203 |

Raw records are in `.runs/selfhost-final-s2/` and `.runs/selfhost-final-s3/`.
The source state was the same, but the compiler seed and cache state differed.
These two stages demonstrate successful rebuilding and a stable fixpoint; they
do not demonstrate a substantial performance improvement. The roughly
10.6 GiB sampled RSS and three observed workers remain important limits of the
real pipeline. A representative large-project benchmark, with repeated cold
and incremental runs and complete build identity, is still needed.

## Build identity

The matrix ran on Windows x64, Intel Core Ultra 7 265KF, 20 logical processors,
34,031,194,112 bytes of physical RAM, with LLVM 22.1.1. The repository HEAD was
`8e44b87647c48584b89fdfd43b82fcdca882b7c7` with uncommitted changes in:

- `bootstrap_compiler/src/codegen/llvm_lower.vyx`
- `bootstrap_compiler/src/core/build_system.vyx`
- `vyx_codegen/src/vyx_bootstrap_rt.cpp`

The compiler-source diff SHA256 was
`C3B8FA0EEFD8CCC976832C52580E1E0D2FEC3E6AF9EF96DE99F22E93513164FE`
in all eighteen observations. Each observation retains the diff, scoped dirty
paths, per-file hashes and toolchain/machine/argument metadata.

| Artifact | SHA256 |
| --- | --- |
| SDK compiler (historical executable, also the identical S2/S3 output) | `70731A1F95454406343EE7D0F1858C01DA7C3FFA9EC31BEA956502282999C3C0` |
| `vyx_compiler_backend.dll` | `1D4BE452AAD600D5ECD5FB777C01D5B3C48B7BCCCB7AD84457BA80B1D5A37BBA` |
| `vyx_runtime.lib` | `AD3C5168F391C824054E4E609F1A21214FB8354E4E5985AB200EE44B55E2D4CC` |
| `tbb12.dll` | `29D99E19D53449B2831E3B053EDFDB47AE0622941249C9F8242A96DF68EDB023` |

Compiler and same-directory native artifact hashes stayed constant throughout
the matrix. The runtime library hash identifies the on-disk link input; it does
not independently prove which runtime object bytes are embedded in the compiler.
The metadata scope excludes editor settings and unrelated workspace content.
