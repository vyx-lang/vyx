#!/usr/bin/env bash
# One Vyx target = one compiler process (rustc crate), not one process per .vyx.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
SRC="$ROOT/bootstrap_compiler/src/core/project/build_system.vyx"
fail=0

if ! grep -q 'let crate_compile_vyx = mir_codegen_enabled && !mir_partition_enabled' "$SRC"; then
  echo "FAIL: missing crate_compile_vyx"
  fail=1
fi
if ! grep -q 'fn build_crate_cc_unit_count' "$SRC"; then
  echo "FAIL: missing crate+cc unit count"
  fail=1
fi
if ! grep -q 'group_key = "crate:" + target_name' "$SRC"; then
  echo "FAIL: missing crate group key"
  fail=1
fi
if ! grep -q 'let split_module_units = false' "$SRC"; then
  echo "FAIL: split_module_units is still the default Vyx split"
  fail=1
fi
if ! grep -q 'compile_uses_project_unit_sources = true' "$SRC"; then
  echo "FAIL: crate compile does not force --project-unit-sources"
  fail=1
fi
HIR="$ROOT/bootstrap_compiler/src/hir/builder/declarations/declaration_scopes.vyx"
if ! grep -q 'fn decl_is_foreign_import' "$HIR"; then
  echo "FAIL: missing crate-member vs foreign-import split"
  fail=1
fi
if grep -q 'let split_module_units = mir_codegen_enabled' "$SRC"; then
  echo "FAIL: split_module_units still derived from multi-source groups"
  fail=1
fi

if [ "$fail" != 0 ]; then
  echo "mem crate_one_process FAIL"
  exit 1
fi
echo "OK: crate_one_process"
exit 0
