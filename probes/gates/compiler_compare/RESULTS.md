# S1 intermediate compiler comparison — 2026-09-27

The updated compiler improved the 16-leaf cold and interface-change builds, but
still trails CMake + Ninja + Clang on this workload. The first 64-leaf measurement
also exposes expensive driver work on a complete cache hit: **7.087 s with zero
compile/link tasks**, versus **0.089 s** for the corresponding Ninja no-op.
This is an intermediate S1 observation, not completion of the compiler roadmap.

All figures below are **one observation per configuration**, with the same
100 ms observer used by the [baseline](BASELINE.md). They are not statistically
established speedups. No 256/1024-unit workload, 64-unit flat graph or 64-unit
serial build was measured in this round.

## Tested identity and correctness

The same S1 executable was used throughout the successful comparisons:

- SDK compiler (historical S1 executable): `5D55754D8E1C3784E3E6B2BF75A9DFCDF94A9578A80B93A80282B4E5D4FF44A1`
- `vyx_compiler_backend.dll`: `3AA164E435418DF61004DBA0939C0687DE4CCF1A6FD41B3B0DE7C3346FE98A11`
- Compiler-source diff at collection: `3173EA46DDF952B022446D7D946E60A10345D823474A87B38C5245FBA89AA755`

Other workers were developing subsequent source changes during measurement.
The recorded checkout is therefore **not** proof that all recorded source edits
were compiled into S1. Compiler and native artifact hashes identify the actual
tested build. This S1 was not presented as a new self-host fixpoint.

Both languages use x86_64 Windows, O2 and no LTO. The 16-leaf layered graph has
22 translation units and 48 reachable output checks; the 64-leaf graph has 74
translation units and 192 checks. Every generated executable passed after every
phase, and every expected function definition was present in an object. The two
successful matrices contain 27 successful commands: three CMake configurations
and 24 builds.

## 16 leaves, fanout 4

The old observations are the paired baseline, not the earlier fixture that only
called one leaf. C++ was measured again with the same complete checks.

| Jobs | Phase | Old Vyx (s) | S1 Vyx (s) | Current C++ (s) |
| --- | --- | --- | --- | --- |
| 1 | Cold | 13.819 | 8.760 | 1.830 |
| 1 | No-op | 0.828 | 0.835 | 0.080 |
| 1 | Leaf body | 1.463 | 1.433 | 0.324 |
| 1 | Public interface | 9.979 | 7.165 | 1.704 |
| 20 | Cold | 5.886 | 2.279 | 0.828 |
| 20 | No-op | 0.782 | 0.882 | 0.081 |
| 20 | Leaf body | 1.474 | 1.334 | 0.308 |
| 20 | Public interface | 5.133 | 1.783 | 0.809 |

CMake configuration took 1.732 s and 1.726 s for the j1 and j20 fixtures.
Including configure, their first C++ builds took 3.562 s and 2.554 s. Configure
and build must remain separate when discussing incremental performance.

The serial Vyx cold build dropped from 13.819 to 8.760 s and the parallel cold
build from 5.886 to 2.279 s in these observations. No-op did not improve. The
parallel cold build is still about 2.75x the C++ build-only duration.

The following Vyx resource values pair old → S1 measurements. CPU is an observed
lower bound, and RSS/private values are simultaneous sampled process-tree peaks.

| Jobs | Phase | RSS (MiB), old → S1 | Private commit (MiB), old → S1 | Observed CPU (s), old → S1 |
| --- | --- | --- | --- | --- |
| 1 | Cold | 605.7 → 253.1 | 694.3 → 343.6 | 6.703 → 3.969 |
| 1 | No-op | 147.3 → 145.4 | 171.5 → 171.0 | 0.812 → 0.828 |
| 1 | Leaf body | 224.2 → 196.4 | 311.6 → 292.0 | 1.109 → 1.047 |
| 1 | Public interface | 617.8 → 252.2 | 709.5 → 343.7 | 6.078 → 3.766 |
| 20 | Cold | 664.3 → 569.3 | 1027.7 → 991.2 | 6.469 → 3.984 |
| 20 | No-op | 141.4 → 140.8 | 166.3 → 166.0 | 0.734 → 0.844 |
| 20 | Leaf body | 227.4 → 213.5 | 317.4 → 300.9 | 1.031 → 1.172 |
| 20 | Public interface | 644.0 → 610.3 | 1002.1 → 1029.8 | 6.281 → 3.641 |

