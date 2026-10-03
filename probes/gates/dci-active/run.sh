#!/usr/bin/env bash
# DCI Active Adapter tools gate: on-demand closure, offline artifact replay,
# unique symbols, and corruption rejection (docs/DCI_SPEC_ZH.md §2.4).
#
#   A. on-demand closure      — rust + cpp sessions close a generic call that no
#                               pre-existing contract covers, refuse the sibling
#                               operation with a producer diagnostic, and publish
#                               a producer-compiled object into the bundle cache
#   B. offline replay         — with source, headers and adapters gone, the
#                               bundle alone links and runs (replay.py imports
#                               only the Phase 0 bundle loader)
#   C. symbol uniqueness      — llvm-nm defined __vyx_* symbols == manifest
#                               covered_operations
#   D. corruption fail-closed — a flipped byte in the published object must be
#                               caught by load_bundle, never linked silently
#
# Gate discipline: no swallowed errors (no `2>/dev/null || true`), every
# count asserted, expected-failure runs captured with if/else before rc use.
#
# Windows toolchain paths (python/clang/llvm-nm) are converted with
# `cygpath -m`; MSYS paths sent to Windows executables resolve to garbage
# like E:\e\Dev\... and every check silently reads the wrong file.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
REPO_W="$(cygpath -m "$REPO_ROOT")"
w() { cygpath -m "$1"; }   # MSYS path -> plain Windows path for PE tools

PY="${PYTHON:-python}"
WORK="$REPO_ROOT/.cache/dci-active/run"
CACHE="$WORK/bundles"

rm -rf "$WORK"
mkdir -p "$CACHE"

RUST_FIXTURE="$REPO_ROOT/tools/dci/tests/fixtures/active_rust/lib.rs"
CPP_FIXTURE="$REPO_ROOT/tools/dci/tests/fixtures/active_cpp/active_fixture.hpp"
CLANG="$REPO_ROOT/clang/bin/clang.exe"
if [ ! -x "$CLANG" ]; then CLANG="$(command -v clang)"; fi
NM="$REPO_ROOT/clang/bin/llvm-nm.exe"
if [ ! -x "$NM" ]; then NM="$(command -v llvm-nm || command -v nm)"; fi
echo "gate: clang=$CLANG nm=$NM"

fail() { echo "GATE FAIL: $1" >&2; exit 1; }

# ---------------------------------------------------------------------------
# A. on-demand closure + admission + publication (rust, then cpp)
for LANG_ in rust cpp; do
  echo "=== A.$LANG_ on-demand closure ==="
  SUMMARY="$WORK/$LANG_.summary"
  if [ "$LANG_" = rust ]; then FIXTURE="$RUST_FIXTURE"; else FIXTURE="$CPP_FIXTURE"; fi
  if ! "$PY" "$REPO_W/probes/gates/dci-active/e2e.py" \
        --lang "$LANG_" --fixture "$(w "$FIXTURE")" \
        --cache "$(w "$CACHE")" --work "$(w "$WORK/$LANG_")" \
        --summary "$(w "$SUMMARY")" \
        > "$WORK/$LANG_.log" 2>&1; then
    cat "$WORK/$LANG_.log" >&2
    fail "A.$LANG_ e2e driver failed"
  fi
  cat "$WORK/$LANG_.log"
  for LINE in "closed=" "admission=rejected:constraint_failed" "bundle=" \
              "single_flight=yes" "invalidated=yes"; do
    grep -q "OK $LANG_ .*$LINE" "$WORK/$LANG_.log" \
      || fail "A.$LANG_ missing assertion line containing $LINE"
  done
  grep -Eq "^BUNDLE=[0-9a-f]{64}$" "$SUMMARY" || fail "A.$LANG_ bad bundle key"
  grep -q "^REPLAY_SYMBOL=" "$SUMMARY" || fail "A.$LANG_ no replay symbol"
  grep -q "^REPLAY_EXPECT=" "$SUMMARY" || fail "A.$LANG_ no replay expectation"
done

# no staging leftovers in the cache (failed/aborted publications)
STAGING=$(find "$CACHE" -maxdepth 1 -name '.staging-*' | wc -l)
[ "$STAGING" -eq 0 ] || fail "A. staging leftovers: $STAGING"

