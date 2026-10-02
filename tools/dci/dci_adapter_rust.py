#!/usr/bin/env python3
"""Rust stable-ABI Adapter for Declarative Code Interface contracts.

The Adapter treats the selected ``rustc`` as the sole ABI authority for this
contract.  ``repr(C)`` / ``extern \"C\"`` remain the C-profile subset; default
``struct`` layout, ``extern \"Rust\"`` lowering, and ``dyn Trait`` vtables are
measured from that same rustc rather than guessed.  Rebuild the contract when
rustc changes.

Exported when rustc can prove the facts:

* ``#[repr(C)]`` / ``#[repr(transparent)]`` value types;
* default-repr structs whose fields themselves have a rustc layout probe;
* fieldless ``#[repr(C)]``/integer-repr enums;
* ``pub extern \"C\"`` / ``pub extern \"system\"`` / ``pub extern \"Rust\"``
  functions, and ordinary ``pub fn`` / inherent ``pub`` methods; ``no_mangle``
  and ``export_name`` keep their chosen link identity, otherwise the Adapter
  copies the mangled symbol rustc actually emitted for this crate;
* ``dyn Trait`` fat-pointer size and vtable slots for in-crate impls;
* thin ``&T`` / ``&mut T`` (one pointer) and fat ``&[T]`` / ``&str`` /
  ``&dyn Trait`` (two words; ``extern "C"`` packs them, ``extern "Rust"``
  splits them), as measured by the same rustc;
* fieldless default-repr enums whose size rustc can prove;
* primitives, raw pointers, and C/system function pointers.

Layout and aggregate lowering are not guessed.  A temporary copy of the crate
root is instrumented with layout/ABI probes and compiled to LLVM IR by the same
``rustc`` and for the same target as the requested contract.  Unsupported or
ambiguous declarations are emitted as rejected symbols; they never leak into
the executable ABI contract.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass, field
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import tomllib
from typing import Any, Sequence
import uuid


TOOL_DIR = Path(__file__).resolve().parent
if str(TOOL_DIR) not in sys.path:
    sys.path.insert(0, str(TOOL_DIR))

import dcib  # noqa: E402
from rust_cargo import CargoContext, CargoError, CargoRustcArgs  # noqa: E402
from rust_views import measure_slice_views  # noqa: E402


ADAPTER_NAME = "vyx-rust-dci-adapter"
ADAPTER_VERSION = "1.3"
RE_OWNERSHIP_BLOCK = re.compile(
    r"/\*\s*dci-ownership\s*(\{.*?\})\s*dci-ownership-end\s*\*/", re.DOTALL
)
RE_LIFECYCLE_BLOCK = re.compile(
    r"/\*\s*dci-lifecycle\s*(\{.*?\})\s*dci-lifecycle-end\s*\*/", re.DOTALL
)
RE_STUB_BLOCK = re.compile(
    r"/\*\s*dci-stub\s*(\{.*?\})\s*dci-stub-end\s*\*/", re.DOTALL
)
RUST_STABLE_ABIS = {"c", "system", "c-unwind"}
RUST_EXPORT_ABIS = {"c", "system", "c-unwind", "rust"}
# rustc 1.x trait-object vtable prefix measured by this Adapter's probes:
# [drop_in_place, size, align, ...methods in trait declaration order].
RUST_VTABLE_HEADER_SLOTS = 3
STUB_REQUEST_KINDS = {"operation_wrapper", "cast_wrapper", "reverse_override"}
PARAMETER_OWNERSHIP_VALUES = {"borrow", "borrow_mut", "copy", "move", "share", "out"}
RETURN_OWNERSHIP_VALUES = {"borrow", "copy", "move", "share", "owned"}
OWNERSHIP_MODEL_VALUES = {"value", "unique", "shared", "borrowed", "external", "manual", "linear"}
COPY_SEMANTICS_VALUES = {"forbidden", "trivial", "operation", "retain", "shared"}
MOVE_SEMANTICS_VALUES = {"forbidden", "trivial", "operation", "relocate"}
DESTRUCTION_VALUES = {"none", "trivial", "operation", "release", "external"}
MOVED_FROM_VALUES = {"valid", "destructible_only", "zeroed", "unspecified"}
# copy_semantics / move_semantics / destruction value -> required operation name.
LIFECYCLE_REQUIRED_OPERATION = {
    ("copy_semantics", "operation"): "copy",
    ("copy_semantics", "retain"): "retain",
    ("move_semantics", "operation"): "move",
    ("move_semantics", "relocate"): "move",
    ("destruction", "operation"): "destroy",
    ("destruction", "release"): "release",
}

RUST_TARGET_ALIASES = {
    "x64_windows": "x86_64-pc-windows-msvc",
    "windows_x64": "x86_64-pc-windows-msvc",
    "arm64_windows": "aarch64-pc-windows-msvc",
    "windows_arm64": "aarch64-pc-windows-msvc",
    "x64_linux": "x86_64-unknown-linux-gnu",
    "linux_x64": "x86_64-unknown-linux-gnu",
    "arm64_linux": "aarch64-unknown-linux-gnu",
    "linux_arm64": "aarch64-unknown-linux-gnu",
    "arm64_android": "aarch64-linux-android",
    "android_arm64": "aarch64-linux-android",
    "x64_android": "x86_64-linux-android",
    "android_x64": "x86_64-linux-android",
    "x86_android": "i686-linux-android",
    "android_x86": "i686-linux-android",
    "arm_android": "armv7-linux-androideabi",
    "android_arm": "armv7-linux-androideabi",
}


class RustAdapterError(RuntimeError):
    """A producer-side error that must stop contract generation."""


@dataclass(frozen=True)
class Token:
    value: str
    kind: str = "punct"


@dataclass
class Attribute:
    tokens: list[Token]

    @property
    def text(self) -> str:
        return tokens_text(self.tokens)


@dataclass
class RustType:
    kind: str
    spelling: str
    name: str = ""
    mutable: bool = False
    element: "RustType | None" = None
    length: int | None = None
    params: list["RustType"] = field(default_factory=list)
    result: "RustType | None" = None
    abi: str = ""
    nullable: bool = False
    decl_path: tuple[str, ...] | None = None
    fat: bool = False


@dataclass
class FieldDecl:
    name: str
    type_tokens: list[Token]
    public: bool


@dataclass
class RecordDecl:
    path: tuple[str, ...]
    fields: list[FieldDecl]
    public: bool
    representation: str
    explicit_align: int | None = None
    packed: int | None = None
    generic: bool = False
    tuple_struct: bool = False
    source: Path | None = None
    cfg_attributes: list[str] = field(default_factory=list)
    type_params: list[str] = field(default_factory=list)
    const_or_lifetime_params: bool = False
    # A monomorphized instance carries the concrete rustc path (turbofish) used
    # for layout probing and the base identity it was closed from.  These are
    # only set for adapter-synthesized closed instances.
    probe_path: str | None = None
    instance_of: tuple[str, ...] | None = None
    instance_args: tuple[str, ...] = ()
    opaque: bool = False

    @property
    def rust_path(self) -> str:
        if self.probe_path is not None:
            return self.probe_path
        return "crate::" + "::".join(self.path)


@dataclass
class EnumDecl:
    path: tuple[str, ...]
    variants: list[str]
    public: bool
    representation: str
    integer_repr: str = ""
    fieldless: bool = True
    generic: bool = False
    source: Path | None = None
    cfg_attributes: list[str] = field(default_factory=list)
    type_params: list[str] = field(default_factory=list)
    const_or_lifetime_params: bool = False
    probe_path: str | None = None
    instance_of: tuple[str, ...] | None = None
    instance_args: tuple[str, ...] = ()

    @property
    def rust_path(self) -> str:
        if self.probe_path is not None:
            return self.probe_path
        return "crate::" + "::".join(self.path)


@dataclass
class AliasDecl:
    path: tuple[str, ...]
    target_tokens: list[Token]
    public: bool
    generic: bool = False


@dataclass
class ParamDecl:
    name: str
    type_tokens: list[Token]


@dataclass
class FunctionDecl:
    path: tuple[str, ...]
    params: list[ParamDecl]
    return_tokens: list[Token]
    public: bool
    abi: str
    export_name: str
    has_stable_link_name: bool
    generic: bool
    variadic: bool
    source: Path | None = None
    cfg_attributes: list[str] = field(default_factory=list)
    type_params: list[str] = field(default_factory=list)
    resolution_context: tuple[str, ...] | None = None

    @property
    def name(self) -> str:
        return self.path[-1]

    @property
    def type_context(self) -> tuple[str, ...]:
        return self.path[:-1] if self.resolution_context is None else self.resolution_context


@dataclass
class GenericMethodDecl:
    """A method declared inside an `impl` of a *generic* type.

    `impl<A, B> Pair2<A, B> { pub fn get_first(&self) -> A }` has no
    standalone ABI -- the receiver's type arguments are open, exactly like a
    free `fn get_first<A, B>(v: &Pair2<A, B>) -> A` would be.  It therefore
    lives in its own table instead of `crate.functions`, and is reachable only
    through an *instance* request (`Pair2::<f64, i32>::get_first`), which the
    Active Adapter closes with rustc -- the same route `swap<T, U>` already
    takes.  Keeping it out of `crate.functions` also preserves the adapter's
    existing contract export surface byte for byte.
    """

    owner: tuple[str, ...]
    generics: list[str]
    owner_args: list[str]
    method: FunctionDecl

    @property
    def name(self) -> str:
        return self.method.path[-1]


@dataclass
class InstanceMethodDecl:
    """A closed `<Instance>::<method>` entry derived from a GenericMethodDecl.

    ``--export-instance Pair2<f64, i32>`` plus the open
    ``impl<A, B> Pair2<A, B> { fn get_first(&self) -> A }`` produces one of
    these per (instance, method): the impl's generic parameters substituted
    with the instance's arguments, so the signature has a rustc-verifiable ABI.
    The method itself still has no symbol in the provider artifact -- the
    contract exports it as an Active request (`semantic_id`) and the shim
    materializes it -- but its *types* are closed, which is what lets the
    contract carry measured layouts instead of hand-written claims.
    """

    owner_name: str              # DCI instance name, e.g. "Pair2<f64, i32>"
    base_name: str               # plain generic base, e.g. "Pair2"
    member: str                  # e.g. "get_first"
    arg_dci_names: list[str]     # e.g. ["f64", "i32"]
    params: list[ParamDecl]      # substituted; params[0] is the receiver
    return_tokens: list[Token]   # substituted
    semantic_id: str             # e.g. "dci.active.rust.Pair2::get_first(f64,i32)"
    link_name: str               # e.g. "vyx_pair2_f64_i32_get_first"


@dataclass
class TraitMethod:
    name: str
    params: list[ParamDecl]
    return_tokens: list[Token]


@dataclass
class TraitDecl:
    path: tuple[str, ...]
    methods: list[TraitMethod]
    public: bool
    source: Path | None = None
    type_params: list[str] = field(default_factory=list)
    const_or_lifetime_params: bool = False
    # A monomorphized *instance* of a generic trait carries the rustc path it is
    # named by (turbofish) plus the base identity it was closed from, exactly
    # like `RecordDecl`.  Only adapter-synthesized instances set these.
    probe_path: str | None = None
    instance_of: tuple[str, ...] | None = None
    instance_args: tuple[str, ...] = ()

    @property
    def rust_path(self) -> str:
        if self.probe_path is not None:
            return self.probe_path
        return "crate::" + "::".join(self.path)

    @property
    def generic(self) -> bool:
        return bool(self.type_params)


@dataclass
class TraitImpl:
    trait_path: tuple[str, ...]
    type_path: tuple[str, ...]
    source: Path | None = None


@dataclass
class ParsedCrate:
    records: dict[tuple[str, ...], RecordDecl] = field(default_factory=dict)
    enums: dict[tuple[str, ...], EnumDecl] = field(default_factory=dict)
    aliases: dict[tuple[str, ...], AliasDecl] = field(default_factory=dict)
    imports: dict[tuple[str, ...], tuple[str, ...]] = field(default_factory=dict)
    functions: list[FunctionDecl] = field(default_factory=list)
    # Methods of `impl<...> Generic<...>` blocks: no standalone ABI, closed
    # only through an instance request (see `GenericMethodDecl`).
    generic_methods: list[GenericMethodDecl] = field(default_factory=list)
    traits: dict[tuple[str, ...], TraitDecl] = field(default_factory=dict)
    trait_impls: list[TraitImpl] = field(default_factory=list)
    drop_types: set[tuple[str, ...]] = field(default_factory=set)
    files: list[Path] = field(default_factory=list)
    generated_source: str = ""


@dataclass
class TargetInfo:
    triple: str
    architecture: str
    pointer_width: int
    endianness: str
    os: str
    environment: str
    object_format: str
    abi_family: str


@dataclass
class LayoutFact:
    size: int
    alignment: int
    offsets: list[int]


@dataclass
class ProbeLowering:
    passing: str
    size: int
    alignment: int
    registers: list[str] = field(default_factory=list)
    coerce_to: str = ""
    attributes: list[str] = field(default_factory=list)


@dataclass
class ProbeFact:
    layout: LayoutFact
    param: ProbeLowering
    returned: ProbeLowering
    rust_param: ProbeLowering | None = None
    rust_returned: ProbeLowering | None = None


MULTI_PUNCT = (
    "::", "->", "=>", "..=", "...", "..", "<<=", ">>=", "==", "!=",
    "<=", ">=", "&&", "||", "+=", "-=", "*=", "/=", "%=", "&=", "|=",
    "^=", "<<", ">>",
)


def lex_rust(text: str) -> list[Token]:
    """Tokenize enough Rust syntax for declarations while ignoring comments."""
    result: list[Token] = []
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
        if ch.isspace():
            i += 1
            continue
        if text.startswith("//", i):
            end = text.find("\n", i + 2)
            i = n if end < 0 else end + 1
            continue
        if text.startswith("/*", i):
            depth = 1
            i += 2
            while i < n and depth:
                if text.startswith("/*", i):
                    depth += 1
                    i += 2
                elif text.startswith("*/", i):
                    depth -= 1
                    i += 2
                else:
                    i += 1
            if depth:
                raise RustAdapterError("unterminated Rust block comment")
            continue
        if ch in {'"', "'"}:
            # A leading apostrophe followed by an identifier is a lifetime, not
            # a character literal.
            if ch == "'" and i + 1 < n and (text[i + 1].isalpha() or text[i + 1] == "_"):
                j = i + 2
                while j < n and (text[j].isalnum() or text[j] == "_"):
                    j += 1
                if j >= n or text[j] != "'":
                    result.append(Token(text[i:j], "lifetime"))
                    i = j
                    continue
            quote = ch
            j = i + 1
            escaped = False
            while j < n:
                current = text[j]
                if escaped:
                    escaped = False
                elif current == "\\":
                    escaped = True
                elif current == quote:
                    j += 1
                    break
                j += 1
            if j > n or text[j - 1] != quote:
                raise RustAdapterError("unterminated Rust literal")
            result.append(Token(text[i:j], "string" if quote == '"' else "char"))
            i = j
            continue
        if ch == "r" and i + 1 < n and text[i + 1] in {'"', '#'}:
            # Raw string literals and raw identifiers.  Raw strings are rare in
            # ABI declarations but handling them avoids corrupting the token
            # stream around attributes such as export_name.
            raw_match = re.match(r'r(#{0,})"', text[i:])
            if raw_match:
                hashes = raw_match.group(1)
                marker = '"' + hashes
                start = i + len(raw_match.group(0))
                end = text.find(marker, start)
                if end < 0:
                    raise RustAdapterError("unterminated Rust raw string")
                end += len(marker)
                result.append(Token(text[i:end], "string"))
                i = end
                continue
            ident_match = re.match(r"r#[A-Za-z_][A-Za-z0-9_]*", text[i:])
            if ident_match:
                raw = ident_match.group(0)
                result.append(Token(raw[2:], "ident"))
                i += len(raw)
                continue
        if ch.isalpha() or ch == "_":
            j = i + 1
            while j < n and (text[j].isalnum() or text[j] == "_"):
                j += 1
            result.append(Token(text[i:j], "ident"))
            i = j
            continue
        if ch.isdigit():
            j = i + 1
            while j < n and (text[j].isalnum() or text[j] in "_xob."):
                j += 1
            result.append(Token(text[i:j], "number"))
            i = j
            continue
        matched = next((value for value in MULTI_PUNCT if text.startswith(value, i)), None)
        if matched:
            result.append(Token(matched))
            i += len(matched)
            continue
        result.append(Token(ch))
        i += 1
    return result


def substitute_type_tokens(
    tokens: Sequence[Token], substitution: dict[str, list[Token]]
) -> list[Token]:
    """Replace bare type-parameter identifiers with their concrete argument.

    The substitution is token-level and intentionally conservative: a parameter
    name is replaced only where it stands alone as a type token, so field types
    such as ``T``, ``*const T`` and ``[T; 4]`` monomorphize while unrelated
    identifiers are preserved.  Anything rustc cannot then compile fails closed
    when the layout probe is built.
    """
    result: list[Token] = []
    for token in tokens:
        if token.kind == "ident" and token.value in substitution:
            result.extend(substitution[token.value])
        else:
            result.append(token)
    return result


def tokens_text(tokens: Sequence[Token]) -> str:
    if not tokens:
        return ""
    out: list[str] = []
    previous = ""
    for token in tokens:
        value = token.value
        if out and (
            (previous[-1:].isalnum() or previous.endswith("_"))
            and (value[:1].isalnum() or value.startswith("_"))
        ):
            out.append(" ")
        out.append(value)
        previous = value
    return "".join(out)


def split_top(tokens: Sequence[Token], separator: str = ",") -> list[list[Token]]:
    result: list[list[Token]] = []
    current: list[Token] = []
    stack: list[str] = []
    pairs = {"(": ")", "[": "]", "{": "}", "<": ">"}
    for token in tokens:
        value = token.value
        if value in pairs:
            stack.append(pairs[value])
        elif stack and value == stack[-1]:
            stack.pop()
        elif value == ">>" and stack and stack[-1] == ">":
            stack.pop()
            if stack and stack[-1] == ">":
                stack.pop()
        if value == separator and not stack:
            result.append(current)
            current = []
        else:
            current.append(token)
    if current or result:
        result.append(current)
    return result


def matching_index(tokens: Sequence[Token], start: int) -> int:
    opening = tokens[start].value
    # Angle brackets are delimiters only when the caller explicitly starts at
    # a generic argument list.  Treating every comparison operator in a
    # function body as a generic opener makes an otherwise balanced body appear
    # unterminated (``if value < 0 { ... }``).
    pairs = {"(": ")", "[": "]", "{": "}"}
    if opening == "<":
        pairs["<"] = ">"
    if opening not in pairs:
        raise RustAdapterError(f"internal parser error: {opening!r} is not an opener")
    stack = [pairs[opening]]
    i = start + 1
    while i < len(tokens):
        value = tokens[i].value
        if value in pairs:
            stack.append(pairs[value])
        elif value == stack[-1]:
            stack.pop()
            if not stack:
                return i
        elif value == ">>" and stack[-1] == ">":
            stack.pop()
            if stack and stack[-1] == ">":
                stack.pop()
            if not stack:
                return i
        i += 1
    raise RustAdapterError(f"unbalanced Rust delimiter {opening!r}")


def unquote_rust_string(value: str) -> str:
    if value.startswith('r'):
        match = re.fullmatch(r'r(#{0,})"(.*)"\1', value, re.DOTALL)
        if match:
            return match.group(2)
    try:
        return json.loads(value)
    except json.JSONDecodeError as exc:
        raise RustAdapterError(f"unsupported Rust string literal {value}") from exc


def attribute_name(attribute: Attribute) -> str:
    tokens = attribute.tokens
    if not tokens:
        return ""
    return tokens[0].value


def parse_generic_impl_header(
    header: Sequence[Token],
) -> tuple[tuple[str, ...], list[str], list[str]] | None:
    """Split an `impl` header into (owner path, impl generics, owner args).

    `header` is the token run *between* the `impl` keyword and the block, so
    ``<A, B> Pair2<A, B>`` -> ``(('Pair2',), ['A', 'B'], ['A', 'B'])`` and
    ``Pair2<f64, i32>`` -> ``(('Pair2',), [], ['f64', 'i32'])``.

    Returns ``None`` for headers this profile does not read, so the caller's
    pre-existing behaviour (skip the block) is untouched.
    """
    text = re.sub(r"\s+", "", tokens_text(header))
    match = re.match(
        r"(?:<([^>]*)>)?([A-Za-z_][A-Za-z0-9_:]*)(?:<([^>]*)>)?", text)
    if match is None or not match.group(2):
        return None
    generics = [
        item.split(":")[0].strip()
        for item in (match.group(1) or "").split(",")
        if item.split(":")[0].strip()
    ]
    owner = tuple(part for part in match.group(2).split("::")
                  if part not in {"crate", "self"})
    if not owner:
        return None
    owner_args = [item.strip() for item in (match.group(3) or "").split(",")
                  if item.strip()]
    return owner, generics, owner_args


def rewrite_receiver_params(params: list[ParamDecl], owner: str) -> list[ParamDecl]:
    """Turn ``self`` / ``&self`` / ``&mut self`` into the impl's concrete type."""
    rewritten: list[ParamDecl] = []
    for index, param in enumerate(params):
        tokens = [
            Token(owner, "ident") if token.value == "Self" else token
            for token in param.type_tokens
        ]
        compact = re.sub(r"'[A-Za-z_][A-Za-z0-9_]*", "", re.sub(r"\s+", "", tokens_text(tokens)))
        if index == 0 and compact == "self":
            rewritten.append(ParamDecl("self", lex_rust(owner)))
            continue
        if index == 0 and compact == "&self":
            rewritten.append(ParamDecl("self", lex_rust("& " + owner)))
            continue
        if index == 0 and compact == "&mutself":
            rewritten.append(ParamDecl("self", lex_rust("&mut " + owner)))
            continue
        rewritten.append(ParamDecl(param.name if compact != "self" else "self", tokens))
    return rewritten


def has_no_mangle(attributes: Sequence[Attribute]) -> bool:
    for attribute in attributes:
        text = re.sub(r"\s+", "", attribute.text)
        if text == "no_mangle" or text == "unsafe(no_mangle)":
            return True
    return False


def export_name_attribute(attributes: Sequence[Attribute]) -> str:
    for attribute in attributes:
        tokens = attribute.tokens
        if len(tokens) >= 3 and tokens[0].value == "export_name" and tokens[1].value == "=":
            return unquote_rust_string(tokens_text(tokens[2:]))
        text = re.sub(r"\s+", "", attribute.text)
        match = re.fullmatch(r'unsafe\(export_name=(.+)\)', text)
        if match:
            return unquote_rust_string(match.group(1))
    return ""


def parse_repr(attributes: Sequence[Attribute]) -> tuple[str, str, int | None, int | None]:
    representation = ""
    integer_repr = ""
    explicit_align: int | None = None
    packed: int | None = None
    integer_reprs = {"u8", "u16", "u32", "u64", "u128", "usize",
                     "i8", "i16", "i32", "i64", "i128", "isize"}
    for attribute in attributes:
        tokens = attribute.tokens
        if not tokens or tokens[0].value != "repr" or len(tokens) < 3 or tokens[1].value != "(":
            continue
        inner = tokens[2:-1] if tokens[-1].value == ")" else tokens[2:]
        for component in split_top(inner):
            text = re.sub(r"\s+", "", tokens_text(component))
            if text == "C":
                representation = "stable"
            elif text == "transparent":
                representation = "transparent"
            elif text in integer_reprs:
                integer_repr = text
            else:
                match = re.fullmatch(r"align\((\d+)\)", text)
                if match:
                    explicit_align = int(match.group(1))
                    continue
                match = re.fullmatch(r"packed(?:\((\d+)\))?", text)
                if match:
                    packed = int(match.group(1) or "1")
    return representation, integer_repr, explicit_align, packed


