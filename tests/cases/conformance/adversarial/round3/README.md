# Adversarial Round 3 (Blind)

12 new scenarios written post-R2, run once blind. 9/12 pass.

## Results

| # | Scenario | Result |
|---|---|---|
| 01 | class method-level generic turbofish | ❌ parser |
| 02 | Option<T> as param + match | ❌ nested generic in sig |
| 03 | Vec<Vec<T>> nested std container | ❌ mono scan |
| 04 | `class : A + B` multi-trait inheritance | ✅ (fixed) |
| 05 | generic class with field of T | ✅ |
| 06 | higher-order `fn(T)->T` param | ✅ (fixed) |
| 07 | interface default method + self dispatch | ❌ "undefined function ''" |
| 08 | `where N*2 == M` cross-const predicate | ✅ |
| 09 | `args...*` empty pack → 1 | ✅ |
| 10 | variadic class template | ✅ |
| 11 | T::fields on primitive | ✅ |
| 12 | closure captures generic param | ✅ |

## Fixes applied this round

- **a3_04**: parseClassDecl accepts `+` as interface-list separator.
- **a3_06**: SemaExprOps now pushes `activeGenericParams_` when resolving
  compound template param types (`fn(T) -> T`, `Option<T>`, etc.) so
  `resolveType` returns Generic(T) instead of "undefined type".

## Documented remaining gaps

- a3_01 class method-level generic parser gap
- a3_02 nested generic in sig param (Option<T>) — Sema instance mangling
- a3_03 Vec<Vec<T>> — Mono scan doesn't enqueue outer when inner is also generic
- a3_07 interface default method body with `self.greet()` — internal name resolution
