#include "abi_fixtures.hpp"

#include <cstddef>
#include <cstdint>

extern "C" {
uint64_t bench_now_ns(void);
uint64_t bench_iters(void);
uint64_t bench_warmup(void);
void bench_report(const char* path, const char* kernel, uint64_t iters, uint64_t ns,
                  uint64_t sink);
void* bench_circle_new(double r);
void bench_shape_delete(void* p);
}

int main() {
    const uint64_t iters = bench_iters();
    const uint64_t warmup = bench_warmup();

    abi::Point p{3, 4};
    for (uint64_t i = 0; i < warmup; ++i) {
        (void)abi::point_sum(p);
    }
    uint64_t sink = 0;
    uint64_t t0 = bench_now_ns();
    for (uint64_t i = 0; i < iters; ++i) {
        sink += static_cast<uint64_t>(static_cast<uint32_t>(abi::point_sum(p)));
    }
    bench_report("native", "point_sum", iters, bench_now_ns() - t0, sink);

    abi::Buffer buf(static_cast<std::size_t>(1));
    for (uint64_t i = 0; i < warmup; ++i) {
        buf.fill(0xAB);
    }
    sink = 0;
    t0 = bench_now_ns();
    for (uint64_t i = 0; i < iters; ++i) {
        buf.fill(0xAB);
        sink += 1ull;
    }
    bench_report("native", "buffer_fill", iters, bench_now_ns() - t0, sink);

    buf.fill(0xAB);
    for (uint64_t i = 0; i < warmup; ++i) {
        (void)buf.checksum();
    }
    sink = 0;
    t0 = bench_now_ns();
    for (uint64_t i = 0; i < iters; ++i) {
        sink ^= buf.checksum();
    }
    bench_report("native", "buffer_checksum", iters, bench_now_ns() - t0, sink);

    for (uint64_t i = 0; i < warmup; ++i) {
        (void)buf.size();
    }
    sink = 0;
    t0 = bench_now_ns();
    for (uint64_t i = 0; i < iters; ++i) {
        sink += static_cast<uint64_t>(buf.size());
    }
    bench_report("native", "buffer_size", iters, bench_now_ns() - t0, sink);

    auto* shape = static_cast<abi::Shape*>(bench_circle_new(2.0));
    for (uint64_t i = 0; i < warmup; ++i) {
        (void)shape->area();
    }
    sink = 0;
    t0 = bench_now_ns();
    for (uint64_t i = 0; i < iters; ++i) {
        sink += static_cast<uint64_t>(static_cast<int64_t>(shape->area() * 1000.0));
    }
    bench_report("native", "shape_area", iters, bench_now_ns() - t0, sink);

    for (uint64_t i = 0; i < warmup; ++i) {
        (void)shape->sides();
    }
    sink = 0;
    t0 = bench_now_ns();
    for (uint64_t i = 0; i < iters; ++i) {
        sink += static_cast<uint64_t>(static_cast<uint32_t>(shape->sides()));
    }
    bench_report("native", "shape_sides", iters, bench_now_ns() - t0, sink);
    bench_shape_delete(shape);
    return 0;
}
