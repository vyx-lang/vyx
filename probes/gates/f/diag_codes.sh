#!/usr/bin/env bash
# WP-F: codes come from the produce site or a carried Exxxx — not a needle table.
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$repo_root"

if grep -n 'diag_classify_sema_message' -A 8 bootstrap_compiler/src/core/diag.vyx \
    | grep -E 'starts_with|undefined symbol|type mismatch'; then
    echo "FAIL: needle/prefix classify table came back"
    exit 1
fi

BOOT="${repo_root}/bootstrap_compiler/out/boot"
if [[ ! -x "$BOOT" ]]; then
    BOOT="/home/ubuntu/wp-split/bootstrap_compiler/out/boot"
fi
if [[ ! -x "$BOOT" ]]; then
    echo "FAIL: no boot to compile error sources"
    exit 1
fi
export LLVM_ROOT="${LLVM_ROOT:-/usr/lib/llvm-22}"
export PATH="${LLVM_ROOT}/bin:${PATH}"
export LD_LIBRARY_PATH="$(dirname "$BOOT")${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

expect_code() {
    local src="$1" code="$2" label="$3"
    local out
    out="$("$BOOT" --src=file "$src" --emit=obj -o /tmp/f-diag.o 2>&1 || true)"
    if echo "$out" | grep -qE "$code"; then
        echo "OK $label -> $code"
        return 0
    fi
    echo "FAIL $label: expected $code"
    echo "$out" | head -20
    return 1
}

printf 'fn main() -> i32 { return missing_name();\n}\n' > /tmp/f-undef.vyx
printf 'fn main() -> i32 { return "x";\n}\n' > /tmp/f-type.vyx
printf 'fn main( {\n' > /tmp/f-syntax.vyx

fail=0
expect_code /tmp/f-undef.vyx 'E[0-9]{4}' "undefined" || fail=1
expect_code /tmp/f-type.vyx 'E[0-9]{4}' "type" || fail=1
expect_code /tmp/f-syntax.vyx 'E[0-9]{4}' "syntax" || fail=1

if [ "$fail" -eq 0 ]; then
    echo "OK: produce-site or carried Exxxx; no prefix table"
    exit 0
fi
exit 1
