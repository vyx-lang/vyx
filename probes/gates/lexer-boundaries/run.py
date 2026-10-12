#!/usr/bin/env python3
"""Located lexical failures and exact AOT string/interpolation boundaries."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
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
    records: list[dict] = []
    environment = os.environ.copy()
    environment["PATH"] = str(runtime) + os.pathsep + environment.get("PATH", "")

    def invoke(name: str, command: list[str], *, expected_exit: int,
               expected_output: bytes | None = None, diagnostic: str = "", location: str = "") -> bool:
        try:
            result = subprocess.run(command, cwd=repo, env=environment, capture_output=True, timeout=90)
            text = (result.stdout + result.stderr).decode("utf-8", errors="replace")
            passed = result.returncode == expected_exit
            if expected_output is not None:
                passed = passed and result.stdout.replace(b"\r\n", b"\n") == expected_output
            if diagnostic:
                passed = passed and diagnostic in text and location in text
            record = {"name": name, "command": command, "exit": result.returncode,
                      "pass": passed, "stdout": result.stdout.decode("utf-8", errors="replace"),
                      "stderr": result.stderr.decode("utf-8", errors="replace")}
        except subprocess.TimeoutExpired:
            passed = False
            record = {"name": name, "command": command, "pass": False, "timeout": True}
        records.append(record)
        print(f"{name}: {'PASS' if passed else 'FAIL'}", flush=True)
        if not passed:
            print(json.dumps(record, ensure_ascii=False, indent=2), flush=True)
        return passed

    expected = (b"a\x00b\n\x00x\x00\nleft\x0042\x00right\na\x00b\n"
                b"ordered=17:a\x00b:tail\ninterpolate=a\x00b\n"
                b"23:printf-bytes\n24:a\x00b\n"
                b"heap=01234567890123456789012345678901\x00y\n"
                b"heap-again=01234567890123456789012345678901\x00yz\n"
                b"multiline=}\n{\n\nraw=\\q } {\nnested=answer=ok\nlegacy=a\ncomments=3\nbytes=ok\n")
    for opt in (0, 2):
        executable = output / (f"strings-O{opt}" + (".exe" if os.name == "nt" else ""))
        if invoke(f"strings-O{opt}-compile", [str(compiler), "--src=file", str(gate / "strings.vyx"),
                  "--emit=exe", f"-O{opt}", "-j", "2", "-o", str(executable),
                  "-L", str(runtime), "-l", "vyx_runtime"], expected_exit=0):
            invoke(f"strings-O{opt}-run", [str(executable)], expected_exit=0, expected_output=expected)

    # The Windows compiler imports printf from its private backend. Exercise
    # that linkage explicitly to detect buffering across different CRTs.
    if os.name == "nt" and (runtime / "vyx_compiler_backend.dll").is_file():
        # Match the SDK's app-local DLL deployment; explicit -l only links the
        # import library and does not install its corresponding runtime DLL.
        shutil.copy2(runtime / "vyx_compiler_backend.dll", output / "vyx_compiler_backend.dll")
        executable = output / "strings-private-backend.exe"
        if invoke("strings-private-backend-compile", [str(compiler), "--src=file", str(gate / "strings.vyx"),
                  "--emit=exe", "-O0", "-j", "2", "-o", str(executable), "-L", str(runtime),
                  "-l", "vyx_compiler_backend", "-l", "vyx_runtime"], expected_exit=0):
            invoke("strings-private-backend-run", [str(executable)], expected_exit=0, expected_output=expected)

    # Physical LF/CR bytes are generated deliberately; source escapes such as
    # '\\n' and '\\r' remain valid and are exercised by the positive fixture.
    invalid = {
        "unclosed-comment": ("fn main() -> i32 { return 0; }\n/* unfinished\n", "E0105: unterminated block comment", ":2:1:"),
        "unclosed-nested-comment": ("fn main() -> i32 { return 0; }\n  /* outer /* inner */\n", "E0105: unterminated block comment", ":2:3:"),
        "char-lf": ("fn main() -> i32 {\n    let c = '\n';\n    return 0;\n}\n", "E0103: unterminated character literal", ":2:13:"),
        "char-cr": ("fn main() -> i32 {\n    let c = '\r';\n    return 0;\n}\n", "E0103: unterminated character literal", ":2:13:"),
        "string-cr": ('fn main() -> i32 {\n    let s = "a\rb";\n    return 0;\n}\n', "E0102: unterminated string literal", ":2:13:"),
        "unclosed-triple-in-interpolation": ('fn main() -> i32 {\n    print("${"""\n}\n");\n    return 0;\n}\n', "E0102: unterminated string interpolation", ":2:11:"),
        "invalid-string-escape": ('fn main() -> i32 {\n    let s = "\\q";\n    return 0;\n}\n', "E0101: unknown escape sequence", ":2:15:"),
    }
    for name, (source, diagnostic, location) in invalid.items():
        file = output / f"{name}.vyx"
        file.write_bytes(source.encode("utf-8"))
        artifact = output / (name + (".exe" if os.name == "nt" else ""))
        artifact.unlink(missing_ok=True)
        invoke(name, [str(compiler), "--src=file", str(file), "--emit=exe", "-o", str(artifact)],
               expected_exit=1, diagnostic=diagnostic, location=location)
        if artifact.exists():
            records.append({"name": name + "-no-artifact", "pass": False})

    # Preserve exact AST byte lengths at both sides of the former 1 MiB cap.
    # Include a NUL and tail byte without dumping the large literal to stdout.
    for length in (1_048_576, 1_048_577):
        file = output / f"large-literal-{length}.vyx"
        file.write_text('fn main() -> i32 {\n    let data = "' + ('x' * (length - 2)) + '\\0y";\n'
                        + f'    if (data.len != {length}) {{ print("actual-length=", data.len); return 9; }}\n'
                        + f'    if (data.charAt(0) != 120 || data.charAt({length - 2}) != 0'
                        + f' || data.charAt({length - 1}) != 121) {{ return 10; }}\n'
                        + '    print("large-literal=ok");\n    return 0;\n}\n', encoding="utf-8")
        executable = output / (f"large-literal-{length}" + (".exe" if os.name == "nt" else ""))
        if invoke(f"large-literal-{length}-compile", [str(compiler), "--src=file", str(file),
                  "--emit=exe", "-O0", "-o", str(executable), "-L", str(runtime), "-l", "vyx_runtime"],
                  expected_exit=0):
            invoke(f"large-literal-{length}-run", [str(executable)], expected_exit=0,
                   expected_output=b"large-literal=ok\n")

    report = {"compiler": str(compiler), "sha256": hashlib.sha256(compiler.read_bytes()).hexdigest(),
              "runtime_dir": str(runtime), "records": records,
              "passed": all(record["pass"] for record in records)}
    report["runtime_artifacts"] = [
        {"path": str(file), "sha256": hashlib.sha256(file.read_bytes()).hexdigest()}
        for name in ("vyx_runtime.lib", "libvyx_runtime.a", "vyx_compiler_backend.dll",
                     "libvyx_compiler_backend.so", "libvyx_compiler_backend.dylib")
        if (file := runtime / name).is_file()
    ]
    (output / "verification.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
