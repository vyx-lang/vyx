#!/usr/bin/env bash
# std_packages is the single authority for std sources. The flat seed std/
# tree is a frozen legacy snapshot pending deletion -- no mirror/sync gate.
# Also: boot can import a real std module (std.time).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
export LLVM_ROOT="${LLVM_ROOT:-/usr/lib/llvm-22}"
export PATH="$LLVM_ROOT/bin:$PATH"
if [ -f /etc/profile.d/vyx-llvm22.sh ]; then
  # shellcheck disable=SC1091
  source /etc/profile.d/vyx-llvm22.sh
fi

fail=0

# Same basename under two package src/ trees is a second truth.
PKGS="$ROOT/bootstrap_compiler/std_packages"
declare -A seen
while IFS= read -r -d '' src; do
  base="$(basename "$src")"
  case "$base" in
    ffi.vyx) continue ;; # WP-I owns FFI bindings; name reuse is allowed
  esac
  if [ -n "${seen[$base]:-}" ]; then
    echo "FAIL: duplicate std module $base in ${seen[$base]} and $src"
    fail=1
  else
    seen[$base]="$src"
  fi
done < <(find "$PKGS" -path '*/src/*.vyx' -print0 | sort -z)

time_hits="$(find "$PKGS" -name time.vyx | wc -l)"
if [ "$time_hits" -ne 1 ]; then
  echo "FAIL: expected exactly one time.vyx under std_packages, got $time_hits"
  fail=1
else
  echo "OK: single time.vyx in std_packages"
fi

if grep -q 'struct _Ref' "$PKGS/core/src/ref.vyx" \
   || grep -q 'struct _Box' "$PKGS/core/src/ref.vyx"; then
  echo "FAIL: _Ref/_Box still in authority ref.vyx"
  fail=1
else
  echo "OK: _Ref/_Box removed from authority ref.vyx"
fi

if grep -q 'does not (yet) have move semantics' "$PKGS/core/src/hash.vyx"; then
  echo "FAIL: hash.vyx still denies move semantics"
  fail=1
else
  echo "OK: hash.vyx move comment aligned with clone.vyx"
fi

cd "$ROOT/bootstrap_compiler"
if [ ! -x out/boot ]; then
  echo "building boot for WP-H probe..."
  vyxc build --target boot -j1
fi
export LD_LIBRARY_PATH="$ROOT/bootstrap_compiler/out${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
BOOT="$ROOT/bootstrap_compiler/out/boot"

SRC="$(mktemp /tmp/h_std.XXXXXX.vyx)"
cat > "$SRC" << 'EOF'
use std.time;

fn main() -> i32 {
  print("hello");
  let t = current_timestamp();
  let ms = clock_ms();
  if (t < 0) { return 2; }
  if (ms < 0) { return 3; }
  return 0;
}
EOF

LOG="$("$BOOT" --src=file "$SRC" --emit=exe -o /tmp/h_std_probe 2>&1 || true)"
rm -f "$SRC"
if echo "$LOG" | grep -q 'error:'; then
  echo "FAIL: boot could not compile use std.time"
  echo "$LOG" | head -30
  fail=1
else
  /tmp/h_std_probe
  rc=$?
  if [ "$rc" -ne 0 ]; then
    echo "FAIL: std.time probe exit=$rc"
    fail=1
  else
    echo "OK: boot compiled and ran std.time"
  fi
fi

if [ "$fail" != 0 ]; then
  echo "std_single_truth FAIL"
  exit 1
fi
echo "OK: std_single_truth"
exit 0
