#!/usr/bin/env bash
# R8.5 gate: the full-mode pool claims by cost priority and does not let a
# non-fitting unit block the ones behind it.
#
# Regression scenario: a non-fitting heavy task must not block runnable tasks.
#
#   full 按 dense 顺序取队首 …… 不按重任务优先，且大任务预算不满足时可能
#   挡住后方可运行任务
#   …… 队首暂时放不下时，允许选择后方能放下的独立任务
#
# The scenario: two heavy units and several light ones, a byte budget that
# admits exactly one heavy + one light at a time, two worker threads plus the
# producer as claimants.
#
#   old claim (dense order):   u1=heavy, then u2=heavy -> 2×heavy > budget ->
#                              the whole pool stalls behind u2; the light unit
#                              behind it never runs concurrently.
#                              peak-reserved == one heavy reservation.
#   new claim (cost priority): u1=heavy, u2 skipped, u3=light admitted.
#                              peak-reserved > one heavy reservation.
#
# Assertions:
#   1. build succeeds in full mode with >= 3 units and reservations that
#      actually exercise the skip (budget < 2×heavy);
#   2. every unit is admitted (no starvation, no deadlock);
#   3. peak-reserved never exceeds the budget;
#   4. peak-reserved is strictly above one heavy unit's reservation -- proof
#      that a light unit ran *concurrently* with the heavy one instead of
#      waiting behind the second heavy unit.
#
# Reserves come from the run itself (`VYX_CGU_PLAN_DUMP=1`); the budget is
# derived from them (heavy + light, rounded up to whole MiB), so recalibrating
# the reservation constants cannot turn this into a vacuous pass.
#
# Usage: bash probes/gates/cgu-parallel/r8/budget_order.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
BOOT="${BOOT:-$ROOT/bootstrap_compiler/out/boot}"
if [ -x "${BOOT}.exe" ] && [ ! -x "$BOOT" ]; then BOOT="${BOOT}.exe"; fi
if [ ! -x "$BOOT" ]; then
    echo "FAIL: missing compiler $BOOT"
    exit 1
fi

if [ -z "${LLVM_ROOT:-}" ]; then
    ROOT_WIN="$(cygpath -m "$ROOT" 2>/dev/null || printf '%s' "$ROOT")"
    LLVM_ROOT="$ROOT_WIN/clang"
fi
export LLVM_ROOT
export PATH="$ROOT/clang/bin:${LLVM_ROOT}/bin:${PATH:-}"
if [ -n "${OS:-}" ] || [ -d /c ]; then
    export OS="${OS:-Windows_NT}"
fi

WORKDIR="${WORKDIR:-$ROOT/out/cgu_r8_budget_order}"
rm -rf "$WORKDIR"
mkdir -p "$WORKDIR"

fail=0
note() { echo "  $1"; }
bad()  { echo "  FAIL  $1"; fail=1; }
num()  { printf '%s' "$1" | sed -n "s/.*$2=\([0-9]*\).*/\1/p"; }

# ── fixture: two heavy + six light functions, one source ─────────────
#
# The heavy bodies are long straight-line runs so their per-kinstr
# reservation term dwarfs the light ones'.  Base and per-kinstr constants
# are pinned by env below.
BIG_STMTS="${BIG_STMTS:-2000}"
GEN="$WORKDIR/src"
mkdir -p "$GEN"
{
    echo "module probe.order;"
    echo
    emit_heavy() {
        echo "public fn heavy$1() -> i32 {"
        echo "    var n: i32 = 0;"
        local i
        for ((i = 0; i < BIG_STMTS; i++)); do
            echo "    n = n + $i;"
        done
        echo "    return n - $((BIG_STMTS - 1));"
        echo "}"
    }
    emit_heavy 1
    echo
    emit_heavy 2
    k=1
    while [ "$k" -le 6 ]; do
        echo
        echo "public fn light$k(n: i32) -> i32 {"
        echo "    var acc: i32 = n;"
        echo "    var i: i32 = 0;"
        echo "    while (i < 5) { acc = acc + i + $k; i = i + 1; }"
        echo "    return acc - $k;"
        echo "}"
        k=$((k + 1))
    done
    echo
    echo "fn main() -> i32 {"
    echo "    let s = heavy1() + heavy2() + light1(0) + light2(1) + light3(2) + light4(3) + light5(4) + light6(5);"
    echo "    if (s < 0) { return 1; }"
    echo '    print("order-ok");'
    echo "    return 0;"
    echo "}"
} > "$GEN/order.vyx"

cat > "$WORKDIR/Vyx.toml" <<EOF
[package]
name = "cgu_r8_order"
version = "0.1.0"
entry = "src/order.vyx"

[build]
cache_dir = ".cache"
output_dir = "target"

[target.cgu_r8_order]
type = "executable"
entry = "src/order.vyx"
sources = ["src/order.vyx"]
EOF

