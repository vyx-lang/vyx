#!/usr/bin/env bash
# Three-generation self-host fixpoint + same-input IR identity, Linux/bash.
#
# Adapted from scripts/test_project_selfhost_fixpoint.ps1 for environments
# without PowerShell. Stage 0 defaults to the checked-in
# `bootstrap_compiler/seed/vyxc.exe`; pass an explicit path when the host cannot
# execute that seed. It builds this bootstrap tree three times in isolated work copies:
#
#   S0 = bootstrap_compiler/seed/vyxc.exe (checked-in seed)
#   S1 = S0 `build` of this tree            -> S1/bootstrap_compiler/out/boot
#   S2 = S1 boot `build` of a clean copy    -> S2/.../out/boot
#   S3 = S2 boot `build` of a clean copy    -> S3/.../out/boot
#
# Snapshots copy the repo slice (bootstrap_compiler + vyx_codegen + runtime),
# because Vyx.toml references ../vyx_codegen and ../runtime.
#
# Stability gate: S2 and S3 out/ artifacts must be byte-identical (sha256).
# IR identity: a small stable program is compiled with --emit=ir by S1/S2/S3;
# IR is normalized (volatile header lines stripped) and diffed.
#
# Usage:
#   scripts/selfhost_3gen_ir.sh [seed_vyxc] [ir_program.vyx]
# Env:
#   VYX_SEED     override the Stage 0 compiler (default: repository seed)
#   LLVM_ROOT    LLVM 22 root (default: repository `clang/` SDK)
#   JOBS         parallel build jobs (default: 10)
#   WORK_ROOT    scratch dir (default: mktemp under $TMPDIR)
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BOOTSTRAP_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$BOOTSTRAP_ROOT/.." && pwd)"

JOBS="${JOBS:-10}"
SEED="${1:-${VYX_SEED:-$BOOTSTRAP_ROOT/seed/vyxc.exe}}"
IR_PROG="${2:-$REPO_ROOT/samples/bootstrap/hello.vyx}"

log()  { printf '[3gen] %s\n' "$*"; }
die()  { printf '[3gen][FATAL] %s\n' "$*" >&2; exit 1; }

# ── Stage 0 resolution ──────────────────────────────────────────────────────
if ! command -v "$SEED" >/dev/null 2>&1 && [ ! -x "$SEED" ]; then
    die "Stage 0 compiler not found: '$SEED' (pass an executable with VYX_SEED or argv[1])"
fi
S0="$(command -v "$SEED" 2>/dev/null || printf '%s' "$SEED")"
log "S0 (seed) = $S0"

# ── LLVM_ROOT ───────────────────────────────────────────────────────────────
if [ -z "${LLVM_ROOT:-}" ]; then
    if [ -d "$REPO_ROOT/clang" ]; then
        export LLVM_ROOT="$REPO_ROOT/clang"
    else
        die "LLVM_ROOT is unset and repository SDK is missing: $REPO_ROOT/clang (set LLVM_ROOT explicitly)"
    fi
fi
log "LLVM_ROOT = $LLVM_ROOT"
[ -d "$LLVM_ROOT/bin" ] && export PATH="$LLVM_ROOT/bin:$PATH"
[ -d "$LLVM_ROOT/toolchain/bin" ] && export PATH="$LLVM_ROOT/toolchain/bin:$PATH"
export LD_LIBRARY_PATH="${LLVM_ROOT}/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

WORK_ROOT="${WORK_ROOT:-$(mktemp -d "${TMPDIR:-/tmp}/vyx_3gen.XXXXXX")}"
mkdir -p "$WORK_ROOT"
LOG_DIR="$WORK_ROOT/logs"; mkdir -p "$LOG_DIR"
log "work root = $WORK_ROOT"

# Clean copy of the repo slice the bootstrap build needs.
snapshot_tree() {
    local dest="$1"
    mkdir -p "$dest"
    if command -v rsync >/dev/null 2>&1; then
        rsync -a \
            --exclude='.git' --exclude='.cache' --exclude='out' --exclude='target' \
            --exclude='dist/*.tar.xz' --exclude='dist/*.tar.gz' --exclude='dist/*.zip' \
            "$REPO_ROOT/" "$dest/"
    else
        ( cd "$REPO_ROOT" && tar --exclude='./.git' --exclude='./.cache' \
            --exclude='./out' --exclude='./target' \
            --exclude='./dist/*.tar.xz' --exclude='./dist/*.tar.gz' \
            --exclude='./dist/*.zip' -cf - . ) | ( cd "$dest" && tar -xf - )
    fi
}

stage_root() {
    printf '%s/bootstrap_compiler' "$1"
}

stage_compiler() {
    local boot_dir="$1"
    for cand in out/boot out/vyxc out/boot.exe out/vyxc.exe; do
        if [ -x "$boot_dir/$cand" ]; then printf '%s' "$boot_dir/$cand"; return 0; fi
    done
    return 1
}

build_stage() {
    local name="$1" compiler="$2" stage_dir="$3"
    local boot_dir; boot_dir="$(stage_root "$stage_dir")"
    local out_log="$LOG_DIR/$name.stdout.log" err_log="$LOG_DIR/$name.stderr.log"
    local cdir; cdir="$(cd "$(dirname "$compiler")" && pwd)"
    log "[$name] cwd=$boot_dir"
    log "[$name] compiler=$compiler"
    log "[$name] command=$compiler build -j$JOBS"
    ( cd "$boot_dir" && LD_LIBRARY_PATH="$cdir:${LD_LIBRARY_PATH:-}" \
        "$compiler" build -j"$JOBS" ) >"$out_log" 2>"$err_log"
    local rc=$?
    if [ "$rc" -ne 0 ]; then
        log "[$name] FAILED exit=$rc"
        log "[$name] --- stderr tail ---"; tail -n 40 "$err_log" >&2
        log "[$name] --- stdout tail ---"; tail -n 40 "$out_log" >&2
        die "$name build failed (exit=$rc). logs: $out_log / $err_log"
    fi
    if ! stage_compiler "$boot_dir" >/dev/null; then
        log "[$name] --- stdout tail ---"; tail -n 20 "$out_log" >&2
        die "$name produced no out/boot|out/vyxc in $boot_dir"
    fi
    log "[$name] OK -> $(stage_compiler "$boot_dir")"
}

