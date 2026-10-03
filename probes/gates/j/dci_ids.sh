#!/usr/bin/env bash
# WP-J: declaration bind is auditable; same last segment / different
# namespace stays distinct after intern.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
export LLVM_ROOT="${LLVM_ROOT:-/usr/lib/llvm-22}"
export PATH="$LLVM_ROOT/bin:$PATH"
BOOT="$ROOT/bootstrap_compiler/out/boot"
fail=0

need() {
  if ! grep -q "$2" "$1"; then
    echo "FAIL: missing $3 in $1"
    fail=1
  fi
}

need "$ROOT/bootstrap_compiler/src/core/dci_ids.vyx" 'fn dci_ids_intern_type' "intern"
need "$ROOT/bootstrap_compiler/src/codegen/dci_abi.vyx" 'dci_ids_lookup_type' "hot path by id"
need "$ROOT/bootstrap_compiler/src/hir/dci_binder.vyx" 'dci_ids_note_declaration_noop()' "auditable declaration"

if [ ! -x "$BOOT" ]; then
  echo "FAIL: missing $BOOT"
  exit 1
fi
export LD_LIBRARY_PATH="$ROOT/bootstrap_compiler/out${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

if ! LOG="$("$BOOT" --src=file "$ROOT/probes/gates/j/hello.vyx" \
      --dci "$ROOT/probes/gates/j/declaration.json" \
      --emit=ir -o /tmp/j_dci.ll 2>&1)"; then
  echo "FAIL: declaration descriptor compilation failed"
  echo "$LOG" | head -40
  exit 1
fi

if printf '%s\n' "$LOG" | grep -q 'dci.bind lifecycle_binding=declaration status=noop'; then
  echo "FAIL: declaration bind leaked into stdout"
  echo "$LOG" | head -40
  fail=1
fi
if ! printf '%s\n' "$LOG" | grep -q 'dci.ids last_segment_not_equal ns1.Foo ns2.Foo'; then
  echo "FAIL: ns1.Foo vs ns2.Foo not reported distinct"
  echo "$LOG" | head -40
  fail=1
fi

if [ "$fail" != 0 ]; then
  echo "j FAIL"
  exit 1
fi
echo "OK: dci_ids"
exit 0
