"""Active Adapter request protocol: typed queries, identities, manifests.

An Active Adapter receives :class:`Query` batches from the host build, asks the
producer compiler to close them, and reports back :class:`Resolution` records
plus :class:`BundleCandidate` artifacts.  Open generic requests live only in
this control plane; canonical ``.dcib`` contracts keep carrying closed facts
exclusively (DCI spec §2.4).

Identity model:

* ``entity_key``   — declared entity + arguments inside the producer env.
* ``request_key``  — entity + operation + request context (what is reusable).
* ``bundle_key``   — facts + implementation + dependencies (what is published).
* Session-numeric IDs are assigned by the compiler per build and never
  persist here.

Canonical bytes reuse :mod:`tools.dci.dcib` encoding, so map ordering is
deterministic and every document is by construction consumable by the
compiler's cJSON DOM.
"""

from __future__ import annotations

import hashlib
from dataclasses import dataclass, field
from typing import Any

from tools.dci import dcib

PROTOCOL = "dci-active/1"

#: Resolution.status
STATUS_CLOSED = "closed"
STATUS_REJECTED = "rejected"
STATUS_UNSUPPORTED = "unsupported"

#: Error taxonomy.  Semantic rejections may be cached; crashes,
#: cancellation and OOM (``resource_limit``) must NOT be cached as permanent.
ERROR_ENTITY_NOT_FOUND = "entity_not_found"
ERROR_CONSTRAINT_FAILED = "constraint_failed"
ERROR_REPRESENTATION_INCOMPATIBLE = "representation_incompatible"
ERROR_MISSING_FACTS = "missing_facts"
ERROR_CAPABILITY_UNSUPPORTED = "capability_unsupported"
ERROR_IDENTITY_CONFLICT = "identity_conflict"
ERROR_MATERIALIZATION_FAILED = "materialization_failed"
ERROR_DEPENDENCY_UNAVAILABLE = "dependency_unavailable"
ERROR_RESOURCE_LIMIT = "resource_limit"

#: Errors that are properties of the semantic environment and may be cached
#: inside the session validity domain.  Everything else is transient or
#: context-specific and must not be replayed as a permanent refusal.
CACHEABLE_ERRORS = frozenset({
    ERROR_ENTITY_NOT_FOUND,
    ERROR_CONSTRAINT_FAILED,
    ERROR_REPRESENTATION_INCOMPATIBLE,
    ERROR_IDENTITY_CONFLICT,
})

#: TypeRef forms.  ``open`` may appear only in queries (request control
#: plane); a closed resolution never emits an open reference.
TYPE_NATIVE = "native"
TYPE_PRIMITIVE = "primitive"
TYPE_RECORD = "record"
TYPE_OPEN = "open"


class ProtocolError(ValueError):
    """Malformed protocol document (client bug, not a semantic refusal)."""


# ---------------------------------------------------------------------------
# canonical documents and content identity


def canonical_bytes(document: Any) -> bytes:
    """Deterministic byte form of a protocol document.

    Values must stay inside the compiler's cJSON-exact domain; the DCIB
    encoder rejects everything else (NaN, huge ints, NUL strings) which keeps
    every identity computed here reproducible on the Vyx side as well.
    """
    dcib.validate_cjson_compatible(document)
    return dcib.encode(document)


def content_digest(document: Any) -> str:
    """sha256 over :func:`canonical_bytes`, hex-encoded."""
    return hashlib.sha256(canonical_bytes(document)).hexdigest()


def _require(mapping: Any, keys: set[str], where: str,
             optional: set[str] = frozenset()) -> None:
    if not isinstance(mapping, dict):
        raise ProtocolError(f"{where}: expected object")
    missing = keys - mapping.keys()
    if missing:
        raise ProtocolError(f"{where}: missing keys {sorted(missing)}")
    unknown = mapping.keys() - keys - optional - {"extensions"}
    if unknown:
        raise ProtocolError(f"{where}: unknown keys {sorted(unknown)}")


# ---------------------------------------------------------------------------
# capabilities / sessions


