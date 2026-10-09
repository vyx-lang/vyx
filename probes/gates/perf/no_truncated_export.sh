#!/usr/bin/env bash
# Per-file module objects must export every public function.  A filename
# special-case that keeps only two build_system entry points drops
# cross-file calls (main -> build_toml_get_platform_value, etc.) at link.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
BS="$ROOT/bootstrap_compiler/src/core/project/build_system.vyx"

if grep -F 'return "build_system_run|build_system_run_script"' "$BS" >/dev/null; then
  echo "FAIL: build_system.vyx still hardcodes a two-name export set"
  exit 1
fi
if ! grep -n 'split_module_units' "$BS" | grep -q .; then
  echo "FAIL: split_module_units missing"
  exit 1
fi
if ! grep -A20 'else if (split_module_units)' "$BS" | grep -q 'build_public_fn_names_for_source_with_methods'; then
  echo "FAIL: split_module_units does not export all public functions"
  exit 1
fi
echo "OK: split_module_units exports all public functions; no two-name hardcode"
