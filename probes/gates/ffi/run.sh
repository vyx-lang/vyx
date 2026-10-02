#!/usr/bin/env bash
# Language C ABI FFI: import/export, repr(C)/packed/align, cfn callbacks,
# C varargs, link/link_name. Does not use .dcib / DCI.
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
  echo "building boot for FFI probe..."
  vyxc build --target boot -j"$(nproc)"
fi
export LD_LIBRARY_PATH="$ROOT/bootstrap_compiler/out${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
BOOT="$ROOT/bootstrap_compiler/out/boot"
CLANG="$LLVM_ROOT/bin/clang"
WORKDIR="$(mktemp -d /tmp/ffi_probe.XXXXXX)"
trap 'rm -rf "$WORKDIR"' EXIT

fail=0

"$CLANG" -c -fPIC "$ROOT/probes/gates/ffi/helpers.c" -o "$WORKDIR/helpers.o"
"$CLANG" -shared -fPIC "$ROOT/probes/gates/ffi/link_lib.c" -o "$WORKDIR/libffi_link_probe.so"

if ! "$BOOT" --src=file "$ROOT/probes/gates/ffi/roundtrip.vyx" --emit=exe \
    --link-obj "$WORKDIR/helpers.o" \
    -L "$WORKDIR" \
    -o "$WORKDIR/roundtrip" >"$WORKDIR/roundtrip.log" 2>&1; then
  echo "FAIL: roundtrip compile"
  head -40 "$WORKDIR/roundtrip.log"
  fail=1
else
  rc=0
  "$WORKDIR/roundtrip" >"$WORKDIR/roundtrip.run" 2>&1 || rc=$?
  if [ "$rc" != 0 ]; then
    echo "FAIL: roundtrip exit=${rc}"
    cat "$WORKDIR/roundtrip.run"
    fail=1
  else
    echo "OK: roundtrip exit=0"
  fi
fi

if ! "$BOOT" --src=file "$ROOT/probes/gates/ffi/link_name.vyx" --emit=exe \
    -L "$WORKDIR" \
    -o "$WORKDIR/link_name" >"$WORKDIR/link_name.log" 2>&1; then
  echo "FAIL: link_name compile"
  head -40 "$WORKDIR/link_name.log"
  fail=1
else
  rc=0
  LD_LIBRARY_PATH="$WORKDIR:$LD_LIBRARY_PATH" "$WORKDIR/link_name" \
    >"$WORKDIR/link_name.run" 2>&1 || rc=$?
  if [ "$rc" != 0 ]; then
    echo "FAIL: link_name exit=${rc}"
    cat "$WORKDIR/link_name.run"
    fail=1
  else
    echo "OK: link_name exit=0"
  fi
fi

if ! "$BOOT" --src=file "$ROOT/probes/gates/ffi/std_ffi.vyx" --emit=exe \
    -o "$WORKDIR/std_ffi" >"$WORKDIR/std_ffi.log" 2>&1; then
  echo "FAIL: std_ffi compile"
  head -40 "$WORKDIR/std_ffi.log"
  fail=1
else
  rc=0
  "$WORKDIR/std_ffi" >"$WORKDIR/std_ffi.run" 2>&1 || rc=$?
  if [ "$rc" != 0 ]; then
    echo "FAIL: std_ffi exit=${rc}"
    cat "$WORKDIR/std_ffi.run"
    fail=1
  else
    echo "OK: std_ffi exit=0"
  fi
fi

neg='fn bump(x: i32) -> i32 { return x + 1; }
extern "C" {
  fn call_cb(cb: cfn(i32) -> i32, x: i32) -> i32;
}
fn main() -> i32 { return call_cb(bump, 1); }
'
neg_file="$WORKDIR/fn_as_cfn.vyx"
printf '%s\n' "$neg" > "$neg_file"
neg_log="$("$BOOT" --src=file "$neg_file" --emit=ir -o "$WORKDIR/fn_as_cfn.ll" 2>&1 || true)"
if echo "$neg_log" | grep -q ": error: E1000:" && echo "$neg_log" | grep -q "call arg"; then
  if echo "$neg_log" | grep -q "I0100"; then
    echo "FAIL: fn_as_cfn leaked to I0100"
    echo "$neg_log" | head -20
    fail=1
  else
    echo "OK: fn_as_cfn -> E1000"
  fi
else
  echo "FAIL: fn_as_cfn expected E1000"
  echo "$neg_log" | head -25
  fail=1
fi

if [ "$fail" != 0 ]; then
  echo "FAIL"
  exit 1
fi
echo "OK: C ABI FFI (import/export/repr/cfn/varargs/link)"
