"""Runtime call-overhead microbench: DCI vs bindgen vs cxx vs native C++."""
from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

BENCH = Path(__file__).resolve().parent
sys.path.insert(0, str(BENCH))
import dci_bench as harness  # noqa: E402

ITERS = int(os.environ.get("DCI_CALLBENCH_ITERS", "2000000000"))
REPEATS = int(os.environ.get("DCI_CALLBENCH_REPEATS", "5"))
CALLBENCH = BENCH / "callbench"
OUT = BENCH / "out" / "windows"
BOOT = harness.REPO_ROOT / "bootstrap_compiler" / "out" / "boot.exe"
HEADER = "path,kernel,iters,ns_total,ns_per_call,sink"


def run(cmd: list[str], cwd: Path | None = None, env: dict | None = None) -> subprocess.CompletedProcess:
    print("+", " ".join(cmd), flush=True)
    return subprocess.run(cmd, cwd=cwd, env=env, text=True, capture_output=True)


def boot_env() -> dict:
    env = dict(os.environ)
    out = str(harness.REPO_ROOT / "bootstrap_compiler" / "out")
    if harness.LLVM_ROOT:
        env["LLVM_ROOT"] = harness.LLVM_ROOT
        llvm_bin = str(Path(harness.LLVM_ROOT) / "bin")
        env["PATH"] = out + os.pathsep + llvm_bin + os.pathsep + env.get("PATH", "")
    else:
        env["PATH"] = out + os.pathsep + env.get("PATH", "")
    for key in ("CC", "CXX", "CFLAGS", "CXXFLAGS"):
        env.pop(key, None)
    return env


def copy_dcib() -> Path:
    dest_dir = CALLBENCH / "contracts"
    dest_dir.mkdir(parents=True, exist_ok=True)
    dest = dest_dir / "abi_fixtures.dcib"
    header = harness.FIXTURES / "abi_fixtures.hpp"
    debug = dest_dir / "abi_fixtures.dci.json"
    proc = harness.dci_adapter(header, dest, debug, triplet="windows_x64")
    if proc.returncode != 0:
        raise SystemExit(proc.stdout + proc.stderr)
    return dest


def compile_native(work: Path) -> Path:
    exe = work / "native_callbench.exe"
    srcs = [
        CALLBENCH / "native" / "native_main.cpp",
        CALLBENCH / "native" / "support.cpp",
        harness.FIXTURES / "abi_fixtures.cpp",
    ]
    cmd = [harness.CLANGXX, f"-std={harness.DEFAULT_STD}", "-O3",
           f"-I{harness.FIXTURES}", f"-I{CALLBENCH / 'native'}",
           "-o", str(exe), *[str(s) for s in srcs]]
    proc = run(cmd)
    if proc.returncode != 0:
        raise SystemExit(proc.stdout + "\n" + proc.stderr)
    return exe


def cargo_release(name: str, extras: list[str], work: Path) -> Path:
    env = harness.cargo_env()
    env["CARGO_TARGET_DIR"] = str(work / "cargo-target")
    env["DCI_CALLBENCH_ITERS"] = str(ITERS)
    env["BENCH_CXX_FLAGS"] = "-O3"
    project = harness._prepare_runner(work, name, extras)
    proc = run(["cargo", "build", "--release", "--locked"], cwd=project, env=env)
    if proc.returncode != 0:
        raise SystemExit(proc.stdout + "\n" + proc.stderr)
    bin_name = name if os.name != "nt" else f"{name}.exe"
    return Path(env["CARGO_TARGET_DIR"]) / "release" / bin_name, env, project


def parse_rows(stdout: str) -> list[dict]:
    rows: list[dict] = []
    for ln in stdout.splitlines():
        if not ln or ln.startswith("path,") or "," not in ln:
            continue
        parts = ln.split(",")
        if len(parts) < 6:
            continue
        rows.append({
            "path": parts[0],
            "kernel": parts[1],
            "iters": int(parts[2]),
            "ns_total": float(parts[3]),
            "ns_per_call": float(parts[4]),
            "sink": int(parts[5]),
        })
    if not rows:
        raise SystemExit("no callbench rows in output:\n" + stdout)
    return rows


def format_row(row: dict) -> str:
    ns_total = int(round(row["ns_total"]))
    iters = row["iters"]
    per = (ns_total / iters) if iters else 0.0
    return (f"{row['path']},{row['kernel']},{iters},{ns_total},"
            f"{per:.3f},{row['sink']}")


def run_repeats(cmd: list[str], n: int, cwd: Path | None = None,
                env: dict | None = None) -> list[list[dict]]:
    repeats: list[list[dict]] = []
    for i in range(n):
        print(f"# repeat {i + 1}/{n}", flush=True)
        proc = run(cmd, cwd=cwd, env=env)
        if proc.returncode != 0:
            raise SystemExit(proc.stdout + "\n" + proc.stderr)
        repeats.append(parse_rows(proc.stdout))
    return repeats


