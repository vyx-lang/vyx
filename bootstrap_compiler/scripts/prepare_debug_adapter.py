#!/usr/bin/env python3
"""Install a pinned, verified CodeLLDB runtime for SDK packaging/source builds."""
import argparse
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import urllib.request
import zipfile

VERSION = "1.12.3"
RELEASE = f"https://github.com/vadimcn/codelldb/releases/download/v{VERSION}"
ARTIFACTS = {
    "windows-x86_64": ("codelldb-win32-x64.vsix", "a916e509308dac817732f63ca604a8b93ed29cd16f38a2fa9f0b64ed58e8f51a"),
    "linux-x86_64": ("codelldb-linux-x64.vsix", "1cd7f386598022b51a5b93b9ffa23e812b23f519cfe1833384ec4bef4bfd1be1"),
}
SOURCE_COMMIT = "62def434dc22c1d77d4837c4d99cddef7ac3338f"
SOURCE_SHA256 = "551f1bdfa72ad164460f8ae4e9df17cbef55132f0d873c94d2e519dc81402ab1"
HEADERS = {
    "windows-x86_64": ("x86_64-windows-msvc", "e35318953efd08c3bbd9f6bbc27a9f6f5b1de542ba06a78748ed2f2a89a12995"),
    "linux-x86_64": ("x86_64-linux-gnu", "9e5d9f183bbb950a4724441bb13553ec4b47b9b8ec04902258df7d557cd2fb7f"),
}


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def verified_archive(path, url, expected):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.is_file():
        temporary = path.with_suffix(path.suffix + ".partial")
        with urllib.request.urlopen(url, timeout=60) as response, temporary.open("wb") as out:
            shutil.copyfileobj(response, out, 1024 * 1024)
        if digest(temporary) != expected:
            raise RuntimeError(f"archive hash mismatch: {temporary}")
        temporary.replace(path)
    if digest(path) != expected:
        raise RuntimeError(f"archive hash mismatch: {path}")
    return path


