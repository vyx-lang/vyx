#!/usr/bin/env bash
# @[comptime] fn + const args must fold to a literal (no call @triple in IR)
# and evaluate to 15 (triple(4) = (4+1)*3).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
export LLVM_ROOT="${LLVM_ROOT:-/usr/lib/llvm-22}"
export PATH="$LLVM_ROOT/bin:$PATH"
if [ -f /etc/profile.d/vyx-llvm22.sh ]; then
  # shellcheck disable=SC1091
  source /etc/profile.d/vyx-llvm22.sh
fi

cd "$ROOT"
BOOT="bootstrap_compiler/out/boot"
if [ ! -x "$BOOT" ]; then
  echo "building boot for comptime probe..."
  (cd bootstrap_compiler && vyxc build --target boot -j"$(nproc)")
fi
export LD_LIBRARY_PATH="$ROOT/bootstrap_compiler/out${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

out="$(mktemp /tmp/comptime_fold.XXXXXX)"
"$BOOT" --src=file probes/gates/comptime/fold.vyx --emit=exe -o "$out"
set +e
"$out"
rc=$?
set -e
rm -f "$out"
if [ "$rc" -ne 0 ]; then
  echo "comptime FAIL: fold exe exit=$rc (expected 0)"
  exit 1
fi

loop_out="$(mktemp /tmp/comptime_loop.XXXXXX)"
"$BOOT" --src=file probes/gates/comptime/loop.vyx --emit=exe -o "$loop_out"
set +e
"$loop_out"
loop_rc=$?
set -e
rm -f "$loop_out"
if [ "$loop_rc" -ne 0 ]; then
  echo "comptime FAIL: loop/io exe exit=$loop_rc (expected 0)"
  exit 1
fi

ir="$(mktemp /tmp/comptime_fold.XXXXXX.ll)"
"$BOOT" --src=file probes/gates/comptime/fold.vyx --emit=ir -o "$ir"
if grep -E 'call[^[:space:]]* .*triple' "$ir" >/dev/null; then
  echo "----- IR (unexpected call @triple) -----"
  grep -n -E 'triple' "$ir" || true
  rm -f "$ir"
  echo "comptime FAIL: IR still calls triple"
  exit 1
fi
rm -f "$ir"
echo "comptime OK: folded triple(4) and IR has no call"
