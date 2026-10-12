// Diagnostic allocator for the Windows x64 SDK compiler, never a release input.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

struct Allocation {
    unsigned char* base;
    size_t bytes;
    size_t page;
    size_t usable;
    bool live;
};
static std::unordered_map<void*, Allocation> allocations;

static void release_guarded(void* p) {
    if (!p) return;
    auto it = allocations.find(p);
    if (it == allocations.end() || !it->second.live) {
        std::fprintf(stderr, "frontend guard: invalid or repeated free %p\n", p);
        std::abort();
    }
    auto& a = it->second;
    a.live = false;
    DWORD previous;
    if (!VirtualProtect(a.base + a.page, a.usable, PAGE_NOACCESS, &previous))
        std::abort();
    // Keep the reservation until process exit, so stale pointers cannot become
    // valid again when another AST record is allocated at the same address.
}

static LONG CALLBACK diagnose(EXCEPTION_POINTERS* exception) {
    if (exception->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    auto fault = exception->ExceptionRecord->ExceptionInformation[1];
    for (const auto& pair : allocations) {
        const auto& a = pair.second;
        auto base = reinterpret_cast<uintptr_t>(a.base);
        if (fault >= base && fault < base + a.usable + 2 * a.page) {
            std::fprintf(stderr, "frontend guard: ptr=%p bytes=%zu live=%d offset=%lld\n",
                pair.first, a.bytes, a.live,
                static_cast<long long>(fault - reinterpret_cast<uintptr_t>(pair.first)));
        }
    }
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}
struct InstallGuard {
    InstallGuard() { AddVectoredExceptionHandler(1, diagnose); }
};
static InstallGuard install_guard;

extern "C" {
void vyx_rt_service_track(void*, void (*)(void*));
void vyx_rt_service_untrack(void*);
void vyx_runtime_free_crt(void*);

void* vyx_frontend_alloc(uint64_t bytes) {
    if (!bytes || bytes > SIZE_MAX - 65536) return nullptr;
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    size_t page = info.dwPageSize;
    size_t aligned = (static_cast<size_t>(bytes) + 15) & ~size_t(15);
    size_t usable = (aligned + page - 1) & ~(page - 1);
    auto base = static_cast<unsigned char*>(VirtualAlloc(nullptr,
        usable + 2 * page, MEM_RESERVE, PAGE_NOACCESS));
    if (!base) return nullptr;
    if (!VirtualAlloc(base + page, usable, MEM_COMMIT, PAGE_READWRITE)) {
        VirtualFree(base, 0, MEM_RELEASE);
        return nullptr;
    }
    void* p = base + page + usable - aligned;
    allocations.emplace(p, Allocation{base, static_cast<size_t>(bytes), page, usable, true});
    vyx_rt_service_track(p, release_guarded);
    return p;
}
void vyx_frontend_free(void* p) {
    if (!p) return;
    vyx_rt_service_untrack(p);
    release_guarded(p);
}
void vyx_runtime_free(void* p) {
    // HIR scratch maps also release ast_dup_c_string through runtime free.
    // Ordinary runtime allocations keep their original allocator and tracking.
    if (allocations.find(p) != allocations.end()) vyx_frontend_free(p);
    else vyx_runtime_free_crt(p);
}
}
