#!/usr/bin/env bash
# WP-MEM: build_task_memory_weight_for_fields must not hard-code compiler
# filenames as weight 10. Size + rootchunk are the only strategies.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
SRC="$ROOT/bootstrap_compiler/src/core/project/build_system.vyx"
fail=0

if ! grep -q 'build_dci_descriptor_stamp' "$SRC"; then
  echo "FAIL: missing content-addressed DCIB stamp"
  fail=1
fi
if ! grep -q 'let size = fileSize(clean_src);' "$SRC"; then
  echo "FAIL: missing size-based weight"
  fail=1
fi

# Only the weight helper is in scope. Other filename checks (chunking) may remain.
awk '
  /fn build_task_memory_weight_for_fields/ { in_fn=1 }
  in_fn && /fn build_task_memory_weight\(/ { in_fn=0 }
  in_fn && /endsWith\("llvm_lower.vyx"\)|endsWith\("hir_builder.vyx"\)|endsWith\("mir_builder.vyx"\)/ {
    print "FAIL: filename weight list still in build_task_memory_weight_for_fields"
    bad=1
  }
  in_fn && /return 10;/ {
    print "FAIL: hardcoded weight 10 still in build_task_memory_weight_for_fields"
    bad=1
  }
  END { if (bad) exit 1 }
' "$SRC" || fail=1

if [ "$fail" != 0 ]; then
  echo "mem FAIL"
  exit 1
fi
echo "OK: no_filename_weight"
exit 0
