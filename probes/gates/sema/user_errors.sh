#!/usr/bin/env bash
# Sema is the user-error channel: call-site types, &T is not an integer,
# struct-literal fields, let immutability. Wrong programs must fail at Sema
# with E1000/E3000 — not I0100 at LLVM.
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
  echo "building boot for Sema probe..."
  vyxc build --target boot -j"$(nproc)"
fi
export LD_LIBRARY_PATH="$ROOT/bootstrap_compiler/out${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
BOOT="$ROOT/bootstrap_compiler/out/boot"

fail=0

assert_error() {
  local name="$1"
  local expect="$2"
  local needle="$3"
  local src="$4"
  local file
  file="$(mktemp /tmp/sema_${name}.XXXXXX.vyx)"
  printf '%s\n' "$src" > "$file"
  local log
  log="$("$BOOT" --src=file "$file" --emit=ir -o /tmp/sema_${name}.ll 2>&1 || true)"
  rm -f "$file"
  if echo "$log" | grep -q ": error: ${expect}:" && echo "$log" | grep -q "$needle"; then
    if echo "$log" | grep -q "I0100"; then
      echo "FAIL: ${name} leaked to I0100"
      echo "$log" | head -20
      fail=1
      return
    fi
    echo "OK: ${name} -> ${expect} (${needle})"
    return
  fi
  echo "FAIL: ${name} expected ${expect} containing '${needle}'"
  echo "$log" | head -25
  fail=1
}

assert_ok() {
  local name="$1"
  local src="$2"
  local file out
  file="$(mktemp /tmp/sema_ok_${name}.XXXXXX.vyx)"
  out="$(mktemp /tmp/sema_ok_${name}.XXXXXX)"
  printf '%s\n' "$src" > "$file"
  if ! "$BOOT" --src=file "$file" --emit=exe -o "$out" >/tmp/sema_ok_${name}.log 2>&1; then
    echo "FAIL: ${name} should compile"
    head -20 /tmp/sema_ok_${name}.log
    rm -f "$file"
    fail=1
    return
  fi
  local rc=0
  "$out" >/tmp/sema_ok_${name}.run 2>&1 || rc=$?
  rm -f "$file" "$out"
  if [ "$rc" != 0 ]; then
    echo "FAIL: ${name} exit=${rc}"
    fail=1
    return
  fi
  echo "OK: ${name} exit=0"
}

assert_error string_to_i64 E1000 'call arg' \
'fn takes_i64(x: i64) -> i64 { return x; }
fn main() -> i32 {
  let s: string = "hi";
  let r: i64 = takes_i64(s);
  return 0;
}'

assert_error greet_int E1000 'call arg' \
'fn greet(name: string) -> i32 { print(name); return 0; }
fn main() -> i32 { return greet(42); }'

assert_error ref_as_value E1000 'call arg' \
'fn takes_i64(x: i64) -> i64 { return x; }
fn main() -> i32 {
  let a: i64 = 41;
  let r: &i64 = &a;
  let b: i64 = takes_i64(r);
  return 0;
}'

assert_error ref_arith E1000 'binary operator' \
'fn main() -> i32 {
  let a: i64 = 41;
  let r: &i64 = &a;
  let b: i64 = r + 1;
  return 0;
}'

assert_error missing_field E3000 'missing field' \
'class User {
  public name: string
  public age: i64
}
fn main() -> i32 {
  let u: User = User { name: "ana" };
  return 0;
}'

assert_error reassign_let E3000 "immutable 'let'" \
'fn main() -> i32 {
  let count: i64 = 0;
  count = count + 1;
  return count as i32;
}'

assert_ok int_literal \
'fn takes_i64(x: i64) -> i64 { return x; }
fn main() -> i32 {
  let r: i64 = takes_i64(1);
  print("ok");
  return 0;
}'

assert_ok suffix_overload \
'fn describe(x: i32) -> i32 { return 1; }
fn describe(x: i64) -> i32 { return 2; }
fn main() -> i32 {
  if (describe(10i64) != 2) { return 1; }
  if (describe(10) != 1) { return 2; }
  print("ok");
  return 0;
}'

assert_ok ref_deref \
'fn main() -> i32 {
  let a: i64 = 41;
  let r: &i64 = &a;
  let b: i64 = *r + 1;
  if (b != 42) { return 1; }
  print("ok");
  return 0;
}'

assert_ok full_struct \
'class User {
  public name: string
  public age: i64
}
fn main() -> i32 {
  let u: User = User { name: "ana", age: 7 };
  if (u.age != 7) { return 1; }
  print("ok");
  return 0;
}'

assert_ok reassign_var \
'fn main() -> i32 {
  var count: i64 = 0;
  count = count + 1;
  if (count != 1) { return 1; }
  print("ok");
  return 0;
}'

assert_ok reassign_param \
'fn clamp(x: i32) -> i32 {
  if (x < 0) { x = 0; }
  return x;
}
fn main() -> i32 {
  if (clamp(-3) != 0) { return 1; }
  print("ok");
  return 0;
}'

if [ "$fail" != 0 ]; then
  echo "FAIL"
  exit 1
fi
echo "OK: Sema user-error channel (call/ref/fields/let)"
