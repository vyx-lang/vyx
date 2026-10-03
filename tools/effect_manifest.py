"""Inspect and validate MOSP Effect manifests without a compiler process.

The compiler remains the authority for lowering.  This tool only checks the
data boundary: wire escaping, canonical row identities, dependency conflicts,
fingerprints, and dependency cycles.  It is intended for CI and artifact
review, not as a replacement for the AOT Effect gate.
"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

MODULUS = 2_147_483_647
VALID_ESCAPES = {"25": "%", "09": "\t", "0A": "\n", "0D": "\r"}
VALID_FLAGS = {0, 1, 2, 4}


class ManifestError(ValueError):
    pass


class DciExportError(ValueError):
    """Malformed or untrusted MOSP DCI export sidecar."""


@dataclass
class DciExport:
    """Data-only DCI export selected by a sealed EffectPlan.

    The sidecar deliberately carries the EffectPlan fingerprint instead of a
    second row hash.  The compiler is authoritative for that plan digest;
    this reader verifies the wire envelope and exposes the digest so a build
    orchestrator can bind generated bridge artifacts to the exact plan that
    authorized them.
    """

    unit: str
    plan_fingerprint: int
    header_wire: str
    records: list[str]
    record_fields: list[list[str]]

    def as_json(self) -> dict[str, Any]:
        return {
            "format": "MOSP-DCI-EXPORT",
            "version": 1,
            "unit": self.unit,
            "plan_fingerprint": self.plan_fingerprint,
            "record_count": len(self.records),
            "canonical": self.records == sorted(self.records),
            "records": [
                {
                    "subject": values[1],
                    "attribute": values[2],
                    "arguments": values[3],
                    "phase": values[4],
                    "scope": values[5],
                    "status": values[6],
                }
                for values in self.record_fields
            ],
        }


def unescape(value: str) -> str:
    out: list[str] = []
    i = 0
    while i < len(value):
        if value[i] != "%":
            out.append(value[i])
            i += 1
            continue
        if i + 2 >= len(value) or value[i + 1:i + 3] not in VALID_ESCAPES:
            raise ManifestError(f"invalid percent escape at byte {i}")
        out.append(VALID_ESCAPES[value[i + 1:i + 3]])
        i += 3
    return "".join(out)


def escape(value: str) -> str:
    return (value.replace("%", "%25")
            .replace("\t", "%09")
            .replace("\n", "%0A")
            .replace("\r", "%0D"))


def hash_text(value: str) -> int:
    result = 5381
    for char in value:
        result = (result * 33 + ord(char)) % MODULUS
    return result


def mix(seed: int, value: int) -> int:
    return (seed + value + 97) % MODULUS


def parse_i64(value: str, field: str) -> int:
    try:
        return int(value, 10)
    except ValueError as exc:
        raise ManifestError(f"{field}: expected decimal integer") from exc


def require_text(value: str, field: str) -> str:
    if not value.strip():
        raise ManifestError(f"{field}: must be non-empty")
    return value


def split_wire(line: str, expected: int, row: int) -> list[str]:
    fields = line.split("\t")
    if len(fields) != expected:
        raise ManifestError(f"row {row}: expected {expected} fields, got {len(fields)}")
    return fields


def edge_parts(key: str) -> tuple[str, str]:
    if not key.startswith("edge:"):
        raise ManifestError(f"invalid edge key {key!r}")
    body = key[5:]
    if body.count("->") != 1:
        raise ManifestError(f"invalid edge key {key!r}")
    source, target = body.split("->", 1)
    for label, value in (("source", source), ("target", target)):
        if not value or any(token in value for token in ("\x1f", "->", "|")):
            raise ManifestError(f"edge {label} contains a reserved separator: {value!r}")
    return source, target


@dataclass
class Manifest:
    header: list[str]
    header_wire: str
    records: list[str]
    dependencies: list[str]
    record_fields: list[list[str]]
    dependency_fields: list[list[str]]
    fingerprint: int
    dependency_fingerprint: int
    cycles: list[list[str]]

    @property
    def source_stamp(self) -> str:
        return self.header[5]

    @property
    def compiler_stamp(self) -> str:
        return self.header[6]

    def canonical_header(self) -> str:
        return "MOSP-EFFECT-MANIFEST\t" + "\t".join(self.header)

    def canonical_records(self) -> str:
        return "".join(row + "\n" for row in sorted(self.records))

    def canonical_dependencies(self) -> str:
        return "".join(row + "\n" for row in sorted(self.dependencies))

    def computed_dependency_fingerprint(self) -> int:
        result = 0
        for row in sorted(self.dependencies):
            result = mix(result, hash_text(row))
        return result

    def computed_fingerprint(self) -> int:
        body = self.canonical_records() + self.canonical_dependencies()
        return mix(hash_text(self.canonical_header() + "\n" + body),
                   self.computed_dependency_fingerprint())

    def as_json(self) -> dict[str, Any]:
        return {
            "version": int(self.header[0]),
            "target": self.header[1],
            "abi": self.header[2],
            "scope": self.header[3],
            "unit": self.header[4],
            "source_stamp": self.source_stamp,
            "compiler_stamp": self.compiler_stamp,
            "record_count": len(self.records),
            "dependency_count": len(self.dependencies),
            "fingerprint": self.fingerprint,
            "computed_fingerprint": self.computed_fingerprint(),
            "dependency_fingerprint": self.dependency_fingerprint,
            "computed_dependency_fingerprint": self.computed_dependency_fingerprint(),
            "canonical": self.records == sorted(self.records)
                         and self.dependencies == sorted(self.dependencies),
            "cycles": self.cycles,
        }


def find_cycles(edges: list[tuple[str, str]]) -> list[list[str]]:
    graph: dict[str, list[str]] = {}
    for source, target in edges:
        graph.setdefault(source, []).append(target)
        graph.setdefault(target, [])
    cycles: set[tuple[str, ...]] = set()

    def visit(node: str, path: list[str], active: set[str]) -> None:
        active.add(node)
        path.append(node)
        for target in sorted(graph.get(node, [])):
            if target in active:
                start = path.index(target)
                cycle = tuple(path[start:] + [target])
                # Rotate to a stable starting node for machine-readable output.
                body = cycle[:-1]
                pivot = min(range(len(body)), key=lambda index: body[index])
                rotated = body[pivot:] + body[:pivot] + (body[pivot],)
                cycles.add(rotated)
            else:
                visit(target, path, active)
        path.pop()
        active.remove(node)

    for node in sorted(graph):
        visit(node, [], set())
    return [list(cycle) for cycle in sorted(cycles)]


def parse_manifest(path: Path) -> Manifest:
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        raise ManifestError(f"cannot read manifest: {exc}") from exc
    if "\r" in text:
        raise ManifestError("manifest must use LF line endings")
    if not text or not text.endswith("\n"):
        raise ManifestError("manifest must end with LF")
    lines = text.splitlines()
    if not lines or not lines[0].startswith("H\t"):
        raise ManifestError("missing H header")
    header_wire = lines[0]
    raw_header = split_wire(header_wire, 9, 0)
    if raw_header[0] != "H":
        raise ManifestError("header tag must be H")
    header = [raw_header[1]] + [unescape(value) for value in raw_header[2:8]]
    version = parse_i64(header[0], "header.version")
    if version != 1:
        raise ManifestError(f"unsupported manifest version {version}")
    for index, value in enumerate(header[1:8], start=1):
        require_text(value, f"header.field[{index}]")
    fingerprint = parse_i64(raw_header[8], "header.fingerprint")
    records: list[str] = []
    dependencies: list[str] = []
    record_fields: list[list[str]] = []
    dependency_fields: list[list[str]] = []
    record_ids: dict[tuple[str, str, str], str] = {}
    dependency_ids: dict[str, str] = {}
    edges: list[tuple[str, str]] = []
    for row_index, line in enumerate(lines[1:], start=1):
        if not line:
            raise ManifestError(f"row {row_index}: blank rows are not allowed")
        if line.startswith("R\t"):
            fields = split_wire(line, 10, row_index)
            values = [fields[0]] + [unescape(value) for value in fields[1:]]
            for index, value in enumerate(values[1:8], start=1):
                require_text(value, f"row {row_index}.field[{index}]")
            flags = parse_i64(fields[8], f"row {row_index}.flags")
            if flags not in VALID_FLAGS:
                raise ManifestError(f"row {row_index}: unsupported flags {flags}")
            identity = (values[1], values[2], values[4], values[5],
                        values[6], values[7])
            previous = record_ids.get(identity)
            if previous is not None and previous != line:
                raise ManifestError(f"row {row_index}: conflicting record identity {identity!r}")
            if previous is not None:
                raise ManifestError(f"row {row_index}: duplicate record identity {identity!r}")
            record_ids[identity] = line
            records.append(line)
            record_fields.append(values)
            continue
        if line.startswith("D\t"):
            fields = split_wire(line, 4, row_index)
            values = [fields[0]] + [unescape(value) for value in fields[1:]]
            key = require_text(values[1], f"row {row_index}.key")
            value = require_text(values[2], f"row {row_index}.value")
            digest = require_text(values[3], f"row {row_index}.digest")
            previous = dependency_ids.get(key)
            if previous is not None and previous != line:
                raise ManifestError(f"row {row_index}: conflicting dependency key {key!r}")
            if previous is not None:
                raise ManifestError(f"row {row_index}: duplicate dependency key {key!r}")
            dependency_ids[key] = line
            if key.startswith("edge:"):
                source, target = edge_parts(key)
                require_text(value, f"row {row_index}.edge.kind")
                edges.append((source, target))
            dependencies.append(line)
            dependency_fields.append(values)
            continue
        raise ManifestError(f"row {row_index}: unknown record tag")
    cycles = find_cycles(edges)
    manifest = Manifest(header, header_wire, records, dependencies, record_fields,
                        dependency_fields, fingerprint, 0, cycles)
    manifest.dependency_fingerprint = manifest.computed_dependency_fingerprint()
    return manifest


def parse_dci_export(path: Path) -> DciExport:
    """Read and validate a compiler-selected ``MOSP-DCI-EXPORT`` sidecar.

    This is intentionally stricter than a convenience text parser.  The
    sidecar is a provenance boundary for generated bridges: malformed UTF-8,
    non-canonical rows, duplicate declarations, unknown escaping, or a status
    other than ``registered`` must fail before any artifact is consumed.
    """
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        raise DciExportError(f"cannot read DCI export: {exc}") from exc
    if text.startswith("\ufeff"):
        raise DciExportError("sidecar must not contain a UTF-8 BOM")
    if "\r" in text:
        raise DciExportError("sidecar must use LF line endings")
    if not text or not text.endswith("\n"):
        raise DciExportError("sidecar must end with LF")
    lines = text.splitlines()
    header = lines[0].split("\t")
    if len(header) != 5 or header[:3] != ["H", "MOSP-DCI-EXPORT", "1"]:
        raise DciExportError("malformed MOSP-DCI-EXPORT header")
    try:
        unit = unescape(header[3])
    except ManifestError as exc:
        raise DciExportError(f"header.unit: {exc}") from exc
    require_text(unit, "header.unit")
    try:
        plan_fingerprint = int(header[4], 10)
    except ValueError as exc:
        raise DciExportError("header.plan_fingerprint: expected decimal integer") from exc
    if not header[4].lstrip("-").isdigit():
        raise DciExportError("header.plan_fingerprint: expected decimal integer")
    records: list[str] = []
    record_fields: list[list[str]] = []
    identities: set[tuple[str, str, str, str, str]] = set()
    for row_index, line in enumerate(lines[1:], start=1):
        if not line:
            raise DciExportError(f"row {row_index}: blank rows are not allowed")
        fields = line.split("\t")
        if len(fields) != 7 or fields[0] != "R":
            raise DciExportError(f"row {row_index}: expected seven-field R row")
        try:
            values = [fields[0]] + [unescape(value) for value in fields[1:]]
        except ManifestError as exc:
            raise DciExportError(f"row {row_index}: {exc}") from exc
        for index, value in enumerate(values[1:], start=1):
            require_text(value, f"row {row_index}.field[{index}]")
        if values[2] != "dci_export":
            raise DciExportError(f"row {row_index}: unsupported attribute {values[2]!r}")
        if values[6] != "registered":
            raise DciExportError(
                f"row {row_index}: export row is not compiler-authorized ({values[6]!r})")
        identity = (values[1], values[2], values[4], values[5], values[6])
        if identity in identities:
            raise DciExportError(f"row {row_index}: duplicate export declaration {identity!r}")
        identities.add(identity)
        records.append(line)
        record_fields.append(values)
    if not records:
        raise DciExportError("sidecar contains no authorized export declarations")
    if records != sorted(records):
        raise DciExportError("sidecar records are not in canonical byte order")
    return DciExport(unit, plan_fingerprint, lines[0], records, record_fields)


def validate(manifest: Manifest, args: argparse.Namespace) -> list[str]:
    errors: list[str] = []
    if args.target and manifest.header[1] != args.target:
        errors.append("target-mismatch")
    if args.abi and manifest.header[2] != args.abi:
        errors.append("abi-mismatch")
    if args.scope and manifest.header[3] != args.scope:
        errors.append("scope-mismatch")
    if args.source and manifest.source_stamp != args.source:
        errors.append("source-stale")
    if args.compiler and manifest.compiler_stamp != args.compiler:
        errors.append("compiler-stale")
    if manifest.records != sorted(manifest.records):
        errors.append("records-not-canonical")
    if manifest.dependencies != sorted(manifest.dependencies):
        errors.append("dependencies-not-canonical")
    if manifest.fingerprint != manifest.computed_fingerprint():
        errors.append("fingerprint-mismatch")
    if manifest.dependency_fingerprint != manifest.computed_dependency_fingerprint():
        errors.append("dependency-fingerprint-mismatch")
    if manifest.cycles:
        errors.append("effect-cycle")
    return errors


def validate_dci_export_provenance(sidecar: DciExport,
                                   manifest: Manifest) -> list[str]:
    """Prove that the sidecar rows were selected from this sealed manifest."""
    errors = validate(manifest, argparse.Namespace(
        target="", abi="", scope="", source="", compiler=""))
    if errors:
        return ["manifest-" + error for error in errors]
    if sidecar.unit != manifest.header[4]:
        errors.append("manifest-unit-mismatch")
    plan_rows = [values for values in manifest.dependency_fields
                 if values[1] == "plan" and values[2] == "facts"]
    if len(plan_rows) != 1:
        errors.append("manifest-plan-dependency-missing")
    else:
        digest = plan_rows[0][3]
        if not digest.lstrip("-").isdigit():
            errors.append("manifest-plan-fingerprint-invalid")
        elif int(digest, 10) != sidecar.plan_fingerprint:
            errors.append("manifest-plan-fingerprint-mismatch")
    authorized = {
        (values[1], values[2], values[3], values[4], values[5])
        for values in manifest.record_fields
        if values[2] == "dci_export" and values[8] == "1"
    }
    selected = {(values[1], values[2], values[3], values[4], values[5])
                for values in sidecar.record_fields}
    if selected != authorized:
        errors.append("manifest-export-facts-mismatch")
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Inspect and validate MOSP Effect manifests")
    sub = parser.add_subparsers(dest="command", required=True)
    inspect = sub.add_parser("inspect", help="print manifest structure and fingerprints")
    inspect.add_argument("manifest", type=Path)
    inspect.add_argument("--json", action="store_true")
    check = sub.add_parser("validate", help="validate manifest structure and freshness")
    check.add_argument("manifest", type=Path)
    check.add_argument("--target", default="")
    check.add_argument("--abi", default="")
    check.add_argument("--scope", default="")
    check.add_argument("--source", default="")
    check.add_argument("--compiler", default="")
    dci_inspect = sub.add_parser("inspect-dci-export",
                                 help="inspect a compiler-selected DCI export sidecar")
    dci_inspect.add_argument("sidecar", type=Path)
    dci_inspect.add_argument("--json", action="store_true")
    dci_check = sub.add_parser("validate-dci-export",
                               help="validate DCI export provenance before consuming artifacts")
    dci_check.add_argument("sidecar", type=Path)
    dci_check.add_argument("--unit", default="")
    dci_check.add_argument("--fingerprint", default="")
    dci_check.add_argument("--manifest", type=Path,
                           help="same-build Effect manifest proving plan and selected fact provenance")
    args = parser.parse_args(argv)
    try:
        if args.command in {"inspect", "validate"}:
            manifest = parse_manifest(args.manifest)
            if args.command == "inspect":
                payload = manifest.as_json()
                if args.json:
                    print(json.dumps(payload, ensure_ascii=False, sort_keys=True, indent=2))
                else:
                    print(json.dumps(payload, ensure_ascii=False, sort_keys=True))
                return 0
            errors = validate(manifest, args)
            if errors:
                print("effect-manifest: REJECT " + ", ".join(errors), file=sys.stderr)
                return 1
            print("effect-manifest: PASS")
            return 0
        sidecar = parse_dci_export(args.sidecar)
        if args.command == "inspect-dci-export":
            payload = sidecar.as_json()
            if args.json:
                print(json.dumps(payload, ensure_ascii=False, sort_keys=True, indent=2))
            else:
                print(json.dumps(payload, ensure_ascii=False, sort_keys=True))
            return 0
        errors: list[str] = []
        if args.unit and sidecar.unit != args.unit:
            errors.append("unit-mismatch")
        if args.fingerprint:
            try:
                expected = int(args.fingerprint, 10)
            except ValueError:
                errors.append("fingerprint-argument-invalid")
            else:
                if sidecar.plan_fingerprint != expected:
                    errors.append("plan-fingerprint-mismatch")
        if args.manifest:
            errors.extend(validate_dci_export_provenance(
                sidecar, parse_manifest(args.manifest)))
        if errors:
            print("dci-export: REJECT " + ", ".join(errors), file=sys.stderr)
            return 1
        print("dci-export: PASS")
        return 0
    except ManifestError as exc:
        print(f"effect-manifest: MALFORMED: {exc}", file=sys.stderr)
        return 1
    except DciExportError as exc:
        print(f"dci-export: MALFORMED: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
