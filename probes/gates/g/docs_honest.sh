#!/usr/bin/env bash
# Honesty probe: docs describe the real fiber / derive / comptime surface,
# and LSP/DAP comments stay accurate.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$repo_root"

fail=0
need() {
    if ! grep -q "$2" "$1"; then
        echo "FAIL: missing honesty statement ($3) in $1"; fail=1
    fi
}

# Async is MIR await-split (poll + yield) on a stackless pump.
need docs/ADVANCED_FEATURES.md 'mir_term_yield' "async MIR await-split"
need docs/ADVANCED_FEATURES.md 'Task.sleep' "async Task.sleep"
need docs/ADVANCED_FEATURES.md 'stackless' "async stackless"
need docs/高级特性_ZH.md 'MIR 拆分' "async MIR await-split (zh)"
need docs/高级特性_ZH.md 'Task.sleep' "async Task.sleep (zh)"

# Derive(Clone) is fieldwise .clone(), not a shallow memcpy of heap handles.
need docs/ADVANCED_FEATURES.md 'fieldwise' "derive fieldwise clone"
need docs/ADVANCED_FEATURES.md 'Raw pointer' "derive rejects rawptr"
need docs/高级特性_ZH.md '逐字段' "derive fieldwise (zh)"
need docs/高级特性_ZH.md 'rawptr' "derive rejects rawptr (zh)"

# Code honesty notes: LSP is Sema-backed; Linux DAP is native ptrace+DWARF.
need bootstrap_compiler/src/core/tooling/lsp_main.vyx 'analyze_unit' "lsp sema"
need bootstrap_compiler/src/core/tooling/dap_main.vyx 'ptrace + DWARF' "dap native"

# Android fiber: StartCoroutine is a no-op; async does not need ucontext.
need docs/ADVANCED_FEATURES.md 'Bionic' "android fiber no-op"
need docs/高级特性_ZH.md 'Bionic' "android fiber no-op (zh)"

if grep -q 'no-op passthrough' docs/ADVANCED_FEATURES.md; then
    echo "FAIL: stale async no-op claim still in ADVANCED_FEATURES.md"; fail=1
fi
if grep -q 'field-shallow' docs/ADVANCED_FEATURES.md; then
    echo "FAIL: stale derive shallow claim still in ADVANCED_FEATURES.md"; fail=1
fi

if [ "$fail" -eq 0 ]; then
    echo "g OK: async/derive/LSP/DAP docs and comments are honest"
else
    exit 1
fi
