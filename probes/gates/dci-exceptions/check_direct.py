"""Exercise the compiler-side fail-closed DCI obligation import."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools" / "dci"))
import dcib  # noqa: E402


HERE = Path(__file__).resolve().parent


def main() -> int:
    if len(sys.argv) < 2:
        raise SystemExit("usage: check_direct.py <compiler> [compiler arguments]")
    compiler = Path(sys.argv[1]).resolve()
    extra = sys.argv[2:]
    document = dcib.decode((HERE / "module_exception.dcib").read_bytes())
    if not isinstance(document, dict):
        raise AssertionError("module_exception.dcib did not decode to an object")
    obligations = document.get("obligations")
    if not isinstance(obligations, dict):
        raise AssertionError("module_exception.dcib has no obligations object")
    obligations["edges"] = []
    invalid = HERE / "out" / "direct-dci-invalid.dcib"
    invalid.parent.mkdir(parents=True, exist_ok=True)
    invalid.write_bytes(dcib.encode(document))
    try:
        command = [
            str(compiler),
            "--src=file",
            str(HERE / "main.vyx"),
            "--dci",
            str(invalid),
            "--dci",
            str(HERE / "module_lifecycle.dcib"),
            "--dump-mir2",
            *extra,
        ]
        result = subprocess.run(command, cwd=ROOT, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if result.returncode == 0 or "EFFECT-DCI-CLEANUP" not in result.stdout:
            raise AssertionError(
                f"compiler accepted incomplete DCI graph: exit={result.returncode}\n"
                f"{result.stdout}"
            )
        print("dci-exceptions compiler import: PASS fail-closed cleanup")
        return 0
    finally:
        invalid.unlink(missing_ok=True)


if __name__ == "__main__":
    raise SystemExit(main())
