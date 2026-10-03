#!/usr/bin/env bash
# derive: fieldwise Clone; ordered Hash; enum Clone/Eq/Ord/Hashable;
# implied generic bounds; rawptr Clone rejected.
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
  echo "building boot for derive probe..."
  (cd bootstrap_compiler && vyxc build --target boot -j"$(nproc)")
fi
export LD_LIBRARY_PATH="$ROOT/bootstrap_compiler/out${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

run_ok() {
  local name="$1"
  local out
  out="$(mktemp /tmp/derive_${name}.XXXXXX)"
  "$BOOT" --src=file "probes/gates/derive/${name}.vyx" --emit=exe -o "$out"
  set +e
  "$out"
  local rc=$?
  set -e
  rm -f "$out"
  if [ "$rc" -ne 0 ]; then
    echo "derive FAIL: ${name} exit=$rc"
    exit 1
  fi
}

run_ok deep_clone
run_ok hash_order
run_ok enum_unit
run_ok enum_payload
run_ok generic_wrap

set +e
err="$("$BOOT" --src=file probes/gates/derive/rawptr_reject.vyx --emit=exe -o /tmp/derive_bad 2>&1)"
rej=$?
set -e
if [ "$rej" -eq 0 ]; then
  echo "derive FAIL: rawptr_reject compiled (should be rejected)"
  exit 1
fi
if ! echo "$err" | grep -q 'raw pointer'; then
  echo "$err"
  echo "derive FAIL: rawptr_reject did not mention raw pointer"
  exit 1
fi
echo "derive OK: deep clone, hash order, enum unit/payload, implied generic bound; rawptr rejected"
