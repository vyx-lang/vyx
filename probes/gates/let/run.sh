#!/usr/bin/env bash
# Module-level let constants of equal value and different types must compile.
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
  echo "building boot for let-intern probe..."
  vyxc build --target boot -j"$(nproc)"
fi
export LD_LIBRARY_PATH="$ROOT/bootstrap_compiler/out${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
BOOT="$ROOT/bootstrap_compiler/out/boot"

cat > /tmp/let_intern.vyx <<'VYX'
let A: i32 = 1;
let B: i64 = 1;
fn main() -> i32 {
  let x: i64 = B;
  if (A != 1) { return 2; }
  if (x != 1) { return 3; }
  return 0;
}
VYX

if ! "$BOOT" --src=file /tmp/let_intern.vyx --emit=exe -o /tmp/let_intern >/tmp/let_intern.log 2>&1; then
  echo "FAIL: same-value different-type module lets did not compile"
  cat /tmp/let_intern.log
  exit 1
fi
if ! grep -q 'mir.global' /tmp/let_intern.log 2>/dev/null; then
  :
fi
if ! /tmp/let_intern; then
  echo "FAIL: let intern program exit != 0"
  exit 1
fi
echo "OK"
exit 0
