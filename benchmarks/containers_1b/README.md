# Vec / Dict / String 1B benchmark

This suite compares the standard Vec, Dict, and String containers in Vyx,
C++26 (`std::vector`, `std::unordered_map`, `std::string`), and Rust
(`Vec`, `HashMap`, `String`). CMake builds C++; Cargo builds Rust; the
bootstrap compiler builds Vyx against the canonical runtime library.

The default workload is 1,000,000,000 operations with a 1,000,000-element
live set and a 1 MiB string bound. Each operation consumes the same three
draws from a 31-bit LCG in every language. The cases are:

- Vec: push, pop, indexed read, and indexed update.
- Dict: insert/update, lookup, remove, and contains.
- String: bounded byte append, byte read, and clear.

Each case runs in a fresh process. The runner rotates language order and
rejects a round unless all three implementations produce the same checksum
and final size. `-Profile matched-o2` is the cross-language comparison mode;
`-Profile native-release` is a separate stronger-optimization experiment.

The primary time metric is runner-measured process wall time, including setup
and process startup/exit. Peak RSS is sampled by the runner, with a native
self-report fallback. Results are written to `out/results.tsv`, medians to
`out/summary.tsv`, and toolchain/build identity to `out/environment.json`.

Run a controlled smoke test:

```powershell
powershell -ExecutionPolicy Bypass -File benchmarks/containers_1b/run.ps1 `
  -Workload 1000000 -LiveSet 100000 -StringBytes 65536 -Profile matched-o2 `
  -Runs 3 -Warmups 1
```

Run the full 1B-operation comparison:

```powershell
powershell -ExecutionPolicy Bypass -File benchmarks/containers_1b/run.ps1 `
  -Profile matched-o2 -Runs 3 -Warmups 1
```

The 1B value is the operation count, not the resident element count. Set
`-LiveSet 1000000000` only for an intentional memory-capacity experiment.
