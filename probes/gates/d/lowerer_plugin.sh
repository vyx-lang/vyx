#!/usr/bin/env bash
# WP-D: LLVM is the acceptance Lowerer plugin; C++ source is optional.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
fail=0
need() {
  if ! grep -q "$2" "$1"; then
    echo "FAIL: missing $3 in $1"
    fail=1
  fi
}
need "$ROOT/bootstrap_compiler/src/codegen/llvm_lower.vyx" \
  'fn llvm_mir_lowerer_is_acceptance_backend() -> bool { return true; }' \
  'llvm acceptance plugin'
need "$ROOT/bootstrap_compiler/src/codegen/mir_cpp_lower.vyx" \
  'fn mir_cpp_plugin_is_acceptance_backend() -> bool' \
  'cpp optional plugin'
if grep -A2 'fn mir_cpp_plugin_is_acceptance_backend' \
     "$ROOT/bootstrap_compiler/src/codegen/mir_cpp_lower.vyx" | grep -q 'return true;'; then
  echo "FAIL: C++ plugin must not be an acceptance backend"
  fail=1
fi
if [ "$fail" -eq 0 ]; then
  echo "OK: LLVM is the acceptance Lowerer; C++ plugin is optional"
else
  exit 1
fi
