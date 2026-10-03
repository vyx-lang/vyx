"""Cross-module lifecycle and exception obligations for DCI contracts.

DCI contracts traditionally carried lifecycle details below individual layouts
and exception details below ``control_flow``.  That is enough to lower one
module, but it leaves the consumer unable to prove that an exception edge in
one module releases objects owned by another module.  This module defines a
small, data-only fact graph that can be embedded in a ``.dci``/``.dcib``
document under the root ``obligations`` key.

The graph is deliberately independent of the compiler's Effect implementation:
producers can emit it from C++, Rust, or Zig adapters and any consumer can
validate it before lowering.  A producer opts into fail-closed checking with
``profile.obligation_mode = "required"``.  Contracts without that opt-in keep
the historical validation path for compatibility.
"""

from __future__ import annotations

from collections import defaultdict
from typing import Any, Iterable, Sequence

OBLIGATION_VERSION = "1"
_BOUNDARIES = {"no_unwind", "abort", "translated", "shared_abi"}
_EDGE_KINDS = {
    "cleanup-before-propagate",
    "propagates",
    "owns",
    "requires",
}

_EFFECT_MANIFEST_VERSION = "1"
_EFFECT_TARGET = "vyx"
_EFFECT_ABI = "vyx.aot.v1"
_EFFECT_SCOPE = "unit"
_EFFECT_COMPILER = "bootstrap.effect.v1"


def _text(value: Any) -> bool:
    return isinstance(value, str) and bool(value.strip())


def _required_mode(document: dict[str, Any]) -> bool:
    profile = document.get("profile")
    return isinstance(profile, dict) and profile.get("obligation_mode") == "required"


def _symbol_names(document: dict[str, Any]) -> set[str]:
    exports = document.get("exports")
    symbols = exports.get("symbols", []) if isinstance(exports, dict) else []
    result: set[str] = set()
    if isinstance(symbols, list):
        for symbol in symbols:
            if not isinstance(symbol, dict):
                continue
            for key in ("link_name", "mangled", "name"):
                value = symbol.get(key)
                if _text(value):
                    result.add(value)
    return result


def _node_id(kind: str, subject: str) -> str:
    return f"{kind}:{subject}"


def _iter_list(value: Any) -> Iterable[tuple[int, Any]]:
    if isinstance(value, list):
        yield from enumerate(value)


