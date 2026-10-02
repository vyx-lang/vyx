/* Hand-authored C ABI facade over the C++ fixture library.
 *
 * This header is what the extern "C" / bindgen path binds against.  It exists
 * because extern "C" cannot see C++ constructs directly: a real integration
 * must hand-write a C surface.  The cost of that surface is part of the study:
 *
 *   - POD aggregates and enums cross by value directly (C-compatible).
 *   - Non-trivial C++ value types (Buffer) collapse to opaque pointers with
 *     manual create/destroy: value semantics and the copy constructor are lost.
 *   - Polymorphic classes (Shape) collapse to opaque pointers plus one C
 *     function per virtual method: dispatch tables and RTTI are lost.
 *   - The throwing function must be wrapped to catch the exception and report
 *     an error code, because a C++ exception crossing extern "C" is undefined.
 *
 * The abi_c_true_* probes return the C++-side ground truth so the harness can
 * cross-check every path's layout against the real compiler.
 */
#ifndef ABI_FIXTURES_C_H
#define ABI_FIXTURES_C_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* C-compatible mirrors of the POD aggregates. */
typedef struct AbiCPoint {
    int32_t x;
    int32_t y;
} AbiCPoint;

typedef struct AbiCRect {
    AbiCPoint origin;
    int32_t w;
    int32_t h;
} AbiCRect;

typedef struct AbiCVec2 {
    double x;
    double y;
} AbiCVec2;

typedef struct AbiCSpan {
    const uint8_t* data;
    uint64_t len;
} AbiCSpan;

typedef struct AbiCQuad {
    int64_t a;
    int64_t b;
    int64_t c;
    int64_t d;
} AbiCQuad;

#pragma pack(push, 1)
typedef struct AbiCPackedHeader {
    uint8_t tag;
    uint32_t length;
} AbiCPackedHeader;
#pragma pack(pop)

/* POD by-value functions (C-expressible subset). */
int32_t abi_c_point_sum(AbiCPoint p);
AbiCPoint abi_c_point_translate(AbiCPoint p, int32_t dx, int32_t dy);
int32_t abi_c_rect_area(AbiCRect r);
double abi_c_vec2_dot(AbiCVec2 a, AbiCVec2 b);
uint64_t abi_c_span_checksum(AbiCSpan s);
int64_t abi_c_quad_sum(AbiCQuad q);
AbiCQuad abi_c_quad_scale(AbiCQuad q, int64_t k);

/* Enums cross as their fixed underlying integer type. */
uint8_t abi_c_color_next(uint8_t c);
int32_t abi_c_status_step(int32_t s);

uint32_t abi_c_packed_total(const AbiCPackedHeader* items, uint64_t count);

/* Non-trivial value type -> opaque pointer (value semantics lost). */
typedef struct AbiBuffer AbiBuffer;
AbiBuffer* abi_c_buffer_create(size_t n);
void abi_c_buffer_fill(AbiBuffer* b, uint8_t value);
uint64_t abi_c_buffer_checksum(const AbiBuffer* b);
size_t abi_c_buffer_size(const AbiBuffer* b);
void abi_c_buffer_destroy(AbiBuffer* b);

/* Polymorphic hierarchy -> opaque pointer + one C function per virtual. */
typedef struct AbiShape AbiShape;
AbiShape* abi_c_make_circle(double radius);
AbiShape* abi_c_make_square(double side);
double abi_c_shape_area(const AbiShape* s);
int32_t abi_c_shape_sides(const AbiShape* s);
void abi_c_shape_destroy(AbiShape* s);

/* Exception -> error code (0 == ok, 1 == threw). */
int32_t abi_c_checked_divide(int32_t a, int32_t b, int32_t* out);

/* Ground-truth ABI facts measured by the C++ compiler. */
uint64_t abi_c_true_point_size(void);
uint64_t abi_c_true_rect_size(void);
uint64_t abi_c_true_rect_origin_offset(void);
uint64_t abi_c_true_quad_size(void);
uint64_t abi_c_true_packed_size(void);
uint64_t abi_c_true_packed_length_offset(void);

#ifdef __cplusplus
}
#endif

#endif /* ABI_FIXTURES_C_H */
