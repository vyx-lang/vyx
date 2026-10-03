#!/usr/bin/env bash
# Native DAP engine is exported from the compiler backend.
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
  echo "building boot for DAP probe..."
  vyxc build --target boot -j"$(nproc)"
fi

SO=""
for cand in \
  "$ROOT/bootstrap_compiler/out/libvyx_compiler_backend.so" \
  "$ROOT/bootstrap_compiler/out/vyx_compiler_backend.so"; do
  if [ -f "$cand" ]; then
    SO="$cand"
    break
  fi
done
if [ -z "$SO" ]; then
  echo "FAIL: missing compiler backend .so in $ROOT/bootstrap_compiler/out"
  ls -la "$ROOT/bootstrap_compiler/out"/*.so 2>/dev/null || true
  exit 1
fi
NM_OUT=/tmp/dap_nm_exports.txt
nm -D "$SO" > "$NM_OUT" || true
if ! grep -q 'vyx_dap_engine_available' "$NM_OUT"; then
  echo "FAIL: vyx_dap_engine_available not exported"
  grep vyx_dap "$NM_OUT" || true
  exit 1
fi
if ! grep -q 'vyx_dap_launch' "$NM_OUT"; then
  echo "FAIL: vyx_dap_launch not exported"
  exit 1
fi

cat > /tmp/dap_engine_probe.c <<'C'
#include <stdio.h>
int vyx_dap_engine_available(void);
int main(void) {
    int n = vyx_dap_engine_available();
    if (n != 1) {
        fprintf(stderr, "available=%d\n", n);
        return 1;
    }
    return 0;
}
C
cc -o /tmp/dap_engine_probe /tmp/dap_engine_probe.c "$SO" -Wl,-rpath,"$ROOT/bootstrap_compiler/out"
if ! /tmp/dap_engine_probe; then
  echo "FAIL: engine not available"
  exit 1
fi
echo "OK"
exit 0
