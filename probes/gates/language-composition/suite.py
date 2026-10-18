#!/usr/bin/env python3
"""Generate, execute, replay and shrink a finite Vyx grammar product."""
import argparse
from collections import Counter
from contextlib import contextmanager
from datetime import datetime, timezone
import hashlib
import itertools
import json
import os
from pathlib import Path
import random
import re
import queue
import shutil
import signal
import subprocess
import sys
import threading
import time

from model import (NEGATIVES, RULE_BY_NAME, RULES, TYPE_BASES, TYPE_CONSTRUCTORS, case_functions, case_id,
                   catalog, expected, main, oracle, recipes, support)

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def csv(text):
    return tuple(x.strip() for x in text.split(",") if x.strip())


def unique_dir(path):
    path = Path(path).resolve()
    # A corpus/run is immutable input evidence. Never overwrite another run.
    path.mkdir(parents=True, exist_ok=False)
    return path


def timestamp():
    return datetime.now().strftime("%Y%m%d-%H%M%S-%f")


def domain(args):
    names = csv(args.rules) if args.rules else tuple(r.name for r in RULES)
    if len(set(names)) != len(names) or any(n not in RULE_BY_NAME for n in names):
        raise ValueError("rules must be unique registered production names")
    values = tuple(int(x) for x in csv(args.values))
    if not values or len(set(values)) != len(values) or any(abs(x) > 1000 for x in values):
        raise ValueError("values must be unique integers in [-1000,1000]")
    forms = csv(args.forms)
    if not forms or len(set(forms)) != len(forms) or set(forms) - {"inline", "bound"}:
        raise ValueError("forms must be a unique subset of inline,bound")
    if not 0 <= args.depth <= 6 or not 0 <= args.negative_depth <= 6:
        raise ValueError("depth must be between 0 and 6 (oracle's overflow-safe bound)")
    if args.random_cases < 0:
        raise ValueError("random-cases cannot be negative")
    if not 0 <= args.type_depth <= 3 or not 0 <= args.type_context_depth <= 2:
        raise ValueError("type-depth is bounded at 3; type-context-depth at 2")
    return names, values, forms


def plan(args):
    names, values, forms = domain(args)
    if args.batch_size <= 0: raise ValueError("batch-size must be positive")
    positive = sum(len(names) ** d for d in range(args.depth + 1)) * len(values) * len(forms)
    typed = 0 if args.no_types else len(TYPE_BASES) * sum(len(TYPE_CONSTRUCTORS) ** d for d in range(args.type_depth + 1)) * sum(len(names) ** d for d in range(args.type_context_depth + 1)) * len(values) * len(forms)
    negative = 0 if args.no_negative else sum(len(names) ** d for d in range(args.negative_depth + 1)) * len(values) * len(forms) * len(NEGATIVES)
    return {"positive_exhaustive": positive + typed, "numeric_exhaustive": positive,
            "typed_exhaustive": typed, "negative_exhaustive": negative,
            "random_attempts": args.random_cases,
            "positive_batches": (positive + typed + args.batch_size - 1) // args.batch_size,
            "negative_programs": negative,
            "domain": {"rules": names, "values": values, "forms": forms,
                       "depth": args.depth, "negative_depth": args.negative_depth,
                       "type_depth": args.type_depth, "type_context_depth": args.type_context_depth},
            "default_execution_matrix_cells": 18,
            "unmodeled": catalog()["unmodeled"]}


def make_case(chain, leaf, form, negative=None, origin="exhaustive", type_recipe=None):
    case = {"rules": list(chain), "leaf": leaf, "form": form,
            "negative": negative, "origin": origin}
    if type_recipe: case["type_recipe"] = type_recipe
    case["id"] = case_id(case)
    case["expected"] = oracle(case) if not negative else None
    return case


