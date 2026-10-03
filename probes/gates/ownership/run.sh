#!/usr/bin/env bash
# Move E3100, Copy suppresses move, borrow conflict E3101.
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
  echo "building boot for ownership probe..."
  vyxc build --target boot -j"$(nproc)"
fi
export LD_LIBRARY_PATH="$ROOT/bootstrap_compiler/out${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
BOOT="$ROOT/bootstrap_compiler/out/boot"

fail=0

run_ok() {
  local name="$1"
  local src="$2"
  local file
  file="$(mktemp /tmp/own_${name}.XXXXXX.vyx)"
  printf '%s\n' "$src" > "$file"
  if ! "$BOOT" --src=file "$file" --emit=exe -o "/tmp/own_${name}" >/tmp/own_${name}.log 2>&1; then
    echo "FAIL: ${name} did not compile"
    tail -30 /tmp/own_${name}.log
    fail=1
    rm -f "$file"
    return
  fi
  if ! "/tmp/own_${name}"; then
    echo "FAIL: ${name} exit != 0"
    fail=1
  else
    echo "OK: ${name}"
  fi
  rm -f "$file"
}

run_err() {
  local name="$1"
  local code="$2"
  local src="$3"
  local file
  file="$(mktemp /tmp/own_${name}.XXXXXX.vyx)"
  printf '%s\n' "$src" > "$file"
  local log
  log="$("$BOOT" --src=file "$file" --emit=ir -o /tmp/own_${name}.ll 2>&1 || true)"
  rm -f "$file"
  if echo "$log" | grep -q "$code"; then
    echo "OK: ${name}"
  else
    echo "FAIL: ${name} expected $code"
    echo "$log" | tail -30
    fail=1
  fi
}

run_err use_after_move E3100 "$(cat <<'VYX'
class Boxy {
  public n: i32;
  public fn drop() {}
}
fn main() -> i32 {
  var a = Boxy { n: 1 };
  var b = a;
  return a.n;
}
VYX
)"

# A class that owns a Vec field is move-only even without an explicit drop().
run_err nested_vec_field E3100 "$(cat <<'VYX'
use std.collections;
class Bag {
  public items: Vec<i32>;
}
fn main() -> i32 {
  var a = Bag { items: Vec::<i32>.new() };
  a.items.push(1);
  var b = a;
  return a.items.get(0);
}
VYX
)"

run_ok nested_vec_move_ok "$(cat <<'VYX'
use std.collections;
class Bag {
  public items: Vec<i32>;
}
fn main() -> i32 {
  var a = Bag { items: Vec::<i32>.new() };
  a.items.push(1);
  var b = a;
  if (b.items.get(0) != 1) { return 1; }
  return 0;
}
VYX
)""

run_ok copy_suppresses_move "$(cat <<'VYX'
@[derive(Copy)]
class Tiny {
  public n: i32;
  public fn drop() {}
}
fn main() -> i32 {
  var a = Tiny { n: 1 };
  var b = a;
  return a.n + b.n - 2;
}
VYX
)"

run_err borrow_conflict E3101 "$(cat <<'VYX'
fn main() -> i32 {
  var x = 1;
  let a = &x;
  let b = &mut x;
  return a + b;
}
VYX
)"

GATE_DIR="$(cd "$(dirname "$0")" && pwd)"

run_ok_file() {
  local name="$1"
  local src="$2"
  if ! "$BOOT" --src=file "$src" --emit=exe -o "/tmp/own_${name}" >/tmp/own_${name}.log 2>&1; then
    echo "FAIL: ${name} did not compile"
    tail -30 /tmp/own_${name}.log
    fail=1
    return
  fi
  if ! "/tmp/own_${name}"; then
    echo "FAIL: ${name} exit != 0"
    fail=1
  else
    echo "OK: ${name}"
  fi
}

run_err_file() {
  local name="$1"
  local code="$2"
  local src="$3"
  local log
  log="$("$BOOT" --src=file "$src" --emit=ir -o /tmp/own_${name}.ll 2>&1 || true)"
  if echo "$log" | grep -q "$code"; then
    echo "OK: ${name}"
  else
    echo "FAIL: ${name} expected $code"
    echo "$log" | tail -30
    fail=1
  fi
}

run_ok_file typed_mutref_bump "$GATE_DIR/typed_mutref_bump.vyx"
run_err_file typed_mutref_take E3100 "$GATE_DIR/typed_mutref_take.vyx"
run_err_file typed_ref_bump E3101 "$GATE_DIR/typed_ref_bump.vyx"
run_err_file compat_this_take E3100 "$GATE_DIR/compat_this_take.vyx"
run_err_file own_self_write E3101 "$GATE_DIR/own_self_write.vyx"
run_err_file own_expr_rejected E0001 "$GATE_DIR/own_expr_rejected.vyx"
run_ok_file unsafe_use_after_move "$GATE_DIR/unsafe_use_after_move.vyx"
run_ok_file let_mut_ok "$GATE_DIR/let_mut_ok.vyx"
run_ok_file unify_ok "$GATE_DIR/unify_ok.vyx"

if [ "$fail" -ne 0 ]; then
  echo "FAIL"
  exit 1
fi
echo "OK"
exit 0
