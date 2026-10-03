#!/usr/bin/env bash
# --src=file must resolve std.* from std_packages/registry, not the flat std/ mirror.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
PKG="$ROOT/bootstrap_compiler/std_packages"
REG="$PKG/registry"
BOOT="$ROOT/bootstrap_compiler/out/boot"
if [ -x "$ROOT/bootstrap_compiler/out/boot.exe" ]; then
  BOOT="$ROOT/bootstrap_compiler/out/boot.exe"
fi
export LLVM_ROOT="${LLVM_ROOT:-/usr/lib/llvm-22}"
export PATH="$LLVM_ROOT/bin:$ROOT/bootstrap_compiler/out:${PATH:-}"
export LD_LIBRARY_PATH="$ROOT/bootstrap_compiler/out${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

fail=0
if [ ! -x "$BOOT" ]; then
  echo "FAIL: missing $BOOT"
  exit 1
fi
if [ ! -f "$REG" ]; then
  echo "FAIL: missing std_packages/registry"
  fail=1
fi

expected="$(mktemp)"
listed="$(mktemp)"
trap 'rm -f "$expected" "$listed"' EXIT

: > "$expected"
for toml in "$PKG"/*/Vyx.toml; do
  pkg="$(basename "$(dirname "$toml")")"
  grep -oE '"[^"]+\.vyx"' "$toml" | tr -d '"' | while read -r rel; do
    rel="${rel//\\//}"
    if [ -f "$PKG/$pkg/$rel" ]; then
      printf '%s/%s\n' "$pkg" "$rel"
    fi
  done
done | sort -u > "$expected"

: > "$listed"
if [ -f "$REG" ]; then
  while IFS= read -r raw || [ -n "$raw" ]; do
    line="$(printf '%s' "$raw" | tr -d '\r' | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')"
    case "$line" in
      ""|\#*|//*) continue ;;
    esac
    line="${line//\\//}"
    printf '%s\n' "$line"
  done < "$REG" | sort -u > "$listed"
fi

while IFS= read -r rel || [ -n "$rel" ]; do
  [ -z "$rel" ] && continue
  if [ ! -f "$PKG/$rel" ]; then
    echo "FAIL: registry path missing on disk: $rel"
    fail=1
  fi
done < "$listed"

while IFS= read -r rel; do
  if ! grep -Fxq "$rel" "$listed"; then
    echo "FAIL: registry missing $rel"
    fail=1
  fi
done < "$expected"
while IFS= read -r rel; do
  if ! grep -Fxq "$rel" "$expected"; then
    echo "FAIL: registry extra $rel"
    fail=1
  fi
done < "$listed"

SRC="$(mktemp /tmp/vyx-src-file-packages.XXXXXX.vyx)"
EXE="$(mktemp /tmp/vyx-src-file-packages.XXXXXX.exe)"
cat > "$SRC" << 'EOF'
use std.string;

fn main() -> i32 {
 print("hello");
 return 0;
}
EOF

LOG="$(VYX_DEBUG_IMPORT=1 "$BOOT" --src=file "$SRC" --dump-mir2 2>&1 || true)"
if ! printf '%s' "$LOG" | grep -Eq 'std_packages[/\\]core[/\\]src[/\\]string\.vyx'; then
  echo "FAIL: --src=file did not load std.string from std_packages"
  fail=1
fi
if printf '%s' "$LOG" | grep -Eq 'bootstrap_compiler[/\\]std[/\\]string\.vyx'; then
  echo "FAIL: --src=file still loaded flat std/string.vyx"
  fail=1
fi

if ! "$BOOT" --src=file "$SRC" --emit=exe -o "$EXE" -L "$ROOT/bootstrap_compiler/out" -l vyx_runtime; then
  echo "FAIL: TEMP hello did not compile"
  fail=1
elif ! "$EXE"; then
  echo "FAIL: TEMP hello non-zero exit"
  fail=1
fi
rm -f "$SRC" "$EXE"

if [ "$fail" != 0 ]; then
  echo "src_file_packages FAIL"
  exit 1
fi
echo "OK: src_file_packages"
exit 0
