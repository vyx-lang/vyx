#!/usr/bin/env bash
# R4/§9.2 gate: the memory budget is enforced, and enforcing it cannot be
# bypassed by falling back to inline emission.
#
# Admission covers queued *and* running units; an oversize unit must be
# *reported* rather than left waiting forever:
#
#   若单任务预留 > 全局预算，返回明确错误（需要更高预算或更细分），不能挂住。
#
# The failure this gate exists to catch was real and subtle: `cgu_pipe_submit`
# used to answer `false` both for "the budget refuses this unit" and for "the
# pool is not running".  The emitter treats `false` as "emit it inline", so an
# oversized unit was emitted anyway -- the declared budget was advisory, and the
# census then read the inline unit as rc == -1 and reported a family that was
# actually complete as incomplete.  Three assertions pin the fixed behaviour:
#
#   1. default      no budget set -> `bytes=0`, nothing enforced, build succeeds.
#   2. oversize     a budget smaller than one unit's reservation must FAIL the
#                   build, name the reason, count it as `oversize`, and publish
#                   nothing -- not "fail and leave half a family on disk".
#   3. admitting    a budget just above one unit's reservation must build, keep
#                   `oversize=0`, and still publish the whole family.
#
# The single reserved unit's size is read out of the run rather than hardcoded,
# so a future recalibration of the reservation constants does not silently turn
# assertion 2 into "budget was fine after all" -- which would make the gate pass
# for the wrong reason.
#
# Usage: bash probes/gates/cgu-parallel/budget.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
FIX="$ROOT/probes/gates/cgu-parallel/fixture"
BOOT="${BOOT:-$ROOT/bootstrap_compiler/out/boot}"
if [ -x "${BOOT}.exe" ] && [ ! -x "$BOOT" ]; then BOOT="${BOOT}.exe"; fi
if [ ! -x "$BOOT" ]; then
    echo "FAIL: missing compiler $BOOT"
    exit 1
fi

# LLVM_ROOT must be a Windows path (`E:/Dev/...`), not MSYS (`/e/Dev/...`).
if [ -z "${LLVM_ROOT:-}" ]; then
    ROOT_WIN="$(cygpath -m "$ROOT" 2>/dev/null || printf '%s' "$ROOT")"
    LLVM_ROOT="$ROOT_WIN/clang"
fi
export LLVM_ROOT
export PATH="$ROOT/clang/bin:${LLVM_ROOT}/bin:${PATH:-}"

WORKDIR="${WORKDIR:-$ROOT/out/cgu_budget_probe}"
rm -rf "$WORKDIR"
mkdir -p "$WORKDIR"

fail=0
note() { echo "  $1"; }
bad()  { echo "  FAIL  $1"; fail=1; }
num()  { printf '%s' "$1" | sed -n "s/.*$2=\([0-9]*\).*/\1/p"; }

run_case() {             # run_case <subdir> <logname> [env...]
    local dest="$WORKDIR/$1" log="$WORKDIR/$2"
    shift 2
    mkdir -p "$dest"
    cp "$FIX/Vyx.toml" "$dest/"
    cp -R "$FIX/src" "$dest/"
    local rc
    if ( cd "$dest" && env "$@" "$BOOT" build ) >"$log" 2>&1; then rc=0; else rc=$?; fi
    printf '%s' "$rc"
}

budget_line() { grep -m1 '^\[llvm-cgu\] budget' "$1" || true; }

echo "== R4/§9.2 memory-budget gate =="
echo "boot: $BOOT"
echo

# ── 1: default, nothing enforced ─────────────────────────────────────
echo "-- 1/3 default (no budget) --"
rc="$(run_case default default.log VYX_PHASE_SUMMARY=1 VYX_CGU_PARALLEL=lazy VYX_CGU_THREADS=2)"
if [ "$rc" != "0" ]; then
    bad "unbudgeted build failed (rc=$rc)"
    tail -n 10 "$WORKDIR/default.log"
else
    bl="$(budget_line "$WORKDIR/default.log")"
    if [ -z "$bl" ]; then
        bad "no '[llvm-cgu] budget' line -- is §9.2 wired in?"
    else
        note "$bl"
        if [ "$(num "$bl" bytes)" != "0" ]; then
            bad "bytes=$(num "$bl" bytes) with VYX_CGU_MEM_BUDGET_MB unset (expected 0)"
        fi
        if [ "$(num "$bl" oversize)" != "0" ]; then
            bad "oversize=$(num "$bl" oversize) on an unbudgeted build"
        fi
    fi
fi

# ── the reservation actually used, read out of the run ───────────────
res="$(grep -m1 -o '^\[llvm-cgu\] plan-unit unit=1 members=[0-9]* cost=[0-9]* reserve=[0-9]*' \
        "$WORKDIR/default.log" 2>/dev/null | sed -n 's/.*reserve=\([0-9]*\)/\1/p' || true)"
