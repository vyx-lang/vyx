# Native child-process memory observation

Run `./probes/gates/pipeline_native_memory/run.ps1` from the repository root on
Windows. The runner uses `clang/bin/clang++.exe`; `-Clang` can select another
compiler. It does not rebuild the compiler or backend.

The runner compiles verbatim excerpts of `vyx_bootstrap_rt.cpp`, including the
production memory query, process map, observation APIs, polling and close. A
real child process commits and touches 64 MiB. The probe checks an increase of
at least 32 MiB, peak retention after completion, invalid and closed IDs, and
queries concurrent with handle close. The test inserts the native child handle
into the production process map; it does not test the spawn command parser.
Generated source and executable remain in the ignored `.runs` directory.

## API contract

- `vyx_bootstrap_process_resident_bytes(id)` reports the direct child's current
  resident memory in bytes. A known finished child reports `0`.
- `vyx_bootstrap_process_peak_resident_bytes(id)` reports its resident high-water
  mark, retained until `vyx_bootstrap_process_close(id)`.
- Unknown or closed IDs and unavailable observations report `-1`. An unavailable
  current observation does not erase an already recorded peak.
- Queries and close share the process-map mutex, protecting the native handle.
- Windows uses `GetProcessMemoryInfo`, dynamically resolved through the existing
  runtime helper. Linux samples `/proc/<pid>/status` (`VmRSS` and `VmHWM`) before
  polling can reap the child. Other platforms currently report unknown for a
  running child's observation. No additional link library is required.

These are **direct-child** counters, not process-tree totals. Global available
memory includes pressure from descendants; a scheduler must not add estimated
descendant usage back to available memory. Linux sampling can miss a peak if no
successful observation occurs before exit. A peak is not a future memory bound
and does not guarantee that a task cannot exhaust memory.

## Validation

The Windows runner and a full-source `clang++ -fsyntax-only` check are the local
gates. Linux implementation requires a Linux execution gate before claiming
runtime validation there.

Verified on Windows x64, 2026-09-27: full-source syntax check passed; the probe
reported `before_bytes=3751936`, `after_bytes=70864896`,
`peak_bytes=70864896`, followed by `pipeline_native_memory: OK` (exit `0`).