def average_repeats(repeats: list[list[dict]]) -> list[dict]:
    keys = [(row["path"], row["kernel"]) for row in repeats[0]]
    averaged: list[dict] = []
    for path, kernel in keys:
        samples: list[dict] = []
        for run_rows in repeats:
            matched = [row for row in run_rows if row["path"] == path and row["kernel"] == kernel]
            if len(matched) != 1:
                raise SystemExit(f"expected one {path}/{kernel} row per run, got {len(matched)}")
            samples.append(matched[0])
        sinks = {sample["sink"] for sample in samples}
        iters = {sample["iters"] for sample in samples}
        if len(sinks) != 1 or len(iters) != 1:
            raise SystemExit(f"unstable sink/iters for {path}/{kernel}: sink={sinks} iters={iters}")
        ns_total = sum(sample["ns_total"] for sample in samples) / len(samples)
        sample_iters = samples[0]["iters"]
        averaged.append({
            "path": path,
            "kernel": kernel,
            "iters": sample_iters,
            "ns_total": ns_total,
            "ns_per_call": ns_total / sample_iters if sample_iters else 0.0,
            "sink": samples[0]["sink"],
        })
    return averaged


def flatten_repeats(repeats: list[list[dict]]) -> list[str]:
    lines = ["repeat,path,kernel,iters,ns_total,ns_per_call,sink"]
    for i, run_rows in enumerate(repeats, start=1):
        for row in run_rows:
            ns_total = int(round(row["ns_total"]))
            lines.append(
                f"{i},{row['path']},{row['kernel']},{row['iters']},"
                f"{ns_total},{row['ns_per_call']:.3f},{row['sink']}"
            )
    return lines


def main() -> int:
    if REPEATS < 1:
        raise SystemExit("DCI_CALLBENCH_REPEATS must be >= 1")
    print(f"ITERS={ITERS} REPEATS={REPEATS}", flush=True)
    OUT.mkdir(parents=True, exist_ok=True)
    work = OUT / "callbench_work"
    work.mkdir(parents=True, exist_ok=True)
    copy_dcib()

    all_repeats: list[list[dict]] = []
    averaged: list[dict] = []

    native = compile_native(work)
    env = dict(os.environ)
    env["DCI_CALLBENCH_ITERS"] = str(ITERS)
    native_runs = run_repeats([str(native)], REPEATS, env=env)
    all_repeats.extend(native_runs)
    averaged.extend(average_repeats(native_runs))

    if not BOOT.exists():
        raise SystemExit(f"boot not found: {BOOT}")
    proc = run([str(BOOT), "build", "-j", "10", "-O3"], cwd=CALLBENCH, env=boot_env())
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout + "\n" + proc.stderr)
        return proc.returncode
    vyx_exe = CALLBENCH / "target" / "dci_callbench.exe"
    if not vyx_exe.exists():
        vyx_exe = CALLBENCH / "target" / "dci_callbench"
    dci_runs = run_repeats([str(vyx_exe)], REPEATS, env=boot_env())
    all_repeats.extend(dci_runs)
    averaged.extend(average_repeats(dci_runs))

    os.environ["CARGO_TARGET_DIR"] = str(work / "cargo-target")
    bindgen_bin, benv, bproj = cargo_release(
        "bindgen_runner",
        ["abi_fixtures.hpp", "abi_fixtures.cpp", "abi_fixtures_c.h", "abi_fixtures_c.cpp"],
        work)
    bindgen_runs = run_repeats([str(bindgen_bin)], REPEATS, cwd=bproj, env=benv)
    all_repeats.extend(bindgen_runs)
    averaged.extend(average_repeats(bindgen_runs))

    cxx_bin, cenv, cproj = cargo_release(
        "cxx_runner", ["abi_fixtures.hpp", "abi_fixtures.cpp"], work)
    cxx_runs = run_repeats([str(cxx_bin)], REPEATS, cwd=cproj, env=cenv)
    all_repeats.extend(cxx_runs)
    averaged.extend(average_repeats(cxx_runs))

    avg_text = HEADER + "\n" + "\n".join(format_row(row) for row in averaged) + "\n"
    out_csv = OUT / "callbench_o3.csv"
    out_csv.write_text(avg_text, encoding="utf-8")
    raw_csv = OUT / "callbench_o3_repeats.csv"
    raw_csv.write_text("\n".join(flatten_repeats(all_repeats)) + "\n", encoding="utf-8")
    print(avg_text, end="")
    print(f"wrote {out_csv} (mean of {REPEATS} runs)")
    print(f"wrote {raw_csv}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
