"""Check parsed host-runtime ABI selection and explicit-target single-file AOT."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backend", type=Path, required=True)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--triplet")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    backend, compiler, output = args.backend.resolve(), args.compiler.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    dll_dir = os.add_dll_directory(str(backend.parent)) if os.name == "nt" else None
    try:
        api = ctypes.CDLL(str(backend))
        compatible = api.vyx_rt_target_runtime_compatible
        compatible.argtypes = [ctypes.c_char_p, ctypes.c_uint64, ctypes.c_char_p, ctypes.c_uint64]
        compatible.restype = ctypes.c_int32
        pairs = [
            ("aarch64-apple-macosx15.0", "arm64-apple-darwin24.5.0", True),
            ("arm64-apple-macosx15.5", "aarch64-apple-darwin24.0.0", True),
            ("x86_64-apple-macosx15.0", "x86_64-apple-darwin24.5.0", True),
            ("arm64-apple-macosx15.0", "arm64-apple-macosx15.5", True),
            ("x86_64-pc-windows-msvc", "x86_64-pc-windows-msvc", True),
            ("x86_64-unknown-linux-gnu", "x86_64-unknown-linux-gnu", True),
            ("x86_64-unknown-linux-gnu", "x86_64-pc-linux-gnu", True),
            ("x86_64-unknown-windows-msvc", "x86_64-pc-windows-msvc", True),
            ("aarch64-apple-macosx15.0", "x86_64-apple-darwin24.5.0", False),
            ("arm64e-apple-macosx15.0", "arm64-apple-darwin24.5.0", False),
            ("aarch64_32-apple-macosx15.0", "arm64-apple-darwin24.5.0", False),
            ("aarch64-apple-ios18.0", "arm64-apple-darwin24.5.0", False),
            ("aarch64-apple-ios18.0-simulator", "arm64-apple-darwin24.5.0", False),
            ("aarch64-apple-ios18.0-macabi", "arm64-apple-darwin24.5.0", False),
            ("aarch64-apple-tvos18.0", "arm64-apple-darwin24.5.0", False),
            ("aarch64-apple-watchos11.0", "arm64-apple-darwin24.5.0", False),
            ("aarch64-unknown-macosx15.0", "arm64-apple-darwin24.5.0", False),
            ("aarch64-apple-macosx15.0-simulator", "arm64-apple-darwin24.5.0", False),
            ("aarch64-apple-macosx15.0-unknown-elf", "arm64-apple-darwin24.5.0", False),
            ("aarch64-unknown-linux-gnu", "arm64-apple-darwin24.5.0", False),
            ("x86_64-unknown-linux-musl", "x86_64-unknown-linux-gnu", False),
            ("x86_64-pc-windows-gnu", "x86_64-pc-windows-msvc", False),
            ("", "arm64-apple-darwin24.5.0", False),
            ("unknown-apple-macosx15.0", "unknown-apple-macosx15.0", False),
        ]
        checks = []
        for target, host, expected in pairs:
            for left, right in ((target, host), (host, target)):
                a, b = left.encode(), right.encode()
                actual = bool(compatible(a, len(a), b, len(b)))
                if actual != expected:
                    raise AssertionError(f"runtime ABI {left!r} / {right!r}: {actual}, expected {expected}")
                checks.append({"target": left, "host": right, "compatible": actual})
        host_triple = api.vyx_rt_default_target_triple
        host_triple.argtypes = [ctypes.c_void_p, ctypes.c_uint64]
        host_triple.restype = ctypes.c_uint64
        buf = ctypes.create_string_buffer(512)
        length = host_triple(buf, len(buf))
        if not 0 < length < len(buf):
            raise RuntimeError("Invalid backend host triple")
        triplet = args.triplet or buf.value.decode()
    finally:
        if dll_dir is not None:
            dll_dir.close()
    source = output / "main.vyx"
    source.write_text('use std.string;\nfn main() -> i32 {\n'
                      '    var builder = StringBuilder.withCapacity(32);\n'
                      '    builder.appendI64(4429);\n'
                      '    let value = builder.build();\n'
                      '    print(value);\n    return 0;\n}\n')
    runs = []
    for mode in ("implicit", "explicit"):
        command = [str(compiler), "--src=file", str(source), "--run=aot", "-O0",
                   "-o", str(output / (mode + (".exe" if os.name == "nt" else "")))]
        if mode == "explicit":
            command += ["--triplet", triplet]
        result = subprocess.run(command, cwd=output, text=True, capture_output=True, timeout=120)
        (output / (mode + ".log")).write_text(result.stdout + result.stderr)
        if result.returncode or not re.search(r"(?m)^4429\r?$", result.stdout):
            raise RuntimeError(f"{mode} AOT runtime link failed ({result.returncode}):\n"
                               + result.stdout + result.stderr)
        runs.append({"mode": mode, "command": command, "exit": result.returncode})
    project = output / "project"
    (project / "src").mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, project / "src/main.vyx")
    (project / "Vyx.toml").write_text('[package]\nname="runtime_target"\nversion="0.1.0"\n'
                                     '[target.app]\ntype="exe"\nentry="src/main.vyx"\n')
    for mode in ("implicit", "explicit"):
        command = [str(compiler), "--src=project", str(project), "--target", "app", "--run=aot", "-j2"]
        if mode == "explicit":
            command += ["--triplet", triplet]
        result = subprocess.run(command, cwd=output, text=True, capture_output=True, timeout=120)
        (output / ("project-" + mode + ".log")).write_text(result.stdout + result.stderr)
        if result.returncode or not re.search(r"(?m)^4429\r?$", result.stdout):
            raise RuntimeError(f"{mode} project AOT failed ({result.returncode}):\n"
                               + result.stdout + result.stderr)
        runs.append({"mode": "project-" + mode, "command": command, "exit": result.returncode})
    foreign = ("x86_64-apple-macosx15.0" if triplet.startswith(("arm64", "aarch64"))
               else "aarch64-apple-macosx15.0")
    rejected = subprocess.run([str(compiler), "--src=project", str(project), "--target", "app",
                               "--run=aot", "--triplet", foreign], cwd=output, text=True,
                              capture_output=True, timeout=30)
    (output / "project-foreign.log").write_text(rejected.stdout + rejected.stderr)
    if rejected.returncode != 1 or "cannot run a cross-compiled target" not in rejected.stdout:
        raise RuntimeError("Project driver did not reject a foreign CPU architecture before execution")
    (output / "verification.json").write_text(json.dumps({
        "result": "PASS", "compiler": str(compiler), "compiler_sha256": digest(compiler),
        "backend": str(backend), "backend_sha256": digest(backend),
        "triplet": triplet, "abi_checks": checks, "aot_runs": runs,
        "foreign_project_rejected": True}, indent=2) + "\n")
    print(f"PASS host-runtime targets: {len(checks)} ABI checks; file/project implicit and explicit AOT; foreign CPU rejected")


if __name__ == "__main__":
    main()
