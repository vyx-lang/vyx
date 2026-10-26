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
    parser.add_argument("--build-root", type=Path, help="Also check the actual driver helper with bootstrap interfaces")
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
            preview = dict(record)
            for channel in ("stdout", "stderr"):
                preview[channel + "_bytes"] = len(preview[channel].encode())
                preview[channel] = preview[channel][:2048]
            print(json.dumps(preview, ensure_ascii=False), flush=True)

    version = run("identity", [compiler, "--version"])
    driver_identity = None
    if args.build_root:
        build = args.build_root.resolve(strict=True)
        driver = build / "src/core/driver/main.vyx"
        text = driver.read_text(encoding="utf-8")
        start = text.index("fn cli_normalize_root_names(")
        end = text.index("\n}\n", start) + 3
        helper = text[start:end]
        cases = [("first,second", "first|second"),
                 (" first , second | third ", "first|second|third"),
                 ("choose<i32,i64>,third", "choose<i32,i64>|third"),
                 ("factory<(i32,i64),fn(i32,i64)->i32>,third",
                  "factory<(i32,i64),fn(i32,i64)->i32>|third"),
                 ("long_first_root_name_for_heap_storage, long_second_root_name_for_heap_storage",
                  "long_first_root_name_for_heap_storage|long_second_root_name_for_heap_storage")]
        source = output / "driver-helper.vyx"
        assertions = "\n".join("    assert_eq(cli_normalize_root_names(" + json.dumps(value)
                               + "), " + json.dumps(expected) + ");" for value, expected in cases)
        source.write_text("use std.core;\nuse std.string;\n" + helper
                          + "\nfn main() -> i32 {\n" + assertions + "\n    return 0;\n}\n", encoding="utf-8")
        interfaces = build / "out/.vyx_interfaces/boot"
        if not interfaces.is_dir():
            raise ValueError(f"Missing bootstrap interface directory: {interfaces}")
        record = run("driver-helper-bootstrap-interfaces", [compiler, "--src=file", source,
                     "--interface-root", interfaces, "--emit=obj", "-o", output / "driver-helper.obj"])
        check(record, record["exit"] == 0)
        record = run("driver-helper-aot", [compiler, "--src=file", source, "--run=aot", "-O2"])
        check(record, record["exit"] == 0)
        views = gate.parent / "loop-call-views/fixtures"
        record = run("primitive-clone-bootstrap-interfaces", [compiler, "--src=file", views / "clone_results.vyx",
                     "--interface-root", interfaces, "--emit=obj", "-o", output / "clone-results.obj"])
        check(record, record["exit"] == 0)
        record = run("primitive-clone-provider-shadow", [compiler, "--src=file",
                     views / "clone_interface_shadow.vyx", "--interface-root", interfaces,
                     "--emit=obj", "-o", output / "clone-interface-shadow.obj"])
        check(record, record["exit"] == 0)
        ir = output / "clone-interface-cleanup.ll"
        record = run("primitive-clone-interface-cleanup", [compiler, "--src=file",
                     views / "clone_interface_cleanup.vyx", "--interface-root", interfaces,
                     "--emit=ir", "-O0", "-o", ir])
        ir_text = ir.read_text(encoding="utf-8") if ir.is_file() else ""
        main_body = re.search(r"^define[^\n]*@main\([^\n]*\)[^\n]*\{\n(.*?)^\}", ir_text, re.M | re.S)
        drops = re.findall(r"\b(?:call|invoke)\b[^\n]*@([\w.$]*_F_str_5Fdrop_[\w.$]*)\(",
                           main_body.group(0) if main_body else "")
        check(record, len(drops) == 2, main_str_drops=drops)
        lifetime = gate.parent / "full-expression"
        positives = [("builder-return", views / "builder_results.vyx", "builder results PASS\n"),
                     ("clone-return", views / "clone_results.vyx", "clone results PASS\n"),
                     ("borrowed-lookalikes", lifetime / "lookalike_string_calls.vyx",
                      "borrowed class field must remain valid until its owner is dropped\n" * 9
                      + "lookalike-string-calls=ok\n"),
                     ("string-drop-identity", lifetime / "string_drop_identity.vyx", "string-drop-identity=ok\n"),
                     ("string-control-flow", lifetime / "string_control_flow.vyx", "string-control-flow=ok\n")]
        for name, source, expected in positives:
            for optimization in (0, 2):
                executable = output / (f"{name}-O{optimization}" + (".exe" if os.name == "nt" else ""))
                record = run(f"{name}-O{optimization}-compile", [compiler, "--src=file", source,
                             "--emit=exe", "-o", executable, f"-O{optimization}"])
                check(record, record["exit"] == 0)
                if record["passed"]:
                    record = run(f"{name}-O{optimization}-run", [executable])
                    check(record, record["stdout"].replace("\r\n", "\n") == expected)
        for name in ("dangling_view", "dangling_alias", "dangling_call", "dangling_clone_name",
                     "dangling_assignment", "dangling_loop", "dangling_branch", "dangling_scalar"):
            artifact = output / (name + ".obj")
            record = run(name, [compiler, "--src=file", views / (name + ".vyx"),
                               "--emit=obj", "-o", artifact])
            record["passed"] = record["exit"] == 1
            check(record, "error: E3102:" in record["stdout"] + record["stderr"] and not artifact.exists())
        artifact = output / "returned_string_slices.obj"
        record = run("returned-string-slices", [compiler, "--src=file", lifetime / "returned_string_slices.vyx",
                     "--emit=obj", "-o", artifact])
        record["passed"] = record["exit"] == 1
        diagnostics = record["stdout"] + record["stderr"]
        check(record, all(fragment in diagnostics for fragment in
                         ("error: E3102:", "return owner.substring(", "return owner.trim()")) and not artifact.exists())
        imported = output / "interfaces/audit"
        imported.mkdir(parents=True)
        (imported / "borrowing.vyi").write_text(
            "module audit.borrowing;\nuse std.string;\n"
            "public class BorrowingOwner {\n    public data: String;\n"
            "    public fn clone(&self) -> str;\n    public fn drop(&mut self);\n}\n", encoding="utf-8")
        source = output / "imported-borrow.vyx"
        source.write_text("use std.string;\nuse audit.borrowing;\nfn invalid() -> str {\n"
                          "    let owner = BorrowingOwner { data: String.from(\"owned bytes\") };\n"
                          "    return owner.clone();\n}\nfn main() -> i32 { print(invalid()); return 0; }\n",
                          encoding="utf-8")
        artifact = output / "imported-borrow.obj"
        record = run("imported-clone-view-rejected", [compiler, "--src=file", source,
                     "--interface-root", output / "interfaces", "--emit=obj", "-o", artifact])
        record["passed"] = record["exit"] == 1
        check(record, "error: E3102:" in record["stdout"] + record["stderr"] and not artifact.exists())
        support = output / "interfaces/support"
        support.mkdir(parents=True)
        record = run("generic-scope-interface", [compiler, "--src=file", lifetime / "support/scoped_library.vyx",
                     "--emit=vyi", "--vyi-shallow", "-o", support / "scoped_library.vyi"])
        check(record, record["exit"] == 0)
        for optimization in (0, 2):
            executable = output / (f"generic-call-scope-O{optimization}" + (".exe" if os.name == "nt" else ""))
            record = run(f"generic-call-scope-O{optimization}-compile", [compiler, "--src=file",
                         lifetime / "generic_call_scope.vyx", "--interface-root", output / "interfaces",
                         "--emit=exe", f"-O{optimization}", "-o", executable])
            check(record, record["exit"] == 0)
            if record["passed"]:
                record = run(f"generic-call-scope-O{optimization}-run", [executable])
                check(record, record["stdout"] == "generic-call-scope=ok\n")
        artifact = output / "generic-call-no-overload.obj"
        record = run("generic-call-no-overload", [compiler, "--src=file", lifetime / "generic_call_no_overload.vyx",
                     "--interface-root", output / "interfaces", "--emit=obj", "-o", artifact])
        record["passed"] = record["exit"] == 1
        check(record, "error: E2200:" in record["stdout"] + record["stderr"] and not artifact.exists())
        driver_identity = {"path": str(driver), "sha256": hashlib.sha256(driver.read_bytes()).hexdigest()}
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
                "version": version["stdout"], "records": records, "passed": passed,
                "driver_source": driver_identity}
    for name in ("vyx_compiler_backend.dll", "vyx_runtime.lib", "libvyx_compiler_backend.so",
                 "libvyx_compiler_backend.dylib", "libvyx_runtime.a"):
        path = compiler.parent / name
        if path.is_file():
            identity[name] = hashlib.sha256(path.read_bytes()).hexdigest()
    report = output / "verification.json"
    report.write_text(json.dumps(identity, ensure_ascii=False, indent=2), encoding="utf-8")
    print("report:", report)
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
