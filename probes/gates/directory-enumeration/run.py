#!/usr/bin/env python3
"""Check native POSIX directory names and standard-library file copying."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--cc", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if os.name == "nt":
        print("SKIP POSIX directory ABI on Windows")
        return 77
    gate = Path(__file__).resolve().parent
    repo = gate.parents[2]
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    source, destination = output / "source", output / "destination"
    source.mkdir()
    destination.mkdir()
    long_name = "long_" + "a" * 210 + ".vyx"
    names = ["main.vyx", long_name, "space+字.vyi", "subdirectory"]
    for name in names[:-1]:
        (source / name).write_text("main payload" if name == "main.vyx" else name, encoding="utf-8")
    (source / names[-1]).mkdir()
    (source / names[-1] / "nested.vyx").write_text("nested", encoding="utf-8")
    runtime = repo / "runtime/src/vyx_directory.c"
    records = []

    def run(name, command, expected=None):
        result = subprocess.run(list(map(str, command)), capture_output=True, timeout=90)
        record = dict(name=name, command=list(map(str, command)), exit=result.returncode,
                      stdout=result.stdout.decode(errors="replace"), stderr=result.stderr.decode(errors="replace"))
        records.append(record)
        if result.returncode or (expected is not None and result.stdout != expected):
            raise RuntimeError(json.dumps(record, ensure_ascii=False))
        print(name + ": PASS", flush=True)

    report = dict(compiler=str(args.compiler.resolve()), cc=str(args.cc.resolve()),
                  cc_version=subprocess.check_output([args.cc, "--version"], text=True),
                  runtime_source_sha256=hashlib.sha256(runtime.read_bytes()).hexdigest(),
                  compiler_sha256=hashlib.sha256(args.compiler.read_bytes()).hexdigest(), records=records)
    try:
        native = output / "entries"
        run("native-build", [args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                             runtime, gate / "entries.c", "-o", native])
        run("native-names", [native, source, *names], b"native directory names PASS\n")
        executable = output / "copy-files"
        run("stdlib-build", [args.compiler, "--src=file", gate / "copy_files.vyx",
                             "--emit=exe", "-o", executable])
        run("stdlib-copy", [executable, source, destination], b"directory copy PASS\n")
        assert sorted(p.name for p in destination.iterdir()) == sorted(names[:2])
        for name in names[:2]:
            assert (destination / name).read_bytes() == (source / name).read_bytes()
        report["passed"] = True
    finally:
        (output / "verification.json").write_text(json.dumps(report, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
