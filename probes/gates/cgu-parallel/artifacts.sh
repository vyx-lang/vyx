#!/usr/bin/env bash
# R4 gate: the codegen-unit object family is complete, unique and published
# atomically.
#
# Manifest publication contract:
#
#   1. 每个任务只写自己的唯一临时路径，关闭并检查完整性，计算 digest。
#   2. 成功结果按 CguKey 收集；检查计划内每个必需定义/CGU 都有结果，
#      不接收重复或缺失。
#   5. 同文件系统内原子替换 manifest/提交指针；链接只读取已提交的一代。
#   6. 最终可执行文件也从临时文件原子发布。失败保留上一代可用输出，
#      但本次命令必须返回失败，不能运行旧产物冒充成功。
#
# This script checks (1)(2)(5)(6) for the object family on the two publish paths
# that exist today: the **build-system** path (`boot build`, a rename transaction
# in the parent) and the **driver** path (`boot --emit/--src` from the CLI, which
# stages the family next to its final name and renames it after the compile).
#
# Sections:
#   A   completeness   exactly `units` non-empty objects; `<obj>.cgu.list` names
#                      exactly those and nothing else; no global symbol defined
#                      twice in the family
#   B   determinism    two identical builds produce byte-identical families
#   C   rollback       a failing build leaves the previous family byte-identical
#   C2  rollback       ... also for a failure *inside* codegen, injected with
#                      `VYX_CGU_ABORT_AFTER_PLAN`, which is the only window that
#                      reaches the commit
#   D   driver path    a failed `--emit=obj -o` recompile must not consume the
#                      previous object, for a front-end failure (D1/D2) and for
#                      an injected codegen-stage failure (D3/D4)
#   E   stale manifest a leftover `<obj>.cgu.list` must not be read as a compile
#                      result
#   F   commit gate    a staged generation that lost a member between codegen
#                      and the commit (`VYX_CGU_PUBLISH_DROP_UNIT`) must be
#                      refused *before* the previous generation is touched
#   G   publish gate   a generation with an empty member (`VYX_CGU_EMPTY_UNIT`,
#                      i.e. a write that reported success) must not get a
#                      manifest at all
#
# Usage: bash probes/gates/cgu-parallel/artifacts.sh
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
FIX="$ROOT/probes/gates/cgu-parallel/fixture"
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
export CODEBUDDY_SAFE_DELETE_BULK_THRESHOLD=1000000

NM="$ROOT/clang/bin/llvm-nm.exe"
OBJ_REL=".cache/crate_cgu_r0_fixture.obj"
LIST_REL="$OBJ_REL.cgu.list"

WORKDIR="${WORKDIR:-$ROOT/out/cgu_artifacts_probe}"
fail=0
note() { echo "  $1"; }
bad()  { echo "  FAIL  $1"; fail=1; }
# The driver path *was* the anomaly in the first R4 slice: it deleted the
# previous family before codegen, so a failure inside codegen left nothing
# usable.  It stages and renames now, and sections D3/D4 assert it.  The
# DRIVER_ATOMIC escape hatch that made those sections report-instead-of-fail is
# gone with the defect; anything this reaches is a regression.
record_driver_defect() {
    bad "$1"
}

new_proj() {
    local dest="$1"
    rm -rf "$dest"
    mkdir -p "$dest"
    cp "$FIX/Vyx.toml" "$dest/"
    cp -R "$FIX/src" "$dest/"
}

run_build() {   # run_build <dir> <log> [env...]
    local d="$1" log="$2"; shift 2
    ( cd "$d" && env "$@" "$BOOT" build ) >"$log" 2>&1
    return $?
}

# Sorted list of sibling object names (no path), i.e. `.cgu.<n>` only.
siblings() {
    ( cd "$1/.cache" 2>/dev/null && ls 2>/dev/null \
      | grep -E '^crate_cgu_r0_fixture\.obj\.cgu\.[0-9]+$' | sort -V ) || true
}

