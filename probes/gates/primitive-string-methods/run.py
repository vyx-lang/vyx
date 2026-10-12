"""Check primitive to_string/toString return types and AOT output."""
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
    output = gate / ".cache" / datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    output.mkdir(parents=True)
    env = dict(os.environ)
    env["PATH"] = str(compiler.parent) + os.pathsep + env.get("PATH", "")
    records = []

    def invoke(name, command, stdout=None):
        command = [str(x) for x in command]
        try:
            result = subprocess.run(command, cwd=gate.parents[2], env=env,
                                    capture_output=True, timeout=120)
            passed = result.returncode == 0
            if stdout is not None:
                passed = passed and result.stdout.replace(b"\r\n", b"\n") == stdout
            record = {"name": name, "command": command, "pass": passed,
                      "exit": result.returncode, "stdout": result.stdout.decode(errors="replace"),
                      "stderr": result.stderr.decode(errors="replace")}
        except subprocess.TimeoutExpired:
            record = {"name": name, "command": command, "pass": False, "timeout": True}
        records.append(record)
        print(name, "PASS" if record["pass"] else "FAIL", flush=True)
        if not record["pass"]:
            print(json.dumps(record, ensure_ascii=False, indent=2))
        return record["pass"]

    for opt in (0, 2):
        exe = output / (f"methods-O{opt}" + (".exe" if os.name == "nt" else ""))
        command = [compiler, "--src=file", gate / "main.vyx", "--emit=exe", f"-O{opt}", "-o", exe]
        if invoke(f"O{opt}-compile", command):
            invoke(f"O{opt}-run", [exe], b"primitive string methods ok\n")
    report = {"compiler": str(compiler), "sha256": hashlib.sha256(compiler.read_bytes()).hexdigest(),
              "records": records, "status": "pass" if all(x["pass"] for x in records) else "fail"}
    (output / "results.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(output / "results.json")
    return 0 if report["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
