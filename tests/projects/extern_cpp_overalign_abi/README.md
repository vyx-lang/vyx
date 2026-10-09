# extern_cpp_overalign_abi

Over-aligned foreign objects across the DCI boundary — `alignas(16)` /
`alignas(32)` / `alignas(64)` / `alignas(128)` C++ records, i.e. objects whose
alignment exceeds both the natural 8-byte ceiling of an ordinary Win64 record
**and** LLVM's largest integer alignment (i128 / 16 bytes).

The contract is **derived from the producer header by the adapter**
(`tools/dci/dci_adapter_msvc.py --include native/Overalign.hpp`), exactly like
`dci_opengeneric` derives its two contracts.  No `.dci` text is hand-written;
the only hand-authored input is the producer header itself, including its
`dci-ownership` block (ownership is a property of the producer's API, so it is
declared there).

## Measured behaviour

Windows x86_64 / `x86_64-pc-windows-msvc`, SDK compiler build with the vector-anchor
change (2026-09-24).  `run.ps1` asserts every cell in both directions: a
surface that stops compiling is a capability loss, and the `E3300` boundary
moving is a capability change.

| # | surface | `alignas(16)` | `alignas(64)` | `alignas(256)` |
|---|---|---|---|---|
| 1 | by value | OK | OK | rejected `E3300` |
| 2 | pointer / reference | OK | OK | OK |
| 3 | field embedding, producer side (`Nested`) | OK | OK | rejected `E3300` |
| 3 | field embedding, consumer side (Vyx record) | OK | OK | rejected `E3300` |
| 4 | constructor (`OverCtor`) | OK | OK | rejected `E3300` |
| 5 | return | OK | OK | rejected `E3300` |
| 6 | heap allocation | OK | OK | rejected `E3300` |
| 7 | generic argument | OK | OK | rejected `E3300` |

`src/main.vyx` runs all seven at `alignas(64)` and checks every address with
the **producer's own** `oa64_ptr_align` (which returns `pointer % 64`), so the
alignment claim is judged by C++ code that never sees how Vyx allocated the
object.  That covers both directions of the heap surface:

* the producer allocates (`oa64_heap_new` → `_aligned_malloc(64)`);
* **Vyx allocates** (`Box::<Big>` over an `@[align(64)]` class, and the
  embedded `Holder.body`), and the producer confirms the address.

## How over-alignment became supported

Vyx gives an imported DCI record a *value* layout by picking an **anchor type
whose own ABI alignment equals the descriptor's alignment**
(`finalize_record_ty_body`, `bootstrap_compiler/src/codegen/llvm/llvm_lower.vyx`).
Because the anchor is self-describing, every generic storage path — local
slot, load, store, the default `byval`/`sret` alignment — inherits the right
alignment without any per-site plumbing.

The original ladder was `1 → i8, 2 → i16, 4 → i32, 8 → i64, 16 → i128` and
hard-errored on anything else.  Measured on this target, that ladder *cannot*
be extended with integers or arrays:

| candidate anchor | ABI alignment (`llc` `.p2align`) |
|---|---|
| `i256` | 16 |
| `i512` | 16 |
| `[4 x i128]` | 16 |
| `<2 x i64>` | 16 |
| `<4 x i64>` | **32** |
| `<8 x i64>` | **64** |
| `<16 x i64>` | **128** |

So the ladder now continues with **vector anchors**
(`vyx_rt_vector_ty`, added to `vyx_codegen`).  Above 128 there is no anchor at
all, and that case is reported as a capability boundary rather than a codegen
failure.

Two supporting changes were needed for the alignment to be *honoured*, not
merely representable:

* **`value_storage_align` now consults the descriptor.**  A foreign record's
  alignment is a fact the producer measured, not something derivable from the
  consumer's field spellings: an `alignas(64)` C++ struct reaches Vyx as a
  field of `[u64; 8]`, whose widest alignment is 8.  Without this the
  descriptor's alignment never reached `alignof::<T>()`, and since `Box<T>` /
  `Ref<T>` allocate with `vyx_aligned_alloc_abi(sizeof::<T>(), alignof::<T>())`,
  every such allocation silently used 8-byte alignment.
* **A dedicated diagnostic** (`E3300`) for the remaining boundary, because the
  shared `I0100` help text tells the user to file a compiler bug.  The new help
  says what *is* supported and what to do instead.

## Known limits this fixture keeps visible

* `Box::<foreign record>` is still rejected, for a reason **unrelated to
  alignment** — it also fails at `alignas(16)`.  A foreign record may only be
  assigned from a direct constructor into final storage
  (`DCI object assignment requires a direct constructor into final storage`,
  `mir_verify.vyx`).  `negative/box_foreign_record.vyx` keeps it visible so the
  limit is not mistaken for an alignment problem.
* After the `E3300` layout failure, the compiler keeps lowering and emits
  follow-on `I0100`s with the generic help.  Those are error-recovery noise,
  not the boundary report; the gate asserts the `E3300` entry's own help and
  does not bless the cascade.

## A discovery bug this fixture caught

`struct alignas(64) OA_EXPORT Over64 { … };` puts the alignment specifier
between the class-key and the record name.  The adapter's record-discovery
regex only tolerated ALL-CAPS export macros or `final` there, so such a record
was invisible to `discover_record_names`:

* its layout was never measured, and
* every by-value symbol mentioning it was rejected as
  `value parameter type 'Over64' has no verified layout/lifecycle`

— a rejection of a fact the producer had already produced.  In this header
`Over64` only survived by accident, because `Nested` embeds it and the layout
closure dragged it in; `Over16` had no such rescuer and was rejected outright.

Fixed in `tools/dci/dci_adapter_msvc.py` by extracting `RE_DECL_SPECIFIER`
(ALL-CAPS macro | `final` | `alignas(…)` | `__declspec(…)` | `__attribute__(…)`)
and using it in both `RE_DECL_RECORD` and `RE_CLASS_SCOPE_OPEN`.  Unit test:
`tools/dci/tests/test_dci_adapter_msvc.py::
test_discovers_over_aligned_records_declared_with_alignas`.

## Layout

```
Vyx.toml                      project manifest (clang-cpp stub backend)
run.ps1                       derives the contract, builds, asserts all surfaces
native/Overalign.hpp          the producer's public API + dci-ownership block
native/Overalign.cpp          the producer implementation
contracts/Overalign.dcib      derived (regenerated on every gate run)
src/main.vyx                  all seven surfaces + runtime alignment checks
surfaces/*.vyx                one probe per surface, each must compile
negative/box_foreign_record.vyx        still rejected (assignment rule, not alignment)
negative/unsupported_alignment_256.vyx rejected with E3300 and its own help text
```