def write_program(directory, cases, layout):
    directory.mkdir(parents=True, exist_ok=True)
    if layout == "file":
        sources = {"main.vyx": support(cases) + case_functions(cases) + main(cases)}
    elif layout == "project":
        sources = {"support.vyx": support(cases), "cases.vyx": case_functions(cases), "main.vyx": main(cases)}
    elif layout == "modules":
        sources = {"support.vyx": "module generated.support;\n" + support(cases),
                   "cases.vyx": "module generated.cases;\nuse generated.support;\n" + case_functions(cases),
                   "main.vyx": "module generated.main;\nuse generated.support;\nuse generated.cases;\n" + main(cases)}
    else:
        raise ValueError(f"unknown layout {layout}")
    for filename, content in sources.items():
        (directory / filename).write_text(content, encoding="utf-8")
    if layout != "file":
        (directory / "Vyx.toml").write_text('''[package]
name = "grammar_suite"
version = "0.1.0"
entry = "main.vyx"
[dependencies]
core = { path = "std:core", target = "std_core" }
collections = { path = "std:collections", target = "std_collections" }
[target.probe]
type = "executable"
entry = "main.vyx"
auto_sources = false
sources = ["support.vyx", "cases.vyx", "main.vyx"]
''', encoding="utf-8")
    write_json(directory / "recipes.json", cases)
    (directory / "expected.txt").write_text(expected(cases) if not cases[0]["negative"] else "", encoding="utf-8")
    return {name: digest(directory / name) for name in sources}


def generate(args):
    names, values, forms = domain(args)
    details = plan(args)
    if details["positive_exhaustive"] + details["negative_exhaustive"] + args.random_cases > args.max_cases:
        raise ValueError(f"plan has more than --max-cases={args.max_cases}; reduce domain explicitly or raise the bound; nothing was truncated")
    cases = [make_case(chain, leaf, form) for chain in recipes(names, args.depth)
             for leaf, form in itertools.product(values, forms)]
    if not args.no_types:
        cases += [make_case(chain, leaf, form, type_recipe={"base": base, "constructors": list(constructors)})
                  for base in TYPE_BASES for constructors in recipes(TYPE_CONSTRUCTORS, args.type_depth)
                  for chain in recipes(names, args.type_context_depth)
                  for leaf, form in itertools.product(values, forms)]
    if not args.no_negative:
        cases += [make_case(chain, leaf, form, bad) for bad in NEGATIVES
                  for chain in recipes(names, args.negative_depth)
                  for leaf, form in itertools.product(values, forms)]
    rng = random.Random(args.seed)
    ids = {c["id"] for c in cases}
    for _ in range(args.random_cases):
        chain = tuple(rng.choice(names) for _ in range(rng.randint(1, 6)))
        c = make_case(chain, rng.randint(-1000, 1000), rng.choice(forms), origin="random")
        if c["id"] not in ids:
            cases.append(c)
            ids.add(c["id"])
    rng.shuffle(cases)
    directory = unique_dir(args.out or HERE / ".runs" / ("corpus-" + timestamp()))
    batches = []
    positive = [c for c in cases if not c["negative"]]
    negative = [c for c in cases if c["negative"]]
    groups = [positive[i:i + args.batch_size] for i in range(0, len(positive), args.batch_size)]
    groups += [[c] for c in negative]
    for index, group in enumerate(groups):
        name = f"batch-{index:05d}"
        hashes = write_program(directory / name, group, "file")
        batches.append({"name": name, "case_ids": [c["id"] for c in group], "sources": hashes})
    manifest = {"format": 1, "seed": args.seed, "catalog_sha256": digest(HERE / "model.py"),
                "generator_sha256": digest(Path(__file__)), "catalog": catalog(),
                "plan": details, "cases": cases, "batches": batches,
                "coverage": coverage(cases), "generated": len(cases), "executed": 0}
    write_json(directory / "suite.json", manifest)
    print(json.dumps({"suite": str(directory / "suite.json"), **details,
                      "generated": len(cases), "executed": 0}, ensure_ascii=False, indent=2))


def coverage(cases):
    production = Counter(r for c in cases for r in c["rules"])
    pairs = Counter(f"{a}>{b}" for c in cases for a, b in zip(c["rules"], c["rules"][1:]))
    types = Counter(c["type_recipe"]["base"] + "|" + ">".join(c["type_recipe"]["constructors"]) for c in cases if c.get("type_recipe"))
    return {"cases": len(cases), "productions": dict(sorted(production.items())),
            "type_trees": dict(sorted(types.items())),
            "ordered_adjacent_pairs": dict(sorted(pairs.items())),
            "depths": dict(sorted(Counter(len(c["rules"]) for c in cases).items())),
            "negative_kinds": dict(sorted(Counter(c["negative"] for c in cases if c["negative"]).items()))}