if [ -z "$res" ]; then
    # The plan dump is opt-in; re-read it from an explicit run so the gate does
    # not depend on VYX_CGU_PLAN_DUMP being left on by accident.
    rc="$(run_case reserved reserved.log VYX_PHASE_SUMMARY=1 VYX_CGU_PLAN_DUMP=1 \
                  VYX_CGU_PARALLEL=lazy VYX_CGU_THREADS=2)"
    res="$(grep -o '^\[llvm-cgu\] plan-unit unit=1 members=[0-9]* cost=[0-9]* reserve=[0-9]*' \
            "$WORKDIR/reserved.log" 2>/dev/null | sed -n 's/.*reserve=\([0-9]*\)/\1/p' || true)"
fi
if [ -z "$res" ] || [ "$res" = "0" ]; then
    bad "could not read unit 1's reservation from the run; the gate cannot size its budgets"
    echo
    echo "budget: FAIL"
    exit 1
fi
res_mb=$(( (res + 1048575) / 1048576 ))
echo "  unit 1 reservation: $res B (~${res_mb} MiB)"
oversize_mb=$(( res_mb / 2 ))          # strictly below one unit's reservation
admit_mb=$(( res_mb + 1 ))             # admits exactly one outstanding unit
[ "$oversize_mb" -lt 1 ] && oversize_mb=1
echo

# ── 2: oversize -> hard failure, nothing published ───────────────────
echo "-- 2/3 oversize budget (${oversize_mb} MiB < ${res_mb} MiB) --"
rc="$(run_case oversize oversize.log VYX_PHASE_SUMMARY=1 VYX_CGU_PARALLEL=lazy \
              VYX_CGU_THREADS=2 "VYX_CGU_MEM_BUDGET_MB=$oversize_mb")"
if [ "$rc" = "0" ]; then
    bad "build succeeded with a budget smaller than one unit's reservation"
    echo "  ----- budget lines -----"
    grep '^\[llvm-cgu\] budget' "$WORKDIR/oversize.log" | sed 's/^/  /' || true
else
    note "build failed (rc=$rc), as §9.2 requires"
    if ! grep -q 'budget reject unit=1' "$WORKDIR/oversize.log"; then
        bad "no 'budget reject unit=' line -- the refusal reason is not on the record"
    else
        note "$(grep -m1 'budget reject' "$WORKDIR/oversize.log")"
    fi
    if ! grep -q 'more than the whole budget' "$WORKDIR/oversize.log"; then
        bad "the failure does not name the budget as the reason (could be failing for something else)"
    fi
    # The half-publication check has to look at the filesystem, not the log: a
    # refused unit that still wrote its object is exactly the bug this gate is
    # about, and it would be invisible in the log.
    leftover="$(find "$WORKDIR/oversize" \( -name '*.stage.obj*' -o -name '*.cgu.list' \) 2>/dev/null | head -n 5)"
    if [ -n "$leftover" ]; then
        bad "refused build left staged artifacts behind:"
        printf '%s\n' "$leftover" | sed 's/^/        /'
    else
        note "no staged objects or manifests left behind"
    fi
    if grep -q '^\[llvm-cgu\] result units=' "$WORKDIR/oversize.log"; then
        bad "a result/manifest line was printed for a refused build"
    fi
    if [ -e "$WORKDIR/oversize/target/cgu_r0_fixture.exe" ] \
       || [ -e "$WORKDIR/oversize/target/cgu_r0_fixture" ]; then
        bad "refused build produced a target binary"
    fi
    if ! grep -q 'oversize=1' "$WORKDIR/oversize.log" \
       && ! grep -q 'oversize=[1-9]' "$WORKDIR/oversize.log"; then
        bad "oversize counter did not record the refusal"
    fi
fi

# ── 3: admitting budget -> builds, whole family, no budget bypass ────
echo
echo "-- 3/3 admitting budget (${admit_mb} MiB >= ${res_mb} MiB) --"
rc="$(run_case admit admit.log VYX_PHASE_SUMMARY=1 VYX_CGU_PARALLEL=lazy \
              VYX_CGU_THREADS=2 "VYX_CGU_MEM_BUDGET_MB=$admit_mb")"
if [ "$rc" != "0" ]; then
    bad "build failed with an admitting budget (rc=$rc)"
    tail -n 10 "$WORKDIR/admit.log"
else
    bl="$(budget_line "$WORKDIR/admit.log")"
    note "$bl"
    if [ "$(num "$bl" oversize)" != "0" ]; then
        bad "oversize=$(num "$bl" oversize) on an admitting budget"
    fi
    resline="$(grep -m1 '^\[llvm-cgu\] result units=' "$WORKDIR/admit.log" || true)"
    if [ -z "$resline" ]; then
        bad "no result line -- an admitting budget must still publish the family"
    else
        note "$resline"
        u="$(num "$resline" units)"
        p="$(num "$resline" present)"
        ok="$(num "$resline" ok)"
        if [ "${u:-X}" != "${p:-Y}" ] || [ "${ok:-0}" != "1" ]; then
            bad "family incomplete under an admitting budget: units=$u present=$p ok=$ok"
        fi
    fi
fi

echo
if [ "$fail" -eq 0 ]; then
    echo "budget: OK"
else
    echo "budget: FAIL"
fi
exit "$fail"
