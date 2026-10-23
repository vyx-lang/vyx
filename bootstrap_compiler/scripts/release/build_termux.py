#!/usr/bin/env python3
"""Cross-build the current SDK for Termux using a fresh Linux host SDK."""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tomllib

from common import REPO, bootstrap_asset, sha256
from build_native import run


def debian_version(label, base_version, *, now=None, compare=None):
    # Older SDKs used 1.0.5 in Debian control despite compiler version
    # 1.0.0-alpha.1. Preserve upgrade ordering when returning to the real version.
    epoch = int(base_version.split(":", 1)[0]) if ":" in base_version else 0
    stamp = (now or datetime.now(timezone.utc)).strftime("%Y%m%d%H%M%S")
    version = label.replace("-", "~", 1) + "+" + stamp
    candidate = (f"{epoch}:" if epoch else "") + version
    if compare is None:
        def compare(left, right):
            result = subprocess.run(["dpkg", "--compare-versions", left, "gt", right], check=False)
            if result.returncode not in (0, 1):
                raise RuntimeError("dpkg could not compare SDK versions")
            return result.returncode == 0
    if not compare(candidate, base_version):
        candidate = f"{epoch + 1}:" + version
    return candidate


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bootstrap-tag", default="latest")
    parser.add_argument("--base-package", type=Path)
    parser.add_argument("--host-sdk", type=Path, required=True)
    parser.add_argument("--ndk", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    sdk = args.host_sdk.resolve()
    compiler = sdk / "bin/.vyxc-bin"
    if not compiler.is_file():
        raise RuntimeError("Expected a Linux host SDK with bin/.vyxc-bin")
    host_environment = os.environ.copy()
    host_environment.update(LLVM_ROOT=str(sdk / "toolchain"),
        LLVM_BIN=str(sdk / "toolchain/bin"), VYX_RUNTIME_ROOT=str(sdk / "lib"),
        VYX_STD_PACKAGES=str(sdk / "std_packages"),
        LD_LIBRARY_PATH=str(sdk / "bin") + ":" + str(sdk / "lib"))
    run([sys.executable, REPO / "probes/gates/aot-portability/run.py",
         "--backend", sdk / "bin/libvyx_compiler_backend.so", "--compiler", sdk / "bin/vyxc",
         "--output", work / "host-sdk-smoke"], env=host_environment)
    name = "vyx-sdk-termux-aarch64.deb"
    base = args.base_package.resolve() if args.base_package else bootstrap_asset(
        args.bootstrap_tag, name, work / "downloads")
    payload = work / "base"
    payload.mkdir(exist_ok=False)
    run(["dpkg-deb", "-x", base, payload])
    target_llvm = payload / "data/data/com.termux/files/usr/lib/vyx-sdk/toolchain"
    if not (target_llvm / "include/llvm/Config/llvm-config.h").is_file():
        raise RuntimeError("Base Termux SDK is missing its matching LLVM development headers")
    source = work / "source"
    source.mkdir(exist_ok=False)
    archive = work / "source.tar"
    run(["git", "archive", "--format=tar", f"--output={archive}", "HEAD"], cwd=REPO)
    with tarfile.open(archive) as bundle:
        bundle.extractall(source, filter="data")
    # Cross builds reuse the base's measured device oneTBB. Running the native
    # prebuild hook here would place a host library in the Android output.
    manifest_path = source / "bootstrap_compiler/Vyx.toml"
    manifest = manifest_path.read_text(encoding="utf-8")
    hook = 'prebuild = ["vyx: ../runtime/scripts/prepare_tbb.vyx"]'
    if manifest.count(hook) != 1:
        raise RuntimeError("oneTBB cross-build hook selection drifted")
    manifest_path.write_text(manifest.replace(hook, "prebuild = []"), encoding="utf-8")
    run(["git", "clone", "--depth", "1", "--branch", "v2022.1.0",
         "https://github.com/oneapi-src/oneTBB.git", source / "runtime/vendor/oneTBB"])
    environment = os.environ.copy()
    environment.update(LLVM_ROOT=str(target_llvm),
        LLVM_BIN=str(args.ndk.resolve() / "toolchains/llvm/prebuilt/linux-x86_64/bin"),
        ANDROID_NDK_ROOT=str(args.ndk.resolve()),
        VYX_RUNTIME_ROOT=str(sdk / "lib"),
        VYX_STD_PACKAGES=str(source / "bootstrap_compiler/std_packages"),
        LD_LIBRARY_PATH=str(sdk / "bin") + ":" + str(sdk / "lib"),
        VYX_BUILD_VYX_SLOTS="1", VYX_BUILD_MEMORY_SLOTS="2", VYX_CGU_EMIT_THREADS="1")
    run([compiler, "build", "--target", "boot", "--triplet=aarch64-unknown-linux-android21",
         f"-j{args.jobs}"], cwd=source / "bootstrap_compiler", env=environment)
    base_version = subprocess.check_output(["dpkg-deb", "--field", str(base), "Version"], text=True).strip()
    version = debian_version(tomllib.loads(manifest)["package"]["version"], base_version)
    run(["python3", source / "bootstrap_compiler/scripts/package_termux_sdk.py",
         "--base-package", base, "--compiler-out", source / "bootstrap_compiler/out",
         "--source-root", source, "--destination", output / name,
         "--work-dir", work / "package", "--version", version])
    record = {"host": "android", "arch": "aarch64", "source_commit": subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=REPO, text=True).strip(),
        "host_compiler_sha256": sha256(compiler), "base_package_sha256": sha256(base),
        "host_backend_sha256": sha256(sdk / "bin/libvyx_compiler_backend.so"),
        "base_package_version": base_version, "package_version": version,
        "compiler_sha256": sha256(source / "bootstrap_compiler/out/boot"),
        "archive_sha256": sha256(output / name), "archive_bytes": (output / name).stat().st_size,
        "ndk": str(args.ndk.resolve()), "jobs": args.jobs, "vyx_slots": 1,
        "tools": [], "retained_payload": "Base SDK LLVM and oneTBB; no Android DAP"}
    (output / "SDK-BUILD.android-aarch64.json").write_text(json.dumps(record, indent=2) + "\n")


if __name__ == "__main__":
    main()
