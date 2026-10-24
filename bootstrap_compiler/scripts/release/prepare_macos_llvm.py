#!/usr/bin/env python3
"""Prepare verified LLVM 22 development files for a native macOS runner."""
import argparse
import json
import os
from pathlib import Path
import platform
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


def macho(path):
    with path.open("rb") as source:
        return source.read(4) in (b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf",
                                  b"\xca\xfe\xba\xbe", b"\xca\xfe\xba\xbf")


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
    prefix = subprocess.check_output(["brew", "--prefix"], text=True).strip()
    replacements = {
        "@@HOMEBREW_PREFIX@@/opt/llvm": str(destination),
        "@@HOMEBREW_CELLAR@@/llvm/22.1.8": str(destination),
        prefix + "/opt/llvm": str(destination),
        prefix + "/Cellar/llvm/22.1.8": str(destination),
        "@@HOMEBREW_PREFIX@@": prefix,
        "@@HOMEBREW_CELLAR@@": prefix + "/Cellar",
    }
    for path in destination.rglob("*"):
        if not path.is_file() or path.is_symlink() or not macho(path):
            continue
        dependencies = subprocess.check_output(["otool", "-L", str(path)], text=True)
        for row in dependencies.splitlines()[1:]:
            original = row.strip().split(" (", 1)[0]
            rewritten = original
            for old, new in replacements.items():
                rewritten = rewritten.replace(old, new)
            if rewritten != original:
                subprocess.run(["install_name_tool", "-change", original, rewritten, str(path)], check=True)
        if path.suffix == ".dylib":
            subprocess.run(["install_name_tool", "-id", str(path), str(path)], check=True)
        subprocess.run(["codesign", "--force", "--sign", "-", str(path)], check=True)
    for name in ("clang++", "llvm-ranlib"):
        path = destination / "bin" / name
        if not path.exists():
            path.symlink_to("clang" if name == "clang++" else "llvm-ar")
    env = os.environ.copy()
    env["DYLD_LIBRARY_PATH"] = str(destination / "lib") + ":" + prefix + "/opt/zstd/lib"
    subprocess.run([str(destination / "bin/clang"), "--version"], env=env, check=True)
    (destination / "VYX-LLVM-INPUT.json").write_text(json.dumps({
        "version": "22.1.8", "arch": arch, "sha256": expected, "url": url,
        "formula_commit": FORMULA_COMMIT}, indent=2) + "\n")


if __name__ == "__main__":
    main()
