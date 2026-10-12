#!/usr/bin/env python3
"""AOT string views require a language-item identity and the complete layout."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True, type=Path)
    parser.add_argument("--runtime-dir", type=Path)
    args = parser.parse_args()
    compiler = args.compiler.resolve(strict=True)
    runtime = (args.runtime_dir or compiler.parent).resolve(strict=True)
    gate = Path(__file__).resolve().parent
    repo = gate.parents[2]
    output = gate / ".cache"
    output.mkdir(parents=True, exist_ok=True)
    environment = os.environ.copy()
    environment["PATH"] = str(runtime) + os.pathsep + environment.get("PATH", "")
    records: list[dict] = []

    def invoke(name: str, command: list[str], expected_exit: int,
               expected_output: bytes | None = None, diagnostic: str = "", location: str = "") -> bool:
        try:
            result = subprocess.run(command, cwd=repo, env=environment, capture_output=True, timeout=90)
            text = (result.stdout + result.stderr).decode("utf-8", errors="replace")
            passed = result.returncode == expected_exit
            if expected_output is not None:
                passed = passed and result.stdout.replace(b"\r\n", b"\n") == expected_output
            if diagnostic:
                passed = passed and diagnostic in text and location in text
            record = {"name": name, "command": command, "exit": result.returncode, "pass": passed,
                      "stdout": result.stdout.decode("utf-8", errors="replace"),
                      "stderr": result.stderr.decode("utf-8", errors="replace")}
        except subprocess.TimeoutExpired:
            passed = False
            record = {"name": name, "command": command, "pass": False, "timeout": True}
        records.append(record)
        print(f"{name}: {'PASS' if passed else 'FAIL'}", flush=True)
        if not passed:
            print(json.dumps(record, ensure_ascii=False, indent=2), flush=True)
        return passed

    positive = {
        "std_string": b"a\x00b\na\x00bc\nalias\x00ok\nqualified\n"
                      b"01234567890123456789012345678901\x00y\n"
                      b"01234567890123456789012345678901\x00yz\nidentity=ok\n",
        "declared_string": b"declared=a\x00b\ndeclared-again=a\x00b\n",
    }
    for name, expected in positive.items():
        for opt in (0, 2):
            executable = output / (f"{name}-O{opt}" + (".exe" if os.name == "nt" else ""))
            executable.unlink(missing_ok=True)
            if invoke(f"{name}-O{opt}-compile", [str(compiler), "--src=file", str(gate / f"{name}.vyx"),
                      "--emit=exe", f"-O{opt}", "-o", str(executable), "-L", str(runtime), "-l", "vyx_runtime"], 0):
                invoke(f"{name}-O{opt}-run", [str(executable)], 0, expected_output=expected)

    standard_fields = [("ptr", "rawptr"), ("len", "i64"), ("cap", "i64"), ("owned", "i64"),
                       ("inline0", "i64"), ("inline1", "i64"), ("inline2", "i64")]
    invalid: dict[str, tuple[str, list[tuple[str, str]]]] = {
        "unmarked-complete": ("", standard_fields),
        "unmarked-i32": ("", [(name, "i32") for name in ("ptr", "len", "cap", "owned", "inline0")]),
        "different-lang-item": ('@[lang_item("other")]', standard_fields),
        "marked-missing-inline2": ('@[lang_item("string")]', standard_fields[:-1]),
        "marked-extra-field": ('@[lang_item("string")]', standard_fields + [("extra", "i64")]),
        "marked-reordered-sso": ('@[lang_item("string")]', standard_fields[:4] +
                                  [standard_fields[4], standard_fields[6], standard_fields[5]]),
    }
    for index, (field, _) in enumerate(standard_fields):
        wrong = standard_fields.copy()
        wrong[index] = (field, "i32")
        invalid[f"marked-wrong-{field}"] = ('@[lang_item("string")]', wrong)

    for name, (attribute, fields) in invalid.items():
        declarations = "\n".join(f"    public {field}: {kind};" for field, kind in fields)
        values = ", ".join(f"{field}: {'null' if kind == 'rawptr' else '0'}" for field, kind in fields)
        source = ("module application.text;\n" + attribute + "\nclass String {\n" + declarations +
                  "\n}\nfn main() -> i32 {\n    let value = String { " + values +
                  " };\n    print(value);\n    return 0;\n}\n")
        file = output / f"{name}.vyx"
        file.write_text(source, encoding="utf-8")
        line = source[:source.index("    print(value)")].count("\n") + 1
        executable = output / (name + (".exe" if os.name == "nt" else ""))
        executable.unlink(missing_ok=True)
        invoke(name, [str(compiler), "--src=file", str(file), "--emit=exe", "-O0", "-o", str(executable)], 1,
               diagnostic="cannot print value type", location=f":{line}:")
        if executable.exists():
            records.append({"name": name + "-no-artifact", "pass": False})

    report = {"compiler": str(compiler), "sha256": hashlib.sha256(compiler.read_bytes()).hexdigest(),
              "runtime_dir": str(runtime), "records": records, "passed": all(record["pass"] for record in records)}
    report["runtime_artifacts"] = [
        {"path": str(file), "sha256": hashlib.sha256(file.read_bytes()).hexdigest()}
        for name in (("vyx_runtime.lib", "vyx_compiler_backend.dll") if os.name == "nt"
                     else ("libvyx_runtime.a", "libvyx_compiler_backend.so", "libvyx_compiler_backend.dylib"))
        if (file := runtime / name).is_file()
    ]
    (output / "verification.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
