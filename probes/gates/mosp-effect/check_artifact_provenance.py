"""Verify provenance and corruption rejection using emitted Effect artifacts."""

from __future__ import annotations

import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO))

from tools.effect_manifest import (  # noqa: E402
    DciExportError,
    ManifestError,
    parse_dci_export,
    parse_manifest,
    validate_dci_export_provenance,
)


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: check_artifact_provenance.py SIDECAR MANIFEST", file=sys.stderr)
        return 2
    sidecar_path, manifest_path = map(Path, sys.argv[1:])
    sidecar = parse_dci_export(sidecar_path)
    manifest = parse_manifest(manifest_path)
    errors = validate_dci_export_provenance(sidecar, manifest)
    if errors:
        raise AssertionError(f"same-build provenance rejected: {errors}")
    text = sidecar_path.read_text(encoding="utf-8")
    lines = text.splitlines()
    with tempfile.TemporaryDirectory(prefix="vyx-effect-provenance-") as temporary:
        path = Path(temporary) / "mutated.dci-export"
        header = lines[0].split("\t")
        header[4] = str(int(header[4], 10) + 1)
        path.write_text("\t".join(header) + "\n" + "\n".join(lines[1:]) + "\n",
                        encoding="utf-8", newline="\n")
        errors = validate_dci_export_provenance(parse_dci_export(path), manifest)
        if "manifest-plan-fingerprint-mismatch" not in errors:
            raise AssertionError(f"changed EffectPlan fingerprint was accepted: {errors}")

        row = lines[1].split("\t")
        row[3] = row[3] + ",provenance_test=1"
        path.write_text(lines[0] + "\n" + "\t".join(row) + "\n", encoding="utf-8",
                        newline="\n")
        errors = validate_dci_export_provenance(parse_dci_export(path), manifest)
        if "manifest-export-facts-mismatch" not in errors:
            raise AssertionError(f"changed export arguments were accepted: {errors}")

        row[6] = "unregistered"
        path.write_text(lines[0] + "\n" + "\t".join(row) + "\n", encoding="utf-8",
                        newline="\n")
        try:
            parse_dci_export(path)
        except (DciExportError, ManifestError):
            pass
        else:
            raise AssertionError("unregistered export sidecar row was accepted")

        path.write_text(text + lines[1] + "\n", encoding="utf-8", newline="\n")
        try:
            parse_dci_export(path)
        except (DciExportError, ManifestError):
            pass
        else:
            raise AssertionError("duplicate sidecar declaration was accepted")

    print("effect-artifact-provenance: PASS same-build plan + facts; mutation rejected")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, DciExportError, ManifestError, OSError, ValueError) as exc:
        print(f"effect-artifact-provenance: FAIL {exc}", file=sys.stderr)
        raise SystemExit(1)