def missing_pairs(cases, names):
    present = {tuple(pair) for c in cases for pair in zip(c["rules"], c["rules"][1:])}
    return [f"{a}>{b}" for a, b in itertools.product(names, repeat=2) if (a, b) not in present]


class Executor:
    def __init__(self, compiler, timeout, memory_mb):
        self.compiler = Path(compiler).resolve(strict=True)
        self.timeout = timeout
        self.memory_mb = memory_mb
        self.bridge = None
        self.responses = queue.Queue()
        self.env = {"PATH": str(self.compiler.parent) + os.pathsep + str(REPO / "clang" / "bin") + os.pathsep + os.environ.get("PATH", ""),
                    "VYX_CODEGEN_UNITS": None, "VYX_CGU_EMIT_THREADS": "1",
                    "VYX_BUILD_VYX_SLOTS": "2", "VYX_BUILD_MEMORY_SLOTS": "4"}
        if (REPO / "clang").is_dir(): self.env["LLVM_ROOT"] = str(REPO / "clang")
        if os.name == "nt":
            shell = shutil.which("pwsh")
            if not shell: raise RuntimeError("PowerShell 7 is required for Windows Job memory bounds")
            self.bridge = subprocess.Popen([shell, "-NoProfile", "-File", str(HERE / "process.ps1"), "-Repo", str(REPO)],
                                           stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                           text=True, encoding="utf-8", creationflags=subprocess.CREATE_NO_WINDOW)
            def read_responses():
                for line in self.bridge.stdout:
                    self.responses.put(line)
                self.responses.put(None)
            self.reader = threading.Thread(target=read_responses, daemon=True)
            self.reader.start()

    def close(self):
        if self.bridge:
            self.bridge.stdin.close()
            try: self.bridge.wait(timeout=15)
            except subprocess.TimeoutExpired:
                self.bridge.kill()
                self.bridge.wait()
            self.reader.join(timeout=5)
            self.bridge.stderr.close()
            self.bridge.stdout.close()

    def execute(self, exe, arguments, directory, name, cgu):
        directory = Path(directory).resolve()
        environment = {**self.env, "VYX_CODEGEN_UNITS": None if cgu == "default" else cgu}
        request = {"exe": str(exe), "arguments": list(map(str, arguments)), "cwd": str(directory),
                   "timeout": self.timeout, "memory_mb": self.memory_mb,
                   "stdout": str(directory / (name + ".stdout.log")),
                   "stderr": str(directory / (name + ".stderr.log")),
                   "dialog": str(directory / (name + ".dialog.log")), "environment": environment}
        started = time.monotonic()
        if self.bridge:
            self.bridge.stdin.write(json.dumps(request) + "\n")
            self.bridge.stdin.flush()
            try: response = self.responses.get(timeout=self.timeout + 45)
            except queue.Empty:
                # Terminating our bridge closes its Job handles, terminating its
                # compiler descendants as well. Never kill by executable name.
                self.bridge.kill()
                self.bridge.wait()
                raise RuntimeError("process bridge exceeded its own response deadline")
            if not response: raise RuntimeError("process bridge terminated: " + self.bridge.stderr.read())
            result = json.loads(response)
            if "BridgeError" in result: raise RuntimeError(result["BridgeError"])
        else:
            env = dict(os.environ)
            for key, value in environment.items():
                if value is None: env.pop(key, None)
                else: env[key] = value
            # RLIMIT_AS covers each child separately on POSIX; it does not claim
            # to bound total resident/commit memory of a project process group.
            def limits():
                if self.memory_mb:
                    import resource
                    limit = self.memory_mb * 1024 * 1024
                    resource.setrlimit(resource.RLIMIT_AS, (limit, limit))
            with open(request["stdout"], "wb") as out, open(request["stderr"], "wb") as err:
                proc = subprocess.Popen([str(exe), *map(str, arguments)], cwd=directory, env=env,
                                        stdout=out, stderr=err, start_new_session=True, preexec_fn=limits)
                timed_out = False
                try: exit_code = proc.wait(timeout=self.timeout)
                except subprocess.TimeoutExpired:
                    timed_out = True
                    os.killpg(proc.pid, signal.SIGKILL)
                    proc.wait()
                    exit_code = -901
            result = {"ExitCode": exit_code, "TimedOut": timed_out, "MemoryExceeded": False,
                      "DialogCaught": False, "JobLimited": False, "PeakWorkingSetMB": None,
                      "PeakPrivateCommitMB": None, "MemoryPolicy": "per-process RLIMIT_AS"}
        return {"request": request, "result": result, "seconds": round(time.monotonic() - started, 3)}

    def test(self, cases, directory, layout, opt, cgu):
        hashes = write_program(directory, cases, layout)
        exe = directory / ("probe.exe" if os.name == "nt" else "probe")
        arguments = (["--src=file", directory / "main.vyx"] if layout == "file" else
                     ["--src=project", directory, "--target", "probe", "-j20"])
        negative = cases[0]["negative"]
        arguments += ["--emit=obj" if negative else "--emit=exe", f"-O{opt}", "-o", directory / "probe.obj" if negative else exe]
        build = self.execute(self.compiler, arguments, directory, "compile", cgu)
        result = {"case_ids": [c["id"] for c in cases], "layout": layout, "optimization": opt,
                  "cgu": cgu, "directory": str(directory), "sources": hashes, "build": build,
                  "passed_ids": [], "failed_ids": [], "executed_ids": []}
        r = build["result"]
        diagnostic = log_text(directory / "compile.stdout.log") + log_text(directory / "compile.stderr.log")
        codes = sorted(set(re.findall(r"error: (E\d+|I\d+):", diagnostic)))
        if r.get("TimedOut") or r.get("MemoryExceeded") or r.get("DialogCaught"):
            result["status"] = "resource" if r.get("TimedOut") or r.get("MemoryExceeded") else "crash"
            result["signature"] = [result["status"], r["ExitCode"]]
        elif negative:
            result["executed_ids"] = result["case_ids"]
            matched = r["ExitCode"] == 1 and re.search(NEGATIVES[negative], diagnostic, re.I) is not None
            result["status"] = "pass" if matched else "negative-accepted" if r["ExitCode"] == 0 else "negative-wrong-diagnostic"
            result["signature"] = [result["status"], r["ExitCode"], codes]
            result["passed_ids" if matched else "failed_ids"] = result["case_ids"]
        elif r["ExitCode"] != 0:
            result["status"] = "compile-error" if r["ExitCode"] == 1 else "crash"
            result["signature"] = [result["status"], r["ExitCode"], codes]
            result["failed_ids"] = result["case_ids"]
        else:
            run = self.execute(exe, [], directory, "run", cgu)
            result["run"] = run
            actual = log_text(directory / "run.stdout.log").replace("\r\n", "\n")
            wanted = expected(cases)
            lines = actual.splitlines(keepends=True)
            expected_lines = wanted.splitlines(keepends=True)
            for index, c in enumerate(cases):
                if index < len(lines): result["executed_ids"].append(c["id"])
                if index < len(lines) and lines[index] == expected_lines[index]: result["passed_ids"].append(c["id"])
                else: result["failed_ids"].append(c["id"])
            rr = run["result"]
            if rr.get("TimedOut") or rr.get("MemoryExceeded"): result["status"] = "resource"
            elif rr["ExitCode"] != 0: result["status"] = "runtime-crash"
            elif actual != wanted: result["status"] = "output-mismatch"
            elif log_text(directory / "run.stderr.log"): result["status"] = "unexpected-stderr"
            else: result["status"] = "pass"
            if result["status"] != "pass" and not result["failed_ids"]:
                result["failed_ids"] = result["case_ids"]
                result["passed_ids"] = []
            result["signature"] = [result["status"], rr["ExitCode"]]
        write_json(directory / "result.json", result)
        return result


