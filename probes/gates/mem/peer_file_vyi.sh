#!/usr/bin/env bash
# Peer interfaces are per-file .vyi, not a unity parse of sibling .vyx bodies.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
SRC="$ROOT/bootstrap_compiler/src/core/project/build_system.vyx"
fail=0

if ! grep -q 'fn build_file_vyi_path' "$SRC"; then
  echo "FAIL: missing per-file vyi path"
  fail=1
fi
if ! grep -q 'fn build_sibling_file_vyi_paths' "$SRC"; then
  echo "FAIL: missing sibling vyi list"
  fail=1
fi
if ! grep -q 'fn build_sibling_file_vyi_stamp' "$SRC"; then
  echo "FAIL: missing sibling vyi content stamp"
  fail=1
fi
if ! grep -q 'build_file_content_stamp' "$SRC"; then
  echo "FAIL: missing content-addressed source stamp"
  fail=1
fi

# The old peer pass parsed sibling .vyx via --unit-sources. That must be gone
# from build_emit_peer_vyi_files.
awk '
  /fn build_emit_peer_vyi_files/ { in_fn=1 }
  in_fn && /^fn build_/ && !/fn build_emit_peer_vyi_files/ { in_fn=0 }
  in_fn && /--unit-sources/ {
    print "FAIL: peer vyi emit still passes --unit-sources sibling .vyx"
    bad=1
  }
  in_fn && /--emit=vyi/ {
    print "FAIL: peer vyi emit still spawns a compiler"
    bad=1
  }
  END { if (bad) exit 1 }
' "$SRC" || fail=1

if grep -n 'fn build_cache_record' -A 40 "$SRC" | grep -q 'build_dep_stamp_dir(src_dir'; then
  echo "FAIL: obj cache still stamps the whole source directory"
  fail=1
fi

if [ "$fail" != 0 ]; then
  echo "mem peer_file_vyi FAIL"
  exit 1
fi
echo "OK: peer_file_vyi"
exit 0
