"""Relink an existing current-tree Windows compiler with frontend memory guards."""
from __future__ import annotations

import argparse
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--link-response", required=True, type=Path)
    parser.add_argument("--clang", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("this allocator uses Windows VirtualAlloc")
    gate = Path(__file__).resolve().parent
    repo = gate.parents[2]
    response = args.link_response.resolve(strict=True)
    clang = args.clang.resolve(strict=True)
    output = args.output.resolve()
    if output.exists():
        parser.error("choose a new output path; existing compiler files are preserved")
    source_text = response.read_text(encoding="utf-8-sig")
    # Response paths remain relative to bootstrap_compiler, as in SDK builds.
    match = re.search(r'(?<!\S)-o\s+("[^"]+"|\S+)', source_text)
    if not match:
        parser.error("link response has no -o output option")
    original = Path(match[1].strip('"'))
    if not original.is_absolute():
        original = repo / "bootstrap_compiler" / original
    original = original.resolve(strict=True)
    if output.parent != original.parent:
        parser.error("output must be beside the compiler's matching backend/runtime")
    build = gate / ".cache" / ("guard-" + datetime.now().strftime("%Y%m%d-%H%M%S-%f"))
    build.mkdir(parents=True)
    commands = []

    def run(command):
        command = [str(x) for x in command]
        result = subprocess.run(command, cwd=repo / "bootstrap_compiler", capture_output=True,
                                timeout=120)
        record = {"command": command, "exit": result.returncode,
                  "output": (result.stdout + result.stderr).decode(errors="replace")}
        commands.append(record)
        if result.returncode:
            print(record["output"])
            raise RuntimeError("guard compiler build failed")

    frontend = build / "frontend.obj"
    runtime = build / "runtime.obj"
    guard = build / "guard.obj"
    run([clang, "-x", "c", "-c", repo / "runtime/src/vyx_service_alloc.c", "-o", frontend,
         "-Dvyx_frontend_alloc=vyx_frontend_alloc_crt",
         "-Dvyx_frontend_free=vyx_frontend_free_crt", "-fms-runtime-lib=static"])
    run([clang, "-x", "c", "-c", repo / "runtime/src/vyx_service_memory.c", "-o", runtime,
         "-Dvyx_runtime_free=vyx_runtime_free_crt", "-fms-runtime-lib=static"])
    run([clang, "-c", gate / "guard_frontend.cpp", "-o", guard, "-std=c++17",
         "-O0", "-fms-runtime-lib=static"])
    relink = build / "link.rsp"
    text = source_text[:match.start(1)] + '"' + output.as_posix() + '"' + source_text[match.end(1):]
    text = " ".join('"' + p.as_posix() + '"' for p in (frontend, runtime, guard)) + " " + text
    relink.write_text(text, encoding="utf-8")
    run([clang, "@" + str(relink), "-Wl,/map:" + str(build / "compiler.map")])
    identity = {"compiler": str(output), "sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
                "base_compiler": str(original),
                "base_sha256": hashlib.sha256(original.read_bytes()).hexdigest(),
                "link_response": str(response), "commands": commands}
    (build / "build.json").write_text(json.dumps(identity, indent=2), encoding="utf-8")
    print(build / "build.json")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
