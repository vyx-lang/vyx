#include "Overalign.hpp"

#include <cstdint>
#include <cstddef>
#include <new>

#if defined(_WIN32)
#include <malloc.h>
#endif

namespace {

void* oa_alloc64(std::size_t bytes) noexcept {
#if defined(_WIN32)
    return _aligned_malloc(bytes, 64);
#else
    // aligned_alloc requires the size to be a multiple of the alignment.
    std::size_t rounded = (bytes + 63u) & ~static_cast<std::size_t>(63u);
    return std::aligned_alloc(64, rounded);
#endif
}

void oa_free64(void* p) noexcept {
#if defined(_WIN32)
    _aligned_free(p);
#else
    std::free(p);
#endif
}

}  // namespace

OverCtor::OverCtor(std::uint64_t seed) noexcept {
    for (int i = 0; i < 8; ++i) {
        x[i] = seed + static_cast<std::uint64_t>(i);
    }
}

std::uint64_t OverCtor::sum() const noexcept {
    std::uint64_t total = 0;
    for (int i = 0; i < 8; ++i) {
        total += x[i];
    }
    return total;
}

// ---------------------------------------------------------------------------
// 1. by value
// ---------------------------------------------------------------------------

std::uint64_t oa16_byval(Over16 v) noexcept {
    return v.x[0] * 1000u + v.x[1];
}

std::uint64_t oa64_byval(Over64 v) noexcept {
    std::uint64_t acc = 0;
    for (int i = 0; i < 8; ++i) {
        acc = acc * 31u + v.x[i];
    }
    return acc;
}

std::uint64_t oa256_byval(Over256 v) noexcept {
    std::uint64_t acc = 0;
    for (int i = 0; i < 32; ++i) {
        acc = acc * 31u + v.x[i];
    }
    return acc;
}

// ---------------------------------------------------------------------------
// 2. pointer / reference
// ---------------------------------------------------------------------------

std::uint64_t oa64_byref(const Over64* p) noexcept {
    std::uint64_t total = 0;
    for (int i = 0; i < 8; ++i) {
        total += p->x[i];
    }
    return total;
}

std::uint64_t oa64_byref_mut(Over64* p) noexcept {
    for (int i = 0; i < 8; ++i) {
        p->x[i] += 1;
    }
    return p->x[0];
}

std::uint64_t oa64_nested_read(const Nested* n) noexcept {
    return static_cast<std::uint64_t>(n->tag) + n->body.x[7];
}

std::uint64_t oa64_ctor_sum(const OverCtor* c) noexcept {
    return c->sum();
}

// ---------------------------------------------------------------------------
// 5. return
// ---------------------------------------------------------------------------

Over16 oa16_make() noexcept {
    Over16 v{};
    v.x[0] = 11;
    v.x[1] = 22;
    return v;
}

Over64 oa64_make() noexcept {
    Over64 v{};
    for (int i = 0; i < 8; ++i) {
        v.x[i] = static_cast<std::uint64_t>(i) + 1u;
    }
    return v;
}

Nested oa64_nested_make() noexcept {
    Nested n{};
    n.tag = 7;
    for (int i = 0; i < 8; ++i) {
        n.body.x[i] = static_cast<std::uint64_t>(i) + 100u;
    }
    return n;
}

OverCtor oa64_ctor_make() noexcept {
    return OverCtor(5);
}

// ---------------------------------------------------------------------------
// 6. heap allocation
// ---------------------------------------------------------------------------

Over64* oa64_heap_new() noexcept {
    void* raw = oa_alloc64(sizeof(Over64));
    if (raw == nullptr) {
        return nullptr;
    }
    Over64* p = new (raw) Over64{};
    for (int i = 0; i < 8; ++i) {
        p->x[i] = static_cast<std::uint64_t>(i) + 1u;
    }
    return p;
}

void oa64_heap_free(Over64* p) noexcept {
    if (p == nullptr) {
        return;
    }
    p->~Over64();
    oa_free64(p);
}

// ---------------------------------------------------------------------------
// measured facts
// ---------------------------------------------------------------------------

std::uint64_t oa64_alignof() noexcept {
    return alignof(Over64);
}

std::uint64_t oa64_sizeof() noexcept {
    return sizeof(Over64);
}

std::uint64_t oa64_ptr_align(const Over64* p) noexcept {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(p) % 64u);
}
