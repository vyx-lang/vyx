"""Verify native value construction and reject incompatible nominal inputs."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
from datetime import datetime


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True, type=Path)
    args = parser.parse_args()
    compiler = args.compiler.resolve(strict=True)
    gate = Path(__file__).resolve().parent
    repo = gate.parents[2]
    output = gate / ".cache" / datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    output.mkdir(parents=True)
    env = dict(os.environ)
    env["PATH"] = str(compiler.parent) + os.pathsep + env.get("PATH", "")
    records = []

    def invoke(name, command, expected_exit=0, diagnostic="", stdout=None):
        try:
            result = subprocess.run([str(x) for x in command], cwd=repo, env=env,
                                    capture_output=True, timeout=90)
            text = (result.stdout + result.stderr).decode(errors="replace")
            passed = result.returncode == expected_exit and diagnostic in text
            if stdout is not None:
                passed = passed and result.stdout.replace(b"\r\n", b"\n") == stdout
            record = {"name": name, "command": [str(x) for x in command],
                      "exit": result.returncode, "diagnostics": text, "pass": passed}
        except subprocess.TimeoutExpired:
            record = {"name": name, "command": [str(x) for x in command],
                      "timeout": True, "pass": False}
        records.append(record)
        print(name, "PASS" if record["pass"] else "FAIL", flush=True)
        if not record["pass"]:
            print(json.dumps(record, indent=2), flush=True)
        return record["pass"]

    suffix = ".exe" if os.name == "nt" else ""
    for opt in (0, 2):
        exe = output / f"values-O{opt}{suffix}"
        if invoke(f"values-O{opt}-compile", [compiler, "--src=file", gate / "values.vyx",
                                            "--emit=exe", f"-O{opt}", "-o", exe]):
            invoke(f"values-O{opt}-run", [exe], stdout=b"value-construction=ok\n")

    prefix = "class Point { public value: i64; }\nclass Other { public value: i64; }\n"
    negatives = {
        "record-new-string": 'let p = Point.new("bytes");',
        "record-new-rawptr": 'let p = Point.new(null);',
        "record-construct-string": 'let p = construct::<Point>("bytes");',
        "record-new-other": 'let p = Point.new(Other { value: 42 });',
        "record-construct-other": 'let p = construct::<Point>(Other { value: 42 });',
        "record-new-extra": 'let p = Point.new(1, 2);',
        "generic-record-string": 'let p = make::<Point>("bytes");',
    }
    for name, body in negatives.items():
        source = output / f"{name}.vyx"
        source.write_text(prefix + "fn make<T, ...Args>(...args: Args) -> T { return construct::<T>(args...); }\n"
                          + "fn main() -> i32 { " + body + " return 0; }\n", encoding="utf-8")
        exe = output / (name + suffix)
        invoke(name, [compiler, "--src=file", source, "--emit=exe", "-O0", "-o", exe],
               expected_exit=1, diagnostic="requires a matching constructor or one compatible value")
        records.append({"name": name + "-no-artifact", "pass": not exe.exists()})

    # Equal leaf names and layouts in separate modules are still distinct
    # nominal types. Supply the source closure explicitly, as project tasks do.
    modules = output / "modules"
    modules.mkdir()
    for module in ("left", "right"):
        (modules / f"{module}.vyx").write_text(
            f"module {module};\npublic class Record {{ public value: i64; }}\n", encoding="utf-8")
    sources = modules / "sources.txt"
    sources.write_text(str(modules / "left.vyx") + "\n" + str(modules / "right.vyx") + "\n",
                       encoding="utf-8")
    source = modules / "main.vyx"
    source.write_text("use left; use right;\nfn main() -> i32 {\n"
                      "let p = left.Record.new(right.Record { value: 42 });\nreturn 0;\n}\n",
                      encoding="utf-8")
    exe = modules / ("denied" + suffix)
    invoke("distinct-module-records", [compiler, "--src=file", source, "--unit-sources", sources,
                                       "--emit=exe", "-O0", "-o", exe], expected_exit=1,
           diagnostic="requires a matching constructor or one compatible value")
    records.append({"name": "distinct-module-records-no-artifact", "pass": not exe.exists()})

    report = {"compiler": str(compiler), "sha256": hashlib.sha256(compiler.read_bytes()).hexdigest(),
              "records": records, "passed": sum(r["pass"] for r in records), "checks": len(records)}
    (output / "results.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f'{report["passed"]}/{report["checks"]} PASS ({output})', flush=True)
    return 0 if all(r["pass"] for r in records) else 1


if __name__ == "__main__":
    raise SystemExit(main())
