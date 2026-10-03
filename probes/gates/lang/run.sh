#!/usr/bin/env bash
# User-facing language: async fiber interleave + comptime fold + derive deep clone.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
bash "$ROOT/probes/gates/async/run.sh"
bash "$ROOT/probes/gates/comptime/run.sh"
bash "$ROOT/probes/gates/derive/run.sh"
bash "$ROOT/probes/gates/g/docs_honest.sh"
bash "$ROOT/probes/gates/lang/nested_records.sh"
echo "lang OK: async + comptime + derive + nested records"
