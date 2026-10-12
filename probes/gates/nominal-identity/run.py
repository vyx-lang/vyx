"""Check nominal identities and pointee compatibility through project/AOT paths."""
from __future__ import annotations

import argparse
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import subprocess


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
    suffix = ".exe" if os.name == "nt" else ""

    def invoke(name, command, expected=0, stdout=None, diagnostic=False):
        try:
            result = subprocess.run([str(x) for x in command], cwd=repo, env=env,
                                    capture_output=True, timeout=120)
            text = (result.stdout + result.stderr).decode(errors="replace")
            passed = result.returncode == expected
            if diagnostic:
                passed = passed and "E1000:" in text and ".vyx:" in text and "type mismatch" in text
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
            print(json.dumps(record, ensure_ascii=False, indent=2), flush=True)
        return record["pass"]

    for opt in (0, 2):
        exe = output / f"project-O{opt}{suffix}"
        if invoke(f"project-O{opt}-build", [compiler, "--src=project", gate, "--emit=exe",
                                           f"-O{opt}", "-o", exe]):
            invoke(f"project-O{opt}-run", [exe], stdout=b"nominal-identity=ok\n")

    sources = output / "sources.txt"
    sources.write_text("\n".join(str(gate / "src" / f"{m}.vyx") for m in ("left", "right"))
                       + "\n", encoding="utf-8")
    left = "identity.left.Record { value: 17 }"
    right = "identity.right.Record { prefix: 100, value: 42 }"
    negatives = {
        "value-init": f"let x: identity.left.Record = {right};",
        "inferred-assignment": f"var x = {left}; x = {right};",
        "named-assignment": f"var x = {left}; let y = {right}; x = y;",
        "value-argument": f"consume({right});",
        "reference-init": f"let y = {right}; let x: &identity.left.Record = &y;",
        "reference-argument": f"let y = {right}; borrow(&y);",
        "reference-return": f"bad_return(&{right});",
        "pointer-init": f"let y = {right}; let x: *identity.left.Record = &y;",
        "pointer-argument": f"let y = {right}; pointer(&y);",
        "shared-to-mutable": f"let y = {left}; let x: &mut identity.left.Record = &y;",
        "generic-base": "let x: identity.left.Box<i64> = identity.right.Box<i64> { value: 42 };",
        "generic-base-argument": "box_arg(identity.right.Box<i64> { value: 42 });",
        "generic-argument": f"let x: identity.left.Box<identity.left.Record> = identity.left.Box<identity.right.Record> {{ value: {right} }};",
        "generic-reference": "let y = identity.right.Box<i64> { value: 42 }; box_borrow(&y);",
        "nested-pointer": f"let y = {right}; let p: *identity.right.Record = &y; let x: &*identity.left.Record = &p;",
        "pointee-width": "let y: i32 = 42; let x: &i64 = &y;",
        "pointer-width": "let y: i32 = 42; let x: *i64 = &y;",
    }
    prefix = """use identity.left;
use identity.right;
fn consume(x: identity.left.Record) -> i32 { return 0; }
fn borrow(x: &identity.left.Record) -> i32 { return 0; }
fn pointer(x: *identity.left.Record) -> i32 { return 0; }
fn box_arg(x: identity.left.Box<i64>) -> i32 { return 0; }
fn box_borrow(x: &identity.left.Box<i64>) -> i32 { return 0; }
"""
    for name, body in negatives.items():
        source = output / f"{name}.vyx"
        returning = ""
        if name == "reference-return":
            returning = "fn bad_return(x: &identity.right.Record) -> &identity.left.Record { return x; }\n"
        source.write_text(prefix + returning + "fn main() -> i32 {\n" + body + "\nreturn 0;\n}\n",
                          encoding="utf-8")
        exe = output / (name + suffix)
        invoke(name, [compiler, "--src=file", source, "--unit-sources", sources,
                      "--emit=exe", "-O0", "-o", exe], expected=1, diagnostic=True)
        records.append({"name": name + "-no-artifact", "pass": not exe.exists()})
    report = {"compiler": str(compiler), "sha256": hashlib.sha256(compiler.read_bytes()).hexdigest(),
              "records": records, "passed": sum(r["pass"] for r in records), "checks": len(records)}
    (output / "results.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f'{report["passed"]}/{report["checks"]} PASS ({output})', flush=True)
    return 0 if all(r["pass"] for r in records) else 1


if __name__ == "__main__":
    raise SystemExit(main())
