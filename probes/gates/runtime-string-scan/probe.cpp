#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
static uint64_t mincore_calls = 0;
extern "C" int mincore(void* address, size_t size, unsigned char* result) noexcept {
    ++mincore_calls;
    return static_cast<int>(syscall(SYS_mincore, address, size, result));
}
#endif
#endif

#ifdef VYX_SCAN_UNIT
#include "../../../vyx_codegen/src/vyx_bootstrap_cstring.h"
static int64_t unit_length(const char* value) { return vyx_rt_bounded_cstr_len(value); }
#endif

int main(int argc, char** argv) {
    using Length = int64_t (*)(const char*);
    using Contains = int32_t (*)(const char*, uint64_t, const char*, uint64_t);
    Length length;
    Contains contains = nullptr;
#ifdef VYX_SCAN_UNIT
    length = unit_length;
#else
    if (argc != 2) return 2;
#ifdef _WIN32
    HMODULE library = LoadLibraryA(argv[1]);
    if (!library) return 3;
    length = reinterpret_cast<Length>(GetProcAddress(library, "vyx_string_len"));
    contains = reinterpret_cast<Contains>(GetProcAddress(library, "vyx_string_contains"));
#else
    void* library = dlopen(argv[1], RTLD_NOW);
    if (!library) { std::fprintf(stderr, "%s\n", dlerror()); return 3; }
    length = reinterpret_cast<Length>(dlsym(library, "vyx_string_len"));
    contains = reinterpret_cast<Contains>(dlsym(library, "vyx_string_contains"));
#endif
    if (!length || !contains) return 4;
#endif

#ifdef _WIN32
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const size_t page = system.dwPageSize;
    char* data = static_cast<char*>(VirtualAlloc(nullptr, page * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
#else
    const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    char* data = static_cast<char*>(mmap(nullptr, page * 4, PROT_READ | PROT_WRITE,
                                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (data == MAP_FAILED) return 5;
#endif
    if (!data) return 5;
    char* short_text = data + 32;
    std::strcpy(short_text, "parse identifier");
    const auto start = std::chrono::steady_clock::now();
#if defined(__linux__)
    const auto before = mincore_calls;
#endif
    for (int i = 0; i < 20000; ++i) {
        if (length(short_text) != 16) return 10;
        if (contains && !contains(short_text, 16, short_text + 6, 10)) return 11;
    }
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();
#if defined(__linux__)
    const auto probes = mincore_calls - before;
    std::printf("short strings: %lld us, mincore calls=%llu\n",
                static_cast<long long>(micros), static_cast<unsigned long long>(probes));
    // Structural performance gate; independent of runner speed or contention.
    if (probes != 0) return 30;
#else
    std::printf("short strings: %lld us\n", static_cast<long long>(micros));
#endif
    std::strcpy(short_text, "new text at the same address");
    if (length(short_text) != static_cast<int64_t>(std::strlen(short_text))) return 12;
    std::strcpy(short_text, "tiny");
    if (length(short_text) != 4) return 13;
    char* crossing = data + page - 8;
    std::memset(crossing, 'q', 32);
    crossing[32] = 0;
    if (length(crossing) != 32) return 14;
    if (contains && !contains(crossing, 32, "qqq", 3)) return 15;

#ifdef _WIN32
    DWORD previous = 0;
    if (!VirtualProtect(data + page * 2, page, PAGE_NOACCESS, &previous)) return 16;
#else
    if (mprotect(data + page * 2, page, PROT_NONE) != 0) return 16;
#endif
    char* bounded = data + page * 2 - 8;
    std::memcpy(bounded, "abcdefgh", 8);
    if (length(bounded) != 8) return 17;
    if (contains && !contains(bounded, 8, "fgh", 3)) return 18;
    // A claimed larger span must stop at an unreadable guard page.
    if (contains && contains(bounded, 32, "abcdefghx", 9)) return 19;
    bounded[7] = 0;
    if (length(bounded) != 7) return 20;
#ifdef _WIN32
    VirtualFree(data, 0, MEM_RELEASE);
#else
    munmap(data, page * 4);
#endif
    std::puts("PASS string scanner: mutation, page crossing, guard, short-input work bound");
    return 0;
}