# Every family member that exists, in list order: primary, siblings, list file.
family_files() {
    local d="$1"
    printf '%s\n' "$OBJ_REL"
    siblings "$d" | sed "s|^|.cache/|"
    [ -f "$d/$LIST_REL" ] && printf '%s\n' "$LIST_REL"
}

family_hashes() {   # absolute paths, sha256, sorted -- stable across dirs
    ( cd "$1" && for f in $(family_files "$1"); do
          [ -f "$f" ] && sha256sum "$f" | sed "s|  .*/|  |"
      done | sort -k2 )
}

echo "== R4/sealed-artifact-family gate =="
echo "boot: $BOOT"
echo

# ── A + B: completeness, manifest fidelity, uniqueness ────────────────
echo "-- A/B: completeness, manifest, uniqueness --"
D1="$WORKDIR/a"
new_proj "$D1"
if ! run_build "$D1" "$WORKDIR/a.log" VYX_PHASE_SUMMARY=1 VYX_CGU_PARALLEL=off VYX_CGU_THREADS=2; then
    bad "baseline fixture build failed"
    tail -n 10 "$WORKDIR/a.log"
else
    units="$(grep -o 'freeze-summary units=[0-9]*' "$WORKDIR/a.log" | head -1 | cut -d= -f2)"
    units="${units:-$(grep -o 'units=[0-9]*' "$WORKDIR/a.log" | head -1 | cut -d= -f2)}"
    note "units=$units (from the build log)"
    sibs="$(siblings "$D1")"
    n_sibs="$(printf '%s' "$sibs" | grep -c . || true)"
    if [ "${units:-0}" -lt 2 ]; then
        bad "fixture did not split into >=2 units (units=$units); the family checks need a split"
    elif [ "$n_sibs" != "$((units - 1))" ]; then
        bad "family has $n_sibs siblings, expected $((units - 1)) for units=$units"
        printf '%s\n' "$sibs" | sed 's/^/      /'
    else
        note "family: 1 primary + $n_sibs siblings = $units objects (complete)"
    fi
    # no empty artifact
    for f in $(family_files "$D1"); do
        if [ ! -s "$D1/$f" ]; then bad "missing or empty artifact: $f"; fi
    done
    # the manifest must name exactly the objects that exist
    if [ ! -f "$D1/$LIST_REL" ]; then
        bad "no manifest $LIST_REL"
    else
        want="$(printf '%s\n' "$OBJ_REL" $(siblings "$D1" | sed 's|^|.cache/|') | sort)"
        got="$(sed 's|\\|/|g; s|^\./||' "$D1/$LIST_REL" | grep . | sort)"
        if [ "$want" != "$got" ]; then
            bad "manifest does not name exactly the objects on disk"
            echo "      want:"; printf '%s\n' "$want" | sed 's/^/        /'
            echo "      got:";  printf '%s\n' "$got"  | sed 's/^/        /'
        else
            note "manifest names exactly the $units objects that exist"
        fi
    fi
    # uniqueness: a global symbol defined twice in one family is a link error
    # waiting to happen (internal `t` symbols are per-object by design).
    #
    # The path handed to `llvm-nm` must be a Windows path: the tool is a Windows
    # binary and answers an MSYS path (`/e/Dev/...`) with "no such file or
    # directory".  `2>/dev/null` swallowed that, so the first version of this
    # check compared nothing against nothing and reported "no duplicates" for
    # the wrong reason -- the same failure shape as everywhere else in this
    # refactor.  A count of zero has to be proved to have looked at something.
    dups=0
    fam_objs="$(family_files "$D1" | grep -E '\.obj(\.cgu\.[0-9]+)?$')"
    n_fam_objs="$(printf '%s\n' $fam_objs | grep -c . || true)"
    nm_seen=0
    : >"$WORKDIR/a.syms"
    for f in $fam_objs; do
        [ -f "$D1/$f" ] || continue
        nm_seen=$((nm_seen + 1))
        "$NM" --defined-only -g "$(cygpath -m "$D1/$f")" >>"$WORKDIR/a.syms" 2>/dev/null || true
    done
    # Two ways this check can pass for the wrong reason, both guarded: the loop
    # not visiting every object (it runs in a subshell if it is piped), and
    # `llvm-nm` producing nothing at all (it does, for an MSYS path).
    if [ "$nm_seen" != "$n_fam_objs" ]; then
        bad "uniqueness check inspected $nm_seen of $n_fam_objs family objects"
    fi
    if [ "$(grep -c . "$WORKDIR/a.syms" || true)" = "0" ]; then
        bad "uniqueness check read no global symbols from any object"
    fi
    awk '{print $NF}' "$WORKDIR/a.syms" | sort | uniq -d \
        | grep -vE '^@feat\.00$|^__real@|^__xmm@' > "$WORKDIR/a.dups" || true
    dups="$(grep -c . "$WORKDIR/a.dups" || true)"
    if [ "${dups:-0}" != "0" ]; then
        bad "$dups global symbol(s) defined in more than one object"
        head -5 "$WORKDIR/a.dups" | sed 's/^/        /'
    else
        note "no global symbol defined twice across the family"
    fi
