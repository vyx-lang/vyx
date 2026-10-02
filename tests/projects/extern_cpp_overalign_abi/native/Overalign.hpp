#pragma once
#include <cstdint>

#if defined(_WIN32)
#define OA_EXPORT __declspec(dllexport)
#else
#define OA_EXPORT __attribute__((visibility("default")))
#endif

// Over-aligned foreign objects.
//
// A record whose alignment exceeds 8 (the natural ceiling of an ordinary
// record on Win64) and, in the `alignas(64)` case, exceeds LLVM's largest
// "anchor" integer type (i128 / 16 bytes).  Vyx derives a foreign record's
// value layout from the descriptor's `alignment` field by mapping it onto one
// integer anchor plus an i8 tail, so alignment values above 16 have no anchor
// at all.
//
// `Over16` is the control: 16 bytes of alignment is representable (i128) and
// must keep working end to end.

struct alignas(16) OA_EXPORT Over16 {
    std::uint64_t x[2];
};

struct alignas(64) OA_EXPORT Over64 {
    std::uint64_t x[8];
};

// The first alignment the backend has no anchor type for: above 128 there is no
// vector width the target gives a matching ABI alignment to, so this record is
// rejected with the capability-boundary diagnostic (E3300) rather than the
// generic codegen failure.  Kept in the same header so the boundary is measured
// next to the alignments that do work.
struct alignas(256) OA_EXPORT Over256 {
    std::uint64_t x[32];
};

// Field embedding: the embedded over-aligned member forces the whole record to
// 64-byte alignment and pushes `body` to offset 64.
struct OA_EXPORT Nested {
    std::int32_t tag;
    Over64 body;
};

// Constructor surface: a user-provided constructor makes the record
// non-trivial, so the adapter must decide what it can promise about its value
// ABI.
struct alignas(64) OA_EXPORT OverCtor {
    std::uint64_t x[8];
    explicit OverCtor(std::uint64_t seed) noexcept;
    std::uint64_t sum() const noexcept;
};

// ---------------------------------------------------------------------------
// 1. by value
// ---------------------------------------------------------------------------
OA_EXPORT std::uint64_t oa16_byval(Over16 v) noexcept;
OA_EXPORT std::uint64_t oa64_byval(Over64 v) noexcept;
OA_EXPORT std::uint64_t oa256_byval(Over256 v) noexcept;

// ---------------------------------------------------------------------------
// 2. pointer / reference
// ---------------------------------------------------------------------------
OA_EXPORT std::uint64_t oa64_byref(const Over64* p) noexcept;
OA_EXPORT std::uint64_t oa64_byref_mut(Over64* p) noexcept;
OA_EXPORT std::uint64_t oa64_nested_read(const Nested* n) noexcept;
OA_EXPORT std::uint64_t oa64_ctor_sum(const OverCtor* c) noexcept;

// ---------------------------------------------------------------------------
// 5. return
// ---------------------------------------------------------------------------
OA_EXPORT Over16 oa16_make() noexcept;
OA_EXPORT Over64 oa64_make() noexcept;
OA_EXPORT Nested oa64_nested_make() noexcept;
OA_EXPORT OverCtor oa64_ctor_make() noexcept;

// ---------------------------------------------------------------------------
// 6. heap allocation
// ---------------------------------------------------------------------------
OA_EXPORT Over64* oa64_heap_new() noexcept;
OA_EXPORT void oa64_heap_free(Over64* p) noexcept;

// Runtime facts the consumer can assert against.
OA_EXPORT std::uint64_t oa64_alignof() noexcept;
OA_EXPORT std::uint64_t oa64_sizeof() noexcept;
OA_EXPORT std::uint64_t oa64_ptr_align(const Over64* p) noexcept;

/* dci-ownership
{
  "oa64_byref(const Over64*)": {
    "parameters": { "0": "borrow" }
  },
  "oa64_byref_mut(Over64*)": {
    "parameters": { "0": "borrow_mut" }
  },
  "oa64_nested_read(const Nested*)": {
    "parameters": { "0": "borrow" }
  },
  "oa64_ctor_sum(const OverCtor*)": {
    "parameters": { "0": "borrow" }
  },
  "oa64_heap_new()": {
    "return": "owned"
  },
  "oa64_heap_free(Over64*)": {
    "parameters": { "0": "move" }
  },
  "oa64_ptr_align(const Over64*)": {
    "parameters": { "0": "borrow" }
  }
}
dci-ownership-end */
