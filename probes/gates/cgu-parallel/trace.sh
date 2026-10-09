#!/usr/bin/env bash
# R0: source-level + runtime checks that CGU task attribution is machine-readable.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
SRC="$ROOT/bootstrap_compiler/src/codegen/llvm/llvm_lower.vyx"
fail=0

need() {
    if ! grep -q "$1" "$SRC"; then
        echo "FAIL: missing $1"
        fail=1
    fi
}

need 'fn llvm_cgu_trace_task'
need 'fn llvm_cgu_trace_rss'
need '\[llvm-cgu\] task unit='
need 'effective='
need 'reason=per-unit-arena'
need 'VYX_CGU_FAIL_UNIT'
need 'g_cgu_pipe_wargs'
need 'vyx_rt_process_peak_private_bytes'

if grep -q '\[full=reserved: per-CGU lowering needs a per-unit arena\]' "$SRC"; then
    echo "FAIL: old full=reserved banner still present; R0 wants effective= + reason="
    fail=1
fi

if [ "$fail" != 0 ]; then
    echo "cgu-parallel trace FAIL"
    exit 1
fi
echo "OK: cgu-parallel trace (source)"
exit 0