@dataclass(frozen=True)
class AdapterCapabilities:
    """Static description of what an adapter can be asked to do."""

    languages: tuple[str, ...]
    #: Versioned operation ids the adapter can close, e.g. ``rust/new/1``.
    operations: tuple[str, ...]
    #: Versioned request capabilities, e.g. ``rust/borrow/1``.
    request_capabilities: tuple[str, ...]
    profiles: tuple[str, ...] = ()

    def to_dict(self) -> dict[str, Any]:
        return {
            "protocol": PROTOCOL,
            "languages": list(self.languages),
            "operations": list(self.operations),
            "request_capabilities": list(self.request_capabilities),
            "profiles": list(self.profiles),
        }


@dataclass(frozen=True)
class SemanticEnvironment:
    """The fixed semantic environment a session is bound to.

    Any change to source contents, dependency contents, features/cfg/defines,
    toolchain or target invalidates the session: results from two different
    environments must never be spliced together.
    """

    language: str
    #: Producer identity, e.g. ``rustc 1.85.0`` or ``clang 22.1``.
    producer: str
    target_triple: str
    abi_family: str
    #: Feature/cfg/defines/edition/include resolution environment.
    environment: dict[str, Any] = field(default_factory=dict)
    #: path -> sha256 for source and dependency contents that participate.
    source_digests: dict[str, str] = field(default_factory=dict)
    #: path -> sha256 for generated inputs participating in resolution.
    generated_inputs: dict[str, str] = field(default_factory=dict)

    def to_dict(self) -> dict[str, Any]:
        return {
            "language": self.language,
            "producer": self.producer,
            "target_triple": self.target_triple,
            "abi_family": self.abi_family,
            "environment": self.environment,
            "source_digests": dict(sorted(self.source_digests.items())),
            "generated_inputs": dict(sorted(self.generated_inputs.items())),
        }

    @property
    def session_identity(self) -> str:
        """SessionIdentity: digest of the full environment manifest."""
        return content_digest(self.to_dict())


# ---------------------------------------------------------------------------
# queries


def type_ref(form: str, name: str = "", args: list[dict[str, Any]] | None = None) -> dict[str, Any]:
    """Build a TypeRef document.  ``open`` refs carry unresolved arguments."""
    if form not in (TYPE_NATIVE, TYPE_PRIMITIVE, TYPE_RECORD, TYPE_OPEN):
        raise ProtocolError(f"type_ref: unknown form {form!r}")
    ref: dict[str, Any] = {"form": form}
    if name:
        ref["name"] = name
    if args:
        ref["args"] = args
    return ref


@dataclass(frozen=True)
class Query:
    """One operation request against the producer environment.

    ``query_id`` is caller-assigned and correlates the Resolution; it never
    participates in the persistent ``request_key``.
    """

    query_id: str
    #: EntityRef: ``{"kind": "generic"|"function"|"record", "path": ...,
    #: "args": [TypeRef...]}``.  ``kind="open"`` marks an unresolved request.
    entity: dict[str, Any]
    #: OperationRef: versioned id plus explicit arguments.  Free-form
    #: expressions are deliberately not representable.
    operation: dict[str, Any]
    #: Request capabilities this query relies on, e.g. ``rust/borrow/1``.
    required_capabilities: tuple[str, ...] = ()

    def to_dict(self) -> dict[str, Any]:
        return {
            "query_id": self.query_id,
            "entity": self.entity,
            "operation": self.operation,
            "required_capabilities": list(self.required_capabilities),
        }

    def key_document(self, session: SemanticEnvironment) -> dict[str, Any]:
        """Persistent identity document: everything except ``query_id``."""
        return {
            "protocol": PROTOCOL,
            "session": session.session_identity,
            "entity": self.entity,
            "operation": self.operation,
            "required_capabilities": sorted(self.required_capabilities),
        }

    def request_key(self, session: SemanticEnvironment) -> str:
        """RequestKey: reusable identity of entity+operation+context."""
        return content_digest(self.key_document(session))

    def entity_key(self, session: SemanticEnvironment) -> str:
        """EntityKey: identity of the declared entity inside the session."""
        return content_digest({
            "protocol": PROTOCOL,
            "session": session.session_identity,
            "entity": self.entity,
        })

    def validate(self, capabilities: AdapterCapabilities | None = None) -> None:
        entity = self.entity
        _require(entity, {"kind"}, "Query.entity", optional={"path", "args"})
        kind = entity["kind"]
        if kind not in ("generic", "function", "record", "open"):
            raise ProtocolError(f"Query.entity.kind: unknown {kind!r}")
        if kind != "open" and not entity.get("path"):
            raise ProtocolError("Query.entity.path: required for closed entities")
        for arg in entity.get("args", []):
            _validate_type_ref(arg, "Query.entity.args")
        _require(self.operation, {"op"}, "Query.operation",
                 optional={"args", "target", "profile"})
        if not isinstance(self.operation["op"], str) or not self.operation["op"]:
            raise ProtocolError("Query.operation.op: must be a non-empty string")
        if capabilities is not None:
            if self.operation["op"] not in capabilities.operations:
                raise ProtocolError(
                    f"Query.operation.op: {self.operation['op']!r} not advertised")
            unsupported = set(self.required_capabilities) - set(
                capabilities.request_capabilities)
            if unsupported:
                raise ProtocolError(
                    f"Query.required_capabilities: unsupported {sorted(unsupported)}")


