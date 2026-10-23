#!/usr/bin/env python3
"""Build an SDK on its native Windows/Linux host, then package it."""
import argparse
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys

from common import REPO, bootstrap_asset, checksum_sidecar, sha256, unpack_sdk


def run(command, *, cwd=REPO, env=None):
    print("+ " + " ".join(map(str, command)), flush=True)
    subprocess.run(list(map(str, command)), cwd=cwd, env=env, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bootstrap-tag", default="latest")
    parser.add_argument("--bootstrap-sdk", type=Path)
    parser.add_argument("--llvm-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    windows = os.name == "nt"
    arch = {"AMD64": "x86_64", "x86_64": "x86_64", "arm64": "aarch64",
            "aarch64": "aarch64"}.get(platform.machine())
    if arch is None or (windows and arch != "x86_64") or (not windows and sys.platform != "linux"):
        parser.error("Supported hosts: Windows x64, Linux x64 and Linux AArch64")
    host = "windows" if windows else "linux"
    leaf = f"vyx-sdk-{host}-{arch}-llvm22"
    name = leaf + (".zip" if windows else ".tar.gz")
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    sdk = args.bootstrap_sdk.resolve() if args.bootstrap_sdk else unpack_sdk(
        bootstrap_asset(args.bootstrap_tag, name, work / "downloads"), work / "bootstrap")
    # Invoke the ELF payload directly so its shell launcher cannot replace the
    # explicitly selected source-build LLVM_ROOT with the bundled SDK root.
    compiler = sdk / "bin" / ("vyxc.exe" if windows else ".vyxc-bin")
    environment = os.environ.copy()
    environment.update(LLVM_ROOT=str(args.llvm_root.resolve()),
        LLVM_BIN=str(args.llvm_root.resolve() / "bin"),
        VYX_STD_PACKAGES=str(sdk / "std_packages"),
        VYX_BUILD_VYX_SLOTS="1", VYX_BUILD_MEMORY_SLOTS="2", VYX_CGU_EMIT_THREADS="1",
        VYX_CODEGEN_UNITS="1")
    environment["PATH"] = str(args.llvm_root.resolve() / "bin") + os.pathsep + environment["PATH"]
    if not windows:
        environment["LD_LIBRARY_PATH"] = str(sdk / "bin") + ":" + str(sdk / "lib") + (
            ":" + environment["LD_LIBRARY_PATH"] if environment.get("LD_LIBRARY_PATH") else "")
    build = REPO / "bootstrap_compiler"
    out = build / "out"
    record = {"host": host, "arch": arch, "source_commit": os.environ.get("VYX_RELEASE_SOURCE_COMMIT") or subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=REPO, text=True).strip(),
        "bootstrap_tag": args.bootstrap_tag,
        "bootstrap_compiler_sha256": sha256(compiler),
        "bootstrap_manifest_sha256": sha256(sdk / "SDK-MANIFEST.json"),
        "bootstrap_version": subprocess.check_output([str(compiler), "--version"], env=environment, text=True).strip(),
        "llvm_root": str(args.llvm_root.resolve()), "jobs": args.jobs, "vyx_slots": 1}
    run([compiler, "build", "--target", "boot", f"-j{args.jobs}"], cwd=build, env=environment)
    built_compiler = out / ("boot.exe" if windows else "boot")
    shutil.copy2(built_compiler, out / ("vyxc.exe" if windows else "vyxc"))
    environment["VYX_STD_PACKAGES"] = str(REPO / "bootstrap_compiler/std_packages")
    if not windows:
        # The source hook copies the library payload as libtbb.so. The ELF
        # SONAME must also be resolvable before running the freshly built SDK.
        tbb = out / "libtbb.so"
        if tbb.exists():
            dynamic = subprocess.check_output(["readelf", "-d", str(tbb)], text=True)
            soname = re.search(r"\(SONAME\).*\[([^]]+)\]", dynamic)
            if soname and not (out / soname[1]).exists():
                (out / soname[1]).symlink_to(tbb.name)
        environment["LD_LIBRARY_PATH"] = str(out) + (":" + environment["LD_LIBRARY_PATH"]
                                                    if environment.get("LD_LIBRARY_PATH") else "")
    tools = ["vyxc-lsp"] + (["vyxc-dap"] if arch == "x86_64" else [])
    for target in tools:
        run([built_compiler, "build", "--target", target, f"-j{args.jobs}"], cwd=build, env=environment)
    arguments = ["pwsh", "-NoProfile", "-File", REPO / "bootstrap_compiler/scripts/package_sdk.ps1",
        "-CompilerPath", built_compiler, "-Destination", output / leaf,
        "-Arch", arch, "-BundleLlvm", "-Archive"]
    if "vyxc-dap" in tools:
        adapter = sdk / "bin/debugger"
        if not (adapter / "MANIFEST.json").is_file():
            raise RuntimeError("Bootstrap SDK has no verified CodeLLDB bundle")
        arguments += ["-DebugAdapterRoot", adapter]
    run(arguments, env=environment)
    archive = output / name
    checksum_sidecar(archive)
    record.update(compiler_sha256=sha256(built_compiler), archive_sha256=sha256(archive),
                  archive_bytes=archive.stat().st_size, tools=tools,
                  backend_sha256=sha256(out / ("vyx_compiler_backend.dll" if windows else "libvyx_compiler_backend.so")),
                  runtime_sha256=sha256(out / ("vyx_runtime.lib" if windows else "libvyx_runtime.a")))
    (output / f"SDK-BUILD.{host}-{arch}.json").write_text(json.dumps(record, indent=2) + "\n")
    print(archive, flush=True)


if __name__ == "__main__":
    main()
