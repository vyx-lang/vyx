#!/usr/bin/env bash
# WP-P0a: class-by-value ABI — unique C aggregate rule.
#
# 1) Heap-backed Vec field mutated in a method, class returned by value.
# 2) Class > 16 bytes returned by value after method writes (must be sret).
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
  echo "building boot for WP-P0a probe..."
  (cd bootstrap_compiler && vyxc build --target boot -j"$(nproc)")
fi
export LD_LIBRARY_PATH="$ROOT/bootstrap_compiler/out${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

fail() { echo "p0a FAIL: $*"; exit 1; }

run_exe() {
  local src="$1"
  local name="$2"
  local out
  out="$(mktemp /tmp/p0a_${name}.XXXXXX)"
  "$BOOT" --src=file "$src" --emit=exe -o "$out"
  "$out"
  local rc=$?
  rm -f "$out"
  if [ "$rc" -ne 0 ]; then
    fail "$name exit=$rc"
  fi
  echo "p0a OK: $name exit=0"
}

run_exe probes/gates/p0a/class_return.vyx class_return
run_exe probes/gates/p0a/large_return.vyx large_return

IR="$(mktemp /tmp/p0a_large.XXXXXX.ll)"
"$BOOT" --src=file probes/gates/p0a/large_return.vyx --emit=ir -o "$IR"
if ! grep -q 'sret(%mir.struct.probe.large_return.Big)' "$IR"; then
  echo "----- make() IR -----"
  grep -n 'define .*make_R_' "$IR" || true
  rm -f "$IR"
  fail "large make() must return via sret (not a first-class struct)"
fi
if ! grep -q 'byval(%mir.struct.probe.large_return.Big)' "$IR"; then
  echo "----- take() IR -----"
  grep -n 'define .*take_R_' "$IR" || true
  rm -f "$IR"
  fail "take(Big) must pass the large aggregate byval"
fi
rm -f "$IR"
echo "p0a OK: large make() uses sret; take(Big) uses byval"