def log_text(path):
    return Path(path).read_text(encoding="utf-8-sig", errors="replace") if Path(path).exists() else ""


@contextmanager
def executor(args):
    instance = Executor(args.compiler, args.timeout, args.memory_mb)
    try: yield instance
    finally: instance.close()


def compiler_identity(instance):
    compiler = instance.compiler
    companion = {}
    for name in ("vyx_compiler_backend.dll", "vyx_runtime.lib", "libvyx_compiler_backend.so", "libvyx_runtime.a"):
        p = compiler.parent / name
        if p.exists(): companion[name] = {"path": str(p), "sha256": digest(p)}
    return {"path": str(compiler), "sha256": digest(compiler), "bytes": compiler.stat().st_size,
            "companions": companion, "platform": sys.platform,
            "memory_limit_mb": instance.memory_mb,
            "memory_policy": "Windows inherited Job private commit" if os.name == "nt" else "per-process RLIMIT_AS"}


def load_suite(path):
    path = Path(path).resolve(strict=True)
    data = read_json(path)
    if data["format"] != 1: raise ValueError("unsupported suite version")
    if data["catalog_sha256"] != digest(HERE / "model.py") or data["generator_sha256"] != digest(Path(__file__)):
        raise ValueError("generator changed since corpus creation; regenerate it explicitly (old source evidence retained)")
    ids = set()
    for case in data["cases"]:
        if case["id"] != case_id(case) or case["id"] in ids:
            raise ValueError("recipe identity changed or duplicated")
        ids.add(case["id"])
        if case["negative"] not in (None, *NEGATIVES): raise ValueError("unknown negative production")
        if case["expected"] != (None if case["negative"] else oracle(case)):
            raise ValueError("stored oracle result differs from recipe semantics")
    for batch in data["batches"]:
        for filename, checksum in batch["sources"].items():
            if digest(path.parent / batch["name"] / filename) != checksum:
                raise ValueError(f"generated source changed: {batch['name']}/{filename}")
    return data


