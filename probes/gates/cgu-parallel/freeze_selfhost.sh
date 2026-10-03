#!/usr/bin/env bash
# R3 self-host freeze probe: read the seal / late-definition lines out of a build
# log produced with VYX_PHASE_SUMMARY=1, and compare the late count with the
# recorded baseline.
#
# This is *not* a build gate: rebuilding the compiler is ~10 minutes, so the
# self-host corpus cannot live inside `freeze.sh`.  Instead the fixpoint script
# (`fixpoint_r3.sh`) builds two generations with VYX_PHASE_SUMMARY=1, and this
# probe turns one of those logs into the same numbers the gate checks on the
# fixture.
#
# A self-host build seals **several** plans, one per compiled crate -- and after
# R3b three of the four are single-unit and therefore also sealed.  So this probe
# iterates every seal line rather than reading the first: an earlier version read
# `-m1` and happened to look at a 328-function helper crate while the interesting
# 2425-function plan went unchecked.
#
# Baseline `freeze_late_selfhost.txt` is 3, not 0.  The collector's closure is
# provably incomplete on the compiler's own source: three `diag.vyx` functions
# are pulled in by whichever unit happens to lower their caller first.  See
# `bootstrap_compiler/docs/cgu-parallel-r3/README.md` §6.  Pinning it at 3 is
# what makes the number *change* visible; it is not a claim that 3 is fine.
#
# Usage: bash probes/gates/cgu-parallel/freeze_selfhost.sh [logfile]
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
LOG="${1:-$ROOT/out/r3_s2.log}"
BASE_FILE="$ROOT/probes/gates/cgu-parallel/freeze_late_selfhost.txt"
WANT="${LATE_SELFHOST:-$(cat "$BASE_FILE" 2>/dev/null || echo 3)}"

if [ ! -f "$LOG" ]; then
    echo "FAIL: no build log at $LOG"
    echo "      produce one with: VYX_PHASE_SUMMARY=1 <boot> build -j10"
    exit 1
fi

echo "== R3 self-host freeze probe =="
echo "log: $LOG"
echo "late baseline (self-host): $WANT"
echo

seals="$(grep '^\[llvm-cgu\] freeze units=' "$LOG" || true)"
if [ -z "$seals" ]; then
    echo "FAIL: no 'freeze units=' line -- was the build run with VYX_PHASE_SUMMARY=1,"
    echo "      and is the freeze wired into this generation?"
    exit 1
fi

n_seal="$(printf '%s\n' "$seals" | grep -c .)"
n_sum="$(grep -c '^\[llvm-cgu\] freeze-summary' "$LOG" || true)"
echo "-- sealed plans: $n_seal (this is one per compiled crate; every one must be"
echo "   checked, not just the largest) --"
fail=0
printf '%s\n' "$seals" | while IFS= read -r line; do
    [ -z "$line" ] && continue
    up="$(printf '%s' "$line" | sed -n 's/.*unplanned=\([0-9]*\).*/\1/p')"
    u="$(printf '%s' "$line" | sed -n 's/.*units=\([0-9]*\).*/\1/p')"
    # `[^n]planned=` not `planned=`: the greedy `.*` would otherwise match the
    # "planned=" inside "unplanned=0" and report planned=0 for every plan.
    p="$(printf '%s' "$line" | sed -n 's/.*[^n]planned=\([0-9]*\).*/\1/p')"
    if [ "${up:-X}" != "0" ]; then
        printf '  FAIL  units=%s planned=%s unplanned=%s\n' "$u" "$p" "$up"
    else
        printf '  ok    units=%-4s planned=%-6s unplanned=0\n' "$u" "$p"
    fi
    printf '%s\n' "$line" | sed 's/^/        /'
done
# `while` runs in a subshell here, so re-check with a simple pass rather than
# relying on the loop's exit status.
bad_up="$(printf '%s\n' "$seals" | grep -o 'unplanned=[0-9]*' | grep -v 'unplanned=0' | head -1)"
if [ -n "$bad_up" ]; then
    echo "FAIL: $bad_up -- the partition left a definition with no unit"
    fail=1
fi

echo
echo "-- late definitions --"
# `freeze late` with no trailing space: the per-definition line is
# `freeze late-def id=...`, and the old pattern required a space right after
# "late", so the *attribution* -- the part a reader actually needs to fix the
# collector -- was silently dropped from this probe while the count above still
# matched.  A gate that filters out the evidence is a gate that cannot fail.
late_lines="$(grep '^\[llvm-cgu\] freeze late' "$LOG" || true)"
if [ -n "$late_lines" ]; then
    printf '%s\n' "$late_lines" | sed 's/^/  /'
else
    echo "  (none)"
fi
late="$(grep -o 'late-definitions=[0-9]*' "$LOG" | cut -d= -f2 \
        | awk '{s+=$1} END {print s+0}')"
echo "  late-definitions total: $late (baseline $WANT)"
if [ "$late" != "$WANT" ]; then
    echo "FAIL: late-definitions changed from the recorded baseline."
    echo "      A change here means the collector's closure moved.  Either the"
    echo "      collector got better (lower the baseline, and say why in the"
    echo "      R3 README) or a dependency went missing (fix the collector; do"
    echo "      NOT raise the baseline)."
    fail=1
else
    echo "ok:     late-definitions matches the recorded baseline"
fi

echo
if [ "$fail" -eq 0 ]; then echo "freeze-selfhost: OK"; else echo "freeze-selfhost: FAIL"; fi
exit "$fail"
