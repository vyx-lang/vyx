"""R0 wall-clock + RSS capture for industrial_mir_stress / the local fixture.

Usage:
  python probes/gates/cgu-parallel/measure.py
  python probes/gates/cgu-parallel/measure.py --project tests/projects/industrial_mir_stress
"""
from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def boot_path() -> Path:
    env = os.environ.get("BOOT")
    if env:
        p = Path(env)
        if p.exists():
            return p
    for cand in (
        ROOT / "bootstrap_compiler" / "out" / "boot.exe",
        ROOT / "bootstrap_compiler" / "out" / "boot",
    ):
        if cand.exists():
            return cand
    raise SystemExit("missing bootstrap_compiler/out/boot.exe")


def sha256_file(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def peak_private_bytes(pid: int) -> int:
    if sys.platform != "win32":
        return 0
    import ctypes
    from ctypes import wintypes

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

    psapi = ctypes.WinDLL("psapi")
    k32 = ctypes.WinDLL("kernel32")
    PROCESS_QUERY_INFORMATION = 0x0400
    PROCESS_VM_READ = 0x0010
    handle = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    if not handle:
        return 0
    try:
        counters = PROCESS_MEMORY_COUNTERS_EX()
        counters.cb = ctypes.sizeof(counters)
        if not psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb):
            return 0
        return int(counters.PrivateUsage)
    finally:
        k32.CloseHandle(handle)


def copy_project(src: Path, dest: Path) -> None:
    dest.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src / "Vyx.toml", dest / "Vyx.toml")
    if (src / "src").is_dir():
        shutil.copytree(src / "src", dest / "src", dirs_exist_ok=True)


def run_one(compiler: Path, project: Path, label: str, env_extra: dict[str, str],
            out_root: Path) -> dict:
    dest = out_root / ("proj_" + label)
    if dest.exists():
        shutil.rmtree(dest)
    copy_project(project, dest)
    log = out_root / f"build_{label}.log"
    env = os.environ.copy()
    env["LLVM_ROOT"] = str(ROOT / "clang")
    env.update(env_extra)
    t0 = time.perf_counter()
    proc = subprocess.Popen(
        [str(compiler), "build"],
        cwd=str(dest),
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    peak = 0
    while True:
        cur = peak_private_bytes(proc.pid)
        if cur > peak:
            peak = cur
        if proc.poll() is not None:
            break
        time.sleep(0.05)
    out, _ = proc.communicate()
    wall = time.perf_counter() - t0
    text = out.decode("utf-8", errors="replace")
    log.write_text(text, encoding="utf-8")
    hashes = {}
    cache = dest / ".cache"
    if cache.is_dir():
        for p in cache.rglob("*"):
            if p.is_file() and (
                p.name.endswith(".obj")
                or ".cgu." in p.name
                or p.name.endswith(".cgu.list")
            ):
                hashes[str(p.relative_to(dest)).replace("\\", "/")] = sha256_file(p)
    summary = [ln.strip() for ln in text.splitlines() if ln.startswith("[llvm-cgu]")]
    return {
        "label": label,
        "exit": proc.returncode,
        "wall_s": round(wall, 3),
        "peak_private_mb": round(peak / (1024 * 1024), 1),
        "hashes": hashes,
        "summary": summary,
        "log": str(log),
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--project", default=str(ROOT / "tests" / "projects" / "industrial_mir_stress"))
    ap.add_argument("--out", default=str(ROOT / "out" / "cgu_r0_measure"))
    args = ap.parse_args()
    compiler = boot_path()
    project = Path(args.project)
    out_root = Path(args.out)
    out_root.mkdir(parents=True, exist_ok=True)

    plan = [
        ("off", {"VYX_PHASE_SUMMARY": "1", "VYX_CGU_PHASES": "1", "VYX_CGU_PARALLEL": "off", "VYX_CGU_THREADS": "0"}),
        ("lazy", {"VYX_PHASE_SUMMARY": "1", "VYX_CGU_PHASES": "1", "VYX_CGU_PARALLEL": "lazy", "VYX_CGU_THREADS": "2"}),
        ("full", {"VYX_PHASE_SUMMARY": "1", "VYX_CGU_PHASES": "1", "VYX_CGU_PARALLEL": "full", "VYX_CGU_THREADS": "2"}),
    ]
    rows = []
    for label, extra in plan:
        print(f"[run] {label}", flush=True)
        rows.append(run_one(compiler, project, label, extra, out_root))

    report = out_root / "report.txt"
    lines = [
        f"compiler={compiler}",
        f"project={project}",
        f"size={compiler.stat().st_size}",
        "",
    ]
    for r in rows:
        lines.append(
            f"{r['label']}: exit={r['exit']} wall={r['wall_s']}s peak_private={r['peak_private_mb']}MiB artefacts={len(r['hashes'])}"
        )
        for s in r["summary"]:
            lines.append("  " + s)
        lines.append("")
    report.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(report.read_text(encoding="utf-8"))
    if any(r["exit"] != 0 for r in rows):
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
