# Managed build-process resource observations

This gate loads the real `vyx_compiler_backend.dll`, uses its exported managed
process launcher, and measures a real root process and descendant. It does not
substitute implementations or build the compiler.

```powershell
pwsh -File probes/gates/build_resources/run.ps1
```

The default runtime is `bootstrap_compiler/out/vyx_compiler_backend.dll`; use
`-Runtime` to select another matching build. The script compiles one small C++
consumer using the repository's LLVM SDK. Evidence, logs and the runtime SHA-256
are emitted under `.runs/` (ignored).

## API contract

All child queries take an `i64` managed process ID returned by
`vyx_bootstrap_process_spawn`, not an OS PID. Memory is `i64` bytes. CPU is `i64`
microseconds of cumulative kernel plus user execution, not wall time or a CPU
percentage. Invalid/closed IDs and unsupported measurements return `-1`.
Queries hold the process-map mutex through the OS call, including against close.

| Export | Scope and meaning |
| --- | --- |
| `vyx_bootstrap_process_private_bytes` | Direct child's current private commit. On Windows a root known to have exited has zero current usage. |
| `vyx_bootstrap_process_peak_private_bytes` | Direct child's observed OS commit high-water mark, retained until close. |
| `vyx_bootstrap_process_cpu_time_us` | Direct child's cumulative CPU, queryable after exit until close. |
| `vyx_bootstrap_process_tree_private_bytes` | Current commit of the per-child Windows JobObject, including descendants. Root completion does not end this observation. |
| `vyx_bootstrap_process_tree_peak_private_bytes` | OS job commit high-water mark, retained until close. |
| `vyx_bootstrap_process_tree_cpu_time_us` | Cumulative CPU of the job including exited processes. |
| `vyx_bootstrap_process_tree_active_count` | Live processes in the job, including the root. Zero is a known complete tree; `-1` is unknown. |
| `vyx_rt_system_available_commit_bytes` | System commit limit minus current system commit, measured independently of physical memory. |
| `vyx_rt_system_available_physical_bytes` | Existing system physical availability observation. |

Existing direct-child `process_resident_bytes` and
`process_peak_resident_bytes` exports keep their RSS semantics. Private commit
and RSS are separate measurements; neither is an alias for the other. The new
private/CPU/tree APIs currently support Windows and explicitly return `-1` on
other platforms. POSIX system commit likewise returns `-1`; its previous
physical-memory approximation has been removed. Physical availability remains
available independently. The scheduler must decide how to constrain a platform
with only one known budget, without interpreting unknown as zero usage.

Current tree commit uses the OS `JobObjectMemoryUsageInformation` query used by
[Microsoft hcsshim](https://github.com/microsoft/hcsshim/blob/main/internal/winapi/jobobject.go).
Windows SDK headers do not declare this information class. The named local ABI
declaration is limited to the native observation layer. Unsupported OS queries
return `-1`; they never silently become direct-root observations. The public
`JobObjectExtendedLimitInformation` query can still provide a valid tree peak
when current job accounting is unavailable. Disabled job objects produce `-1`
for every tree observation while direct-process observations remain available.

Windows can retain a small commit charge for terminated process/job bookkeeping
while handles remain open. A complete tree therefore need not report exactly
zero commit. Completion must use `tree_active_count == 0`, independently of
memory, and must never be inferred from a root exit or low memory usage.

System commit uses `GetPerformanceInfo`, resolved dynamically from kernel32 or
psapi; no new link library is required. `GlobalMemoryStatusEx::ullAvailPageFile`
is not used as system commit because it can also reflect the calling process's
limit. The observations do not themselves impose a hard allocation limit.

## Assertions

The test holds 16 MiB in a root, adds 32 MiB, and starts a descendant holding
64 MiB. It verifies the allocation growth, separate direct/tree commit and CPU,
known root completion with a still-live descendant, job completion and released
allocation after the descendant exits, repeated final peak/CPU reads, and concurrent queries against
close. A separate fresh process disables job objects and verifies that tree
queries are unknown while direct observations still work. Both runs check all
resource APIs against invalid and closed IDs. Peak explicit allocations are
112 MiB; this is a correctness probe, not a compiler performance benchmark.
