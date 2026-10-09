#!/usr/bin/env bash
# R4 gate (second slice): every driver emit mode still publishes its artifact,
# and none of them leaks the staging names the publish transaction introduced.
#
# `driver_publish_object_family` (src/core/driver/main.vyx) renames a staged generation
# onto the names the caller asked for.  That is invisible to the caller by
# design, which is also why it needs a gate: a mistake there produces either a
# missing artifact or an artifact under the wrong name, and a gate that only
# ever looks at `--emit=obj` (section D of artifacts.sh) would not notice.
#
# What is asserted, per mode:
#   exe   the executable exists, runs, and prints what the source prints
#   run   `--run=aot` runs it without leaving the executable behind
#   obj   the object exists and defines the symbol it should
#   lib   the archive exists and contains the object
#   ir    LLVM IR text, not an object
#   and, after every mode, that no `<out>.stage.obj*` file survives -- the
#   staging layer is an implementation detail, so a leftover is a leak.
#
# Usage: bash probes/gates/cgu-parallel/driver_modes.sh
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
BOOT="${BOOT:-$ROOT/bootstrap_compiler/out/boot}"
if [ -x "${BOOT}.exe" ] && [ ! -x "$BOOT" ]; then BOOT="${BOOT}.exe"; fi
if [ ! -x "$BOOT" ]; then echo "FAIL: missing compiler $BOOT"; exit 1; fi

# LLVM_ROOT must be a Windows path (`E:/Dev/...`), not MSYS (`/e/Dev/...`); see
# the note in baseline.sh.
if [ -z "${LLVM_ROOT:-}" ]; then
    ROOT_WIN="$(cygpath -m "$ROOT" 2>/dev/null || printf '%s' "$ROOT")"
    LLVM_ROOT="$ROOT_WIN/clang"
fi
export LLVM_ROOT
export PATH="$ROOT/clang/bin:${LLVM_ROOT}/bin:${PATH:-}"

WORKDIR="${WORKDIR:-$ROOT/out/cgu_driver_modes}"
rm -rf "$WORKDIR"; mkdir -p "$WORKDIR"
fail=0
note() { echo "  $1"; }
bad()  { echo "  FAIL  $1"; fail=1; }

SRC="$WORKDIR/modes.vyx"
cat >"$SRC" <<'VYX'
fn bump(a: i32) -> i32 {
    return a + 41;
}

fn main() -> i32 {
    print("driver-modes");
    return bump(1) - 42;
}
VYX
W_SRC="$(cygpath -m "$SRC")"

