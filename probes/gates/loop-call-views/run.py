#!/usr/bin/env python3
"""Check project duplicate definitions, call conditions and loop-local owners."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "language-composition"))
from suite import Executor, compiler_identity, digest, log_text, unique_dir, write_json


def large_body(count):
    declarations = ["use std.core;", "var calls: i64 = 0;",
                    "fn tick(v: i64) -> i64 { calls += 1; return v; }",
                    "fn large() -> i64 {", "    var sum: i64 = 0;"]
    for i in range(1, count + 1):
        declarations.append(f"    let value_{i} = tick({i}i64);")
        declarations.append(f"    sum += value_{i};")
        if i == count // 2:
            declarations.append('    print("middle");')
    declarations += ["    return sum;", "}", "fn repeated() -> i64 {",
                     "    var i: i32 = 0;", "    var sum: i64 = 0;",
                     "    while (i < 3) { sum += large(); i += 1; }",
                     "    return sum;", "}", "fn main() -> i32 {",
                     f"    assert_eq(repeated(), {3 * count * (count + 1) // 2}i64);",
                     f"    assert_eq(calls, {3 * count}i64);",
                     '    print("large body PASS");', "    return 0;", "}"]
    return "\n".join(declarations) + "\n"


def project(directory, sources):
    for name, text in sources.items():
        (directory / name).write_text(text, encoding="utf-8")
    names = json.dumps(list(sources))
    (directory / "Vyx.toml").write_text(
        '[package]\nname="duplicate_gate"\nversion="0.1.0"\n'
        '[target.app]\ntype="exe"\nentry="main.vyx"\nauto_sources=false\n'
        f'sources={names}\n', encoding="utf-8")


def failed_resource(result):
    return any(result.get(k) for k in ("TimedOut", "MemoryExceeded", "DialogCaught"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--optimizations", default="0,2")
    parser.add_argument("--cgus", default="1,4,8")
    args = parser.parse_args()
    out = unique_dir(args.out)
    engine = Executor(Path(args.compiler), 90, 2048)
    engine.env["VYX_STD_PACKAGES"] = str(HERE.parents[2] / "bootstrap_compiler/std_packages")
    engine.env["VYX_RUNTIME_ROOT"] = str(engine.compiler.parent)
    rows = []

    def check(name, directory, arguments, expected=None, diagnostic=None, cgu="1"):
        build = engine.execute(engine.compiler, arguments, directory, "compile", cgu)
        text = log_text(directory / "compile.stdout.log") + log_text(directory / "compile.stderr.log")
        status = build["result"]
        row = {"name": name, "build": build, "diagnostic": text,
               "sources": {str(p.relative_to(directory)): digest(p) for p in directory.rglob("*.vyx")}}
        if diagnostic:
            row["pass"] = not failed_resource(status) and status["ExitCode"] == 1 and re.search(diagnostic, text) is not None
        elif status["ExitCode"] == 0 and not failed_resource(status):
            exe = directory / ("probe.exe" if os.name == "nt" else "probe")
            run = engine.execute(exe, [], directory, "run", cgu)
            row["run"] = run
            actual = log_text(directory / "run.stdout.log").replace("\r\n", "\n")
            row["actual"] = actual
            row["expected"] = expected
            row["pass"] = not failed_resource(run["result"]) and run["result"]["ExitCode"] == 0 and actual == expected and not log_text(directory / "run.stderr.log")
        else:
            row["pass"] = False
        rows.append(row)
        write_json(directory / "result.json", row)
        print(f"{name}: {'PASS' if row['pass'] else 'FAIL'}", flush=True)

    try:
        identity = compiler_identity(engine)
        positives = {"byte_scan": "3\n", "view_loop": "44\n",
                     "conditions": "conditions PASS\n", "nested_owner": "nested owner PASS\n",
                     "borrowed_return": "borrowed return PASS\n",
                     "view_results": "long input uses heap storage rather than inline storage\nview results PASS\n",
                     "view_mutation": "view mutation PASS\n",
                     "builder_results": "builder results PASS\n",
                     "clone_results": "clone results PASS\n"}
        for name, expected in positives.items():
            for opt in args.optimizations.split(","):
                for cgu in args.cgus.split(","):
                    directory = out / f"{name}-O{opt}-cgu{cgu}"
                    directory.mkdir()
                    shutil.copyfile(HERE / "fixtures" / f"{name}.vyx", directory / "main.vyx")
                    exe = directory / ("probe.exe" if os.name == "nt" else "probe")
                    check(directory.name, directory, ["--src=file", directory / "main.vyx", "--emit=exe", f"-O{opt}", "-o", exe], expected, cgu=cgu)
        for count in (96, 224, 416):
            for opt in args.optimizations.split(","):
                for cgu in args.cgus.split(","):
                    directory = out / f"large-{count}-O{opt}-cgu{cgu}"
                    directory.mkdir()
                    (directory / "main.vyx").write_text(large_body(count), encoding="utf-8")
                    exe = directory / ("probe.exe" if os.name == "nt" else "probe")
                    check(directory.name, directory, ["--src=file", directory / "main.vyx", "--emit=exe", f"-O{opt}", "-o", exe], "middle\n" * 3 + "large body PASS\n", cgu=cgu)

        for name, code in (("plain", "E2100"), ("generic", "E2101"), ("cross_file", "E2100")):
            directory = out / f"duplicate-{name}"
            directory.mkdir()
            header = "module duplicate_gate;\n"
            fn1 = 'public fn selected' + ('<T>(x: T) -> T { return x; }\n' if name == "generic" else '(x: i32) -> i32 { return x; }\n')
            fn2 = fn1
            main_source = header + 'fn main() -> i32 { print(selected(1)); return 0; }\n'
            if name == "cross_file":
                main_source += fn1
                worker = header + fn2
            else:
                worker = header + fn1 + fn2
            project(directory, {"main.vyx": main_source, "worker.vyx": worker})
            # The diagnostic must identify the second definition in worker.vyx.
            line = 2 if name == "cross_file" else 3
            check(directory.name, directory, ["--src=project", directory, "--target", "app", "-j2"],
                  diagnostic=rf"worker\.vyx:{line}:\d+: error: {code}: (?:duplicate function|redefinition of generic function).*selected")

        directory = out / "valid-overloads-externs"
        directory.mkdir()
        project(directory, {
            "main.vyx": 'module duplicate_gate;\nextern "C" { fn abs(x: i32) -> i32; }\nfn main() -> i32 { print(selected(1), "/", selected(3i64), "/", abs(-7)); return 0; }\n',
            "worker.vyx": 'module duplicate_gate;\nextern "C" { fn abs(x: i32) -> i32; }\npublic fn selected(x: i32) -> i32 { return x + 1; }\npublic fn selected(x: i64) -> i64 { return x + 2; }\n'})
        exe = directory / ("probe.exe" if os.name == "nt" else "probe")
        check(directory.name, directory, ["--src=project", directory, "--target", "app", "-j2", "-o", exe], "2/5/7\n")

        directory = out / "duplicate-mutref-project"
        shutil.copytree(HERE / "fixtures/duplicate_project", directory,
                        ignore=shutil.ignore_patterns(".cache", "target", "*.exe"))
        check(directory.name, directory, ["--src=project", directory, "--target", "app", "-j2"],
              diagnostic=r"worker\.vyx:4:\d+: error: E2100: duplicate function selected")
        for name in ("duplicate_body", "duplicate_mutref_body"):
            directory = out / name
            directory.mkdir()
            shutil.copyfile(HERE / f"fixtures/{name}.vyx", directory / "main.vyx")
            check(name, directory, ["--src=file", directory / "main.vyx", "--emit=obj", "-o", directory / "probe.obj"],
                  diagnostic=r"main\.vyx:\d+:\d+: error: E2100: duplicate function selected")

        for negative in (False, True):
            directory = out / ("cross-file-dangling" if negative else "cross-file-copy")
            directory.mkdir()
            worker = ('module views.worker;\nuse std.string;\n'
                      'public fn produce() -> str {\n'
                      '    let owner = String.from("cross-file local storage must not escape");\n'
                      '    return owner.raw();\n}\n') if negative else (
                      'module views.worker;\nuse std.string;\n'
                      'public fn copy<T>(owner: &String, marker: T) -> str { return owner.raw().clone(); }\n'
                      'public fn produce() -> str {\n'
                      '    let owner = String.from("cross-file view PASS");\n'
                      '    return copy(&owner, 1);\n}\n')
            project(directory, {"main.vyx": 'module views.main;\nuse views.worker;\nfn main() -> i32 { print(produce()); return 0; }\n',
                                "worker.vyx": worker})
            exe = directory / ("probe.exe" if os.name == "nt" else "probe")
            check(directory.name, directory, ["--src=project", directory, "--target", "app", "-j2", "-o", exe],
                  expected="cross-file view PASS\n" if not negative else None,
                  diagnostic=r"worker\.vyx:5:\d+: error: E3102:" if negative else None)

        for name in ("dangling_view", "dangling_alias", "dangling_call", "dangling_clone_name",
                     "dangling_assignment", "dangling_loop", "dangling_branch", "dangling_scalar"):
            directory = out / name
            directory.mkdir()
            shutil.copyfile(HERE / f"fixtures/{name}.vyx", directory / "main.vyx")
            check(directory.name, directory, ["--src=file", directory / "main.vyx", "--emit=obj", "-o", directory / "probe.obj"],
                  diagnostic=r"error: E3102: returned view borrows an owning temporary")
        write_json(out / "results.json", {"compiler": identity, "results": rows,
                   "passed": sum(r["pass"] for r in rows), "failed": sum(not r["pass"] for r in rows)})
    finally:
        engine.close()
    return 0 if rows and all(r["pass"] for r in rows) else 1


if __name__ == "__main__":
    raise SystemExit(main())
