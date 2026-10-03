# Adversarial Tests (True Anti-Overfit)

Written blind — 12 tests written consecutively WITHOUT looking at the
implementation, THEN run once. Failures are real capability gaps, not
test bugs.

## Results at HEAD 3808bd3

**8/12 pass + 4 real gaps exposed.**

| # | Scenario | Outcome |
|---|---|---|
| 01 | `T::name` inside generic class method | ✅ Pass (Mono lowering covers class methods) |
| 02 | `typeof(pack_elem)` | ✅ Pass |
| 03 | `where N > M` two-const-param predicate | ✅ Pass |
| 04 | `var arr: [i64; N]` const in type position | ✅ Pass |
| 05 | 10-element pack fold | ✅ Pass |
| 06 | `where typeof(v) == "i32"` | ❌ parser: where doesn't recognize typeof |
| 07 | inherited T::fields count | ⚠ Pass within sanity bounds (not strict-checked) |
| 08 | `typeof(typeof(x))` | ✅ Pass |
| 09 | `const N: i64` with negative literal | ✅ Pass |
| 10 | `<const N, ...Ts>` mixed ordering | ❌ parser: param order rejected |
| 11 | empty-pack fold `args...+` | ❌ codegen: unhandled expr kind (PackFold=26) |
| 12 | `T::notamember` (invalid member) | ❌ "undefined symbol" (not P2D-008) |

## Real gaps (not overfitting, just missing features)

1. typeof as where-clause predicate — would need grammar extension
2. variadic-before-const OR mixed param ordering — parser assumption
3. empty pack fold CodeGen path — never reached until now
4. reflection unknown-member diagnostic (minor quality issue)

These are NOT blocking for the main "surpass C++26" story but would be
polish items in a future pass.
