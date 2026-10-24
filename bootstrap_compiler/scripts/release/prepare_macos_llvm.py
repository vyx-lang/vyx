#!/usr/bin/env python3
"""Prepare verified LLVM 22 development files for a native macOS runner."""
import argparse
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tarfile
import urllib.request

from common import sha256

# Homebrew's current versioned formula has no Intel bottle. These immutable
# 22.1.8 bottles are recorded in the official formula at this source commit.
FORMULA_COMMIT = "c3a5b29227d4fd975ab30f0a0607133f45398746"
BOTTLES = {
    "aarch64": "5819a7751ca18dd6dd54e182464de9d3c911ccf13900ed94a631ce64e414d710",
    "x86_64": "2f07536754d0854565f9ac37436681bb3d04a4fbb15c45c51896933262df5e48",
}
TOOLS = {"clang", "clang++", "clang-22", "llvm-ar", "llvm-nm", "llvm-readobj",
         "llvm-strip", "llvm-config", "llvm-ranlib"}
SYSTEM_PREFIXES = ("/usr/lib/", "/System/Library/")


def macho(path):
    with path.open("rb") as source:
        return source.read(4) in (b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf",
                                  b"\xca\xfe\xba\xbe", b"\xca\xfe\xba\xbf")


def macho_dependencies(path):
    """Return load-command dependencies, excluding the tool's own dylib id."""
    output = subprocess.check_output(["otool", "-L", str(path)], text=True)
    names = [row.strip().split(" (", 1)[0] for row in output.splitlines()[1:]
             if " (" in row]
    if path.suffix == ".dylib":
        identity = subprocess.check_output(["otool", "-D", str(path)], text=True).splitlines()[1:]
        names = [name for name in names if name not in identity]
    return names


def macho_rpaths(path):
    output = subprocess.check_output(["otool", "-l", str(path)], text=True)
    rows = output.splitlines()
    result = []
    for index, row in enumerate(rows):
        if row.strip() == "cmd LC_RPATH":
            for following in rows[index + 1:index + 6]:
                if following.strip().startswith("path "):
                    result.append(following.strip()[5:].split(" (", 1)[0])
                    break
    return result


def system_dependency(name):
    return name.startswith(SYSTEM_PREFIXES)


def brew_formula_and_relative(path, prefix):
    """Decode a Homebrew opt/Cellar path without basename guessing."""
    path, prefix = Path(path), Path(prefix)
    try:
        relative = path.relative_to(prefix)
    except ValueError:
        return None
    parts = relative.parts
    if len(parts) >= 3 and parts[0] == "opt":
        return parts[1], Path(*parts[2:])
    if len(parts) >= 4 and parts[0] == "Cellar":
        return parts[1], Path(*parts[3:])
    return None


def dependency_rewrite(name, destination, prefix):
    """Expand bottle placeholders so dependency resolution is deterministic."""
    destination, prefix = Path(destination), Path(prefix)
    replacements = {
        "@@HOMEBREW_PREFIX@@": str(prefix),
        "@@HOMEBREW_CELLAR@@": str(prefix / "Cellar"),
        "@@HOMEBREW_PREFIX@@/opt/llvm": str(destination),
        "@@HOMEBREW_CELLAR@@/llvm/22.1.8": str(destination),
    }
    result = name
    # The longer substitutions must win over the generic placeholders.
    for old, new in sorted(replacements.items(), key=lambda item: len(item[0]), reverse=True):
        result = result.replace(old, new)
    return result


