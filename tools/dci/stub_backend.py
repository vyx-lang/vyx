#!/usr/bin/env python3
"""Shared implementation of the external DCI Stub backend protocol.

`dci_stub_backend = "external"` in a build manifest hands every open-generic
closure to a tool speaking this protocol:

  emit    --host-source <vyx> --output <stub> --compiler <exe>
          [--unit-sources <f>] [--descriptor <dcib>] [--requests <f>]
          [--host-args <s>]
  compile --source <stub> --output <obj> [--include-args <s>]
          [--compile-args <s>]

The build system splices `dci_stub_backend_tool_args` in front of the
subcommand, so everything this backend needs beyond the protocol goes there::

  dci_stub_backend_tool_args = "../../../tools/dci/stub_backend.py --provider native/lib.rs"

`--provider` names the producer source.  It is the only thing a project has to
state, and it is the point-to-point half of the arrangement: a `.dcib` is
written against one producer's artifact, so the build states which producer it
believes it is linking and this backend refuses a contract whose recorded
source disagrees.  The producer language is derived from the extension
(`.rs` -> rust, `.hpp`/`.h`/`.cpp`/... -> cpp) and can be overridden with
`--lang` when a source uses an unusual extension.

The producer compiler remains the only semantic authority.  Each closure is
adjudicated by a probe translation unit compiled with the producer's own
compiler (`resolve_batch`); the emitted stub then monomorphizes exactly the
closed requests, so a request the producer refuses (a bound that does not
hold) fails the build with the producer's diagnostic instead of reaching
codegen.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from tools.dci import dcib  # noqa: E402
from tools.dci import active_protocol as ap  # noqa: E402
from tools.dci.active_rust import PRIMITIVES, RustActiveSession  # noqa: E402

SEMANTIC_PREFIX = "dci.active."

#: Producer language by source extension.
LANGUAGE_BY_SUFFIX = {
    ".rs": "rust",
    ".hpp": "cpp", ".hh": "cpp", ".hxx": "cpp", ".h": "cpp",
    ".cpp": "cpp", ".cc": "cpp", ".cxx": "cpp",
}

#: Operation surface per producer language (the adapter validates it).
OPERATION = {"rust": "rust/call/1", "cpp": "cpp/call/1"}


def _language_for_provider(provider: Path) -> str:
    lang = LANGUAGE_BY_SUFFIX.get(provider.suffix.lower())
    if lang is None:
        raise RuntimeError(
            f"cannot tell the producer language from {provider.name!r}; "
            f"pass --lang rust|cpp")
    return lang


def _check_provider_against_source(descriptor: dict, provider: Path) -> None:
    """Refuse a contract whose recorded producer is not the one we were given.

    A `.dcib` is point-to-point: it describes one producer's artifact.  The
    descriptor records where it came from (`source.inputs` for the hand-written
    rust contracts, `source.headers` for the cpp adapter's output, which are
    absolute).  Only the file name is compared -- the two spellings disagree
    about relative/absolute and there is no single base to resolve against.
    """
    source = descriptor.get("source") or {}
    recorded: list[str] = []
    for key in ("inputs", "headers", "files"):
        value = source.get(key)
        if isinstance(value, list):
            recorded.extend(str(item) for item in value)
        elif isinstance(value, str) and value:
            recorded.append(value)
    if not recorded:
        return
    names = {re.split(r"[\\/]", item)[-1] for item in recorded}
    if provider.name not in names:
        raise RuntimeError(
            f"contract was written against {sorted(names)}, but --provider is "
            f"{provider.name!r}: a .dcib is point-to-point, so the build must "
            f"name the producer the contract describes")


# ---------------------------------------------------------------------------
# request parsing


def _parse_semantic_id(semantic_id: str) -> tuple[tuple[str, ...], list[str]]:
    """`<lang>.<path>(<arg>, ...)` after the `dci.active.` prefix.

    The language segment names the producer the *contract* was written
    against; it does not select the provider -- `--provider` does.  A build
    that asks for a cpp producer still honours the contract's symbols, because
    the contract is what fixes the closed symbols' ABI, and both producers
    implement the same entry points.
    """
    if not semantic_id.startswith(SEMANTIC_PREFIX):
        raise RuntimeError(
            f"semantic_id {semantic_id!r} does not carry the "
            f"{SEMANTIC_PREFIX!r} active-adapter prefix")
    head = semantic_id[len(SEMANTIC_PREFIX):]
    if "." not in head:
        raise RuntimeError(f"malformed semantic_id: {semantic_id!r}")
    _, _, head = head.partition(".")
    path_text, _, arg_text = head.partition("(")
    if not arg_text.endswith(")"):
        raise RuntimeError(f"malformed semantic_id: {semantic_id!r}")
    args = [item.strip() for item in arg_text[:-1].split(",") if item.strip()]
    path = tuple(part for part in path_text.split("::") if part)
    return path, args


def _parse_request_line(line: str) -> tuple[str, tuple[str, ...], list[str]]:
    parts = line.split("\t")
    if len(parts) != 5 or parts[0] != "open":
        raise RuntimeError(f"malformed open-generic request line: {line!r}")
    _, link, base, _ret_text, arg_text = parts
    args = [item.strip() for item in arg_text.split(",") if item.strip()]
    path = tuple(part for part in base.split("::") if part)
    return link, path, args


# ---------------------------------------------------------------------------
# consumer-declared records
#
# An open-generic instance closed over a record only the *consumer* declares has
# no descriptor entry: the producer's artifact never mentioned that type, so no
# adapter ever measured it.  The consumer compiler still has to know the layout
# before it can emit IR, so it derives one -- from what both sides can see:
# primitive widths, descriptor-measured records, and the C rule for `@[repr(C)]`
# records -- and states it in the requests sidecar next to the closures:
#
#   record  <name> <size> <align> <field:type:offset,...>
#   layout  <name> <size> <align>
#
# Both are handed to the producer here, and both are *claims*: the definition is
# what makes the producer able to name the instance at all (rustc cannot
# monomorphize `swap::<TestS, i32>` for a type it has never seen), while the
# compile-time assertions make it measure the derived numbers back.  A
# derivation that disagrees with the producer is therefore a build failure with
# the producer's own diagnostic, not a silently mis-laid-out boundary.


@dataclass(frozen=True)
class ConsumerRecord:
    """A record the descriptor never measured, described field by field."""

    name: str
    size: int
    align: int
    fields: tuple[tuple[str, str, int], ...]


@dataclass(frozen=True)
class DerivedLayout:
    """A layout the consumer derived for an instance the descriptor lacks."""

    name: str
    size: int
    align: int


def _parse_record_fields(csv: str) -> tuple[tuple[str, str, int], ...]:
    fields: list[tuple[str, str, int]] = []
    for item in csv.split(","):
        field, _, rest = item.partition(":")
        type_text, _, offset = rest.rpartition(":")
        if not field or not type_text or not offset.isdigit():
            raise ValueError(f"malformed record field {item!r}")
        fields.append((field, type_text, int(offset)))
    if not fields:
        raise ValueError("record line carries no fields")
    return tuple(fields)


def _parse_requests(path: Path):
    """Split the requests sidecar into closures, records and derivations."""
    opens: list[tuple[str, tuple[str, ...], list[str]]] = []
    records: list[ConsumerRecord] = []
    layouts: list[DerivedLayout] = []
    for lineno, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line:
            continue
        parts = line.split("\t")
        kind = parts[0]
        try:
            if kind == "open":
                opens.append(_parse_request_line(line))
            elif kind == "record":
                if len(parts) != 5:
                    raise ValueError("expected 5 tab-separated fields")
                _, name, size, align, csv = parts
                records.append(ConsumerRecord(name, int(size), int(align),
                                              _parse_record_fields(csv)))
            elif kind == "layout":
                if len(parts) != 4:
                    raise ValueError("expected 4 tab-separated fields")
                _, name, size, align = parts
                layouts.append(DerivedLayout(name, int(size), int(align)))
            elif kind in {"instance", "list-init"}:
                # Compiler -> closure-loop request (a closed instance whose
                # method symbol was missing during lowering).  The stub side
                # consumes those facts through the supplement descriptor's
                # semantic_id symbols instead, so the line is only noise here.
                continue
            else:
                raise ValueError(f"unknown request line kind {kind!r}")
        except ValueError as error:
            raise RuntimeError(
                f"{path}:{lineno}: malformed DCI request line ({error}): {line!r}"
            ) from error
    return opens, records, layouts


#: Field names that need a raw identifier on the Rust side.
_RUST_NON_RAW_IDENTS = frozenset({"self", "super", "crate", "Self"})

#: C++ keywords a Vyx field name could collide with.
_CPP_KEYWORDS = frozenset({
    "alignas", "alignof", "and", "asm", "auto", "bitand", "bitor", "bool",
    "break", "case", "catch", "char", "class", "compl", "concept", "const",
    "consteval", "constexpr", "constinit", "const_cast", "continue",
    "co_await", "co_return", "co_yield", "decltype", "default", "delete",
    "do", "double", "dynamic_cast", "else", "enum", "explicit", "export",
    "extern", "false", "float", "for", "friend", "goto", "if", "inline",
    "int", "long", "mutable", "namespace", "new", "noexcept", "not",
    "not_eq", "nullptr", "operator", "or", "or_eq", "private", "protected",
    "public", "register", "reinterpret_cast", "requires", "return", "short",
    "signed", "sizeof", "static", "static_assert", "static_cast", "struct",
    "switch", "template", "this", "thread_local", "throw", "true", "try",
    "typedef", "typeid", "typename", "union", "unsigned", "using", "virtual",
    "void", "volatile", "wchar_t", "while", "xor", "xor_eq",
})


def _producer_ident(name: str, language: str) -> str:
    """A field name spelled so the producer accepts it (layout untouched)."""
    if not re.fullmatch(r"[A-Za-z_]\w*", name or ""):
        raise RuntimeError(f"consumer record field name {name!r} is not an identifier")
    if language == "rust":
        # `r#type` is a valid Rust identifier for a field named `type`; the
        # name only has to be spellable, never meaningful -- field order is
        # what carries the layout.
        return "f_" + name if name in _RUST_NON_RAW_IDENTS else "r#" + name
    return "f_" + name if name in _CPP_KEYWORDS else name


def _spell_type_name(name: str, primitives: dict[str, str]) -> str:
    """`Pair2<i32,TestS>` -> `Pair2<int, TestS>`; every identifier mapped.

    The consumer's canonical type text is built out of primitives and record
    names only (that is exactly what the derived surface admits), so mapping
    identifiers and passing the separators through is complete -- no template
    parsing is involved.
    """
    # The consumer may spell a closed instance either `Pair2<i32, TestS>` or
    # `Pair2::<i32, TestS>`; only the first is a type in both producer
    # languages (`::<` is expression-position turbofish in Rust and not C++ at
    # all).  Normalise before mapping so one spelling reaches the producer.
    name = re.sub(r"::\s*<", "<", name)
    out: list[str] = []
    index = 0
    for match in re.finditer(r"[A-Za-z_]\w*", name):
        out.append(name[index:match.start()])
        token = match.group(0)
        out.append(primitives.get(token, token))
        index = match.end()
    out.append(name[index:])
    return "".join(out)


def _primitive_spellings(language: str) -> dict[str, str]:
    if language == "rust":
        # The rust request surface passes DCI primitive names through verbatim
        # (see RustActiveSession._arg_spelling), so the spelling is the name.
        return {name: name for name in PRIMITIVES}
    from tools.dci.active_cpp import PRIMITIVE_SPELLINGS
    return dict(PRIMITIVE_SPELLINGS)


def render_consumer_records(language: str, records: Sequence[ConsumerRecord],
                            layouts: Sequence[DerivedLayout]) -> str:
    """Producer-language source defining the consumer records and asserting them.

    Deliberately free of any module prefix: it is appended to the producer crate
    root for the closure session, and to the provider module for the emitted
    stub, and both places see `Pair2` / `TestS` unqualified.
    """
    if not records and not layouts:
        return ""
    primitives = _primitive_spellings(language)
    lines = [
        "// Consumer-declared records, generated by the DCI Active Adapter stub",
        "// backend, plus the layouts the consumer derived for them.  The",
        "// assertions are not decoration: the consumer derived these numbers",
        "// before the producer ever saw the type, so the producer is asked to",
        "// measure them back here.",
    ]
    if language == "cpp":
        lines.append("#include <cstddef>")
        lines.append("")
        for record in records:
            lines.append(f"struct {record.name} {{")
            for field, type_text, _offset in record.fields:
                lines.append(f"    {_spell_type_name(type_text, primitives)} "
                             f"{_producer_ident(field, language)};")
            lines.append("};")
            lines.append("")
            lines.append(f'static_assert(sizeof({record.name}) == {record.size}, '
                         f'"DCI: consumer-derived size for {record.name}");')
            lines.append(f'static_assert(alignof({record.name}) == {record.align}, '
                         f'"DCI: consumer-derived alignment for {record.name}");')
            for field, _type_text, offset in record.fields:
                lines.append(f'static_assert(offsetof({record.name}, '
                             f'{_producer_ident(field, language)}) == {offset}, '
                             f'"DCI: consumer-derived offset for '
                             f'{record.name}::{field}");')
            lines.append("")
        for layout in layouts:
            spelled = _spell_type_name(layout.name, primitives)
            lines.append(f'static_assert(sizeof({spelled}) == {layout.size}, '
                         f'"DCI: consumer-derived size for {layout.name}");')
            lines.append(f'static_assert(alignof({spelled}) == {layout.align}, '
                         f'"DCI: consumer-derived alignment for {layout.name}");')
        return "\n".join(lines) + "\n"

    for record in records:
        lines.append("#[repr(C)]")
        lines.append("#[derive(Clone, Copy)]")
        lines.append("#[allow(dead_code, non_camel_case_types)]")
        lines.append(f"pub struct {record.name} {{")
        for field, type_text, _offset in record.fields:
            lines.append(f"    pub {_producer_ident(field, language)}: "
                         f"{_spell_type_name(type_text, primitives)},")
        lines.append("}")
        lines.append(f"const _: () = assert!(::core::mem::size_of::<{record.name}>() "
                     f"== {record.size});")
        lines.append(f"const _: () = assert!(::core::mem::align_of::<{record.name}>() "
                     f"== {record.align});")
        for field, _type_text, offset in record.fields:
            lines.append(f"const _: () = assert!(::core::mem::offset_of!("
                         f"{record.name}, {_producer_ident(field, language)}) "
                         f"== {offset});")
        lines.append("")
    for layout in layouts:
        spelled = _spell_type_name(layout.name, primitives)
        lines.append(f"const _: () = assert!(::core::mem::size_of::<{spelled}>() "
                     f"== {layout.size});")
        lines.append(f"const _: () = assert!(::core::mem::align_of::<{spelled}>() "
                     f"== {layout.align});")
    return "\n".join(lines) + "\n"


def _type_ref(name: str):
    if name in PRIMITIVES:
        return ap.type_ref(ap.TYPE_PRIMITIVE, name)
    # Vyx-side type texts carry the consumer module prefix (dci_open.Vec2);
    # the provider only knows the last segment.
    short = re.split(r"[.::]", name)[-1]
    return ap.type_ref(ap.TYPE_RECORD, short)


def _query(query_id: str, lang: str, path: tuple[str, ...],
           args: list[str]) -> ap.Query:
    return ap.Query(
        query_id=query_id,
        entity={"kind": "generic", "path": list(path),
                "args": [_type_ref(name) for name in args]},
        operation={"op": OPERATION[lang]},
    )


# ---------------------------------------------------------------------------
# provider signatures: how a closed type parameter maps onto parameters


def _expand_args(type_vars: list[str], param_types: list[str],
                 generic_args: list[str]) -> list[str]:
    """Fill each parameter's type from the closed type parameters.

    The compiler's requests carry the closed *type parameters* (T=i32), while
    the request surface is per-parameter.  Type variables are recovered by
    first-occurrence order across the provider signature: `max_of<T>(a: T,
    b: T)` with args [i32] becomes query args [i32, i32].
    """
    bound = {name: arg for name, arg in zip(type_vars, generic_args)}
    return [bound.get(text, text) for text in param_types]


def _rust_records(session: RustActiveSession) -> set[str]:
    names: set[str] = set()
    for table in (session.parsed.records, session.parsed.enums):
        for key in table:
            names.add(key[-1])
    return names


def _rust_signature(session: RustActiveSession, path: tuple[str, ...]):
    function = next((f for f in session.parsed.functions if f.path == path),
                    None)
    if function is None:
        return None
    records = _rust_records(session)
    type_vars: list[str] = []
    for param in [p.type_tokens for p in function.params] + [function.return_tokens]:
        for token in param:
            text = token.value
            if (token.kind == "ident" and text not in PRIMITIVES
                    and text not in records and text not in type_vars):
                type_vars.append(text)
    param_types = ["".join(t.value for t in p.type_tokens).strip()
                   for p in function.params]
    return type_vars, param_types


_CPP_TEMPLATE = re.compile(
    r"template\s*<(?P<tparams>[^>]*)>\s*"
    r"(?P<ret>[A-Za-z_][\w:<>,\s\*&]*?)\s+"
    r"(?P<name>\w+)\s*\((?P<params>[^)]*)\)")

_CPP_TO_SHARED = None  # filled lazily from the adapter's spelling table


def _shared_from_cpp() -> dict[str, str]:
    global _CPP_TO_SHARED
    if _CPP_TO_SHARED is None:
        from tools.dci.active_cpp import PRIMITIVE_SPELLINGS
        _CPP_TO_SHARED = {spelling: name
                          for name, spelling in PRIMITIVE_SPELLINGS.items()}
    return _CPP_TO_SHARED


def _split_top_level(text: str, separator: str = ",") -> list[str]:
    parts, depth, current = [], 0, ""
    for char in text:
        if char in "<(":
            depth += 1
        elif char in ">)":
            depth -= 1
        if char == separator and depth == 0:
            parts.append(current)
            current = ""
        else:
            current += char
    parts.append(current)
    return parts


def _param_type(decl: str) -> str:
    """`T v` -> `T`, `double x` -> `double`, `T` -> `T`."""
    decl = decl.split("=")[0].strip()
    match = re.match(r"^(?P<type>.*?)\s*(?P<name>[A-Za-z_]\w*)$", decl)
    if match and match.group("type").strip():
        return match.group("type").strip()
    return decl


def _cpp_signatures(text: str) -> dict[str, tuple[list[str], list[str]]]:
    """name -> (template parameter names, per-parameter type texts)."""
    shared = _shared_from_cpp()
    table: dict[str, tuple[list[str], list[str]]] = {}
    for match in _CPP_TEMPLATE.finditer(text):
        name = match.group("name")
        tparams = re.findall(r"\b(?:typename|class)\s+([A-Za-z_]\w*)",
                             match.group("tparams"))
        params = [p for p in _split_top_level(match.group("params")) if p.strip()]
        texts = []
        for param in params:
            spelling = _param_type(param)
            texts.append(shared.get(spelling, spelling))
        table[name] = (tparams, texts)
    return table


def _cpp_records(text: str) -> set[str]:
    return set(re.findall(r"\b(?:struct|class)\s+([A-Za-z_]\w*)", text))


def _cpp_spelling(name: str, records: set[str]) -> str:
    from tools.dci.active_cpp import PRIMITIVE_SPELLINGS
    if name in PRIMITIVE_SPELLINGS:
        return PRIMITIVE_SPELLINGS[name]
    short = re.split(r"[.::]", name)[-1]
    return short if short in records else name


# ---------------------------------------------------------------------------
# emitters


def _qualify(spelling: str, records: set[str]) -> str:
    text = spelling.replace("crate_shim::", "provider::")
    for name in records:
        text = re.sub(rf"(?<!::)\b{name}\b", f"provider::{name}", text)
    return text


class _RustEmitter:
    lang = "rust"

    def __init__(self, provider: Path) -> None:
        if not provider.is_file():
            raise RuntimeError(f"producer source not found: {provider}")
        self.provider = provider
        #: Path the emitted stub reaches the producer through.  It is the
        #: provider itself unless consumer records had to be defined, in which
        #: case it is a generated file holding the provider *plus* them -- the
        #: records have to live in the same module as the records the consumer
        #: refers to by bare name, so `mod provider` has to be the whole thing.
        self.provider_source = provider
        #: What `#[path = ..]` has to say.  rustc resolves `#[path]` relative
        #: to the directory of the file that contains the `mod` item, and the
        #: stub is handed to rustc as a *relative* path (`.cache/...`), so an
        #: absolute-or-project-relative spelling gets re-rooted under `.cache`
        #: and doubled.  The generated file is always a sibling of the stub,
        #: so its bare name is the one spelling that is right either way.
        self.provider_mod_path = provider.as_posix()
        self.consumer_source = ""

    def prepare(self, records, layouts, stub_output: Path) -> None:
        """Generate the records source and retarget the provider module at it."""
        self.consumer_source = render_consumer_records("rust", records, layouts)
        if not self.consumer_source:
            return
        combined = stub_output.with_name(stub_output.name + "_provider.rs")
        combined.write_text(
            self.provider.read_text(encoding="utf-8") + "\n" + self.consumer_source,
            encoding="utf-8")
        self.provider_source = combined
        self.provider_mod_path = combined.name

    def session(self):
        return RustActiveSession(self.provider,
                                 work_root=Path(tempfile.mkdtemp(
                                     prefix="dci-active-backend-")),
                                 extra_source=self.consumer_source)

    def records(self, session) -> set[str]:
        return _rust_records(session)

    def open_query(self, session, query_id: str, path: tuple[str, ...],
                   generic_args: list[str]) -> ap.Query:
        signature = _rust_signature(session, path)
        if signature is None:
            return _query(query_id, self.lang, path, generic_args)
        type_vars, param_types = signature
        return _query(query_id, self.lang, path,
                      _expand_args(type_vars, param_types, generic_args))

    def header(self) -> list[str]:
        return [
            "#![allow(improper_ctypes_definitions, non_snake_case, dead_code)]",
            "// Generated by the DCI Active Adapter external stub backend.",
            "// Each #[no_mangle] symbol monomorphizes one closed open-generic",
            "// request; bounds were adjudicated by the producer compiler via",
            "// the active session probe before this source was emitted.",
            f'#[path = "{self.provider_mod_path}"]',
            "mod provider;",
            "",
        ]

    def symbol(self, link: str, path: list[str], spellings: list[str],
               records: set[str], return_type: str,
               facts: dict | None = None) -> list[str]:
        facts = facts or {}
        params = ", ".join(f"v{i}: {_qualify(spelling, records)}"
                           for i, spelling in enumerate(spellings))
        if facts.get("method"):
            # A method of an open generic: the shim calls it by UFCS on the
            # concrete receiver, so the monomorphization happens here and the
            # producer needs no `#[no_mangle]` entry point of its own.  This
            # is the Rust answer to C++'s explicit instantiation -- the
            # producer's generic definition is enough, and rustc decides the
            # layout of whatever instance the request names.
            owner = "provider::" + "::".join(path)
            turbofish = ("::<" + ", ".join(_qualify(a, records)
                                           for a in facts["owner_args"]) + ">"
                         if facts.get("owner_args") else "")
            borrow = "&" if facts.get("receiver_borrow") else ""
            call = f"{owner}{turbofish}::{facts['method']}({borrow}v0)"
        else:
            call = ("provider::" + "::".join(path) + "("
                    + ", ".join(f"v{i}" for i in range(len(spellings))) + ")")
        if return_type in ("", "()", "unit"):
            return [f"#[no_mangle]",
                    f'pub extern "system" fn {link}({params}) {{ {call}; }}',
                    ""]
        return ["#[no_mangle]",
                f'pub extern "system" fn {link}({params}) '
                f"-> {_qualify(return_type, records)} {{ {call} }}",
                ""]

    def contract_symbol(self, link: str, path: tuple[str, ...],
                        args: list[str]) -> list[str]:
        """A provider-exported contract symbol: emit nothing.

        The Rust contract is hand-written (`dci/open_generic.dci`), and a symbol
        there **without** a `semantic_id` says "the producer already exports this
        link" -- e.g. `vyx_pair2_f64_i32_swapped`, one of the `#[no_mangle]`
        entry points that pin the instantiations this artifact contains.  That is
        the Rust spelling of C++'s `template struct Pair2<double, int>;`
        (`native/lib.hpp`): neither producer can be *told* to lay out a member of
        an uninstantiated generic, so "which instantiations this artifact
        contains" is a provider statement on both sides.

        A `semantic_id`, by contrast, *is* the request -- it names a producer path
        for the Active Adapter to close, and the shim is what materializes the
        resulting link.  Routing a provider export through the session would emit
        a second definition of a link the provider already defines, which fails at
        link time as a duplicate symbol rather than at build time as a miss.
        """
        return []


class _CppEmitter:
    lang = "cpp"

    def __init__(self, provider: Path) -> None:
        if not provider.is_file():
            raise RuntimeError(f"producer source not found: {provider}")
        self.provider = provider
        self._text = self.provider.read_text(encoding="utf-8")
        self._signatures = _cpp_signatures(self._text)
        self._records = _cpp_records(self._text)
        self._force_index = 0
        self.consumer_source = ""

    def prepare(self, records, layouts, stub_output: Path) -> None:
        """Render the consumer records; the stub carries them inline."""
        self.consumer_source = render_consumer_records("cpp", records, layouts)

    def session(self):
        from tools.dci.active_cpp import CppActiveSession
        return CppActiveSession(self.provider,
                                work_root=Path(tempfile.mkdtemp(
                                    prefix="dci-active-backend-")),
                                extra_source=self.consumer_source)

    def records(self, session) -> set[str]:
        return self._records

    def open_query(self, session, query_id: str, path: tuple[str, ...],
                   generic_args: list[str]) -> ap.Query:
        signature = self._signatures.get(path[-1] if path else "")
        if signature is None:
            return _query(query_id, self.lang, path, generic_args)
        type_vars, param_types = signature
        return _query(query_id, self.lang, path,
                      _expand_args(type_vars, param_types, generic_args))

    def header(self) -> list[str]:
        lines = [
            "// Generated by the DCI Active Adapter external stub backend.",
            "// Each extern \"C\" symbol instantiates one closed open-generic",
            "// request; the template constraints were adjudicated by clang++",
            "// via the active session probe before this source was emitted.",
            f'#include "{self.provider.as_posix()}"',
        ]
        if self.consumer_source:
            # After the provider: the records the consumer declares are only
            # needed by the instantiations below, never by the provider itself.
            lines.append(self.consumer_source)
        lines.append("")
        return lines

    def symbol(self, link: str, path: list[str], spellings: list[str],
               records: set[str], return_type: str,
               facts: dict | None = None) -> list[str]:
        if facts and facts.get("method"):
            return self._method_force_source(link, path, spellings, records)
        qualified = [spelling if spelling in self._records else spelling
                     for spelling in spellings]
        params = ", ".join(f"{sp} v{i}" for i, sp in enumerate(qualified))
        arguments = ", ".join(f"v{i}" for i in range(len(qualified)))
        call = "::" + "::".join(path) + "(" + arguments + ")"
        # decltype keeps the shim's ABI exactly the template's own return type
        # without re-deriving template argument deduction here.
        return [f'extern "C" auto {link}({params}) -> decltype({call}) '
                f"{{ return {call}; }}", ""]

    def _method_force_source(self, link: str, path: list[str],
                             spellings: list[str], records: set[str]) -> list[str]:
        """Materialize a record-instance method by *using* it.

        The consumer's call site references the final MSVC-mangled member
        symbol (`?get_first@?$Pair2@NH@@QEBANXZ`), and the producer header
        never instantiated the instance -- so the shim must make clang emit
        the member's own definition.  A force-use body does exactly that:
        clang instantiates `Pair2<int, double>::get_first` with its real
        ABI (this/sret order included), spelling included in the symbol it
        derives, and the emitted COMDAT symbol satisfies the consumer's
        undefined reference.  We never restate the ABI by hand.
        """
        owner = "::" + "::".join(path[:-1])
        member = path[-1]
        args_text = ", ".join(_cpp_spelling(sp, records) for sp in spellings)
        receiver = f"{owner}<{args_text}>" if args_text else owner
        ident = "dci_active_materialize_" + re.sub(r"[^A-Za-z0-9]", "_", link)[:60]
        ident = f"{ident}_{self._force_index}"
        keep = f"dci_active_keep_{self._force_index}"
        self._force_index += 1
        # The volatile member-pointer store is what actually pins the symbol:
        # clang folds a bare `(void)&X::f` (a compile-time constant) and at
        # -O2 it inlines the call away, dropping the COMDAT definition
        # entirely -- a volatile store cannot be folded, so the member's own
        # definition is emitted no matter the optimization level.
        return [
            f'extern "C" void {ident}() {{',
            f"    {receiver} recv{{}};",
            f"    (void)(recv.{member}(), 0);",
            "}",
            f"static auto volatile {keep} = &{receiver}::{member};",
            "",
        ]

    def contract_symbol(self, link: str, path: tuple[str, ...],
                        args: list[str]) -> list[str]:
        """A closed contract symbol: the provider exports it, so emit nothing.

        The C++ contract is derived from the provider header itself
        (`dci adapter --language cpp native/lib.hpp`), so `vyx_vec2_make` and
        friends are the provider's own `extern "C"` definitions -- a shim here
        would be a redefinition.  The contract still fixes their ABI for the
        Vyx consumer, and clang++ still checks every call the shim makes.
        """
        return []


def _make_emitter(lang: str, provider: Path):
    if lang == "rust":
        return _RustEmitter(provider)
    if lang == "cpp":
        return _CppEmitter(provider)
    raise RuntimeError(f"unknown provider language {lang!r}")


# ---------------------------------------------------------------------------


def _emit(args: argparse.Namespace) -> int:
    provider = Path(args.provider).resolve()
    emitter = _make_emitter(args.lang, provider)
    # (link_name, query factory taking the open session)
    requests: list[tuple[str, object]] = []
    # Descriptor-only symbols, per language.  A `.dcib` symbol that carries a
    # `semantic_id` is a *request*: it names a producer path for the Active
    # Adapter to close, and the shim materializes the resulting link.  A symbol
    # with no `semantic_id` is a *provider export* -- the producer's own
    # definition, which the shim must not redefine.  Rust reaches that state
    # for its closed non-generic symbols (`vyx_vec2_*`), while its generic
    # instance methods still come back as `semantic_id` requests; C++ is the
    # same shape now that instance methods are exported as requests too and
    # only the non-generic `extern "C"` functions stay provider exports.
    contract: list[tuple[str, tuple[str, ...], list[str]]] = []
    native_cpp: list[dict] = []
    cpp_standard: str | None = None

    if args.descriptor:
        # The build hands every descriptor root of the unit over -- the base
        # contract first, then any closure supplements the build-internal
        # fixpoint appended -- newline-joined into one argument (the
        # compiler's own `--dci` list shape).  Decode them in order: the
        # provider check runs against the base contract only (a supplement
        # is build-derived and carries the same provenance anyway), and the
        # symbol walk must not reset its request index between roots.
        descriptor_texts = [
            text.strip() for text in str(args.descriptor).splitlines()
            if text.strip()
        ]
        if not descriptor_texts:
            raise RuntimeError("external DCI Stub backend got an empty descriptor list")
        request_index = 0
        for position, descriptor_text in enumerate(descriptor_texts):
            descriptor_path = Path(descriptor_text)
            if descriptor_path.suffix.lower() != ".dcib":
                raise RuntimeError(
                    "external DCI Stub backend requires a .dcib descriptor")
            descriptor = dcib.decode(descriptor_path.read_bytes())
            if emitter.lang == "cpp":
                standard = (descriptor.get("source") or {}).get("cxx_standard")
                if standard:
                    if cpp_standard is not None and cpp_standard != standard:
                        raise RuntimeError("C++ descriptor roots use different language standards")
                    cpp_standard = standard
            if position == 0:
                _check_provider_against_source(descriptor, provider)
            symbols = descriptor.get("exports", {}).get("symbols", [])
            for symbol in symbols:
                link = symbol["link_name"]
                if emitter.lang == "cpp" and symbol.get("cpp_materialization"):
                    if symbol.get("visibility", "public") == "public":
                        native_cpp.append(symbol)
                    continue
                if not symbol.get("semantic_id"):
                    # The provider exports this link itself; the shim must not
                    # define it a second time.
                    contract.append((link, (), []))
                    continue
                path, type_args = _parse_semantic_id(symbol.get("semantic_id", ""))
                query = _query(f"q{request_index}", emitter.lang, path, type_args)
                requests.append((link, lambda s, q=query: q))
                request_index += 1

    if args.requests:
        request_file = Path(args.requests)
        # Parsed before anything is emitted: the consumer-declared records in
        # the same file have to reach the producer *before* it is asked to
        # close an instance over them, both in the closure session and in the
        # emitted stub.
        open_requests, consumer_records, derived_layouts = \
            _parse_requests(request_file)
        emitter.prepare(consumer_records, derived_layouts, Path(args.output))
        index = 0
        for link, path, generic_args in open_requests:
            if any(existing_link == link for existing_link, _ in requests):
                continue
            requests.append(
                (link, lambda s, i=index, p=path, a=generic_args:
                 emitter.open_query(s, f"r{i}", p, a)))
            index += 1

    if not requests and not contract and not native_cpp:
        raise RuntimeError("no consumer symbols and no open-generic requests")

    lines = emitter.header()
    if cpp_standard:
        lines.insert(0, "// dci-cxx-standard: " + json.dumps(cpp_standard))
    if native_cpp:
        from tools.dci.cpp_materialize import member_source
        lines += ["#include <new>", "#include <utility>"]
        for index, symbol in enumerate(native_cpp):
            lines += member_source(symbol, index)
            print(f"[dci-active] cpp {symbol['name']} -> {symbol['link_name']} CLOSED", file=sys.stderr)

    if requests:
        with emitter.session() as session:
            queries = [build(session) for _, build in requests]
            resolutions = session.resolve_batch(queries)
            records = emitter.records(session)

            failures = []
            for (link, _), resolution in zip(requests, resolutions):
                if resolution.status != ap.STATUS_CLOSED:
                    diagnostic = resolution.diagnostics[0]
                    failures.append(
                        f"  {resolution.query_id} ({link}): "
                        f"{resolution.status} {diagnostic.code}: "
                        f"{diagnostic.message.splitlines()[0]}")
            if failures:
                print("active adapter refused to close the following requests:",
                      file=sys.stderr)
                print("\n".join(failures), file=sys.stderr)
                return 1

            for (link, _), resolution in zip(requests, resolutions):
                facts = resolution.facts
                lines += emitter.symbol(link, facts["path"],
                                        facts["arguments"], records,
                                        facts.get("return_type", ""), facts)
                print(f"[dci-active] {emitter.lang} "
                      f"{'::'.join(facts['path'])} -> {link} CLOSED",
                      file=sys.stderr)

    for link, path, type_args in contract:
        lines += emitter.contract_symbol(link, path, type_args)
        detail = "::".join(path) if path else "provider export"
        print(f"[dci-active] {emitter.lang} {detail} -> {link} "
              f"CONTRACT", file=sys.stderr)

    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(lines), encoding="utf-8")
    return 0


def _clangxx() -> str:
    candidates = [
        os.environ.get("CLANGXX"),
        REPO_ROOT / "clang" / "bin" / "clang++.exe",
        "clang++",
    ]
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return str(Path(candidate))
    for candidate in candidates:
        if candidate and shutil.which(str(candidate)):
            return str(candidate)
    raise RuntimeError("clang++ was not found")


def _extra_compile_args(args: argparse.Namespace) -> list[str]:
    """Split the space-separated --include-args/--compile-args values.

    The Vyx build system passes these quoted to the external backend; before
    2026-09-18 they were parsed and then silently dropped, which made the
    external backend fail on any consumer that needed real include paths.
    """
    extra: list[str] = []
    for value in (getattr(args, "include_args", None),
                  getattr(args, "compile_args", None)):
        if value:
            extra.extend(shlex.split(value))
    return extra


def _compile_rust(args: argparse.Namespace) -> int:
    rustc = os.environ.get("RUSTC") or shutil.which("rustc")
    if not rustc:
        raise RuntimeError("rustc was not found")
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    command = [
        rustc,
        "--crate-name", "dci_open_generic_stub",
        "--crate-type", "lib",
        "--edition", "2021",
        "--target", "x86_64-pc-windows-msvc",
        "--emit=obj",
        "-C", "panic=abort",
        "-C", "opt-level=2",
        args.source,
        "-o", str(output),
    ]
    command.extend(_extra_compile_args(args))
    return subprocess.run(command, check=False).returncode


def _compile_cpp(args: argparse.Namespace) -> int:
    output = Path(args.output)
    first = Path(args.source).read_text(encoding="utf-8").splitlines()[0]
    standard = "c++20"
    if first.startswith("// dci-cxx-standard: "):
        standard = json.loads(first[len("// dci-cxx-standard: "):])
        if standard not in {"c++11", "c++14", "c++17", "c++20", "c++23", "c++26"}:
            raise RuntimeError("unsupported recorded C++ language standard")
    output.parent.mkdir(parents=True, exist_ok=True)
    command = [
        _clangxx(),
        "-std=" + standard,
        "--target=x86_64-pc-windows-msvc",
        # -O0 is load-bearing for method materialization: at -O2 clang folds
        # `recv.get_first()` on a zero-initialized receiver AND the
        # constant `&X::f` member-pointer store (static volatile globals get
        # constant-initialized), dropping the member's COMDAT definition and
        # leaving the consumer's reference undefined at link.  At -O0 the
        # call survives, the definition is emitted, and the runtime cost is
        # irrelevant for build-time materialization artifacts.
        "-O0",
        "-c", args.source,
        "-o", str(output),
    ]
    command.extend(_extra_compile_args(args))
    return subprocess.run(command, check=False).returncode


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(
        prog="dci-stub-backend",
        description=(
            "Stub backend for DCI contracts: emit host-callable shim source "
            "from a contract's stub requests / Active materialization "
            "requests, and compile the emitted source into a real object file."
        ),
    )
    # `--provider`/`--lang` are ROOT options, not subcommand options: the build
    # system splices `dci_stub_backend_tool_args` in front of the subcommand,
    # so the producer has to be parseable before `emit`/`compile` appears.
    root.add_argument("--provider", required=True,
                      help="producer source (native/lib.rs, native/lib.hpp, ...)")
    root.add_argument("--lang", choices=("rust", "cpp"),
                      help="producer language; defaults to the source extension")
    commands = root.add_subparsers(
        dest="command", required=True,
        help="emit: generate shim source; compile: compile shim source to an object",
    )

    emit_parser = commands.add_parser(
        "emit", help="generate shim source for the contract's requests")
    emit_parser.add_argument("--host-source", required=True,
                             help="consumer host source the shims are emitted for")
    emit_parser.add_argument("--output", required=True,
                             help="path of the shim source file to write")
    emit_parser.add_argument("--compiler", required=True,
                             help="compiler identity recorded in the emitted source")
    emit_parser.add_argument("--unit-sources",
                             help="path list file of consumer translation units")
    emit_parser.add_argument("--descriptor",
                             help="path of the .dcib contract driving the shims")
    emit_parser.add_argument("--requests",
                             help="path of a JSON file with explicit requests (overrides --descriptor)")
    emit_parser.add_argument("--host-args",
                             help="extra compiler args recorded for the host build")
    emit_parser.set_defaults(run=_emit)

    compile_parser = commands.add_parser(
        "compile", help="compile emitted shim source into an object file")
    compile_parser.add_argument("--source", required=True,
                                help="shim source file (from `emit`)")
    compile_parser.add_argument("--output", required=True,
                                help="object file path to produce")
    compile_parser.add_argument("--include-args",
                                help="space-separated include flags (-I/-isystem ...); "
                                "appended to the compiler command")
    compile_parser.add_argument("--compile-args",
                                help="extra space-separated compiler arguments "
                                "(e.g. \"-DFOO=1 -std=c++20\"); appended to the "
                                "compiler command")
    compile_parser.set_defaults(run=_compile_dispatch)
    return root


# --------------------------------------------------------------------------
# SDK stub-backend registry
# --------------------------------------------------------------------------
# These two built-ins are registered under the same names the Vyx build
# system uses for its backends (`clang-cpp`, `rustc`), so a third party can
# add another compile strategy through dci_plugin.register_stub_backend and
# enumerate everything with `dci plugins`.

def _compile_dispatch(args: argparse.Namespace) -> int:
    backend = None
    try:
        try:
            from dci_plugin import STUB_BACKENDS
        except ImportError:
            from tools.dci.dci_plugin import STUB_BACKENDS  # type: ignore
        backend = STUB_BACKENDS.get("clang-cpp" if args.lang == "cpp" else "rustc")
    except Exception:
        backend = None  # registry unavailable: fall back to built-ins below
    if backend is not None:
        return backend.compile(args)
    return _compile_cpp(args) if args.lang == "cpp" else _compile_rust(args)


def _register_builtin_stub_backends() -> None:
    try:
        try:
            from dci_plugin import StubBackendPlugin, register_stub_backend
        except ImportError:
            from tools.dci.dci_plugin import (  # type: ignore
                StubBackendPlugin, register_stub_backend,
            )
        register_stub_backend(StubBackendPlugin(
            name="clang-cpp", compile=_compile_cpp, languages=("cpp",)))
        register_stub_backend(StubBackendPlugin(
            name="rustc", compile=_compile_rust, languages=("rust",)))
    except Exception:
        pass  # dci_plugin unavailable: keep this module standalone-usable


_register_builtin_stub_backends()


# Values may legitimately start with "-" (e.g. `--compile-args "-DFOO=1"`),
# which argparse would otherwise mistake for another option.  The Vyx build
# system always passes these as two argv tokens, so join them into the
# `--opt=value` form before parsing — same trick as
# dci_adapter_msvc.normalize_dash_valued_options.
_VALUE_OPTIONS = frozenset({
    "--provider", "--lang", "--host-source", "--output", "--compiler",
    "--unit-sources", "--descriptor", "--requests", "--host-args",
    "--source", "--include-args", "--compile-args",
})


def _join_dash_valued_options(argv: list[str]) -> list[str]:
    joined: list[str] = []
    index = 0
    while index < len(argv):
        token = argv[index]
        if (token in _VALUE_OPTIONS and index + 1 < len(argv)
                and argv[index + 1].startswith("-")):
            joined.append(token + "=" + argv[index + 1])
            index += 2
        else:
            joined.append(token)
            index += 1
    return joined


def main(argv: list[str]) -> int:
    args = parser().parse_args(_join_dash_valued_options(list(argv)))
    if args.lang is None:
        args.lang = _language_for_provider(Path(args.provider))
    return args.run(args)


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except SystemExit:
        raise
    except Exception as error:
        print(f"external DCI Stub backend failed: {error}", file=sys.stderr)
        raise SystemExit(1)