run_order() {              # run_order <subdir> [extra env...]
    local sub="$1"
    shift
    local dest="$WORKDIR/$sub" log="$WORKDIR/$sub.log"
    mkdir -p "$dest"
    cp "$WORKDIR/Vyx.toml" "$dest/"
    cp -R "$GEN" "$dest/src"
    local rc
    if ( cd "$dest" && env VYX_PHASE_SUMMARY=1 VYX_CGU_PLAN_DUMP=1 \
              VYX_CGU_PARALLEL=full VYX_CGU_THREADS=2 \
              VYX_CGU_RESERVE_UNIT_MB=1 VYX_CGU_RESERVE_PER_KINSTR_KB=512 \
              "$@" "$BOOT" build ) >"$log" 2>&1
    then rc=0; else rc=$?; fi
    printf '%s' "$rc"
}

plan_of() { grep -m1 '^\[llvm-cgu\] plan mode=' "$1" || true; }
budget_of() { grep -m1 '^\[llvm-cgu\] budget(full) ' "$1" || true; }

echo "== R8 budget-order gate =="
echo "boot: $BOOT"
echo

# ── calibration pass: read the reservations, pick the budget ─────────
echo "-- calibration (no budget) --"
rc="$(run_order calib)"
if [ "$rc" != "0" ]; then
    bad "calibration build failed (rc=$rc)"
    tail -n 10 "$WORKDIR/calib.log"
    echo
    echo "budget_order: FAIL"
    exit 1
fi
mapfile -t reserves < <(grep '^\[llvm-cgu\] plan-unit ' "$WORKDIR/calib.log" \
                        | sed -n 's/.*reserve=\([0-9]*\).*/\1/p' | sort -n)
n_units="${#reserves[@]}"
if [ "$n_units" -lt 3 ]; then
    bad "calibration produced $n_units plan units (need >= 3: two heavy + light)"
    echo
    echo "budget_order: FAIL"
    exit 1
fi
r_min="${reserves[0]}"
r_2="${reserves[-2]}"      # second-largest: the other heavy unit
r_max="${reserves[-1]}"
note "reserves: min=$r_min second=$r_2 max=$r_max units=$n_units"
if [ "$r_max" -le "$((r_min + r_min / 2))" ]; then
    bad "reserves are too uniform to exercise the skip (max=$r_max min=$r_min) -- raise BIG_STMTS"
    echo
    echo "budget_order: FAIL"
    exit 1
fi
# budget = heavy + light, rounded up to whole MiB; must stay below 2×heavy
budget_bytes=$(( (r_max + r_min + 1048575) / 1048576 * 1048576 ))
if [ "$budget_bytes" -ge $(( r_max + r_2 )) ]; then
    bad "derived budget $budget_bytes would admit both heavy units (max+second=$((r_max + r_2))) -- the scenario is vacuous"
    echo
    echo "budget_order: FAIL"
    exit 1
fi
budget_mb=$(( budget_bytes / 1048576 ))
note "budget: ${budget_mb} MiB (heavy=$r_max, light=$r_min)"

echo
echo "-- 1/3 full-mode build under the budget --"
rc="$(run_order ordered VYX_CGU_MEM_BUDGET_MB=$budget_mb)"
if [ "$rc" != "0" ]; then
    bad "budgeted build failed (rc=$rc)"
    tail -n 10 "$WORKDIR/ordered.log"
else
    b="$(budget_of "$WORKDIR/ordered.log")"
    note "${b:-<no budget report>}"
    eff="$(grep -m1 'effective=' "$WORKDIR/ordered.log" | sed -n 's/.*effective=\([a-z]*\).*/\1/p')"
    if [ "$eff" != "full" ]; then
        bad "run did not report effective=full (got '${eff:-?}')"
    fi
fi

echo
echo "-- 2/3 every unit admitted --"
b_admitted="$(num "$(budget_of "$WORKDIR/ordered.log")" admitted)"
b_units="$(num "$(plan_of "$WORKDIR/ordered.log")" units)"
if [ "${b_admitted:-0}" -ne "${b_units:-0}" ]; then
    bad "admitted=${b_admitted:-?} but units=${b_units:-?} -- starvation or deadlock"
else
    note "admitted=$b_admitted == units=$b_units"
fi

echo
echo "-- 3/3 peak within budget, above one heavy unit --"
b_peak="$(num "$(budget_of "$WORKDIR/ordered.log")" peak-reserved)"
if [ "${b_peak:-0}" -gt "$budget_bytes" ]; then
    bad "peak-reserved=$b_peak exceeds the budget $budget_bytes"
elif [ "${b_peak:-0}" -le "$r_max" ]; then
    bad "peak-reserved=$b_peak never exceeded one heavy unit ($r_max) -- the light unit waited behind the second heavy one (dense-order claim)"
else
    note "peak-reserved=$b_peak in ($r_max, $budget_bytes] -- the skip happened"
fi

echo
if [ "$fail" -eq 0 ]; then
    echo "budget_order: OK"
else
    echo "budget_order: FAIL"
fi
exit "$fail"
