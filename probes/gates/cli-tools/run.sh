#!/usr/bin/env bash
# CLI subcommands: test / bench / fmt / doc
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
BOOT="$ROOT/bootstrap_compiler/out/boot"
OUT="$ROOT/bootstrap_compiler/out"
FIXTURE="$(cd "$(dirname "$0")/fixture" && pwd)"
TMP="${TMPDIR:-/tmp}/vyx-cli-tools"
mkdir -p "$TMP"

export LLVM_ROOT="${LLVM_ROOT:-/usr/lib/llvm-22}"
export PATH="$LLVM_ROOT/bin:$PATH"
export LD_LIBRARY_PATH="$OUT${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
if [ -f /etc/profile.d/vyx-llvm22.sh ]; then
  # shellcheck disable=SC1091
  source /etc/profile.d/vyx-llvm22.sh
fi

if [ ! -x "$BOOT" ]; then
  echo "cli-tools FAIL: missing $BOOT"
  exit 1
fi

fail() {
  echo "FAIL: $1"
  if [ -n "${2:-}" ] && [ -f "$2" ]; then
    tail -n 40 "$2" || true
  fi
  exit 1
}

HELP_LOG="$TMP/help.log"
"$BOOT" help >"$HELP_LOG" 2>&1 || true
grep -q ' test ' "$HELP_LOG" || fail "help missing test usage" "$HELP_LOG"
grep -q ' bench ' "$HELP_LOG" || fail "help missing bench usage" "$HELP_LOG"
grep -q ' fmt ' "$HELP_LOG" || fail "help missing fmt usage" "$HELP_LOG"
grep -q ' doc ' "$HELP_LOG" || fail "help missing doc usage" "$HELP_LOG"
echo "OK: help lists test/bench/fmt/doc"

cd "$FIXTURE"

TEST_LOG="$TMP/test.log"
"$BOOT" test >"$TEST_LOG" 2>&1 || fail "test failed" "$TEST_LOG"
grep -q 'test_add' "$TEST_LOG" || fail "test output missing test_add" "$TEST_LOG"
grep -q 'test result: ok' "$TEST_LOG" || fail "test output missing ok" "$TEST_LOG"
echo "OK: test"

FILTER_LOG="$TMP/filter.log"
"$BOOT" test --filter no_such_name >"$FILTER_LOG" 2>&1 || fail "filtered test should exit 0" "$FILTER_LOG"
grep -qi 'no tests' "$FILTER_LOG" || fail "filter should report no tests" "$FILTER_LOG"
echo "OK: test --filter"

BENCH_LOG="$TMP/bench.log"
"$BOOT" bench >"$BENCH_LOG" 2>&1 || fail "bench failed" "$BENCH_LOG"
grep -q 'bench_spin' "$BENCH_LOG" || fail "bench output missing name" "$BENCH_LOG"
grep -q ' ms' "$BENCH_LOG" || fail "bench output missing ms" "$BENCH_LOG"
echo "OK: bench"

UGLY="$FIXTURE/src/ugly.vyx"
cp "$FIXTURE/src/ugly.raw.vyx" "$UGLY"
CHECK_LOG="$TMP/fmt-check.log"
set +e
"$BOOT" fmt --check "$UGLY" >"$CHECK_LOG" 2>&1
CHECK_RC=$?
set -e
if [ "$CHECK_RC" -eq 0 ]; then
  fail "fmt --check should fail on ugly source" "$CHECK_LOG"
fi
echo "OK: fmt --check dirty"

FMT_LOG="$TMP/fmt.log"
"$BOOT" fmt "$UGLY" >"$FMT_LOG" 2>&1 || fail "fmt rewrite failed" "$FMT_LOG"
"$BOOT" fmt --check "$UGLY" >"$CHECK_LOG" 2>&1 || fail "fmt --check should pass after rewrite" "$CHECK_LOG"
grep -q 'fn main() -> i32' "$UGLY" || fail "fmt rewrite missing signature" "$UGLY"
grep -q 'return 0;' "$UGLY" || fail "fmt rewrite missing return" "$UGLY"
echo "OK: fmt"

DOC_LOG="$TMP/doc.log"
"$BOOT" doc "$FIXTURE/src/lib.vyx" >"$DOC_LOG" 2>&1 || fail "doc failed" "$DOC_LOG"
grep -q 'inc' "$DOC_LOG" || fail "doc missing inc" "$DOC_LOG"
grep -q 'Adds one' "$DOC_LOG" || fail "doc missing Adds one" "$DOC_LOG"
echo "OK: doc"

echo "cli-tools E2E: OK"