Requested j20 produced a sampled peak of nine Vyx compiler children in S1's
16-leaf cold and interface builds. This is an observation, not a complete record
of instantaneous scheduling or a CPU-utilization percentage. Full C++ and Vyx
parent/tree metrics for every phase remain in each matrix's `runs.csv` and
per-command `result.json`.

## 64 leaves, fanout 8, j20

This is the first successful paired 64-leaf measurement. It has no old compiler
64-leaf measurement to use as a direct before/after comparison.

| Phase | Vyx wall (s) | C++ wall (s) | Vyx RSS (MiB) | C++ RSS (MiB) | Vyx private (MiB) | C++ private (MiB) | Vyx observed CPU (s) | C++ observed CPU (s) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Cold | 12.269 | 2.236 | 1651.4 | 112.3 | 2367.4 | 37.9 | 22.672 | 2.703 |
| No-op | 7.087 | 0.089 | 682.9 | 19.1 | 717.9 | 6.5 | 6.812 | 0.062 |
| Leaf body | 8.095 | 0.328 | 797.4 | 69.6 | 899.1 | 46.1 | 7.656 | 0.234 |
| Public interface | 10.996 | 2.198 | 1532.5 | 106.0 | 2178.0 | 46.2 | 21.734 | 2.609 |

CMake configuration added 1.761 s, making configure + first C++ build 3.997 s.
Vyx cold compilation remains about 5.49x the C++ build-only duration here. The
Vyx driver alone reached a lifetime peak RSS of about 690–694 MiB across these
phases. Cold and interface builds reached sampled peaks of 12 and 11 Vyx
compiler children, respectively. No Vyx compiler child was observed in the
no-op build, consistent with its zero executed compilation tasks.

These results locate substantial remaining cost outside code generation; they
do not identify a specific driver function without further profiling. The
64-leaf no-op spends 6.812 observed CPU seconds despite compiling and linking
nothing. Increasing the worker count cannot parallelize work that the driver
still performs serially.

## Task counts and exact no-op records

| Leaves | Phase | Vyx interfaces | Vyx objects | Vyx links | C++ objects | C++ links |
| --- | --- | --- | --- | --- | --- | --- |
| 16 | Cold | 22 | 22 | 1 | 22 | 1 |
| 16 | No-op | 0 | 0 | 0 | 0 | 0 |
| 16 | Leaf body | 1 | 1 | 1 | 1 | 1 |
| 16 | Public interface | 1 | 22 | 1 | 21 | 1 |
| 64 | Cold | 74 | 74 | 1 | 74 | 1 |
| 64 | No-op | 0 | 0 | 0 | 0 | 0 |
| 64 | Leaf body | 1 | 1 | 1 | 1 | 1 |
| 64 | Public interface | 1 | 74 | 1 | 73 | 1 |

The 16-leaf counts are the same for j1 and j20 and match the old task counts.
The public-interface extra Vyx main rebuild is explained in [BASELINE.md](BASELINE.md).

In all three successful Vyx no-op observations, the complete build-record file
was byte-identical to the corresponding cold-build snapshot. Comparison hashes
and results are in the 64-leaf run's `noop-record-checks.json`. The 7-second
64-leaf no-op is therefore not a return of the record-corruption/recompilation
bug: it checks stable records and executes zero compile/link tasks.

## Initial S1 failure and correction

Before these successful runs, the first S1 16-leaf j1 cold build crashed with
`0xC0000005` after 2.030 s. It is retained under `.runs/s1-16-layered-20260927/`
and is excluded from successful performance comparisons. Its backend hash was
`29C666C4C3508FFAAE6B205CD78AD32AA2FAE9E5053C2952AB45F3840D04BC9B`.

The crash was traced from SDK compiler RVA `0x80338` to `build_run_action_batch`'s
generated pointer checks. Native pointer metadata used a 40-byte C++ structure,
while generated checks and the canonical Vyx runtime consumed a 56-byte layout.
In particular, generated code read rank/dimensions at byte offsets 32/40, where
the native structure had its next pointer and memory beyond the descriptor.

