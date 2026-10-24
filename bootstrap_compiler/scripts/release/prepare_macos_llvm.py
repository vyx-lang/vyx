#!/usr/bin/env python3
"""Prepare the pinned, relocatable LLVM 22 toolchain on a native macOS host."""
import argparse
import copy
import json
import os
from pathlib import Path, PurePosixPath
import platform
import subprocess
import tarfile
import urllib.request

from common import sha256
from package_macos import dependencies, dylib_ids, macho, rpaths, system_library

# All three inputs come from this immutable official Homebrew formula tree.
FORMULA_COMMIT = "c3a5b29227d4fd975ab30f0a0607133f45398746"
INPUTS = {
    "llvm": {"version": "22.1.8", "keg": "22.1.8", "bottles": {
        "aarch64": "5819a7751ca18dd6dd54e182464de9d3c911ccf13900ed94a631ce64e414d710",
        "x86_64": "2f07536754d0854565f9ac37436681bb3d04a4fbb15c45c51896933262df5e48"}},
    "z3": {"version": "4.16.0", "keg": "4.16.0", "bottles": {
        "aarch64": "08478660968932e8353796d24fee205a57321a4945d48991e8f82dae723d97a3",
        "x86_64": "a520309ac4d170897f5bcd9b238231bdec1cbb4f0f326c62abe7eb06b7616975"}},
    "zstd": {"version": "1.5.7", "keg": "1.5.7_1", "bottles": {
        "aarch64": "d72adf48460a8384b256f88061cd7b9df4977df7fa2e0794051d427db754a565",
        "x86_64": "8b8656acd6f30bcbbb9a033ae840afea299c9f0852f71b7540492b0fe7a36742"}},
}
TOOLS = {"clang", "clang++", "clang-22", "llvm-ar", "llvm-nm", "llvm-readobj",
         "llvm-strip", "llvm-objcopy", "llvm-config", "llvm-ranlib"}
CORE_SHARED_LIBS = {"libLLVM.dylib", "libLLVM-22.dylib", "libLTO.dylib", "libRemarks.dylib",
                    "libclang-cpp.dylib", "libclang.dylib"}


def selected_member(relative):
    """Compiler headers, tools and runtime resources; no LLDB/Python or MLIR."""
    relative = PurePosixPath(relative)
    return (relative.parts[0] == "include" or
            (relative.parts[0] == "bin" and relative.name in TOOLS) or
            (relative.parts[0] == "lib" and
             ("clang" in relative.parts or relative.name in CORE_SHARED_LIBS)))


def download_bottle(repository, digest, archive):
    archive = Path(archive)
    if archive.is_file() and sha256(archive) == digest:
        return
    with urllib.request.urlopen(
            "https://ghcr.io/token?service=ghcr.io&scope=repository:" + repository + ":pull",
            timeout=60) as response:
        token = json.load(response)["token"]
    request = urllib.request.Request(
        "https://ghcr.io/v2/" + repository + "/blobs/sha256:" + digest,
        headers={"Authorization": "Bearer " + token})

    class Redirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, request, *arguments):
            redirected = super().redirect_request(request, *arguments)
            if redirected is not None:
                redirected.remove_header("Authorization")
            return redirected

    temporary = archive.with_suffix(archive.suffix + ".partial")
    with urllib.request.build_opener(Redirect()).open(request, timeout=120) as source, temporary.open("wb") as target:
        received, last = 0, 0
        while data := source.read(8 * 1024 * 1024):
            target.write(data)
            received += len(data)
            if received - last >= 128 * 1024 * 1024:
                print(f"{repository}: {received // (1024 * 1024)} MiB", flush=True)
                last = received
    if sha256(temporary) != digest:
        raise RuntimeError(f"Homebrew bottle checksum mismatch: {repository}")
    temporary.replace(archive)


