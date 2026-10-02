"""Active C++ adapter: on-demand semantic closure and code materialization.

Implements the C++ endpoint (doc §5.2, §6) on the Phase 0 control plane, the
mirror image of :mod:`tools.dci.active_rust`:

* ``open_session``  — pins compiler identity, standard, include resolution and
  header contents into a :class:`SemanticEnvironment`.
* ``resolve_batch`` — closes ``cpp/call/1`` requests for function templates
  declared in the session headers with concrete primitive/record arguments.
  Template selection and constraint checking (concepts, operator overloads)
  are delegated to the producer authority: a probe translation unit calling
  the template with concrete argument types must compile.
* ``materialize_batch`` — emits one ``extern "C"`` shim per closed request and
  compiles it with clang++ into a real object file.
* ``validate_and_publish`` — publishes through the Phase 0 artifact bundle.

Doc boundary: no layout conversion, no arbitrary expression requests — the
only operation surface is the explicit, versioned ``cpp/call/1``.
"""

from __future__ import annotations

import dataclasses
import hashlib
import re
import shutil
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable

from tools.dci import active_protocol as ap
from tools.dci import artifact_bundle as bundle

CAPABILITIES = ap.AdapterCapabilities(
    languages=("cpp",),
    operations=("cpp/call/1",),
    request_capabilities=(),
    profiles=("native",),
)

ADAPTER_ID = "active-cpp/1"

#: C++ spellings for the shared primitive namespace (width-fixed).
PRIMITIVE_SPELLINGS = {
    "i8": "signed char", "i16": "short", "i32": "int", "i64": "long long",
    "isize": "ptrdiff_t",
    "u8": "unsigned char", "u16": "unsigned short", "u32": "unsigned int",
    "u64": "unsigned long long", "usize": "size_t",
    "f32": "float", "f64": "double", "bool": "bool",
    # DCI `char` is the 1-byte character type (dci_translate_unwind: char is
    # (1,1)); C++ `char` is the matching spelling.  NOT the 4-byte Rust `char`.
    "char": "char",
}

_ERROR_CLASS = (
    ("no template named", ap.ERROR_ENTITY_NOT_FOUND),
    ("no member named", ap.ERROR_ENTITY_NOT_FOUND),
    ("undeclared identifier", ap.ERROR_ENTITY_NOT_FOUND),
    ("invalid operands", ap.ERROR_CONSTRAINT_FAILED),
    ("no matching function", ap.ERROR_CONSTRAINT_FAILED),
    ("static assertion", ap.ERROR_CONSTRAINT_FAILED),
    ("constraint failure", ap.ERROR_CONSTRAINT_FAILED),
    ("no type named", ap.ERROR_REPRESENTATION_INCOMPATIBLE),
    ("unknown type name", ap.ERROR_REPRESENTATION_INCOMPATIBLE),
)

_PRODUCER_SPAN = re.compile(r"^\s*\S*error.*?(\S+:\d+:\d+)", re.MULTILINE)


class CppActiveError(RuntimeError):
    """Active adapter failure that is not a per-query semantic refusal."""


class _Refused(Exception):
    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code
        self.message = message


class _ProbeError(Exception):
    def __init__(self, code: str, message: str, span: str = ""):
        super().__init__(message)
        self.code = code
        self.message = message
        self.span = span


@dataclass(frozen=True)
class CppSessionHandle:
    session: ap.SemanticEnvironment
    header: Path          # header inside the adapter work copy
    work: Path
    clangxx: str
    std: str
    include_dirs: tuple[Path, ...]
    triple: str