def validate_contract(
    document: dict[str, Any],
    path: str = "$",
    *,
    allow_external_nodes: bool = False,
    allow_external_symbols: bool = False,
) -> list[str]:
    """Validate one contract's obligation declarations.

    The function is intentionally strict once the producer opts in.  Missing
    or malformed fields are errors rather than silently falling back to the
    legacy layout/control-flow fields.
    """

    profile = document.get("profile")
    required = _required_mode(document)
    obligations = document.get("obligations")
    if obligations is None:
        if required:
            return [f"{path}.obligations: required by profile.obligation_mode"]
        return []
    if not isinstance(obligations, dict):
        return [f"{path}.obligations: must be an object"]

    errors: list[str] = []
    version = obligations.get("version")
    if version != OBLIGATION_VERSION:
        errors.append(
            f"{path}.obligations.version: unsupported version {version!r}; expected {OBLIGATION_VERSION!r}"
        )
    lifecycle = obligations.get("lifecycle", [])
    exceptions = obligations.get("exceptions", [])
    edges = obligations.get("edges", [])
    if not isinstance(lifecycle, list):
        errors.append(f"{path}.obligations.lifecycle: must be an array")
        lifecycle = []
    if not isinstance(exceptions, list):
        errors.append(f"{path}.obligations.exceptions: must be an array")
        exceptions = []
    if not isinstance(edges, list):
        errors.append(f"{path}.obligations.edges: must be an array")
        edges = []

    seen: set[str] = set()
    symbols = _symbol_names(document)
    for index, item in _iter_list(lifecycle):
        at = f"{path}.obligations.lifecycle[{index}]"
        if not isinstance(item, dict):
            errors.append(f"{at}: must be an object")
            continue
        subject = item.get("subject")
        if not _text(subject):
            errors.append(f"{at}.subject: required non-empty string")
            continue
        node = _node_id("lifecycle", subject)
        if node in seen:
            errors.append(f"{at}.subject: duplicate {subject!r}")
        seen.add(node)
        if not _text(item.get("ownership")):
            errors.append(f"{at}.ownership: required non-empty string")
        operations = item.get("required_operations")
        if not isinstance(operations, list) or not operations:
            errors.append(f"{at}.required_operations: must contain at least one operation")
        else:
            for op_index, operation in _iter_list(operations):
                op_at = f"{at}.required_operations[{op_index}]"
                if not isinstance(operation, dict):
                    errors.append(f"{op_at}: must be an object")
                    continue
                name = operation.get("name")
                symbol = operation.get("symbol")
                if not _text(name):
                    errors.append(f"{op_at}.name: required non-empty string")
                if not _text(symbol):
                    errors.append(f"{op_at}.symbol: required non-empty string")
                elif symbol not in symbols and not allow_external_symbols:
                    errors.append(f"{op_at}.symbol: unresolved exported symbol {symbol!r}")
                when = operation.get("when", "normal")
                if when not in {"normal", "unwind", "normal-and-unwind"}:
                    errors.append(f"{op_at}.when: unsupported value {when!r}")
        unwind = item.get("on_unwind")
        if unwind not in {"release", "retain", "forbid"}:
            errors.append(f"{at}.on_unwind: expected release, retain, or forbid")

    for index, item in _iter_list(exceptions):
        at = f"{path}.obligations.exceptions[{index}]"
        if not isinstance(item, dict):
            errors.append(f"{at}: must be an object")
            continue
        subject = item.get("subject")
        if not _text(subject):
            errors.append(f"{at}.subject: required non-empty string")
            continue
        node = _node_id("exception", subject)
        if node in seen:
            errors.append(f"{at}.subject: duplicate {subject!r}")
        seen.add(node)
        boundary = item.get("boundary")
        if boundary not in _BOUNDARIES:
            errors.append(f"{at}.boundary: unsupported value {boundary!r}")
        if boundary == "shared_abi" and not _text(item.get("abi")):
            errors.append(f"{at}.abi: required for shared_abi exception propagation")
        if boundary == "translated" and not _text(item.get("translator_symbol")):
            errors.append(f"{at}.translator_symbol: required for translated propagation")
        cleanup = item.get("cleanup")
        if cleanup not in {"unwind", "translate", "terminate", "none"}:
            errors.append(f"{at}.cleanup: expected unwind, translate, terminate, or none")
        if boundary == "shared_abi" and cleanup != "unwind":
            errors.append(f"{at}.cleanup: shared_abi exceptions must clean up during unwind")

    nodes = set(seen)
    edge_pairs: list[tuple[str, str, str, str]] = []
    for index, item in _iter_list(edges):
        at = f"{path}.obligations.edges[{index}]"
        if not isinstance(item, dict):
            errors.append(f"{at}: must be an object")
            continue
        source = item.get("from")
        target = item.get("to")
        kind = item.get("kind")
        if not _text(source) or not _text(target):
            errors.append(f"{at}.from/to: required non-empty node ids")
            continue
        if source not in nodes and not allow_external_nodes:
            errors.append(f"{at}.from: unresolved obligation node {source!r}")
        if target not in nodes and not allow_external_nodes:
            errors.append(f"{at}.to: unresolved obligation node {target!r}")
        if kind not in _EDGE_KINDS:
            errors.append(f"{at}.kind: unsupported value {kind!r}")
        edge_pairs.append((source, target, kind if isinstance(kind, str) else "", at))
        if kind == "cleanup-before-propagate" and not source.startswith("exception:"):
            errors.append(f"{at}.from: cleanup-before-propagate must start at an exception node")
        if kind == "cleanup-before-propagate" and not target.startswith("lifecycle:"):
            errors.append(f"{at}.to: cleanup-before-propagate must target a lifecycle node")

    errors.extend(_cycle_errors(edge_pairs))
    return errors


def _cycle_errors(edges: list[tuple[str, str, str, str]]) -> list[str]:
    """Reject dependency cycles in the cross-module obligation graph."""

    graph: dict[str, list[tuple[str, str]]] = defaultdict(list)
    for source, target, kind, _ in edges:
        if kind in {"requires", "propagates", "owns"}:
            graph[source].append((target, kind))
    state: dict[str, int] = {}
    stack: list[str] = []
    errors: list[str] = []

    def visit(node: str) -> None:
        state[node] = 1
        stack.append(node)
        for target, kind in graph.get(node, []):
            if state.get(target, 0) == 0:
                visit(target)
            elif state.get(target) == 1:
                start = stack.index(target)
                cycle = " -> ".join(stack[start:] + [target])
                errors.append(
                    f"$.obligations.edges: dependency cycle ({kind}): {cycle}"
                )
        stack.pop()
        state[node] = 2

    for node in graph:
        if state.get(node, 0) == 0:
            visit(node)
    return errors


