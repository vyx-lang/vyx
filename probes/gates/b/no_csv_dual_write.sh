#!/usr/bin/env bash
# WP-B: set_function_generic_env interns CSV into GenericEnvId and clears
# the function-record string slots. Getters must prefer env id.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
MODEL="$ROOT/bootstrap_compiler/src/hir/hir_model.vyx"
fail=0

need() {
  if ! grep -q "$2" "$1"; then
    echo "FAIL: missing $3"
    fail=1
  fi
}

need "$MODEL" 'fn intern_type_list_from_csv' "CSV intern helper"
need "$MODEL" 'self.set_function_generic_env_id(function_id, env_id);' "env id publish"
need "$MODEL" 'self.clear_function_generic_env_strings(function_id);' "CSV clear after intern"
need "$MODEL" 'if (env > 0) { return self.type_list_texts(self.genv_param_list(env)); }' "params prefer env id"

# The string-write path must not remain as the only body of set_function_generic_env.
if grep -A8 'public fn set_function_generic_env(self, function_id: i32, params: string, args: string, owner_type: string)' "$MODEL" \
   | grep -q 'hir_write_string(rec, 48, 56, params);'; then
  echo "FAIL: set_function_generic_env still writes CSV as the source of truth"
  fail=1
fi

if [ "$fail" != 0 ]; then
  echo "b FAIL"
  exit 1
fi
echo "OK: no_csv_dual_write"
exit 0