class RustParser:
    def __init__(self, root: Path):
        self.root = root.resolve()
        self.crate = ParsedCrate()
        self.visited: set[Path] = set()

    def parse(self) -> ParsedCrate:
        self._parse_file(self.root, ())
        return self.crate

    def _parse_file(self, path: Path, module: tuple[str, ...]) -> None:
        resolved = path.resolve()
        if resolved in self.visited:
            return
        if not resolved.is_file():
            raise RustAdapterError(f"Rust module source not found: {resolved}")
        self.visited.add(resolved)
        self.crate.files.append(resolved)
        try:
            text = resolved.read_text(encoding="utf-8")
        except (OSError, UnicodeError) as exc:
            raise RustAdapterError(f"cannot read Rust source {resolved}: {exc}") from exc
        self._parse_items(lex_rust(text), module, resolved)

    def _attributes(self, tokens: Sequence[Token], index: int) -> tuple[list[Attribute], int]:
        attributes: list[Attribute] = []
        i = index
        while i + 1 < len(tokens) and tokens[i].value == "#" and tokens[i + 1].value in {"[", "!"}:
            if tokens[i + 1].value == "!":
                if i + 2 >= len(tokens) or tokens[i + 2].value != "[":
                    break
                start = i + 2
            else:
                start = i + 1
            end = matching_index(tokens, start)
            attributes.append(Attribute(list(tokens[start + 1:end])))
            i = end + 1
        return attributes, i

    @staticmethod
    def _visibility(tokens: Sequence[Token], index: int) -> tuple[bool, int]:
        if index >= len(tokens) or tokens[index].value != "pub":
            return False, index
        i = index + 1
        if i < len(tokens) and tokens[i].value == "(":
            i = matching_index(tokens, i) + 1
        return True, i

    @staticmethod
    def _generic_end(tokens: Sequence[Token], index: int) -> tuple[bool, int]:
        if index < len(tokens) and tokens[index].value == "<":
            return True, matching_index(tokens, index) + 1
        return False, index

    @staticmethod
    def _generic_params(tokens: Sequence[Token], index: int) -> tuple[list[str], bool, bool, int]:
        """Return (type_param_names, has_const_or_lifetime, is_generic, next_index).

        Only bare type parameters are recognized as substitutable.  Lifetime and
        const generic parameters make a record ineligible for adapter-side
        closed-instance monomorphization, keeping the Producer fail-closed.
        """
        if index >= len(tokens) or tokens[index].value != "<":
            return [], False, False, index
        end = matching_index(tokens, index)
        type_params: list[str] = []
        has_const_or_lifetime = False
        for component in split_top(tokens[index + 1:end]):
            component = [token for token in component if token.value]
            if not component:
                continue
            first = component[0]
            if first.kind == "lifetime" or first.value == "const":
                has_const_or_lifetime = True
                continue
            if first.kind == "ident":
                type_params.append(first.value)
        return type_params, has_const_or_lifetime, True, end + 1

    @staticmethod
    def _use_path(parts: Sequence[str], module: tuple[str, ...]) -> tuple[str, ...]:
        values = list(parts)
        if not values:
            return ()
        if values[0] == "crate":
            return tuple(values[1:])
        if values[0] == "self":
            return module + tuple(values[1:])
        base = module
        while values and values[0] == "super":
            base = base[:-1]
            values.pop(0)
        if len(values) != len(parts):
            return base + tuple(values)
        # Rust 2018+ use paths start at the crate root.  This is also the only
        # stable interpretation available without edition-specific name
        # resolution from rustc internals.
        return tuple(values)

    def _record_use(self, tokens: Sequence[Token], module: tuple[str, ...]) -> None:
        values = list(tokens)
        if not values:
            return
        open_brace = next((index for index, token in enumerate(values) if token.value == "{"), -1)
        if open_brace >= 0:
            end = matching_index(values, open_brace)
            prefix = [token.value for token in values[:open_brace] if token.kind == "ident"]
            for component in split_top(values[open_brace + 1:end]):
                identifiers = [token.value for token in component if token.kind == "ident"]
                if not identifiers:
                    continue
                if identifiers[0] == "self":
                    target_parts = prefix
                    alias = prefix[-1] if prefix else ""
                else:
                    target_parts = prefix + [identifiers[0]]
                    alias = identifiers[-1] if "as" in identifiers else identifiers[0]
                if alias and alias != "_":
                    self.crate.imports[module + (alias,)] = self._use_path(target_parts, module)
            return
        identifiers = [token.value for token in values if token.kind == "ident"]
        if not identifiers or "*" in [token.value for token in values]:
            return
        alias = identifiers[-1]
        target_parts = identifiers
        if "as" in identifiers:
            position = identifiers.index("as")
            target_parts = identifiers[:position]
            alias = identifiers[position + 1] if position + 1 < len(identifiers) else ""
        if alias and alias != "_":
            self.crate.imports[module + (alias,)] = self._use_path(target_parts, module)

    def _parse_trait_methods(self, tokens: Sequence[Token]) -> list[TraitMethod]:
        methods: list[TraitMethod] = []
        i = 0
        while i < len(tokens):
            _, i2 = self._attributes(tokens, i)
            _, j = self._visibility(tokens, i2)
            k = j
            while k < len(tokens) and tokens[k].value in {"const", "async", "unsafe", "extern", "default"}:
                if tokens[k].value == "extern" and k + 1 < len(tokens) and tokens[k + 1].kind == "string":
                    k += 1
                k += 1
            if k >= len(tokens) or tokens[k].value != "fn" or k + 1 >= len(tokens):
                while i < len(tokens) and tokens[i].value not in {"{", ";"}:
                    i += 1
                if i < len(tokens) and tokens[i].value == "{":
                    i = matching_index(tokens, i) + 1
                else:
                    i = min(i + 1, len(tokens))
                continue
            name = tokens[k + 1].value
            p = k + 2
            if p < len(tokens) and tokens[p].value == "<":
                p = matching_index(tokens, p) + 1
            while p < len(tokens) and tokens[p].value != "(":
                p += 1
            if p >= len(tokens):
                break
            params_end = matching_index(tokens, p)
            params: list[ParamDecl] = []
            for param_index, component in enumerate(split_top(tokens[p + 1:params_end])):
                if not component:
                    continue
                colon = -1
                depth = 0
                for position, token in enumerate(component):
                    if token.value in {"(", "[", "{", "<"}:
                        depth += 1
                    elif token.value in {")", "]", "}", ">"}:
                        depth = max(0, depth - 1)
                    elif token.value == ":" and depth == 0:
                        colon = position
                        break
                if colon < 0:
                    params.append(ParamDecl(f"arg{param_index}", list(component)))
                else:
                    param_name = next(
                        (token.value for token in reversed(component[:colon]) if token.kind == "ident"),
                        f"arg{param_index}",
                    )
                    params.append(ParamDecl(param_name, list(component[colon + 1:])))
            q = params_end + 1
            returned: list[Token] = []
            if q < len(tokens) and tokens[q].value == "->":
                q += 1
                start = q
                depth = 0
                while q < len(tokens):
                    value = tokens[q].value
                    if depth == 0 and value in {"where", "{", ";"}:
                        break
                    if value in {"(", "[", "<"}:
                        depth += 1
                    elif value in {")", "]", ">"}:
                        depth = max(0, depth - 1)
                    q += 1
                returned = list(tokens[start:q])
            while q < len(tokens) and tokens[q].value not in {"{", ";"}:
                q += 1
            if q < len(tokens) and tokens[q].value == "{":
                i = matching_index(tokens, q) + 1
            else:
                i = min(q + 1, len(tokens))
            methods.append(TraitMethod(name, params, returned))
        return methods

    def _parse_items(
        self, tokens: Sequence[Token], module: tuple[str, ...], source: Path,
        receiver_owner: str | None = None,
        generic_impl: tuple[tuple[str, ...], list[str], list[str]] | None = None,
    ) -> None:
        i = 0
        while i < len(tokens):
            if tokens[i].value in {";", "}"}:
                i += 1
                continue
            attributes, i2 = self._attributes(tokens, i)
            public, j = self._visibility(tokens, i2)
            if j >= len(tokens):
                break

            # Modules.
            if tokens[j].value == "mod" and j + 1 < len(tokens):
                name = tokens[j + 1].value
                k = j + 2
                if k < len(tokens) and tokens[k].value == "{":
                    end = matching_index(tokens, k)
                    self._parse_items(tokens[k + 1:end], module + (name,), source)
                    i = end + 1
                    continue
                if k < len(tokens) and tokens[k].value == ";":
                    override = ""
                    for attribute in attributes:
                        at = attribute.tokens
                        if len(at) >= 3 and at[0].value == "path" and at[1].value == "=":
                            override = unquote_rust_string(tokens_text(at[2:]))
                    if override:
                        child = source.parent / override
                    else:
                        module_base = (
                            source.parent
                            if not module or source.name == "mod.rs"
                            else source.parent / source.stem
                        )
                        direct = module_base / f"{name}.rs"
                        nested = module_base / name / "mod.rs"
                        child = direct if direct.is_file() else nested
                    self._parse_file(child, module + (name,))
                    i = k + 1
                    continue

            if tokens[j].value == "use":
                k = j + 1
                while k < len(tokens) and tokens[k].value != ";":
                    k += 1
                self._record_use(tokens[j + 1:k], module)
                i = min(k + 1, len(tokens))
                continue

            # Trait declarations.  Method order is the rustc vtable order.
            if tokens[j].value == "trait" and j + 1 < len(tokens):
                name = tokens[j + 1].value
                type_params, trait_const_or_lifetime, trait_generic, k = \
                    self._generic_params(tokens, j + 2)
                while k < len(tokens) and tokens[k].value != "{":
                    k += 1
                methods: list[TraitMethod] = []
                if k < len(tokens) and tokens[k].value == "{":
                    end = matching_index(tokens, k)
                    methods = self._parse_trait_methods(tokens[k + 1:end])
                    i = end + 1
                else:
                    i = min(k + 1, len(tokens))
                self.crate.traits[module + (name,)] = TraitDecl(
                    module + (name,), methods, public, source,
                    type_params if trait_generic else [],
                    trait_const_or_lifetime,
                )
                continue

            # Struct declarations.
            if tokens[j].value == "struct" and j + 1 < len(tokens):
                name = tokens[j + 1].value
                type_params, has_const_or_lifetime, generic, k = self._generic_params(tokens, j + 2)
                while k < len(tokens) and tokens[k].value not in {"{", "(", ";"}:
                    k += 1
                representation, _, explicit_align, packed = parse_repr(attributes)
                fields: list[FieldDecl] = []
                tuple_struct = False
                if k < len(tokens) and tokens[k].value == "{":
                    end = matching_index(tokens, k)
                    for component in split_top(tokens[k + 1:end]):
                        component = [token for token in component if token.value]
                        if not component:
                            continue
                        _, field_start = self._attributes(component, 0)
                        field_public, field_start = self._visibility(component, field_start)
                        colon = next((p for p in range(field_start, len(component))
                                      if component[p].value == ":"), -1)
                        if colon < 0:
                            continue
                        fields.append(FieldDecl(component[colon - 1].value,
                                                list(component[colon + 1:]), field_public))
                    i = end + 1
                elif k < len(tokens) and tokens[k].value == "(":
                    tuple_struct = True
                    end = matching_index(tokens, k)
                    for field_index, component in enumerate(split_top(tokens[k + 1:end])):
                        _, field_start = self._attributes(component, 0)
                        field_public, field_start = self._visibility(component, field_start)
                        if field_start < len(component):
                            fields.append(FieldDecl(str(field_index), list(component[field_start:]), field_public))
                    i = end + 1
                    if i < len(tokens) and tokens[i].value == ";":
                        i += 1
                else:
                    i = min(k + 1, len(tokens))
                path = module + (name,)
                cfg_attributes = [attribute.text for attribute in attributes
                                  if attribute_name(attribute) == "cfg"]
                self.crate.records[path] = RecordDecl(
                    path, fields, public, representation, explicit_align, packed,
                    generic, tuple_struct, source, cfg_attributes,
                    type_params, has_const_or_lifetime,
                )
                continue

            # Enum declarations.  Payload variants are detected and rejected
            # later rather than being silently represented as a fieldless enum.
            if tokens[j].value == "enum" and j + 1 < len(tokens):
                name = tokens[j + 1].value
                generic, k = self._generic_end(tokens, j + 2)
                while k < len(tokens) and tokens[k].value != "{":
                    k += 1
                variants: list[str] = []
                fieldless = True
                if k < len(tokens):
                    end = matching_index(tokens, k)
                    for component in split_top(tokens[k + 1:end]):
                        if not component:
                            continue
                        _, variant_start = self._attributes(component, 0)
                        if variant_start >= len(component):
                            continue
                        variants.append(component[variant_start].value)
                        if any(token.value in {"(", "{"} for token in component[variant_start + 1:]):
                            fieldless = False
                    i = end + 1
                else:
                    i = len(tokens)
                representation, integer_repr, _, _ = parse_repr(attributes)
                path = module + (name,)
                cfg_attributes = [attribute.text for attribute in attributes
                                  if attribute_name(attribute) == "cfg"]
                self.crate.enums[path] = EnumDecl(
                    path, variants, public, representation, integer_repr,
                    fieldless, generic, source, cfg_attributes,
                )
                continue

            # Type aliases.
            if tokens[j].value == "type" and j + 1 < len(tokens):
                name = tokens[j + 1].value
                generic, k = self._generic_end(tokens, j + 2)
                while k < len(tokens) and tokens[k].value not in {"=", ";"}:
                    k += 1
                target: list[Token] = []
                if k < len(tokens) and tokens[k].value == "=":
                    start = k + 1
                    while k < len(tokens) and tokens[k].value != ";":
                        k += 1
                    target = list(tokens[start:k])
                self.crate.aliases[module + (name,)] = AliasDecl(module + (name,), target, public, generic)
                i = min(k + 1, len(tokens))
                continue

            # Function qualifiers can occur in either order.
            k = j
            abi = "Rust"
            while k < len(tokens) and tokens[k].value in {"const", "async", "unsafe", "extern", "default"}:
                if tokens[k].value == "extern":
                    abi = "C"
                    if k + 1 < len(tokens) and tokens[k + 1].kind == "string":
                        abi = unquote_rust_string(tokens[k + 1].value)
                        k += 1
                k += 1
            if k < len(tokens) and tokens[k].value == "fn" and k + 1 < len(tokens):
                name = tokens[k + 1].value
                type_params, _, generic, p = self._generic_params(tokens, k + 2)
                while p < len(tokens) and tokens[p].value != "(":
                    p += 1
                if p >= len(tokens):
                    i = k + 1
                    continue
                params_end = matching_index(tokens, p)
                params: list[ParamDecl] = []
                variadic = False
                for param_index, component in enumerate(split_top(tokens[p + 1:params_end])):
                    if not component:
                        continue
                    if any(token.value == "..." for token in component):
                        variadic = True
                        continue
                    colon = -1
                    depth = 0
                    for position, token in enumerate(component):
                        if token.value in {"(", "[", "{", "<"}:
                            depth += 1
                        elif token.value in {")", "]", "}", ">"}:
                            depth = max(0, depth - 1)
                        elif token.value == ":" and depth == 0:
                            colon = position
                            break
                    if colon < 0:
                        # self parameters imply a method, never a no_mangle free
                        # C boundary in the supported profile.
                        params.append(ParamDecl(f"arg{param_index}", list(component)))
                    else:
                        param_name = next((token.value for token in reversed(component[:colon])
                                           if token.kind == "ident"), f"arg{param_index}")
                        params.append(ParamDecl(param_name, list(component[colon + 1:])))
                q = params_end + 1
                returned: list[Token] = []
                if q < len(tokens) and tokens[q].value == "->":
                    q += 1
                    start = q
                    depth = 0
                    while q < len(tokens):
                        value = tokens[q].value
                        if depth == 0 and value in {"where", "{", ";"}:
                            break
                        if value in {"(", "[", "<"}:
                            depth += 1
                        elif value in {")", "]", ">"}:
                            depth = max(0, depth - 1)
                        q += 1
                    returned = list(tokens[start:q])
                while q < len(tokens) and tokens[q].value not in {"{", ";"}:
                    q += 1
                if q < len(tokens) and tokens[q].value == "{":
                    i = matching_index(tokens, q) + 1
                else:
                    i = min(q + 1, len(tokens))
                explicit_export = export_name_attribute(attributes)
                stable_name = has_no_mangle(attributes) or bool(explicit_export)
                cfg_attributes = [attribute.text for attribute in attributes
                                  if attribute_name(attribute) == "cfg"]
                params = rewrite_receiver_params(params, receiver_owner) if receiver_owner else params
                if receiver_owner:
                    returned = [Token(receiver_owner, "ident") if token.value == "Self" else token
                                for token in returned]
                declaration = FunctionDecl(
                    module + (name,), params, returned, public, abi,
                    explicit_export or name, stable_name, generic, variadic, source,
                    cfg_attributes, type_params,
                )
                if generic_impl is not None and receiver_owner:
                    # A method of an open generic has no standalone ABI, so it
                    # is *not* a free function: putting it in `crate.functions`
                    # would change the contract export surface for every rust
                    # producer.  The Active Adapter reaches it through an
                    # instance request instead (`Pair2::<f64, i32>::get_first`).
                    owner_path, impl_generics, owner_args = generic_impl
                    self.crate.generic_methods.append(GenericMethodDecl(
                        owner=owner_path, generics=impl_generics,
                        owner_args=owner_args, method=declaration,
                    ))
                else:
                    self.crate.functions.append(declaration)
                continue

            # Detect Drop and Trait-for-Type implementations.
            if tokens[j].value == "impl":
                k = j + 1
                header: list[Token] = []
                while k < len(tokens) and tokens[k].value not in {"{", ";"}:
                    header.append(tokens[k])
                    k += 1
                text = tokens_text(header)
                drop_match = re.search(r"(?:^|::)Drop\s+for\s+([A-Za-z_][A-Za-z0-9_:]*)", text)
                if drop_match:
                    raw = tuple(part for part in drop_match.group(1).split("::") if part not in {"crate", "self"})
                    self.crate.drop_types.add(raw if len(raw) > 1 else module + raw)
                impl_match = re.search(
                    r"(?:^|::)([A-Za-z_][A-Za-z0-9_]*)\s+for\s+([A-Za-z_][A-Za-z0-9_:]*)",
                    text,
                )
                if impl_match and impl_match.group(1) != "Drop":
                    trait_raw = tuple(
                        part for part in impl_match.group(1).split("::") if part not in {"crate", "self"}
                    )
                    type_raw = tuple(
                        part for part in impl_match.group(2).split("::") if part not in {"crate", "self"}
                    )
                    trait_path = trait_raw if len(trait_raw) > 1 else module + trait_raw
                    type_path = type_raw if len(type_raw) > 1 else module + type_raw
                    self.crate.trait_impls.append(TraitImpl(trait_path, type_path, source))
                if k < len(tokens) and tokens[k].value == "{":
                    end = matching_index(tokens, k)
                    is_trait_impl = bool(impl_match and impl_match.group(1) != "Drop")
                    header_generic = any(token.value == "<" for token in header)
                    if drop_match is None and not is_trait_impl:
                        if header_generic:
                            # `impl<A, B> Pair2<A, B>` (open) or
                            # `impl Pair2<f64, i32>` (concrete): either way the
                            # methods belong to a generic type and are kept in
                            # their own table (see GenericMethodDecl).
                            parsed_header = parse_generic_impl_header(header)
                            if parsed_header is not None:
                                owner_path, impl_generics, owner_args = parsed_header
                                owner_module = module + owner_path
                                display = owner_path[-1]
                                if owner_args:
                                    display = display + "<" + ", ".join(owner_args) + ">"
                                self._parse_items(
                                    tokens[k + 1:end], owner_module, source,
                                    receiver_owner=display,
                                    generic_impl=(owner_module, impl_generics, owner_args),
                                )
                        else:
                            idents = [
                                token.value for token in header
                                if token.kind == "ident" and token.value not in {"crate", "self", "super"}
                            ]
                            if idents:
                                owner = idents[-1]
                                self._parse_items(
                                    tokens[k + 1:end], module + (owner,), source,
                                    receiver_owner=owner,
                                )
                    i = end + 1
                else:
                    i = min(k + 1, len(tokens))
                continue

            # Skip an unknown item without interpreting its body as top-level
            # declarations.
            k = j
            while k < len(tokens) and tokens[k].value not in {"{", ";"}:
                k += 1
            if k < len(tokens) and tokens[k].value == "{":
                i = matching_index(tokens, k) + 1
            else:
                i = min(k + 1, len(tokens))


PRIMITIVE_SIZES = {
    "i8": 1, "u8": 1,
    "i16": 2, "u16": 2,
    "i32": 4, "u32": 4, "f32": 4,
    "i64": 8, "u64": 8, "f64": 8,
    # Rust `char` is a 4-byte scalar (a Unicode scalar value), and that is
    # what rustc lowers in an extern "C" signature (LLVM carrier i32).  This
    # table must describe the *producer's* facts -- the consumer's 1-byte
    # `char` is the C++ side's business, and each .dcib is point-to-point.
    "char": 4,
}
KNOWN_C_ALIASES = {
    "c_schar": "i8", "c_uchar": "u8", "c_char": "i8",
    "c_short": "i16", "c_ushort": "u16", "c_int": "i32", "c_uint": "u32",
    "c_longlong": "i64", "c_ulonglong": "u64", "c_float": "f32", "c_double": "f64",
}


def _is_unsized(value: RustType | None) -> bool:
    return value is not None and value.kind in {"slice", "str", "dyn"}


