#!/usr/bin/env bash
# R8.4 gate: the per-lowerer declaration pass is the plan's closure, not a
# full-table scan.
#
# Declaration closure is measured as:
#
#   decl_scan_factor = total declaration candidates inspected / unique definitions
#
# Before R8.4 every CGU lowerer walked ALL functions (1..function_count) and
# asked `function_should_emit` on each, so the factor was ~units; with the
# CSR closure each lowerer visits exactly its own members and the factor
# collapses to ~1.
#
# Four assertions:
#   1. closure build succeeds and produces the same number of units as the
#      legacy build on the identical input;
#   2. object bytes are IDENTICAL between closure and legacy -- the closure
#      must be a pure scan optimization, not a behavior change;
#   3. the decl-scan report shows closure inspected <= collected + slack
#      while legacy inspected >= units * collected;
#   4. VYX_MAP_STATS reports the dense-map totals and zero overflow (the
#      proportional §6.2 floor held for this input).
#
# Usage: bash probes/gates/cgu-parallel/r8/decl_closure.sh
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

WORKDIR="${WORKDIR:-$ROOT/out/cgu_r8_decl_closure}"
rm -rf "$WORKDIR"
mkdir -p "$WORKDIR"

fail=0
note() { echo "  $1"; }
bad()  { echo "  FAIL  $1"; fail=1; }
num()  { printf '%s' "$1" | sed -n "s/.*$2=\([0-9]*\).*/\1/p"; }

# ── fixture: one source, many independent functions (same shape as the
# giant_source gate so the plan splits) ────────────────────────────────
FNS=48
GEN="$WORKDIR/src"
mkdir -p "$GEN"
{
    echo "module probe.declclosure;"
    echo
    echo "public fn h1() -> i32 {"
    echo "    var n: i32 = 0;"
    echo "    var i: i32 = 0;"
    echo "    while (i < 6) { n = n + i; i = i + 1; }"
    echo "    return n;"
    echo "}"
    k=2
    while [ "$k" -le "$FNS" ]; do
        echo
        echo "public fn h$k(n: i32) -> i32 {"
        echo "    var acc: i32 = n;"
        echo "    var i: i32 = 0;"
        echo "    while (i < 5) { acc = acc + i + $k; i = i + 1; }"
        echo "    return acc - $((k - 1));"
        echo "}"
        k=$((k + 1))
    done
    echo
    printf 'fn main() -> i32 {\n    var s: i32 = h1();\n'
    k=2
    while [ "$k" -le "$FNS" ]; do
        echo "    s = s + h$k($((k - 1)));"
        k=$((k + 1))
    done
    echo '    if (s < 0) { return 1; }'
    echo '    print("declclosure-ok");'
    echo '    return 0;'
    echo '}'
} > "$GEN/decl.vyx"

cat > "$WORKDIR/Vyx.toml" <<EOF
[package]
name = "cgu_r8_decl_closure"
version = "0.1.0"
entry = "src/decl.vyx"

[build]
cache_dir = ".cache"
output_dir = "target"

[target.cgu_r8_decl_closure]
type = "executable"
entry = "src/decl.vyx"
sources = ["src/decl.vyx"]
EOF

# run_case <subdir> <extra env...>; emits objects into <subdir>/target
run_case() {
    local sub="$1"
    shift
    local dest="$WORKDIR/$sub" log="$WORKDIR/$sub.log"
    mkdir -p "$dest"
    cp "$WORKDIR/Vyx.toml" "$dest/"
    cp -R "$GEN" "$dest/src"
    local rc
    if ( cd "$dest" && env VYX_PHASE_SUMMARY=1 VYX_CGU_PLAN_DUMP=1 VYX_MAP_STATS=1 \
              VYX_CGU_PARALLEL=lazy VYX_CGU_THREADS=2 "$@" \
              "$BOOT" build ) >"$log" 2>&1
    then rc=0; else rc=$?; fi
    printf '%s' "$rc"
}

decl_line() { grep -m1 '^\[llvm-cgu\] decl-scan producer ' "$1" || true; }
map_line()  { grep -m1 '^\[llvm-map\] tables=' "$1" || true; }
plan_of()   { grep -m1 '^\[llvm-cgu\] plan mode=' "$1" || true; }

echo "== R8.4 declaration-closure gate =="
echo "boot: $BOOT"
echo

