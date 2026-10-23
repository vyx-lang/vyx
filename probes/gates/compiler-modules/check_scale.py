#!/usr/bin/env python3
"""Generate named-module symbol growth regressions and run native AOT paths."""
import argparse
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "language-composition"))
from suite import Executor, compiler_identity, digest, log_text, unique_dir, write_json


def succeeded(run):
    status = run["result"]
    return status["ExitCode"] == 0 and not any(
        status.get(key) for key in ("TimedOut", "MemoryExceeded", "DialogCaught"))


def function_source(count, module="scale.functions"):
    declarations = "".join(
        f"public fn symbol_{i}() -> i64 {{ return {i}; }}\n" for i in range(count))
    body = "var total: i64 = 0;\n" + "".join(
        f"total += symbol_{i}();\n" for i in range(count))
    return (f"module {module};\n" + declarations + "fn main() -> i32 {\n" + body
            + f"if (total != {count * (count - 1) // 2} as i64) {{ return 1; }}\nreturn 0;\n}}\n")


def class_source(count):
    declarations = "".join(
        f"public class Record_{i} {{ public value: i64; }}\n" for i in range(count))
    body = "var total: i64 = 0;\n" + "".join(
        f"{{ let item = scale.types.Record_{i} {{ value: {i} }}; total += item.value; }}\n"
        for i in range(count))
    return ("module scale.types;\n" + declarations + "fn main() -> i32 {\n" + body
            + f"if (total != {count * (count - 1) // 2} as i64) {{ return 1; }}\nreturn 0;\n}}\n")


