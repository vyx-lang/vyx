#!/usr/bin/env python3
"""Merge declared instance layouts into an adapter-produced contract.

The adapter *measures* the layout of every concrete template instance the
provider header explicitly instantiates (`discover_explicit_instantiation_names`)
and keeps those records in the contract, so an instance-typed value (such as the
return of `swapped() -> Pair2<B, A>`) no longer arrives without a layout.  Two
things still need a project statement:

* the *spelling*.  The adapter derives instance names the provider's way
  (``Pair2<double, int>``, or rust's ``lib.Pair2<f64, i32>`` with a crate
  prefix), while Vyx looks an instance up by its own spelling
  (``Pair2<f64,i32>``).  ``provider_spelling`` is exactly that mapping, and this
  tool re-spells both the layout ``type_name`` and the ``owner``/``name`` of
  derived symbols.  The rewrite is name-for-name: it never invents a symbol or a
  layout, and a name that denotes no declared instance is left exactly as the
  adapter derived it.
* an instance layout the provider never instantiates, if a project wants to
  state one.  A statement is a claim about a real artifact, so it is compared
  against the measurement and a disagreement fails; a gate should additionally
  verify it with the producer's own compiler (size/alignment/field offsets)
  rather than trust the file.

    python tools/dci/merge_instance_layouts.py <contract.dcib> <layouts.json>

`<layouts.json>` is `{"layouts": [...]}` in the contract's own layout shape, with
`type_name` in the *consumer's* spelling -- that is the key the layout lookup
matches on, and the provider's own spelling does not match it.  Re-spelling runs
*before* the statements are merged, so a declaration the adapter already
measured is cross-checked against it instead of being added a second time under
a second spelling.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from tools.dci import dcib  # noqa: E402

#: Keys that belong to the declaration file, not to the contract schema.
#: `provider_spelling` exists so the layout probe can name the instance the way
#: the producer does; a contract carrying it would be schema-invalid.
DECLARATION_ONLY_KEYS = ("provider_spelling",)


def canonical_instance_name(raw: str) -> str:
    """Fold the whitespace variants of a template instance name.

    ``Pair2<double, int>`` and ``Pair2<double,int>`` must be the same key: the
    declaration file is written by hand while the adapter's owner comes from the
    AST, and neither side may be reformatted into the other by eye.
    """
    text = " ".join(str(raw).split())
    text = re.sub(r"\s*<\s*", "<", text)
    text = re.sub(r"\s*>\s*", ">", text)
    text = re.sub(r"\s*,\s*", ",", text)
    return text


def instance_owner_aliases(declarations: dict) -> dict[str, str]:
    """Map ``provider spelling -> consumer spelling`` for every declared instance."""
    aliases: dict[str, str] = {}
    for declared in declarations.get("layouts", []):
        consumer = declared.get("type_name")
        spellings = declared.get("provider_spelling")
        if not consumer or not isinstance(spellings, dict):
            continue
        for spelling in spellings.values():
            if isinstance(spelling, str) and spelling:
                aliases[canonical_instance_name(spelling)] = consumer
    return aliases


def respell_name(value: str, aliases: dict[str, str]) -> str | None:
    """Re-spell a DCI name that denotes a declared instance.

    Handles both a bare type name (``owner``, ``abi.receiver.type.name``) and a
    qualified one (``Pair2<double,int>::get_first``), because the adapter writes
    the specialization's spelling into all three.  Returns ``None`` when the
    name does not denote a declared instance -- an unknown name is left exactly
    as the adapter derived it.
    """
    canonical = canonical_instance_name(value)
    if canonical in aliases:
        return aliases[canonical]
    for provider, consumer in aliases.items():
        prefix = provider + "::"
        if canonical.startswith(prefix):
            return consumer + canonical[len(provider):]
    return None


def respell_instance_names(node: Any, aliases: dict[str, str], applied: set[str]) -> None:
    """Walk a contract fragment and re-spell every instance *name* it carries.

    Only the ``name``/``owner`` keys are touched.  ``link_name``/``mangled`` and
    ``cpp_type`` are the provider's own provenance and must stay verbatim -- the
    linker matches on them, and a re-spelled mangled name would be a fiction.
    """
    if isinstance(node, list):
        for item in node:
            respell_instance_names(item, aliases, applied)
        return
    if not isinstance(node, dict):
        return
    for key, value in list(node.items()):
        if isinstance(value, str) and key in ("name", "owner"):
            replaced = respell_name(value, aliases)
            if replaced is not None and replaced != value:
                node[key] = replaced
                applied.add(f"{value} -> {replaced}")
        elif isinstance(value, (dict, list)):
            respell_instance_names(value, aliases, applied)


def rewrite_instance_symbol_names(document: dict, declarations: dict) -> list[str]:
    """Re-spell derived instance names to the consumer's spelling.

    Returns the ``provider -> consumer`` pairs that were actually applied, so a
    gate can assert the re-spelling happened instead of trusting silence.
    """
    aliases = instance_owner_aliases(declarations)
    if not aliases:
        return []
    applied: set[str] = set()
    respell_instance_names(document.setdefault("exports", {}).get("symbols", []),
                           aliases, applied)
    return sorted(applied)


def rewrite_instance_layout_names(layouts: list, declarations: dict) -> list[str]:
    """Re-spell adapter-measured instance layout names to the consumer's spelling.

    The adapter measures a specialization under the provider's spelling
    (``Pair2<double, int>``).  The consumer looks a layout up by *its own*
    spelling, so an un-rewritten entry is one the consumer can never find -- and
    it would also make the declaration below look absent, adding a second entry
    for the same type.  Only ``type_name`` is touched: the numbers stay exactly
    as the adapter measured them.
    """
    aliases = instance_owner_aliases(declarations)
    if not aliases:
        return []
    applied: set[str] = set()
    for layout in layouts:
        name = layout.get("type_name")
        if not isinstance(name, str) or not name:
            continue
        replaced = respell_name(name, aliases)
        if replaced is not None and replaced != name:
            layout["type_name"] = replaced
            applied.add(f"{name} -> {replaced}")
    return sorted(applied)


def merge(contract: Path, declarations: dict) -> str:
    document = dcib.decode(contract.read_bytes())
    layouts = document.setdefault("exports", {}).setdefault("layouts", [])
    # Re-spell first: the statements below are keyed by the consumer's spelling,
    # so an adapter-measured instance must already carry that spelling or it
    # would be *added* a second time instead of cross-checked.
    layout_respelled = rewrite_instance_layout_names(layouts, declarations)
    existing = {layout.get("type_name"): layout for layout in layouts}

    added: list[str] = []
    verified = 0
    for declared in declarations.get("layouts", []):
        name = declared.get("type_name")
        if not name:
            raise RuntimeError("an instance layout needs a type_name")
        wanted = {key: value for key, value in declared.items()
                  if key not in DECLARATION_ONLY_KEYS}
        current = existing.get(name)
        if current is None:
            layouts.append(wanted)
            existing[name] = wanted
            added.append(name)
            continue
        for key in ("size", "alignment"):
            if wanted.get(key) != current.get(key):
                raise RuntimeError(
                    f"{name}: declared {key}={wanted.get(key)} but the adapter "
                    f"measured {current.get(key)}")
        declared_fields = [(f.get("name"), f.get("offset"))
                           for f in wanted.get("fields") or []]
        measured_fields = [(f.get("name"), f.get("offset"))
                           for f in current.get("fields") or []]
        if declared_fields != measured_fields:
            raise RuntimeError(
                f"{name}: declared fields {declared_fields} but the adapter "
                f"measured {measured_fields}")
        verified += 1

    missing = [layout.get("type_name") for layout in layouts
               if not layout.get("type_name")]
    if missing:
        raise RuntimeError(f"contract has layouts without a type_name: {missing}")

    respelled = rewrite_instance_symbol_names(document, declarations)

    contract.write_bytes(dcib.encode(document))
    return (f"instance layouts merged into {contract.name}: "
            f"added {len(added)} {added}, adapter already derived {verified}, "
            f"re-spelled {len(respelled)} instance symbol name(s) {respelled}, "
            f"re-spelled {len(layout_respelled)} instance layout name(s) "
            f"{layout_respelled}")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        prog="merge-instance-layouts",
        description=(
            "Merge declared instance layouts into an adapter-produced DCIB "
            "contract.  Adapter-measured layouts are cross-checked (size, "
            "alignment, field offsets); declarations the adapter never "
            "measured are added; instance symbol and layout names are "
            "re-spelled from the provider's spelling to the consumer's "
            "spelling via the declared provider_spelling mapping."
        ),
    )
    parser.add_argument(
        "contract",
        type=Path,
        help="adapter-produced .dcib contract; modified in place",
    )
    parser.add_argument(
        "declarations",
        type=Path,
        help='JSON file {"layouts": [...], "provider_spelling": {...}} with '
        "type_name in the consumer's spelling",
    )
    args = parser.parse_args(argv)
    declarations = json.loads(args.declarations.read_text(encoding="utf-8"))
    print(merge(args.contract, declarations))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
