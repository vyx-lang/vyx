#!/usr/bin/env bash
# R8.0 / §7 gate: a non-zero reservation must actually enter the admission
# decision in full mode.
#
# Reservations must be initialized before tasks enter admission:
#
#   预留初始化顺序可疑：set_est 在 pool_start 之前；pool_start 才分配并清零
#   g_full_est …… 必须先修探针和初始化顺序，不能基于失效预算评价扩展性
#   …… 尤其需要覆盖两个单独可运行、合计超预算的任务，验证它们不会同时准入。
#
# The failure this gate exists to catch is the one it was written against:
# `cgu_full_set_est` early-returns while `g_full_est == null`, and the table is
# only allocated inside `cgu_full_pool_start`, which runs *after* the estimate
# loop.  Every estimate was therefore dropped, `cgu_full_claim` compared
# `g_full_reserved + 0 <= budget`, and the declared budget was never enforced in
# full mode -- `peak-reserved=0` next to `admitted=4` on a 4-unit fixture.
#
# Three assertions, none of which can pass "for the wrong reason":
#
#   1. generous    a budget many times one unit's reservation: the build must
#                  admit every unit AND report `peak-reserved > 0`.  This is the
#                  assertion that fails while the estimates are dropped.
#   2. serialising a budget between 1x and 2x one unit's reservation: every unit
#                  must still be admitted (no starvation, no deadlock), but the
#                  peak that was reached must be at least one unit's worth and
#                  never exceed the budget -- i.e. two units did not get in at
#                  once.
#   3. oversize    a budget below one unit's reservation must still fail the
#                  build, name the reason, and admit nothing.
#
# Per-unit reservations are read out of the run (`VYX_CGU_PLAN_DUMP=1`) rather
# than hardcoded, so recalibrating the constants cannot turn assertion 1 into
# "the budget was fine after all".  Assertion 1 also demands `effective=full`
# and `units >= 2`: a run that silently fell back to lazy, or that only ever had
# one unit, would otherwise read as a pass.
#
# Usage: bash probes/gates/cgu-parallel/r8/budget_init.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
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
if [ -n "${OS:-}" ] || [ -d /c ]; then
    export OS="${OS:-Windows_NT}"
fi

WORKDIR="${WORKDIR:-$ROOT/out/cgu_r8_budget_init}"
rm -rf "$WORKDIR"
mkdir -p "$WORKDIR"

fail=0
note() { echo "  $1"; }
bad()  { echo "  FAIL  $1"; fail=1; }

# num <text> <key> -> the integer after `key=` on the matching line.
num() { printf '%s\n' "$1" | sed -n "s/.*$2=\([0-9]*\).*/\1/p" | head -1; }

have() { printf '%s\n' "$1" | grep -q "$2"; }

run_case() {              # run_case <subdir> <logname> <budget_mb>
    local dest="$WORKDIR/$1" log="$WORKDIR/$2" mb="$3"
    mkdir -p "$dest"
    cp "$FIX/Vyx.toml" "$dest/"
    cp -R "$FIX/src" "$dest/"
    local rc
    if ( cd "$dest" && env VYX_PHASE_SUMMARY=1 VYX_CGU_PLAN_DUMP=1 \
             VYX_CGU_PARALLEL=full VYX_CGU_THREADS=2 \
             VYX_CGU_MEM_BUDGET_MB="$mb" "$BOOT" build ) >"$log" 2>&1
    then rc=0; else rc=$?; fi
    printf '%s' "$rc"
}

echo "== R8.0 budget-initialisation gate =="
echo "boot: $BOOT"
echo

# ── 1: generous budget -- every unit admitted, reservation non-zero ───
echo "-- 1/3 generous budget: reservation must reach the admission table --"
rc="$(run_case generous generous.log 256)"
log="$(cat "$WORKDIR/generous.log")"
units="$(printf '%s\n' "$log" | sed -n 's/^\[llvm-cgu\] units=\([0-9]*\) .*/\1/p' | head -1)"
eff="$(printf '%s\n' "$log" | sed -n 's/.*effective=\([a-z]*\).*/\1/p' | head -1)"
bl="$(printf '%s\n' "$log" | grep -m1 '^\[llvm-cgu\] budget(full)' || true)"
admitted="$(num "$bl" admitted)"
peak="$(num "$bl" peak-reserved)"
echo "     units=${units:-?} effective=${eff:-?} admitted=${admitted:-?} peak-reserved=${peak:-?}"
if [ "$rc" != "0" ]; then bad "generous build failed (rc=$rc)"; fi
if [ -z "$units" ] || [ "$units" -lt 2 ]; then
    bad "probe did not see >=2 units (units=${units:-none}); the fixture changed"
