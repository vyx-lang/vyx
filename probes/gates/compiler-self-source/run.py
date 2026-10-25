#!/usr/bin/env python3
"""Check a newly built compiler against every module in its own build plan.

Reuse the actual module inputs and interfaces, but stream MIR validation instead
of emitting another compiler generation. A successful process with no lowered
bodies is a failure here.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import time
import tomllib


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def split_command(command):
    if os.name != "nt":
        return shlex.split(command)
    from ctypes import wintypes
    parse = ctypes.windll.shell32.CommandLineToArgvW
    parse.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(ctypes.c_int)]
    parse.restype = ctypes.POINTER(wintypes.LPWSTR)
    release = ctypes.windll.kernel32.LocalFree
    release.argtypes = [ctypes.c_void_p]
    release.restype = ctypes.c_void_p
    count = ctypes.c_int()
    pointer = parse(command, ctypes.byref(count))
    if not pointer:
        raise ValueError("Cannot parse recorded compiler command")
    try:
        return [pointer[i] for i in range(count.value)]
    finally:
        release(pointer)


def option(args, name):
    if name not in args:
        return None
    index = args.index(name)
    if index + 1 == len(args):
        raise ValueError(f"Recorded command has no value for {name}")
    return args[index + 1]


def function_bodies(path):
    # This only distinguishes a declaration-only host-services module from a
    # module that must lower code. It never grants a semantic ownership fact.
    text = path.read_text(encoding="utf-8")
    text = re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*[\s\S]*?\*/',
                  " ", text)
    bodies = 0
    for function in re.finditer(r"\bfn\s+[A-Za-z_]\w*", text):
        depth = 0
        for char in text[function.end():]:
            if char == "(":
                depth += 1
            elif char == ")":
                depth -= 1
            elif depth == 0 and char in "{;":
                bodies += char == "{"
                break
        else:
            raise ValueError(f"Cannot classify function declaration in {path}")
    return bodies


def build_plan(build, target):
    manifest = tomllib.loads((build / "Vyx.toml").read_text(encoding="utf-8"))
    config = manifest["target"][target]
    expected = {(build / name).resolve() for name in [config["entry"], *config.get("sources", [])]
                if name.endswith(".vyx")}
    cache = build / manifest.get("build", {}).get("cache_dir", ".cache")
    candidates = {}
    for record in cache.glob("*.vyx-cost"):
        command = next((line for line in record.read_text(encoding="utf-8").splitlines()
                        if "--src=file" in line and "--emit=obj" in line), None)
        if command is None:
            continue
        args = split_command(command)
        source = (build / option(args, "--src=file")).resolve()
        if source not in expected:
            continue
        # Old local caches can contain both COFF and ELF plans. The most recent
        # completed compile for this exact source is the relevant plan.
        if source not in candidates or record.stat().st_mtime_ns > candidates[source][0]:
            candidates[source] = (record.stat().st_mtime_ns, record, args)
    covered = set()
    plans = []
    for source, (_, record, args) in sorted(candidates.items()):
        sources = {source}
        unit_list = option(args, "--unit-sources")
        if unit_list:
            unit_file = build / unit_list
            sources.update((build / line.strip()).resolve() for line in
                           unit_file.read_text(encoding="utf-8").splitlines() if line.strip())
        covered.update(sources & expected)
        plans.append({"record": str(record), "arguments": args,
                      "declared_function_bodies": sum(function_bodies(path) for path in sources),
                      "sources": {str(path): digest(path) for path in sorted(sources)}})
    missing = expected - covered
    if missing:
        raise ValueError("No completed object plan for compiler sources:\n" +
                         "\n".join(str(path) for path in sorted(missing)))
    if not plans:
        raise ValueError("No compiler module plans were found")
    return plans, len(expected)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--target", default="boot")
    parser.add_argument("--triplet", help="Recheck target-specific source branches without linking")
    parser.add_argument("--timeout", type=int, default=300)
    args = parser.parse_args()
    compiler, build, output = args.compiler.resolve(strict=True), args.build_root.resolve(strict=True), args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    environment = os.environ.copy()
    environment.update(VYX_STD_PACKAGES=str(build / "std_packages"), VYX_CODEGEN_UNITS="1",
                       VYX_CGU_EMIT_THREADS="1")
    rows = []
    report = {"compiler": str(compiler), "compiler_sha256": digest(compiler),
              "build_root": str(build), "target": args.target, "triplet": args.triplet,
              "companions": {}, "result": "FAIL", "checks": rows}
    for name in ("vyx_compiler_backend.dll", "libvyx_compiler_backend.so", "libvyx_compiler_backend.dylib"):
        path = compiler.parent / name
        if path.is_file():
            report["companions"][name] = {"path": str(path), "sha256": digest(path)}
    runtime = Path(environment.get("VYX_RUNTIME_ROOT", str(compiler.parent)))
    for name in ("vyx_runtime.lib", "libvyx_runtime.a"):
        path = runtime / name
        if path.is_file():
            report["companions"][name] = {"path": str(path), "sha256": digest(path)}

    def execute(name, command, *, negative=False, declaration_only=False):
        started = time.monotonic()
        row = {"name": name, "command": list(map(str, command))}
        if args.triplet:
            for flag in ("--triplet", "--triple"):
                if flag in row["command"]:
                    index = row["command"].index(flag)
                    del row["command"][index:index + 2]
            row["command"] = [item for item in row["command"]
                              if not item.startswith(("--triplet=", "--triple="))]
            row["command"].extend(["--triplet", args.triplet])
        try:
            result = subprocess.run(row["command"], cwd=build, env=environment,
                                    capture_output=True, timeout=args.timeout)
            data = result.stdout + result.stderr
            row["exit"] = result.returncode
        except subprocess.TimeoutExpired as error:
            data = (error.stdout or b"") + (error.stderr or b"")
            row.update(exit=None, timed_out=True)
        (output / (name + ".log")).write_bytes(data)
        text = data.decode("utf-8", "replace")
        matches = re.findall(r"verified discarded_bodies=(\d+)", text)
        row["lowered_bodies"] = sum(map(int, matches))
        row["declaration_only"] = declaration_only
        row["pass"] = (row["exit"] == 1 and "error: E3102:" in text if negative else
                       row["exit"] == 0 and bool(matches)
                       and (row["lowered_bodies"] > 0 or declaration_only))
        row["seconds"] = round(time.monotonic() - started, 3)
        rows.append(row)
        (output / "verification.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(f"{name}: {'PASS' if row['pass'] else 'FAIL'}; bodies={row['lowered_bodies']}; "
              f"{row['seconds']:.1f}s", flush=True)
        if not row["pass"]:
            print(text[-12000:], flush=True)

    try:
        plans, source_count = build_plan(build, args.target)
        report.update(source_count=source_count, module_count=len(plans), plans=plans)
        fixture = output / "escaped.vyx"
        fixture.write_text('use std.string;\npublic fn escaped() -> str {\n'
                           '    let owner = String.from("local storage");\n'
                           '    return owner.as_str();\n}\n', encoding="utf-8")
        # These must lower the exported body even without --export-all/main.
        for mode, flags in (("named", ["--export-root-names", "escaped"]),
                            ("top-level", ["--export-top-level-roots"])):
            execute("reject-" + mode, [compiler, "--src=file", fixture, "--emit=mir-verify",
                                      "--no-process-entry", *flags], negative=True)
        for index, plan in enumerate(plans):
            command = plan["arguments"].copy()
            command[0] = str(compiler)
            command = ["--emit=mir-verify" if item == "--emit=obj" else
                       "-O0" if item.startswith("-O") else item for item in command]
            # Existing bootstrap SDKs do not enable named roots in MIR verify.
            # The focused negatives above prevent hiding that bug in new SDKs.
            command.append("--export-all")
            if "-o" in command:
                command[command.index("-o") + 1] = str(output / f"module-{index:03d}.unused")
            execute(f"module-{index:03d}", command,
                    declaration_only=plan["declared_function_bodies"] == 0)
        report["result"] = "PASS" if all(row["pass"] for row in rows) else "FAIL"
        print(f"self-source {report['result']}: {len(plans)} modules; {source_count} declared sources", flush=True)
    finally:
        (output / "verification.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if report["result"] != "PASS":
        raise SystemExit(1)


if __name__ == "__main__":
    main()
