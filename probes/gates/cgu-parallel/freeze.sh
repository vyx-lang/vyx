#!/usr/bin/env bash
# R3 gate: the codegen plan is sealed before emission, and the backend defines
# nothing the collector did not plan.
#
# Unregistered functions after freeze indicate missing collector dependencies:
#
#   freeze 之后发现一个未登记函数，说明 collector 遗漏了依赖。推荐返回带
#   `MonoKey + caller + source span` 的内部编译错误 …… 不能让"最先碰到它的
#   worker"临时决定该函数属于哪个 CGU。
#
# Three assertions:
#
#   1. sealed        `[llvm-cgu] freeze units=U planned=P unplanned=N` with
#                    N == 0: every function the collect walk saw got a unit.
#   2. late          `[llvm-cgu] freeze-summary ... late-definitions=D` where D
#                    equals the recorded baseline.  D is *not* required to be 0
#                    in this tree: the lowerer legitimately synthesises glue
#                    (DCI stub wrappers, function-value adapters, raw callback
#                    adapters) per unit.  Pinning it to a recorded number is
#                    what makes a *change* visible instead of invisible.
#   3. strict        `VYX_CGU_STRICT_FREEZE=1` must accept a plan with no late
#                    definitions, and must reject one that has them.  The
#                    reject half needs `VYX_CGU_FORCE_LATE=n`, because the
#                    collector is currently complete on every corpus in the
#                    repo: without the injection the switch would never be
#                    observed failing, and an unexercised switch is not
#                    evidence.
#
# FULL=1 additionally runs industrial_mir_stress.
#
# Usage: bash probes/gates/cgu-parallel/freeze.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
FIX="$ROOT/probes/gates/cgu-parallel/fixture"
BOOT="${BOOT:-$ROOT/bootstrap_compiler/out/boot}"
if [ -x "${BOOT}.exe" ] && [ ! -x "$BOOT" ]; then BOOT="${BOOT}.exe"; fi
if [ ! -x "$BOOT" ]; then
    echo "FAIL: missing compiler $BOOT"
    exit 1
fi

# LLVM_ROOT must be a Windows path (`E:/Dev/...`), not MSYS (`/e/Dev/...`) --
# see the note in baseline.sh.
if [ -z "${LLVM_ROOT:-}" ]; then
    ROOT_WIN="$(cygpath -m "$ROOT" 2>/dev/null || printf '%s' "$ROOT")"
    LLVM_ROOT="$ROOT_WIN/clang"
fi
export LLVM_ROOT
export PATH="$ROOT/clang/bin:${LLVM_ROOT}/bin:${PATH:-}"

BASE_FILE="$ROOT/probes/gates/cgu-parallel/freeze_late_baseline.txt"
LATE_BASELINE="${LATE_BASELINE:-$(cat "$BASE_FILE" 2>/dev/null || echo 0)}"
LATE_CORPUS_BASELINE="${LATE_CORPUS_BASELINE:-$LATE_BASELINE}"
WITH_BODY_FILE="$ROOT/probes/gates/cgu-parallel/freeze_late_with_body_baseline.txt"
LATE_WITH_BODY_BASELINE="${LATE_WITH_BODY_BASELINE:-$(cat "$WITH_BODY_FILE" 2>/dev/null || echo 0)}"
LATE_WITH_BODY_CORPUS_BASELINE="${LATE_WITH_BODY_CORPUS_BASELINE:-$LATE_WITH_BODY_BASELINE}"

WORKDIR="${WORKDIR:-$ROOT/out/cgu_freeze_probe}"
rm -rf "$WORKDIR"
mkdir -p "$WORKDIR"

fail=0
note() { echo "  $1"; }
bad()  { echo "  FAIL  $1"; fail=1; }

copy_proj() {
    local src="$1" dest="$2"
    mkdir -p "$dest"
    cp "$src/Vyx.toml" "$dest/"
    cp -R "$src/src" "$dest/"
}

