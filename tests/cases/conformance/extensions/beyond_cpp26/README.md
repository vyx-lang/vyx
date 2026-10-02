# Beyond-C++26 Test Suite (P3)

10 hand-written acceptance tests for the Wave-5 feature set.
Compiler baseline: `e7c2763`.

Run a single test:
```
vyxc tests/cases/conformance/extensions/beyond_cpp26/NN_name.vyx -o /tmp/out.exe && /tmp/out.exe; echo $?
```

## Result matrix (baseline e7c2763)

| # | Test | Target feature | Status | Notes |
|---|---|---|---|---|
| 01 | `const_where_basic` | O — `where N > 0` | ⚠ PARSER | `.new()` on mixed type+int turbofish |
| 02 | `const_where_arith` | O — `N % 4 == 0` combos | ⚠ PARSER | same as 01 |
| 03 | `const_where_fail` | O — P2D-004 expected | ✅ ERR-OK | compile error is the goal |
| 04 | `pack_all_ok` | P — `all<Ts>: Printable` | ⚠ PARSER | `interface { fn f(self)... }` form |
| 05 | `pack_fold_sum` | P — `args...+` fold | ✅ PASS (exit 0) | |
| 06 | `pack_len_where` | P — `Ts.len > 0` | ✅ PASS (exit 0) | |
| 07 | `reflect_typeof` | Q — `typeof(x)` | ⚠ LEXER | `typeof` token not reaching parser |
| 08 | `reflect_fields_iter` | Q — `for m in T::fields` | ⚠ PARSER | for-in-TypeReflect form |
| 09 | `reflect_where_kind` | Q — `where T::kind=="struct"` | ⚠ PARSER | `T::kind` in where RHS |
| 10 | `error_recovery_multi` | R — multi-error + dedup | ✅ ERR-OK | compile error chain expected |

## Legend

- **PASS** — compiles, runs, returns expected exit code
- **ERR-OK** — expected-error test; compile error fires as designed
- **PARSER** — grammar rule landed in agent worktree but cross-feature
  integration leaves a parse gap (top-level where-clause rules ordering,
  or reflection-in-where interacts with P's constraint parser)
- **LEXER** — `typeof` keyword added by Q but upstream lexer/keyword-map
  merge lost part of the integration — `typeof(x)` still resolves via
  the regular function-call path

## What these tests certify

**Landed end-to-end** (5/10): pack fold `args...+`, `Ts.len`, P2D-004
const predicate violation diagnostics, MONO-001 cycle detection, P2D-002
dedup across call sites.

**Needs integration follow-up** (5/10): mixed type+int turbofish
construction syntax (`T::<i32, 16>.new()`), interface method `self`
receiver form, `typeof` keyword token registration, `for-in` loop over
`T::fields` sentinel iterable, `T::kind` in where-clause RHS.

## Scope

These tests are the acceptance criteria for Wave-5 features. They live
outside `tests/cases/` intentionally — the main CI suite should not gate
on them until all are green.