def unpack(archive, dest, transform):
    with zipfile.ZipFile(archive) as bundle:
        for info in bundle.infolist():
            if info.is_dir():
                continue
            relative = transform(pathlib.PurePosixPath(info.filename))
            if relative is None:
                continue
            if not relative.parts or ".." in relative.parts or relative.is_absolute():
                raise RuntimeError(f"unsafe archive member: {info.filename}")
            output = dest.joinpath(*relative.parts)
            output.parent.mkdir(parents=True, exist_ok=True)
            payload = bundle.read(info)
            if output.is_file() and output.read_bytes() == payload:
                continue
            output.write_bytes(payload)
            mode = info.external_attr >> 16
            if os.name != "nt" and mode:
                output.chmod(mode & 0o777)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--platform", choices=ARTIFACTS, default="windows-x86_64" if os.name == "nt" else "linux-x86_64")
    ap.add_argument("--archive", type=pathlib.Path, help="Use an already downloaded VSIX (hash checked).")
    ap.add_argument("--destination", type=pathlib.Path, default=pathlib.Path(__file__).resolve().parents[1] / "out/_debug_adapter")
    ap.add_argument("--build-root", type=pathlib.Path, help="Ignored source/header/Cargo build cache.")
    ap.add_argument("--source-archive", type=pathlib.Path)
    ap.add_argument("--headers-archive", type=pathlib.Path)
    ap.add_argument("--cxx", type=pathlib.Path, help="clang++ for the LLDB C++ bindings.")
    ap.add_argument("--target-dir", type=pathlib.Path, help="Optional existing Cargo target cache.")
    ap.add_argument("--jobs", type=int, default=min(os.cpu_count() or 1, 8))
    args = ap.parse_args()
    host = "windows-x86_64" if os.name == "nt" else "linux-x86_64"
    if args.platform != host:
        raise RuntimeError("Build the adapter on the matching host; cross compilation is not supported here.")
    asset, expected = ARTIFACTS[args.platform]
    dest = args.destination.resolve()
    dest.mkdir(parents=True, exist_ok=True)
    build = (args.build_root or dest.parent / "_debug_build").resolve()
    build.mkdir(parents=True, exist_ok=True)
    archive = verified_archive(args.archive or build / asset, f"{RELEASE}/{asset}", expected)
    patch = pathlib.Path(__file__).resolve().parents[2] / "tools/debugger/patches/codelldb-1.12.3-vyx.patch"
    patch_hash = digest(patch)
    source_archive = verified_archive(args.source_archive or build / "source.zip",
        f"https://codeload.github.com/vadimcn/codelldb/zip/{SOURCE_COMMIT}", SOURCE_SHA256)
    source = build / f"source-{patch_hash[:16]}"
    if not (source / ".git").is_dir():
        prefix = f"codelldb-{SOURCE_COMMIT}"
        unpack(source_archive, source, lambda p: p.relative_to(prefix) if p.parts[0] == prefix else None)
        subprocess.run(["git", "init", "--quiet", str(source)], check=True)
        subprocess.run(["git", "-C", str(source), "apply", "--check", str(patch)], check=True)
        subprocess.run(["git", "-C", str(source), "apply", str(patch)], check=True)
    subprocess.run(["git", "-C", str(source), "apply", "--reverse", "--check", str(patch)], check=True)
    triple, header_hash = HEADERS[args.platform]
    header_archive = verified_archive(args.headers_archive or build / f"headers-{triple}.zip",
        f"https://github.com/vadimcn/lldb-build/releases/download/codelldb%2F22.x-74/lldb--{triple}.zip", header_hash)
    headers = build / f"headers-{triple}"
    unpack(header_archive, headers, lambda p: p if p.parts[0] == "include" else None)

    def runtime_member(p):
        if len(p.parts) < 2 or p.parts[0] != "extension": return None
        relative = p.relative_to("extension")
        # Never install the stock executable: it does not implement our paging
        # or Windows output isolation. Preserve a working binary if build fails.
        if relative.as_posix() in ("adapter/codelldb", "adapter/codelldb.exe"):
            return None
        return relative if relative.parts[0] in ("adapter", "lldb", "lang_support", "bin") else None
    unpack(archive, dest, runtime_member)
    found_cxx = str(args.cxx) if args.cxx else shutil.which("clang++")
    if not found_cxx:
        raise RuntimeError("clang++ is required; pass --cxx with its path.")
    cxx = pathlib.Path(found_cxx).resolve()
    if not cxx.is_file():
        raise RuntimeError(f"clang++ does not exist: {cxx}")
    env = os.environ.copy()
    env.update(LLDB_INCLUDE=str(headers / "include"),
        LLDB_DYLIB=str(dest / ("lldb/bin/liblldb.dll" if os.name == "nt" else "lldb/lib/liblldb.so")),
        ADAPTER_SOURCE_DIR=str(source / "adapter"), CXX=str(cxx.resolve()),
        CARGO_TARGET_DIR=str((args.target_dir or build / "target").resolve()), CARGO_NET_GIT_FETCH_WITH_CLI="true")
    cargo = shutil.which("cargo")
    if not cargo: raise RuntimeError("Rust/Cargo is required to build the patched SDK adapter.")
    subprocess.run([cargo, "build", "--manifest-path", str(source / "Cargo.toml"),
        "--locked", "--package", "codelldb", "--bin", "codelldb", "--features", "no_link_args",
        "--release", "-j", str(args.jobs)], env=env, check=True)
    binary_name = "codelldb.exe" if os.name == "nt" else "codelldb"
    binary = dest / "adapter" / binary_name
    temporary_binary = binary.with_suffix(binary.suffix + ".new")
    shutil.copy2(pathlib.Path(env["CARGO_TARGET_DIR"]) / "release" / binary_name, temporary_binary)
    temporary_binary.replace(binary)
    shutil.copy2(source / "LICENSE", dest / "LICENSE-CodeLLDB.txt")
    shutil.copy2(patch.parents[1] / "LICENSE-LLVM.txt", dest / "LICENSE-LLVM.txt")
    # Retain the full source patch and notice with the distributed binary.
    shutil.copy2(patch, dest / "VYX-CHANGES.patch")
    (dest / "NOTICE-Vyx.txt").write_text("CodeLLDB 1.12.3, modified for Vyx: variable pagination, "
        "Windows internal-console isolation and C++17 bindings. Original project: "
        "https://github.com/vadimcn/codelldb. See VYX-CHANGES.patch and component licenses.\n", encoding="utf-8")
    (dest / "MANIFEST.json").write_text(json.dumps({
        "name": "CodeLLDB", "version": VERSION, "platform": args.platform,
        "source": f"{RELEASE}/{asset}", "sha256": expected,
        "source_commit": SOURCE_COMMIT, "source_sha256": SOURCE_SHA256,
        "patch_sha256": patch_hash, "binary_sha256": digest(binary), "llvm": "22.1.8-codelldb",
        "license": "MIT; bundled LLVM Apache-2.0 WITH LLVM-exception; bundled component licenses retained",
    }, indent=2) + "\n", encoding="utf-8")
    print(f"CodeLLDB {VERSION}: {dest}")


if __name__ == "__main__":
    main()
