#!/usr/bin/env bash
# R1 / SPLIT2 gate: the mechanical extraction must not change what the
# compiler emits.
#
#   1. Boundary  — the moved symbols are defined exactly once, in the new
#                  module, and no longer in llvm_mir_lower.
#   2. IR hash   — hello.vyx --emit=ir is byte-identical to the pre-SPLIT2
#                  compiler (recorded in baseline_ir_sha256.txt).
#   3. Gate A    — the mir2.unit counts still match the frozen baseline.
#
# Usage: bash probes/gates/cgu-parallel/split2.sh
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
SRC="$ROOT/bootstrap_compiler/src"
BOOT="${BOOT:-$ROOT/bootstrap_compiler/out/boot.exe}"

# `--src=file` only accepts Windows-style paths on Windows hosts, so every
# compiler invocation below runs from ROOT with relative paths.
WORK_REL="probes/gates/cgu-parallel/.work/split2"
HELLO_REL="bootstrap_compiler/docs/cgu-parallel-r0/hello.vyx"
cd "$ROOT" || exit 2

fail=0
ok()   { echo "  PASS  $1"; }
bad()  { echo "  FAIL  $1"; fail=1; }

mkdir -p "$WORK_REL"

echo "== R1/SPLIT2 gate =="
echo "boot: $BOOT"
echo

# ── 1. boundary ──────────────────────────────────────────────────────
echo "-- boundary: each moved symbol has exactly one definition --"

define_sites() {   # $1 = symbol name
    grep -rn --include=*.vyx -E "^(public )?fn $1\b" "$SRC" | sed "s|$SRC/||"
}

BOUNDARY_SYMS="
llvm_mir_env_value llvm_mir_env_enabled llvm_mir_parse_positive_i32 llvm_mir_parse_nonneg_i32
llvm_cgu_clamp_threads llvm_cgu_default_threads llvm_cgu_cli_threads llvm_cgu_env_emit_threads
llvm_cgu_parallel_mode llvm_cgu_parallel_mode_name llvm_cgu_object_path
cgu_par_mode_off cgu_par_mode_full cgu_pipe_max_threads
cgu_ws_kind_emit llvm_cgu_fail_unit cgu_pipe_active
llvm_cgu_trace_task llvm_cgu_trace_rss
cgu_pipe_start cgu_pipe_submit cgu_pipe_finish cgu_pipe_dispose cgu_pipe_unit_ok
"

for sym in $BOUNDARY_SYMS; do
    sites="$(define_sites "$sym")"
    n="$(printf '%s\n' "$sites" | grep -c .)"
    if [ "$n" -ne 1 ]; then
        bad "$sym defined $n times: $(printf '%s' "$sites" | tr '\n' ' ')"
    elif printf '%s' "$sites" | grep -q '^codegen/llvm_lower.vyx:'; then
        bad "$sym still defined in llvm_mir_lower"
    fi
done
[ "$fail" -eq 0 ] && ok "every moved symbol has exactly one definition outside llvm_lower"

echo
echo "-- boundary: llvm_lower references them as a consumer only --"
for sym in llvm_mir_env_value llvm_cgu_parallel_mode cgu_pipe_start cgu_pipe_submit \
           cgu_pipe_finish cgu_pipe_dispose cgu_pipe_unit_ok cgu_pipe_active \
           llvm_cgu_trace_task llvm_cgu_trace_rss cgu_ws_kind_emit; do
    if grep -q "\b$sym\b" "$SRC/codegen/llvm_lower.vyx"; then
        ok "llvm_lower consumes $sym"
    else
        echo "  NOTE  llvm_lower does not reference $sym (allowed)"
    fi
done

# ── 2. IR hash ───────────────────────────────────────────────────────
echo
echo "-- IR: hello.vyx --emit=ir byte-identical to pre-SPLIT2 --"
WANT="$(tr -d ' \r\n' < "$HERE/baseline_ir_sha256.txt")"
IR="$WORK_REL/hello.split2.ll"
if [ ! -x "$BOOT" ]; then
    bad "boot not found at $BOOT"
else
    "$BOOT" --src=file "$HELLO_REL" --emit=ir -o "$IR" >"$WORK_REL/ir.log" 2>&1
    rc=$?
    if [ "$rc" -ne 0 ]; then
        bad "hello --emit=ir exited $rc (see $WORK_REL/ir.log)"
    else
        GOT="$(sha256sum "$IR" | cut -d' ' -f1)"
        if [ "$GOT" = "$WANT" ]; then
            ok "sha256 $GOT"
        else
            bad "sha256 $GOT != pre-SPLIT2 $WANT"
        fi
    fi
fi

# ── 3. gate A ────────────────────────────────────────────────────────
echo
echo "-- gate A: mir2.unit counts --"
MIR="$WORK_REL/hello.mir2.txt"
if [ -x "$BOOT" ]; then
    "$BOOT" --src=file "$HELLO_REL" --dump-mir2 >"$MIR" 2>"$WORK_REL/mir2.err"
    line="$(grep '^mir2.unit' "$MIR" | head -n 1)"
    want="mir2.unit functions=1 blocks=1 locals=1 places=1 values=6 instrs=2 cases=0 types=5"
    if [ "$line" = "$want" ]; then
        ok "$line"
    else
        bad "gate A drift: '$line' (see $WORK_REL/mir2.err)"
    fi
fi

echo
if [ "$fail" -eq 0 ]; then
    echo "SPLIT2: OK"
else
    echo "SPLIT2: FAIL"
fi
exit "$fail"