# Any staged file left behind, anywhere under the output directory.  The staging
# suffix sits in the middle of the name (`prog.exe.stage.obj...`), so the glob
# has to be `*.stage.obj*`, not `.stage.obj*`.
staging_leaks() {
    ( cd "$WORKDIR" && ls */*.stage.obj* ./*.stage.obj* 2>/dev/null \
      | grep -E '\.stage\.obj(\.cgu\.[0-9]+|\.cgu\.list)?$' ) || true
}

# `llvm-nm` is a Windows binary: an MSYS path gets "no such file or directory",
# and `2>/dev/null` would turn that into an empty symbol list -- i.e. a check
# that passes because it never ran.
NM="$ROOT/clang/bin/llvm-nm.exe"
check_no_leak() {   # check_no_leak <mode>
    local leaks; leaks="$(staging_leaks)"
    if [ -n "$leaks" ]; then
        bad "$1: staging files leaked: $(printf '%s ' $leaks)"
    else
        note "$1: no staging files left behind"
    fi
}

echo "== R4/driver emit modes =="
echo "boot: $BOOT"
echo

# ── exe ───────────────────────────────────────────────────────────────
echo "-- exe --"
D="$WORKDIR/exe"; mkdir -p "$D"
EXE="$D/prog.exe"; W_EXE="$(cygpath -m "$EXE")"
if "$BOOT" --src=file "$W_SRC" --emit=exe -o "$W_EXE" >"$WORKDIR/exe.log" 2>&1; then
    if [ ! -s "$EXE" ]; then
        bad "exe: no executable produced"
    else
        note "exe: produced $(basename "$EXE") ($(stat -c %s "$EXE") bytes)"
        out="$("$EXE" 2>&1)"; rc=$?
        if [ "$out" != "driver-modes" ] || [ "$rc" != "0" ]; then
            bad "exe: ran with rc=$rc output='$out' (expected 'driver-modes', rc=0)"
        else
            note "exe: runs and prints 'driver-modes' (rc=0)"
        fi
    fi
else
    bad "exe: compile failed"; tail -n 5 "$WORKDIR/exe.log" | sed 's/^/        /'
fi
check_no_leak exe

# ── run=aot ───────────────────────────────────────────────────────────
echo
echo "-- run=aot --"
D="$WORKDIR/run"; mkdir -p "$D"
EXE="$D/prog.exe"; W_EXE="$(cygpath -m "$EXE")"
if "$BOOT" --src=file "$W_SRC" --run=aot -o "$W_EXE" >"$WORKDIR/run.log" 2>&1; then
    note "run=aot: rc=0 (the program's own exit code is the driver's)"
    if [ -e "$EXE" ]; then
        note "note: the temporary executable is still present"
    fi
else
    bad "run=aot: driver returned non-zero"; tail -n 5 "$WORKDIR/run.log" | sed 's/^/        /'
fi
check_no_leak run

# ── obj ───────────────────────────────────────────────────────────────
echo
echo "-- obj --"
D="$WORKDIR/obj"; mkdir -p "$D"
OBJ="$D/prog.o"; W_OBJ="$(cygpath -m "$OBJ")"
if "$BOOT" --src=file "$W_SRC" --emit=obj -o "$W_OBJ" >"$WORKDIR/obj.log" 2>&1; then
    if [ ! -s "$OBJ" ]; then
        bad "obj: no object produced"
    else
        syms="$("$NM" --defined-only "$(cygpath -m "$OBJ")" | awk '{print $NF}')"
        if [ -z "$syms" ]; then
            bad "obj: llvm-nm produced no symbols, so the check would pass vacuously"
        elif printf '%s\n' "$syms" | grep -q '^main$'; then
            note "obj: object defines 'main'"
        else
            bad "obj: object does not define 'main'"
        fi
    fi
else
    bad "obj: compile failed"; tail -n 5 "$WORKDIR/obj.log" | sed 's/^/        /'
fi
check_no_leak obj

# ── lib ───────────────────────────────────────────────────────────────
echo
echo "-- lib --"
D="$WORKDIR/lib"; mkdir -p "$D"
LIB="$D/libprog.a"; W_LIB="$(cygpath -m "$LIB")"
if "$BOOT" --src=file "$W_SRC" --emit=lib -o "$W_LIB" >"$WORKDIR/lib.log" 2>&1; then
    if [ ! -s "$LIB" ]; then
        bad "lib: no archive produced"
    else
        members="$("$ROOT/clang/bin/llvm-ar.exe" t "$(cygpath -m "$LIB")" 2>/dev/null | grep . || true)"
        n_members="$(printf '%s' "$members" | grep -c . || true)"
        if [ "${n_members:-0}" = "0" ]; then
            bad "lib: the archive has no members"
        else
            note "lib: produced $(basename "$LIB") ($(stat -c %s "$LIB") bytes, $n_members member(s))"
        fi
    fi
else
    bad "lib: compile failed"; tail -n 5 "$WORKDIR/lib.log" | sed 's/^/        /'
fi
check_no_leak lib

# ── ir ────────────────────────────────────────────────────────────────
echo
echo "-- ir --"
D="$WORKDIR/ir"; mkdir -p "$D"
LL="$D/prog.ll"; W_LL="$(cygpath -m "$LL")"
if "$BOOT" --src=file "$W_SRC" --emit=ir -o "$W_LL" >"$WORKDIR/ir.log" 2>&1; then
    if [ ! -s "$LL" ]; then
        bad "ir: no IR produced"
    elif ! grep -q '^target triple' "$LL"; then
        # Not `head -c 200 | grep define`: the module opens with the ModuleID,
        # datalayout and triple, and the first `define` comes after the globals,
        # so a short-prefix probe fails on correct output.
        bad "ir: output does not look like LLVM IR (no target triple)"
    elif ! grep -q '^define ' "$LL"; then
        bad "ir: IR contains no function definition"
    else
        note "ir: produced LLVM IR text with $(grep -c '^define ' "$LL") function definition(s)"
    fi
else
    bad "ir: compile failed"; tail -n 5 "$WORKDIR/ir.log" | sed 's/^/        /'
fi
check_no_leak ir

# ── keep-obj: the published names, not the staging names ──────────────
# This is the assertion that the rename really lands on the caller's path: the
# caller asked for `prog.exe`, so the object it is allowed to see is
# `prog.exe.tmp.obj` -- never `prog.exe.tmp.obj.stage.obj`.
echo
echo "-- exe --keep-obj --"
D="$WORKDIR/keep"; mkdir -p "$D"
EXE="$D/prog.exe"; W_EXE="$(cygpath -m "$EXE")"
OBJ="$EXE.tmp.obj"
if "$BOOT" --src=file "$W_SRC" --emit=exe --keep-obj -o "$W_EXE" >"$WORKDIR/keep.log" 2>&1; then
    if [ -s "$OBJ" ]; then
        note "keep-obj: object published at the caller's path ($(basename "$OBJ"))"
    else
        bad "keep-obj: no object at $(basename "$OBJ")"
    fi
    if [ -s "$EXE" ]; then
        note "keep-obj: executable still produced"
    else
        bad "keep-obj: no executable produced"
    fi
else
    bad "keep-obj: compile failed"; tail -n 5 "$WORKDIR/keep.log" | sed 's/^/        /'
fi
check_no_leak keep-obj

echo
if [ "$fail" -eq 0 ]; then echo "driver_modes: OK"; else echo "driver_modes: FAIL"; fi
exit "$fail"