fi

# ── B: a rebuild produces the same family ─────────────────────────────
echo
echo "-- B: rebuild determinism --"
if [ -f "$D1/$OBJ_REL" ]; then
    h1="$(family_hashes "$D1")"
    D2="$WORKDIR/b"
    new_proj "$D2"
    if ! run_build "$D2" "$WORKDIR/b.log" VYX_PHASE_SUMMARY=1 VYX_CGU_PARALLEL=off VYX_CGU_THREADS=2; then
        bad "second identical build failed"
    else
        h2="$(family_hashes "$D2")"
        if [ "$h1" = "$h2" ]; then
            note "two identical builds produced a byte-identical family"
        else
            bad "family differs between two identical builds"
            diff <(printf '%s\n' "$h1") <(printf '%s\n' "$h2") | head -10 | sed 's/^/        /'
        fi
    fi
fi

# ── C: a failing build must not consume the previous generation ───────
echo
echo "-- C: rollback on failure (build-system path) --"
if [ -f "$D1/$OBJ_REL" ]; then
    before="$(family_hashes "$D1")"
    # The sources are unchanged from the successful run above, so the build
    # would be a cache hit (`[1/2] cache src/main.vyx`) and the child compiler --
    # the process that honours VYX_CGU_FAIL_UNIT -- would never run.  The cache
    # key is a content hash, not an mtime, so `touch` is not enough: drop the
    # cache entry.  Getting this wrong makes the gate report "the injected
    # failure did not fire" as if the injection were broken, which is what the
    # first two versions of this script did.
    rm -f "$D1/.cache/.vyx_cache"
    touch "$D1/src/"*.vyx
    if run_build "$D1" "$WORKDIR/c.log" VYX_PHASE_SUMMARY=1 VYX_CGU_PARALLEL=off \
                 VYX_CGU_THREADS=2 VYX_CGU_FAIL_UNIT=2; then
        bad "VYX_CGU_FAIL_UNIT=2 build unexpectedly succeeded"
        tail -n 5 "$WORKDIR/c.log" | sed 's/^/        /'
    else
        if grep -q 'cache src/' "$WORKDIR/c.log"; then
            bad "the injected build was a cache hit, so the injection was never exercised"
        else
            note "injected unit-2 failure: build returned non-zero (correct)"
        fi
        after="$(family_hashes "$D1")"
        if [ "$before" = "$after" ]; then
            note "previous family survived byte-identical (§10.3 item 6)"
        else
            bad "a failed build changed or deleted the published family"
            diff <(printf '%s\n' "$before") <(printf '%s\n' "$after") | head -10 | sed 's/^/        /'
        fi
        # no temp debris from the aborted generation
        debris="$(cd "$D1" && ls .cache 2>/dev/null | grep -E '\.obj\.tmp|\.obj\.old|\.cgu\.list\.tmp' || true)"
        if [ -n "$debris" ]; then
            note "note: temp debris left in .cache: $(printf '%s ' $debris)"
        else
            note "no temp debris left behind"
        fi
    fi
fi

