#!/usr/bin/env bash
# R0 observation capture.  Writes a timestamped snapshot under
# bootstrap_compiler/docs/cgu-parallel-r0/ so later stages can compare.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
BOOT="${BOOT:-$ROOT/bootstrap_compiler/out/boot}"
if [ -x "${BOOT}.exe" ] && [ ! -x "$BOOT" ]; then BOOT="${BOOT}.exe"; fi
if [ ! -x "$BOOT" ]; then
    echo "FAIL: missing compiler $BOOT"
    exit 1
fi

# LLVM_ROOT must be a *Windows* path (`E:/Dev/...`), not an MSYS one
# (`/e/Dev/...`): the compiler rewrites `/` to `\` when it composes the clang++
# command line, so an MSYS root comes out as `\e\Dev\...` and every C++ job dies
# with "failed to start compile job".  `cygpath -m` yields the mixed form that
# survives that rewrite.  Same class of rule as `--src=file` needing Windows
# paths -- see the workspace MEMORY note.
if [ -z "${LLVM_ROOT:-}" ]; then
    ROOT_WIN="$(cygpath -m "$ROOT" 2>/dev/null || printf '%s' "$ROOT")"
    LLVM_ROOT="$ROOT_WIN/clang"
fi
export LLVM_ROOT
export PATH="$ROOT/clang/bin:${LLVM_ROOT}/bin:${PATH:-}"
STAMP="$(date +%Y%m%d-%H%M%S)"
OUTDIR="$ROOT/bootstrap_compiler/docs/cgu-parallel-r0"
mkdir -p "$OUTDIR"
SNAP="$OUTDIR/snapshot-$STAMP.txt"

# Gate A is the one number this whole refactor is required not to move, so it is
# computed first and compared, not just recorded.  Two ways it used to come out
# empty for the wrong reason: `--src=file` needs a *Windows* path (an MSYS one
# is not readable, the compile fails, and `2>/dev/null | grep ... || true`
# publishes a blank line), and a blank line reads exactly like "the line was
# suppressed".  Both are silent.  So: pass the Windows form, and fail loudly if
# the line is missing or differs from the recorded expectation.
HELLO="$OUTDIR/hello.vyx"
HELLO_WIN="$(cygpath -m "$HELLO" 2>/dev/null || printf '%s' "$HELLO")"
printf 'fn main() -> i32 {\n    print("hello");\n    return 0;\n}\n' > "$HELLO"
HELLO_LINE="$("$BOOT" --src=file "$HELLO_WIN" --dump-mir2 2>/dev/null | grep '^mir2.unit' || true)"
EXPECT_HELLO="mir2.unit functions=1 blocks=1 locals=1 places=1 values=6 instrs=2 cases=0 types=5"
gate_a=0
if [ -z "$HELLO_LINE" ]; then
    echo "FAIL: gate A produced no mir2.unit line (check that --src=file got a Windows path)"
    gate_a=1
elif [ "$HELLO_LINE" != "$EXPECT_HELLO" ]; then
    echo "FAIL: gate A moved"
    echo "  expected: $EXPECT_HELLO"
    echo "  actual:   $HELLO_LINE"
    gate_a=1
fi

{
    echo "R0 snapshot $STAMP"
    echo "HEAD=$(cd "$ROOT" && git rev-parse HEAD)"
    echo "branch=$(cd "$ROOT" && git rev-parse --abbrev-ref HEAD)"
    echo "boot=$BOOT"
    echo "boot-size=$(wc -c < "$BOOT" | tr -d ' ')"
    if command -v md5sum >/dev/null 2>&1; then
        echo "boot-md5=$(md5sum "$BOOT" | awk '{print $1}')"
    fi
    echo
    echo "=== --version ==="
    "$BOOT" --version || true
    echo
    echo "=== gate A hello --dump-mir2 ==="
    echo "$HELLO_LINE"
} >"$SNAP"

echo "wrote $SNAP"
cat "$SNAP"
if [ "$gate_a" != "0" ]; then exit 1; fi