class CppActiveSession:
    """One Active Adapter session bound to a fixed C++ semantic environment."""

    def __init__(self, header: Path, *, include_dirs: Iterable[Path] = (),
                 std: str = "c++20", target: str = "x86_64-pc-windows-msvc",
                 clangxx: str | None = None, work_root: Path | None = None,
                 extra_source: str = ""):
        self.header = Path(header).resolve()
        if not self.header.is_file():
            raise CppActiveError(f"header not found: {self.header}")
        self.clangxx = clangxx or shutil.which("clang++") \
            or shutil.which("clang-cl")
        if not self.clangxx:
            raise CppActiveError("clang++ executable not found")
        self.std = std
        self.target = target
        self.include_dirs = [Path(d).resolve() for d in include_dirs]

        self._temp: tempfile.TemporaryDirectory | None = None
        if work_root is None:
            self._temp = tempfile.TemporaryDirectory(prefix="dci-active-cpp-")
            work = Path(self._temp.name)
        else:
            work = Path(work_root)
            work.mkdir(parents=True, exist_ok=True)
        self.work = work
        self.header_copy = work / self.header.name
        self.header_copy.write_bytes(self.header.read_bytes())
        if extra_source:
            # Definitions of records only the *consumer* declares, appended to
            # the header the probe TU includes: `_record_exists` and every
            # instantiation the probe makes have to see them.
            self.header_copy.write_text(
                self.header_copy.read_text(encoding="utf-8", errors="replace")
                + "\n" + extra_source,
                encoding="utf-8")

        version = subprocess.run([self.clangxx, "--version"], capture_output=True,
                                 text=True, encoding="utf-8", errors="replace",
                                 timeout=30, check=True).stdout
        producer = version.splitlines()[0].strip() if version else "clang++"

        digests: dict[str, str] = {
            self.header.name: hashlib.sha256(self.header.read_bytes()).hexdigest()
        }
        for directory in self.include_dirs:
            for candidate in sorted(directory.glob("**/*.h")):
                digests[f"{directory.name}/{candidate.name}"] = \
                    hashlib.sha256(candidate.read_bytes()).hexdigest()
        if extra_source:
            digests["dci_consumer_records"] = hashlib.sha256(
                extra_source.encode("utf-8")).hexdigest()

        self.handle = CppSessionHandle(
            session=ap.SemanticEnvironment(
                language="cpp",
                producer=producer,
                target_triple=target,
                abi_family="win64" if "windows" in target else "sysv64",
                environment={
                    "std": std,
                    "include_dirs": [str(d) for d in self.include_dirs],
                },
                source_digests=digests,
            ),
            header=self.header_copy,
            work=work,
            clangxx=self.clangxx,
            std=std,
            include_dirs=tuple(self.include_dirs),
            triple=target,
        )
        self._template_names: set[str] | None = None
        self._class_templates: set[str] | None = None

    # -- lifecycle ----------------------------------------------------------

    def close(self) -> None:
        if self._temp is not None:
            self._temp.cleanup()
            self._temp = None

    def __enter__(self) -> "CppActiveSession":
        return self

    def __exit__(self, *_exc: Any) -> None:
        self.close()

    @property
    def session(self) -> ap.SemanticEnvironment:
        return self.handle.session

    # -- protocol surface ---------------------------------------------------

    def resolve_batch(self, queries: Iterable[ap.Query]) -> list[ap.Resolution]:
        """Close, refuse or mark unsupported every query (doc §6)."""
        queries = list(queries)
        for query in queries:
            query.validate(CAPABILITIES)
        checked = [self._check_query(query) for query in queries]
        if not any(result is None for result in checked):
            return checked

        # Producer closure: one probe TU for all open requests, then per-query
        # attribution when the combined TU fails.
        probe = self._write_probe(queries, checked, "dci_probe_all")
        try:
            self._compile_probe(probe)
        except _ProbeError:
            return self._resolve_individually(queries, checked)
        return [static if static is not None else self._close_request(query)
                for query, static in zip(queries, checked)]

    def materialize_batch(self, resolutions: Iterable[ap.Resolution]) -> ap.BundleCandidate:
        """Build one shim object for every closed resolution (doc §6)."""
        closed = [r for r in resolutions if r.status == ap.STATUS_CLOSED]
        if not closed:
            raise CppActiveError("materialize_batch: no closed resolutions")
        seen: set[str] = set()
        for resolution in closed:
            if resolution.request_key in seen:
                raise CppActiveError(
                    f"materialize_batch: duplicate request {resolution.request_key[:12]}")
            seen.add(resolution.request_key)

        source = self._shim_source(closed)
        shim_path = self.handle.work / "dci_shim.cpp"
        shim_path.write_text(source, encoding="utf-8")
        object_path = self.handle.work / "dci_shim.obj"
        command = [
            self.handle.clangxx, f"-std={self.handle.std}",
            f"--target={self.handle.triple}",
            *[f"-I{d}" for d in self.handle.include_dirs],
            "-c", str(shim_path), "-o", str(object_path),
        ]
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                text=True, encoding="utf-8", errors="replace",
                                timeout=180, check=False)
        if result.returncode != 0:
            detail = (result.stderr or result.stdout).strip()
            raise CppActiveError(f"materialization_failed: clang++ rejected shim\n{detail}")
        if not object_path.is_file() or object_path.stat().st_size == 0:
            raise CppActiveError("materialization_failed: clang++ produced no object")
        artifact_bytes = _normalize_coff_timestamp(
            object_path.read_bytes(), self.handle.triple)

        artifact_name = "shims.obj"
        contracts: dict[str, bytes] = {}
        for resolution in closed:
            contracts[f"{resolution.request_key[:24]}.dcib"] = ap.canonical_bytes({
                "protocol": ap.PROTOCOL,
                "language": "cpp",
                "request_key": resolution.request_key,
                "instance_key": resolution.instance_key,
                "operation_key": resolution.operation_key,
                "symbol": resolution.facts["symbol"],
                "artifact": artifact_name,
                "facts": resolution.facts,
            })
        object_format = "coff" if "windows" in self.handle.triple else "elf"
        return ap.BundleCandidate(
            session=self.handle.session,
            contracts=contracts,
            artifacts={artifact_name: (artifact_bytes, f"{object_format}-object")},
            resolutions=[
                dataclasses.replace(r, implementations=(
                    ap.RequiredImplementation(
                        symbol=r.facts["symbol"], artifact=artifact_name,
                        kind="object"),))
                for r in closed
            ],
            link_requirements=[{"artifact": artifact_name, "kind": "object",
                                "link_mode": "required"}],
            adapter=ADAPTER_ID,
            producer=self.handle.session.producer,
        )

    @staticmethod
    def validate_and_publish(candidate: ap.BundleCandidate, cache_root: Path) -> str:
        return bundle.stage_and_publish(candidate, Path(cache_root))

    # -- internals ----------------------------------------------------------

    @property
    def template_names(self) -> set[str]:
        if self._template_names is None:
            text = self.header_copy.read_text(encoding="utf-8", errors="replace")
            self._template_names = _template_function_names(text)
        return self._template_names

    @property
    def class_template_names(self) -> set[str]:
        if self._class_templates is None:
            self._class_templates = _class_template_names(
                self.header_copy.read_text(encoding="utf-8", errors="replace"))
        return self._class_templates

    def _is_method_query(self, path: tuple[str, ...]) -> bool:
        """`Type::member` shape: path[0] names a class template in the header.

        A method of a record template has no standalone ABI, so it is not a
        free function: the request names the *type* and the member, and the
        type arguments carried by the request select the instance -- the
        mirror image of active_rust's generic-method closure.
        """
        return len(path) >= 2 and path[0] in self.class_template_names

    def _header_text(self) -> str:
        return self.header_copy.read_text(encoding="utf-8", errors="replace")

    def _record_exists(self, name: str) -> bool:
        return re.search(rf"\b(?:struct|class|union)\s+{re.escape(name)}\b",
                         self._header_text()) is not None

    def _arg_spelling(self, ref: dict[str, Any]) -> str:
        form, name = ref.get("form"), ref.get("name", "")
        if form == ap.TYPE_PRIMITIVE:
            spelling = PRIMITIVE_SPELLINGS.get(name)
            if spelling is None:
                raise _Refused(ap.ERROR_REPRESENTATION_INCOMPATIBLE,
                               f"unknown primitive {name!r}")
            return spelling
        if form == ap.TYPE_RECORD:
            if not self._record_exists(name):
                raise _Refused(ap.ERROR_REPRESENTATION_INCOMPATIBLE,
                               f"record {name!r} does not exist in the session headers")
            return name
        raise _Refused(ap.ERROR_CAPABILITY_UNSUPPORTED,
                       f"type form {form!r} is outside the phase 1 request surface")

    def _check_query(self, query: ap.Query) -> ap.Resolution | None:
        entity = query.entity
        if entity["kind"] not in ("generic", "function"):
            return ap.Resolution.refused(
                query, self.session, status=ap.STATUS_UNSUPPORTED,
                code=ap.ERROR_CAPABILITY_UNSUPPORTED,
                message=f"entity kind {entity['kind']!r} not supported")
        path = _normalize_path(entity.get("path", ""))
        name = path[-1]
        if self._is_method_query(path):
            # `Type::member`: static checks only -- whether the instance
            # actually has that member with usable constraints is the
            # producer's call, decided by the probe below.
            try:
                [self._arg_spelling(arg) for arg in entity.get("args", [])]
            except _Refused as refused:
                return ap.Resolution.refused(
                    query, self.session, status=ap.STATUS_REJECTED,
                    code=refused.code, message=refused.message)
            return None
        if name not in self.template_names:
            return ap.Resolution.refused(
                query, self.session, status=ap.STATUS_REJECTED,
                code=ap.ERROR_ENTITY_NOT_FOUND,
                message=f"no function template {name!r} in the session headers")
        try:
            spellings = [self._arg_spelling(arg) for arg in entity.get("args", [])]
        except _Refused as refused:
            return ap.Resolution.refused(
                query, self.session, status=ap.STATUS_REJECTED,
                code=refused.code, message=refused.message)
        return None

    def _resolve_individually(self, queries: list[ap.Query],
                              checked: list[ap.Resolution | None]) -> list[ap.Resolution]:
        results: list[ap.Resolution] = []
        for query, static in zip(queries, checked):
            if static is not None:
                results.append(static)
                continue
            probe = self._write_probe([query], [None], "dci_probe_one")
            try:
                self._compile_probe(probe)
            except _ProbeError as exc:
                results.append(ap.Resolution.refused(
                    query, self.session, status=ap.STATUS_REJECTED,
                    code=exc.code, message=exc.message, producer_span=exc.span))
            else:
                results.append(self._close_request(query))
        return results

    def _write_probe(self, queries: list[ap.Query],
                     static: list[ap.Resolution | None], module: str) -> Path:
        lines = [f'#include "{self.handle.header.name}"']
        for index, (query, is_static) in enumerate(zip(queries, static)):
            if is_static is not None:
                continue
            path = _normalize_path(query.entity.get("path", ""))
            try:
                spellings = [self._arg_spelling(arg)
                             for arg in query.entity.get("args", [])]
            except _Refused as refused:
                raise _ProbeError(refused.code, refused.message) from refused
            if self._is_method_query(path):
                # Probe = call the member on the instance.  clang is the
                # authority on existence, constraints and ABI; constructing
                # the receiver default-initializes it, which every record
                # the session can name must support anyway.
                receiver = _receiver_spelling(path, spellings)
                lines.append(
                    f"[[maybe_unused]] static void __{module}_{index}() {{ "
                    f"{receiver} recv{{}}; (void)(recv.{path[-1]}(), 0); }}")
                continue
            params = ", ".join(f"{sp} v{i}" for i, sp in enumerate(spellings))
            arguments = ", ".join(f"v{i}" for i in range(len(spellings)))
            lines.append(
                f'[[maybe_unused]] static void __{module}_{index}({params}) '
                f"{{ (void){'::'.join(path)}({arguments}); }}")
        probe = self.handle.work / f"{module}.cpp"
        probe.write_text("\n".join(lines) + "\n", encoding="utf-8")
        return probe

    def _compile_probe(self, probe: Path) -> None:
        command = [
            self.handle.clangxx, f"-std={self.handle.std}",
            f"--target={self.handle.triple}",
            *[f"-I{d}" for d in self.handle.include_dirs],
            "-fsyntax-only", str(probe),
        ]
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                text=True, encoding="utf-8", errors="replace",
                                timeout=120, check=False)
        if result.returncode != 0:
            stderr = (result.stderr or result.stdout).strip()
            code, message = _classify_clang(stderr)
            raise _ProbeError(code, message, span=_first_span(stderr))

    def _close_request(self, query: ap.Query) -> ap.Resolution:
        path = _normalize_path(query.entity.get("path", ""))
        spellings = [self._arg_spelling(arg) for arg in query.entity.get("args", [])]
        symbol = _symbol_name(path, spellings)
        if self._is_method_query(path):
            facts = {
                "operation": "cpp/call/1",
                "symbol": symbol,
                "method": True,
                "abi": "cxx-method",
                "calling_convention":
                    "win64" if "windows" in self.handle.triple else "sysv64",
                "path": list(path),
                "arguments": spellings,
                "receiver": _receiver_spelling(path, spellings),
            }
            return ap.Resolution.closed(
                query, self.session,
                result_types=[ap.type_ref(ap.TYPE_NATIVE, "decltype(producer-call)")],
                facts=facts,
            )
        facts = {
            "operation": "cpp/call/1",
            "symbol": symbol,
            "abi": "c-shim",
            "calling_convention": "cdecl" if "windows" in self.handle.triple else "sysv64",
            "path": list(path),
            "arguments": spellings,
            "parameters": [
                {"index": i, "type": spelling, "ownership": "by-value"}
                for i, spelling in enumerate(spellings)
            ],
        }
        return ap.Resolution.closed(
            query, self.session,
            result_types=[ap.type_ref(ap.TYPE_NATIVE, "decltype(producer-call)")],
            facts=facts,
        )

    def _shim_source(self, closed: list[ap.Resolution]) -> str:
        lines = [f'#include "{self.handle.header.name}"']
        for resolution in closed:
            facts = resolution.facts
            symbol = facts["symbol"]
            spellings: list[str] = facts["arguments"]
            path = facts["path"]
            if facts.get("method"):
                receiver = facts.get("receiver") or _receiver_spelling(path, spellings)
                member = path[-1]
                keep = f"dci_active_keep_{len(lines)}"
                lines += [
                    f'extern "C" void {symbol}() {{',
                    f"    {receiver} recv{{}};",
                    f"    (void)(recv.{member}(), 0);",
                    "}",
                    f"static auto volatile {keep} = &{receiver}::{member};",
                ]
                continue
            spell_text = "::".join(str(p) for p in path)
            params = ", ".join(f"{sp} v{i}" for i, sp in enumerate(spellings))
            arguments = ", ".join(f"v{i}" for i in range(len(spellings)))
            # decltype keeps the shim's ABI exactly the template's own return
            # type without re-implementing template type deduction here.
            lines.append(
                f'extern "C" auto {symbol}({params}) '
                f"-> decltype({spell_text}({arguments})) "
                f"{{ return {spell_text}({arguments}); }}")
        return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# helpers


