#!/usr/bin/env bash
# Trait solver: nominal impl + blanket impl with where-clause.
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
  echo "building boot for trait probe..."
  vyxc build --target boot -j"$(nproc)"
fi
export LD_LIBRARY_PATH="$ROOT/bootstrap_compiler/out${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
BOOT="$ROOT/bootstrap_compiler/out/boot"

fail=0

run_ok() {
  local name="$1"
  local src="$2"
  local file
  file="$(mktemp /tmp/trait_${name}.XXXXXX.vyx)"
  printf '%s\n' "$src" > "$file"
  if ! "$BOOT" --src=file "$file" --emit=exe -o "/tmp/trait_${name}" >/tmp/trait_${name}.log 2>&1; then
    echo "FAIL: ${name} did not compile"
    cat /tmp/trait_${name}.log | tail -30
    fail=1
    rm -f "$file"
    return
  fi
  if ! "/tmp/trait_${name}"; then
    echo "FAIL: ${name} exit != 0"
    fail=1
  else
    echo "OK: ${name}"
  fi
  rm -f "$file"
}

run_err() {
  local name="$1"
  local needle="$2"
  local src="$3"
  local file
  file="$(mktemp /tmp/trait_${name}.XXXXXX.vyx)"
  printf '%s\n' "$src" > "$file"
  local log
  log="$("$BOOT" --src=file "$file" --emit=ir -o /tmp/trait_${name}.ll 2>&1 || true)"
  rm -f "$file"
  if echo "$log" | grep -E -q "$needle"; then
    echo "OK: ${name}"
  else
    echo "FAIL: ${name} expected '$needle'"
    echo "$log" | tail -30
    fail=1
  fi
}

run_ok nominal "$(cat <<'VYX'
interface Marker {
  fn only_marker() -> i32;
}
class Point {
  public x: i32;
}
impl Marker for Point {
  fn only_marker() -> i32 { return 1; }
}
fn needs<T>(v: T) -> i32 where T: Marker { return 0; }
fn main() -> i32 {
  let p = Point { x: 1 };
  return needs::<Point>(p);
}
VYX
)"

run_ok blanket "$(cat <<'VYX'
interface Flag {}
interface Marker {
  fn only_via_blanket() -> i32;
}
class Point {
  public x: i32;
}
impl Flag for Point {}
impl<T> Marker for T where T: Flag {
  fn only_via_blanket() -> i32 { return 1; }
}
fn needs<T>(v: T) -> i32 where T: Marker { return 0; }
fn main() -> i32 {
  let p = Point { x: 1 };
  return needs::<Point>(p);
}
VYX
)"

run_err no_impl "E1100" "$(cat <<'VYX'
interface Marker {
  fn only_marker() -> i32;
}
class Nope {
  public x: i32;
}
fn needs<T>(v: T) -> i32 where T: Marker { return 0; }
fn main() -> i32 {
  let n = Nope { x: 1 };
  return needs::<Nope>(n);
}
VYX
)"

if [ "$fail" -ne 0 ]; then
  echo "FAIL"
  exit 1
fi
echo "OK"
exit 0