def _validate_type_ref(ref: Any, where: str) -> None:
    _require(ref, {"form"}, where, optional={"name", "args"})
    if ref["form"] not in (TYPE_NATIVE, TYPE_PRIMITIVE, TYPE_RECORD, TYPE_OPEN):
        raise ProtocolError(f"{where}.form: unknown {ref['form']!r}")
    if ref["form"] != TYPE_OPEN and not ref.get("name"):
        raise ProtocolError(f"{where}.name: required for closed type refs")
    for nested in ref.get("args", []):
        _validate_type_ref(nested, f"{where}.args")


def resolve_batch_request(session: SemanticEnvironment,
                          queries: list[Query]) -> dict[str, Any]:
    """Full ``resolve_batch`` request document (canonical, loggable)."""
    return {
        "protocol": PROTOCOL,
        "session": session.to_dict(),
        "queries": [query.to_dict() for query in queries],
    }


# ---------------------------------------------------------------------------
# resolutions


@dataclass(frozen=True)
class Diagnostic:
    code: str
    message: str
    #: Producer-side attribution, e.g. file:line:col from rustc/clang.
    producer_span: str = ""

    def to_dict(self) -> dict[str, Any]:
        document = {"code": self.code, "message": self.message}
        if self.producer_span:
            document["producer_span"] = self.producer_span
        return document


@dataclass(frozen=True)
class RequiredImplementation:
    """One linked artifact the closed operation depends on.

    ``artifact`` is a path relative to the bundle's ``artifacts/`` directory
    (e.g. ``"pair.obj"``); contracts use paths relative to ``contracts/``.
    """

    symbol: str
    artifact: str          # artifact path relative to the bundle artifacts/
    kind: str = "object"   # object | archive | bitcode | library

    def to_dict(self) -> dict[str, Any]:
        return {"symbol": self.symbol, "artifact": self.artifact, "kind": self.kind}


