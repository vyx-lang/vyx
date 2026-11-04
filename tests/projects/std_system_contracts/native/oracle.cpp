#include <chrono>
#include <cstdint>
#include <ctime>
#ifndef _WIN32
#include <time.h>
#endif
extern "C" std::int64_t system_reference_nanos() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
extern "C" std::int64_t system_reference_timestamp() { return std::time(nullptr); }
extern "C" int system_monotonic_id() {
#ifdef _WIN32
    return -1;
#else
    return CLOCK_MONOTONIC;
#endif
}
