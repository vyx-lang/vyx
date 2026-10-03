"""Exercise the DCI lifecycle/exception fact graph across two modules."""

from __future__ import annotations

import copy
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools" / "dci"))
import dcib  # noqa: E402
import dci_obligations  # noqa: E402


HERE = Path(__file__).resolve().parent


def read(name: str) -> dict:
    document = dcib.decode((HERE / name).read_bytes())
    if not isinstance(document, dict):
        raise AssertionError(f"{name} did not decode to an object")
    return document


def expect_failure(label: str, pairs: list[tuple[str, dict]], needle: str) -> None:
    errors = dci_obligations.validate_graph(pairs)
    if not any(needle in error for error in errors):
        raise AssertionError(f"{label}: expected {needle!r}, got {errors!r}")


def main() -> int:
    exception = read("module_exception.dcib")
    lifecycle = read("module_lifecycle.dcib")
    pairs = [("module_exception.dcib", exception), ("module_lifecycle.dcib", lifecycle)]
    errors = dci_obligations.validate_graph(pairs)
    if errors:
        raise AssertionError(f"cross-module graph should pass: {errors}")

    # A shared_abi exception cannot be accepted when its owner module drops the
    # cleanup edge.  This is the fail-closed consumer boundary.
    missing_edge = copy.deepcopy(exception)
    missing_edge["obligations"]["edges"] = []
    expect_failure(
        "missing cleanup edge",
        [("exception.dcib", missing_edge), ("lifecycle.dcib", lifecycle)],
        "cleanup-before-propagate",
    )

    # Duplicate declarations from two producers must agree byte-for-byte.
    conflict = copy.deepcopy(lifecycle)
    conflict["obligations"]["lifecycle"][0]["allocator_domain"] = "other.heap"
    expect_failure(
        "conflicting lifecycle declaration",
        pairs + [("conflict.dcib", conflict)],
        "conflicting declaration",
    )

    missing_symbol = copy.deepcopy(lifecycle)
    missing_symbol["obligations"]["lifecycle"][0]["required_operations"][0]["symbol"] = "missing_destroy"
    expect_failure(
        "missing lifecycle operation",
        [("exception.dcib", exception), ("lifecycle-missing-symbol.dcib", missing_symbol)],
        "unresolved exported symbol",
    )

    # Dependency edges are checked as a graph, including edges split across
    # producer modules.
    cycle_exception = copy.deepcopy(exception)
    cycle_exception["obligations"]["edges"].append({
        "from": "lifecycle:eh::Guard",
        "to": "exception:contract_throw",
        "kind": "requires",
    })
    cycle_lifecycle = copy.deepcopy(lifecycle)
    cycle_lifecycle["obligations"]["edges"].append({
        "from": "exception:contract_throw",
        "to": "lifecycle:eh::Guard",
        "kind": "requires",
    })
    expect_failure(
        "dependency cycle",
        [("exception-cycle.dcib", cycle_exception), ("lifecycle-cycle.dcib", cycle_lifecycle)],
        "dependency cycle",
    )

    print("dci-exceptions obligations: PASS cross-module merge, fail-closed cleanup, conflict, cycle")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
