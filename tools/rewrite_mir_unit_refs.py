# Rewrite MirUnit-by-value helpers to & / &mut. One-shot, not a build step.
from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "bootstrap_compiler" / "src"

MUT_PREFIXES = (
    "add_",
    "set_",
    "remove_",
    "discard_",
    "tombstone_",
    "clear_",
)

UNIT_FN = re.compile(r"\bmir_unit_([A-Za-z0-9_]+)\(")
SIG_UNIT = re.compile(r"\bm: MirUnit\b")
SIG_MUT_ALREADY = "m: &mut MirUnit"


def is_mut(name: str) -> bool:
    return name.startswith(MUT_PREFIXES)


def amp_for(name: str) -> str:
    return "&mut " if is_mut(name) else "&"


def rewrite_signatures(text: str) -> str:
    # Keep existing &mut. Replace remaining by-value params.
    out = []
    i = 0
    for m in SIG_UNIT.finditer(text):
        # Skip if this is already part of `&mut MirUnit` — the pattern is `m: MirUnit`
        # and `&mut MirUnit` does not match `m: MirUnit`.
        out.append(text[i:m.start()])
        start = m.start()
        # If preceded by "mut " after "&", we wouldn't match. Good.
        out.append("m: &MirUnit")
        i = m.end()
    out.append(text[i:])
    text = "".join(out)
    # Mutating mir_unit_* wrappers: bump &MirUnit -> &mut MirUnit on those defs.
    def sig_mut(mm: re.Match[str]) -> str:
        name = mm.group(1)
        # only the definition line: `public fn mir_unit_NAME(m: &MirUnit`
        prefix = mm.group(0)
        if not is_mut(name):
            return prefix
        return prefix.replace("m: &MirUnit", "m: &mut MirUnit")

    text = re.sub(
        r"(?:public )?fn mir_unit_([A-Za-z0-9_]+)\(m: &MirUnit",
        sig_mut,
        text,
    )
    return text


def rewrite_unit_calls(text: str, *, first_arg_is_already_ref: bool) -> str:
    """Insert & / &mut on first args of mir_unit_* calls."""

    def repl(m: re.Match[str]) -> str:
        name = m.group(1)
        start = m.end()
        # first argument slice until comma or closing paren at depth 1
        rest = text[start:]
        if rest.startswith("&"):
            return m.group(0)
        amp = amp_for(name)
        if first_arg_is_already_ref:
            # Caller's `m` is already & / &mut MirUnit (wrapper body).
            return m.group(0)
        # Don't double-amp `&self.m` etc. Handled by startswith("&").
        return f"mir_unit_{name}({amp}"

    # We only prefix the opening `mir_unit_X(`  — actually we need to insert
    # after the paren. repl returns `mir_unit_X(&` replacing `mir_unit_X(`.
    return UNIT_FN.sub(repl, text)


HELPERS = (
    "llvm_mir_type_kind",
    "llvm_mir_type_text",
    "llvm_mir_max_static_item_target",
    "mir_backend_type_kind",
    "mir_backend_type_text",
)


def rewrite_helper_signatures(text: str) -> str:
    text = text.replace("fn llvm_mir_max_static_item_target(m: MirUnit)",
                        "fn llvm_mir_max_static_item_target(m: &MirUnit)")
    text = text.replace("fn llvm_mir_type_kind(m: MirUnit,",
                        "fn llvm_mir_type_kind(m: &MirUnit,")
    text = text.replace("fn llvm_mir_type_text(m: MirUnit,",
                        "fn llvm_mir_type_text(m: &MirUnit,")
    text = text.replace("public fn mir_backend_type_kind(m: MirUnit,",
                        "public fn mir_backend_type_kind(m: &MirUnit,")
    text = text.replace("public fn mir_backend_type_text(m: MirUnit,",
                        "public fn mir_backend_type_text(m: &MirUnit,")
    return text


def amp_helper_calls(text: str, first_args: str) -> str:
    for fn in HELPERS:
        text = re.sub(
            rf"\b{fn}\((?:&(?:mut )?)?({first_args})([,\)])",
            rf"{fn}(&\1\2",
            text,
        )
        text = text.replace(f"{fn}(&&mut ", f"{fn}(&mut ")
        text = text.replace(f"{fn}(&&", f"{fn}(&")
    return text


def main() -> None:
    model = SRC / "mir" / "mir_model.vyx"
    mt = rewrite_signatures(model.read_text(encoding="utf-8"))
    model.write_text(mt, encoding="utf-8")
    print("updated", model.relative_to(ROOT))

    backend = SRC / "mir" / "mir_backend.vyx"
    bt = rewrite_helper_signatures(backend.read_text(encoding="utf-8"))
    backend.write_text(bt, encoding="utf-8")
    print("updated", backend.relative_to(ROOT))

    # Lowerer stores MirUnit on self; amp field access, not the &MirUnit param `m`.
    lower = SRC / "codegen" / "llvm_lower.vyx"
    lt = rewrite_helper_signatures(lower.read_text(encoding="utf-8"))
    lt = amp_helper_calls(
        lt,
        r"self\.m|builder\.unit",
    )
    lt = rewrite_unit_calls(lt, first_arg_is_already_ref=False)
    lt = lt.replace("(&&mut ", "(&mut ").replace("(&&", "(&")
    lower.write_text(lt, encoding="utf-8")
    print("updated", lower.relative_to(ROOT))

    # These still take MirUnit by value, so amp the local `m`.
    byval_files = [
        SRC / "codegen" / "mir_cpp_lower.vyx",
        SRC / "mir" / "mir_verify.vyx",
        SRC / "mir" / "mir_pass.vyx",
        SRC / "mir" / "mir_dump.vyx",
        SRC / "mir" / "mir_analysis.vyx",
        SRC / "mir" / "mir_copy_prop.vyx",
        SRC / "mir" / "mir_sccp.vyx",
        SRC / "core" / "main.vyx",
    ]
    first = r"self\.m|builder\.unit|stream_builder\.unit|munit|m"
    for path in byval_files:
        text = path.read_text(encoding="utf-8")
        text = amp_helper_calls(text, first)
        text = rewrite_unit_calls(text, first_arg_is_already_ref=False)
        text = text.replace("(&&mut ", "(&mut ").replace("(&&", "(&")
        path.write_text(text, encoding="utf-8")
        print("updated", path.relative_to(ROOT))


if __name__ == "__main__":
    main()
