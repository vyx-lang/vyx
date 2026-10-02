#!/usr/bin/env python3
"""Compile a Vyx rustc Stub crate against the Adapter-measured producer rlib.

The producer crate must be compiled alone so rustc-mangled `extern "Rust"`
symbols keep the identity hash recorded in the DCI contract.  The generated
stub crate then depends on that rlib and is emitted as a staticlib so the
host C/C++ linker receives libstd/alloc.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

import dci_adapter_rust


HEADER_KEYS = (
    "dci-rust-producer",
    "dci-rust-crate-name",
    "dci-rust-edition",
    "dci-rust-target",
)
OPTIONAL_HEADER_KEYS = (
    "dci-rust-opt-level",
)


class StubCompileError(RuntimeError):
    pass


def parse_stub_header(source: str) -> dict[str, str]:
    values: dict[str, str] = {}
    known = set(HEADER_KEYS) | set(OPTIONAL_HEADER_KEYS)
    for raw_line in source.splitlines():
        line = raw_line.strip()
        if not line.startswith("//"):
            if line.startswith("#!") or not line:
                continue
            break
        body = line[2:].strip()
        if "=" not in body:
            continue
        key, value = body.split("=", 1)
        key = key.strip()
        if key in known:
            values[key] = value.strip()
    missing = [key for key in HEADER_KEYS if not values.get(key)]
    if missing:
        raise StubCompileError(
            "generated rustc stub is missing header fields: " + ", ".join(missing)
        )
    return values


def run_rustc(command: list[str], cwd: Path | None = None) -> str:
    try:
        result = subprocess.run(command, capture_output=True, text=True, cwd=cwd)
    except OSError as exc:
        raise StubCompileError(f"failed to launch rustc: {exc}") from exc
    output = ((result.stderr or "") + (result.stdout or "")).strip()
    if result.returncode != 0:
        raise StubCompileError(
            "command failed ({0}): {1}\n{2}".format(
                result.returncode, " ".join(command), output,
            )
        )
    return output


def wrap_stub_with_native_links(stub: Path, libs: list[str]) -> Path:
    if not libs:
        return stub
    extra = "".join(f'#[link(name = "{name}")]\n' for name in libs)
    extra += "unsafe extern \"C\" {}\n\n"
    lines = stub.read_text(encoding="utf-8").splitlines(keepends=True)
    insert_at = 0
    for i, line in enumerate(lines):
        stripped = line.strip()
        if stripped.startswith("//") or stripped.startswith("#![") or stripped == "":
            insert_at = i + 1
            continue
        break
    lines.insert(insert_at, extra)
    wrapped = stub.with_name(stub.name + ".nativelibs.rs")
    wrapped.write_text("".join(lines), encoding="utf-8")
    return wrapped


def native_libs_for_target(target: str) -> list[str]:
    if "windows" in target:
        return ["kernel32", "ntdll", "userenv", "ws2_32", "dbghelp"]
    return []


def producer_rlib_path(output: Path, crate_name: str) -> Path:
    # rustc rejects names that are not lib*.rlib (or *.dll).  Keep the
    # output stem so two stub archives in one cache directory cannot collide.
    return output.with_name(f"lib{crate_name}.{output.stem}.rlib")


def compile_opt_level(header: dict[str, str], env_name: str, default: str) -> str:
    env = os.environ.get(env_name, "").strip()
    if env:
        return env
    recorded = header.get("dci-rust-opt-level", "").strip()
    if recorded:
        return recorded
    return default


def compile_stub(stub: Path, output: Path, rustc: str) -> None:
    header = parse_stub_header(stub.read_text(encoding="utf-8"))
    producer = Path(header["dci-rust-producer"]).resolve()
    if not producer.is_file():
        raise StubCompileError(f"rustc stub producer crate not found: {producer}")
    crate_name = header["dci-rust-crate-name"]
    edition = header["dci-rust-edition"]
    target = header["dci-rust-target"]
    stub = stub.resolve()
    output = output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    rustc_output = output
    if output.suffix.lower() not in {".lib", ".a"}:
        rustc_output = output.with_name(
            output.name + (".lib" if sys.platform == "win32" else ".a")
        )
    producer_rlib = producer_rlib_path(rustc_output, crate_name)
    native_libs = native_libs_for_target(target)
    linked_stub = wrap_stub_with_native_links(stub, native_libs)
    rustc_lib_args: list[str] = []
    for name in native_libs:
        rustc_lib_args.extend(["-l", name])
    producer_opt = compile_opt_level(header, "VYX_DCI_RUSTC_OPT_LEVEL", "0")
    stub_opt = compile_opt_level(header, "VYX_DCI_RUSTC_STUB_OPT_LEVEL", "2")
    producer_root = producer
    crate = dci_adapter_rust.RustParser(producer).parse()
    keep = dci_adapter_rust.unmanaged_pub_keep_source(crate)
    keep_dir = tempfile.TemporaryDirectory(prefix="vyx-dci-producer-") if keep else None
    if keep_dir is not None:
        copied = dci_adapter_rust.copy_crate_tree(producer, Path(keep_dir.name))
        copied.write_text(copied.read_text(encoding="utf-8") + keep, encoding="utf-8")
        producer_root = copied
    producer_ir = producer_rlib.with_suffix(".ll")
    producer_args = [
        rustc, str(producer_root),
        "--crate-name", crate_name,
        "--crate-type", "lib",
        "--emit=link",
        f"--emit=llvm-ir={producer_ir}",
        "-C", "panic=abort",
        "-C", "opt-level=" + producer_opt,
        "--edition", edition,
        "--target", target,
        "-o", str(producer_rlib),
    ]
    try:
        run_rustc(producer_args, cwd=rustc_output.parent)
        reexport = ""
        if producer_ir.is_file():
            ir_functions = dci_adapter_rust.llvm_functions(
                producer_ir.read_text(encoding="utf-8"),
            )
            reexport = dci_adapter_rust.unmanaged_pub_reexport_source(
                crate_name, crate, ir_functions,
            )
        if reexport:
            stub_text = linked_stub.read_text(encoding="utf-8")
            linked_stub = linked_stub.with_name(linked_stub.name + ".reexport.rs")
            linked_stub.write_text(stub_text + "\n" + reexport, encoding="utf-8")
        run_rustc([
            rustc, str(linked_stub),
            "--crate-name", "vyx_dci_stubs",
            "--crate-type", "staticlib",
            "-C", "panic=abort",
            "-C", "opt-level=" + stub_opt,
            "--edition", "2024",
            "--target", target,
            "--extern", f"{crate_name}={producer_rlib}",
            "-A", "unsafe_op_in_unsafe_fn",
            *rustc_lib_args,
            "-o", str(rustc_output),
        ], cwd=rustc_output.parent)
    except dci_adapter_rust.RustAdapterError as exc:
        raise StubCompileError(str(exc)) from exc
    finally:
        if keep_dir is not None:
            keep_dir.cleanup()
    if rustc_output != output:
        shutil.copyfile(rustc_output, output)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="dci-rustc-stub-compile")
    parser.add_argument("--stub", required=True, help="generated stub .rs")
    parser.add_argument("--output", required=True, help="staticlib output path")
    parser.add_argument("--rustc", help="rustc executable; defaults to RUSTC/PATH")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    rustc = args.rustc or shutil.which("rustc")
    if not rustc:
        print("dci-rustc-stub-compile: rustc not found", file=sys.stderr)
        return 2
    try:
        compile_stub(Path(args.stub), Path(args.output), rustc)
    except (StubCompileError, OSError, UnicodeError) as exc:
        print(f"dci-rustc-stub-compile: error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