# ── C2: the same rollback, but with a failure *inside* codegen ────────
# Section C fails in the front end, so the child compiler never reaches the
# object-emission stage and the build-system transaction is not really stressed.
# `VYX_CGU_ABORT_AFTER_PLAN=2` fails after the first unit object has been
# written, i.e. after the transaction's staging directory already contains half
# a family.  This is the case §10.3 item 6 is about, and it is the case the
# driver path fails (section D3).
echo
echo "-- C2: rollback on a codegen-stage failure (build-system path) --"
if [ -f "$D1/$OBJ_REL" ]; then
    before2="$(family_hashes "$D1")"
    rm -f "$D1/.cache/.vyx_cache"
    touch "$D1/src/"*.vyx
    if run_build "$D1" "$WORKDIR/c2.log" VYX_PHASE_SUMMARY=1 VYX_CGU_PARALLEL=off \
                 VYX_CGU_THREADS=2 VYX_CGU_ABORT_AFTER_PLAN=2; then
        bad "VYX_CGU_ABORT_AFTER_PLAN=2 build unexpectedly succeeded"
        tail -n 5 "$WORKDIR/c2.log" | sed 's/^/        /'
    elif grep -q 'cache src/' "$WORKDIR/c2.log"; then
        bad "the injected build was a cache hit, so the injection was never exercised"
    elif ! grep -q 'injected, VYX_CGU_ABORT_AFTER_PLAN' "$WORKDIR/c2.log"; then
        bad "the build failed, but not through the injected codegen abort"
        tail -n 8 "$WORKDIR/c2.log" | sed 's/^/        /'
    else
        note "injected codegen-stage failure: build returned non-zero (correct)"
        after2="$(family_hashes "$D1")"
        if [ "$before2" = "$after2" ]; then
            note "previous family survived byte-identical through a codegen-stage failure"
        else
            bad "a codegen-stage failure changed or deleted the published family"
            diff <(printf '%s\n' "$before2") <(printf '%s\n' "$after2") | head -10 | sed 's/^/        /'
        fi
    fi
fi

# ── D: the driver path (`--emit=obj -o`) ─────────────────────────────
#   D1/D2  a front-end failure.  It happens before codegen, so the commit is
#          never reached and the previous object survives -- which is why a
#          front-end failure cannot show whether the driver path is atomic.
#   D3/D4  a codegen-stage failure, the window that actually matters.  Injected
#          with `VYX_CGU_ABORT_AFTER_PLAN`, which fires inside the emitter, after
#          the staged family has been opened and before it is complete.  The
#          previous generation must survive byte-identically; this is the
#          property §10.3 item 6 states as 失败保留上一代可用输出.
echo
echo "-- D: driver path (--emit=obj -o) --"
DD="$WORKDIR/d"
rm -rf "$DD"; mkdir -p "$DD"
cat >"$DD/src.vyx" <<'VYX'
fn helper(a: i32) -> i32 {
    return a + 1;
}

fn main() -> i32 {
    print("art");
    return helper(1) - 2;
}
VYX
cat >"$DD/bad.vyx" <<'VYX'
fn main() -> i32 {
    print("art");
    return no_such_function(1);
}
VYX
OBJ_OUT="$DD/art.o"
WIN_OUT="$(cygpath -m "$OBJ_OUT")"
WIN_SRC="$(cygpath -m "$DD/src.vyx")"
WIN_BAD="$(cygpath -m "$DD/bad.vyx")"

