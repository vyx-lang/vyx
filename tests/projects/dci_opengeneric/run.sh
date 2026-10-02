#!/usr/bin/env bash
# dci_opengeneric — 开放泛型演示门。
# 双语言（rust/cpp）各跑一遍 open-session -> resolve -> materialize ->
# publish -> offline replay -> invalidate，输出与 expected_output.txt 逐字节比对。
set -euo pipefail

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
PROJ="$REPO/tests/projects/dci_opengeneric"
cd "$REPO"

# 传给 PE 工具链（python 内部再起 rustc/clang/nm）的路径一律 Windows 形式
w() { cygpath -m "$1"; }

PY="$(command -v python)"
[ -n "$PY" ] || { echo "FAIL python not found"; exit 1; }

rm -rf "$(w "$PROJ/target")"
mkdir -p "$(w "$PROJ/target")"
"$PY" "$(w "$PROJ/run_demo.py")" \
  --work "$(w "$PROJ/target/demo")" \
  --cache "$(w "$PROJ/target/bundles")" \
  > "$(w "$PROJ/target/actual_output.txt")"

diff -u "$(w "$PROJ/expected_output.txt")" "$(w "$PROJ/target/actual_output.txt")" \
  || { echo "FAIL golden mismatch"; exit 1; }

echo "PASS golden (byte-identical across runs)"
