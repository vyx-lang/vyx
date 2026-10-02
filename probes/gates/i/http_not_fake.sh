#!/usr/bin/env bash
# WP-I: boot curl must capture a real body; docs must state C-only FFI + DCI ±2^53.
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$repo_root"
fail=0

if git grep -n 'return "(response)"' -- bootstrap_compiler/std/curl.vyx; then
    echo "FAIL: boot curl still returns the fake (response) literal"
    fail=1
fi
if ! grep -q 'http_perform_capture' bootstrap_compiler/std/curl.vyx; then
    echo "FAIL: boot curl does not call http_perform_capture"
    fail=1
fi
if ! grep -q 'Language-level FFI is C-only' docs/DCI_SPEC.md; then
    echo "FAIL: docs/DCI_SPEC.md missing C-only FFI statement"
    fail=1
fi
if ! grep -q '±2^53' docs/DCI_SPEC.md; then
    echo "FAIL: docs/DCI_SPEC.md missing ±2^53 bound"
    fail=1
fi
if ! grep -q '语言级 FFI 只有 C' docs/DCI_SPEC_ZH.md; then
    echo "FAIL: docs/DCI_SPEC_ZH.md missing C-only FFI statement"
    fail=1
fi
if ! grep -q '±2^53' docs/DCI_SPEC_ZH.md; then
    echo "FAIL: docs/DCI_SPEC_ZH.md missing ±2^53 bound"
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    echo "OK: boot curl is not fake; DCI docs state C-only FFI and ±2^53"
    exit 0
fi
exit 1
