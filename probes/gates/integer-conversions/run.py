"""Verify fitting integer literals, widening, explicit casts and rejected truncation."""
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
    records = []
    env = dict(os.environ)
    env["PATH"] = str(compiler.parent) + os.pathsep + env.get("PATH", "")
    suffix = ".exe" if os.name == "nt" else ""

    def invoke(name, command, expected=0, stdout=None):
        try:
            result = subprocess.run([str(x) for x in command], cwd=repo, env=env,
                                    capture_output=True, timeout=120)
            text = (result.stdout + result.stderr).decode(errors="replace")
            passed = result.returncode == expected
            if expected == 1:
                passed = passed and "E1000:" in text and "type mismatch" in text and ".vyx:" in text
            if stdout is not None:
                passed = passed and result.stdout.replace(b"\r\n", b"\n") == stdout
            record = {"name": name, "command": [str(x) for x in command],
                      "exit": result.returncode, "diagnostics": text, "pass": passed}
        except subprocess.TimeoutExpired:
            record = {"name": name, "timeout": True, "pass": False}
        records.append(record)
        print(name, "PASS" if record["pass"] else "FAIL", flush=True)
        if not record["pass"]:
            print(json.dumps(record, ensure_ascii=False, indent=2), flush=True)
        return record["pass"]

    for opt in (0, 2):
        exe = output / f"values-O{opt}{suffix}"
        if invoke(f"values-O{opt}-compile", [compiler, "--src=file", gate / "values.vyx",
                                            "--emit=exe", f"-O{opt}", "-o", exe]):
            invoke(f"values-O{opt}-run", [exe], stdout=b"integer-conversions=ok\n")
    prefix = "use std.collections;\nclass Packet { public value: i8; }\n"
    negatives = {
        "let-width": "let n: i32 = 300; let byte: i8 = n;",
        "assignment-width": "let n: i32 = 300; var byte: i8 = 1; byte = n;",
        "return-width": "bad_return();",
        "literal-width": "let byte: i8 = 128;",
        "literal-negative-width": "let byte: i8 = -129;",
        "literal-unsigned-width": "let byte: u8 = 256;",
        "array-literal-width": "let array: [i8; 1] = [300];",
        "array-literal-size": "let array: [i8; 1] = [1, 2];",
        "array-zero-size": "let array: [i8; 0] = [1];",
        "tuple-literal-width": "let pair: (i8, bool) = (300, true);",
        "field-width": "let n: i32 = 300; var packet = Packet { value: 1 }; packet.value = n;",
        "field-literal-width": "var packet = Packet { value: 1 }; packet.value = 300;",
        "field-init-width": "let packet = Packet { value: 300 };",
        "array-width": "let n: i32 = 300; var array: [i8; 2] = [1, 2]; array[0] = n;",
        "deref-width": "let n: i32 = 300; var byte: i8 = 1; let p: &mut i8 = &mut byte; *p = n;",
        "vec-width": "var vec = Vec::<i8>.new(); vec.push(1); vec[0] = 300;",
    }
    sources = [(name, gate.parent / "industrial" / f"{name}.vyx")
               for name in ("int_narrow_err", "int_unarrow_err", "int_sign_err")]
    for name, body in negatives.items():
        source = output / f"{name}.vyx"
        returning = "fn bad_return() -> i8 { let n: i32 = 300; return n; }\n" if name == "return-width" else ""
        source.write_text(prefix + returning + "fn main() -> i32 {\n" + body + "\nreturn 0;\n}\n", encoding="utf-8")
        sources.append((name, source))
    for name, source in sources:
        exe = output / (name + suffix)
        invoke(name, [compiler, "--src=file", source, "--emit=exe", "-O0", "-o", exe], expected=1)
        records.append({"name": name + "-no-artifact", "pass": not exe.exists()})
    report = {"compiler": str(compiler), "sha256": hashlib.sha256(compiler.read_bytes()).hexdigest(),
              "records": records, "passed": sum(r["pass"] for r in records), "checks": len(records)}
    (output / "results.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f'{report["passed"]}/{report["checks"]} PASS ({output})', flush=True)
    return 0 if all(r["pass"] for r in records) else 1


if __name__ == "__main__":
    raise SystemExit(main())
