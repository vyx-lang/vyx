# Benchmark Workloads

[Documentation](../../docs/README.md) · [Compiler verification](../../docs/TESTING_GUIDE.md)

These projects compare optimized native programs, not source syntax or isolated
library calls. Vyx and C++ use their normal language and standard-library
facilities while preserving the same algorithm, scale, integer model, and
observable result.

The C++ host is archived and is not a current quality gate. Run a benchmark only
with the SDK compiler freshly built from current source using a compatible
Release SDK compiler as Stage 0; benchmark results must record that seed's
identity and the source commit.

## Fairness Rules

- Both sides use the same `-O` level. The default is `-O2`.
- Vyx is optimized as a whole project. C++ therefore uses `-flto` and LLD by
  default; pass `-SkipCppLto` only when measuring non-LTO builds explicitly.
- Runtime measurements use a process warmup by default. This excludes the
  one-time cost of loading and scanning a newly linked Windows executable.
- Capacity is reserved when the final size is known. Vyx and C++ may use
  different APIs when those are the normal efficient APIs for that language.
- The dynamic-dispatch workload creates its two polymorphic objects before the
  loop on both sides. Its C++ call has one runtime-selected base reference, so
  LTO cannot turn two concrete call sites into two direct calls. Repeated
  concrete-to-`dyn` conversion is a separate allocation cost, not dispatch.
- `string` is a borrowed Vyx text view. Workloads that retain generated text
  take an owned clone before storing it, matching C++'s owning `std::string`.
- Every workload checks invariants or exact expected checksums internally and
  exits nonzero on failure. `-RequireSemanticMatch` additionally requires equal
  stdout hashes between Vyx and C++ for every scenario.
- Generated files under `benchmarks/out/` are measurements, not source data.

## Workload Matrix

| Workload | Scale and purpose |
|---|---|
| `baseline` | Mixed arithmetic, strings, generic value aggregates, GCD, polynomial folding, and string scanning. |
| `core_compute` | 40M arithmetic rounds, 25M small-matrix state rounds, and 35M state-machine rounds. |
| `strings` | Reserved string construction plus more than 100M bytes of iteration, ASCII transformation, search, prefix/suffix checks, and slicing. |
| `generics` | Constrained min/max and 100M iterations through flat and nested generic value aggregates. |
| `collections` | A 1M-element vector reduction and a 160K-element sort with 40K binary-search probes. |
| `memory_model` | Value copies (100M), unique heap ownership (5M), and atomic shared ownership (5M) in one run. |
| `memory_value` | Isolated 100M-iteration value-copy and mutation pipeline. |
| `memory_unique` | Vyx `Box<T>` versus C++ `unique_ptr<T>` across 5M allocations and mutations. |
| `memory_shared` | Vyx atomic `Ref<T>` versus C++ `shared_ptr<T>`, validating the `1 -> 2 -> 3 -> 2 -> 1` ownership sequence across 5M allocations. |
| `type_abstraction` | 30M generic dispatches, 30M concrete trait calls, and 40M deliberately non-inlined calls through two retained runtime-selected trait objects. |
| `heavy` | Large vector sort/merge, integer and string hash maps, matrix multiplication/power, and large string build/scan/sort. |

## Reproducible Gate

From the repository root:

```powershell
$compiler = (Resolve-Path ./bootstrap_compiler/out/vyxc.exe).Path
$runtime = Split-Path -Parent $compiler
powershell -NoProfile -ExecutionPolicy Bypass -File .\benchmarks\run.ps1 `
  -BootstrapVyxc $compiler -RuntimeDir $runtime -RuntimeLib vyx_runtime `
  -Workloads baseline,core_compute,strings,generics,collections,memory_model,memory_value,memory_unique,memory_shared,type_abstraction,heavy `
  -Iterations 3 -Warmups 1 `
  -RuntimeWarmups 1 -RuntimeRuns 3 `
  -SkipHost -RequireSemanticMatch
```

Use `runtime_comparison_summary.csv` for average and median timing ratios and
`runtime_semantic_summary.csv` for correctness evidence.
