#!/usr/bin/env python3
"""Round-trip platform guards through interfaces before cross-target codegen."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import sys

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
sys.path.insert(0, str(HERE.parent / "language-composition"))
from suite import Executor, compiler_identity, digest, log_text, unique_dir, write_json

TARGETS = {
    "windows": "x86_64-pc-windows-msvc",
    "linux": "x86_64-unknown-linux-gnu",
    "android": "aarch64-unknown-linux-android21",
    "macos-arm64": "aarch64-apple-macosx15.0",
    "macos-x64": "x86_64-apple-macosx15.0",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    out = unique_dir(args.out)
    engine = Executor(Path(args.compiler), 120, 2048)
    engine.env["VYX_STD_PACKAGES"] = str(REPO / "bootstrap_compiler/std_packages")
    engine.env["VYX_RUNTIME_ROOT"] = str(engine.compiler.parent)
    rows = []

    def run(name, directory, arguments, validate):
        result = engine.execute(engine.compiler, arguments, directory, name, "1")
        text = log_text(directory / (name + ".stdout.log")) + log_text(directory / (name + ".stderr.log"))
        status = result["result"]
        passed = status["ExitCode"] == 0 and not any(status.get(k) for k in ("TimedOut", "MemoryExceeded", "DialogCaught"))
        problems = validate() if passed else [text]
        row = {"name": name, "build": result, "pass": passed and not problems, "problems": problems}
        rows.append(row)
        write_json(directory / (name + ".json"), row)
        print(f"{name}: {'PASS' if row['pass'] else 'FAIL'}", flush=True)
        if problems:
            print("\n".join(problems)[:3000], flush=True)
        return row["pass"]

    try:
        identity = compiler_identity(engine)
        for mode in ("shallow", "full", "project-private"):
            directory = out / mode
            (directory / "gate").mkdir(parents=True)
            source = directory / "api.vyx"
            shutil.copy2(HERE / "fixtures/api.vyx", source)
            interface = directory / "gate/platform_api.vyi"
            command = ["--src=file", source, "--emit=vyi", "-o", interface]
            if mode != "full":
                command += ["--vyi-shallow"]
            if mode == "project-private":
                command += ["--vyi-project-private"]

            def interface_valid():
                text = interface.read_text(encoding="utf-8")
                problems = []
                if mode != "full":
                    for token in ('@[platform("windows")]', '@[platform("posix")]',
                                  '@[platform("linux")]', '@[platform("android")]', '@[platform("macos")]'):
                        if token not in text:
                            problems.append("lost attribute: " + token)
                    if len(re.findall(r"fn ordinary\(", text)) != 4:
                        problems.append("shallow interface must preserve all ordinary platform alternatives")
                    if not re.search(r'@\[platform\("windows"\)\]\s*extern "C"\s*\{[^}]*'
                                     r'@\[platform\("linux"\)\]\s*fn gate_parent_child_disjoint', text):
                        problems.append("lost parent/child platform intersection")
                if text.count('@[ffi_returns_independent]') != 1:
                    problems.append("lost independent-return contract or promoted quoted opaque text")
                if 'fn extern_C_block(' in text:
                    problems.append("exported a synthetic wrapper as an ordinary function")
                return problems

            if not run(mode + "-interface", directory, command, interface_valid):
                continue
            # A full interface is target-filtered; a shallow one remains reusable
            # across targets. Exercise every object format without pretending to
            # execute a foreign executable on this host.
            selected_targets = TARGETS if mode != "full" else {
                key: value for key, value in TARGETS.items()
                if (os.name == "nt" and key == "windows")
                or (sys.platform == "darwin" and key == "macos-arm64")
                or (sys.platform.startswith("linux") and key == "linux")}
            consumer = directory / "consumer.vyx"
            shutil.copy2(HERE / "fixtures/consumer.vyx", consumer)
            for target, triple in selected_targets.items():
                llvm = directory / (target + ".ll")
                selected = "macos" if target.startswith("macos") else target

                def llvm_valid():
                    text = llvm.read_text(encoding="utf-8")
                    problems = []
                    if f"@gate_{selected}(" not in text:
                        problems.append("selected foreign symbol missing: gate_" + selected)
                    for other in ("windows", "linux", "android", "macos"):
                        if other != selected and f"@gate_{other}(" in text:
                            problems.append("foreign platform leaked: gate_" + other)
                    if "gate_parent_child_disjoint" in text:
                        problems.append("nested guards were incorrectly flattened into a union")
                    if ("GetFileAttributesA" in text) != (selected == "windows"):
                        problems.append("Win32 declaration target selection mismatch")
                    if ("@opendir(" in text) != (selected != "windows"):
                        problems.append("POSIX declaration target selection mismatch")
                    if ("gate_repeated_clause_union" in text) != (selected in ("linux", "macos")):
                        problems.append("repeated clauses did not retain their union")
                    return problems

                command = ["--src=file", consumer, "--interface-root", directory,
                           "--triplet=" + triple, "--emit=ir", "--export-all", "-o", llvm]
                if run(mode + "-" + target + "-ir", directory, command, llvm_valid):
                    obj = directory / (target + ".o")
                    run(mode + "-" + target + "-object", directory,
                        command[:-4] + ["--emit=obj", "--export-all", "-o", obj],
                        lambda: [] if obj.exists() and obj.stat().st_size > 0 else ["no object artifact"])

        # The exact standard package that leaked GetFileAttributesA in the
        # SDK's split LLVM module. Give its unchanged source another module
        # identity so std auto-import cannot silently pick the original source.
        directory = out / "filesystem"
        (directory / "gate").mkdir(parents=True)
        fs = REPO / "bootstrap_compiler/std_packages/io/src/fs.vyx"
        source = directory / "fs.vyx"
        original = fs.read_text(encoding="utf-8")
        assert original.startswith("module std.fs;")
        source.write_text("module gate.filesystem;" + original[len("module std.fs;"):], encoding="utf-8")
        interface = directory / "gate/filesystem.vyi"
        ok = run("filesystem-shallow", directory, ["--src=file", source, "--emit=vyi", "--vyi-shallow", "-o", interface],
                 lambda: [] if interface.exists() else ["missing filesystem interface"])
        consumer = directory / "consumer.vyx"
        consumer.write_text('use gate.filesystem;\npublic fn directory_check(path: str) -> bool { return isDir(path); }\n', encoding="utf-8")
        if ok:
            for target, triple in TARGETS.items():
                llvm = directory / (target + ".ll")
                run("filesystem-" + target, directory,
                    ["--src=file", consumer, "--interface-root", directory, "--triplet=" + triple,
                     "--emit=ir", "--export-all", "-o", llvm],
                    lambda: [] if ("GetFileAttributesA" in llvm.read_text()) == (target == "windows")
                    else ["filesystem platform guard lost"])
        write_json(out / "results.json", {"compiler": identity, "filesystem_sha256": digest(fs),
                   "results": rows, "passed": sum(r["pass"] for r in rows), "failed": sum(not r["pass"] for r in rows),
                   "execution": "cross-target IR/object generation; no foreign executable run"})
    finally:
        engine.close()
    return 0 if rows and all(r["pass"] for r in rows) else 1


if __name__ == "__main__":
    raise SystemExit(main())
