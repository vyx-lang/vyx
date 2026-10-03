# Industrial Vyx compiler pressure gate

This gate exercises the AOT driver on a deterministic, generated Vyx project.
It keeps the source seed and scale in `fixture.json`, then measures cold, warm
and one-file incremental builds at `-j1` and `-jN`. Each build records wall
clock, process-tree working set, scheduler concurrency, compile task count,
object cache hashes, executable semantics, and the scheduler reserve watermark.
It is an AOT correctness and work-accounting gate; its name does not certify that
the compiler has met every industrial workload or performance target.

The generated project contains:

- 64 (or the requested scale) independent modules with repeated arithmetic
  bodies, so the scheduler has real fanout and the frontend sees large input.
- A generic `Cell<T>` module and a trait/interface dispatch module. The gate
  emits a versioned template interface and requires a separate consumer to
  compile and execute successfully.
- A deliberately invalid source checked separately for a structured diagnostic.

Run from the repository root with the SDK compiler built from current source
and its matching backend/runtime:

```powershell
./probes/gates/compiler-industrial/run.ps1 `
  -Compiler ./bootstrap_compiler/out/vyxc.exe `
  -Scale 64 -Jobs 20
```

The command returns nonzero when a warm build recompiles work, when the
incremental build does not invalidate the edited unit, when `-jN` has no
observable scheduler fanout, when the scheduler reserve watermark is not
`3221225472`, when cache artifacts differ between cold and warm runs, when
semantics or the cross-module generic consumer fail, or when the negative source is accepted without an `E####`/
`error` diagnostic. Generated runs live under the selected output
folder, which defaults to Git-ignored `.runs/`. The harness refuses to overwrite
an existing output directory. The incremental edit adds a deterministic comment
to one source: it checks invalidation/work accounting without changing semantics.

`report.json` contains the matrix and failures; each project retains stdout,
stderr, combined logs, run logs, and `fixture.json`. `-Seed` defaults to `424242`;
`-Scale` accepts 8–1024 leaf modules and `-Jobs` accepts 1–256. The default 64/20
command above is a reproducible larger run, not a claim that it has passed in
every environment. The report currently records the compiler path; retain its
SHA-256, source commit, LLVM/runtime identity, and environment alongside it.

## Recorded smoke matrix — 2026-10-01

Windows x64, repository LLVM 22, seed `424242`, **8 leaf modules**, `-Jobs 4`:

The example uses the current SDK compiler entry point; the table retains the
recorded historical observations.

```powershell
./probes/gates/compiler-industrial/run.ps1 `
  -Compiler ./bootstrap_compiler/out/vyxc.exe -Scale 8 -Jobs 4
```

| Run | Wall seconds | Compile tasks | Scheduler active peak | Build/run exit |
|---|---:|---:|---:|---|
| j1 cold | 4.645 | 22 | 1 | 0 / 0 |
| j1 warm | 0.480 | 0 | 0 | 0 / 0 |
| j1 incremental | 0.925 | 2 | 1 | 0 / 0 |
| j4 cold | 1.654 | 22 | 4 | 0 / 0 |
| j4 warm | 0.578 | 0 | 0 | 0 / 0 |
| j4 incremental | 1.017 | 2 | 1 | 0 / 0 |

Both warm object digests matched their cold run; negative diagnostics and the
separate generic consumer passed. Both cold runs reported a positive
`reserve_bytes=3221225472`, covering the scheduler's former `i32` overflow in the
3 GiB reserve. Evidence: `.runs/industrial-20261001-060001-058/report.json`.
This retained matrix predates the final long-chain artifact follow-up in
`61d31e51`; the report does not carry a compiler hash, so it must not be assigned
the later artifact fixed-point hash.

These are single observations, not medians or a controlled before/after
comparison. Process-tree working-set sampling may miss short-lived compiler
processes and counts shared pages more than once. A warm sample of zero is not
proof of zero memory use. Scheduler concurrency is distinct from simultaneous
LLVM lowering or sustained CPU utilization. Repeat and enlarge the workload
before drawing large-project performance conclusions.
