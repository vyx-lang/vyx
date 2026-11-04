#!/usr/bin/env python3
"""Prepare Linux build dependencies and LLVM development inputs from Stage 0."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys

from common import sha256


PACKAGES = ("cmake", "ninja-build", "git", "patchelf", "zlib1g-dev", "libzstd-dev", "libxml2-dev", "libsqlite3-dev")
TOOLS = ("clang", "clang++", "llvm-ar", "llvm-nm", "llvm-readobj", "llvm-strip", "lld")
SYSTEM_LIBRARIES = {"libc.so.6", "libm.so.6", "libdl.so.2", "librt.so.1", "libpthread.so.0"}


def run(command, **kwargs):
    print("+ " + " ".join(map(str, command)), flush=True)
    return subprocess.run(list(map(str, command)), check=True, **kwargs)


def install_system_dependencies():
    # LLVM comes from the checksum-verified Stage 0 SDK, not an APT repository.
    apt = ["sudo", "apt-get", "-o", "Acquire::Retries=3",
           "-o", "Acquire::http::Timeout=30", "-o", "Acquire::https::Timeout=30"]
    run([*apt, "--error-on=any", "update"])
    run([*apt, "install", "-y", *PACKAGES])


def needed(path):
    result = run(["readelf", "-d", path], capture_output=True, text=True,
                 env=dict(os.environ, LC_ALL="C"))
    return re.findall(r"\(NEEDED\).*Shared library: \[([^\]]+)\]", result.stdout)


def sdk_file(sdk, path):
    resolved = path.resolve(strict=True)
    if not resolved.is_relative_to(sdk) or not resolved.is_file():
        raise RuntimeError(f"Invalid SDK LLVM input: {path}")
    return resolved


def llvm_dependency(sdk):
    backend = sdk_file(sdk, sdk / "bin/libvyx_compiler_backend.so")
    libraries = [name for name in needed(backend)
                 if re.fullmatch(r"libLLVM(?:-[0-9]+)?\.so(?:\.[0-9]+)*", name)]
    if len(libraries) != 1:
        raise RuntimeError(f"SDK backend must have exactly one LLVM dependency: {libraries}")
    return libraries[0]


def verify(root):
    source = root / "input-check.cpp"
    source.write_text('''#include <llvm/Config/llvm-config.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm-c/Core.h>
#include <cstdio>
static_assert(LLVM_VERSION_MAJOR == 22, "Vyx requires LLVM 22");
int main() {
    llvm::LLVMContext context;
    unsigned major = 0, minor = 0, patch = 0;
    LLVMGetVersion(&major, &minor, &patch);
    if (major != LLVM_VERSION_MAJOR || minor != LLVM_VERSION_MINOR || patch != LLVM_VERSION_PATCH) return 1;
    std::printf("%u.%u.%u\\n", major, minor, patch);
    return 0;
}
''', encoding="utf-8")
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = str(root / "lib") + (
        ":" + env["LD_LIBRARY_PATH"] if env.get("LD_LIBRARY_PATH") else "")
    run([root / "bin/clang++", "-std=c++17", source, "-I" + str(root / "include"),
         "-L" + str(root / "lib"), "-Wl,-rpath," + str(root / "lib"), "-lLLVM",
         "-o", root / "input-check"], env=env)
    return run([root / "input-check"], env=env, capture_output=True, text=True).stdout.strip()


def prepare_sdk_llvm(sdk, destination):
    """The caller verifies the Release archive checksum before unpacking SDK."""
    sdk = Path(sdk).resolve(strict=True)
    destination = Path(destination).resolve()
    manifest = sdk_file(sdk, sdk / "SDK-MANIFEST.json")
    if json.loads(manifest.read_text(encoding="utf-8")).get("bundled_compiler_llvm_toolchain") is not True:
        raise RuntimeError("Stage 0 SDK does not contain a bundled LLVM toolchain")
    toolchain = sdk / "toolchain"
    for header in ("include/llvm/IR/Module.h", "include/llvm/IR/LLVMContext.h",
                   "include/llvm/Config/llvm-config.h", "include/llvm-c/Core.h",
                   "lib/clang/22/include/stddef.h"):
        sdk_file(sdk, toolchain / header)
    seeds = [sdk_file(sdk, toolchain / "bin" / name) for name in TOOLS]
    llvm = llvm_dependency(sdk)
    seeds.append(sdk_file(sdk, sdk / "lib" / llvm))

    # Exact DT_NEEDED names in the verified SDK; never search host basenames.
    # Do not expose Stage 0 oneTBB/application runtime to current-source builds.
    pending = list(seeds)
    visited = set()
    libraries = {llvm: seeds[-1]}
    while pending:
        source = pending.pop()
        if source in visited:
            continue
        visited.add(source)
        for name in needed(source):
            if "/" in name or "\\" in name:
                raise RuntimeError(f"Non-relocatable SDK dependency {name!r} from {source}")
            if name in SYSTEM_LIBRARIES or re.fullmatch(r"ld-linux-[A-Za-z0-9_-]+\.so\.[0-9]+", name):
                continue
            if name not in libraries:
                dependency = sdk_file(sdk, sdk / "lib" / name)
                libraries[name] = dependency
                pending.append(dependency)

    # The SDK retains SONAME files for runtime loading. Source builds also need
    # the exact unversioned linker names requested by the backend manifest.
    linker_names = {"libLLVM.so": llvm, "libz.so": "libz.so.1", "libzstd.so": "libzstd.so.1"}
    for name in linker_names.values():
        if name not in libraries:
            raise RuntimeError(f"SDK LLVM dependency closure is missing linker input {name}")
    destination.mkdir(parents=True, exist_ok=False)
    (destination / "bin").symlink_to(toolchain / "bin", target_is_directory=True)
    (destination / "include").symlink_to(toolchain / "include", target_is_directory=True)
    (destination / "lib").mkdir()
    (destination / "lib/clang").symlink_to(toolchain / "lib/clang", target_is_directory=True)
    for name, source in libraries.items():
        (destination / "lib" / name).symlink_to(source)
    for name, soname in linker_names.items():
        if name != soname:
            (destination / "lib" / name).symlink_to(soname)
    version = verify(destination)
    record = {"version": version, "source": "bootstrap-sdk", "sdk": str(sdk),
              "sdk_manifest_sha256": sha256(manifest), "llvm_soname": llvm,
              "libraries": {name: sha256(path) for name, path in sorted(libraries.items())},
              "linker_names": linker_names,
              "tools": {name: sha256(sdk_file(sdk, toolchain / "bin" / name)) for name in TOOLS}}
    (destination / "VYX-LLVM-INPUT.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    print(f"Verified SDK LLVM {version}: headers, C++ ABI and shared-library version agree", flush=True)
    return destination


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("system", help="Install non-LLVM dependencies from Ubuntu repositories")
    sdk = sub.add_parser("sdk", help="Use a caller-verified, unpacked Stage 0 SDK")
    sdk.add_argument("--sdk", type=Path, required=True)
    sdk.add_argument("--destination", type=Path, required=True)
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.error("Linux LLVM preparation requires a native Linux host")
    if args.command == "system":
        install_system_dependencies()
    else:
        prepare_sdk_llvm(args.sdk, args.destination)


if __name__ == "__main__":
    main()
