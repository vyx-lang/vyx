#pragma once
// Producer surface for the C++ open-generic DCI reverse-override fixture.
//
// `SinkG<T>` is a *generic* producer trait; nothing here pre-instantiates it
// (no `template struct ...;`, no explicit specialization).  The closed
// instances a Vyx consumer inherits (`SinkG<int>` / `SinkG<VyxBox>` /
// `SinkG<Sample>`) are materialized on demand: the consumer's lowering
// reports them through `.dci_open` sidecars and the build closes each one
// against this header with the C++ adapter.
//
// `Sample` is a producer-owned record (row ④ of the capability matrix:
// a *producer* record as the type argument of an inherited generic base).
// `VyxBox` mirrors the consumer's own `@[repr(C)]` record (row ③): the
// consumer declares the same C layout independently and both sides measure
// it -- 12 bytes / align 4, indirect parameter + sret return under Win64.
#include <cstdint>

struct Sample {
    uint32_t lo;
    uint32_t hi;
};

// The consumer cannot literal-construct or freely move an *imported* record
// (the MIR verifier only accepts a DCI call result constructed directly into
// final storage), so the producer hands out values through extern "C" free
// functions -- the same pattern as `vyx_vec2_make` in dci_opengeneric.
// Declarations first: the adapter's free-function discovery scans for
// declaration lines ending in `;`, not inline definitions.
extern "C" Sample sample_make(uint32_t lo, uint32_t hi) noexcept;
extern "C" Sample sample_pass(Sample s) noexcept;

extern "C" Sample sample_make(uint32_t lo, uint32_t hi) noexcept {
    return Sample{lo, hi};
}

extern "C" Sample sample_pass(Sample s) noexcept { return s; }

struct VyxBox {
    uint32_t lo;
    uint32_t mid;
    uint32_t hi;
};

template <class T>
struct SinkG {
    virtual ~SinkG() = default;
    virtual T consume(T value) = 0;
};