@dataclass(frozen=True)
class Resolution:
    """Outcome for one query: closed, rejected (with producer diagnostics)
    or unsupported (fact known, consumer/adapter capability missing)."""

    query_id: str
    request_key: str
    status: str
    instance_key: str = ""
    operation_key: str = ""
    result_types: tuple[dict[str, Any], ...] = ()
    #: Operation admission facts (layout, lowering, lifetime, ownership...).
    facts: dict[str, Any] = field(default_factory=dict)
    dependencies: tuple[str, ...] = ()           # further request_keys
    implementations: tuple[RequiredImplementation, ...] = ()
    diagnostics: tuple[Diagnostic, ...] = ()

    def to_dict(self) -> dict[str, Any]:
        document: dict[str, Any] = {
            "query_id": self.query_id,
            "request_key": self.request_key,
            "status": self.status,
        }
        if self.status == STATUS_CLOSED:
            document.update({
                "instance_key": self.instance_key,
                "operation_key": self.operation_key,
                "result_types": list(self.result_types),
                "facts": self.facts,
                "dependencies": list(self.dependencies),
                "implementations": [impl.to_dict() for impl in self.implementations],
            })
        else:
            document["diagnostics"] = [diag.to_dict() for diag in self.diagnostics]
        return document

    @staticmethod
    def closed(query: Query, session: SemanticEnvironment, *,
               result_types: list[dict[str, Any]],
               facts: dict[str, Any],
               implementations: list[RequiredImplementation] = (),
               dependencies: list[str] = ()) -> "Resolution":
        # Implementations may be empty when resolving a return type before
        # materializing its code. Publication enforces the non-empty requirement
        # in artifact_bundle.validate_candidate.
        operation = query.operation["op"]
        identity_context = {
            "request": query.request_key(session),
            "operation": operation,
            "facts": facts,
        }
        return Resolution(
            query_id=query.query_id,
            request_key=query.request_key(session),
            status=STATUS_CLOSED,
            instance_key=query.entity_key(session),
            operation_key=content_digest(identity_context),
            result_types=tuple(result_types),
            facts=facts,
            dependencies=tuple(sorted(dependencies)),
            implementations=tuple(implementations),
        )

    @staticmethod
    def refused(query: Query, session: SemanticEnvironment, *,
                status: str, code: str, message: str,
                producer_span: str = "") -> "Resolution":
        if status not in (STATUS_REJECTED, STATUS_UNSUPPORTED):
            raise ProtocolError(f"refused: bad status {status!r}")
        return Resolution(
            query_id=query.query_id,
            request_key=query.request_key(session),
            status=status,
            diagnostics=(Diagnostic(code, message, producer_span),),
        )


# ---------------------------------------------------------------------------
# artifact bundles


@dataclass(frozen=True)
class BundleCandidate:
    """Unverified bundle staged by an adapter, ready for validation."""

    session: SemanticEnvironment
    #: path (posix, relative) -> bytes for ``.dcib`` contracts.
    contracts: dict[str, bytes] = field(default_factory=dict)
    #: path -> (bytes, format) for implementation artifacts (objects etc).
    artifacts: dict[str, tuple[bytes, str]] = field(default_factory=dict)
    resolutions: list[Resolution] = field(default_factory=list)
    #: External runtime/native libraries this bundle requires at link time.
    link_requirements: list[dict[str, Any]] = field(default_factory=list)
    adapter: str = ""
    producer: str = ""

    def manifest(self) -> dict[str, Any]:
        """Deterministic bundle manifest; ``bundle.key`` binds all content."""
        covered = sorted(
            (resolution.to_dict() for resolution in self.resolutions
             if resolution.status == STATUS_CLOSED),
            key=lambda entry: entry["request_key"],
        )
        refusals = sorted(
            (resolution.to_dict() for resolution in self.resolutions
             if resolution.status != STATUS_CLOSED),
            key=lambda entry: entry["request_key"],
        )
        document: dict[str, Any] = {
            "protocol": PROTOCOL,
            "bundle": {"key": ""},
            "session": self.session.to_dict(),
            "contracts": _file_entries(self.contracts),
            "artifacts": _artifact_entries(self.artifacts),
            "covered_operations": covered,
            "refused_requests": refusals,
            "link_requirements": self.link_requirements,
            "provenance": {
                "adapter": self.adapter,
                "producer": self.producer,
            },
        }
        key = content_digest({**document, "bundle": {"key": ""}})
        document["bundle"]["key"] = key
        return document


def _file_entries(files: dict[str, bytes]) -> list[dict[str, Any]]:
    entries = [
        {
            "path": path,
            "sha256": hashlib.sha256(files[path]).hexdigest(),
            "bytes": len(files[path]),
        }
        for path in files
    ]
    return sorted(entries, key=lambda entry: entry["path"])


def _artifact_entries(artifacts: dict[str, tuple[bytes, str]]) -> list[dict[str, Any]]:
    entries = [
        {
            "path": path,
            "sha256": hashlib.sha256(artifacts[path][0]).hexdigest(),
            "bytes": len(artifacts[path][0]),
            "format": artifacts[path][1],
        }
        for path in artifacts
    ]
    return sorted(entries, key=lambda entry: entry["path"])
