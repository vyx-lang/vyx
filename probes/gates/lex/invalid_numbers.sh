#!/usr/bin/env bash
# Invalid numeric literals must be one E0104 token, not a number plus a
# stray identifier that produces a misleading parser follow-on error.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
export LLVM_ROOT="${LLVM_ROOT:-/usr/lib/llvm-22}"
export PATH="$LLVM_ROOT/bin:$PATH"
if [ -f /etc/profile.d/vyx-llvm22.sh ]; then
  # shellcheck disable=SC1091
  source /etc/profile.d/vyx-llvm22.sh
fi

cd "$ROOT/bootstrap_compiler"
if [ ! -x out/boot ] && [ ! -x out/boot.exe ]; then
  echo "building boot for lex probe..."
  vyxc build --target boot -j"$(nproc)"
fi
export LD_LIBRARY_PATH="$ROOT/bootstrap_compiler/out${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
if [ -x out/boot.exe ]; then
  BOOT="$ROOT/bootstrap_compiler/out/boot.exe"
else
  BOOT="$ROOT/bootstrap_compiler/out/boot"
fi

fail=0

assert_invalid() {
  local name="$1"
  local needle="$2"
  local src="$3"
  local file log
  file="$(mktemp /tmp/lex_${name}.XXXXXX.vyx)"
  printf '%s\n' "$src" > "$file"
  log="$("$BOOT" --src=file "$file" --emit=ir -o /tmp/lex_${name}.ll 2>&1 || true)"
  rm -f "$file"
  if echo "$log" | grep -q ": error: E0104:" && echo "$log" | grep -q "invalid numeric literal" && echo "$log" | grep -q "$needle"; then
    echo "OK: ${name} -> E0104 (${needle})"
    return
  fi
  echo "FAIL: ${name} expected E0104 invalid numeric literal containing '${needle}'"
  echo "$log" | head -25
  fail=1
}

assert_invalid hex_empty 'base-16 prefix' 'fn main() -> i32 { return 0x; }'
assert_invalid bin_bad_digit 'digit is not valid for this base' 'fn main() -> i32 { return 0b102; }'
assert_invalid oct_empty 'base-8 prefix' 'fn main() -> i32 { return 0o; }'
assert_invalid exp_empty 'exponent' 'fn main() -> i32 { return (1e+) as i32; }'
assert_invalid bad_int_suffix 'suffix is not recognised' 'fn main() -> i32 { return 42i; }'
assert_invalid float_suffix 'floating-point literal' 'fn main() -> i32 { return (1.0f) as i32; }'

VALID="$ROOT/probes/gates/lex/valid_numbers.vyx"
OUT="$(mktemp /tmp/lex_valid.XXXXXX)"
if ! "$BOOT" --src=file "$VALID" --emit=exe -o "$OUT" >/tmp/lex_valid.log 2>&1; then
  echo "FAIL: valid_numbers.vyx should compile"
  head -20 /tmp/lex_valid.log
  fail=1
else
  rc=0
  "$OUT" >/tmp/lex_valid.run 2>&1 || rc=$?
  rm -f "$OUT"
  if [ "$rc" != 0 ]; then
    echo "FAIL: valid_numbers exit=${rc}"
    fail=1
  else
    echo "OK: valid_numbers exit=0"
  fi
fi

if [ "$fail" != 0 ]; then
  echo "FAIL"
  exit 1
fi
echo "OK: lexer rejects malformed numeric literals as E0104"