if "$BOOT" --src=file "$WIN_SRC" --emit=obj -o "$WIN_OUT" >"$WORKDIR/d1.log" 2>&1; then
    if [ ! -s "$OBJ_OUT" ]; then
        bad "successful --emit=obj produced no object"
    else
        h_before="$(sha256sum "$OBJ_OUT" | cut -d' ' -f1)"
        note "published object: $(basename "$OBJ_OUT") sha256=${h_before:0:16}…"
        # D1/D2: a later compile to the SAME output path that fails in the
        # front end.
        printf 'planted\n' >"$OBJ_OUT.cgu.list"
        "$BOOT" --src=file "$WIN_BAD" --emit=obj -o "$WIN_OUT" >"$WORKDIR/d2.log" 2>&1
        rc2=$?
        if [ "$rc2" = "0" ]; then
            bad "compile of an undefined-function source unexpectedly succeeded"
        else
            note "front-end failure returned non-zero (correct)"
            if [ ! -f "$OBJ_OUT" ]; then
                record_driver_defect "D1: the previous object was deleted by a failed compile"
            elif [ "$h_before" = "$(sha256sum "$OBJ_OUT" | cut -d' ' -f1)" ]; then
                note "D1: previous object survived byte-identical"
            else
                record_driver_defect "D1: the previous object was overwritten by a failed compile"
            fi
            if [ -f "$OBJ_OUT.cgu.list" ]; then
                note "D2: front-end failure did not reach the commit (the manifest is intact)"
                note "    it returns before codegen, so it cannot show whether the"
                note "     commit is atomic -- D3/D4 inject inside codegen instead"
                rm -f "$OBJ_OUT.cgu.list"
            else
                note "D2: front-end failure reached the family deletion (family already gone)"
            fi
        fi
        # D3/D4: a codegen-stage failure on the same output path, on a compile
        # that really does split.  `--src=project` runs the whole project in one
        # driver process, which is what produces a multi-unit family here; a
        # single file only has one module and therefore one unit.  Level 1 fails
        # with nothing written yet, level 2 after the first unit object exists,
        # so between them they cover both halves of the window.  Either way the
        # previous generation must be intact: whatever the failure wrote is not
        # a family, and a family is the only usable output.
        DPA="$WORKDIR/dproj"
        rm -rf "$DPA"; mkdir -p "$DPA"
        cp "$FIX/Vyx.toml" "$DPA/"
        cp -R "$FIX/src" "$DPA/"
        WIN_DPROJ="$(cygpath -m "$DPA")"
        drv_out="$DPA/drv.o"
        win_drv_out="$(cygpath -m "$drv_out")"
        # driver_* names below are written by the compile in $DPA.
        drv_family() {
            ( cd "$DPA" && for f in $(ls drv.o drv.o.cgu.* 2>/dev/null | sort -V); do
                  sha256sum "$f" | sed 's|  .*/|  |'
              done )
        }
        if "$BOOT" --src=project "$WIN_DPROJ" --emit=obj -o "$win_drv_out" \
               >"$WORKDIR/d3.log" 2>&1 && [ -s "$drv_out" ]; then
            drv_units="$(ls "$DPA" | grep -cE '^drv\.o\.cgu\.[0-9]+$' || true)"
            note "D3/D4 setup: --src=project published 1 primary + $drv_units siblings"
            fam_before="$(drv_family)"
            n_before="$(printf '%s' "$fam_before" | grep -c . || true)"
            lvl=1
            while [ "$lvl" -le 2 ]; do
                tag="D$((lvl + 2))"
                if env VYX_CGU_ABORT_AFTER_PLAN="$lvl" "$BOOT" --src=project "$WIN_DPROJ" \
                       --emit=obj -o "$win_drv_out" >"$WORKDIR/d$((lvl + 2)).log" 2>&1; then
                    bad "$tag: VYX_CGU_ABORT_AFTER_PLAN=$lvl compile unexpectedly succeeded"
                elif ! grep -q 'injected, VYX_CGU_ABORT_AFTER_PLAN' "$WORKDIR/d$((lvl + 2)).log"; then
                    bad "$tag: the compile failed, but not through the injected codegen abort"
                    tail -n 5 "$WORKDIR/d$((lvl + 2)).log" | sed 's/^/        /'
                else
                    fam_after="$(drv_family)"
                    n_after="$(printf '%s' "$fam_after" | grep -c . || true)"
                    if [ "$fam_before" = "$fam_after" ]; then
                        note "$tag: previous family survived a codegen-stage failure byte-identical (level $lvl)"
                    else
                        record_driver_defect "$tag: abort level $lvl left $n_after of $n_before family member(s) -- a codegen-stage failure consumed the previous generation (main.vyx:6371 deletes it before codegen)"
                        survivors="$(cd "$DPA" && ls drv.o drv.o.cgu.* 2>/dev/null | tr '\n' ' ')"
                        note "left: ${survivors:-(nothing)}"
                    fi
                    # restore a complete generation for the next level
                    rm -f "$DPA"/drv.o "$DPA"/drv.o.cgu.*
                    "$BOOT" --src=project "$WIN_DPROJ" --emit=obj -o "$win_drv_out" \
                            >"$WORKDIR/d$((lvl + 2))r.log" 2>&1 || true
                fi
                lvl=$((lvl + 1))
            done
        else
            note "skipped D3/D4: --src=project baseline compile failed"
            tail -n 5 "$WORKDIR/d3.log" | sed 's/^/      /'
        fi
    fi
