#!/usr/bin/env bash
# WP-MEM: Vyx obj slots + skip-to-fit scheduler (rustc/zig-style).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
SRC="$ROOT/bootstrap_compiler/src/core/build_system.vyx"
fail=0

if ! grep -q 'VYX_BUILD_VYX_SLOTS' "$SRC"; then
  echo "FAIL: missing VYX_BUILD_VYX_SLOTS"
  fail=1
fi
if ! grep -q 'fn build_find_launchable_task_index' "$SRC"; then
  echo "FAIL: missing skip-to-fit finder"
  fail=1
fi
if ! grep -q 'fn build_remove_line_at' "$SRC"; then
  echo "FAIL: missing remaining-task splice"
  fail=1
fi
if ! grep -q 'return 2;' "$SRC"; then
  echo "FAIL: default Vyx obj slots is not 2"
  fail=1
fi
if grep -n 'fn build_memory_slot_limit' -A 8 "$SRC" | grep -q 'var out = jobs as i64'; then
  echo "FAIL: memory_slot_limit still defaults to jobs"
  fail=1
fi

if [ "$fail" != 0 ]; then
  echo "mem skip-fit FAIL"
  exit 1
fi
echo "OK: vyx_slots_skip_fit"
exit 0