run_build() {
    local src="$1" dest="$2" log="$3"
    shift 3
    copy_proj "$src" "$dest"
    ( cd "$dest" && env "$@" "$BOOT" build ) >"$log" 2>&1 || true
}

num() { printf '%s' "$1" | sed -n "s/.*$2=\([0-9]*\).*/\1/p"; }

check_log() {
    local tag="$1" log="$2" want_late="$3" want_with_body="${4:-0}"
    local seal sum unplanned late
    seal="$(grep -m1 '^\[llvm-cgu\] freeze units=' "$log" || true)"
    if [ -z "$seal" ]; then
        bad "$tag: no '[llvm-cgu] freeze units=' line (is the freeze wired?)"
        return
    fi
    note "$seal"
    unplanned="$(num "$seal" unplanned)"
    if [ "${unplanned:-X}" != "0" ]; then
        bad "$tag: unplanned=$unplanned (a planned function got no unit)"
    fi
    sum="$(grep -m1 '^\[llvm-cgu\] freeze-summary' "$log" || true)"
    if [ -z "$sum" ]; then
        bad "$tag: no 'freeze-summary' line"
        return
    fi
    note "$sum"
    late="$(num "$sum" 'late-definitions')"
    if [ "${late:-X}" != "$want_late" ]; then
        bad "$tag: late-definitions=$late (recorded baseline $want_late)"
        echo "  ----- named late definitions -----"
        grep '^\[llvm-cgu\] freeze late' "$log" | head -n 12 || true
    fi
    # `late-definitions` counts *records* the arena gained past the seal; a
    # record can be a bodiless shell, which is not a plan violation.  Only
    # `late-with-body` answers §5.2's question, and it is read from the
    # completed arena -- the per-round count could not tell the two apart.
    wb="$(num "$sum" 'late-with-body')"
    if [ "${wb:-X}" != "$want_with_body" ]; then
        bad "$tag: late-with-body=$wb (recorded baseline $want_with_body) -- a late record that ended up with a body is a definition the plan did not own"
        grep '^\[llvm-cgu\] freeze late' "$log" | head -n 12 || true
    fi
    cls="$(grep -m1 '^\[llvm-cgu\] freeze late-classified' "$log" || true)"
    if [ "${late:-0}" != "0" ] && [ -z "$cls" ]; then
        bad "$tag: $late late record(s) but no 'late-classified' line"
    fi
    if [ "${late:-0}" = "0" ] && [ -n "$cls" ]; then
        bad "$tag: zero late records but a 'late-classified' line was printed"
    fi
    if [ "${late:-1}" = "0" ] && grep -q '^\[llvm-cgu\] freeze late ' "$log"; then
        bad "$tag: zero late definitions but per-definition lines printed"
    fi
}

echo "== R3/sealed-plan gate =="
echo "boot: $BOOT"
echo "late baseline: fixture=$LATE_BASELINE corpus=$LATE_CORPUS_BASELINE"
echo "late-with-body baseline: fixture=$LATE_WITH_BODY_BASELINE corpus=$LATE_WITH_BODY_CORPUS_BASELINE"
echo

# ── 1/2: the plan seals, and the count matches the baseline ──────────
echo "-- fixture: seal + late definitions --"
run_build "$FIX" "$WORKDIR/fix" "$WORKDIR/fix.log" \
    VYX_PHASE_SUMMARY=1 VYX_CGU_PARALLEL=lazy VYX_CGU_THREADS=2
check_log fixture "$WORKDIR/fix.log" "$LATE_BASELINE" "$LATE_WITH_BODY_BASELINE"

