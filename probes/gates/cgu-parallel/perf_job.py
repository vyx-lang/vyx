"""Wall-clock + peak-private-RSS capture for ONE compiler job.

`measure.py` measures `boot build` on a whole project; that hides the fact that
a self-host build is dominated by a single unity job (`.cache/build_task_times.tsv`
records ~620 s for `crate_boot.obj` while every other job is under 9 s).  This
script runs one job command directly so individual knobs can be compared without
paying for a full build.

Usage:
  python probes/gates/cgu-parallel/perf_job.py --label lazy --out out/perfjobs/lazy.log
  python probes/gates/cgu-parallel/perf_job.py --label off --env VYX_CGU_PARALLEL=off

The job is the exact command the build system issues for the self-host crate
(see `[boot-cmd]` / Win32_Process CommandLine of a running build).  Run it from
`bootstrap_compiler/`.
"""
from __future__ import annotations

import argparse
import ctypes
import os
import subprocess
import sys
import time
from ctypes import wintypes
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

# The build system compiles the whole self-host crate as one unity object.
DEFAULT_JOB = [
    "out/boot.exe", "--emit=obj", "--src=file", "src/core/main.vyx",
    "-o", ".cache/perfjob.obj",
    "-L", "out", "-l", "vyx_compiler_backend", "-l", "vyx_runtime",
    "-l", "synchronization", "-O2",
    "--project-unit-sources", ".cache/unit_crate_boot.sources",
]


class PROCESS_MEMORY_COUNTERS_EX(ctypes.Structure):
    _fields_ = [
        ("cb", wintypes.DWORD),
        ("PageFaultCount", wintypes.DWORD),
        ("PeakWorkingSetSize", ctypes.c_size_t),
        ("WorkingSetSize", ctypes.c_size_t),
        ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
        ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
        ("PagefileUsage", ctypes.c_size_t),
        ("PeakPagefileUsage", ctypes.c_size_t),
        ("PrivateUsage", ctypes.c_size_t),
    ]


def counters(pid: int) -> tuple[int, int]:
    """(private bytes now, peak private bytes) for `pid`."""
    psapi = ctypes.WinDLL("psapi")
    k32 = ctypes.WinDLL("kernel32")
    handle = k32.OpenProcess(0x0400 | 0x0010, False, pid)
    if not handle:
        return (0, 0)
    try:
        c = PROCESS_MEMORY_COUNTERS_EX()
        c.cb = ctypes.sizeof(c)
        if not psapi.GetProcessMemoryInfo(handle, ctypes.byref(c), c.cb):
            return (0, 0)
        return (int(c.PrivateUsage), int(c.PeakPagefileUsage))
    finally:
        k32.CloseHandle(handle)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--label", required=True)
    ap.add_argument("--out", default="")
    ap.add_argument("--sample-ms", type=int, default=500)
    ap.add_argument("--env", action="append", default=[])
    ap.add_argument("--project", default=str(ROOT / "bootstrap_compiler"))
    ap.add_argument("job", nargs="*", default=[])
    args = ap.parse_args()

    proj = Path(args.project)
    job = args.job if args.job else list(DEFAULT_JOB)
    # CreateProcess resolves argv[0] against the *parent's* directory, not `cwd`,
    # so a relative `out/boot.exe` has to be made absolute here.
    head = Path(job[0])
    if not head.is_absolute() and ("/" in job[0] or "\\" in job[0]):
        job[0] = str(proj / head)
    out_log = Path(args.out) if args.out else proj / ".cache" / f"perfjob_{args.label}.log"
    out_log.parent.mkdir(parents=True, exist_ok=True)

    env = os.environ.copy()
    env["LLVM_ROOT"] = str(ROOT / "clang").replace("\\", "/")
    if "VYX_PHASE_SUMMARY" not in env:
        env["VYX_PHASE_SUMMARY"] = "1"
    for kv in args.env:
        k, _, v = kv.partition("=")
        env[k] = v

    t0 = time.perf_counter()
    with out_log.open("wb") as fh:
        proc = subprocess.Popen(job, cwd=str(proj), env=env,
                                stdout=fh, stderr=subprocess.STDOUT)
        # Peak private is read straight out of the kernel counters, so the
        # sampling interval cannot miss the peak the way a WorkingSet sample can.
        samples = []
        while True:
            cur, _ = counters(proc.pid)
            if cur:
                samples.append(cur)
            if proc.poll() is not None:
                break
            time.sleep(args.sample_ms / 1000.0)
    wall = time.perf_counter() - t0
    rc = proc.returncode

    text = out_log.read_text(encoding="utf-8", errors="replace")
    lines = [ln.rstrip() for ln in text.splitlines()
             if ln.startswith("[llvm-cgu]") or ln.startswith("[mir-timing]")
             or ln.startswith("[llvm-owned-body]")]

    summary = [
        f"label={args.label}",
        f"rc={rc}",
        f"wall_s={wall:.1f}",
        f"peak_private_mb={round(max(samples, default=0) / (1024 * 1024), 1)}",
        f"final_private_mb={round(samples[-1] / (1024 * 1024), 1) if samples else 0}",
        f"log={out_log}",
    ]
    print("\n".join(summary))
    for ln in lines:
        print("  " + ln)
    return 0 if rc == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
