#!/usr/bin/env bash
# R2 gate: the OwnedMirBody observation must reproduce every materialised
# function body exactly, and must not change what the compiler emits.
#
#   1. capture/relocate/deep-copy   — `[llvm-owned-body] bodies=N` with
#      mismatch=0 oob=0 alias-string=0 loc-ref-dead=0, and loc-ref>0 so the
#      shared-local obligation is actually exercised over the whole 4-file
#      fixture build.  loc-ref-other must be 0 here: no fixture body names
#      another function's local.
#   2. observation-only             — the published artifact hash is identical
#      with VYX_OWNED_BODY_CHECK on and off.
#   3. bounds                       — bytes/bodies is reported so peak per-body
#      ownership is measurable (the probe holds one body at a time).
#
# FULL=1 additionally runs the whole industrial_mir_stress project, which is
# the only real compiler regression corpus in this repo.
#
# Usage: bash probes/gates/cgu-parallel/owned_body.sh
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

WORKDIR="${WORKDIR:-$ROOT/out/cgu_owned_body_probe}"
rm -rf "$WORKDIR"
mkdir -p "$WORKDIR"

fail=0
note() { echo "  $1"; }
bad()  { echo "  FAIL  $1"; fail=1; }

hash_cmd() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        python -c "import hashlib,sys; print(hashlib.sha256(open(sys.argv[1],'rb').read()).hexdigest())" "$1"
    fi
}

