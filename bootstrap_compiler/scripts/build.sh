#!/usr/bin/env bash
set -euo pipefail

compiler_project="$(cd "$(dirname "$0")/.." && pwd)"
compiler="${VYX_BOOTSTRAP_VYXC:-$(command -v vyxc)}"
compiler_target="${1:-boot}"
compiler_jobs="${2:-1}"
compiler="$(command -v "$compiler")"
if [[ "$compiler" != /* ]]; then
    compiler="$(cd "$(dirname "$compiler")" && pwd)/$(basename "$compiler")"
fi
export LLVM_ROOT="${LLVM_ROOT:-/usr/lib/llvm-22}"
printf 'Stage 0: %s\n' "$compiler"
"$compiler" --version
sha256sum "$compiler"
cd "$compiler_project"
exec "$compiler" build --target "$compiler_target" "-j$compiler_jobs"