def run(args):
    suite = load_suite(args.suite)
    cases = suite["cases"]
    if args.ids:
        requested = set(csv(args.ids))
        if requested - {c["id"] for c in cases}: raise ValueError("unknown case ID")
        cases = [c for c in cases if c["id"] in requested]
    if args.kind != "all": cases = [c for c in cases if bool(c["negative"]) == (args.kind == "negative")]
    if args.family != "all": cases = [c for c in cases if bool(c.get("type_recipe")) == (args.family == "types")]
    if not cases: raise ValueError("selection is empty")
    selected_ids = {c["id"] for c in cases}
    layouts, opts, cgus = csv(args.layouts), csv(args.optimizations), csv(args.cgus)
    if not layouts or set(layouts) - {"file", "project", "modules"}: raise ValueError("invalid layouts")
    if not opts or set(opts) - {"0", "2"}: raise ValueError("AOT optimization domain is 0,2")
    if not cgus or set(cgus) - {"default", "1", "4"}: raise ValueError("CGU domain is default,1,4")
    if any(len(set(v)) != len(v) for v in (layouts, opts, cgus)): raise ValueError("matrix axes must not contain duplicates")
    matrix = list(itertools.product(layouts, opts, cgus))
    directory = unique_dir(args.out or HERE / ".runs" / ("run-" + timestamp()))
    positive = [c for c in cases if not c["negative"]]
    groups = [positive[i:i + args.batch_size] for i in range(0, len(positive), args.batch_size)]
    groups += [[c] for c in cases if c["negative"]]
    report = {"suite": str(Path(args.suite).resolve()), "seed": suite["seed"], "catalog": suite["catalog"],
              "generated": len(suite["cases"]), "selected": len(cases), "matrix": matrix,
              "planned_program_cells": len(groups) * len(matrix), "records": []}
    executed = set()
    with executor(args) as instance:
        report["compiler"] = compiler_identity(instance)
        version = instance.execute(instance.compiler, ["--version"], directory, "version", "default")
        report["compiler"]["version_process"] = version
        report["compiler"]["version"] = log_text(directory / "version.stdout.log")
        if version["result"]["ExitCode"] != 0: raise RuntimeError("compiler identity command failed")
        for index, (group, cell) in enumerate(itertools.product(groups, matrix)):
            layout, opt, cgu = cell
            cell_dir = directory / f"{index:05d}-{layout}-O{opt}-cgu{cgu}"
            result = instance.test(group, cell_dir, layout, opt, cgu)
            report["records"].append(result)
            executed.update(result["executed_ids"])
            report["executed_coverage"] = coverage([c for c in cases if c["id"] in executed])
            report["passed_case_cell_count"] = sum(len(r["passed_ids"]) for r in report["records"])
            report["completed_program_cells"] = len(report["records"])
            report["failed_program_cells"] = sum(r["status"] != "pass" for r in report["records"])
            report["missing_case_cells"] = len(cases) * len(matrix) - sum(len(r["executed_ids"]) for r in report["records"])
            report["unselected_cases"] = len(suite["cases"]) - len(cases)
            write_json(directory / "results.json", report)
            print(f"[{index + 1}/{report['planned_program_cells']}] {layout}/O{opt}/cgu{cgu} {result['status']} cases={len(group)}", flush=True)
            if args.stop_on_failure and result["status"] != "pass": break
    report["complete"] = len(report["records"]) == report["planned_program_cells"]
    report["unselected_ids"] = [c["id"] for c in suite["cases"] if c["id"] not in selected_ids]
    report["missing_cases_by_cell"] = []
    for cell in matrix:
        delivered = {i for r in report["records"] if (r["layout"], r["optimization"], r["cgu"]) == cell for i in r["executed_ids"]}
        missing = [c["id"] for c in cases if c["id"] not in delivered]
        if missing: report["missing_cases_by_cell"].append({"cell": cell, "case_ids": missing})
    report["missing_ordered_pairs"] = missing_pairs([c for c in cases if c["id"] in executed], suite["plan"]["domain"]["rules"])
    report["status"] = "pass" if report["complete"] and not report["failed_program_cells"] else "fail"
    write_json(directory / "results.json", report)
    print(json.dumps({k: report[k] for k in ("status", "generated", "selected", "completed_program_cells", "failed_program_cells", "missing_case_cells", "unselected_cases")}, indent=2))
    print(f"results: {directory / 'results.json'}")
    return 0 if report["status"] == "pass" else 1


