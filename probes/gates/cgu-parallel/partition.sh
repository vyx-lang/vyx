#!/usr/bin/env bash
# R5/R8 gate: placement profiles are deterministic, self-describing, and
# bounded by their own declared key space -- not by the source-file count.
#
# Definition-level partitioning replaced the old source-count bound:
#
#   旧探针限制错误粒度 …… 断言 bucket units ≤ merge units/modules，将单巨
#   文件不可拆锁成"正确"；R8 新语义需更新门
#
# The pre-R8 assertion "stable units <= merge units <= modules" is gone on
# purpose: the atom is now the definition, so one giant source legitimately
# splits into more units than it has files (giant_source.sh locks that in).
# What must still hold:
#
#   1. modes       both `cold` and `stable` build, and each says which it is.
#   2. bounded     stable reports its root count and never exceeds it.
#   3. stable      two runs of the same input produce the same plan and the
#                  same object digest, in both profiles.  Determinism is the
#                  whole point of a placement rule; a hash seed that leaked in
#                  would show up here as a digest difference.
#   4. dump        `VYX_CGU_PLAN_DUMP=1` must actually print the plan, and the
#                  per-unit member counts must add up to `collected` -- the
#                  dump must be the plan that ran, in both profiles.
#
# Usage: bash probes/gates/cgu-parallel/partition.sh
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

WORKDIR="${WORKDIR:-$ROOT/out/cgu_partition_probe}"
rm -rf "$WORKDIR"
mkdir -p "$WORKDIR"

fail=0
note() { echo "  $1"; }
bad()  { echo "  FAIL  $1"; fail=1; }
num()  { printf '%s' "$1" | sed -n "s/.*$2=\([0-9]*\).*/\1/p"; }

run_part() {             # run_part <subdir> <mode> [extra env...]
    local sub="$1" mode="$2"
    shift 2
    local dest="$WORKDIR/$sub" log="$WORKDIR/$sub.log"
    mkdir -p "$dest"
    cp "$FIX/Vyx.toml" "$dest/"
    cp -R "$FIX/src" "$dest/"
    local rc
    if ( cd "$dest" && env VYX_PHASE_SUMMARY=1 VYX_CGU_PLAN_DUMP=1 \
              VYX_CGU_PARALLEL=lazy VYX_CGU_THREADS=2 \
              "VYX_CGU_PARTITION=$mode" "$@" "$BOOT" build ) >"$log" 2>&1
    then rc=0; else rc=$?; fi
    printf '%s' "$rc"
}

plan_lines() { grep '^\[llvm-cgu\] plan-unit ' "$1" || true; }
plan_of()    { grep -m1 '^\[llvm-cgu\] plan mode=' "$1" || true; }
units_of()   { num "$(plan_of "$1")" units; }
digest_of()  { sed -n 's/.*digest=\([0-9a-f]*\).*/\1/p' "$1" | head -n 1; }
sum_members() {
    plan_lines "$1" | sed -n 's/.*members=\([0-9]*\).*/\1/p' | awk '{s+=$1} END {print s+0}'
}

echo "== R5/R8 placement gate =="
echo "boot: $BOOT"
echo

# ── 1: both modes build and name themselves ──────────────────────────
echo "-- 1/4 cold --"
rc="$(run_part cold cold)"
if [ "$rc" != "0" ]; then
    bad "cold build failed (rc=$rc)"
    tail -n 10 "$WORKDIR/cold.log"
else
    m="$(plan_of "$WORKDIR/cold.log")"
    note "${m:-<no plan mode line>}"
    if ! printf '%s' "$m" | grep -q 'mode=cold'; then
        bad "cold run did not report mode=cold"
    fi
fi

echo
echo "-- 2/4 stable --"
rc="$(run_part stable stable)"
if [ "$rc" != "0" ]; then
    bad "stable build failed (rc=$rc)"
    tail -n 10 "$WORKDIR/stable.log"
else
    b="$(plan_of "$WORKDIR/stable.log")"
    note "${b:-<no plan mode line>}"
    if ! printf '%s' "$b" | grep -q 'mode=stable'; then
        bad "stable run did not report mode=stable"
    fi
fi

# ── 2: stable stays within its declared key space ────────────────────
echo
echo "-- 3/4 bounded: stable units <= declared roots --"
s_units="$(units_of "$WORKDIR/stable.log")"
s_roots="$(num "$(plan_of "$WORKDIR/stable.log")" roots)"
if [ -z "${s_units:-}" ] || [ "${s_units:-0}" -lt 1 ]; then
    bad "could not read a unit count from the stable run"
elif [ "${s_roots:-0}" -lt 1 ]; then
    bad "stable plan header did not declare its root count"
elif [ "$s_units" -gt "$s_roots" ]; then
    bad "stable produced $s_units units but declared only $s_roots roots -- placement escaped its key space"
else
    note "stable: units=$s_units within roots=$s_roots"
fi

# ── 3: the dump is the plan that ran ─────────────────────────────────
echo
echo "-- 4/4 plan dump + determinism --"
for mode in cold stable; do
    log="$WORKDIR/$mode.log"
    n_plan="$(plan_lines "$log" | grep -c . || true)"
    if [ "${n_plan:-0}" -lt 1 ]; then
        bad "$mode: VYX_CGU_PLAN_DUMP=1 printed no plan-unit line"
        continue
    fi
    note "$mode: $n_plan unit lines"
    # The per-unit member counts must add up to the collected function count,
    # otherwise the dump describes a different plan than the one that ran.
    tot="$(sum_members "$log")"
    n_fn="$(num "$(plan_of "$log")" collected)"
    if [ -n "${n_fn:-}" ] && [ "${tot:-0}" != "$n_fn" ]; then
        bad "$mode: plan members sum to $tot but collected=$n_fn -- the dump is not the plan that ran"
    else
        note "$mode: members sum=$tot == collected=${n_fn:-?}"
    fi
done

rc="$(run_part cold2 cold)"
rc2="$(run_part stable2 stable)"
for pair in "cold cold2" "stable stable2"; do
    set -- $pair
    d1="$(digest_of "$WORKDIR/$1.log")"
    d2="$(digest_of "$WORKDIR/$2.log")"
    if [ "$rc" != "0" ] || [ "$rc2" != "0" ]; then
        bad "second-round $1 build failed (rc=$rc/$rc2)"
        continue
    fi
    if [ -z "$d1" ] || [ -z "$d2" ]; then
        bad "$1: no result digest to compare ($1=$d1 $2=$d2)"
    elif [ "$d1" != "$d2" ]; then
        bad "$1: placement is not deterministic: digest $d1 vs $d2"
    else
        note "$1: digest stable across runs: $d1"
    fi
    p1="$(plan_lines "$WORKDIR/$1.log" | sha256sum | cut -d' ' -f1)"
    p2="$(plan_lines "$WORKDIR/$2.log" | sha256sum | cut -d' ' -f1)"
    if [ "$p1" != "$p2" ]; then
        bad "$1: plan lines differ between two runs of the same input"
    else
        note "$1: plan lines identical across runs"
    fi
done

echo
if [ "$fail" -eq 0 ]; then
    echo "partition: OK"
else
    echo "partition: FAIL"
fi
exit "$fail"
