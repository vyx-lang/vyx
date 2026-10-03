#!/usr/bin/env bash
# LLVM lowerer intern: type_id_by_text must not be a 3-pass linear scan.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
SRC="$ROOT/bootstrap_compiler/src/codegen/llvm_lower.vyx"
fail=0

if ! grep -q 'fn ensure_type_text_index' "$SRC"; then
  echo "FAIL: missing type text intern index"
  fail=1
fi
if ! grep -q 'interned_rawptr_type_id' "$SRC"; then
  echo "FAIL: missing interned rawptr"
  fail=1
fi
if ! grep -q 'option_abi_cache' "$SRC"; then
  echo "FAIL: missing Option ABI cache"
  fail=1
fi
if ! grep -q 'result_abi_cache' "$SRC"; then
  echo "FAIL: missing Result ABI cache"
  fail=1
fi

awk '
  /public fn type_id_by_text\(/ { in_fn=1 }
  in_fn && /^    public fn / && !/public fn type_id_by_text\(/ { in_fn=0 }
  in_fn && /self.ensure_type_text_index\(\)/ { intern=1 }
  END {
    if (!intern) {
      print "FAIL: type_id_by_text does not intern via ensure_type_text_index"
      exit 1
    }
  }
' "$SRC" || fail=1

if [ "$fail" != 0 ]; then
  echo "mem type_text_index FAIL"
  exit 1
fi
echo "OK: type_text_index"
exit 0