def validate_graph(
    documents: Iterable[tuple[str, dict[str, Any]]],
    *,
    check_local: bool = True,
) -> list[str]:
    """Validate obligations spanning multiple producer contracts.

    ``documents`` contains ``(display_path, document)`` pairs.  Subjects are
    globally keyed by their ``kind:subject`` node id.  Duplicate declarations
    are accepted only when their normalized JSON payload is identical; this
    allows a consumer to merge C++ and Rust views while rejecting drift.
    """

    pairs = list(documents)
    errors: list[str] = []
    declarations: dict[str, tuple[str, dict[str, Any]]] = {}
    all_edges: list[tuple[str, str, str, str]] = []
    all_symbols: set[str] = set()

    import json

    for path, document in pairs:
        if check_local:
            errors.extend(validate_contract(
                document,
                path,
                allow_external_nodes=True,
                allow_external_symbols=True,
            ))
        all_symbols.update(_symbol_names(document))
        obligations = document.get("obligations")
        if not isinstance(obligations, dict):
            if _required_mode(document):
                errors.append(f"{path}.obligations: required by profile.obligation_mode")
            continue
        for kind in ("lifecycle", "exceptions"):
            values = obligations.get(kind, [])
            if not isinstance(values, list):
                continue
            for index, item in enumerate(values):
                if not isinstance(item, dict) or not _text(item.get("subject")):
                    continue
                node = _node_id("lifecycle" if kind == "lifecycle" else "exception", item["subject"])
                normalized = json.dumps(item, sort_keys=True, ensure_ascii=False)
                previous = declarations.get(node)
                if previous is not None and json.dumps(previous[1], sort_keys=True, ensure_ascii=False) != normalized:
                    errors.append(
                        f"{path}.obligations.{kind}[{index}].subject: conflicting declaration for {node!r}; "
                        f"previously declared by {previous[0]}"
                    )
                else:
                    declarations.setdefault(node, (path, item))
        edges = obligations.get("edges", [])
        if isinstance(edges, list):
            for index, item in enumerate(edges):
                if isinstance(item, dict) and _text(item.get("from")) and _text(item.get("to")):
                    all_edges.append((item["from"], item["to"], item.get("kind", ""), f"{path}.obligations.edges[{index}]"))

    nodes = set(declarations)
    for node, (path, item) in declarations.items():
        if not node.startswith("lifecycle:"):
            continue
        operations = item.get("required_operations", [])
        if not isinstance(operations, list):
            continue
        for index, operation in enumerate(operations):
            if not isinstance(operation, dict):
                continue
            symbol = operation.get("symbol")
            if _text(symbol) and symbol not in all_symbols:
                errors.append(
                    f"{path}.obligations.lifecycle: unresolved exported symbol {symbol!r} "
                    f"for {node} operation[{index}]"
                )
    for source, target, kind, at in all_edges:
        if source not in nodes:
            errors.append(f"{at}.from: unresolved cross-module obligation node {source!r}")
        if target not in nodes:
            errors.append(f"{at}.to: unresolved cross-module obligation node {target!r}")

    errors.extend(_cycle_errors(all_edges))
    for node, (path, item) in declarations.items():
        if not node.startswith("exception:") or item.get("boundary") != "shared_abi":
            continue
        cleanup_edges = [
            edge for edge in all_edges
            if edge[0] == node and edge[2] == "cleanup-before-propagate"
        ]
        if not cleanup_edges:
            errors.append(
                f"{path}.obligations.exceptions: shared_abi node {node!r} has no cross-module cleanup-before-propagate edge"
            )
    return errors


# ---------------------------------------------------------------------------
# MOSP Effect manifest bridge
# ---------------------------------------------------------------------------

def _effect_escape(value: str) -> str:
    """Encode one field using ``bootstrap.effect_manifest``'s wire rules."""
    return (value.replace("%", "%25")
            .replace("\t", "%09")
            .replace("\n", "%0A")
            .replace("\r", "%0D"))


def _effect_hash(value: str) -> int:
    modulus = 2_147_483_647
    result = 5381
    for char in value:
        result = (result * 33 + ord(char)) % modulus
    return result if result >= 0 else result + modulus


def _effect_mix(seed: int, value: int) -> int:
    modulus = 2_147_483_647
    # Keep this in lockstep with bootstrap_compiler/src/core/effect_manifest.vyx.
    # The manifest accumulator is intentionally additive so a parsed envelope
    # can rebuild the same fingerprint from its rows.
    result = (seed + value + 97) % modulus
    return result if result >= 0 else result + modulus


