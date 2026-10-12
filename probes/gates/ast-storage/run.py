"""Check AST metadata lifetime and semantic target tags using AOT fixtures."""
from __future__ import annotations

import argparse
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True, type=Path)
    parser.add_argument("--guarded-compiler", type=Path)
    args = parser.parse_args()
    compiler = args.compiler.resolve(strict=True)
    gate = Path(__file__).resolve().parent
    repo = gate.parents[2]
    output = gate / ".cache" / datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    output.mkdir(parents=True)
    compilers = {"normal": compiler}
    if args.guarded_compiler:
        compilers["guarded"] = args.guarded_compiler.resolve(strict=True)
    cases = {
        "vec-ref": (repo / "tests/cases/misc/test_vec_ref_type.vyx", b""),
        "indirect": (gate / "indirect_calls.vyx", b"indirect calls ok\n"),
        "inheritance": (repo / "probes/gates/industrial/inherit_fields.vyx", b""),
        "derived-trait": (repo / "probes/gates/compiler-usability/derived_trait.vyx", b"42\n"),
        "iter-fold": (repo / "tests/cases/conformance/stdlib/iter_trait/verify_iter_fold_function_value.vyx",
                      b"iter_fold_function_value OK\n"),
        "comptime": (repo / "probes/gates/comptime/fold.vyx", b""),
    }
    records = []
    identities = {}

    def invoke(name, command, env, stdout=None):
        command = [str(x) for x in command]
        try:
            result = subprocess.run(command, cwd=repo, env=env, capture_output=True, timeout=120)
            passed = result.returncode == 0
            if stdout is not None:
                passed = passed and result.stdout.replace(b"\r\n", b"\n") == stdout
            record = {"name": name, "command": command, "exit": result.returncode, "pass": passed,
                      "stdout": result.stdout.decode(errors="replace"),
                      "stderr": result.stderr.decode(errors="replace")}
        except subprocess.TimeoutExpired:
            record = {"name": name, "command": command, "pass": False, "timeout": True}
        records.append(record)
        print(name, "PASS" if record["pass"] else "FAIL", flush=True)
        if not record["pass"]:
            print(json.dumps(record, ensure_ascii=False, indent=2), flush=True)
        return record["pass"]

    for mode, current in compilers.items():
        env = dict(os.environ)
        env["PATH"] = str(current.parent) + os.pathsep + env.get("PATH", "")
        identities[mode] = {"path": str(current),
                            "sha256": hashlib.sha256(current.read_bytes()).hexdigest()}
        for name, (source, stdout) in cases.items():
            for opt in (0, 2):
                exe = output / (f"{mode}-{name}-O{opt}" + (".exe" if os.name == "nt" else ""))
                command = [current, "--src=file", source, "--emit=exe", f"-O{opt}", "-o", exe]
                if invoke(f"{mode}-{name}-O{opt}-compile", command, env):
                    invoke(f"{mode}-{name}-O{opt}-run", [exe], env, stdout)
        # Keep the original driver path in addition to exact executable output.
        exe = output / (f"{mode}-vec-ref-driver" + (".exe" if os.name == "nt" else ""))
        invoke(f"{mode}-vec-ref-driver", [current, "--src=file", cases["vec-ref"][0],
               "--run=aot", "-O0", "-o", exe], env)
        ir = output / f"{mode}-comptime-O0.ll"
        if invoke(f"{mode}-comptime-ir", [current, "--src=file", cases["comptime"][0],
                  "--emit=ir", "-O0", "-o", ir], env):
            text = ir.read_text(encoding="utf-8")
            body = re.search(r"^define[^\n]*@main\([^\n]*\)[^\n]*\{\n(.*?)^\}", text, re.M | re.S)
            # The expected folded result must appear before LLVM optimization.
            passed = bool(body and "store i32 15," in body[1]
                          and not re.search(r"\b(?:call|invoke)\b[^\n]*triple", body[1]))
            records.append({"name": f"{mode}-comptime-folded", "pass": passed, "ir": str(ir)})
            print(f"{mode}-comptime-folded", "PASS" if passed else "FAIL", flush=True)
    report = {"compilers": identities, "records": records,
              "status": "pass" if all(r["pass"] for r in records) else "fail",
              "guard": "executed" if "guarded" in compilers else "not executed"}
    (output / "results.json").write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
    print(output / "results.json")
    return 0 if report["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
