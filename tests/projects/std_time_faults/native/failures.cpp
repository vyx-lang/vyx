#include <cstdint>
#include <limits>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <cstdlib>
#else
#include <time.h>
#include <errno.h>
#endif
namespace {
int mode, calls;
std::uint64_t milliseconds;
}
#ifdef _WIN32
static void install_clock_faults();
#endif
extern "C" void time_fault_mode(int value) {
#ifdef _WIN32
    install_clock_faults();
#endif
    mode = value; calls = 0; milliseconds = 0;
}
extern "C" int time_fault_calls() { return calls; }
extern "C" std::uint64_t time_fault_milliseconds() { return milliseconds; }
#ifdef _WIN32
static BOOL WINAPI fault_frequency(LARGE_INTEGER* out) {
    out->QuadPart = mode == 4 ? std::numeric_limits<std::int64_t>::max() : 1000000000;
    return mode != 1;
}
static BOOL WINAPI fault_counter(LARGE_INTEGER* out) {
    out->QuadPart = mode == 2 ? -1 : mode == 3 || mode == 7 ? std::numeric_limits<std::int64_t>::max() :
           mode == 4 ? std::numeric_limits<std::int64_t>::max() - 1 : 10123456789;
    return mode != 9;
}
static void WINAPI fault_sleep(DWORD value) { ++calls; milliseconds += value; }
extern "C" {
    extern void* __imp_QueryPerformanceCounter;
    extern void* __imp_QueryPerformanceFrequency;
    extern void* __imp_Sleep;
}
static void replace_import(void** slot, void* replacement) {
    DWORD previous, ignored;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &previous)) std::abort();
    *slot = replacement;
    if (!VirtualProtect(slot, sizeof(*slot), previous, &ignored)) std::abort();
}
static void install_clock_faults() {
    // Change only this test executable's import slots. No OS DLL or producer
    // artifact is edited; each test invocation runs in its own process.
    static bool installed;
    if (installed) return;
    replace_import(&__imp_QueryPerformanceCounter, reinterpret_cast<void*>(&fault_counter));
    replace_import(&__imp_QueryPerformanceFrequency, reinterpret_cast<void*>(&fault_frequency));
    replace_import(&__imp_Sleep, reinterpret_cast<void*>(&fault_sleep));
    installed = true;
}
#else
#ifdef __linux__
#define TIME_NOTHROW noexcept
#else
#define TIME_NOTHROW
#endif
extern "C" int clock_gettime(clockid_t id, timespec* out) TIME_NOTHROW {
    if (id != CLOCK_MONOTONIC || mode == 1) { errno = EINVAL; return -1; }
    out->tv_sec = mode == 3 ? 9223372037LL : mode == 7 ? 9223372036LL : mode == 4 ? 0 : 10;
    out->tv_nsec = mode == 2 ? 1000000000L : mode == 4 ? 999999999L : mode == 7 ? 854775807L : 123456789L;
    return 0;
}
extern "C" int nanosleep(const timespec* req, timespec* remaining) {
    ++calls;
    if (mode == 5 && calls == 1) {
        remaining->tv_sec = 1;
        remaining->tv_nsec = 25000000;
        errno = EINTR;
        return -1;
    }
    if (mode == 6) { errno = EINVAL; return -1; }
    if (mode == 5 && (req->tv_sec != 1 || req->tv_nsec != 25000000)) {
        errno = EINVAL; return -1;
    }
    milliseconds += req->tv_sec * 1000ULL + req->tv_nsec / 1000000ULL;
    return 0;
}
#endif
