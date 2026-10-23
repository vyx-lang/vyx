#!/usr/bin/env python3
"""Extract only LLVM development inputs, in a single streaming XZ pass."""
import argparse
import json
from pathlib import Path, PurePosixPath
import shutil
import tarfile
import time

from common import api, download, sha256

VERSION = "22.1.8"
TOOLS = {"clang.exe", "clang++.exe", "clang-cl.exe", "clang-22.exe", "lld-link.exe",
         "llvm-ar.exe", "llvm-nm.exe", "llvm-strip.exe", "llvm-readobj.exe", "llvm-config.exe"}


def relative_member(name):
    path = PurePosixPath(name)
    if path.is_absolute() or ".." in path.parts or "\\" in name or ":" in name:
        raise ValueError(f"Unsafe LLVM archive member: {name}")
    return PurePosixPath(*path.parts[1:])


def selected(path):
    parts = path.parts
    return (parts[:1] == ("include",)
            or (len(parts) == 2 and parts[0] == "lib" and path.suffix == ".lib")
            or parts[:2] == ("lib", "clang")
            or (len(parts) == 2 and parts[0] == "bin"
                and (path.name in TOOLS or path.suffix == ".dll"))
            or path.name.lower().startswith(("license", "notice")))


def extract_development(archive, destination):
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=False)
    started, last_report = time.monotonic(), time.monotonic()
    count, size, links = 0, 0, []
    # Windows bsdtar's full .tar.xz extraction was still running after 97 min.
    # Streaming keeps one decoder and skips unrelated executables/docs/PDBs.
    with tarfile.open(archive, mode="r|xz") as bundle:
        for member in bundle:
            relative = relative_member(member.name)
            if selected(relative):
                target = destination.joinpath(*relative.parts)
                if member.isfile():
                    target.parent.mkdir(parents=True, exist_ok=True)
                    with bundle.extractfile(member) as source, target.open("wb") as output:
                        shutil.copyfileobj(source, output, length=1024 * 1024)
                    count += 1
                    size += member.size
                elif member.islnk():
                    links.append((target, destination.joinpath(*relative_member(member.linkname).parts)))
                elif not member.isdir():
                    raise ValueError(f"Unsupported LLVM archive member: {member.name}")
            if time.monotonic() - last_report >= 15:
                print(f"  LLVM: {count} files, {size // (1024 * 1024)} MiB selected, "
                      f"{time.monotonic() - started:.0f}s", flush=True)
                last_report = time.monotonic()
    while links:
        remaining = []
        for target, source in links:
            if not source.is_file():
                remaining.append((target, source))
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
        if len(remaining) == len(links):
            raise ValueError(f"Unresolved LLVM hard links: {remaining}")
        links = remaining
    for name in ("clang++.exe", "clang-cl.exe"):
        target = destination / "bin" / name
        if not target.exists() and (destination / "bin/clang.exe").is_file():
            shutil.copy2(destination / "bin/clang.exe", target)
    for required in ("include/llvm/IR/Module.h", "lib/LLVMCore.lib", "bin/clang.exe",
                     "bin/clang++.exe", "bin/clang-cl.exe", "bin/lld-link.exe", "bin/llvm-ar.exe"):
        if not (destination / required).is_file():
            raise RuntimeError(f"LLVM archive is missing {required}")
    print(f"LLVM development files ready in {time.monotonic() - started:.1f}s", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--destination", required=True, type=Path)
    parser.add_argument("--archive", type=Path, help="Local official archive, for extraction diagnostics")
    args = parser.parse_args()
    if args.archive:
        extract_development(args.archive, args.destination)
        return
    release = api(f"repos/llvm/llvm-project/releases/tags/llvmorg-{VERSION}")
    name = f"clang+llvm-{VERSION}-x86_64-pc-windows-msvc.tar.xz"
    asset = next(item for item in release["assets"] if item["name"] == name)
    digest = asset.get("digest", "")
    if not digest.startswith("sha256:"):
        raise RuntimeError("LLVM release has no SHA-256 asset digest")
    archive = args.destination.parent / name
    if not archive.is_file() or sha256(archive) != digest.removeprefix("sha256:"):
        download(asset["browser_download_url"], archive)
    if sha256(archive) != digest.removeprefix("sha256:"):
        raise RuntimeError("LLVM archive digest mismatch")
    extract_development(archive, args.destination)
    (args.destination / "VYX-LLVM-INPUT.json").write_text(json.dumps({
        "version": VERSION, "asset_id": asset["id"], "archive_sha256": digest[7:]
    }, indent=2) + "\n")


if __name__ == "__main__":
    main()
