#!/usr/bin/env bash
# R0 runtime probes for CGU parallel work:
#   1. off / lazy / full produce the same object hashes on a 4-file fixture
#   2. full actually runs the parallel lowering path (effective=full)
#   3. lazy pool path prints one [llvm-cgu] task line per unit
#   4. VYX_CGU_FAIL_UNIT injects a structured failure and does not publish success
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
if [ -n "${OS:-}" ] || [ -d /c ]; then
    export OS="${OS:-Windows_NT}"
fi

WORKDIR="${WORKDIR:-$ROOT/out/cgu_r0_probe}"
rm -rf "$WORKDIR"
mkdir -p "$WORKDIR"

hash_cmd() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        python -c "import hashlib,sys; print(hashlib.sha256(open(sys.argv[1],'rb').read()).hexdigest())" "$1"
    fi
}

copy_fix() {
    local dest="$1"
    mkdir -p "$dest"
    cp "$FIX/Vyx.toml" "$dest/"
    cp -R "$FIX/src" "$dest/"
}

run_build() {
    local dest="$1"
    local log="$2"
    shift 2
    copy_fix "$dest"
    (
        cd "$dest"
        env "$@" "$BOOT" build
    ) >"$log" 2>&1 || true
}

artifact_hash() {
    local dest="$1"
    local name rel posix
    name="$(find "$dest/.cache" -name '*.cgu.list' 2>/dev/null | head -n 1)"
    if [ -z "$name" ]; then
        echo "NO-LIST"
        return
    fi
    {
        hash_cmd "$name"
        while IFS= read -r p; do
            [ -z "$p" ] && continue
            posix="${p//\\//}"
            if [ -f "$posix" ]; then
                hash_cmd "$posix"
            elif [ -f "$dest/$posix" ]; then
                hash_cmd "$dest/$posix"
            fi
        done < "$name"
    } | tr '\n' ' '
}

fail=0

# --- 1/2/3: off vs lazy vs full ---
run_build "$WORKDIR/off" "$WORKDIR/off.log" \
    VYX_PHASE_SUMMARY=1 VYX_CGU_PARALLEL=off VYX_CGU_THREADS=0
run_build "$WORKDIR/lazy" "$WORKDIR/lazy.log" \
    VYX_PHASE_SUMMARY=1 VYX_CGU_PARALLEL=lazy VYX_CGU_THREADS=2
run_build "$WORKDIR/full" "$WORKDIR/full.log" \
    VYX_PHASE_SUMMARY=1 VYX_CGU_PARALLEL=full VYX_CGU_THREADS=2

for mode in off lazy full; do
    if ! grep -q '\[llvm-cgu\] units=' "$WORKDIR/$mode.log"; then
        echo "FAIL: $mode missing units= line"
        fail=1
    fi
    if ! grep -q '\[llvm-cgu\] rss ' "$WORKDIR/$mode.log"; then
        echo "FAIL: $mode missing rss line"
        fail=1
    fi
done

if ! grep -q 'effective=off' "$WORKDIR/off.log"; then
    echo "FAIL: off did not print effective=off"
    fail=1
fi
if ! grep -q 'effective=lazy' "$WORKDIR/lazy.log"; then
    echo "FAIL: lazy did not print effective=lazy"
    fail=1
fi
if ! grep -q 'parallel=full' "$WORKDIR/full.log"; then
    echo "FAIL: full did not print parallel=full"
    fail=1
fi
if ! grep -q 'effective=full' "$WORKDIR/full.log"; then
    echo "FAIL: full did not run the parallel lowering path (expected effective=full)"
    fail=1
fi

# Task lines: off is all inline; lazy/full must have at least one pool line
# and one inline last-unit line when units>1.
units=$(grep -o 'units=[0-9]*' "$WORKDIR/off.log" | head -n 1 | cut -d= -f2 || echo 0)
if [ "${units:-0}" -lt 2 ]; then
    echo "FAIL: fixture produced units=$units (need >=2)"
    fail=1