def write_text(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf8")


def make_project(directory, count):
    write_text(directory / "Vyx.toml", '''[package]
name = "named_module_scale"
version = "0.1.0"
entry = "src/main.vyx"
[build]
output_dir = "target"
[target.named_module_scale]
type = "executable"
entry = "src/main.vyx"
auto_sources = false
parallel_vyx = true
sources = ["src/first.vyx", "src/middle.vyx", "src/last.vyx", "src/noise.vyx"]
''')
    # One logical module spans several physical files. The consumer calls every
    # function; checking only its first or last symbol can miss lost prefixes.
    boundaries = [0, count // 3, 2 * count // 3, count]
    for index, name in enumerate(("first", "middle", "last")):
        write_text(directory / f"src/{name}.vyx", "module scale.library;\n" + "".join(
            f"public fn symbol_{i}() -> i64 {{ return {i}; }}\n"
            for i in range(boundaries[index], boundaries[index + 1])))
    write_text(directory / "src/noise.vyx", "module scale.noise;\n" + "".join(
        f"public fn symbol_{i}() -> i64 {{ return {-i - 1}; }}\n"
        for i in (0, count // 2, count - 1)))
    body = "".join(f"total += scale.library.symbol_{i}();\n" for i in range(count))
    noise = "".join(
        f"if (scale.noise.symbol_{i}() != {-i - 1} as i64) {{ return 2; }}\n"
        for i in (0, count // 2, count - 1))
    write_text(directory / "src/main.vyx", "module scale.app;\nuse scale.library;\nuse scale.noise;\n"
               + "fn main() -> i32 {\nvar total: i64 = 0;\n" + body + noise
               + f"if (total != {count * (count - 1) // 2} as i64) {{ return 1; }}\nreturn 0;\n}}\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--functions", default="2047,2048,2049,4097")
    parser.add_argument("--classes", default="1023,1024,1025,2049")
    parser.add_argument("--skip-project", action="store_true")
    args = parser.parse_args()
    try:
        functions = [int(x) for x in args.functions.split(",")]
        classes = [int(x) for x in args.classes.split(",")]
        if any(n < 1 or n > 20000 for n in functions + classes):
            raise ValueError("counts must be between 1 and 20000")
        if len(set(functions)) != len(functions) or len(set(classes)) != len(classes):
            raise ValueError("count lists must not contain duplicates")
    except ValueError as error:
        parser.error(str(error))
    out = unique_dir(args.out)
    engine = Executor(Path(args.compiler), 180, 2048)
    # A project build may retain a coordinator and a Vyx child. Bound admission
    # independently of -j20 so this gate can run on ordinary development hosts.
    engine.env["VYX_BUILD_VYX_SLOTS"] = "1"
    engine.env["VYX_BUILD_MEMORY_SLOTS"] = "2"
    rows = []
    identity = {}

    def record(name, runs, passed, **extra):
        rows.append({"name": name, "runs": runs, "passed": passed, **extra})
        write_json(out / "results.json", {"compiler": identity, "results": rows,
                   "passed": sum(row["passed"] for row in rows),
                   "failed": sum(not row["passed"] for row in rows)})
        print(f"{name}: {'pass' if passed else 'FAIL'}", flush=True)

    try:
        identity = compiler_identity(engine)
        for kind, counts, generator in (("functions", functions, function_source),
                                        ("classes", classes, class_source)):
            for count in counts:
                directory = out / f"{kind}-{count}"
                directory.mkdir()
                source = directory / "main.vyx"
                write_text(source, generator(count))
                parse = engine.execute(engine.compiler, ["--src=file", source,
                                       "--stop-after-parse"], directory, "parse", "1")
                record(f"{kind}-{count}/parse", [parse], succeeded(parse), source_sha256=digest(source))
                # AOT also checks name binding, not just syntactic acceptance.
                variants = [("0", "default")]
                if count == max(counts):
                    variants += [("2", "default"), ("2", "4")]
                for opt, cgu in variants:
                    label = f"O{opt}-cgu{cgu}"
                    exe = directory / (label + ".exe")
                    build = engine.execute(engine.compiler, ["--src=file", source,
                        "--emit=exe", f"-O{opt}", "-o", exe], directory, label + "-build", cgu)
                    runs = [build]
                    if succeeded(build):
                        runs.append(engine.execute(exe, [], directory, label + "-run", cgu))
                    record(f"{kind}-{count}/{label}", runs, len(runs) == 2 and all(map(succeeded, runs)),
                           source_sha256=digest(source))
                if kind == "functions" and count == max(counts):
                    interface = directory / "module.vyi"
                    emit = engine.execute(engine.compiler, ["--src=file", source, "--emit=vyi",
                        "--vyi-shallow", "-o", interface], directory, "vyi", "1")
                    text = interface.read_text(encoding="utf8") if interface.exists() else ""
                    names = set(re.findall(r"\bfn (symbol_\d+)\(", text))
                    record(f"{kind}-{count}/interface", [emit], succeeded(emit)
                           and names == {f"symbol_{i}" for i in range(count)})
        if not args.skip_project:
            count = max(functions)
            project = out / "project"
            make_project(project, count)
            expected = count * (count - 1) // 2
            for mode in ("cold", "warm", "incremental"):
                if mode == "incremental":
                    source = project / "src/last.vyx"
                    write_text(source, source.read_text(encoding="utf8").replace(
                        f"return {count - 1};", f"return {count - 1} + 7;"))
                    entry = project / "src/main.vyx"
                    write_text(entry, entry.read_text(encoding="utf8").replace(
                        f"total != {expected}", f"total != {expected + 7}"))
                build = engine.execute(engine.compiler, ["--src=project", project,
                    "--target", "named_module_scale", "--run=aot", "-O2", "-j20"],
                    project, mode, "default")
                output = log_text(project / (mode + ".stdout.log"))
                tasks = len(re.findall(r"^\[\d+/\d+\] compile ", output, re.M))
                passed = succeeded(build) and "[run] " in output
                if mode == "warm":
                    passed = passed and tasks == 0
                if mode == "incremental":
                    passed = passed and tasks >= 1 and "module_scale_library" in output
                record(f"project/{mode}", [build], passed, compile_tasks=tasks,
                       sources={str(path.relative_to(project)): digest(path)
                                for path in sorted((project / "src").glob("*.vyx"))})
    finally:
        engine.close()
    return 0 if rows and all(row["passed"] for row in rows) else 1


if __name__ == "__main__":
    raise SystemExit(main())
