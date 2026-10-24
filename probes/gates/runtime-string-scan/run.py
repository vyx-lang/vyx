#!/usr/bin/env python3
"""Test bootstrap compatibility scans against a real backend or scanner unit."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    selection = parser.add_mutually_exclusive_group(required=True)
    selection.add_argument("--backend", type=Path)
    selection.add_argument("--unit", action="store_true")
    parser.add_argument("--cxx")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    cxx = args.cxx or shutil.which("clang++")
    if not cxx and os.name == "nt":
        cxx = str(root / "clang/bin/clang++.exe")
    if not cxx:
        parser.error("clang++ is required")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    executable = output / ("scan.exe" if os.name == "nt" else "scan")
    command = [cxx, "-std=c++20", "-O2", "-D_CRT_SECURE_NO_WARNINGS",
               str(Path(__file__).with_name("probe.cpp")), "-o", str(executable)]
    if args.unit:
        command += ["-DVYX_SCAN_UNIT"]
    if sys.platform == "linux":
        command += ["-ldl", "-Wl,--export-dynamic"]
    subprocess.run(command, check=True)
    environment = os.environ.copy()
    arguments = [str(executable)]
    if args.backend:
        backend = args.backend.resolve()
        print(f"backend={backend} sha256={hashlib.sha256(backend.read_bytes()).hexdigest()}", flush=True)
        arguments += [str(backend)]
        if os.name == "nt":
            environment["PATH"] = str(backend.parent) + os.pathsep + environment["PATH"]
    else:
        print("Testing production scanner header as a unit; no complete backend build", flush=True)
    subprocess.run(arguments, env=environment, check=True, timeout=30)


if __name__ == "__main__":
    main()
