# Vyx / CMake + Ninja + Clang build comparison

This gate generates paired Vyx and C++ projects with the same integer algorithm,
module graph and output checks. It measures real compiler processes and preserves
the inputs and observations needed to inspect a result. Initial 16-leaf smoke
measurements are recorded in [BASELINE.md](BASELINE.md); they are small-project
observations, not an industrial-scale performance claim.
The next S1 compiler's paired 16- and 64-leaf observations, including its initial
ABI failure, are recorded in [RESULTS.md](RESULTS.md).

## Run

Windows and PowerShell 7 are required. From the repository root, select the SDK
compiler freshly built from current source with its matching backend/runtime:

```powershell
$env:LLVM_ROOT = Join-Path (Get-Location) 'clang'
./probes/gates/compiler_compare/Run-Comparison.ps1 `
    -Compiler ./bootstrap_compiler/out/vyxc.exe `
    -UnitCount 16 -Layout Layered -Fanout 4 -Jobs @(1, 20) `
    -Repetitions 1 -SampleIntervalMs 100
```

The defaults select the repository's `clang/bin/clang++.exe`,
`D:/Jetbrains/CLion/bin/cmake/win/x64/bin/cmake.exe`, and
`D:/LLVM/bin/ninja.exe`. `-CppCompiler`, `-CMake`, `-Ninja` and `-LlvmNm` override
those paths. Both builds target `x86_64-pc-windows-msvc` at `-O2`, without LTO.
C++ uses CMake's Ninja generator, explicitly disables interprocedural
optimization, passes `-fno-lto`, and links through LLD. Vyx uses its native object
pipeline and LLD; no LTO option is enabled. Generated programs use no external
packages, exceptions, containers, templates, reflection or strings.

`-UnitCount 64`, `256` and `1024` select larger workloads. `-Layout Flat` makes
main call every leaf directly. `-Layout Layered -Fanout 8` adds small verification
groups until each group and main refer to at most eight child units. Every leaf
is still called three times and checked. Layering changes the dependency graph
and number of translation units; compare the two languages within the same
layout and report layout differences separately.

Use `-Repetitions 3` or higher for repeated observations. Each worker count and
repetition gets a new fixture, so cold runs never require deleting a cache.
Subsequent phases preserve that fixture and change only the designated sources.
Language order and worker-count order alternate by repetition. Keep the tested
compiler and native libraries unchanged while the runner is active; tool hashes
are checked before each build. Do not run unrelated heavy builds concurrently.

To generate source without compiling:

```powershell
./probes/gates/compiler_compare/New-PairedFixture.ps1 -UnitCount 64 -Layout Flat
./probes/gates/compiler_compare/New-PairedFixture.ps1 -UnitCount 64 -Layout Layered -Fanout 8
```

All generated content is confined to ignored `.runs/` directories. Existing
fixtures and observations are never overwritten or cleaned. Larger fanout
fixtures can expose compiler memory problems; a successful 16-unit smoke run
does not establish that 64/256/1024 builds will fit the machine.

## Equivalent work and changes

Each leaf exports `computeNNNN(i64) -> i64`, calls a shared bias function, and
performs 24 rounds of a positive integer modular recurrence. `-Rounds` changes
both languages identically. For all supported parameters the intermediate
arithmetic stays below 34 million, so signed overflow and differing overflow
semantics do not affect the comparison. The generator independently calculates
expected results for seeds `0`, `17` and `991`.

Main reaches all three checks for every leaf, either directly or through group
functions. A failed check returns a nonzero exit code. After every measured
build, the runner executes the program and uses `llvm-nm` to require each
expected function definition and at least one defined text symbol per object.
All generated source units therefore contribute actual code; this is not a
fixture with many unused files and a main that only calls one function.

The phases are:

1. **Cold:** compile a newly generated source-only project.
2. **No-op:** repeat the build without modifying sources or configuration.
3. **Leaf body:** change only `unit0000`'s implementation, rewriting
   `value * 33` as `value * 32 + value` in both languages. The arithmetic and
   expected results remain identical, so main is not modified as a side effect.
4. **Public interface:** add `compare_api_revision() -> i64` to the shared Vyx
   module and add the matching declaration/definition to the C++ shared
   header/source. This tests invalidation caused by expanding a public API. The
   added function is not called; this phase does not test consuming a new API
   or changing a data-layout ABI.

The public-interface phase follows the body edit; it is an incremental sequence,
not a second cold build. C++ declaration headers and Vyx module interfaces have
different dependency semantics. Rebuild counts expose those differences instead
of forcing equal counts. Vyx's separate `.vyi` generation is counted separately
from object generation; C++ header processing is included in each object task.

## Measurement and artifacts

`Measure-Command.ps1` is a reusable Windows process-tree observer. Its parameters
are `Executable`, `ArgumentList[]`, `WorkingDirectory`, `OutputDir`, `Label` and
`SampleIntervalMs`. `Invoke-Measurement.ps1` accepts a JSON request so the caller
does not serialize an argument array into a shell command. Processes use hidden
windows and redirected output. Compiler failures retain their logs and exit code.
No unrelated command lines or environment secrets are collected.

CMake configuration is a separate observation. Its compiler identification and
try-compiles are included in configuration wall/CPU/memory, but not in the
reported project object-task count. Vyx has no separate configure command:
manifest reading and project planning remain included in its build time. Report
configuration and cold build individually; when first-build latency matters,
also report their sum for C++.

Each observation records wall time, process-tree CPU lower bounds, parent
lifetime RSS/private peaks, simultaneous sampled tree RSS/private peaks,
observed compiler/linker/build-driver child counts, stdout and stderr. PID and
creation time jointly identify a process, and the root process's retained handle
provides its lifetime counters after exit. Tree RSS counts shared pages more
than once. Private commit is not physical residency. Short-lived processes and
their CPU tails can be missed; sampled process counts are not exact task counts.
The observer's own time is reported, and instrumentation is material for tiny
builds, especially sub-100 ms Ninja no-op runs.

Actual **project build tasks** come from Vyx's executed compile/link progress
records and newly appended Ninja build-log output records. This fixture has one
object output per C++ compile and one executable output per link. Configuration
rows have no project task count. Raw logs are retained so these counts can be
checked independently; a sampled process count is never substituted for them.

The output directory contains:

- `comparison.json`: tool identities, compiler-source state, target/optimization,
  fixture parameters and measurement policy.
- `compiler-source.diff`: compiler/runtime/DCI changes from HEAD, excluding
  editor settings and unrelated workspace state. Dirty source hashes include
  untracked compiler files. Checkout state is not proof of binary provenance.
- `scripts/`: measurement/generator script versions used for the run.
- `fixtures/`: generated sources, build outputs, manifests and change records.
- `requests/`: exact structured command requests.
- `measurements/*/`: JSON/CSV samples, process records, stdout/stderr, executed
  task records, object symbols, output hashes and build-record/compile-command
  snapshots. Verification and artifact hashing happen outside measured builds.
- `runs.csv`, `summary.csv`, `ratios.csv`, `summary.json`: individual observations
  and per-language/stage/worker-count aggregates. Regenerate these with
  `Summarize-Comparison.ps1 -InputDir <run-directory>`.

The old 16-unit baseline predates two metadata additions: it retained scoped
untracked path names but not their individual hashes, and kept Vyx records/C++
compile commands only in the final fixture rather than per-stage snapshots.
Those historical observations are not retroactively presented as having fields
that were not collected. Its executable and object hashes were recorded per
stage, and the unchanged tested compiler/native artifacts were fingerprinted.

## Interpretation

This measures equivalent generated program behavior and ordinary project build
workflows. It does not make the two languages' frontend algorithms, metadata,
runtime startup, object sizes or dependency systems identical. No claim about
Zyn, a full compiler self-host, or a production C++ application follows from this
fixture alone. Publish counts, layout, repetition count, all timing stages,
memory, actual rebuilt tasks and failures alongside ratios.
