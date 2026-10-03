"""Active Rust adapter: on-demand semantic closure and code materialization.

Implements the Rust endpoint (doc §5.3, §6) on top of the Phase 0 control
plane (:mod:`tools.dci.active_protocol`, :mod:`tools.dci.artifact_bundle`):

* ``open_session``  — pins the producer environment (rustc identity, target,
  edition, source digests) into a :class:`SemanticEnvironment`.
* ``resolve_batch`` — closes ``rust/call/1`` requests for in-crate generic
  functions with concrete primitive/record arguments.  Constraint checking is
  delegated to the producer authority: a probe crate calling the requested
  function with concrete argument types is type-checked by rustc, so trait
  bounds (``T: Clone`` ...) are decided by rustc, never guessed here.
* ``materialize_batch`` — emits one ``#[no_mangle] pub extern "C"`` shim per
  closed request and compiles it with rustc into a real object file.
* ``validate_and_publish`` — publishes through the Phase 0 artifact bundle
  (content addressing + atomic rename + full re-verification on load).

Doc boundary: representations are never converted and open generics never
enter the closed plane.  A request this adapter cannot close is ``rejected``
or ``unsupported`` with a §6 error code and the producer diagnostic.
"""

from __future__ import annotations

import dataclasses
import hashlib
import json
import re
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable

from tools.dci import active_protocol as ap
from tools.dci import artifact_bundle as bundle
from tools.dci.dci_adapter_rust import (
    GenericMethodDecl,
    RustParser,
    copy_crate_tree,
    discover_rustc,
    inferred_edition,
    cargo_package,
    cargo_declarations,
    find_cargo_manifest,
    lex_rust,
    matching_index,
    rustc_identity,
    run_checked,
    target_info,
    tokens_text,
)
from tools.dci.rust_cargo import CargoContext

CAPABILITIES = ap.AdapterCapabilities(
    languages=("rust",),
    operations=("rust/call/1",),
    request_capabilities=("rust/borrow/1",),
    profiles=("native",),
)

ADAPTER_ID = "active-rust/1"

#: Primitives this adapter can pass through the C shim ABI unmodified.
PRIMITIVES = frozenset({
    "i8", "i16", "i32", "i64", "isize",
    "u8", "u16", "u32", "u64", "usize",
    "f32", "f64", "bool", "char",
})

_RUSTC_ERROR_CLASS = (
    ("E0277", ap.ERROR_CONSTRAINT_FAILED),            # trait bound not satisfied
    ("E0308", ap.ERROR_REPRESENTATION_INCOMPATIBLE),  # mismatched types
    ("E0425", ap.ERROR_ENTITY_NOT_FOUND),             # unresolved name
    ("E0412", ap.ERROR_ENTITY_NOT_FOUND),             # unresolved type
    ("E0463", ap.ERROR_DEPENDENCY_UNAVAILABLE),       # can't find crate
)

_PRODUCER_SPAN = re.compile(r"-->\s+(\S+:\d+:\d+)")


class RustActiveError(RuntimeError):
    """Active adapter failure that is not a per-query semantic refusal."""


class _Refused(Exception):
    """Internal: static refusal found before the producer compiler runs."""

    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code
        self.message = message


class _ProbeError(Exception):
    """Internal: the producer compiler refused one probe."""

    def __init__(self, code: str, message: str, span: str = ""):
        super().__init__(message)
        self.code = code
        self.message = message
        self.span = span


# ---------------------------------------------------------------------------
# session


@dataclass(frozen=True)
class RustSessionHandle:
    """Everything resolve/materialize need; ``session`` is the protocol doc."""

    session: ap.SemanticEnvironment
    crate_name: str
    edition: str
    entry: Path           # entry .rs inside the adapter work copy
    work: Path            # parent of the entry (scratch dir)
    rustc: str
    triple: str
    object_format: str