def extract_bottle(archive, destination, formula):
    """Extract the selected payload and its full dylib link chain and notices."""
    destination = Path(destination)
    root = PurePosixPath(formula, INPUTS[formula]["keg"])
    links, selected, notices = [], set(), []
    with tarfile.open(archive, "r|gz") as bundle:
        for source in bundle:
            name = PurePosixPath(source.name)
            if not name.is_relative_to(root) or name == root:
                continue
            relative = name.relative_to(root)
            if ".." in relative.parts or "\\" in source.name:
                raise ValueError(f"Unsafe bottle member: {source.name}")
            notice = relative.name.lower().startswith(("license", "copying", "notice"))
            keep = selected_member(relative) if formula == "llvm" else (
                relative.parent == PurePosixPath("lib") and relative.name.endswith(".dylib"))
            if not keep and not notice:
                continue
            if source.isdir():
                continue
            target = PurePosixPath("licenses", formula, relative) if notice else relative
            item = copy.copy(source)
            item.name = target.as_posix()
            if item.issym() or item.islnk():
                if notice:
                    raise ValueError(f"License must be a regular file: {source.name}")
                if item.islnk():
                    link = PurePosixPath(item.linkname)
                    if not link.is_relative_to(root):
                        raise ValueError(f"Hardlink escapes bottle: {source.name}")
                    item.linkname = link.relative_to(root).as_posix()
                links.append(item)
            elif item.isfile() or item.isdir():
                bundle.extract(item, destination, filter="data")
            else:
                raise ValueError(f"Unsupported bottle member: {source.name}")
            selected.add(target.as_posix())
            if notice:
                notices.append(target.as_posix())
        # Hardlinks can precede their targets in a bottle. Resolve links only
        # after regular files, but retain their original relative link text.
        for item in links:
            bundle.extract(item, destination, filter="data")
    for name in selected:
        path = destination / name
        if not path.resolve().is_relative_to(destination.resolve()) or not path.exists():
            raise ValueError(f"Broken or escaping bottle link: {name}")
    if not notices:
        raise ValueError(f"Missing license notices in {formula} bottle")
    return sorted(selected)


def resolve_input(name, source, destination, prefix):
    """Resolve exact install paths only against the pinned toolchain inputs."""
    destination = Path(destination).resolve()
    for formula, config in INPUTS.items():
        for base in (f"@@HOMEBREW_PREFIX@@/opt/{formula}/",
                     f"@@HOMEBREW_CELLAR@@/{formula}/{config['keg']}/",
                     f"{prefix}/opt/{formula}/",
                     f"{prefix}/Cellar/{formula}/{config['keg']}/"):
            if name.startswith(base):
                relative = PurePosixPath(name[len(base):])
                if ".." in relative.parts or relative.is_absolute():
                    raise ValueError(f"Invalid pinned install path {name!r}")
                path = destination.joinpath(*relative.parts)
                if path.is_file() and path.resolve().is_relative_to(destination):
                    return path
                raise RuntimeError(f"Pinned {formula} payload missing: {relative}")
    if name.startswith("@rpath/"):
        path = destination / "lib" / name[7:]
    elif name.startswith("@loader_path/"):
        path = source.parent / name[len("@loader_path/"):]
    else:
        raise RuntimeError(f"Unpinned Mach-O dependency {name!r} from {source}")
    if path.is_file() and path.resolve().is_relative_to(destination):
        return path
    raise RuntimeError(f"Missing Mach-O dependency {name!r} from {source}")