def _effect_json(value: Any) -> str:
    import json
    return json.dumps(value, sort_keys=True, ensure_ascii=False, separators=(",", ":"))


def to_effect_manifest(
    documents: Sequence[tuple[str, dict[str, Any]]],
    *,
    unit: str = "dci-obligations",
    source_stamp: str | None = None,
    compiler_stamp: str = _EFFECT_COMPILER,
) -> str:
    """Encode DCI obligations as ordinary MOSP Effect facts.

    This is the bridge between the DCI producer graph and the compiler's
    ``VYX_EFFECT_MANIFEST_IN`` boundary.  The output deliberately uses only
    data rows: ``dci.lifecycle`` / ``dci.exception`` are compiler-owned
    record handlers and ``edge:`` dependencies are consumed by the Effect
    graph.  Multiple producer modules are merged before the envelope is
    sealed, so the compiler sees one graph and one fingerprint.
    """
    pairs = list(documents)
    errors = validate_graph(pairs)
    if errors:
        raise ValueError("cannot publish invalid DCI obligations: " + "; ".join(errors))
    if not source_stamp:
        source_stamp = "+".join(path for path, _ in pairs) or "dci-obligations@unit"

    records: list[str] = []
    dependencies: list[str] = []
    seen_records: set[str] = set()
    seen_edges: set[str] = set()
    for _path, document in pairs:
        obligations = document.get("obligations")
        if not isinstance(obligations, dict):
            continue
        for kind, attribute in (("lifecycle", "dci.lifecycle"),
                                ("exceptions", "dci.exception")):
            values = obligations.get(kind, [])
            if not isinstance(values, list):
                continue
            for item in values:
                if not isinstance(item, dict) or not _text(item.get("subject")):
                    continue
                # Fact identities use singular node kinds.  Keep them aligned
                # with obligation edge endpoints (exception:/lifecycle:) so
                # the compiler can prove that every edge has a real fact.
                fact_kind = "exception" if kind == "exceptions" else "lifecycle"
                subject = f"dci:{fact_kind}:{item['subject']}"
                row = ("R\t" + _effect_escape(subject) + "\t"
                       + _effect_escape(attribute) + "\t"
                       + _effect_escape(_effect_json(item)) + "\tabi\tmodule\t"
                       + _EFFECT_TARGET + "\t" + _EFFECT_ABI + "\t1\t")
                if row not in seen_records:
                    records.append(row)
                    seen_records.add(row)
        edges = obligations.get("edges", [])
        if isinstance(edges, list):
            for edge in edges:
                if not isinstance(edge, dict) or not _text(edge.get("from")) or not _text(edge.get("to")):
                    continue
                source = _effect_node_id(edge["from"])
                target = _effect_node_id(edge["to"])
                kind = edge.get("kind", "")
                key = f"edge:{source}->{target}"
                row = ("D\t" + _effect_escape(key) + "\t"
                       + _effect_escape(str(kind)) + "\t"
                       + _effect_escape(_effect_hash(_effect_json(edge)).__str__()))
                if key not in seen_edges:
                    dependencies.append(row)
                    seen_edges.add(key)

    # The Vyx codec hashes decoded header values but serializes escaped
    # fields.  Keep both spellings explicit so a path containing `%`, tab, or
    # newline cannot produce a manifest that only looks valid on the wire.
    header_values = (_EFFECT_MANIFEST_VERSION, _EFFECT_TARGET, _EFFECT_ABI,
                     _EFFECT_SCOPE, unit, source_stamp, compiler_stamp)
    header = "H\t" + "\t".join(_effect_escape(value) for value in header_values)
    canonical_header = "MOSP-EFFECT-MANIFEST\t" + "\t".join(header_values)
    records_text = "".join(row + "\n" for row in records)
    dependencies_text = "".join(row + "\n" for row in dependencies)
    envelope = canonical_header + "\n" + records_text + dependencies_text
    dependency_fingerprint = 0
    for row in dependencies:
        dependency_fingerprint = _effect_mix(dependency_fingerprint, _effect_hash(row))
    fingerprint = _effect_mix(_effect_hash(envelope), dependency_fingerprint)
    return header + "\t" + str(fingerprint) + "\n" + records_text + dependencies_text


def _effect_node_id(node: str) -> str:
    """Map DCI graph node ids onto the corresponding Effect fact subject."""
    if node.startswith("dci:exception:") or node.startswith("dci:lifecycle:"):
        return node
    if node.startswith("exception:") or node.startswith("lifecycle:"):
        return "dci:" + node
    return node
