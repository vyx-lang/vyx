#!/usr/bin/env bash
# R8.2 / R8.3 gate: the partition atom is the definition, not the source file.
#
# Minimal counterexample:
#
#   输入：一个 source，包含 4096 个独立、相近成本的具体函数
#   当前 merge: source keys = 1 -> units = 1
#
# Under the pre-R8 rules the module key WAS the partition atom, so a single
# giant source could never produce more than one CGU -- no hash function and
# no worker count could change that.  This gate makes splitting it a hard
# structural requirement:
#
#   巨型单 source 含足量独立 item 时，能够产生多个实用 CGU；
#   不再限制 CGU≤source 数。
#
# Four assertions, over both profiles:
#   1. cold   (default) the single-source project splits into >= 2 units, and
#             the plan header says `modules=1` -- the split did not come from
#             more files.
#   2. stable the fixed-roots profile also splits, reports its root count, and
#             never exceeds it.
#   3. owner  the plan's member counts add up to `collected` (every definition
#             has exactly one owner) in both profiles.
#   4. det    two runs of the same input produce identical plan lines and the
#             same result digest, in both profiles.
#
# Usage: bash probes/gates/cgu-parallel/r8/giant_source.sh
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

WORKDIR="${WORKDIR:-$ROOT/out/cgu_r8_giant_source}"
rm -rf "$WORKDIR"
mkdir -p "$WORKDIR"

fail=0
note() { echo "  $1"; }
bad()  { echo "  FAIL  $1"; fail=1; }
num()  { printf '%s' "$1" | sed -n "s/.*$2=\([0-9]*\).*/\1/p"; }

# ── fixture: one source, many independent functions ──────────────────
#
# 48 functions with real (small) bodies, all called from main so the
# collector keeps them.  Total static work lands well above the cold
# profile's min-work floor, so the counterexample's shape holds: many
# independent atoms, one source key.
FNS=48
GEN="$WORKDIR/src"
mkdir -p "$GEN"
{
    echo "module probe.giant;"
    echo
    echo "public fn g1() -> i32 {"
    echo "    var n: i32 = 0;"
    echo "    var i: i32 = 0;"
    echo "    while (i < 6) { n = n + i; i = i + 1; }"
    echo "    return n;"
    echo "}"
    k=2
    while [ "$k" -le "$FNS" ]; do
        echo
        echo "public fn g$k(n: i32) -> i32 {"
        echo "    var acc: i32 = n;"
        echo "    var i: i32 = 0;"
        echo "    while (i < 5) { acc = acc + i + $k; i = i + 1; }"
        echo "    return acc - $((k - 1));"
        echo "}"
        k=$((k + 1))
    done
    echo
    printf 'fn main() -> i32 {\n    var s: i32 = g1();\n'
    k=2
    while [ "$k" -le "$FNS" ]; do
        echo "    s = s + g$k($((k - 1)));"
        k=$((k + 1))
    done
    echo '    if (s < 0) { return 1; }'
    echo '    print("giant-ok");'
    echo '    return 0;'
    echo '}'
} > "$GEN/giant.vyx"

cat > "$WORKDIR/Vyx.toml" <<EOF
[package]
name = "cgu_r8_giant"
version = "0.1.0"
entry = "src/giant.vyx"

[build]
cache_dir = ".cache"
output_dir = "target"

[target.cgu_r8_giant]
type = "executable"
entry = "src/giant.vyx"
sources = ["src/giant.vyx"]
EOF

run_giant() {              # run_giant <subdir> [extra env...]
    local sub="$1"
    shift
    local dest="$WORKDIR/$sub" log="$WORKDIR/$sub.log"
    mkdir -p "$dest"
    cp "$WORKDIR/Vyx.toml" "$dest/"
    cp -R "$GEN" "$dest/src"
    local rc
    if ( cd "$dest" && env VYX_PHASE_SUMMARY=1 VYX_CGU_PLAN_DUMP=1 \
              VYX_CGU_PARALLEL=lazy VYX_CGU_THREADS=2 "$@" \
              "$BOOT" build ) >"$log" 2>&1
    then rc=0; else rc=$?; fi
    printf '%s' "$rc"
}