artifact_hash() {
    local dest="$1" name rel posix
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

copy_fix() {
    local dest="$1"
    mkdir -p "$dest"
    cp "$FIX/Vyx.toml" "$dest/"
    cp -R "$FIX/src" "$dest/"
}

run_build() {
    local dest="$1" log="$2"
    shift 2
    copy_fix "$dest"
    ( cd "$dest" && env "$@" "$BOOT" build ) >"$log" 2>&1 || true
}

echo "== R2/owned-body gate =="
echo "boot: $BOOT"
echo

# ── 1: capture / check ───────────────────────────────────────────────
echo "-- capture: deep copy + relocation + string ownership --"
run_build "$WORKDIR/on" "$WORKDIR/on.log" \
    VYX_PHASE_SUMMARY=1 VYX_CGU_PARALLEL=lazy VYX_CGU_THREADS=2 VYX_OWNED_BODY_CHECK=1

summary="$(grep -m1 '^\[llvm-owned-body\] bodies=' "$WORKDIR/on.log" || true)"
if [ -z "$summary" ]; then
    bad "no owned-body summary line (is VYX_OWNED_BODY_CHECK wired?)"
else
    note "$summary"
    bodies="$(printf '%s' "$summary" | sed -n 's/.*bodies=\([0-9]*\).*/\1/p')"
    mismatch="$(printf '%s' "$summary" | sed -n 's/.*mismatch=\([0-9]*\).*/\1/p')"
    oob="$(printf '%s' "$summary" | sed -n 's/.* oob=\([0-9]*\).*/\1/p')"
    alias="$(printf '%s' "$summary" | sed -n 's/.*alias-string=\([0-9]*\).*/\1/p')"
    bytes="$(printf '%s' "$summary" | sed -n 's/.*bytes=\([0-9]*\).*/\1/p')"
    locref="$(printf '%s' "$summary" | sed -n 's/.* loc-ref=\([0-9]*\).*/\1/p')"
    locother="$(printf '%s' "$summary" | sed -n 's/.*loc-ref-other=\([0-9]*\).*/\1/p')"
    locdead="$(printf '%s' "$summary" | sed -n 's/.*loc-ref-dead=\([0-9]*\).*/\1/p')"
    if [ "${bodies:-0}" -lt 1 ]; then bad "bodies=$bodies (nothing captured)"; fi
    if [ "${mismatch:-1}" != "0" ]; then bad "mismatch=$mismatch"; fi
    if [ "${oob:-1}" != "0" ]; then bad "oob=$oob (body references outside its own range)"; fi
    if [ "${alias:-1}" != "0" ]; then bad "alias-string=$alias (shallow string copy)"; fi
    # Locals are shared metadata, so the body's local obligation splits in two:
    # a reference that resolves to nothing is dangling (fail), a reference that
    # resolves to another function's live local is legal but coupled (counted).
    if [ "${locdead:-1}" != "0" ]; then bad "loc-ref-dead=$locdead (dangling local reference)"; fi
    if [ "${locother:-1}" != "0" ]; then bad "loc-ref-other=$locother (fixture must not borrow locals)"; fi
    if [ "${locref:-0}" -lt 1 ]; then bad "loc-ref=$locref (local obligation never exercised)"; fi
    if [ -n "${bodies:-}" ] && [ "${bodies:-0}" -gt 0 ]; then
        note "per-body bytes = $(( ${bytes:-0} / bodies ))"
        # Failure detail lines only print when a body disagrees.
        if grep -q '^\[llvm-owned-body\] fn=' "$WORKDIR/on.log"; then
            bad "per-body detail lines present:"
            grep '^\[llvm-owned-body\] fn=' "$WORKDIR/on.log" | head -n 10
        fi
    fi
fi

# ── 2: observation-only ──────────────────────────────────────────────
echo
echo "-- observation-only: artifact hash unchanged by the check --"
run_build "$WORKDIR/off" "$WORKDIR/off.log" \
    VYX_PHASE_SUMMARY=1 VYX_CGU_PARALLEL=lazy VYX_CGU_THREADS=2
h_on="$(artifact_hash "$WORKDIR/on")"
h_off="$(artifact_hash "$WORKDIR/off")"
if [ "$h_on" = "NO-LIST" ] || [ "$h_off" = "NO-LIST" ]; then
    bad "missing .cgu.list (on=$h_on off=$h_off)"
elif [ "$h_on" != "$h_off" ]; then
    bad "artifact hash changed when the check was enabled"
    note "  on  $h_on"
    note "  off $h_off"
else
    note "artifact hash identical with the check on and off"
fi

# ── 3: real corpus (opt-in; industrial_mir_stress takes a few minutes) ──
if [ "${FULL:-0}" = "1" ]; then
    echo
    echo "-- corpus: industrial_mir_stress --"
    CORPUS="$ROOT/tests/projects/industrial_mir_stress"
    if [ ! -f "$CORPUS/Vyx.toml" ]; then
        bad "corpus project not found at $CORPUS"
    else
        rm -rf "$WORKDIR/corpus"
        mkdir -p "$WORKDIR/corpus"
        cp "$CORPUS/Vyx.toml" "$WORKDIR/corpus/"
        cp -R "$CORPUS/src" "$WORKDIR/corpus/"
        ( cd "$WORKDIR/corpus" \
          && env VYX_PHASE_SUMMARY=1 VYX_CGU_PARALLEL=lazy VYX_CGU_THREADS=4 \
                   VYX_OWNED_BODY_CHECK=1 "$BOOT" build ) >"$WORKDIR/corpus.log" 2>&1 || true
        csum="$(grep -m1 '^\[llvm-owned-body\] bodies=' "$WORKDIR/corpus.log" || true)"
        if [ -z "$csum" ]; then
            bad "corpus produced no owned-body summary (see $WORKDIR/corpus.log)"
        else
            note "$csum"
            for field in mismatch oob alias-string loc-ref-dead; do
                v="$(printf '%s' "$csum" | sed -n "s/.*${field}=\([0-9]*\).*/\1/p")"
                [ "$v" = "0" ] || bad "corpus ${field}=$v"
            done
            locref="$(printf '%s' "$csum" | sed -n 's/.* loc-ref=\([0-9]*\).*/\1/p')"
            [ "${locref:-0}" -ge 1 ] || bad "corpus loc-ref=$locref (never exercised)"
            # Recorded baseline: generated glue (closure / callback adapters)
            # names the original function's locals.  Expected to be non-zero
            # until R4 gives each body its own local edge table; a *change* here
            # means the coupling grew or shrank and wants an explanation.
            locother="$(printf '%s' "$csum" | sed -n 's/.*loc-ref-other=\([0-9]*\).*/\1/p')"
            if [ "${locother:-X}" != "${LOC_OTHER_BASELINE:-28}" ]; then
                bad "corpus loc-ref-other=$locother (baseline ${LOC_OTHER_BASELINE:-28})"
            else
                note "loc-ref-other=$locother (recorded baseline)"
            fi
            if grep -q '^\[llvm-owned-body\] fn=' "$WORKDIR/corpus.log"; then
                bad "corpus per-body detail lines present:"
                grep '^\[llvm-owned-body\] fn=' "$WORKDIR/corpus.log" | head -n 10
            fi
        fi
    fi
else
    echo
    echo "-- corpus: skipped (set FULL=1 to run industrial_mir_stress) --"
fi

echo
if [ "$fail" -eq 0 ]; then
    echo "owned-body: OK"
else
    echo "owned-body: FAIL"
    echo "----- on.log -----"
    tail -n 30 "$WORKDIR/on.log" || true
fi
exit "$fail"