# ── 3: strict mode does what it says ─────────────────────────────────
echo
echo "-- strict mode --"
# The rc of each of the three runs below is the *assertion*, so none of them may
# be spelled as a bare command: under `set -e` an expected non-zero exit would
# kill the script before the `$?` was read, and the gate would report "FAIL"
# without ever having evaluated its own expectation.  `if` keeps the command in
# a condition context, which `set -e` does not act on.
strict_log="$WORKDIR/fix.strict.log"
copy_proj "$FIX" "$WORKDIR/fix.strict"
if ( cd "$WORKDIR/fix.strict" \
     && env VYX_PHASE_SUMMARY=1 VYX_CGU_PARALLEL=lazy VYX_CGU_THREADS=2 \
              VYX_CGU_STRICT_FREEZE=1 "$BOOT" build ) >"$strict_log" 2>&1
then strict_rc=0; else strict_rc=$?; fi
if [ "$strict_rc" != "0" ]; then
    bad "strict mode failed on a plan with no late definitions"
    tail -n 15 "$strict_log"
else
    note "strict=1 accepted the same plan (rc=0), late=$LATE_BASELINE"
fi

# The switch must also be able to fail.  Inject late definitions so the strict
# path is exercised rather than assumed.
inject_log="$WORKDIR/fix.inject.log"
copy_proj "$FIX" "$WORKDIR/fix.inject"
if ( cd "$WORKDIR/fix.inject" \
     && env VYX_PHASE_SUMMARY=1 VYX_CGU_PARALLEL=lazy VYX_CGU_THREADS=2 \
              VYX_CGU_STRICT_FREEZE=1 VYX_CGU_FORCE_LATE=3 \
              "$BOOT" build ) >"$inject_log" 2>&1
then inject_rc=0; else inject_rc=$?; fi
if [ "$inject_rc" = "0" ]; then
    bad "strict mode accepted 3 injected late definitions"
else
    if grep -q 'late definitions' "$inject_log"; then
        note "strict=1 rejected injected late definitions (rc=$inject_rc)"
    else
        bad "injected build failed, but not with the late-definition diag"
        tail -n 15 "$inject_log"
    fi
fi
# ...and the same injection without strict must still build, with the count
# reported rather than dropped.
report_log="$WORKDIR/fix.inject.report.log"
copy_proj "$FIX" "$WORKDIR/fix.inject.report"
if ( cd "$WORKDIR/fix.inject.report" \
     && env VYX_PHASE_SUMMARY=1 VYX_CGU_PARALLEL=lazy VYX_CGU_THREADS=2 \
              VYX_CGU_FORCE_LATE=3 "$BOOT" build ) >"$report_log" 2>&1
then report_rc=0; else report_rc=$?; fi
if [ "$report_rc" != "0" ]; then
    bad "report-only mode failed on the injected late definitions"
    tail -n 15 "$report_log"
else
    injected="$(num "$(grep -m1 '^\[llvm-cgu\] freeze-summary' "$report_log" || true)" 'late-definitions')"
    if [ "${injected:-X}" != "3" ]; then
        bad "report-only mode counted late-definitions=$injected, expected 3"
    else
        note "report-only mode counted the injection (late-definitions=3)"
    fi
fi

# ── corpus ───────────────────────────────────────────────────────────
if [ "${FULL:-0}" = "1" ]; then
    echo
    echo "-- corpus: industrial_mir_stress --"
    CORPUS="$ROOT/tests/projects/industrial_mir_stress"
    if [ ! -f "$CORPUS/Vyx.toml" ]; then
        bad "corpus project not found at $CORPUS"
    else
        run_build "$CORPUS" "$WORKDIR/corpus" "$WORKDIR/corpus.log" \
            VYX_PHASE_SUMMARY=1 VYX_CGU_PARALLEL=lazy VYX_CGU_THREADS=4
        check_log corpus "$WORKDIR/corpus.log" "$LATE_CORPUS_BASELINE" "$LATE_WITH_BODY_CORPUS_BASELINE"
    fi
else
    echo
    echo "-- corpus: skipped (set FULL=1 to run industrial_mir_stress) --"
fi

echo
if [ "$fail" -eq 0 ]; then
    echo "freeze: OK"
else
    echo "freeze: FAIL"
fi
exit "$fail"
