#!/usr/bin/env python3
"""Download the LLVM 22 development archive and verify GitHub's SHA-256 digest."""
import argparse
from pathlib import Path
import subprocess

from common import api, download, sha256

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--destination", required=True, type=Path)
args = parser.parse_args()
release = api("repos/llvm/llvm-project/releases/tags/llvmorg-22.1.8")
name = "clang+llvm-22.1.8-x86_64-pc-windows-msvc.tar.xz"
asset = next(item for item in release["assets"] if item["name"] == name)
digest = asset.get("digest", "")
if not digest or not digest.startswith("sha256:"):
    raise RuntimeError("LLVM release has no SHA-256 asset digest")
archive = download(asset["browser_download_url"], args.destination.parent / name)
if sha256(archive) != digest.removeprefix("sha256:"):
    raise RuntimeError("LLVM archive digest mismatch")
args.destination.mkdir(parents=True, exist_ok=False)
subprocess.run(["tar", "-xf", str(archive), "--strip-components=1", "-C", str(args.destination)], check=True)
if not (args.destination / "lib/LLVMCore.lib").is_file():
    raise RuntimeError("LLVM archive is missing development libraries")
