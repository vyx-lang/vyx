#!/usr/bin/env bash
# WP-ESC: fail-open escapes are documented concessions, not silent.
# string=Copy / null-or-rawptr universal / last-segment name eq stay
# because each breaks self-host fixpoint if closed.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
SEMA="$ROOT/bootstrap_compiler/src/core/sema.vyx"
POL="$ROOT/bootstrap_compiler/src/core/policy.vyx"
fail=0

need() {
  if ! grep -q "$2" "$1"; then
    echo "FAIL: missing $3 in $1"
    fail=1
  fi
}

need "$SEMA" 'WP-ESC concession (extreme freeze): primitive `string` stays Copy' "string=Copy concession"
need "$SEMA" 'WP-ESC concession (fixpoint): a missing TypeRef' "null TypeRef concession"
need "$SEMA" 'WP-ESC concession (fixpoint): either side rawptr is still universal' "rawptr concession"
need "$POL" 'WP-ESC concession: last dotted segment' "last-segment concession"
need "$POL" 'fn policy_type_name_equal_qualified' "qualified name helper"

if ! grep -q 'if (k == ty_STRING()) { return true; }' "$SEMA"; then
  echo "FAIL: string=Copy semantic changed without a new WP"
  fail=1
fi
if ! grep -A2 'if (expected == null || actual == null)' "$SEMA" | grep -q 'return true;'; then
  echo "FAIL: null TypeRef semantic changed without a new WP"
  fail=1
fi
if ! grep -A3 'if (ek == ty_RAWPTR() || ak == ty_RAWPTR())' "$SEMA" | grep -q 'return true;'; then
  echo "FAIL: rawptr semantic changed without a new WP"
  fail=1
fi

if [ "$fail" != 0 ]; then
  echo "esc FAIL"
  exit 1
fi
echo "OK: esc concessions documented; string/null/rawptr semantics unchanged"
exit 0
