# 编 QPoint-only 探针并采样 RSS。
import os
import shutil
import subprocess
import threading
import time
from pathlib import Path


ROOT = Path(r"e:\Dev\C++\VyxLan-selfhost-yolo")
PROBE = ROOT / "probes" / "gates" / "dci-qt" / "run-output" / "qpoint-only"
LLVM = ROOT / "clang"
BOOT = ROOT / "bootstrap_compiler" / "out" / "boot.exe"
QT = Path(os.environ.get("QTDIR") or r"E:\Qt\6.7.3\msvc2019_64")
log_path = ROOT / "probes" / "gates" / "dci-qt" / "run-output" / "qpoint_only_rss.txt"


def process_ws_sum():
    cmd = (
        "(Get-Process -Name boot -ErrorAction SilentlyContinue "
        "| Measure-Object WorkingSet64 -Sum).Sum"
    )
    r = subprocess.run(
        ["powershell", "-NoProfile", "-Command", cmd],
        capture_output=True,
        text=True,
    )
    try:
        return int((r.stdout or "0").strip() or "0")
    except ValueError:
        return 0


env = os.environ.copy()
env["LLVM_ROOT"] = str(LLVM)
env["PATH"] = (
    str(LLVM / "bin") + ";"
    + str(ROOT / "bootstrap_compiler" / "out") + ";"
    + str(QT / "bin") + ";"
    + env.get("PATH", "")
)
env["DCI_QT_CXX"] = str(LLVM / "bin" / "clang++.exe")
env["DCI_QT_INCLUDE"] = str(QT / "include")
env["DCI_QT_INCLUDE_QTCORE"] = str(QT / "include" / "QtCore")
env["DCI_QT_MKSPECS"] = str(QT / "mkspecs" / "win32-msvc")
env["DCI_QT_LIBDIR"] = str(QT / "lib")
env.pop("VYX_DEBUG_LINK", None)

for name in (".cache", "target"):
    p = PROBE / name
    if p.exists():
        shutil.rmtree(p, ignore_errors=True)

peak = {"ws": 0, "n": 0, "stop": False}


def sampler():
    while not peak["stop"]:
        ws = process_ws_sum()
        if ws > peak["ws"]:
            peak["ws"] = ws
        peak["n"] = peak["n"] + 1
        time.sleep(0.05)


th = threading.Thread(target=sampler, daemon=True)
th.start()
start = time.time()
proc = subprocess.Popen(
    [str(BOOT), "build", "--target", "dci_qt", "-j", "1"],
    cwd=str(PROBE),
    env=env,
    stdout=subprocess.PIPE,
    stderr=subprocess.STDOUT,
    text=True,
    encoding="utf-8",
    errors="replace",
)
out = proc.communicate()[0]
peak["stop"] = True
th.join(timeout=2)
elapsed = time.time() - start
report = (
    f"exit={proc.returncode}\n"
    f"elapsed_s={elapsed:.1f}\n"
    f"peak_ws_mb={peak['ws'] / (1024 * 1024):.1f}\n"
    f"samples={peak['n']}\n"
)
log_path.write_text(report + "\n" + (out or "")[-12000:], encoding="utf-8")
print(out[-6000:] if out and len(out) > 6000 else out)
print(report)
if proc.returncode == 0:
    exe = PROBE / "target" / "dci_qt.exe"
    if not exe.exists():
        exe = PROBE / "out" / "dci_qt.exe"
    if exe.exists():
        r2 = subprocess.run([str(exe)], cwd=str(PROBE), env=env, capture_output=True, text=True)
        print("run_exit", r2.returncode)
        print(r2.stdout)
        print(r2.stderr)
raise SystemExit(0)