class BrewDependencyResolver:
    """Resolve bottle and Homebrew dependencies by formula and exact path."""

    def __init__(self, destination, prefix):
        self.destination = Path(destination).resolve()
        self.prefix = Path(prefix).resolve()
        self.formula_roots = {}

    def _formula_root(self, formula):
        if formula not in self.formula_roots:
            root = self.prefix / "opt" / formula
            if not root.exists():
                try:
                    root = Path(subprocess.check_output(
                        ["brew", "--prefix", formula], text=True).strip())
                except subprocess.CalledProcessError as error:
                    raise RuntimeError(
                        f"Homebrew formula {formula!r} is required by the LLVM bottle") from error
            self.formula_roots[formula] = root.resolve()
        return self.formula_roots[formula]

    def _destination_equivalent(self, path):
        decoded = brew_formula_and_relative(path, self.prefix)
        if not decoded:
            return None
        formula, relative = decoded
        if formula != "llvm":
            return None
        candidate = self.destination.joinpath(relative)
        return candidate if candidate.is_file() else None

    def resolve(self, name, source):
        name = dependency_rewrite(name, self.destination, self.prefix)
        if name.startswith("@loader_path/"):
            candidates = [Path(source).parent / name.removeprefix("@loader_path/")]
        elif name.startswith("@rpath/"):
            suffix = name.removeprefix("@rpath/")
            roots = []
            for rpath in macho_rpaths(Path(source)):
                if rpath.startswith("@loader_path/"):
                    roots.append(Path(source).parent / rpath.removeprefix("@loader_path/")
                                 )
                elif rpath.startswith("@executable_path/"):
                    roots.append(Path(source).parent / rpath.removeprefix("@executable_path/"))
                elif rpath.startswith("/"):
                    roots.append(Path(rpath))
            candidates = [root / suffix for root in roots]
        elif name.startswith("/"):
            candidates = [Path(name)]
        else:
            candidates = [self.destination / "lib" / name]
        for candidate in candidates:
            if candidate.is_file():
                equivalent = self._destination_equivalent(candidate)
                return equivalent or candidate.resolve()
        # A bottle can encode a Cellar path that is not the current opt symlink.
        if name.startswith("/"):
            decoded = brew_formula_and_relative(Path(name), self.prefix)
            if decoded:
                formula, relative = decoded
                candidate = self._formula_root(formula) / relative
                if candidate.is_file():
                    return candidate.resolve()
        raise RuntimeError(f"Cannot resolve Mach-O dependency {name!r} from {source}")