class RustActiveSession:
    """One Active Adapter session bound to a fixed semantic environment."""

    def __init__(self, crate_entry: Path | None = None, *, target_triple: str = "x86_64-pc-windows-msvc",
                 rustc: str | None = None, work_root: Path | None = None,
                 extra_source: str = "", manifest_path: Path | None = None,
                 package: str | None = None, features: tuple[str, ...] = (),
                 no_default_features: bool = False, offline: bool = False,
                 locked: bool = False):
        self.rustc = discover_rustc(rustc)
        self.target = target_info(self.rustc, target_triple)
        self._temp: tempfile.TemporaryDirectory | None = None
        if work_root is None:
            self._temp = tempfile.TemporaryDirectory(prefix="dci-active-rust-")
            root = Path(self._temp.name)
        else:
            root = Path(work_root)
            root.mkdir(parents=True, exist_ok=True)
        self.cargo = None
        if manifest_path is not None:
            self.cargo = CargoContext(manifest_path, package=package, rustc=self.rustc,
                                      target=target_triple, features=features,
                                      no_default_features=no_default_features,
                                      offline=offline, locked=locked,
                                      work_root=root / "cargo")
            self.crate_entry = self.cargo.root
        elif crate_entry is not None:
            self.crate_entry = Path(crate_entry).resolve()
        else:
            raise RustActiveError("crate_entry or manifest_path is required")
        if not self.crate_entry.is_file():
            raise RustActiveError(f"crate entry not found: {self.crate_entry}")
        self.edition = self.cargo.edition if self.cargo else inferred_edition(self.crate_entry)
        self._producer_args = self.cargo.args if self.cargo else []
        self._environment = self.cargo.environment if self.cargo else None
        self._dependency_args = []
        for index, argument in enumerate(self._producer_args):
            if argument == "-L" and index + 1 < len(self._producer_args):
                self._dependency_args.extend([argument, self._producer_args[index + 1]])
        self.work = copy_crate_tree(self.crate_entry, root)
        if extra_source:
            # Definitions of records only the *consumer* declares.  They belong
            # at the crate root: the request spellings name them as
            # `crate_shim::TestS`, and neither the producer's source nor its
            # crate has ever mentioned them.
            self.work.write_text(
                self.work.read_text(encoding="utf-8") + "\n" + extra_source,
                encoding="utf-8")
        identity = rustc_identity(self.rustc)
        _, package = cargo_package(self.crate_entry)
        name = self.cargo.crate_name if self.cargo else (str(package.get("name", "")).strip() or self.crate_entry.stem)
        self.crate_name = re.sub(r"[^A-Za-z0-9_]", "_", name)

        digests: dict[str, str] = {}
        for source in sorted(self.crate_entry.parent.rglob("*.rs")):
            if source.is_file():
                digest_file(source, source.relative_to(self.crate_entry.parent), digests)
        manifest = find_cargo_manifest(self.crate_entry)
        if manifest is not None:
            digest_file(manifest, Path("Cargo.toml"), digests)
            lock = manifest.parent / "Cargo.lock"
            if lock.is_file():
                digest_file(lock, Path("Cargo.lock"), digests)
        if self.cargo:
            digests["cargo-environment"] = hashlib.sha256(json.dumps(
                self.cargo.provenance, sort_keys=True).encode()).hexdigest()
            for argument in self._producer_args:
                if "=" in argument:
                    dependency = Path(argument.split("=", 1)[1])
                    if dependency.is_file() and dependency.suffix in {".rlib", ".rmeta", ".dll", ".so"}:
                        digest_file(dependency, Path("dependencies") / dependency.name, digests)
        if extra_source:
            digests["dci_consumer_records"] = hashlib.sha256(
                extra_source.encode("utf-8")).hexdigest()

        self.handle = RustSessionHandle(
            session=ap.SemanticEnvironment(
                language="rust",
                producer=f"rustc {identity['version']}",
                target_triple=self.target.triple,
                abi_family=self.target.abi_family,
                environment={"edition": self.edition},
                source_digests=digests,
            ),
            crate_name=self.crate_name,
            edition=self.edition,
            entry=self.work,
            work=self.work.parent,
            rustc=self.rustc,
            triple=self.target.triple,
            object_format=self.target.object_format,
        )
        self._parsed = None
        self._rlib: Path | None = None

    # -- lifecycle ----------------------------------------------------------

    def close(self) -> None:
        if self.cargo is not None:
            self.cargo.close()
        if self._temp is not None:
            self._temp.cleanup()
            self._temp = None

    def __enter__(self) -> "RustActiveSession":
        return self

    def __exit__(self, *_exc: Any) -> None:
        self.close()

    @property
    def session(self) -> ap.SemanticEnvironment:
        return self.handle.session

    @property
    def parsed(self):
        if self._parsed is None:
            self._parsed = (cargo_declarations(self.cargo, self.rustc, self.handle.triple)
                            if self.cargo else RustParser(self.work).parse())
        return self._parsed

    # -- protocol surface ---------------------------------------------------

    def resolve_batch(self, queries: Iterable[ap.Query]) -> list[ap.Resolution]:
        """Close, refuse or mark unsupported every query (doc §6).

        Fast path: one combined probe closes all open requests with a single
        producer invocation.  When the combined probe fails, each request is
        re-probed individually so diagnostics attach to the right key.
        """
        queries = list(queries)
        for query in queries:
            query.validate(CAPABILITIES)
        checked = [self._check_query(query) for query in queries]
        if not any(result is None for result in checked):
            return checked
        rlib = self._ensure_rlib()
        combined = self._write_probe(queries, checked, "dci_probe_all")
        try:
            self._compile_probe(combined, rlib)
        except _ProbeError:
            return self._resolve_individually(queries, checked, rlib)
        return [static if static is not None else self._close_request(query)
                for query, static in zip(queries, checked)]

    def materialize_batch(self, resolutions: Iterable[ap.Resolution]) -> ap.BundleCandidate:
        """Build one shim object for every closed resolution (doc §6)."""
        closed = [r for r in resolutions if r.status == ap.STATUS_CLOSED]
        if not closed:
            raise RustActiveError("materialize_batch: no closed resolutions")
        seen: set[str] = set()
        for resolution in closed:
            if resolution.request_key in seen:
                raise RustActiveError(
                    f"materialize_batch: duplicate request {resolution.request_key[:12]}")
            seen.add(resolution.request_key)
        rlib = self._ensure_rlib()
        source, symbols = self._shim_source(closed)
        shim_path = self.handle.work / "dci_shim.rs"
        shim_path.write_text(source, encoding="utf-8")
        object_path = self.handle.work / "dci_shim.obj"
        command = [
            self.handle.rustc, "--edition", self.handle.edition,
            "--crate-type", "lib", "--emit=obj",
            "--crate-name", "dci_shim",
            "--target", self.handle.triple,
            "--extern", f"crate_shim={rlib}",
            "-C", "panic=abort", "-C", "opt-level=1",
            "-A", "warnings",
            str(shim_path), "-o", str(object_path),
        ]
        command.extend(self._dependency_args)
        try:
            run_checked(command, env=self._environment, timeout=180)
        except Exception as exc:  # RustAdapterError carries the rustc detail
            raise RustActiveError(f"materialization_failed: rustc rejected shim\n{exc}") from exc
        if not object_path.is_file() or object_path.stat().st_size == 0:
            raise RustActiveError("materialization_failed: rustc produced no object")
        artifact_bytes = _normalize_coff_timestamp(
            object_path.read_bytes(), self.handle.triple)

        artifact_name = "shims.obj"
        contracts: dict[str, bytes] = {}
        for resolution, symbol in zip(closed, symbols):
            closed_with_impl = dataclasses.replace(
                resolution,
                implementations=(ap.RequiredImplementation(
                    symbol=symbol, artifact=artifact_name, kind="object"),))
            contracts[f"{resolution.request_key[:24]}.dcib"] = ap.canonical_bytes({
                "protocol": ap.PROTOCOL,
                "language": "rust",
                "request_key": resolution.request_key,
                "instance_key": resolution.instance_key,
                "operation_key": resolution.operation_key,
                "symbol": symbol,
                "artifact": artifact_name,
                "facts": resolution.facts,
            })
        return ap.BundleCandidate(
            session=self.handle.session,
            contracts=contracts,
            artifacts={artifact_name: (
                artifact_bytes, f"{self.handle.object_format}-object")},
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

    def _normalize_path(self, raw: Any) -> tuple[str, ...]:
        text = raw if isinstance(raw, str) else "::".join(str(p) for p in raw)
        text = re.sub(r"^crate::", "", text)
        return tuple(part for part in text.split("::") if part)

    def _find_function(self, path: tuple[str, ...]):
        for function in self.parsed.functions:
            if function.path == path:
                return function
        return None

    def _find_record(self, path: tuple[str, ...]):
        for table in (self.parsed.records, self.parsed.enums):
            for key in table:
                if key == path:
                    return key
        return None

    def _find_generic_method(self, path: tuple[str, ...]):
        """Match `Pair2::get_first` against an `impl<A, B> Pair2<A, B>` method.

        A method of an open generic has no standalone ABI, so it is not a
        free function: the request names the *type* and the member, and the
        type arguments are carried by the request's `args` -- which is what
        lets the Active Adapter close `Pair2::<f64, i32>::get_first` with
        rustc, whether or not the producer ever mentioned that instance.
        """
        if len(path) < 2:
            return None
        owner_name, member = path[-2], path[-1]
        for declaration in self.parsed.generic_methods:
            if declaration.owner == path[:-1] and declaration.name == member:
                return declaration
        # A method of a *non-generic* type is a method all the same.  C++ has
        # always described `Vec2::norm1` as `kind=method`; a rust
        # `impl Vec2 { pub fn norm1(&self) -> f64 }` is the same shape, except
        # that the request for it carries no type arguments at all.  Without
        # this the request fell through to the free-function path, where the
        # receiver had to be spelled as a *value* argument -- `&Vec2 -- which
        # no record in the producer answers to, so the whole build died at
        #   representation_incompatible: record '&Vec2' does not exist in the
        #   producer crate
        # `rewrite_receiver_params` always names the receiver `self`, and
        # `self` is not a legal Rust parameter name, so this test is exact.
        for declaration in self.parsed.functions:
            if (len(declaration.path) < 2
                    or declaration.path[:-1] != path[:-1]
                    or declaration.path[-1] != member
                    or not declaration.params
                    or declaration.params[0].name != "self"):
                continue
            return GenericMethodDecl(owner=declaration.path[:-1], generics=[],
                                     owner_args=[], method=declaration)
        return None

    def _instance_argument_ok(self, name: str) -> bool:
        """Is `name` a type argument the producer can write down?"""
        if name in PRIMITIVES:
            return True
        return self._find_record(self._normalize_path(name)) is not None

    def _check_method_query(self, query: ap.Query,
                            declaration) -> ap.Resolution | None:
        """Static checks for `Type<...>::member` (no value parameters)."""
        spellings = [ref.get("name", "") for ref in query.entity.get("args", [])]
        if len(spellings) != len(declaration.generics):
            return ap.Resolution.refused(
                query, self.session, status=ap.STATUS_REJECTED,
                code=ap.ERROR_CONSTRAINT_FAILED,
                message=f"instance {declaration.owner[-1]}::"
                        f"{declaration.name} takes "
                        f"{len(declaration.generics)} type argument(s), got "
                        f"{len(spellings)}")
        for spelling in spellings:
            if not self._instance_argument_ok(spelling):
                return ap.Resolution.refused(
                    query, self.session, status=ap.STATUS_REJECTED,
                    code=ap.ERROR_REPRESENTATION_INCOMPATIBLE,
                    message=f"type argument {spelling!r} does not exist in the "
                            f"producer crate")
        if not declaration.method.public:
            return ap.Resolution.refused(
                query, self.session, status=ap.STATUS_REJECTED,
                code=ap.ERROR_ENTITY_NOT_FOUND,
                message=f"{declaration.owner[-1]}::{declaration.name} is not public")
        return None

    def _qualified_arg(self, name: str) -> str:
        """Spell a type argument for generated rust source.

        A record argument (`Vec2`) lives in the provider crate, so in the
        probe/shim module scope it must be spelled `crate_shim::Vec2` --
        a bare `Vec2` does not resolve and reads like a failed bound.  A
        primitive passes through unchanged.  The *query* spellings stay
        plain; this is only for rustc-facing source.
        """
        record = self._find_record(self._normalize_path(name))
        if record is not None:
            return "crate_shim::" + "::".join(record)
        return name

    def _method_receiver_spelling(self, declaration, args: list[str]) -> str:
        owner = "crate_shim::" + "::".join(declaration.owner)
        if not args:
            return owner
        return owner + "<" + ", ".join(self._qualified_arg(a) for a in args) + ">"

    def _method_borrows_receiver(self, declaration) -> bool:
        """`&self` / `&mut self` vs a by-value `self`."""
        if not declaration.method.params:
            return False
        text = "".join(token.value for token in
                       declaration.method.params[0].type_tokens)
        return text.strip().startswith("&")

    def _method_call(self, declaration, args: list[str], argument: str) -> str:
        owner = "crate_shim::" + "::".join(declaration.owner)
        turbofish = (f"::<{', '.join(self._qualified_arg(a) for a in args)}>"
                     if args else "")
        return f"{owner}{turbofish}::{declaration.name}({argument})"

    def _method_parameter_spellings(self, declaration, args: list[str]) -> list[str]:
        mapping = {name: self._qualified_arg(value)
                   for name, value in zip(declaration.generics, args)}
        result = []
        for index, parameter in enumerate(declaration.method.params):
            if index == 0 and parameter.name == "self":
                receiver = self._method_receiver_spelling(declaration, args)
                original = tokens_text(parameter.type_tokens).strip()
                if original.startswith("&"):
                    receiver = ("&mut " if "mut" in original.split() else "&") + receiver
                result.append(receiver)
            else:
                result.append(self._qualify_type(parameter.type_tokens,
                                                 declaration.owner[:-1], mapping))
        return result

    def _qualify_type(self, tokens, context: tuple[str, ...], mapping: dict[str, str]) -> str:
        tokens = lex_rust(tokens_text(tokens))
        result = []
        index = 0
        while index < len(tokens):
            token = tokens[index]
            if token.kind != "ident":
                result.append(token)
                index += 1
                continue
            if token.value in mapping:
                result.append(_SubstitutedToken(mapping[token.value]))
                index += 1
                continue
            end = index + 1
            path = [token.value]
            while end + 1 < len(tokens) and tokens[end].value == "::" and tokens[end + 1].kind == "ident":
                path.append(tokens[end + 1].value)
                end += 2
            resolved = None
            if path[0] == "crate":
                resolved = self._find_record(tuple(path[1:]))
            else:
                for depth in range(len(context), -1, -1):
                    candidate = context[:depth] + tuple(path)
                    imported = self.parsed.imports.get(candidate, candidate)
                    resolved = self._find_record(imported)
                    if resolved:
                        break
            if resolved:
                result.append(_SubstitutedToken("crate_shim::" + "::".join(resolved)))
            else:
                result.extend(tokens[index:end])
            index = end
        return tokens_text(result)

    def _method_return_spelling(self, declaration, args: list[str]) -> str:
        """Return type with the impl's generic parameters substituted.

        `swapped` on `Pair2<f64, i32>` returns `Pair2<B, A>` -- substituting
        the impl generics positionally gives `crate_shim::Pair2<i32, f64>`,
        which is the type whose *layout* the shim's signature then requires.
        """
        mapping = {generic: self._qualified_arg(spelling)
                   for generic, spelling in zip(declaration.generics, args)}
        return self._qualify_type(declaration.method.return_tokens,
                                 declaration.owner[:-1], mapping)

    def _arg_spelling(self, ref: dict[str, Any]) -> str:
        form, name = ref.get("form"), ref.get("name", "")
        if form == ap.TYPE_PRIMITIVE:
            if name not in PRIMITIVES:
                raise _Refused(ap.ERROR_REPRESENTATION_INCOMPATIBLE,
                               f"unknown primitive {name!r}")
            return name
        if form == ap.TYPE_RECORD:
            if self._find_record(self._normalize_path(name)) is None:
                raise _Refused(ap.ERROR_REPRESENTATION_INCOMPATIBLE,
                               f"record {name!r} does not exist in the producer crate")
            return "crate_shim::" + "::".join(self._normalize_path(name))
        raise _Refused(ap.ERROR_CAPABILITY_UNSUPPORTED,
                       f"type form {form!r} is outside the phase 1 request surface")

    def _check_query(self, query: ap.Query) -> ap.Resolution | None:
        """Static checks; returns a Resolution, or None (needs the producer)."""
        entity = query.entity
        if entity["kind"] not in ("generic", "function"):
            return ap.Resolution.refused(
                query, self.session, status=ap.STATUS_UNSUPPORTED,
                code=ap.ERROR_CAPABILITY_UNSUPPORTED,
                message=f"entity kind {entity['kind']!r} not supported")
        path = self._normalize_path(entity.get("path", ""))
        method = self._find_generic_method(path)
        if method is not None:
            return self._check_method_query(query, method)
        function = self._find_function(path)
        if function is None or not function.public:
            return ap.Resolution.refused(
                query, self.session, status=ap.STATUS_REJECTED,
                code=ap.ERROR_ENTITY_NOT_FOUND,
                message=f"no public function {'::'.join(path)!r} in the producer crate")
        if function.variadic:
            return ap.Resolution.refused(
                query, self.session, status=ap.STATUS_UNSUPPORTED,
                code=ap.ERROR_CAPABILITY_UNSUPPORTED,
                message="variadic functions are outside the request surface")
        try:
            spellings = [self._arg_spelling(arg) for arg in entity.get("args", [])]
        except _Refused as refused:
            return ap.Resolution.refused(
                query, self.session, status=ap.STATUS_REJECTED,
                code=refused.code, message=refused.message)
        if len(spellings) != len(function.params):
            return ap.Resolution.refused(
                query, self.session, status=ap.STATUS_REJECTED,
                code=ap.ERROR_CONSTRAINT_FAILED,
                message=f"arity mismatch: {function.name} takes "
                        f"{len(function.params)} argument(s), got {len(spellings)}")
        return None

    def _resolve_individually(self, queries: list[ap.Query],
                              checked: list[ap.Resolution | None],
                              rlib: Path) -> list[ap.Resolution]:
        results: list[ap.Resolution] = []
        for query, static in zip(queries, checked):
            if static is not None:
                results.append(static)
                continue
            probe = self._write_probe([query], [None], "dci_probe_one")
            try:
                self._compile_probe(probe, rlib)
            except _ProbeError as exc:
                results.append(ap.Resolution.refused(
                    query, self.session, status=ap.STATUS_REJECTED,
                    code=exc.code, message=exc.message, producer_span=exc.span))
            else:
                results.append(self._close_request(query))
        return results

    def _write_probe(self, queries: list[ap.Query],
                     static: list[ap.Resolution | None], module: str) -> Path:
        lines = ["#![allow(dead_code, unused_variables)]"]
        for index, (query, is_static) in enumerate(zip(queries, static)):
            if is_static is not None:
                continue  # already decided; keeps probe indices stable
            path = self._normalize_path(query.entity.get("path", ""))
            method = self._find_generic_method(path)
            if method is not None:
                # The probe is what makes rustc the adjudicator for an
                # instance the producer may never have laid out: it names the
                # concrete receiver (`crate_shim::Pair2<f64, i32>`) and the
                # UFCS call, and rustc either accepts the monomorphization or
                # reports the failed bound with a producer span.
                args = [ref.get("name", "") for ref in query.entity.get("args", [])]
                spellings = self._method_parameter_spellings(method, args)
                parameters = ", ".join(f"v{i}: {spelling}" for i, spelling in enumerate(spellings))
                arguments = ", ".join(f"v{i}" for i in range(len(spellings)))
                lines.append(
                    f"fn __{module}_{index}({parameters}) "
                    f"{{ let _ = {self._method_call(method, args, arguments)}; }}")
                continue
            try:
                spellings = [self._arg_spelling(arg)
                             for arg in query.entity.get("args", [])]
            except _Refused as refused:
                raise _ProbeError(refused.code, refused.message) from refused
            call_path = "crate_shim::" + "::".join(path)
            params = ", ".join(f"v{i}: {sp}" for i, sp in enumerate(spellings))
            arguments = ", ".join(f"v{i}" for i in range(len(spellings)))
            lines.append(
                f"fn __{module}_{index}({params}) "
                f"{{ let _ = {call_path}({arguments}); }}")
        probe = self.handle.work / f"{module}.rs"
        probe.write_text("\n".join(lines) + "\n", encoding="utf-8")
        return probe

    def _compile_probe(self, probe: Path, rlib: Path) -> None:
        rmeta = probe.with_suffix(".rmeta")
        command = [
            self.handle.rustc, "--edition", self.handle.edition,
            "--crate-type", "lib", "--emit=metadata",
            "--crate-name", "dci_probe",
            "--target", self.handle.triple,
            "--extern", f"crate_shim={rlib}",
            "-A", "warnings",
            str(probe), "-o", str(rmeta),
        ]
        command.extend(self._dependency_args)
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                text=True, encoding="utf-8", errors="replace",
                                timeout=180, check=False, env=self._environment)
        if result.returncode != 0:
            stderr = (result.stderr or result.stdout).strip()
            code, message = _classify_rustc(stderr)
            raise _ProbeError(code, message, span=_first_span(stderr))

    def _close_method_request(self, query: ap.Query,
                              declaration) -> ap.Resolution:
        """Close `Type<...>::member` from the impl, not from a free function."""
        args = [ref.get("name", "") for ref in query.entity.get("args", [])]
        spellings = self._method_parameter_spellings(declaration, args)
        borrows = self._method_borrows_receiver(declaration)
        return_spelling = self._method_return_spelling(declaration, args)
        symbol = _symbol_name(declaration.owner + (declaration.name,), args)
        facts = {
            "operation": "rust/call/1",
            "symbol": symbol,
            "abi": "c-shim",
            "unwind": "abort",
            # `path` is the *owner* here, so the shim can qualify the call as
            # `crate_shim::<owner>::<method>`; `method`/`owner_args` carry the
            # UFCS spelling the shim needs.
            "path": list(declaration.owner),
            "method": declaration.name,
            "owner_args": args,
            "receiver_borrow": borrows,
            "arguments": spellings,
            "parameters": [
                {"index": index, "type": spelling,
                 "ownership": "borrow_mut" if spelling.startswith("&mut ") else (
                     "borrow" if spelling.startswith("&") else "by-value")}
                for index, spelling in enumerate(spellings)
            ],
            "return_type": return_spelling,
        }
        result_form = (ap.TYPE_PRIMITIVE if return_spelling in PRIMITIVES
                       else ap.TYPE_NATIVE)
        return ap.Resolution.closed(
            query, self.session,
            result_types=[ap.type_ref(result_form, return_spelling)],
            facts=facts,
        )

    def _close_request(self, query: ap.Query) -> ap.Resolution:
        path = self._normalize_path(query.entity.get("path", ""))
        method = self._find_generic_method(path)
        if method is not None:
            return self._close_method_request(query, method)
        function = self._find_function(path)
        spellings = [self._arg_spelling(arg) for arg in query.entity.get("args", [])]
        symbol = _symbol_name(path, spellings)
        return_spelling = self._return_spelling(function, spellings)
        facts = {
            "operation": "rust/call/1",
            "symbol": symbol,
            "abi": "c-shim",
            "unwind": "abort",
            "path": list(path),
            "arguments": spellings,
            "parameters": [
                {"index": i, "type": spelling, "ownership": "by-value"}
                for i, spelling in enumerate(spellings)
            ],
            "return_type": return_spelling,
        }
        result_form = (ap.TYPE_PRIMITIVE if return_spelling in PRIMITIVES
                       else ap.TYPE_NATIVE)
        return ap.Resolution.closed(
            query, self.session,
            result_types=[ap.type_ref(result_form, return_spelling)],
            facts=facts,
        )

    def _return_spelling(self, function, spellings: list[str]) -> str:
        """Concrete spelling of the return type with generic params replaced.

        Only type parameters declared by this function may be substituted.
        Producer type constructors are never mistaken for type parameters.
        rustc checks the concrete call and the resulting shim signature.
        """
        mapping: dict[str, str] = {}
        generic_names = set(function.type_params)
        for param, spelling in zip(function.params, spellings):
            _infer_type_parameters(param.type_tokens, lex_rust(spelling.removeprefix("crate_shim::")),
                                   generic_names, mapping)
        return self._qualify_type(function.return_tokens, function.type_context, mapping)

    def _ensure_rlib(self) -> Path:
        if self._rlib is not None:
            return self._rlib
        # rustc only accepts extern rlibs named lib*.rlib
        rlib = self.handle.work / "libdci_original.rlib"
        run_checked([
            self.handle.rustc, "--edition", self.handle.edition,
            "--crate-type", "rlib", "--crate-name", self.handle.crate_name,
            "--target", self.handle.triple,
            str(self.handle.entry), "-o", str(rlib), *self._producer_args,
        ], env=self._environment, timeout=300)
        self._rlib = rlib
        return rlib

    def _shim_source(self, closed: list[ap.Resolution]) -> tuple[str, list[str]]:
        """Emit ``#[no_mangle] extern "C"`` shims from closed facts."""
        lines = ["#![allow(improper_ctypes_definitions, unused_variables)]"]
        symbols: list[str] = []
        for resolution in closed:
            facts = resolution.facts
            symbol = facts["symbol"]
            symbols.append(symbol)
            spellings: list[str] = facts["arguments"]
            params = ", ".join(f"v{i}: {sp}" for i, sp in enumerate(spellings))
            if facts.get("method"):
                # An instance method closes by UFCS on the concrete receiver,
                # so a request for an instance the producer never laid out
                # still gets a real monomorphization here.
                owner = "crate_shim::" + "::".join(facts["path"])
                turbofish = ("::<" + ", ".join(
                                self._qualified_arg(a)
                                for a in facts["owner_args"]) + ">"
                             if facts.get("owner_args") else "")
                call = f"{owner}{turbofish}::{facts['method']}"
                arguments = ", ".join(f"v{i}" for i in range(len(spellings)))
            else:
                call = "crate_shim::" + "::".join(facts["path"])
                arguments = ", ".join(f"v{i}" for i in range(len(spellings)))
            ret = facts["return_type"]
            if ret in ("()", ""):
                lines.append(f'#[unsafe(no_mangle)]\npub extern "C" fn {symbol}({params}) '
                             f"{{ {call}({arguments}); }}")
            else:
                lines.append(f'#[unsafe(no_mangle)]\npub extern "C" fn {symbol}({params}) '
                             f"-> {ret} "
                             f"{{ let __ret = {call}({arguments}); __ret }}")
        return "\n".join(lines) + "\n", symbols


