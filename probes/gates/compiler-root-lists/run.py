#!/usr/bin/env python3
"""Check export/emit root list syntax against native IR, AOT and CUDA PTX."""
import argparse
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


NATIVE = """fn helper(x: i32) -> i32 { return x + 10; }
public fn first() -> i32 { return helper(1); }
public fn second() -> i32 { return 22; }
public fn third() -> i32 { return 33; }
public fn first_extra() -> i32 { return 99; }
public fn choose<A, B>(a: A, b: B) -> A { return a; }
"""
DEVICE = """@[cuda_kernel]
public fn first(values: *i32) { unsafe { values[0] = 11; } }
@[cuda_kernel]
public fn second(values: *i32) { unsafe { values[0] = 22; } }
@[cuda_kernel]
public fn third(values: *i32) { unsafe { values[0] = 33; } }
@[cuda_kernel]
public fn first_extra(values: *i32) { unsafe { values[0] = 99; } }
"""
ROOT_SYMBOLS = {"first": "_F_first_", "second": "_F_second_", "third": "_F_third_",
                "first_extra": "_F_first_5Fextra_"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    compiler = args.compiler.resolve(strict=True)
    gate = Path(__file__).resolve().parent
    output = (args.output_dir or gate / ".cache" / datetime.now().strftime("%Y%m%d-%H%M%S-%f")).resolve()
    output.mkdir(parents=True, exist_ok=False)
    environment = os.environ.copy()
    environment["PATH"] = str(compiler.parent) + os.pathsep + environment.get("PATH", "")
    native, device = output / "native.vyx", output / "device.vyx"
    native.write_text(NATIVE, encoding="utf-8")
    device.write_text(DEVICE, encoding="utf-8")
    records = []

    def run(name, command):
        result = subprocess.run(list(map(str, command)), cwd=output, env=environment,
                                capture_output=True, text=True, timeout=120)
        record = {"name": name, "command": list(map(str, command)), "exit": result.returncode,
                  "stdout": result.stdout, "stderr": result.stderr, "passed": result.returncode == 0}
        records.append(record)
        return record

    def check(record, passed, **facts):
        record.update(passed=record["passed"] and passed, **facts)
        print(record["name"] + (": PASS" if record["passed"] else ": FAIL"), flush=True)
        if not record["passed"]:
            print(json.dumps(record, ensure_ascii=False), flush=True)

    version = run("identity", [compiler, "--version"])
    variants = {
        "single": ("first", ["first"]),
        "comma-two": ("first,second", ["first", "second"]),
        "comma-three": ("first,second,third", ["first", "second", "third"]),
        "comma-spaces": (" first , second , third ", ["first", "second", "third"]),
        "pipe": ("first|second", ["first", "second"]),
        "mixed": ("first,second|third", ["first", "second", "third"]),
        "empty": ("", []),
    }
    for backend, source in (("native", native), ("ptx", device)):
        for mode in ("export", "emit"):
            for variant, (roots, expected) in variants.items():
                # File and inline forms must feed the same normalized root set.
                for from_file in (False, True):
                    name = f"{backend}-{mode}-{variant}" + ("-file" if from_file else "")
                    artifact = output / (name + (".ll" if backend == "native" else ".ptx"))
                    option = f"--{mode}-root-names"
                    value = roots
                    if from_file:
                        names = output / (name + ".txt")
                        names.write_text(roots, encoding="utf-8")
                        option += "-file"
                        value = names
                    command = [compiler, "--src=file", source, "--no-process-entry", "-O0",
                               "--emit=ir" if backend == "native" else "--emit=obj", "-o", artifact,
                               option, value]
                    if mode == "emit":
                        command.append("--export-top-level-roots")
                    if backend == "ptx":
                        command += ["--target=nvptx64-nvidia-cuda", "--target-cpu", "sm_70"]
                    record = run(name, command)
                    text = artifact.read_text(encoding="utf-8") if artifact.is_file() else ""
                    symbols = (re.findall(r"(?m)^define\b[^\n]*@([\w.$]+)\(", text) if backend == "native"
                               else re.findall(r"(?m)^\.visible \.entry\s+([\w.$]+)\(", text))
                    selected = [name for name in ("first", "second", "third", "first_extra")
                                if any(ROOT_SYMBOLS[name] in symbol for symbol in symbols)]
                    check(record, selected == expected, selected=selected, expected=expected)

        for flag in ("--export-all", "--export-top-level-roots"):
            name = backend + "-" + flag.removeprefix("--")
            artifact = output / (name + (".ll" if backend == "native" else ".ptx"))
            command = [compiler, "--src=file", source, "--no-process-entry", "-O0", flag,
                       "--emit=ir" if backend == "native" else "--emit=obj", "-o", artifact]
            if backend == "ptx":
                command += ["--target=nvptx64-nvidia-cuda", "--target-cpu", "sm_70"]
            record = run(name, command)
            text = artifact.read_text(encoding="utf-8") if artifact.is_file() else ""
            check(record, all(symbol in text for symbol in ROOT_SYMBOLS.values()))

    # A comma within generic arguments is part of the root, not a list separator.
    for from_file in (False, True):
        name = "generic-comma" + ("-file" if from_file else "")
        roots = "choose<i32,i64>,third"
        option, value = "--export-root-names", roots
        if from_file:
            value = output / (name + ".txt")
            value.write_text(roots, encoding="utf-8")
            option += "-file"
        artifact = output / (name + ".ll")
        record = run(name, [compiler, "--src=file", native, "--emit=ir", "-O0", "-o", artifact, option, value])
        text = artifact.read_text(encoding="utf-8") if artifact.is_file() else ""
        symbols = re.findall(r"(?m)^define\b[^\n]*@([\w.$]+)\(", text)
        check(record, any(re.fullmatch(r"__vyx_F_mir2_24choose_.*_R_i32_P_i32_i64", symbol)
                          for symbol in symbols)
              and any("_F_third_" in symbol for symbol in symbols), symbols=symbols)

    app = output / "app.vyx"
    app.write_text(NATIVE + "\nfn main() -> i32 {\n"
                   "    if (first() != 11 || second() != 22 || third() != 33) { return 1; }\n"
                   "    return 0;\n}\n", encoding="utf-8")
    record = run("native-aot", [compiler, "--src=file", app, "--run=aot", "-O2",
                                "--export-root-names", "main,first,second,third"])
    check(record, record["exit"] == 0)
    passed = all(record["passed"] for record in records)
    identity = {"compiler": str(compiler), "sha256": hashlib.sha256(compiler.read_bytes()).hexdigest(),
                "version": version["stdout"], "records": records, "passed": passed}
    for name in ("vyx_compiler_backend.dll", "vyx_runtime.lib"):
        path = compiler.parent / name
        if path.is_file():
            identity[name] = hashlib.sha256(path.read_bytes()).hexdigest()
    report = output / "verification.json"
    report.write_text(json.dumps(identity, ensure_ascii=False, indent=2), encoding="utf-8")
    print("report:", report)
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
