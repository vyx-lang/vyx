# Paired 16-leaf smoke baseline — 2026-09-27

The initial equivalent-work smoke test completed successfully for Vyx and
CMake + Ninja + Clang at both `-j1` and `-j20`. Vyx was substantially slower
on this small workload. These are single observations per configuration, not
median estimates, a large-project benchmark or an industrial acceptance result.

## Workload and command

Each project contained 16 compute leaves, one shared implementation, four
verification groups and main: **22 translation units**. All leaves ran the same
24-round i64 recurrence. Main reached all **48 output checks**. Both programs
returned zero after all eight language/stage combinations per worker count.
Object inspection found every expected function definition and executable code
in each object. Both languages emitted 22 objects on a cold build.

The example below uses the current SDK compiler entry point. The recorded
measurements and hashes remain observations of the dated historical build.

```powershell
./probes/gates/compiler_compare/Run-Comparison.ps1 `
    -Compiler ./bootstrap_compiler/out/vyxc.exe `
    -UnitCount 16 -Layout Layered -Fanout 4 -Repetitions 1 `
    -SampleIntervalMs 100 `
    -OutputDir ./probes/gates/compiler_compare/.runs/baseline16-layered-20260927
```

The runner used its default worker counts `1,20`. Both languages targeted
`x86_64-pc-windows-msvc`, with `-O2`, no LTO and LLD linking. CMake's retained
`compile_commands.json` contains the explicit target, `-fno-lto` and `-O2` flags.
Vyx's retained build records contain `-O2` and the same target. Machine:
Windows x64, Core Ultra 7 265KF, 20 logical processors, approximately 31.69 GiB
physical memory. Sampling interval was 100 ms.

## Build wall time

C++ configure time is excluded from the following build rows and reported
separately below.

| Workers | Phase | Vyx (s) | C++ (s) | Vyx / C++ |
| --- | --- | --- | --- | --- |
| 1 | Cold | 13.819 | 1.791 | 7.72x |
| 1 | No-op | 0.828 | 0.078 | 10.60x |
| 1 | Leaf body | 1.463 | 0.314 | 4.66x |
| 1 | Public interface | 9.979 | 1.727 | 5.78x |
| 20 | Cold | 5.886 | 0.834 | 7.05x |
| 20 | No-op | 0.782 | 0.081 | 9.62x |
| 20 | Leaf body | 1.474 | 0.312 | 4.73x |
| 20 | Public interface | 5.133 | 0.811 | 6.33x |

CMake configuration took **2.684 s** for the first fixture and **1.721 s** for
the second. C++ configure + cold build therefore took **4.475 s** and **2.556 s**,
respectively. Vyx has no separately timed configure step; its cold build already
contains project planning. Tiny no-op durations are particularly sensitive to
instrumentation, process-start overhead and system load.

## Actual project tasks

The task counts were identical at the two worker settings:

| Phase | Vyx interfaces | Vyx objects | Vyx links | C++ objects | C++ links |
| --- | --- | --- | --- | --- | --- |
| Cold | 22 | 22 | 1 | 22 | 1 |
| No-op | 0 | 0 | 0 | 0 | 0 |
| Leaf body | 1 | 1 | 1 | 1 | 1 |
| Public interface | 1 | 22 | 1 | 21 | 1 |

The public-interface edit adds a shared function declaration and definition;
it does not change main or call the new API. C++ main includes only group
declarations, so it is unaffected by the shared header edit. The other 21 C++
translation units include that header directly or transitively and rebuild.
Vyx's main build record includes a transitive `dep_iface:src/shared.vyx` stamp,
and it recompiles along with the other 21 source units. This is a measured
dependency-invalidation difference, not an equal task count imposed by the test.

The implementation-only edit changes one leaf recurrence to an algebraically
equivalent form. Both languages rebuild exactly that object's implementation
and relink. Main and expected results remain unchanged, avoiding an extra
main-source edit that would contaminate the leaf-body measurement.

## Cold-build resource observations

| Language | Workers | Tree peak RSS (MiB) | Tree peak private commit (MiB) | Observed CPU (s) | Peak observed compiler children |
| --- | --- | --- | --- | --- | --- |
| Vyx | 1 | 605.7 | 694.3 | 6.703 | 1 Vyx |
| C++ | 1 | 56.9 | 24.9 | 0.734 | 1 Clang |
| Vyx | 20 | 664.3 | 1027.7 | 6.469 | 8 Vyx |
| C++ | 20 | 107.6 | 33.3 | 0.844 | 3 Clang |

Tree memory is a simultaneous sampled sum; RSS double-counts shared pages.
CPU is a lower bound. The small Clang tasks can start and finish between
samples, so the sampled Clang count of three is not evidence that Ninja only
scheduled three workers. The observer records real processes and actual task
records separately. Full parent peaks, private memory and all stage samples
are retained in `runs.csv` and per-observation JSON/CSV files.

## Identity and retained evidence

All 18 measured commands exited zero: two CMake configurations and 16 project
builds. The compiler and same-directory backend/runtime/TBB hashes remained
constant during the comparison. The tested Vyx executable was the previous
self-host fixpoint, before the next scheduler/frontend changes.

| Tool | SHA256 |
| --- | --- |
| Vyx SDK compiler (historical executable) | `70731A1F95454406343EE7D0F1858C01DA7C3FFA9EC31BEA956502282999C3C0` |
| `clang++.exe` | `4967756289310BB8369D5C036506197E9DBDAB24E8ACF29B70F2626A972767F2` |
| `vyx_compiler_backend.dll` | `1D4BE452AAD600D5ECD5FB777C01D5B3C48B7BCCCB7AD84457BA80B1D5A37BBA` |
| `vyx_runtime.lib` | `AD3C5168F391C824054E4E609F1A21214FB8354E4E5985AB200EE44B55E2D4CC` |
| `tbb12.dll` | `29D99E19D53449B2831E3B053EDFDB47AE0622941249C9F8242A96DF68EDB023` |

HEAD at collection was `e52de175f2a4673ae45a3245b387077285bdc945`, while other
workers were editing the next compiler version. Scoped source state is recorded
in `comparison.json`, with a tracked diff hash of
`B31A55E3BB75D35BACF6F9092679F18E35779CD970FBD39791C1F6BC73EEEA14`.
Those edits do not identify the tested binary; the frozen artifact hashes above
do. The initial metadata recorded untracked compiler path names but did not
hash their contents. Later runner versions add that field.

Raw evidence remains under `.runs/baseline16-layered-20260927/`: generated
fixtures, compiler inputs, structured argument requests, every output stream,
samples, process identities, task records, object symbols and artifact hashes.
The script versions used for this smoke run were copied before later reporting
and snapshot enhancements. `summary.json`, `summary.csv` and `ratios.csv` were
generated from the preserved observations without running another build.

Only source generation and static checks were additionally performed for
64-leaf Flat and Layered fixtures. They contain 66 and 74 translation units,
respectively, and both have 192 reachable output checks with matching
recurrences and independently recomputed expected values. No 64/256/1024-unit
build has been measured by this gate yet.
