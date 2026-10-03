# Typed action batch lifecycle gate

This Windows gate executes the **actual production batch scheduler** against
real native child processes. It extracts `build_run_action_batch`, its transitive
`build_*` / `policy_*` dependencies, and the `BuildAction` model verbatim. It links
the compiled Vyx probe to the compiler's matching native backend. Scheduling,
process polling, cancellation, resource admission, and artifact publication are
not reimplemented in the test.

The test constructor supplies 40 typed actions, each reserving 8 MiB for a small
native fixture child. This reservation describes the fixture; it is not a claim
that compiler workers use 8 MiB. The fixture child sleeps, writes a temporary
output, optionally spawns a real descendant, and can exit with code 17.

```powershell
pwsh -NoProfile -File probes/gates/build_actions/run.ps1 -Compiler bootstrap_compiler/out/vyxc.exe
# Compile the extracted gate, but defer actual process tests:
pwsh -NoProfile -File probes/gates/build_actions/run.ps1 -Compiler bootstrap_compiler/out/vyxc.exe -PrepareOnly
pwsh -NoProfile -File probes/gates/build_actions/run.ps1 -PreparedDir '<printed-directory>'
```

Select the SDK compiler built from current source at
`bootstrap_compiler/out/vyxc.exe` with `-Compiler`. It must be beside its
matching backend/runtime. The runner compiles the native fixture with the
repository's `clang/bin/clang++`.
Each run has a private working directory and cannot reuse prior case outputs.
Compilation and execution logs, original extracted Vyx, source hashes, compiler
and backend hashes, per-child timestamps/PIDs, and final JSON remain in `.runs/`.

The four cases are:

| Case | Jobs | Assertions |
|---|---:|---|
| Direct child success | 1 | All 40 actions complete; exactly one active token and one overlapping child lifetime |
| Direct child success | 20 | All 40 actions complete; 20 active tokens and 20 overlapping child lifetimes |
| Launcher exits before descendant | 20 | Token survives direct-process exit; publication waits for descendant output; all 40 results are valid |
| Failed launcher with live descendants | 20 | Exit 17 remains failed; 19 running siblings are cancelled; remaining 20 actions do not launch; no root or descendant survives |

Successful actions must retain measured memory/CPU counters, close their process
IDs, publish expected output content, and remove the temporary file by rename.
The failure case starts with a known-good `action_0.obj`: the failed temporary
output must not replace it. Cancelled/pending actions must not publish outputs.
The PowerShell runner checks every recorded PID and verifies cancelled descendants
did not finish their delayed write. Cleanup, if necessary, only stops recorded
PIDs whose executable path still matches this run's fixture child.

The gate deliberately uses a small reserve appropriate for its fixture and
enables production trace logging. It tests lifecycle correctness and token
accounting, not the compiler's estimate accuracy or real-project build speed.
Run it separately from throughput benchmarks.

## Reproduced pre-fix failure

The first prepared gate used self-host compiler
`5D55754D8E1C3784E3E6B2BF75A9DFCDF94A9578A80B93A80282B4E5D4FF44A1`.
The direct-child `jobs=1` case crashed after several successful publications:

```text
exit -1073741819
[vyx-av] access=0 addr=FFFFFFFFFFFFFFFF ... rva=0x2e372 ... batch.exe
```

Evidence: `.runs/lifecycle-20260927-170519-879/success-j1/stderr.log` and its
`.cache/build_trace.log`. The production scheduler and `BuildAction` were compiled
in the same probe file, so this reproduction does not depend on cross-module
class lookup. Root-cause work identified a mismatch between the native checked
pointer header and the runtime layout used by generated inline checks. Native
layout correction and passing lifecycle results are separate acceptance gates.

## Passing native ABI correction gate

After rebuilding the backend with the canonical 56-byte checked-pointer header,
the compiler remained `5D55754D...`. Backend SHA-256 became
`3AA164E435418DF61004DBA0939C0687DE4CCF1A6FD41B3B0DE7C3346FE98A11`.
The gate passed all four cases on 2026-09-27:

| Case | Wall | Launched | Active token peak | Actual child overlap | Recorded roots/descendants | Survivors |
|---|---:|---:|---:|---:|---:|---:|
| success, j1 | 9.918 s | 40 | 1 | 1 | 40 | 0 |
| success, j20 | 0.555 s | 40 | 20 | 20 | 40 | 0 |
| tree success, j20 | 0.864 s | 40 | 20 | 20 | 80 | 0 |
| expected failure, j20 | 0.937 s | 20 | 20 | not measured | 40 | 0 |

All gate exit codes were 0, including the negative case: the latter means the
probe correctly observed and validated the production batch failure, not that
the child failure was accepted as success. Its remaining 20 actions never
launched, the previous-good output survived, and no failed/cancelled artifact was
published. Artifact directory: `.runs/lifecycle-20260927-170915-265`; results and
hashes are in `results.json` and `extraction.json`.

The wall times contain deliberate child sleeps and process-launch overhead.
They establish scheduler behavior, not compiler speedup.


## Measured memory admission

`BuildAction.memory_bytes` now uses bytes, not rounded GiB slots or an output
name such as `rootchunk_0`. The planner supplies the size of the actual source
unit in its task record. Cold model v1 reserves 512 MiB plus 4096 bytes per
source byte for codegen; other tools reserve 256 MiB plus 128 bytes per source
byte. These are explicit startup/expansion priors informed by the retained
self-host observations, **not hard limits or calibrated guarantees**. Imported
interfaces, headers and generic expansion can exceed them.

Each successful published action stores one bounded `.vyx-cost` sidecar with
its full compiler identity, command and input byte count, plus actual peak
resident/tree-private counters. A matching complete key uses the larger peak
plus 25 percent headroom. A missing, truncated or different-key cost record
uses the cold prior; it never makes an object cache valid. During execution,
observed growth raises a live reservation before more tasks are admitted.
`VYX_BUILD_NO_COST_HISTORY=1` disables learning for controlled cold-model tests.
An estimate cannot stop already launched jobs from growing simultaneously;
shared frontend state and body ownership remain required to bound total cost.

The final lifecycle rerun before whole-compiler validation is retained in
`.runs/lifecycle-20260927-173600-290/`; all four cases pass, including 20-way
success and no surviving descendants after failure. CPU saturation on real
compiler inputs is measured separately.
