"""CI coverage for the DCI vs Rust-FFI vs cxx benchmark harness.

These tests exercise the offline, deterministic part of the harness (the DCI
path, the ground-truth ABI probe and the fingerprint).  The Rust binding paths
need network access on first run to fetch the pinned cxx / bindgen crates, so
they are driven by ``dci_bench.py run`` rather than the CI self-check.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

import unittest

REPO_ROOT = Path(__file__).resolve().parents[3]
BENCH = REPO_ROOT / "tools" / "dci" / "bench" / "dci_bench.py"


def _llvm_root() -> str | None:
    root = os.environ.get("LLVM_ROOT")
    if root and Path(root).exists():
        return root
    for candidate in ("/usr/lib/llvm-22", "/usr/lib/llvm-21", "/usr/lib/llvm-20"):
        if Path(candidate).exists():
            return candidate
    return None


def _clangxx_available() -> bool:
    root = _llvm_root()
    if root and (Path(root) / "bin" / "clang++").exists():
        return True
    return shutil.which("clang++") is not None


def _bench_env() -> dict:
    env = dict(os.environ)
    root = _llvm_root()
    if root:
        env["LLVM_ROOT"] = root
    lib = str(REPO_ROOT / "bootstrap_compiler" / "out")
    existing = env.get("LD_LIBRARY_PATH", "")
    env["LD_LIBRARY_PATH"] = lib + (os.pathsep + existing if existing else "")
    return env


class DciBenchTests(unittest.TestCase):
    @unittest.skipUnless(_clangxx_available(), "clang++ / LLVM toolchain unavailable")
    def test_self_check_passes(self) -> None:
        proc = subprocess.run(
            [sys.executable, str(BENCH), "self-check"],
            capture_output=True, text=True, env=_bench_env(), timeout=600,
        )
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("self-check OK", proc.stdout)

    def test_fingerprint_is_valid_json(self) -> None:
        proc = subprocess.run(
            [sys.executable, str(BENCH), "fingerprint"],
            capture_output=True, text=True, env=_bench_env(), timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        document = json.loads(proc.stdout)
        self.assertEqual(document["seed"], 20260828)
        self.assertEqual(document["pinned_crates"]["cxx"], "1.0.129")
        self.assertEqual(document["pinned_crates"]["bindgen"], "0.70.1")

    def test_catalog_is_consistent(self) -> None:
        sys.path.insert(0, str(BENCH.parent))
        import dci_bench  # noqa: E402

        ids = [entity.id for entity in dci_bench.CATALOG]
        self.assertEqual(len(ids), len(set(ids)), "duplicate entity ids")
        self.assertTrue("point_sum" in ids and "vec2_dot" in ids and "buffer_value" in ids)


if __name__ == "__main__":
    unittest.main()
