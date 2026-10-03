"""Merge DCI producer sidecars into the MOSP Effect manifest format."""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "tools" / "dci"))

import dcib  # noqa: E402
import dci_obligations  # noqa: E402


def main(argv: list[str]) -> int:
    if len(argv) != 4:
        print("usage: emit_dci_effect_manifest.py OUT MODULE_EXCEPTION.DCIB MODULE_LIFECYCLE.DCIB", file=sys.stderr)
        return 2
    out = Path(argv[1])
    paths = [Path(argv[2]), Path(argv[3])]
    pairs = [(str(path), dcib.decode(path.read_bytes())) for path in paths]
    if any(not isinstance(document, dict) for _, document in pairs):
        raise SystemExit("DCIB root must be an object")
    out.parent.mkdir(parents=True, exist_ok=True)
    # The Vyx wire parser treats LF as the record delimiter.  ``Path.write_text``
    # performs platform newline translation on Windows, which would leave a
    # trailing CR in every field and make an otherwise valid fingerprint look
    # malformed.  Write bytes so the manifest is identical on every host.
    out.write_bytes(
        dci_obligations.to_effect_manifest(
            [(path, document) for path, document in pairs if isinstance(document, dict)],
            source_stamp="+".join(path.name for path in paths),
        ).encode("utf-8")
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
