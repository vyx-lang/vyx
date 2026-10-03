# Build pipeline measurements (Windows)

The first measured runs and their limitations are recorded in [BASELINE.md](BASELINE.md).
The repeated final measurements and the separate self-host observations are
reported in [RESULTS.md](RESULTS.md).

`Measure-Build.ps1` runs the selected compiler and samples its real descendant
process tree. It preserves the existing project cache and never deletes or
cleans build output. Prepare cold, warm or no-op state before invoking it, and
record that choice with `-CacheLabel`.

To check cache-record stability, save snapshots before and after an unchanged
build and run `python Compare-Records.py <before.cache> <after.cache>` from
this directory. Exit `0` means equal records; exit `1` reports exact changes.
Stabilize records after replacing compiler/backend/runtime artifacts before
making this comparison. A real toolchain change must invalidate the cache.

PowerShell 7 examples from the repository root, with the SDK compiler freshly
built from current source and its matching backend/runtime:

```powershell
$env:LLVM_ROOT = Join-Path (Get-Location) 'clang'
$compiler = Join-Path (Get-Location) 'bootstrap_compiler/out/vyxc.exe'
$serial = ./probes/gates/pipeline_perf/New-Fixture.ps1
./probes/gates/pipeline_perf/Measure-Build.ps1 -Compiler $compiler `
    -ProjectDir $serial -Target pipeline_perf -Jobs 1 -CacheLabel cold
./probes/gates/pipeline_perf/Measure-Build.ps1 -Compiler $compiler `
    -ProjectDir $serial -Target pipeline_perf -Jobs 1 -CacheLabel no-op

# Fresh source-only fixture: no recursive deletion or shared-cache reset.
$parallel = ./probes/gates/pipeline_perf/New-Fixture.ps1
./probes/gates/pipeline_perf/Measure-Build.ps1 -Compiler $compiler `
    -ProjectDir $parallel -Target pipeline_perf -Jobs 20 -CacheLabel cold
./probes/gates/pipeline_perf/Measure-Build.ps1 -Compiler $compiler `
    -ProjectDir $parallel -Target pipeline_perf -Jobs 20 -CacheLabel no-op
& (Join-Path $parallel 'target/pipeline_perf.exe')
```

For repeated measurements, `Run-Matrix.ps1` defaults to three fresh cold builds
per worker count (`1` and `20`), each followed by two consecutive unchanged-source
builds. It alternates the worker-count order, checks generated executable results
and object counts, and rejects a compiler/native artifact change mid-matrix.
Every observation keeps the same default 100 ms interval; no cache is deleted.

```powershell
./probes/gates/pipeline_perf/Run-Matrix.ps1 -Compiler $compiler -Repetitions 3
```

`Summarize-Runs.ps1` can regenerate `runs.csv`, `summary.csv` and `summary.json`
from a matrix's `measurements` directory, including partial runs after a failure.
It reports cold and the two no-op passes separately, with minima, medians, means
and maxima. Compare these repeated observations with the historical single-run
baseline only as an exploratory comparison, not as a controlled large-project
or Clang-equivalence claim.

The generator creates 64 independent Vyx modules plus the entry module by
default, each explicitly listed as a build source. The entry calls one unit
and checks its exact expected result; the other units expose public functions
and still compile as separate objects. This keeps the entry's import closure
small, so the fixture measures independent units rather than import fan-out.
`-SourceCount` can increase the fixture size. Generated sources exist
only beneath this directory's ignored `.runs/`, and existing fixtures are never
overwritten. Project source contents are deterministic for a given count.

The harness accepts `-Compiler`, `-ProjectDir`, `-Target`, `-Jobs`, `-OutputDir`,
`-BuildArgs`, `-CacheLabel` and `-SampleIntervalMs`. Build arguments are passed as
an argument array, without shell interpolation. It neither overrides scheduler
memory limits nor changes resource-control environment switches. Processes run
without opening a console window. Build stdout and stderr go to separate files;
the harness returns the compiler's exit code, including failure.

Each invocation saves:

- `metadata.json`: compiler hash/version, same-directory backend DLL/runtime
  library/TBB DLL hashes when present, LLVM version, commit, machine CPU and
  memory, exact argument array and cache label. Compiler-only dirty paths,
  per-file hashes, diff statistics and a diff hash identify uncommitted inputs.
- `compiler-source.diff`: tracked staged/unstaged differences from `HEAD`,
  scoped to compiler/runtime/DCI inputs. Untracked source files are fingerprinted
  in metadata. Editor settings, session files and other workspace state are
  excluded. A dirty checkout does not prove the tested binary includes those
  edits; compiler and native artifact hashes identify the actual files tested.
- `result.json`: wall time, parent peak RSS/private commit, simultaneous sampled
  tree peak RSS/private commit, observed CPU time and peak active Vyx children.
- `samples.csv`: per-sample active Vyx/native compiler counts, memory and CPU.
- `processes.csv`: last observations keyed by process ID and creation time.
- `stdout.log` and `stderr.log`: unmodified compiler output streams.

The parent peaks use Windows lifetime counters. The tree peaks are sampled
sums, not sums of each process's lifetime peak. Summed RSS includes shared pages
more than once; private commit is not resident physical memory. CPU time is the
sum of each process's latest observed cumulative CPU time. Children that start
and finish between samples, and the final CPU tail of an exited child, can be
missed. Consequently CPU/wall is an observed lower bound, not a complete CPU
utilization measurement. Active Vyx children exclude the parent driver; native
compiler/linker children are counted separately.

The default interval is 100 ms. Toolhelp process snapshots and process queries
have a cost, especially on large trees; `sample_work_ms` quantifies time spent
observing them. Compare with a larger interval or an uninstrumented run when
measuring very short builds. Machine metadata and compiler version queries
finish before the measured build starts. No unrelated process arguments or
environment secrets are collected.
