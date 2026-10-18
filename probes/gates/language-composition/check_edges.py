#!/usr/bin/env python3
"""Targeted ownership/protocol edges complement the generated grammar."""
import argparse
import json
import re
import shutil
from pathlib import Path
from suite import Executor, compiler_identity, digest, log_text, unique_dir, write_json


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--optimizations", default="0,2")
    parser.add_argument("--cgus", default="default,4")
    parser.add_argument("--cases", help="explicit fixture names; omitted fixtures are recorded")
    args = parser.parse_args()
    out = unique_dir(args.out)
    fixtures = Path(__file__).resolve().parent / "edges"
    expectations = json.loads((fixtures / "expected.json").read_text(encoding="utf8"))
    omitted = []
    if args.cases:
        selected = args.cases.split(",")
        if len(set(selected)) != len(selected) or any(name not in expectations for name in selected):
            parser.error("cases must be unique fixture names")
        omitted = [name for name in expectations if name not in selected]
        expectations = {name: expectations[name] for name in selected}
    engine = Executor(Path(args.compiler), 120, 2048)
    rows = []
    try:
        identity = compiler_identity(engine)
        for name, expected in expectations.items():
            for opt in args.optimizations.split(","):
                for cgu in args.cgus.split(","):
                    directory = out / f"{name}-O{opt}-cgu{cgu}"
                    directory.mkdir()
                    source = directory / "main.vyx"
                    shutil.copyfile(fixtures / f"{name}.vyx", source)
                    negative = "diagnostic" in expected
                    exe = directory / "probe.exe"
                    build = engine.execute(engine.compiler, ["--src=file", source,
                        "--emit=obj" if negative else "--emit=exe", f"-O{opt}", "-o",
                        directory / "probe.obj" if negative else exe], directory, "compile", cgu)
                    result = {"name": name, "opt": opt, "cgu": cgu,
                              "source_sha256": digest(source), "build": build, "expected": expected}
                    diagnostic = log_text(directory / "compile.stdout.log") + log_text(directory / "compile.stderr.log")
                    status = build["result"]
                    resource_failure = any(status.get(k) for k in ("TimedOut", "MemoryExceeded", "DialogCaught"))
                    if negative:
                        result["diagnostic"] = diagnostic
                        result["pass"] = not resource_failure and status["ExitCode"] == 1 and re.search(expected["diagnostic"], diagnostic) is not None
                    elif not resource_failure and status["ExitCode"] == 0:
                        run = engine.execute(exe, [], directory, "run", cgu)
                        result["run"] = run
                        result["actual"] = log_text(directory / "run.stdout.log").replace("\r\n", "\n")
                        result["pass"] = run["result"]["ExitCode"] == 0 and result["actual"] == expected["output"] and not log_text(directory / "run.stderr.log")
                    else:
                        result["diagnostic"] = diagnostic
                        result["pass"] = False
                    rows.append(result)
                    write_json(directory / "result.json", result)
                    print(f"{name}/O{opt}/cgu{cgu}: {'pass' if result['pass'] else 'FAIL'}", flush=True)
        write_json(out / "results.json", {"compiler": identity, "results": rows,
            "omitted_fixtures": omitted, "passed": sum(row["pass"] for row in rows),
            "failed": sum(not row["pass"] for row in rows)})
    finally:
        engine.close()
    return 0 if all(row["pass"] for row in rows) else 1


if __name__ == "__main__":
    raise SystemExit(main())