class _SubstitutedToken:
    """Minimal stand-in with ``value``/``kind`` for token substitution."""

    def __init__(self, value: str):
        self.value = value
        self.kind = "ident"


def _infer_type_parameters(pattern, actual, generic_names: set[str],
                           mapping: dict[str, str]) -> None:
    """Unify declared type parameters with a producer-checked argument type.

    Matching is structural: a constructor such as UserResult<T> is a concrete
    path, while T is a wildcard only when the function actually declares T.
    This is source spelling substitution, never an ABI compatibility rule.
    """
    p = a = 0
    while p < len(pattern) and a < len(actual):
        token = pattern[p]
        if token.kind == "ident" and token.value in generic_names:
            start = a
            depth = 0
            while a < len(actual):
                value = actual[a].value
                if depth == 0 and value in {",", ">", ")", "]"}:
                    break
                if value in {"<", "(", "["}:
                    depth += 1
                elif value in {">", ")", "]"}:
                    depth -= 1
                a += 1
            if a == start:
                return
            spelling = tokens_text(actual[start:a])
            previous = mapping.get(token.value)
            if previous is not None and previous != spelling:
                raise RustActiveError(f"producer type parameter {token.value} has inconsistent arguments")
            mapping[token.value] = spelling
            p += 1
            continue
        if token.value != actual[a].value:
            return
        p += 1
        a += 1


