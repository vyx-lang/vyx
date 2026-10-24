#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <sys/mman.h>
#  include <unistd.h>
#endif

// Compatibility for pointer-only bootstrap string APIs. Explicit registered
// byte views do not use this scanner. Unknown scan results must stay uncached:
// an external owner can mutate or reuse the same address at any time.
static uint64_t vyx_rt_readable_span(const char* s, uint64_t limit) {
    if (!s || limit == 0) return 0;
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(s, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
    const DWORD prot = mbi.Protect & 0xff;
    if (prot != PAGE_READONLY && prot != PAGE_READWRITE && prot != PAGE_WRITECOPY &&
        prot != PAGE_EXECUTE_READ && prot != PAGE_EXECUTE_READWRITE &&
        prot != PAGE_EXECUTE_WRITECOPY) return 0;
    const auto address = reinterpret_cast<std::uintptr_t>(s);
    const auto begin = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
    if (address < begin || address - begin >= mbi.RegionSize) return 0;
    return std::min(limit, static_cast<uint64_t>(mbi.RegionSize - (address - begin)));
#else
    static const auto page = static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
    if (page == 0 || page > (1u << 20)) {
        std::fputs("[vyx-rt] cannot determine memory page size\n", stderr);
        std::abort();
    }
    const auto address = reinterpret_cast<std::uintptr_t>(s);
    const uint64_t first_page = page - address % page;
    // The caller supplies a valid starting pointer. Reading within its page
    // needs no probes of unrelated subsequent pages.
    if (limit <= first_page) return limit;
#if defined(__linux__)
    // Only page-crossing inputs reach here. mincore reports residency, not
    // read permission: a mapped PROT_NONE guard must not be treated as data.
    FILE* maps = std::fopen("/proc/self/maps", "r");
    if (!maps) {
        std::fputs("[vyx-rt] cannot inspect memory permissions for a page-crossing string\n", stderr);
        std::abort();
    }
    char line[2048];
    uint64_t end = address;
    while (std::fgets(line, sizeof(line), maps)) {
        // A long mapped filename can occupy multiple buffer fills; only the
        // first fragment contains a mapping record.
        if (!std::strchr(line, '\n')) {
            int c;
            do { c = std::fgetc(maps); } while (c != '\n' && c != EOF);
        }
        unsigned long long begin = 0, finish = 0;
        char permissions[5]{};
        if (std::sscanf(line, "%llx-%llx %4s", &begin, &finish, permissions) != 3) continue;
        if (finish <= end) continue;
        if (begin > end || permissions[0] != 'r') break;
        end = finish;
        if (end - address >= limit) break;
    }
    std::fclose(maps);
    return std::min(limit, end - address);
#else
    // Other POSIX hosts retain the mapping check, bounded by the requested
    // input length rather than prefetching a fixed 1 MiB for every operation.
    uint64_t span = first_page;
    while (span < limit) {
        unsigned char residency = 0;
        if (mincore(reinterpret_cast<void*>(address + span), page, &residency) != 0) break;
        span += std::min(page, limit - span);
    }
    return std::min(span, limit);
#endif
#endif
}

static uint64_t vyx_rt_bounded_cstr_len(const char* s) {
    if (!s) return 0;
    constexpr uint64_t cap = 1ull << 20;
#ifdef _WIN32
    const uint64_t span = vyx_rt_readable_span(s, cap);
    const void* zero = std::memchr(s, 0, static_cast<size_t>(span));
    return zero ? static_cast<const char*>(zero) - s : span;
#else
    static const auto page = static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
    if (page == 0 || page > cap) {
        std::fputs("[vyx-rt] cannot determine memory page size\n", stderr);
        std::abort();
    }
    const uint64_t first = std::min(cap, page - reinterpret_cast<std::uintptr_t>(s) % page);
    if (const void* zero = std::memchr(s, 0, static_cast<size_t>(first)))
        return static_cast<const char*>(zero) - s;
    const uint64_t span = vyx_rt_readable_span(s, cap);
    if (span <= first) return span;
    const void* zero = std::memchr(s + first, 0, static_cast<size_t>(span - first));
    return zero ? static_cast<const char*>(zero) - s : span;
#endif
}

static uint64_t vyx_rt_clip_string_len(const char* s, uint64_t len) {
    return vyx_rt_readable_span(s, len);
}
