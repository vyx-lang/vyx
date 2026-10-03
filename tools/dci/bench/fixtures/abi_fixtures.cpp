// Implementation of the shared C++ ABI fixture library.
#include "abi_fixtures.hpp"

#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>

namespace abi {

Buffer::Buffer(std::size_t n) noexcept : data_(new (std::nothrow) uint8_t[n]), size_(n) {
    if (data_ == nullptr) {
        std::abort();
    }
    std::memset(data_, 0, n);
}

Buffer::Buffer(const Buffer& other) : data_(new uint8_t[other.size_]), size_(other.size_) {
    std::memcpy(data_, other.data_, size_);
}

Buffer& Buffer::operator=(const Buffer& other) {
    if (this != &other) {
        uint8_t* replacement = new uint8_t[other.size_];
        std::memcpy(replacement, other.data_, other.size_);
        delete[] data_;
        data_ = replacement;
        size_ = other.size_;
    }
    return *this;
}

Buffer::~Buffer() noexcept { delete[] data_; }

std::size_t Buffer::size() const noexcept { return size_; }

void Buffer::fill(uint8_t value) noexcept {
    std::memset(data_, value, size_);
}

uint64_t Buffer::checksum() const noexcept {
    uint64_t acc = 1469598103934665603ULL;  // FNV-1a offset basis
    for (std::size_t i = 0; i < size_; ++i) {
        acc ^= data_[i];
        acc *= 1099511628211ULL;
    }
    return acc;
}

uint64_t buffer_consume(Buffer b) { return b.checksum(); }

Shape::Shape() noexcept = default;
Shape::~Shape() noexcept = default;

Circle::Circle(double radius) noexcept : radius_(radius) {}
Circle::~Circle() noexcept = default;
double Circle::area() const noexcept { return 3.141592653589793 * radius_ * radius_; }
int32_t Circle::sides() const noexcept { return 0; }

Square::Square(double side) noexcept : side_(side) {}
Square::~Square() noexcept = default;
double Square::area() const noexcept { return side_ * side_; }
int32_t Square::sides() const noexcept { return 4; }

int32_t point_sum(Point p) noexcept { return p.x + p.y; }

Point point_translate(Point p, int32_t dx, int32_t dy) noexcept {
    return Point{p.x + dx, p.y + dy};
}

int32_t rect_area(Rect r) noexcept { return r.w * r.h; }

double vec2_dot(Vec2 a, Vec2 b) noexcept { return a.x * b.x + a.y * b.y; }

uint64_t span_checksum(Span s) noexcept {
    uint64_t acc = 1469598103934665603ULL;
    for (uint64_t i = 0; i < s.len; ++i) {
        acc ^= s.data[i];
        acc *= 1099511628211ULL;
    }
    return acc;
}

int64_t quad_sum(Quad q) noexcept { return q.a + q.b + q.c + q.d; }

Quad quad_scale(Quad q, int64_t k) noexcept {
    return Quad{q.a * k, q.b * k, q.c * k, q.d * k};
}

Color color_next(Color c) noexcept {
    return static_cast<Color>((static_cast<uint8_t>(c) + 1) % 3);
}

Status status_step(Status s) noexcept {
    switch (s) {
        case Ok:
            return Retry;
        case Retry:
            return Fatal;
        default:
            return Fatal;
    }
}

uint32_t packed_total(const PackedHeader* items, uint64_t count) noexcept {
    uint32_t total = 0;
    for (uint64_t i = 0; i < count; ++i) {
        total += items[i].length;
    }
    return total;
}

int32_t span_first(const Span* s) noexcept {
    if (s == nullptr || s->len == 0) {
        return -1;
    }
    return static_cast<int32_t>(s->data[0]);
}

int32_t checked_divide(int32_t a, int32_t b) {
    if (b == 0) {
        throw std::runtime_error("abi::checked_divide: division by zero");
    }
    return a / b;
}

}  // namespace abi
