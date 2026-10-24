"""Package a native macOS SDK with a relocatable, verified Mach-O dependency closure."""
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tarfile

from build_native import run
from common import REPO, sha256


def macho(path):
    with path.open("rb") as source:
        magic = source.read(4)
        if magic in (b"\xca\xfe\xba\xbe", b"\xca\xfe\xba\xbf"):
            # A universal compiler-rt .a has the same FAT magic as a dylib.
            # Its architecture slices contain ar archives, not load commands.
            count = source.read(4)
            if len(count) != 4 or struct.unpack(">I", count)[0] == 0:
                return False
            source.seek(16)
            size = 8 if magic == b"\xca\xfe\xba\xbf" else 4
            offset = source.read(size)
            if len(offset) != size:
                return False
            source.seek(int.from_bytes(offset, "big"))
            magic = source.read(4)
        if magic not in (b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf"):
            return False
        header = source.read(12)
        if len(header) != 12:
            return False
        kind = int.from_bytes(header[8:12], "little" if magic[0] == 0xCF else "big")
        return kind in (2, 6, 8)  # MH_EXECUTE, MH_DYLIB, MH_BUNDLE


def parse_dependencies(output):
    # Universal otool output repeats non-indented "file (architecture ...)"
    # headers. They are not dependencies, even though they contain " (".
    return [row.strip().split(" (compatibility version", 1)[0]
            for row in output.splitlines() if row[:1].isspace() and " (compatibility version" in row]


def dylib_ids(path):
    output = subprocess.check_output(["otool", "-D", path], text=True)
    return {row.strip() for row in output.splitlines()[1:] if row.strip() and not row.endswith(":")}


def dependencies(path):
    output = subprocess.check_output(["otool", "-L", path], text=True)
    return list(dict.fromkeys(parse_dependencies(output)))


def rpaths(path):
    output = subprocess.check_output(["otool", "-l", path], text=True)
    return list(dict.fromkeys(re.findall(r"cmd LC_RPATH\s+cmdsize \d+\s+path (.*?) \(offset", output)))


def system_library(name):
    return name.startswith(("/usr/lib/", "/System/Library/"))


def resolve_dependency(name, original, search):
    if name.startswith("@loader_path/"):
        candidates = [original.parent / name.removeprefix("@loader_path/")]
    elif name.startswith("@rpath/"):
        roots = [Path(path.replace("@loader_path", str(original.parent))) for path in rpaths(original)]
        candidates = [root / name.removeprefix("@rpath/") for root in roots + list(search)]
    elif name.startswith("/"):
        candidates = [Path(name)]
    else:
        raise RuntimeError(f"Unsupported Mach-O dependency {name!r} in {original}")
    # The same install name in multiple search roots must identify the same bytes.
    found = list(dict.fromkeys(path.resolve() for path in candidates if path.is_file()))
    if not found or len({sha256(path) for path in found}) != 1:
        raise RuntimeError(f"Missing or ambiguous Mach-O dependency {name!r} in {original}: {found}")
    return found[0]


def relocate(root, sources, search, arch):
    """Resolve load commands against original files, then rewrite the packaged copies."""
    pending, copied = list(sources.items()), {}
    while pending:
        target, original = pending.pop(0)
        changes = []
        for name in dependencies(original):
            if system_library(name):
                continue
            # otool -L lists LC_ID_DYLIB as its first entry; it is not an input.
            if original.suffix == ".dylib":
                identity = dylib_ids(original)
                if name in identity:
                    continue
            dependency = resolve_dependency(name, original, search)
            library = root / "lib" / Path(name).name
            digest = sha256(dependency)
            if library in copied and copied[library] != digest:
                raise RuntimeError(f"Conflicting dylib payloads: {library.name}")
            if library not in copied:
                copied[library] = digest
                if library in sources and sha256(sources[library]) != digest:
                    raise RuntimeError(f"Dylib collides with staged source: {library}")
                if library not in sources:
                    shutil.copy2(dependency, library)
                    sources[library] = dependency
                    pending.append((library, dependency))
            changes += ["-change", name, "@rpath/" + library.name]
        if target.suffix == ".dylib":
            changes += ["-id", "@rpath/" + target.name]
        existing = rpaths(target)
        relative = os.path.relpath(root / "lib", target.parent)
        desired = "@loader_path" if relative == "." else "@loader_path/" + relative
        for old in existing:
            if old != desired:
                changes += ["-delete_rpath", old]
        if desired not in existing:
            changes += ["-add_rpath", desired]
        if changes:
            subprocess.run(["install_name_tool", *changes, target], check=True)
        # install_name_tool invalidates the ad-hoc signature on Apple Silicon.
        subprocess.run(["codesign", "--force", "--sign", "-", target], check=True)
        arches = subprocess.check_output(["lipo", "-archs", target], text=True).split()
        if ("arm64" if arch == "aarch64" else arch) not in arches:
            raise RuntimeError(f"SDK binary architecture mismatch: {target}: {arches}")
    for target in sources:
        identity = dylib_ids(target) if target.suffix == ".dylib" else set()
        for name in dependencies(target):
            if name in identity:
                continue
            if not system_library(name) and not (name.startswith("@rpath/") and (root / "lib" / name[7:]).is_file()):
                raise RuntimeError(f"SDK retains an external dependency: {target}: {name}")


def launcher(path, payload):
    path.write_text('#!/bin/sh\nset -eu\n'
        'VYX_HOME=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)\nexport VYX_HOME\n'
        'export LLVM_ROOT="$VYX_HOME/toolchain"\nexport LLVM_BIN="$LLVM_ROOT/bin"\n'
        'export PATH="$LLVM_BIN:$PATH"\nexport VYX_STD_PACKAGES="$VYX_HOME/std_packages"\n'
        'export VYX_RUNTIME_ROOT="$VYX_HOME/lib"\n'
        'export DYLD_LIBRARY_PATH="$VYX_HOME/bin:$VYX_HOME/lib"\n'
        'export LIBRARY_PATH="$VYX_HOME/lib${LIBRARY_PATH:+:$LIBRARY_PATH}"\n'
        'export SDKROOT="${SDKROOT:-$(xcrun --sdk macosx --show-sdk-path)}"\n'
        'export MACOSX_DEPLOYMENT_TARGET=15.0\n'
        f'exec "$VYX_HOME/bin/{payload}" "$@"\n', encoding="utf-8")
    path.chmod(0o755)


def package(build, llvm, output, arch, environment, work):
    leaf = f"vyx-sdk-macos-{arch}-llvm22"
    output.mkdir(parents=True, exist_ok=True)
    root = output / leaf
    root.mkdir(exist_ok=False)
    for directory in ("bin", "lib", "toolchain/bin", "licenses"):
        (root / directory).mkdir(parents=True, exist_ok=True)
    out = build / "out"
    sources = {}
    for original, relative in ((out / "boot", "bin/.vyxc-bin"),
                               (out / "vyxc-lsp", "bin/.vyxc-lsp-bin"),
                               (out / "libvyx_compiler_backend.dylib", "bin/libvyx_compiler_backend.dylib"),
                               (out / "libtbb.12.dylib", "lib/libtbb.12.dylib")):
        target = root / relative
        shutil.copy2(original, target)
        sources[target] = original
    (root / "lib/libtbb.dylib").symlink_to("libtbb.12.dylib")
    shutil.copy2(out / "libvyx_runtime.a", root / "lib/libvyx_runtime.a")
    ignore = shutil.ignore_patterns(".git", ".cache", "__pycache__", "*.pyc", "target", "out", "node_modules")
    for directory in ("std", "std_packages"):
        shutil.copytree(build / directory, root / directory, ignore=ignore)
    for directory in ("dci", "packages"):
        shutil.copytree(build.parent / "tools" / directory, root / "tools" / directory, ignore=ignore)
    shutil.copytree(build.parent / "runtime/src", root / "runtime/src", ignore=ignore)
    for name in ("LICENSE-MIT", "LICENSE-APACHE"):
        shutil.copy2(build.parent / name, root / "licenses" / name)
    shutil.copy2(build.parent / "runtime/vendor/oneTBB/LICENSE.txt", root / "licenses/oneTBB.txt")
    shutil.copytree(llvm / "licenses", root / "licenses/toolchain")
    shutil.copy2(llvm / "VYX-LLVM-INPUT.json", root / "toolchain/VYX-LLVM-INPUT.json")
    # clang's resource directory includes compiler-rt libraries, not only headers.
    shutil.copytree(llvm / "lib/clang", root / "toolchain/lib/clang", symlinks=True)
    for name in ("clang", "llvm-ar", "llvm-nm", "llvm-readobj", "llvm-strip"):
        original = (llvm / "bin" / name).resolve(strict=True)
        target = root / "toolchain/bin" / name
        shutil.copy2(original, target)
        sources[target] = original
    for name, target in (("clang++", "clang"), ("llvm-ranlib", "llvm-ar")):
        (root / "toolchain/bin" / name).symlink_to(target)
    # Shared compiler-rt sanitizers are Mach-O inputs too.
    for path in (root / "toolchain/lib/clang").rglob("*.dylib"):
        if path.is_file() and not path.is_symlink() and macho(path):
            sources[path] = llvm / path.relative_to(root / "toolchain")
    relocate(root, sources, [out, llvm / "lib"], arch)
    shutil.copy2(build.parent / "tools/debugger/LICENSE-LLVM.txt", root / "licenses/LLVM.txt")
    launcher(root / "bin/vyxc", ".vyxc-bin")
    launcher(root / "bin/vyxc-lsp", ".vyxc-lsp-bin")
    (root / "README.txt").write_text(
        "Vyx macOS SDK (macOS 15 or newer)\n\nAdd bin/ to PATH. LLVM 22 is bundled.\n"
        "Linking requires Apple's Command Line Tools: xcode-select --install\n"
        "Includes the SDK compiler and LSP. A macOS DAP adapter is not bundled.\n")
    (root / "SDK-MANIFEST.json").write_text(json.dumps({
        "schema": 1, "platform": "macos", "arch": arch, "llvm": 22, "minimum_macos": "15.0",
        "compiler": "bin/vyxc", "compiler_backend": "bin/libvyx_compiler_backend.dylib",
        "application_runtime": "lib/libvyx_runtime.a", "application_runtime_linkage": "static",
        "application_runtime_language": "Vyx", "llvm_required_by_applications": False,
        "bundled_compiler_llvm_toolchain": True, "debug_adapter": None,
        "tools": ["vyxc", "vyxc-lsp"],
        "files": {path.relative_to(root).as_posix(): sha256(path) for path in sorted(root.rglob("*"))
                  if path.is_file() and not path.is_symlink()}}, indent=2) + "\n")
    # Relocation is tested before accepting an archive. Drop source-build loader
    # paths so a missing dylib cannot be supplied by the runner's build tree.
    relocated = work / "relocated SDK"
    root.rename(relocated)
    smoke_environment = {key: value for key, value in environment.items()
                         if key not in ("DYLD_LIBRARY_PATH", "LIBRARY_PATH", "LLVM_ROOT", "LLVM_BIN",
                                        "VYX_RUNTIME_ROOT", "VYX_STD_PACKAGES")}
    try:
        smoke_environment["DYLD_LIBRARY_PATH"] = str(relocated / "bin") + ":" + str(relocated / "lib")
        run([sys.executable, REPO / "probes/gates/aot-portability/run.py", "--backend",
             relocated / "bin/libvyx_compiler_backend.dylib", "--compiler", relocated / "bin/vyxc",
             "--output", work / "packaged-sdk-smoke"], env=smoke_environment)
        run([relocated / "bin/vyxc-lsp", "--help"], env=smoke_environment)
        cxx = work / "standard-library.cpp"
        cxx.write_text('#include <vector>\nint main() { std::vector<int> v{1,2,3}; return v.at(2) == 3 ? 0 : 1; }\n')
        run([relocated / "toolchain/bin/clang++", cxx, "-o", work / "cxx-smoke"], env=smoke_environment)
        run([work / "cxx-smoke"], env=smoke_environment)
    finally:
        relocated.rename(root)
    archive = output / (leaf + ".tar.gz")
    with tarfile.open(archive, "w:gz") as bundle:
        bundle.add(root, arcname=leaf)
    return archive