fi
if [ "${eff:-}" != "full" ]; then
    bad "effective=${eff:-none}, expected full -- the run did not exercise cgu_full_claim"
fi
if [ -z "$bl" ]; then bad "no \`[llvm-cgu] budget(full)\` line; VYX_PHASE_SUMMARY output changed"; fi
if [ "${admitted:-0}" != "${units:-0}" ]; then
    bad "admitted=${admitted:-0} != units=${units:-0}"
fi
if [ "${peak:-0}" -le 0 ]; then
    bad "peak-reserved=${peak:-0}: estimates never entered the admission table (§2 init order)"
fi

# Per-unit reservations, read from this same run.
min_reserve="$(printf '%s\n' "$log" \
    | sed -n 's/^\[llvm-cgu\] plan-unit .* reserve=\([0-9]*\).*/\1/p' \
    | sort -n | head -1)"
if [ -z "$min_reserve" ] || [ "$min_reserve" -le 0 ]; then
    bad "no per-unit reserve= lines; VYX_CGU_PLAN_DUMP output changed"
    min_reserve=0
fi
echo "     min unit reserve=${min_reserve} bytes"

# ── 2: 1x < budget < 2x -- units serialise, nobody starves ───────────
echo "-- 2/3 serialising budget: two units must not be admitted at once --"
mb=$(( (min_reserve * 3 / 2) / (1024 * 1024) ))
if [ "$mb" -lt 1 ]; then mb=1; fi
rc="$(run_case serial serial.log "$mb")"
log="$(cat "$WORKDIR/serial.log")"
units2="$(printf '%s\n' "$log" | sed -n 's/^\[llvm-cgu\] units=\([0-9]*\) .*/\1/p' | head -1)"
bl="$(printf '%s\n' "$log" | grep -m1 '^\[llvm-cgu\] budget(full)' || true)"
admitted="$(num "$bl" admitted)"
peak="$(num "$bl" peak-reserved)"
bytes="$(num "$bl" bytes)"
waits="$(num "$bl" budget-waits)"
echo "     budget_mb=${mb} bytes=${bytes:-?} admitted=${admitted:-?} peak-reserved=${peak:-?} budget-waits=${waits:-?}"
if [ "$rc" != "0" ]; then bad "serialising build failed (rc=$rc) -- a bound budget must not deadlock"; fi
if [ "${admitted:-0}" != "${units2:-0}" ]; then
    bad "admitted=${admitted:-0} != units=${units2:-0}: a unit was starved"
fi
if [ "${peak:-0}" -lt "$min_reserve" ]; then
    bad "peak-reserved=${peak:-0} < one unit (${min_reserve}): reservation still not seen"
fi
if [ "${bytes:-0}" -gt 0 ] && [ "${peak:-0}" -gt "$bytes" ]; then
    bad "peak-reserved=${peak} > budget=${bytes}: admission admitted more than the budget"
fi

# ── 3: oversize -- still a hard, named failure ───────────────────────
echo "-- 3/3 oversize: a unit bigger than the whole budget must fail loudly --"
rc="$(run_case oversize oversize.log 1)"
log="$(cat "$WORKDIR/oversize.log")"
echo "     rc=$rc"
if [ "$rc" = "0" ]; then bad "oversize build succeeded; the budget is only advisory again"; fi
if ! have "$log" "more than the whole budget"; then
    bad "oversize failure did not name the reason (see $WORKDIR/oversize.log)"
fi

echo
if [ "$fail" != "0" ]; then
    echo "RESULT: FAIL (R8.0 budget initialisation)"
    exit 1
fi
echo "RESULT: PASS (R8.0 budget initialisation)"