class TypeSystem:
    def __init__(self, crate: ParsedCrate, target: TargetInfo, crate_name: str):
        self.crate = crate
        self.target = target
        self.crate_name = crate_name
        # Canonical closed-instance key -> synthetic crate.records path.
        self.instances: dict[tuple[Any, ...], tuple[str, ...]] = {}
        # Same, for closed instances of *generic traits*: key -> synthetic
        # crate.traits path.  Kept apart because the two namespaces never
        # collide (a path is a record or a trait, never both).
        self.trait_instances: dict[tuple[Any, ...], tuple[str, ...]] = {}
        # Synthetic path -> friendly DCI type name (e.g. "crate.Pair<i32>").
        self.instance_names: dict[tuple[str, ...], str] = {}
        # Record paths with an evidence-based lifecycle contract; these may carry
        # non-trivial destruction (e.g. Drop) and still be exportable.
        self.lifecycle_contract_paths: set[tuple[str, ...]] = set()

    def parse(self, tokens: Sequence[Token], context: tuple[str, ...]) -> RustType:
        values = list(tokens)
        while values and values[0].kind == "lifetime":
            values.pop(0)
        spelling = tokens_text(values) or "()"
        if not values or (len(values) == 2 and values[0].value == "(" and values[1].value == ")"):
            return RustType("void", "()", name="void")
        if len(values) == 1 and values[0].value == "!":
            return RustType("never", "!", name="never")
        if values[0].value == "&":
            index = 1
            mutable = False
            if index < len(values) and values[index].kind == "lifetime":
                index += 1
            if index < len(values) and values[index].value == "mut":
                mutable = True
                index += 1
            if index < len(values) and values[index].kind == "lifetime":
                index += 1
            if index >= len(values):
                return RustType("unsupported", spelling)
            element = self.parse(values[index:], context)
            return RustType(
                "reference", spelling, mutable=mutable, element=element,
                fat=_is_unsized(element),
            )
        if len(values) >= 3 and values[0].value == "*" and values[1].value in {"const", "mut"}:
            element = self.parse(values[2:], context)
            return RustType(
                "pointer", spelling, mutable=values[1].value == "mut",
                element=element, nullable=True, fat=_is_unsized(element),
            )
        if values[0].value == "[" and values[-1].value == "]":
            parts = split_top(values[1:-1], ";")
            if len(parts) == 1 and parts[0]:
                return RustType("slice", spelling, element=self.parse(parts[0], context))
            if len(parts) != 2:
                return RustType("unsupported", spelling)
            try:
                raw_length = tokens_text(parts[1]).replace("_", "")
                raw_length = re.sub(r"(?:usize|u(?:8|16|32|64|128))$", "", raw_length)
                length = int(raw_length, 0)
            except ValueError:
                return RustType("unsupported", spelling)
            return RustType("array", spelling, element=self.parse(parts[0], context), length=length)

        # Nullable C function pointer niche.  This must be recognized before
        # the general `fn` scan because the inner signature also contains the
        # `fn` token.
        option_position = next((index for index, token in enumerate(values)
                                if token.value == "Option"), -1)
        if option_position >= 0:
            less = next((index for index in range(option_position + 1, len(values))
                         if values[index].value == "<"), -1)
            if less >= 0:
                end = matching_index(values, less)
                if end == len(values) - 1:
                    inner = self.parse(values[less + 1:end], context)
                    if inner.kind == "function":
                        inner.nullable = True
                        inner.spelling = spelling
                        return inner
        fn_index = next((index for index, token in enumerate(values) if token.value == "fn"), -1)
        if fn_index >= 0:
            abi = "Rust"
            for index, token in enumerate(values[:fn_index]):
                if token.value == "extern":
                    abi = "C"
                    if index + 1 < fn_index and values[index + 1].kind == "string":
                        abi = unquote_rust_string(values[index + 1].value)
            if fn_index + 1 >= len(values) or values[fn_index + 1].value != "(":
                return RustType("unsupported", spelling)
            end = matching_index(values, fn_index + 1)
            params = [self.parse(part, context) for part in split_top(values[fn_index + 2:end]) if part]
            returned = RustType("void", "()", name="void")
            if end + 1 < len(values) and values[end + 1].value == "->":
                returned = self.parse(values[end + 2:], context)
            return RustType("function", spelling, params=params, result=returned, abi=abi)

        result_position = next(
            (index for index, token in enumerate(values) if token.value == "Result"),
            -1,
        )
        if result_position >= 0:
            less = next(
                (
                    index
                    for index in range(result_position + 1, len(values))
                    if values[index].value == "<"
                ),
                -1,
            )
            if less >= 0:
                try:
                    end = matching_index(values, less)
                except RustAdapterError:
                    end = -1
                if end == len(values) - 1:
                    args = [part for part in split_top(values[less + 1:end]) if part]
                    if len(args) == 2:
                        ok_ty = self.parse(args[0], context)
                        err_ty = self.parse(args[1], context)
                        return RustType(
                            "result", spelling, name="Result", params=[ok_ty, err_ty]
                        )

        # Closed generic applications of an in-crate generic type are exported
        # on demand: rustc has already monomorphized them, so the Adapter only
        # asks for the final layout/symbol.  Open or foreign generics remain
        # unsupported.
        if any(token.value == "<" for token in values):
            instance = self._parse_generic_application(values, context)
            if instance is not None:
                return instance
            return RustType("unsupported", spelling)
        path_parts = [token.value for token in values if token.kind == "ident"]
        raw_name = "::".join(path_parts)
        short = path_parts[-1] if path_parts else spelling
        if values and values[0].value == "dyn":
            trait_parts = [token.value for token in values[1:] if token.kind == "ident"]
            trait_raw = "::".join(trait_parts)
            resolved = self.resolve_path(trait_raw, context)
            if resolved in self.crate.traits:
                return RustType(
                    "dyn", spelling, name=self.dci_name(resolved), decl_path=resolved,
                )
            return RustType("unsupported", spelling, name="dyn")
        if short == "str":
            return RustType("str", spelling, name="str")
        if short in PRIMITIVE_SIZES:
            return RustType("primitive", spelling, name=short)
        if short in {"isize", "usize"}:
            return RustType("primitive", spelling, name=short)
        if short in {"bool", "i128", "u128"}:
            return RustType("unsupported", spelling, name=short)
        if short in KNOWN_C_ALIASES:
            return RustType("primitive", spelling, name=KNOWN_C_ALIASES[short])
        if short in {"c_long", "c_ulong"}:
            width = 32 if self.target.os == "windows" or self.target.pointer_width == 32 else 64
            prefix = "i" if short == "c_long" else "u"
            return RustType("primitive", spelling, name=f"{prefix}{width}")
        if short == "c_void":
            return RustType("opaque", spelling, name="c_void")

        resolved = self.resolve_path(raw_name, context)
        if resolved in self.crate.aliases:
            alias = self.crate.aliases[resolved]
            if alias.generic:
                return RustType("unsupported", spelling)
            target = self.parse(alias.target_tokens, resolved[:-1])
            target.spelling = spelling
            return target
        if resolved in self.crate.records:
            return RustType("record", spelling, name=self.dci_name(resolved), decl_path=resolved)
        if resolved in self.crate.enums:
            return RustType("enum", spelling, name=self.dci_name(resolved), decl_path=resolved)
        # Unknown pointee types are allowed as opaque only when the caller is
        # validating a raw pointer.  A by-value unknown remains unsupported.
        return RustType("opaque", spelling, name=self.dci_name(resolved) if resolved else raw_name)

    def resolve_path(self, raw: str, context: tuple[str, ...]) -> tuple[str, ...]:
        parts = tuple(part for part in raw.split("::") if part)
        if not parts:
            return ()
        if parts[0] == "crate":
            return parts[1:]
        if parts[0] == "self":
            return context + parts[1:]
        if parts[0] == "super":
            base = context
            index = 0
            while index < len(parts) and parts[index] == "super":
                base = base[:-1]
                index += 1
            return base + parts[index:]
        if len(parts) == 1:
            for depth in range(len(context), -1, -1):
                imported = self.crate.imports.get(context[:depth] + parts)
                if imported:
                    return imported
        for depth in range(len(context), -1, -1):
            candidate = context[:depth] + parts
            if (candidate in self.crate.records or candidate in self.crate.enums
                    or candidate in self.crate.aliases or candidate in self.crate.traits):
                return candidate
        return parts

    def dci_name(self, path: tuple[str, ...]) -> str:
        return ".".join((self.crate_name,) + path)

    def active_name(self, value: RustType) -> str:
        """Consumer-facing plain spelling for Active-request type references.

        The stub backend turns a contract symbol's type names into producer
        queries by taking the crate-relative path (`Vec2`, `Pair2<f64, i32>`);
        the DCI `crate.Name` namespace is adapter-internal bookkeeping.
        """
        if value.kind == "primitive":
            return value.name or value.spelling
        path = self.decl_path_for(value)
        if path is not None:
            return "::".join(path)
        name = value.name or value.spelling
        prefix = self.crate_name + "."
        return name[len(prefix):] if name.startswith(prefix) else name

    def decl_path_for(self, value: RustType) -> tuple[str, ...] | None:
        if value.kind not in {"record", "enum", "trait"}:
            return None
        if value.decl_path is not None:
            return value.decl_path
        prefix = self.crate_name + "."
        if not value.name.startswith(prefix):
            return None
        return tuple(value.name[len(prefix):].split("."))

    def rust_spelling(self, value: RustType) -> str | None:
        """Return a rustc-resolvable spelling for a closed type argument."""
        if value.kind == "primitive":
            return value.name
        if value.kind == "void":
            return "()"
        if value.kind == "opaque" and value.name.endswith("c_void"):
            return "core::ffi::c_void"
        if value.kind == "pointer" and value.element is not None:
            inner = self.rust_spelling(value.element)
            if inner is None:
                return None
            return ("*mut " if value.mutable else "*const ") + inner
        if value.kind in {"record", "enum"}:
            path = self.decl_path_for(value)
            if path is None:
                return None
            instance = self.crate.records.get(path)
            if instance is not None and instance.probe_path is not None:
                return instance.probe_path
            return "crate::" + "::".join(path)
        return None

    def _parse_generic_application(
        self, values: Sequence[Token], context: tuple[str, ...]
    ) -> RustType | None:
        values = list(values)
        less = next((index for index, token in enumerate(values)
                     if token.value == "<"), -1)
        if less <= 0:
            return None
        try:
            end = matching_index(values, less)
        except RustAdapterError:
            return None
        if end != len(values) - 1:
            return None
        base_tokens = values[:less]
        if any(token.kind != "ident" and token.value != "::" for token in base_tokens):
            return None
        base_name = "::".join(token.value for token in base_tokens if token.kind == "ident")
        resolved = self.resolve_path(base_name, context)
        declared = (self.crate.records.get(resolved)
                    or self.crate.enums.get(resolved)
                    or self.crate.traits.get(resolved))
        if declared is None or not getattr(declared, "generic", False):
            return None
        if getattr(declared, "const_or_lifetime_params", False):
            return None
        type_params = list(getattr(declared, "type_params", []))
        arg_components = [component for component in split_top(values[less + 1:end]) if component]
        if len(arg_components) != len(type_params) or not type_params:
            return None
        arg_types: list[RustType] = []
        for component in arg_components:
            argument = self.parse(component, context)
            ok, _ = self.is_stable(argument)
            if not ok:
                return None
            arg_types.append(argument)
        if resolved in self.crate.traits:
            return self.ensure_trait_instance(resolved, arg_types)
        return self.ensure_instance(resolved, arg_types)

    def ensure_instance(
        self, base_path: tuple[str, ...], arg_types: Sequence[RustType]
    ) -> RustType | None:
        base_record = self.crate.records.get(base_path)
        base_enum = self.crate.enums.get(base_path)
        declared = base_record or base_enum
        if declared is None:
            return None
        arg_spellings: list[str] = []
        arg_plain_names: list[str] = []
        for argument in arg_types:
            spelling = self.rust_spelling(argument)
            if spelling is None:
                return None
            arg_spellings.append(spelling)
            # Plain, consumer-facing spellings (`Vec2`, not `crate::Vec2` /
            # `lib.Vec2`).  The synthetic path is round-tripped through
            # `decl_path_for`, which splits a DCI name on `.` -- a dotted
            # argument spelling would shatter the path into two and desync
            # the friendly name from the consumer's own spelling.
            arg_plain_names.append(self.active_name(argument))
        key = (base_path, tuple(arg_spellings))
        dci_name = self.dci_name(base_path) + "<" + ", ".join(arg_plain_names) + ">"
        if key in self.instances:
            synthetic_path = self.instances[key]
            kind = "record" if synthetic_path in self.crate.records else "enum"
            return RustType(kind, dci_name, name=dci_name, decl_path=synthetic_path)
        turbofish = "crate::" + "::".join(base_path) + "::<" + ", ".join(arg_spellings) + ">"
        synthetic_path = base_path[:-1] + (
            base_path[-1] + "$" + "$".join(arg_plain_names),
        )
        self.instances[key] = synthetic_path
        self.instance_names[synthetic_path] = dci_name
        if base_record is not None:
            substitution = {
                name: lex_rust(spelling)
                for name, spelling in zip(base_record.type_params, arg_spellings)
            }
            substituted_fields: list[FieldDecl] = []
            for field_decl in base_record.fields:
                substituted_fields.append(FieldDecl(
                    field_decl.name,
                    substitute_type_tokens(field_decl.type_tokens, substitution),
                    field_decl.public,
                ))
            self.crate.records[synthetic_path] = RecordDecl(
                synthetic_path, substituted_fields, True, base_record.representation,
                base_record.explicit_align, base_record.packed, False,
                base_record.tuple_struct, base_record.source, list(base_record.cfg_attributes),
                [], False, turbofish, base_path, tuple(arg_plain_names),
            )
            return RustType("record", dci_name, name=dci_name, decl_path=synthetic_path)
        assert base_enum is not None
        self.crate.enums[synthetic_path] = EnumDecl(
            synthetic_path, list(base_enum.variants), True, base_enum.representation,
            base_enum.integer_repr, base_enum.fieldless, False, base_enum.source,
            list(base_enum.cfg_attributes), [], False, turbofish, base_path,
            tuple(arg_plain_names),
        )
        return RustType("enum", dci_name, name=dci_name, decl_path=synthetic_path)

    def ensure_trait_instance(
        self, base_path: tuple[str, ...], arg_types: Sequence[RustType]
    ) -> RustType | None:
        """Close a generic *trait* against concrete arguments.

        `export_public_traits` walks `crate.traits`, so closing an instance is
        just registering one more `TraitDecl` -- with every method signature
        substituted -- under a synthetic path.  The synthetic entry carries the
        consumer-facing DCI name (`native.Sink<i32>`) and the rustc path it is
        named by (`crate::Sink::<i32>`), which is what the Vyx-side stub
        emitter has to write for `impl native::Sink<i32> for ...`.
        """
        trait_decl = self.crate.traits.get(base_path)
        if trait_decl is None or not trait_decl.type_params:
            return None
        if trait_decl.const_or_lifetime_params:
            return None
        arg_spellings: list[str] = []
        arg_plain_names: list[str] = []
        for argument in arg_types:
            spelling = self.rust_spelling(argument)
            if spelling is None:
                return None
            arg_spellings.append(spelling)
            arg_plain_names.append(self.active_name(argument))
        key = (base_path, tuple(arg_spellings))
        dci_name = self.dci_name(base_path) + "<" + ", ".join(arg_plain_names) + ">"
        if key in self.trait_instances:
            synthetic_path = self.trait_instances[key]
            return RustType("trait", dci_name, name=dci_name,
                            decl_path=synthetic_path)
        turbofish = ("crate::" + "::".join(base_path) + "::<"
                     + ", ".join(arg_spellings) + ">")
        synthetic_path = base_path[:-1] + (
            base_path[-1] + "$" + "$".join(arg_plain_names),
        )
        substitution = {
            name: lex_rust(spelling)
            for name, spelling in zip(trait_decl.type_params, arg_spellings)
        }
        methods = [
            TraitMethod(
                method.name,
                [ParamDecl(param.name,
                           substitute_type_tokens(param.type_tokens, substitution))
                 for param in method.params],
                substitute_type_tokens(method.return_tokens, substitution),
            )
            for method in trait_decl.methods
        ]
        self.trait_instances[key] = synthetic_path
        self.instance_names[synthetic_path] = dci_name
        self.crate.traits[synthetic_path] = TraitDecl(
            synthetic_path, methods, True, trait_decl.source,
            [], False, turbofish, base_path, tuple(arg_plain_names),
        )
        return RustType("trait", dci_name, name=dci_name, decl_path=synthetic_path)

    def is_stable(self, value: RustType, behind_pointer: bool = False,
                  seen: set[tuple[str, ...]] | None = None) -> tuple[bool, str]:
        if value.kind in {"primitive", "void", "never"}:
            return True, ""
        if value.kind == "pointer":
            if value.element is None:
                return False, "raw pointer has no pointee type"
            if value.element.kind == "void" or value.element.name == "c_void":
                return True, ""
            if value.fat or _is_unsized(value.element):
                return self._unsized_pointee_is_stable(value.element)
            if value.element.kind in {"reference", "unsupported"}:
                return False, f"unsupported raw-pointer pointee {value.element.spelling!r}"
            # The pointee layout need not be public/stable for an opaque handle.
            return True, ""
        if value.kind == "function":
            if value.abi.lower() not in RUST_STABLE_ABIS:
                return False, f"function pointer uses unstable Rust ABI {value.abi!r}"
            for parameter in value.params:
                ok, reason = self.is_stable(parameter)
                if not ok:
                    return False, reason
            if value.result:
                return self.is_stable(value.result)
            return True, ""
        if value.kind == "array":
            if value.length is None or value.length < 0 or value.element is None:
                return False, f"array length is not a compile-time integer in {value.spelling!r}"
            return self.is_stable(value.element, seen=seen)
        if value.kind == "reference":
            if value.element is None:
                return False, "reference has no pointee type"
            if value.fat or _is_unsized(value.element):
                return self._unsized_pointee_is_stable(value.element)
            if value.element.kind in {"unsupported", "void"}:
                return False, f"unsupported reference pointee {value.element.spelling!r}"
            return True, ""
        if value.kind == "result":
            return False, "Rust Result<T,E> has no stable C ABI; use a translate_unwind translator"
        if value.kind == "unsupported":
            return False, f"unsupported Rust ABI type {value.spelling!r}"
        if value.kind == "opaque":
            return (True, "") if behind_pointer else (False, f"opaque type {value.spelling!r} is used by value")
        path = self.decl_path_for(value)
        if path is None:
            return False, f"cannot resolve ABI type {value.spelling!r}"
        seen = set() if seen is None else set(seen)
        if path in seen:
            return False, f"recursive by-value type {value.name!r}"
        seen.add(path)
        if value.kind == "record":
            record = self.crate.records[path]
            if record.generic:
                return False, f"generic record {value.name!r} is not monomorphized"
            if record.representation not in {"stable", "transparent", ""}:
                return False, f"record {value.name!r} uses an unsupported #[repr]"
            if record.packed is not None:
                return False, f"packed record {value.name!r} is rejected because unaligned field access is not portable"
            if record.opaque:
                # A closed public type can cross the boundary without exposing
                # its private fields.  rustc measures the entire native layout,
                # and its generated drop operation owns the lifecycle.
                return True, ""
            drops = path in self.crate.drop_types or (
                record.instance_of is not None and record.instance_of in self.crate.drop_types
            )
            has_contract = path in self.lifecycle_contract_paths or (
                record.instance_of is not None
                and record.instance_of in self.lifecycle_contract_paths
            )
            if drops and not has_contract:
                return False, f"record {value.name!r} implements Drop and has no declared DCI lifecycle"
            for field_decl in record.fields:
                field_type = self.parse(field_decl.type_tokens, path[:-1])
                ok, reason = self.is_stable(field_type, seen=seen)
                if not ok:
                    return False, f"field {value.name}.{field_decl.name}: {reason}"
            return True, ""
        enum = self.crate.enums[path]
        if enum.generic:
            return False, f"generic enum {value.name!r} is not monomorphized"
        if not enum.fieldless:
            return False, f"data-carrying enum {value.name!r} has no normalized DCI union contract"
        return True, ""

    def _unsized_pointee_is_stable(self, element: RustType) -> tuple[bool, str]:
        if element.kind == "str":
            return True, ""
        if element.kind == "slice":
            if element.element is None:
                return False, "slice has no element type"
            if _is_unsized(element.element):
                return False, f"nested unsized slice {element.spelling!r}"
            return self.is_stable(element.element)
        if element.kind == "dyn":
            if element.decl_path is None or element.decl_path not in self.crate.traits:
                return False, f"dyn trait {element.spelling!r} is not an in-crate trait"
            return True, ""
        return False, f"unsupported unsized pointee {element.spelling!r}"

    def size_of_scalar(self, value: RustType) -> int | None:
        if value.kind == "primitive":
            if value.name in {"usize", "isize"}:
                return self.target.pointer_width // 8
            return PRIMITIVE_SIZES.get(value.name)
        if value.kind in {"pointer", "function", "reference"}:
            if value.fat or (value.element is not None and _is_unsized(value.element)):
                return None
            return self.target.pointer_width // 8
        return None

    def type_json(self, value: RustType, reference_override: str | None = None) -> dict[str, Any]:
        reference = reference_override
        nullable = value.nullable
        target = value
        if value.kind in {"pointer", "reference"} and value.element is not None:
            target = value.element
            reference = "pointer"
            nullable = value.kind == "pointer" or value.nullable
        elif value.kind == "function":
            reference = "pointer"
        elif reference is None:
            reference = "value"
        kind = target.kind
        if kind == "array":
            kind = "vector"
        if kind not in {"primitive", "enum", "record", "opaque", "function", "vector"}:
            kind = "opaque"
        name = target.name or target.spelling
        result: dict[str, Any] = {
            "name": name,
            "kind": kind,
            "nullable": bool(nullable),
            "reference": reference,
        }
        if value.kind in {"pointer", "reference"}:
            result["rust_type"] = value.spelling
            result["pointee_const"] = not value.mutable
            if value.fat or _is_unsized(value.element):
                meta = "vtable" if value.element is not None and value.element.kind == "dyn" else "len"
                result["wide"] = True
                result["metadata_kind"] = meta
                result["size"] = (self.target.pointer_width // 8) * 2
                result["alignment"] = self.target.pointer_width // 8
                binding = getattr(self, "view_bindings", {}).get(value.spelling)
                if binding:
                    result["binding_name"] = binding
        elif value.kind == "array" and value.element is not None:
            result["length"] = value.length
            result["element"] = self.type_json(value.element)
        elif value.kind == "function":
            result["rust_type"] = value.spelling
            result["signature"] = {
                "calling_convention": value.abi.lower(),
                "params": [self.type_json(parameter) for parameter in value.params],
                "return": None if not value.result or value.result.kind == "void" else self.type_json(value.result),
            }
        return result


def run_checked(command: Sequence[str], *, cwd: Path | None = None,
                env: dict[str, str] | None = None, timeout: int = 120) -> subprocess.CompletedProcess[str]:
    try:
        result = subprocess.run(
            list(command), cwd=str(cwd) if cwd else None, env=env,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            encoding="utf-8", errors="replace", timeout=timeout, check=False,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        raise RustAdapterError(f"cannot execute {command[0]}: {exc}") from exc
    if result.returncode != 0:
        detail = (result.stderr or result.stdout).strip()
        raise RustAdapterError(
            f"command failed ({result.returncode}): {' '.join(command)}"
            + (f"\n{detail}" if detail else "")
        )
    return result


def discover_rustc(explicit: str | None = None) -> str:
    candidate = explicit or os.environ.get("RUSTC", "") or "rustc"
    path = Path(candidate).expanduser()
    if path.is_file():
        return str(path.resolve())
    found = shutil.which(candidate)
    if found:
        return found
    raise RustAdapterError(f"rustc executable not found: {candidate}")


def rustc_identity(rustc: str) -> dict[str, str]:
    output = run_checked([rustc, "--version", "--verbose"], timeout=15).stdout
    values: dict[str, str] = {}
    for line in output.splitlines():
        if ":" in line:
            key, value = line.split(":", 1)
            values[key.strip().lower()] = value.strip()
        elif line.startswith("rustc "):
            values["release"] = line.split()[1]
    return {
        "name": "rustc",
        "version": values.get("release", "unknown"),
        "build": values.get("commit-hash", ""),
        "llvm": values.get("llvm version", ""),
        "host": values.get("host", ""),
    }


def canonical_rust_target(value: str) -> str:
    target = RUST_TARGET_ALIASES.get(value.strip().lower(), value.strip())
    # Android API levels are a Clang/NDK triple convention.  rustc targets use
    # the level-independent spelling and receive the API level through linker
    # selection/options.
    target = re.sub(
        r"^((?:aarch64|armv7|i686|x86_64)-linux-android(?:eabi)?)\d+$",
        r"\1",
        target,
    )
    return target


def target_info(rustc: str, triple: str) -> TargetInfo:
    triple = canonical_rust_target(triple)
    output = run_checked([rustc, "--print", "cfg", "--target", triple], timeout=20).stdout
    cfg: dict[str, str] = {}
    for line in output.splitlines():
        if "=" in line:
            key, raw = line.split("=", 1)
            cfg[key.strip()] = raw.strip().strip('"')
    architecture = cfg.get("target_arch", "")
    width_text = cfg.get("target_pointer_width", "")
    endian = cfg.get("target_endian", "")
    target_os = cfg.get("target_os", "")
    environment = cfg.get("target_env", "") or target_os
    if not architecture or not width_text or endian not in {"little", "big"}:
        raise RustAdapterError(f"rustc returned incomplete target facts for {triple!r}")
    pointer_width = int(width_text)
    if target_os == "windows":
        object_format = "coff"
    elif target_os in {"macos", "ios", "tvos", "watchos", "visionos"}:
        object_format = "macho"
    elif architecture in {"wasm32", "wasm64"}:
        object_format = "wasm"
    else:
        object_format = "elf"
    if architecture == "x86_64" and target_os == "windows":
        abi_family = "win64"
    elif architecture == "x86_64":
        abi_family = "sysv64"
    elif architecture == "aarch64":
        abi_family = "aapcs64"
    elif architecture in {"x86", "i686", "i586"}:
        abi_family = "x86"
    elif architecture.startswith("wasm"):
        abi_family = architecture
    else:
        abi_family = f"{architecture}-{environment or target_os}"
    return TargetInfo(triple, architecture, pointer_width, endian, target_os,
                      environment, object_format, abi_family)


def copy_crate_tree(root: Path, destination: Path) -> Path:
    """Copy the source tree for instrumentation without touching the input."""
    manifest = find_cargo_manifest(root)
    source_dir = manifest.parent if manifest is not None else root.parent
    target_dir = destination / "source"

    def ignore(directory: str, names: list[str]) -> set[str]:
        ignored = {name for name in names if name in {"target", ".git", ".cache", "__pycache__"}}
        return ignored

    shutil.copytree(source_dir, target_dir, ignore=ignore)
    return target_dir / root.relative_to(source_dir)


def find_cargo_manifest(root: Path) -> Path | None:
    for directory in (root.parent, *root.parents):
        candidate = directory / "Cargo.toml"
        if candidate.is_file():
            return candidate
    return None


def cargo_package(root: Path) -> tuple[Path | None, dict[str, Any]]:
    manifest = find_cargo_manifest(root)
    if manifest is None:
        return None, {}
    try:
        document = tomllib.loads(manifest.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, tomllib.TOMLDecodeError) as exc:
        raise RustAdapterError(f"cannot read Cargo manifest {manifest}: {exc}") from exc
    package = document.get("package", {})
    return manifest, package if isinstance(package, dict) else {}


def cargo_declarations(cargo: CargoContext, rustc: str, target: str) -> ParsedCrate:
    """Import the producer-expanded declarations and compiler-proven public API."""
    expanded, exports = cargo.declarations(rustc, target)
    crate = RustParser(expanded).parse()
    crate.files = sorted(cargo.root.parent.rglob("*.rs"))
    canonical_to_exposed: dict[tuple[str, ...], list[tuple[str, ...]]] = {}
    for exposed, canonical in exports.items():
        canonical_to_exposed.setdefault(canonical, []).append(exposed)
        if exposed != canonical:
            crate.imports[exposed] = canonical
    functions = []
    for function in crate.functions:
        exposed = canonical_to_exposed.get(function.path)
        if not exposed:
            continue
        if function.path[:-1] not in crate.records and function.path[:-1] not in crate.enums:
            original = function.path
            function.path = original if original in exposed else sorted(exposed)[0]
            function.resolution_context = original[:-1]
        functions.append(function)
    crate.functions = functions
    crate.generic_methods = [method for method in crate.generic_methods
                             if method.owner + (method.name,) in canonical_to_exposed]
    for table in (crate.records, crate.enums, crate.aliases, crate.traits):
        for path, declaration in table.items():
            declaration.public = path in canonical_to_exposed
    return crate


def inferred_edition(root: Path) -> str:
    _, package = cargo_package(root)
    edition = str(package.get("edition", "")).strip()
    return edition if edition in {"2015", "2018", "2021", "2024"} else "2021"


def probe_environment(root: Path, temp: Path,
                      rustc_args: Sequence[str] = ()) -> dict[str, str]:
    cargo_environment = getattr(rustc_args, "environment", None)
    if cargo_environment is not None:
        return dict(cargo_environment)
    environment = dict(os.environ)
    manifest, package = cargo_package(root)
    if manifest is None:
        return environment
    authors = package.get("authors", [])
    if not isinstance(authors, list):
        authors = []
    environment.update({
        "CARGO_MANIFEST_DIR": str(manifest.parent),
        "CARGO_PKG_NAME": str(package.get("name", "")),
        "CARGO_PKG_VERSION": str(package.get("version", "")),
        "CARGO_PKG_VERSION_MAJOR": str(package.get("version", "")).split(".")[0] if package.get("version") else "",
        "CARGO_PKG_AUTHORS": ":".join(str(author) for author in authors),
        "CARGO_PKG_DESCRIPTION": str(package.get("description", "")),
        "CARGO_PKG_HOMEPAGE": str(package.get("homepage", "")),
        "CARGO_PKG_REPOSITORY": str(package.get("repository", "")),
        "CARGO_PKG_LICENSE": str(package.get("license", "")),
        "OUT_DIR": str(temp / "cargo-out"),
    })
    (temp / "cargo-out").mkdir(parents=True, exist_ok=True)
    return environment


def discover_ownership_annotations(files: Sequence[Path]) -> dict[str, dict[str, Any]]:
    """Read language-neutral ownership blocks embedded in Rust comments."""
    rules: dict[str, dict[str, Any]] = {}
    for path in files:
        try:
            source = path.read_text(encoding="utf-8")
        except (OSError, UnicodeError) as exc:
            raise RustAdapterError(f"cannot read ownership annotations from {path}: {exc}") from exc
        for match in RE_OWNERSHIP_BLOCK.finditer(source):
            line = source.count("\n", 0, match.start()) + 1
            try:
                block = json.loads(match.group(1))
            except json.JSONDecodeError as exc:
                raise RustAdapterError(
                    f"{path}: invalid dci-ownership JSON at line {line}: {exc}"
                ) from exc
            if not isinstance(block, dict):
                raise RustAdapterError(f"{path}:{line}: dci-ownership block must be an object")
            for selector, raw_rule in block.items():
                if not isinstance(selector, str) or not selector:
                    raise RustAdapterError(f"{path}:{line}: ownership selector must be a non-empty string")
                if not isinstance(raw_rule, dict):
                    raise RustAdapterError(f"{path}:{line}: ownership rule {selector!r} must be an object")
                if selector in rules and rules[selector] != raw_rule:
                    raise RustAdapterError(f"conflicting dci-ownership rules for {selector!r}")
                parameters = raw_rule.get("parameters", {})
                if not isinstance(parameters, dict):
                    raise RustAdapterError(f"dci-ownership {selector!r}.parameters must be an object")
                normalized_parameters: dict[int, str] = {}
                for raw_index, ownership in parameters.items():
                    try:
                        index = int(raw_index)
                    except (TypeError, ValueError) as exc:
                        raise RustAdapterError(
                            f"dci-ownership {selector!r} has non-numeric parameter index {raw_index!r}"
                        ) from exc
                    if index < 0 or ownership not in PARAMETER_OWNERSHIP_VALUES:
                        raise RustAdapterError(
                            f"dci-ownership {selector!r} parameter #{index} has invalid ownership {ownership!r}"
                        )
                    normalized_parameters[index] = ownership
                returned = raw_rule.get("return", "")
                if returned and returned not in RETURN_OWNERSHIP_VALUES:
                    raise RustAdapterError(
                        f"dci-ownership {selector!r} has invalid return ownership {returned!r}"
                    )
                rules[selector] = {"parameters": normalized_parameters, "return": returned}
    return rules


def discover_lifecycle_annotations(files: Sequence[Path]) -> dict[str, dict[str, Any]]:
    """Read evidence-based lifecycle contracts embedded in Rust comments.

    Each block binds a record selector to an explicit lifecycle contract whose
    non-trivial operations reference real exported symbols.  The Adapter never
    infers lifecycle from type names; a record that carries hidden destruction
    (a ``Drop`` impl) stays rejected unless an operator supplies this evidence.
    """
    contracts: dict[str, dict[str, Any]] = {}
    for path in files:
        try:
            source = path.read_text(encoding="utf-8")
        except (OSError, UnicodeError) as exc:
            raise RustAdapterError(f"cannot read lifecycle annotations from {path}: {exc}") from exc
        for match in RE_LIFECYCLE_BLOCK.finditer(source):
            line = source.count("\n", 0, match.start()) + 1
            try:
                block = json.loads(match.group(1))
            except json.JSONDecodeError as exc:
                raise RustAdapterError(
                    f"{path}: invalid dci-lifecycle JSON at line {line}: {exc}"
                ) from exc
            if not isinstance(block, dict):
                raise RustAdapterError(f"{path}:{line}: dci-lifecycle block must be an object")
            for selector, raw in block.items():
                if not isinstance(selector, str) or not selector:
                    raise RustAdapterError(f"{path}:{line}: lifecycle selector must be a non-empty string")
                if not isinstance(raw, dict):
                    raise RustAdapterError(f"{path}:{line}: lifecycle rule {selector!r} must be an object")
                normalized = normalize_lifecycle_contract(selector, raw)
                if selector in contracts and contracts[selector] != normalized:
                    raise RustAdapterError(f"conflicting dci-lifecycle rules for {selector!r}")
                contracts[selector] = normalized
    return contracts


def normalize_lifecycle_contract(selector: str, raw: dict[str, Any]) -> dict[str, Any]:
    ownership_model = raw.get("ownership_model", "unique")
    copy_semantics = raw.get("copy_semantics", "forbidden")
    move_semantics = raw.get("move_semantics", "operation")
    destruction = raw.get("destruction", "operation")
    moved_from_state = raw.get("moved_from_state", "destructible_only")
    for name, value, allowed in (
        ("ownership_model", ownership_model, OWNERSHIP_MODEL_VALUES),
        ("copy_semantics", copy_semantics, COPY_SEMANTICS_VALUES),
        ("move_semantics", move_semantics, MOVE_SEMANTICS_VALUES),
        ("destruction", destruction, DESTRUCTION_VALUES),
        ("moved_from_state", moved_from_state, MOVED_FROM_VALUES),
    ):
        if value not in allowed:
            raise RustAdapterError(
                f"dci-lifecycle {selector!r}.{name} has invalid value {value!r}"
            )
    raw_operations = raw.get("operations", {})
    if not isinstance(raw_operations, dict):
        raise RustAdapterError(f"dci-lifecycle {selector!r}.operations must be an object")
    operations: dict[str, str] = {}
    for op_name, op_value in raw_operations.items():
        symbol = op_value.get("symbol") if isinstance(op_value, dict) else op_value
        if not isinstance(symbol, str) or not symbol:
            raise RustAdapterError(
                f"dci-lifecycle {selector!r}.operations.{op_name} must bind a non-empty symbol"
            )
        operations[op_name] = symbol
    contract: dict[str, Any] = {
        "ownership_model": ownership_model,
        "copy_semantics": copy_semantics,
        "move_semantics": move_semantics,
        "destruction": destruction,
        "moved_from_state": moved_from_state,
        "operations": operations,
    }
    allocator_domain = raw.get("allocator_domain")
    if allocator_domain is not None:
        if not isinstance(allocator_domain, str) or not allocator_domain:
            raise RustAdapterError(
                f"dci-lifecycle {selector!r}.allocator_domain must be a non-empty string"
            )
        contract["allocator_domain"] = allocator_domain
    # Every semantic that demands an operation must name one, and only real
    # exported symbols are accepted as evidence (checked against the crate's
    # stable link identities by the caller).
    for (semantic_key, semantic_value), operation_name in LIFECYCLE_REQUIRED_OPERATION.items():
        if contract[semantic_key] == semantic_value and operation_name not in operations:
            raise RustAdapterError(
                f"dci-lifecycle {selector!r}: {semantic_key} = {semantic_value!r} requires "
                f"operations.{operation_name}"
            )
    return contract


def discover_stub_annotations(files: Sequence[Path]) -> dict[str, dict[str, Any]]:
    """Read explicit, normalized Stub requests from source annotations.

    A ``dci-stub`` block declares that a specific boundary needs an ABI-native
    wrapper (reverse override, cast, or operation wrapper).  The Adapter emits
    the language-neutral request and the required ``stub`` consumer mode; it
    never infers a stub from type names.  The concrete wrapper is produced later
    by a capability-matched backend, not by this Adapter.
    """
    requests: dict[str, dict[str, Any]] = {}
    for path in files:
        try:
            source = path.read_text(encoding="utf-8")
        except (OSError, UnicodeError) as exc:
            raise RustAdapterError(f"cannot read stub annotations from {path}: {exc}") from exc
        for match in RE_STUB_BLOCK.finditer(source):
            line = source.count("\n", 0, match.start()) + 1
            try:
                block = json.loads(match.group(1))
            except json.JSONDecodeError as exc:
                raise RustAdapterError(
                    f"{path}: invalid dci-stub JSON at line {line}: {exc}"
                ) from exc
            if not isinstance(block, dict):
                raise RustAdapterError(f"{path}:{line}: dci-stub block must be an object")
            for request_id, raw in block.items():
                if not isinstance(request_id, str) or not request_id:
                    raise RustAdapterError(f"{path}:{line}: stub request id must be a non-empty string")
                if not isinstance(raw, dict):
                    raise RustAdapterError(f"{path}:{line}: stub request {request_id!r} must be an object")
                normalized = normalize_stub_request(request_id, raw)
                if request_id in requests and requests[request_id] != normalized:
                    raise RustAdapterError(f"conflicting dci-stub requests for {request_id!r}")
                requests[request_id] = normalized
    return requests


def normalize_stub_request(request_id: str, raw: dict[str, Any]) -> dict[str, Any]:
    kind = raw.get("kind")
    if kind not in STUB_REQUEST_KINDS:
        raise RustAdapterError(
            f"dci-stub {request_id!r}.kind must be one of {sorted(STUB_REQUEST_KINDS)}"
        )
    wrapper_name = raw.get("wrapper")
    if isinstance(wrapper_name, dict):
        wrapper_name = wrapper_name.get("link_name")
    if not isinstance(wrapper_name, str) or not wrapper_name:
        raise RustAdapterError(f"dci-stub {request_id!r}.wrapper must name a non-empty link symbol")
    capabilities = raw.get("capabilities", ["emit-source", "compile-object"])
    if not isinstance(capabilities, list) or not capabilities or not all(
        isinstance(capability, str) and capability for capability in capabilities
    ):
        raise RustAdapterError(f"dci-stub {request_id!r}.capabilities must be non-empty strings")
    request: dict[str, Any] = {
        "id": request_id,
        "kind": kind,
        "reason": raw.get("reason", "requires_stub"),
        "capabilities": capabilities,
        "wrapper": {"link_name": wrapper_name},
        "control_flow": {
            "default_boundary": "no_unwind",
            "propagation": {"mode": "forbidden"},
        },
    }
    target = raw.get("target")
    if target is not None:
        if not isinstance(target, str) or not target:
            raise RustAdapterError(f"dci-stub {request_id!r}.target must be a non-empty string")
        request["target"] = target
    for abi_key in ("input", "output"):
        abi_value = raw.get(abi_key)
        if abi_value is not None:
            if not isinstance(abi_value, dict):
                raise RustAdapterError(f"dci-stub {request_id!r}.{abi_key} must be an object")
            request[abi_key] = abi_value
    operations = raw.get("operations")
    if operations is not None:
        if not isinstance(operations, list) or not all(
            isinstance(operation, dict) for operation in operations
        ):
            raise RustAdapterError(f"dci-stub {request_id!r}.operations must be a list of objects")
        request["operations"] = operations
    return request


def probe_source(crate: ParsedCrate, records: Sequence[tuple[tuple[str, ...], list[str]]],
                 enums: Sequence[tuple[str, ...]], prefix: str) -> str:
    lines = ["", "// Automatically appended to a temporary source copy by the Vyx DCI Adapter."]
    def append_probe(path: tuple[str, ...], declaration: str) -> None:
        declared = crate.records.get(path) or crate.enums.get(path)
        for cfg in getattr(declared, "cfg_attributes", []):
            lines.append(f"#[{cfg}]")
        lines.append(declaration)

    probe_index = 0
    for path, fields in records:
        declared_record = crate.records.get(path)
        rust_path = declared_record.rust_path if declared_record is not None \
            else "crate::" + "::".join(path)
        for declaration in [
            f'#[unsafe(no_mangle)] pub extern "C" fn {prefix}_size_{probe_index}() -> usize '
            f'{{ core::mem::size_of::<{rust_path}>() }}',
            f'#[unsafe(no_mangle)] pub extern "C" fn {prefix}_align_{probe_index}() -> usize '
            f'{{ core::mem::align_of::<{rust_path}>() }}',
        ]:
            append_probe(path, declaration)
        for field_index, field_name in enumerate(fields):
            append_probe(
                path,
                f'#[unsafe(no_mangle)] pub extern "C" fn {prefix}_offset_{probe_index}_{field_index}() -> usize '
                f'{{ core::mem::offset_of!({rust_path}, {field_name}) }}'
            )
        for declaration in [
            f'#[unsafe(no_mangle)] pub extern "C" fn {prefix}_arg_{probe_index}(value: {rust_path}) '
            '{ core::mem::forget(value); }',
            f'#[unsafe(no_mangle)] pub unsafe extern "C" fn {prefix}_ret_{probe_index}() -> {rust_path} '
            '{ unsafe { core::hint::unreachable_unchecked() } }',
            f'#[unsafe(no_mangle)] pub extern "Rust" fn {prefix}_rust_arg_{probe_index}(value: {rust_path}) '
            '{ core::mem::forget(value); }',
            f'#[unsafe(no_mangle)] pub unsafe extern "Rust" fn {prefix}_rust_ret_{probe_index}() -> {rust_path} '
            '{ unsafe { core::hint::unreachable_unchecked() } }',
        ]:
            append_probe(path, declaration)
        probe_index += 1
    for path in enums:
        declared_enum = crate.enums.get(path)
        rust_path = declared_enum.rust_path if declared_enum is not None \
            else "crate::" + "::".join(path)
        for declaration in [
            f'#[unsafe(no_mangle)] pub extern "C" fn {prefix}_size_{probe_index}() -> usize '
            f'{{ core::mem::size_of::<{rust_path}>() }}',
            f'#[unsafe(no_mangle)] pub extern "C" fn {prefix}_align_{probe_index}() -> usize '
            f'{{ core::mem::align_of::<{rust_path}>() }}',
            f'#[unsafe(no_mangle)] pub extern "C" fn {prefix}_arg_{probe_index}(value: {rust_path}) '
            '{ core::mem::forget(value); }',
            f'#[unsafe(no_mangle)] pub unsafe extern "C" fn {prefix}_ret_{probe_index}() -> {rust_path} '
            '{ unsafe { core::hint::unreachable_unchecked() } }',
            f'#[unsafe(no_mangle)] pub extern "Rust" fn {prefix}_rust_arg_{probe_index}(value: {rust_path}) '
            '{ core::mem::forget(value); }',
            f'#[unsafe(no_mangle)] pub unsafe extern "Rust" fn {prefix}_rust_ret_{probe_index}() -> {rust_path} '
            '{ unsafe { core::hint::unreachable_unchecked() } }',
        ]:
            append_probe(path, declaration)
        probe_index += 1
    return "\n".join(lines) + "\n"


def llvm_unescape_name(name: str) -> str:
    if name.startswith('"') and name.endswith('"'):
        name = name[1:-1]
    return re.sub(r"\\([0-9A-Fa-f]{2})", lambda value: chr(int(value.group(1), 16)), name)


def llvm_functions(ir: str) -> dict[str, tuple[str, str]]:
    """Return link name -> (return fragment, argument fragment)."""
    result: dict[str, tuple[str, str]] = {}
    # LLVM function headers remain one logical line in rustc textual IR.  Names
    # with punctuation are quoted; escaped bytes are preserved as link identity.
    pattern = re.compile(r'^define\s+(.*?)\s+@(?:"((?:[^"\\]|\\.)*)"|([^\s(]+))\((.*)\)\s+.*\{\s*$', re.MULTILINE)
    for match in pattern.finditer(ir):
        name = llvm_unescape_name(f'"{match.group(2)}"' if match.group(2) is not None else match.group(3))
        result[name] = (match.group(1).strip(), match.group(4).strip())
    return result


def llvm_aliases(ir: str) -> dict[str, str]:
    """Map LLVM alias names to the function they point at.

    O3 folds identical constant probes into ``alias`` of another define.
    """
    pattern = re.compile(
        r'^@("(?:[^"\\]|\\.)+"|[A-Za-z0-9_.$]+)\s*=\s*'
        r'(?:(?:private|internal|weak|linkonce(?:_odr)?|weak_odr|external|'
        r'dso_local|hidden|protected|unnamed_addr|local_unnamed_addr)\s+)*'
        r'alias\s+.*,\s*ptr\s+@("(?:[^"\\]|\\.)+"|[A-Za-z0-9_.$]+)\s*$',
        re.MULTILINE,
    )
    result: dict[str, str] = {}
    for match in pattern.finditer(ir):
        result[llvm_unescape_name(match.group(1))] = llvm_unescape_name(match.group(2))
    return result


def llvm_resolve_alias(name: str, aliases: dict[str, str]) -> str:
    seen: set[str] = set()
    while name in aliases and name not in seen:
        seen.add(name)
        name = aliases[name]
    return name


def demangle_legacy_rust_symbol(name: str) -> tuple[str, ...] | None:
    """Decode rustc legacy (``_ZN``) mangling, dropping the ``h`` disambiguator."""
    if name.startswith("\x01"):
        name = name[1:]
    if not name.startswith("_ZN") or not name.endswith("E"):
        return None
    body = name[3:-1]
    parts: list[str] = []
    index = 0
    while index < len(body):
        if not body[index].isdigit():
            return None
        cursor = index
        while cursor < len(body) and body[cursor].isdigit():
            cursor += 1
        length = int(body[index:cursor])
        ident = body[cursor:cursor + length]
        if len(ident) != length:
            return None
        index = cursor + length
        if ident.startswith("h") and len(ident) >= 17 and all(
            ch in "0123456789abcdef" for ch in ident[1:]
        ):
            continue
        ident = ident.replace("$u20$", " ").replace("$LT$", "<").replace("$GT$", ">")
        ident = ident.replace("$u7b$", "{").replace("$u7d$", "}").replace("..", "::")
        parts.append(ident)
    return tuple(parts) if parts else None


def rustc_symbol_paths(
    functions: dict[str, tuple[str, str]], crate_name: str,
) -> dict[tuple[str, ...], str]:
    """Map crate-relative item paths to the rustc-emitted LLVM link name."""
    crate = crate_name.replace("-", "_")
    result: dict[tuple[str, ...], str] = {}
    for link_name, (header, _) in functions.items():
        parts = demangle_legacy_rust_symbol(link_name)
        if parts is None or not parts or parts[0] != crate:
            continue
        path = parts[1:]
        if not path or "{{closure}}" in path[-1] or path[-1].startswith("{closure"):
            continue
        result.setdefault(path, link_name)
    return result


def llvm_argument_parts(arguments: str) -> list[str]:
    if not arguments.strip():
        return []
    parts: list[str] = []
    current: list[str] = []
    stack: list[str] = []
    pairs = {"(": ")", "[": "]", "{": "}", "<": ">"}
    for ch in arguments:
        if ch in pairs:
            stack.append(pairs[ch])
        elif stack and ch == stack[-1]:
            stack.pop()
        if ch == "," and not stack:
            parts.append("".join(current).strip())
            current = []
        else:
            current.append(ch)
    if current:
        parts.append("".join(current).strip())
    return parts


def llvm_base_type(fragment: str) -> str:
    value = fragment.strip()
    # Return fragments contain linkage/attribute modifiers before the actual
    # scalar type.  The last known type-shaped token is the ABI carrier.
    matches = re.findall(r"(?:ptr|void|half|float|double|fp128|i\d+|\[[^\]]+\]|\{[^}]+\}|<[^>]+>)", value)
    return matches[-1] if matches else value.split()[-1]


def llvm_argument_type(fragment: str) -> str:
    """Return the leading LLVM carrier type, ignoring typed attributes."""
    value = fragment.strip()
    match = re.match(r"(ptr|void|half|float|double|fp128|i\d+|\[[^\]]+\]|\{[^}]+\}|<[^>]+>)", value)
    return match.group(1) if match else llvm_base_type(value)


def llvm_type_size(value: str, pointer_width: int) -> int | None:
    value = value.strip()
    if value == "ptr":
        return pointer_width // 8
    match = re.fullmatch(r"i(\d+)", value)
    if match:
        bits = int(match.group(1))
        return (bits + 7) // 8
    return {"half": 2, "float": 4, "double": 8, "fp128": 16}.get(value)


def carrier_type_json(carrier: str, pointer_width: int) -> dict[str, Any]:
    size = llvm_type_size(carrier, pointer_width)
    if carrier == "ptr":
        name = f"u{pointer_width}"
    elif carrier.startswith("i") and carrier[1:].isdigit():
        name = "u" + carrier[1:]
    elif carrier in {"half", "float", "double", "fp128"}:
        name = {"half": "f16", "float": "f32", "double": "f64", "fp128": "f128"}[carrier]
    else:
        name = carrier
    return {"name": name, "kind": "primitive", "nullable": False, "reference": "value",
            **({"size": size} if size is not None else {})}


def constant_return(ir: str, function_name: str) -> int:
    escaped = re.escape(function_name)
    match = re.search(
        rf'^define\s+[^@]+@{escaped}\([^)]*\).*?\{{(.*?)^\}}',
        ir, re.MULTILINE | re.DOTALL,
    )
    if not match:
        raise RustAdapterError(f"rustc did not emit layout probe {function_name}")
    returned = re.search(r"\bret\s+i\d+\s+(-?\d+)", match.group(1))
    if not returned:
        raise RustAdapterError(f"layout probe {function_name} was not constant-folded by rustc")
    return int(returned.group(1))


def lowering_from_probe(return_fragment: str, argument_fragments: list[str], *,
                        size: int, alignment: int, pointer_width: int,
                        returned: bool) -> ProbeLowering:
    if returned:
        if argument_fragments and "sret(" in argument_fragments[0]:
            attributes = ["sret"]
            if "noalias" in argument_fragments[0]:
                attributes.append("noalias")
            return ProbeLowering("sret", size, alignment, attributes=attributes)
        carrier = llvm_base_type(return_fragment)
        if carrier == "void":
            return ProbeLowering("ignore", size, alignment)
        carrier_size = llvm_type_size(carrier, pointer_width)
        if carrier_size is not None:
            return ProbeLowering("coerce", size, alignment, coerce_to=carrier)
        return ProbeLowering("direct", size, alignment, registers=[carrier])

    if not argument_fragments:
        return ProbeLowering("ignore", size, alignment)
    if len(argument_fragments) > 1:
        carriers = [llvm_argument_type(fragment) for fragment in argument_fragments]
        return ProbeLowering("split", size, alignment, registers=carriers)
    fragment = argument_fragments[0]
    carrier = llvm_argument_type(fragment)
    attributes: list[str] = []
    if "byval(" in fragment:
        attributes.append("byval")
        return ProbeLowering("byval", size, alignment, attributes=attributes)
    if "byref(" in fragment:
        attributes.append("byref")
        return ProbeLowering("indirect", size, alignment, attributes=attributes)
    if carrier == "ptr" and size != pointer_width // 8:
        return ProbeLowering("indirect", size, alignment)
    if carrier.startswith(("i", "f")) or carrier in {"half", "float", "double", "fp128", "ptr"}:
        return ProbeLowering("coerce", size, alignment, coerce_to=carrier)
    return ProbeLowering("direct", size, alignment, registers=[carrier])


def compile_probes(root: Path, crate: ParsedCrate,
                   probe_records: Sequence[tuple[tuple[str, ...], list[str]]],
                   probe_enums: Sequence[tuple[str, ...]], rustc: str, target: TargetInfo,
                   edition: str, rustc_args: Sequence[str]) -> tuple[dict[tuple[str, ...], ProbeFact], str]:
    prefix = "__vyx_dci_" + uuid.uuid4().hex[:12]
    ordered_paths = [path for path, _ in probe_records] + list(probe_enums)
    fields_by_path = {path: fields for path, fields in probe_records}
    with tempfile.TemporaryDirectory(prefix="vyx-rust-dci-") as temp_text:
        temp = Path(temp_text)
        copied_root = copy_crate_tree(root, temp)
        copied_text = copied_root.read_text(encoding="utf-8")
        copied_text = re.sub(
            r"(?m)^\s*#!\[\s*crate_(?:type|name)\s*=.*?\]\s*$",
            "",
            copied_text,
        )
        if "improper_ctypes_definitions" not in copied_text:
            copied_text = "#![allow(improper_ctypes_definitions)]\n" + copied_text
        copied_root.write_text(copied_text, encoding="utf-8", newline="\n")
        with copied_root.open("a", encoding="utf-8", newline="\n") as stream:
            stream.write(crate.generated_source)
            stream.write(probe_source(crate, probe_records, probe_enums, prefix))
        ir_path = temp / "probe.ll"
        command = [
            rustc, str(copied_root), "--crate-name", "vyx_dci_probe", "--crate-type", "lib",
            "--emit=llvm-ir", "-C", "panic=abort",
            "--edition", edition, "--target", target.triple, "-o", str(ir_path),
        ]
        command.extend(rustc_args)
        try:
            run_checked(
                command, cwd=copied_root.parent,
                env=probe_environment(root, temp, rustc_args), timeout=180,
            )
        except RustAdapterError as exc:
            message = str(exc).replace(str(copied_root.parent), str(root.parent))
            message = message.replace(str(temp), "<dci-probe>")
            raise RustAdapterError(message) from exc
        try:
            ir = ir_path.read_text(encoding="utf-8")
        except (OSError, UnicodeError) as exc:
            raise RustAdapterError(f"cannot read rustc LLVM probe output: {exc}") from exc

    functions = llvm_functions(ir)
    aliases = llvm_aliases(ir)
    facts: dict[tuple[str, ...], ProbeFact] = {}
    for index, path in enumerate(ordered_paths):
        size_name = llvm_resolve_alias(f"{prefix}_size_{index}", aliases)
        if size_name not in functions:
            # The declaration and its probes were removed by the selected cfg.
            continue
        size = constant_return(ir, size_name)
        alignment = constant_return(ir, llvm_resolve_alias(f"{prefix}_align_{index}", aliases))
        if size == 0:
            # ZSTs have no C object representation.  Omit their layout and let
            # any attempted by-value export be rejected for lacking a verified
            # probe rather than aborting unrelated exports.
            continue
        offsets = [
            constant_return(
                ir, llvm_resolve_alias(f"{prefix}_offset_{index}_{field_index}", aliases),
            )
            for field_index, _ in enumerate(fields_by_path.get(path, []))
        ]
        arg_signature = functions.get(llvm_resolve_alias(f"{prefix}_arg_{index}", aliases))
        ret_signature = functions.get(llvm_resolve_alias(f"{prefix}_ret_{index}", aliases))
        if arg_signature is None or ret_signature is None:
            raise RustAdapterError(f"rustc omitted ABI probes for {'::'.join(path)}")
        arg_parts = llvm_argument_parts(arg_signature[1])
        ret_parts = llvm_argument_parts(ret_signature[1])
        rust_arg_signature = functions.get(llvm_resolve_alias(f"{prefix}_rust_arg_{index}", aliases))
        rust_ret_signature = functions.get(llvm_resolve_alias(f"{prefix}_rust_ret_{index}", aliases))
        rust_param = None
        rust_returned = None
        if rust_arg_signature is not None and rust_ret_signature is not None:
            rust_param = lowering_from_probe(
                rust_arg_signature[0], llvm_argument_parts(rust_arg_signature[1]),
                size=size, alignment=alignment, pointer_width=target.pointer_width, returned=False,
            )
            rust_returned = lowering_from_probe(
                rust_ret_signature[0], llvm_argument_parts(rust_ret_signature[1]),
                size=size, alignment=alignment, pointer_width=target.pointer_width, returned=True,
            )
        facts[path] = ProbeFact(
            LayoutFact(size, alignment, offsets),
            lowering_from_probe(arg_signature[0], arg_parts, size=size, alignment=alignment,
                                pointer_width=target.pointer_width, returned=False),
            lowering_from_probe(ret_signature[0], ret_parts, size=size, alignment=alignment,
                                pointer_width=target.pointer_width, returned=True),
            rust_param,
            rust_returned,
        )
    return facts, ir


def unmanaged_pub_items(crate: ParsedCrate) -> list[FunctionDecl]:
    items: list[FunctionDecl] = []
    for function in crate.functions:
        if (not function.public or function.has_stable_link_name or function.generic
                or function.path == ("main",)):
            continue
        items.append(function)
    return items


def unmanaged_pub_fn_type(function: FunctionDecl) -> str:
    params = ", ".join(tokens_text(param.type_tokens) for param in function.params)
    returned = tokens_text(function.return_tokens) or "()"
    abi = function.abi.strip()
    if abi.lower() in {"", "rust"}:
        return f"fn({params}) -> {returned}"
    return f'extern "{abi}" fn({params}) -> {returned}'


def unmanaged_pub_keep_source(crate: ParsedCrate) -> str:
    """Force rustc to codegen every unadorned pub item at the requested opt-level."""
    lines: list[str] = [""]
    for kept, function in enumerate(unmanaged_pub_items(crate)):
        fn_ty = unmanaged_pub_fn_type(function)
        item = "crate::" + "::".join(function.path)
        lines.append("#[used]")
        lines.append(f"static __VYX_DCI_KEEP_{kept}: {fn_ty} = {item} as {fn_ty};")
    if len(lines) == 1:
        return ""
    return "\n".join(lines) + "\n"


def llvm_define_is_linker_visible(header: str) -> bool:
    for token in header.replace(",", " ").split():
        if token in {"internal", "private", "available_externally"}:
            return False
    return True


def unmanaged_pub_reexport_source(
    crate_name: str, crate: ParsedCrate, functions: dict[str, tuple[str, str]],
) -> str:
    """Give local O3 symbols a global linker name matching rustc's mangled identity."""
    paths = rustc_symbol_paths(functions, crate_name)
    lines: list[str] = []
    index = 0
    for function in unmanaged_pub_items(crate):
        link = paths.get(function.path, "")
        if not link:
            continue
        header = functions.get(link, ("", ""))[0]
        if llvm_define_is_linker_visible(header):
            continue
        params: list[str] = []
        args: list[str] = []
        for i, param in enumerate(function.params):
            name = f"a{i}"
            params.append(f"{name}: {tokens_text(param.type_tokens)}")
            args.append(name)
        returned = tokens_text(function.return_tokens) or "()"
        item = crate_name + "::" + "::".join(function.path)
        abi = function.abi.strip() or "Rust"
        lines.append(f'#[unsafe(export_name = "{link}")]')
        lines.append(
            f'pub unsafe extern "{abi}" fn __vyx_dci_reexport_{index}'
            f'({", ".join(params)}) -> {returned} {{'
        )
        lines.append(f"    {item}({', '.join(args)})")
        lines.append("}")
        lines.append("")
        index += 1
    return "\n".join(lines)



def compile_identity_ir(
    root: Path, crate: ParsedCrate, crate_name: str, rustc: str, target: TargetInfo,
    edition: str, rustc_args: Sequence[str],
) -> str:
    """Compile the crate so mangled ``pub`` symbols keep this rustc's identity."""
    with tempfile.TemporaryDirectory(prefix="vyx-rust-dci-id-") as temp_text:
        temp = Path(temp_text)
        copied_root = copy_crate_tree(root, temp)
        keep = crate.generated_source + unmanaged_pub_keep_source(crate)
        if keep:
            copied_root.write_text(
                copied_root.read_text(encoding="utf-8") + keep, encoding="utf-8", newline="\n",
            )
        ir_path = temp / "crate.ll"
        command = [
            rustc, str(copied_root), "--crate-name", crate_name, "--crate-type", "lib",
            "--emit=llvm-ir", "-C", "panic=abort",
            "--edition", edition, "--target", target.triple, "-o", str(ir_path),
        ]
        command.extend(rustc_args)
        try:
            run_checked(
                command, cwd=copied_root.parent, env=probe_environment(root, temp, rustc_args), timeout=180,
            )
        except RustAdapterError as exc:
            message = str(exc).replace(str(copied_root.parent), str(root.parent))
            message = message.replace(str(temp), "<dci-identity>")
            raise RustAdapterError(message) from exc
        try:
            return ir_path.read_text(encoding="utf-8")
        except (OSError, UnicodeError) as exc:
            raise RustAdapterError(f"cannot read rustc identity LLVM output: {exc}") from exc


def crate_defines_main(crate: ParsedCrate) -> bool:
    return any(function.path[-1] == "main" for function in crate.functions)


def trait_dyn_link_name(trait_name: str, method_name: str) -> str:
    return f"dci.rust.dyn.{trait_name}.{method_name}"


def trait_method_receiver_kind(method: TraitMethod) -> str:
    if not method.params:
        return ""
    compact = re.sub(
        r"'[A-Za-z_][A-Za-z0-9_]*",
        "",
        re.sub(r"\s+", "", tokens_text(method.params[0].type_tokens)),
    )
    if method.params[0].name == "self" or compact in {"self", "&self", "&mutself"}:
        if compact == "self":
            return "by_value"
        if compact == "&mutself":
            return "mut_ref"
        return "ref"
    return ""


def trait_dispatch_entries(
    trait_decl: TraitDecl, trait_name: str, pointer_size: int,
) -> list[dict[str, Any]]:
    entries: list[dict[str, Any]] = []
    for index, name in enumerate(["drop_in_place", "size", "align"]):
        entries.append({
            "index": index,
            "offset": index * pointer_size,
            "address_point_relative_offset": index * pointer_size,
            "kind": "metadata",
            "member_name": name,
        })
    for method_index, method in enumerate(trait_decl.methods):
        index = RUST_VTABLE_HEADER_SLOTS + method_index
        link = trait_dyn_link_name(trait_name, method.name)
        entries.append({
            "index": index,
            "offset": index * pointer_size,
            "address_point_relative_offset": index * pointer_size,
            "kind": "method",
            "owner": trait_name,
            "member_name": method.name,
            "link_name": link,
            "mangled": link,
        })
    return entries


def compile_trait_dispatch(
    root: Path, crate: ParsedCrate, types: TypeSystem, rustc: str, target: TargetInfo,
    edition: str, rustc_args: Sequence[str],
) -> list[dict[str, Any]]:
    """Run a rustc-built probe binary to dump in-crate dyn Trait vtables."""
    if crate_defines_main(crate) or not crate.traits or not crate.trait_impls:
        return []
    # A generic trait's `dyn` object is only nameable as a closed instance
    # (`&dyn Sink<i32>`), and the impl header this walk sees does not carry the
    # instance arguments -- so the probe would emit `&dyn crate::Sink` and fail
    # to compile, taking every other table down with it.  Closed instances get
    # their table from the stub the producer compiler builds instead.
    impls = [
        impl for impl in crate.trait_impls
        if impl.trait_path in crate.traits and impl.type_path in crate.records
        and crate.traits[impl.trait_path].public
        and not crate.traits[impl.trait_path].type_params
    ]
    if not impls:
        return []
    dump_lines = [
        "",
        "fn main() {",
        "    let mut out = String::from(\"[\");",
        "    let mut first = true;",
    ]
    for impl in impls:
        trait_decl = crate.traits[impl.trait_path]
        record = crate.records[impl.type_path]
        slot_count = RUST_VTABLE_HEADER_SLOTS + len(trait_decl.methods)
        trait_name = types.dci_name(impl.trait_path)
        type_name = types.dci_name(impl.type_path)
        if record.tuple_struct:
            zeros = ", ".join("unsafe { core::mem::zeroed() }" for _ in record.fields)
            construct = f"{record.rust_path}({zeros})"
        elif record.fields:
            zeros = ", ".join(
                f"{field.name}: unsafe {{ core::mem::zeroed() }}" for field in record.fields
            )
            construct = f"{record.rust_path} {{ {zeros} }}"
        else:
            construct = record.rust_path
        dump_lines.extend([
            "    {",
            f"        let holder = {construct};",
            f"        let obj: &dyn {trait_decl.rust_path} = &holder;",
            "        let fat: [*const (); 2] = unsafe { core::mem::transmute_copy(&obj) };",
            "        let vtbl = fat[1] as *const usize;",
            "        if !first { out.push(','); }",
            "        first = false;",
            f"        out.push_str(\"{{\\\"trait\\\":\\\"{trait_name}\\\",\\\"type\\\":\\\"{type_name}\\\",\\\"slots\\\":[\");",
            f"        for index in 0usize..{slot_count}usize {{",
            "            if index > 0 { out.push(','); }",
            "            out.push_str(&unsafe { *vtbl.add(index) }.to_string());",
            "        }",
            "        out.push_str(\"]}\");",
            "    }",
        ])
    dump_lines.extend([
        "    out.push(']');",
        "    let path = std::env::args().nth(1).expect(\"vtable dump path\");",
        "    std::fs::write(path, out).expect(\"write vtable dump\");",
        "}",
        "",
    ])
    with tempfile.TemporaryDirectory(prefix="vyx-rust-dci-vtable-") as temp_text:
        temp = Path(temp_text)
        copied_root = copy_crate_tree(root, temp)
        copied_text = copied_root.read_text(encoding="utf-8")
        copied_text = re.sub(
            r"(?m)^\s*#!\[\s*crate_(?:type|name)\s*=.*?\]\s*$",
            "",
            copied_text,
        )
        copied_text = "#![allow(improper_ctypes_definitions, invalid_value)]\n" + copied_text
        copied_root.write_text(copied_text + "\n" + "\n".join(dump_lines), encoding="utf-8", newline="\n")
        exe_path = temp / ("vyx_dci_vtable.exe" if os.name == "nt" else "vyx_dci_vtable")
        dump_path = temp / "vtables.json"
        command = [
            rustc, str(copied_root), "--crate-name", "vyx_dci_vtable_probe", "--crate-type", "bin",
            "-C", "panic=abort", "--edition", edition,
            "--target", target.triple, "-o", str(exe_path),
        ]
        command.extend(rustc_args)
        try:
            run_checked(command, cwd=copied_root.parent, env=probe_environment(root, temp, rustc_args), timeout=180)
            run_checked([str(exe_path), str(dump_path)], cwd=temp, timeout=30)
        except RustAdapterError:
            return []
        try:
            raw = json.loads(dump_path.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError):
            return []
    tables: list[dict[str, Any]] = []
    pointer_size = target.pointer_width // 8
    for item in raw:
        if not isinstance(item, dict):
            continue
        trait_name = str(item.get("trait", ""))
        type_name = str(item.get("type", ""))
        slots = item.get("slots")
        if not trait_name or not type_name or not isinstance(slots, list):
            continue
        trait_path = next((path for path, decl in crate.traits.items()
                           if types.dci_name(path) == trait_name), None)
        methods = crate.traits[trait_path].methods if trait_path is not None else []
        entries: list[dict[str, Any]] = []
        header = ["drop_in_place", "size", "align"]
        for index, name in enumerate(header):
            if index >= len(slots):
                break
            entries.append({
                "index": index,
                "offset": index * pointer_size,
                "address_point_relative_offset": index * pointer_size,
                "kind": "metadata",
                "member_name": name,
            })
        for method_index, method in enumerate(methods):
            index = RUST_VTABLE_HEADER_SLOTS + method_index
            if index >= len(slots):
                break
            link = trait_dyn_link_name(trait_name, method.name)
            entries.append({
                "index": index,
                "offset": index * pointer_size,
                "address_point_relative_offset": index * pointer_size,
                "kind": "method",
                "owner": trait_name,
                "member_name": method.name,
                "link_name": link,
                "mangled": link,
            })
        tables.append({
            "class_name": type_name,
            "base_class": trait_name,
            "address_point_offset": 0,
            "entries": entries,
        })
    return tables


def export_public_traits(
    crate: ParsedCrate, types: TypeSystem, target: TargetInfo,
    facts: dict[tuple[str, ...], ProbeFact],
) -> tuple[list[dict[str, Any]], list[dict[str, Any]], list[dict[str, Any]]]:
    """Export object-safe public traits as inheritable DCI layouts.

    rustc trait objects keep the vtable next to the data pointer, not inside
    the object.  Reverse-override stubs store that vtable pointer at offset 0
    so the existing Consumer virtual-call lowering (load vptr from object+0)
    matches the rustc-generated dyn vtable.
    """
    layouts: list[dict[str, Any]] = []
    symbols: list[dict[str, Any]] = []
    tables: list[dict[str, Any]] = []
    pointer_size = target.pointer_width // 8
    for path, trait_decl in crate.traits.items():
        if not trait_decl.public or not trait_decl.methods:
            continue
        # A synthetic closed instance of a generic trait keeps its consumer
        # facing DCI name (`native.Sink<i32>`), which is what the Vyx side
        # spells in `class VyxHost : native.Sink<i32>`; the plain
        # `types.dci_name(path)` would spell the internal synthetic path.
        trait_name = types.instance_names.get(path, types.dci_name(path))
        method_payloads: list[tuple[TraitMethod, list[tuple[ParamDecl, RustType]], RustType, str]] = []
        skip = False
        for method in trait_decl.methods:
            recv = trait_method_receiver_kind(method)
            if recv not in {"ref", "mut_ref"}:
                skip = True
                break
            parsed: list[tuple[ParamDecl, RustType]] = []
            for param in method.params[1:]:
                value = types.parse(param.type_tokens, path[:-1])
                ok, _ = types.is_stable(value)
                if not ok:
                    skip = True
                    break
                parsed.append((param, value))
            if skip:
                break
            if method.return_tokens:
                returned = types.parse(method.return_tokens, path[:-1])
            else:
                returned = RustType("void", "()", name="void")
            ok, _ = types.is_stable(returned)
            if not ok:
                skip = True
                break
            try:
                for _, value in parsed:
                    value_lowering(value, types, facts, False, rust_abi=True)
                value_lowering(returned, types, facts, True, rust_abi=True)
            except RustAdapterError:
                skip = True
                break
            method_payloads.append((method, parsed, returned, recv))
        if skip or not method_payloads:
            continue
        layouts.append({
            "type_name": trait_name,
            "size": pointer_size,
            "alignment": pointer_size,
            "representation": "native",
            "has_vtable": True,
            "is_pod": False,
            "is_trivially_destructible": True,
            "bases": [],
            "fields": [],
            "lifecycle": {
                "ownership_model": "value",
                "copy_semantics": "trivial",
                "move_semantics": "trivial",
                "destruction": "trivial",
                "moved_from_state": "valid",
                "operations": {},
            },
            "rust": {"path": trait_decl.rust_path, "kind": "trait"},
        })
        tables.append({
            "class_name": trait_name,
            "base_class": trait_name,
            "address_point_offset": 0,
            "entries": trait_dispatch_entries(trait_decl, trait_name, pointer_size),
        })
        for method, parsed, returned, recv in method_payloads:
            link = trait_dyn_link_name(trait_name, method.name)
            parameters: list[dict[str, Any]] = []
            lowerings: list[dict[str, Any]] = []
            for index, (param, value) in enumerate(parsed):
                parameters.append({
                    "name": param.name,
                    "type": types.type_json(value),
                    "location": "abi",
                    "ownership": parameter_ownership(value),
                })
                lowerings.append(lowering_json(
                    value_lowering(value, types, facts, False, rust_abi=True), target, index,
                ))
            returned_json: dict[str, Any] | None = None
            return_lowering = value_lowering(returned, types, facts, True, rust_abi=True)
            if returned.kind not in {"void", "never"}:
                returned_json = {
                    "type": types.type_json(returned),
                    "location": "abi",
                    "ownership": return_ownership(returned),
                }
            symbols.append({
                "name": "::".join(path + (method.name,)),
                "semantic_id": f"{trait_name}::{method.name}",
                "link_name": link,
                "mangled": link,
                "kind": "method",
                "owner": trait_name,
                "member_name": method.name,
                "calling_convention": "rust",
                "linkage": "external",
                "visibility": "public",
                "is_static": False,
                "is_virtual": True,
                "is_override": False,
                "is_final": False,
                "is_const": False,
                "this_adjust": 0,
                "params": parameters,
                "return": returned_json,
                "abi": {
                    "calling_convention": "rust",
                    "receiver": {
                        "passing": "direct",
                        "type": {"name": trait_name, "kind": "class", "reference": "pointer"},
                        "this_adjust": 0,
                        "ownership": "borrow_mut" if recv == "mut_ref" else "borrow",
                    },
                    "variadic": False,
                    "parameters": lowerings,
                    "return": lowering_json(return_lowering, target),
                },
                "control_flow": {
                    "boundary": "no_unwind",
                    "default_boundary": "no_unwind",
                    "unwind": "no_unwind",
                    "propagation": {"mode": "forbidden"},
                },
            })
    return layouts, symbols, tables


def scalar_layout(value: RustType, types: TypeSystem, facts: dict[tuple[str, ...], ProbeFact]) -> tuple[int, int]:
    scalar = types.size_of_scalar(value)
    if scalar is not None:
        return scalar, scalar
    if value.kind in {"record", "enum"}:
        path = types.decl_path_for(value)
        if path in facts:
            fact = facts[path]
            return fact.layout.size, fact.layout.alignment
    raise RustAdapterError(f"no verified layout for {value.spelling!r}")


def lowering_json(lowering: ProbeLowering, target: TargetInfo, index: int | None = None) -> dict[str, Any]:
    result: dict[str, Any] = {"passing": lowering.passing}
    if index is not None:
        result["index"] = index
    if lowering.passing in {"indirect", "byval", "sret"}:
        result["size"] = lowering.size
        result["alignment"] = lowering.alignment
    elif lowering.passing == "coerce":
        result["size"] = lowering.size
        result["coerce_to"] = carrier_type_json(lowering.coerce_to, target.pointer_width)
    elif lowering.passing == "split":
        result["size"] = lowering.size
        result["registers"] = lowering.registers
        cursor = 0
        offsets: list[int] = []
        for carrier in lowering.registers:
            offsets.append(cursor)
            piece = llvm_type_size(carrier, target.pointer_width)
            cursor += piece if piece is not None else 8
        result["register_offsets"] = offsets
    elif lowering.passing == "direct":
        result["size"] = lowering.size
    if lowering.attributes:
        result["attributes"] = lowering.attributes
    return result


def fat_pointer_lowering(value: RustType, types: TypeSystem, rust_abi: bool) -> ProbeLowering:
    width = types.target.pointer_width // 8
    size = width * 2
    element = value.element
    if rust_abi:
        if element is not None and element.kind == "dyn":
            return ProbeLowering("split", size, width, registers=["ptr", "ptr"])
        return ProbeLowering("split", size, width, registers=["ptr", f"i{types.target.pointer_width}"])
    return ProbeLowering("indirect", size, width)


def value_lowering(value: RustType, types: TypeSystem,
                   facts: dict[tuple[str, ...], ProbeFact], returned: bool,
                   rust_abi: bool = False) -> ProbeLowering:
    if value.kind in {"void", "never"}:
        return ProbeLowering("ignore", 0, 1)
    if value.kind in {"pointer", "reference"} and (
            value.fat or (value.element is not None and _is_unsized(value.element))):
        return fat_pointer_lowering(value, types, rust_abi)
    scalar = types.size_of_scalar(value)
    if scalar is not None:
        return ProbeLowering("direct", scalar, min(max(scalar, 1), target_alignment_cap(types.target)))
    if value.kind == "enum":
        path = types.decl_path_for(value)
        if path not in facts:
            raise RustAdapterError(f"missing enum ABI probe for {value.name}")
        fact = facts[path]
        if rust_abi and fact.rust_returned is not None and fact.rust_param is not None:
            return fact.rust_returned if returned else fact.rust_param
        return fact.returned if returned else fact.param
    if value.kind == "record":
        path = types.decl_path_for(value)
        if path not in facts:
            raise RustAdapterError(f"missing record ABI probe for {value.name}")
        fact = facts[path]
        if rust_abi and fact.rust_returned is not None and fact.rust_param is not None:
            return fact.rust_returned if returned else fact.rust_param
        return fact.returned if returned else fact.param
    raise RustAdapterError(f"cannot lower unsupported ABI type {value.spelling!r}")


def target_alignment_cap(target: TargetInfo) -> int:
    return max(1, target.pointer_width // 8)


def parameter_ownership(value: RustType) -> str:
    if value.kind in {"pointer", "reference"}:
        return "borrow_mut" if value.mutable else "borrow"
    return "copy"


def return_ownership(value: RustType) -> str:
    if value.kind in {"pointer", "function", "reference"}:
        return "borrow"
    return "copy"


def rust_translate_leaf(value: RustType, types: TypeSystem) -> bool:
    if value.kind in {"void", "never", "primitive", "pointer"}:
        return True
    return False


def rust_can_translate(parsed_params: Sequence[RustType], returned: RustType,
                       types: TypeSystem) -> bool:
    if any(not rust_translate_leaf(value, types) for value in parsed_params):
        return False
    if returned.kind == "result":
        if len(returned.params) != 2:
            return False
        ok_ty, err_ty = returned.params
        return (
            ok_ty.kind in {"primitive", "void", "never"}
            and err_ty.kind == "primitive"
            and types.size_of_scalar(err_ty) is not None
        )
    return returned.kind in {"void", "never", "primitive"}


def rust_ok_type(returned: RustType) -> RustType:
    if returned.kind == "result" and returned.params:
        return returned.params[0]
    return returned


def rust_align_up(value: int, align: int) -> int:
    if align <= 1:
        return value
    return (value + align - 1) // align * align


def rust_failure_layout() -> dict[str, Any]:
    def field(name: str, offset: int, type_name: str, pointer: bool) -> dict[str, Any]:
        return {
            "name": name,
            "offset": offset,
            "type": {
                "name": type_name,
                "kind": "primitive",
                "nullable": pointer,
                "reference": "pointer" if pointer else "value",
            },
            "visibility": "public",
        }
    return {
        "type_name": "dci.Failure",
        "size": 64,
        "alignment": 8,
        "representation": "stable",
        "has_vtable": False,
        "is_pod": False,
        "is_trivially_destructible": False,
        "bases": [],
        "fields": [
            field("type_identity", 0, "i8", True),
            field("type_identity_len", 8, "u64", False),
            field("message", 16, "i8", True),
            field("message_len", 24, "u64", False),
            field("payload", 32, "u8", True),
            field("payload_size", 40, "u64", False),
            field("payload_align", 48, "u64", False),
            field("producer_tag", 56, "u32", False),
        ],
        "lifecycle": {
            "ownership_model": "value",
            "copy_semantics": "operation",
            "move_semantics": "trivial",
            "destruction": "operation",
            "moved_from_state": "unspecified",
            "operations": {
                "copy": {"symbol": "dci_failure_copy", "availability": "required", "no_unwind": True},
                "destroy": {"symbol": "dci_failure_destroy", "availability": "required", "no_unwind": True},
            },
        },
    }


def rust_translated_layout(record_name: str, ok: RustType | None, types: TypeSystem,
                           destroy_link: str) -> dict[str, Any]:
    fields = [{"name": "tag", "offset": 0,
               "type": {"name": "i8", "kind": "primitive", "nullable": False, "reference": "value"},
               "visibility": "public"}]
    offset = 1
    align = 8
    if ok is not None and ok.kind not in {"void", "never"}:
        ok_size = types.size_of_scalar(ok) or types.target.pointer_width // 8
        ok_align = min(max(ok_size, 1), 8)
        offset = rust_align_up(offset, ok_align)
        fields.append({
            "name": "ok",
            "offset": offset,
            "type": types.type_json(ok),
            "visibility": "public",
        })
        offset += ok_size
        align = max(align, ok_align)
    offset = rust_align_up(offset, 8)
    fields.append({
        "name": "err",
        "offset": offset,
        "type": {"name": "dci.Failure", "kind": "class", "nullable": False, "reference": "value"},
        "visibility": "public",
    })
    offset += 64
    size = rust_align_up(offset, align)
    return {
        "type_name": record_name,
        "size": size,
        "alignment": align,
        "representation": "stable",
        "has_vtable": False,
        "is_pod": False,
        "is_trivially_destructible": False,
        "bases": [],
        "fields": fields,
        "lifecycle": {
            "ownership_model": "value",
            "copy_semantics": "forbidden",
            "move_semantics": "trivial",
            "destruction": "operation",
            "moved_from_state": "unspecified",
            "operations": {
                "destroy": {"symbol": destroy_link, "availability": "required", "no_unwind": True},
            },
        },
    }


def rust_translator_link(export_name: str, used: set[str]) -> str:
    token = re.sub(r"[^A-Za-z0-9_]", "_", export_name).strip("_") or "sym"
    if token[0].isdigit():
        token = "x_" + token
    base = "dci_tr_" + token
    candidate = base
    serial = 2
    while candidate in used:
        candidate = f"{base}_{serial}"
        serial += 1
    used.add(candidate)
    return candidate


def rust_failure_helper_symbols() -> list[dict[str, Any]]:
    failure_ptr = {
        "name": "dci.Failure",
        "kind": "class",
        "nullable": False,
        "reference": "pointer",
    }
    def fn(name: str, params: list[dict[str, Any]], ret: dict[str, Any] | None) -> dict[str, Any]:
        return {
            "name": name,
            "link_name": name,
            "kind": "function",
            "calling_convention": "c",
            "linkage": "external",
            "visibility": "public",
            "params": params,
            "return": ret,
            "abi": {
                "calling_convention": "c",
                "variadic": False,
                "parameters": [
                    {"index": i, "passing": "direct"} for i, _ in enumerate(params)
                ],
                "return": {"passing": "direct", "size": 4} if ret else {"passing": "direct"},
            },
            "control_flow": {
                "boundary": "no_unwind",
                "default_boundary": "no_unwind",
                "unwind": "no_unwind",
                "propagation": {"mode": "forbidden"},
                "boundary_action": "direct",
            },
        }
    return [
        fn(
            "dci_failure_destroy",
            [{"name": "failure", "type": failure_ptr, "location": "abi", "ownership": "borrow_mut"}],
            None,
        ),
        fn(
            "dci_failure_copy",
            [
                {"name": "src", "type": {**failure_ptr, "pointee_const": True}, "location": "abi", "ownership": "borrow"},
                {"name": "dst", "type": failure_ptr, "location": "abi", "ownership": "borrow_mut"},
            ],
            {
                "type": {"name": "i32", "kind": "primitive", "nullable": False, "reference": "value"},
                "location": "abi",
                "ownership": "copy",
            },
        ),
    ]


def emit_rust_translators(
    crate_functions: Sequence[FunctionDecl],
    parsed_by_export: dict[str, tuple[list[RustType], RustType]],
    types: TypeSystem,
    used_links: set[str],
) -> tuple[list[dict[str, Any]], list[dict[str, Any]], list[dict[str, Any]], str]:
    extra_symbols: list[dict[str, Any]] = []
    extra_layouts: list[dict[str, Any]] = []
    requests: list[dict[str, Any]] = []
    stub_fns: list[str] = []
    failure_path = (TOOL_DIR / "runtime" / "dci_rust_failure.rs").as_posix()
    extra_layouts.append(rust_failure_layout())
    extra_symbols.extend(rust_failure_helper_symbols())
    used_links.update(symbol["link_name"] for symbol in extra_symbols)
    emitted = 0
    for function in crate_functions:
        parsed = parsed_by_export.get(function.export_name)
        if parsed is None:
            continue
        parsed_params, returned = parsed
        if not rust_can_translate(parsed_params, returned, types):
            continue
        abi_lower = function.abi.lower()
        if returned.kind != "result" and abi_lower != "c-unwind":
            continue
        tr_link = rust_translator_link(function.export_name, used_links)
        destroy_link = rust_translator_link(tr_link + "_destroy", used_links)
        ok_ty = rust_ok_type(returned)
        record_name = "dci.Translated_" + tr_link
        layout = rust_translated_layout(record_name, ok_ty, types, destroy_link)
        extra_layouts.append(layout)
        param_json = []
        rust_params = []
        call_args = []
        for index, (param, value) in enumerate(zip(function.params, parsed_params)):
            param_json.append({
                "name": param.name,
                "type": types.type_json(value),
                "location": "abi",
                "ownership": parameter_ownership(value),
            })
            rust_params.append(f"{param.name}: {value.spelling}")
            call_args.append(param.name)
        extra_symbols.append({
            "name": function.export_name + "_translated",
            "link_name": tr_link,
            "kind": "function",
            "calling_convention": "c",
            "linkage": "external",
            "visibility": "public",
            "params": param_json,
            "return": {
                "type": {"name": record_name, "kind": "class", "nullable": False, "reference": "value"},
                "location": "abi",
                "ownership": "owned",
            },
            "abi": {
                "calling_convention": "c",
                "variadic": False,
                "parameters": [{"index": i, "passing": "direct"} for i, _ in enumerate(param_json)],
                "return": {
                    "passing": "sret",
                    "reason": "aggregate_abi",
                    "size": layout["size"],
                    "alignment": layout["alignment"],
                },
            },
            "control_flow": {
                "boundary": "no_unwind",
                "default_boundary": "no_unwind",
                "unwind": "no_unwind",
                "propagation": {"mode": "forbidden"},
                "boundary_action": "direct",
            },
        })
        extra_symbols.append({
            "name": destroy_link,
            "link_name": destroy_link,
            "kind": "function",
            "calling_convention": "c",
            "linkage": "external",
            "visibility": "public",
            "params": [{
                "name": "value",
                "type": {"name": record_name, "kind": "class", "reference": "pointer"},
                "location": "abi",
                "ownership": "borrow_mut",
            }],
            "return": None,
            "abi": {
                "calling_convention": "c",
                "variadic": False,
                "parameters": [{"index": 0, "passing": "direct"}],
                "return": {"passing": "direct"},
            },
            "control_flow": {
                "boundary": "no_unwind",
                "default_boundary": "no_unwind",
                "unwind": "no_unwind",
                "propagation": {"mode": "forbidden"},
                "boundary_action": "direct",
            },
        })
        requests.append({
            "id": f"translate.{function.export_name}",
            "kind": "operation_wrapper",
            "target": function.export_name,
            "synthesis": {"strategy": "translate_unwind", "target": function.export_name},
            "wrapper": {"link_name": tr_link},
        })
        ok_spell = "()" if ok_ty.kind in {"void", "never"} else (ok_ty.name or ok_ty.spelling)
        helper = "translate_result" if returned.kind == "result" else "translate_catch_unwind"
        joined_params = ", ".join(rust_params)
        joined_args = ", ".join(call_args)
        stub_fns.append(
            f"#[no_mangle]\n"
            f"pub extern \"C\" fn {tr_link}({joined_params}) "
            f"-> dci_failure::DciTranslated<{ok_spell}> {{\n"
            f"    dci_failure::{helper}(|| unsafe {{ {function.export_name}({joined_args}) }})\n"
            f"}}\n"
            f"#[no_mangle]\n"
            f"pub unsafe extern \"C\" fn {destroy_link}("
            f"value: *mut dci_failure::DciTranslated<{ok_spell}>) {{\n"
            f"    if !value.is_null() {{\n"
            f"        dci_failure::dci_failure_destroy(&mut (*value).err);\n"
            f"    }}\n"
            f"}}\n"
        )
        emitted += 1
    if emitted == 0:
        return [], [], [], ""
    stub_source = (
        "\n#[allow(dead_code)]\n"
        "mod dci_failure {\n"
        f'    include!("{failure_path}");\n'
        "}\n\n"
        "#[no_mangle]\n"
        "pub unsafe extern \"C\" fn dci_failure_destroy(failure: *mut dci_failure::DciFailure) {\n"
        "    if !failure.is_null() { dci_failure::dci_failure_destroy(&mut *failure); }\n"
        "}\n"
        "#[no_mangle]\n"
        "pub unsafe extern \"C\" fn dci_failure_copy(\n"
        "    src: *const dci_failure::DciFailure, dst: *mut dci_failure::DciFailure,\n"
        ") -> i32 {\n"
        "    if dst.is_null() { return 0; }\n"
        "    if src.is_null() { dci_failure::dci_failure_clear(&mut *dst); return 1; }\n"
        "    if dci_failure::dci_failure_copy(&*src, &mut *dst) { 1 } else { 0 }\n"
        "}\n\n"
        + "".join(stub_fns)
    )
    return extra_symbols, extra_layouts, requests, stub_source


def primitive_llvm_carrier(value: RustType, target: TargetInfo) -> str:
    if value.kind in {"pointer", "function", "reference"}:
        return "ptr"
    if value.kind != "primitive":
        return ""
    if value.name in {"usize", "isize"}:
        return f"i{target.pointer_width}"
    if value.name == "f32":
        return "float"
    if value.name == "f64":
        return "double"
    size = PRIMITIVE_SIZES.get(value.name)
    return f"i{size * 8}" if size else ""


def lowering_carriers(value: RustType, lowering: ProbeLowering,
                      target: TargetInfo) -> list[str]:
    if lowering.passing == "ignore":
        return []
    if lowering.passing in {"indirect", "byval", "sret"}:
        return ["ptr"]
    if lowering.passing == "split":
        return list(lowering.registers)
    if lowering.passing == "coerce":
        return [lowering.coerce_to]
    primitive = primitive_llvm_carrier(value, target)
    if primitive:
        return [primitive]
    if lowering.registers:
        return list(lowering.registers)
    return []


def verify_actual_function(function: FunctionDecl, parsed_params: Sequence[RustType], returned: RustType,
                           facts: dict[tuple[str, ...], ProbeFact], types: TypeSystem,
                           ir_functions: dict[str, tuple[str, str]], link_name: str) -> str:
    signature = ir_functions.get(link_name)
    if signature is None:
        return f"rustc did not emit link symbol {link_name!r} for the selected cfg/target"
    expected_carriers: list[str] = []
    rust_abi = function.abi.lower() == "rust"
    return_lower = value_lowering(returned, types, facts, True, rust_abi=rust_abi)
    if return_lower.passing == "sret":
        expected_carriers.append("ptr")
    for value in parsed_params:
        lowering = value_lowering(value, types, facts, False, rust_abi=rust_abi)
        expected_carriers.extend(lowering_carriers(value, lowering, types.target))
    actual_parts = llvm_argument_parts(signature[1])
    if len(actual_parts) != len(expected_carriers):
        return (
            f"rustc ABI lowering for {function.export_name!r} has {len(actual_parts)} machine arguments; "
            f"verified per-type lowering predicts {len(expected_carriers)}"
        )
    actual_carriers = [llvm_argument_type(fragment) for fragment in actual_parts]
    if actual_carriers != expected_carriers:
        return (
            f"rustc ABI carriers for {function.export_name!r} are {actual_carriers}; "
            f"verified per-type lowering predicts {expected_carriers}"
        )
    actual_sret = bool(actual_parts and "sret(" in actual_parts[0])
    if actual_sret != (return_lower.passing == "sret"):
        return f"rustc return lowering for {function.export_name!r} disagrees with the verified type probe"
    if return_lower.passing not in {"sret", "ignore"}:
        expected_return = lowering_carriers(returned, return_lower, types.target)
        actual_return = llvm_base_type(signature[0])
        if expected_return and actual_return != expected_return[0]:
            return (
                f"rustc return carrier for {function.export_name!r} is {actual_return}; "
                f"verified return probe predicts {expected_return[0]}"
            )
    return ""


def discover_instances(
    types: TypeSystem, crate: ParsedCrate, instance_specs: Sequence[str]
) -> None:
    """Register every closed generic instance the host references.

    Instantiation itself is done by rustc; the Adapter only walks the requested
    entry points (explicit ``--export-instance`` specs, concrete type aliases,
    exported function signatures and record fields) and records the closed
    instances so their monomorphized layout can be probed.  Open generics and
    foreign generics such as ``Vec<u8>`` are never instantiated here.
    """
    for spec in instance_specs:
        header = parse_generic_impl_header(lex_rust(spec))
        base_path = types.resolve_path("::".join(header[0]), ()) if header else ()
        value = types.parse(lex_rust(spec), ())
        path = types.decl_path_for(value)
        if base_path in crate.traits:
            # A closed instance of a generic *trait*: the request names the
            # trait object the Vyx host subclasses (`Sink<i32>`), not a value
            # type.  `ensure_trait_instance` did the work while parsing; the
            # synthetic descriptor is picked up by `export_public_traits`.
            if value.kind != "trait" or path is None:
                raise RustAdapterError(
                    f"--export-instance {spec!r} could not be closed: trait "
                    f"{'::'.join(base_path)!r} is not a monomorphizable public "
                    "generic trait, or one of its arguments is not a stable type"
                )
            continue
        if path is None or path not in types.instance_names:
            raise RustAdapterError(
                f"--export-instance {spec!r} did not resolve to a closed in-crate "
                "generic instance"
            )
        ok, reason = types.is_stable(value)
        if not ok:
            raise RustAdapterError(f"--export-instance {spec!r} is not stable: {reason}")

    for path, alias in list(crate.aliases.items()):
        if not alias.generic:
            types.parse(alias.target_tokens, path[:-1])
    for function in list(crate.functions):
        for param in function.params:
            types.parse(param.type_tokens, function.path[:-1])
        types.parse(function.return_tokens, function.path[:-1])

    # Fields (including fields of freshly synthesized instances) can reference
    # further closed instances; iterate to a fixed point with a bounded cap.
    scanned: set[tuple[str, ...]] = set()
    for _ in range(8):
        pending = [
            path for path in list(crate.records.keys()) if path not in scanned
        ]
        if not pending:
            break
        for path in pending:
            scanned.add(path)
            record = crate.records[path]
            for field_decl in record.fields:
                types.parse(field_decl.type_tokens, path[:-1])


def _instance_method_link_name(
    base_name: str, args: Sequence[str], member: str,
) -> str:
    text = "_".join([base_name, *args, member])
    return "vyx_" + re.sub(r"[^A-Za-z0-9]+", "_", text).strip("_").lower()


def discover_instance_methods(
    types: TypeSystem, crate: ParsedCrate, instance_specs: Sequence[str],
) -> list[InstanceMethodDecl]:
    """Close every generic `impl` method against the requested instances.

    This is the rust counterpart of what an explicit instantiation does for
    the C++ adapter: the *consumer's* build names the instances it needs
    (`--export-instance Pair2<f64, i32>`), and the Adapter closes each open
    `impl<A, B> Pair2<A, B>` method against them.  Nothing is hand-written:
    the signatures come from the provider source, the arguments come from the
    instance request, and the substitution is the same token-level rewrite
    rustc itself performs when monomorphizing.  Type arguments may be any
    stable type -- primitives, or records declared in the crate -- because
    stability and layout are both adjudicated by rustc (`is_stable` + the
    layout probes), never by a hardcoded type table.
    """
    by_synthetic = {
        synthetic: (base, list(args))
        for (base, args), synthetic in types.instances.items()
    }
    out: list[InstanceMethodDecl] = []
    for spec in instance_specs:
        value = types.parse(lex_rust(spec), ())
        if value.kind != "record":
            continue  # enums carry no impl methods in this profile
        synthetic = value.decl_path
        resolved = by_synthetic.get(synthetic)
        if resolved is None:
            continue
        base_path, arg_spellings = resolved
        instance_record = crate.records.get(synthetic)
        base_record = crate.records.get(base_path)
        if instance_record is None or base_record is None:
            continue
        # Consumer-facing plain spellings: the Active request surface and the
        # stub backend resolve type/owner names against the *crate-relative*
        # paths (`Pair2`, `Vec2`), not the DCI `crate.Name` namespace.
        prefix = types.crate_name + "."
        dci_owner = types.instance_names.get(synthetic) or types.dci_name(base_path)
        owner_name = dci_owner[len(prefix):] if dci_owner.startswith(prefix) else dci_owner
        base_name = "::".join(base_path)
        substitution = {
            name: lex_rust(spelling)
            for name, spelling in zip(base_record.type_params, arg_spellings)
        }
        for method in crate.generic_methods:
            if method.owner != base_path or not method.method.public:
                continue
            if method.method.generic:
                raise RustAdapterError(
                    f"instance method {base_name}::<{spec}>::{method.name} has its "
                    "own type parameters; closing it needs argument inference, "
                    "which an instance request cannot express")
            member = method.name
            params = [
                ParamDecl(param.name,
                          substitute_type_tokens(param.type_tokens, substitution))
                for param in method.method.params
            ]
            return_tokens = substitute_type_tokens(
                method.method.return_tokens, substitution)
            # Materialize every instance the closed signature references
            # *now*: the ABI probes compile after this function returns, so
            # an instance that only appears as a method's parameter or return
            # type (`swapped` on `Pair2<f64, f32>` returns `Pair2<f32, f64>`)
            # must be registered before the stable-record sweep or it never
            # gets measured and `value_lowering` dies on a missing probe.
            for parameter in params:
                types.parse(parameter.type_tokens, base_path)
            types.parse(return_tokens, base_path)
            semantic_id = (
                f"dci.active.rust.{base_name}::{member}"
                f"({','.join(instance_record.instance_args)})"
            )
            out.append(InstanceMethodDecl(
                owner_name=owner_name,
                base_name=base_name,
                member=member,
                arg_dci_names=list(instance_record.instance_args),
                params=params,
                return_tokens=return_tokens,
                semantic_id=semantic_id,
                link_name=_instance_method_link_name(
                    base_name, instance_record.instance_args, member),
            ))
    return out


def build_contract(root: Path, crate: ParsedCrate, crate_name: str, rustc: str,
                   identity: dict[str, str], target: TargetInfo, edition: str,
                   rustc_args: Sequence[str], artifacts: Sequence[str],
                   deny_rejected: bool = False,
                   instance_specs: Sequence[str] = (),
                   export_active_requests: bool = False,
                   item_paths: Sequence[str] = (),
                   opaque_types: Sequence[str] = (),
                   emit_views: bool = False) -> dict[str, Any]:
    if item_paths:
        selected = {tuple(item.removeprefix("crate::").split("::")) for item in item_paths}
        available = {function.path for function in crate.functions if function.public}
        missing = selected - available
        if missing:
            raise RustAdapterError("requested public Rust items were not found: " +
                                   ", ".join("::".join(item) for item in sorted(missing)))
        crate.functions = [function for function in crate.functions if function.path in selected]
    types = TypeSystem(crate, target, crate_name)

    # Evidence-based lifecycle contracts are resolved before stability analysis
    # so a Drop record with a declared contract becomes exportable, while an
    # unannotated Drop record stays rejected.
    lifecycle_annotations = discover_lifecycle_annotations(crate.files)
    exported_link_names = {
        function.export_name
        for function in crate.functions
        if function.public and function.has_stable_link_name
        and function.abi.lower() in RUST_STABLE_ABIS
    }
    lifecycle_contracts: dict[tuple[str, ...], dict[str, Any]] = {}
    for selector in opaque_types:
        path = types.resolve_path(selector, ())
        record = crate.records.get(path)
        if record is None or not record.public or record.generic:
            raise RustAdapterError(f"opaque type {selector!r} must name a closed public producer record")
        record.opaque = True
        record.fields = []
        link = "__vyx_dci_drop_" + "_".join(path)
        rust_path = record.rust_path
        crate.generated_source += (
            f'\n#[unsafe(no_mangle)] pub unsafe extern "C" fn {link}(value: *mut {rust_path}) '
            f'{{ unsafe {{ core::ptr::drop_in_place(value); }} }}\n')
        crate.functions.append(FunctionDecl(
            (link,), [ParamDecl("value", lex_rust("*mut " + "::".join(path)))], [],
            True, "C", link, True, False, False))
        lifecycle_contracts[path] = {
            "ownership_model": "unique", "copy_semantics": "forbidden",
            # A native Rust move leaves the source uninitialized. Until the
            # consumer tracks that state, expose construction and borrowing
            # and explicitly forbid relocation of this opaque owner.
            "move_semantics": "forbidden", "destruction": "operation",
            "moved_from_state": "valid", "operations": {"destroy": link},
            "allocator_domain": "rustc." + crate_name,
        }
        types.lifecycle_contract_paths.add(path)
    for selector, contract in lifecycle_annotations.items():
        resolved = types.resolve_path(selector, ())
        if resolved not in crate.records:
            raise RustAdapterError(
                f"dci-lifecycle selector {selector!r} does not resolve to a crate record"
            )
        for operation_name, symbol in contract["operations"].items():
            if symbol not in exported_link_names:
                raise RustAdapterError(
                    f"dci-lifecycle {selector!r}.operations.{operation_name} references "
                    f"{symbol!r}, which is not an exported stable C/system symbol"
                )
        lifecycle_contracts[resolved] = contract
        types.lifecycle_contract_paths.add(resolved)

    discover_instances(types, crate, instance_specs)
    # Close generic impl methods against the requested instances *before*
    # stable_records is computed: the substitution's parse calls materialize
    # the return-side instances (e.g. `Pair2<i32, f64>` for
    # `Pair2<f64, i32>::swapped`), which then get probed like any other
    # record -- measured layouts, never hand-written claims.
    instance_methods = discover_instance_methods(types, crate, instance_specs)
    reachable: set[tuple[str, ...]] | None = None
    if item_paths:
        reachable = set()
        def visit(value: RustType) -> None:
            path = types.decl_path_for(value) if value.kind in {"record", "enum"} else value.decl_path
            if path and path not in reachable:
                reachable.add(path)
                record = crate.records.get(path)
                if record:
                    for field in record.fields:
                        visit(types.parse(field.type_tokens, path[:-1]))
            for child in [value.element, value.result, *value.params]:
                if child is not None:
                    visit(child)
        for function in crate.functions:
            for parameter in function.params:
                visit(types.parse(parameter.type_tokens, function.type_context))
            visit(types.parse(function.return_tokens, function.type_context))
    stable_records: list[tuple[tuple[str, ...], list[str]]] = []
    stable_enums: list[tuple[str, ...]] = []
    for path, record in crate.records.items():
        if reachable is not None and path not in reachable:
            continue
        value = RustType("record", record.rust_path, name=types.dci_name(path))
        ok, _ = types.is_stable(value)
        if ok:
            stable_records.append((path, [field.name for field in record.fields]))
    for path, enum in crate.enums.items():
        if reachable is not None and path not in reachable:
            continue
        value = RustType("enum", enum.rust_path, name=types.dci_name(path))
        ok, _ = types.is_stable(value)
        if ok:
            stable_enums.append(path)

    view_layouts = []
    if emit_views:
        values = []
        for function in crate.functions:
            values.extend(types.parse(parameter.type_tokens, function.type_context)
                          for parameter in function.params)
            values.append(types.parse(function.return_tokens, function.type_context))
        types.view_bindings, view_layouts = measure_slice_views(
            root, crate, values, types, rustc, target, edition, rustc_args)
    facts, ir = compile_probes(root, crate, stable_records, stable_enums, rustc,
                               target, edition, rustc_args)
    identity_ir = compile_identity_ir(root, crate, crate_name, rustc, target, edition, rustc_args)
    identity_functions = llvm_functions(identity_ir)
    mangled_by_path = rustc_symbol_paths(identity_functions, crate_name)
    ir_functions = llvm_functions(ir)
    ir_functions.update(identity_functions)
    dispatch_tables = compile_trait_dispatch(
        root, crate, types, rustc, target, edition, rustc_args,
    )
    ownership_rules = discover_ownership_annotations(crate.files)
    # Generated opaque destructors consume their raw owner pointer. Feed the
    # same explicit move ownership through the normal symbol emitter so the
    # DCI binder does not mistake the pointer for a borrow.
    for contract in lifecycle_contracts.values():
        destroy = contract.get("operations", {}).get("destroy")
        if destroy:
            ownership_rules.setdefault(destroy, {"parameters": {0: "move"}})
    consumed_ownership_rules: set[str] = set()

    layouts: list[dict[str, Any]] = list(view_layouts)
    for path, fields in stable_records:
        if path not in facts:
            continue
        record = crate.records[path]
        fact = facts[path]
        emitted_fields: list[dict[str, Any]] = []
        for index, field_decl in enumerate(record.fields):
            value = types.parse(field_decl.type_tokens, path[:-1])
            emitted_fields.append({
                "name": field_decl.name,
                "offset": fact.layout.offsets[index],
                "type": types.type_json(value),
                "visibility": "public" if field_decl.public else "private",
                "is_readonly": False,
            })
        contract_key = path if path in lifecycle_contracts else record.instance_of
        declared_contract = lifecycle_contracts.get(contract_key) if contract_key else None
        if declared_contract is not None:
            lifecycle_json: dict[str, Any] = {
                "ownership_model": declared_contract["ownership_model"],
                "copy_semantics": declared_contract["copy_semantics"],
                "move_semantics": declared_contract["move_semantics"],
                "destruction": declared_contract["destruction"],
                "moved_from_state": declared_contract["moved_from_state"],
                "operations": {
                    name: {"symbol": symbol, "availability": "required", "no_unwind": True}
                    for name, symbol in declared_contract["operations"].items()
                },
            }
            if "allocator_domain" in declared_contract:
                lifecycle_json["allocator_domain"] = declared_contract["allocator_domain"]
            is_pod = declared_contract["destruction"] in {"none", "trivial"} and \
                declared_contract["copy_semantics"] == "trivial"
            is_trivially_destructible = declared_contract["destruction"] in {"none", "trivial"}
        else:
            lifecycle_json = {
                "ownership_model": "value",
                "copy_semantics": "trivial",
                "move_semantics": "trivial",
                "destruction": "trivial",
                "moved_from_state": "valid",
                "operations": {},
            }
            is_pod = True
            is_trivially_destructible = True
        layout: dict[str, Any] = {
            "type_name": types.instance_names.get(path, types.dci_name(path)),
            "size": fact.layout.size,
            "alignment": fact.layout.alignment,
            "representation": record.representation or "native",
            "has_vtable": False,
            "is_pod": is_pod,
            "is_trivially_destructible": is_trivially_destructible,
            "bases": [],
            "fields": emitted_fields,
            "lifecycle": lifecycle_json,
            "rust": {"path": record.rust_path},
        }
        if record.opaque:
            layout["rust"]["opaque"] = True
        if fact.rust_param is not None and fact.rust_returned is not None:
            layout["rust"]["param"] = lowering_json(fact.rust_param, target)
            layout["rust"]["return"] = lowering_json(fact.rust_returned, target)
        if record.explicit_align is not None:
            layout["rust"]["explicit_alignment"] = record.explicit_align
        if record.instance_of is not None:
            layout["instance"] = {
                "of": types.dci_name(record.instance_of),
                "arguments": list(record.instance_args),
                "closed": True,
            }
        layouts.append(layout)
    for path in stable_enums:
        if path not in facts:
            continue
        enum = crate.enums[path]
        fact = facts[path]
        enum_layout: dict[str, Any] = {
            "type_name": types.instance_names.get(path, types.dci_name(path)),
            "size": fact.layout.size,
            "alignment": fact.layout.alignment,
            "representation": enum.representation or ("native" if not enum.integer_repr else "stable"),
            "has_vtable": False,
            "is_pod": True,
            "is_trivially_destructible": True,
            "bases": [],
            "fields": [],
            "lifecycle": {
                "ownership_model": "value", "copy_semantics": "trivial",
                "move_semantics": "trivial", "destruction": "trivial",
                "moved_from_state": "valid", "operations": {},
            },
            "enum": {"integer_repr": enum.integer_repr or "C", "variants": enum.variants},
            "rust": {"path": enum.rust_path},
        }
        if enum.instance_of is not None:
            enum_layout["instance"] = {
                "of": types.dci_name(enum.instance_of),
                "arguments": list(enum.instance_args),
                "closed": True,
            }
        layouts.append(enum_layout)

    trait_layouts, trait_symbols, trait_tables = export_public_traits(
        crate, types, target, facts,
    )
    layouts.extend(trait_layouts)
    seen_dispatch = {
        (table.get("class_name"), table.get("base_class")) for table in dispatch_tables
    }
    for table in trait_tables:
        key = (table.get("class_name"), table.get("base_class"))
        if key not in seen_dispatch:
            dispatch_tables.append(table)
            seen_dispatch.add(key)

    symbols: list[dict[str, Any]] = []
    symbols.extend(trait_symbols)
    rejected: list[dict[str, Any]] = []
    conventions: set[str] = {"rust"} if trait_symbols else set()
    parsed_by_export: dict[str, tuple[list[RustType], RustType]] = {}
    for function in crate.functions:
        # Public items are the crate's native boundary for this rustc.  Private
        # helpers are omitted rather than rejected.  Top-level ``main`` is the
        # program entry, not a DCI export.
        if not function.public or function.path == ("main",):
            continue
        abi_lower = function.abi.lower()
        link = (
            function.export_name if function.has_stable_link_name
            else mangled_by_path.get(function.path, "")
        )
        if function.cfg_attributes and link and link not in ir_functions:
            # Selected cfg removed this declaration; it is not part of the
            # attempted native boundary for this contract.
            continue
        reason = ""
        if not link:
            reason = "rustc did not emit a concrete symbol for this pub item"
        elif abi_lower not in RUST_EXPORT_ABIS:
            reason = f"ABI {function.abi!r} is not a rustc-measured C/system/Rust boundary"
        elif function.generic:
            reason = "generic function is not a concrete exported instantiation"
        elif function.variadic:
            reason = "Rust-defined C variadic functions are not supported by the stable Adapter"

        parsed_params = [types.parse(param.type_tokens, function.type_context) for param in function.params]
        returned = types.parse(function.return_tokens, function.type_context)
        is_result = returned.kind == "result"
        if not reason:
            for index, value in enumerate(parsed_params):
                ok, detail = types.is_stable(value)
                if not ok:
                    reason = f"parameter {index} ({function.params[index].name}): {detail}"
                    break
        if not reason and is_result:
            if not rust_can_translate(parsed_params, returned, types):
                reason = "Result<T,E> Ok/Err are not stable scalar types"
        elif not reason:
            ok, detail = types.is_stable(returned)
            if not ok:
                reason = f"return type: {detail}"
        if not reason and not is_result:
            try:
                reason = verify_actual_function(
                    function, parsed_params, returned, facts, types, ir_functions, link,
                )
            except RustAdapterError as exc:
                reason = str(exc)

        selector = f"{'::'.join(function.path)}({', '.join(value.spelling for value in parsed_params)})"
        # Active-request profile: a public non-generic function without a
        # stable link name (no #[no_mangle]/extern) has no symbol the consumer
        # could rely on -- rustc's mangling embeds an instance hash.  Instead
        # of rejecting it, export it as an Active request and let the shim
        # materialize it under a stable `vyx_` link.  This is what lets a
        # provider stay pure generic code with zero export boilerplate.
        active_request = (
            export_active_requests and not function.generic
            and not function.variadic and not function.has_stable_link_name
            and returned.kind != "result"
        )
        if active_request:
            if reason and not reason.startswith("rustc did not emit"):
                # A real stability/ABI problem: keep the rejection, do not
                # paper over it by materializing a broken signature.
                rejected.append({
                    "name": "::".join(function.path), "selector": selector,
                    "link_name": function.export_name, "reason": reason,
                })
                continue
            ok, detail = types.is_stable(returned)
            if not ok and returned.kind != "result":
                rejected.append({
                    "name": "::".join(function.path), "selector": selector,
                    "link_name": function.export_name,
                    "reason": f"return type: {detail}",
                })
                continue
            link = "vyx_" + re.sub(
                r"[^A-Za-z0-9]+", "_", "::".join(function.path)
            ).strip("_").lower()
            # A receiver-carrying method (`pub fn norm1(&self) -> f64`) is not
            # a free function.  rustc still materializes it under a stable
            # `vyx_` link like any other active request, but the contract has
            # to describe it as a *method*: the receiver travels as `this`, not
            # as parameter 0.  Emitting it as a plain function left `params[0]`
            # carrying `&Vec2` (the `&` folded into the type *name*) and
            # `abi.parameters[0]` with no type at all, so the consumer died on
            #   I0100: DCI Descriptor has no matching symbol for external
            #         declaration `norm1` (kind=method, member=norm1, owner=Vec2)
            # `rewrite_receiver_params` always names the receiver `self`, and
            # `self` is not a legal Rust parameter name, so this test is exact.
            receiver = function.params[0] if function.params else None
            is_method = receiver is not None and receiver.name == "self"
            receiver_tokens = receiver.type_tokens if receiver else []
            receiver_is_ref = bool(receiver_tokens) and receiver_tokens[0].value == "&"
            receiver_is_mut = any(token.value == "mut" for token in receiver_tokens)
            active_start = 1 if is_method else 0
            active_params: list[dict[str, Any]] = []
            active_lowerings: list[dict[str, Any]] = []
            for out_index, src_index in enumerate(range(active_start, len(parsed_params))):
                value = parsed_params[src_index]
                type_entry = types.type_json(value)
                type_entry["name"] = types.active_name(value)
                active_params.append({
                    "name": function.params[src_index].name,
                    "type": type_entry,
                    "location": "abi",
                    "ownership": parameter_ownership(value),
                })
                active_lowerings.append(lowering_json(
                    value_lowering(value, types, facts, False), target, out_index,
                ))
            active_returned_json: dict[str, Any] | None = None
            if returned.kind not in {"void", "never"}:
                active_returned_json = {
                    "type": dict(types.type_json(returned),
                                 name=types.active_name(returned)),
                    "location": "abi",
                    "ownership": return_ownership(returned),
                }
            active_return_lowering = value_lowering(returned, types, facts, True)
            active_abi: dict[str, Any] = {
                "calling_convention": "system",
                "variadic": False,
                "parameters": active_lowerings,
                "return": lowering_json(active_return_lowering, target),
            }
            if is_method:
                # Same receiver shape as the stable-link path below.  Note
                # `is_const` is not cosmetic: the consumer's `fn x() const`
                # declaration is matched *by constness*, so a shared `&self`
                # has to say True and `&mut self` has to say False.
                active_abi["receiver"] = {
                    "passing": "direct",
                    "type": {
                        "name": types.dci_name(function.path[:-1]),
                        "kind": "class",
                        "reference": "pointer" if receiver_is_ref else "value",
                    },
                    "this_adjust": 0,
                    "ownership": "borrow_mut" if receiver_is_mut else "borrow",
                }
            if active_return_lowering.passing == "sret":
                active_abi["hidden_parameters"] = [
                    lowering_json(active_return_lowering, target)
                ]
            active_symbol: dict[str, Any] = {
                # A method is named `Owner::member` like the C++ producer's,
                # not by its link name -- `link_name` stays the `vyx_` symbol
                # the shim materializes, which is what the shim keys on.
                "name": "::".join(function.path) if is_method else link,
                # The arg list is the *request*'s type-argument list.  A
                # method's receiver is not one: it is named by the path
                # (`Vec2::norm1`) and travels as `this`.  Spelling it here
                # (`&Vec2`) made the request ask the producer for a record
                # literally called `&Vec2`, which the producer refused with
                #   representation_incompatible: record '&Vec2' does not exist
                #   in the producer crate
                "semantic_id": (
                    f"dci.active.rust.{'::'.join(function.path)}"
                    f"({','.join(types.active_name(v) for v in parsed_params[active_start:])})"
                ),
                "link_name": link,
                "kind": "method" if is_method else "function",
                "calling_convention": "system",
                "linkage": "external",
                "visibility": "public",
                "params": active_params,
                "return": active_returned_json,
                "abi": active_abi,
                "control_flow": {
                    "boundary": "no_unwind",
                    "default_boundary": "no_unwind",
                    "unwind": "no_unwind",
                    "propagation": {"mode": "forbidden"},
                },
            }
            if is_method:
                active_symbol["owner"] = types.dci_name(function.path[:-1])
                active_symbol["member_name"] = function.path[-1]
                active_symbol["is_static"] = False
                active_symbol["is_virtual"] = False
                active_symbol["is_const"] = not receiver_is_mut
            symbols.append(active_symbol)
            conventions.add("system")
            continue
        rule_key = selector if selector in ownership_rules else (
            function.export_name if function.export_name in ownership_rules else ""
        )
        ownership_rule = ownership_rules.get(rule_key, {})
        if rule_key:
            consumed_ownership_rules.add(rule_key)
        parameter_overrides = ownership_rule.get("parameters", {})
        for override_index in parameter_overrides:
            if override_index >= len(parsed_params):
                raise RustAdapterError(
                    f"dci-ownership {rule_key!r} parameter index {override_index} is out of range"
                )
        if reason:
            rejected.append({
                "name": "::".join(function.path), "selector": selector,
                "link_name": link or function.export_name, "reason": reason,
            })
            continue

        convention = abi_lower
        conventions.add(convention)
        parameters: list[dict[str, Any]] = []
        lowerings: list[dict[str, Any]] = []
        rust_abi = abi_lower == "rust"
        is_method = function.path[:-1] in crate.records
        has_receiver = bool(function.params) and function.params[0].name == "self"
        start = 1 if has_receiver else 0
        for out_index, src_index in enumerate(range(start, len(function.params))):
            param = function.params[src_index]
            value = parsed_params[src_index]
            parameters.append({
                "name": param.name,
                "type": types.type_json(value),
                "location": "abi",
                "ownership": parameter_overrides.get(src_index, parameter_ownership(value)),
            })
            lowerings.append(lowering_json(
                value_lowering(value, types, facts, False, rust_abi=rust_abi), target, out_index,
            ))
        returned_json: dict[str, Any] | None = None
        if returned.kind == "result":
            returned_json = {
                "type": types.type_json(returned),
                "location": "abi",
                "ownership": "copy",
            }
            return_lowering = value_lowering(
                rust_ok_type(returned), types, facts, True, rust_abi=rust_abi,
            )
        else:
            return_lowering = value_lowering(returned, types, facts, True, rust_abi=rust_abi)
            if returned.kind not in {"void", "never"}:
                returned_json = {
                    "type": types.type_json(returned),
                    "location": "abi",
                    "ownership": ownership_rule.get("return") or (
                        "owned" if returned.kind == "record" and crate.records[types.decl_path_for(returned)].opaque
                        else return_ownership(returned)),
                }
        abi_json: dict[str, Any] = {
            "calling_convention": convention,
            "variadic": False,
            "parameters": lowerings,
            "return": lowering_json(return_lowering, target),
        }
        if has_receiver:
            owner_name = types.dci_name(function.path[:-1])
            receiver = parsed_params[0]
            receiver_lowering = value_lowering(receiver, types, facts, False, rust_abi=rust_abi)
            abi_json["receiver"] = {
                "passing": receiver_lowering.passing,
                "type": {"name": owner_name, "kind": "class",
                         "reference": "pointer" if receiver.kind == "reference" else "value"},
                "this_adjust": 0,
                "ownership": "borrow_mut" if receiver.mutable else (
                    "borrow" if receiver.kind == "reference" else "move"),
            }
        if return_lowering.passing == "sret":
            abi_json["hidden_parameters"] = [lowering_json(return_lowering, target)]
        symbol_json: dict[str, Any] = {
            "name": "::".join(function.path),
            "semantic_id": selector,
            "link_name": link,
            "kind": "method" if is_method else "function",
            "calling_convention": convention,
            "linkage": "external",
            "visibility": "public",
            "params": parameters,
            "return": returned_json,
            "abi": abi_json,
            "control_flow": {
                "boundary": "no_unwind",
                "default_boundary": "no_unwind",
                "unwind": "no_unwind",
                "propagation": {"mode": "forbidden"},
            },
        }
        if is_method:
            symbol_json["owner"] = types.dci_name(function.path[:-1])
            symbol_json["member_name"] = function.path[-1]
            symbol_json["is_static"] = not has_receiver
            symbol_json["is_virtual"] = False
            # This flag is the C++-style declaration qualifier consumed by
            # existing Vyx native Rust imports.  Rust's borrow is represented
            # separately and must not silently change that declaration ABI.
            symbol_json["is_const"] = False
            if has_receiver:
                receiver = parsed_params[0]
                symbol_json["rust"] = {"receiver_kind": (
                    "mutable_reference" if receiver.mutable else "shared_reference")
                    if receiver.kind == "reference" else "value"}
        symbols.append(symbol_json)
        parsed_by_export[link] = (parsed_params, returned)

    unused_rules = sorted(set(ownership_rules) - consumed_ownership_rules)
    if unused_rules:
        raise RustAdapterError(
            "dci-ownership selectors resolved to no attempted native export: "
            + ", ".join(repr(selector) for selector in unused_rules)
        )

    if deny_rejected and rejected:
        details = "\n".join(f"  {item['selector']}: {item['reason']}" for item in rejected)
        raise RustAdapterError(f"Rust DCI boundary contains rejected exports:\n{details}")

    # Closed instance methods: exported as Active requests (semantic_id), the
    # same shape the Active Adapter's own close produces, so the consumer's
    # compiler sees one symbol shape regardless of whether the instance was
    # closed offline (Adapter export) or on demand (Active close).
    seen_instance_links: set[str] = set()
    for entry in instance_methods:
        if entry.link_name in seen_instance_links:
            raise RustAdapterError(
                f"instance method link name {entry.link_name!r} is not unique; "
                "type arguments must sanitize to distinct symbols")
        seen_instance_links.add(entry.link_name)
        parameters: list[dict[str, Any]] = []
        lowerings: list[dict[str, Any]] = []
        for out_index, param in enumerate(entry.params[1:]):
            value = types.parse(param.type_tokens, ())
            ok, reason = types.is_stable(value)
            if not ok:
                raise RustAdapterError(
                    f"instance method {entry.owner_name}::{entry.member} parameter "
                    f"{param.name!r} is not stable: {reason}")
            parameters.append({
                "name": param.name,
                "type": types.type_json(value),
                "location": "abi",
                "ownership": parameter_ownership(value),
            })
            lowerings.append(lowering_json(
                value_lowering(value, types, facts, False), target, out_index,
            ))
        returned = types.parse(entry.return_tokens, ())
        ok, reason = types.is_stable(returned)
        if not ok:
            raise RustAdapterError(
                f"instance method {entry.owner_name}::{entry.member} return "
                f"type is not stable: {reason}")
        returned_json: dict[str, Any] | None = None
        if returned.kind not in {"void", "never"}:
            returned_json = {
                "type": types.type_json(returned),
                "location": "abi",
                "ownership": return_ownership(returned),
            }
        return_lowering = value_lowering(returned, types, facts, True)
        abi_json: dict[str, Any] = {
            "calling_convention": "system",
            "variadic": False,
            "parameters": lowerings,
            "return": lowering_json(return_lowering, target),
        }
        abi_json["receiver"] = {
            "passing": "direct",
            "type": {"name": entry.owner_name, "kind": "record",
                     "reference": "pointer"},
            "this_adjust": 0,
            "ownership": "borrow",
        }
        receiver_mutates = any(
            token.value == "mut" for token in entry.params[0].type_tokens
        )
        if return_lowering.passing == "sret":
            abi_json["hidden_parameters"] = [lowering_json(return_lowering, target)]
        symbol_json = {
            "name": f"{entry.owner_name}::{entry.member}",
            "semantic_id": entry.semantic_id,
            "link_name": entry.link_name,
            "kind": "method",
            "calling_convention": "system",
            "linkage": "external",
            "visibility": "public",
            "is_static": False,
            "is_virtual": False,
            "is_override": False,
            "is_final": False,
            "is_const": not receiver_mutates,
            "this_adjust": 0,
            "owner": entry.owner_name,
            "member_name": entry.member,
            "params": parameters,
            "return": returned_json,
            "abi": abi_json,
            "control_flow": {
                "boundary": "no_unwind",
                "default_boundary": "no_unwind",
                "unwind": "no_unwind",
                "propagation": {"mode": "forbidden"},
            },
        }
        symbols.append(symbol_json)
        conventions.add("system")

    extra_symbols, extra_layouts, translate_requests, stub_source = emit_rust_translators(
        crate.functions,
        parsed_by_export,
        types,
        {symbol["link_name"] for symbol in symbols if symbol.get("link_name")},
    )
    translate_targets = {request["target"] for request in translate_requests}
    for symbol in symbols:
        if symbol.get("link_name") in translate_targets:
            symbol["control_flow"]["unwind"] = "may_unwind"
            symbol["control_flow"]["boundary_action"] = "stub_required"
    layouts.extend(extra_layouts)
    symbols.extend(extra_symbols)

    aliases: list[dict[str, Any]] = []
    for path, alias in crate.aliases.items():
        if not alias.public or alias.generic:
            continue
        value = types.parse(alias.target_tokens, path[:-1])
        ok, _ = types.is_stable(value)
        if ok:
            aliases.append({"name": types.dci_name(path), "target": value.name or value.spelling,
                            "kind": value.kind})

    source_inputs: list[str] = []
    root_parent = root.parent.resolve()
    for source_file in crate.files:
        try:
            source_inputs.append(source_file.relative_to(root_parent).as_posix())
        except ValueError:
            source_inputs.append(source_file.name)
    artifact_json: list[dict[str, Any]] = []
    for raw in artifacts:
        artifact = Path(raw).expanduser()
        suffix = artifact.suffix.lower()
        kind = "static_library" if suffix in {".a", ".lib"} else "shared_library" if suffix in {".so", ".dll", ".dylib"} else "object"
        portable_path = artifact
        if artifact.is_absolute():
            try:
                portable_path = artifact.resolve().relative_to(root.parent.resolve())
            except ValueError:
                portable_path = Path(artifact.name)
        artifact_json.append({"kind": kind, "path": portable_path.as_posix(),
                              "format": target.object_format, "link_mode": "required"})

    calling_conventions = [{
        "name": name,
        "this_pointer": None,
        "abi_family": target.abi_family,
        "callee_cleanup": False,
        "description": f"Rust extern {name!r} mapped to the selected target ABI",
    } for name in sorted(conventions)]

    stub_request_map = discover_stub_annotations(crate.files)
    stub_requests: list[dict[str, Any]] = []
    if stub_request_map:
        exported_link_names_all = {symbol["link_name"] for symbol in symbols}
        known_targets = {layout["type_name"] for layout in layouts}
        known_targets.update(exported_link_names_all)
        known_targets.update(symbol["name"] for symbol in symbols)
        known_targets.update(types.instance_names.values())
        for request_id in sorted(stub_request_map):
            request = stub_request_map[request_id]
            wrapper_name = request["wrapper"]["link_name"]
            if wrapper_name in exported_link_names_all:
                raise RustAdapterError(
                    f"dci-stub {request_id!r} wrapper {wrapper_name!r} collides with an "
                    "exported symbol"
                )
            target_ref = request.get("target")
            if target_ref is not None:
                resolved = types.resolve_path(target_ref, ())
                resolved_name = types.dci_name(resolved)
                if target_ref not in known_targets and resolved_name not in known_targets:
                    raise RustAdapterError(
                        f"dci-stub {request_id!r} target {target_ref!r} does not reference a "
                        "known exported type or symbol"
                    )
            stub_requests.append(request)

    stub_requests.extend(translate_requests)
    consumer_modes = ["direct", "stub"] if stub_requests or trait_layouts else ["direct"]

    document = {
        "$schema": "https://vyxlang.org/dci/schema/dci-1.0.schema.json",
        "dci": "1.0",
        "kind": "abi",
        "schema": {
            "name": "dci", "version": "1.0", "encoding": "dcib",
            "file_extensions": [".dci", ".abi.json", ".dcib"],
        },
        "profile": {
            "id": f"dci.rust.{target.abi_family}",
            "version": "1.0",
            "level": "L3" if dispatch_tables else ("L2" if layouts else "L1"),
            "conformance": "feature_subset",
            "consumer_modes": consumer_modes,
            "lifecycle_binding": "automatic",
            "features": [
                "rust-stable-abi", "rustc-verified-layout", "rustc-verified-lowering",
                "rustc-native-layout", "rustc-dyn-vtable", "rustc-fat-pointer",
                "rustc-mangled-symbols", "rustc-trait-object",
            ],
        },
        "source": {
            "language": "rust",
            "compiler": identity,
            "adapter": {"name": ADAPTER_NAME, "version": ADAPTER_VERSION},
            "crate_name": crate_name,
            "crate_root": str(root.resolve()),
            "edition": edition,
            "inputs": sorted(set(source_inputs)),
        },
        "target": {
            "triple": target.triple,
            "architecture": target.architecture,
            "pointer_width": target.pointer_width,
            "endianness": target.endianness,
            "object_format": target.object_format,
            "environment": target.environment,
            "abi": "system",
            "abi_family": target.abi_family,
        },
        "control_flow": {
            "boundary": "no_unwind",
            "default_boundary": "no_unwind",
            "propagation": {"mode": "forbidden"},
            "failure_channels": ["return_value", "abort"],
        },
        "artifacts": artifact_json,
        "exports": {
            "aliases": aliases,
            "layouts": layouts,
            "symbols": symbols,
            "rejected_symbols": rejected,
            "calling_conventions": calling_conventions,
            "vtables": list(dispatch_tables),
            "dispatch_tables": list(dispatch_tables),
            "runtime_type_operations": [],
            "stub_requests": stub_requests,
        },
    }
    if export_active_requests:
        _consumer_facing_names(document, crate_name)
    return document, stub_source


def _consumer_facing_names(document: dict[str, Any], crate_name: str) -> None:
    """Rewrite `crate.Name` DCI namespace spellings to crate-relative names.

    The Active-request profile produces a contract whose only consumers are
    the Vyx compiler and the stub backend, and both resolve type names
    against crate-relative paths (`Pair2<f64, i32>`, `Vec2`).  The `crate.`
    prefix is adapter-internal bookkeeping; keeping it would make the
    consumer spell the provider's crate name it has no reason to know -- and
    the compiler's layout lookup matches on the consumer's own spelling, so
    a prefixed `type_name` simply never matches (measured: `lib.Pair2<...>`
    in the contract fails a `Pair2<...>` request while the unprefixed,
    space-tolerant spelling matches).
    """
    prefix = crate_name + "."

    def strip(text: str) -> str:
        return text[len(prefix):] if text.startswith(prefix) else text

    def walk(node: Any) -> None:
        if isinstance(node, dict):
            for key, value in node.items():
                if isinstance(value, str) and key in {
                    "type_name", "name", "owner", "of", "target",
                }:
                    node[key] = strip(value)
                else:
                    walk(value)
        elif isinstance(node, list):
            for item in node:
                walk(item)

    walk(document.get("exports", {}))


def write_contract(document: dict[str, Any], output: Path,
                   debug_json: Path | None = None) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.suffix.lower() == ".dcib":
        output.write_bytes(dcib.encode(document))
    else:
        output.write_text(json.dumps(document, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    if debug_json is not None:
        debug_json.parent.mkdir(parents=True, exist_ok=True)
        debug_json.write_text(json.dumps(document, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def _rust_tokens_text(tokens: Sequence[Token]) -> str:
    """Compact a declaration token run while retaining Rust type syntax."""
    return re.sub(r"\s+", "", tokens_text(tokens))


def _native_bridge_type(type_text: str, index: int) -> tuple[str, str] | None:
    """Return (wrapper parameter, call expression) for a Rust ABI value.

    `native-lib-out` is a C-linkable artifact. Rust keeps ordinary `pub fn`
    symbols local inside a staticlib, so the artifact needs an exported bridge
    at each original mangled name. A slice or string is expanded to its two
    machine words, which is the split lowering recorded in the DCI contract.
    """
    name = f"a{index}"
    compact = type_text.replace("'static", "").replace("'a", "")
    if compact.startswith("&mut[") and compact.endswith("]"):
        elem = compact[5:-1]
        return f"{name}_data: *mut {elem}, {name}_len: usize", \
            f"core::slice::from_raw_parts_mut({name}_data, {name}_len)"
    if compact.startswith("&[") and compact.endswith("]"):
        elem = compact[2:-1]
        return f"{name}_data: *const {elem}, {name}_len: usize", \
            f"core::slice::from_raw_parts({name}_data, {name}_len)"
    if compact == "&mutstr":
        return f"{name}_data: *mut u8, {name}_len: usize", \
            f"core::str::from_utf8_unchecked_mut(core::slice::from_raw_parts_mut({name}_data, {name}_len))"
    if compact == "&str":
        return f"{name}_data: *const u8, {name}_len: usize", \
            f"core::str::from_utf8_unchecked(core::slice::from_raw_parts({name}_data, {name}_len))"
    if compact.startswith("&mut"):
        return f"{name}: *mut {compact[4:]}", f"&mut *{name}"
    if compact.startswith("&"):
        return f"{name}: *const {compact[1:]}", f"&*{name}"
    if compact.startswith("*const ") or compact.startswith("*mut "):
        return f"{name}: {compact}", name
    # Primitive values have the same machine carrier under the supported
    # target C ABI. Native Rust records/enums stay on the rustc stub path.
    if compact in PRIMITIVE_SIZES or compact in {"usize", "isize", "bool", "()"}:
        return f"{name}: {compact}", name
    return None


def native_bridge_source(crate: ParsedCrate, document: dict[str, Any]) -> str:
    """Export raw Rust link names from a native static library.

    Rust may internalize ordinary public functions when building a staticlib.
    The DCI contract still records their rustc mangled identity; an
    `export_name` bridge makes that exact identity externally linkable while
    retaining the measured Rust signature in the descriptor.
    """
    by_name = {"::".join(function.path): function for function in crate.functions}
    lines: list[str] = []
    bridge_index = 0
    for symbol in document.get("exports", {}).get("symbols", []):
        if symbol.get("kind") != "function":
            continue
        link = str(symbol.get("link_name") or "")
        original_link = str(symbol.get("rust_link_name") or link)
        name = str(symbol.get("name") or "")
        function = by_name.get(name)
        if function is None or function.path[:-1] or function.has_stable_link_name:
            continue
        if not original_link.startswith("_ZN"):
            continue
        returned = _rust_tokens_text(function.return_tokens) or "()"
        if returned.startswith("&"):
            raise RustAdapterError(
                f"native-lib-out cannot bridge borrowed return from {name!r}; "
                "use the rustc stub backend or expose an owned/C view"
            )
        params: list[str] = []
        calls: list[str] = []
        for index, parameter in enumerate(function.params):
            bridge = _native_bridge_type(_rust_tokens_text(parameter.type_tokens), index)
            if bridge is None:
                raise RustAdapterError(
                    f"native-lib-out cannot bridge Rust aggregate parameter "
                    f"{parameter.name!r} of {name!r}; use the rustc stub backend"
                )
            params.append(bridge[0])
            calls.append(bridge[1])
        call = "crate::" + "::".join(function.path) + "(" + ", ".join(calls) + ")"
        # The producer's local mangled definition has the same spelling, so a
        # bridge cannot export that exact name from the same crate. Keep the
        # measured identity in `rust_link_name` and give the native artifact a
        # distinct, stable C-linkable alias.
        bridge_link = "__vyx_dci_export_" + hashlib.sha256(original_link.encode()).hexdigest()[:24]
        symbol["rust_link_name"] = original_link
        symbol["link_name"] = bridge_link
        export_name = json.dumps(bridge_link)
        bridge_name = f"__vyx_dci_native_bridge_{bridge_index}"
        lines.append(
            f"\n#[unsafe(export_name = {export_name})]\n"
            f"pub unsafe extern \"C\" fn {bridge_name}({', '.join(params)}) -> {returned} {{\n"
            f"    unsafe {{ {call} }}\n"
            "}\n"
        )
        bridge_index += 1
    return "".join(lines)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="dci-adapter-rust",
        description="Generate a rustc-measured DCI contract for C/system and native Rust exports.",
    )
    parser.add_argument("crate_root", nargs="?", help="Rust crate root (.rs); optional with --manifest-path")
    parser.add_argument("--manifest-path", help="Cargo manifest selecting the real producer environment")
    parser.add_argument("--package", help="Cargo package name or exact package ID, including a dependency")
    parser.add_argument("--features", action="append", default=[], help="Cargo feature selection; repeat as needed")
    parser.add_argument("--no-default-features", action="store_true")
    parser.add_argument("--offline", action="store_true", help="Cargo must use the local verified registry cache")
    parser.add_argument("--locked", action="store_true", help="Cargo must preserve Cargo.lock")
    parser.add_argument("--item", action="append", default=[], help="public producer path to import; repeat as needed")
    parser.add_argument("--opaque-type", action="append", default=[], help="measure a public native record while keeping its fields private")
    parser.add_argument("--emit-views", action="store_true", help="measure explicitly named consumer views for native Rust slices and str")
    parser.add_argument("--native-lib-out", help="compile the measured producer to a static library")
    parser.add_argument("-o", "--output", required=True, help="output .dcib or diagnostic JSON")
    parser.add_argument("--debug-json-out", help="also write readable diagnostic JSON")
    parser.add_argument("--rustc", help="rustc executable; defaults to RUSTC/PATH")
    parser.add_argument("--target", "--triplet", dest="target", help="target triple; defaults to rustc host")
    parser.add_argument("--crate-name", help="DCI namespace; defaults to crate-root stem")
    parser.add_argument(
        "--edition", choices=["2015", "2018", "2021", "2024"],
        help="Rust edition; defaults to Cargo.toml or 2021",
    )
    parser.add_argument("--artifact", action="append", default=[], help="native object/library represented by the contract")
    parser.add_argument("--rustc-arg", action="append", default=[], help="extra rustc argument; repeat as needed")
    parser.add_argument("--deny-rejected", action="store_true", help="fail if any attempted public native export is unsupported")
    parser.add_argument("--export-active-requests", action="store_true",
                        help="export public non-generic functions without a stable "
                             "link name as Active requests (shim-materialized `vyx_` "
                             "symbols) instead of rejecting them")
    parser.add_argument(
        "--export-instance", action="append", default=[], metavar="TYPE<ARGS>",
        help="export a closed generic instance the host references, e.g. 'Pair<i32>'; "
             "a generic *trait* instance ('Sink<i32>') exports the trait object the "
             "host subclasses; repeat as needed",
    )
    parser.add_argument(
        "--stub-out",
        help="write crate root plus generated translate_unwind wrappers",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args, extra = parser.parse_known_args(argv)
    cargo = None
    try:
        rustc = discover_rustc(args.rustc)
        identity = rustc_identity(rustc)
        triple = args.target or identity.get("host", "")
        if not triple:
            raise RustAdapterError("cannot determine rustc host; pass --target")
        target = target_info(rustc, triple)
        if args.manifest_path:
            cargo = CargoContext(Path(args.manifest_path), package=args.package,
                                 rustc=rustc, target=triple, features=args.features,
                                 no_default_features=args.no_default_features,
                                 offline=args.offline, locked=args.locked)
            root = cargo.root
            if args.crate_root and Path(args.crate_root).resolve() != root:
                raise RustAdapterError("crate_root does not match the selected Cargo library")
        elif args.crate_root:
            root = Path(args.crate_root).expanduser().resolve()
        else:
            raise RustAdapterError("provide crate_root or --manifest-path")
        if not root.is_file():
            raise RustAdapterError(f"Rust crate root not found: {root}")
        crate_name = args.crate_name or (cargo.crate_name if cargo else root.stem.replace("-", "_"))
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", crate_name):
            raise RustAdapterError(f"invalid crate name {crate_name!r}")
        crate = cargo_declarations(cargo, rustc, triple) if cargo else RustParser(root).parse()
        rustc_args = list(args.rustc_arg) + list(extra)
        if cargo:
            rustc_args = CargoRustcArgs([*cargo.args, *rustc_args], cargo.environment)
        edition = args.edition or (cargo.edition if cargo else inferred_edition(root))
        document, stub_source = build_contract(
            root, crate, crate_name, rustc, identity, target, edition,
            rustc_args, args.artifact, args.deny_rejected, args.export_instance,
            args.export_active_requests, args.item, args.opaque_type, args.emit_views,
        )
        if cargo:
            document["source"]["cargo"] = cargo.provenance | {
                "requested_features": args.features,
                "no_default_features": args.no_default_features,
                "offline": args.offline, "locked": args.locked,
            }
        if args.item:
            document["source"]["selected_items"] = args.item
        if args.opaque_type:
            document["source"]["opaque_types"] = args.opaque_type
        if args.emit_views:
            document["source"]["emit_views"] = True
        if args.native_lib_out:
            native = Path(args.native_lib_out).resolve()
            native.parent.mkdir(parents=True, exist_ok=True)
            with tempfile.TemporaryDirectory(prefix="vyx-rust-dci-native-") as temp_text:
                temp = Path(temp_text)
                native_root = copy_crate_tree(root, temp)
                native_root.write_text(native_root.read_text(encoding="utf-8") +
                                       crate.generated_source + unmanaged_pub_keep_source(crate) +
                                       native_bridge_source(crate, document), encoding="utf-8")
                run_checked([rustc, str(native_root), "--crate-name", crate_name,
                             "--crate-type", "staticlib", "--edition", edition,
                             "--target", target.triple, "-C", "panic=abort",
                             "-o", str(native), *rustc_args],
                            cwd=native_root.parent,
                            env=probe_environment(root, temp, rustc_args), timeout=300)
        output = Path(args.output).expanduser()
        debug = Path(args.debug_json_out).expanduser() if args.debug_json_out else None
        write_contract(document, output, debug)
        if stub_source:
            stub_path = (
                Path(args.stub_out).expanduser()
                if args.stub_out
                else output.with_name(output.stem + "_translate_unwind.rs")
            )
            stub_path.write_text(root.read_text(encoding="utf-8") + stub_source, encoding="utf-8")
        print(
            f"Rust DCI adapter OK: {output} "
            f"({len(document['exports']['layouts'])} layouts, "
            f"{len(document['exports']['symbols'])} symbols, "
            f"{len(document['exports']['rejected_symbols'])} rejected)"
        )
        return 0
    except (RustAdapterError, CargoError, OSError, UnicodeError, dcib.DcibError) as exc:
        print(f"dci-rust: error: {exc}", file=sys.stderr)
        return 2
    finally:
        if cargo:
            cargo.close()


if __name__ == "__main__":
    raise SystemExit(main())
