// Shared C++ ABI fixture library for the DCI vs Rust-FFI vs cxx benchmark.
//
// This header is the single source of truth exercised by all three binding
// paths.  It is deliberately self-contained (only <cstdint>, <cstddef>,
// <string>, <stdexcept>) and does NOT depend on the Vyx standard library.  The
// type families below span the C++ ABI surface a real native library exposes:
// register-class aggregates, SSE aggregates, memory-class aggregates, nested
// aggregates, scoped/unscoped enums, packed records, bit-fields, non-trivial
// (RAII) value types, polymorphic classes, and a throwing function.
//
// A DCI adapter reads this header directly.  cxx binds the expressible subset
// through a #[cxx::bridge].  extern "C" / bindgen cannot see any of these C++
// entities and instead binds the hand-written C facade in abi_fixtures_c.h.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace abi {

// --- Family: register-class POD aggregate (single INTEGER eightbyte) --------
struct Point {
    int32_t x;
    int32_t y;
};

// --- Family: nested POD aggregate (two INTEGER eightbytes) ------------------
struct Rect {
    Point origin;
    int32_t w;
    int32_t h;
};

// --- Family: SSE aggregate (two doubles -> xmm0/xmm1) -----------------------
struct Vec2 {
    double x;
    double y;
};

// --- Family: mixed pointer+integer aggregate (string view shape) ------------
struct Span {
    const uint8_t* data;
    uint64_t len;
};

// --- Family: memory-class aggregate (32 bytes, passed indirectly) -----------
struct Quad {
    int64_t a;
    int64_t b;
    int64_t c;
    int64_t d;
};

// --- Family: scoped enum ----------------------------------------------------
enum class Color : uint8_t { Red = 0, Green = 1, Blue = 2 };

// --- Family: unscoped enum with fixed underlying type -----------------------
enum Status : int32_t { Ok = 0, Retry = 1, Fatal = 2 };

// --- Family: packed record (ABI trap: no natural alignment padding) ---------
#pragma pack(push, 1)
struct PackedHeader {
    uint8_t tag;
    uint32_t length;
};
#pragma pack(pop)

// --- Family: non-trivial (RAII) value type ----------------------------------
// User-declared copy constructor and destructor make this non-trivial for the
// purposes of calls; the Itanium ABI passes it in memory and the caller must
// run the copy constructor.  Passing it across a plain C boundary is unsound.
class Buffer {
public:
    explicit Buffer(std::size_t n) noexcept;
    Buffer(const Buffer& other);
    Buffer& operator=(const Buffer& other);
    ~Buffer() noexcept;

    std::size_t size() const noexcept;
    void fill(uint8_t value) noexcept;
    uint64_t checksum() const noexcept;

private:
    uint8_t* data_;
    std::size_t size_;
};

// Passing a non-trivial (RAII) value type by value: the Itanium ABI passes it
// in memory and the caller must run the copy constructor.  No plain C boundary
// can express this, and the DCI adapter fails closed on the by-value lowering.
uint64_t buffer_consume(Buffer b);

// --- Family: polymorphic class hierarchy (virtual dispatch + RTTI) ----------
class Shape {
public:
    Shape() noexcept;
    virtual ~Shape() noexcept;
    virtual double area() const noexcept = 0;
    virtual int32_t sides() const noexcept = 0;
};

class Circle : public Shape {
public:
    explicit Circle(double radius) noexcept;
    ~Circle() noexcept override;
    double area() const noexcept override;
    int32_t sides() const noexcept override;

private:
    double radius_;
};

class Square : public Shape {
public:
    explicit Square(double side) noexcept;
    ~Square() noexcept override;
    double area() const noexcept override;
    int32_t sides() const noexcept override;

private:
    double side_;
};

// --- Free functions: by-value ABI classification exercise -------------------
// Single INTEGER eightbyte, expected to coerce to i64.
int32_t point_sum(Point p) noexcept;
// Returns an aggregate by value. Clang CodeGen typically uses one i64.
Point point_translate(Point p, int32_t dx, int32_t dy) noexcept;
// Two INTEGER eightbytes: Clang IR is i64,i64 (not a synthetic u128).
int32_t rect_area(Rect r) noexcept;
// Two SSE eightbytes: Clang IR is double,double.
double vec2_dot(Vec2 a, Vec2 b) noexcept;
// Pointer plus integer: Clang IR is ptr,i64.
uint64_t span_checksum(Span s) noexcept;
// Memory-class aggregate: byval/indirect param, sret return.
int64_t quad_sum(Quad q) noexcept;
Quad quad_scale(Quad q, int64_t k) noexcept;
// Scoped enum lowered to its underlying integer.
Color color_next(Color c) noexcept;
// Unscoped enum lowered to its underlying integer.
Status status_step(Status s) noexcept;

// Raw-pointer parameter WITH an ownership annotation (see block below): the
// DCI adapter binds it as a borrow.
uint32_t packed_total(const PackedHeader* items, uint64_t count) noexcept;

// Raw-pointer parameter WITHOUT an ownership annotation: the DCI adapter must
// fail closed and record a precise reason in exports.rejected_symbols.
int32_t span_first(const Span* s) noexcept;

// Throwing function: divides a by b and throws std::runtime_error when b == 0.
// The default DCI boundary is no_unwind, so a translated boundary or a stub is
// required for a Vyx caller to observe the failure without native unwind.
int32_t checked_divide(int32_t a, int32_t b);

}  // namespace abi

// Ownership annotations for the raw-pointer parameters so the DCI adapter can
// bind them instead of fail-closing.  span_first is intentionally left
// unannotated to exercise the fail-closed path.
/* dci-ownership
{
  "packed_total(const PackedHeader*,uint64_t)": {
    "parameters": { "0": "borrow" }
  }
}
dci-ownership-end */