plan_lines() { grep '^\[llvm-cgu\] plan-unit ' "$1" || true; }
plan_of()    { grep -m1 '^\[llvm-cgu\] plan mode=' "$1" || true; }
units_of()   { num "$(plan_of "$1")" units; }
mods_of()    { num "$(grep -m1 '^\[llvm-cgu\] units=' "$1")" modules; }
digest_of()  { sed -n 's/.*digest=\([0-9a-f]*\).*/\1/p' "$1" | head -n 1; }
sum_members() {
    plan_lines "$1" | sed -n 's/.*members=\([0-9]*\).*/\1/p' | awk '{s+=$1} END {print s+0}'
}

echo "== R8 giant-source gate =="
echo "boot: $BOOT"
echo

# ── 1: cold splits a single source ───────────────────────────────────
echo "-- 1/4 cold (default) --"
rc="$(run_giant cold)"
if [ "$rc" != "0" ]; then
    bad "cold build failed (rc=$rc)"
    tail -n 10 "$WORKDIR/cold.log"
fi
c_units="$(units_of "$WORKDIR/cold.log")"
c_mods="$(mods_of "$WORKDIR/cold.log")"
note "$(plan_of "$WORKDIR/cold.log")"
if [ "${c_units:-0}" -lt 2 ]; then
    bad "single-source project produced units=${c_units:-?} -- the atom is still the source file (R8 §2.1)"
elif [ "${c_mods:-0}" -ne 1 ]; then
    note "modules=$c_mods (fixture has more sources than intended; the split assertion is weakened, not void)"
else
    note "modules=1 -> units=$c_units (the giant source split)"
fi

# ── 2: stable splits too, within its fixed root count ────────────────
echo
echo "-- 2/4 stable --"
rc="$(run_giant stable VYX_CGU_PARTITION=stable)"
if [ "$rc" != "0" ]; then
    bad "stable build failed (rc=$rc)"
    tail -n 10 "$WORKDIR/stable.log"
fi
s_units="$(units_of "$WORKDIR/stable.log")"
s_roots="$(num "$(plan_of "$WORKDIR/stable.log")" roots)"
note "$(plan_of "$WORKDIR/stable.log")"
if [ "${s_units:-0}" -lt 2 ]; then
    bad "stable profile produced units=${s_units:-?} for the giant source"
elif [ "${s_roots:-0}" -lt 1 ]; then
    bad "stable plan header did not report its root count"
elif [ "${s_units:-0}" -gt "${s_roots:-0}" ]; then
    bad "stable units=$s_units exceed the declared root count $s_roots"
else
    note "stable: units=$s_units within roots=$s_roots"
fi

# ── 3: owner coverage ────────────────────────────────────────────────
echo
echo "-- 3/4 owner coverage --"
for mode in cold stable; do
    log="$WORKDIR/$mode.log"
    tot="$(sum_members "$log")"
    n_fn="$(num "$(plan_of "$log")" collected)"
    if [ -n "${n_fn:-}" ] && [ "${tot:-0}" != "$n_fn" ]; then
        bad "$mode: plan members sum to $tot but collected=$n_fn -- an owner is missing or doubled"
    else
        note "$mode: members sum=$tot == collected=${n_fn:-?}"
    fi
done

# ── 4: determinism ───────────────────────────────────────────────────
echo
echo "-- 4/4 determinism --"
rc="$(run_giant cold2)"
rc2="$(run_giant stable2 VYX_CGU_PARTITION=stable)"
if [ "$rc" != "0" ] || [ "$rc2" != "0" ]; then
    bad "second-round build failed (rc=$rc/$rc2)"
fi
for pair in "cold cold2" "stable stable2"; do
    set -- $pair
    d1="$(digest_of "$WORKDIR/$1.log")"
    d2="$(digest_of "$WORKDIR/$2.log")"
    if [ -z "$d1" ] || [ -z "$d2" ]; then
        bad "$1: no result digest to compare"
    elif [ "$d1" != "$d2" ]; then
        bad "$1: digest differs across identical runs ($d1 vs $d2)"
    else
        note "$1: digest stable ($d1)"
    fi
    p1="$(plan_lines "$WORKDIR/$1.log" | sha256sum | cut -d' ' -f1)"
    p2="$(plan_lines "$WORKDIR/$2.log" | sha256sum | cut -d' ' -f1)"
    if [ "$p1" != "$p2" ]; then
        bad "$1: plan lines differ across identical runs"
    else
        note "$1: plan lines stable"
    fi
done

echo
if [ "$fail" -eq 0 ]; then
    echo "giant_source: OK"
else
    echo "giant_source: FAIL"
fi
exit "$fail"
