#!/usr/bin/env python3
"""Industrial DCI AOT pressure runner.

This runner composes the real C++/Rust DCI fixtures and the required project
fixtures into a repeatable workload.  It deliberately measures the process
instead of printing hand-written benchmark numbers: wall time is always
recorded, while peak RSS is sampled when ``psutil`` is installed and is
explicitly ``null`` otherwise.  A result is only ``PASS`` when the child
returned zero (or the fixture's documented ``77`` skip status).

Examples::

    python probes/gates/dci-industrial/run.py --repeat 2 --scale 2 --parallel 3
    python probes/gates/dci-industrial/run.py --case dci-vector --timeout-sec 900

``scale`` multiplies the deterministic number of iterations.  Fixtures keep
their own inputs and caches isolated; the runner never mutates a contract or
source to manufacture a larger result.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import os
import platform
import shutil
import signal
import subprocess
import sys
import threading
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Sequence


SEED = 20261001
SKIP_EXIT = 77
ROOT = Path(__file__).resolve().parents[3]
OUT = Path(__file__).resolve().parent / "out"
_CASE_LOCKS: dict[str, threading.Lock] = {}


@dataclass(frozen=True)
class Case:
    name: str
    command: tuple[str, ...]
    windows_only: bool = False
    resource_key: str | None = None


@dataclass
class Result:
    name: str
    iteration: int
    command: list[str]
    status: str
    exit_code: int | None
    wall_seconds: float
    peak_rss_bytes: int | None
    peak_rss_status: str
    stdout: str
    stderr: str
    output_sha256: str


def _python() -> str:
    return sys.executable


def _powershell() -> str | None:
    return shutil.which("pwsh") or shutil.which("powershell")


def _fixture(path: str) -> Path:
    return ROOT / path


def _ps_case(name: str, path: str, compiler: str) -> Case:
    shell = _powershell()
    # A missing PowerShell executable is represented by a harmless command;
    # run_case turns this into a documented SKIP rather than a fake pass.
    if shell is None:
        return Case(name, ("__missing_powershell__",), windows_only=True)
    compiler_param = "BootstrapCompiler" if path.startswith("tests/projects/") else "Compiler"
    # Zig 0.16 reads its bundled stdlib through shared files and can report a
    # transient truncated bpf.zig when two real fixtures invoke it together.
    # Serialize that toolchain while retaining parallelism for unrelated APIs.
    resource_key = None
    if path in {
        "tests/projects/dci_multilang/run.ps1",
        "tests/projects/dci_zig_abi/run.ps1",
    }:
        resource_key = "zig-toolchain"
    elif path == "probes/gates/dci-exceptions/run.ps1":
        # The exception gate launches many boot instances and links shared
        # runtime artifacts. Keep it out of the long adapter-unit process so a
        # Windows loader failure cannot be misreported as an ABI regression.
        resource_key = "boot-runtime"
    return Case(
        name,
        (
            shell,
            "-NoProfile",
            "-NonInteractive",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            str(_fixture(path)),
            f"-{compiler_param}",
            compiler,
        ),
        windows_only=True,
        resource_key=resource_key,
    )


def cases(compiler: str) -> list[Case]:
    py = _python()
    return [
        Case(
            "adapter-unit",
            (
                py,
                "-m",
                "unittest",
                "tools.dci.tests.test_dci_adapter_msvc",
                "tools.dci.tests.test_cpp_adapter_multiabi",
                "tools.dci.tests.test_cpp_native_materialize",
                "tools.dci.tests.test_dci_validate",
                "tools.dci.tests.test_dci_consumer",
                "tools.dci.tests.test_dci_adapter_rust",
                "tools.dci.tests.test_dci_bench",
                "-q",
            ),
            resource_key="boot-runtime",
        ),
        Case("dci-bench-self-check", (py, "tools/dci/bench/dci_bench.py", "self-check")),
        _ps_case("dci-cpp-trait", "tests/projects/dci_cpp_trait/run.ps1", compiler),
        _ps_case("dci-rust-trait", "tests/projects/dci_rust_trait/run.ps1", compiler),
        _ps_case("dci-multilang", "tests/projects/dci_multilang/run.ps1", compiler),
        _ps_case("dci-zig-abi", "tests/projects/dci_zig_abi/run.ps1", compiler),
        _ps_case("dci-vector", "probes/gates/dci-vector/run.ps1", compiler),
        _ps_case("dci-cpp-ecosystem", "probes/gates/dci-cpp-ecosystem/run.ps1", compiler),
        _ps_case("dci-rust-ecosystem", "probes/gates/dci-rust-ecosystem/run.ps1", compiler),
        _ps_case("dci-storage", "probes/gates/dci-storage/run.ps1", compiler),
        _ps_case("dci-exceptions", "probes/gates/dci-exceptions/run.ps1", compiler),
    ]


def _tree_rss(process) -> int | None:
    """Return sampled RSS for a process tree, or None if psutil is absent."""
    try:
        import psutil  # type: ignore
    except ImportError:
        return None
    try:
        total = process.memory_info().rss
        for child in process.children(recursive=True):
            try:
                total += child.memory_info().rss
            except (psutil.NoSuchProcess, psutil.AccessDenied):
                continue
        return int(total)
    except (psutil.NoSuchProcess, psutil.AccessDenied):
        return None


def _kill_tree(proc: subprocess.Popen[bytes]) -> None:
    if proc.poll() is not None:
        return
    if os.name == "nt":
        subprocess.run(
            ["taskkill", "/PID", str(proc.pid), "/T", "/F"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
    else:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            proc.kill()


def _run_one_unlocked(case: Case, iteration: int, timeout: int, out_dir: Path) -> Result:
    out_dir.mkdir(parents=True, exist_ok=True)
    stdout_path = out_dir / f"{case.name}.{iteration}.stdout.log"
    stderr_path = out_dir / f"{case.name}.{iteration}.stderr.log"
    command = list(case.command)
    if case.windows_only and os.name != "nt":
        reason = "fixture requires Windows PowerShell"
        stdout_path.write_text(reason + "\n", encoding="utf-8")
        stderr_path.write_text("", encoding="utf-8")
        return Result(
            case.name,
            iteration,
            command,
            "SKIP",
            SKIP_EXIT,
            0.0,
            None,
            reason,
            str(stdout_path),
            str(stderr_path),
            hashlib.sha256(reason.encode()).hexdigest(),
        )
    if command and command[0] == "__missing_powershell__":
        reason = "PowerShell executable not found"
        stdout_path.write_text(reason + "\n", encoding="utf-8")
        stderr_path.write_text("", encoding="utf-8")
        return Result(
            case.name,
            iteration,
            command,
            "SKIP",
            SKIP_EXIT,
            0.0,
            None,
            reason,
            str(stdout_path),
            str(stderr_path),
            hashlib.sha256(reason.encode()).hexdigest(),
        )

    env = os.environ.copy()
    # Keep benchmark/toolchain discovery on the repository's pinned LLVM;
    # otherwise an unrelated system clang can make one machine skip or measure
    # a different producer configuration.
    llvm_root = ROOT / "clang"
    runtime_dir = ROOT / "bootstrap_compiler" / "out"
    path_parts = []
    if (llvm_root / "bin").exists():
        env["LLVM_ROOT"] = str(llvm_root)
        path_parts.append(str(llvm_root / "bin"))
    if runtime_dir.exists():
        # Windows resolves the boot executable's runtime DLLs through PATH;
        # Linux consumers also receive LD_LIBRARY_PATH below.
        path_parts.append(str(runtime_dir))
        env["LD_LIBRARY_PATH"] = str(runtime_dir) + os.pathsep + env.get("LD_LIBRARY_PATH", "")
    if path_parts:
        env["PATH"] = os.pathsep.join(path_parts) + os.pathsep + env.get("PATH", "")
    env["DCI_INDUSTRIAL_SEED"] = str(SEED)
    env["DCI_INDUSTRIAL_CASE"] = case.name
    started = time.monotonic()
    peak: int | None = None
    rss_status = "sampled with psutil"
    creationflags = 0
    preexec_fn = None
    if os.name == "nt":
        creationflags = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
    else:
        preexec_fn = os.setsid
    with stdout_path.open("wb") as stdout_file, stderr_path.open("wb") as stderr_file:
        try:
            proc = subprocess.Popen(
                command,
                cwd=str(ROOT),
                env=env,
                stdout=stdout_file,
                stderr=stderr_file,
                creationflags=creationflags,
                preexec_fn=preexec_fn,
            )
        except OSError as exc:
            stderr_path.write_text(str(exc) + "\n", encoding="utf-8")
            return Result(
                case.name,
                iteration,
                command,
                "FAIL",
                None,
                time.monotonic() - started,
                None,
                f"spawn failed: {exc}",
                str(stdout_path),
                str(stderr_path),
                "",
            )
        try:
            try:
                import psutil  # type: ignore

                ps_process = psutil.Process(proc.pid)
            except ImportError:
                ps_process = None
                rss_status = "not measured: psutil is unavailable"
            while proc.poll() is None:
                if ps_process is not None:
                    sample = _tree_rss(ps_process)
                    if sample is not None:
                        peak = max(peak or 0, sample)
                if time.monotonic() - started > timeout:
                    _kill_tree(proc)
                    proc.wait(timeout=20)
                    status = "TIMEOUT"
                    code = None
                    break
                time.sleep(0.05)
            else:
                code = proc.returncode
                status = "PASS" if code == 0 else ("SKIP" if code == SKIP_EXIT else "FAIL")
        except Exception as exc:  # pragma: no cover - process failure path
            _kill_tree(proc)
            code = None
            status = "FAIL"
            rss_status = f"runner error: {exc}"
    wall = time.monotonic() - started
    payload = stdout_path.read_bytes() + b"\0" + stderr_path.read_bytes()
    if peak is None and rss_status == "sampled with psutil":
        rss_status = "not measured: process exited before a sample"
    return Result(
        case.name,
        iteration,
        command,
        status,
        code,
        round(wall, 6),
        peak,
        rss_status,
        str(stdout_path),
        str(stderr_path),
        hashlib.sha256(payload).hexdigest(),
    )


def _run_one(case: Case, iteration: int, timeout: int, out_dir: Path) -> Result:
    # A fixture owns its contract/cache/target directories. Keep repeated
    # runs of that fixture serial even when unrelated projects are scheduled
    # in parallel; otherwise two adapters could delete or rewrite one another's
    # derived state and turn a pressure run into a race detector.
    lock = _CASE_LOCKS.setdefault(case.resource_key or case.name, threading.Lock())
    with lock:
        return _run_one_unlocked(case, iteration, timeout, out_dir)


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default=str(ROOT / "bootstrap_compiler/out/boot.exe"))
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--scale", type=int, default=1)
    parser.add_argument("--parallel", type=int, default=1)
    parser.add_argument("--timeout-sec", type=int, default=1800)
    parser.add_argument("--case", action="append", dest="selected", help="case name (repeatable)")
    parser.add_argument("--out", type=Path, default=OUT)
    return parser.parse_args()


def main() -> int:
    args = _parse_args()
    if args.repeat < 1 or args.scale < 1 or args.parallel < 1 or args.timeout_sec < 1:
        raise SystemExit("repeat, scale, parallel and timeout-sec must be positive")
    all_cases = cases(args.compiler)
    selected = all_cases
    if args.selected:
        wanted = set(args.selected)
        unknown = wanted - {item.name for item in all_cases}
        if unknown:
            raise SystemExit(f"unknown case(s): {', '.join(sorted(unknown))}")
        selected = [item for item in all_cases if item.name in wanted]
    OUT_DIR = args.out.resolve()
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    iterations = args.repeat * args.scale
    jobs = [(case, n) for n in range(1, iterations + 1) for case in selected]
    results: list[Result] = []
    # Runs of the same fixture are intentionally serialized by the case name;
    # its contract/cache paths are part of the fixture and are not shared by
    # the runner.  Different real projects may be pressure-tested in parallel.
    if args.parallel == 1:
        for case, n in jobs:
            results.append(_run_one(case, n, args.timeout_sec, OUT_DIR))
    else:
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.parallel) as pool:
            futures = [
                pool.submit(_run_one, case, n, args.timeout_sec, OUT_DIR)
                for case, n in jobs
            ]
            for future in futures:
                results.append(future.result())
    results.sort(key=lambda item: (item.iteration, item.name))
    document = {
        "schema": "dci-industrial-metrics-1",
        "seed": SEED,
        "requested": {
            "repeat": args.repeat,
            "scale": args.scale,
            "iterations_per_case": iterations,
            "parallel": args.parallel,
            "timeout_sec": args.timeout_sec,
        },
        "host": {
            "platform": platform.platform(),
            "machine": platform.machine(),
            "python": platform.python_version(),
            "compiler": str(Path(args.compiler).resolve()),
        },
        "results": [asdict(item) for item in results],
    }
    json_path = OUT_DIR / "metrics.json"
    json_path.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"dci-industrial: seed={SEED} cases={len(selected)} iterations={iterations} parallel={args.parallel}")
    for item in results:
        rss = str(item.peak_rss_bytes) if item.peak_rss_bytes is not None else "unmeasured"
        print(f"{item.status:7} {item.name:24} run={item.iteration:<3} wall={item.wall_seconds:.3f}s rss={rss}")
    failures = [item for item in results if item.status == "FAIL" or item.status == "TIMEOUT"]
    skips = sum(item.status == "SKIP" for item in results)
    print(f"dci-industrial: PASS={len(results) - len(failures) - skips} SKIP={skips} FAIL={len(failures)} metrics={json_path}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
