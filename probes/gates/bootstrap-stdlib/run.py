"""Run the same Stage 0 standard-library preflight used by SDK release jobs."""
import argparse
import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "bootstrap_compiler/scripts/release"))
from build_native import bootstrap_manifest, verify_bootstrap_standard_library


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True, type=Path)
    parser.add_argument("--sdk", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    compiler, sdk, output = args.compiler.resolve(strict=True), args.sdk.resolve(strict=True), args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    manifest = output / "bootstrap.toml"
    original = (ROOT / "bootstrap_compiler/Vyx.toml").read_text(encoding="utf-8")
    manifest.write_text(bootstrap_manifest(original, sdk), encoding="utf-8")
    environment = dict(os.environ, VYX_STD_PACKAGES=str(sdk / "std_packages"), VYX_RUNTIME_ROOT=str(sdk / "lib"))
    verify_bootstrap_standard_library(compiler, sdk, output, environment, manifest)


if __name__ == "__main__":
    main()