# ── 1 + 2: closure vs legacy -- same units, identical object bytes ────
echo "-- 1/4 legacy build (VYX_CGU_DECL_CLOSURE=0) --"
rc="$(run_case legacy VYX_CGU_DECL_CLOSURE=0 VYX_MAP_FLOOR=legacy)"
if [ "$rc" != "0" ]; then
    bad "legacy build failed (rc=$rc)"
    tail -n 10 "$WORKDIR/legacy.log"
fi

echo
echo "-- 2/4 closure build (defaults) --"
rc="$(run_case closure)"
if [ "$rc" != "0" ]; then
    bad "closure build failed (rc=$rc)"
    tail -n 20 "$WORKDIR/closure.log"
fi

l_units="$(num "$(plan_of "$WORKDIR/legacy.log")" units)"
c_units="$(num "$(plan_of "$WORKDIR/closure.log")" units)"
if [ "${l_units:-0}" -lt 2 ]; then
    note "plan did not split (units=${l_units:-?}); comparing single-unit output anyway"
fi
if [ "${l_units:-0}" != "${c_units:-0}" ]; then
    bad "unit count differs: legacy=$l_units closure=$c_units"
else
    note "units agree: $c_units"
fi

if [ "$fail" -eq 0 ]; then
    l_objs="$(find "$WORKDIR/legacy/.cache" -name '*.obj.cgu.*' ! -name '*.list' 2>/dev/null | sort)"
    c_objs="$(find "$WORKDIR/closure/.cache" -name '*.obj.cgu.*' ! -name '*.list' 2>/dev/null | sort)"
    if [ -z "$l_objs" ]; then
        bad "legacy build produced no CGU objects"
    elif [ "$(echo "$l_objs" | wc -l)" != "$(echo "$c_objs" | wc -l)" ]; then
        bad "object count differs between closure and legacy ($(echo "$l_objs" | wc -l) vs $(echo "$c_objs" | wc -l))"
    else
        # pair by name; the subdirectory prefix differs
        same=1
        while IFS= read -r lo; do
            rel="${lo#"$WORKDIR/legacy/"}"
            co="$WORKDIR/closure/$rel"
            if [ ! -f "$co" ]; then
                bad "missing closure object: $rel"
                same=0
            elif ! cmp -s "$lo" "$co"; then
                bad "object bytes differ: $rel"
                same=0
            fi
        done <<< "$l_objs"
        if [ "$same" -eq 1 ]; then
            note "all $(echo "$l_objs" | wc -l) CGU object files byte-identical (closure is a pure scan change)"
        fi
    fi
fi

# ── 3: decl-scan factor ──────────────────────────────────────────────
echo
echo "-- 3/4 decl-scan factor --"
dl="$(decl_line "$WORKDIR/legacy.log")"
dc="$(decl_line "$WORKDIR/closure.log")"
note "legacy:  $dl"
note "closure: $dc"
l_ins="$(num "$dl" inspected)"; c_ins="$(num "$dc" inspected)"
c_col="$(num "$dc" collected)"
if [ -z "${c_ins:-}" ]; then
    bad "closure run has no decl-scan report line"
elif [ "${c_ins:-0}" -gt $(( ${c_col:-0} + 64 )) ]; then
    bad "closure inspected=$c_ins is far above collected=$c_col -- the closure did not engage"
else
    note "closure inspected=$c_ins <= collected=$c_col + 64 (factor ~1)"
fi
if [ -n "${l_ins:-}" ] && [ -n "${c_ins:-}" ] && [ "${l_ins:-0}" -le "${c_ins:-0}" ]; then
    bad "legacy inspected=$l_ins should exceed closure inspected=$c_ins on a split build"
fi

# ── 4: dense-map stats under the proportional floor ──────────────────
echo
echo "-- 4/4 map stats (proportional floor) --"
ml="$(map_line "$WORKDIR/closure.log")"
note "$ml"
if [ -z "$ml" ]; then
    bad "closure run has no [llvm-map] stats line (VYX_MAP_STATS ignored?)"
else
    ovf="$(num "$ml" overflow)"
    if [ "${ovf:--1}" -ne 0 ]; then
        bad "dense-map overflow=$ovf on a proportional floor (§6.2 floor too tight)"
    else
        note "overflow=0 with the proportional floor"
    fi
fi

echo
if [ "$fail" -eq 0 ]; then
    echo "decl_closure: OK"
else
    echo "decl_closure: FAIL"
fi
exit "$fail"
