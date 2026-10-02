"""Materialize measured native C++ template members without restating their ABI.

The member pointer's exact type disambiguates overloads. Constructors and
destructors have no address, so an unoptimized placement construction/destruction
keeps the producer's own member definition. The consumer links that original
mangled symbol; these keeper functions are never consumer entry points.
"""
from __future__ import annotations

import re

PRIMITIVES = {
    "void": "void", "bool": "bool", "char": "char",
    "i8": "signed char", "u8": "unsigned char", "i16": "short", "u16": "unsigned short",
    "i32": "int", "u32": "unsigned int", "i64": "long long", "u64": "unsigned long long",
    "f32": "float", "f64": "double",
}


def cpp_type(info: dict) -> str:
    canonical = info.get("canonical_cpp_type")
    if canonical:
        if not re.fullmatch(r"[\w:.,<> *&]+", canonical):
            raise RuntimeError(f"unrepresentable canonical C++ type {canonical!r}")
        return canonical
    reference = info.get("reference", "value")
    pointee = info.get("pointee")
    if reference in {"pointer", "reference"}:
        value = cpp_type(pointee or dict(info, reference="value"))
        if info.get("pointee_const") and not (value.startswith("const ") or value.endswith(" const")):
            value = "const " + value
        return value + ("*" if reference == "pointer" else "&&" if info.get("reference_kind") == "rvalue" else "&")
    name = info.get("name", "")
    if name in PRIMITIVES:
        return PRIMITIVES[name]
    if not name or not re.fullmatch(r"[\w:.,<> *&]+", name):
        raise RuntimeError(f"unrepresentable native C++ type {name!r}")
    name = name.replace("::<", "<").replace(".", "::")
    return re.sub(r"\b(?:" + "|".join(PRIMITIVES) + r")\b", lambda m: PRIMITIVES[m.group()], name)


def member_source(symbol: dict, index: int) -> list[str]:
    request = symbol["cpp_materialization"]
    owner = request["owner"]  # Producer spelling, never consumer-respelled.
    member = request["member"]
    if request.get("kind") not in {"native_member", "list_constructor"} or symbol.get("visibility", "public") != "public":
        raise RuntimeError("native materialization requires an accessible measured member")
    params = [cpp_type(p["type"]) for p in symbol.get("params", [])]
    arguments = [f"std::forward<{ty}>(p{i})" for i, ty in enumerate(params)]
    signature = ", ".join(f"{ty} p{i}" for i, ty in enumerate(params))
    args = ", ".join(arguments)
    kind = symbol["kind"]
    lines = [f"namespace dci_native_{index} {{", f"using R = ::{owner};"]
    scope = owner.split("<", 1)[0].rpartition("::")[0]
    if scope:
        lines += [f"using namespace ::{scope};"]
    if request["kind"] == "list_constructor":
        comma = ", " if signature else ""
        lines += [f'extern "C" R* {symbol["link_name"]}(R* self{comma}{signature}) {{ return ::new (static_cast<void*>(self)) R{{{args}}}; }}']
    elif kind in {"constructor", "destructor"}:
        comma = ", " if signature else ""
        call = f"::new (static_cast<void*>(self)) R({args});" if kind == "constructor" else f"self->~{owner.split('<', 1)[0].split('::')[-1]}();"
        lines += [f'extern "C" __attribute__((optnone, noinline)) void dci_native_keep_{index}(R* self{comma}{signature}) {{ {call} }}']
    else:
        returned = cpp_type((symbol.get("return") or {"type": {"name": "void"}})["type"])
        qualifiers = request.get("qualifiers", "const" if symbol.get("is_const") else "")
        if qualifiers and not re.fullmatch(r"(?:const|volatile|&|&&|\s)+", qualifiers):
            raise RuntimeError(f"unsupported C++ member qualifiers {qualifiers!r}")
        suffix = " " + qualifiers if qualifiers else ""
        pointer = f"{returned} ({'' if symbol.get('is_static') else 'R::'}*)({', '.join(params)}){suffix}"
        lines += [f"using Member = {pointer};",
                  f"__attribute__((used)) static Member volatile keep = static_cast<Member>(&R::{member});"]
    return lines + ["}", ""]
