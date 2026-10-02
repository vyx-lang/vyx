# Adversarial Round 2 (Blind)

10 tests composed post-Round-1-fixes, run once blind.

## Results (post R2 fixes)

**8/10 pass** + 2 real gaps.

| # | Scenario | Result |
|---|---|---|
| 01 | class Buffer<T, const N> with N in return-type | ❌ resolver gap |
| 02 | T::kind for primitives | ✅ |
| 03 | const N in while condition | ✅ |
| 04 | pack fold `*` | ✅ |
| 05 | generic calls generic | ✅ |
| 06 | two distinct variadic fns back-to-back | ✅ |
| 07 | T::name + typeof chained | ✅ |
| 08 | same concept, 3 concrete types | ✅ |
| 09 | const template calls const template with N | ❌ cascade subst gap |
| 10 | typeof(classInstance) | ✅ |

## Round 2 fixes applied along the way

- adv_11 empty pack fold — identity element per op
- adv_10 `<const N, ...Ts>` parser order
- Related: parser double-registers ident turbofish (both type + expr slot);
  SemaResolve accepts activeConstGenericParams_; subst walks
  Identifier.callArgExprs and nulls out corresponding callTypeArgs

## Known remaining gaps

- a2_01: N in type-position of return (requires resolver awareness)
- a2_09: multi-hop const value propagation through generic call chain