fi

off_tasks=$(grep -c '\[llvm-cgu\] task unit=' "$WORKDIR/off.log" || true)
lazy_tasks=$(grep -c '\[llvm-cgu\] task unit=' "$WORKDIR/lazy.log" || true)
if [ "$off_tasks" -lt "$units" ]; then
    echo "FAIL: off task lines=$off_tasks expected >= $units"
    fail=1
fi
if [ "$lazy_tasks" -lt "$units" ]; then
    echo "FAIL: lazy task lines=$lazy_tasks expected >= $units"
    fail=1
fi
if ! grep -q 'where=inline' "$WORKDIR/off.log"; then
    echo "FAIL: off missing where=inline"
    fail=1
fi
if ! grep -q 'where=pool' "$WORKDIR/lazy.log"; then
    echo "FAIL: lazy missing where=pool (emit pool not engaged)"
    fail=1
fi
# §8.1 (commit a4ccce8e / ce5bff5c): the main thread is a pool member and
# drains the queue as `where=drain`; it is no longer a privileged inline last
# unit.  Asserting `where=inline` here went stale with that change and read as
# a regression in the pool rather than as an outdated expectation.
if ! grep -q 'where=drain' "$WORKDIR/lazy.log"; then
    echo "FAIL: lazy missing where=drain (main thread did not drain the queue)"
    fail=1
fi
# Every unit must produce exactly one task line: fewer means a unit was
# emitted without a record, more means a unit was retried or double-counted.
if [ "$lazy_tasks" -ne "$units" ]; then
    echo "FAIL: lazy task lines=$lazy_tasks expected exactly $units"
    fail=1
fi

hoff=$(artifact_hash "$WORKDIR/off")
hlazy=$(artifact_hash "$WORKDIR/lazy")
hfull=$(artifact_hash "$WORKDIR/full")
if [ "$hoff" = "NO-LIST" ] || [ "$hlazy" = "NO-LIST" ] || [ "$hfull" = "NO-LIST" ]; then
    echo "FAIL: missing .cgu.list (off=$hoff lazy=$hlazy full=$hfull)"
    fail=1
elif [ "$hoff" != "$hlazy" ] || [ "$hoff" != "$hfull" ]; then
    echo "FAIL: object hashes differ across off/lazy/full"
    echo "  off  $hoff"
    echo "  lazy $hlazy"
    echo "  full $hfull"
    fail=1
fi

# --- 4: injected failure ---
run_build "$WORKDIR/fail" "$WORKDIR/fail.log" \
    VYX_PHASE_SUMMARY=1 VYX_CGU_PARALLEL=off VYX_CGU_FAIL_UNIT=1
if grep -q 'built ' "$WORKDIR/fail.log" && ! grep -qiE 'error:|failed' "$WORKDIR/fail.log"; then
    echo "FAIL: injected failure still reported success"
    fail=1
fi
if ! grep -q 'object emission failed for codegen unit 1' "$WORKDIR/fail.log"; then
    echo "FAIL: injected failure did not report unit 1"
    fail=1
fi
if [ -f "$WORKDIR/fail/target/cgu_r0_fixture.exe" ] || [ -f "$WORKDIR/fail/target/cgu_r0_fixture" ]; then
    echo "FAIL: injected failure published a final executable"
    fail=1
fi

if [ "$fail" != 0 ]; then
    echo "----- off.log -----"
    tail -n 40 "$WORKDIR/off.log" || true
    echo "----- lazy.log -----"
    tail -n 40 "$WORKDIR/lazy.log" || true
    echo "----- full.log -----"
    tail -n 40 "$WORKDIR/full.log" || true
    echo "----- fail.log -----"
    tail -n 40 "$WORKDIR/fail.log" || true
    echo "cgu-parallel run FAIL"
    exit 1
fi

echo "OK: cgu-parallel run (off/lazy/full hashes match, task trace present, fail-unit injected)"
exit 0