def reduce_failure(args):
    suite = load_suite(args.suite)
    original = read_json(args.result)
    if original["status"] in ("pass", "resource"):
        raise ValueError("reduce requires a semantic/diagnostic/crash failure; resource failures are not compiler-equivalence evidence")
    by_id = {c["id"]: c for c in suite["cases"]}
    group = [by_id[i] for i in original["case_ids"]]
    directory = unique_dir(args.out or HERE / ".runs" / ("reduce-" + timestamp()))
    attempts = []
    with executor(args) as instance:
        recorded = read_json(Path(args.result).resolve().parents[1] / "results.json")["compiler"]
        actual = compiler_identity(instance)
        recorded_libs = {name: data["sha256"] for name, data in recorded["companions"].items()}
        actual_libs = {name: data["sha256"] for name, data in actual["companions"].items()}
        if actual["sha256"] != recorded["sha256"] or actual_libs != recorded_libs:
            raise ValueError("reduction must use the same compiler as the failure")

        def reproduce(candidate):
            if len(attempts) >= args.max_attempts: return False
            result = instance.test(candidate, directory / f"attempt-{len(attempts):04d}",
                                   original["layout"], original["optimization"], original["cgu"])
            attempts.append({"cases": candidate, "result": result})
            write_json(directory / "reduction.json", {"original": str(Path(args.result).resolve()),
                                                       "compiler": compiler_identity(instance), "attempts": attempts})
            print(f"reduce {len(attempts)}/{args.max_attempts}: {result['status']} size={len(candidate)}", flush=True)
            return result["signature"] == original["signature"]

        if not reproduce(group): raise ValueError("original failure did not reproduce; no shrink claimed")
        # ddmin across batch members retains cross-function/module interactions.
        chunks = 2
        while len(group) > 1 and len(attempts) < args.max_attempts:
            size = (len(group) + chunks - 1) // chunks
            candidates = [group[i:i + size] for i in range(0, len(group), size)]
            candidates += [group[:i] + group[i + size:] for i in range(0, len(group), size) if group[:i] + group[i + size:]]
            found = next((c for c in candidates if reproduce(c)), None)
            if found is not None: group = found; chunks = 2
            elif chunks >= len(group): break
            else: chunks = min(len(group), chunks * 2)
        if len(group) == 1:
            case = group[0]
            changed = True
            while changed and len(attempts) < args.max_attempts:
                changed = False
                for i in range(len(case["rules"])):
                    chain = case["rules"][:i] + case["rules"][i + 1:]
                    c = make_case(chain, case["leaf"], case["form"], case["negative"], "reduced", case.get("type_recipe"))
                    if reproduce([c]): case = c; changed = True; break
                if not changed and case.get("type_recipe"):
                    constructors = case["type_recipe"]["constructors"]
                    for i in range(len(constructors)):
                        recipe = {**case["type_recipe"], "constructors": constructors[:i] + constructors[i + 1:]}
                        c = make_case(case["rules"], case["leaf"], case["form"], case["negative"], "reduced", recipe)
                        if reproduce([c]): case = c; changed = True; break
            for leaf in (0, 1, -1):
                if abs(leaf) >= abs(case["leaf"]): continue
                c = make_case(case["rules"], leaf, case["form"], case["negative"], "reduced", case.get("type_recipe"))
                if reproduce([c]): case = c
            if case["form"] != "inline":
                c = make_case(case["rules"], case["leaf"], "inline", case["negative"], "reduced", case.get("type_recipe"))
                if reproduce([c]): case = c
            group = [case]
        write_program(directory / "minimal", group, original["layout"])
        write_json(directory / "minimal" / "failure.json", {"signature": original["signature"], "cases": group,
                                                            "attempts": len(attempts), "budget_exhausted": len(attempts) >= args.max_attempts})
    print(f"retained reproducer: {directory / 'minimal'}")
    return 0


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    commands = p.add_subparsers(dest="command", required=True)
    for command in ("plan", "generate"):
        sub = commands.add_parser(command)
        sub.add_argument("--rules", help="comma-separated production subset (default: catalog)")
        sub.add_argument("--depth", type=int, default=2)
        sub.add_argument("--negative-depth", type=int, default=1)
        sub.add_argument("--type-depth", type=int, default=2)
        sub.add_argument("--type-context-depth", type=int, default=0, help="numeric wrappers outside a typed owner tree")
        sub.add_argument("--no-types", action="store_true")
        sub.add_argument("--values", default="-7,0,13", help="finite integer leaf domain")
        sub.add_argument("--forms", default="inline,bound")
        sub.add_argument("--seed", type=int, default=20261014)
        sub.add_argument("--random-cases", type=int, default=0, help="additional seeded depth 1..6 samples")
        sub.add_argument("--no-negative", action="store_true")
        sub.add_argument("--batch-size", type=int, default=128)
        sub.add_argument("--max-cases", type=int, default=100000)
        sub.add_argument("--out")
    for command in ("run", "replay", "reduce"):
        sub = commands.add_parser(command)
        sub.add_argument("--suite", required=True, help="generated suite.json")
        sub.add_argument("--compiler", required=True, help="current-source compiler and matching companions")
        sub.add_argument("--timeout", type=int, default=120)
        sub.add_argument("--memory-mb", type=int, default=2048)
        sub.add_argument("--out")
        if command == "reduce":
            sub.add_argument("--result", required=True, help="failed program's result.json")
            sub.add_argument("--max-attempts", type=int, default=30)
        else:
            sub.add_argument("--ids", help="explicit case IDs; omitted cases counted")
            sub.add_argument("--kind", choices=("all", "positive", "negative"), default="all")
            sub.add_argument("--family", choices=("all", "numeric", "types"), default="all")
            sub.add_argument("--layouts", default="file,project,modules")
            sub.add_argument("--optimizations", default="0,2")
            sub.add_argument("--cgus", default="default,1,4")
            sub.add_argument("--batch-size", type=int, default=128)
            sub.add_argument("--stop-on-failure", action="store_true")
    return p


def entry():
    args = parser().parse_args()
    if getattr(args, "batch_size", 1) <= 0 or getattr(args, "max_cases", 1) <= 0:
        raise ValueError("batch-size and max-cases must be positive")
    if getattr(args, "timeout", 1) <= 0 or getattr(args, "memory_mb", 1) <= 0:
        raise ValueError("timeout and memory-mb must be positive")
    if args.command == "plan": print(json.dumps(plan(args), ensure_ascii=False, indent=2)); return 0
    if args.command == "generate": generate(args); return 0
    if args.command == "reduce": return reduce_failure(args)
    return run(args)


if __name__ == "__main__":
    try: sys.exit(entry())
    except (OSError, ValueError, RuntimeError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(2)
