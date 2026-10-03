#!/usr/bin/env bash
# rustc-style codegen units: collect once, merge modules to N LLVM modules.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
SRC="$ROOT/bootstrap_compiler/src/codegen/llvm_lower.vyx"
BS="$ROOT/bootstrap_compiler/src/core/build_system.vyx"
fail=0

if ! grep -q 'fn llvm_mir_codegen_unit_limit' "$SRC"; then
    echo "FAIL: missing codegen-unit limit (default 16)"
    fail=1
fi
if ! grep -q 'fn llvm_cgu_find_root' "$SRC"; then
    echo "FAIL: missing CGU merge"
    fail=1
fi
if ! grep -q 'fn cgu_collect_from_shells' "$SRC"; then
    echo "FAIL: missing cheap shell CGU collect (rustc item list, not 2x HIR)"
    fail=1
fi
if ! grep -q 'fn build_expand_cgu_object_lists' "$BS"; then
    echo "FAIL: linker does not consume .cgu.list"
    fail=1
fi
if grep -q 'flush every N' "$SRC"; then
    echo "FAIL: count-based LLVM flush is not rustc CGU partition"
    fail=1
fi

if [ "$fail" != 0 ]; then
    echo "mem cgu_partition FAIL"
    exit 1
fi
echo "OK: cgu_partition"
exit 0
