#!/usr/bin/env bash
# file:// registry: publish, search, install, lock.
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
  echo "building boot for pkg probe..."
  vyxc build --target boot -j"$(nproc)"
fi
export LD_LIBRARY_PATH="$ROOT/bootstrap_compiler/out${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
BOOT="$ROOT/bootstrap_compiler/out/boot"

WORK="$(mktemp -d /tmp/vyx_pkg.XXXXXX)"
export VYX_REGISTRY="$WORK/registry"
mkdir -p "$WORK/lib/src" "$WORK/app"

cat > "$WORK/lib/Vyx.toml" <<'TOML'
[package]
name = "demo_lib"
version = "1.2.3"
entry = "src/lib.vyx"
TOML
cat > "$WORK/lib/src/lib.vyx" <<'VYX'
module demo_lib;
public fn answer() -> i32 { return 7; }
VYX

cd "$WORK/lib"
if ! "$BOOT" publish > "$WORK/publish.log" 2>&1; then
  echo "FAIL: publish"
  cat "$WORK/publish.log"
  exit 1
fi
if ! grep -q 'published demo_lib 1.2.3' "$WORK/publish.log"; then
  echo "FAIL: publish message"
  cat "$WORK/publish.log"
  exit 1
fi

if ! "$BOOT" search demo_lib > "$WORK/search.log" 2>&1; then
  echo "FAIL: search"
  cat "$WORK/search.log"
  exit 1
fi
if ! grep -q 'demo_lib' "$WORK/search.log"; then
  echo "FAIL: search missed demo_lib"
  cat "$WORK/search.log"
  exit 1
fi

cat > "$WORK/app/Vyx.toml" <<'TOML'
[package]
name = "demo_app"
version = "0.1.0"
entry = "main.vyx"
TOML
cat > "$WORK/app/main.vyx" <<'VYX'
fn main() -> i32 { return 0; }
VYX
cd "$WORK/app"
if ! "$BOOT" install demo_lib@1.2.3 > "$WORK/install.log" 2>&1; then
  echo "FAIL: install"
  cat "$WORK/install.log"
  exit 1
fi
if [ ! -f "$WORK/app/.cache/registry/demo_lib/1.2.3/Vyx.toml" ]; then
  echo "FAIL: install did not materialize cache"
  cat "$WORK/install.log"
  exit 1
fi
if [ ! -f "$WORK/app/Vyx.lock" ]; then
  echo "FAIL: install did not write Vyx.lock"
  exit 1
fi
if ! "$BOOT" lock > "$WORK/lock.log" 2>&1; then
  echo "FAIL: lock"
  cat "$WORK/lock.log"
  exit 1
fi

echo "OK"
exit 0
