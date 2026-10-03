#!/usr/bin/env python3
"""Fixture implementation of the language-neutral external DCI Stub protocol."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys


REPO_ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(REPO_ROOT))

from tools.dci import dcib


SYMBOL = "dci_rust_stub_add"
SEMANTIC_ID = "fixture.external_stub.add_900"
CACHE_PROBE = 1


def emit(args: argparse.Namespace) -> int:
    descriptor_path = Path(args.descriptor)
    if descriptor_path.suffix.lower() != ".dcib":
        raise RuntimeError("external DCI Stub backend requires a .dcib descriptor")
    descriptor = dcib.decode(descriptor_path.read_bytes())
    symbols = descriptor.get("exports", {}).get("symbols", [])
    matched = [
        symbol
        for symbol in symbols
        if symbol.get("semantic_id") == SEMANTIC_ID
        and symbol.get("link_name") == SYMBOL
    ]
    if len(matched) != 1:
        raise RuntimeError("normalized DCI stub symbol is missing or ambiguous")

    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(
        "#![no_std]\n\n"
        "#[used]\n"
        f"static DCI_CACHE_PROBE: u8 = {CACHE_PROBE};\n\n"
        "#[no_mangle]\n"
        f"pub extern \"system\" fn {SYMBOL}(value: i32) -> i32 {{\n"
        "    value + 900\n"
        "}\n",
        encoding="utf-8",
    )
    return 0


def compile_source(args: argparse.Namespace) -> int:
    rustc = os.environ.get("RUSTC") or shutil.which("rustc")
    if not rustc:
        raise RuntimeError("rustc was not found")
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    command = [
        rustc,
        "--crate-name",
        "dci_rust_external_stub",
        "--crate-type",
        "lib",
        "--edition",
        "2021",
        "--target",
        "x86_64-pc-windows-msvc",
        "--emit=obj",
        "-C",
        "panic=abort",
        "-C",
        "opt-level=2",
        args.source,
        "-o",
        str(output),
    ]
    return subprocess.run(command, check=False).returncode


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser()
    commands = root.add_subparsers(dest="command", required=True)

    emit_parser = commands.add_parser("emit")
    emit_parser.add_argument("--host-source", required=True)
    emit_parser.add_argument("--output", required=True)
    emit_parser.add_argument("--compiler", required=True)
    emit_parser.add_argument("--unit-sources")
    emit_parser.add_argument("--descriptor", required=True)
    emit_parser.add_argument("--host-args")
    emit_parser.set_defaults(run=emit)

    compile_parser = commands.add_parser("compile")
    compile_parser.add_argument("--source", required=True)
    compile_parser.add_argument("--output", required=True)
    compile_parser.add_argument("--include-args")
    compile_parser.add_argument("--compile-args")
    compile_parser.set_defaults(run=compile_source)
    return root


def main(argv: list[str]) -> int:
    args = parser().parse_args(argv)
    return args.run(args)


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except Exception as error:
        print(f"external DCI Stub backend failed: {error}", file=sys.stderr)
        raise SystemExit(1)
