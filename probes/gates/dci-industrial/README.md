# DCI industrial AOT pressure gate

`run.py` is a deterministic orchestration gate for the DCI producer/consumer
boundary.  It executes the real fixtures instead of making a synthetic API
claim:

- C++ open generics, ICU 78.3, and raw `std::vector<T>` stress;
- Rust Cargo projects (`crc32fast` and `adler2`) and the Rust trait fixture;
- the multilang and Zig ABI projects;
- storage lifetime and cross-boundary exception cleanup; and
- the offline DCI adapter/materialization/validation regression suite plus the
  existing ABI benchmark self-check.

Build the SDK compiler from current source before invoking this runner, keeping
its matching backend/runtime beside it. A Release SDK compiler may seed that
build; the tested Consumer is the freshly built SDK compiler. Producer tools,
pinned ICU files and locked Cargo registry dependencies must be available for
the corresponding cases.
The default set has 11 named cases; Linux shared propagation is a separate
`dci-exceptions/run-linux.ps1` WSL gate and is not included in the default set.
The [Qt Widgets counter](../dci-qt-counter/README.md) is a separate Windows gate
requiring a Qt SDK; it is not included in either this default set or the
`tests/projects/dci_*` scan.

The workload is reproducible with `--repeat`, `--scale`, `--parallel`, and a
fixed seed (`20261001`).  `--scale N` runs every selected case `N` times. Repeated runs of one fixture
are serialized because the fixture owns its cache and target paths; different
fixtures still run concurrently under `--parallel`. The Zig fixtures also
share a lock because Zig 0.16's bundled stdlib is not safe to read from two
producer processes at once. Regression tests for both locks live in
`tools/dci/tests/test_dci_industrial.py`. A case is never
counted as successful from a log string: exit status `0` is
`PASS`, the fixture's documented `77` is `SKIP`, and nonzero/non-`77` is
`FAIL`.  Every invocation writes stdout/stderr logs and `metrics.json` under
`probes/gates/dci-industrial/out/`.

Wall time is measured for every child.  Peak resident memory is sampled across
the child process tree when Python `psutil` is present; otherwise the JSON
contains `peak_rss_bytes: null` plus a reason.  The gate never substitutes a
zero for an unavailable measurement.

Windows example:

```powershell
pwsh -File probes/gates/dci-industrial/run.ps1 -Compiler bootstrap_compiler/out/vyxc.exe -Repeat 2 -Scale 2 -Parallel 3
```

Use `-Case dci-vector` (repeatable) to focus on one fixture while debugging.
The gate is AOT-only; JIT is deliberately outside this acceptance surface.
Its `repeat × scale` controls fixture invocations; it does not enlarge a single
fixture's internal input size. A fixture pass, a skip and a resource measurement
are separate outcomes. Repeated passing cases are evidence for those scenarios;
this runner does not certify full Core conformance, arbitrary ecosystem
coverage or industrial readiness.
