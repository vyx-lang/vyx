#!/usr/bin/env bash
# WP-A: no live emit_unit references outside deleted host-era codegen.
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$repo_root"

if git grep -n 'emit_unit' -- bootstrap_compiler/src | grep -v 'codegen.vyx'; then
    echo "STILL REFERENCED"
    exit 1
fi
echo "OK: no live refs"