# ---------------------------------------------------------------------------
# B+C. offline replay, symbol uniqueness, corruption fail-closed
for LANG_ in rust cpp; do
  echo "=== B.$LANG_ offline replay ==="
  SUMMARY="$WORK/$LANG_.summary"
  BUNDLE=$(grep '^BUNDLE=' "$SUMMARY" | cut -d= -f2)
  SYMBOL=$(grep '^REPLAY_SYMBOL=' "$SUMMARY" | cut -d= -f2)
  EXPECT=$(grep '^REPLAY_EXPECT=' "$SUMMARY" | cut -d= -f2)
  OBJ="$CACHE/$BUNDLE/artifacts/shims.obj"
  [ -f "$OBJ" ] || fail "B.$LANG_ bundle object missing: $OBJ"

  # C. defined __vyx_* symbols must match the manifest operation count exactly
  COVERED=$("$PY" -c "import json,sys;print(len(json.load(open(sys.argv[1]))['covered_operations']))" "$(w "$CACHE/$BUNDLE/manifest.cjson.json")")
  DEFINED=$("$NM" "$(w "$OBJ")" | grep -c " T __vyx_" || true)
  echo "gate: covered=$COVERED defined=$DEFINED"
  [ "$DEFINED" -eq "$COVERED" ] || fail "C.$LANG_ symbol count $DEFINED != covered $COVERED"

  # B. replay with only the bundle (no source, no adapters, no session)
  DRIVER="$WORK/$LANG_-driver.c"
  if [ "$LANG_" = rust ]; then
    cat > "$DRIVER" <<EOF
#include <stdio.h>
extern int $SYMBOL(int v);
int main(void) { printf("%d\n", $SYMBOL(42)); return 0; }
EOF
  else
    cat > "$DRIVER" <<EOF
#include <stdio.h>
extern int $SYMBOL(int a, int b);
int main(void) { printf("%d\n", $SYMBOL(40, 2)); return 0; }
EOF
  fi
  if ! "$PY" "$REPO_W/probes/gates/dci-active/replay.py" --cache "$(w "$CACHE")" \
        --bundle "$BUNDLE" --driver "$(w "$DRIVER")" --cc "$(w "$CLANG")" \
        --expect "$EXPECT" --out "$(w "$WORK/$LANG_-replay")" \
        > "$WORK/$LANG_-replay.log" 2>&1; then
    cat "$WORK/$LANG_-replay.log" >&2
    fail "B.$LANG_ offline replay failed"
  fi
  grep -q "^REPLAY=$EXPECT$" "$WORK/$LANG_-replay.log" \
    || fail "B.$LANG_ replay output mismatch: $(cat "$WORK/$LANG_-replay.log")"
  echo "gate: $LANG_ replay=$EXPECT (bundle $BUNDLE)"

  # D. corruption fail-closed: flip one byte inside the published object
  echo "=== D.$LANG_ corruption fail-closed ==="
  CORRUPT="$WORK/$LANG_-corrupt-cache"
  mkdir -p "$CORRUPT/$BUNDLE"
  cp -r "$CACHE/$BUNDLE/." "$CORRUPT/$BUNDLE/"
  "$PY" - "$(w "$CORRUPT/$BUNDLE/artifacts/shims.obj")" <<'PYEOF'
import sys
path = sys.argv[1]
data = bytearray(open(path, "rb").read())
data[len(data) // 2] ^= 0xFF
open(path, "wb").write(data)
PYEOF
  if "$PY" "$REPO_W/probes/gates/dci-active/replay.py" --cache "$(w "$CORRUPT")" \
        --bundle "$BUNDLE" --driver "$(w "$DRIVER")" --cc "$(w "$CLANG")" \
        --expect "$EXPECT" --out "$(w "$WORK/$LANG_-corrupt")" \
        > "$WORK/$LANG_-corrupt.log" 2>&1; then
    cat "$WORK/$LANG_-corrupt.log" >&2
    fail "D.$LANG_ corrupted bundle was linked and ran"
  fi
  grep -qE "content mismatch|BundleError|unreadable manifest" "$WORK/$LANG_-corrupt.log" \
    || fail "D.$LANG_ corruption rejected for the wrong reason"
  echo "gate: $LANG_ corruption rejected"
done

echo "GATE OK: dci-active (closure, admission, publication, single-flight,"
echo "         invalidation, offline replay, symbol uniqueness, corruption)"