def relocate_dependencies(destination, prefix):
    """Copy and rewrite the complete non-system Mach-O dependency closure."""
    destination, prefix = Path(destination).resolve(), Path(prefix).resolve()
    resolver = BrewDependencyResolver(destination, prefix)
    pending = [(path, path) for path in destination.rglob("*")
               if path.is_file() and not path.is_symlink() and macho(path)]
    copied = {}
    while pending:
        target, source = pending.pop(0)
        changes = []
        for name in macho_dependencies(source):
            if system_dependency(name):
                continue
            dependency = resolver.resolve(name, source)
            try:
                dependency.relative_to(destination)
                library = dependency
            except ValueError:
                library = destination / "lib" / Path(name).name
                digest = sha256(dependency)
                if library in copied and copied[library] != digest:
                    raise RuntimeError(f"Conflicting Homebrew dylibs named {library.name}")
                if library not in copied:
                    if library.exists() and sha256(library) != digest:
                        raise RuntimeError(f"Homebrew dylib collides with LLVM payload: {library}")
                    library.parent.mkdir(parents=True, exist_ok=True)
                    if not library.exists():
                        shutil.copy2(dependency, library)
                    copied[library] = digest
                    pending.append((library, dependency))
            changes += ["-change", name, "@rpath/" + library.name]
        if target.suffix == ".dylib":
            changes += ["-id", "@rpath/" + target.name]
        desired = "@loader_path" if target.parent == destination / "lib" else "@loader_path/../lib"
        for old in macho_rpaths(source):
            if old != desired:
                changes += ["-delete_rpath", old]
        if desired not in macho_rpaths(source):
            changes += ["-add_rpath", desired]
        if changes:
            subprocess.run(["install_name_tool", *changes, str(target)], check=True)
        subprocess.run(["codesign", "--force", "--sign", "-", str(target)], check=True)

    # No copied or extracted binary may retain an absolute Homebrew load path.
    for path in destination.rglob("*"):
        if not path.is_file() or path.is_symlink() or not macho(path):
            continue
        for name in macho_dependencies(path):
            if not system_dependency(name) and not (name.startswith("@rpath/") and
                                                     (destination / "lib" / name[7:]).is_file()):
                raise RuntimeError(f"LLVM retains external Mach-O dependency {name!r} in {path}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--destination", type=Path, required=True)
    args = parser.parse_args()
    if platform.system() != "Darwin":
        parser.error("Run LLVM preparation on macOS")
    arch = {"arm64": "aarch64", "x86_64": "x86_64"}[platform.machine()]
    expected = BOTTLES[arch]
    destination = args.destination.resolve()
    destination.mkdir(parents=True, exist_ok=False)
    archive = destination.parent / f"llvm-22.1.8-{arch}.bottle.tar.gz"
    url = "https://ghcr.io/v2/homebrew/core/llvm/blobs/sha256:" + expected
    if not archive.exists():
        with urllib.request.urlopen("https://ghcr.io/token?service=ghcr.io&scope=repository:homebrew/core/llvm:pull") as response:
            token = json.load(response)["token"]
        request = urllib.request.Request(url, headers={"Authorization": "Bearer " + token})
        # Do not forward registry credentials through object-store redirects.
        class Redirect(urllib.request.HTTPRedirectHandler):
            def redirect_request(self, request, *arguments):
                redirected = super().redirect_request(request, *arguments)
                if redirected is not None:
                    redirected.remove_header("Authorization")
                return redirected
        with urllib.request.build_opener(Redirect()).open(request, timeout=120) as response, archive.open("wb") as target:
            received = 0
            while data := response.read(8 * 1024 * 1024):
                target.write(data)
                received += len(data)
                if received % (128 * 1024 * 1024) == 0:
                    print(f"LLVM bottle: {received // (1024 * 1024)} MiB", flush=True)
    if sha256(archive) != expected:
        raise RuntimeError("LLVM macOS bottle checksum mismatch")
    with tarfile.open(archive, "r|gz") as bundle:
        for item in bundle:
            parts = Path(item.name).parts
            if len(parts) < 3 or parts[:2] != ("llvm", "22.1.8"):
                continue
            relative = Path(*parts[2:])
            if not (relative.parts[0] == "include" or
                    (relative.parts[0] == "bin" and relative.name in TOOLS) or
                    (relative.parts[0] == "lib" and
                     ("clang" in relative.parts or relative.name.endswith(".dylib"))) or
                    relative.name.lower().startswith(("license", "notice"))):
                continue
            item.name = relative.as_posix()
            # Hardlinks in bottles use paths relative to the archive root.
            if item.islnk():
                prefix = "llvm/22.1.8/"
                if not item.linkname.startswith(prefix):
                    raise RuntimeError("LLVM hardlink escapes bottle root")
                item.linkname = item.linkname.removeprefix(prefix)
            bundle.extract(item, destination, filter="data")
    prefix = Path(subprocess.check_output(["brew", "--prefix"], text=True).strip())
    relocate_dependencies(destination, prefix)
    for name in ("clang++", "llvm-ranlib"):
        path = destination / "bin" / name
        if not path.exists():
            path.symlink_to("clang" if name == "clang++" else "llvm-ar")
    env = os.environ.copy()
    env["DYLD_LIBRARY_PATH"] = str(destination / "lib")
    subprocess.run([str(destination / "bin/clang"), "--version"], env=env, check=True)
    (destination / "VYX-LLVM-INPUT.json").write_text(json.dumps({
        "version": "22.1.8", "arch": arch, "sha256": expected, "url": url,
        "formula_commit": FORMULA_COMMIT}, indent=2) + "\n")


if __name__ == "__main__":
    main()