else
    note "skipped: baseline --emit=obj compile failed"
    tail -n 5 "$WORKDIR/d1.log" | sed 's/^/      /'
fi

# ── E: a leftover manifest is not a compile result ───────────────────
# The driver used to decide "the CGU split already wrote my output" by testing
# `fileExists(path + ".cgu.list")`.  Any manifest answers that test, including
# one left behind by an interrupted earlier generation, and the answer "yes"
# makes the driver emit nothing and report success.
#
# That hazard was *masked*, not absent: `driver_delete_object_family` ran before
# codegen and removed exactly that file.  Measured -- the pre-fix compiler passes
# this section too.  Removing the pre-delete is what the publish transaction
# requires, and it is what would expose the hazard, so the property has to be
# carried by something other than an accident of ordering.  It is now carried by
# a typed answer from the backend (`llvm_mir_note_published_object`) plus the
# commit's own cleanup of the stale family.  This section is a regression guard
# for that, not a reproduction of an observable defect.
#
# The project must not split, or the child would rewrite the manifest itself and
# the planted one would never be consulted.
echo
echo "-- E: a stale manifest must not suppress the compile --"
DEP="$WORKDIR/e"
rm -rf "$DEP"; mkdir -p "$DEP/src"
cat >"$DEP/Vyx.toml" <<'TOML'
[package]
name = "cgu_single"
version = "0.1.0"
entry = "src/main.vyx"

[build]
cache_dir = ".cache"
output_dir = "target"
threads = 2

[target.cgu_single]
type = "executable"
entry = "src/main.vyx"
sources = []
TOML
cat >"$DEP/src/main.vyx" <<'VYX'
fn main() -> i32 {
    print("single");
    return 0;
}
VYX
E_OBJ_REL=".cache/crate_cgu_single.obj"
if run_build "$DEP" "$WORKDIR/e1.log" VYX_PHASE_SUMMARY=1; then
    if grep -q 'freeze-summary units=1 ' "$WORKDIR/e1.log"; then
        note "E setup: the project compiles as a single codegen unit"
    else
        note "note: could not confirm units=1; the planted manifest may be rewritten"
    fi
    e_before="$(sha256sum "$DEP/$E_OBJ_REL" | cut -d' ' -f1)"
    printf 'stale\n' >"$DEP/$E_OBJ_REL.tmp.cgu.list"
    rm -f "$DEP/.cache/.vyx_cache"
    if run_build "$DEP" "$WORKDIR/e2.log" VYX_PHASE_SUMMARY=1; then
        if grep -q 'cache src/' "$WORKDIR/e2.log"; then
            bad "E: the second build was a cache hit, so the planted manifest was never consulted"
        else
            e_after="$(sha256sum "$DEP/$E_OBJ_REL" | cut -d' ' -f1)"
            if [ "$e_before" = "$e_after" ]; then
                note "E: build succeeded and republished the object despite a stale manifest"
            else
                bad "E: the republished object differs from the first build's"
            fi
            if [ -e "$DEP/$E_OBJ_REL.tmp.cgu.list" ]; then
                bad "E: the stale manifest was left in the staging area"
            else
                note "E: the stale manifest is gone (the new generation replaced it)"
            fi
        fi
    else
        bad "E: a stale <obj>.tmp.cgu.list made the build fail"
        tail -n 8 "$WORKDIR/e2.log" | sed 's/^/        /'
    fi
