#!/usr/bin/env bash
# rustc-style MIR await-split: dump has term=7; yield_now actually returns
# Pending to the pump; sleeper/quick still interleave as log==21.
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
  echo "building boot for async probe..."
  (cd bootstrap_compiler && vyxc build --target boot -j"$(nproc)")
fi
export LD_LIBRARY_PATH="$ROOT/bootstrap_compiler/out${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

dump="$("$BOOT" --src=file probes/gates/async/await_split.vyx --dump-mir2 2>/dev/null || true)"
if ! printf '%s\n' "$dump" | grep -q 'term=7'; then
  echo "async FAIL: dump-mir2 missing term=7 (mir_term_yield)"
  exit 1
fi

out="$(mktemp /tmp/async_split.XXXXXX)"
"$BOOT" --src=file probes/gates/async/await_split.vyx --emit=exe -o "$out"
set +e
"$out"
rc=$?
set -e
rm -f "$out"
if [ "$rc" -ne 0 ]; then
  echo "async FAIL: await_split exit=$rc (expected 0 / log 21 + pending_returns>=1)"
  exit 1
fi

out2="$(mktemp /tmp/async_interleave.XXXXXX)"
"$BOOT" --src=file probes/gates/async/interleave.vyx --emit=exe -o "$out2"
set +e
"$out2"
rc2=$?
set -e
rm -f "$out2"
if [ "$rc2" -ne 0 ]; then
  echo "async FAIL: interleave exit=$rc2 (expected 0 / log 21)"
  exit 1
fi
echo "async OK: await-split term=7 + pending_returns + interleave exit=0"