def _normalize_coff_timestamp(data: bytes, triple: str) -> bytes:
    """Zero the COFF TimeDateStamp so identical shims hash identically.

    clang stamps every COFF object with the wall clock and ignores
    SOURCE_DATE_EPOCH for objects; the bundle identity must bind shim
    *content*, not the minute it happened to be built.
    """
    if "windows" not in triple or len(data) < 8 or data[:2] not in (b"\x64\x86", b"\x4c\x01"):
        return data
    return data[:4] + b"\x00\x00\x00\x00" + data[8:]


def _normalize_path(raw: Any) -> tuple[str, ...]:
    text = raw if isinstance(raw, str) else "::".join(str(p) for p in raw)
    return tuple(part for part in re.sub(r"^(?:global|::)", "", text).split("::")
                 if part)


def _receiver_spelling(path: tuple[str, ...], spellings: list[str]) -> str:
    """`Pair2<int, double>` -- the instance a method request is received on."""
    owner = "::".join(path[:-1])
    if not spellings:
        return owner
    return f"{owner}<{', '.join(spellings)}>"


def _class_template_names(text: str) -> set[str]:
    """Names of class/struct templates declared in ``text``.

    The scan walks every ``template <...>`` header and requires a
    ``struct|class`` declaration afterwards; function templates
    (``template <typename T> T twice(T v) {``) have no such keyword before
    the declaration, so they are excluded.
    """
    names: set[str] = set()
    for match in re.finditer(r"\btemplate\s*<", text):
        depth, index = 1, match.end()
        while index < len(text) and depth:
            if text[index] == "<":
                depth += 1
            elif text[index] == ">":
                depth -= 1
            index += 1
        segment = text[index:index + 200]
        declared = re.match(r"[^;={}]*?\b(?:struct|class)\s+([A-Za-z_]\w*)", segment)
        if declared and "{" in segment:
            names.add(declared.group(1))
    return names


