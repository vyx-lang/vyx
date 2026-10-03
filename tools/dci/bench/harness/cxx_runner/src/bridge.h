// Hand-written C++ trampolines for the cxx binding path.
//
// cxx shared structs are *cxx-defined* types, so binding an existing C++ library
// requires per-item C++ glue compiled by the producer's C++ compiler: each
// trampoline converts a cxx shared struct to the library type and forwards the
// call.  This file is the measured "hand-written glue" cost of the cxx path.
#pragma once

#include "abi_fixtures.hpp"
#include "cxx_runner/src/main.rs.h"

#include <cstdint>
#include <memory>

namespace bench {

inline int32_t point_sum(Point p) { return abi::point_sum(abi::Point{p.x, p.y}); }

inline Point point_translate(Point p, int32_t dx, int32_t dy) {
    abi::Point r = abi::point_translate(abi::Point{p.x, p.y}, dx, dy);
    return Point{r.x, r.y};
}

inline int32_t rect_area(Rect r) {
    return abi::rect_area(abi::Rect{abi::Point{r.origin.x, r.origin.y}, r.w, r.h});
}

inline double vec2_dot(Vec2 a, Vec2 b) {
    return abi::vec2_dot(abi::Vec2{a.x, a.y}, abi::Vec2{b.x, b.y});
}

inline uint64_t span_checksum(Span s) { return abi::span_checksum(abi::Span{s.data, s.len}); }

inline int64_t quad_sum(Quad q) { return abi::quad_sum(abi::Quad{q.a, q.b, q.c, q.d}); }

inline Quad quad_scale(Quad q, int64_t k) {
    abi::Quad r = abi::quad_scale(abi::Quad{q.a, q.b, q.c, q.d}, k);
    return Quad{r.a, r.b, r.c, r.d};
}

inline uint8_t color_next(uint8_t c) {
    return static_cast<uint8_t>(abi::color_next(static_cast<abi::Color>(c)));
}

inline int32_t status_step(int32_t s) {
    return static_cast<int32_t>(abi::status_step(static_cast<abi::Status>(s)));
}

inline std::unique_ptr<abi::Buffer> make_buffer(std::size_t n) {
    return std::unique_ptr<abi::Buffer>(new abi::Buffer(n));
}

inline std::unique_ptr<abi::Shape> make_circle(double r) {
    return std::unique_ptr<abi::Shape>(new abi::Circle(r));
}

inline std::unique_ptr<abi::Shape> make_square(double s) {
    return std::unique_ptr<abi::Shape>(new abi::Square(s));
}

// Throws std::runtime_error on b == 0; cxx converts the exception into a Rust
// Result at the bridge boundary.
inline int32_t checked_divide(int32_t a, int32_t b) { return abi::checked_divide(a, b); }

}  // namespace bench