else
    note "skipped E: the single-unit project did not build"
    tail -n 5 "$WORKDIR/e1.log" | sed 's/^/      /'
fi

# ── F: the commit must refuse an incomplete staged generation ────────
# §10.3 item 2 says a result set with a missing CGU is not a result, and the
# order in which that is enforced is what item 6 turns on.  The commit used to
# enforce it *after* it had destroyed the previous generation: it dropped
# `<obj>.cgu.list` and the stale siblings first, then renamed the new members one
# at a time.  A member that had disappeared in between therefore failed the
# publish with the previous generation already gone and the new one half in
# place -- §10.3 item 6's forbidden outcome (失败保留上一代可用输出).
#
# `VYX_CGU_PUBLISH_DROP_UNIT=N` removes unit N from the staged generation between
# codegen and the commit, which is what a scanner, another process or a full disk
# can do to a family that was written successfully.  Unit 2 is the interesting
# one -- unit 1 *is* the primary, and a missing primary is caught by an older,
# weaker check -- so the case that matters is a hole in the middle of the family.
echo
echo "-- F: the commit refuses an incomplete staged generation --"
DF="$WORKDIR/fproj"
rm -rf "$DF"; mkdir -p "$DF"
cp "$FIX/Vyx.toml" "$DF/"
cp -R "$FIX/src" "$DF/"
WIN_DF="$(cygpath -m "$DF")"
f_out="$DF/drv.o"; win_f_out="$(cygpath -m "$f_out")"
f_family() {
    ( cd "$DF" && for f in $(ls drv.o drv.o.cgu.* 2>/dev/null | sort -V); do
          sha256sum "$f" | sed 's|  .*/|  |'
      done )
}
f_debris() {
    ( cd "$DF" && ls drv.o.stage.obj* 2>/dev/null ) || true
}
if VYX_PHASE_SUMMARY=1 "$BOOT" --src=project "$WIN_DF" --emit=obj -o "$win_f_out" \
       >"$WORKDIR/f0.log" 2>&1 && [ -s "$f_out" ]; then
    f_units="$(grep -o '\[llvm-cgu\] result units=[0-9]*' "$WORKDIR/f0.log" \
               | tail -n 1 | grep -o '[0-9]*$')"
    if [ -z "${f_units:-}" ] || [ "$f_units" -lt 2 ]; then
        bad "F: could not confirm a multi-unit driver compile (units='${f_units:-}')"
        grep -o '\[llvm-cgu\] result[^\\]*' "$WORKDIR/f0.log" | sed 's/^/        /'
    else
        note "F setup: the driver published a ${f_units}-unit family"
        if grep -q "present=${f_units} " "$WORKDIR/f0.log" \
           && grep -q ' ok=1 ' "$WORKDIR/f0.log"; then
            note "F setup: the result line reports a whole generation"
        else
            bad "F setup: the baseline result line does not report a whole generation"
            grep -o '\[llvm-cgu\] result[^\\]*' "$WORKDIR/f0.log" | sed 's/^/        /'
        fi
        f_before="$(f_family)"
        if VYX_CGU_PUBLISH_DROP_UNIT=2 "$BOOT" --src=project "$WIN_DF" \
               --emit=obj -o "$win_f_out" >"$WORKDIR/f1.log" 2>&1; then
            bad "F: a compile whose staged unit 2 had vanished reported success"
        elif ! grep -q 'codegen unit family is incomplete' "$WORKDIR/f1.log"; then
            bad "F: the compile failed, but not through the generation check"
            tail -n 5 "$WORKDIR/f1.log" | sed 's/^/        /'
        else
            note "F: the incomplete staged generation was refused"
            grep -o 'codegen unit family is incomplete[^\\]*' "$WORKDIR/f1.log" \
                | head -n 1 | sed 's/^/        /'
            if [ "$f_before" = "$(f_family)" ]; then
                note "F: the previous generation survived byte-identical"
            else
                bad "F: an incomplete generation destroyed the previous one"
                ( cd "$DF" && ls drv.o drv.o.cgu.* 2>/dev/null | sed 's/^/        left: /' ) || true
            fi
            if [ -n "$(f_debris)" ]; then
                bad "F: the discarded staged generation left debris: $(printf '%s ' $(f_debris))"
            else
                note "F: no staged debris left behind"
            fi
        fi
    fi