def _template_function_names(text: str) -> set[str]:
    """Names of function templates declared in ``text`` (phase 1 scan)."""
    names: set[str] = set()
    for match in re.finditer(r"\btemplate\s*<", text):
        depth, index = 1, match.end()
        while index < len(text) and depth:
            if text[index] == "<":
                depth += 1
            elif text[index] == ">":
                depth -= 1
            index += 1
        paren = text.find("(", index)
        if paren == -1:
            continue
        segment = text[index:paren]
        if "{" in segment:
            continue  # class/variable template body, not a function template
        identifiers = re.findall(r"[A-Za-z_]\w*", segment)
        if identifiers:
            names.add(identifiers[-1])
    return names


def _symbol_name(path: tuple[str, ...], spellings: list[str]) -> str:
    parts = ["__vyx_cpp", *path]
    for spelling in spellings:
        parts.append(re.sub(r"[^A-Za-z0-9]", "_", spelling))
    return "_".join(parts)


def _first_error_line(stderr: str) -> str:
    """The diagnostic itself, not the `In file included from ...` context.

    A failure inside a template body is reported with the include chain first,
    and that chain carries absolute paths -- carrying it into a resolution
    makes the message (and anything hashed or golden-compared from it)
    machine-dependent.
    """
    for line in stderr.splitlines():
        if "error:" in line:
            return line.strip()
    return stderr.splitlines()[0] if stderr else "clang++ failed"


def _classify_clang(stderr: str) -> tuple[str, str]:
    lowered = stderr.lower()
    for marker, code in _ERROR_CLASS:
        if marker in lowered:
            return code, _first_error_line(stderr)
    return ap.ERROR_MATERIALIZATION_FAILED, _first_error_line(stderr)


def _first_span(stderr: str) -> str:
    match = _PRODUCER_SPAN.search(stderr)
    return match.group(1) if match else ""


def describe_capabilities() -> ap.AdapterCapabilities:
    return CAPABILITIES


def open_session(header: Path, **kwargs: Any) -> CppActiveSession:
    return CppActiveSession(header, **kwargs)


__all__ = [
    "ADAPTER_ID", "CAPABILITIES", "PRIMITIVE_SPELLINGS",
    "CppActiveError", "CppActiveSession", "CppSessionHandle",
    "describe_capabilities", "open_session",
]
