#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdint.h>
#include <stdio.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

#ifndef VYX_STDIO_ABI
#define VYX_STDIO_ABI
#endif

/* The compiler supplies an exact slice, including embedded NUL bytes.
 * Keep stdout ownership inside the platform C runtime rather than lowering
 * FILE handles or Windows CRT internals into generated LLVM IR. */
#ifdef __cplusplus
extern "C"
#endif
VYX_STDIO_ABI void vyx_print_bytes_abi(const char* value, uint64_t len, int32_t newline) {
    if (len > 0) {
        fwrite(value, 1, (size_t)len, stdout);
    }
    if (newline != 0) {
        fputc('\n', stdout);
        fflush(stdout);
    }
}

/* A failed monotonic clock is an error, never a substitute CPU/wall clock. */
#ifdef __cplusplus
extern "C"
#endif
VYX_STDIO_ABI int64_t vyx_bench_now_ns(void) {
#if defined(_WIN32)
    LARGE_INTEGER counter, frequency;
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 ||
        !QueryPerformanceCounter(&counter)) return -1;
    return (counter.QuadPart / frequency.QuadPart) * INT64_C(1000000000) +
        (counter.QuadPart % frequency.QuadPart) * INT64_C(1000000000) / frequency.QuadPart;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1;
    return (int64_t)ts.tv_sec * INT64_C(1000000000) + ts.tv_nsec;
#endif
}

/* An opaque out-of-line memory barrier for benchmark inputs and results.
 * It does not read the pointee or make claims about arbitrary optimizer
 * behavior: callers must also expose the actual result they want to retain. */
#ifdef __cplusplus
extern "C"
#endif
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
VYX_STDIO_ABI void vyx_bench_black_box(const void* value) {
#if defined(__GNUC__) || defined(__clang__)
    __asm__ __volatile__("" : : "r"(value) : "memory");
#else
    static const void* volatile sink;
    sink = value;
#endif
}
