#!/usr/bin/env python3
"""Check declared instance layouts against the producer's real artifact.

`merge_instance_layouts.py` lets a project state a generic type's instance
layout: the adapter measures the instances a provider *explicitly instantiates*,
and a statement covers what no instantiation produced (or pins a spelling).  A
statement about an artifact has to be checked *against that artifact*, not
trusted -- otherwise "the contract says 16 bytes" and "the compiler lays it out
as 16 bytes" are two unrelated claims that merely agree today.

This tool reads the layout back out of the contract and compiles a probe with
the producer's own compiler asserting, for every declared field:

    sizeof/alignof == the contract's size/alignment
    offsetof(field) == the contract's offset

so a contract that drifts from the producer fails the build with the producer's
compiler as the judge.

    verify_instance_layouts.py <contract.dcib> <declarations.json> \
        --lang rust|cpp --provider <producer source>

A declaration names a language through `provider_spelling`: the contract's
`type_name` is the *consumer's* spelling (`Pair2<f64,i32>`) and is not valid in
the producer's language (`Pair2<double, int>`, `Pair2<f64, i32>`).  A declaration
that names no spelling for the language being checked states nothing about that
producer, so it is skipped -- loudly, in the summary line, so it cannot pass
unnoticed -- instead of being failed; a declaration that *does* name the language
must be carried by that language's contract.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from tools.dci import dcib  # noqa: E402

TARGET = "x86_64-pc-windows-msvc"


def _clangxx() -> str:
    for candidate in (os.environ.get("CLANGXX"),
                      REPO_ROOT / "clang" / "bin" / "clang++.exe"):
        if candidate and Path(candidate).is_file():
            return str(Path(candidate))
    found = shutil.which("clang++")
    if not found:
        raise RuntimeError("clang++ was not found")
    return found


def _cpp_probe(provider: Path, checks: list[tuple[str, dict]]) -> str:
    lines = ["#include <cstddef>", f'#include "{provider.as_posix()}"']
    for index, (spelling, layout) in enumerate(checks):
        # `offsetof` is a macro, so a template-id spelled inline would have its
        # comma read as an argument separator.  An alias keeps the type out of
        # the macro's argument list.
        alias = f"Probe{index}"
        lines.append(f"using {alias} = {spelling};")
        lines.append(f'static_assert(sizeof({alias}) == {layout["size"]}, '
                     f'"{spelling} size");')
        lines.append(f'static_assert(alignof({alias}) == '
                     f'{layout["alignment"]}, "{spelling} alignment");')
        for field in layout.get("fields") or []:
            lines.append(
                f'static_assert(offsetof({alias}, {field["name"]}) == '
                f'{field["offset"]}, "{spelling}.{field["name"]} offset");')
    return "\n".join(lines) + "\n"


def _rust_probe(provider: Path, checks: list[tuple[str, dict]]) -> str:
    lines = [f'#[path = "{provider.as_posix()}"]', "mod provider;",
             # A record used as a *type argument* is spelled bare in the
             # declaration's provider spelling (`Pair2<Vec2, i32>`) but lives
             # in the provider module -- without this glob the probe fails to
             # resolve `Vec2`, which reads like a layout mismatch but is just
             # a missing import.
             "use provider::*;"]
    for spelling, layout in checks:
        qualified = f"provider::{spelling}"
        lines.append(f"const _: () = assert!(std::mem::size_of::<{qualified}>() "
                     f'== {layout["size"]}, "{qualified} size");')
        lines.append(f"const _: () = assert!(std::mem::align_of::<{qualified}>() "
                     f'== {layout["alignment"]}, "{qualified} alignment");')
        for field in layout.get("fields") or []:
            lines.append(
                f"const _: () = assert!(std::mem::offset_of!({qualified}, "
                f'{field["name"]}) == {field["offset"]}, '
                f'"{qualified}.{field["name"]} offset");')
    return "\n".join(lines) + "\n"


def _layout_key(text: str | None) -> str:
    """Match type names whitespace-insensitively inside generic arguments.

    The compiler itself is space-tolerant (`Pair2<f64,i32>` and
    `Pair2<f64, i32>` are the same instance to it), so the cross-check must
    be too -- otherwise a producer that spells instances with spaces looks
    like it "does not carry" a layout the consumer names compactly.
    """
    return re.sub(r"\s+", "", text or "")


def verify(contract: Path, declarations: dict, lang: str,
           provider: Path) -> str:
    document = dcib.decode(contract.read_bytes())
    layouts = {_layout_key(layout.get("type_name")): layout
               for layout in document.get("exports", {}).get("layouts", [])}

    checks: list[tuple[str, dict]] = []
    skipped: list[str] = []
    fields = 0
    for declared in declarations.get("layouts", []):
        name = declared.get("type_name")
        spelling = (declared.get("provider_spelling") or {}).get(lang)
        if not spelling:
            # The declaration names no spelling for this language, so it states
            # nothing about this producer: skip it *out loud* (see the return
            # summary).  A declaration that does name the language is still
            # required to be carried by the contract below, and if every
            # declaration is skipped the `not checks` guard still fails.
            skipped.append(str(name))
            continue
        layout = layouts.get(_layout_key(name))
        if layout is None:
            raise RuntimeError(
                f"{name}: declared as an instance layout for {lang} but the "
                f"contract does not carry it -- the declaration is not being "
                f"applied")
        fields += len(layout.get("fields") or [])
        checks.append((spelling, layout))

    if not checks:
        raise RuntimeError("nothing to verify: no declared instance layouts")

    work = Path(tempfile.mkdtemp(prefix="dci-instance-layout-"))
    # The probe embeds the provider by path (`#include` / `#[path]`), and both
    # resolve such paths relative to the *probe file's* directory, which is a
    # temp dir -- not the caller's cwd.  Anchor to an absolute path.
    provider = provider.resolve()
    if lang == "cpp":
        probe = work / "probe.cpp"
        probe.write_text(_cpp_probe(provider, checks), encoding="utf-8")
        command = [_clangxx(), "-std=c++20", f"--target={TARGET}",
                   "-fsyntax-only", str(probe)]
    elif lang == "rust":
        probe = work / "probe.rs"
        probe.write_text(_rust_probe(provider, checks), encoding="utf-8")
        rustc = os.environ.get("RUSTC") or shutil.which("rustc")
        if not rustc:
            raise RuntimeError("rustc was not found")
        command = [rustc, "--edition", "2021", "--crate-type", "lib",
                   "--emit=metadata", "--out-dir", str(work), str(probe)]
    else:
        raise RuntimeError(f"unknown producer language {lang!r}")

    result = subprocess.run(command, capture_output=True, text=True,
                            check=False)
    if result.returncode != 0:
        sys.stderr.write(result.stderr)
        raise RuntimeError(
            f"the {lang} producer lays out a declared instance differently "
            f"than the contract says")
    summary = (f"instance layouts verified with {lang}: {len(checks)} type(s), "
               f"{fields} field(s) "
               f"[{', '.join(spelling for spelling, _ in checks)}]")
    if skipped:
        # Say what was *not* checked, so "0 failures" cannot be read as
        # "everything was checked".
        summary += (f"; skipped (no {lang} spelling declared): {skipped}")
    return summary


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        prog="verify-instance-layouts",
        description=(
            "Verify every instance layout a contract carries against the "
            "producer's own compiler (sizeof/alignof/field offsets), using "
            "the declarations file merged earlier.  Fully skipped "
            "verifications (no spelling declared for the language) are "
            "reported loudly; a contract that verifies nothing still fails."
        ),
    )
    parser.add_argument("contract", help=".dcib contract to verify")
    parser.add_argument("declarations",
                        help='JSON file {"layouts": [...], "provider_spelling": {...}}')
    parser.add_argument("--lang", required=True, choices=("rust", "cpp"),
                        help="consumer language whose spellings are checked")
    parser.add_argument("--provider", required=True,
                        help="producer source the contract was generated from")
    args = parser.parse_args(argv)
    print(verify(Path(args.contract),
                 json.loads(Path(args.declarations).read_text(encoding="utf-8")),
                 args.lang, Path(args.provider)))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except Exception as error:
        print(f"verify-instance-layouts: {error}", file=sys.stderr)
        raise SystemExit(1)
