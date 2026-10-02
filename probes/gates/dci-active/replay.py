"""Offline replay driver for the DCI Active gate (MOSP §53 experiment 4).

This driver deliberately imports ONLY the Phase 0 artifact bundle loader —
never the active adapters and never the producer toolchain helpers.  The
semantic inputs (crate source, headers, rustc/clang++ sessions) are absent by
construction; the bundle's contract + object must be sufficient to link and
run the covered operation.

Steps:
 1. load_bundle() fully re-verifies every byte of the published bundle.
 2. Extracts the shim object referenced by the covered operation.
 3. Links a plain C driver against the object.
 4. Runs the resulting executable and compares its output with --expect.

Prints ``REPLAY=<stdout>`` on success; exits non-zero on any failure.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO))

from tools.dci import artifact_bundle as bundle  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", type=Path, required=True,
                        help="bundle cache root (the ONLY semantic-free input)")
    parser.add_argument("--bundle", required=True, help="published bundle key")
    parser.add_argument("--driver", type=Path, required=True,
                        help="C source file declaring and calling the symbol")
    parser.add_argument("--cc", required=True, help="C compiler used to link")
    parser.add_argument("--expect", required=True, help="expected stdout line")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()

    manifest = bundle.load_bundle(args.cache, args.bundle)
    covered = manifest["covered_operations"]
    if len(covered) < 1 or len(manifest["artifacts"]) != 1:
        print(f"REPLAY=bad_manifest covered={len(covered)} "
              f"artifacts={len(manifest['artifacts'])}")
        return 1
    artifact = bundle.bundle_path(
        args.cache, args.bundle,
        "artifacts/" + manifest["artifacts"][0]["path"])
    args.out.mkdir(parents=True, exist_ok=True)
    exe = args.out / "replay.exe"
    link = subprocess.run([args.cc, str(args.driver), str(artifact), "-o", str(exe)],
                          capture_output=True, text=True, timeout=120, check=False)
    if link.returncode != 0:
        print("REPLAY=link_failed")
        print(link.stderr.strip())
        return 1
    run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30,
                         check=False)
    if run.returncode != 0:
        print(f"REPLAY=run_failed rc={run.returncode}")
        print(run.stderr.strip())
        return 1
    stdout = run.stdout.strip()
    print(f"REPLAY={stdout}")
    if stdout != args.expect:
        print(f"REPLAY=mismatch expected={args.expect}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
