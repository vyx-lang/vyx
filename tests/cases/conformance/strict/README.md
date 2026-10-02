# Strict / Adversarial Tests

Anti-overfit suite. These tests were written AFTER P4 with no corresponding
implementation changes — any failure here indicates that P4's
"10/10 green" on `tests/cases/conformance/extensions/beyond_cpp26/` may have been achieved by
over-specializing to the acceptance tests.

## Categories

- **Boundary**: 0 / 1 / many, min / max
- **Cross-feature**: typeof + pack + const + reflect in same TU
- **Negation**: forms C++26 allows (`!=`, `any`, violation-expected) that
  might have been implemented for `==` / `all` only
- **Distinct monomorphs**: two instantiations of the same template
  with different values must NOT share a cached body
- **Negative / expected-error**: compile MUST fail with a specific P2D code

## Test map

| # | Category | Guards against |
|---|---|---|
| 01 | pack boundary | Hardcoded len>=2 assumption |
| 02 | const N positive boundary | Constant comparison against literal 16 |
| 03 | const N=0 violation | Silent P2D-004 skip |
| 04 | typeof in generic body | typeof returning "T" not the concrete |
| 05 | reflect zero fields | Off-by-one on empty class |
| 06 | `!=` reflect constraint | Parser accepting only `==` |
| 07 | reflect constraint violation | P2D-007 not firing |
| 08 | pack fold subtract | Assuming commutative op only |
| 09 | two const params | Second param aliased to first |
| 10 | distinct const monomorphs | Mangled-name collision |
| 11 | typeof vs T::name cross-check | Drift between two reflection paths |
| 12 | fold `&&` short-circuit | Bitwise misinterpretation |
| 13 | multi-error harvest count | Dedup merges too aggressively |
| 14 | type + const mixed | Slot order confusion |
| 15 | pack-all negative | P2D-005 not fired on bad member |

## Success criteria

All "ok" tests must exit 0. All "-NEGATIVE" tests must fail compilation
with the exact error code noted in their comments. Anything else =
overfit-exposure.
