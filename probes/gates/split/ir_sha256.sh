#!/usr/bin/env bash
# WP-SPLIT probe: hello --emit=ir sha256 must match the recorded baseline.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
BOOT="${ROOT}/bootstrap_compiler/out/boot"
BASELINE_FILE="$(dirname "$0")/hello.ir.sha256"
export LLVM_ROOT="${LLVM_ROOT:-/usr/lib/llvm-22}"
export PATH="${LLVM_ROOT}/bin:${PATH}"
export LD_LIBRARY_PATH="${ROOT}/bootstrap_compiler/out${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

if [[ ! -x "$BOOT" ]]; then
  echo "FAIL: missing $BOOT"
  exit 1
fi
if [[ ! -f "$BASELINE_FILE" ]]; then
  echo "FAIL: missing baseline $BASELINE_FILE"
  exit 1
fi

# Filename is part of the LLVM module id; keep the same path as the baseline.
printf 'fn main() -> i32 {\n print("hello");\n return 0;\n}\n' > /tmp/hello.vyx
"$BOOT" --src=file /tmp/hello.vyx --emit=ir -o /tmp/hello-split.ll
got="$(sha256sum /tmp/hello-split.ll | awk '{print $1}')"
want="$(tr -d ' \n' < "$BASELINE_FILE")"
echo "got=$got"
echo "want=$want"
if [[ "$got" == "$want" ]]; then
  echo "OK: hello IR sha256 matches SPLIT baseline"
  exit 0
fi
echo "FAIL: hello IR sha256 drifted"
exit 1
