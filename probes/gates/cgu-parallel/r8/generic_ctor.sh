#!/usr/bin/env bash
# R8 gate: a closed generic instance's value constructor must actually write
# the field values its body assigns.
#
# ENFORCED -- regression gate for a defect that was open until the
# constructor-identity fix (2026-09-19).
#
# Observed before the fix: `Box::<i32>()` returned a struct whose `a`/`b`/`v`
# held whatever the caller's stack slot already contained (sum() printed 14
# instead of 3, get() printed 0 instead of 7).  The instance was registered
# under the mangled `mir2$…::<args>::constructor…` item name, so every
# `function_name(fn) == "constructor"` test in HIR/MIR missed it: the hidden
# `self` receiver was never materialized, `self.field = …` resolved to no
# place at all, and the whole constructor body was dropped.  Only the trailing
# expression survived, and it was stored into the return slot.
#
# Fix: `hir_function_flag_constructor()` is set on the instance shell in
# `ensure_function_instance`, and the constructor tests
# (`local_is_constructor_abi_self` / `ensure_constructor_abi_self_param` /
# `return_type_is_constructor`) read it instead of relying on the source name.
#
# Impact of the defect: any generic record with a user-written constructor was
# miscompiled.  Projects "worked" only when the stack happened to be clean,
# which is why single-unit builds of some fixtures appeared fine.  The
# allocator/pointer variant (heap-backed generic container) crashed outright
# with a pointer-audit trap -> abort (0xC0000409).
#
# Assertions:
#   1. the fixture builds with > 1 codegen unit (the gate is vacuous if the
#      partition never splits it);
#   2. running the produced binary prints every expected line -- a dropped
#      field store makes `push` trap before the program can finish;
#   3. the exit code is 0.
#
# Usage: bash probes/gates/cgu-parallel/r8/generic_ctor.sh
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

WORKDIR="${WORKDIR:-$ROOT/out/cgu_r8_generic_ctor}"
rm -rf "$WORKDIR"
mkdir -p "$WORKDIR/src"

fail=0
note() { echo "  $1"; }
bad()  { echo "  FAIL  $1"; fail=1; }

# ── fixture ──────────────────────────────────────────────────────────
# A generic record with a value constructor: every field store has to survive
# lowering, and the allocator field sits last so a missing GEP shows up as a
# wrong-offset write instead of a harmless one.
# A generic record whose constructor initializes every field.  The instance is
# spelled `Box::<i32>` while the field table lives on the template `Box`, so a
# regression in instance field resolution drops the stores silently.
# Deliberately allocation-free: the allocator/pointer path has its own
# separate cross-unit regression; this gate isolates constructor initialization.
cat > "$WORKDIR/src/ctor.vyx" <<'EOF'
module probe.ctor;

class Box<T> {
    var a: i64;
    var b: i64;
    var v: T;

    Box() {
        self.a = 1;
        self.b = 2;
        self.v = 7;
    }

    public fn sum(&mut self) -> i64 {
        return self.a + self.b;
    }

    public fn get(&mut self) -> T {
        return self.v;
    }
}

fn main() {
    var x = Box::<i32>();
    print(x.sum());
    print(x.get());
    print("ctor-ok");
}
EOF

cat > "$WORKDIR/Vyx.toml" <<'EOF'
[package]
name = "cgu_r8_ctor"
version = "0.1.0"
entry = "src/ctor.vyx"

[build]
cache_dir = ".cache"
output_dir = "target"

[target.cgu_r8_ctor]
type = "executable"
entry = "src/ctor.vyx"
sources = ["src/ctor.vyx"]
EOF

echo "== R8 generic-ctor gate =="
echo "boot: $BOOT"
echo

echo "-- 1/3 build (default partition) --"
build_log="$WORKDIR/build.log"
# VYX_CGU_MIN_UNIT_WORK=1 forces the partition to split even a tiny fixture;
# without it a small project stays single-unit and the gate proves nothing.
if ( cd "$WORKDIR" && env VYX_CGU_MIN_UNIT_WORK=1 "$BOOT" build ) >"$build_log" 2>&1
then rc=0; else rc=$?; fi
if [ "$rc" != "0" ]; then
    bad "build failed (rc=$rc)"
    tail -n 12 "$build_log"
    echo
    echo "generic_ctor: FAIL"
    exit 1
fi

units="$(find "$WORKDIR/.cache" -maxdepth 1 -name '*.obj.cgu.*' ! -name '*.list' 2>/dev/null | wc -l)"
# `find | wc -l` can carry trailing whitespace in some shells
units="$(printf '%s' "$units" | tr -d '[:space:]')"
if [ "${units:-0}" -lt 2 ]; then
    bad "build produced ${units:-0} codegen units -- the partition never split the fixture, so this gate proves nothing"
else
    note "units=$units (fixture is split, cross-unit materialization is exercised)"
fi

echo
echo "-- 2/3 run: every field must have been initialized --"
exe="$WORKDIR/target/cgu_r8_ctor.exe"
if [ ! -x "$exe" ]; then
    bad "no binary at $exe"
    echo
    echo "generic_ctor: FAIL"
    exit 1
fi
run_log="$WORKDIR/run.log"
if ( cd "$WORKDIR" && "$exe" ) >"$run_log" 2>&1
then rrc=0; else rrc=$?; fi
cat "$run_log" | sed 's/^/    /'

for want in "3" "7" "ctor-ok"; do
    if ! grep -qx "$want" "$run_log"; then
        bad "output is missing '$want' -- the program died before finishing (dropped field stores make push trap)"
    fi
done
if [ "$fail" -eq 0 ]; then
    note "all expected lines present"
fi

echo
echo "-- 3/3 exit code --"
if [ "$rrc" != "0" ]; then
    bad "exit code $rrc (0x$(printf '%X' "$rrc" 2>/dev/null || printf '?')) -- expected 0"
else
    note "exit code 0"
fi

echo
if [ "$fail" -eq 0 ]; then
    echo "generic_ctor: OK"
else
    echo "generic_ctor: FAIL"
fi
exit "$fail"
