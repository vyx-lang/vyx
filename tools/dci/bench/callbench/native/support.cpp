#include "abi_fixtures.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#if defined(_WIN32)
#include <windows.h>
static uint64_t qpc_ns() {
    static LARGE_INTEGER freq = {0};
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<uint64_t>((now.QuadPart * 1000000000ull) / static_cast<uint64_t>(freq.QuadPart));
}
#else
#include <time.h>
static uint64_t qpc_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}
#endif

extern "C" {

uint64_t bench_now_ns(void) { return qpc_ns(); }

uint64_t bench_iters(void) {
    const char* raw = std::getenv("DCI_CALLBENCH_ITERS");
    if (!raw || !raw[0]) {
        return 2000000000ull;
    }
    unsigned long long parsed = std::strtoull(raw, nullptr, 10);
    return parsed ? static_cast<uint64_t>(parsed) : 2000000000ull;
}

uint64_t bench_warmup(void) {
    uint64_t n = bench_iters();
    return n > 20000ull ? 20000ull : n;
}

void bench_report(const char* path, const char* kernel, uint64_t iters, uint64_t ns,
                  uint64_t sink) {
    double per = iters ? (static_cast<double>(ns) / static_cast<double>(iters)) : 0.0;
    std::printf("%s,%s,%llu,%llu,%.3f,%llu\n", path, kernel,
                static_cast<unsigned long long>(iters),
                static_cast<unsigned long long>(ns), per,
                static_cast<unsigned long long>(sink));
    std::fflush(stdout);
}

void* bench_circle_new(double r) { return new abi::Circle(r); }

void bench_shape_delete(void* p) { delete static_cast<abi::Shape*>(p); }

}  // extern "C"
