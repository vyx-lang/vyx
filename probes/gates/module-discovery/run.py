"""Check bounded implicit module discovery and explicit cached module imports."""
from __future__ import annotations

import argparse
import ctypes
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

GATE = Path(__file__).resolve().parent
ROOT = GATE.parents[2]
FIXTURES = GATE / "fixtures"


def normalized(path):
    return str(path).replace("\\", "/").rstrip("/").casefold()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--case", action="append", choices=[
        "nested-cache-hello", "missing-module", "explicit-source",
        "explicit-library-root", "ordinary-cache-data", "ordinary-attr-directory",
        "ordinary-vyi-directory", "missing-normal-module",
        "ordinary-module-functions", "generic-module-functions", "specialized-module-functions",
        "ordinary-lexical-functions", "generic-lexical-functions",
        "root-lexical-functions",
        "relative-project-sibling",
        "ordinary-large-inventory", "ordinary-large-inventory-aot",
        "same-module-duplicate", "same-module-generic-duplicate", "same-module-specialized-duplicate",
    ], help="run only selected cases; repeat to select more than one")
    parser.add_argument("--budget", type=float, default=5,
                        help="maximum seconds for each discovery-only check (default: 5)")
    args = parser.parse_args()
    if args.budget <= 0:
        parser.error("--budget must be positive")
    compiler = args.compiler.resolve(strict=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    work = (args.output_dir or GATE / ".cache" / stamp).resolve()
    if work.exists():
        parser.error(f"refusing to overwrite existing output: {work}")
    work.mkdir(parents=True)
    # Ensure that even a custom output directory exercises an ancestor .cache,
    # rather than relying on its own path having a generated-directory name.
    generated = work / ".cache" / "nested" / "src" / "consumer"
    generated.mkdir(parents=True)
    unused = generated / "unused"
    unused.mkdir()
    for index in range(1024):
        (unused / f"decoy_{index:04}.vyx").write_text(
            f"module discovery_decoy_{index:04};\n"
            f"public fn unused() -> i32 {{ return {index}; }}\n", encoding="utf-8")
    for name in ("hello", "consumer", "missing"):
        shutil.copyfile(FIXTURES / f"{name}.vyx", generated / f"{name}.vyx")
    # This makes input_std look valid to the implicit compatibility-root check.
    std = generated / "std"
    std.mkdir()
    (std / "clone.vyx").write_text(
        "module discovery_unwanted_std;\npublic fn unused() -> i32 { return 0; }\n",
        encoding="utf-8")
    producer = work / ".cache" / "explicit" / "cached_module.vyx"
    producer.parent.mkdir(parents=True)
    shutil.copyfile(FIXTURES / "cached_module.vyx", producer)
    module_functions = FIXTURES / "module_functions"
    module_function_sources = work / "module-function-sources.txt"
    module_function_sources.write_text(str(module_functions / "left.vyx") + "\n"
                                       + str(module_functions / "right.vyx") + "\n"
                                       + str(module_functions / "special_left.vyx") + "\n"
                                       + str(module_functions / "special_right.vyx") + "\n", encoding="utf-8")
    lexical_functions = FIXTURES / "lexical_functions"
    lexical_function_sources = work / "lexical-function-sources.txt"
    lexical_function_sources.write_text(str(lexical_functions / "left.vyx") + "\n"
                                        + str(lexical_functions / "right.vyx") + "\n", encoding="utf-8")
    env = dict(os.environ)
    for name in ("VYX_DCI_EXPORT_OUT", "VYX_EFFECT_MANIFEST_OUT", "VYX_PACKAGE_PROJECT"):
        env.pop(name, None)
    env["VYX_DEBUG_IMPORT"] = "1"
    env["PATH"] = str(compiler.parent) + os.pathsep + env.get("PATH", "")
    if os.name == "nt":
        # Child error dialogs would otherwise turn an access violation into a
        # hung gate. This error mode applies only to this runner and its children.
        ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x0002 | 0x8000)
    else:
        env["LD_LIBRARY_PATH"] = str(compiler.parent) + os.pathsep + env.get("LD_LIBRARY_PATH", "")
    records = []
    failures = []
    identity = {
        "path": str(compiler),
        "sha256": hashlib.sha256(compiler.read_bytes()).hexdigest(),
        "size": compiler.stat().st_size,
        "mtime_ns": compiler.stat().st_mtime_ns,
    }

    def check(label, source, *, options=(), expected=None, reject=None,
              reject_line=None, no_generated_scan=False, working_directory=None,
              memory_budget=None):
        if args.case and label not in args.case:
            return
        executable = work / (label + (".exe" if os.name == "nt" else ""))
        command = [str(compiler), "--src=file", str(source), "--no-implicit-std"]
        if expected is None:
            command.append("--stop-after-sema")
            timeout = args.budget
        else:
            command.extend(["--run=aot", "-o", str(executable), "-L", str(compiler.parent),
                            "-l", "vyx_runtime"])
            timeout = 60
        command.extend(str(option) for option in options)
        log = work / f"{label}.log"
        start = time.monotonic()
        with log.open("wb") as output:
            process = subprocess.Popen(command, cwd=working_directory or work, env=env,
                                       stdout=output, stderr=subprocess.STDOUT)
            try:
                code = process.wait(timeout=timeout)
                timed_out = False
            except subprocess.TimeoutExpired:
                process.kill()
                code = process.wait()
                timed_out = True
            peak_bytes = None
            if os.name == "nt":
                class MemoryCounters(ctypes.Structure):
                    _fields_ = [("cb", ctypes.c_ulong), ("PageFaultCount", ctypes.c_ulong)] + [
                        (name, ctypes.c_size_t) for name in (
                            "PeakWorkingSetSize", "WorkingSetSize", "QuotaPeakPagedPoolUsage",
                            "QuotaPagedPoolUsage", "QuotaPeakNonPagedPoolUsage", "QuotaNonPagedPoolUsage",
                            "PagefileUsage", "PeakPagefileUsage")]
                counters = MemoryCounters()
                counters.cb = ctypes.sizeof(counters)
                query = ctypes.windll.psapi.GetProcessMemoryInfo
                query.argtypes = [ctypes.c_void_p, ctypes.POINTER(MemoryCounters), ctypes.c_ulong]
                if query(int(process._handle), ctypes.byref(counters), counters.cb):
                    peak_bytes = counters.PeakPagefileUsage
        text = log.read_text(encoding="utf-8", errors="replace")
        errors = []
        if timed_out:
            errors.append(f"exceeded {timeout}s budget")
        elif reject:
            if code not in (1, 2) or "error" not in text.lower() or reject not in text:
                errors.append(f"expected a diagnostic naming {reject}, received exit {code}")
            if reject_line is not None and f"{source.name}:{reject_line}:" not in text:
                errors.append(f"expected the duplicate at {source.name}:{reject_line}")
        elif code != 0:
            errors.append(f"exit {code}")
        elif expected and expected not in text:
            errors.append(f"missing runtime assertion marker: {expected}")
        if no_generated_scan:
            for line in text.splitlines():
                prefix = "[import-debug] file_list root="
                if line.startswith(prefix):
                    scanned_root = line[len(prefix):].split(" tmp=", 1)[0]
                    root_path = Path(scanned_root)
                    if not root_path.is_absolute():
                        root_path = work / root_path
                    # An explicit producer root is permitted only in the -L
                    # test, which does not request no_generated_scan.
                    if root_path.is_dir() and normalized(root_path.resolve()).startswith(normalized(work)):
                        errors.append(f"implicit scan entered generated root: {scanned_root}")
            if "discovery_decoy_" in text or "discovery_unwanted_std" in text:
                errors.append("unrelated generated modules entered the registry")
        if memory_budget is not None and os.name == "nt":
            if peak_bytes is None:
                errors.append("peak committed memory measurement unavailable")
            elif peak_bytes > memory_budget:
                errors.append(f"peak committed memory {peak_bytes} exceeds {memory_budget} byte budget")
        record = {
            "name": label, "command": command, "exit": code,
            "timeout": timed_out, "elapsed_seconds": round(time.monotonic() - start, 3),
            "log": str(log), "errors": errors,
            "peak_committed_bytes": peak_bytes,
        }
        records.append(record)
        failures.extend(f"{label}: {error}" for error in errors)
        print(f"{'FAIL' if errors else 'PASS'} {label} ({record['elapsed_seconds']}s)", flush=True)
        (work / "results.json").write_text(json.dumps({
            "compiler": identity, "decoy_modules": 1024, "results": records,
            "failures": failures,
        }, ensure_ascii=False, indent=2), encoding="utf-8")

    check("nested-cache-hello", generated / "hello.vyx", no_generated_scan=True)
    check("missing-module", generated / "missing.vyx",
          reject="unavailable_module_discovery_fixture", no_generated_scan=True)
    check("explicit-source", generated / "consumer.vyx",
          options=["--module-source", producer], expected="explicit cached module PASS",
          no_generated_scan=True)
    check("explicit-library-root", generated / "consumer.vyx",
          options=["-L", producer.parent], expected="explicit cached module PASS")
    check("ordinary-cache-data", FIXTURES / "cache_data" / "main.vyx",
          expected="ordinary cache_data module PASS")
    check("ordinary-attr-directory", FIXTURES / "source.attr" / "main.vyx",
          expected="ordinary source.attr module PASS")
    check("ordinary-vyi-directory", FIXTURES / "source.vyi" / "main.vyx",
          expected="ordinary source.vyi module PASS")
    check("missing-normal-module", FIXTURES / "missing.vyx",
          reject="unavailable_module_discovery_fixture", no_generated_scan=True)
    for name, marker in (("ordinary", "ordinary module functions PASS"),
                         ("generic", "generic module functions PASS"),
                         ("specialized", "specialized module functions PASS")):
        check(f"{name}-module-functions", module_functions / f"{name}.vyx",
              options=["--module-sources-file", module_function_sources], expected=marker)
    for name, source, diagnostic in (
            ("same-module-duplicate", "duplicate", "duplicate function identity_duplicate"),
            ("same-module-generic-duplicate", "duplicate_generic", "redefinition of generic function 'identity_generic_duplicate'"),
            ("same-module-specialized-duplicate", "duplicate_specialized", "redefinition of full template specialization 'identity_specialized_duplicate")):
        check(name, module_functions / f"{source}.vyx", reject=diagnostic, reject_line=8)
    for name in ("ordinary", "generic"):
        check(f"{name}-lexical-functions", lexical_functions / f"{name}.vyx",
              options=["--module-sources-file", lexical_function_sources],
              expected=f"{name} lexical functions PASS")
    check("root-lexical-functions", lexical_functions / "root.vyx",
          options=["--module-source", lexical_functions / "right.vyx"],
          expected="root lexical functions PASS")
    # Keep this source in an ordinary directory and invoke it by relative
    # path. A .cache root skips implicit scanning and would hide a registry
    # row such as ./sibling.vyx loading the unity source a second time.
    relative_project = FIXTURES / "relative_project"
    check("relative-project-sibling", Path("main.vyx"),
          options=["--project-unit-sources", relative_project / "sources.txt"],
          expected="relative project sibling PASS",
          working_directory=relative_project)
    large_cases = {"ordinary-large-inventory", "ordinary-large-inventory-aot"}
    if not args.case or large_cases.intersection(args.case):
        # This tree must be outside .cache: ordinary source directories still
        # require an inventory, and skipping them would hide the memory bug.
        with tempfile.TemporaryDirectory(prefix="inventory-", dir=FIXTURES) as temporary:
            inventory = Path(temporary).resolve()
            if inventory.parent != FIXTURES.resolve():
                raise RuntimeError("inventory directory escaped fixture root")
            for index in range(1024):
                (inventory / f"module_{index:04}.vyx").write_text(
                    f"module inventory_{index:04};\n"
                    f"public fn marker() -> i32 {{ return {index}; }}\n", encoding="utf-8")
            source = inventory / "main.vyx"
            source.write_text(
                "use inventory_1023;\nfn main() -> i32 {\n"
                "  if (inventory_1023.marker() != 1023) { return 1; }\n"
                '  print("large inventory PASS");\n  return 0;\n}\n', encoding="utf-8")
            for label, expected in (("ordinary-large-inventory", None),
                                    ("ordinary-large-inventory-aot", "large inventory PASS")):
                check(label, source, expected=expected, working_directory=work,
                      memory_budget=1024 * 1024 * 1024)
    print(f"Results: {work / 'results.json'}", flush=True)
    if failures:
        for failure in failures:
            print(failure, flush=True)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
