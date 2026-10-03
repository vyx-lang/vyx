#!/usr/bin/env bash
# WP-C: --verify-mir2 verified functions= equals --dump-mir2 functions=.
# Does not undo streaming discard. Do not `boot build --verify-mir2`.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
export LLVM_ROOT="${LLVM_ROOT:-/usr/lib/llvm-22}"
export PATH="$LLVM_ROOT/bin:$PATH"

BOOT="$ROOT/bootstrap_compiler/out/boot"
if [[ ! -x "$BOOT" ]]; then
    BOOT="/home/ubuntu/wp-split/bootstrap_compiler/out/boot"
fi
if [[ ! -x "$BOOT" ]]; then
    echo "FAIL: no boot"
    exit 1
fi
export LD_LIBRARY_PATH="$(dirname "$BOOT")${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

UNIT="$(mktemp /tmp/c_big_unit.XXXXXX.vyx)"
{
    echo '// Many live functions. Names are gN (f32 is a reserved type name).'
    i=0
    while [ "$i" -lt 40 ]; do
        echo "fn g${i}(x: i32) -> i32 { return x + ${i}; }"
        i=$((i + 1))
    done
    echo 'fn main() -> i32 {'
    echo '  print("hello");'
    echo '  var s: i32 = 0;'
    i=0
    while [ "$i" -lt 40 ]; do
        echo "  s = s + g${i}(${i});"
        i=$((i + 1))
    done
    echo '  return s;'
    echo '}'
} > "$UNIT"

DUMP_LOG="$("$BOOT" --src=file "$UNIT" --dump-mir2 2>&1 || true)"
VERIFY_LOG="$("$BOOT" --src=file "$UNIT" --verify-mir2 2>&1 || true)"
rm -f "$UNIT"

DUMP_N="$(echo "$DUMP_LOG" | sed -n 's/^mir2.unit functions=\([0-9][0-9]*\).*/\1/p' | tail -1)"
VER_N="$(echo "$VERIFY_LOG" | sed -n 's/^verified functions=\([0-9][0-9]*\).*/\1/p' | tail -1)"

echo "dump functions=$DUMP_N"
echo "verified functions=$VER_N"

if [ -z "$DUMP_N" ] || [ -z "$VER_N" ]; then
    echo "FAIL: missing functions= or verified functions="
    echo "$DUMP_LOG" | tail -15
    echo "$VERIFY_LOG" | tail -15
    exit 1
fi
if [ "$DUMP_N" != "$VER_N" ]; then
    echo "FAIL: dump functions=$DUMP_N verify functions=$VER_N"
    exit 1
fi
if [ "$DUMP_N" -lt 41 ]; then
    echo "FAIL: expected at least 41 functions, got $DUMP_N"
    exit 1
fi
echo "OK: verified functions=$VER_N matches dump functions=$DUMP_N"
