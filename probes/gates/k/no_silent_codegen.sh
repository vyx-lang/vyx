#!/usr/bin/env bash
# WP-K: lower_pipeline has no *_silent; a user type error prints Exxxx
# and does not say "code generation failed".
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
PIPE="$ROOT/bootstrap_compiler/src/codegen/lower_pipeline.vyx"
MAIN="$ROOT/bootstrap_compiler/src/core/driver/main.vyx"
BOOT="$ROOT/bootstrap_compiler/out/boot"
fail=0

if grep -nE 'resolve_unit_silent|ownership_resolve_unit_silent|verify_unit_silent' "$PIPE"; then
  echo "FAIL: silent helpers still called in lower_pipeline.vyx"
  fail=1
fi
if ! grep -q 'lower_pipeline_prepare_hir' "$MAIN"; then
  echo "FAIL: driver does not call lower_pipeline_prepare_hir"
  fail=1
fi
if grep -n 'code generation failed: HIR semantic' "$MAIN"; then
  echo "FAIL: driver still wraps HIR resolve as codegen failure"
  fail=1
fi

if [ ! -x "$BOOT" ]; then
  echo "FAIL: missing $BOOT"
  exit 1
fi
export LLVM_ROOT="${LLVM_ROOT:-/usr/lib/llvm-22}"
export PATH="$LLVM_ROOT/bin:$PATH"
export LD_LIBRARY_PATH="$ROOT/bootstrap_compiler/out${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

SRC="$(mktemp /tmp/k_typeerr.XXXXXX.vyx)"
printf 'fn main() -> i32 { return "x"; }\n' > "$SRC"
LOG="$("$BOOT" --src=file "$SRC" --emit=ir -o /tmp/k_typeerr.ll 2>&1 || true)"
rm -f "$SRC"

if ! printf '%s\n' "$LOG" | grep -qE 'error: E[0-9]{4}:'; then
  echo "FAIL: type error missing E[0-9]{4}"
  echo "$LOG" | head -30
  fail=1
fi
if printf '%s\n' "$LOG" | grep -q 'code generation failed'; then
  echo "FAIL: type error wrapped as code generation failed"
  echo "$LOG" | head -30
  fail=1
fi

if [ "$fail" != 0 ]; then
  echo "k FAIL"
  exit 1
fi
echo "OK: no_silent_codegen"
exit 0
