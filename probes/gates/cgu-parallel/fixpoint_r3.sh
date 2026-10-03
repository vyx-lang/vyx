#!/usr/bin/env bash
# Gate B for the R3 change, Windows/Git-Bash flavour.
#
#   S1 = the compiler currently in out/  (built from R3 sources by the R2 boot)
#   S2 = S1 rebuilds the tree
#   S3 = S2 rebuilds the tree, with `.cache` moved aside first
#
# A fixpoint can pass for the wrong reason if a generation reuses the previous
# generation's objects. The cache is moved aside to prevent that. S2 and S3
# have byte-identical *sources*, so an all-hit S3 build would make `cmp S2 S3`
# true by construction.  Removing the cache forces S2 to actually emit every
# object, which is what "S2 reproduces itself" has to mean.
#
# `VYX_PHASE_SUMMARY=1` is set for both generations so the run doubles as the
# evidence harvest for the R3 freeze plan on the compiler's own source.  The
# switch is print-only (all 17 uses in `src/` are print guards), and it is set
# identically for both generations, so it cannot make the comparison unfair.
#
# Usage: bash probes/gates/cgu-parallel/fixpoint_r3.sh
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
BC="$ROOT/bootstrap_compiler"
# LLVM_ROOT must be a Windows path (`E:/Dev/...`), not MSYS (`/e/Dev/...`) --
# see the note in baseline.sh.  Getting this wrong is not cosmetic: the C++
# host jobs in `vyx_codegen/src` fail to spawn and the build dies in 1 second.
if [ -z "${LLVM_ROOT:-}" ]; then
    ROOT_WIN="$(cygpath -m "$ROOT" 2>/dev/null || printf '%s' "$ROOT")"
    LLVM_ROOT="$ROOT_WIN/clang"
fi
export LLVM_ROOT
export PATH="$ROOT/clang/bin:${LLVM_ROOT}/bin:${PATH:-}"
export VYX_PHASE_SUMMARY=1

LOG="$ROOT/out/r3_fixpoint.log"
: >"$LOG"
say() { echo "$@" | tee -a "$LOG"; }

say "== R3 fixpoint =="
say "boot: $BC/out/boot.exe"

build_once() {           # build_once <compiler> <logfile> <label>
    local cc="$1" log="$2" label="$3"
    local t0 t1 rc
    t0=$(date +%s)
    ( cd "$BC" && "$cc" build -j10 ) >"$log" 2>&1
    rc=$?
    t1=$(date +%s)
    say "$label: rc=$rc elapsed=$((t1 - t0))s log=$log"
    if [ "$rc" != "0" ]; then
        # A "failed to start compile job" here is almost always a malformed
        # LLVM_ROOT rather than a broken toolchain; the build dumps the exact
        # command line it could not spawn, so print it instead of making the
        # next person re-derive it.
        say "  --- build errors ---"
        grep -m5 'error' "$log" 2>/dev/null | sed 's/^/  /' | tee -a "$LOG"
        if [ -f "$BC/.cache/_spawn_fail_task.txt" ]; then
            say "  --- .cache/_spawn_fail_task.txt ---"
            sed 's/^/  /' "$BC/.cache/_spawn_fail_task.txt" | tee -a "$LOG"
        fi
    fi
    return "$rc"
}

# ── S2 ────────────────────────────────────────────────────────────────
if ! build_once "$BC/out/boot.exe" "$ROOT/out/r3_s2.log" S2; then
    say "FAIL: S2 build failed"; exit 1
fi
cp "$BC/out/boot.exe" "$BC/out/_s2_r3.exe" || exit 1

# ── S3, cold cache ────────────────────────────────────────────────────
if [ -d "$BC/.cache" ]; then
    # Timestamped stash: a fixed name makes the *second* run of this script move
    # `.cache` *inside* the existing stash directory (mv semantics), leaving the
    # real `.cache` in place and silently turning S3 into an incremental build --
    # which is exactly the false fixpoint this script exists to prevent.
    STASH="$BC/.cache.r3_stash.$(date +%Y%m%d-%H%M%S)"
    if [ -e "$STASH" ]; then say "FAIL: stash already exists: $STASH"; exit 1; fi
    mv "$BC/.cache" "$STASH" || exit 1
    say "cache moved aside -> $(basename "$STASH") (S3 is a full rebuild)"
fi
if ! build_once "$BC/out/_s2_r3.exe" "$ROOT/out/r3_s3.log" S3; then
    say "FAIL: S3 build failed"; exit 1
fi
cp "$BC/out/boot.exe" "$BC/out/_s3_r3.exe" || exit 1

# ── compare ───────────────────────────────────────────────────────────
say ""
say "-- provenance: what each generation actually emitted --"
# A generation that skipped codegen shows zero "compile" lines.
for g in s2 s3; do
    n="$(grep -c 'compile' "$ROOT/out/r3_$g.log" 2>/dev/null || true)"
    say "  $g: compile-lines=${n:-0}"
done
say "  S2 objects: cache present (incremental)   S3 objects: cache cleared (full)"
say ""
if cmp -s "$BC/out/_s2_r3.exe" "$BC/out/_s3_r3.exe"; then
    say "FIXPOINT OK  size=$(stat -c %s "$BC/out/_s2_r3.exe") bytes"
    say "  sha256 $(sha256sum "$BC/out/_s2_r3.exe" | cut -d' ' -f1)"
    rc=0
else
    say "FIXPOINT BROKEN"
    ls -l "$BC/out/_s2_r3.exe" "$BC/out/_s3_r3.exe"
    rc=1
fi

# ── self-host freeze numbers, aggregated over every module ────────────
# Aggregate the *summary* lines only: `unplanned` and friends appear twice per
# module (once in the seal line, once in the summary), so grepping the whole log
# would double-count.
SUM="$ROOT/out/r3_s2_freeze_summary.txt"
grep '^\[llvm-cgu\] freeze-summary' "$ROOT/out/r3_s2.log" >"$SUM" 2>/dev/null || true
say ""
say "-- R3 freeze evidence on the compiler's own source (S2 generation) --"
say "  modules that sealed a plan: $(wc -l <"$SUM" | tr -d ' ')"
for k in unplanned bodiless late-definitions late-units strict-violations; do
    tot=$(grep -o "$k=[0-9]*" "$SUM" | cut -d= -f2 | awk '{s+=$1} END {print s+0}')
    mx=$(grep -o "$k=[0-9]*" "$SUM" | cut -d= -f2 | sort -n | tail -1)
    say "  $k: total=$tot max-in-one-module=${mx:-n/a}"
done
say ""
say "s2=$BC/out/_s2_r3.exe  s3=$BC/out/_s3_r3.exe"
exit "$rc"
