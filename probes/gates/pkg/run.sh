#!/usr/bin/env bash
# Kept as a compatibility gate entry; exercises JSON package sources and AOT.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
: "${VYX_TEST_COMPILER:?Set VYX_TEST_COMPILER to the compiler built from this checkout}"
: "${LLVM_ROOT:?Set LLVM_ROOT to the matching LLVM toolchain}"
PYTHON="${VYX_PYTHON:-python3}"
TRIPLET="${VYX_TEST_TRIPLET:-x86_64-unknown-linux-gnu}"
ABI="${VYX_TEST_ABI:-gnu}"
"$PYTHON" "$ROOT/probes/gates/package-sources/test_sources.py"
"$PYTHON" "$ROOT/probes/gates/package-sources/run.py" \
    --compiler "$VYX_TEST_COMPILER" --cxx "$LLVM_ROOT/bin/clang++" \
    --ar "$LLVM_ROOT/bin/llvm-ar" --triplet "$TRIPLET" --abi "$ABI"
