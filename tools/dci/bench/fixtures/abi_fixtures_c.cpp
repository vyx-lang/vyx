// Implementation of the hand-authored C ABI facade.  Compiled as C++ so it can
// call the C++ fixture library, but every exported symbol has C linkage.
#include "abi_fixtures_c.h"

#include "abi_fixtures.hpp"

#include <cstddef>
#include <new>

extern "C" {

int32_t abi_c_point_sum(AbiCPoint p) {
    return abi::point_sum(abi::Point{p.x, p.y});
}

AbiCPoint abi_c_point_translate(AbiCPoint p, int32_t dx, int32_t dy) {
    abi::Point r = abi::point_translate(abi::Point{p.x, p.y}, dx, dy);
    return AbiCPoint{r.x, r.y};
}

int32_t abi_c_rect_area(AbiCRect r) {
    return abi::rect_area(abi::Rect{abi::Point{r.origin.x, r.origin.y}, r.w, r.h});
}

double abi_c_vec2_dot(AbiCVec2 a, AbiCVec2 b) {
    return abi::vec2_dot(abi::Vec2{a.x, a.y}, abi::Vec2{b.x, b.y});
}

uint64_t abi_c_span_checksum(AbiCSpan s) {
    return abi::span_checksum(abi::Span{s.data, s.len});
}

int64_t abi_c_quad_sum(AbiCQuad q) {
    return abi::quad_sum(abi::Quad{q.a, q.b, q.c, q.d});
}

AbiCQuad abi_c_quad_scale(AbiCQuad q, int64_t k) {
    abi::Quad r = abi::quad_scale(abi::Quad{q.a, q.b, q.c, q.d}, k);
    return AbiCQuad{r.a, r.b, r.c, r.d};
}

uint8_t abi_c_color_next(uint8_t c) {
    return static_cast<uint8_t>(abi::color_next(static_cast<abi::Color>(c)));
}

int32_t abi_c_status_step(int32_t s) {
    return static_cast<int32_t>(abi::status_step(static_cast<abi::Status>(s)));
}

uint32_t abi_c_packed_total(const AbiCPackedHeader* items, uint64_t count) {
    return abi::packed_total(reinterpret_cast<const abi::PackedHeader*>(items), count);
}

AbiBuffer* abi_c_buffer_create(size_t n) {
    return reinterpret_cast<AbiBuffer*>(new (std::nothrow) abi::Buffer(n));
}

void abi_c_buffer_fill(AbiBuffer* b, uint8_t value) {
    reinterpret_cast<abi::Buffer*>(b)->fill(value);
}

uint64_t abi_c_buffer_checksum(const AbiBuffer* b) {
    return reinterpret_cast<const abi::Buffer*>(b)->checksum();
}

size_t abi_c_buffer_size(const AbiBuffer* b) {
    return reinterpret_cast<const abi::Buffer*>(b)->size();
}

void abi_c_buffer_destroy(AbiBuffer* b) {
    delete reinterpret_cast<abi::Buffer*>(b);
}

AbiShape* abi_c_make_circle(double radius) {
    return reinterpret_cast<AbiShape*>(static_cast<abi::Shape*>(new (std::nothrow) abi::Circle(radius)));
}

AbiShape* abi_c_make_square(double side) {
    return reinterpret_cast<AbiShape*>(static_cast<abi::Shape*>(new (std::nothrow) abi::Square(side)));
}

double abi_c_shape_area(const AbiShape* s) {
    return reinterpret_cast<const abi::Shape*>(s)->area();
}

int32_t abi_c_shape_sides(const AbiShape* s) {
    return reinterpret_cast<const abi::Shape*>(s)->sides();
}

void abi_c_shape_destroy(AbiShape* s) {
    delete reinterpret_cast<abi::Shape*>(s);
}

int32_t abi_c_checked_divide(int32_t a, int32_t b, int32_t* out) {
    try {
        int32_t value = abi::checked_divide(a, b);
        if (out != nullptr) {
            *out = value;
        }
        return 0;
    } catch (...) {
        return 1;
    }
}

uint64_t abi_c_true_point_size(void) { return sizeof(abi::Point); }
uint64_t abi_c_true_rect_size(void) { return sizeof(abi::Rect); }
uint64_t abi_c_true_rect_origin_offset(void) { return offsetof(abi::Rect, origin); }
uint64_t abi_c_true_quad_size(void) { return sizeof(abi::Quad); }
uint64_t abi_c_true_packed_size(void) { return sizeof(abi::PackedHeader); }
uint64_t abi_c_true_packed_length_offset(void) { return offsetof(abi::PackedHeader, length); }

}  // extern "C"