# ---------------------------------------------------------------------------
# helpers


def _normalize_coff_timestamp(data: bytes, triple: str) -> bytes:
    """Zero the COFF TimeDateStamp so identical shims hash identically.

    Producer objects carry a wall-clock stamp; the bundle identity must bind
    shim content, not the minute the shim happened to be compiled.  Only the
    4 bytes at offset 4 of the COFF file header are touched.
    """
    if "windows" not in triple or len(data) < 8 or data[:2] not in (b"\x64\x86", b"\x4c\x01"):
        return data
    return data[:4] + b"\x00\x00\x00\x00" + data[8:]


def digest_file(path: Path, relative: Path, sink: dict[str, str]) -> None:
    data = path.read_bytes()
    sink[relative.as_posix()] = hashlib.sha256(data).hexdigest()


def _symbol_name(path: tuple[str, ...], spellings: list[str]) -> str:
    parts = ["__vyx_rust", *path]
    for spelling in spellings:
        parts.append(re.sub(r"[^A-Za-z0-9]", "_", spelling))
    return "_".join(parts)


def _classify_rustc(stderr: str) -> tuple[str, str]:
    for marker, code in _RUSTC_ERROR_CLASS:
        if marker in stderr:
            return code, stderr.splitlines()[0]
    return ap.ERROR_MATERIALIZATION_FAILED, (
        stderr.splitlines()[0] if stderr else "rustc failed")


def _first_span(stderr: str) -> str:
    match = _PRODUCER_SPAN.search(stderr)
    return match.group(1) if match else ""


def describe_capabilities() -> ap.AdapterCapabilities:
    return CAPABILITIES


def open_session(crate_entry: Path, **kwargs: Any) -> RustActiveSession:
    return RustActiveSession(crate_entry, **kwargs)


__all__ = [
    "ADAPTER_ID", "CAPABILITIES", "PRIMITIVES",
    "RustActiveError", "RustActiveSession", "RustSessionHandle",
    "describe_capabilities", "open_session",
]