else
    note "skipped F: baseline --src=project compile failed"
    tail -n 5 "$WORKDIR/f0.log" | sed 's/^/      /'
fi

# ── G: an incomplete generation is never published ───────────────────
# The emitter builds the manifest from the plan, and between "the emit job
# returned 0" and "the object has bytes in it" there is a gap that a return code
# cannot see: a short write, an unwritable destination, a file a scanner took
# away.  `VYX_CGU_EMPTY_UNIT=N` produces exactly that -- unit N's object exists,
# is named by the plan, and is empty -- and what has to happen is that the
# compile fails naming the unit, without a manifest ever being written for the
# generation.  The user-visible outcome (non-zero exit, previous generation
# untouched) is the same as for any other failure; what this section pins is the
# mechanism, because a manifest published for an incomplete generation is read as
# a result by whoever consumes it next.
echo
echo "-- G: an incomplete generation is never published --"
DG="$WORKDIR/gproj"
rm -rf "$DG"; mkdir -p "$DG"
cp "$FIX/Vyx.toml" "$DG/"
cp -R "$FIX/src" "$DG/"
WIN_DG="$(cygpath -m "$DG")"
g_out="$DG/drv.o"; win_g_out="$(cygpath -m "$g_out")"
g_family() {
    ( cd "$DG" && for f in $(ls drv.o drv.o.cgu.* 2>/dev/null | sort -V); do
          sha256sum "$f" | sed 's|  .*/|  |'
      done )
}
g_debris() {
    ( cd "$DG" && ls drv.o.stage.obj* 2>/dev/null ) || true
}
if VYX_PHASE_SUMMARY=1 "$BOOT" --src=project "$WIN_DG" --emit=obj -o "$win_g_out" \
       >"$WORKDIR/g0.log" 2>&1 && [ -s "$g_out" ]; then
    g_before="$(g_family)"
    if VYX_CGU_EMPTY_UNIT=1 VYX_PHASE_SUMMARY=1 "$BOOT" --src=project "$WIN_DG" \
           --emit=obj -o "$win_g_out" >"$WORKDIR/g1.log" 2>&1; then
        bad "G: a compile whose unit 1 object is empty reported success"
    elif ! grep -q 'produced an empty object' "$WORKDIR/g1.log"; then
        bad "G: the compile failed, but not through the gather"
        tail -n 5 "$WORKDIR/g1.log" | sed 's/^/        /'
    else
        note "G: the empty unit was caught by the gather, naming the unit"
        grep -o 'codegen unit family is incomplete[^\\]*' "$WORKDIR/g1.log" \
            | head -n 1 | sed 's/^/        /'
        if grep -q ' ok=0' "$WORKDIR/g1.log"; then
            note "G: the result line reports an incomplete generation (ok=0)"
        else
            bad "G: the result line does not report the incomplete generation"
            grep -o '\[llvm-cgu\] result[^\\]*' "$WORKDIR/g1.log" | sed 's/^/        /'
        fi
        if [ "$g_before" = "$(g_family)" ]; then
            note "G: the previous generation survived byte-identical"
        else
            bad "G: an incomplete generation changed the published family"
        fi
        if [ -n "$(g_debris)" ]; then
            bad "G: the discarded staged generation left debris: $(printf '%s ' $(g_debris))"
        else
            note "G: no staged debris left behind"
        fi
    fi
else
    note "skipped G: baseline --src=project compile failed"
    tail -n 5 "$WORKDIR/g0.log" | sed 's/^/      /'
fi

echo
if [ "$fail" -eq 0 ]; then
    echo "artifacts: OK"
else
    echo "artifacts: FAIL"
fi
exit "$fail"
