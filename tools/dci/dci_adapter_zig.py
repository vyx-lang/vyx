#!/usr/bin/env python3
"""Zig Adapter for Declarative Code Interface contracts.

The Adapter treats the selected ``zig`` as the sole ABI authority for this
contract.  ``export fn`` / ``callconv(.c)`` / ``extern struct`` remain the C
profile subset; default ``struct`` layout and ordinary ``pub fn`` / inherent
``pub`` methods use Zig's own calling convention, measured from that same zig
rather than rewritten as C.  Rebuild the contract when zig changes.

Exported when zig can prove the facts:

* ``extern struct`` value types (C layout) and default-repr structs whose
  fields themselves have a zig layout probe;
* packed structs whose ``@sizeOf`` / ``@offsetOf`` zig can prove;
* integer-tag enums;
* ``export fn`` / ``pub fn callconv(.c)`` (C ABI) and ordinary ``pub fn`` /
  inherent ``pub`` methods (Zig ABI).  C symbols keep their export identity;
  Zig-ABI symbols use the LLVM name zig actually emitted for this crate;
* primitives and pointers.

Layout and aggregate lowering are not guessed.  A temporary copy of the crate
is instrumented with layout/ABI probes and compiled to LLVM IR by the same
zig and for the same target as the requested contract.  Function-signature
facts for Zig calling convention are read from unoptimized LLVM IR so IPO
cannot rewrite ``internal`` functions.  Unsupported declarations are emitted
as rejected symbols; they never leak into the executable ABI contract.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass, field
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
from typing import Any, Sequence
import uuid


TOOL_DIR = Path(__file__).resolve().parent
if str(TOOL_DIR) not in sys.path:
    sys.path.insert(0, str(TOOL_DIR))

import cpp_toolchains  # noqa: E402
import dcib  # noqa: E402


ADAPTER_NAME = "dci-adapter-zig"
ADAPTER_VERSION = "1.0.0"
C_CALLCONVS = {"c", "C", "cdecl"}
STDLIB_IMPORTS = {"std", "builtin", "root"}
PRIMITIVE_SIZES = {
    "i8": 1, "i16": 2, "i32": 4, "i64": 8, "i128": 16,
    "u8": 1, "u16": 2, "u32": 4, "u64": 8, "u128": 16,
    "bool": 1, "f16": 2, "f32": 4, "f64": 8, "f128": 16,
    "c_char": 1, "c_short": 2, "c_ushort": 2, "c_int": 4, "c_uint": 4,
    "c_longlong": 8, "c_ulonglong": 8,
}
POINTER_PRIMITIVES = {"usize", "isize", "c_size_t", "c_ssize_t", "c_long", "c_ulong"}
IGNORED_PREFIXES = {
    "pub", "export", "extern", "packed", "inline", "noinline", "comptime",
    "async", "threadlocal",
}


class ZigAdapterError(RuntimeError):
    pass


@dataclass
class Token:
    kind: str
    value: str


@dataclass
class FieldDecl:
    name: str
    type_tokens: list[Token]
    public: bool = True


@dataclass
class ParamDecl:
    name: str
    type_tokens: list[Token]
    comptime: bool = False


@dataclass
class RecordDecl:
    path: tuple[str, ...]
    local_name: str
    file: Path
    representation: str
    fields: list[FieldDecl] = field(default_factory=list)
    c_abi: bool = False


@dataclass
class EnumDecl:
    path: tuple[str, ...]
    local_name: str
    file: Path
    integer_repr: str
    variants: list[dict[str, Any]] = field(default_factory=list)


@dataclass
class FunctionDecl:
    path: tuple[str, ...]
    file: Path
    file_prefix: tuple[str, ...]
    params: list[ParamDecl]
    return_tokens: list[Token]
    abi: str
    exported: bool
    export_name: str
    public: bool
    generic: bool = False
    error_union: bool = False
    variadic: bool = False


@dataclass
class ParsedModule:
    records: dict[tuple[str, ...], RecordDecl] = field(default_factory=dict)
    enums: dict[tuple[str, ...], EnumDecl] = field(default_factory=dict)
    functions: list[FunctionDecl] = field(default_factory=list)
    files: list[Path] = field(default_factory=list)


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
class ZigType:
    kind: str
    spelling: str
    name: str = ""
    decl_path: tuple[str, ...] | None = None
    element: ZigType | None = None
    length: int | None = None
    nullable: bool = False
    mutable: bool = True
    abi: str = ""


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
    param: ProbeLowering | None = None
    returned: ProbeLowering | None = None
    zig_param: ProbeLowering | None = None
    zig_returned: ProbeLowering | None = None


def tokens_text(tokens: Sequence[Token]) -> str:
    parts: list[str] = []
    for token in tokens:
        if token.kind == "string":
            parts.append('"' + token.value + '"')
        else:
            parts.append(token.value)
    return "".join(parts)


def lex_zig(source: str) -> list[Token]:
    tokens: list[Token] = []
    index = 0
    length = len(source)
    while index < length:
        ch = source[index]
        if ch in " \t\r\n":
            index += 1
            continue
        if ch == "/" and index + 1 < length and source[index + 1] == "/":
            index = source.find("\n", index)
            if index < 0:
                break
            continue
        if ch == "/" and index + 1 < length and source[index + 1] == "*":
            end = source.find("*/", index + 2)
            if end < 0:
                raise ZigAdapterError("unterminated block comment")
            index = end + 2
            continue
        if ch == '"':
            index += 1
            value: list[str] = []
            while index < length:
                cur = source[index]
                if cur == "\\":
                    if index + 1 < length:
                        value.append(source[index:index + 2])
                        index += 2
                        continue
                if cur == '"':
                    index += 1
                    break
                value.append(cur)
                index += 1
            tokens.append(Token("string", "".join(value)))
            continue
        if ch == "'" and index + 1 < length:
            end = index + 1
            if source[end] == "\\":
                end += 2
            else:
                end += 1
            if end < length and source[end] == "'":
                tokens.append(Token("char", source[index:end + 1]))
                index = end + 1
                continue
        if ch == "@" and index + 1 < length and (source[index + 1].isalpha() or source[index + 1] == "_"):
            start = index
            index += 2
            while index < length and (source[index].isalnum() or source[index] == "_"):
                index += 1
            tokens.append(Token("ident", source[start:index]))
            continue
        if ch.isalpha() or ch == "_":
            start = index
            index += 1
            while index < length and (source[index].isalnum() or source[index] == "_"):
                index += 1
            tokens.append(Token("ident", source[start:index]))
            continue
        if ch.isdigit():
            start = index
            index += 1
            while index < length and (source[index].isalnum() or source[index] in "._"):
                index += 1
            tokens.append(Token("number", source[start:index]))
            continue
        tokens.append(Token("punct", ch))
        index += 1
    return tokens


class TokenStream:
    def __init__(self, tokens: Sequence[Token]) -> None:
        self.tokens = list(tokens)
        self.pos = 0

    def eof(self) -> bool:
        return self.pos >= len(self.tokens)

    def peek(self, offset: int = 0) -> Token | None:
        index = self.pos + offset
        if index >= len(self.tokens):
            return None
        return self.tokens[index]

    def peek_value(self, offset: int = 0) -> str:
        token = self.peek(offset)
        return "" if token is None else token.value

    def peek_kind(self, offset: int = 0) -> str:
        token = self.peek(offset)
        return "" if token is None else token.kind

    def advance(self) -> Token:
        token = self.tokens[self.pos]
        self.pos += 1
        return token

    def consume(self, value: str) -> bool:
        if self.peek_value() == value:
            self.advance()
            return True
        return False

    def expect(self, value: str) -> Token:
        if self.peek_value() != value:
            raise ZigAdapterError(f"expected {value!r}, found {self.peek_value()!r}")
        return self.advance()

    def skip_balanced(self, opener: str, closer: str) -> None:
        depth = 1
        self.expect(opener)
        while not self.eof() and depth:
            value = self.peek_value()
            if value == opener:
                depth += 1
            elif value == closer:
                depth -= 1
            self.advance()

    def skip_item(self) -> None:
        if self.eof():
            return
        value = self.peek_value()
        if value == "{":
            self.skip_balanced("{", "}")
            return
        if value == "(":
            self.skip_balanced("(", ")")
            return
        if value == "[":
            self.skip_balanced("[", "]")
            return
        self.advance()
        if self.consume(";"):
            return


def collect_until(stream: TokenStream, stops: set[str], *, keep_depth: bool = True) -> list[Token]:
    collected: list[Token] = []
    depth_curly = 0
    depth_paren = 0
    depth_brack = 0
    while not stream.eof():
        value = stream.peek_value()
        if keep_depth and depth_curly == 0 and depth_paren == 0 and depth_brack == 0 and value in stops:
            break
        if value == "{":
            depth_curly += 1
        elif value == "}":
            depth_curly = max(0, depth_curly - 1)
        elif value == "(":
            depth_paren += 1
        elif value == ")":
            depth_paren = max(0, depth_paren - 1)
        elif value == "[":
            depth_brack += 1
        elif value == "]":
            depth_brack = max(0, depth_brack - 1)
        collected.append(stream.advance())
    return collected


class ZigParser:
    def __init__(self, root: Path) -> None:
        self.root = root.resolve()
        self.module = ParsedModule()

    def parse(self) -> ParsedModule:
        self._parse_file(self.root, ())
        return self.module

    def _parse_file(self, path: Path, prefix: tuple[str, ...]) -> None:
        resolved = path.resolve()
        if resolved in self.module.files:
            return
        try:
            source = resolved.read_text(encoding="utf-8")
        except (OSError, UnicodeError) as exc:
            raise ZigAdapterError(f"cannot read {resolved}: {exc}") from exc
        self.module.files.append(resolved)
        stream = TokenStream(lex_zig(source))
        self._parse_decls(stream, resolved, prefix, file_prefix=prefix)

    def _parse_decls(
        self, stream: TokenStream, file: Path, prefix: tuple[str, ...],
        *, file_prefix: tuple[str, ...],
    ) -> None:
        while not stream.eof():
            if stream.peek_value() in {";", ","}:
                stream.advance()
                continue
            if stream.peek_value() == "test":
                stream.advance()
                if stream.peek_kind() == "string" or stream.peek_kind() == "ident":
                    stream.advance()
                if stream.peek_value() == "{":
                    stream.skip_balanced("{", "}")
                continue
            if stream.peek_value() == "comptime" and stream.peek_value(1) == "{":
                stream.advance()
                stream.skip_balanced("{", "}")
                continue
            exported = False
            public = False
            packed = False
            extern = False
            while stream.peek_value() in IGNORED_PREFIXES:
                value = stream.advance().value
                if value == "pub":
                    public = True
                elif value == "export":
                    exported = True
                    public = True
                elif value == "packed":
                    packed = True
                elif value == "extern":
                    extern = True
                    if stream.peek_kind() == "string":
                        stream.advance()
            if stream.peek_value() == "fn":
                self._parse_fn(
                    stream, file, prefix, file_prefix=file_prefix,
                    public=public or exported, exported=exported,
                )
                continue
            if stream.peek_value() in {"const", "var"}:
                self._parse_const(
                    stream, file, prefix, file_prefix=file_prefix,
                    public=public, packed=packed, extern=extern,
                )
                continue
            stream.skip_item()

    def _parse_const(
        self, stream: TokenStream, file: Path, prefix: tuple[str, ...],
        *, file_prefix: tuple[str, ...], public: bool, packed: bool, extern: bool,
    ) -> None:
        stream.advance()
        if stream.peek_kind() != "ident":
            stream.skip_item()
            return
        name = stream.advance().value
        if stream.peek_value() == ":":
            stream.advance()
            collect_until(stream, {"=", ";", "{"})
        if not stream.consume("="):
            stream.skip_item()
            return
        path = prefix + (name,)
        if stream.peek_value() == "@import" or (
            stream.peek_value() == "@" and stream.peek_value(1) == "import"
        ):
            if stream.peek_value() == "@":
                stream.advance()
            stream.advance()
            stream.expect("(")
            if stream.peek_kind() != "string":
                stream.skip_item()
                return
            imported = stream.advance().value
            stream.consume(")")
            stream.consume(";")
            self._follow_import(file, imported, path)
            return
        if stream.peek_value() == "extern" or extern:
            if stream.peek_value() == "extern":
                stream.advance()
                if stream.peek_kind() == "string":
                    stream.advance()
            packed = packed or stream.consume("packed")
            if stream.peek_value() == "struct":
                self._parse_struct(
                    stream, file, path, name, "stable", c_abi=True, file_prefix=file_prefix,
                )
                return
            if stream.peek_value() == "union":
                stream.skip_item()
                return
        if stream.peek_value() == "packed" or packed:
            if stream.peek_value() == "packed":
                stream.advance()
            if stream.peek_value() == "struct":
                self._parse_struct(
                    stream, file, path, name, "stable", c_abi=False, file_prefix=file_prefix,
                )
                return
        if stream.peek_value() == "struct":
            self._parse_struct(
                stream, file, path, name, "native", c_abi=False, file_prefix=file_prefix,
            )
            return
        if stream.peek_value() == "enum":
            self._parse_enum(stream, file, path, name)
            return
        collect_until(stream, {";"})
        stream.consume(";")

    def _follow_import(self, current: Path, imported: str, prefix: tuple[str, ...]) -> None:
        if imported in STDLIB_IMPORTS or imported.startswith("std."):
            return
        if not imported.endswith(".zig"):
            return
        candidate = (current.parent / imported).resolve()
        if not candidate.is_file():
            raise ZigAdapterError(f"{current}: @import({imported!r}) was not found")
        self._parse_file(candidate, prefix)

    def _parse_struct(
        self, stream: TokenStream, file: Path, path: tuple[str, ...],
        local_name: str, representation: str, *, c_abi: bool,
        file_prefix: tuple[str, ...],
    ) -> None:
        stream.expect("struct")
        if stream.peek_value() == "(":
            stream.skip_balanced("(", ")")
        stream.expect("{")
        record = RecordDecl(path, local_name, file, representation, c_abi=c_abi)
        while not stream.eof() and stream.peek_value() != "}":
            if stream.peek_value() in {";", ","}:
                stream.advance()
                continue
            public = stream.consume("pub")
            if stream.peek_value() in {"comptime", "inline", "noinline"}:
                stream.advance()
            if stream.peek_value() == "fn":
                self._parse_fn(
                    stream, file, path, file_prefix=file_prefix,
                    public=public, exported=False,
                )
                continue
            if stream.peek_value() in {"const", "var"}:
                self._parse_const(
                    stream, file, path, file_prefix=file_prefix,
                    public=public, packed=False, extern=False,
                )
                continue
            if stream.peek_kind() != "ident":
                stream.skip_item()
                continue
            field_name = stream.advance().value
            if not stream.consume(":"):
                stream.skip_item()
                continue
            type_tokens = collect_until(stream, {"=", ",", "}"})
            if stream.consume("="):
                collect_until(stream, {",", "}"})
            stream.consume(",")
            record.fields.append(FieldDecl(field_name, type_tokens, public=True))
        stream.consume("}")
        stream.consume(";")
        if record.path not in self.module.records:
            self.module.records[record.path] = record

    def _parse_enum(
        self, stream: TokenStream, file: Path, path: tuple[str, ...], local_name: str,
    ) -> None:
        stream.expect("enum")
        integer_repr = ""
        if stream.consume("("):
            repr_tokens = collect_until(stream, {")"})
            integer_repr = tokens_text(repr_tokens)
            stream.expect(")")
        stream.expect("{")
        variants: list[dict[str, Any]] = []
        discriminant = 0
        while not stream.eof() and stream.peek_value() != "}":
            if stream.peek_value() in {",", ";"}:
                stream.advance()
                continue
            if stream.peek_kind() != "ident":
                stream.skip_item()
                continue
            variant_name = stream.advance().value
            value: Any = discriminant
            if stream.consume("="):
                expr = collect_until(stream, {",", "}"})
                text = tokens_text(expr).strip()
                if re.fullmatch(r"-?\d+", text):
                    value = int(text)
                    discriminant = value
            variants.append({"name": variant_name, "value": value})
            discriminant += 1
            stream.consume(",")
        stream.consume("}")
        stream.consume(";")
        self.module.enums[path] = EnumDecl(path, local_name, file, integer_repr, variants)

    def _parse_fn(
        self, stream: TokenStream, file: Path, prefix: tuple[str, ...],
        *, file_prefix: tuple[str, ...], public: bool, exported: bool,
    ) -> None:
        stream.expect("fn")
        if stream.peek_kind() not in {"ident", "string"}:
            stream.skip_item()
            return
        name = stream.advance().value
        if stream.peek_value() != "(":
            stream.skip_item()
            return
        params, generic, variadic = self._parse_params(stream)
        callconv = ""
        error_union = False
        return_tokens: list[Token] = []
        while not stream.eof() and stream.peek_value() not in {"{", ";"}:
            if stream.peek_value() == "callconv":
                stream.advance()
                if stream.peek_value() == "(":
                    stream.advance()
                    stream.consume(".")
                    callconv = stream.advance().value if not stream.eof() else ""
                    while not stream.eof() and stream.peek_value() != ")":
                        stream.advance()
                    stream.consume(")")
                continue
            if stream.peek_value() == "!":
                error_union = True
                stream.advance()
                continue
            if stream.peek_value() in {"addrspace", "align", "linksection"}:
                stream.advance()
                if stream.peek_value() == "(":
                    stream.skip_balanced("(", ")")
                continue
            return_tokens.append(stream.advance())
        if stream.peek_value() == "{":
            stream.skip_balanced("{", "}")
        else:
            stream.consume(";")
        if not public:
            return
        abi = "c" if exported or callconv in C_CALLCONVS else "zig"
        export_name = name if exported or abi == "c" else ""
        self.module.functions.append(FunctionDecl(
            prefix + (name,), file, file_prefix, params, return_tokens, abi, exported,
            export_name, public, generic=generic, error_union=error_union, variadic=variadic,
        ))

    def _parse_params(self, stream: TokenStream) -> tuple[list[ParamDecl], bool, bool]:
        stream.expect("(")
        params: list[ParamDecl] = []
        generic = False
        variadic = False
        while not stream.eof() and stream.peek_value() != ")":
            if stream.consume("..."):
                variadic = True
                break
            comptime = stream.consume("comptime")
            stream.consume("noalias")
            if stream.peek_value() == "anytype" or (
                stream.peek_kind() == "ident" and stream.peek_value(1) != ":"
                and stream.peek_value() == "anytype"
            ):
                generic = True
            if stream.peek_kind() != "ident" and stream.peek_value() != "_":
                collect_until(stream, {",", ")"})
                stream.consume(",")
                continue
            name = stream.advance().value
            if not stream.consume(":"):
                generic = True
                stream.consume(",")
                continue
            if stream.peek_value() == "anytype":
                generic = True
                stream.advance()
                params.append(ParamDecl(name, [], comptime=True))
            else:
                type_tokens = collect_until(stream, {",", ")"})
                if any(token.value == "anytype" for token in type_tokens):
                    generic = True
                params.append(ParamDecl(name, type_tokens, comptime=comptime))
            if comptime:
                generic = True
            stream.consume(",")
        stream.consume(")")
        return params, generic, variadic


class TypeSystem:
    def __init__(self, module: ParsedModule, target: TargetInfo, crate_name: str) -> None:
        self.module = module
        self.target = target
        self.crate_name = crate_name

    def dci_name(self, path: tuple[str, ...]) -> str:
        return self.crate_name + "." + ".".join(path)

    def parse(self, tokens: Sequence[Token], scope: tuple[str, ...]) -> ZigType:
        spelling = tokens_text(tokens).strip() or "void"
        index = 0
        items = list(tokens)
        value, index = self._parse_from(items, index, scope, spelling)
        return value

    def _parse_from(
        self, tokens: list[Token], index: int, scope: tuple[str, ...], spelling: str,
    ) -> tuple[ZigType, int]:
        if index >= len(tokens):
            return ZigType("void", "void", name="void"), index
        token = tokens[index]
        if token.value == "?":
            inner, index = self._parse_from(tokens, index + 1, scope, spelling)
            if inner.kind == "pointer":
                inner.nullable = True
                return inner, index
            return ZigType("unsupported", spelling, name=spelling), index
        if token.value == "*":
            index += 1
            mutable = True
            while index < len(tokens) and tokens[index].value in {"const", "volatile", "allowzero"}:
                if tokens[index].value == "const":
                    mutable = False
                index += 1
            inner, index = self._parse_from(tokens, index, scope, spelling)
            return ZigType("pointer", spelling, element=inner, mutable=mutable, name="ptr"), index
        if token.value == "[":
            index += 1
            many = False
            c_ptr = False
            length = None
            if index < len(tokens) and tokens[index].value == "*":
                many = True
                index += 1
                if index < len(tokens) and tokens[index].value == "c":
                    c_ptr = True
                    index += 1
            elif index < len(tokens) and tokens[index].kind == "number":
                raw = tokens[index].value.split("_")[0]
                try:
                    length = int(raw, 0)
                except ValueError:
                    length = None
                index += 1
            if index < len(tokens) and tokens[index].value == "]":
                index += 1
            inner, index = self._parse_from(tokens, index, scope, spelling)
            if c_ptr or many:
                return ZigType("pointer", spelling, element=inner, name="ptr"), index
            if length is None:
                return ZigType("unsupported", spelling, name=spelling), index
            return ZigType("array", spelling, element=inner, length=length, name=spelling), index
        if token.value == "void":
            return ZigType("void", "void", name="void"), index + 1
        if token.value in PRIMITIVE_SIZES or token.value in POINTER_PRIMITIVES:
            return ZigType("primitive", token.value, name=token.value), index + 1
        if token.value in {"anytype", "type", "noreturn"} or token.value.startswith("@"):
            return ZigType("unsupported", spelling, name=spelling), index + 1
        parts: list[str] = []
        while index < len(tokens) and tokens[index].kind == "ident":
            parts.append(tokens[index].value)
            index += 1
            if index < len(tokens) and tokens[index].value == ".":
                index += 1
                continue
            break
        if not parts:
            return ZigType("unsupported", spelling, name=spelling), index
        path = self._resolve_path(tuple(parts), scope)
        if path in self.module.records:
            return ZigType(
                "record", spelling, name=self.dci_name(path), decl_path=path,
            ), index
        if path in self.module.enums:
            return ZigType(
                "enum", spelling, name=self.dci_name(path), decl_path=path,
            ), index
        return ZigType("unsupported", spelling, name=spelling), index

    def _resolve_path(self, parts: tuple[str, ...], scope: tuple[str, ...]) -> tuple[str, ...]:
        if parts in self.module.records or parts in self.module.enums:
            return parts
        if scope + parts in self.module.records or scope + parts in self.module.enums:
            return scope + parts
        if len(scope) >= 1:
            parent = scope[:-1] + parts
            if parent in self.module.records or parent in self.module.enums:
                return parent
        return parts

    def size_of_scalar(self, value: ZigType) -> int | None:
        if value.kind == "void":
            return 0
        if value.kind == "pointer":
            return self.target.pointer_width // 8
        if value.kind == "primitive":
            if value.name in PRIMITIVE_SIZES:
                return PRIMITIVE_SIZES[value.name]
            if value.name in POINTER_PRIMITIVES:
                if value.name in {"c_long", "c_ulong"} and self.target.os == "windows":
                    return 4
                return self.target.pointer_width // 8
        if value.kind == "enum":
            path = value.decl_path
            if path is None:
                return None
            repr_name = self.module.enums[path].integer_repr
            if repr_name in PRIMITIVE_SIZES:
                return PRIMITIVE_SIZES[repr_name]
            if repr_name in POINTER_PRIMITIVES:
                return self.target.pointer_width // 8
        return None

    def is_c_stable(self, value: ZigType, *, seen: set[tuple[str, ...]] | None = None) -> tuple[bool, str]:
        return self._is_stable(value, require_c_layout=True, seen=seen)

    def is_zig_stable(self, value: ZigType, *, seen: set[tuple[str, ...]] | None = None) -> tuple[bool, str]:
        return self._is_stable(value, require_c_layout=False, seen=seen)

    def _is_stable(
        self, value: ZigType, *, require_c_layout: bool,
        seen: set[tuple[str, ...]] | None = None,
    ) -> tuple[bool, str]:
        if value.kind == "void":
            return True, ""
        if value.kind == "unsupported":
            return False, f"unsupported Zig ABI type {value.spelling!r}"
        if value.kind == "primitive":
            return True, ""
        if value.kind == "pointer":
            if value.element is None:
                return False, "pointer has no pointee"
            if value.element.kind in {"unsupported", "void"}:
                if value.element.kind == "void":
                    return True, ""
                return False, f"unsupported pointer pointee {value.element.spelling!r}"
            return True, ""
        if value.kind == "array":
            if value.element is None or value.length is None:
                return False, f"array {value.spelling!r} is not closed"
            return self._is_stable(value.element, require_c_layout=require_c_layout, seen=seen)
        if value.kind == "enum":
            path = value.decl_path
            if path is None or path not in self.module.enums:
                return False, f"cannot resolve enum {value.spelling!r}"
            if not self.module.enums[path].integer_repr:
                return False, f"enum {value.name} has no integer tag"
            return True, ""
        if value.kind != "record":
            return False, f"unsupported Zig ABI type {value.spelling!r}"
        path = value.decl_path
        if path is None or path not in self.module.records:
            return False, f"cannot resolve record {value.spelling!r}"
        record = self.module.records[path]
        if require_c_layout and not record.c_abi:
            return False, (
                f"record {value.name} has no C object representation; "
                "C ABI requires extern struct, a pointer, or export fn / callconv(.c)"
            )
        seen = set() if seen is None else set(seen)
        if path in seen:
            return False, f"recursive by-value type {value.name}"
        seen.add(path)
        for field_decl in record.fields:
            field_type = self.parse(field_decl.type_tokens, path[:-1] or ())
            ok, reason = self._is_stable(
                field_type, require_c_layout=require_c_layout, seen=seen,
            )
            if not ok:
                return False, f"field {value.name}.{field_decl.name}: {reason}"
        return True, ""

    def type_json(self, value: ZigType) -> dict[str, Any]:
        if value.kind == "pointer":
            inner = value.element or ZigType("void", "void", name="void")
            result = self.type_json(inner)
            result["reference"] = "pointer"
            result["nullable"] = bool(value.nullable)
            result["pointee_const"] = not value.mutable
            result["size"] = self.target.pointer_width // 8
            return result
        if value.kind == "record":
            kind = "class"
        elif value.kind == "enum":
            kind = "enum"
        elif value.kind == "array":
            kind = "primitive"
        else:
            kind = "primitive" if value.kind in {"primitive", "void"} else value.kind
            if kind not in {"primitive", "enum", "record", "class", "interface", "opaque", "function", "vector"}:
                kind = "opaque"
        result = {
            "name": value.name or value.spelling or "void",
            "kind": kind,
            "nullable": bool(value.nullable),
            "reference": "value",
        }
        if value.kind == "array" and value.element is not None:
            result["length"] = value.length
            result["element"] = self.type_json(value.element)
        return result


def run_checked(command: Sequence[str], *, cwd: Path | None = None, timeout: int = 180) -> subprocess.CompletedProcess[str]:
    try:
        result = subprocess.run(
            list(command), cwd=str(cwd) if cwd else None,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            encoding="utf-8", errors="replace", timeout=timeout, check=False,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        raise ZigAdapterError(f"cannot execute {command[0]}: {exc}") from exc
    if result.returncode != 0:
        detail = (result.stderr or result.stdout).strip()
        raise ZigAdapterError(
            f"command failed ({result.returncode}): {' '.join(command)}"
            + (f"\n{detail}" if detail else "")
        )
    return result


def discover_zig(explicit: str | None = None) -> str:
    candidate = explicit or os.environ.get("ZIG", "") or "zig"
    path = Path(candidate).expanduser()
    if path.is_file():
        return str(path.resolve())
    found = shutil.which(candidate)
    if found:
        return found
    raise ZigAdapterError(f"zig executable not found: {candidate}")


def zig_identity(zig: str) -> dict[str, str]:
    version = run_checked([zig, "version"], timeout=15).stdout.strip()
    env_text = run_checked([zig, "env"], timeout=15).stdout
    host = ""
    match = re.search(r"\.target\s*=\s*\"([^\"]+)\"", env_text)
    if match:
        host = match.group(1)
    else:
        match = re.search(r"\.target\s*=\s*([^\n,]+)", env_text)
        if match:
            host = match.group(1).strip().strip(",")
    return {
        "name": "zig",
        "version": version or "unknown",
        "host": host,
        "executable": zig,
    }


def canonical_zig_target(value: str) -> str:
    raw = value.strip()
    llvm = cpp_toolchains.canonical_target(raw) or raw
    llvm = re.sub(
        r"^((?:aarch64|armv7|arm|i686|x86_64|x86)-linux-android(?:eabi)?)\d+$",
        r"\1",
        llvm,
    )
    mapped = {
        "x86_64-pc-windows-msvc": "x86_64-windows-msvc",
        "aarch64-pc-windows-msvc": "aarch64-windows-msvc",
        "i686-pc-windows-msvc": "x86-windows-msvc",
        "x86_64-unknown-linux-gnu": "x86_64-linux-gnu",
        "aarch64-unknown-linux-gnu": "aarch64-linux-gnu",
        "i686-unknown-linux-gnu": "x86-linux-gnu",
        "x86_64-w64-windows-gnu": "x86_64-windows-gnu",
        "aarch64-linux-android": "aarch64-linux-android",
        "x86_64-linux-android": "x86_64-linux-android",
        "x86_64-apple-darwin": "x86_64-macos",
        "aarch64-apple-darwin": "aarch64-macos",
        "arm64-apple-darwin": "aarch64-macos",
    }
    if llvm in mapped:
        return mapped[llvm]
    rewritten = llvm.replace("-pc-", "-").replace("-unknown-", "-")
    rewritten = re.sub(r"^i686-", "x86-", rewritten)
    rewritten = re.sub(r"-apple-darwin.*$", "-macos", rewritten)
    return rewritten


def target_info_from_triple(zig_triple: str) -> TargetInfo:
    triple = canonical_zig_target(zig_triple)
    architecture = triple.split("-", 1)[0]
    if architecture in {"x86_64", "aarch64", "wasm64"}:
        pointer_width = 64
    elif architecture in {"x86", "arm", "wasm32"}:
        pointer_width = 32
    else:
        pointer_width = 64
    if "windows" in triple:
        os_name = "windows"
        object_format = "coff"
    elif "macos" in triple or "ios" in triple:
        os_name = "macos" if "macos" in triple else "ios"
        object_format = "macho"
    elif "android" in triple:
        os_name = "android"
        object_format = "elf"
    elif "linux" in triple:
        os_name = "linux"
        object_format = "elf"
    else:
        os_name = triple.split("-")[1] if "-" in triple else triple
        object_format = "elf"
    environment = triple.rsplit("-", 1)[-1] if triple.count("-") >= 2 else os_name
    if architecture == "x86_64" and os_name == "windows":
        abi_family = "win64"
    elif architecture == "x86_64":
        abi_family = "sysv64"
    elif architecture == "aarch64":
        abi_family = "aapcs64"
    elif architecture == "x86":
        abi_family = "x86"
    else:
        abi_family = f"{architecture}-{environment}"
    return TargetInfo(
        triple, architecture, pointer_width, "little", os_name, environment,
        object_format, abi_family,
    )


def copy_crate_tree(root: Path, destination: Path) -> Path:
    source_dir = root.parent
    target_dir = destination / "source"

    def ignore(directory: str, names: list[str]) -> set[str]:
        return {name for name in names if name in {"zig-cache", ".zig-cache", "zig-out", ".git", ".cache", "__pycache__"}}

    shutil.copytree(source_dir, target_dir, ignore=ignore)
    return target_dir / root.name


def has_optimize_arg(args: Sequence[str]) -> bool:
    return any(arg == "-O" or arg.startswith("-O") for arg in args)


def has_target_arg(args: Sequence[str]) -> bool:
    return "-target" in args


def zig_build_obj_command(
    zig: str, source: Path, ir_path: Path, bin_path: Path, target: TargetInfo,
    zig_args: Sequence[str], *, unoptimized_ir: bool = False,
) -> list[str]:
    command = [
        zig, "build-obj", str(source),
        f"-femit-bin={bin_path}",
    ]
    if unoptimized_ir:
        command.append(f"--verbose-llvm-ir={ir_path}")
    else:
        command.append(f"-femit-llvm-ir={ir_path}")
    if not has_target_arg(zig_args):
        command.extend(["-target", target.triple])
    if not has_optimize_arg(zig_args):
        command.extend(["-O", "ReleaseFast"])
    command.extend(zig_args)
    return command


def llvm_unescape_name(name: str) -> str:
    if name.startswith('"') and name.endswith('"'):
        name = name[1:-1]
    return re.sub(r"\\([0-9A-Fa-f]{2})", lambda value: chr(int(value.group(1), 16)), name)


def llvm_functions(ir: str) -> dict[str, tuple[str, str]]:
    result: dict[str, tuple[str, str]] = {}
    pattern = re.compile(
        r'^define\s+(.*?)\s+@(?:"((?:[^"\\]|\\.)*)"|([^\s(]+))\((.*)\)\s+.*\{\s*$',
        re.MULTILINE,
    )
    for match in pattern.finditer(ir):
        name = llvm_unescape_name(
            f'"{match.group(2)}"' if match.group(2) is not None else match.group(3)
        )
        result[name] = (match.group(1).strip(), match.group(4).strip())
    return result


def llvm_aliases(ir: str) -> dict[str, str]:
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
    matches = re.findall(
        r"(?:ptr|void|half|float|double|fp128|i\d+|\[[^\]]+\]|\{[^}]+\}|<[^>]+>)",
        value,
    )
    return matches[-1] if matches else value.split()[-1]


def llvm_argument_type(fragment: str) -> str:
    value = fragment.strip()
    match = re.match(
        r"(ptr|void|half|float|double|fp128|i\d+|\[[^\]]+\]|\{[^}]+\}|<[^>]+>)",
        value,
    )
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
    return {
        "name": name, "kind": "primitive", "nullable": False, "reference": "value",
        **({"size": size} if size is not None else {}),
    }


def constant_return(ir: str, function_name: str) -> int:
    escaped = re.escape(function_name)
    match = re.search(
        rf'^define\s+[^@]+@{escaped}\([^)]*\).*?\{{(.*?)^\}}',
        ir, re.MULTILINE | re.DOTALL,
    )
    if not match:
        raise ZigAdapterError(f"zig did not emit layout probe {function_name}")
    returned = re.search(r"\bret\s+i\d+\s+(-?\d+)", match.group(1))
    if not returned:
        raise ZigAdapterError(f"layout probe {function_name} was not constant-folded by zig")
    return int(returned.group(1))


def lowering_from_probe(
    return_fragment: str, argument_fragments: list[str], *,
    size: int, alignment: int, pointer_width: int, returned: bool,
) -> ProbeLowering:
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
    elif lowering.passing == "direct":
        result["size"] = lowering.size
    if lowering.attributes:
        result["attributes"] = lowering.attributes
    return result


def import_expr(root: Path, file: Path, local_name: str) -> str:
    relative = file.resolve().relative_to(root.parent.resolve()).as_posix()
    return f'@import("{relative}").{local_name}'


def unmanaged_c_items(module: ParsedModule) -> list[FunctionDecl]:
    items: list[FunctionDecl] = []
    for function in module.functions:
        if function.public and function.abi == "c" and not function.exported and not function.generic:
            items.append(function)
    return items


def unmanaged_zig_items(module: ParsedModule) -> list[FunctionDecl]:
    items: list[FunctionDecl] = []
    for function in module.functions:
        if function.public and function.abi == "zig" and not function.generic:
            items.append(function)
    return items


def function_local_expr(function: FunctionDecl) -> str:
    return ".".join(function.path[len(function.file_prefix):])


def identity_link_name(function: FunctionDecl) -> str:
    local = function_local_expr(function)
    return f"{function.file.stem}.{local}" if local else function.file.stem


def resolve_identity_link(
    function: FunctionDecl, ir_functions: dict[str, tuple[str, str]], aliases: dict[str, str],
) -> str:
    primary = identity_link_name(function)
    if lookup_ir_function(primary, ir_functions, aliases) is not None:
        return primary
    local = function_local_expr(function)
    suffix = "." + local
    for name in ir_functions:
        if name == local or name.endswith(suffix):
            return name
    return primary


def unmanaged_c_export_source(file: Path, module: ParsedModule) -> str:
    lines = [""]
    for function in unmanaged_c_items(module):
        if function.file.resolve() != file.resolve():
            continue
        local = function_local_expr(function)
        lines.append("comptime {")
        lines.append(f'    @export(&{local}, .{{ .name = "{function.export_name}" }});')
        lines.append("}")
    if len(lines) == 1:
        return ""
    return "\n".join(lines) + "\n"


def unmanaged_zig_keep_source(module: ParsedModule) -> str:
    exprs: list[str] = []
    for function in unmanaged_zig_items(module):
        exprs.append(f"@intFromPtr(&{'.'.join(function.path)})")
    if not exprs:
        return ""
    return (
        "\nexport fn __vyx_dci_keep() usize {\n"
        f"    return {' + '.join(exprs)};\n"
        "}\n"
    )


def compile_identity_ir(
    root: Path, module: ParsedModule, zig: str, target: TargetInfo, zig_args: Sequence[str],
) -> tuple[str, list[str]]:
    with tempfile.TemporaryDirectory(prefix="vyx-zig-dci-id-") as temp_text:
        temp = Path(temp_text)
        copied_root = copy_crate_tree(root, temp)
        by_original = {
            original.resolve(): copied_root.parent / original.relative_to(root.parent.resolve())
            for original in module.files
        }
        for original, copied in by_original.items():
            extra = unmanaged_c_export_source(original, module)
            if extra:
                copied.write_text(
                    copied.read_text(encoding="utf-8") + extra, encoding="utf-8", newline="\n",
                )
        keep = unmanaged_zig_keep_source(module)
        if keep:
            copied_root.write_text(
                copied_root.read_text(encoding="utf-8") + keep, encoding="utf-8", newline="\n",
            )
        ir_path = temp / "identity.ll"
        bin_path = temp / "identity.obj"
        command = zig_build_obj_command(
            zig, copied_root, ir_path, bin_path, target, zig_args, unoptimized_ir=True,
        )
        try:
            run_checked(command, cwd=copied_root.parent, timeout=180)
        except ZigAdapterError as exc:
            message = str(exc).replace(str(copied_root.parent), str(root.parent))
            message = message.replace(str(temp), "<dci-identity>")
            raise ZigAdapterError(message) from exc
        try:
            return ir_path.read_text(encoding="utf-8"), command[3:]
        except (OSError, UnicodeError) as exc:
            raise ZigAdapterError(f"cannot read zig identity LLVM output: {exc}") from exc


def probe_source(root: Path, module: ParsedModule, prefix: str) -> str:
    lines = [
        "// Automatically generated by the Vyx DCI Zig Adapter.",
    ]
    ordered: list[tuple[str, tuple[str, ...], str, list[str]]] = []
    for path, record in module.records.items():
        ordered.append(("record", path, import_expr(root, record.file, record.local_name),
                        [field.name for field in record.fields]))
    for path, enum in module.enums.items():
        ordered.append(("enum", path, import_expr(root, enum.file, enum.local_name), []))
    for index, (kind, path, expr, fields) in enumerate(ordered):
        lines.append(f"export fn {prefix}_size_{index}() usize {{ return @sizeOf({expr}); }}")
        lines.append(f"export fn {prefix}_align_{index}() usize {{ return @alignOf({expr}); }}")
        for field_index, field_name in enumerate(fields):
            lines.append(
                f'export fn {prefix}_offset_{index}_{field_index}() usize {{ '
                f'return @offsetOf({expr}, "{field_name}"); }}'
            )
        record = module.records.get(path)
        emit_c = kind == "enum" or (record is not None and record.c_abi)
        if emit_c:
            lines.append(f"export fn {prefix}_arg_{index}(value: {expr}) void {{ _ = value; }}")
            if kind == "enum":
                lines.append(
                    f"export fn {prefix}_ret_{index}() {expr} {{ return @as({expr}, @enumFromInt(0)); }}"
                )
            else:
                lines.append(f"export fn {prefix}_ret_{index}() {expr} {{ return undefined; }}")
        zig_ret = (
            f"return @as({expr}, @enumFromInt(0));" if kind == "enum"
            else "return undefined;"
        )
        lines.append(f"fn {prefix}_zarg_{index}(value: {expr}) void {{ _ = value; }}")
        lines.append(f"fn {prefix}_zret_{index}() {expr} {{ {zig_ret} }}")
        lines.append(
            f"export fn {prefix}_zkeep_{index}() usize {{ "
            f"return @intFromPtr(&{prefix}_zarg_{index}) + @intFromPtr(&{prefix}_zret_{index}); }}"
        )
    return "\n".join(lines) + "\n"


def compile_probes(
    root: Path, module: ParsedModule, zig: str, target: TargetInfo, zig_args: Sequence[str],
) -> dict[tuple[str, ...], ProbeFact]:
    prefix = "dci_z" + uuid.uuid4().hex[:10]
    ordered_paths: list[tuple[str, ...]] = list(module.records.keys()) + list(module.enums.keys())
    fields_by_path = {
        path: [field.name for field in record.fields]
        for path, record in module.records.items()
    }
    with tempfile.TemporaryDirectory(prefix="vyx-zig-dci-") as temp_text:
        temp = Path(temp_text)
        copied_root = copy_crate_tree(root, temp)
        probe_path = copied_root.parent / "vyx_dci_probe.zig"
        probe_path.write_text(probe_source(root, module, prefix), encoding="utf-8", newline="\n")
        ir_path = temp / "probe.ll"
        bin_path = temp / "probe.obj"
        command = zig_build_obj_command(
            zig, probe_path, ir_path, bin_path, target, zig_args, unoptimized_ir=True,
        )
        try:
            run_checked(command, cwd=copied_root.parent, timeout=180)
        except ZigAdapterError as exc:
            message = str(exc).replace(str(copied_root.parent), str(root.parent))
            message = message.replace(str(temp), "<dci-probe>")
            raise ZigAdapterError(message) from exc
        try:
            ir = ir_path.read_text(encoding="utf-8")
        except (OSError, UnicodeError) as exc:
            raise ZigAdapterError(f"cannot read zig LLVM probe output: {exc}") from exc
    functions = llvm_functions(ir)
    aliases = llvm_aliases(ir)
    probe_stem = "vyx_dci_probe"
    facts: dict[tuple[str, ...], ProbeFact] = {}
    for index, path in enumerate(ordered_paths):
        size_name = llvm_resolve_alias(f"{prefix}_size_{index}", aliases)
        if size_name not in functions:
            continue
        size = constant_return(ir, size_name)
        alignment = constant_return(ir, llvm_resolve_alias(f"{prefix}_align_{index}", aliases))
        if size == 0:
            continue
        offsets = [
            constant_return(
                ir, llvm_resolve_alias(f"{prefix}_offset_{index}_{field_index}", aliases),
            )
            for field_index, _ in enumerate(fields_by_path.get(path, []))
        ]
        arg_signature = functions.get(llvm_resolve_alias(f"{prefix}_arg_{index}", aliases))
        ret_signature = functions.get(llvm_resolve_alias(f"{prefix}_ret_{index}", aliases))
        param = None
        returned = None
        if arg_signature is not None and ret_signature is not None:
            param = lowering_from_probe(
                arg_signature[0], llvm_argument_parts(arg_signature[1]),
                size=size, alignment=alignment, pointer_width=target.pointer_width, returned=False,
            )
            returned = lowering_from_probe(
                ret_signature[0], llvm_argument_parts(ret_signature[1]),
                size=size, alignment=alignment, pointer_width=target.pointer_width, returned=True,
            )
        zig_arg = lookup_ir_function(
            f"{probe_stem}.{prefix}_zarg_{index}", functions, aliases,
        ) or lookup_ir_function(f"{prefix}_zarg_{index}", functions, aliases)
        zig_ret = lookup_ir_function(
            f"{probe_stem}.{prefix}_zret_{index}", functions, aliases,
        ) or lookup_ir_function(f"{prefix}_zret_{index}", functions, aliases)
        zig_param = None
        zig_returned = None
        if zig_arg is not None and zig_ret is not None:
            zig_param = lowering_from_probe(
                zig_arg[0], llvm_argument_parts(zig_arg[1]),
                size=size, alignment=alignment, pointer_width=target.pointer_width, returned=False,
            )
            zig_returned = lowering_from_probe(
                zig_ret[0], llvm_argument_parts(zig_ret[1]),
                size=size, alignment=alignment, pointer_width=target.pointer_width, returned=True,
            )
        facts[path] = ProbeFact(
            LayoutFact(size, alignment, offsets), param, returned, zig_param, zig_returned,
        )
    return facts


def lookup_ir_function(
    name: str, functions: dict[str, tuple[str, str]], aliases: dict[str, str],
) -> tuple[str, str] | None:
    resolved = llvm_resolve_alias(name, aliases)
    return functions.get(resolved) or functions.get(name)


def value_lowering(
    value: ZigType, types: TypeSystem, facts: dict[tuple[str, ...], ProbeFact], returned: bool,
    *, zig_abi: bool = False,
) -> ProbeLowering:
    if value.kind == "void":
        return ProbeLowering("ignore", 0, 1)
    scalar = types.size_of_scalar(value)
    if scalar is not None:
        alignment = min(max(scalar, 1), types.target.pointer_width // 8)
        return ProbeLowering("direct", scalar, alignment)
    if value.kind in {"record", "enum"}:
        path = value.decl_path
        if path in facts:
            fact = facts[path]
            chosen = (
                (fact.zig_returned if returned else fact.zig_param) if zig_abi
                else (fact.returned if returned else fact.param)
            )
            if chosen is not None:
                return chosen
    kind = "Zig" if zig_abi else "C"
    raise ZigAdapterError(f"no verified {kind} ABI lowering for {value.spelling!r}")


def lowering_carriers(value: ZigType, lowering: ProbeLowering, target: TargetInfo) -> list[str]:
    if lowering.passing in {"ignore"}:
        return []
    if lowering.passing in {"sret", "indirect", "byval"}:
        return ["ptr"]
    if lowering.passing == "coerce":
        return [lowering.coerce_to]
    primitive = None
    if value.kind == "primitive":
        size = PRIMITIVE_SIZES.get(value.name) or (
            target.pointer_width // 8 if value.name in POINTER_PRIMITIVES else None
        )
        if size == 1:
            primitive = "i8"
        elif size == 2:
            primitive = "i16"
        elif size == 4:
            primitive = "i32" if value.name not in {"f32"} else "float"
        elif size == 8:
            primitive = "i64" if value.name not in {"f64"} else "double"
    if value.kind == "pointer":
        primitive = "ptr"
    if primitive:
        return [primitive]
    if lowering.registers:
        return list(lowering.registers)
    return []


def verify_actual_function(
    function: FunctionDecl, parsed_params: Sequence[ZigType], returned: ZigType,
    facts: dict[tuple[str, ...], ProbeFact], types: TypeSystem,
    functions: dict[str, tuple[str, str]], aliases: dict[str, str], link_name: str,
    *, zig_abi: bool,
) -> str:
    signature = lookup_ir_function(link_name, functions, aliases)
    label = function.export_name or ".".join(function.path)
    if signature is None:
        return f"zig did not emit link symbol {link_name!r} for the selected target/optimize mode"
    expected_carriers: list[str] = []
    return_lower = value_lowering(returned, types, facts, True, zig_abi=zig_abi)
    if return_lower.passing == "sret":
        expected_carriers.append("ptr")
    for value in parsed_params:
        lowering = value_lowering(value, types, facts, False, zig_abi=zig_abi)
        expected_carriers.extend(lowering_carriers(value, lowering, types.target))
    actual_parts = llvm_argument_parts(signature[1])
    if len(actual_parts) != len(expected_carriers):
        return (
            f"zig ABI lowering for {label!r} has {len(actual_parts)} machine arguments; "
            f"verified per-type lowering predicts {len(expected_carriers)}"
        )
    actual_carriers = [llvm_argument_type(fragment) for fragment in actual_parts]
    if actual_carriers != expected_carriers:
        return (
            f"zig ABI carriers for {label!r} are {actual_carriers}; "
            f"verified per-type lowering predicts {expected_carriers}"
        )
    actual_sret = bool(actual_parts and "sret(" in actual_parts[0])
    if actual_sret != (return_lower.passing == "sret"):
        return f"zig return lowering for {label!r} disagrees with the verified type probe"
    if return_lower.passing not in {"sret", "ignore"}:
        expected_return = lowering_carriers(returned, return_lower, types.target)
        actual_return = llvm_base_type(signature[0])
        if expected_return and actual_return != expected_return[0]:
            return (
                f"zig return carrier for {label!r} is {actual_return}; "
                f"verified return probe predicts {expected_return[0]}"
            )
    return ""


def parameter_ownership(value: ZigType) -> str:
    if value.kind == "pointer":
        return "borrow_mut" if value.mutable else "borrow"
    return "copy"


def return_ownership(value: ZigType) -> str:
    if value.kind == "pointer":
        return "owned" if value.mutable else "borrow"
    return "copy"


def artifact_json(artifacts: Sequence[str], root: Path, target: TargetInfo) -> list[dict[str, Any]]:
    result: list[dict[str, Any]] = []
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
                portable_path = artifact.resolve().relative_to(root.parent.resolve())
            except ValueError:
                portable_path = Path(artifact.name)
        result.append({
            "kind": kind, "path": portable_path.as_posix(),
            "format": target.object_format, "link_mode": "required",
        })
    return result


def build_contract(
    root: Path, module: ParsedModule, crate_name: str, zig: str, identity: dict[str, str],
    target: TargetInfo, zig_args: Sequence[str], artifacts: Sequence[str],
    deny_rejected: bool,
) -> dict[str, Any]:
    types = TypeSystem(module, target, crate_name)
    identity_ir, compiler_flags = compile_identity_ir(root, module, zig, target, zig_args)
    identity["flags"] = compiler_flags
    facts = compile_probes(root, module, zig, target, zig_args)
    ir_functions = llvm_functions(identity_ir)
    ir_aliases = llvm_aliases(identity_ir)

    layouts: list[dict[str, Any]] = []
    for path, record in module.records.items():
        if path not in facts:
            continue
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
        zig_json: dict[str, Any] = {"path": ".".join(path)}
        if fact.zig_param is not None and fact.zig_returned is not None:
            zig_json["param"] = lowering_json(fact.zig_param, target)
            zig_json["return"] = lowering_json(fact.zig_returned, target)
        layouts.append({
            "type_name": types.dci_name(path),
            "size": fact.layout.size,
            "alignment": fact.layout.alignment,
            "representation": record.representation,
            "has_vtable": False,
            "is_pod": True,
            "is_trivially_destructible": True,
            "bases": [],
            "fields": emitted_fields,
            "lifecycle": {
                "ownership_model": "value",
                "copy_semantics": "trivial",
                "move_semantics": "trivial",
                "destruction": "trivial",
                "moved_from_state": "valid",
                "operations": {},
            },
            "zig": zig_json,
        })
    for path, enum in module.enums.items():
        if path not in facts:
            continue
        fact = facts[path]
        layouts.append({
            "type_name": types.dci_name(path),
            "size": fact.layout.size,
            "alignment": fact.layout.alignment,
            "representation": "stable" if enum.integer_repr else "native",
            "has_vtable": False,
            "is_pod": True,
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
            "enum": {"integer_repr": enum.integer_repr or "C", "variants": enum.variants},
            "zig": {"path": ".".join(path)},
        })

    symbols: list[dict[str, Any]] = []
    rejected: list[dict[str, Any]] = []
    for function in module.functions:
        if not function.public:
            continue
        selector = (
            f"{'::'.join(function.path)}("
            f"{', '.join(tokens_text(param.type_tokens) for param in function.params)})"
        )
        display = "::".join(function.path)
        parsed_params = [types.parse(param.type_tokens, function.path[:-1]) for param in function.params]
        returned = types.parse(function.return_tokens, function.path[:-1]) if function.return_tokens else ZigType("void", "void", name="void")
        zig_cc = function.abi != "c"
        link = (
            resolve_identity_link(function, ir_functions, ir_aliases) if zig_cc
            else function.export_name
        )
        reason = ""
        if function.generic:
            reason = "generic function is not a concrete exported instantiation"
        elif function.variadic:
            reason = "Zig C variadic functions are not supported by the Adapter"
        elif function.error_union:
            reason = "Zig error unions are not a measured Adapter boundary"
        elif not zig_cc and not link:
            reason = "C ABI function has no export name"
        if not reason:
            stable = types.is_zig_stable if zig_cc else types.is_c_stable
            for index, value in enumerate(parsed_params):
                ok, detail = stable(value)
                if not ok:
                    reason = f"parameter {index} ({function.params[index].name}): {detail}"
                    break
        if not reason:
            ok, detail = (types.is_zig_stable if zig_cc else types.is_c_stable)(returned)
            if not ok:
                reason = f"return type: {detail}"
        if not reason:
            try:
                reason = verify_actual_function(
                    function, parsed_params, returned, facts, types,
                    ir_functions, ir_aliases, link, zig_abi=zig_cc,
                )
            except ZigAdapterError as exc:
                reason = str(exc)
        if reason:
            rejected.append({
                "name": display, "selector": selector,
                "link_name": link or function.path[-1], "reason": reason,
            })
            continue
        convention = "zig" if zig_cc else "c"
        is_method = function.path[:-1] in module.records
        start = 1 if is_method else 0
        parameters: list[dict[str, Any]] = []
        lowerings: list[dict[str, Any]] = []
        for out_index, src_index in enumerate(range(start, len(function.params))):
            param = function.params[src_index]
            value = parsed_params[src_index]
            parameters.append({
                "name": param.name,
                "type": types.type_json(value),
                "location": "abi",
                "ownership": parameter_ownership(value),
            })
            lowerings.append(lowering_json(
                value_lowering(value, types, facts, False, zig_abi=zig_cc), target, out_index,
            ))
        return_lowering = value_lowering(returned, types, facts, True, zig_abi=zig_cc)
        returned_json = None
        if returned.kind != "void":
            returned_json = {
                "type": types.type_json(returned),
                "location": "abi",
                "ownership": return_ownership(returned),
            }
        abi_json: dict[str, Any] = {
            "calling_convention": convention,
            "variadic": False,
            "parameters": lowerings,
            "return": lowering_json(return_lowering, target),
        }
        if is_method and parsed_params:
            receiver_value = parsed_params[0]
            receiver_lowering = value_lowering(
                receiver_value, types, facts, False, zig_abi=zig_cc,
            )
            abi_json["receiver"] = {
                "passing": receiver_lowering.passing,
                "type": types.type_json(receiver_value),
                "this_adjust": 0,
                "ownership": parameter_ownership(receiver_value),
            }
            if receiver_lowering.passing in {"indirect", "byval", "sret"}:
                abi_json["receiver"]["size"] = receiver_lowering.size
                abi_json["receiver"]["alignment"] = receiver_lowering.alignment
        if return_lowering.passing == "sret":
            abi_json["hidden_parameters"] = [lowering_json(return_lowering, target)]
        symbol_json: dict[str, Any] = {
            "name": display,
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
        symbols.append(symbol_json)

    if deny_rejected and rejected:
        details = "\n".join(f"  {item['selector']}: {item['reason']}" for item in rejected)
        raise ZigAdapterError(f"Zig DCI boundary contains rejected exports:\n{details}")

    source_inputs: list[str] = []
    root_parent = root.parent.resolve()
    for source_file in module.files:
        try:
            source_inputs.append(source_file.relative_to(root_parent).as_posix())
        except ValueError:
            source_inputs.append(source_file.name)

    return {
        "$schema": "https://vyxlang.org/dci/schema/dci-1.0.schema.json",
        "dci": "1.0",
        "kind": "abi",
        "schema": {
            "name": "dci", "version": "1.0", "encoding": "dcib",
            "file_extensions": [".dci", ".abi.json", ".dcib"],
        },
        "profile": {
            "id": f"dci.zig.{target.abi_family}",
            "version": "1.0",
            "level": "L2" if layouts else "L1",
            "conformance": "feature_subset",
            "consumer_modes": ["direct"],
            "lifecycle_binding": "automatic",
            "features": [
                "zig-c-abi", "zig-verified-layout", "zig-verified-lowering",
                "zig-native-layout", "zig-calling-convention",
            ],
        },
        "source": {
            "language": "zig",
            "compiler": identity,
            "adapter": {"name": ADAPTER_NAME, "version": ADAPTER_VERSION},
            "crate_name": crate_name,
            "crate_root": str(root.resolve()),
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
        "artifacts": artifact_json(artifacts, root, target),
        "exports": {
            "aliases": [],
            "layouts": layouts,
            "symbols": symbols,
            "rejected_symbols": rejected,
            "calling_conventions": [
                {
                    "name": "c",
                    "this_pointer": None,
                    "abi_family": target.abi_family,
                    "callee_cleanup": False,
                    "description": "Zig export fn / callconv(.c) mapped to the selected target C ABI",
                },
                {
                    "name": "zig",
                    "this_pointer": None,
                    "abi_family": target.abi_family,
                    "callee_cleanup": False,
                    "description": "Zig default calling convention measured from zig unoptimized LLVM IR",
                },
            ],
            "vtables": [],
            "dispatch_tables": [],
            "runtime_type_operations": [],
            "stub_requests": [],
        },
    }


def write_contract(document: dict[str, Any], output: Path, debug_json: Path | None = None) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.suffix.lower() == ".dcib":
        output.write_bytes(dcib.encode(document))
    else:
        output.write_text(json.dumps(document, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    if debug_json is not None:
        debug_json.parent.mkdir(parents=True, exist_ok=True)
        debug_json.write_text(
            json.dumps(document, ensure_ascii=False, indent=2) + "\n", encoding="utf-8",
        )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="dci-adapter-zig",
        description="Generate a zig-measured DCI contract for C and Zig ABI exports.",
    )
    parser.add_argument("crate_root", help="Zig root source (.zig)")
    parser.add_argument("-o", "--output", required=True, help="output .dcib or diagnostic JSON")
    parser.add_argument("--debug-json-out", help="also write readable diagnostic JSON")
    parser.add_argument("--zig", help="zig executable; defaults to ZIG/PATH")
    parser.add_argument("--target", "--triplet", dest="target", help="target triple; defaults to host")
    parser.add_argument("--crate-name", help="DCI namespace; defaults to crate-root stem")
    parser.add_argument("--artifact", action="append", default=[], help="native object/library represented by the contract")
    parser.add_argument("--zig-arg", action="append", default=[], help="extra zig argument; repeat as needed")
    parser.add_argument("--deny-rejected", action="store_true", help="fail if any attempted public export is unsupported")
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args, extra = parser.parse_known_args(argv)
    try:
        root = Path(args.crate_root).expanduser().resolve()
        if not root.is_file():
            raise ZigAdapterError(f"Zig crate root not found: {root}")
        zig = discover_zig(args.zig)
        identity = zig_identity(zig)
        triple = args.target or identity.get("host", "")
        if not triple:
            raise ZigAdapterError("cannot determine zig host; pass --target")
        target = target_info_from_triple(triple)
        crate_name = args.crate_name or root.stem.replace("-", "_")
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", crate_name):
            raise ZigAdapterError(f"invalid crate name {crate_name!r}")
        module = ZigParser(root).parse()
        zig_args = list(args.zig_arg) + list(extra)
        document = build_contract(
            root, module, crate_name, zig, identity, target, zig_args,
            args.artifact, args.deny_rejected,
        )
        output = Path(args.output).expanduser()
        debug = Path(args.debug_json_out).expanduser() if args.debug_json_out else None
        write_contract(document, output, debug)
        print(
            f"Zig DCI adapter OK: {output} "
            f"({len(document['exports']['layouts'])} layouts, "
            f"{len(document['exports']['symbols'])} symbols, "
            f"{len(document['exports']['rejected_symbols'])} rejected)"
        )
        return 0
    except (ZigAdapterError, OSError, UnicodeError, dcib.DcibError) as exc:
        print(f"dci-zig: error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
