#!/usr/bin/env bash
# WP-P0b: malloc / local *_dup_bytes / node byte-offset accessors must
# drop below the release baseline in the four frontend modules.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$ROOT"

fail() { echo "p0b FAIL: $*"; exit 1; }

count_malloc() {
  git grep -c 'malloc' -- "$1" | awk -F: '{s+=$NF} END {print s+0}'
}

# p0a-era ceiling after TokenSlot / FbSlot / CanonSlot / ast_*_pack.
# Remaining malloc calls are arena growth (buffer / state / pack), not
# per-node byte accessors. Do not raise these without a WP-P0b note.
AST_BASE=4
LEX_BASE=5
FB_BASE=5
CANON_BASE=3

AST=$(count_malloc bootstrap_compiler/src/core/ast.vyx)
LEX=$(count_malloc bootstrap_compiler/src/core/lexer.vyx)
FB=$(count_malloc bootstrap_compiler/src/core/fact_base.vyx)
CANON=$(count_malloc bootstrap_compiler/src/core/canon.vyx)

echo "p0b malloc counts: ast=$AST (base $AST_BASE) lexer=$LEX (base $LEX_BASE) fact_base=$FB (base $FB_BASE) canon=$CANON (base $CANON_BASE)"

if [ "$AST" -gt "$AST_BASE" ]; then fail "ast.vyx malloc $AST above $AST_BASE"; fi
if [ "$LEX" -gt "$LEX_BASE" ]; then fail "lexer.vyx malloc $LEX above $LEX_BASE"; fi
if [ "$FB" -gt "$FB_BASE" ]; then fail "fact_base.vyx malloc $FB above $FB_BASE"; fi
if [ "$CANON" -gt "$CANON_BASE" ]; then fail "canon.vyx malloc $CANON above $CANON_BASE"; fi

if git grep -n 'fn canon_dup_bytes\|fn fb_dup_bytes\|fn lexer_copy_token_bytes' -- \
     bootstrap_compiler/src/core/canon.vyx \
     bootstrap_compiler/src/core/fact_base.vyx \
     bootstrap_compiler/src/core/lexer.vyx; then
  fail "local *_dup_bytes helpers still present"
fi

if git grep -n 'fn write_str_field\|fn read_str_field\|fn slot_read_i32\|fn slot_write_i32\|fn slot_read_i64\|fn slot_write_i64\|fn slot_read_rawptr\|fn slot_write_rawptr' -- \
     bootstrap_compiler/src/core/ast.vyx \
     bootstrap_compiler/src/core/lexer.vyx; then
  fail "byte-offset field accessors still present"
fi

if ! git grep -q 'fn fb_slot_pack' -- bootstrap_compiler/src/core/fact_base.vyx; then
  fail "fact_base missing fb_slot_pack"
fi
if ! git grep -q 'fn ast_tref_pack' -- bootstrap_compiler/src/core/ast.vyx; then
  fail "ast missing ast_tref_pack"
fi
if ! git grep -q 'fn ast_expr_pack' -- bootstrap_compiler/src/core/ast.vyx; then
  fail "ast missing ast_expr_pack"
fi
if ! git grep -q 'fn ast_stmt_pack' -- bootstrap_compiler/src/core/ast.vyx; then
  fail "ast missing ast_stmt_pack"
fi
if ! git grep -q 'fn ast_decl_pack' -- bootstrap_compiler/src/core/ast.vyx; then
  fail "ast missing ast_decl_pack"
fi
if ! git grep -q 'fn ast_unit_pack' -- bootstrap_compiler/src/core/ast.vyx; then
  fail "ast missing ast_unit_pack"
fi

echo "p0b OK: malloc counts dropped; no local dup_bytes; no slot/str byte accessors"