The native descriptor now uses the canonical layout, including i64 ownership,
atomic i64 liveness and i64 rank, with complete size/alignment/offset assertions.
The exported entry-point signatures and the S1 compiler executable stayed the
same. The [native pointer ABI gate](../pointer_region_abi/README.md) passed its
positive case and six distinct failure cases. Successful measurements above
use the rebuilt, corrected backend hash shown at the top.

## Follow-up driver diagnostics

Two single no-op profiles reused the 64-leaf fixture after its public-interface
phase. These are localization runs, not another comparison matrix. Sampling
was relaxed to 500 ms; S1 overlapped a self-host build. Before the S2 profile,
one successful 12.304 s warm-up let the changed compiler identity invalidate
and rebuild its cache. Neither profile executed an interface compile, object
compile or link.

| Diagnostic | S1 | S2 |
| --- | --- | --- |
| Wall (s) | 7.031 | 7.563 |
| Driver target total (ms) | 6912 | 7448 |
| Through interface planning (ms) | 3445 | 3760 |
| Object planning (ms) | 3444 | 3664 |
| Within object planning: dependency stamps (ms) | 2208 | 2383 |
| Within object planning: root probes (ms) | 349 | 372 |
| Within object planning: records / cache checks (ms) | 94 / 3 | 100 / 4 |

S2's compiler SHA256 is
`37FBD96EF670275B445FEAE288AE2983CF4A56D62D02AF5E187CD77240972031`;
the corrected backend remains `3AA164E435418DF61004DBA0939C0687DE4CCF1A6FD41B3B0DE7C3346FE98A11`.
Its sampled tree RSS was 649.1 MiB and observed CPU time was 7.391 s. This
single diagnostic does not establish a speed regression, but it does show
that the S2 set/group/history changes have not removed this fixture's main
no-op cost.

The `interface-vyi` timer starts at target entry, so its value includes earlier
configuration and grouping work. Most `vyi detail` sub-timers currently stay
at their initialized zero; those zeros cannot prove that those operations are
free. Dependency stamp timing is a separately measured part of object
planning, not an additional phase to add to its total.

The remaining repeated dependency work follows this live call chain in
`bootstrap_compiler/src/core/project/build_system.vyx`:

```text
build_target
  build_partition_dependency_stamp
    build_partition_dependency_sources
      build_module_file_from_sources (for each import)
        build_module_name_from_file (scan the source candidates)
          build_module_name_cache_get (linear text-cache lookup)
```

Each source's partition walk independently resolves imported modules and then
computes interface stamps. The current module lookup scans source candidates,
performs file queries and consults a line-oriented cache; this is still driver
work even when every object is cached. A per-invocation source/module index,
precomputed dependency adjacency and reuse of dependency stamps are concrete
next targets. Exact module matching and the existing longest valid dotted
prefix rule must be preserved. The profile does not justify bypassing
dependency validation or changing cache correctness rules.

Raw diagnostics are in `.runs/driver-trace-s1-64/`,
`.runs/driver-warm-s2-64/` and `.runs/driver-trace-s2-64/`. Both profiling
environment variables were restored after each invocation.

## Reproduction and retained paths

These examples select the SDK compiler built from current source. The tables
above retain measurements of the recorded S1 executable.

```powershell
./probes/gates/compiler_compare/Run-Comparison.ps1 `
    -Compiler ./bootstrap_compiler/out/vyxc.exe `
    -UnitCount 16 -Layout Layered -Fanout 4 -Repetitions 1 `
    -SampleIntervalMs 100

./probes/gates/compiler_compare/Run-Comparison.ps1 `
    -Compiler ./bootstrap_compiler/out/vyxc.exe `
    -UnitCount 64 -Layout Layered -Fanout 8 -Jobs @(20) `
    -Repetitions 1 -SampleIntervalMs 100
```

The exact measured runs are retained in:

- `.runs/s1-abi-fixed-16-layered-20260927/`
- `.runs/s1-abi-fixed-64-layered-20260927/`

Each contains tool and source identities, the script versions, fresh paired
fixtures, structured commands, raw output, sampled process metrics, actual task
records, symbol checks and artifact hashes. No additional larger batch was
started after these two matrices.


## Current regression entry points

The tables above remain measurements of their recorded compiler versions.
For current source, use the [source-index gates](../build_source_index/README.md),
[generic-interface gates](../generic_interfaces/README.md), and
[compiler pressure gate](../compiler-industrial/README.md).
