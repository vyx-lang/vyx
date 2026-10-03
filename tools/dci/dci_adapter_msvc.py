#!/usr/bin/env python3
"""Clang/GCC/MSVC C++ adapter for Vyx DCI v1.0.

This adapter is deliberately independent from the Vyx compiler. It validates
headers with the selected native frontend, asks Clang for structured target
facts, then writes a DCI ABI document for either the Microsoft or Itanium C++
ABI. The compiler is only a consumer of this normalized description; it must
not hardcode producer layout or mangling rules.

The filename is retained for compatibility with existing scripts.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile
from collections import deque
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterator, Sequence

try:
    from . import dcib
    from . import cpp_toolchains
except ImportError:
    _DCIB_SPEC = importlib.util.spec_from_file_location(
        "vyx_dci_dcib", Path(__file__).with_name("dcib.py")
    )
    if _DCIB_SPEC is None or _DCIB_SPEC.loader is None:
        raise RuntimeError("could not load the sibling DCIB codec")
    dcib = importlib.util.module_from_spec(_DCIB_SPEC)
    sys.modules[_DCIB_SPEC.name] = dcib
    _DCIB_SPEC.loader.exec_module(dcib)
    _TOOLCHAINS_SPEC = importlib.util.spec_from_file_location(
        "vyx_dci_cpp_toolchains", Path(__file__).with_name("cpp_toolchains.py")
    )
    if _TOOLCHAINS_SPEC is None or _TOOLCHAINS_SPEC.loader is None:
        raise RuntimeError("could not load the sibling C++ toolchain helpers")
    cpp_toolchains = importlib.util.module_from_spec(_TOOLCHAINS_SPEC)
    sys.modules[_TOOLCHAINS_SPEC.name] = cpp_toolchains
    _TOOLCHAINS_SPEC.loader.exec_module(cpp_toolchains)


RE_RECORD = re.compile(r"^\s*0 \| (class|struct) (.+?)(?: \(empty\))?\s*$")
RE_LAYOUT_ITEM = re.compile(r"^\s*(\d+) \|(?P<indent> +)(?P<payload>.+?)\s*$")
RE_BITFIELD_LAYOUT_ITEM = re.compile(
    r"^\s*(?P<byte_offset>\d+):(?P<bit_start>\d+)-(?P<bit_end>\d+)"
    r" \|(?P<indent> +)(?P<payload>.+?)\s*$"
)
# Microsoft layouts spell this as ``sizeof, align`` while Itanium layouts add
# ``dsize`` between those facts.  Both are compiler facts, not assumptions.
RE_SIZE = re.compile(
    r"^\s*\| \[sizeof=(?P<size>\d+),"
    r"(?:\s*dsize=\d+,)?\s*align=(?P<align>\d+),"
)
# Specifiers that may sit between the class-key and the record name: an
# export/visibility macro (``SPDLOG_API``, ``OA_EXPORT``), ``final``, or an
# explicit alignment specifier.  ``struct alignas(64) OA_EXPORT Over64 { ... };``
# is the standard spelling for over-aligned foreign objects; while the
# alignment alternatives were missing the record stayed invisible to
# discovery, so its layout was never measured and every by-value symbol that
# mentioned it was rejected as "no verified layout/lifecycle" -- a rejection of
# a fact the producer had already produced.  (The record only survived by
# accident when some *other* discovered record embedded it as a field and the
# layout closure dragged it in.)
RE_DECL_SPECIFIER = (
    r"(?:[A-Z_][A-Z0-9_]*"
    r"|final"
    r"|alignas\s*\([^;{}]*\)"
    r"|__declspec\s*\([^;{}]*\)"
    r"|__attribute__\s*\([^;{}]*\))"
)
RE_DECL_RECORD = re.compile(
    r"(?<!enum )\b(?:class|struct)\s+"
    r"(?:" + RE_DECL_SPECIFIER + r"\s+)*"
    r"([A-Za-z_][A-Za-z0-9_]*)\s*"
    r"(?:final\s*)?(?::|\{)"
)
RE_NAMESPACE = re.compile(r"^\s*namespace\s+([A-Za-z_][A-Za-z0-9_]*(?:::[A-Za-z_][A-Za-z0-9_]*)*)\s*\{")
# Opening line of a class/struct definition, tolerating a leading run of
# ALL-CAPS visibility/export macros (SPDLOG_API, FMT_API, FOO_EXPORT, ...) the
# way RE_DECL_RECORD does.  Without the macro-skip a body like
# ``class SPDLOG_API sink { ... };`` is not recognized as a class scope, so its
# members leak out of the scope trackers (e.g. a virtual ``log(...)`` gets
# mistaken for a free function and resolved against ``std::log``).
RE_CLASS_SCOPE_OPEN = re.compile(
    r"\s*(?:class|struct)\s+"
    r"(?:" + RE_DECL_SPECIFIER + r"\s+)*"
    r"[A-Za-z_][A-Za-z0-9_]*(?:\s+final)?(?:\s*:|\s*\{)"
)
RE_USING_ALIAS = re.compile(r"\busing\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+?)\s*;")
RE_OWNERSHIP_BLOCK = re.compile(
    r"/\*\s*dci-ownership\s*(\{.*?\})\s*dci-ownership-end\s*\*/", re.S
)
PARAMETER_OWNERSHIP_VALUES = {"borrow", "borrow_mut", "copy", "move", "share", "out"}
RETURN_OWNERSHIP_VALUES = {"borrow", "copy", "move", "share", "owned"}


CPP_TO_DCI_PRIMITIVE = {
    "void": ("void", "primitive", "value"),
    "bool": ("bool", "primitive", "value"),
    "_Bool": ("bool", "primitive", "value"),
    # In the C++ DCI profile `char` preserves C++'s distinct one-byte
    # character type. It is intentionally not folded into `i8`: signed char,
    # unsigned char, and plain char are distinct template/mangled identities.
    # The Active session spells this identity back through PRIMITIVE_SPELLINGS,
    # so changing it to i8 would request a different producer instantiation.
    "char": ("char", "primitive", "value"),
    "signed char": ("i8", "primitive", "value"),
    "unsigned char": ("u8", "primitive", "value"),
    "char8_t": ("u8", "primitive", "value"),
    "char16_t": ("u16", "primitive", "value"),
    "char32_t": ("u32", "primitive", "value"),
    "short": ("i16", "primitive", "value"),
    "short int": ("i16", "primitive", "value"),
    "signed short": ("i16", "primitive", "value"),
    "signed short int": ("i16", "primitive", "value"),
    "unsigned short": ("u16", "primitive", "value"),
    "unsigned short int": ("u16", "primitive", "value"),
    "int": ("i32", "primitive", "value"),
    "signed int": ("i32", "primitive", "value"),
    "unsigned": ("u32", "primitive", "value"),
    "unsigned int": ("u32", "primitive", "value"),
    "long long": ("i64", "primitive", "value"),
    "long long int": ("i64", "primitive", "value"),
    "signed long long": ("i64", "primitive", "value"),
    "signed long long int": ("i64", "primitive", "value"),
    "unsigned long long": ("u64", "primitive", "value"),
    "unsigned long long int": ("u64", "primitive", "value"),
    "int8_t": ("i8", "primitive", "value"),
    "uint8_t": ("u8", "primitive", "value"),
    "int16_t": ("i16", "primitive", "value"),
    "uint16_t": ("u16", "primitive", "value"),
    "int32_t": ("i32", "primitive", "value"),
    "uint32_t": ("u32", "primitive", "value"),
    "int64_t": ("i64", "primitive", "value"),
    "uint64_t": ("u64", "primitive", "value"),
    "std.int8_t": ("i8", "primitive", "value"),
    "std.uint8_t": ("u8", "primitive", "value"),
    "std.int16_t": ("i16", "primitive", "value"),
    "std.uint16_t": ("u16", "primitive", "value"),
    "std.int32_t": ("i32", "primitive", "value"),
    "std.uint32_t": ("u32", "primitive", "value"),
    "std.int64_t": ("i64", "primitive", "value"),
    "std.uint64_t": ("u64", "primitive", "value"),
    "float": ("f32", "primitive", "value"),
    "double": ("f64", "primitive", "value"),
}

CPP_SIGNED_LONG_TYPES = {
    "long",
    "long int",
    "signed long",
    "signed long int",
}
CPP_UNSIGNED_LONG_TYPES = {"unsigned long", "unsigned long int"}
CPP_UNSIGNED_SIZE_TYPES = {
    "size_t",
    "std.size_t",
    "uintptr_t",
    "std.uintptr_t",
}
CPP_SIGNED_SIZE_TYPES = {
    "ptrdiff_t",
    "std.ptrdiff_t",
    "intptr_t",
    "std.intptr_t",
}
# Kept as explicit Adapter state because type normalization is also used by
# standalone parser tests.  ``main`` always replaces it with the verified
# target data model before parsing a translation unit.
CPP_LONG_WIDTH = 32
CPP_POINTER_WIDTH = 64
MSVC_ENV_CACHE: dict[tuple[str, str], dict[str, str]] = {}

RE_FUNCTION_POINTER_TYPE = re.compile(
    r"^(?P<ret>.+?)\s*\(\s*(?:__[A-Za-z_][A-Za-z0-9_]*\s+)*"
    r"(?P<reference>[*&])(?:\s+const)?\s*\)\s*"
    r"\((?P<params>.*)\)\s*(?P<suffix>.*)$"
)
RE_MEMBER_POINTER_TYPE = re.compile(r"\([^)]*::\s*\*[^)]*\)")
RE_ARRAY_TYPE = re.compile(
    r"^(?P<base>.+?)\s*\[(?P<count>\d+)\]"
    r"(?P<remaining>(?:\s*\[\d+\])*)$"
)
RE_OBJECT_POINTER_TYPE = re.compile(
    r"^(?P<pointee>.+?)\s*\*\s*(?P<cv>(?:(?:const|volatile)\s*)*)$"
)
RE_REFERENCE_TYPE = re.compile(r"^(?P<pointee>.+?)\s*(?P<reference>&&|&)\s*$")

CPP_ENUM_UNDERLYING: dict[str, str] = {}
CPP_TYPE_ALIAS_TARGETS: dict[str, str] = {}


class AdapterContractError(RuntimeError):
    """Raised when an external entity cannot be described without guessing."""


@dataclass
class FieldLayout:
    name: str
    cpp_type: str
    offset: int
    visibility: str = "public"
    bit_offset: int | None = None
    bit_width: int | None = None
    storage_size: int | None = None
    is_signed: bool | None = None


@dataclass
class BaseLayout:
    type_name: str
    offset: int
    visibility: str = "public"
    is_virtual: bool = False
    adjustment: dict[str, Any] = field(default_factory=dict)


@dataclass
class RecordLayout:
    type_name: str
    size: int = -1
    alignment: int = -1
    fields: list[FieldLayout] = field(default_factory=list)
    bases: list[BaseLayout] = field(default_factory=list)
    has_vtable: bool = False
    vbptr_offsets: list[int] = field(default_factory=list)
    traits: dict[str, Any] = field(default_factory=dict)
    lifecycle: dict[str, Any] = field(default_factory=dict)


@dataclass
class TypeAlias:
    name: str
    target: str


@dataclass
class Symbol:
    name: str
    owner: str
    member_name: str
    mangled: str
    kind: str
    calling_convention: str
    params: list[dict[str, Any]]
    ret: dict[str, Any] | None
    is_static: bool = False
    is_virtual: bool = False
    is_override: bool = False
    is_final: bool = False
    is_const: bool = False
    visibility: str = "public"
    this_adjust: int = 0
    native_calling_convention: str = ""
    unwind: str = "unknown"
    parameter_ownerships: dict[int, str] = field(default_factory=dict)
    return_ownership: str = ""
    # Non-empty only for methods of instances the project handed to
    # --export-instance: the symbol describes the ABI the producer will
    # implement, but the definition itself is materialized at consumer build
    # time by the Active Adapter (same channel as the rust contract).
    semantic_id: str = ""
    native_template: bool = False
    list_arguments: list[str] | None = None
    member_qualifiers: str = ""


@dataclass
class VTableEntry:
    index: int
    offset: int
    name: str
    kind: str
    mangled: str = ""
    owner: str = ""
    member_name: str = ""
    this_adjust: int = 0
    return_adjust: int = 0


@dataclass
class VTable:
    class_name: str
    base_class: str
    entries: list[VTableEntry] = field(default_factory=list)


def vtable_address_point_offset(vt: VTable) -> int:
    for entry in vt.entries:
        if entry.kind not in {"rtti", "metadata"}:
            return entry.offset
    return 0


def run(cmd: list[str]) -> str:
    try:
        proc = subprocess.run(
            cmd,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            errors="replace",
        )
    except OSError as error:
        raise AdapterContractError(
            f"cannot execute C++ fact extractor {cmd[0]!r}: {error}"
        ) from error
    if proc.returncode != 0:
        detail = (proc.stdout + "\n" + proc.stderr).strip()
        raise AdapterContractError(
            f"C++ fact extraction command failed with exit code {proc.returncode}"
            + (f":\n{detail}" if detail else "")
        )
    return proc.stdout + "\n" + proc.stderr


def try_run(cmd: list[str]) -> str | None:
    try:
        proc = subprocess.run(
            cmd,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            errors="replace",
        )
    except OSError:
        return None
    if proc.returncode != 0:
        return None
    return proc.stdout + "\n" + proc.stderr


def normalize_record_name(raw: str) -> str:
    text = raw.strip()
    for prefix in ("class ", "struct ", "enum "):
        if text.startswith(prefix):
            text = text[len(prefix) :].strip()
    return text


def canonical_cpp_name(raw: str) -> str:
    text = collapse_spaces(normalize_record_name(raw))
    text = re.sub(r"\b(?:class|struct|enum)\s+", "", text)
    text = text.replace(" <", "<").replace("< ", "<")
    text = text.replace(" >", ">").replace("> ", ">")
    text = text.replace(" ,", ",").replace(", ", ",")
    text = text.replace(" &", "&").replace("& ", "&")
    text = text.replace(" *", "*").replace("* ", "*")
    return text


def collapse_spaces(text: str) -> str:
    return " ".join(text.strip().split())


def _split_template_args(text: str) -> list[str]:
    """Top-level comma split of a canonical ``Name<a,b>`` argument list."""
    if "<" not in text or not text.rstrip().endswith(">"):
        return []
    inner = text[text.index("<") + 1 : text.rindex(">")]
    parts: list[str] = []
    depth = 0
    current: list[str] = []
    for ch in inner:
        if ch == "<":
            depth += 1
        elif ch == ">":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append("".join(current).strip())
            current = []
        else:
            current.append(ch)
    parts.append("".join(current).strip())
    return [part for part in parts if part]


def _cpp_arg_to_dci(arg: str) -> str:
    """DCI-namespace spelling of one template argument (semantic_id args)."""
    canonical = canonical_cpp_name(arg)
    primitive = CPP_TO_DCI_PRIMITIVE.get(canonical)
    if primitive is not None:
        return primitive[0]
    return canonical


def normalize_cpp_type(raw: str) -> str:
    text = collapse_spaces(raw)
    changed = True
    while changed:
        changed = False
        for prefix in ("const ", "volatile ", "class ", "struct ", "enum "):
            if text.startswith(prefix):
                text = text[len(prefix) :].strip()
                changed = True
    while text.endswith(" const"):
        text = text[:-6].strip()
    while text.endswith(" volatile"):
        text = text[:-9].strip()
    text = text.replace(" *", "*").replace("* ", "*")
    text = text.replace(" &", "&").replace("& ", "&")
    text = text.replace("::", ".")
    # ``::size_t`` / ``::std::size_t`` become ``.size_t`` / ``.std.size_t``.
    while text.startswith("."):
        text = text[1:]
    return text


def cpp_spelling_is_known_primitive(raw: str) -> bool:
    """True when ``raw`` is already a mapped integer/float spelling.

    Used to keep ``size_t`` / ``std::size_t`` from being shadowed by a
    compiler desugaring onto ``unsigned long long``.  Unknown typedefs may
    still alias onto a primitive.
    """
    name = normalize_cpp_type(raw)
    return (
        name in CPP_TO_DCI_PRIMITIVE
        or name in CPP_SIGNED_LONG_TYPES
        or name in CPP_UNSIGNED_LONG_TYPES
        or name in CPP_UNSIGNED_SIZE_TYPES
        or name in CPP_SIGNED_SIZE_TYPES
    )


def resolve_cpp_type_alias(clean: str, owner: str) -> str:
    canonical = normalize_cpp_type(clean)
    # Class-local aliases belong to the exact specialization. `iterator`
    # from vector<int> must never describe vector<double>::iterator.
    if owner:
        scoped_owner = CPP_TYPE_ALIAS_TARGETS.get(normalize_cpp_type(owner) + "." + canonical)
        if scoped_owner:
            return scoped_owner
    for namespace in enclosing_namespaces(owner):
        scoped = CPP_TYPE_ALIAS_TARGETS.get(namespace.replace("::", ".") + "." + canonical)
        if scoped:
            return scoped
    return CPP_TYPE_ALIAS_TARGETS.get(canonical, "")


def dci_type(raw: str, owner: str = "", _alias_stack: tuple[str, ...] = ()) -> dict[str, Any]:
    value = _dci_type(raw, owner, _alias_stack)
    if value.get("kind") == "primitive" and value.get("reference") == "value":
        value.setdefault("canonical_cpp_type", raw.strip())
    return value


def _dci_type(raw: str, owner: str = "", _alias_stack: tuple[str, ...] = ()) -> dict[str, Any]:
    raw_text = collapse_spaces(raw)
    text = raw_text
    if RE_MEMBER_POINTER_TYPE.search(text):
        return {
            "name": canonical_cpp_name(text).replace("::", "."),
            "kind": "opaque",
            "nullable": False,
            "reference": "value",
            "cpp_type": raw,
            "unsupported_reason": (
                "C++ member pointers have ABI- and inheritance-dependent representation"
            ),
        }
    function_pointer = RE_FUNCTION_POINTER_TYPE.match(text)
    if function_pointer:
        ret_text = function_pointer.group("ret").strip()
        params_text = function_pointer.group("params").strip()
        _unused_ret, params, _unused_const = split_function_type(
            f"void ({params_text})"
        )
        return {
            "name": "fn",
            "kind": "function",
            "nullable": function_pointer.group("reference") == "*",
            "reference": (
                "pointer"
                if function_pointer.group("reference") == "*"
                else "reference"
            ),
            "cpp_type": raw,
            "calling_convention": declared_calling_convention(text) or "cdecl",
            "signature": {
                "params": [dci_type_contract(dci_type(param, owner)) for param in params],
                "return": dci_type_contract(dci_type(ret_text, owner)),
            },
        }
    array_type = RE_ARRAY_TYPE.match(text)
    if array_type:
        element = dci_type(
            array_type.group("base") + array_type.group("remaining"), owner
        )
        count = int(array_type.group("count"))
        return {
            "name": f"[{element['name']};{count}]",
            "kind": "vector",
            "nullable": False,
            "reference": "value",
            "cpp_type": raw,
            "element": dci_type_contract(element),
            "length": count,
        }
    pointer_type = RE_OBJECT_POINTER_TYPE.match(text)
    if pointer_type:
        pointee_text = pointer_type.group("pointee").strip()
        inner = dci_type(pointee_text, owner)
        pointer_cv = pointer_type.group("cv") or ""
        return {
            "name": inner["name"],
            "kind": inner["kind"],
            "nullable": True,
            "reference": "pointer",
            "pointee_const": cpp_top_level_const(pointee_text),
            "pointer_const": re.search(r"\bconst\b", pointer_cv) is not None,
            "pointee": dci_type_contract(inner),
            "cpp_type": raw,
        }
    reference_type = RE_REFERENCE_TYPE.match(text)
    if reference_type:
        pointee_text = reference_type.group("pointee").strip()
        inner = dci_type(pointee_text, owner)
        is_rvalue = reference_type.group("reference") == "&&"
        return {
            "name": inner["name"],
            "kind": inner["kind"],
            "nullable": False,
            "reference": "reference",
            "reference_kind": "rvalue" if is_rvalue else "lvalue",
            "pointee_const": cpp_top_level_const(pointee_text),
            "pointee": dci_type_contract(inner),
            "cpp_type": raw,
        }
    clean = normalize_cpp_type(text)
    alias_target = resolve_cpp_type_alias(clean, owner)
    if (
        alias_target
        and normalize_cpp_type(alias_target) != clean
        and clean not in _alias_stack
    ):
        resolved = dci_type(alias_target, owner, _alias_stack + (clean,))
        resolved["canonical_cpp_type"] = resolved.get("canonical_cpp_type") or resolved.get("cpp_type") or alias_target
        resolved["cpp_type"] = raw
        resolved["alias_name"] = clean
        return resolved
    if clean in CPP_SIGNED_LONG_TYPES:
        name = "i64" if CPP_LONG_WIDTH == 64 else "i32"
        return {
            "name": name,
            "kind": "primitive",
            "nullable": False,
            "reference": "value",
            "cpp_type": raw,
        }
    if clean in CPP_UNSIGNED_LONG_TYPES:
        name = "u64" if CPP_LONG_WIDTH == 64 else "u32"
        return {
            "name": name,
            "kind": "primitive",
            "nullable": False,
            "reference": "value",
            "cpp_type": raw,
        }
    if clean in CPP_UNSIGNED_SIZE_TYPES:
        name = "u64" if CPP_POINTER_WIDTH == 64 else "u32"
        return {
            "name": name,
            "kind": "primitive",
            "nullable": False,
            "reference": "value",
            "cpp_type": raw,
        }
    if clean in CPP_SIGNED_SIZE_TYPES:
        name = "i64" if CPP_POINTER_WIDTH == 64 else "i32"
        return {
            "name": name,
            "kind": "primitive",
            "nullable": False,
            "reference": "value",
            "cpp_type": raw,
        }
    enum_name = resolve_dci_enum_type_name(clean, owner)
    enum_underlying = None
    if enum_name:
        enum_underlying = CPP_ENUM_UNDERLYING.get(
            canonical_cpp_name(enum_name.replace(".", "::"))
        ) or CPP_ENUM_UNDERLYING.get(normalize_cpp_type(enum_name))
    if enum_underlying:
        prim = dci_type(enum_underlying, owner)
        prim["cpp_type"] = raw
        prim["enum_name"] = enum_name
        return prim
    if clean in CPP_TO_DCI_PRIMITIVE:
        name, kind, reference = CPP_TO_DCI_PRIMITIVE[clean]
        return {"name": name, "kind": kind, "nullable": False, "reference": reference, "cpp_type": raw}
    return {"name": clean, "kind": "class", "nullable": False, "reference": "value", "cpp_type": raw}


def parse_field_payload(payload: str) -> tuple[str, str] | None:
    text = payload.strip()
    if not text or text.startswith("("):
        return None
    if "(primary base)" in text or "(base)" in text or "(virtual base)" in text:
        return None
    if " vftable pointer" in text:
        return None
    parts = text.rsplit(" ", 1)
    if len(parts) != 2:
        return None
    type_text, name = parts[0].strip(), parts[1].strip()
    if not name or name.startswith("("):
        return None
    return type_text, name


def enum_underlying_for_type(type_name: str, owner: str = "") -> str:
    enum_name = resolve_dci_enum_type_name(type_name, owner)
    if not enum_name:
        return ""
    return CPP_ENUM_UNDERLYING.get(
        canonical_cpp_name(enum_name.replace(".", "::")), ""
    ) or CPP_ENUM_UNDERLYING.get(normalize_cpp_type(enum_name), "")


def bitfield_storage_size(cpp_type: str) -> int | None:
    normalized = normalize_cpp_type(cpp_type)
    type_info = dci_type(normalized)
    dci_name = type_info.get("name") if type_info.get("kind") == "primitive" else ""
    match = re.fullmatch(r"[iu](8|16|32|64)", dci_name)
    if match:
        return int(match.group(1)) // 8
    if dci_name == "bool":
        return 1
    enum_underlying = enum_underlying_for_type(normalized)
    if enum_underlying:
        return bitfield_storage_size(enum_underlying)
    return None


def bitfield_is_signed(cpp_type: str) -> bool | None:
    normalized = normalize_cpp_type(cpp_type)
    type_info = dci_type(normalized)
    dci_name = type_info.get("name") if type_info.get("kind") == "primitive" else ""
    if dci_name.startswith("i"):
        return True
    if dci_name.startswith("u") or dci_name == "bool":
        return False
    enum_underlying = enum_underlying_for_type(normalized)
    if enum_underlying:
        return bitfield_is_signed(enum_underlying)
    return None


def parse_base_payload(payload: str) -> tuple[str, bool] | None:
    text = payload.strip()
    if "(base)" not in text and "(primary base)" not in text and "(virtual base)" not in text:
        return None
    is_virtual = "(virtual base)" in text
    text = text.replace("(primary base)", "").replace("(virtual base)", "").replace("(base)", "").strip()
    return (normalize_record_name(text), is_virtual) if text else None


RE_PROJECT_INCLUDE = re.compile(
    r'^\s*#\s*include\s*[<"]([^">]+)[">]', re.MULTILINE
)
RE_NAMESPACE_SPLIT_BRACE = re.compile(
    r"^(\s*namespace\s+[A-Za-z_][A-Za-z0-9_:]*\s*)\r?\n(\s*\{)", re.MULTILINE
)
RE_ENUM_SPLIT_BRACE = re.compile(
    r"^(\s*enum\s+(?:(?:class|struct)\s+)?[A-Za-z_][A-Za-z0-9_]*(?:\s*:\s*[^\r\n{]+)?\s*)\r?\n(\s*\{)",
    re.MULTILINE,
)
# Qt / many industrial headers put the class name on one line and `{` on the
# next (`class Q_CORE_EXPORT QString\n{`). Forward declarations (`class QPoint;`)
# must not match.
RE_RECORD_SPLIT_BRACE = re.compile(
    r"^("
    r"\s*(?:class|struct)\s+"
    r"(?:(?:[A-Z_][A-Z0-9_]*|final)\s+)*"
    r"[A-Za-z_][A-Za-z0-9_]*"
    r"(?:\s+final)?"
    r"(?:\s*:[^\r\n{]+)?"
    r"\s*)"
    r"(?:"
    r"\r?\n[ \t]*#(?:if|ifdef|ifndef|else|elif|endif|define)\b[^\r\n]*"
    r"|"
    r"\r?\n[ \t]*:[^\r\n{]+"
    r")*"
    r"\r?\n(\s*\{)",
    re.MULTILINE,
)


def project_source_lines(path: Path) -> list[str]:
    """Read project declarations while normalizing split `namespace`/`enum`/`class` braces."""
    text = path.read_text(encoding="utf-8", errors="ignore")
    text = RE_NAMESPACE_SPLIT_BRACE.sub(r"\1{", text)
    text = RE_ENUM_SPLIT_BRACE.sub(r"\1{", text)
    text = RE_RECORD_SPLIT_BRACE.sub(r"\1{", text)
    return text.splitlines()


def _strip_line_comments(text: str, in_block: bool) -> tuple[str, bool]:
    out: list[str] = []
    i = 0
    n = len(text)
    while i < n:
        if in_block:
            end = text.find("*/", i)
            if end < 0:
                return "".join(out), True
            in_block = False
            i = end + 2
            continue
        if text.startswith("//", i):
            break
        if text.startswith("/*", i):
            in_block = True
            i += 2
            continue
        out.append(text[i])
        i += 1
    return "".join(out), in_block


def iter_discovery_code_lines(path: Path):
    """Yield header lines with comments/`#define` bodies removed.

    Regex discovery is not a preprocessor, but it must not treat documentation
    samples, `Q_OBJECT` macro guts, or `#endif` separators as declarations.
    """
    in_block = False
    in_macro = False
    for raw_line in project_source_lines(path):
        code, in_block = _strip_line_comments(raw_line, in_block)
        stripped = code.lstrip()
        continued = code.rstrip().endswith("\\")
        if in_macro or stripped.startswith("#"):
            in_macro = continued
            continue
        in_macro = False
        yield code


def _resolve_project_include(
    include_name: str, source_dir: Path, include_dirs: list[Path]
) -> Path | None:
    """Resolve a quoted/project include without treating SDK headers as API roots."""
    candidates = [source_dir / include_name]
    candidates.extend(directory / include_name for directory in include_dirs)
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    return None


def discover_project_headers(
    roots: list[str], project_roots: list[str] | None = None
) -> list[str]:
    """Return the recursive public-header closure rooted at ``roots``.

    Clang remains the authority for parsing the translation unit.  This pass
    only finds project-local headers so the adapter also discovers declarations
    that are reachable through umbrella headers such as ``Cacao.hpp``.
    System and third-party headers outside the declared project roots are left
    to Clang and are not accidentally exported as producer API.
    """
    root_paths = [Path(path).resolve() for path in roots if Path(path).is_file()]
    allowed_roots = [Path(path).resolve() for path in (project_roots or [])]
    if not allowed_roots:
        allowed_roots = [path.parent for path in root_paths]
    include_dirs = list(allowed_roots)
    pending = list(root_paths)
    seen: set[Path] = set()
    result: list[str] = []

    def is_project_path(path: Path) -> bool:
        return any(path.is_relative_to(root) for root in allowed_roots)

    while pending:
        header = pending.pop()
        if header in seen or not header.is_file() or not is_project_path(header):
            continue
        seen.add(header)
        result.append(str(header))
        text = header.read_text(encoding="utf-8", errors="ignore")
        for match in RE_PROJECT_INCLUDE.finditer(text):
            included = _resolve_project_include(match.group(1), header.parent, include_dirs)
            if included is not None and included not in seen and is_project_path(included):
                pending.append(included)
    return result


def public_api_headers(headers: Sequence[str]) -> list[str]:
    """Drop producer implementation headers from the contract source list.

    ``discover_project_headers`` follows quoted includes even inside
    ``#ifdef SPDLOG_HEADER_ONLY``, so ``logger-inl.h`` lands in the closure.
    Stub TUs that ``#include`` those files emit the library's out-of-line
    bodies and then fail to link against ``libspdlog.a``.
    """
    result: list[str] = []
    for header in headers:
        name = Path(header).name.lower()
        if name.endswith("-inl.h") or name.endswith("-inl.hpp") or name.endswith("-inl.hh"):
            continue
        result.append(str(header))
    return result


def discover_public_header_roots(project_roots: list[str]) -> list[str]:
    """Return explicit public-header roots for a requested full API scan."""
    headers: list[str] = []
    for root_text in project_roots:
        root = Path(root_text)
        if not root.is_dir():
            continue
        for pattern in ("*.h", "*.hpp"):
            headers.extend(str(path.resolve()) for path in root.rglob(pattern))
    return sorted(set(headers))


def discover_record_names(headers: list[str]) -> list[str]:
    out: list[str] = []
    seen: set[str] = set()
    for header in headers:
        p = Path(header)
        if not p.exists():
            continue
        depth = 0
        namespaces: list[tuple[str, int]] = []
        class_scopes: list[int] = []
        pending_template_decl = False
        for raw_line in iter_discovery_code_lines(p):
            # Comments are not declarations.  A line like
            # ``// Memory-class aggregate:`` would otherwise match
            # ``class aggregate:`` and invent a fake nested type.
            while namespaces and depth < namespaces[-1][1]:
                namespaces.pop()
            while class_scopes and depth < class_scopes[-1]:
                class_scopes.pop()
            if re.match(r"^\s*template\s*<", raw_line):
                # Namespace/file-scope templates bind the next class/struct.
                # Templates nested in a class (Qt friend get<>, QString helpers)
                # must not mark the next top-level class as a template.
                if not class_scopes:
                    pending_template_decl = True
                depth += raw_line.count("{") - raw_line.count("}")
                if depth < 0:
                    depth = 0
                continue
            if pending_template_decl and raw_line.strip() and not RE_DECL_RECORD.search(raw_line):
                pending_template_decl = False
            ns_m = RE_NAMESPACE.match(raw_line)
            if ns_m:
                ns_depth = depth + raw_line.count("{") - raw_line.count("}")
                if ns_depth > depth:
                    namespaces.append((ns_m.group(1), ns_depth))
            prefix = "::".join(ns for ns, _ in namespaces)
            for m in RE_DECL_RECORD.finditer(raw_line):
                if class_scopes:
                    continue
                if pending_template_decl:
                    pending_template_decl = False
                    continue
                name = m.group(1)
                if prefix:
                    name = prefix + "::" + name
                if name not in seen:
                    seen.add(name)
                    out.append(name)
            if RE_DECL_RECORD.search(raw_line) and not class_scopes:
                pending_template_decl = False
            if RE_CLASS_SCOPE_OPEN.match(raw_line):
                class_depth = depth + raw_line.count("{") - raw_line.count("}")
                if class_depth > depth:
                    class_scopes.append(class_depth)
            depth += raw_line.count("{") - raw_line.count("}")
            if depth < 0:
                depth = 0
            while namespaces and depth < namespaces[-1][1]:
                namespaces.pop()
            while class_scopes and depth < class_scopes[-1]:
                class_scopes.pop()
    return out


RE_EXPLICIT_INSTANTIATION = re.compile(
    r"^\s*template\s+(?:struct|class)\s+"
    r"([A-Za-z_][A-Za-z0-9_]*(?:::[A-Za-z_][A-Za-z0-9_]*)*\s*<[^;{]*>)\s*;"
)


def discover_explicit_instantiation_names(headers: list[str]) -> list[str]:
    """Concrete instances the header *itself* instantiates.

    ``template struct Pair2<double, int>;`` is the provider stating that this
    specialization exists in its artifact.  That makes it a concrete entity
    with a stable layout and final symbols -- exactly what DCI is allowed to
    describe -- whereas the bare ``template <typename A, typename B> struct
    Pair2`` has no ABI of its own and is therefore skipped by
    ``discover_record_names``.

    Only *definitions* count.  ``extern template struct X<int>;`` is an
    explicit instantiation **declaration**: it suppresses instantiation in this
    translation unit, so nothing guarantees the symbol or the layout exists.

    Names are returned canonicalized (``Pair2<double,int>``) so they compare
    against the AST's ``name + <args>`` spelling the way ``resolve_record_owner``
    expects.  They feed the *symbol* scan only; instance layouts keep coming
    from the project's declaration file, so this cannot silently change what
    the layout closure exports.
    """
    out: list[str] = []
    seen: set[str] = set()
    for header in headers:
        path = Path(header)
        if not path.exists():
            continue
        for raw_line in iter_discovery_code_lines(path):
            if raw_line.lstrip().startswith("extern"):
                continue
            match = RE_EXPLICIT_INSTANTIATION.match(raw_line)
            if not match:
                continue
            name = canonical_cpp_name(match.group(1))
            if name and name not in seen:
                seen.add(name)
                out.append(name)
    return sorted(out)


def discover_ownership_annotations(headers: list[str]) -> dict[str, dict[str, Any]]:
    rules: dict[str, dict[str, Any]] = {}
    for header in headers:
        path = Path(header)
        if not path.exists():
            continue
        text = path.read_text(encoding="utf-8", errors="ignore")
        for match in RE_OWNERSHIP_BLOCK.finditer(text):
            try:
                block = json.loads(match.group(1))
            except json.JSONDecodeError as error:
                raise AdapterContractError(
                    f"{path}: invalid dci-ownership JSON at line "
                    f"{text.count(chr(10), 0, match.start()) + 1}: {error.msg}"
                ) from error
            if not isinstance(block, dict):
                raise AdapterContractError(f"{path}: dci-ownership block must be an object")
            for selector, rule in block.items():
                if not isinstance(selector, str) or not selector or not isinstance(rule, dict):
                    raise AdapterContractError(
                        f"{path}: each dci-ownership entry must map a signature to an object"
                    )
                if selector in rules and rules[selector] != rule:
                    raise AdapterContractError(
                        f"{path}: conflicting dci-ownership rules for {selector!r}"
                    )
                rules[selector] = rule
    return rules


def symbol_selector(symbol: Symbol) -> str:
    params = ",".join(
        canonical_cpp_name(str(parameter.get("type", {}).get("cpp_type") or ""))
        for parameter in symbol.params
    )
    suffix = " const" if symbol.is_const else ""
    return f"{symbol.name}({params}){suffix}"


def symbol_identity(symbol: Symbol) -> tuple[Any, ...]:
    parameter_types = tuple(
        canonical_cpp_name(str(parameter.get("type", {}).get("cpp_type") or ""))
        for parameter in symbol.params
    )
    return (
        symbol.kind,
        symbol.owner,
        symbol.member_name,
        parameter_types,
        symbol.is_const,
        symbol.is_static,
        symbol.mangled,
    )


def apply_ownership_annotations(
    symbols: list[Symbol], rules: dict[str, dict[str, Any]]
) -> None:
    symbols_by_selector: dict[str, list[Symbol]] = {}
    for symbol in symbols:
        symbols_by_selector.setdefault(symbol_selector(symbol), []).append(symbol)

    for selector, rule in rules.items():
        candidates = symbols_by_selector.get(selector, [])
        if len(candidates) != 1:
            raise AdapterContractError(
                f"dci-ownership selector {selector!r} resolved to {len(candidates)} symbols"
            )
        symbol = candidates[0]
        parameters = rule.get("parameters", {})
        if not isinstance(parameters, dict):
            raise AdapterContractError(
                f"dci-ownership {selector!r}.parameters must be an object"
            )
        for raw_index, ownership in parameters.items():
            try:
                index = int(raw_index)
            except (TypeError, ValueError) as error:
                raise AdapterContractError(
                    f"dci-ownership {selector!r} has non-numeric parameter index {raw_index!r}"
                ) from error
            if index < 0 or index >= len(symbol.params):
                raise AdapterContractError(
                    f"dci-ownership {selector!r} parameter index {index} is out of range"
                )
            if ownership not in PARAMETER_OWNERSHIP_VALUES:
                raise AdapterContractError(
                    f"dci-ownership {selector!r} parameter #{index} has invalid ownership {ownership!r}"
                )
            symbol.parameter_ownerships[index] = ownership
        returned = rule.get("return")
        if returned is not None:
            if returned not in RETURN_OWNERSHIP_VALUES:
                raise AdapterContractError(
                    f"dci-ownership {selector!r} has invalid return ownership {returned!r}"
                )
            symbol.return_ownership = returned


def split_top_level_template_args(text: str) -> list[str]:
    args: list[str] = []
    depth = 0
    start = 0
    for i, ch in enumerate(text):
        if ch == "<":
            depth += 1
        elif ch == ">" and depth > 0:
            depth -= 1
        elif ch == "," and depth == 0:
            args.append(text[start:i].strip())
            start = i + 1
    tail = text[start:].strip()
    if tail:
        args.append(tail)
    return args


def qualify_alias_target(target: str, namespace_prefix: str) -> str:
    text = collapse_spaces(target.strip())
    if text.startswith("::"):
        return text[2:].strip()
    lt = text.find("<")
    if lt >= 0 and text.endswith(">"):
        head = text[:lt].strip()
        inner = text[lt + 1 : -1].strip()
        qhead = qualify_alias_target(head, namespace_prefix)
        qargs = [qualify_alias_target(arg, namespace_prefix) for arg in split_top_level_template_args(inner)]
        return qhead + "<" + ", ".join(qargs) + ">"
    if not namespace_prefix:
        return text
    head = text
    for sep in ("<", "*", "&", " "):
        idx = head.find(sep)
        if idx >= 0:
            head = head[:idx]
    if "::" in head:
        return text
    if head.startswith("std::"):
        return text
    if (
        head in CPP_TO_DCI_PRIMITIVE
        or head in CPP_SIGNED_LONG_TYPES
        or head in CPP_UNSIGNED_LONG_TYPES
        or head in {"void", "bool", "char", "float", "double"}
    ):
        return text
    return namespace_prefix + "::" + text


def discover_type_aliases(headers: list[str]) -> list[TypeAlias]:
    out: list[TypeAlias] = []
    seen: set[str] = set()
    for header in headers:
        p = Path(header)
        if not p.exists():
            continue
        depth = 0
        namespaces: list[tuple[str, int]] = []
        class_scopes: list[int] = []
        pending_template_decl = False
        for raw_line in project_source_lines(p):
            while namespaces and depth < namespaces[-1][1]:
                namespaces.pop()
            while class_scopes and depth < class_scopes[-1]:
                class_scopes.pop()
            ns_m = RE_NAMESPACE.match(raw_line)
            if ns_m:
                ns_depth = depth + raw_line.count("{") - raw_line.count("}")
                if ns_depth > depth:
                    namespaces.append((ns_m.group(1), ns_depth))
            if RE_CLASS_SCOPE_OPEN.match(raw_line):
                class_depth = depth + raw_line.count("{") - raw_line.count("}")
                if class_depth > depth:
                    class_scopes.append(class_depth)
            if re.match(r"^\s*template\s*<", raw_line):
                pending_template_decl = True
            if not class_scopes:
                m = RE_USING_ALIAS.search(raw_line)
                if m:
                    if pending_template_decl:
                        pending_template_decl = False
                        continue
                    prefix = "::".join(ns for ns, _ in namespaces)
                    alias = m.group(1)
                    if prefix:
                        alias = prefix + "::" + alias
                    target = qualify_alias_target(m.group(2), prefix)
                    if alias not in seen:
                        seen.add(alias)
                        out.append(TypeAlias(alias, target))
            if raw_line.strip() and not raw_line.lstrip().startswith("template"):
                pending_template_decl = False
            depth += raw_line.count("{") - raw_line.count("}")
            if depth < 0:
                depth = 0
            while namespaces and depth < namespaces[-1][1]:
                namespaces.pop()
            while class_scopes and depth < class_scopes[-1]:
                class_scopes.pop()
    alias_by_name = {a.name: a for a in out}
    alias_by_short = {a.name.split("::")[-1]: a for a in out}
    for alias in out:
        target = alias.target
        seen_chain: set[str] = set()
        while target not in seen_chain:
            seen_chain.add(target)
            next_alias = alias_by_name.get(target) or alias_by_short.get(target.split("::")[-1])
            if next_alias is None:
                break
            target = next_alias.target
        alias.target = target
    return out


def unique_names(names: list[str]) -> list[str]:
    out: list[str] = []
    seen: set[str] = set()
    for name in names:
        if name and name not in seen:
            seen.add(name)
            out.append(name)
    return out


def last_cpp_component(name: str) -> str:
    text = name.replace(".", "::")
    depth = 0
    start = 0
    i = 0
    while i < len(text):
        ch = text[i]
        if ch == "<":
            depth += 1
        elif ch == ">" and depth > 0:
            depth -= 1
        elif ch == ":" and i + 1 < len(text) and text[i + 1] == ":" and depth == 0:
            start = i + 2
            i += 1
        i += 1
    return text[start:]


def last_cpp_component_without_template_args(name: str) -> str:
    last = last_cpp_component(name)
    idx = last.find("<")
    if idx >= 0:
        return last[:idx]
    return last


def destructor_member_name_for_owner(owner: str) -> str:
    return "~" + last_cpp_component_without_template_args(owner)


def template_arg_suffix(node: dict[str, Any]) -> str:
    args: list[str] = []
    for child in node.get("inner", []) or []:
        if not isinstance(child, dict) or child.get("kind") != "TemplateArgument":
            continue
        ty = (child.get("type") or {}).get("qualType") or ""
        if ty:
            args.append(collapse_spaces(ty))
    if not args:
        return ""
    return "<" + ", ".join(args) + ">"


def resolve_record_owner(ast_name: str, record_names: set[str]) -> str:
    for name in record_names:
        if names_match(ast_name, name):
            return name
    matches = [name for name in record_names if last_cpp_component(name) == ast_name]
    if len(matches) == 1:
        return matches[0]
    template_matches = [name for name in record_names if last_cpp_component_without_template_args(name) == ast_name]
    if len(template_matches) == 1:
        return template_matches[0]
    return ""


def resolve_record_owner_from_node(node: dict[str, Any], record_names: set[str]) -> str:
    if node.get("dci_verified_owner") in record_names:
        return node["dci_verified_owner"]
    if node.get("dci_instance_unmapped"):
        return ""
    ast_name = node.get("name") or ""
    if not ast_name:
        return ""
    specialized = ast_name + template_arg_suffix(node)
    if specialized != ast_name:
        exact = [name for name in record_names if names_match(last_cpp_component(name), specialized)]
        if len(exact) == 1:
            return exact[0]
        # The AST dump prints template arguments *desugared*
        # (``SinkG<int32_t>`` dumps as ``SinkG<int>``), while the requested
        # instance names carry the header's spelling.  Match through the
        # desugared form; identical specializations are deduplicated by the
        # compiler itself, so at most one spelling can correspond to a node.
        desugared_exact = [
            name
            for name in record_names
            if "<" in name
            and names_match(last_cpp_component(desugar_dump_spelling(name)), specialized)
        ]
        if len(desugared_exact) == 1:
            return desugared_exact[0]
    return resolve_record_owner(ast_name, record_names)


def canonical_scoped_record_name(name: str) -> str:
    return canonical_cpp_name(name.replace(".", "::")).lstrip(":")


def last_top_level_cpp_scope_separator(name: str) -> int:
    depth = 0
    result = -1
    index = 0
    while index + 1 < len(name):
        ch = name[index]
        if ch == "<":
            depth += 1
        elif ch == ">" and depth > 0:
            depth -= 1
        elif ch == ":" and name[index + 1] == ":" and depth == 0:
            result = index
            index += 1
        index += 1
    return result


def cpp_namespace_for_record(name: str) -> str:
    text = canonical_scoped_record_name(name)
    separator = last_top_level_cpp_scope_separator(text)
    if separator < 0:
        return ""
    return text[:separator]


def scoped_name_has_root_namespace(name: str) -> bool:
    base = name.split("<", 1)[0]
    return "::" in base


def enclosing_namespaces(owner: str) -> list[str]:
    """C++ enclosing namespace scopes of ``owner``, most specific first.

    For ``spdlog::sinks::sink`` this yields ``["spdlog::sinks", "spdlog"]`` so a
    relative qualified name written inside that scope (``details::log_msg``) can
    be resolved the way the C++ compiler does: try each enclosing scope in turn.
    """
    namespace = cpp_namespace_for_record(owner)
    out: list[str] = []
    while namespace:
        out.append(namespace)
        separator = last_top_level_cpp_scope_separator(namespace)
        if separator < 0:
            break
        namespace = namespace[:separator]
    return out


def resolve_dci_record_type_name(
    type_name: str, owner: str, record_names: set[str]
) -> str:
    """Resolve an AST-spelled record type without guessing across namespaces."""
    scoped_name = canonical_scoped_record_name(type_name)
    if not scoped_name:
        return type_name

    # A template argument can be qualified even when the record name itself
    # is not.  Only the root record name decides whether it is already scoped.
    if scoped_name_has_root_namespace(scoped_name):
        exact = [
            record
            for record in record_names
            if canonical_scoped_record_name(record) == scoped_name
        ]
        if len(exact) == 1:
            return canonical_scoped_record_name(exact[0]).replace("::", ".")
        # A partially-qualified name (``details::log_msg`` written inside
        # namespace ``spdlog``) has ``::`` yet is *relative* to the owner's
        # scope, so the global exact match fails.  Resolve it via C++
        # enclosing-scope lookup before giving up: only accept a namespace
        # prefix that yields exactly one record so we never guess.
        for prefix in enclosing_namespaces(owner):
            candidate = prefix + "::" + scoped_name
            matches = [
                record
                for record in record_names
                if canonical_scoped_record_name(record) == candidate
            ]
            if len(matches) == 1:
                return canonical_scoped_record_name(matches[0]).replace("::", ".")
        return type_name

    candidates = [
        record
        for record in record_names
        if canonical_cpp_name(last_cpp_component(record)) == scoped_name
    ]
    owner_namespace = cpp_namespace_for_record(owner)
    same_namespace = [
        record
        for record in candidates
        if cpp_namespace_for_record(record) == owner_namespace
    ]
    if len(same_namespace) == 1:
        return canonical_scoped_record_name(same_namespace[0]).replace("::", ".")
    if len(candidates) == 1:
        return canonical_scoped_record_name(candidates[0]).replace("::", ".")
    return type_name


def resolve_dci_enum_type_name(type_name: str, owner: str) -> str:
    """Resolve an enum name only when its declaration is unambiguous."""
    scoped_name = canonical_scoped_record_name(type_name)
    if not scoped_name:
        return ""
    enum_names = {
        canonical_scoped_record_name(name.replace(".", "::"))
        for name in CPP_ENUM_UNDERLYING
        if canonical_scoped_record_name(name)
    }
    if scoped_name_has_root_namespace(scoped_name):
        if scoped_name in enum_names:
            return scoped_name.replace("::", ".")
        # A partially-qualified name (``level::level_enum`` written inside
        # namespace ``spdlog``) contains ``::`` yet is relative to the owner's
        # scope, so the global match fails.  Resolve it via C++ enclosing-scope
        # lookup, accepting a namespace prefix only when it is unambiguous.
        for prefix in enclosing_namespaces(owner):
            candidate = prefix + "::" + scoped_name
            if candidate in enum_names:
                return candidate.replace("::", ".")
        return ""
    candidates = [
        name
        for name in enum_names
        if canonical_cpp_name(last_cpp_component(name)) == scoped_name
    ]
    if owner:
        owner_namespace = cpp_namespace_for_record(owner)
        same_namespace = [
            name for name in candidates if cpp_namespace_for_record(name) == owner_namespace
        ]
        if len(same_namespace) == 1:
            return same_namespace[0].replace("::", ".")
    if len(candidates) == 1:
        return candidates[0].replace("::", ".")
    return ""


def qualify_symbol_record_types(symbol: Symbol, record_names: set[str]) -> None:
    type_infos = [parameter.get("type") for parameter in symbol.params]
    if symbol.ret is not None:
        type_infos.append(symbol.ret.get("type"))
    for type_info in type_infos:
        if not isinstance(type_info, dict):
            continue
        if type_info.get("kind") == "class":
            type_info["name"] = resolve_dci_record_type_name(
                str(type_info.get("name") or ""), symbol.owner, record_names
            )
        enum_name = str(type_info.get("enum_name") or "")
        if enum_name:
            resolved_enum = resolve_dci_enum_type_name(enum_name, symbol.owner)
            if resolved_enum:
                type_info["enum_name"] = resolved_enum


def dci_type_contract(type_info: dict[str, Any]) -> dict[str, Any]:
    """Return the normalized source type identity without adapter evidence."""
    return {key: value for key, value in type_info.items() if key != "cpp_type"}


def dci_type_contract_for_owner(
    raw_type: str, owner: str, record_names: set[str]
) -> dict[str, Any]:
    type_info = dci_type(raw_type, owner)
    if type_info.get("kind") == "class":
        type_info["name"] = resolve_dci_record_type_name(
            str(type_info.get("name") or ""), owner, record_names
        )
    return dci_type_contract(type_info)


def discover_free_function_names(headers: list[str]) -> list[str]:
    out: list[str] = []
    seen: set[str] = set()
    for header in headers:
        p = Path(header)
        if not p.exists():
            continue
        depth = 0
        namespaces: list[tuple[str, int]] = []
        class_scopes: list[int] = []
        for raw_line in iter_discovery_code_lines(p):
            while namespaces and depth < namespaces[-1][1]:
                namespaces.pop()
            while class_scopes and depth < class_scopes[-1]:
                class_scopes.pop()
            line = raw_line.strip()
            ns_m = RE_NAMESPACE.match(raw_line)
            if ns_m:
                ns_depth = depth + raw_line.count("{") - raw_line.count("}")
                if ns_depth > depth:
                    namespaces.append((ns_m.group(1), ns_depth))
            if RE_CLASS_SCOPE_OPEN.match(raw_line):
                class_depth = depth + raw_line.count("{") - raw_line.count("}")
                if class_depth > depth:
                    class_scopes.append(class_depth)
            if not class_scopes and line.endswith(";") and "(" in line and ")" in line:
                if not line.startswith(("class ", "struct ", "enum ", "using ", "typedef ")):
                    before = line.split("(", 1)[0].strip()
                    m = re.search(r"([A-Za-z_][A-Za-z0-9_]*)\s*$", before)
                    if m:
                        name = m.group(1)
                        prefix = "::".join(ns for ns, _ in namespaces)
                        names = [name]
                        if prefix:
                            names.append(prefix + "::" + name)
                        for candidate in names:
                            if candidate not in seen:
                                seen.add(candidate)
                                out.append(candidate)
            depth += raw_line.count("{") - raw_line.count("}")
            if depth < 0:
                depth = 0
            while namespaces and depth < namespaces[-1][1]:
                namespaces.pop()
            while class_scopes and depth < class_scopes[-1]:
                class_scopes.pop()
    return out


RE_ENUM = re.compile(
    r"\benum\s+(?:class\s+|struct\s+)?"
    r"([A-Za-z_][A-Za-z0-9_]*)"
    r"(?:\s*:\s*([^\\{]+?))?\s*\{"
)


def discover_enum_underlying(headers: list[str]) -> dict[str, str]:
    out: dict[str, str] = {}
    for header in headers:
        p = Path(header)
        if not p.exists():
            continue
        depth = 0
        namespaces: list[tuple[str, int]] = []
        for raw_line in project_source_lines(p):
            while namespaces and depth < namespaces[-1][1]:
                namespaces.pop()
            ns_m = RE_NAMESPACE.match(raw_line)
            if ns_m:
                ns_depth = depth + raw_line.count("{") - raw_line.count("}")
                if ns_depth > depth:
                    namespaces.append((ns_m.group(1), ns_depth))
            m = RE_ENUM.search(raw_line)
            if m:
                name = m.group(1)
                underlying = collapse_spaces((m.group(2) or "int").strip())
                prefix = "::".join(ns for ns, _ in namespaces)
                qualified = f"{prefix}::{name}" if prefix else name
                out[canonical_cpp_name(qualified)] = underlying
            depth += raw_line.count("{") - raw_line.count("}")
            if depth < 0:
                depth = 0
            while namespaces and depth < namespaces[-1][1]:
                namespaces.pop()
    return out


def record_allowed(name: str, allowed: set[str]) -> bool:
    clean = canonical_cpp_name(name)
    for item in allowed:
        if names_match(clean, item):
            return True
    return False


def parse_record_layouts(text: str, allowed: set[str]) -> list[RecordLayout]:
    records: list[RecordLayout] = []
    current: RecordLayout | None = None
    keep = False
    for line in text.splitlines():
        rec_m = RE_RECORD.match(line)
        if rec_m:
            if current is not None and keep:
                records.append(current)
            name = normalize_record_name(rec_m.group(2))
            keep = record_allowed(name, allowed)
            current = RecordLayout(type_name=name)
            continue
        if current is None:
            continue
        size_m = RE_SIZE.match(line)
        if size_m:
            current.size = int(size_m.group("size"))
            current.alignment = int(size_m.group("align"))
            continue
        bitfield_m = RE_BITFIELD_LAYOUT_ITEM.match(line)
        if bitfield_m:
            if len(bitfield_m.group("indent")) != 3:
                continue
            field_parsed = parse_field_payload(bitfield_m.group("payload"))
            if field_parsed is None:
                continue
            cpp_type, name = field_parsed
            bit_start = int(bitfield_m.group("bit_start"))
            bit_end = int(bitfield_m.group("bit_end"))
            storage_size = bitfield_storage_size(cpp_type) or max(1, (bit_end + 8) // 8)
            current.fields.append(
                FieldLayout(
                    name=name,
                    cpp_type=cpp_type,
                    offset=int(bitfield_m.group("byte_offset")),
                    bit_offset=bit_start,
                    bit_width=bit_end - bit_start + 1,
                    storage_size=storage_size,
                    is_signed=bitfield_is_signed(cpp_type),
                )
            )
            continue
        item_m = RE_LAYOUT_ITEM.match(line)
        if not item_m:
            continue
        indent = len(item_m.group("indent"))
        payload = item_m.group("payload")
        if "vftable pointer" in payload or "vtable pointer" in payload:
            current.has_vtable = True
        if indent != 3:
            continue
        if "vbtable pointer" in payload:
            current.vbptr_offsets.append(int(item_m.group(1)))
            continue
        base = parse_base_payload(payload)
        if base is not None:
            base_name, is_virtual = base
            offset = int(item_m.group(1))
            current.bases.append(
                BaseLayout(
                    base_name,
                    offset,
                    is_virtual=is_virtual,
                    adjustment=(
                        {
                            "kind": "table",
                            "fallback_complete_object_offset": offset,
                            "null_preserving": True,
                        }
                        if is_virtual
                        else {"kind": "constant", "offset": offset, "null_preserving": True}
                    ),
                )
            )
            continue
        field_parsed = parse_field_payload(payload)
        if field_parsed is not None:
            cpp_type, name = field_parsed
            current.fields.append(FieldLayout(name, cpp_type, int(item_m.group(1))))
    if current is not None and keep:
        records.append(current)
    return records


RE_VTABLE = re.compile(r"^VFTable for '([^']+)'(?: in '([^']+)')? \((\d+) entries\)\.")
RE_ITANIUM_VTABLE = re.compile(r"^Vtable for '([^']+)' \((\d+) entries\)\.")
RE_ITANIUM_CONSTRUCTION_VTABLE = re.compile(
    r"^Construction vtable for \('([^']+)',\s*-?\d+\) in '([^']+)'"
    r" \((\d+) entries\)\."
)
RE_ITANIUM_ADDRESS_POINT = re.compile(
    r"^-- \((?P<base>.+?),\s*-?\d+\) vtable address --$"
)
RE_ITANIUM_THIS_ADJUST = re.compile(
    r"^\[this adjustment:\s*(?P<offset>-?\d+)\s+non-virtual(?:[^]]*)\]"
)
RE_ITANIUM_RETURN_ADJUST = re.compile(
    r"^\[return adjustment:\s*(?P<offset>-?\d+)\s+non-virtual(?:[^]]*)\]"
)
RE_VTABLE_ENTRY = re.compile(r"^\s*(\d+) \| (.+?)\s*$")


def _split_outside_angle_brackets(text: str) -> list[str]:
    """Split on top-level separators, ignoring separators inside ``<...>``.

    Template arguments contain both spaces (``long long``, ``unsigned int``)
    and commas (``Pair2<double, int>``); a blind ``split()``/``split(",")``
    truncates the qualified member name or splits an argument in half.
    """
    parts: list[str] = []
    current: list[str] = []
    depth = 0
    for ch in text:
        if ch == "<":
            depth += 1
            current.append(ch)
        elif ch == ">":
            depth = max(0, depth - 1)
            current.append(ch)
        elif ch.isspace() and depth == 0:
            if current:
                parts.append("".join(current))
                current = []
        else:
            current.append(ch)
    if current:
        parts.append("".join(current))
    return parts


def _split_top_level_commas(text: str) -> list[str]:
    """Comma split that ignores commas inside ``<...>``."""
    parts: list[str] = []
    current: list[str] = []
    depth = 0
    for ch in text:
        if ch == "<":
            depth += 1
        elif ch == ">":
            depth = max(0, depth - 1)
        if ch == "," and depth == 0:
            parts.append("".join(current).strip())
            current = []
        else:
            current.append(ch)
    tail = "".join(current).strip()
    if tail:
        parts.append(tail)
    return parts


def parse_vtable_signature(text: str) -> tuple[str, str, list[str], str]:
    clean = text.strip()
    for suffix in (
        "[vector deleting]",
        "[scalar deleting]",
        "[complete]",
        "[deleting]",
        "[base]",
        "[pure]",
    ):
        if clean.endswith(suffix):
            clean = clean[: -len(suffix)].strip()
    open_idx = clean.find("(")
    close_idx = clean.rfind(")")
    if open_idx < 0 or close_idx < open_idx:
        return "", clean, [], "method"
    before = clean[:open_idx].strip()
    params_text = clean[open_idx + 1 : close_idx].strip()
    params = (
        [] if not params_text or params_text == "void"
        else _split_top_level_commas(params_text)
    )
    # The return type is whitespace-separated from the qualified member name,
    # but the member name itself may contain spaces inside template arguments
    # (``T SinkG<long long>::consume(T)``); whitespace inside ``<...>`` never
    # separates tokens.
    tokens = _split_outside_angle_brackets(before)
    name_part = tokens[-1] if tokens else ""
    if "::" not in name_part:
        return "", name_part, params, "method"
    owner, member = name_part.rsplit("::", 1)
    kind = "destructor" if member.startswith("~") else "method"
    return owner, member, params, kind


def symbol_for_vtable_entry(
    symbols: list[Symbol],
    owner: str,
    member: str,
    params_raw: list[str],
    kind: str,
    record_names: set[str],
) -> str:
    params = [
        dci_type_contract_for_owner(param, owner, record_names)
        for param in params_raw
    ]
    for sym in symbols:
        if not names_match(sym.owner, owner) or sym.member_name != member or sym.kind != kind:
            continue
        sym_params = [dci_type_contract(p["type"]) for p in sym.params]
        if sym_params == params:
            return sym.mangled
    return ""


#: <cstdint> typedefs and their desugared spellings.  ``-fdump-vtable-layouts``
#: prints the compiler-desugared type in the section header and in entry
#: owners (``SinkG<int32_t>`` dumps as ``SinkG<int>``), while the AST scan
#: spells owners the way the header did (``SinkG<int32_t>``).  Both spellings
#: must therefore compare equal when instance vtables are matched.
_DUMP_DESUGAR = {
    "int8_t": "signed char", "uint8_t": "unsigned char",
    "int16_t": "short", "uint16_t": "unsigned short",
    "int32_t": "int", "uint32_t": "unsigned int",
    "int64_t": "long long", "uint64_t": "unsigned long long",
    "intptr_t": "long long", "uintptr_t": "unsigned long long",
    "size_t": "unsigned long long", "ssize_t": "long long",
}


def desugar_dump_spelling(name: str) -> str:
    """Rewrite <cstdint> typedefs to the spellings the vtable dump prints."""
    return re.sub(
        r"[A-Za-z_][A-Za-z0-9_]*",
        lambda m: _DUMP_DESUGAR.get(m.group(0), m.group(0)),
        name,
    )


def retitle_template_instance_vtables(
    vtables: list[VTable],
    instance_names: list[str],
    symbols: list[Symbol],
) -> list[VTable]:
    """Name template-instance vtables the way the AST spelled them.

    ``-fdump-vtable-layouts`` prints a template instance's own vtable under
    the *bare* template name (``VFTable for 'SinkG'`` -- no template
    arguments, and two instantiations of the same template are
    indistinguishable by header).  The entry lines, however, carry the full
    desugared owner (``SinkG<int>::consume``), which identifies the instance.
    Retitle each bare-named self table to the ``--export-instance`` /
    explicit-instantiation spelling it belongs to, and rebind its entries to
    that instance's AST symbols so the mangled link names survive.  A
    bare-named table whose entries cannot be attributed to a requested
    instance is dropped: it would otherwise leak as a dispatch table for an
    instance nobody asked to close.  Without this pass, a C++ producer
    exports an Active materialization request for the instance (semantic_id)
    but no dispatch-table facts, and the consumer cannot lower a reverse
    override against it.
    """
    if not instance_names:
        return vtables
    by_desugared: dict[str, str] = {}
    for name in instance_names:
        canonical = canonical_cpp_name(name)
        if "<" not in canonical:
            continue
        by_desugared[normalize_record_name(desugar_dump_spelling(canonical))] = canonical
    bare_names = {name.split("<", 1)[0] for name in by_desugared.values()}
    out: list[VTable] = []
    for table in vtables:
        if table.class_name not in bare_names or table.class_name != table.base_class:
            out.append(table)
            continue
        owners = [entry.owner for entry in table.entries if entry.owner]
        if not owners or "<" not in owners[0]:
            continue
        owner = normalize_record_name(owners[0])
        target = by_desugared.get(owner)
        if target is None:
            continue
        if owner.split("<", 1)[0] != table.class_name.split("<", 1)[0]:
            continue
        table.class_name = target
        table.base_class = target
        instance_symbols = [
            sym
            for sym in symbols
            if sym.owner
            and normalize_record_name(desugar_dump_spelling(sym.owner)) == owner
        ]
        for entry in table.entries:
            if entry.owner != owners[0]:
                continue
            entry.owner = target
            if entry.name:
                entry.name = entry.name.replace(owners[0], target)
            match = next(
                (
                    sym
                    for sym in instance_symbols
                    if sym.member_name == entry.member_name
                    and sym.kind == entry.kind
                ),
                None,
            )
            if match is not None:
                entry.mangled = match.mangled
        out.append(table)
    return out


def parse_vtable_layouts(
    text: str,
    allowed: set[str],
    symbols: list[Symbol],
    pointer_width: int = 64,
) -> list[VTable]:
    out: list[VTable] = []
    current: VTable | None = None
    keep = False
    itanium_mode = False
    segment_origin = 0
    pointer_size = pointer_width // 8
    if pointer_size not in {2, 4, 8, 16}:
        raise AdapterContractError(
            f"cannot parse vtable layout with pointer width {pointer_width}"
        )
    for raw in text.splitlines():
        if raw.strip().startswith("Thunks for "):
            if current is not None and keep:
                out.append(current)
            current = None
            keep = False
            itanium_mode = False
            continue
        if raw.strip().startswith(("VFTable indices for ", "VTable indices for ")):
            if current is not None and keep:
                out.append(current)
            current = None
            keep = False
            itanium_mode = False
            continue
        clean_line = raw.strip()
        m = RE_VTABLE.match(clean_line)
        if m:
            if current is not None and keep:
                out.append(current)
            base = normalize_record_name(m.group(1))
            derived = normalize_record_name(m.group(2) or base)
            keep = record_allowed(derived, allowed) or record_allowed(base, allowed)
            current = VTable(class_name=derived, base_class=base)
            itanium_mode = False
            segment_origin = 0
            continue
        construction = RE_ITANIUM_CONSTRUCTION_VTABLE.match(clean_line)
        if construction:
            if current is not None and keep:
                out.append(current)
            base = normalize_record_name(construction.group(1))
            derived = normalize_record_name(construction.group(2))
            keep = record_allowed(derived, allowed) or record_allowed(base, allowed)
            current = VTable(class_name=derived, base_class=base)
            itanium_mode = True
            segment_origin = 0
            continue
        itanium = RE_ITANIUM_VTABLE.match(clean_line)
        if itanium:
            if current is not None and keep:
                out.append(current)
            owner = normalize_record_name(itanium.group(1))
            keep = record_allowed(owner, allowed)
            current = VTable(class_name=owner, base_class=owner)
            itanium_mode = True
            segment_origin = 0
            continue
        if current is None:
            continue
        address_point = RE_ITANIUM_ADDRESS_POINT.match(clean_line)
        if itanium_mode and address_point:
            base = normalize_record_name(address_point.group("base"))
            if names_match(current.base_class, current.class_name):
                current.base_class = base
            continue
        this_adjust = RE_ITANIUM_THIS_ADJUST.match(clean_line)
        if itanium_mode and this_adjust and current.entries:
            current.entries[-1].this_adjust = int(this_adjust.group("offset"))
            # The direct implementation symbol is not the adjusted thunk that
            # occupies this slot.  Keep dispatch executable through the slot,
            # but never claim a false direct link name.
            current.entries[-1].mangled = ""
            continue
        return_adjust = RE_ITANIUM_RETURN_ADJUST.match(clean_line)
        if itanium_mode and return_adjust and current.entries:
            current.entries[-1].return_adjust = int(return_adjust.group("offset"))
            current.entries[-1].mangled = ""
            continue
        em = RE_VTABLE_ENTRY.match(raw)
        if not em:
            continue
        idx = int(em.group(1))
        display = em.group(2).strip()
        if (
            itanium_mode
            and display.startswith("offset_to_top ")
            and any(
                entry.kind not in {"rtti", "metadata"}
                for entry in current.entries
            )
        ):
            # One Itanium vtable group can contain several address points.
            # Normalize each component into an independent DCI dispatch table.
            if keep:
                out.append(current)
            current = VTable(
                class_name=current.class_name,
                base_class=current.class_name,
            )
            segment_origin = idx
        local_index = idx - segment_origin if itanium_mode else idx
        entry_offset = local_index * pointer_size
        if display.endswith("RTTI"):
            current.entries.append(
                VTableEntry(local_index, entry_offset, display, "rtti")
            )
            continue
        if display.startswith(("offset_to_top ", "vcall_offset ", "vbase_offset ")):
            current.entries.append(
                VTableEntry(local_index, entry_offset, display, "metadata")
            )
            continue
        owner, member, params, kind = parse_vtable_signature(display)
        current.entries.append(
            VTableEntry(
                index=local_index,
                offset=entry_offset,
                name=display,
                kind=kind,
                mangled=symbol_for_vtable_entry(
                    symbols, owner, member, params, kind, allowed
                ),
                owner=owner,
                member_name=member,
            )
        )
    if current is not None and keep:
        out.append(current)
    return out


def json_stream(text: str) -> list[dict[str, Any]]:
    out: list[dict[str, Any]] = []
    dec = json.JSONDecoder()
    idx = 0
    while idx < len(text):
        while idx < len(text) and text[idx].isspace():
            idx += 1
        if idx >= len(text):
            break
        obj, idx = dec.raw_decode(text, idx)
        if isinstance(obj, dict):
            out.append(obj)
    return out


def split_function_type(qual_type: str) -> tuple[str, list[str], bool]:
    text = qual_type.strip()
    open_idx = text.find("(")
    if open_idx < 0:
        return text, [], False
    depth = 0
    close_idx = -1
    for index in range(open_idx, len(text)):
        if text[index] == "(":
            depth += 1
        elif text[index] == ")":
            depth -= 1
            if depth == 0:
                close_idx = index
                break
    if close_idx < open_idx:
        return text, [], False
    ret = text[:open_idx].strip()
    rest = text[close_idx + 1 :].strip()
    is_const = rest.startswith("const") or " const" in rest
    params_text = text[open_idx + 1 : close_idx].strip()
    if not params_text or params_text == "void":
        return ret, [], is_const
    params: list[str] = []
    angle = 0
    paren = 0
    start = 0
    for i, ch in enumerate(params_text):
        if ch == "<":
            angle += 1
        elif ch == ">" and angle > 0:
            angle -= 1
        elif ch == "(":
            paren += 1
        elif ch == ")" and paren > 0:
            paren -= 1
        elif ch == "," and angle == 0 and paren == 0:
            params.append(params_text[start:i].strip())
            start = i + 1
    params.append(params_text[start:].strip())
    return ret, params, is_const


def declared_calling_convention(qual_type: str) -> str:
    match = re.search(
        r"__attribute__\s*\(\(\s*(cdecl|stdcall|thiscall|fastcall|vectorcall)\s*\)\)",
        qual_type,
    )
    if match:
        return match.group(1)
    keyword = re.search(
        r"\b__(cdecl|stdcall|thiscall|fastcall|vectorcall)\b", qual_type
    )
    return keyword.group(1) if keyword else ""


def function_unwind_contract(qual_type: str) -> str:
    match = re.search(r"\bnoexcept\b(.*)$", qual_type)
    if not match:
        return "may_unwind"
    expression = match.group(1).strip()
    return "no_unwind" if expression in {"", "(true)", "(1)"} else "may_unwind"


def node_has_attr(node: dict[str, Any], kind: str) -> bool:
    return any(isinstance(ch, dict) and ch.get("kind") == kind for ch in node.get("inner", []) or [])


def lifecycle_node_kind(owner: str, node: dict[str, Any]) -> str:
    node_kind = node.get("kind") or ""
    name = node.get("name") or ""
    params = [
        ((child.get("type") or {}).get("qualType") or "")
        for child in node.get("inner", []) or []
        if isinstance(child, dict) and child.get("kind") == "ParmVarDecl"
    ]
    if node_kind == "CXXDestructorDecl":
        return "destroy"
    if node_kind not in {"CXXConstructorDecl", "CXXMethodDecl"}:
        return ""
    if node_kind == "CXXConstructorDecl" and not params:
        return "default_construct"
    if len(params) != 1:
        return ""
    raw_param = collapse_spaces(params[0])
    value_type = re.sub(r"\b(?:const|volatile)\b", "", raw_param)
    value_type = collapse_spaces(value_type.replace("&&", "").replace("&", ""))
    if not names_match(value_type, owner):
        return ""
    is_move = "&&" in raw_param
    if node_kind == "CXXConstructorDecl":
        return "move_construct" if is_move else "copy_construct"
    if name == "operator=":
        return "move_assign" if is_move else "copy_assign"
    return ""


def lifecycle_operation(
    operation: str,
    definition_data: dict[str, Any],
    node: dict[str, Any] | None,
    access: str,
) -> dict[str, Any]:
    metadata_key = {
        "default_construct": "defaultCtor",
        "copy_construct": "copyCtor",
        "move_construct": "moveCtor",
        "copy_assign": "copyAssign",
        "move_assign": "moveAssign",
        "destroy": "dtor",
    }[operation]
    metadata = definition_data.get(metadata_key) or {}
    if node is not None:
        exists = True
    elif operation in {"copy_construct", "copy_assign", "destroy"}:
        exists = bool(metadata)
    else:
        exists = bool(metadata.get("exists"))
    deleted = bool(
        node
        and (
            node.get("explicitlyDeleted")
            or node.get("explicitlyDefaulted") == "deleted"
        )
    )
    trivial: bool | None
    if metadata.get("trivial"):
        trivial = True
    elif metadata.get("nonTrivial"):
        trivial = False
    else:
        trivial = None
    qual_type = ((node or {}).get("type") or {}).get("qualType") or ""
    if node is None:
        declared = "implicit" if metadata.get("needsImplicit") else "unavailable"
    elif node.get("isImplicit"):
        declared = "implicit"
    elif node.get("explicitlyDefaulted"):
        declared = "deleted" if deleted else "defaulted"
    else:
        declared = "user"
    link_name = (
        ""
        if deleted or (node or {}).get("isImplicit")
        else ((node or {}).get("mangledName") or "")
    )
    execution = "unavailable"
    if exists and not deleted:
        if trivial:
            execution = "trivial"
        elif link_name:
            execution = "direct"
        else:
            execution = "stub_required"
    return {
        "available": exists and not deleted,
        "accessible": access == "public",
        "deleted": deleted,
        "trivial": trivial,
        "declared": declared,
        "execution": execution,
        "link_name": link_name,
        "unwind": function_unwind_contract(qual_type) if qual_type else "unknown",
    }


def ast_record_contracts(
    nodes: list[dict[str, Any]], record_names: set[str]
) -> dict[str, dict[str, Any]]:
    contracts: dict[str, dict[str, Any]] = {}
    for top in nodes:
        for rec in walk(top):
            if rec.get("kind") not in {"CXXRecordDecl", "ClassTemplateSpecializationDecl"}:
                continue
            if not rec.get("completeDefinition"):
                continue
            owner = resolve_record_owner_from_node(rec, record_names)
            if not owner:
                continue
            definition_data = rec.get("definitionData") or {}
            operations: dict[str, tuple[dict[str, Any], str]] = {}
            access = "public" if rec.get("tagUsed") == "struct" else "private"
            for child in rec.get("inner", []) or []:
                if not isinstance(child, dict):
                    continue
                if child.get("kind") == "AccessSpecDecl":
                    access = child.get("access") or access
                    continue
                op = lifecycle_node_kind(owner, child)
                if not op:
                    continue
                previous = operations.get(op)
                if previous is None or previous[0].get("isImplicit"):
                    operations[op] = (child, access)
            lifecycle = {}
            for operation in (
                "default_construct",
                "copy_construct",
                "move_construct",
                "copy_assign",
                "move_assign",
                "destroy",
            ):
                pair = operations.get(operation)
                lifecycle[operation] = lifecycle_operation(
                    operation,
                    definition_data,
                    pair[0] if pair else None,
                    pair[1] if pair else "public",
                )
            bases = []
            for base in rec.get("bases", []) or []:
                if not isinstance(base, dict):
                    continue
                base_name = ((base.get("type") or {}).get("qualType") or "")
                if base_name:
                    bases.append(
                        {
                            "type_name": base_name,
                            "visibility": base.get("access") or "private",
                            "is_virtual": bool(base.get("isVirtual")),
                        }
                    )
            contracts[owner] = {
                "traits": {
                    "pod": bool(definition_data.get("isPOD")),
                    "trivial": bool(definition_data.get("isTrivial")),
                    "trivially_copyable": bool(definition_data.get("isTriviallyCopyable")),
                    "can_pass_in_registers": bool(definition_data.get("canPassInRegisters")),
                    "standard_layout": bool(definition_data.get("isStandardLayout")),
                    "aggregate": bool(definition_data.get("isAggregate")),
                    "polymorphic": bool(definition_data.get("isPolymorphic")),
                    "abstract": bool(definition_data.get("isAbstract")),
                    "final": node_has_attr(rec, "FinalAttr"),
                    "has_virtual_bases": any(base["is_virtual"] for base in bases),
                },
                "lifecycle": lifecycle,
                "bases": bases,
            }
    return contracts


def apply_record_contracts(
    records: list[RecordLayout], contracts: dict[str, dict[str, Any]]
) -> None:
    for record in records:
        contract = next(
            (value for name, value in contracts.items() if names_match(name, record.type_name)),
            None,
        )
        if contract is None:
            continue
        record.traits = dict(contract.get("traits") or {})
        record.lifecycle = dict(contract.get("lifecycle") or {})
        contract_bases = contract.get("bases") or []
        for base in record.bases:
            semantic = next(
                (
                    item
                    for item in contract_bases
                    if names_match(item.get("type_name") or "", base.type_name)
                    and bool(item.get("is_virtual")) == base.is_virtual
                ),
                None,
            )
            if semantic is not None:
                base.visibility = semantic.get("visibility") or base.visibility


def msvc_complete_destructor_mangled(ast_mangled: str, is_virtual: bool) -> str:
    """Return the complete-object destructor symbol for MSVC targets.

    Clang's AST JSON reports the deleting destructor for CXXDestructorDecl on
    MSVC. DCI scope-drop needs the complete-object destructor, matching what
    native C++ emits for automatic storage objects.
    """
    if not ast_mangled.startswith("??_D"):
        return ast_mangled
    owner_end = ast_mangled.find("@@")
    if owner_end < 4:
        return ast_mangled
    owner_encoding = ast_mangled[4:owner_end]
    access_call = "UEAA" if is_virtual else "QEAA"
    return "??1" + owner_encoding + "@@" + access_call + "@XZ"


def symbol_from_method(owner: str, node: dict[str, Any], access: str) -> Symbol | None:
    if (
        node.get("isImplicit")
        or node.get("explicitlyDeleted")
        or node.get("explicitlyDefaulted") == "deleted"
    ):
        return None
    mangled = node.get("mangledName") or ""
    if not mangled:
        return None
    node_kind = node.get("kind")
    name = node.get("name") or ""
    if not name:
        return None
    qual_type = (node.get("type") or {}).get("qualType") or ""
    ret, params_raw, is_const = split_function_type(qual_type)
    qualifiers = qual_type.rsplit(")", 1)[-1].split("noexcept", 1)[0].strip()
    if "noexcept(" in qual_type or "noexcept (" in qual_type:
        qualifiers = qual_type[:qual_type.index("noexcept")].rsplit(")", 1)[-1].strip()
    kind = "method"
    member_name = name
    is_static = node.get("storageClass") == "static"
    is_override = node_has_attr(node, "OverrideAttr")
    is_final = node_has_attr(node, "FinalAttr")
    is_virtual = bool(node.get("virtual") or is_override)
    if node_kind == "CXXConstructorDecl":
        kind = "constructor"
        member_name = "constructor"
        ret = "void"
        is_static = False
    elif node_kind == "CXXDestructorDecl":
        kind = "destructor"
        member_name = destructor_member_name_for_owner(owner)
        ret = "void"
        is_static = False
        mangled = msvc_complete_destructor_mangled(mangled, is_virtual)
    params = [
        {"name": f"p{i}", "type": dci_type(p, owner), "location": "abi"}
        for i, p in enumerate(params_raw)
    ]
    if kind == "method" and not is_static:
        call_cc = "cxx_virtual_method" if is_virtual else "cxx_method"
    elif kind == "constructor":
        call_cc = "cxx_constructor"
    elif kind == "destructor":
        call_cc = "cxx_destructor"
    else:
        call_cc = "cxx_static_method"
    return Symbol(
        name=(owner + "::" + member_name) if owner else member_name,
        owner=owner,
        member_name=member_name,
        mangled=mangled,
        kind=kind,
        calling_convention=call_cc,
        params=params,
        ret=None if kind in {"constructor", "destructor"} else {"type": dci_type(ret, owner), "location": "abi"},
        is_static=is_static,
        is_virtual=is_virtual,
        is_override=is_override,
        is_final=is_final,
        is_const=is_const,
        visibility=access,
        native_calling_convention=declared_calling_convention(qual_type),
        unwind=function_unwind_contract(qual_type),
        member_qualifiers=qualifiers,
    )


def walk(node: Any):
    if isinstance(node, dict):
        yield node
        for child in node.get("inner", []) or []:
            yield from walk(child)
    elif isinstance(node, list):
        for child in node:
            yield from walk(child)


def ast_symbols(nodes: list[dict[str, Any]], record_names: set[str], free_names: set[str]) -> list[Symbol]:
    out: list[Symbol] = []
    seen: set[tuple[str, str, str, str]] = set()
    for top in nodes:
        for rec in walk(top):
            if rec.get("kind") in ("CXXRecordDecl", "ClassTemplateSpecializationDecl") and rec.get("completeDefinition"):
                owner = resolve_record_owner_from_node(rec, record_names)
                if not owner:
                    continue
                for alias in rec.get("inner", []) or []:
                    if alias.get("kind") not in {"TypedefDecl", "TypeAliasDecl"} or not alias.get("name"):
                        continue
                    alias_type = alias.get("type") or {}
                    target = alias_type.get("desugaredQualType") or alias_type.get("qualType")
                    if target and not any(t in target for t in ("type-parameter", "...")):
                        CPP_TYPE_ALIAS_TARGETS[normalize_cpp_type(owner + "::" + alias["name"])] = target
                access = "public" if rec.get("tagUsed") == "struct" else "private"
                for child in rec.get("inner", []) or []:
                    if not isinstance(child, dict):
                        continue
                    if child.get("kind") == "AccessSpecDecl":
                        access = child.get("access") or access
                        continue
                    if child.get("kind") in ("CXXConstructorDecl", "CXXDestructorDecl", "CXXMethodDecl"):
                        sym = symbol_from_method(owner, child, access)
                        if sym is None:
                            continue
                        qualify_symbol_record_types(sym, record_names)
                        key = symbol_identity(sym)[:-1]
                        if key not in seen:
                            seen.add(key)
                            out.append(sym)
            elif rec.get("kind") == "FunctionDecl" and (rec.get("name") or "") in free_names:
                if rec.get("isImplicit") or rec.get("explicitlyDeleted"):
                    continue
                mangled = rec.get("mangledName") or ""
                name = rec.get("name") or ""
                if not mangled or not name:
                    continue
                qual_type = (rec.get("type") or {}).get("qualType") or ""
                ret, params_raw, _ = split_function_type(qual_type)
                params = [{"name": f"p{i}", "type": dci_type(p), "location": "abi"} for i, p in enumerate(params_raw)]
                sym = Symbol(
                    name=name,
                    owner="",
                    member_name=name,
                    mangled=mangled,
                    kind="function",
                    calling_convention="cxx_free_function",
                    params=params,
                    ret={"type": dci_type(ret), "location": "abi"},
                    visibility="public",
                    native_calling_convention=declared_calling_convention(qual_type),
                    unwind=function_unwind_contract(qual_type),
                )
                qualify_symbol_record_types(sym, record_names)
                key = symbol_identity(sym)[:-1]
                if key not in seen:
                    seen.add(key)
                    out.append(sym)
    return out


def register_desugared_alias(sugared: str, desugared: str) -> None:
    """Adopt a compiler-reported ``alias -> record`` desugaring.

    Clang annotates a sugared type reference with ``desugaredQualType`` (for
    example a ``string_view_t`` parameter whose canonical type is
    ``fmt::basic_string_view<char>``).  These are authoritative type identities
    from the fact extractor, so recording them lets ``dci_type`` match a spelled
    alias to the record whose layout the same dump provides -- even when the
    alias declaration lives in a header outside the scanned project roots.
    Scalar typedefs already in the primitive map (``size_t``, ``std::size_t``,
    ``std::uint32_t``, ...) must never be shadowed by a desugaring.  An
    unknown typedef whose canonical type is a primitive (``using MySize =
    unsigned long long``) is recorded so ``dci_type`` can treat it as that
    integer.  Conflicting desugarings are refused rather than guessed.
    """
    sugared_clean = normalize_cpp_type(sugared)
    if not sugared_clean:
        return
    if any(token in sugared_clean for token in ("*", "&", "(", "...", "type-parameter")):
        return
    desugared_clean = normalize_cpp_type(desugared)
    if not desugared_clean or desugared_clean == sugared_clean:
        return
    if any(token in desugared_clean for token in ("*", "&", "(", "...", "type-parameter")):
        return
    if cpp_spelling_is_known_primitive(sugared_clean):
        return
    existing = CPP_TYPE_ALIAS_TARGETS.get(sugared_clean)
    if existing and normalize_cpp_type(existing) != desugared_clean:
        return
    CPP_TYPE_ALIAS_TARGETS[sugared_clean] = collapse_spaces(desugared)


def harvest_ast_type_aliases(nodes: list[dict[str, Any]]) -> None:
    """Register every ``alias -> record`` desugaring the fact extractor emitted."""
    for top in nodes:
        for node in walk(top):
            if not isinstance(node, dict):
                continue
            type_obj = node.get("type")
            if not isinstance(type_obj, dict):
                continue
            sugared = type_obj.get("qualType")
            desugared = type_obj.get("desugaredQualType")
            if isinstance(sugared, str) and isinstance(desugared, str):
                register_desugared_alias(sugared, desugared)


def stdint_cpp_name(size: int, is_signed: bool) -> str:
    """Return the fixed-width C++ spelling for a probed integer size."""
    if size in (1, 2, 4, 8):
        return f"{'int' if is_signed else 'uint'}{size * 8}_t"
    return ""


RE_PLAIN_SCOPED_IDENTIFIER = re.compile(r"^[A-Za-z_]\w*(?:::[A-Za-z_]\w*)*$")


def enum_name_candidates(spelled: str, owner: str) -> list[str]:
    """Owner-scoped C++ name candidates for a spelled (possibly relative) type."""
    scoped = canonical_scoped_record_name(spelled)
    if not scoped:
        return []
    # An enum is always a plain (possibly scoped) identifier.  Reject template
    # specializations, ``typename`` dependent names, references/pointers, lambda
    # placeholders and library-internal typedefs so the diagnostic probe is fed
    # only real enum candidates and does not choke on unrelated garbage types.
    if not RE_PLAIN_SCOPED_IDENTIFIER.match(scoped):
        return []
    candidates: list[str] = []
    for prefix in enclosing_namespaces(owner):
        candidates.append(prefix + "::" + scoped)
    candidates.append(scoped)
    seen: set[str] = set()
    unique: list[str] = []
    for candidate in candidates:
        if candidate not in seen:
            seen.add(candidate)
            unique.append(candidate)
    return unique


RE_ENUM_PROBE = re.compile(
    r"__vyx_enum<\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*>"
)


def _run_diagnostic_type_probe(
    toolchain: cpp_toolchains.CppToolchain,
    std: str,
    probe: Path,
    compiler_args: list[str],
    target: str,
    extra_flags: list[str] | None = None,
) -> str:
    """Compile a syntax-only diagnostic probe with the selected compiler.

    Like ``_run_self_layout_probe`` the probe deliberately triggers undefined
    template instantiations whose arguments echo the compiler's own answers;
    a non-zero exit is expected and the combined output is returned.
    """
    flags = list(extra_flags or [])
    if toolchain.family == "msvc" or toolchain.driver_mode == "clang-cl":
        command = [
            toolchain.executable,
            "/nologo",
            "/Zs",
            f"/std:{std}",
            *flags,
            str(probe),
            *compiler_args,
        ]
    else:
        error_limit = (
            "-ferror-limit=0" if toolchain.family == "clang" else "-fmax-errors=0"
        )
        if toolchain.family == "clang":
            # Typo-correction would silently "fix" an invalid candidate
            # qualification into a nearby valid one, producing false positives.
            flags = ["-fno-spell-checking", *flags]
        command = [
            toolchain.executable,
            f"-std={std}",
            "-fsyntax-only",
            error_limit,
            *flags,
            str(probe),
            *compiler_args,
        ]
    environment = msvc_validation_environment(toolchain, target)
    proc = subprocess.run(
        command,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
        env=environment,
    )
    return proc.stdout or ""


def discover_enum_underlying_with_selected_compiler(
    toolchain: cpp_toolchains.CppToolchain,
    std: str,
    target: str,
    headers: list[str],
    candidates: list[str],
    compiler_args: list[str],
) -> dict[str, str]:
    """Probe candidate type names; record those the selected compiler proves are enums.

    The selected ``--toolchain`` compiler is the authority: only names it
    accepts as ``std::is_enum`` are recorded, keyed by their canonical C++ name,
    with the exact underlying width/signedness it reports.  Names it rejects
    (non-enums, invalid qualifications) simply produce no diagnostic and are
    dropped -- never guessed.
    """
    unique: list[str] = []
    seen: set[str] = set()
    for name in candidates:
        canonical = canonical_scoped_record_name(name)
        if not canonical or not RE_PLAIN_SCOPED_IDENTIFIER.match(canonical):
            continue
        if canonical not in seen:
            seen.add(canonical)
            unique.append(canonical)
    if not unique:
        return {}
    with tempfile.TemporaryDirectory(prefix="vyx_dci_enum_") as temp_dir:
        probe = Path(temp_dir) / "dci_enum_probe.cpp"
        lines = [f'#include "{header}"' for header in headers]
        lines.append("#include <type_traits>")
        lines.append("namespace {")
        lines.append(
            "template<class T, bool = std::is_enum<T>::value> struct __vyx_ep {"
            " static constexpr int is_enum = 0;"
            " static constexpr unsigned long long size = 0;"
            " static constexpr int is_signed = 0; };"
        )
        lines.append(
            "template<class T> struct __vyx_ep<T, true> {"
            " static constexpr int is_enum = 1;"
            " static constexpr unsigned long long size = sizeof(T);"
            " using U = typename std::underlying_type<T>::type;"
            " static constexpr int is_signed = std::is_signed<U>::value ? 1 : 0; };"
        )
        lines.append("template<int, int, unsigned long long, int> struct __vyx_enum;")
        lines.append("}")
        for index, name in enumerate(unique):
            probe_expr = f"__vyx_ep<{name}>"
            lines.append(
                f"__vyx_enum<{index}, {probe_expr}::is_enum, {probe_expr}::size, "
                f"{probe_expr}::is_signed> __vyx_ev{index};"
            )
        probe.write_text("\n".join(lines) + "\n", encoding="utf-8")
        output = _run_diagnostic_type_probe(
            toolchain, std, probe, compiler_args, target
        )
    result: dict[str, str] = {}
    for match in RE_ENUM_PROBE.finditer(output):
        index = int(match.group(1))
        is_enum = int(match.group(2))
        size = int(match.group(3))
        is_signed = int(match.group(4))
        if not is_enum or index >= len(unique):
            continue
        underlying = stdint_cpp_name(size, bool(is_signed))
        if underlying:
            result[unique[index]] = underlying
    return result


def collect_enum_probe_candidates(
    symbols: list[Symbol], layouts: list[RecordLayout]
) -> list[str]:
    """Owner-scoped candidate names for every not-yet-resolved class-like type.

    Field, base, parameter and return types that ``dci_type`` currently treats
    as an opaque ``class`` (because no layout/enum/alias resolved them) are the
    only places an undiscovered enum can hide.  Real records among them are
    harmless: the enum probe simply reports ``is_enum = 0`` for them.
    """
    candidates: list[str] = []

    def consider(spelled: str, owner: str) -> None:
        info = dci_type(spelled, owner)
        if info.get("kind") != "class":
            return
        if any(names_match(record.type_name, str(info.get("name") or "")) for record in layouts):
            return
        candidates.extend(enum_name_candidates(spelled, owner))

    for record in layouts:
        for field_layout in record.fields:
            consider(field_layout.cpp_type, record.type_name)
    for symbol in symbols:
        for parameter in symbol.params:
            type_info = parameter.get("type")
            if isinstance(type_info, dict) and type_info.get("cpp_type"):
                consider(str(type_info.get("cpp_type")), symbol.owner)
        if symbol.ret is not None:
            type_info = symbol.ret.get("type")
            if isinstance(type_info, dict) and type_info.get("cpp_type"):
                consider(str(type_info.get("cpp_type")), symbol.owner)
    unique: list[str] = []
    seen: set[str] = set()
    for candidate in candidates:
        if candidate not in seen:
            seen.add(candidate)
            unique.append(candidate)
    return unique


def rewrite_enum_symbol_types(symbols: list[Symbol]) -> None:
    """Re-derive param/return types now that ``CPP_ENUM_UNDERLYING`` is complete.

    Only types that newly resolve to an enum primitive are rewritten; class
    types keep the qualified names already resolved by ``ast_symbols``.
    """
    def rewrite(type_info: dict[str, Any], owner: str) -> None:
        if type_info.get("kind") != "class":
            return
        cpp_type = str(type_info.get("cpp_type") or "")
        if not cpp_type:
            return
        rederived = dci_type(cpp_type, owner)
        if rederived.get("kind") == "primitive" and rederived.get("enum_name"):
            type_info.clear()
            type_info.update(rederived)

    for symbol in symbols:
        for parameter in symbol.params:
            type_info = parameter.get("type")
            if isinstance(type_info, dict):
                rewrite(type_info, symbol.owner)
        if symbol.ret is not None:
            type_info = symbol.ret.get("type")
            if isinstance(type_info, dict):
                rewrite(type_info, symbol.owner)
    for symbol in symbols:
        qualify_symbol_record_types(symbol, set())


def record_names_from_field_types(
    records: list[RecordLayout], allowed: set[str]
) -> set[str]:
    """Record type names referenced by fields/bases that are class aggregates.

    Returns canonical ``a::b::c`` names for every field or base whose
    ``dci_type`` is a class (i.e. needs its own layout) so the layout dump can
    be re-parsed with an expanded ``allowed`` closure.
    """
    discovered: set[str] = set()
    for record in records:
        referenced: list[str] = [field.cpp_type for field in record.fields]
        referenced.extend(base.type_name for base in record.bases)
        for cpp_type in referenced:
            info = dci_type(cpp_type, record.type_name)
            if info.get("kind") != "class":
                continue
            scoped = canonical_scoped_record_name(str(info.get("name") or "").replace(".", "::"))
            if scoped and not record_allowed(scoped, allowed):
                discovered.add(scoped)
    return discovered


def expand_layout_closure(
    layout_dump: str, record_names: list[str], max_depth: int = 4
) -> tuple[list[RecordLayout], list[str]]:
    """Close the parsed layouts over nested class-typed fields and bases.

    ``parse_record_layouts`` only keeps records named in ``allowed``; a nested
    field type such as ``fmt::basic_string_view<char>`` (``log_msg.payload``)
    is emitted by Clang in the same dump but dropped.  This iterates to a
    fixpoint (capped at ``max_depth``), each round adding the field/base record
    types of the currently-kept layouts to ``allowed`` and re-parsing, so Vyx
    can read the nested members.  Returns the closed layout list and the final
    expanded record-name list.
    """
    allowed = list(record_names)
    layouts = parse_record_layouts(layout_dump, set(allowed))
    for _ in range(max_depth):
        discovered = record_names_from_field_types(layouts, set(allowed))
        # Only adopt names Clang actually dumped as their own record layout.
        dumped = discovered & {
            canonical_scoped_record_name(record.type_name)
            for record in parse_record_layouts(layout_dump, discovered)
        }
        new_names = [name for name in dumped if not record_allowed(name, set(allowed))]
        if not new_names:
            break
        allowed.extend(new_names)
        layouts = parse_record_layouts(layout_dump, set(allowed))
    return layouts, allowed


RE_AGG_TRAITS_PROBE = re.compile(
    r"__vyx_tc<\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*>"
)


def _clang_codegen_driver(toolchain: cpp_toolchains.CppToolchain) -> str:
    """Prefer ``clang++`` so we can dump LLVM IR even when the user selected clang-cl."""
    executable = Path(toolchain.executable)
    if toolchain.driver_mode == "clang-cl":
        sibling_name = "clang++.exe" if executable.suffix.lower() == ".exe" else "clang++"
        sibling = executable.with_name(sibling_name)
        if sibling.exists():
            return str(sibling)
    return toolchain.executable


def _run_codegen_probe(
    toolchain: cpp_toolchains.CppToolchain,
    std: str,
    probe: Path,
    compiler_args: list[str],
    target: str,
) -> str | None:
    """Ask the selected compiler to dump its own CodeGen for the probe.

    * Clang: ``-S -emit-llvm`` (the public CodeGen dump of ``ABIArgInfo``).
    * GCC: ``-S`` (the compiler's own backend, not a reconstructed SysV table).
    * MSVC: ``/FA`` listing (the compiler's own x64 argument setup).

    Returns ``None`` when the compiler could not produce output so the caller
    fails closed.  This function does not classify the dump.
    """
    if toolchain.family == "clang":
        output = probe.with_suffix(".ll")
        command = [
            _clang_codegen_driver(toolchain),
            f"-std={std}",
            "-S",
            "-emit-llvm",
            "-O0",
            "-fno-exceptions",
            "-o",
            str(output),
            str(probe),
            *compiler_args,
        ]
    elif toolchain.family == "gcc":
        output = probe.with_suffix(".s")
        command = [
            toolchain.executable,
            f"-std={std}",
            "-S",
            "-O0",
            "-fno-exceptions",
            "-fno-asynchronous-unwind-tables",
            "-fverbose-asm",
            "-o",
            str(output),
            str(probe),
            *compiler_args,
        ]
    elif toolchain.family == "msvc":
        output = probe.with_suffix(".asm")
        obj_output = probe.with_suffix(".obj")
        command = [
            toolchain.executable,
            "/nologo",
            f"/std:{std}",
            "/Od",
            "/c",
            "/FA",
            f"/Fa{output}",
            f"/Fo{obj_output}",
            str(probe),
            *compiler_args,
        ]
    else:
        return None
    environment = msvc_validation_environment(toolchain, target)
    proc = subprocess.run(
        command,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
        env=environment,
    )
    if proc.returncode != 0 or not output.exists():
        return None
    return output.read_text(encoding="utf-8", errors="replace")


def _split_ir_params(params_text: str) -> list[str]:
    parts: list[str] = []
    depth = 0
    current = ""
    for char in params_text:
        if char in "([<{":
            depth += 1
        elif char in ")]>}":
            depth -= 1
        if char == "," and depth == 0:
            parts.append(current.strip())
            current = ""
        else:
            current += char
    if current.strip():
        parts.append(current.strip())
    return parts


_LLVM_SIMPLE_TYPE = re.compile(r"(ptr|void|half|float|double|fp128|x86_fp80|i\d+)")


def _ir_leading_type(text: str) -> str | None:
    """First LLVM type in a ``declare`` parameter or return fragment.

    This copies the compiler's CodeGen type, including a struct written as
    ``{ i64, i64 }``.  It does not invent a type from record fields.
    """
    text = text.strip()
    if not text:
        return None
    if text.startswith("{"):
        depth = 0
        for index, char in enumerate(text):
            if char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0:
                    inner = text[1:index]
                    pieces: list[str] = []
                    for part in _split_ir_params(inner):
                        piece = _ir_leading_type(part)
                        if piece is None:
                            return None
                        pieces.append(piece)
                    return "{" + ",".join(pieces) + "}"
        return None
    match = _LLVM_SIMPLE_TYPE.match(text)
    return match.group(1) if match else None


def _llvm_type_pieces(llvm_type: str) -> list[str]:
    text = llvm_type.strip()
    if text.startswith("{") and text.endswith("}"):
        pieces: list[str] = []
        for part in _split_ir_params(text[1:-1]):
            piece = _ir_leading_type(part)
            if piece is None:
                return []
            pieces.append(piece)
        return pieces
    return [text] if text else []


def _llvm_token_sysv_class(token: str) -> str:
    if token in {"float", "double", "half", "fp128", "x86_fp80"} or token.startswith("<"):
        return "SSE"
    if token == "ptr" or re.fullmatch(r"i\d+", token):
        return "INTEGER"
    if token.startswith("{"):
        return "STRUCT"
    return "UNKNOWN"


def _clang_ir_param_shape(ir_text: str, index: int) -> dict[str, Any] | None:
    """Clang CodeGen's LLVM parameter list for ``vyx_arg{index}``."""
    match = re.search(
        r"declare[^\n@]*@vyx_arg%d\(([^)]*)\)" % index, ir_text
    )
    if match is None:
        return None
    params_text = match.group(1).strip()
    if not params_text:
        return {"memory": False, "byval": False, "inalloca": False, "llvm_args": []}
    llvm_args: list[str] = []
    for param in _split_ir_params(params_text):
        if "inalloca" in param:
            return {"memory": True, "byval": False, "inalloca": True, "llvm_args": []}
        if "byval" in param:
            return {"memory": True, "byval": True, "inalloca": False, "llvm_args": []}
        if "sret" in param:
            continue
        token = _ir_leading_type(param)
        if token is None or token == "void":
            return None
        llvm_args.append(token)
    return {"memory": False, "byval": False, "llvm_args": llvm_args}


def _clang_ir_return_shape(ir_text: str, index: int) -> dict[str, Any] | None:
    """Clang CodeGen's LLVM return type for ``vyx_ret{index}``."""
    match = re.search(
        r"declare(?P<pre>[^\n@]*)@vyx_ret%d\((?P<args>[^)]*)\)" % index,
        ir_text,
    )
    if match is None:
        return None
    args = match.group("args")
    if "sret" in args:
        return {"ret_sret": True, "ret_llvm": "void"}
    pre = match.group("pre")
    struct = re.search(r"\{[^{}]*\}", pre)
    if struct:
        llvm_type = _ir_leading_type(struct.group(0))
        if llvm_type is None:
            return None
        return {"ret_sret": False, "ret_llvm": llvm_type}
    found = _LLVM_SIMPLE_TYPE.findall(pre)
    if not found:
        return None
    last = found[-1]
    if last == "void":
        return {"ret_sret": True, "ret_llvm": "void"}
    return {"ret_sret": False, "ret_llvm": last}


def _clang_ir_argument_classes(ir_text: str, index: int) -> dict[str, Any] | None:
    param = _clang_ir_param_shape(ir_text, index)
    returned = _clang_ir_return_shape(ir_text, index)
    if param is None and returned is None:
        return None
    param = param or {"memory": False, "byval": False, "llvm_args": []}
    returned = returned or {}
    llvm_args = list(param.get("llvm_args") or [])
    classes = [_llvm_token_sysv_class(token) for token in llvm_args]
    return {
        "memory": bool(param.get("memory")),
        "byval": bool(param.get("byval")),
        "llvm_args": llvm_args,
        "classes": classes,
        "ret_sret": bool(returned.get("ret_sret")),
        "ret_llvm": returned.get("ret_llvm"),
    }


def _gcc_asm_function_body(asm_text: str, index: int) -> list[str] | None:
    lines = asm_text.splitlines()
    label = f"vyx_call{index}:"
    body: list[str] = []
    inside = False
    for line in lines:
        stripped = line.strip()
        if stripped == label:
            inside = True
            continue
        if inside:
            if re.match(r"^[A-Za-z_.][\w.$]*:", stripped) and not stripped.startswith("."):
                break
            body.append(line)
            # `-fverbose-asm` copies the C++ source into `#` comments. Those
            # lines contain both `vyx_call0()` and `vyx_arg0`, so a raw
            # `"call" in line` substring match stops the body before any
            # real instruction (g++-13).
            code = _strip_asm_comment(line, "#")
            if f"vyx_arg{index}" in code and re.search(r"\bcall\b", code):
                break
    return body or None


def _strip_asm_comment(line: str, comment_marks: str) -> str:
    text = line
    for mark in comment_marks:
        index = text.find(mark)
        if index >= 0:
            text = text[:index]
    return text.strip()


def _att_instruction(line: str) -> tuple[str, str] | None:
    text = _strip_asm_comment(line, "#")
    if not text or text.startswith(".") or text.endswith(":"):
        return None
    match = re.match(r"^([A-Za-z][A-Za-z0-9.]*)(?:\s+(.*))?$", text)
    if match is None:
        return None
    return match.group(1).lower(), match.group(2) or ""


def _intel_instruction(line: str) -> tuple[str, list[str]] | None:
    text = _strip_asm_comment(line, ";")
    if not text or text.endswith(":") or text.upper().startswith("INCLUDE"):
        return None
    match = re.match(r"^([A-Za-z][A-Za-z0-9]*)(?:\s+(.*))?$", text)
    if match is None:
        return None
    operands = _split_ir_params(match.group(2) or "")
    return match.group(1).lower(), operands


_SYSV_ARG_ORDER = (
    "rdi",
    "rsi",
    "rdx",
    "rcx",
    "r8",
    "r9",
    *(f"xmm{i}" for i in range(8)),
)

_SYSV_ARG_REGS = {
    "rdi": ("int", "rdi"),
    "edi": ("int", "rdi"),
    "di": ("int", "rdi"),
    "dil": ("int", "rdi"),
    "rsi": ("int", "rsi"),
    "esi": ("int", "rsi"),
    "si": ("int", "rsi"),
    "sil": ("int", "rsi"),
    "rdx": ("int", "rdx"),
    "edx": ("int", "rdx"),
    "dx": ("int", "rdx"),
    "dl": ("int", "rdx"),
    "rcx": ("int", "rcx"),
    "ecx": ("int", "rcx"),
    "cx": ("int", "rcx"),
    "cl": ("int", "rcx"),
    "r8": ("int", "r8"),
    "r8d": ("int", "r8"),
    "r8w": ("int", "r8"),
    "r8b": ("int", "r8"),
    "r9": ("int", "r9"),
    "r9d": ("int", "r9"),
    "r9w": ("int", "r9"),
    "r9b": ("int", "r9"),
    **{f"xmm{i}": ("sse", f"xmm{i}") for i in range(8)},
}

_SYSV_RET_REGS = {
    "rax": "i64",
    "eax": "i32",
    "ax": "i16",
    "al": "i8",
    "rdx": "i64",
    "edx": "i32",
    "xmm0": "double",
    "xmm1": "double",
}

_WIN64_ARG_REGS = {
    "rcx": ("int", "rcx"),
    "ecx": ("int", "rcx"),
    "cx": ("int", "rcx"),
    "cl": ("int", "rcx"),
    "rdx": ("int", "rdx"),
    "edx": ("int", "rdx"),
    "dx": ("int", "rdx"),
    "dl": ("int", "rdx"),
    "r8": ("int", "r8"),
    "r8d": ("int", "r8"),
    "r8w": ("int", "r8"),
    "r8b": ("int", "r8"),
    "r9": ("int", "r9"),
    "r9d": ("int", "r9"),
    "r9w": ("int", "r9"),
    "r9b": ("int", "r9"),
    **{f"xmm{i}": ("sse", f"xmm{i}") for i in range(4)},
}


def _integer_token_for_reg(reg: str) -> str:
    if reg in {"dil", "sil", "dl", "cl", "r8b", "r9b", "al"}:
        return "i8"
    if reg in {"di", "si", "dx", "cx", "r8w", "r9w", "ax"}:
        return "i16"
    if reg in {"edi", "esi", "edx", "ecx", "r8d", "r9d", "eax"}:
        return "i32"
    return "i64"


def _sse_token_for_mnemonic(mnemonic: str) -> str:
    if mnemonic.endswith("ss") or mnemonic in {"movd"}:
        return "float"
    return "double"


def _token_from_move(mnemonic: str, dest_reg: str) -> str | None:
    if dest_reg.startswith("xmm"):
        return _sse_token_for_mnemonic(mnemonic)
    if mnemonic.startswith("lea"):
        return None
    if not mnemonic.startswith("mov"):
        return None
    return _integer_token_for_reg(dest_reg)


def _att_dest_reg(operands: str) -> str | None:
    parts = _split_ir_params(operands)
    if not parts:
        return None
    dest = parts[-1].strip()
    if dest.startswith("*"):
        dest = dest[1:].strip()
    if dest.startswith("%"):
        return dest[1:].lower()
    return None


def _att_src_reg(operands: str) -> str | None:
    parts = _split_ir_params(operands)
    if len(parts) < 2:
        return None
    src = parts[0].strip()
    if src.startswith("%"):
        return src[1:].lower()
    return None


def _gcc_call_regions(body: list[str], index: int) -> tuple[list[str], list[str]]:
    pre: list[str] = []
    between: list[str] = []
    phase = "pre"
    for line in body:
        stripped = line.strip()
        is_call = bool(re.search(r"\bcall\b", stripped))
        if phase == "pre":
            if is_call and f"vyx_ret{index}" in stripped:
                phase = "between"
                continue
            pre.append(line)
        else:
            if is_call and f"vyx_arg{index}" in stripped:
                break
            between.append(line)
    return pre, between


def _gcc_collect_arg_shape(lines: list[str]) -> dict[str, Any]:
    """Copy GCC's observed argument-register writes.  No sizeof fill-in."""
    memory = False
    for line in lines:
        parsed = _att_instruction(line)
        if parsed is None:
            continue
        mnemonic, operands = parsed
        joined = f"{mnemonic} {operands}"
        if "movs" in joined and (
            "rep" in joined or mnemonic.startswith("rep")
        ):
            memory = True
            continue
        dest = _att_dest_reg(operands)
        if dest is None:
            continue
        mapped = _SYSV_ARG_REGS.get(dest)
        if mapped is None:
            continue
        if mnemonic.startswith("lea"):
            memory = True
            continue
    if memory:
        return {"memory": True, "byval": False, "llvm_args": []}
    # g++ -O0 shuffles rax/rdx through rcx/rbx before the last rdi/rsi
    # stores. First-write-wins would count those temps as extra eightbytes.
    last: dict[str, str] = {}
    for line in reversed(lines):
        parsed = _att_instruction(line)
        if parsed is None:
            continue
        mnemonic, operands = parsed
        dest = _att_dest_reg(operands)
        if dest is None:
            break
        mapped = _SYSV_ARG_REGS.get(dest)
        if mapped is None:
            break
        if mnemonic.startswith("lea"):
            break
        token = _token_from_move(mnemonic, dest)
        if token is None:
            break
        _kind, canonical = mapped
        if canonical not in last:
            last[canonical] = token
    tokens = [last[reg] for reg in _SYSV_ARG_ORDER if reg in last]
    return {"memory": False, "byval": False, "llvm_args": tokens}


def _gcc_collect_return_shape(pre: list[str], between: list[str]) -> dict[str, Any]:
    for line in pre:
        parsed = _att_instruction(line)
        if parsed is None:
            continue
        mnemonic, operands = parsed
        dest = _att_dest_reg(operands)
        if mnemonic.startswith("lea") and dest in {"rdi", "edi"}:
            return {"ret_sret": True, "ret_llvm": "void"}
    tokens: list[str] = []
    seen: set[str] = set()
    for line in between:
        parsed = _att_instruction(line)
        if parsed is None:
            continue
        mnemonic, operands = parsed
        src = _att_src_reg(operands)
        if src is None or not mnemonic.startswith("mov"):
            continue
        if src.startswith("xmm"):
            token = _sse_token_for_mnemonic(mnemonic)
            key = src
        elif src in _SYSV_RET_REGS:
            token = _SYSV_RET_REGS[src]
            key = "rax" if src in {"rax", "eax", "ax", "al"} else (
                "rdx" if src in {"rdx", "edx"} else src
            )
        else:
            continue
        if key in seen:
            continue
        seen.add(key)
        tokens.append(token)
    if not tokens:
        return {"ret_sret": False, "ret_llvm": None}
    if len(tokens) == 1:
        return {"ret_sret": False, "ret_llvm": tokens[0]}
    return {"ret_sret": False, "ret_llvm": "{" + ",".join(tokens) + "}"}


def _gcc_asm_argument_classes(asm_text: str, index: int) -> dict[str, Any] | None:
    """GCC CodeGen for the probe, taken from the compiler's own ``-S`` output.

    Tokens are the registers GCC actually wrote.  This does not reconstruct
    INTEGER eightbytes from ``sizeof``, and it does not walk C++ fields.
    """
    body = _gcc_asm_function_body(asm_text, index)
    if body is None:
        return None
    pre, between = _gcc_call_regions(body, index)
    args = _gcc_collect_arg_shape(between)
    returned = _gcc_collect_return_shape(pre, between)
    llvm_args = list(args.get("llvm_args") or [])
    memory = bool(args.get("memory"))
    ret_sret = bool(returned.get("ret_sret"))
    ret_llvm = returned.get("ret_llvm")
    if memory:
        return {
            "memory": True,
            "byval": False,
            "llvm_args": [],
            "classes": [],
            "ret_sret": ret_sret,
            "ret_llvm": "void" if ret_sret else (ret_llvm or "void"),
        }
    if not llvm_args:
        return None
    if not ret_sret and not ret_llvm:
        ret_llvm = llvm_args[0] if len(llvm_args) == 1 else "{" + ",".join(llvm_args) + "}"
    classes = [_llvm_token_sysv_class(token) for token in llvm_args]
    return {
        "memory": False,
        "byval": False,
        "llvm_args": llvm_args,
        "classes": classes,
        "ret_sret": ret_sret,
        "ret_llvm": ret_llvm,
    }


def _msvc_asm_function_body(asm_text: str, index: int) -> list[str] | None:
    lines = asm_text.splitlines()
    start = re.compile(rf"^vyx_call{index}\s+PROC\b", re.I)
    end = re.compile(rf"^vyx_call{index}\s+ENDP\b", re.I)
    body: list[str] = []
    inside = False
    for line in lines:
        stripped = line.strip()
        if start.match(stripped):
            inside = True
            continue
        if inside:
            if end.match(stripped):
                break
            body.append(line)
    return body or None


def _msvc_call_regions(body: list[str], index: int) -> tuple[list[str], list[str]]:
    pre: list[str] = []
    between: list[str] = []
    phase = "pre"
    for line in body:
        stripped = line.strip()
        is_call = bool(re.search(r"\bcall\b", stripped, re.I))
        if phase == "pre":
            if is_call and f"vyx_ret{index}" in stripped:
                phase = "between"
                continue
            pre.append(line)
        else:
            if is_call and f"vyx_arg{index}" in stripped:
                break
            between.append(line)
    return pre, between


def _msvc_collect_arg_shape(lines: list[str]) -> dict[str, Any]:
    tokens: list[str] = []
    seen: set[str] = set()
    memory = False
    for line in lines:
        parsed = _intel_instruction(line)
        if parsed is None:
            continue
        mnemonic, operands = parsed
        if not operands:
            continue
        dest = operands[0].strip().lower()
        mapped = _WIN64_ARG_REGS.get(dest)
        if mapped is None:
            continue
        _kind, canonical = mapped
        if mnemonic.startswith("lea"):
            memory = True
            continue
        token = _token_from_move(mnemonic, dest)
        if token is None or canonical in seen:
            continue
        seen.add(canonical)
        tokens.append(token)
    if memory:
        return {"memory": True, "byval": False, "llvm_args": []}
    return {"memory": False, "byval": False, "llvm_args": tokens}


def _msvc_collect_return_shape(pre: list[str], between: list[str]) -> dict[str, Any]:
    for line in pre:
        parsed = _intel_instruction(line)
        if parsed is None:
            continue
        mnemonic, operands = parsed
        if mnemonic.startswith("lea") and operands and operands[0].strip().lower() in {"rcx", "ecx"}:
            return {"ret_sret": True, "ret_llvm": "void"}
    tokens: list[str] = []
    seen: set[str] = set()
    for line in between:
        parsed = _intel_instruction(line)
        if parsed is None or len(parsed[1]) < 2:
            continue
        mnemonic, operands = parsed
        src = operands[1].strip().lower()
        if not mnemonic.startswith("mov"):
            continue
        if src.startswith("xmm"):
            token = _sse_token_for_mnemonic(mnemonic)
            key = src
        elif src in {"rax", "eax"}:
            token = "i64" if src == "rax" else "i32"
            key = "rax"
        elif src in {"rdx", "edx"}:
            token = "i64" if src == "rdx" else "i32"
            key = "rdx"
        else:
            continue
        if key in seen:
            continue
        seen.add(key)
        tokens.append(token)
    if not tokens:
        return {"ret_sret": False, "ret_llvm": None}
    if len(tokens) == 1:
        return {"ret_sret": False, "ret_llvm": tokens[0]}
    return {"ret_sret": False, "ret_llvm": "{" + ",".join(tokens) + "}"}


def _msvc_asm_argument_classes(asm_text: str, index: int) -> dict[str, Any] | None:
    """MSVC CodeGen for the probe, taken from the compiler's own ``/FA`` listing."""
    body = _msvc_asm_function_body(asm_text, index)
    if body is None:
        return None
    pre, between = _msvc_call_regions(body, index)
    args = _msvc_collect_arg_shape(between)
    returned = _msvc_collect_return_shape(pre, between)
    llvm_args = list(args.get("llvm_args") or [])
    memory = bool(args.get("memory"))
    ret_sret = bool(returned.get("ret_sret"))
    ret_llvm = returned.get("ret_llvm")
    if memory:
        return {
            "memory": True,
            "byval": False,
            "llvm_args": [],
            "classes": [],
            "ret_sret": ret_sret,
            "ret_llvm": "void" if ret_sret else (ret_llvm or "void"),
        }
    if not llvm_args:
        return None
    if not ret_sret and not ret_llvm:
        ret_llvm = llvm_args[0] if len(llvm_args) == 1 else "{" + ",".join(llvm_args) + "}"
    classes = [_llvm_token_sysv_class(token) for token in llvm_args]
    return {
        "memory": False,
        "byval": False,
        "llvm_args": llvm_args,
        "classes": classes,
        "ret_sret": ret_sret,
        "ret_llvm": ret_llvm,
    }


def probe_itanium_aggregate_signatures(
    toolchain: cpp_toolchains.CppToolchain,
    std: str,
    target: str,
    headers: list[str],
    type_names: list[str],
    compiler_args: list[str],
) -> dict[str, dict[str, Any]]:
    """Record the selected compiler's CodeGen for each by-value aggregate.

    Clang: ``-S -emit-llvm`` parameter/return types (Clang CodeGen / ABIArgInfo).
    GCC: ``-S`` of the same probe (GCC's own backend register writes).
    MSVC: ``/FA`` of the same probe (MSVC's own x64 argument setup).
    The Descriptor copies those machine types; it does not reconstruct them
    from C++ fields, SysV class nicknames, or ``sizeof``.
    """
    unique: list[str] = []
    seen: set[str] = set()
    for name in type_names:
        canonical = canonical_scoped_record_name(name)
        if canonical and canonical not in seen:
            seen.add(canonical)
            unique.append(canonical)
    if not unique:
        return {}
    include_lines = [f'#include "{header}"' for header in headers]
    with tempfile.TemporaryDirectory(prefix="vyx_dci_abi_") as temp_dir:
        # 1) Traits (selected compiler is the authority for the value contract).
        traits_probe = Path(temp_dir) / "dci_abi_traits.cpp"
        lines = list(include_lines)
        lines.append("#include <type_traits>")
        lines.append("namespace { template<int,int,int,int> struct __vyx_tc; }")
        for index, name in enumerate(unique):
            lines.append(
                f"__vyx_tc<{index}, std::is_trivially_copyable<{name}>::value, "
                f"std::is_standard_layout<{name}>::value, "
                f"std::is_trivially_destructible<{name}>::value> __vyx_tv{index};"
            )
        traits_probe.write_text("\n".join(lines) + "\n", encoding="utf-8")
        traits_output = _run_diagnostic_type_probe(
            toolchain, std, traits_probe, compiler_args, target
        )
        traits: dict[int, tuple[bool, bool, bool]] = {}
        for match in RE_AGG_TRAITS_PROBE.finditer(traits_output):
            traits[int(match.group(1))] = (
                bool(int(match.group(2))),
                bool(int(match.group(3))),
                bool(int(match.group(4))),
            )
        # 2) Calling-convention classification via the selected compiler's codegen.
        sig_probe = Path(temp_dir) / "dci_abi_sig.cpp"
        lines = list(include_lines)
        for index, name in enumerate(unique):
            lines.append(f"using vyx_t{index} = {name};")
            lines.append(f'extern "C" vyx_t{index} vyx_ret{index}();')
            lines.append(f'extern "C" void vyx_arg{index}(vyx_t{index});')
            lines.append(
                f'extern "C" void vyx_call{index}() {{ vyx_arg{index}(vyx_ret{index}()); }}'
            )
        sig_probe.write_text("\n".join(lines) + "\n", encoding="utf-8")
        codegen = _run_codegen_probe(
            toolchain, std, sig_probe, compiler_args, target
        )
    result: dict[str, dict[str, Any]] = {}
    for index, name in enumerate(unique):
        trivially_copyable, standard_layout, trivially_destructible = traits.get(
            index, (False, False, False)
        )
        classification: dict[str, Any] | None = None
        if codegen is not None:
            if toolchain.family == "clang":
                classification = _clang_ir_argument_classes(codegen, index)
            elif toolchain.family == "gcc":
                classification = _gcc_asm_argument_classes(codegen, index)
            elif toolchain.family == "msvc":
                classification = _msvc_asm_argument_classes(codegen, index)
        entry: dict[str, Any] = {
            "trivially_copyable": trivially_copyable,
            "standard_layout": standard_layout,
            "trivially_destructible": trivially_destructible,
        }
        if classification is None:
            entry["classes"] = None
            entry["memory"] = False
            entry["byval"] = False
            entry["llvm_args"] = None
            entry["ret_sret"] = False
            entry["ret_llvm"] = None
        else:
            entry["classes"] = classification.get("classes")
            entry["memory"] = bool(classification.get("memory"))
            entry["byval"] = bool(classification.get("byval"))
            entry["inalloca"] = bool(classification.get("inalloca"))
            entry["llvm_args"] = classification.get("llvm_args")
            entry["ret_sret"] = bool(classification.get("ret_sret"))
            entry["ret_llvm"] = classification.get("ret_llvm")
        result[name] = entry
    return result


def collect_by_value_aggregate_names(
    symbols: list[Symbol], records: list[RecordLayout]
) -> list[str]:
    """Canonical names of class aggregates passed/returned by value in symbols.

    These are the only types whose calling convention the Itanium
    machine-signature probe must classify; scalars, references and pointers
    already have a verified lowering.
    """
    names: list[str] = []
    seen: set[str] = set()

    def consider(type_info: Any) -> None:
        if not isinstance(type_info, dict):
            return
        if type_info.get("kind") != "class":
            return
        if (type_info.get("reference") or "value") != "value":
            return
        record = record_for_dci_type(type_info, records)
        if record is None:
            return
        canonical = canonical_scoped_record_name(record.type_name)
        if canonical and canonical not in seen:
            seen.add(canonical)
            names.append(canonical)

    for symbol in symbols:
        for parameter in symbol.params:
            consider(parameter.get("type"))
        if symbol.ret is not None:
            consider(symbol.ret.get("type"))
    return names


def _llvm_token_byte_size(token: str, pointer_width: int) -> int | None:
    if token == "ptr":
        return max(1, pointer_width // 8)
    match = re.fullmatch(r"i(\d+)", token)
    if match:
        return (int(match.group(1)) + 7) // 8
    return {"half": 2, "float": 4, "double": 8, "fp128": 16, "x86_fp80": 16}.get(token)


def _coerce_to_from_llvm_token(token: str) -> dict[str, Any]:
    if token == "ptr":
        return {"name": "ptr", "kind": "primitive", "reference": "value"}
    if token == "double":
        return {"name": "f64", "kind": "primitive", "reference": "value"}
    if token == "float":
        return {"name": "f32", "kind": "primitive", "reference": "value"}
    if token == "half":
        return {"name": "f16", "kind": "primitive", "reference": "value"}
    match = re.fullmatch(r"i(\d+)", token)
    if match:
        bits = int(match.group(1))
        if bits in {8, 16, 32, 64, 128}:
            return {"name": f"i{bits}", "kind": "primitive", "reference": "value"}
    raise AdapterContractError(
        f"unsupported compiler LLVM type token {token!r}"
    )


def _register_offsets_from_tokens(tokens: list[str], pointer_width: int) -> list[int]:
    offsets: list[int] = []
    cursor = 0
    for token in tokens:
        size = _llvm_token_byte_size(token, pointer_width)
        if size is None:
            raise AdapterContractError(
                f"unsupported compiler LLVM type token {token!r}"
            )
        offsets.append(cursor)
        cursor += size
    return offsets


def _dcib_from_llvm_pieces(
    pieces: list[str],
    record: RecordLayout,
    *,
    split: bool,
    pointer_width: int,
) -> dict[str, Any]:
    """Translate compiler LLVM pieces into the portable DCIB lowering."""
    result: dict[str, Any] = {
        "type": None,
        "size": record.size,
        "alignment": record.alignment,
    }
    if not pieces:
        raise AdapterContractError(
            f"Itanium by-value aggregate {record.type_name!r} has no "
            "verified machine-signature lowering"
        )
    if split:
        if any(piece.startswith("{") for piece in pieces):
            raise AdapterContractError(
                f"Itanium by-value aggregate {record.type_name!r} has no "
                "verified machine-signature lowering"
            )
        result["passing"] = "split"
        result["registers"] = list(pieces)
        result["register_offsets"] = _register_offsets_from_tokens(pieces, pointer_width)
        return result
    if len(pieces) == 1:
        result["passing"] = "coerce"
        result["coerce_to"] = _coerce_to_from_llvm_token(pieces[0])
        return result
    result["passing"] = "coerce"
    result["registers"] = list(pieces)
    result["register_offsets"] = _register_offsets_from_tokens(pieces, pointer_width)
    return result


def _aggregate_lowered_to_bare_pointer(signature: dict[str, Any]) -> bool:
    """True when the compiler lowered a by-value aggregate to a bare ``ptr``.

    Clang spells this ``ptr dead_on_return``: the caller hands over storage and
    the callee must not keep it -- exactly the ``indirect`` (borrowed pointer)
    contract.  Classifying it as a register lowering instead yields
    ``coerce -> ptr``, but ``coerce`` requires its carrier to be the same width
    as the storage, so the descriptor would be rejected with
    ``DCI coerce size mismatch`` (the shape ``dci_rust_generic/run.ps1`` pins
    down as invalid via its ``coerce_parameter_width`` variant).
    """
    llvm_args = signature.get("llvm_args")
    if not isinstance(llvm_args, list) or len(llvm_args) != 1:
        return False
    return str(llvm_args[0]) == "ptr"


def itanium_aggregate_lowering(
    record: RecordLayout,
    position: str,
    signature: dict[str, Any] | None,
    pointer_width: int = 64,
) -> dict[str, Any]:
    """Copy the selected compiler's CodeGen into a portable DCIB lowering.

    ``signature`` must carry the compiler's own LLVM argument/return types
    (Clang ``-emit-llvm`` or GCC ``-S``).  This function does not reconstruct
    those types from SysV class nicknames, record fields, or ``u128``.
    """
    reason = (
        f"Itanium by-value aggregate {record.type_name!r} has no "
        "verified machine-signature lowering"
    )
    if signature is None:
        raise AdapterContractError(reason)
    if not signature.get("trivially_copyable"):
        raise AdapterContractError(reason)
    size = record.size
    base: dict[str, Any] = {
        "type": None,
        "size": size,
        "alignment": record.alignment,
    }
    if position == "return":
        if signature.get("ret_sret") or (
            signature.get("memory") and not signature.get("ret_llvm")
        ):
            return {**base, "passing": "sret"}
        llvm_type = signature.get("ret_llvm")
        if not isinstance(llvm_type, str) or llvm_type in {"", "void"}:
            raise AdapterContractError(reason)
        pieces = _llvm_type_pieces(llvm_type)
        if not pieces:
            raise AdapterContractError(reason)
        return _dcib_from_llvm_pieces(
            pieces, record, split=False, pointer_width=pointer_width
        )
    if signature.get("memory") or _aggregate_lowered_to_bare_pointer(signature):
        if signature.get("inalloca"):
            passing = "inalloca"
            result = {**base, "passing": passing, "attributes": ["inalloca"]}
            return result
        passing = "byval" if signature.get("byval") else "indirect"
        result = {**base, "passing": passing}
        if passing == "byval":
            result["attributes"] = ["byval"]
        return result
    llvm_args = signature.get("llvm_args")
    if not isinstance(llvm_args, list) or not llvm_args:
        raise AdapterContractError(reason)
    if len(llvm_args) == 1:
        pieces = _llvm_type_pieces(str(llvm_args[0]))
        if not pieces:
            raise AdapterContractError(reason)
        return _dcib_from_llvm_pieces(
            pieces, record, split=False, pointer_width=pointer_width
        )
    tokens: list[str] = []
    for arg in llvm_args:
        pieces = _llvm_type_pieces(str(arg))
        if len(pieces) != 1:
            raise AdapterContractError(reason)
        tokens.append(pieces[0])
    return _dcib_from_llvm_pieces(
        tokens, record, split=True, pointer_width=pointer_width
    )


ADAPTER_JOBS: int | None = None


def adapter_job_count() -> int:
    """How many Clang fact-extractor processes to run at once."""
    if ADAPTER_JOBS is not None:
        return max(1, ADAPTER_JOBS)
    raw = os.environ.get("DCI_ADAPTER_JOBS", "").strip()
    if raw:
        try:
            return max(1, int(raw))
        except ValueError:
            pass
    return max(1, min(32, os.cpu_count() or 4))


def clang_ast_dump_filter_command(
    clang: str,
    std: str,
    target: str,
    probe: Path,
    extra: list[str],
    name: str,
) -> list[str]:
    return [
        clang,
        f"-std={std}",
        "-target",
        target,
        "-Xclang",
        "-ast-dump=json",
        "-Xclang",
        f"-ast-dump-filter={name}",
        "-fsyntax-only",
        str(probe),
        *extra,
    ]


def clang_ast_dump_filter_texts(
    clang: str,
    std: str,
    target: str,
    probe: Path,
    extra: list[str],
    filters: list[str],
) -> Iterator[str]:
    """Run one Clang AST dump per filter, concurrently.

    Each filter still gets its own process (Clang accepts one
    ``-ast-dump-filter``). Workers stay a sliding window of ``jobs`` so
    dump JSON is not retained for the whole filter list. Yields stay in
    ``filters`` order.
    """
    if not filters:
        return
    commands = [
        clang_ast_dump_filter_command(clang, std, target, probe, extra, name)
        for name in filters
    ]
    jobs = min(adapter_job_count(), len(commands))
    total = len(commands)
    print(
        f"dci-cpp: ast-dump filters={total} jobs={jobs}",
        file=sys.stderr,
        flush=True,
    )

    def report(done: int) -> None:
        if done == total or done % 50 == 0:
            print(f"dci-cpp: ast-dump {done}/{total}", file=sys.stderr, flush=True)

    if jobs == 1:
        done = 0
        for command in commands:
            yield run(command)
            done += 1
            report(done)
        return
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        pending: deque = deque()
        cmd_iter = iter(commands)
        for _ in range(jobs):
            command = next(cmd_iter, None)
            if command is None:
                break
            pending.append(pool.submit(run, command))
        done = 0
        while pending:
            yield pending.popleft().result()
            done += 1
            report(done)
            command = next(cmd_iter, None)
            if command is not None:
                pending.append(pool.submit(run, command))


def emit_ast_scan(
    clang: str,
    std: str,
    target: str,
    probe: Path,
    extra: list[str],
    records: list[str],
    frees: list[str],
    instance_records: list[str] | None = None,
) -> tuple[list[Symbol], dict[str, dict[str, Any]]]:
    """Extract linkable symbols (and AST record traits) from the probe dump.

    ``instance_records`` are concrete template instances the header explicitly
    instantiates (``Pair2<double, int>``).  They are added to the AST-dump
    filter and to the owner-resolution set so the methods of the specialization
    become ``kind="method"`` symbols -- the DCI_SPEC §12 requirement that a
    selected method of a concrete instance export its final symbol and ABI.
    They are deliberately *not* passed to ``ast_record_contracts``: instance
    layouts stay whatever the project declared and the gate re-measured.
    """
    instance_names = list(instance_records or [])
    symbols: list[Symbol] = []
    contracts: dict[str, dict[str, Any]] = {}
    filters: list[str] = []
    for name in records + frees + instance_names:
        if name not in filters:
            filters.append(name)
        short = last_cpp_component(name)
        if short and short not in filters:
            filters.append(short)
        short_no_template = last_cpp_component_without_template_args(name)
        if short_no_template and short_no_template not in filters:
            filters.append(short_no_template)
    for text in clang_ast_dump_filter_texts(
        clang, std, target, probe, extra, filters
    ):
        nodes = json_stream(text)
        # Alias RecordType IDs and specialization IDs come from the SAME
        # Clang invocation. This resolves omitted default template arguments
        # and preserves namespace identity without guessing a short name.
        instance_ids: dict[str, str] = {}
        for node in walk(nodes):
            if node.get("kind") != "TypeAliasDecl" or not str(node.get("name", "")).startswith("vyx_dci_instance_"):
                continue
            spelling = canonical_cpp_name((node.get("type") or {}).get("qualType", ""))
            exact = [name for name in instance_names if canonical_cpp_name(name) == spelling]
            if len(exact) == 1:
                for child in walk(node):
                    if child.get("kind") == "RecordType" and (child.get("decl") or {}).get("id"):
                        instance_ids[child["decl"]["id"]] = exact[0]
        for node in walk(nodes):
            if node.get("kind") == "ClassTemplateDecl" and any(last_cpp_component_without_template_args(name) == node.get("name") for name in instance_names):
                for child in node.get("inner", []):
                    if child.get("kind") == "CXXRecordDecl":
                        child["dci_instance_unmapped"] = True
            if node.get("kind") == "ClassTemplateSpecializationDecl":
                if node.get("id") in instance_ids:
                    node["dci_verified_owner"] = instance_ids[node["id"]]
                elif any(last_cpp_component_without_template_args(name) == node.get("name") for name in instance_names):
                    node["dci_instance_unmapped"] = True
        # Adopt the fact extractor's alias -> record desugarings before building
        # symbols so a spelled alias (``string_view_t``) resolves to the record
        # whose layout the same dump provides.
        harvest_ast_type_aliases(nodes)
        symbols.extend(ast_symbols(nodes, set(records) | set(instance_names), set(frees)))
        contracts.update(ast_record_contracts(nodes, set(records)))
    dedup: list[Symbol] = []
    seen: set[tuple[str, str, str, str, str]] = set()
    for sym in symbols:
        key = symbol_identity(sym)
        if key not in seen:
            seen.add(key)
            dedup.append(sym)
    return dedup, contracts


def emit_ast_record_contracts(
    clang: str,
    std: str,
    target: str,
    probe: Path,
    extra: list[str],
    record_names: list[str],
) -> dict[str, dict[str, Any]]:
    """AST ``definitionData`` traits + lifecycle for records, emitting no symbols.

    The nested layout-closure records (``fmt::basic_string_view<char>``, ...)
    need their trait/lifecycle facts so a by-value parameter of that type has an
    executable copy/move contract and a verified stable-value layout, but they
    must not widen the exported *symbol* surface with std/fmt internals.  This
    reuses the fact extractor's ``-ast-dump-filter`` per record name and keeps
    only ``ast_record_contracts`` (never ``ast_symbols``).
    """
    filters: list[str] = []
    for name in record_names:
        for candidate in (
            name,
            last_cpp_component(name),
            last_cpp_component_without_template_args(name),
        ):
            if candidate and candidate not in filters:
                filters.append(candidate)
    names_set = set(record_names)
    contracts: dict[str, dict[str, Any]] = {}
    for text in clang_ast_dump_filter_texts(
        clang, std, target, probe, extra, filters
    ):
        nodes = json_stream(text)
        for owner, contract in ast_record_contracts(nodes, names_set).items():
            contracts.setdefault(owner, contract)
    return contracts


def emit_ast_symbol_scan(
    clang: str,
    std: str,
    target: str,
    probe: Path,
    extra: list[str],
    records: list[str],
    frees: list[str],
) -> list[Symbol]:
    """Compatibility wrapper for callers that only need linkable symbols."""
    return emit_ast_scan(clang, std, target, probe, extra, records, frees)[0]


def sanitize_probe_name(name: str) -> str:
    return re.sub(r"[^A-Za-z0-9_]", "_", name)


def destructor_probe_owners(symbols: list[Symbol]) -> list[str]:
    """Owners whose destructor can be invoked from a file-scope probe.

    Protected/private destructors (Qt ``QIODeviceBase``, ``QStandardPaths``,
    ...) cannot be called as ``p->T::~T()`` from the probe TU. Those symbols
    keep the AST mangled name; only public destructors are re-resolved from IR.
    """
    unique: list[str] = []
    seen: set[str] = set()
    for symbol in symbols:
        if symbol.kind != "destructor":
            continue
        if (symbol.visibility or "public") != "public":
            continue
        owner = symbol.owner
        if not owner or owner in seen:
            continue
        seen.add(owner)
        unique.append(owner)
    return unique


def direct_destructor_symbols(clang: str, std: str, target: str, includes: list[str], extra: list[str], owners: list[str]) -> dict[str, str]:
    unique: list[str] = []
    seen: set[str] = set()
    for owner in owners:
        if owner and owner not in seen:
            seen.add(owner)
            unique.append(owner)
    if not unique:
        return {}
    include_text = "".join(f'#include "{h}"\n' for h in includes)
    include_lines = include_text.count("\n")
    remaining = list(unique)
    ir = ""
    with tempfile.TemporaryDirectory(prefix="vyx_dci_dtor_") as td:
        probe = Path(td) / "dci_dtor_probe.cpp"
        probe_error = re.compile(
            rf"(?:{re.escape(str(probe))}|{re.escape(probe.name)}):(\d+):\d+: error:",
            re.IGNORECASE,
        )
        for _ in range(16):
            if not remaining:
                return {}
            text = include_text
            index_owner: dict[str, str] = {}
            for idx, owner in enumerate(remaining):
                cpp_owner = owner.replace(".", "::")
                fn_name = f"__dci_probe_dtor_{idx}_{sanitize_probe_name(owner)}"
                index_owner[fn_name] = owner
                dtor_name = destructor_member_name_for_owner(cpp_owner)
                text += (
                    f'extern "C" void {fn_name}({cpp_owner}* p) {{ '
                    f"p->{cpp_owner}::{dtor_name}(); }}\n"
                )
            probe.write_text(text, encoding="utf-8")
            proc = subprocess.run(
                [
                    clang,
                    f"-std={std}",
                    "-target",
                    target,
                    "-S",
                    "-emit-llvm",
                    str(probe),
                    "-o",
                    "-",
                    *extra,
                ],
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                errors="replace",
            )
            if proc.returncode == 0:
                ir = (proc.stdout or "") + "\n" + (proc.stderr or "")
                break
            detail = ((proc.stdout or "") + "\n" + (proc.stderr or "")).strip()
            bad: set[str] = set()
            for match in probe_error.finditer(detail):
                lineno = int(match.group(1))
                index = lineno - include_lines - 1
                if 0 <= index < len(remaining):
                    bad.add(remaining[index])
            if not bad:
                raise AdapterContractError(
                    "C++ fact extraction command failed with exit code "
                    f"{proc.returncode}"
                    + (f":\n{detail}" if detail else "")
                )
            remaining = [name for name in remaining if name not in bad]
        else:
            raise AdapterContractError("could not stabilize destructor IR probe")
    out: dict[str, str] = {}
    for fn_name, owner in index_owner.items():
        m = re.search(
            r"define\b[^{@]*@" + re.escape(fn_name) + r"\b.*?\{(?P<body>.*?)\n\}",
            ir,
            re.S,
        )
        if not m:
            continue
        call = re.search(
            r"\bcall\b[^\n@]*@(?:\"(?P<quoted>[^\"]+)\"|(?P<plain>[-A-Za-z$._0-9]+))",
            m.group("body"),
        )
        if call:
            out[owner] = call.group("quoted") or call.group("plain") or ""
    return out


def patch_direct_destructor_symbols(symbols: list[Symbol], dtor_symbols: dict[str, str]) -> None:
    if not dtor_symbols:
        return
    for sym in symbols:
        if sym.kind == "destructor":
            direct = dtor_symbols.get(sym.owner)
            if direct:
                sym.mangled = direct


def append_vtable_force_uses(probe: Path, symbols: list[Symbol]) -> None:
    """Force Clang to materialize vtable layouts without defining host code.

    Merely applying ``sizeof`` to a polymorphic declaration does not make the
    Itanium code generator calculate its vtable.  Taking an unambiguous
    virtual member address does.  ``delete`` is not used: Qt and similar
    libraries give many polymorphic types a protected/private destructor
    (``QIODeviceBase``, ``QStandardPaths``), so ``delete`` fails closed on
    a complete public TU.  ``typeid(*p)`` still requires a vtable and does
    not invoke the destructor.
    """
    by_owner_name: dict[tuple[str, str], list[Symbol]] = {}
    by_owner: dict[str, list[Symbol]] = {}
    for symbol in symbols:
        if not symbol.owner:
            continue
        by_owner_name.setdefault((symbol.owner, symbol.member_name), []).append(symbol)
        if symbol.is_virtual:
            by_owner.setdefault(symbol.owner, []).append(symbol)

    lines: list[str] = []
    for index, (owner, candidates) in enumerate(sorted(by_owner.items())):
        cpp_owner = owner.replace(".", "::")
        selected = next(
            (
                symbol
                for symbol in candidates
                if symbol.is_virtual
                and symbol.kind == "method"
                and len(by_owner_name.get((owner, symbol.member_name), [])) == 1
            ),
            None,
        )
        if selected is not None:
            lines.append(
                f'[[maybe_unused]] auto __dci_vtable_force_{index} = '
                f'&{cpp_owner}::{selected.member_name};'
            )
            continue
        lines.append(
            f'extern "C" void __dci_vtable_force_{index}({cpp_owner}* value) '
            "{ (void)typeid(*value); }"
        )
    if lines:
        with probe.open("a", encoding="utf-8") as stream:
            stream.write("\n#include <typeinfo>\n")
            stream.write("// DCI vtable fact probes.\n")
            stream.write("\n".join(lines))
            stream.write("\n")


def virtual_base_adjustment_from_ir(
    body: str, fallback_complete_object_offset: int
) -> dict[str, Any] | None:
    vbptr_match = re.search(
        r"getelementptr(?: inbounds)? i8, ptr [^,]+, i(?:32|64) (-?\d+)", body
    )
    entry_match = re.search(
        r"getelementptr(?: inbounds)? i32, ptr [^,]+, i32 (-?\d+)", body
    )
    if vbptr_match is not None and entry_match is not None:
        vbptr_offset = int(vbptr_match.group(1))
        entry_index = int(entry_match.group(1))
        return {
            "kind": "table",
            "table_offset": entry_index * 4,
            "table_pointer_offset": vbptr_offset,
            "table_entry_offset": entry_index * 4,
            "table_entry_size": 4,
            "table_entry_signed": True,
            "displacement_base_offset": vbptr_offset,
            "result": "object_plus_base_plus_displacement",
            "null_preserving": True,
            "fallback_complete_object_offset": fallback_complete_object_offset,
        }

    # A virtual base is not necessarily dynamically adjusted when the probe's
    # static source type fixes the complete-object layout (for example, a
    # final most-derived type).  Accept a constant only when Clang emitted the
    # same offset as the independently observed record layout.  In particular,
    # never promote fallback_complete_object_offset into an executable fact.
    for constant_match in re.finditer(
        r"(?m)^\s*(?P<result>%[-A-Za-z$._0-9]+)\s*=\s*"
        r"getelementptr(?:\s+(?:inbounds|nuw|nsw))*\s+i8, ptr [^,]+, "
        r"i(?:32|64) (?P<offset>-?\d+)",
        body,
    ):
        offset = int(constant_match.group("offset"))
        if offset != fallback_complete_object_offset:
            continue
        result = re.escape(constant_match.group("result"))
        # This is the first step of a vbtable lookup, not the result of a
        # constant base conversion.  Keep it unavailable unless the complete
        # table contract above was recovered.
        if re.search(r"\bload\s+ptr,\s+ptr\s+" + result + r"(?:\s|,|$)", body):
            continue
        return {
            "kind": "constant",
            "offset": offset,
            "null_preserving": True,
        }
    return None


def apply_virtual_base_adjustments(
    clang: str,
    std: str,
    target: str,
    includes: list[str],
    extra: list[str],
    records: list[RecordLayout],
) -> None:
    relations: list[tuple[str, RecordLayout, BaseLayout]] = []
    for record in records:
        for base in record.bases:
            if base.is_virtual and base.visibility == "public":
                fn_name = f"__dci_vbase_{len(relations)}_{sanitize_probe_name(record.type_name)}"
                relations.append((fn_name, record, base))
    if not relations:
        return
    with tempfile.TemporaryDirectory(prefix="vyx_dci_vbase_") as td:
        probe = Path(td) / "dci_vbase_probe.cpp"
        source = "".join(f'#include "{header}"\n' for header in includes)
        for fn_name, record, base in relations:
            derived_name = record.type_name.replace(".", "::")
            base_name = base.type_name.replace(".", "::")
            source += (
                f'extern "C" {base_name}* {fn_name}({derived_name}* value) '
                "{ return value; }\n"
            )
        probe.write_text(source, encoding="utf-8")
        ir = try_run(
            [
                clang,
                f"-std={std}",
                "-target",
                target,
                "-S",
                "-emit-llvm",
                "-O0",
                str(probe),
                "-o",
                "-",
                *extra,
            ]
        )
    if ir is None:
        return
    for fn_name, _record, base in relations:
        function_match = re.search(
            r"define\b[^@]*@" + re.escape(fn_name) + r"\([^)]*\)[^{]*\{(?P<body>.*?)\n\}",
            ir,
            re.S,
        )
        if function_match is None:
            continue
        body = function_match.group("body")
        adjustment = virtual_base_adjustment_from_ir(body, base.offset)
        if adjustment is not None:
            base.adjustment = adjustment


def names_match(lhs: str, rhs: str) -> bool:
    if not lhs or not rhs:
        return False
    lhs2 = canonical_cpp_name(lhs)
    rhs2 = canonical_cpp_name(rhs)
    if lhs2 == rhs2:
        return True
    return canonical_cpp_name(last_cpp_component(lhs2)) == canonical_cpp_name(last_cpp_component(rhs2))


def alias_for_target(target: str, aliases: list[TypeAlias]) -> list[TypeAlias]:
    return [alias for alias in aliases if names_match(alias.target, target)]


def first_alias_name_for_target(target: str, aliases: list[TypeAlias]) -> str:
    for alias in aliases:
        if names_match(alias.target, target):
            return alias.name
    return target


def copy_layout_for_aliases(layouts: list[RecordLayout], aliases: list[TypeAlias]) -> list[RecordLayout]:
    out = list(layouts)
    existing = {layout.type_name for layout in out}
    for alias in aliases:
        if alias.name in existing:
            continue
        source = next((layout for layout in layouts if names_match(layout.type_name, alias.target)), None)
        if source is None:
            continue
        out.append(
            RecordLayout(
                type_name=alias.name,
                size=source.size,
                alignment=source.alignment,
                fields=[
                    FieldLayout(
                        f.name,
                        f.cpp_type,
                        f.offset,
                        f.visibility,
                        f.bit_offset,
                        f.bit_width,
                        f.storage_size,
                        f.is_signed,
                    )
                    for f in source.fields
                ],
                bases=[
                    BaseLayout(
                        first_alias_name_for_target(b.type_name, aliases),
                        b.offset,
                        b.visibility,
                        b.is_virtual,
                        dict(b.adjustment),
                    )
                    for b in source.bases
                ],
                has_vtable=source.has_vtable,
                vbptr_offsets=list(source.vbptr_offsets),
                traits=dict(source.traits),
                lifecycle={key: dict(value) for key, value in source.lifecycle.items()},
            )
        )
        existing.add(alias.name)
    return out


def symbol_copy_for_alias(sym: Symbol, alias: TypeAlias) -> Symbol:
    member_name = sym.member_name
    if sym.kind == "destructor":
        member_name = "~" + alias.name.split("::")[-1]
    return Symbol(
        name=alias.name + "::" + member_name if alias.name else member_name,
        owner=alias.name,
        member_name=member_name,
        mangled=sym.mangled,
        kind=sym.kind,
        calling_convention=sym.calling_convention,
        params=sym.params,
        ret=sym.ret,
        is_static=sym.is_static,
        is_virtual=sym.is_virtual,
        is_override=sym.is_override,
        is_final=sym.is_final,
        is_const=sym.is_const,
        visibility=sym.visibility,
        this_adjust=sym.this_adjust,
        native_calling_convention=sym.native_calling_convention,
        unwind=sym.unwind,
        parameter_ownerships=dict(sym.parameter_ownerships),
        return_ownership=sym.return_ownership,
        semantic_id=sym.semantic_id,
        native_template=sym.native_template,
        list_arguments=sym.list_arguments,
        member_qualifiers=sym.member_qualifiers,
    )


def copy_symbols_for_aliases(symbols: list[Symbol], aliases: list[TypeAlias]) -> list[Symbol]:
    out = [symbol for symbol in symbols if not alias_for_target(symbol.owner, aliases)]
    seen = {symbol_identity(sym) for sym in out}
    for sym in symbols:
        if not sym.owner:
            continue
        for alias in alias_for_target(sym.owner, aliases):
            copied = symbol_copy_for_alias(sym, alias)
            key = symbol_identity(copied)
            if key not in seen:
                seen.add(key)
                out.append(copied)
    return out


def link_lifecycle_symbols(records: list[RecordLayout], symbols: list[Symbol]) -> None:
    def is_self_parameter(sym: Symbol, owner: str) -> bool:
        if len(sym.params) != 1:
            return False
        raw = sym.params[0]["type"].get("cpp_type") or ""
        value_type = re.sub(r"\b(?:const|volatile)\b", "", raw)
        value_type = collapse_spaces(value_type.replace("&&", "").replace("&", ""))
        return names_match(value_type, owner)

    for record in records:
        for operation, contract in record.lifecycle.items():
            if not contract.get("available") or contract.get("deleted"):
                continue
            contract["link_name"] = ""
            if not contract.get("trivial"):
                contract["execution"] = "stub_required"
            candidates = [sym for sym in symbols if names_match(sym.owner, record.type_name)]
            if operation == "default_construct":
                candidates = [sym for sym in candidates if sym.kind == "constructor" and not sym.params]
            elif operation in {"copy_construct", "move_construct"}:
                want_move = operation == "move_construct"
                candidates = [
                    sym
                    for sym in candidates
                    if sym.kind == "constructor"
                    and is_self_parameter(sym, record.type_name)
                    and ("&&" in (sym.params[0]["type"].get("cpp_type") or "")) == want_move
                ]
            elif operation in {"copy_assign", "move_assign"}:
                want_move = operation == "move_assign"
                candidates = [
                    sym
                    for sym in candidates
                    if sym.kind == "method"
                    and sym.member_name == "operator="
                    and is_self_parameter(sym, record.type_name)
                    and ("&&" in (sym.params[0]["type"].get("cpp_type") or "")) == want_move
                ]
            elif operation == "destroy":
                candidates = [sym for sym in candidates if sym.kind == "destructor"]
            if not candidates:
                continue
            contract["link_name"] = candidates[0].mangled
            if not contract.get("trivial"):
                contract["execution"] = "direct"


def vtable_copy_for_alias(vt: VTable, alias: TypeAlias, aliases: list[TypeAlias]) -> VTable:
    entries: list[VTableEntry] = []
    for entry in vt.entries:
        owner = alias.name if names_match(entry.owner, alias.target) else entry.owner
        member = entry.member_name
        if entry.kind == "destructor" and names_match(entry.owner, alias.target):
            member = "~" + alias.name.split("::")[-1]
        entries.append(
            VTableEntry(
                index=entry.index,
                offset=entry.offset,
                name=entry.name,
                kind=entry.kind,
                mangled=entry.mangled,
                owner=owner,
                member_name=member,
                this_adjust=entry.this_adjust,
                return_adjust=entry.return_adjust,
            )
        )
    base_class = alias.name if names_match(vt.base_class, alias.target) else first_alias_name_for_target(vt.base_class, aliases)
    return VTable(class_name=alias.name, base_class=base_class, entries=entries)


def copy_vtables_for_aliases(vtables: list[VTable], aliases: list[TypeAlias]) -> list[VTable]:
    out = list(vtables)
    existing = {(vt.class_name, vt.base_class) for vt in out}
    for vt in vtables:
        for alias in alias_for_target(vt.class_name, aliases):
            key = (alias.name, alias.name)
            if key in existing:
                continue
            existing.add(key)
            out.append(vtable_copy_for_alias(vt, alias, aliases))
    return out


def target_contract(target: str) -> dict[str, Any]:
    try:
        info = cpp_toolchains.target_info(target)
    except cpp_toolchains.CppToolchainError as error:
        raise AdapterContractError(str(error)) from error
    if not info.architecture or not info.platform or not info.abi_family:
        raise AdapterContractError(f"unsupported C++ target triple {info.triple!r}")
    return info.descriptor()


def record_for_dci_type(type_info: dict[str, Any], records: list[RecordLayout]) -> RecordLayout | None:
    name = type_info.get("name") or ""
    return next((record for record in records if names_match(record.type_name, name)), None)


def lifecycle_operation_is_executable(record: RecordLayout, operation: str) -> bool:
    contract = record.lifecycle.get(operation) or {}
    return bool(
        contract.get("available")
        and contract.get("accessible", True)
        and not contract.get("deleted")
        and (contract.get("trivial") or contract.get("link_name"))
    )


def cpp_borrow_is_const(type_info: dict[str, Any]) -> bool:
    if type_info.get("pointee_const") is True or type_info.get("reference") == "pointer_const":
        return True
    raw = collapse_spaces(str(type_info.get("cpp_type") or ""))
    if not raw:
        return False
    if "&&" in raw:
        raw = raw.split("&&", 1)[0]
    elif "&" in raw:
        raw = raw.split("&", 1)[0]
    elif "*" in raw:
        raw = raw.rsplit("*", 1)[0]
    return re.search(r"\bconst\b", raw) is not None


def parameter_ownership(
    type_info: dict[str, Any],
    records: list[RecordLayout],
    symbol_name: str,
    explicit: str = "",
) -> str:
    reference = type_info.get("reference") or "value"
    raw = str(type_info.get("cpp_type") or "")
    if type_info.get("kind") == "function" and reference in {
        "pointer",
        "reference",
    }:
        if explicit and explicit != "copy":
            raise AdapterContractError(
                f"{symbol_name}: function pointer/reference parameters use copy ownership"
            )
        return "copy"
    if explicit:
        if explicit in {"borrow", "borrow_mut", "share", "out"}:
            if reference == "value":
                raise AdapterContractError(
                    f"{symbol_name}: explicit {explicit} requires a pointer or reference parameter"
                )
            if explicit in {"borrow_mut", "out"} and cpp_borrow_is_const(type_info):
                raise AdapterContractError(
                    f"{symbol_name}: explicit {explicit} cannot target a const pointee"
                )
            return explicit
        if explicit == "copy":
            if reference != "value":
                raise AdapterContractError(
                    f"{symbol_name}: explicit copy requires a value parameter"
                )
            if type_info.get("kind") != "class":
                return explicit
            record = record_for_dci_type(type_info, records)
            if record is not None and lifecycle_operation_is_executable(
                record, "copy_construct"
            ):
                return explicit
            raise AdapterContractError(
                f"{symbol_name}: explicit copy has no executable copy lifecycle"
            )
        if explicit == "move":
            if reference in {"pointer", "pointer_const", "pointer_mut"}:
                return explicit
            if type_info.get("kind") != "class":
                return explicit
            record = record_for_dci_type(type_info, records)
            if record is not None and lifecycle_operation_is_executable(
                record, "move_construct"
            ):
                return explicit
            raise AdapterContractError(
                f"{symbol_name}: explicit move has no executable move lifecycle"
            )
        raise AdapterContractError(f"{symbol_name}: unsupported parameter ownership {explicit!r}")
    if reference != "value":
        if "&&" in raw:
            if type_info.get("kind") != "class":
                return "move"
            record = record_for_dci_type(type_info, records)
            if record is not None and lifecycle_operation_is_executable(record, "move_construct"):
                return "move"
            raise AdapterContractError(
                f"{symbol_name}: rvalue-reference parameter {raw!r} has no executable move contract"
            )
        if reference in {"pointer", "pointer_const", "pointer_mut"}:
            raise AdapterContractError(
                f"{symbol_name}: raw-pointer parameter {raw!r} requires explicit ownership annotation"
            )
        return "borrow" if cpp_borrow_is_const(type_info) else "borrow_mut"
    if type_info.get("kind") != "class":
        return "copy"

    record = record_for_dci_type(type_info, records)
    if record is None:
        raise AdapterContractError(
            f"{symbol_name}: value parameter type {type_info.get('name')!r} has no verified layout/lifecycle"
        )
    if lifecycle_operation_is_executable(record, "copy_construct"):
        return "copy"
    if lifecycle_operation_is_executable(record, "move_construct"):
        return "move"
    raise AdapterContractError(
        f"{symbol_name}: value parameter type {record.type_name!r} has no executable copy or move contract"
    )


def return_ownership(
    type_info: dict[str, Any],
    records: list[RecordLayout],
    symbol_name: str,
    explicit: str = "",
) -> str:
    reference = type_info.get("reference") or "value"
    if type_info.get("kind") == "function" and reference in {
        "pointer",
        "reference",
    }:
        if explicit and explicit != "copy":
            raise AdapterContractError(
                f"{symbol_name}: function pointer/reference returns use copy ownership"
            )
        return "copy"
    if explicit:
        if explicit in {"borrow", "share", "owned"}:
            if reference == "value":
                raise AdapterContractError(
                    f"{symbol_name}: explicit {explicit} return requires a pointer or reference"
                )
            return explicit
        if explicit in {"copy", "move"}:
            if reference != "value":
                raise AdapterContractError(
                    f"{symbol_name}: explicit {explicit} return requires a value"
                )
            if type_info.get("kind") != "class":
                return explicit
            record = record_for_dci_type(type_info, records)
            operation = "copy_construct" if explicit == "copy" else "move_construct"
            if record is not None and lifecycle_operation_is_executable(record, operation):
                return explicit
            raise AdapterContractError(
                f"{symbol_name}: explicit {explicit} return has no executable lifecycle"
            )
        raise AdapterContractError(f"{symbol_name}: unsupported return ownership {explicit!r}")
    if reference != "value":
        raise AdapterContractError(
            f"{symbol_name}: pointer/reference return requires explicit ownership annotation"
        )
    if type_info.get("kind") != "class":
        return "copy"

    record = record_for_dci_type(type_info, records)
    if record is None or record.size <= 0 or record.alignment <= 0:
        raise AdapterContractError(
            f"{symbol_name}: value return type {type_info.get('name')!r} has no verified nonzero layout"
        )
    if bool(record.traits.get("trivially_copyable")) and lifecycle_operation_is_executable(
        record, "destroy"
    ):
        return "copy"
    if lifecycle_operation_is_executable(record, "destroy"):
        return "owned"
    raise AdapterContractError(
        f"{symbol_name}: value return type {record.type_name!r} has no executable destruction contract"
    )


def abi_value_lowering(
    type_info: dict[str, Any] | None,
    records: list[RecordLayout],
    position: str,
    target: str = "",
    machine_signatures: dict[str, dict[str, Any]] | None = None,
) -> dict[str, Any]:
    if type_info is None:
        return {"passing": "direct", "type": {"name": "void", "kind": "primitive"}}
    result: dict[str, Any] = {"passing": "direct", "type": type_info}
    if type_info.get("name") == "void":
        return result
    if type_info.get("kind") in {"opaque", "vector"} and (
        type_info.get("reference") or "value"
    ) == "value":
        reason = type_info.get("unsupported_reason") or (
            f"{type_info.get('kind')} by-value ABI lowering is not verified"
        )
        raise AdapterContractError(f"{type_info.get('name')}: {reason}")
    reference = type_info.get("reference") or "value"
    if reference != "value" or type_info.get("kind") != "class":
        return result
    record = record_for_dci_type(type_info, records)
    signature = None
    if machine_signatures and record is not None:
        signature = next(
            (
                value
                for name, value in machine_signatures.items()
                if names_match(name, record.type_name)
            ),
            None,
        )
    abi_family = target_contract(target)["abi_family"] if target else ""
    pointer_width = (
        int(target_contract(target).get("pointer_width") or 64) if target else 64
    )
    if signature is not None:
        if record is None or record.size <= 0 or record.alignment <= 0:
            raise AdapterContractError(
                f"by-value aggregate {type_info.get('name')!r} has no "
                "verified machine-signature lowering"
            )
        lowering = itanium_aggregate_lowering(
            record, position, signature, pointer_width
        )
        lowering["type"] = type_info
        return lowering
    if (
        record is not None
        and record.size > 0
        and record.alignment > 0
        and record.traits.get("closed_tagged_return")
    ):
        result["size"] = record.size
        result["alignment"] = record.alignment
        result["passing"] = "sret" if position == "return" else "indirect"
        result["reason"] = "aggregate_abi"
        return result
    # MSVC's class-layout dump records canPassInRegisters.  When the compiler
    # said the type cannot pass in registers, that dump *is* the CodeGen fact
    # for memory passing.  Register-passed types still require a /FA or LLVM IR
    # dump so we do not invent iN from sizeof.
    if (
        abi_family == "msvc"
        and record is not None
        and record.size > 0
        and record.alignment > 0
        and not bool(record.traits.get("can_pass_in_registers"))
    ):
        result["size"] = record.size
        result["alignment"] = record.alignment
        result["passing"] = "sret" if position == "return" else "indirect"
        result["reason"] = "aggregate_abi"
        return result
    raise AdapterContractError(
        f"by-value aggregate {type_info.get('name')!r} has no "
        "verified machine-signature lowering"
    )


def symbol_native_calling_convention(sym: Symbol, target: str) -> str:
    declared = sym.native_calling_convention
    if declared == "vectorcall":
        return "vectorcall"
    descriptor = target_contract(target)
    if descriptor["platform"] == "windows" and descriptor["architecture"] == "x86_64":
        return "win64"
    if declared:
        return declared
    if descriptor["platform"] == "windows" and descriptor["architecture"] == "x86" and sym.owner and not sym.is_static:
        return "thiscall"
    return "cdecl"


def constructor_abi_return(sym: Symbol, target: str) -> dict[str, Any] | None:
    if sym.kind != "constructor":
        return None
    descriptor = target_contract(target)
    # The x64 Microsoft C++ ABI returns the constructed `this` pointer in RAX.
    # This is a machine-level result even though the C++ source return is void.
    if (
        descriptor["platform"] == "windows"
        and descriptor["architecture"] == "x86_64"
        and descriptor["abi_family"] == "msvc"
    ):
        return {
            "passing": "direct",
            "type": {"name": sym.owner, "kind": "class", "reference": "pointer"},
        }
    return None


def symbol_json(
    sym: Symbol,
    records: list[RecordLayout] | None = None,
    target: str = "x86_64-pc-windows-msvc",
    machine_signatures: dict[str, dict[str, Any]] | None = None,
) -> dict[str, Any]:
    known_records = records or []
    native_cc = symbol_native_calling_convention(sym, target)
    normalized_params = []
    abi_params = []
    for index, param in enumerate(sym.params):
        type_info = param.get("type")
        if not isinstance(type_info, dict):
            raise AdapterContractError(f"{sym.name}: parameter #{index} has no normalized type")
        normalized_param = dict(param)
        normalized_param["ownership"] = parameter_ownership(
            type_info,
            known_records,
            sym.name,
            sym.parameter_ownerships.get(index, ""),
        )
        normalized_params.append(normalized_param)
        lowering = abi_value_lowering(
            type_info, known_records, "parameter", target, machine_signatures
        )
        lowering["index"] = index
        lowering["name"] = param.get("name") or f"p{index}"
        abi_params.append(lowering)
    normalized_return = None
    if sym.ret is not None:
        return_type = sym.ret.get("type")
        if not isinstance(return_type, dict):
            raise AdapterContractError(f"{sym.name}: return has no normalized type")
        if return_type.get("name") != "void":
            normalized_return = dict(sym.ret)
            normalized_return["ownership"] = return_ownership(
                return_type, known_records, sym.name, sym.return_ownership
            )
    abi_return = abi_value_lowering(
        normalized_return.get("type") if normalized_return is not None else None,
        known_records,
        "return",
        target,
        machine_signatures,
    )
    if (
        native_cc == "win64"
        and sym.kind == "method"
        and not sym.is_static
        and abi_return.get("passing") == "coerce"
    ):
        # MSVC x64: a non-static member function returns EVERY user-defined
        # type through the hidden pointer (rcx=this, rdx=sret, r8=arg1; RAX
        # holds the sret pointer) -- the register-return rule is reserved for
        # global and static functions.  The machine-signature probe measures
        # free functions, so a small record return would otherwise be recorded
        # as a register coerce and the consumer would call the vtable slot
        # with the wrong shape (clang lowers `Sample consume(Sample)` to sret
        # even at 8 bytes; verified against clang 22 windows-msvc output).
        return_type = (normalized_return or {}).get("type") or {}
        if return_type.get("kind") == "class" and (
            return_type.get("reference") or "value"
        ) == "value":
            abi_return = {
                "passing": "sret",
                "type": return_type,
                "size": abi_return.get("size"),
                "alignment": abi_return.get("alignment"),
                "reason": "msvc_member_return_hidden_pointer",
            }
    native_constructor_return = constructor_abi_return(sym, target)
    if native_constructor_return is not None:
        abi_return = native_constructor_return
    receiver = None
    if sym.owner and not sym.is_static:
        receiver = {
            "passing": "direct",
            "type": {"name": sym.owner, "kind": "class", "reference": "pointer"},
            "this_adjust": sym.this_adjust,
            "ownership": "borrow" if sym.is_const else "borrow_mut",
        }
    payload = {
        "name": sym.name,
        "owner": sym.owner,
        "member_name": sym.member_name,
        "mangled": sym.mangled,
        "link_name": sym.mangled,
        "kind": sym.kind,
        # The DCI contract names the target machine ABI.  Member/virtual/
        # lifecycle shape is carried by the normalized symbol and receiver
        # fields, not by a producer-language calling-convention alias.
        "calling_convention": native_cc,
        "linkage": "external",
        "visibility": sym.visibility,
        "is_static": sym.is_static,
        "is_virtual": sym.is_virtual,
        "is_override": sym.is_override,
        "is_final": sym.is_final,
        "is_const": sym.is_const,
        "this_adjust": sym.this_adjust,
        "params": normalized_params,
        "return": normalized_return,
        "control_flow": {
            "default_boundary": "no_unwind",
            "propagation": {"mode": "forbidden"},
            "unwind": sym.unwind,
            "boundary_action": "direct" if sym.unwind == "no_unwind" else "stub_required",
        },
        "abi": {
            "calling_convention": native_cc,
            "receiver": receiver,
            "params": abi_params,
            "parameters": abi_params,
            "return": abi_return,
            "unwind": sym.unwind,
        },
    }
    if sym.semantic_id:
        payload["semantic_id"] = sym.semantic_id
    if sym.native_template:
        if "&&" in sym.member_qualifiers:
            raise AdapterContractError("rvalue-qualified native receiver requires explicit move semantics")
        if sym.kind == "destructor":
            payload["member_name"] = "destructor"
        payload["cpp_materialization"] = {
            "kind": "list_constructor" if sym.list_arguments is not None else "native_member",
            "owner": sym.owner,
            "member": sym.member_name,
            "qualifiers": sym.member_qualifiers,
        }
        if sym.list_arguments is not None:
            payload["cpp_materialization"]["arguments"] = sym.list_arguments
    return payload


def lifecycle_operation_ref(
    contract: dict[str, Any], exported_link_names: set[str] | None = None
) -> dict[str, Any]:
    if not contract.get("available"):
        return {"availability": "unavailable"}
    if contract.get("trivial"):
        return {"availability": "trivial", "no_unwind": True}
    link_name = contract.get("link_name") or ""
    if link_name and (exported_link_names is None or link_name in exported_link_names):
        return {
            "symbol": link_name,
            "availability": "required",
            "no_unwind": contract.get("unwind") == "no_unwind",
        }
    return {
        "availability": "optional",
        "no_unwind": contract.get("unwind") == "no_unwind",
        "requires_stub": True,
    }


def lifecycle_json(
    lifecycle: dict[str, Any], exported_link_names: set[str] | None = None
) -> dict[str, Any]:
    copy = lifecycle.get("copy_construct") or {}
    move = lifecycle.get("move_construct") or {}
    destroy = lifecycle.get("destroy") or {}

    def transfer_semantics(contract: dict[str, Any]) -> str:
        if not contract.get("available"):
            return "forbidden"
        if contract.get("trivial"):
            return "trivial"
        link_name = contract.get("link_name") or ""
        if link_name and (exported_link_names is None or link_name in exported_link_names):
            return "operation"
        return "forbidden"

    if not destroy.get("available"):
        destruction = "none"
    elif destroy.get("trivial"):
        destruction = "trivial"
    else:
        destroy_link = destroy.get("link_name") or ""
        destruction = (
            "operation"
            if destroy_link
            and (exported_link_names is None or destroy_link in exported_link_names)
            else "external"
        )
    result = dict(lifecycle)
    result.update(
        {
            "ownership_model": "value",
            "copy_semantics": transfer_semantics(copy),
            "move_semantics": transfer_semantics(move),
            "destruction": destruction,
            "moved_from_state": "unspecified",
            "operations": {
                "default_construct": lifecycle_operation_ref(
                    lifecycle.get("default_construct") or {}, exported_link_names
                ),
                "copy": lifecycle_operation_ref(copy, exported_link_names),
                "move": lifecycle_operation_ref(move, exported_link_names),
                "copy_assign": lifecycle_operation_ref(
                    lifecycle.get("copy_assign") or {}, exported_link_names
                ),
                "move_assign": lifecycle_operation_ref(
                    lifecycle.get("move_assign") or {}, exported_link_names
                ),
                "destroy": lifecycle_operation_ref(destroy, exported_link_names),
            },
        }
    )
    return result


def cpp_top_level_const(raw: str) -> bool:
    text = collapse_spaces(raw)
    if "*" in text:
        return re.search(r"\bconst\b", text.rsplit("*", 1)[1]) is not None
    if "&" in text:
        return False
    return re.search(r"\bconst\b", text) is not None


def field_json(field_layout: FieldLayout, owner: str = "") -> dict[str, Any]:
    result = {
        "name": field_layout.name,
        "offset": field_layout.offset,
        "type": dci_type(field_layout.cpp_type, owner),
        "cpp_type": field_layout.cpp_type,
        "visibility": field_layout.visibility,
        "is_readonly": cpp_top_level_const(field_layout.cpp_type),
    }
    if field_layout.bit_width is not None:
        bitfield = {
            "storage_offset": field_layout.offset,
            "storage_size": field_layout.storage_size,
            "bit_offset": field_layout.bit_offset,
            "bit_width": field_layout.bit_width,
            "signed": field_layout.is_signed,
            "bit_order": "lsb0",
            "read": "direct",
            "write": "direct",
        }
        result["bitfield"] = bitfield
        result["storage"] = {
            "kind": "bitfield",
            "byte_offset": field_layout.offset,
            "bit_offset": field_layout.bit_offset,
            "bit_width": field_layout.bit_width,
            "unit_size": field_layout.storage_size,
            "unit_bits": field_layout.storage_size * 8 if field_layout.storage_size else None,
            "signed": field_layout.is_signed,
            "access": "masked",
        }
    return result


def base_adjustment_is_directly_executable(adjustment: Any) -> bool:
    if not isinstance(adjustment, dict):
        return False
    if adjustment.get("kind") == "constant":
        return isinstance(adjustment.get("offset"), int) and not isinstance(
            adjustment.get("offset"), bool
        )
    if adjustment.get("kind") != "table":
        return False
    required = (
        "table_pointer_offset",
        "table_entry_offset",
        "table_entry_size",
        "table_entry_signed",
        "displacement_base_offset",
        "result",
        "null_preserving",
    )
    if not all(field in adjustment for field in required):
        return False
    integer_fields = (
        "table_pointer_offset",
        "table_entry_offset",
        "table_entry_size",
        "displacement_base_offset",
    )
    if not all(
        isinstance(adjustment.get(field), int)
        and not isinstance(adjustment.get(field), bool)
        for field in integer_fields
    ):
        return False
    return (
        adjustment["table_entry_size"] in {1, 2, 4, 8}
        and isinstance(adjustment.get("table_entry_signed"), bool)
        and isinstance(adjustment.get("null_preserving"), bool)
        and adjustment.get("result") == "object_plus_base_plus_displacement"
    )


def runtime_type_contract(
    record: RecordLayout, vtables: list[VTable], rtti_enabled: bool
) -> dict[str, Any]:
    polymorphic = bool(record.traits.get("polymorphic")) or record.has_vtable
    has_rtti_entry = any(
        names_match(vtable.class_name, record.type_name)
        and any(entry.kind == "rtti" for entry in vtable.entries)
        for vtable in vtables
    )
    identity_available = polymorphic and rtti_enabled and has_rtti_entry
    upcast_available = bool(record.bases)
    upcast_requires_stub = any(
        base.is_virtual
        and not base_adjustment_is_directly_executable(base.adjustment)
        for base in record.bases
    )
    return {
        "polymorphic": polymorphic,
        "operations": {
            "type_identity": {
                "available": identity_available,
                "requires_stub": identity_available,
            },
            "upcast": {
                "available": upcast_available,
                "requires_stub": upcast_requires_stub,
            },
            "checked_downcast": {
                "available": identity_available,
                "requires_stub": identity_available,
            },
            "checked_crosscast": {
                "available": identity_available,
                "requires_stub": identity_available,
            },
        },
    }


def runtime_type_operations_json(
    records: list[RecordLayout], vtables: list[VTable], rtti_enabled: bool
) -> list[dict[str, Any]]:
    result = []
    for record in records:
        contract = runtime_type_contract(record, vtables, rtti_enabled)
        operations = contract["operations"]
        capabilities = []
        exported_operations = {}
        if operations["type_identity"]["available"]:
            capabilities.extend(["query_type", "downcast", "cast"])
            exported_operations["query_type"] = {
                "availability": "optional",
                "requires_stub": True,
            }
            exported_operations["downcast"] = {
                "availability": "optional",
                "requires_stub": True,
            }
            exported_operations["cast"] = {
                "availability": "optional",
                "requires_stub": True,
            }
        if operations["upcast"]["available"]:
            capabilities.append("upcast")
            if operations["upcast"]["requires_stub"]:
                exported_operations["upcast"] = {
                    "availability": "optional",
                    "requires_stub": True,
                }
            else:
                exported_operations["upcast"] = {
                    "availability": "required",
                    "intrinsic": "dci.layout.upcast",
                    "requires_stub": False,
                }
        if capabilities:
            result.append(
                {
                    "type_name": record.type_name,
                    "capabilities": capabilities,
                    "operations": exported_operations,
                }
            )
    return result


def cpp_artifact_entries(
    artifacts: Sequence[str], headers: Sequence[str], target: str,
) -> list[dict[str, Any]]:
    root = Path(headers[0]).resolve().parent if headers else Path(".")
    object_format = "coff" if "windows" in target else "elf"
    if "apple" in target or "darwin" in target or "macos" in target:
        object_format = "macho"
    entries: list[dict[str, Any]] = []
    for raw in artifacts:
        artifact = Path(raw).expanduser()
        suffix = artifact.suffix.lower()
        kind = (
            "static_library" if suffix in {".a", ".lib"}
            else "shared_library" if suffix in {".so", ".dll", ".dylib"}
            else "object"
        )
        portable_path = artifact
        if artifact.is_absolute():
            try:
                portable_path = artifact.resolve().relative_to(root)
            except ValueError:
                portable_path = Path(artifact.name)
        entries.append({
            "kind": kind,
            "path": portable_path.as_posix(),
            "format": object_format,
            "link_mode": "required",
        })
    return entries


def write_abi(
    path: Path,
    target: str,
    headers: list[str],
    records: list[RecordLayout],
    symbols: list[Symbol],
    vtables: list[VTable],
    aliases: list[TypeAlias],
    rtti_enabled: bool = True,
    debug_json_path: Path | None = None,
    toolchain: cpp_toolchains.CppToolchain | None = None,
    fact_extractor: cpp_toolchains.CppToolchain | None = None,
    compiler_flags: list[str] | None = None,
    unverified_records: set[str] | None = None,
    machine_signatures: dict[str, dict[str, Any]] | None = None,
    stub_source_path: Path | None = None,
    artifacts: Sequence[str] | None = None,
    boundary: str = "no_unwind",
    borrowed_returns: Sequence[str] = (),
    cxx_standard: str = "c++20",
    closure_settings: dict[str, Any] | None = None,
) -> None:
    headers = public_api_headers(headers)
    if toolchain is None:
        toolchain = cpp_toolchains.CppToolchain(
            family="clang",
            executable="clang++",
            version="",
            version_line="",
            native_target=target,
            driver_mode="clang",
        )
    try:
        metadata = cpp_toolchains.descriptor_metadata(
            toolchain,
            target,
            headers=headers,
            rtti_enabled=rtti_enabled,
            flags=compiler_flags,
        )
    except cpp_toolchains.CppToolchainError as error:
        raise AdapterContractError(str(error)) from error
    normalized_target = metadata["target"]
    producer = dict(metadata["producer"])
    source = dict(metadata["source"])
    source["borrowed_template_returns"] = list(borrowed_returns)
    source["cxx_standard"] = cxx_standard
    if closure_settings is not None:
        source["closure_settings"] = closure_settings
    if fact_extractor is not None:
        extractor_identity = cpp_toolchains.compiler_identity_metadata(fact_extractor)
        producer["fact_extractor"] = extractor_identity
        source["fact_extractor"] = extractor_identity
    unverified = set(unverified_records or ())
    exported_symbols = []
    rejected_symbols = []
    for symbol in symbols:
        if symbol.owner and symbol.owner in unverified:
            rejected_symbols.append(
                {
                    "name": symbol.name,
                    "selector": symbol_selector(symbol),
                    "link_name": symbol.mangled,
                    "reason": (
                        f"the selected {toolchain.family} compiler could not extract "
                        f"stable layout facts for {symbol.owner!r}"
                    ),
                }
            )
            continue
        try:
            exported_symbols.append(
                symbol_json(symbol, records, target, machine_signatures)
            )
        except AdapterContractError as error:
            rejected_symbols.append(
                {
                    "name": symbol.name,
                    "selector": symbol_selector(symbol),
                    "link_name": symbol.mangled,
                    "reason": str(error),
                }
            )
    _translate_mod = sys.modules.get("dci_translate_unwind")
    if _translate_mod is None:
        _translate_path = Path(__file__).with_name("dci_translate_unwind.py")
        _translate_spec = importlib.util.spec_from_file_location(
            "dci_translate_unwind", _translate_path
        )
        if _translate_spec is None or _translate_spec.loader is None:
            raise AdapterContractError(f"cannot load {_translate_path}")
        _translate_mod = importlib.util.module_from_spec(_translate_spec)
        _translate_spec.loader.exec_module(_translate_mod)
        sys.modules["dci_translate_unwind"] = _translate_mod
    attach_translate_unwind = _translate_mod.attach_translate_unwind

    extra_symbols, extra_records, stub_requests, stub_source = ([], [], [], "") if boundary == "shared_abi" else attach_translate_unwind(
        sys.modules[__name__],
        exported_symbols=exported_symbols,
        records=records,
        original_symbols=symbols,
        headers=headers,
        target=target,
        toolchain=toolchain,
        machine_signatures=machine_signatures,
    )
    if extra_records:
        records = list(records) + extra_records
    if extra_symbols:
        exported_symbols.extend(extra_symbols)
    if stub_source and stub_source_path is not None:
        stub_source_path.parent.mkdir(parents=True, exist_ok=True)
        stub_source_path.write_text(stub_source, encoding="utf-8")
    exported_link_names = {
        symbol["link_name"]
        for symbol in exported_symbols
        if symbol.get("link_name")
    }
    # Surface the alias identities the fact extractor embedded in exported type
    # objects (e.g. ``std::string`` -> ``std::basic_string<char>``) into the
    # top-level alias table.  ``discover_type_aliases`` only sees ``using``
    # declarations in the scanned project headers, so library typedefs such as
    # ``std::string`` never appear there; but ``dci_type`` already recorded the
    # compiler-desugared identity on each referencing type object as
    # ``alias_name``.  Publishing those pairs lets a consumer match a spelled
    # alias (``std::string`` parameter of ``sink::set_pattern``) to the record
    # whose layout backs it, instead of failing symbol resolution.
    alias_pairs: dict[str, str] = {}

    def _collect_alias_pairs(type_obj: Any) -> None:
        if not isinstance(type_obj, dict):
            return
        alias_name = type_obj.get("alias_name")
        name = type_obj.get("name")
        if (
            isinstance(alias_name, str)
            and isinstance(name, str)
            and alias_name
            and name
            and normalize_cpp_type(alias_name) != normalize_cpp_type(name)
        ):
            alias_pairs.setdefault(alias_name, name)
        _collect_alias_pairs(type_obj.get("pointee"))

    for symbol in exported_symbols:
        abi = symbol.get("abi") or {}
        for param in abi.get("params", []) or []:
            _collect_alias_pairs(param.get("type"))
        _collect_alias_pairs((abi.get("return") or {}).get("type"))
        _collect_alias_pairs((abi.get("receiver") or {}).get("type"))
    existing_alias_names = {normalize_cpp_type(alias.name) for alias in aliases}
    for alias_name, target in alias_pairs.items():
        if normalize_cpp_type(alias_name) not in existing_alias_names:
            aliases = aliases + [TypeAlias(alias_name, target)]
            existing_alias_names.add(normalize_cpp_type(alias_name))
    dispatch_tables = [
        {
            "class_name": table.class_name,
            "base_class": table.base_class,
            "vtable_symbol": "",
            "address_point_offset": vtable_address_point_offset(table),
            "entries": [
                {
                    "index": entry.index,
                    "offset": entry.offset,
                    "address_point_relative_offset": entry.offset
                    - vtable_address_point_offset(table),
                    "name": entry.name,
                    "kind": entry.kind,
                    "owner": entry.owner,
                    "member_name": entry.member_name,
                    "mangled": entry.mangled,
                    "this_adjust": entry.this_adjust,
                    "return_adjust": entry.return_adjust,
                }
                for entry in table.entries
            ],
        }
        for table in vtables
    ]
    doc = {
        "dci": "1.0",
        "kind": "abi",
        "schema": {
            "name": "dci",
            "version": "1.0",
            "encoding": "dcib" if path.suffix.lower() == ".dcib" else "json",
            "file_extensions": [".dcib", ".dci", ".abi.json"],
        },
        "profile": {
            "id": "native-object-v1",
            "version": "1.0",
            "level": "L3",
            "conformance": "feature_subset",
            "consumer_modes": ["direct", "stub"],
            "lifecycle_binding": "declaration",
            "features": [
                "functions",
                "object_layout",
                "lifecycle",
                "by_value_abi",
                "bitfields",
                "inheritance",
                "virtual_dispatch",
                "virtual_bases",
                "runtime_type",
            ],
            "capabilities": [
                "functions",
                "object_layout",
                "lifecycle",
                "by_value_abi",
                "bitfields",
                "inheritance",
                "virtual_dispatch",
                "virtual_bases",
                "runtime_type",
            ],
        },
        "target": normalized_target,
        "control_flow": {
            "default_boundary": "no_unwind",
            "propagation": {"mode": "forbidden"},
            "boundary": "no_unwind",
            "foreign_unwind": False,
            "panic": False,
            "exceptions": False,
            "propagation_abi": None,
        },
        "producer": producer,
        "profile_extension": metadata["profile_extension"],
        "source": source,
        "exports": {
            "aliases": [
                {
                    "name": alias.name,
                    "target": alias.target,
                    "kind": "type_alias",
                }
                for alias in aliases
            ],
            "layouts": [
                {
                    "type_name": rec.type_name,
                    "size": rec.size,
                    "alignment": rec.alignment,
                    # A type has a stable by-value representation when its bytes
                    # ARE its value: standard-layout + trivially-copyable +
                    # trivially-destructible. POD additionally demands a trivial
                    # default constructor, which is irrelevant to copying an
                    # already-built object -- e.g. fmt::basic_string_view<char> is
                    # a {const char*, size_t} value that is trivially copyable and
                    # standard-layout but non-POD only because of a user-declared
                    # default ctor. This criterion is a strict superset of the old
                    # (pod && standard_layout), so it never declassifies a type
                    # that was already stable.
                    "representation": (
                        "stable"
                        if bool(rec.traits.get("standard_layout"))
                        and bool(rec.traits.get("trivially_copyable"))
                        and bool((rec.lifecycle.get("destroy") or {}).get("trivial"))
                        else "native"
                    ),
                    "has_vtable": rec.has_vtable,
                    "is_pod": bool(rec.traits.get("pod")),
                    "is_trivially_destructible": bool(
                        (rec.lifecycle.get("destroy") or {}).get("trivial")
                    ),
                    "traits": rec.traits,
                    "lifecycle": lifecycle_json(
                        rec.lifecycle, exported_link_names
                    ),
                    "runtime_type": runtime_type_contract(rec, vtables, rtti_enabled),
                    "virtual_base_table_pointer_offsets": rec.vbptr_offsets,
                    "bases": [
                        {
                            "type_name": base.type_name,
                            "offset": base.offset,
                            "visibility": base.visibility,
                            "is_virtual": base.is_virtual,
                            "adjustment": base.adjustment,
                        }
                        for base in rec.bases
                    ],
                    "fields": [field_json(fld, rec.type_name) for fld in rec.fields],
                }
                for rec in records
                if rec.type_name not in unverified
            ],
            "symbols": exported_symbols,
            "rejected_symbols": rejected_symbols,
            **({"stub_requests": stub_requests} if stub_requests else {}),
            "runtime_type_operations": runtime_type_operations_json(
                records, vtables, rtti_enabled
            ),
            "calling_conventions": [
                {
                    "name": calling_convention,
                    "description": "Target machine calling convention used by this descriptor",
                }
                for calling_convention in sorted(
                    {
                        symbol["abi"]["calling_convention"]
                        for symbol in exported_symbols
                    }
                )
            ],
            "dispatch_tables": dispatch_tables,
            "vtables": dispatch_tables,
        },
    }
    artifact_entries = cpp_artifact_entries(artifacts or [], headers, target)
    source["boundary"] = boundary
    if boundary == "shared_abi":
        family = normalized_target["abi_family"]
        if normalized_target["architecture"] != "x86_64" or family not in {"msvc", "sysv"}:
            raise AdapterContractError("shared C++ unwind requires the implemented x86_64 MSVC/System V ABI")
        identity = "dci.eh.msvc-cxx.v1" if family == "msvc" else "dci.eh.itanium-cxx.v1"
        flow = {"default_boundary": boundary, "propagation": {"mode": boundary, "abi": identity}}
        doc["control_flow"] = flow
        for exported in exported_symbols:
            exported["control_flow"] = dict(flow, unwind=exported["abi"]["unwind"], boundary_action="direct")
    if artifact_entries:
        doc["artifacts"] = artifact_entries
    dcib.validate_cjson_compatible(doc)
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.suffix.lower() == ".dcib":
        path.write_bytes(dcib.encode(doc))
    else:
        path.write_text(json.dumps(doc, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    if debug_json_path is not None:
        debug_json_path.parent.mkdir(parents=True, exist_ok=True)
        debug_json_path.write_text(
            json.dumps(doc, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
        )


def validate_translation_unit_with_selected_compiler(
    toolchain: cpp_toolchains.CppToolchain,
    std: str,
    probe: Path,
    compiler_args: list[str],
    target: str = "",
    *,
    force: bool = False,
) -> None:
    """Require the selected frontend to parse the exact public probe.

    The selected ``--toolchain`` compiler is the sole ABI authority.  Clang is
    still used as a structured parse/layout helper, but the selected compiler
    must be able to parse the public API before any descriptor is emitted; this
    is the fail-closed parse gate for the single trusted toolchain.  It is not a
    cross-compiler consensus check -- Clang and the selected compiler are never
    required to agree, only the selected compiler must accept its own contract.
    """
    if not force and toolchain.family == "clang" and toolchain.driver_mode == "clang":
        return
    if toolchain.family == "msvc":
        command = [
            toolchain.executable,
            "/nologo",
            "/Zs",
            f"/std:{std}",
            str(probe),
            *compiler_args,
        ]
    else:
        command = [
            toolchain.executable,
            f"-std={std}",
            "-fsyntax-only",
            str(probe),
            *compiler_args,
        ]
    if toolchain.family == "clang" and target:
        command += ["-target", target]
    environment = msvc_validation_environment(toolchain, target)
    proc = subprocess.run(
        command,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
        env=environment,
    )
    if proc.returncode != 0:
        detail = (proc.stdout or "").strip()
        raise AdapterContractError(
            f"{toolchain.family} rejected the DCI public-header probe"
            + (f":\n{detail}" if detail else "")
        )


RE_SIZEOF_ERROR_LINE = re.compile(r":(\d+):\d+: error:")


def filter_sizeofable_record_names(
    clang: str,
    std: str,
    target: str,
    include_text: str,
    names: list[str],
    extra: list[str],
    workdir: Path,
) -> list[str]:
    """Drop regex-discovered names the fact extractor cannot `sizeof`.

    Industrial headers (Qt) leave template-ids, nested private tags and
    documentation samples in a line-oriented scan. Those names must not abort
    the public TU; Clang's layout dump still covers the complete types.
    """
    remaining = list(names)
    include_lines = include_text.count("\n")
    probe = workdir / "dci_sizeof_filter.cpp"
    for _ in range(16):
        if not remaining:
            return []
        probe.write_text(
            include_text
            + "".join(
                f'static_assert(sizeof({name}) >= 0, "vyx_dci_{idx}");\n'
                for idx, name in enumerate(remaining)
            ),
            encoding="utf-8",
        )
        proc = subprocess.run(
            [
                clang,
                f"-std={std}",
                "-target",
                target,
                "-fsyntax-only",
                str(probe),
                *extra,
            ],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
        )
        if proc.returncode == 0:
            return remaining
        bad: set[str] = set()
        probe_error = re.compile(
            rf"(?:{re.escape(str(probe))}|{re.escape(probe.name)}):(\d+):\d+: error:",
            re.IGNORECASE,
        )
        for match in probe_error.finditer(proc.stdout or ""):
            lineno = int(match.group(1))
            index = lineno - include_lines - 1
            if 0 <= index < len(remaining):
                bad.add(remaining[index])
        if not bad:
            # Clang also emits `error:` on notes in included headers; keep
            # dropping by `sizeof(NAME)` spellings in the probe diagnostics.
            for match in re.finditer(
                r"sizeof\(([^)]+)\)",
                proc.stdout or "",
            ):
                spelled = match.group(1).strip()
                if spelled in remaining:
                    bad.add(spelled)
        if not bad:
            detail = (proc.stdout or "").strip()
            raise AdapterContractError(
                "C++ fact extractor rejected the DCI sizeof probe"
                + (f":\n{detail}" if detail else "")
            )
        remaining = [name for name in remaining if name not in bad]
    raise AdapterContractError("could not stabilize sizeofable C++ record set")


def msvc_validation_environment(
    toolchain: cpp_toolchains.CppToolchain,
    target: str,
) -> dict[str, str] | None:
    if toolchain.family != "msvc" or os.environ.get("INCLUDE", "").strip():
        return None
    cache_key = (str(Path(toolchain.executable)).lower(), target.lower())
    cached = MSVC_ENV_CACHE.get(cache_key)
    if cached is not None:
        return dict(cached)
    compiler = Path(toolchain.executable)
    if not compiler.is_absolute():
        return None
    try:
        vc_root = compiler.parents[6]
    except IndexError:
        return None
    vcvarsall = vc_root / "Auxiliary" / "Build" / "vcvarsall.bat"
    if not vcvarsall.is_file():
        raise AdapterContractError(
            f"MSVC environment is not initialized and vcvarsall.bat was not found "
            f"for {toolchain.executable!r}"
        )
    info = cpp_toolchains.target_info(target or toolchain.native_target)
    argument = {
        "x86_64": "amd64",
        "x86": "amd64_x86",
        "aarch64": "amd64_arm64",
        "arm": "amd64_arm",
    }.get(info.architecture)
    if not argument:
        raise AdapterContractError(
            f"cannot initialize MSVC environment for architecture {info.architecture!r}"
        )
    command_processor = os.environ.get("COMSPEC", "cmd.exe")
    activation = f'call "{vcvarsall}" {argument} >nul && set'
    command_line = (
        subprocess.list2cmdline([command_processor]) + " /d /c " + activation
    )
    try:
        proc = subprocess.run(
            command_line,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
        )
    except OSError as error:
        raise AdapterContractError(
            f"cannot initialize the MSVC build environment: {error}"
        ) from error
    if proc.returncode != 0:
        raise AdapterContractError(
            "vcvarsall.bat failed while initializing the MSVC build environment"
            + (f":\n{proc.stdout.strip()}" if proc.stdout.strip() else "")
        )
    environment = dict(os.environ)
    for line in proc.stdout.splitlines():
        if not line or line.startswith("=") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        environment[key] = value
    if not environment.get("INCLUDE", "").strip():
        raise AdapterContractError(
            "vcvarsall.bat completed without defining the MSVC INCLUDE path"
        )
    MSVC_ENV_CACHE[cache_key] = dict(environment)
    return environment


def frontend_validation_args(
    toolchain: cpp_toolchains.CppToolchain,
    explicit: list[str],
    extractor_args: list[str],
) -> list[str]:
    """Translate portable extractor flags for the selected frontend.

    Include search paths, defines/undefs, the language standard and
    layout-affecting flags are forwarded so the selected compiler observes the
    same ABI-relevant configuration as the Clang fact extractor.  Clang-only
    driver plumbing (``-Xclang``, ``-target``) is dropped.
    """
    is_msvc = toolchain.family == "msvc" or toolchain.driver_mode == "clang-cl"
    result = list(explicit)
    index = 0
    while index < len(extractor_args):
        argument = extractor_args[index]
        if argument in {"-Xclang", "-target"}:
            index += 2
            continue
        if argument.startswith("--target="):
            index += 1
            continue
        value: str | None = None
        option = argument
        if argument in {"-I", "-D", "-U", "-isystem", "--sysroot"}:
            if index + 1 >= len(extractor_args):
                raise AdapterContractError(
                    f"missing value after extraction argument {argument!r}"
                )
            value = extractor_args[index + 1]
            index += 2
        else:
            index += 1
            for prefix in ("-I", "-D", "-U", "-isystem"):
                if argument.startswith(prefix) and len(argument) > len(prefix):
                    option, value = prefix, argument[len(prefix) :]
                    break
            if value is None:
                if argument.startswith("--sysroot="):
                    option, value = "--sysroot", argument.split("=", 1)[1]
                elif argument.startswith("-std=") or argument.startswith("/std:"):
                    standard = argument.split("=", 1)[1] if argument.startswith("-std=") else argument.split(":", 1)[1]
                    result.append(f"/std:{standard}" if is_msvc else f"-std={standard}")
                    continue
                elif not is_msvc and (argument.startswith("-f") or argument.startswith("-m")):
                    # Layout/ABI-affecting flags are meaningful to gcc/clang.
                    result.append(argument)
                    continue
                else:
                    # Unknown or frontend-incompatible flag: skip for validation.
                    continue
        if value is None:
            continue
        if is_msvc:
            translated = {"-I": "/I", "-isystem": "/I", "-D": "/D", "-U": "/U"}.get(option)
            if translated:
                result.append(translated + value)
        else:
            if option in {"-I", "-D", "-U"}:
                result.append(option + value)
            else:
                result.extend([option, value])
    return result


def _run_self_layout_probe(
    toolchain: cpp_toolchains.CppToolchain,
    std: str,
    probe: Path,
    compiler_args: list[str],
    target: str,
) -> str:
    """Compile the diagnostic layout probe with the selected compiler.

    The probe intentionally references incomplete template instantiations so
    that each ``sizeof``/``alignof`` result is echoed back inside a compiler
    diagnostic.  A non-zero exit status is therefore expected; the combined
    output is returned for parsing.
    """
    if toolchain.family == "msvc" or toolchain.driver_mode == "clang-cl":
        command = [
            toolchain.executable,
            "/nologo",
            "/Zs",
            f"/std:{std}",
            str(probe),
            *compiler_args,
        ]
    else:
        error_limit = "-ferror-limit=0" if toolchain.family == "clang" else "-fmax-errors=0"
        command = [
            toolchain.executable,
            f"-std={std}",
            "-fsyntax-only",
            error_limit,
            str(probe),
            *compiler_args,
        ]
    environment = msvc_validation_environment(toolchain, target)
    proc = subprocess.run(
        command,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
        env=environment,
    )
    return proc.stdout or ""


def measure_record_layout_with_selected_compiler(
    toolchain: cpp_toolchains.CppToolchain,
    std: str,
    target: str,
    headers: list[str],
    records: list[RecordLayout],
    compiler_args: list[str],
) -> set[str]:
    """Re-measure record size/alignment with the *selected* compiler.

    The selected ``--toolchain`` compiler is the sole ABI authority, so the
    layout numbers written to the contract must be its own -- not the Clang
    fact extractor's.  Clang still parses the headers and dumps structured
    facts, but every record's ``sizeof``/``alignof`` is re-measured here with a
    portable compile-time diagnostic probe compiled by the selected compiler,
    and the selected compiler's value overrides the extracted one when they
    differ.

    This is a *self-check of a single toolchain*, never a Clang-vs-GCC/MSVC
    cross-check: a legitimate Itanium-ABI difference between two compilers is
    recorded (with the selected compiler's number), not rejected.  Fail-closed
    happens only when the selected compiler cannot prove a stable size and
    alignment for an entity; those record names are returned so their symbols
    move to ``exports.rejected_symbols``.
    """
    if toolchain.family == "clang" and toolchain.driver_mode == "clang":
        # Clang is both the extractor and the authority here; the dumped facts
        # are already the selected compiler's own numbers.
        return set()
    candidates = [
        record
        for record in records
        if record.size >= 0 and record.alignment > 0
    ]
    if not candidates:
        return set()
    with tempfile.TemporaryDirectory(prefix="vyx_dci_self_layout_") as temp_dir:
        probe = Path(temp_dir) / "dci_self_layout.cpp"
        lines = [f'#include "{header}"' for header in headers]
        lines.append(
            "namespace { template<int, unsigned long long> struct __vyx_dci_size; }"
        )
        lines.append(
            "namespace { template<int, unsigned long long> struct __vyx_dci_align; }"
        )
        for index, record in enumerate(candidates):
            cpp_name = record.type_name
            lines.append(
                f"__vyx_dci_size<{index}, sizeof({cpp_name})> __vyx_dci_s{index};"
            )
            lines.append(
                f"__vyx_dci_align<{index}, alignof({cpp_name})> __vyx_dci_a{index};"
            )
        probe.write_text("\n".join(lines) + "\n", encoding="utf-8")
        output = _run_self_layout_probe(
            toolchain, std, probe, compiler_args, target
        )
    sizes = {
        int(match.group(1)): int(match.group(2))
        for match in re.finditer(
            r"__vyx_dci_size<\s*(\d+)\s*,\s*(\d+)\s*>", output
        )
    }
    aligns = {
        int(match.group(1)): int(match.group(2))
        for match in re.finditer(
            r"__vyx_dci_align<\s*(\d+)\s*,\s*(\d+)\s*>", output
        )
    }
    unverified: set[str] = set()
    for index, record in enumerate(candidates):
        measured_size = sizes.get(index)
        measured_align = aligns.get(index)
        if measured_size is None or measured_align is None or measured_align <= 0:
            unverified.add(record.type_name)
            continue
        record.size = measured_size
        record.alignment = measured_align
    return unverified


LAYOUT_FLAG_PREFIXES = (
    "-fpack-struct",
    "-fshort-enums",
    "-fno-short-enums",
    "-fabi-version=",
    "-fno-rtti",
    "-frtti",
    "-fexceptions",
    "-fno-exceptions",
    "-fsigned-char",
    "-funsigned-char",
    "-fno-signed-char",
    "-m32",
    "-m64",
    "-mabi=",
    "-march=",
    "-mtune=",
    "-mfpmath=",
)


def _compile_entry_argv(entry: dict[str, Any]) -> list[str]:
    arguments = entry.get("arguments")
    if isinstance(arguments, list) and arguments:
        return [str(item) for item in arguments]
    command = entry.get("command")
    if isinstance(command, str) and command:
        try:
            # ``compile_commands.json`` is commonly generated on Windows by
            # MSBuild/CMake with native ``C:\\...`` paths.  POSIX shlex treats
            # every backslash as an escape, turning ``C:\\Users\\...`` into
            # the drive-relative ``C:Users...`` and causing include paths to
            # be joined to the build directory.  Keep backslashes intact on
            # Windows while retaining the POSIX parser for Unix command DBs.
            if os.name == "nt":
                # Protect native separators while still letting shlex honour
                # Windows' quoted paths (``-I"C:\\Program Files\\api"``).
                # ``posix=False`` preserves backslashes but splits attached
                # quotes at spaces; a private sentinel gives us both rules.
                sentinel = "\ue000"
                protected = command.replace("\\", sentinel)
                return [token.replace(sentinel, "\\") for token in shlex.split(protected)]
            return shlex.split(command)
        except ValueError:
            return []
    return []


def _normalize_compile_flags(argv: list[str], base_dir: str) -> list[str]:
    """Extract include dirs, defines, std and layout flags from a TU command.

    The compiler binary, ``-c``, the input file and ``-o`` output are dropped;
    only ABI/layout-relevant flags are kept and rewritten in gnu joined form so
    they can be forwarded to both the Clang fact extractor and the selected
    validation frontend.
    """
    base = Path(base_dir)

    def unquote(value: str) -> str:
        value = value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in {"'", '"'}:
            return value[1:-1]
        return value

    def absolute(value: str) -> str:
        path = Path(unquote(value))
        return str(path if path.is_absolute() else (base / path))

    includes: list[str] = []
    defines: list[str] = []
    standard: str | None = None
    layout: list[str] = []
    index = 1  # argv[0] is the compiler binary
    count = len(argv)
    while index < count:
        token = argv[index]
        nxt = argv[index + 1] if index + 1 < count else None
        if token in {"-I", "-isystem", "-iquote", "/I"}:
            if nxt is not None:
                includes.append("-I" + absolute(nxt))
                index += 2
                continue
            index += 1
            continue
        if token.startswith("-I") and len(token) > 2:
            includes.append("-I" + absolute(token[2:]))
            index += 1
            continue
        if token.startswith("-isystem") and len(token) > len("-isystem"):
            includes.append("-I" + absolute(token[len("-isystem"):]))
            index += 1
            continue
        if token.startswith("/I") and len(token) > 2:
            includes.append("-I" + absolute(token[2:]))
            index += 1
            continue
        if token in {"-D", "/D"}:
            if nxt is not None:
                defines.append("-D" + nxt)
                index += 2
                continue
            index += 1
            continue
        if token.startswith("-D") and len(token) > 2:
            defines.append("-D" + token[2:])
            index += 1
            continue
        if token.startswith("/D") and len(token) > 2:
            defines.append("-D" + token[2:])
            index += 1
            continue
        if token.startswith("-std="):
            standard = token
            index += 1
            continue
        if token.startswith("/std:"):
            standard = "-std=" + token[len("/std:"):]
            index += 1
            continue
        if any(token == prefix or token.startswith(prefix) for prefix in LAYOUT_FLAG_PREFIXES):
            layout.append(token)
            index += 1
            continue
        if token in {"-o", "-MF", "-MT", "-MQ", "-Xclang", "-include"}:
            index += 2
            continue
        index += 1
    result: list[str] = list(includes) + list(defines)
    if standard is not None:
        result.append(standard)
    result.extend(layout)
    return result


def extract_cmake_compile_flags(build_path: str, headers: list[str]) -> list[str]:
    """Read ``compile_commands.json`` and union ABI-relevant flags.

    Commands whose include search path is an ancestor of one of the adapted
    headers are preferred; when none correspond, every command is unioned.  The
    result is a de-duplicated, order-preserving list of gnu-style flags.
    """
    directory = Path(build_path).expanduser()
    compile_db = directory / "compile_commands.json"
    if not compile_db.is_file():
        raise AdapterContractError(
            f"--cmake_build_path {build_path!r} does not contain compile_commands.json"
        )
    try:
        entries = json.loads(compile_db.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise AdapterContractError(f"cannot read {compile_db}: {error}") from error
    if not isinstance(entries, list):
        raise AdapterContractError(f"{compile_db} is not a compile_commands array")

    header_paths: list[Path] = []
    for header in headers:
        try:
            header_paths.append(Path(header).resolve())
        except OSError:
            continue

    preferred: list[list[str]] = []
    others: list[list[str]] = []
    for entry in entries:
        if not isinstance(entry, dict):
            continue
        argv = _compile_entry_argv(entry)
        if not argv:
            continue
        base_dir = str(entry.get("directory") or directory)
        flags = _normalize_compile_flags(argv, base_dir)
        if not flags:
            continue
        related = False
        for flag in flags:
            if not flag.startswith("-I"):
                continue
            try:
                include_dir = Path(flag[2:]).resolve()
            except OSError:
                continue
            if any(
                header_path == include_dir or header_path.is_relative_to(include_dir)
                for header_path in header_paths
            ):
                related = True
                break
        (preferred if related else others).append(flags)

    chosen = preferred or others
    seen: set[str] = set()
    union: list[str] = []
    for flags in chosen:
        for flag in flags:
            if flag not in seen:
                seen.add(flag)
                union.append(flag)
    return union


def load_compile_flags(values: list[str]) -> list[str]:
    """Resolve repeated ``--compile_flags`` file-or-flag arguments.

    An existing file is read as clangd-style one-flag-per-line input; anything
    else is treated as an inline flag string (which may hold several flags).
    """
    result: list[str] = []
    for value in values:
        path = Path(value).expanduser()
        if path.is_file():
            for line in path.read_text(encoding="utf-8").splitlines():
                stripped = line.strip()
                if not stripped or stripped.startswith("#"):
                    continue
                result.append(stripped)
            continue
        try:
            result.extend(shlex.split(value))
        except ValueError:
            result.append(value)
    return result


def select_cpp_frontends(
    selection: str,
    cxx: str | None,
    clang: str | None,
    target: str,
) -> tuple[cpp_toolchains.CppToolchain, cpp_toolchains.CppToolchain]:
    """Return (selected compiler, structured-fact extractor).

    The selected compiler is the sole ABI authority.  Clang is still returned
    as a structured parse/layout helper because the adapter reads Clang's
    AST/record-layout/vtable dumps, but its numbers never override the selected
    compiler and the two are never required to agree.
    """
    explicit = cxx
    if not explicit and cpp_toolchains.normalize_compiler_family(selection) == "clang":
        explicit = clang
    try:
        selected = cpp_toolchains.discover_cpp_toolchain(
            selection,
            executable=explicit,
            target=target,
        )
        if selected.family == "clang" and selected.driver_mode == "clang":
            extractor = selected
        else:
            extractor = cpp_toolchains.discover_cpp_toolchain(
                "clang",
                executable=clang if clang and clang != selected.executable else None,
                target=target,
            )
    except cpp_toolchains.CppToolchainError as error:
        raise AdapterContractError(str(error)) from error
    if extractor.driver_mode != "clang":
        raise AdapterContractError(
            "structured C++ fact extraction requires clang++ (not clang-cl); "
            "pass --clang <clang++ path>"
        )
    return selected, extractor


def normalize_dash_valued_options(argv: list[str], options: set[str]) -> list[str]:
    """Join ``--opt -value`` into ``--opt=-value`` so argparse keeps the value.

    Repeatable flag options such as ``--compile_flags`` legitimately take values
    that start with ``-`` (e.g. ``-Iinclude``); argparse would otherwise treat
    the value as an unknown option.
    """
    result: list[str] = []
    index = 0
    while index < len(argv):
        token = argv[index]
        if token == "--":
            result.extend(argv[index:])
            break
        if (
            token in options
            and index + 1 < len(argv)
            and argv[index + 1].startswith("-")
            and argv[index + 1] != "--"
        ):
            result.append(f"{token}={argv[index + 1]}")
            index += 2
            continue
        result.append(token)
        index += 1
    return result


def main(argv: list[str]) -> int:
    argv = normalize_dash_valued_options(
        argv, {"--compile_flags", "--compile-flags"}
    )
    parser = argparse.ArgumentParser(
        prog="dci-adapter-cpp",
        description=(
            "Generate a DCI v1.0 ABI descriptor for C++ using the selected "
            "compiler as the sole ABI authority (Clang assists parsing) for the "
            "target's Microsoft/Itanium ABI."
        ),
        epilog=(
            "examples:\n"
            "  %(prog)s --include native/lib.hpp --out contracts/native.dcib\n"
            "  %(prog)s --include native/lib.hpp --out native.dcib "
            "--export-instance \"Pair2<double, int>\" --clang clang++\n"
            "Trailing arguments after the options are forwarded to the Clang "
            "fact extractor (use \"--\" first if an argument starts with -)."
        ),
    )
    parser.add_argument("--out", required=True, help="output .dcib contract path")
    parser.add_argument("--boundary", choices=("no_unwind", "shared_abi"), default="no_unwind",
                        help="C++ exception boundary; shared_abi preserves native C++ exceptions")
    parser.add_argument("--borrow-return", "--borrow-template-return", dest="borrow_template_return",
                        action="append", default=[], metavar="OWNER::METHOD",
                        help="Explicitly declare a method's pointer/reference return borrowed (all instances of an open template)")
    parser.add_argument("--list-initializer", action="append", default=[], metavar="JSON",
                        help="Consumer-derived [owner, argument types] brace-constructor request")
    parser.add_argument("--debug-json-out", help="Optional human-readable JSON diagnostic output")
    parser.add_argument(
        "--stub-out",
        help="Write generated translate_unwind C++ stub source to this path",
    )
    parser.add_argument(
        "--include", action="append", default=[],
        help="public header to export; repeat as needed",
    )
    parser.add_argument(
        "--project-root",
        action="append",
        default=[],
        help="Root directory whose recursively included public headers are exported",
    )
    parser.add_argument(
        "--scan-public-root",
        action="store_true",
        help="Treat every .h/.hpp under --project-root as a public API root",
    )
    parser.add_argument(
        "--toolchain",
        default="clang",
        choices=("auto", "clang", "gcc", "msvc"),
        help="C++ frontend whose accepted public API is described",
    )
    parser.add_argument("--cxx", help="Selected clang++, g++ or cl.exe driver")
    parser.add_argument(
        "--clang",
        default=os.environ.get("CLANGXX") or None,
        help="clang++ fact extractor; also the legacy Clang selection option",
    )
    parser.add_argument(
        "--target", default="x86_64-pc-windows-msvc",
        help="LLVM-style target triple selecting the ABI (default: x86_64-pc-windows-msvc)",
    )
    parser.add_argument(
        "--std", default="c++17",
        help="C++ language standard for extraction (default: c++17)",
    )
    parser.add_argument(
        "-j",
        "--jobs",
        type=int,
        default=None,
        help="parallel Clang AST-dump workers (default: min(32, CPU); "
        "also DCI_ADAPTER_JOBS)",
    )
    parser.add_argument(
        "--cxx-arg",
        action="append",
        default=[],
        help="argument passed only to the selected GCC/MSVC validation frontend",
    )
    parser.add_argument(
        "--cmake_build_path",
        "--cmake-build-path",
        dest="cmake_build_path",
        help="CMake build directory containing compile_commands.json; its real "
        "include dirs, defines, standard and layout flags are applied",
    )
    parser.add_argument(
        "--compile_flags",
        "--compile-flags",
        dest="compile_flags",
        action="append",
        default=[],
        help="flag file (clangd compile_flags.txt style) or inline flag; repeatable",
    )
    parser.add_argument(
        "--artifact",
        action="append",
        default=[],
        help="native object/library represented by the contract; repeat as needed",
    )
    parser.add_argument(
        "--export-instance",
        dest="export_instance",
        action="append",
        default=[],
        help=(
            "concrete template instance (e.g. \"Pair2<double, int>\") whose "
            "layout is measured and whose member methods are exported as "
            "Active materialization requests (semantic_id) instead of "
            "provider-exported closed symbols; repeatable"
        ),
    )
    parser.add_argument("clang_args", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    global ADAPTER_JOBS
    ADAPTER_JOBS = args.jobs
    if ADAPTER_JOBS is not None and ADAPTER_JOBS < 1:
        parser.error("--jobs must be >= 1")
    if not args.include:
        parser.error("at least one --include header is required")
    extra = list(args.clang_args)
    if extra and extra[0] == "--":
        extra = extra[1:]
    target = cpp_toolchains.canonical_target(args.target)
    selected_toolchain, extractor = select_cpp_frontends(
        args.toolchain, args.cxx, args.clang, target
    )
    target_info = cpp_toolchains.validate_target_compatibility(
        selected_toolchain, target
    )
    global CPP_LONG_WIDTH, CPP_POINTER_WIDTH
    CPP_LONG_WIDTH = target_info.long_width
    CPP_POINTER_WIDTH = target_info.pointer_width
    include_roots = list(args.include)
    if args.scan_public_root:
        include_roots.extend(discover_public_header_roots(args.project_root))
    project_headers = discover_project_headers(include_roots, args.project_root)
    if not project_headers:
        project_headers = list(args.include)
    cmake_flags = (
        extract_cmake_compile_flags(args.cmake_build_path, list(args.include))
        if args.cmake_build_path
        else []
    )
    explicit_flags = load_compile_flags(list(args.compile_flags))
    effective_flags = cmake_flags + explicit_flags
    extra = list(extra) + effective_flags
    with tempfile.TemporaryDirectory(prefix="vyx_dci_") as td:
        probe = Path(td) / "dci_probe.cpp"
        global CPP_ENUM_UNDERLYING, CPP_TYPE_ALIAS_TARGETS
        CPP_ENUM_UNDERLYING = discover_enum_underlying(project_headers)
        aliases = discover_type_aliases(project_headers)
        CPP_TYPE_ALIAS_TARGETS = {
            normalize_cpp_type(alias.name): alias.target for alias in aliases
        }
        ownership_annotations = discover_ownership_annotations(project_headers)
        explicit_record_names = discover_record_names(project_headers)
        # Concrete template instances the header itself instantiates.  They are
        # records for the *symbol* scan (DCI_SPEC §12: a selected method of a
        # concrete instance exports its final symbol) *and* for measurement:
        # the instance was closed by the producer's own toolchain
        # (``template struct Pair2<int, double>;``), so its layout is a fact the
        # adapter is the authoritative collector of -- exactly the same kind of
        # fact whether it existed before the adapter started or only after
        # asking the producer compiler (Active Adapter design doc §1).
        #
        # Keeping instances out of the sizeof probe and the layout closure (the
        # earlier decision) left every instance-typed *value* without a layout,
        # because clang dumps a record layout only for records whose layout is
        # actually *required*: no ``sizeof`` mention, no dump entry -- so a
        # method returning another instance was rejected with "value return
        # type '...' has no verified nonzero layout".  That rejects a fact the
        # producer had already produced, which §12 and §5.2 do not allow.
        instance_record_names = discover_explicit_instantiation_names(project_headers)
        # --export-instance names instances the *project* commits to without
        # pinning an explicit instantiation into the header: the layout is
        # still measured (sizeof probe + -fdump-record-layouts below) but the
        # member methods are exported as Active materialization requests
        # (semantic_id) instead of provider-exported closed symbols -- the
        # same channel the rust contract uses.  The provider keeps pure
        # generic source; nothing in its artifact is pre-instantiated.
        exported_instance_names: list[str] = []
        for raw in args.export_instance:
            canonical = canonical_cpp_name(raw)
            if canonical and canonical not in exported_instance_names:
                exported_instance_names.append(canonical)
        for name in exported_instance_names:
            if name not in instance_record_names:
                instance_record_names.append(name)
        active_instance_owners = set(exported_instance_names)
        record_names = unique_names(
            explicit_record_names
            + [alias.name for alias in aliases]
            + [alias.target for alias in aliases]
        )
        free_names = discover_free_function_names(project_headers)
        include_text = "".join(f'#include "{h}"\n' for h in args.include)
        record_names = filter_sizeofable_record_names(
            extractor.executable,
            args.std,
            target,
            include_text,
            record_names,
            extra,
            Path(td),
        )
        # Measurement is the one place instances are covered.  `record_names`
        # keeps its declared-record meaning for the AST trait/lifecycle and enum
        # probe closure; instances are measured separately so that their
        # layouts reach the layout closure -- and therefore the contract -- as
        # *measured* facts rather than as a project's hand-written claim.
        measured_instance_names = filter_sizeofable_record_names(
            extractor.executable,
            args.std,
            target,
            include_text,
            instance_record_names,
            extra,
            Path(td),
        )
        measured_record_names = unique_names(record_names + measured_instance_names)
        text = include_text
        for idx, name in enumerate(measured_record_names):
            leaf = last_cpp_component_without_template_args(name)
            text += f'using vyx_dci_instance_{leaf}_{idx} = {name};\n'
            text += f'static_assert(sizeof({name}) >= 0, "vyx_dci_{idx}");\n'
        probe.write_text(text, encoding="utf-8")
        native_compiler_args = frontend_validation_args(
            selected_toolchain, list(args.cxx_arg), extra
        )
        validate_translation_unit_with_selected_compiler(
            selected_toolchain,
            args.std,
            probe,
            native_compiler_args,
            target,
        )
        layout_dump = run([
            extractor.executable,
            f"-std={args.std}",
            "-target",
            target,
            "-Xclang",
            "-fdump-record-layouts",
            "-fsyntax-only",
            str(probe),
            *extra,
        ])
        # A. Close the parsed layouts over nested class-typed fields/bases so a
        #    field type such as ``fmt::basic_string_view<char>`` gets its own
        #    RecordLayout and Vyx can read its members.  The closure feeds record
        #    layouts only; the exported *symbol* surface stays the public API so
        #    std/fmt internals are not dragged into the contract.  Traits for the
        #    by-value nested aggregates are recovered from the selected compiler
        #    by the Itanium CodeGen probe below.
        layouts, _closure_record_names = expand_layout_closure(
            layout_dump, measured_record_names
        )
        # B. Discover enum underlying integers with the selected compiler (the
        #    ABI authority) from every not-yet-resolved class-like field type,
        #    before the AST scan turns symbol parameters into DCI types.
        enum_underlying = discover_enum_underlying_with_selected_compiler(
            selected_toolchain,
            args.std,
            target,
            args.include,
            collect_enum_probe_candidates([], layouts),
            native_compiler_args,
        )
        if enum_underlying:
            CPP_ENUM_UNDERLYING.update(enum_underlying)
        symbols, contracts = emit_ast_scan(
            extractor.executable,
            args.std,
            target,
            probe,
            extra,
            record_names,
            free_names,
            instance_record_names,
        )
        # Methods of --export-instance instances describe a *request*, not a
        # provider export: the producer header never instantiated them, so
        # the definition does not exist in any artifact yet.  semantic_id is
        # what turns the contract entry into an Active materialization
        # request at consumer build time (stub backend / active session);
        # link_name/ABI stay the final MSVC-mangled member symbol, which is
        # what the consumer's call site references either way.
        for sym in symbols:
            if sym.kind not in {"method", "constructor", "destructor"} or not sym.owner:
                continue
            owner = canonical_cpp_name(sym.owner)
            if owner not in active_instance_owners:
                continue
            sym.native_template = True
            if sym.kind == "method":
                base = owner.split("<", 1)[0]
                dci_args = ",".join(_cpp_arg_to_dci(arg) for arg in _split_template_args(owner))
                sym.semantic_id = f"dci.active.cpp.{base}::{sym.member_name}({dci_args})"
        for raw_request in args.list_initializer:
            try:
                owner, arguments = json.loads(raw_request)
                if not isinstance(owner, str) or not isinstance(arguments, list) or not all(isinstance(t, str) and t for t in arguments):
                    raise ValueError("expected an owner and argument type list")
            except (ValueError, TypeError) as exc:
                raise AdapterContractError(f"invalid list initializer request: {exc}") from exc
            owner = canonical_cpp_name(owner)
            if owner not in active_instance_owners or not isinstance(arguments, list):
                raise AdapterContractError("list initializer must name an actually requested template instance")
            try:
                from .cpp_materialize import member_source
            except ImportError:
                from cpp_materialize import member_source
            link = "dci_list_" + hashlib.sha256(json.dumps([owner, arguments]).encode()).hexdigest()[:32]
            ctor = Symbol(name=owner + "::constructor", owner=owner, member_name="constructor", mangled=link,
                          kind="constructor", calling_convention="cxx_constructor", unwind="may_unwind",
                          params=[{"name": f"p{i}", "type": dci_type(ty, owner), "location": "abi"}
                                  for i, ty in enumerate(arguments)], ret=None,
                          native_template=True, list_arguments=arguments)
            # Admit only a brace expression accepted by the producer. It
            # decides overloads, narrowing and default allocator selection.
            check_payload = symbol_json(ctor, layouts, target)
            list_probe = Path(td) / (link + ".cpp")
            list_probe.write_text(include_text + "#include <new>\n#include <utility>\n" + "\n".join(member_source(check_payload, 0)), encoding="utf-8")
            validate_translation_unit_with_selected_compiler(selected_toolchain, args.std, list_probe,
                                                               native_compiler_args, target, force=True)
            symbols.append(ctor)
        # B (cont). Catch enums that only appear as symbol parameters/returns,
        #    then re-derive those types now that the underlying integers are known.
        symbol_enum_underlying = discover_enum_underlying_with_selected_compiler(
            selected_toolchain,
            args.std,
            target,
            args.include,
            collect_enum_probe_candidates(symbols, layouts),
            native_compiler_args,
        )
        if symbol_enum_underlying:
            CPP_ENUM_UNDERLYING.update(symbol_enum_underlying)
        rewrite_enum_symbol_types(symbols)
        # A (cont). Give the nested closure records their AST trait/lifecycle
        #    facts.  Without them a by-value parameter such as
        #    ``fmt::basic_string_view<char>`` (``logger::log``) has no executable
        #    copy/move contract and is rejected before ABI lowering is reached.
        #    Only records that appear as by-value parameters/returns need this;
        #    scanning just those keeps std/fmt internals off the symbol surface.
        closure_only = [
            name
            for name in _closure_record_names
            if not record_allowed(name, set(record_names))
        ]
        by_value_closure = [
            name
            for name in closure_only
            if any(
                names_match(aggregate, name)
                for aggregate in collect_by_value_aggregate_names(symbols, layouts)
            )
        ]
        if by_value_closure:
            nested_contracts = emit_ast_record_contracts(
                extractor.executable,
                args.std,
                target,
                probe,
                extra,
                by_value_closure,
            )
            for owner, contract in nested_contracts.items():
                contracts.setdefault(owner, contract)
        apply_record_contracts(layouts, contracts)
        unverified_records = measure_record_layout_with_selected_compiler(
            selected_toolchain,
            args.std,
            target,
            args.include,
            layouts,
            native_compiler_args,
        )
        if unverified_records:
            layouts = [
                record
                for record in layouts
                if record.type_name not in unverified_records
            ]
        append_vtable_force_uses(probe, symbols)
        vtable_dump = run([
            extractor.executable,
            f"-std={args.std}",
            "-target",
            target,
            "-Xclang",
            "-fdump-vtable-layouts",
            "-c",
            str(probe),
            "-o",
            str(Path(td) / "dci_probe.obj"),
            *extra,
        ])
        # Template instances dump their vtables under the bare template name;
        # allow the bare primary-template names (so the sections survive
        # parsing), the instance spellings desugared the way the dump prints
        # them, and then retitle the parsed tables to the canonical instance
        # names.
        instance_bare_names = {
            canonical_cpp_name(name).split("<", 1)[0]
            for name in instance_record_names
            if "<" in name
        }
        vtable_allowed = set(record_names) | instance_bare_names | {
            normalize_record_name(desugar_dump_spelling(canonical_cpp_name(name)))
            for name in instance_record_names
        }
        vtables = parse_vtable_layouts(
            vtable_dump,
            vtable_allowed,
            symbols,
            pointer_width=target_info.pointer_width,
        )
        vtables = retitle_template_instance_vtables(
            vtables, instance_record_names, symbols,
        )
        dtor_symbols = direct_destructor_symbols(
            extractor.executable,
            args.std,
            target,
            project_headers,
            extra,
            destructor_probe_owners(symbols),
        )
        patch_direct_destructor_symbols(symbols, dtor_symbols)
        if target_info.abi_family == "msvc":
            apply_virtual_base_adjustments(
                extractor.executable,
                args.std,
                target,
                project_headers,
                extra,
                layouts,
            )
        layouts = copy_layout_for_aliases(layouts, aliases)
        symbols = copy_symbols_for_aliases(symbols, aliases)
        vtables = copy_vtables_for_aliases(vtables, aliases)
        apply_ownership_annotations(symbols, ownership_annotations)
        for sym in symbols:
            if sym.owner and sym.kind == "method" and sym.ret is not None:
                key = sym.owner.split("<", 1)[0] + "::" + sym.member_name
                if key in args.borrow_template_return:
                    sym.return_ownership = "borrow"
        link_lifecycle_symbols(layouts, symbols)
        rtti_enabled = not any(arg in {"-fno-rtti", "/GR-"} for arg in extra)
        # C. Class aggregates passed/returned by value have no ABI lowering
        #    until the selected compiler's CodeGen dump names the machine
        #    types.  Probe every by-value aggregate (Clang LLVM IR, GCC ``-S``,
        #    MSVC ``/FA``).  Copy those types into the Descriptor.  Nontrivial
        #    aggregates still fail closed.
        machine_signatures: dict[str, dict[str, Any]] = {}
        aggregate_names = collect_by_value_aggregate_names(symbols, layouts)
        if aggregate_names:
            machine_signatures = probe_itanium_aggregate_signatures(
                selected_toolchain,
                args.std,
                target,
                args.include,
                aggregate_names,
                native_compiler_args,
            )
        write_abi(
            Path(args.out),
            target,
            project_headers,
            layouts,
            symbols,
            vtables,
            aliases,
            rtti_enabled,
            Path(args.debug_json_out) if args.debug_json_out else None,
            selected_toolchain,
            extractor,
            compiler_flags=effective_flags,
            unverified_records=unverified_records,
            machine_signatures=machine_signatures,
            stub_source_path=(
                Path(args.stub_out)
                if args.stub_out
                else Path(args.out).with_name(Path(args.out).stem + "_translate_unwind.cpp")
            ),
            artifacts=list(args.artifact),
            boundary=args.boundary,
            borrowed_returns=args.borrow_template_return,
            cxx_standard=args.std,
            closure_settings={"toolchain": selected_toolchain.family,
                              "cxx": selected_toolchain.executable,
                              "clang": extractor.executable,
                              "clang_flags": extra,
                              "cxx_flags": list(args.cxx_arg)},
        )
    return 0


def cli(argv: list[str] | None = None) -> int:
    try:
        return main(list(sys.argv[1:] if argv is None else argv))
    except (AdapterContractError, cpp_toolchains.CppToolchainError) as error:
        print(f"dci-cpp: error: {error}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("dci-cpp: interrupted", file=sys.stderr)
        return 130


if __name__ == "__main__":
    raise SystemExit(cli())
