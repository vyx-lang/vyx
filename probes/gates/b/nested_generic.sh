#!/usr/bin/env bash
# WP-B: nested generic inference (Wrap<Wrap<i32>>, Pair<i32, Wrap<i32>>).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
export LLVM_ROOT="${LLVM_ROOT:-/usr/lib/llvm-22}"
export PATH="$LLVM_ROOT/bin:$PATH"
if [ -f /etc/profile.d/vyx-llvm22.sh ]; then
  # shellcheck disable=SC1091
  source /etc/profile.d/vyx-llvm22.sh
fi

cd "$ROOT/bootstrap_compiler"
if [ ! -x out/boot ]; then
  echo "building boot for WP-B probe..."
  vyxc build --target boot -j"$(nproc)"
fi
export LD_LIBRARY_PATH="$ROOT/bootstrap_compiler/out${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
BOOT="$ROOT/bootstrap_compiler/out/boot"
SRC="$ROOT/probes/gates/b/nested_generic.vyx"

LOG="$("$BOOT" --src=file "$SRC" --emit=exe -o /tmp/b_nested_generic 2>&1 || true)"
if echo "$LOG" | grep -q 'error:'; then
  echo "FAIL: --emit=exe"
  echo "$LOG" | head -30
  exit 1
fi
/tmp/b_nested_generic
rc=$?
if [ "$rc" -ne 0 ]; then
  echo "FAIL: nested_generic exit=$rc"
  exit 1
fi
echo "OK: nested_generic exe exit=0"

HIR="$("$BOOT" --src=file "$SRC" --verify-hir2 2>&1 || true)"
echo "$HIR" | tail -8
if echo "$HIR" | grep -E 'error:|errors=[1-9]'; then
  echo "FAIL: --verify-hir2"
  exit 1
fi
echo "OK: --verify-hir2"
echo "OK: WP-B nested generic probe"