out_manifest() {
    local boot_dir; boot_dir="$(stage_root "$1")"
    ( cd "$boot_dir/out" && find . -type f ! -name '*.vyx-provenance' -print0 \
        | sort -z | xargs -0 sha256sum ) 2>/dev/null
}

normalize_ir() {
    sed -E \
      -e '/^[[:space:]]*;[[:space:]]*ModuleID\b/d' \
      -e '/^[[:space:]]*source_filename\b/d' \
      -e '/^[[:space:]]*target[[:space:]]+datalayout\b/d' \
      -e '/^[[:space:]]*target[[:space:]]+triple\b/d' \
      "$1" | sed -E "s#${REPO_ROOT//\#/\\\#}#<REPO>#g; s#${WORK_ROOT//\#/\\\#}#<WORK>#g"
}

emit_ir() {
    local name="$1" compiler="$2" src="$3" out_ll="$4"
    local cdir; cdir="$(cd "$(dirname "$compiler")" && pwd)"
    log "[$name IR] $compiler --emit=ir -O2 -o $out_ll --src=file $src"
    ( cd "$REPO_ROOT" && LD_LIBRARY_PATH="$cdir:${LD_LIBRARY_PATH:-}" \
        "$compiler" --emit=ir -O2 -o "$out_ll" --src=file "$src" ) \
        >"$LOG_DIR/$name.ir.stdout.log" 2>"$LOG_DIR/$name.ir.stderr.log"
    local rc=$?
    [ "$rc" -eq 0 ] || { tail -n 30 "$LOG_DIR/$name.ir.stderr.log" >&2; die "$name IR emit failed (exit=$rc)"; }
    [ -f "$out_ll" ] || die "$name produced no IR at $out_ll"
}

S1_DIR="$WORK_ROOT/S1"; S2_DIR="$WORK_ROOT/S2"; S3_DIR="$WORK_ROOT/S3"
log "snapshotting trees..."; snapshot_tree "$S1_DIR"; snapshot_tree "$S2_DIR"; snapshot_tree "$S3_DIR"

build_stage "S1" "$S0" "$S1_DIR"
S1C="$(stage_compiler "$(stage_root "$S1_DIR")")"
build_stage "S2" "$S1C" "$S2_DIR"
S2C="$(stage_compiler "$(stage_root "$S2_DIR")")"
build_stage "S3" "$S2C" "$S3_DIR"

out_manifest "$S2_DIR" >"$WORK_ROOT/S2.out.sha256"
out_manifest "$S3_DIR" >"$WORK_ROOT/S3.out.sha256"
log "=== S2 vs S3 out/ manifest diff ==="
if diff -u "$WORK_ROOT/S2.out.sha256" "$WORK_ROOT/S3.out.sha256"; then
    log "STABILITY: PASS (S2 and S3 out/ are byte-identical)"
    STAB=PASS
else
    log "STABILITY: FAIL (see differing files above)"
    STAB=FAIL
fi
for c in boot vyxc; do
    p2="$(stage_root "$S2_DIR")/out/$c"
    p3="$(stage_root "$S3_DIR")/out/$c"
    [ -f "$p2" ] && printf '[3gen] S2 %-5s %s\n' "$c" "$(sha256sum "$p2" | cut -d' ' -f1)"
    [ -f "$p3" ] && printf '[3gen] S3 %-5s %s\n' "$c" "$(sha256sum "$p3" | cut -d' ' -f1)"
done

[ -f "$IR_PROG" ] || die "IR program not found: $IR_PROG"
emit_ir "S1" "$S1C" "$IR_PROG" "$WORK_ROOT/ir.S1.ll"
emit_ir "S2" "$S2C" "$IR_PROG" "$WORK_ROOT/ir.S2.ll"
emit_ir "S3" "$(stage_compiler "$(stage_root "$S3_DIR")")" "$IR_PROG" "$WORK_ROOT/ir.S3.ll"
for g in S1 S2 S3; do normalize_ir "$WORK_ROOT/ir.$g.ll" >"$WORK_ROOT/ir.$g.norm.ll"; done
log "=== IR identity (normalized: headers stripped) on $IR_PROG ==="
IR=PASS
if ! diff -u "$WORK_ROOT/ir.S2.norm.ll" "$WORK_ROOT/ir.S3.norm.ll"; then IR="FAIL(S2!=S3)"; fi
if ! diff -q "$WORK_ROOT/ir.S1.norm.ll" "$WORK_ROOT/ir.S2.norm.ll" >/dev/null; then
    [ "$IR" = PASS ] && IR="S2==S3 but S1 differs"
fi
for g in S1 S2 S3; do
    printf '[3gen] IR %s normalized sha256 = %s\n' "$g" "$(sha256sum "$WORK_ROOT/ir.$g.norm.ll" | cut -d' ' -f1)"
done

log "================ SUMMARY ================"
log "work root         : $WORK_ROOT"
log "S2/S3 stability   : $STAB"
log "IR identity       : $IR"
[ "$STAB" = PASS ] && [ "$IR" = PASS ] || exit 1
log "RESULT: PASS"
