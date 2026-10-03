# Build scheduler helper regression

Run from the repository root on Windows:

```powershell
./probes/gates/pipeline_scheduler/run.ps1 -Compiler ./bootstrap_compiler/out/vyxc.exe
```

This is a **unit extraction gate**. The runner extracts current production
functions and their `build_*` dependencies verbatim from
`bootstrap_compiler/src/core/build_system.vyx`, `build_action.vyx`, and
`build_resources.vyx`, appends Vyx assertions, compiles
them with the SDK compiler built from current source, and executes the result.
It does not emulate the implementation in PowerShell or Python. The script only creates fixtures,
whose expected queue order is fixed by their construction.

The current sorting gate uses the production `BuildAction` class and
`build_action_launch_order`. Its test adapter constructs typed actions from
fixture rows and serializes the selected IDs solely for comparison. Sentinel
state/telemetry values prove ordering does not mutate the action records. The
test does not retain or emulate the removed text-based production scheduler.

The runner also compiles the original cache helpers from commit `8e44b876`,
showing that the baseline accepts a wrong record with an equal-length hash
collision. `-BaselineRevision` selects another revision with the old three-arg
cache helper; `-SkipBaseline` skips that historical reproduction. The compiler
must be accompanied by its matching `vyx_compiler_backend` because the extracted
production path/env helpers use native runtime services.

## Coverage

- Cache: `Aa` and `B@` have equal-length/djb2 digests; only an exact record may
  match. Includes CRLF, last line without newline, multiple collision
  candidates, missing objects, empty cache, partial records and one short
  record.
- Cache merge: replace existing entries, append new keys, preserve unreplaced
  entries and their order, keep the first update for duplicate keys, handle
  empty inputs, and retain both short and long output after the builder owner
  is destroyed.
- Queue: 96 tasks (above the old 64-task cutoff), descending measured/file-size
  cost, stable order for equal cost, empty/single/two-task queues, and unchanged
  ordering when equal-size sources are renamed away from former privileged
  compiler filenames. Both timing-history and disable-prioritization settings
  are exercised.
- Memory arithmetic: unavailable RSS gives no credit; observed RSS is capped by
  its reservation; current free memory can recover; observed active RSS is not
  charged twice; physical and commit headroom independently constrain admission;
  manual limits remain caps; unknown headroom cannot invent memory. Byte-level
  admission handles sub-GiB budgets and avoids overflowing reservation sums.
- String lifetime: builder output is detached using `owned.raw().clone()` before
  destroying its owner. Tests 0, 1, 23, 24, 63 and 64 bytes, then overwrites the
  helper's stack through another call. The short cache and short queue cases
  additionally test production cache return paths and the typed assertion adapter.

The lifetime checks caught a preexisting implicit `String` → borrowed `string`
conversion escaping stack-backed SSO storage. This gate retains those checks;
padding or inflating capacities is not a valid fix.

These assertions validate helper behavior, not process-tree memory protection,
end-to-end throughput, or global thread utilization. Those require instrumented
real builds. This gate does not promise OOM prevention for an unknown task.

## Verified result

Windows x64, 2026-09-27, with native self-host compiler SHA-256
`5D55754D8E1C3784E3E6B2BF75A9DFCDF94A9578A80B93A80282B4E5D4FF44A1`
and backend `3AA164E435418DF61004DBA0939C0687DE4CCF1A6FD41B3B0DE7C3346FE98A11`:

```text
baseline cache collision reproduced
pipeline_scheduler: OK ownership 0/1/23/24/63/64
pipeline_scheduler: OK cold
pipeline_scheduler: OK history
pipeline_scheduler: OK disabled
```

All compiles and runs exited `0`. Generated Vyx, compile logs, fixtures and
executables remain under ignored `.runs/`. The runner restores its environment
variables and working directory. Latest typed helper extraction run:
`.runs/20260927-170947-155` (33 current production functions). This includes the
production `build_apply_task_times` path that reads timing history once and
indexes output identities before sorting. The earlier
`.runs/20260927-165748-050` run used 35 functions and compiler `70731A1F...` before
that history-index change.