def relocate_dependencies(destination, prefix, arch):
    destination = Path(destination).resolve()
    binaries = [p for p in destination.rglob("*") if p.is_file() and not p.is_symlink() and macho(p)]
    # Read the whole load-command inventory before changing any binary; a
    # dependent's original commands must not depend on traversal order.
    metadata = [(p, dependencies(p), rpaths(p)) for p in binaries]
    for path, names, existing in metadata:
        changes = []
        identity = dylib_ids(path) if path.suffix == ".dylib" else set()
        for name in names:
            if name in identity or system_library(name):
                continue
            library = resolve_input(name, path, destination, prefix)
            changes += ["-change", name, "@rpath/" + library.name]
        if path.suffix == ".dylib":
            changes += ["-id", "@rpath/" + path.name]
        relative = os.path.relpath(destination / "lib", path.parent).replace("\\", "/")
        desired = "@loader_path" if relative == "." else "@loader_path/" + relative
        for old in existing:
            if old != desired:
                changes += ["-delete_rpath", old]
        if desired not in existing:
            changes += ["-add_rpath", desired]
        if changes:
            subprocess.run(["install_name_tool", *changes, path], check=True)
        subprocess.run(["codesign", "--force", "--sign", "-", path], check=True)
        arches = subprocess.check_output(["lipo", "-archs", path], text=True).split()
        if ("arm64" if arch == "aarch64" else arch) not in arches:
            raise RuntimeError(f"Wrong toolchain architecture: {path}: {arches}")
    for path in binaries:
        identity = dylib_ids(path) if path.suffix == ".dylib" else set()
        for name in dependencies(path):
            if name in identity:
                continue
            if not system_library(name) and not (name.startswith("@rpath/") and
                    (destination / "lib" / name[7:]).is_file()):
                raise RuntimeError(f"Toolchain retains external load command: {path}: {name}")


def verify_toolchain(destination):
    """Exercise dylib loading, Apple SDK headers, compiler-rt and native linking."""
    destination = Path(destination)
    environment = {k: v for k, v in os.environ.items() if not k.startswith("DYLD_")}
    environment["SDKROOT"] = subprocess.check_output(
        ["xcrun", "--sdk", "macosx", "--show-sdk-path"], text=True).strip()
    environment["MACOSX_DEPLOYMENT_TARGET"] = "15.0"
    for tool in ("clang", "llvm-config"):
        subprocess.run([destination / "bin" / tool, "--version"], env=environment, check=True)
    import tempfile
    with tempfile.TemporaryDirectory(prefix="vyx llvm smoke ") as directory:
        work = Path(directory)
        for name, content, tool in (
            ("c", "#include <stdio.h>\nint main(void) { return puts(\"llvm C smoke\") < 0; }\n", "clang"),
            ("cpp", "#include <vector>\nint main() { std::vector<int> v{1,2,3}; return v.at(2) == 3 ? 0 : 1; }\n", "clang++")):
            source = work / ("main." + name)
            source.write_text(content)
            executable = work / name
            subprocess.run([destination / "bin" / tool, source, "-o", executable], env=environment, check=True)
            subprocess.run([executable], env=environment, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--destination", type=Path, required=True)
    args = parser.parse_args()
    if platform.system() != "Darwin":
        parser.error("Run LLVM preparation on macOS")
    arch = {"arm64": "aarch64", "x86_64": "x86_64"}[platform.machine()]
    destination = args.destination.resolve()
    destination.mkdir(parents=True, exist_ok=False)
    inputs = {}
    for formula, config in INPUTS.items():
        digest = config["bottles"][arch]
        archive = destination.parent / f"{formula}-{config['keg']}-{arch}.bottle.tar.gz"
        download_bottle("homebrew/core/" + formula, digest, archive)
        files = extract_bottle(archive, destination, formula)
        inputs[formula] = {"version": config["version"], "sha256": digest,
                           "url": f"https://ghcr.io/v2/homebrew/core/{formula}/blobs/sha256:{digest}",
                           "files": {name: sha256(destination / name) for name in files}}
    prefix = "/opt/homebrew" if arch == "aarch64" else "/usr/local"
    relocate_dependencies(destination, prefix, arch)
    verify_toolchain(destination)
    (destination / "VYX-LLVM-INPUT.json").write_text(json.dumps({
        "schema": 2, "version": "22.1.8", "arch": arch,
        "formula_commit": FORMULA_COMMIT, "inputs": inputs}, indent=2) + "\n")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"::error title=macOS toolchain::{error}", flush=True)
        raise
