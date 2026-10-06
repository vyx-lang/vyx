/**
 *  Runtime helpers for bootstrap-generated IR (string predicates, etc.).
 *  Linked into vyx_codegen.dll so boot_a / clang links resolve symbols.
 */

#include <cstring>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstdarg>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

extern "C" void* vyx_rt_service_alloc(uint64_t size);
extern "C" void vyx_rt_service_free(void* ptr);
extern "C" int32_t vyx_rt_service_active();
extern "C" void vyx_rt_service_string_alias(void* base, void* text);

#pragma warning(push, 0)
#include <llvm/ADT/StringRef.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/DynamicLibrary.h>
#include <llvm/Support/SHA256.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/IPO/GlobalDCE.h>
#include <llvm/Transforms/IPO/Internalize.h>
#include <llvm/Transforms/IPO/StripDeadPrototypes.h>
#pragma warning(pop)

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <psapi.h>
#  include <malloc.h>
#  include <intrin.h>
#  ifdef VYX_CODEGEN_BUILD
#    define VYX_RT_ABI __declspec(dllexport)
#  else
#    define VYX_RT_ABI __declspec(dllimport)
#  endif
#else
#  include <cerrno>
#  include <csignal>
#  include <fcntl.h>
#  include <sys/file.h>
#  include <sys/mman.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
#  include <ucontext.h>
#  include <pthread.h>
#  define VYX_RT_ABI __attribute__((visibility("default")))
#endif

extern "C" VYX_RT_ABI int32_t vyx_bootstrap_link_ir_files(const char* inputs, const char* output) {
    if (!inputs || !output || !*output) return 1;
    llvm::LLVMContext context;
    std::unique_ptr<llvm::Module> merged;
    std::string rows(inputs);
    size_t start = 0;
    while (start < rows.size()) {
        const size_t end = rows.find('\n', start);
        std::string path = rows.substr(start, end == std::string::npos ? end : end - start);
        start = end == std::string::npos ? rows.size() : end + 1;
        if (!path.empty() && path.back() == '\r') path.pop_back();
        if (path.empty()) continue;
        llvm::SMDiagnostic diagnostic;
        auto unit = llvm::parseIRFile(path, diagnostic, context);
        if (!unit) {
            diagnostic.print("vyxc project IR", llvm::errs());
            return 1;
        }
        if (!merged) {
            merged = std::move(unit);
        } else {
            if (merged->getTargetTriple() != unit->getTargetTriple() ||
                merged->getDataLayoutStr() != unit->getDataLayoutStr()) {
                llvm::errs() << "vyxc project IR: incompatible target or data layout: " << path << "\n";
                return 1;
            }
            if (llvm::Linker::linkModules(*merged, std::move(unit))) return 1;
        }
    }
    if (!merged || llvm::verifyModule(*merged, &llvm::errs())) return 1;
    std::error_code error;
    llvm::raw_fd_ostream stream(output, error);
    if (error) {
        llvm::errs() << "vyxc project IR: " << output << ": " << error.message() << "\n";
        return 1;
    }
    merged->print(stream, nullptr);
    stream.flush();
    if (stream.has_error()) return 1;
    return 0;
}

#define VYX_POINTER_RT_ABI VYX_RT_ABI // pointer-handle ABI v3: module-owned release
#include "vyx_pointer_handle_rt.inc"

#ifdef _WIN32
extern "C" {
    extern int __argc;
    extern char** __argv;
    extern char** _environ;
}

#endif

extern "C" VYX_RT_ABI int64_t vyx_atomic_load_i64(const int64_t* value) {
    if (!value) return 0;
#if defined(_MSC_VER) && !defined(__clang__)
    std::atomic_ref<const int64_t> atomicValue(*value);
    return atomicValue.load(std::memory_order_seq_cst);
#else
    return __atomic_load_n(value, __ATOMIC_SEQ_CST);
#endif
}

extern "C" VYX_RT_ABI int64_t vyx_atomic_fetch_add_i64(int64_t* value, int64_t delta) {
    if (!value) return 0;
#if defined(_MSC_VER) && !defined(__clang__)
    std::atomic_ref<int64_t> atomicValue(*value);
    return atomicValue.fetch_add(delta, std::memory_order_seq_cst);
#else
    return __atomic_fetch_add(value, delta, __ATOMIC_SEQ_CST);
#endif
}

extern "C" VYX_RT_ABI int64_t vyx_atomic_compare_exchange_i64(int64_t* value,
                                                                 int64_t expected,
                                                                 int64_t desired) {
    if (!value) return 0;
    int64_t observed = expected;
#if defined(_MSC_VER) && !defined(__clang__)
    std::atomic_ref<int64_t> atomicValue(*value);
    atomicValue.compare_exchange_strong(observed, desired,
                                        std::memory_order_seq_cst,
                                        std::memory_order_seq_cst);
#else
    __atomic_compare_exchange_n(value, &observed, desired, false,
                                __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
#endif
    return observed;
}

// A Ref control block and payload share one allocation. The control block is
// kept at the allocation base so `free(rc)` is always valid while the payload
// starts at an independently aligned address after its fixed 24-byte header.
extern "C" VYX_RT_ABI void* vyx_ref_alloc_abi(int64_t payload_size,
                                                int64_t payload_alignment) {
    constexpr std::size_t kRefHeaderBytes = 24u;
    if (payload_size < 0 || payload_alignment <= 0) {
        return nullptr;
    }
    const auto requested_size = static_cast<std::uint64_t>(payload_size);
    const auto size = requested_size == 0u ? std::uint64_t{1u} : requested_size;
    const auto alignment = static_cast<std::uint64_t>(payload_alignment);
    if ((alignment & (alignment - 1u)) != 0u
        || size > std::numeric_limits<std::size_t>::max()
        || alignment > std::numeric_limits<std::size_t>::max()
        || size > std::numeric_limits<std::size_t>::max() - kRefHeaderBytes
        || alignment - 1u > std::numeric_limits<std::size_t>::max()
                                 - kRefHeaderBytes - static_cast<std::size_t>(size)) {
        return nullptr;
    }
    const auto total = kRefHeaderBytes + static_cast<std::size_t>(alignment - 1u)
        + static_cast<std::size_t>(size);
    auto* control = static_cast<unsigned char*>(std::malloc(total));
    if (!control) {
        return nullptr;
    }
    const auto payload_start = reinterpret_cast<std::uintptr_t>(control + kRefHeaderBytes);
    const auto payload_bits = (payload_start + alignment - 1u) & ~(alignment - 1u);
    auto* payload = reinterpret_cast<void*>(payload_bits);
    const int64_t one = 1;
    std::memcpy(control, &one, sizeof(one));
    std::memcpy(control + 8u, &one, sizeof(one));
    std::memcpy(control + 16u, &payload, sizeof(payload));
    return control;
}

extern "C" VYX_RT_ABI void vyx_ref_free_abi(void* control) {
    std::free(control);
}

// Portable over-aligned allocation ABI used by Box<T>. The original malloc
// base is stored immediately before the aligned payload so allocation and
// release remain a matched pair on every host CRT (including Windows, where
// `_aligned_malloc` must not be paired with `free`).
extern "C" VYX_RT_ABI void* vyx_aligned_alloc_abi(int64_t payload_size,
                                                    int64_t payload_alignment) {
    constexpr std::size_t kBaseSlotBytes = sizeof(void*);
    if (payload_size < 0 || payload_alignment <= 0) {
        return nullptr;
    }
    const auto requested_size = static_cast<std::uint64_t>(payload_size);
    const auto size = requested_size == 0u ? std::uint64_t{1u} : requested_size;
    const auto alignment = static_cast<std::uint64_t>(payload_alignment);
    if ((alignment & (alignment - 1u)) != 0u
        || size > std::numeric_limits<std::size_t>::max()
        || alignment > std::numeric_limits<std::size_t>::max()
        || size > std::numeric_limits<std::size_t>::max() - kBaseSlotBytes
        || alignment - 1u > std::numeric_limits<std::size_t>::max()
                                 - kBaseSlotBytes - static_cast<std::size_t>(size)) {
        return nullptr;
    }
    const auto total = kBaseSlotBytes + static_cast<std::size_t>(alignment - 1u)
        + static_cast<std::size_t>(size);
    auto* base = static_cast<unsigned char*>(std::malloc(total));
    if (!base) {
        return nullptr;
    }
    const auto payload_start = reinterpret_cast<std::uintptr_t>(base + kBaseSlotBytes);
    const auto payload_bits = (payload_start + alignment - 1u) & ~(alignment - 1u);
    auto* payload = reinterpret_cast<unsigned char*>(payload_bits);
    void* allocation_base = base;
    std::memcpy(payload - kBaseSlotBytes, &allocation_base, sizeof(allocation_base));
    return payload;
}

extern "C" VYX_RT_ABI void vyx_aligned_free_abi(void* payload) {
    if (!payload) {
        return;
    }
    void* allocation_base = nullptr;
    auto* bytes = static_cast<unsigned char*>(payload);
    std::memcpy(&allocation_base, bytes - sizeof(void*), sizeof(allocation_base));
    std::free(allocation_base);
}

static int32_t vyx_rt_process_argc = 0;
static char** vyx_rt_process_argv = nullptr;
static bool vyx_rt_process_args_set = false;

static std::unordered_map<const char*, uint64_t>& vyx_rt_string_lengths() {
    static std::unordered_map<const char*, uint64_t> lengths;
    return lengths;
}

static std::mutex& vyx_rt_string_lengths_mutex() {
    static std::mutex mutex;
    return mutex;
}

struct VyxRtStringLenCache {
    const char* ptr;
    uint64_t len;
};

struct VyxRtStringLenDirectSlot {
    const char* ptr;
    uint64_t len;
    // `tracked` means the global map owns an entry for this pointer. Most
    // generated strings use only this direct cache and need no map lookup on
    // release.
    bool tracked;
    uint32_t epoch;
};

static uint32_t& vyx_rt_string_len_epoch() {
    static thread_local uint32_t epoch = 1;
    return epoch;
}

static bool vyx_rt_direct_slot_holds(const VyxRtStringLenDirectSlot& slot, const char* s) {
    return slot.ptr == s && slot.epoch == vyx_rt_string_len_epoch();
}

static VyxRtStringLenDirectSlot vyx_rt_make_direct_slot(const char* s, uint64_t len, bool tracked) {
    return VyxRtStringLenDirectSlot{s, len, tracked, vyx_rt_string_len_epoch()};
}

static VyxRtStringLenDirectSlot vyx_rt_empty_direct_slot() {
    return VyxRtStringLenDirectSlot{nullptr, 0, false, 0};
}

struct VyxRtStringArenaChunk {
    char* begin;
    char* end;
};

struct VyxRtStringHeader {
    uint64_t magic;
    uint64_t len;
};

static constexpr uint64_t kVyxRtStringHeaderMagic = 0x5659585354524c45ull;

// Short-lived `str` results have a separate ownership tag and are returned to
// this thread-local pool. The ABI remains a normal NUL-terminated byte buffer;
// only allocation/release changes. Large strings retain their normal exact
// allocation and are reclaimed through the same release entry point.
struct VyxRtPooledStringNode {
    VyxRtPooledStringNode* next;
    uint64_t magic;
    uint32_t class_index;
    uint32_t reserved;
};

static constexpr uint64_t kVyxRtPooledStringMagic = 0x565958504f4f4c45ull;
static constexpr std::array<std::size_t, 5> kVyxRtPooledStringClasses = {
    16u, 32u, 64u, 128u, 256u
};
static constexpr uint32_t kVyxRtPooledStringRetainedPerClass = 64u;

struct VyxRtPooledStringState {
    std::array<VyxRtPooledStringNode*, kVyxRtPooledStringClasses.size()> free_lists{};
    std::array<uint32_t, kVyxRtPooledStringClasses.size()> counts{};

    ~VyxRtPooledStringState() {
        for (auto* head : free_lists) {
            while (head) {
                auto* next = head->next;
                std::free(head);
                head = next;
            }
        }
    }
};

static VyxRtPooledStringState& vyx_rt_pooled_string_state() {
    static thread_local VyxRtPooledStringState state;
    return state;
}

static std::atomic<uint64_t> g_vyx_alloc_abi_n{0};
static std::atomic<uint64_t> g_vyx_intern_hit{0};
static std::atomic<uint64_t> g_vyx_intern_miss{0};
#ifdef _WIN32
static thread_local std::unordered_map<const void*, uint64_t> g_vyx_alloc_abi_ra;
#endif

// Per-call-site allocation accounting is a diagnostic, not part of the
// allocator. It previously ran unconditionally on Windows for every
// `vyx_string_alloc_abi` call, which put an unordered_map lookup/insert plus an
// atomic read-modify-write on the hottest path in the whole compiler. Make it
// opt-in; the default build pays nothing.
static bool vyx_rt_alloc_abi_trace_enabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("VYX_RT_ALLOC_ABI_TRACE");
        return v != nullptr && v[0] != '\0' && v[0] != '0';
    }();
    return enabled;
}

static void vyx_rt_dump_alloc_abi_sites() {
#ifdef _WIN32
    HMODULE boot = GetModuleHandleW(nullptr);
    HMODULE dll = GetModuleHandleW(L"vyx_compiler_backend.dll");
    MODULEINFO boot_mi{};
    MODULEINFO dll_mi{};
    if (boot) {
        GetModuleInformation(GetCurrentProcess(), boot, &boot_mi, sizeof(boot_mi));
    }
    if (dll) {
        GetModuleInformation(GetCurrentProcess(), dll, &dll_mi, sizeof(dll_mi));
    }
    std::vector<std::pair<uint64_t, const void*>> ranked;
    ranked.reserve(g_vyx_alloc_abi_ra.size());
    for (const auto& kv : g_vyx_alloc_abi_ra) {
        ranked.emplace_back(kv.second, kv.first);
    }
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    const std::size_t n = std::min<std::size_t>(ranked.size(), 12u);
    for (std::size_t i = 0; i < n; ++i) {
        const auto* ra = ranked[i].second;
        const char* mod = "?";
        std::uintptr_t off = reinterpret_cast<std::uintptr_t>(ra);
        const auto* ra_c = static_cast<const char*>(ra);
        if (boot && boot_mi.lpBaseOfDll) {
            const auto* base = static_cast<const char*>(boot_mi.lpBaseOfDll);
            if (ra_c >= base && ra_c < base + boot_mi.SizeOfImage) {
                mod = "boot";
                off = static_cast<std::uintptr_t>(ra_c - base);
            }
        }
        if (dll && dll_mi.lpBaseOfDll) {
            const auto* base = static_cast<const char*>(dll_mi.lpBaseOfDll);
            if (ra_c >= base && ra_c < base + dll_mi.SizeOfImage) {
                mod = "dll";
                off = static_cast<std::uintptr_t>(ra_c - base);
            }
        }
        std::fprintf(stderr,
                     "[vyx-rt] alloc_abi site %s+0x%llx n=%llu\n",
                     mod,
                     static_cast<unsigned long long>(off),
                     static_cast<unsigned long long>(ranked[i].first));
    }
#else
    (void)0;
#endif
}

#ifdef _WIN32
static SRWLOCK g_vyx_intern_arena_lock = SRWLOCK_INIT;
#else
static pthread_mutex_t g_vyx_intern_arena_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

static void vyx_rt_intern_arena_lock() {
#ifdef _WIN32
    AcquireSRWLockExclusive(&g_vyx_intern_arena_lock);
#else
    pthread_mutex_lock(&g_vyx_intern_arena_lock);
#endif
}

static void vyx_rt_intern_arena_unlock() {
#ifdef _WIN32
    ReleaseSRWLockExclusive(&g_vyx_intern_arena_lock);
#else
    pthread_mutex_unlock(&g_vyx_intern_arena_lock);
#endif
}

static void* vyx_rt_intern_arena_alloc(std::size_t bytes) {
    static char* cur = nullptr;
    static std::size_t remaining = 0;
    static std::vector<char*> chunks;
    static uint64_t total_bytes = 0;
    constexpr std::size_t kChunk = 32u * 1024u * 1024u;
    if (bytes == 0u) {
        bytes = 1u;
    }
    if (bytes > std::numeric_limits<std::size_t>::max() - 7u) {
        return nullptr;
    }
    const std::size_t aligned = (bytes + 7u) & ~std::size_t{7u};
    vyx_rt_intern_arena_lock();
    if (aligned > remaining) {
        const std::size_t chunk_size = std::max(kChunk, aligned);
#ifdef _WIN32
        auto* chunk = static_cast<char*>(VirtualAlloc(nullptr,
                                                      chunk_size,
                                                      MEM_COMMIT | MEM_RESERVE,
                                                      PAGE_READWRITE));
#else
        auto* chunk = static_cast<char*>(std::malloc(chunk_size));
#endif
        if (!chunk) {
            std::fprintf(stderr,
                         "[vyx-rt] intern arena chunk malloc failed size=%zu total_mb=%llu chunks=%zu alloc_abi=%llu intern_hit=%llu intern_miss=%llu\n",
                         chunk_size,
                         static_cast<unsigned long long>(total_bytes / (1024ull * 1024ull)),
                         chunks.size(),
                         static_cast<unsigned long long>(g_vyx_alloc_abi_n.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(g_vyx_intern_hit.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(g_vyx_intern_miss.load(std::memory_order_relaxed)));
            vyx_rt_intern_arena_unlock();
            return nullptr;
        }
        chunks.push_back(chunk);
        total_bytes += static_cast<uint64_t>(chunk_size);
        // Same rule as the alloc-ABI accounting above: a successful run must
        // keep stderr clean. This line used to fire on every process that
        // touched the intern arena, which poisoned stderr for hosts that treat
        // native stderr as an error (PowerShell 5.1 with
        // $ErrorActionPreference = 'Stop' fails the whole script). Opt in via
        // VYX_RT_ALLOC_ABI_TRACE when the arena growth curve is what you want.
        if ((chunks.size() == 1u || (chunks.size() % 8u) == 0u)
            && vyx_rt_alloc_abi_trace_enabled()) {
            std::fprintf(stderr,
                         "[vyx-rt] intern chunk=%zu total_mb=%llu alloc_abi=%llu intern_hit=%llu intern_miss=%llu\n",
                         chunks.size(),
                         static_cast<unsigned long long>(total_bytes / (1024ull * 1024ull)),
                         static_cast<unsigned long long>(g_vyx_alloc_abi_n.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(g_vyx_intern_hit.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(g_vyx_intern_miss.load(std::memory_order_relaxed)));
            vyx_rt_dump_alloc_abi_sites();
        }
        const char* abort_chunks = std::getenv("VYX_INTERN_ABORT_CHUNKS");
        if (abort_chunks && abort_chunks[0] != '\0') {
            const auto limit = static_cast<std::size_t>(std::strtoull(abort_chunks, nullptr, 10));
            if (limit > 0u && chunks.size() >= limit) {
                std::fprintf(stderr, "[vyx-rt] intern abort after chunks=%zu\n", chunks.size());
                vyx_rt_dump_alloc_abi_sites();
                std::abort();
            }
        }
        cur = chunk;
        remaining = chunk_size;
    }
    void* out = cur;
    cur += aligned;
    remaining -= aligned;
    vyx_rt_intern_arena_unlock();
    return out;
}

struct VyxInternedStr {
    const char* ptr = nullptr;
    uint64_t len = 0;
};

struct VyxInternKey {
    const char* ptr = nullptr;
    uint64_t len = 0;
};

struct VyxInternKeyHash {
    std::size_t operator()(VyxInternKey k) const noexcept {
        return std::hash<std::string_view>{}(
            std::string_view(k.ptr ? k.ptr : "", static_cast<std::size_t>(k.len)));
    }
};

struct VyxInternKeyEq {
    bool operator()(VyxInternKey a, VyxInternKey b) const noexcept {
        if (a.len != b.len) {
            return false;
        }
        if (a.len == 0) {
            return true;
        }
        if (!a.ptr || !b.ptr) {
            return a.ptr == b.ptr;
        }
        return std::memcmp(a.ptr, b.ptr, static_cast<std::size_t>(a.len)) == 0;
    }
};

#ifdef _WIN32
static SRWLOCK g_vyx_intern_lock = SRWLOCK_INIT;
#else
static pthread_rwlock_t g_vyx_intern_lock = PTHREAD_RWLOCK_INITIALIZER;
#endif
static std::unordered_map<VyxInternKey, VyxInternedStr, VyxInternKeyHash, VyxInternKeyEq>
    g_vyx_intern_by_content;
static std::unordered_map<const void*, VyxInternedStr> g_vyx_type_text_by_ty;

static void vyx_intern_lock_shared() {
#ifdef _WIN32
    AcquireSRWLockShared(&g_vyx_intern_lock);
#else
    pthread_rwlock_rdlock(&g_vyx_intern_lock);
#endif
}

static void vyx_intern_unlock_shared() {
#ifdef _WIN32
    ReleaseSRWLockShared(&g_vyx_intern_lock);
#else
    pthread_rwlock_unlock(&g_vyx_intern_lock);
#endif
}

static void vyx_intern_lock_excl() {
#ifdef _WIN32
    AcquireSRWLockExclusive(&g_vyx_intern_lock);
#else
    pthread_rwlock_wrlock(&g_vyx_intern_lock);
#endif
}

static void vyx_intern_unlock_excl() {
#ifdef _WIN32
    ReleaseSRWLockExclusive(&g_vyx_intern_lock);
#else
    pthread_rwlock_unlock(&g_vyx_intern_lock);
#endif
}

static void vyx_rt_note_string_len_cached(const char* s, uint64_t len);

static char* vyx_rt_dup_bytes(const char* s, uint64_t len);

static VyxInternedStr vyx_rt_intern_bytes(const char* s, uint64_t len) {
    if (vyx_rt_service_active()) return VyxInternedStr{vyx_rt_dup_bytes(s, len), len};
    VyxInternedStr out;
    VyxInternKey probe{s, len};
    vyx_intern_lock_shared();
    auto it = g_vyx_intern_by_content.find(probe);
    if (it != g_vyx_intern_by_content.end()) {
        g_vyx_intern_hit.fetch_add(1u, std::memory_order_relaxed);
        out = it->second;
        vyx_intern_unlock_shared();
        vyx_rt_note_string_len_cached(out.ptr, out.len);
        return out;
    }
    vyx_intern_unlock_shared();
    vyx_intern_lock_excl();
    it = g_vyx_intern_by_content.find(probe);
    if (it != g_vyx_intern_by_content.end()) {
        g_vyx_intern_hit.fetch_add(1u, std::memory_order_relaxed);
        out = it->second;
    } else {
        g_vyx_intern_miss.fetch_add(1u, std::memory_order_relaxed);
        auto* mem = static_cast<char*>(vyx_rt_intern_arena_alloc(static_cast<std::size_t>(len) + 1u));
        if (!mem) {
            std::fprintf(stderr,
                         "[vyx-rt] intern copy exhausted len=%llu\n",
                         static_cast<unsigned long long>(len));
            std::abort();
        }
        if (len > 0 && s) {
            std::memcpy(mem, s, static_cast<std::size_t>(len));
        }
        mem[len] = '\0';
        out = VyxInternedStr{mem, len};
        g_vyx_intern_by_content.emplace(VyxInternKey{mem, len}, out);
    }
    vyx_intern_unlock_excl();
    vyx_rt_note_string_len_cached(out.ptr, out.len);
    return out;
}

static uint64_t vyx_intern_norm_len(const char* s, int64_t len) {
    if (len >= 0) {
        return static_cast<uint64_t>(len);
    }
    if (!s) {
        return 0;
    }
    return static_cast<uint64_t>(std::strlen(s));
}

static const char* vyx_rt_intern_concat_parts(const char* const* parts,
                                              const uint64_t* lens,
                                              std::size_t count) {
    uint64_t total = 0;
    for (std::size_t i = 0; i < count; ++i) {
        total += lens[i];
    }
    char stack[512];
    char* buf = stack;
    bool heap = false;
    if (total + 1u > sizeof(stack)) {
        buf = static_cast<char*>(std::malloc(static_cast<std::size_t>(total) + 1u));
        if (!buf) {
            std::fprintf(stderr, "[vyx-rt] intern concat staging malloc failed\n");
            std::abort();
        }
        heap = true;
    }
    char* dst = buf;
    for (std::size_t i = 0; i < count; ++i) {
        if (parts[i] && lens[i] > 0) {
            std::memcpy(dst, parts[i], static_cast<std::size_t>(lens[i]));
            dst += lens[i];
        }
    }
    const auto interned = vyx_rt_intern_bytes(buf, total);
    if (heap) {
        std::free(buf);
    }
    return interned.ptr;
}

extern "C" VYX_RT_ABI const char* vyx_intern_bytes(const char* s, int64_t len) {
    const auto n = vyx_intern_norm_len(s, len);
    return vyx_rt_intern_bytes(s, n).ptr;
}

extern "C" VYX_RT_ABI const char* vyx_intern_concat2(const char* a, int64_t alen,
                                                     const char* b, int64_t blen) {
    const char* parts[2] = {a, b};
    const uint64_t lens[2] = {vyx_intern_norm_len(a, alen), vyx_intern_norm_len(b, blen)};
    return vyx_rt_intern_concat_parts(parts, lens, 2);
}

extern "C" VYX_RT_ABI const char* vyx_intern_concat3(const char* a, int64_t alen,
                                                     const char* b, int64_t blen,
                                                     const char* c, int64_t clen) {
    const char* parts[3] = {a, b, c};
    const uint64_t lens[3] = {
        vyx_intern_norm_len(a, alen),
        vyx_intern_norm_len(b, blen),
        vyx_intern_norm_len(c, clen),
    };
    return vyx_rt_intern_concat_parts(parts, lens, 3);
}

extern "C" VYX_RT_ABI const char* vyx_intern_concat4(const char* a, int64_t alen,
                                                     const char* b, int64_t blen,
                                                     const char* c, int64_t clen,
                                                     const char* d, int64_t dlen) {
    const char* parts[4] = {a, b, c, d};
    const uint64_t lens[4] = {
        vyx_intern_norm_len(a, alen),
        vyx_intern_norm_len(b, blen),
        vyx_intern_norm_len(c, clen),
        vyx_intern_norm_len(d, dlen),
    };
    return vyx_rt_intern_concat_parts(parts, lens, 4);
}

extern "C" VYX_RT_ABI const char* vyx_type_text_cache_get(const void* ty) {
    if (!ty) {
        return nullptr;
    }
    vyx_intern_lock_shared();
    auto it = g_vyx_type_text_by_ty.find(ty);
    if (it == g_vyx_type_text_by_ty.end()) {
        vyx_intern_unlock_shared();
        return nullptr;
    }
    const char* ptr = it->second.ptr;
    vyx_intern_unlock_shared();
    return ptr;
}

extern "C" VYX_RT_ABI void vyx_type_text_cache_put(const void* ty, const char* s, int64_t len) {
    if (!ty || !s || len < 0) {
        return;
    }
    const auto interned = vyx_rt_intern_bytes(s, static_cast<uint64_t>(len));
    vyx_intern_lock_excl();
    g_vyx_type_text_by_ty[ty] = interned;
    vyx_intern_unlock_excl();
}

extern "C" VYX_RT_ABI void vyx_type_text_cache_clear() {
    vyx_intern_lock_excl();
    g_vyx_type_text_by_ty.clear();
    vyx_intern_unlock_excl();
}

extern "C" VYX_RT_ABI char* vyx_string_alloc_abi(int64_t capacity) {
    if (vyx_rt_alloc_abi_trace_enabled()) {
        g_vyx_alloc_abi_n.fetch_add(1u, std::memory_order_relaxed);
#ifdef _WIN32
        g_vyx_alloc_abi_ra[_ReturnAddress()] += 1u;
#endif
    }
    if (capacity < 1) {
        capacity = 1;
    }
    if (static_cast<uint64_t>(capacity)
        > std::numeric_limits<std::size_t>::max() - sizeof(VyxRtPooledStringNode)) {
        std::fprintf(stderr, "[vyx-rt] pooled string request too large\n");
        std::abort();
    }
    const auto requested = static_cast<std::size_t>(capacity);
    uint32_t class_index = static_cast<uint32_t>(kVyxRtPooledStringClasses.size());
    for (uint32_t i = 0; i < kVyxRtPooledStringClasses.size(); ++i) {
        if (requested <= kVyxRtPooledStringClasses[i]) {
            class_index = i;
            break;
        }
    }
    const auto payload = class_index < kVyxRtPooledStringClasses.size()
        ? kVyxRtPooledStringClasses[class_index]
        : requested;
    const auto bytes = sizeof(VyxRtPooledStringNode) + payload;
    if (vyx_rt_service_active()) { class_index = static_cast<uint32_t>(kVyxRtPooledStringClasses.size()); }
    auto& state = vyx_rt_pooled_string_state();
    if (class_index < kVyxRtPooledStringClasses.size()) {
        auto*& head = state.free_lists[class_index];
        if (head) {
            auto* recycled = head;
            head = recycled->next;
            if (state.counts[class_index] > 0u) {
                state.counts[class_index] -= 1u;
            }
            recycled->next = nullptr;
            auto* data = reinterpret_cast<char*>(recycled + 1);
            data[0] = '\0';
            return data;
        }
    }
    auto* node = static_cast<VyxRtPooledStringNode*>(vyx_rt_service_alloc(bytes));
    if (!node) {
        std::fprintf(stderr,
                     "[vyx-rt] pooled string malloc failed bytes=%zu\n",
                     bytes);
        std::abort();
    }
    node->next = nullptr;
    node->magic = kVyxRtPooledStringMagic;
    node->class_index = class_index;
    node->reserved = 0;
    auto* data = reinterpret_cast<char*>(node + 1);
    vyx_rt_service_string_alias(node, data);
    data[0] = '\0';
    return data;
}

extern "C" VYX_RT_ABI void vyx_free_pooled_string(char* data) {
    if (!data) {
        return;
    }
    auto* node = reinterpret_cast<VyxRtPooledStringNode*>(data) - 1;
    if (node->magic != kVyxRtPooledStringMagic) {
        std::abort();
    }
    auto& state = vyx_rt_pooled_string_state();
    if (node->class_index < kVyxRtPooledStringClasses.size()
        && state.counts[node->class_index] < kVyxRtPooledStringRetainedPerClass) {
        node->next = state.free_lists[node->class_index];
        state.free_lists[node->class_index] = node;
        state.counts[node->class_index] += 1u;
        return;
    }
    vyx_rt_service_free(node);
}

static VyxRtStringLenCache& vyx_rt_string_len_cache() {
    static thread_local VyxRtStringLenCache cache{nullptr, 0};
    return cache;
}

static constexpr std::size_t kVyxRtStringLenDirectSlots = 1u << 16;

static std::array<VyxRtStringLenDirectSlot, kVyxRtStringLenDirectSlots>& vyx_rt_string_len_direct_cache() {
    static thread_local std::array<VyxRtStringLenDirectSlot, kVyxRtStringLenDirectSlots> cache{};
    return cache;
}

static std::size_t vyx_rt_string_len_direct_slot(const char* s) {
    auto bits = reinterpret_cast<std::uintptr_t>(s);
    bits ^= bits >> 16;
    bits ^= bits >> 32;
    return static_cast<std::size_t>(bits) & (kVyxRtStringLenDirectSlots - 1u);
}

struct VyxRtStringArena {
    std::vector<char*> chunks;
    std::vector<VyxRtStringArenaChunk> ranges;
    char* cur = nullptr;
    std::size_t remaining = 0;
    std::size_t active_chunk = 0;
};

struct VyxRtStringArenaMark {
    char* cur;
    std::size_t remaining;
    std::size_t active_chunk;
};

static VyxRtStringArena& vyx_rt_string_arena() {
    static thread_local VyxRtStringArena arena;
    return arena;
}

static void vyx_rt_clear_string_len_thread_cache() {
    auto& cache = vyx_rt_string_len_cache();
    cache.ptr = nullptr;
    cache.len = 0;
    // Bump epoch so the 64K direct cache is O(1) invalid. Filling every
    // slot on each arena rewind was ~25ms per function in streaming lower.
    auto& epoch = vyx_rt_string_len_epoch();
    epoch += 1u;
    if (epoch == 0u) {
        auto& direct = vyx_rt_string_len_direct_cache();
        direct.fill(vyx_rt_empty_direct_slot());
        epoch = 1u;
    }
}

static void vyx_rt_forget_string_lengths_in_range(const char* begin, const char* end) {
    if (!begin || !end || begin >= end) {
        return;
    }

    auto& cache = vyx_rt_string_len_cache();
    if (cache.ptr >= begin && cache.ptr < end) {
        cache.ptr = nullptr;
        cache.len = 0;
    }

    // Arena strings are untracked (direct cache / epoch only). Walking the
    // process-wide length map and the 64K direct table on every rewind made
    // lower_function's release_temp_strings the 25ms/fn floor.
}

enum class BootstrapRtProfId : std::size_t {
    StringLen,
    StartsWith,
    EndsWith,
    Contains,
    StringEquals,
    StringCompare,
    StringEqualsLen,
    CharAt,
    Substring,
    Trim,
    Clone,
    ToUpper,
    ToLower,
    Replace,
    Concat,
    StrEquals,
    StrLength,
    IntToString,
    BoolToString,
    Print,
    ArgCount,
    GetArg,
    ReadFile,
    FileSize,
    FromCstr,
    FromCstrLen,
    FromCstrViewLen,
    ToRawptr,
    PtrOffset,
    PtrReadI32,
    PtrReadU8,
    PtrReadI64,
    PtrWriteI32,
    PtrWriteI64,
    Count
};

struct BootstrapRtProfRec {
    const char* name;
    uint64_t calls;
};

static std::array<BootstrapRtProfRec, static_cast<std::size_t>(BootstrapRtProfId::Count)>& bootstrapRtProfileRecords() {
    static std::array<BootstrapRtProfRec, static_cast<std::size_t>(BootstrapRtProfId::Count)> records{{
        {"vyx_string_len", 0},
        {"vyx_string_starts_with", 0},
        {"vyx_string_ends_with", 0},
        {"vyx_string_contains", 0},
        {"vyx_string_equals", 0},
        {"vyx_string_compare", 0},
        {"vyx_string_equals_len", 0},
        {"vyx_string_char_at", 0},
        {"vyx_string_substring", 0},
        {"vyx_string_trim", 0},
        {"vyx_string_clone", 0},
        {"vyx_string_to_upper", 0},
        {"vyx_string_to_lower", 0},
        {"vyx_string_replace", 0},
        {"vyx_string_concat", 0},
        {"str_equals", 0},
        {"str_length", 0},
        {"int_to_string", 0},
        {"bool_to_string", 0},
        {"print", 0},
        {"argCount", 0},
        {"getArg", 0},
        {"readFile", 0},
        {"fileSize", 0},
        {"from_cstr", 0},
        {"from_cstr_len", 0},
        {"from_cstr_view_len", 0},
        {"to_rawptr", 0},
        {"ptr_offset", 0},
        {"ptr_read_i32", 0},
        {"ptr_read_u8", 0},
        {"ptr_read_i64", 0},
        {"ptr_write_i32", 0},
        {"ptr_write_i64", 0},
    }};
    return records;
}

static int bootstrapRtProfileInitEnabled() {
    const char* raw = std::getenv("VYX_RT_PROFILE");
    int enabled = (raw && raw[0] != '\0') ? 1 : 0;
    if (enabled) {
        std::atexit([] {
            auto& records = bootstrapRtProfileRecords();
            std::fprintf(stderr, "[vyx-bootstrap-rt-prof] calls\n");
            for (const auto& rec : records) {
                if (rec.calls == 0) continue;
                std::fprintf(stderr, "[vyx-bootstrap-rt-prof] %-32s %llu\n",
                             rec.name,
                             static_cast<unsigned long long>(rec.calls));
            }
        });
    }
    return enabled;
}

static const int bootstrapRtProfileEnabledValue = bootstrapRtProfileInitEnabled();

#define VYX_BOOTSTRAP_RT_PROF(ID)                                                   \
    do {                                                                            \
        if (bootstrapRtProfileEnabledValue) {                                       \
            bootstrapRtProfileRecords()[static_cast<std::size_t>(BootstrapRtProfId::ID)].calls += 1; \
        }                                                                           \
    } while (false)

static int bootstrapRtMemProfileEnabled() {
    const char* raw = std::getenv("VYX_RT_MEM_PROFILE");
    return (raw && raw[0] != '\0') ? 1 : 0;
}

static const int bootstrapRtMemProfileEnabledValue = bootstrapRtMemProfileEnabled();
static std::atomic<uint64_t> bootstrapRtArenaBytes{0};
static std::atomic<uint64_t> bootstrapRtArenaNextReport{64ull * 1024ull * 1024ull};
static std::atomic<uint64_t> bootstrapRtAstDupBytes{0};
static std::atomic<uint64_t> bootstrapRtAstDupCalls{0};
static std::atomic<uint64_t> bootstrapRtAstDupNextReport{64ull * 1024ull * 1024ull};
static std::atomic<uint64_t> bootstrapRtAstNodeBytes{0};
static std::atomic<uint64_t> bootstrapRtAstNodeCalls{0};
static std::atomic<uint64_t> bootstrapRtRawDupBytesTotal{0};
static std::atomic<uint64_t> bootstrapRtRawDupBytesLive{0};
static std::atomic<uint64_t> bootstrapRtRawDupCalls{0};
static std::atomic<uint64_t> bootstrapRtStringArenaMarksTotal{0};
static std::atomic<uint64_t> bootstrapRtStringArenaMarksLive{0};

extern "C" VYX_RT_ABI void vyx_rt_ast_dup_profile(int64_t bytes) {
    if (bytes <= 0) {
        return;
    }
    const uint64_t n = static_cast<uint64_t>(bytes);
    const uint64_t total = bootstrapRtAstDupBytes.fetch_add(n, std::memory_order_relaxed) + n;
    const uint64_t calls = bootstrapRtAstDupCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!bootstrapRtMemProfileEnabledValue) {
        return;
    }
    uint64_t next = bootstrapRtAstDupNextReport.load(std::memory_order_relaxed);
    while (total >= next) {
        if (bootstrapRtAstDupNextReport.compare_exchange_weak(
                next,
                next + 64ull * 1024ull * 1024ull,
                std::memory_order_relaxed)) {
            std::fprintf(stderr,
                         "[vyx-rt-mem] ast_dup=%lluMB calls=%llu last=%llu\n",
                         static_cast<unsigned long long>(total / (1024ull * 1024ull)),
                         static_cast<unsigned long long>(calls),
                         static_cast<unsigned long long>(n));
        }
    }
}

extern "C" VYX_RT_ABI void vyx_rt_ast_node_alloc_profile(int64_t bytes) {
    if (bytes <= 0) {
        return;
    }
    bootstrapRtAstNodeBytes.fetch_add(static_cast<uint64_t>(bytes), std::memory_order_relaxed);
    bootstrapRtAstNodeCalls.fetch_add(1, std::memory_order_relaxed);
}

static uint64_t vyx_rt_known_string_len(const char* s);
static char* vyx_rt_empty_cstr();

static void vyx_rt_note_string_len_cached(const char* s, uint64_t len) {
    if (!s) {
        return;
    }

    auto& cache = vyx_rt_string_len_cache();
    if (cache.ptr == s && cache.len == len) {
        return;
    }
    cache.ptr = s;
    cache.len = len;

    auto& direct = vyx_rt_string_len_direct_cache();
    const auto slot = vyx_rt_string_len_direct_slot(s);
    if (vyx_rt_direct_slot_holds(direct[slot], s) && direct[slot].len == len) {
        return;
    }
    // Direct slots collide. Spill the live untracked view so later
    // charAt/concat still see the real length instead of scanning.
    if (vyx_rt_direct_slot_holds(direct[slot], direct[slot].ptr)
        && direct[slot].ptr != nullptr && direct[slot].ptr != s && !direct[slot].tracked) {
        const char* evicted = direct[slot].ptr;
        const uint64_t evicted_len = direct[slot].len;
        std::lock_guard<std::mutex> lock(vyx_rt_string_lengths_mutex());
        auto& lengths = vyx_rt_string_lengths();
        auto it = lengths.find(evicted);
        if (it == lengths.end()) {
            lengths.emplace(evicted, evicted_len);
        } else if (it->second != evicted_len) {
            it->second = evicted_len;
        }
    }
    direct[slot] = vyx_rt_make_direct_slot(s, len, false);
}

static void vyx_rt_note_string_len(const char* s, uint64_t len) {
    if (!s) {
        return;
    }

    vyx_rt_note_string_len_cached(s, len);

    {
        std::lock_guard<std::mutex> lock(vyx_rt_string_lengths_mutex());
        auto& lengths = vyx_rt_string_lengths();
        auto it = lengths.find(s);
        if (it == lengths.end()) {
            lengths.emplace(s, len);
        } else if (it->second != len) {
            it->second = len;
        }
    }
    auto& direct = vyx_rt_string_len_direct_cache();
    direct[vyx_rt_string_len_direct_slot(s)] = vyx_rt_make_direct_slot(s, len, true);
}

static bool vyx_rt_update_shared_string_len(const char* s, uint64_t len) {
    if (!s) {
        return false;
    }
    std::lock_guard<std::mutex> lock(vyx_rt_string_lengths_mutex());
    auto& lengths = vyx_rt_string_lengths();
    auto it = lengths.find(s);
    if (it == lengths.end()) {
        return false;
    }
    it->second = len;
    return true;
}

static uint64_t vyx_rt_forget_string_len(const char* s) {
    if (!s) {
        return 0;
    }

    auto& cache = vyx_rt_string_len_cache();
    if (cache.ptr == s) {
        cache.ptr = nullptr;
        cache.len = 0;
    }

    auto& direct = vyx_rt_string_len_direct_cache();
    const auto slot = vyx_rt_string_len_direct_slot(s);
    const bool known_untracked = vyx_rt_direct_slot_holds(direct[slot], s) && !direct[slot].tracked;
    if (vyx_rt_direct_slot_holds(direct[slot], s)) {
        direct[slot] = vyx_rt_empty_direct_slot();
    }
    // `note_string_len_cached` records the normal generated-string path. It
    // has no global entry, so releasing it must not acquire the map mutex.
    if (known_untracked) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(vyx_rt_string_lengths_mutex());
    auto& lengths = vyx_rt_string_lengths();
    const auto it = lengths.find(s);
    if (it == lengths.end()) {
        return 0;
    }
    const uint64_t bytes = it->second + 1u;
    lengths.erase(it);
    return bytes;
}

static bool vyx_rt_arena_string_len(const char* s, uint64_t& out) {
    if (!s) {
        return false;
    }
    auto& arena = vyx_rt_string_arena();
    const char* p = s;
    for (const auto& range : arena.ranges) {
        if (p <= range.begin + static_cast<std::ptrdiff_t>(sizeof(VyxRtStringHeader)) ||
            p >= range.end) {
            continue;
        }
        auto* header = reinterpret_cast<const VyxRtStringHeader*>(p - sizeof(VyxRtStringHeader));
        if (header->magic == kVyxRtStringHeaderMagic) {
            out = header->len;
            return true;
        }
    }
    return false;
}

// Seed-compiled `.charAt` / `.substring` / concat only pass a `char*`.
// Many compiler strings are length-bounded views (no trailing NUL).
// Unbounded `strlen`/`strstr` into those views is the 0xC0000005.
static uint64_t vyx_rt_readable_span(const char* s) {
    if (!s) {
        return 0;
    }
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(s, &mbi, sizeof(mbi)) == 0) {
        return 0;
    }
    if (mbi.State != MEM_COMMIT) {
        return 0;
    }
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) {
        return 0;
    }
    const DWORD prot = mbi.Protect & 0xff;
    const bool readable =
        prot == PAGE_READONLY || prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
        prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE ||
        prot == PAGE_EXECUTE_WRITECOPY;
    if (!readable) {
        return 0;
    }
    const auto* begin = static_cast<const char*>(mbi.BaseAddress);
    const auto* end = begin + mbi.RegionSize;
    if (s < begin || s >= end) {
        return 0;
    }
    return static_cast<uint64_t>(end - s);
#else
    const long page_long = sysconf(_SC_PAGESIZE);
    const std::size_t page =
        page_long > 0 ? static_cast<std::size_t>(page_long) : static_cast<std::size_t>(4096);
    const auto addr = reinterpret_cast<std::uintptr_t>(s);
    const auto page_end = (addr + page) & ~(static_cast<std::uintptr_t>(page) - 1u);
    uint64_t span = static_cast<uint64_t>(page_end - addr);
    constexpr uint64_t kCap = 1ull << 20;
    auto* p = reinterpret_cast<unsigned char*>(page_end);
    while (span < kCap) {
        char vec = 0;
        if (mincore(p, page, reinterpret_cast<unsigned char*>(&vec)) != 0) {
            break;
        }
        span += static_cast<uint64_t>(page);
        p += page;
    }
    return span > kCap ? kCap : span;
#endif
}

static uint64_t vyx_rt_bounded_cstr_len(const char* s) {
    constexpr uint64_t kScanCap = 1ull << 20;
    uint64_t maxn = vyx_rt_readable_span(s);
    if (maxn > kScanCap) {
        maxn = kScanCap;
    }
    for (uint64_t i = 0; i < maxn; ++i) {
        if (s[i] == '\0') {
            return i;
        }
    }
    return maxn;
}

static uint64_t vyx_rt_clip_string_len(const char* s, uint64_t len) {
    const uint64_t span = vyx_rt_readable_span(s);
    return len < span ? len : span;
}

#ifdef _WIN32
static LONG CALLBACK vyx_rt_av_veh(PEXCEPTION_POINTERS info) {
    if (!info || !info->ExceptionRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const auto rip = reinterpret_cast<void*>(info->ContextRecord->Rip);
    HMODULE mod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                           | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(rip),
                       &mod);
    wchar_t mod_name[MAX_PATH] = {};
    if (mod) {
        GetModuleFileNameW(mod, mod_name, MAX_PATH);
    }
    void* frames[8] = {};
    const USHORT n = CaptureStackBackTrace(0, 8, frames, nullptr);
    const auto rva = (mod != nullptr)
        ? static_cast<unsigned long long>(
              reinterpret_cast<std::uintptr_t>(rip) - reinterpret_cast<std::uintptr_t>(mod))
        : 0ull;
    std::fprintf(stderr,
                 "[vyx-av] access=%llu addr=%p rip=%p rva=0x%llx module=%ls\n",
                 static_cast<unsigned long long>(info->ExceptionRecord->ExceptionInformation[0]),
                 reinterpret_cast<void*>(info->ExceptionRecord->ExceptionInformation[1]),
                 rip,
                 rva,
                 mod_name);
    for (USHORT i = 0; i < n; ++i) {
        std::fprintf(stderr, "[vyx-av]   #%u %p\n", i, frames[i]);
    }
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

namespace {
struct VyxRtAvVehInstall {
    VyxRtAvVehInstall() {
        AddVectoredExceptionHandler(1, vyx_rt_av_veh);
    }
};
static VyxRtAvVehInstall g_vyx_rt_av_veh_install;
} // namespace
#endif

static uint64_t vyx_rt_known_string_len(const char* s) {
    if (!s) {
        return 0;
    }

    auto& cache = vyx_rt_string_len_cache();
    if (cache.ptr == s) {
        return cache.len;
    }

    auto& direct = vyx_rt_string_len_direct_cache();
    const auto slot = vyx_rt_string_len_direct_slot(s);
    if (vyx_rt_direct_slot_holds(direct[slot], s)) {
        cache.ptr = s;
        cache.len = direct[slot].len;
        return direct[slot].len;
    }

    uint64_t arena_len = 0;
    if (vyx_rt_arena_string_len(s, arena_len)) {
        cache.ptr = s;
        cache.len = arena_len;
        direct[slot] = vyx_rt_make_direct_slot(s, arena_len, false);
        return arena_len;
    }

    {
        std::lock_guard<std::mutex> lock(vyx_rt_string_lengths_mutex());
        auto& lengths = vyx_rt_string_lengths();
        auto it = lengths.find(s);
        if (it != lengths.end()) {
            cache.ptr = s;
            cache.len = it->second;
            direct[slot] = vyx_rt_make_direct_slot(s, it->second, true);
            return it->second;
        }
    }

    // No registered lifetime means no reusable length fact. External buffers
    // can be mutated or freed/reallocated at the same address without notifying
    // this runtime. Caching that address would also allow a later direct-cache
    // eviction to promote the stale length into the shared registry.
    return vyx_rt_bounded_cstr_len(s);
}

static char* vyx_rt_adopt_if_header_string(const char* s, uint64_t len) {
    uint64_t known = 0;
    if (vyx_rt_arena_string_len(s, known) && known == len) {
        vyx_rt_note_string_len_cached(s, len);
        return const_cast<char*>(s);
    }
    return nullptr;
}

static void* vyx_rt_alloc_string_arena_bytes(std::size_t bytes) {
    constexpr std::size_t kDefaultChunk = 32u * 1024u * 1024u;
    if (bytes == 0u) {
        bytes = 1u;
    }
    if (bytes > std::numeric_limits<std::size_t>::max() - 7u) {
        return nullptr;
    }
    const std::size_t aligned = (bytes + 7u) & ~std::size_t{7u};
    auto& arena = vyx_rt_string_arena();
    if (aligned > arena.remaining) {
        const std::size_t chunk_size = std::max(kDefaultChunk, aligned);
        std::size_t next_index = arena.cur ? arena.active_chunk + 1u : 0u;
        bool reused = false;
        while (next_index < arena.chunks.size()) {
            const auto& range = arena.ranges[next_index];
            const auto existing_size = static_cast<std::size_t>(range.end - range.begin);
            if (existing_size >= aligned) {
                arena.active_chunk = next_index;
                arena.cur = range.begin;
                arena.remaining = existing_size;
                reused = true;
                break;
            }
            next_index += 1u;
        }
        if (!reused) {
            auto* chunk = static_cast<char*>(std::malloc(chunk_size));
            if (!chunk) {
                return nullptr;
            }
            if (bootstrapRtMemProfileEnabledValue) {
                const uint64_t total = bootstrapRtArenaBytes.fetch_add(
                    static_cast<uint64_t>(chunk_size),
                    std::memory_order_relaxed) + static_cast<uint64_t>(chunk_size);
                uint64_t next = bootstrapRtArenaNextReport.load(std::memory_order_relaxed);
                while (total >= next) {
                    if (bootstrapRtArenaNextReport.compare_exchange_weak(
                            next,
                            next + 64ull * 1024ull * 1024ull,
                            std::memory_order_relaxed)) {
                        std::fprintf(stderr,
                                     "[vyx-rt-mem] string_arena=%lluMB chunksize=%lluMB concat=%llu substring=%llu clone=%llu from_cstr_len=%llu from_cstr_view_len=%llu int_to_string=%llu\n",
                                     static_cast<unsigned long long>(total / (1024ull * 1024ull)),
                                     static_cast<unsigned long long>(static_cast<uint64_t>(chunk_size) / (1024ull * 1024ull)),
                                     static_cast<unsigned long long>(bootstrapRtProfileRecords()[static_cast<std::size_t>(BootstrapRtProfId::Concat)].calls),
                                     static_cast<unsigned long long>(bootstrapRtProfileRecords()[static_cast<std::size_t>(BootstrapRtProfId::Substring)].calls),
                                     static_cast<unsigned long long>(bootstrapRtProfileRecords()[static_cast<std::size_t>(BootstrapRtProfId::Clone)].calls),
                                     static_cast<unsigned long long>(bootstrapRtProfileRecords()[static_cast<std::size_t>(BootstrapRtProfId::FromCstrLen)].calls),
                                     static_cast<unsigned long long>(bootstrapRtProfileRecords()[static_cast<std::size_t>(BootstrapRtProfId::FromCstrViewLen)].calls),
                                     static_cast<unsigned long long>(bootstrapRtProfileRecords()[static_cast<std::size_t>(BootstrapRtProfId::IntToString)].calls));
                    }
                }
            }
            arena.active_chunk = arena.chunks.size();
            arena.chunks.push_back(chunk);
            arena.ranges.push_back(VyxRtStringArenaChunk{chunk, chunk + chunk_size});
            arena.cur = chunk;
            arena.remaining = chunk_size;
        }
    }
    void* out = arena.cur;
    arena.cur += aligned;
    arena.remaining -= aligned;
    return out;
}

static char* vyx_rt_alloc_string_storage(uint64_t len) {
    if (len > std::numeric_limits<std::size_t>::max()
                  - sizeof(VyxRtStringHeader) - 1u) {
        return nullptr;
    }
    const std::size_t bytes = sizeof(VyxRtStringHeader)
        + static_cast<std::size_t>(len) + 1u;
    auto* raw = static_cast<char*>(vyx_rt_alloc_string_arena_bytes(bytes));
    if (!raw) {
        return nullptr;
    }
    auto* header = reinterpret_cast<VyxRtStringHeader*>(raw);
    header->magic = kVyxRtStringHeaderMagic;
    header->len = len;
    return raw + sizeof(VyxRtStringHeader);
}

static char* vyx_rt_dup_bytes(const char* s, uint64_t len) {
    if (!s) {
        len = 0;
    }
    if (len == 0) {
        return vyx_rt_empty_cstr();
    }
    auto* out = vyx_rt_alloc_string_storage(len);
    if (!out) {
        return nullptr;
    }
    if (s && len > 0) {
        std::memcpy(out, s, static_cast<size_t>(len));
    }
    out[len] = '\0';
    vyx_rt_note_string_len_cached(out, len);
    return out;
}

static char* vyx_rt_dup_cstr(const char* s) {
    // This boundary accepts a NUL-terminated C string, not a registered Vyx
    // byte view. Metadata associated with a prior owner of the same address
    // must never determine the C string's length. Explicit-length callers use
    // vyx_rt_dup_bytes/from_cstr_len and preserve embedded NUL bytes.
    return vyx_rt_dup_bytes(s, s ? static_cast<uint64_t>(std::strlen(s)) : 0u);
}

static char* vyx_rt_empty_cstr() {
    static char empty[1] = {'\0'};
    vyx_rt_note_string_len_cached(empty, 0);
    return empty;
}

extern "C" VYX_RT_ABI int64_t vyx_string_len(const char* s) {
    VYX_BOOTSTRAP_RT_PROF(StringLen);
    return static_cast<int64_t>(vyx_rt_known_string_len(s));
}

extern "C" VYX_RT_ABI void vyx_note_string_len(const char* s, int64_t len) {
    if (!s) {
        return;
    }
    if (len < 0) {
        len = 0;
    }
    vyx_rt_note_string_len(s, static_cast<uint64_t>(len));
}

extern "C" VYX_RT_ABI void vyx_forget_string_len(const char* s) {
    (void)vyx_rt_forget_string_len(s);
}

extern "C" VYX_RT_ABI void vyx_free_runtime_string(char* s) {
    if (!s) {
        return;
    }
    if (s == vyx_rt_empty_cstr()) {
        // Legacy bootstrap helpers may expose the shared empty sentinel as a
        // runtime string. It has no allocator ownership and must never reach
        // std::free, regardless of the caller generation's ownership tag.
        (void)vyx_rt_forget_string_len(s);
        return;
    }
    uint64_t arena_len = 0;
    if (vyx_rt_arena_string_len(s, arena_len)) {
        // Arena strings are reclaimed with their chunk. Some bootstrap-only
        // helpers expose them through the same C ABI, so never pass an
        // interior arena pointer to the CRT allocator.
        (void)vyx_rt_forget_string_len(s);
        return;
    }
    (void)vyx_rt_forget_string_len(s);
    vyx_rt_service_free(s);
}

extern "C" VYX_RT_ABI void* vyx_rt_string_arena_alloc(int64_t bytes) {
    if (bytes < 0
        || static_cast<uint64_t>(bytes) > std::numeric_limits<std::size_t>::max()) {
        return nullptr;
    }
    return vyx_rt_alloc_string_arena_bytes(static_cast<std::size_t>(bytes));
}

extern "C" VYX_RT_ABI void vyx_rt_heap_trim() {
    auto& arena = vyx_rt_string_arena();
    if (bootstrapRtStringArenaMarksLive.load(std::memory_order_relaxed) == 0
        && arena.chunks.size() > 1u) {
        uint64_t reserved = 0;
        for (const auto& range : arena.ranges) {
            reserved += static_cast<uint64_t>(range.end - range.begin);
        }

        constexpr uint64_t kArenaTrimThreshold = 256ull * 1024ull * 1024ull;
        if (reserved >= kArenaTrimThreshold) {
            const std::size_t keep_count = arena.cur ? (arena.active_chunk + 1u) : 0u;
            if (keep_count < arena.chunks.size()) {
                for (std::size_t i = keep_count; i < arena.chunks.size(); ++i) {
                    std::free(arena.chunks[i]);
                }
                arena.chunks.resize(keep_count);
                arena.ranges.resize(keep_count);
                if (keep_count == 0u) {
                    arena.cur = nullptr;
                    arena.remaining = 0;
                    arena.active_chunk = 0;
                }
                vyx_rt_clear_string_len_thread_cache();
            }
        }
    }
#ifdef _WIN32
    const char* compact = std::getenv("VYX_HEAP_COMPACT");
    if (compact != nullptr && compact[0] != '\0' && compact[0] != '0') {
        _heapmin();
        HeapCompact(GetProcessHeap(), 0);
        SetProcessWorkingSetSize(GetCurrentProcess(),
                                 static_cast<SIZE_T>(-1),
                                 static_cast<SIZE_T>(-1));
    }
#endif
}

extern "C" VYX_RT_ABI void* vyx_rt_string_arena_checkpoint() {
    auto* mark = static_cast<VyxRtStringArenaMark*>(std::malloc(sizeof(VyxRtStringArenaMark)));
    if (!mark) {
        return nullptr;
    }
    bootstrapRtStringArenaMarksTotal.fetch_add(1u, std::memory_order_relaxed);
    bootstrapRtStringArenaMarksLive.fetch_add(1u, std::memory_order_relaxed);
    auto& arena = vyx_rt_string_arena();
    mark->cur = arena.cur;
    mark->remaining = arena.remaining;
    mark->active_chunk = arena.cur ? arena.active_chunk : 0u;
    return mark;
}

extern "C" VYX_RT_ABI void vyx_rt_string_arena_release(void* raw_mark) {
    if (!raw_mark) {
        return;
    }
    auto* mark = static_cast<VyxRtStringArenaMark*>(raw_mark);
    auto& arena = vyx_rt_string_arena();
    const char* old_cur = arena.cur;
    const std::size_t old_active_chunk = arena.cur ? arena.active_chunk : 0u;

    if (!arena.ranges.empty() && old_cur && old_active_chunk < arena.ranges.size()) {
        const std::size_t mark_chunk = mark->cur ? mark->active_chunk : 0u;
        if (!mark->cur) {
            std::size_t i = 0;
            while (i < old_active_chunk && i < arena.ranges.size()) {
                vyx_rt_forget_string_lengths_in_range(arena.ranges[i].begin, arena.ranges[i].end);
                i += 1u;
            }
            vyx_rt_forget_string_lengths_in_range(arena.ranges[old_active_chunk].begin, old_cur);
        } else if (mark_chunk < arena.ranges.size()) {
            if (mark_chunk == old_active_chunk) {
                vyx_rt_forget_string_lengths_in_range(mark->cur, old_cur);
            } else if (mark_chunk < old_active_chunk) {
                vyx_rt_forget_string_lengths_in_range(mark->cur, arena.ranges[mark_chunk].end);
                std::size_t i = mark_chunk + 1u;
                while (i < old_active_chunk && i < arena.ranges.size()) {
                    vyx_rt_forget_string_lengths_in_range(arena.ranges[i].begin, arena.ranges[i].end);
                    i += 1u;
                }
                vyx_rt_forget_string_lengths_in_range(arena.ranges[old_active_chunk].begin, old_cur);
            }
        }
    }

    arena.cur = mark->cur;
    arena.remaining = mark->remaining;
    arena.active_chunk = mark->active_chunk;
    if (!arena.cur) {
        arena.remaining = 0;
        arena.active_chunk = 0;
    } else if (arena.active_chunk >= arena.chunks.size()) {
        arena.cur = nullptr;
        arena.remaining = 0;
        arena.active_chunk = 0;
    }

    vyx_rt_clear_string_len_thread_cache();
    uint64_t live = bootstrapRtStringArenaMarksLive.load(std::memory_order_relaxed);
    while (live > 0
           && !bootstrapRtStringArenaMarksLive.compare_exchange_weak(
               live,
               live - 1u,
               std::memory_order_relaxed)) {
    }
    std::free(mark);
}

extern "C" VYX_RT_ABI int64_t vyx_rt_string_arena_reserved_bytes() {
    auto& arena = vyx_rt_string_arena();
    uint64_t total = 0;
    for (const auto& range : arena.ranges) {
        total += static_cast<uint64_t>(range.end - range.begin);
    }
    return static_cast<int64_t>(total);
}

extern "C" VYX_RT_ABI int64_t vyx_rt_string_arena_active_bytes() {
    auto& arena = vyx_rt_string_arena();
    if (!arena.cur || arena.active_chunk >= arena.ranges.size()) {
        return 0;
    }
    uint64_t total = 0;
    std::size_t i = 0;
    while (i < arena.active_chunk) {
        total += static_cast<uint64_t>(arena.ranges[i].end - arena.ranges[i].begin);
        i += 1u;
    }
    const auto& active = arena.ranges[arena.active_chunk];
    if (arena.cur > active.begin) {
        total += static_cast<uint64_t>(arena.cur - active.begin);
    }
    return static_cast<int64_t>(total);
}

extern "C" VYX_RT_ABI int64_t vyx_rt_ast_dup_bytes_total() {
    return static_cast<int64_t>(bootstrapRtAstDupBytes.load(std::memory_order_relaxed));
}

extern "C" VYX_RT_ABI int64_t vyx_rt_ast_node_bytes_total() {
    return static_cast<int64_t>(bootstrapRtAstNodeBytes.load(std::memory_order_relaxed));
}

extern "C" VYX_RT_ABI int64_t vyx_rt_raw_dup_bytes_total() {
    return static_cast<int64_t>(bootstrapRtRawDupBytesTotal.load(std::memory_order_relaxed));
}

extern "C" VYX_RT_ABI int64_t vyx_rt_raw_dup_bytes_live() {
    return static_cast<int64_t>(bootstrapRtRawDupBytesLive.load(std::memory_order_relaxed));
}

extern "C" VYX_RT_ABI int64_t vyx_rt_string_len_entry_count() {
    std::lock_guard<std::mutex> lock(vyx_rt_string_lengths_mutex());
    return static_cast<int64_t>(vyx_rt_string_lengths().size());
}

extern "C" VYX_RT_ABI int64_t vyx_rt_heap_used_bytes() {
#ifdef _WIN32
    _HEAPINFO info{};
    info._pentry = nullptr;
    uint64_t total = 0;
    for (;;) {
        const int rc = _heapwalk(&info);
        if (rc == _HEAPEND) {
            return static_cast<int64_t>(total);
        }
        if (rc != _HEAPOK) {
            return -1;
        }
        if (info._useflag == _USEDENTRY) {
            total += static_cast<uint64_t>(info._size);
        }
    }
#else
    return -1;
#endif
}

#ifdef _WIN32
static bool vyx_rt_query_windows_process_memory(HANDLE process,
                                                 PROCESS_MEMORY_COUNTERS_EX& counters) {
    using GetProcessMemoryInfoFn = BOOL(WINAPI*)(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);
    static GetProcessMemoryInfoFn get_process_memory_info = []() -> GetProcessMemoryInfoFn {
        if (HMODULE kernel = GetModuleHandleA("kernel32.dll")) {
            if (auto* proc = GetProcAddress(kernel, "K32GetProcessMemoryInfo")) {
                return reinterpret_cast<GetProcessMemoryInfoFn>(proc);
            }
        }
        if (HMODULE psapi = LoadLibraryA("psapi.dll")) {
            if (auto* proc = GetProcAddress(psapi, "GetProcessMemoryInfo")) {
                return reinterpret_cast<GetProcessMemoryInfoFn>(proc);
            }
        }
        return nullptr;
    }();
    if (!get_process_memory_info) {
        return false;
    }
    counters = {};
    counters.cb = sizeof(counters);
    return get_process_memory_info(process,
                                   reinterpret_cast<PPROCESS_MEMORY_COUNTERS>(&counters),
                                   sizeof(counters)) != FALSE;
}
#endif

static bool vyx_rt_query_process_memory(uint64_t& private_bytes,
                                        uint64_t& working_set_bytes,
                                        uint64_t& peak_private_bytes,
                                        uint64_t& peak_working_set_bytes) {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (!vyx_rt_query_windows_process_memory(GetCurrentProcess(), counters)) {
        return false;
    }
    private_bytes = static_cast<uint64_t>(counters.PrivateUsage);
    working_set_bytes = static_cast<uint64_t>(counters.WorkingSetSize);
    peak_private_bytes = static_cast<uint64_t>(counters.PeakPagefileUsage);
    peak_working_set_bytes = static_cast<uint64_t>(counters.PeakWorkingSetSize);
    return true;
#else
    private_bytes = 0;
    working_set_bytes = 0;
    peak_private_bytes = 0;
    peak_working_set_bytes = 0;
    return false;
#endif
}

extern "C" VYX_RT_ABI void vyx_rt_heap_bucket_report(const char* label) {
#ifdef _WIN32
    static const uint64_t limits[] = {
        16ull, 32ull, 64ull, 128ull, 256ull, 512ull, 1024ull,
        4096ull, 16384ull, 65536ull, 262144ull, 1048576ull,
        4194304ull, UINT64_MAX
    };
    static const char* names[] = {
        "<=16", "<=32", "<=64", "<=128", "<=256", "<=512", "<=1K",
        "<=4K", "<=16K", "<=64K", "<=256K", "<=1M",
        "<=4M", ">4M"
    };
    uint64_t counts[sizeof(limits) / sizeof(limits[0])] = {};
    uint64_t bytes[sizeof(limits) / sizeof(limits[0])] = {};
    uint64_t small_counts[129] = {};
    uint64_t total = 0;
    _HEAPINFO info{};
    info._pentry = nullptr;
    for (;;) {
        const int rc = _heapwalk(&info);
        if (rc == _HEAPEND) {
            break;
        }
        if (rc != _HEAPOK) {
            std::fprintf(stderr,
                         "[vyx-heap-buckets] %s heapwalk-error=%d total_mb=%llu\n",
                         label ? label : "",
                         rc,
                         static_cast<unsigned long long>(total / (1024ull * 1024ull)));
            return;
        }
        if (info._useflag != _USEDENTRY) {
            continue;
        }
        const uint64_t n = static_cast<uint64_t>(info._size);
        total += n;
        if (n <= 128ull) {
            small_counts[static_cast<std::size_t>(n)] += 1u;
        }
        std::size_t bucket = 0;
        while (bucket + 1u < (sizeof(limits) / sizeof(limits[0])) && n > limits[bucket]) {
            bucket += 1u;
        }
        counts[bucket] += 1u;
        bytes[bucket] += n;
    }
    std::fprintf(stderr,
                 "[vyx-heap-buckets] %s total_mb=%llu arena_marks=%llu/%llu",
                 label ? label : "",
                 static_cast<unsigned long long>(total / (1024ull * 1024ull)),
                 static_cast<unsigned long long>(bootstrapRtStringArenaMarksLive.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(bootstrapRtStringArenaMarksTotal.load(std::memory_order_relaxed)));
    for (std::size_t i = 0; i < (sizeof(limits) / sizeof(limits[0])); ++i) {
        if (counts[i] == 0) {
            continue;
        }
        std::fprintf(stderr,
                     " %s:%llu/%lluMB",
                     names[i],
                     static_cast<unsigned long long>(counts[i]),
                     static_cast<unsigned long long>(bytes[i] / (1024ull * 1024ull)));
    }
    std::fprintf(stderr, " small_top=");
    for (int rank = 0; rank < 8; ++rank) {
        std::size_t best_size = 0;
        uint64_t best_count = 0;
        for (std::size_t i = 1; i < (sizeof(small_counts) / sizeof(small_counts[0])); ++i) {
            if (small_counts[i] > best_count) {
                best_size = i;
                best_count = small_counts[i];
            }
        }
        if (best_count == 0) {
            break;
        }
        std::fprintf(stderr,
                     "%s%zu:%llu",
                     rank == 0 ? "" : ",",
                     best_size,
                     static_cast<unsigned long long>(best_count));
        small_counts[best_size] = 0;
    }
    std::fprintf(stderr, "\n");
#else
    (void)label;
#endif
}

extern "C" VYX_RT_ABI int64_t vyx_rt_process_private_bytes() {
    uint64_t private_bytes = 0;
    uint64_t working_set_bytes = 0;
    uint64_t peak_private_bytes = 0;
    uint64_t peak_working_set_bytes = 0;
    if (!vyx_rt_query_process_memory(private_bytes,
                                     working_set_bytes,
                                     peak_private_bytes,
                                     peak_working_set_bytes)) {
        return -1;
    }
    return static_cast<int64_t>(private_bytes);
}

extern "C" VYX_RT_ABI int64_t vyx_rt_process_working_set_bytes() {
    uint64_t private_bytes = 0;
    uint64_t working_set_bytes = 0;
    uint64_t peak_private_bytes = 0;
    uint64_t peak_working_set_bytes = 0;
    if (!vyx_rt_query_process_memory(private_bytes,
                                     working_set_bytes,
                                     peak_private_bytes,
                                     peak_working_set_bytes)) {
        return -1;
    }
    return static_cast<int64_t>(working_set_bytes);
}

// Commit charge the machine can still hand out.
//
// The CGU emit pipeline used to size its worker pool from the core count
// alone.  Every worker owns an LLVMContext plus the module it is lowering, and
// on a unit of several thousand functions that IR is measured in gigabytes:
// a 20-core host sized the pool at the `CGU_PIPE_AUTO_MAX_THREADS` ceiling of
// 8, reached 44 GB of private commit for one crate, exhausted the system commit
// limit and died with `pooled string malloc failed` inside LLVM.  Exposing the
// remaining commit budget lets the codegen cap the pool by memory instead of
// over-committing on a large crate.
extern "C" VYX_RT_ABI int64_t vyx_rt_system_available_commit_bytes() {
#ifdef _WIN32
    using GetPerformanceInfoFn = BOOL(WINAPI*)(PPERFORMANCE_INFORMATION, DWORD);
    static GetPerformanceInfoFn get_performance_info = []() -> GetPerformanceInfoFn {
        if (HMODULE kernel = GetModuleHandleA("kernel32.dll")) {
            if (auto* fn = GetProcAddress(kernel, "K32GetPerformanceInfo")) {
                return reinterpret_cast<GetPerformanceInfoFn>(fn);
            }
        }
        if (HMODULE psapi = LoadLibraryA("psapi.dll")) {
            if (auto* fn = GetProcAddress(psapi, "GetPerformanceInfo")) {
                return reinterpret_cast<GetPerformanceInfoFn>(fn);
            }
        }
        return nullptr;
    }();
    PERFORMANCE_INFORMATION info{};
    info.cb = sizeof(info);
    if (!get_performance_info || !get_performance_info(&info, sizeof(info))) {
        return -1;
    }
    // GlobalMemoryStatusEx::ullAvailPageFile can be constrained by the caller's
    // per-process limit. Query actual system commit accounting instead.
    const auto pages = info.CommitLimit > info.CommitTotal
        ? info.CommitLimit - info.CommitTotal : 0;
    if (info.PageSize == 0 || pages > static_cast<SIZE_T>(INT64_MAX) / info.PageSize) {
        return -1;
    }
    return static_cast<int64_t>(pages * info.PageSize);
#else
    // Physical availability is a separate budget, not a commit observation.
    // No POSIX commit/cgroup implementation is installed yet.
    return -1;
#endif
}

extern "C" VYX_RT_ABI int64_t vyx_rt_system_total_physical_bytes() {
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status)) {
        return -1;
    }
    return static_cast<int64_t>(status.ullTotalPhys);
#else
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long page_size = sysconf(_SC_PAGESIZE);
    if (pages <= 0 || page_size <= 0) {
        return -1;
    }
    return static_cast<int64_t>(pages) * static_cast<int64_t>(page_size);
#endif
}

extern "C" VYX_RT_ABI int64_t vyx_rt_system_available_physical_bytes() {
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status)) {
        return -1;
    }
    return static_cast<int64_t>(status.ullAvailPhys);
#else
    const long pages = sysconf(_SC_AVPHYS_PAGES);
    const long page_size = sysconf(_SC_PAGESIZE);
    if (pages <= 0 || page_size <= 0) {
        return -1;
    }
    return static_cast<int64_t>(pages) * static_cast<int64_t>(page_size);
#endif
}

extern "C" VYX_RT_ABI int64_t vyx_rt_process_peak_private_bytes() {
    uint64_t private_bytes = 0;
    uint64_t working_set_bytes = 0;
    uint64_t peak_private_bytes = 0;
    uint64_t peak_working_set_bytes = 0;
    if (!vyx_rt_query_process_memory(private_bytes,
                                     working_set_bytes,
                                     peak_private_bytes,
                                     peak_working_set_bytes)) {
        return -1;
    }
    return static_cast<int64_t>(peak_private_bytes);
}

extern "C" VYX_RT_ABI int64_t vyx_rt_process_peak_working_set_bytes() {
    uint64_t private_bytes = 0;
    uint64_t working_set_bytes = 0;
    uint64_t peak_private_bytes = 0;
    uint64_t peak_working_set_bytes = 0;
    if (!vyx_rt_query_process_memory(private_bytes,
                                     working_set_bytes,
                                     peak_private_bytes,
                                     peak_working_set_bytes)) {
        return -1;
    }
    return static_cast<int64_t>(peak_working_set_bytes);
}

extern "C" VYX_RT_ABI char* vyx_alloc_string_len(int64_t len) {
    if (len < 0) {
        len = 0;
    }
    const auto n = static_cast<uint64_t>(len);
    char* out = vyx_rt_alloc_string_storage(n);
    if (!out) {
        return nullptr;
    }
    out[n] = '\0';
    vyx_rt_note_string_len_cached(out, n);
    return out;
}

extern "C" VYX_RT_ABI char* vyx_dup_bytes_raw(const char* s, int64_t len) {
    if (len < 0) {
        len = 0;
    }
    const auto n = static_cast<uint64_t>(len);
    auto* out = static_cast<char*>(std::malloc(static_cast<std::size_t>(n + 1)));
    if (!out) {
        return nullptr;
    }
    if (s && n > 0) {
        std::memcpy(out, s, static_cast<std::size_t>(n));
    }
    out[n] = '\0';
    vyx_rt_note_string_len(out, n);
    bootstrapRtRawDupBytesTotal.fetch_add(n + 1u, std::memory_order_relaxed);
    bootstrapRtRawDupBytesLive.fetch_add(n + 1u, std::memory_order_relaxed);
    bootstrapRtRawDupCalls.fetch_add(1u, std::memory_order_relaxed);
    return out;
}

extern "C" VYX_RT_ABI void vyx_free_bytes_raw(char* s) {
    if (!s) {
        return;
    }
    const uint64_t bytes = vyx_rt_forget_string_len(s);
    if (bytes > 0) {
        uint64_t live = bootstrapRtRawDupBytesLive.load(std::memory_order_relaxed);
        while (live >= bytes
               && !bootstrapRtRawDupBytesLive.compare_exchange_weak(
                   live,
                   live - bytes,
                   std::memory_order_relaxed)) {
        }
    }
    std::free(s);
}

extern "C" VYX_RT_ABI int64_t vyx_bootstrap_now_ms() {
    using clock = std::chrono::steady_clock;
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            clock::now().time_since_epoch()).count());
}

extern "C" VYX_RT_ABI int32_t vyx_bootstrap_profile_level() {
    const char* raw = std::getenv("VYX_BOOTSTRAP_PROFILE");
    if (!raw || raw[0] == '\0') {
        return 0;
    }
    char* end = nullptr;
    long level = std::strtol(raw, &end, 10);
    if (end == raw) {
        return 1;
    }
    if (level < 0) {
        return 0;
    }
    if (level > 9) {
        return 9;
    }
    return static_cast<int32_t>(level);
}

extern "C" VYX_RT_ABI int32_t vyx_bootstrap_platform() {
#if defined(_WIN32)
    return 1;
#elif defined(__ANDROID__)
    return 3;
#elif defined(__linux__)
    return 2;
#elif defined(__APPLE__)
    return 4;
#else
    return 0;
#endif
}

extern "C" VYX_RT_ABI const char* vyx_bootstrap_llvm_version() {
#ifdef LLVM_VERSION_STRING
    return LLVM_VERSION_STRING;
#else
    return "";
#endif
}

extern "C" VYX_RT_ABI const char* vyx_bootstrap_llvm_host_triple() {
#if defined(LLVM_HOST_TRIPLE)
    return LLVM_HOST_TRIPLE;
#elif defined(LLVM_DEFAULT_TARGET_TRIPLE)
    return LLVM_DEFAULT_TARGET_TRIPLE;
#else
    return "";
#endif
}

extern "C" VYX_RT_ABI int64_t vyx_bootstrap_file_size(const char* path) {
    if (!path || !*path) {
        return -1;
    }
    std::error_code ec;
    const auto sz = std::filesystem::file_size(std::filesystem::path(path), ec);
    if (ec) {
        return -1;
    }
    return static_cast<int64_t>(sz);
}

#if defined(__linux__) || defined(__APPLE__)
extern "C" VYX_RT_ABI int64_t vyx_bootstrap_ucontext_size() {
    return static_cast<int64_t>(sizeof(ucontext_t));
}

extern "C" VYX_RT_ABI int64_t vyx_bootstrap_ucontext_stack_sp_offset() {
    return static_cast<int64_t>(offsetof(ucontext_t, uc_stack) + offsetof(stack_t, ss_sp));
}

extern "C" VYX_RT_ABI int64_t vyx_bootstrap_ucontext_stack_size_offset() {
    return static_cast<int64_t>(offsetof(ucontext_t, uc_stack) + offsetof(stack_t, ss_size));
}

extern "C" VYX_RT_ABI int64_t vyx_bootstrap_ucontext_link_offset() {
    return static_cast<int64_t>(offsetof(ucontext_t, uc_link));
}

static thread_local void* g_vyx_rt_current_fiber = nullptr;
static thread_local void* g_vyx_rt_scheduler_ctx = nullptr;

using VyxRtFiberEntry = int32_t (*)(void*);

struct VyxRtPosixFiberScheduler {
    uint64_t magic;
};

struct VyxRtPosixFiber {
    ucontext_t ctx;
    void* stack;
    std::size_t stack_size;
    VyxRtFiberEntry entry;
    void* arg;
    int32_t finished;
};

static constexpr uint64_t kVyxRtFiberSchedulerMagic = 0x5659584649424552ull;
static constexpr std::size_t kVyxRtFiberStackSize = 1u << 20;

static thread_local VyxRtPosixFiber* g_vyx_rt_current_posix_fiber = nullptr;
static thread_local ucontext_t* g_vyx_rt_current_posix_scheduler = nullptr;

static void vyx_rt_posix_fiber_trampoline(uintptr_t fiber_bits) {
    auto* fiber = reinterpret_cast<VyxRtPosixFiber*>(fiber_bits);
    if (!fiber) {
        return;
    }
    if (fiber->entry) {
        fiber->entry(fiber->arg);
    }
    fiber->finished = 1;
}

extern "C" VYX_RT_ABI void vyx_bootstrap_fiber_current_set(void* fiber) {
    g_vyx_rt_current_fiber = fiber;
}

extern "C" VYX_RT_ABI void* vyx_bootstrap_fiber_current_get() {
    return g_vyx_rt_current_fiber;
}

extern "C" VYX_RT_ABI void vyx_bootstrap_scheduler_ctx_set(void* ctx) {
    g_vyx_rt_scheduler_ctx = ctx;
}

extern "C" VYX_RT_ABI void* vyx_bootstrap_scheduler_ctx_get() {
    return g_vyx_rt_scheduler_ctx;
}

extern "C" VYX_RT_ABI void* vyx_bootstrap_fiber_start() {
    auto* scheduler = static_cast<VyxRtPosixFiberScheduler*>(
        std::malloc(sizeof(VyxRtPosixFiberScheduler)));
    if (!scheduler) {
        return nullptr;
    }
    scheduler->magic = kVyxRtFiberSchedulerMagic;
    return scheduler;
}

extern "C" VYX_RT_ABI void vyx_bootstrap_fiber_stop(void* scheduler) {
    if (!scheduler) {
        return;
    }
    auto* s = static_cast<VyxRtPosixFiberScheduler*>(scheduler);
    if (s->magic == kVyxRtFiberSchedulerMagic) {
        s->magic = 0;
    }
    std::free(scheduler);
}

extern "C" VYX_RT_ABI void* vyx_bootstrap_fiber_create(void* scheduler, void* work, void* arg) {
    auto* s = static_cast<VyxRtPosixFiberScheduler*>(scheduler);
    if (!s || s->magic != kVyxRtFiberSchedulerMagic || !work) {
        return nullptr;
    }

    auto* fiber = static_cast<VyxRtPosixFiber*>(std::malloc(sizeof(VyxRtPosixFiber)));
    if (!fiber) {
        return nullptr;
    }
    std::memset(fiber, 0, sizeof(VyxRtPosixFiber));
    fiber->stack_size = kVyxRtFiberStackSize;
    fiber->entry = reinterpret_cast<VyxRtFiberEntry>(work);
    fiber->arg = arg;

    if (getcontext(&fiber->ctx) != 0) {
        std::free(fiber);
        return nullptr;
    }

    fiber->stack = std::malloc(fiber->stack_size);
    if (!fiber->stack) {
        std::free(fiber);
        return nullptr;
    }
    fiber->ctx.uc_stack.ss_sp = fiber->stack;
    fiber->ctx.uc_stack.ss_size = fiber->stack_size;
    fiber->ctx.uc_stack.ss_flags = 0;
    fiber->ctx.uc_link = nullptr;
    makecontext(&fiber->ctx,
                reinterpret_cast<void (*)()>(&vyx_rt_posix_fiber_trampoline),
                1,
                static_cast<uintptr_t>(reinterpret_cast<std::uintptr_t>(fiber)));
    return fiber;
}

extern "C" VYX_RT_ABI void vyx_bootstrap_fiber_switch(void* fiber_handle) {
    auto* fiber = static_cast<VyxRtPosixFiber*>(fiber_handle);
    if (!fiber || fiber->finished) {
        return;
    }

    ucontext_t scheduler_ctx;
    std::memset(&scheduler_ctx, 0, sizeof(scheduler_ctx));
    fiber->ctx.uc_link = &scheduler_ctx;

    g_vyx_rt_current_posix_fiber = fiber;
    g_vyx_rt_current_posix_scheduler = &scheduler_ctx;
    swapcontext(&scheduler_ctx, &fiber->ctx);
    g_vyx_rt_current_posix_scheduler = nullptr;
    g_vyx_rt_current_posix_fiber = nullptr;
}

extern "C" VYX_RT_ABI void vyx_bootstrap_fiber_yield() {
    if (!g_vyx_rt_current_posix_fiber || !g_vyx_rt_current_posix_scheduler) {
        return;
    }
    swapcontext(&g_vyx_rt_current_posix_fiber->ctx, g_vyx_rt_current_posix_scheduler);
}

extern "C" VYX_RT_ABI void vyx_bootstrap_fiber_delete(void* fiber_handle) {
    auto* fiber = static_cast<VyxRtPosixFiber*>(fiber_handle);
    if (!fiber) {
        return;
    }
    if (fiber->stack) {
        std::free(fiber->stack);
        fiber->stack = nullptr;
    }
    std::free(fiber);
}

extern "C" VYX_RT_ABI int32_t vyx_bootstrap_fiber_finished(void* fiber_handle) {
    auto* fiber = static_cast<VyxRtPosixFiber*>(fiber_handle);
    if (!fiber) {
        return 1;
    }
    return fiber->finished != 0 ? 1 : 0;
}
#endif

extern "C" VYX_RT_ABI void vyx_bootstrap_sleep_ms(int32_t ms) {
    if (ms <= 0) {
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

extern "C" VYX_RT_ABI int32_t vyx_bootstrap_mkdir_p(const char* path) {
    if (!path || !*path) {
        return 0;
    }
    std::error_code ec;
    const std::filesystem::path p(path);
    if (std::filesystem::is_directory(p, ec)) {
        return 0;
    }
    ec.clear();
    if (std::filesystem::create_directories(p, ec)) {
        return 0;
    }
    if (!ec && std::filesystem::is_directory(p, ec)) {
        return 0;
    }
    return 1;
}

static int64_t vyx_bootstrap_current_pid_value() {
#ifdef _WIN32
    return static_cast<int64_t>(::GetCurrentProcessId());
#else
    return static_cast<int64_t>(::getpid());
#endif
}

static bool vyx_bootstrap_legacy_lock_process_alive(int64_t pid) {
    if (pid <= 0) {
        return false;
    }
#ifdef _WIN32
    HANDLE handle = ::OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!handle) {
        const DWORD error = ::GetLastError();
        return error != ERROR_INVALID_PARAMETER;
    }
    const DWORD wait_result = ::WaitForSingleObject(handle, 0);
    ::CloseHandle(handle);
    return wait_result != WAIT_OBJECT_0;
#else
    if (::kill(static_cast<pid_t>(pid), 0) == 0) {
        return true;
    }
    return errno == EPERM;
#endif
}

static int64_t vyx_bootstrap_legacy_lock_owner_pid(const std::filesystem::path& lock_path) {
    std::ifstream in(lock_path / "owner.txt", std::ios::binary);
    if (!in) {
        return -1;
    }
    std::string line;
    std::getline(in, line);
    constexpr std::string_view prefix = "pid=";
    if (line.rfind(prefix, 0) != 0) {
        return -1;
    }
    char* end = nullptr;
    const long long pid = std::strtoll(line.c_str() + prefix.size(), &end, 10);
    if (end == line.c_str() + prefix.size() || pid <= 0) {
        return -1;
    }
    return static_cast<int64_t>(pid);
}

static int32_t vyx_bootstrap_migrate_legacy_build_lock(
    const std::filesystem::path& lock_path) {
    std::error_code ec;
    const bool is_legacy_dir = std::filesystem::is_directory(lock_path, ec);
    if (ec) {
        if (ec == std::errc::no_such_file_or_directory
            || ec == std::errc::not_a_directory) {
            return 0;
        }
        return 2;
    }
    if (!is_legacy_dir) {
        return 0;
    }

    const int64_t owner_pid = vyx_bootstrap_legacy_lock_owner_pid(lock_path);
    if (owner_pid <= 0) {
        return 2;
    }
    if (vyx_bootstrap_legacy_lock_process_alive(owner_pid)) {
        return 1;
    }

    const auto stale_path = lock_path.parent_path()
        / (lock_path.filename().string() + ".legacy-stale."
           + std::to_string(vyx_bootstrap_current_pid_value()));
    std::filesystem::remove_all(stale_path, ec);
    if (ec) {
        return 2;
    }
    std::filesystem::rename(lock_path, stale_path, ec);
    if (ec) {
        return 1;
    }
    std::filesystem::remove_all(stale_path, ec);
    return ec ? 2 : 0;
}

struct VyxBootstrapBuildLock {
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
    OVERLAPPED range{};
#else
    int fd = -1;
#endif
};

static std::mutex& vyx_bootstrap_build_lock_mutex() {
    static std::mutex mutex;
    return mutex;
}

static std::unordered_map<std::string, std::unique_ptr<VyxBootstrapBuildLock>>&
vyx_bootstrap_build_locks() {
    static std::unordered_map<std::string, std::unique_ptr<VyxBootstrapBuildLock>> locks;
    return locks;
}

static bool vyx_bootstrap_build_lock_identity(const char* lock_file,
                                              std::filesystem::path& path,
                                              std::string& key) {
    if (!lock_file || !*lock_file) {
        return false;
    }
    std::error_code ec;
    path = std::filesystem::absolute(std::filesystem::path(lock_file), ec).lexically_normal();
    if (ec) {
        return false;
    }
    key = path.string();
    return !key.empty();
}

static std::string vyx_bootstrap_build_lock_owner_text(const char* owner) {
    std::string text = "pid=" + std::to_string(vyx_bootstrap_current_pid_value()) + "\n";
    if (owner && *owner) {
        text += owner;
        text += '\n';
    }
    return text;
}

static bool vyx_bootstrap_write_locked_owner(VyxBootstrapBuildLock& lock,
                                             const std::string& text) {
#ifdef _WIN32
    LARGE_INTEGER zero{};
    if (!::SetFilePointerEx(lock.handle, zero, nullptr, FILE_BEGIN)
        || !::SetEndOfFile(lock.handle)) {
        return false;
    }
    DWORD written = 0;
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<DWORD>::max())
        || !::WriteFile(lock.handle,
                        text.data(),
                        static_cast<DWORD>(text.size()),
                        &written,
                        nullptr)
        || written != text.size()) {
        return false;
    }
    return ::FlushFileBuffers(lock.handle) != FALSE;
#else
    if (::ftruncate(lock.fd, 0) != 0 || ::lseek(lock.fd, 0, SEEK_SET) < 0) {
        return false;
    }
    std::size_t offset = 0;
    while (offset < text.size()) {
        const ssize_t written = ::write(lock.fd, text.data() + offset, text.size() - offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (written == 0) {
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return ::fsync(lock.fd) == 0;
#endif
}

static bool vyx_bootstrap_close_build_lock(VyxBootstrapBuildLock& lock) {
#ifdef _WIN32
    bool ok = true;
    if (lock.handle != INVALID_HANDLE_VALUE) {
        if (!::UnlockFileEx(lock.handle, 0, 1, 0, &lock.range)) {
            ok = false;
        }
        if (!::CloseHandle(lock.handle)) {
            ok = false;
        }
        lock.handle = INVALID_HANDLE_VALUE;
    }
    return ok;
#else
    bool ok = true;
    if (lock.fd >= 0) {
        if (::flock(lock.fd, LOCK_UN) != 0) {
            ok = false;
        }
        if (::close(lock.fd) != 0) {
            ok = false;
        }
        lock.fd = -1;
    }
    return ok;
#endif
}

extern "C" VYX_RT_ABI int32_t vyx_bootstrap_build_lock_try_acquire(const char* lock_dir,
                                                                     const char* owner) {
    std::filesystem::path lock_path;
    std::string key;
    if (!vyx_bootstrap_build_lock_identity(lock_dir, lock_path, key)) {
        return 2;
    }
    std::lock_guard<std::mutex> guard(vyx_bootstrap_build_lock_mutex());
    auto& locks = vyx_bootstrap_build_locks();
    if (locks.find(key) != locks.end()) {
        return 1;
    }
    const int32_t migration_rc = vyx_bootstrap_migrate_legacy_build_lock(lock_path);
    if (migration_rc != 0) {
        return migration_rc;
    }

    auto lock = std::make_unique<VyxBootstrapBuildLock>();
#ifdef _WIN32
    lock->handle = ::CreateFileW(lock_path.c_str(),
                                 GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr,
                                 OPEN_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL,
                                 nullptr);
    if (lock->handle == INVALID_HANDLE_VALUE) {
        return 2;
    }
    if (!::LockFileEx(lock->handle,
                      LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                      0,
                      1,
                      0,
                      &lock->range)) {
        const DWORD error = ::GetLastError();
        ::CloseHandle(lock->handle);
        lock->handle = INVALID_HANDLE_VALUE;
        if (error == ERROR_LOCK_VIOLATION || error == ERROR_IO_PENDING) {
            return 1;
        }
        return 2;
    }
#else
    // Deliberately inherit the locked open-file-description across exec.
    // A normal parent release uses explicit LOCK_UN, while an abruptly killed
    // parent leaves the lock held until its still-writing child tree exits.
    lock->fd = ::open(lock_path.c_str(), O_CREAT | O_RDWR, 0666);
    if (lock->fd < 0) {
        return 2;
    }
    if (::flock(lock->fd, LOCK_EX | LOCK_NB) != 0) {
        const int error = errno;
        ::close(lock->fd);
        lock->fd = -1;
        if (error == EWOULDBLOCK || error == EAGAIN) {
            return 1;
        }
        return 2;
    }
#endif
    if (!vyx_bootstrap_write_locked_owner(*lock,
                                          vyx_bootstrap_build_lock_owner_text(owner))) {
        vyx_bootstrap_close_build_lock(*lock);
        return 2;
    }
    locks.emplace(key, std::move(lock));
    return 0;
}

extern "C" VYX_RT_ABI int32_t vyx_bootstrap_build_lock_release(const char* lock_dir) {
    std::filesystem::path lock_path;
    std::string key;
    if (!vyx_bootstrap_build_lock_identity(lock_dir, lock_path, key)) {
        return 2;
    }
    std::lock_guard<std::mutex> guard(vyx_bootstrap_build_lock_mutex());
    auto& locks = vyx_bootstrap_build_locks();
    const auto it = locks.find(key);
    if (it == locks.end()) {
        return 2;
    }
    const bool ok = vyx_bootstrap_close_build_lock(*it->second);
    locks.erase(it);
    return ok ? 0 : 2;
}

extern "C" VYX_RT_ABI int32_t vyx_bootstrap_list_vyx_deps(const char* root, const char* out_path) {
    if (!root || !*root || !out_path || !*out_path) {
        return 1;
    }

    std::error_code ec;
    const std::filesystem::path root_path(root);
    const bool root_is_directory = std::filesystem::is_directory(root_path, ec);
    if (ec) {
        if (ec != std::errc::no_such_file_or_directory
            && ec != std::errc::not_a_directory) {
            return 1;
        }
        ec.clear();
    }
    if (!root_is_directory) {
        std::ofstream empty(out_path, std::ios::binary | std::ios::trunc);
        return empty ? 0 : 1;
    }

    std::vector<std::string> rows;
    std::filesystem::recursive_directory_iterator it(root_path, ec);
    const std::filesystem::recursive_directory_iterator end;
    while (!ec && it != end) {
        const auto& entry = *it;
        std::error_code entry_ec;
        const bool is_regular = entry.is_regular_file(entry_ec);
        if (entry_ec) {
            return 1;
        }
        if (is_regular) {
            const auto ext = entry.path().extension().string();
            // Package metadata schemas live beside source files and use the
            // same deterministic recursive file inventory.  Keep this
            // helper's name for ABI compatibility; callers filter the
            // extensions they consume.
            if (ext == ".vyx" || ext == ".vyi" || ext == ".attr") {
                rows.push_back(entry.path().string());
            }
        }
        it.increment(ec);
    }
    if (ec) {
        return 1;
    }
    std::sort(rows.begin(), rows.end());

    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return 1;
    }
    for (const auto& row : rows) {
        out << row << '\n';
    }
    return out ? 0 : 1;
}

static void vyx_bootstrap_write_file_stamp_row(std::ostream& out, const std::filesystem::path& path) {
    const auto text = path.string();
    std::error_code ec;
    const auto ts = std::filesystem::last_write_time(path, ec);
    if (ec) {
        out << text << "=missing;";
        return;
    }
    const auto sz = std::filesystem::file_size(path, ec);
    out << text << "=mtime:" << static_cast<int64_t>(ts.time_since_epoch().count())
        << ",size:" << (ec ? -1LL : static_cast<long long>(sz)) << ";";
}

extern "C" VYX_RT_ABI int32_t vyx_bootstrap_write_vyi_stamp(const char* dep_name,
                                                            const char* manifest_path,
                                                            const char* primary_vyi_path,
                                                            const char* output_root,
                                                            const char* out_path) {
    if (!dep_name || !manifest_path || !primary_vyi_path || !output_root || !out_path || !*out_path) {
        return 1;
    }

    std::vector<std::filesystem::path> vyi_paths;
    std::error_code ec;
    const std::filesystem::path root_path(output_root);
    if (std::filesystem::is_directory(root_path, ec) && !ec) {
        const auto opts = std::filesystem::directory_options::skip_permission_denied;
        std::filesystem::recursive_directory_iterator it(root_path, opts, ec);
        const std::filesystem::recursive_directory_iterator end;
        while (!ec && it != end) {
            const auto& entry = *it;
            std::error_code entry_ec;
            if (entry.is_regular_file(entry_ec) && entry.path().extension() == ".vyi") {
                vyi_paths.push_back(entry.path());
            }
            it.increment(ec);
        }
        std::sort(vyi_paths.begin(), vyi_paths.end(),
                  [](const auto& a, const auto& b) { return a.string() < b.string(); });
    }

    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return 1;
    }
    out << "dep=" << dep_name << ";";
    vyx_bootstrap_write_file_stamp_row(out, std::filesystem::path(manifest_path));
    vyx_bootstrap_write_file_stamp_row(out, std::filesystem::path(primary_vyi_path));
    for (const auto& path : vyi_paths) {
        vyx_bootstrap_write_file_stamp_row(out, path);
    }
    return out ? 0 : 1;
}

extern "C" VYX_RT_ABI int32_t vyx_bootstrap_copy_files_with_ext(const char* src_dir,
                                                                const char* dst_dir,
                                                                const char* ext) {
    if (!src_dir || !*src_dir || !dst_dir || !*dst_dir || !ext || !*ext) {
        return 1;
    }

    std::error_code ec;
    const std::filesystem::path src_root(src_dir);
    const std::filesystem::path dst_root(dst_dir);
    if (!std::filesystem::is_directory(src_root, ec) || ec) {
        return 0;
    }
    std::filesystem::create_directories(dst_root, ec);
    if (ec) {
        return 1;
    }

    // Local manifest dependencies can already share the consumer's directory.
    // Compare filesystem identity, including symlinks and case on Windows.
    if (std::filesystem::equivalent(src_root, dst_root, ec)) return 0;
    if (ec) return 1;

    int32_t errors = 0;
    const auto opts = std::filesystem::directory_options::skip_permission_denied;
    std::filesystem::recursive_directory_iterator it(src_root, opts, ec);
    const std::filesystem::recursive_directory_iterator end;
    while (!ec && it != end) {
        const auto& entry = *it;
        std::error_code entry_ec;
        if (entry.is_regular_file(entry_ec) && entry.path().extension() == ext) {
            const auto dst = dst_root / entry.path().filename();
            std::error_code copy_ec;
            std::filesystem::copy_file(entry.path(),
                                       dst,
                                       std::filesystem::copy_options::overwrite_existing,
                                       copy_ec);
            if (copy_ec) {
                ++errors;
            }
        }
        it.increment(ec);
    }
    return errors == 0 ? 0 : 1;
}

static bool vyx_bootstrap_is_runtime_asset_extension(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext.empty()) {
        return true;
    }
    if (ext == ".dll" || ext == ".so" || ext == ".dylib") { return false; }
    if (ext == ".exe" || ext == ".lib" || ext == ".a") { return false; }
    if (ext == ".obj" || ext == ".o" || ext == ".bc" || ext == ".ll") { return false; }
    if (ext == ".vyi" || ext == ".pdb" || ext == ".ilk" || ext == ".exp" || ext == ".def") { return false; }
    if (ext == ".tmp" || ext == ".stamp") { return false; }
    return true;
}

extern "C" VYX_RT_ABI int32_t vyx_bootstrap_copy_runtime_assets(const char* src_dir,
                                                                 const char* dst_dir) {
    if (!src_dir || !*src_dir || !dst_dir || !*dst_dir) {
        return 1;
    }

    std::error_code ec;
    const std::filesystem::path src_root(src_dir);
    const std::filesystem::path dst_root(dst_dir);
    if (!std::filesystem::is_directory(src_root, ec) || ec) {
        return 0;
    }
    std::filesystem::create_directories(dst_root, ec);
    if (ec) {
        return 1;
    }

    if (std::filesystem::equivalent(src_root, dst_root, ec)) return 0;
    if (ec) return 1;

    int32_t errors = 0;
    const auto opts = std::filesystem::directory_options::skip_permission_denied;
    std::filesystem::recursive_directory_iterator it(src_root, opts, ec);
    const std::filesystem::recursive_directory_iterator end;
    while (!ec && it != end) {
        const auto& entry = *it;
        std::error_code entry_ec;
        if (entry.is_directory(entry_ec)) {
            const auto name = entry.path().filename().string();
            if (name == ".cache" || name == ".vyx_interfaces") {
                it.disable_recursion_pending();
            }
        } else if (entry.is_regular_file(entry_ec)
                   && vyx_bootstrap_is_runtime_asset_extension(entry.path())) {
            std::error_code rel_ec;
            const auto rel = std::filesystem::relative(entry.path(), src_root, rel_ec);
            if (rel_ec) {
                ++errors;
            } else {
                const auto dst = dst_root / rel;
                std::error_code mk_ec;
                std::filesystem::create_directories(dst.parent_path(), mk_ec);
                if (mk_ec) {
                    ++errors;
                } else {
                    std::error_code copy_ec;
                    std::filesystem::copy_file(entry.path(),
                                               dst,
                                               std::filesystem::copy_options::overwrite_existing,
                                               copy_ec);
                    if (copy_ec) {
                        ++errors;
                    }
                }
            }
        }
        it.increment(ec);
    }
    return errors == 0 ? 0 : 1;
}

extern "C" VYX_RT_ABI int32_t vyx_bootstrap_list_dirs(const char* root, const char* out_path) {
    if (!root || !*root || !out_path || !*out_path) {
        return 1;
    }

    std::error_code ec;
    const std::filesystem::path root_path(root);
    if (!std::filesystem::is_directory(root_path, ec)) {
        std::ofstream empty(out_path, std::ios::binary | std::ios::trunc);
        return empty ? 0 : 1;
    }

    std::vector<std::string> rows;
    const auto opts = std::filesystem::directory_options::skip_permission_denied;
    std::filesystem::directory_iterator it(root_path, opts, ec);
    const std::filesystem::directory_iterator end;
    while (!ec && it != end) {
        const auto& entry = *it;
        std::error_code entry_ec;
        if (entry.is_directory(entry_ec)) {
            rows.push_back(entry.path().string());
        }
        it.increment(ec);
    }
    std::sort(rows.begin(), rows.end());

    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return 1;
    }
    for (const auto& row : rows) {
        out << row << '\n';
    }
    return 0;
}

extern "C" VYX_RT_ABI int64_t vyx_bootstrap_file_mtime(const char* path) {
    if (!path || !*path) {
        return -1;
    }
    std::error_code ec;
    auto ts = std::filesystem::last_write_time(std::filesystem::path(path), ec);
    if (ec) {
        return -1;
    }
    return static_cast<int64_t>(ts.time_since_epoch().count());
}

extern "C" VYX_RT_ABI char* vyx_bootstrap_file_stamp(const char* path) {
    if (!path || !*path) {
        return vyx_rt_dup_cstr("=missing;");
    }
    const std::filesystem::path fs_path(path);
    std::error_code ec;
    if (!std::filesystem::exists(fs_path, ec) || ec) {
        std::string missing(path);
        missing += "=missing;";
        return vyx_rt_dup_bytes(missing.data(), missing.size());
    }
    ec.clear();
    const auto ts = std::filesystem::last_write_time(fs_path, ec);
    const auto mtime = ec ? static_cast<int64_t>(-1)
                          : static_cast<int64_t>(ts.time_since_epoch().count());
    ec.clear();
    const auto size = std::filesystem::file_size(fs_path, ec);
    const auto file_size = ec ? static_cast<uintmax_t>(0) : size;
    std::string out(path);
    out += "=mtime:";
    out += std::to_string(mtime);
    out += ",size:";
    out += std::to_string(file_size);
    out += ";";
    return vyx_rt_dup_bytes(out.data(), out.size());
}

// Content-addressed file stamp for incremental Descriptor caching (SPEC 16.5).
// Unlike vyx_bootstrap_file_stamp, which reports only mtime and size, this reads
// the file's bytes and folds them into a stable FNV-1a 64 digest. Binary inputs
// such as canonical .dcib contracts contain embedded NULs, so a content stamp
// cannot go through the NUL-terminated string readers; it must hash raw bytes.
extern "C" VYX_RT_ABI char* vyx_bootstrap_file_content_stamp(const char* path) {
    if (!path || !*path) {
        return vyx_rt_dup_cstr("=missing;");
    }
    const std::filesystem::path fs_path(path);
    std::error_code ec;
    if (!std::filesystem::exists(fs_path, ec) || ec) {
        std::string missing(path);
        missing += "=missing;";
        return vyx_rt_dup_bytes(missing.data(), missing.size());
    }
    std::ifstream stream(fs_path, std::ios::binary);
    if (!stream) {
        std::string missing(path);
        missing += "=unreadable;";
        return vyx_rt_dup_bytes(missing.data(), missing.size());
    }
    uint64_t hash = 1469598103934665603ULL; // FNV-1a offset basis
    uint64_t byte_count = 0;
    char buffer[65536];
    while (stream) {
        stream.read(buffer, sizeof(buffer));
        const std::streamsize got = stream.gcount();
        for (std::streamsize i = 0; i < got; ++i) {
            hash ^= static_cast<uint8_t>(buffer[i]);
            hash *= 1099511628211ULL; // FNV-1a prime
        }
        byte_count += static_cast<uint64_t>(got);
    }
    if (stream.bad()) {
        std::string missing(path);
        missing += "=unreadable;";
        return vyx_rt_dup_bytes(missing.data(), missing.size());
    }
    std::string out("bytes:");
    out += std::to_string(byte_count);
    out += ",fnv1a64:";
    out += std::to_string(hash);
    out += ";";
    return vyx_rt_dup_bytes(out.data(), out.size());
}

// Stable SHA-256 helper for serialized compiler artifacts.  Bootstrap Vyx
// code passes an explicit byte length because template payloads are text
// assembled in managed strings and their identity must include every byte,
// including any embedded NUL should the format grow binary sections later.
extern "C" VYX_RT_ABI char* vyx_bootstrap_sha256_bytes(const char* data,
                                                         int64_t len) {
    if (len < 0) {
        return vyx_rt_dup_cstr("invalid-length");
    }
    if (len > 0 && data == nullptr) {
        return vyx_rt_dup_cstr("null-input");
    }
    llvm::SHA256 sha;
    sha.update(llvm::StringRef(data ? data : "", static_cast<size_t>(len)));
    const auto digest = sha.final();
    static constexpr char hex[] = "0123456789abcdef";
    char text[65]{};
    for (size_t i = 0; i < digest.size(); ++i) {
        text[i * 2] = hex[(digest[i] >> 4) & 0x0f];
        text[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    return vyx_rt_dup_bytes(text, 64);
}

extern "C" VYX_RT_ABI int32_t vyx_bootstrap_hw_threads() {
    unsigned n = std::thread::hardware_concurrency();
    if (n == 0) {
        return 1;
    }
    if (n > static_cast<unsigned>(INT32_MAX)) {
        return INT32_MAX;
    }
    return static_cast<int32_t>(n);
}

static bool vyx_bootstrap_is_env_name_char(char c) {
    const auto uc = static_cast<unsigned char>(c);
    return std::isalnum(uc) != 0 || c == '_';
}

static std::string vyx_bootstrap_expand_path_env(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size();) {
        std::size_t name_begin = 0;
        std::size_t name_end = 0;
        std::size_t token_end = 0;
        if (value[i] == '$' && i + 2 < value.size() && value[i + 1] == '{') {
            const auto close = value.find('}', i + 2);
            if (close != std::string_view::npos) {
                name_begin = i + 2;
                name_end = close;
                token_end = close + 1;
            }
        } else if (value[i] == '$' && i + 1 < value.size()
                   && vyx_bootstrap_is_env_name_char(value[i + 1])) {
            name_begin = i + 1;
            name_end = name_begin;
            while (name_end < value.size()
                   && vyx_bootstrap_is_env_name_char(value[name_end])) {
                ++name_end;
            }
            token_end = name_end;
        } else if (value[i] == '%') {
            const auto close = value.find('%', i + 1);
            if (close != std::string_view::npos) {
                name_begin = i + 1;
                name_end = close;
                token_end = close + 1;
            }
        }

        if (token_end != 0 && name_end > name_begin) {
            const std::string name(value.substr(name_begin, name_end - name_begin));
            if (const char* env = std::getenv(name.c_str()); env && env[0] != '\0') {
                out += env;
                i = token_end;
                continue;
            }
        }
        out.push_back(value[i]);
        ++i;
    }
    return out;
}

static char* vyx_rt_dup_malloc_bytes(const char* s, uint64_t len) {
    if (!s || len == 0) {
        auto* out = static_cast<char*>(vyx_rt_service_alloc(1));
        if (out) {
            out[0] = '\0';
            vyx_rt_note_string_len_cached(out, 0);
        }
        return out;
    }
    auto* out = static_cast<char*>(vyx_rt_service_alloc(static_cast<std::size_t>(len + 1)));
    if (!out) { return nullptr; }
    std::memcpy(out, s, static_cast<std::size_t>(len));
    out[len] = '\0';
    vyx_rt_note_string_len_cached(out, len);
    return out;
}

static bool vyx_bootstrap_append_utf8(std::string& out, uint32_t cp) {
    if (cp > 0x10ffffu || (cp >= 0xd800u && cp <= 0xdfffu)) {
        return false;
    }
    if (cp <= 0x7fu) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7ffu) {
        out.push_back(static_cast<char>(0xc0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3fu)));
    } else if (cp <= 0xffffu) {
        out.push_back(static_cast<char>(0xe0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3fu)));
    } else {
        out.push_back(static_cast<char>(0xf0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3fu)));
    }
    return true;
}

static int vyx_bootstrap_hex_digit(char c) {
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}

static bool vyx_bootstrap_parse_toml_basic_string(std::string_view text,
                                                   std::size_t& pos,
                                                   std::string& out) {
    if (pos >= text.size() || text[pos] != '"') {
        return false;
    }
    ++pos;
    while (pos < text.size()) {
        const char c = text[pos++];
        if (c == '"') {
            return true;
        }
        if (c != '\\') {
            if (c == '\n' || c == '\r') {
                return false;
            }
            out.push_back(c);
            continue;
        }
        if (pos >= text.size()) {
            return false;
        }
        const char escaped = text[pos++];
        switch (escaped) {
        case 'b': out.push_back('\b'); break;
        case 't': out.push_back('\t'); break;
        case 'n': out.push_back('\n'); break;
        case 'f': out.push_back('\f'); break;
        case 'r': out.push_back('\r'); break;
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case 'u':
        case 'U': {
            const std::size_t digits = escaped == 'u' ? 4u : 8u;
            if (pos + digits > text.size()) {
                return false;
            }
            uint32_t cp = 0;
            for (std::size_t i = 0; i < digits; ++i) {
                const int nibble = vyx_bootstrap_hex_digit(text[pos++]);
                if (nibble < 0) {
                    return false;
                }
                cp = (cp << 4) | static_cast<uint32_t>(nibble);
            }
            if (!vyx_bootstrap_append_utf8(out, cp)) {
                return false;
            }
            break;
        }
        default:
            return false;
        }
    }
    return false;
}

static bool vyx_bootstrap_parse_toml_literal_string(std::string_view text,
                                                     std::size_t& pos,
                                                     std::string& out) {
    if (pos >= text.size() || text[pos] != '\'') {
        return false;
    }
    ++pos;
    const auto end = text.find('\'', pos);
    if (end == std::string_view::npos) {
        return false;
    }
    out.assign(text.substr(pos, end - pos));
    pos = end + 1;
    return true;
}

static void vyx_bootstrap_skip_toml_array_space(std::string_view text, std::size_t& pos) {
    while (pos < text.size()) {
        const auto c = static_cast<unsigned char>(text[pos]);
        if (std::isspace(c) != 0) {
            ++pos;
            continue;
        }
        if (text[pos] == '#') {
            while (pos < text.size() && text[pos] != '\n') {
                ++pos;
            }
            continue;
        }
        break;
    }
}

static bool vyx_bootstrap_parse_toml_string_array(std::string_view text,
                                                   std::vector<std::string>& values) {
    std::size_t pos = 0;
    vyx_bootstrap_skip_toml_array_space(text, pos);
    const bool bracketed = pos < text.size() && text[pos] == '[';
    if (bracketed) {
        ++pos;
    }
    while (true) {
        vyx_bootstrap_skip_toml_array_space(text, pos);
        if (pos >= text.size()) {
            return !bracketed;
        }
        if (bracketed && text[pos] == ']') {
            ++pos;
            vyx_bootstrap_skip_toml_array_space(text, pos);
            return pos == text.size();
        }

        std::string value;
        bool parsed = false;
        if (text[pos] == '"') {
            parsed = vyx_bootstrap_parse_toml_basic_string(text, pos, value);
        } else if (text[pos] == '\'') {
            parsed = vyx_bootstrap_parse_toml_literal_string(text, pos, value);
        } else {
            const auto begin = pos;
            while (pos < text.size() && text[pos] != ','
                   && (!bracketed || text[pos] != ']')
                   && std::isspace(static_cast<unsigned char>(text[pos])) == 0) {
                ++pos;
            }
            value.assign(text.substr(begin, pos - begin));
            parsed = !value.empty();
        }
        if (!parsed) {
            return false;
        }
        if (!value.empty()) {
            values.push_back(std::move(value));
        }

        vyx_bootstrap_skip_toml_array_space(text, pos);
        if (pos < text.size() && text[pos] == ',') {
            ++pos;
            continue;
        }
        if (bracketed && pos < text.size() && text[pos] == ']') {
            continue;
        }
        if (!bracketed && pos >= text.size()) {
            return true;
        }
        return false;
    }
}

static std::filesystem::path vyx_bootstrap_path_from_utf8(std::string_view value) {
#if defined(__cpp_char8_t)
    return std::filesystem::path(std::u8string_view(
        reinterpret_cast<const char8_t*>(value.data()), value.size()));
#else
    return std::filesystem::u8path(value.begin(), value.end());
#endif
}

static std::string vyx_bootstrap_path_to_utf8(const std::filesystem::path& value) {
    const auto bytes = value.u8string();
#if defined(__cpp_char8_t)
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
#else
    return bytes;
#endif
}

extern "C" VYX_RT_ABI const char* vyx_bootstrap_toml_array_abs_paths(
    const char* project_dir,
    int64_t project_dir_len,
    const char* array_text,
    int64_t array_text_len) {
    const auto bounded_view = [](const char* data, int64_t len) -> std::string_view {
        if (!data || len <= 0) {
            return {};
        }
        const auto unsigned_len = static_cast<uint64_t>(len);
        if (unsigned_len > static_cast<uint64_t>(std::numeric_limits<std::size_t>::max())) {
            return {};
        }
        return std::string_view(data, static_cast<std::size_t>(unsigned_len));
    };
    const auto project_dir_view = bounded_view(project_dir, project_dir_len);
    const auto array_text_view = bounded_view(array_text, array_text_len);

    std::vector<std::string> values;
    if (!vyx_bootstrap_parse_toml_string_array(array_text_view, values)) {
        return vyx_rt_empty_cstr();
    }

    std::error_code ec;
    auto cwd = std::filesystem::current_path(ec);
    if (ec) {
        cwd = std::filesystem::path(".");
        ec.clear();
    }
    auto base = !project_dir_view.empty()
        ? vyx_bootstrap_path_from_utf8(project_dir_view)
        : cwd;
    if (base.is_relative()) {
        base = cwd / base;
    }
    base = base.lexically_normal();

    std::string out;
    std::vector<std::string> seen;
    seen.reserve(values.size());
    for (const auto& raw : values) {
        const auto expanded = vyx_bootstrap_expand_path_env(raw);
        if (expanded.empty()) {
            continue;
        }
        auto path = vyx_bootstrap_path_from_utf8(expanded);
        if (path.is_relative()) {
            path = base / path;
        }
        path = path.lexically_normal().make_preferred();
        auto normalized = vyx_bootstrap_path_to_utf8(path);
        if (std::find(seen.begin(), seen.end(), normalized) != seen.end()) {
            continue;
        }
        seen.push_back(normalized);
        out += normalized;
        out.push_back('\n');
    }
    return vyx_rt_dup_bytes(out.data(), static_cast<uint64_t>(out.size()));
}

extern "C" VYX_RT_ABI const char* vyx_bootstrap_abs_path(const char* path) {
    std::filesystem::path p = (path && path[0]) ? std::filesystem::path(path)
                                                : std::filesystem::current_path();
    if (p.is_relative()) {
        p = std::filesystem::current_path() / p;
    }
    const auto out = p.lexically_normal().make_preferred().string();
    return vyx_rt_dup_bytes(out.data(), static_cast<uint64_t>(out.size()));
}

extern "C" VYX_RT_ABI const char* vyx_bootstrap_cwd() {
    static thread_local std::string out;
#ifdef _WIN32
    DWORD needed = GetCurrentDirectoryW(0, nullptr);
    if (needed == 0) {
        out.clear();
        return out.c_str();
    }
    std::vector<wchar_t> wide(static_cast<std::size_t>(needed) + 1u, L'\0');
    DWORD written = GetCurrentDirectoryW(needed, wide.data());
    if (written == 0) {
        out.clear();
        return out.c_str();
    }
    const int bytes = WideCharToMultiByte(CP_UTF8,
                                          0,
                                          wide.data(),
                                          static_cast<int>(written),
                                          nullptr,
                                          0,
                                          nullptr,
                                          nullptr);
    if (bytes <= 0) {
        out.clear();
        return out.c_str();
    }
    out.assign(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8,
                        0,
                        wide.data(),
                        static_cast<int>(written),
                        out.data(),
                        bytes,
                        nullptr,
                        nullptr);
#else
    std::error_code ec;
    auto cwd = std::filesystem::current_path(ec);
    out = ec ? std::string() : cwd.lexically_normal().string();
#endif
    return out.c_str();
}

struct VyxBootstrapProcess {
#ifdef _WIN32
    PROCESS_INFORMATION pi{};
    HANDLE job = nullptr;
#else
    pid_t pid = -1;
    pid_t pgid = -1;
#endif
    int32_t exit_code = -1;
    bool finished = false;
    // Direct-child resident high-water mark. Kept until process_close; -1
    // means no successful observation. This is not process-tree accounting.
    int64_t peak_resident_bytes = -1;
    int64_t peak_private_bytes = -1;
    int64_t cpu_time_us = -1;
    int64_t tree_peak_private_bytes = -1;
    int64_t tree_cpu_time_us = -1;

    ~VyxBootstrapProcess() {
#ifdef _WIN32
        if (job) {
            CloseHandle(job);
            job = nullptr;
        }
        if (pi.hThread) {
            CloseHandle(pi.hThread);
            pi.hThread = nullptr;
        }
        if (pi.hProcess) {
            CloseHandle(pi.hProcess);
            pi.hProcess = nullptr;
        }
#endif
    }
};

static std::mutex& vyx_bootstrap_process_mutex() {
    static std::mutex mutex;
    return mutex;
}

static std::unordered_map<int64_t, std::unique_ptr<VyxBootstrapProcess>>& vyx_bootstrap_processes() {
    static std::unordered_map<int64_t, std::unique_ptr<VyxBootstrapProcess>> processes;
    return processes;
}

static int64_t& vyx_bootstrap_next_process_id() {
    static int64_t next_id = 1;
    return next_id;
}

static std::vector<std::string> vyx_bootstrap_split_command_line(const std::string& command) {
    std::vector<std::string> args;
    std::string cur;
    bool in_quotes = false;
    for (char c : command) {
        if (c == '"') {
            in_quotes = !in_quotes;
            continue;
        }
        if (!in_quotes && std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) {
                args.push_back(cur);
                cur.clear();
            }
            continue;
        }
        cur.push_back(c);
    }
    if (!cur.empty()) {
        args.push_back(cur);
    }
    return args;
}

static bool vyx_bootstrap_env_name_char(char c) {
    const unsigned char uc = static_cast<unsigned char>(c);
    return std::isalnum(uc) || c == '_';
}

static std::string vyx_bootstrap_env_value(std::string_view name) {
    if (name.empty()) {
        return {};
    }
    std::string key(name);
#ifdef _WIN32
    DWORD needed = GetEnvironmentVariableA(key.c_str(), nullptr, 0);
    if (needed == 0) {
        return {};
    }
    std::vector<char> buf(static_cast<std::size_t>(needed), '\0');
    DWORD written = GetEnvironmentVariableA(key.c_str(), buf.data(), needed);
    if (written == 0 || written >= needed) {
        return {};
    }
    return std::string(buf.data(), static_cast<std::size_t>(written));
#else
    const char* value = std::getenv(key.c_str());
    return value ? std::string(value) : std::string();
#endif
}

static std::string vyx_bootstrap_expand_environment_variables(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const char c = text[i];
        if (c == '$' && i + 1u < text.size()) {
            if (text[i + 1u] == '{') {
                const std::size_t end = text.find('}', i + 2u);
                if (end != std::string::npos) {
                    const std::string value = vyx_bootstrap_env_value(
                        std::string_view(text).substr(i + 2u, end - (i + 2u)));
                    if (!value.empty()) {
                        out += value;
                        i = end + 1u;
                        continue;
                    }
                }
            } else if (vyx_bootstrap_env_name_char(text[i + 1u])) {
                std::size_t end = i + 1u;
                while (end < text.size() && vyx_bootstrap_env_name_char(text[end])) {
                    ++end;
                }
                const std::string value = vyx_bootstrap_env_value(
                    std::string_view(text).substr(i + 1u, end - (i + 1u)));
                if (!value.empty()) {
                    out += value;
                    i = end;
                    continue;
                }
            }
        }
        if (c == '%') {
            const std::size_t end = text.find('%', i + 1u);
            if (end != std::string::npos && end > i + 1u) {
                const std::string value = vyx_bootstrap_env_value(
                    std::string_view(text).substr(i + 1u, end - (i + 1u)));
                if (!value.empty()) {
                    out += value;
                    i = end + 1u;
                    continue;
                }
            }
        }
        out.push_back(c);
        ++i;
    }
    return out;
}

#ifdef _WIN32
static bool vyx_bootstrap_windows_file_exists(const std::string& path) {
    DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static std::string vyx_bootstrap_windows_executable_path(const std::string& path) {
    if (vyx_bootstrap_windows_file_exists(path)) {
        return path;
    }
    std::string lower = path;
    for (char& c : lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    const bool has_exe_suffix = lower.size() >= 4u
        && lower.compare(lower.size() - 4u, 4u, ".exe") == 0;
    if (!has_exe_suffix) {
        std::string with_exe = path + ".exe";
        if (vyx_bootstrap_windows_file_exists(with_exe)) {
            return with_exe;
        }
    }
    return path;
}

static std::string vyx_bootstrap_windows_quote_arg(const std::string& arg) {
    std::string out;
    out.push_back('"');
    std::size_t backslashes = 0;
    for (char c : arg) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        if (c == '"') {
            out.append(backslashes * 2u + 1u, '\\');
            out.push_back('"');
            backslashes = 0;
            continue;
        }
        if (backslashes > 0) {
            out.append(backslashes, '\\');
            backslashes = 0;
        }
        out.push_back(c);
    }
    if (backslashes > 0) {
        out.append(backslashes * 2u, '\\');
    }
    out.push_back('"');
    return out;
}

static std::string vyx_bootstrap_windows_command_line(const std::vector<std::string>& argv) {
    std::string out;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i != 0) {
            out.push_back(' ');
        }
        out += vyx_bootstrap_windows_quote_arg(argv[i]);
    }
    return out;
}

#ifdef _WIN32
static void vyx_bootstrap_rewrite_long_export_root_names(std::vector<std::string>& argv) {
    constexpr std::size_t kLimit = 8000;
    for (std::size_t i = 0; i + 1 < argv.size(); ++i) {
        if (argv[i] != "--export-root-names") {
            continue;
        }
        if (argv[i + 1].size() <= kLimit) {
            continue;
        }
        char dir[MAX_PATH];
        const DWORD n = GetTempPathA(MAX_PATH, dir);
        if (n == 0 || n >= MAX_PATH) {
            return;
        }
        char path[MAX_PATH];
        if (GetTempFileNameA(dir, "vyx", 0, path) == 0) {
            return;
        }
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            return;
        }
        out.write(argv[i + 1].data(), static_cast<std::streamsize>(argv[i + 1].size()));
        if (!out) {
            return;
        }
        argv[i] = "--export-root-names-file";
        argv[i + 1] = path;
    }
}
#endif

static bool vyx_bootstrap_windows_job_disabled() {
    const std::string raw = vyx_bootstrap_env_value("VYX_BOOTSTRAP_DISABLE_JOB_OBJECT");
    return raw == "1" || raw == "true" || raw == "TRUE" || raw == "True";
}

static uint64_t vyx_bootstrap_windows_memory_limit_bytes(const char* env_name,
                                                         uint64_t default_mb) {
    constexpr uint64_t mib = 1024u * 1024u;
    const uint64_t max_mb = static_cast<uint64_t>(std::numeric_limits<SIZE_T>::max()) / mib;
    const uint64_t safe_default_mb = std::min(default_mb, max_mb);
    const std::string raw = vyx_bootstrap_env_value(env_name);
    if (raw.empty()) {
        return safe_default_mb * mib;
    }

    uint64_t mb = 0;
    bool valid = true;
    for (char c : raw) {
        if (c < '0' || c > '9') {
            valid = false;
            break;
        }
        const uint64_t digit = static_cast<uint64_t>(c - '0');
        if (mb > (max_mb - digit) / 10u) {
            valid = false;
            break;
        }
        mb = mb * 10u + digit;
    }
    if (!valid) {
        std::fprintf(stderr,
                     "warning: invalid %s='%s'; using %llu MiB\n",
                     env_name,
                     raw.c_str(),
                     static_cast<unsigned long long>(safe_default_mb));
        return safe_default_mb * mib;
    }
    return mb * mib;
}

static uint64_t vyx_bootstrap_windows_child_memory_limit_bytes() {
    // Job objects group descendants for cancellation and crash cleanup. They
    // must not impose a hidden compiler heap ceiling; the Vyx scheduler owns
    // concurrency and memory budgeting. A limit remains available as an
    // explicit diagnostics override only.
    return vyx_bootstrap_windows_memory_limit_bytes("VYX_BOOTSTRAP_CHILD_MEMORY_MB", 0u);
}

static uint64_t vyx_bootstrap_windows_total_child_memory_limit_bytes() {
    return vyx_bootstrap_windows_memory_limit_bytes(
        "VYX_BOOTSTRAP_TOTAL_CHILD_MEMORY_MB",
        0u);
}

static HANDLE vyx_bootstrap_windows_create_child_job(DWORD& error) {
    error = ERROR_SUCCESS;
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) {
        error = GetLastError();
        return nullptr;
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    // The aggregate job remains kill-on-close for crash cleanup. Per-process
    // jobs are cancelled explicitly so a normal close cannot kill descendants
    // that are still completing after their launcher exits successfully.
    info.BasicLimitInformation.LimitFlags = 0;
    const uint64_t limit = vyx_bootstrap_windows_child_memory_limit_bytes();
    if (limit > 0) {
        const uint64_t size_max = static_cast<uint64_t>(std::numeric_limits<SIZE_T>::max());
        const SIZE_T native_limit = static_cast<SIZE_T>(std::min(limit, size_max));
        info.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY
                                               | JOB_OBJECT_LIMIT_JOB_MEMORY;
        info.ProcessMemoryLimit = native_limit;
        info.JobMemoryLimit = native_limit;
    }
    if (!SetInformationJobObject(job,
                                 JobObjectExtendedLimitInformation,
                                 &info,
                                 static_cast<DWORD>(sizeof(info)))) {
        error = GetLastError();
        CloseHandle(job);
        return nullptr;
    }
    return job;
}

static bool vyx_bootstrap_windows_wait_process(VyxBootstrapProcess& proc,
                                               DWORD timeout_ms,
                                               DWORD& error) {
    error = ERROR_SUCCESS;
    if (!proc.pi.hProcess) {
        error = ERROR_INVALID_HANDLE;
        return false;
    }
    const DWORD wait_result = WaitForSingleObject(proc.pi.hProcess, timeout_ms);
    if (wait_result != WAIT_OBJECT_0) {
        error = wait_result == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT;
        return false;
    }
    DWORD exit_code = 0;
    if (!GetExitCodeProcess(proc.pi.hProcess, &exit_code)) {
        error = GetLastError();
        return false;
    }
    proc.exit_code = static_cast<int32_t>(exit_code);
    proc.finished = true;
    return true;
}

static bool vyx_bootstrap_windows_wait_job_empty(HANDLE job,
                                                 DWORD timeout_ms,
                                                 DWORD& error) {
    error = ERROR_SUCCESS;
    if (!job) {
        return true;
    }
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeout_ms);
    for (;;) {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info{};
        if (!QueryInformationJobObject(job,
                                       JobObjectBasicAccountingInformation,
                                       &info,
                                       static_cast<DWORD>(sizeof(info)),
                                       nullptr)) {
            error = GetLastError();
            return false;
        }
        if (info.ActiveProcesses == 0) {
            return true;
        }
        if (GetTickCount64() >= deadline) {
            error = ERROR_TIMEOUT;
            return false;
        }
        Sleep(10);
    }
}

struct VyxBootstrapAggregateChildJob {
    HANDLE handle = nullptr;
    DWORD error = ERROR_SUCCESS;

    VyxBootstrapAggregateChildJob() {
        handle = CreateJobObjectW(nullptr, nullptr);
        if (!handle) {
            error = GetLastError();
            return;
        }

        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        const uint64_t limit = vyx_bootstrap_windows_total_child_memory_limit_bytes();
        if (limit > 0) {
            const uint64_t size_max = static_cast<uint64_t>(std::numeric_limits<SIZE_T>::max());
            info.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_JOB_MEMORY;
            info.JobMemoryLimit = static_cast<SIZE_T>(std::min(limit, size_max));
        }
        if (!SetInformationJobObject(handle,
                                     JobObjectExtendedLimitInformation,
                                     &info,
                                     static_cast<DWORD>(sizeof(info)))) {
            error = GetLastError();
            CloseHandle(handle);
            handle = nullptr;
        }
    }

    ~VyxBootstrapAggregateChildJob() {
        if (handle) {
            CloseHandle(handle);
            handle = nullptr;
        }
    }

    VyxBootstrapAggregateChildJob(const VyxBootstrapAggregateChildJob&) = delete;
    VyxBootstrapAggregateChildJob& operator=(const VyxBootstrapAggregateChildJob&) = delete;
};

static VyxBootstrapAggregateChildJob& vyx_bootstrap_windows_aggregate_child_job() {
    // Initialization is synchronized, and intentionally leaving this one
    // allocation to process cleanup avoids static-destruction ordering races
    // with the process registry. The OS closes the handle during process teardown.
    static VyxBootstrapAggregateChildJob* const job = new VyxBootstrapAggregateChildJob();
    return *job;
}

// The bootstrap executable imports `printf` from the private compiler backend. MSVC's UCRT
// exposes the formatting implementation through internal entry points rather
// than an export named `printf`, so the module-definition file aliases that
// ABI name to this bridge instead of relying on a transitive CRT export.
extern "C" VYX_RT_ABI int vyx_rt_printf_bridge(const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int result = std::vprintf(format, args);
    va_end(args);
    return result;
}
#endif

static std::string vyx_bootstrap_ascii_lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

static std::vector<char> vyx_bootstrap_build_environment_block(const char* name,
                                                               const char* value) {
    std::vector<char> block;
    if (!name || !name[0]) {
        block.push_back('\0');
        block.push_back('\0');
        return block;
    }

#ifdef _WIN32
    std::vector<std::string> entries;
    bool replaced = false;
    const std::string key_lower = vyx_bootstrap_ascii_lower(name);
    char** env = _environ;
    if (env) {
        for (char** p = env; *p; ++p) {
            std::string item(*p);
            if (!item.empty() && item[0] != '=') {
                const std::size_t eq = item.find('=');
                if (eq != std::string::npos) {
                    const std::string item_key = vyx_bootstrap_ascii_lower(item.substr(0, eq));
                    if (item_key == key_lower) {
                        entries.push_back(std::string(name) + "=" + (value ? value : ""));
                        replaced = true;
                        continue;
                    }
                }
            }
            entries.push_back(item);
        }
    }
    if (!replaced) {
        entries.push_back(std::string(name) + "=" + (value ? value : ""));
    }
    std::size_t total = 1u;
    for (const auto& e : entries) {
        total += e.size() + 1u;
    }
    block.reserve(total);
    for (const auto& e : entries) {
        block.insert(block.end(), e.begin(), e.end());
        block.push_back('\0');
    }
    block.push_back('\0');
#else
    (void)value;
    block.push_back('\0');
    block.push_back('\0');
#endif
    return block;
}

#ifdef _WIN32
static bool vyx_bootstrap_open_output_handle(const char* path,
                                             HANDLE& handle) {
    handle = nullptr;
    if (!path || !path[0]) {
        return true;
    }
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;
    handle = CreateFileA(path,
                         GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE,
                         &sa,
                         CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL,
                         nullptr);
    return handle != INVALID_HANDLE_VALUE;
}
#endif

#ifndef _WIN32
static bool vyx_bootstrap_open_output_fd(const char* path, int& fd) {
    fd = -1;
    if (!path || !path[0]) {
        return true;
    }
    fd = ::open(path, O_CREAT | O_WRONLY | O_TRUNC, 0666);
    return fd >= 0;
}
#endif

static int64_t vyx_bootstrap_process_spawn_impl(std::vector<std::string> argv,
                                                          const char* cwd,
                                                          const char* stdout_path,
                                                          const char* stderr_path,
                                                          const char* env_name,
                                                          const char* env_value) {
    if (argv.empty() || argv[0].empty()) {
        return 0;
    }

    auto proc = std::make_unique<VyxBootstrapProcess>();
    const std::string expanded_cwd = cwd ? cwd : "";

#ifdef _WIN32
    auto vyx_bootstrap_utf8_to_wide = [](const std::string& s) -> std::vector<wchar_t> {
        if (s.empty()) {
            return std::vector<wchar_t>{L'\0'};
        }
        int needed = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        if (needed <= 0) {
            return std::vector<wchar_t>{L'\0'};
        }
        std::vector<wchar_t> out(static_cast<std::size_t>(needed), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), needed);
        return out;
    };
    if (argv.empty()) {
        return 0;
    }
    argv[0] = vyx_bootstrap_windows_executable_path(argv[0]);
    vyx_bootstrap_rewrite_long_export_root_names(argv);
    HANDLE out_handle = nullptr;
    HANDLE err_handle = nullptr;
    bool redirect_out = stdout_path && stdout_path[0];
    bool redirect_err = stderr_path && stderr_path[0];
    if (redirect_out && !vyx_bootstrap_open_output_handle(stdout_path, out_handle)) {
        return 0;
    }
    if (redirect_err) {
        if (stdout_path && stdout_path[0] && std::strcmp(stdout_path, stderr_path) == 0) {
            err_handle = out_handle;
        } else if (!vyx_bootstrap_open_output_handle(stderr_path, err_handle)) {
            if (out_handle) { CloseHandle(out_handle); }
            return 0;
        }
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const bool use_job = !vyx_bootstrap_windows_job_disabled();
    VyxBootstrapAggregateChildJob* aggregate_job = nullptr;
    DWORD job_error = ERROR_SUCCESS;
    HANDLE child_job = nullptr;
    if (use_job) {
        aggregate_job = &vyx_bootstrap_windows_aggregate_child_job();
        if (!aggregate_job->handle) {
            job_error = aggregate_job->error;
        } else {
            child_job = vyx_bootstrap_windows_create_child_job(job_error);
        }
    }
    if (use_job && (!aggregate_job || !aggregate_job->handle || !child_job)) {
        if (out_handle) { CloseHandle(out_handle); }
        if (err_handle && err_handle != out_handle) { CloseHandle(err_handle); }
        if (child_job) { CloseHandle(child_job); }
        std::fprintf(stderr, "error: cannot create bootstrap child process jobs (win32=%lu)\n",
                     static_cast<unsigned long>(job_error));
        return 0;
    }
    DWORD flags = (out_handle || err_handle) ? CREATE_NO_WINDOW : 0;
    if (use_job) {
        flags |= CREATE_SUSPENDED;
    }
    auto valid_handle = [](HANDLE h) {
        return h != nullptr && h != INVALID_HANDLE_VALUE;
    };
    HANDLE inherited_in = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE inherited_out = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE inherited_err = GetStdHandle(STD_ERROR_HANDLE);
    const bool use_std_handles = out_handle || err_handle
        || valid_handle(inherited_in)
        || valid_handle(inherited_out)
        || valid_handle(inherited_err);
    if (use_std_handles) {
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdInput = valid_handle(inherited_in) ? inherited_in : nullptr;
        si.hStdOutput = out_handle ? out_handle
            : (valid_handle(inherited_out) ? inherited_out : nullptr);
        si.hStdError = err_handle ? err_handle
            : (valid_handle(inherited_err) ? inherited_err : nullptr);
    }

    const char* cwd_arg = expanded_cwd.empty() ? nullptr : expanded_cwd.c_str();
    std::vector<char> env_block;
    LPVOID env_ptr = nullptr;
    if (env_name && env_name[0]) {
        env_block = vyx_bootstrap_build_environment_block(env_name, env_value);
        env_ptr = env_block.data();
    }
    const std::string windows_cmd = vyx_bootstrap_windows_command_line(argv);
    auto wide_app = vyx_bootstrap_utf8_to_wide(argv[0]);
    auto wide_cmd = vyx_bootstrap_utf8_to_wide(windows_cmd);
    auto wide_cwd = cwd_arg ? vyx_bootstrap_utf8_to_wide(cwd_arg) : std::vector<wchar_t>{};
    UINT previous_error_mode = GetErrorMode();
    SetErrorMode(previous_error_mode
                 | SEM_FAILCRITICALERRORS
                 | SEM_NOGPFAULTERRORBOX
                 | SEM_NOOPENFILEERRORBOX);
    BOOL ok = CreateProcessW(wide_app.data(),
                             wide_cmd.data(),
                             nullptr,
                             nullptr,
                             use_std_handles ? TRUE : FALSE,
                             flags,
                             env_ptr,
                             wide_cwd.empty() ? nullptr : wide_cwd.data(),
                             &si,
                             &pi);
    SetErrorMode(previous_error_mode);
    if (out_handle) { CloseHandle(out_handle); }
    if (err_handle && err_handle != out_handle) { CloseHandle(err_handle); }
    if (!ok) {
        if (child_job) { CloseHandle(child_job); }
        return 0;
    }
    if (use_job) {
        if (!AssignProcessToJobObject(aggregate_job->handle, pi.hProcess)) {
            const DWORD assign_error = GetLastError();
            const BOOL terminated = TerminateProcess(pi.hProcess, 1);
            const DWORD terminate_error = terminated ? ERROR_SUCCESS : GetLastError();
            const DWORD wait_result = WaitForSingleObject(pi.hProcess, 5000);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            CloseHandle(child_job);
            std::fprintf(stderr,
                         "error: cannot assign bootstrap child to aggregate job (win32=%lu); "
                         "cleanup terminate=%lu wait=%lu; "
                         "set VYX_BOOTSTRAP_DISABLE_JOB_OBJECT=1 only for unsupported hosts\n",
                         static_cast<unsigned long>(assign_error),
                         static_cast<unsigned long>(terminate_error),
                         static_cast<unsigned long>(wait_result));
            return 0;
        }
        if (!AssignProcessToJobObject(child_job, pi.hProcess)) {
            const DWORD assign_error = GetLastError();
            const BOOL terminated = TerminateProcess(pi.hProcess, 1);
            const DWORD terminate_error = terminated ? ERROR_SUCCESS : GetLastError();
            const DWORD wait_result = WaitForSingleObject(pi.hProcess, 5000);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            CloseHandle(child_job);
            std::fprintf(stderr,
                         "error: cannot assign bootstrap child to per-child job (win32=%lu); "
                         "cleanup terminate=%lu wait=%lu; "
                         "set VYX_BOOTSTRAP_DISABLE_JOB_OBJECT=1 only for unsupported hosts\n",
                         static_cast<unsigned long>(assign_error),
                         static_cast<unsigned long>(terminate_error),
                         static_cast<unsigned long>(wait_result));
            return 0;
        }
        if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) {
            const DWORD resume_error = GetLastError();
            const BOOL terminated = TerminateJobObject(child_job, 1);
            const DWORD terminate_error = terminated ? ERROR_SUCCESS : GetLastError();
            CloseHandle(child_job);
            child_job = nullptr;
            const DWORD wait_result = WaitForSingleObject(pi.hProcess, 5000);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            std::fprintf(stderr,
                         "error: cannot resume bootstrap child (win32=%lu); cleanup terminate=%lu wait=%lu\n",
                         static_cast<unsigned long>(resume_error),
                         static_cast<unsigned long>(terminate_error),
                         static_cast<unsigned long>(wait_result));
            return 0;
        }
    }
    CloseHandle(pi.hThread);
    pi.hThread = nullptr;
    proc->pi = pi;
    proc->job = child_job;
#else
    if (argv.empty()) {
        return 0;
    }

    int out_fd = -1;
    int err_fd = -1;
    if (!vyx_bootstrap_open_output_fd(stdout_path, out_fd)) {
        return 0;
    }
    if (stderr_path && stderr_path[0] && (!stdout_path || std::strcmp(stdout_path, stderr_path) != 0)) {
        if (!vyx_bootstrap_open_output_fd(stderr_path, err_fd)) {
            if (out_fd >= 0) { ::close(out_fd); }
            return 0;
        }
    } else {
        err_fd = out_fd;
    }

    pid_t pid = fork();
    if (pid < 0) {
        if (out_fd >= 0) { ::close(out_fd); }
        if (err_fd >= 0 && err_fd != out_fd) { ::close(err_fd); }
        return 0;
    }
    if (pid == 0) {
        if (::setpgid(0, 0) != 0) {
            _exit(126);
        }
        if (env_name && env_name[0]) {
            ::setenv(env_name, env_value ? env_value : "", 1);
        }
        if (!expanded_cwd.empty()) {
            ::chdir(expanded_cwd.c_str());
        }
        if (out_fd >= 0) {
            ::dup2(out_fd, STDOUT_FILENO);
        }
        if (err_fd >= 0) {
            ::dup2(err_fd, STDERR_FILENO);
        }
        if (out_fd >= 0) { ::close(out_fd); }
        if (err_fd >= 0 && err_fd != out_fd) { ::close(err_fd); }

        std::vector<char*> cargv;
        cargv.reserve(argv.size() + 1u);
        for (auto& s : argv) {
            cargv.push_back(const_cast<char*>(s.c_str()));
        }
        cargv.push_back(nullptr);
        ::execvp(cargv[0], cargv.data());
        _exit(127);
    }
    if (out_fd >= 0) { ::close(out_fd); }
    if (err_fd >= 0 && err_fd != out_fd) { ::close(err_fd); }
    int setpgid_rc = 0;
    do {
        setpgid_rc = ::setpgid(pid, pid);
    } while (setpgid_rc != 0 && errno == EINTR);
    if (setpgid_rc != 0 && errno != EACCES && errno != ESRCH) {
        const int setpgid_error = errno;
        (void)::kill(pid, SIGKILL);
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        std::fprintf(stderr,
                     "error: cannot create bootstrap child process group (errno=%d)\n",
                     setpgid_error);
        return 0;
    }
    proc->pid = pid;
    proc->pgid = pid;
#endif

    std::lock_guard<std::mutex> lock(vyx_bootstrap_process_mutex());
    int64_t id = vyx_bootstrap_next_process_id();
    vyx_bootstrap_next_process_id() = id + 1;
    vyx_bootstrap_processes().emplace(id, std::move(proc));
    return id;
}

extern "C" VYX_RT_ABI int64_t vyx_bootstrap_process_spawn(const char* command,
    const char* cwd, const char* stdout_path, const char* stderr_path,
    const char* env_name, const char* env_value) {
    if (!command) return 0;
    const std::string expanded_cwd = cwd
        ? vyx_bootstrap_expand_environment_variables(cwd) : "";
    return vyx_bootstrap_process_spawn_impl(
        vyx_bootstrap_split_command_line(vyx_bootstrap_expand_environment_variables(command)),
        expanded_cwd.c_str(), stdout_path, stderr_path, env_name, env_value);
}

extern "C" VYX_RT_ABI int64_t vyx_bootstrap_process_spawn_args(const char* args,
    const char* cwd, const char* stdout_path, const char* stderr_path,
    const char* env_name, const char* env_value) {
    if (!args) return 0;
    const std::string_view packed(args);
    std::vector<std::string> argv;
    std::size_t offset = 0;
    while (offset < packed.size()) {
        std::size_t length = 0;
        const std::size_t start = offset;
        while (offset < packed.size() && packed[offset] >= '0' && packed[offset] <= '9') {
            const unsigned digit = packed[offset++] - '0';
            if (length > packed.size() / 10 ||
                (length == packed.size() / 10 && digit > packed.size() % 10)) return 0;
            length = length * 10 + digit;
        }
        if (offset == start || offset == packed.size() || packed[offset++] != ':' ||
            length > packed.size() - offset) return 0;
        argv.emplace_back(packed.substr(offset, length));
        offset += length;
    }
    // On Windows compiler printf is exported by this DLL and uses its CRT.
    // Flush that stream before the child inherits stdout; flushing only the
    // executable's CRT would leave the build log behind the program output.
    std::fflush(nullptr);
    return vyx_bootstrap_process_spawn_impl(std::move(argv), cwd, stdout_path,
                                           stderr_path, env_name, env_value);
}

// Caller holds vyx_bootstrap_process_mutex(), which also guards handle close.
// Never add descendant usage here: each child owns only its own observation.
static int64_t vyx_bootstrap_sample_process_resident(VyxBootstrapProcess& proc) {
    if (proc.finished) { return 0; }
    int64_t resident = -1;
    int64_t peak = -1;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (!proc.pi.hProcess
        || !vyx_rt_query_windows_process_memory(proc.pi.hProcess, counters)) {
        return -1;
    }
    resident = static_cast<int64_t>(counters.WorkingSetSize);
    peak = static_cast<int64_t>(counters.PeakWorkingSetSize);
    proc.peak_private_bytes = std::max(proc.peak_private_bytes,
        static_cast<int64_t>(counters.PeakPagefileUsage));
#elif defined(__linux__)
    if (proc.pid <= 0) { return -1; }
    const std::string path = "/proc/" + std::to_string(proc.pid) + "/status";
    FILE* status = std::fopen(path.c_str(), "r");
    if (!status) { return -1; }
    char line[256];
    while (std::fgets(line, sizeof(line), status)) {
        unsigned long long kb = 0;
        const auto max_kb = static_cast<unsigned long long>(INT64_MAX / 1024);
        if (std::sscanf(line, "VmRSS: %llu kB", &kb) == 1 && kb <= max_kb) {
            resident = static_cast<int64_t>(kb * 1024);
        } else if (std::sscanf(line, "VmHWM: %llu kB", &kb) == 1 && kb <= max_kb) {
            peak = static_cast<int64_t>(kb * 1024);
        }
    }
    const bool read_failed = std::ferror(status) != 0;
    std::fclose(status);
    if (read_failed) { return -1; }
#else
    // An unsupported platform is an unknown observation, never zero usage.
    return -1;
#endif
    if (resident >= 0) {
        proc.peak_resident_bytes = std::max(proc.peak_resident_bytes, resident);
    }
    if (peak >= 0) {
        proc.peak_resident_bytes = std::max(proc.peak_resident_bytes, peak);
    }
    return resident;
}

extern "C" VYX_RT_ABI int64_t vyx_bootstrap_process_resident_bytes(int64_t id) {
    std::lock_guard<std::mutex> lock(vyx_bootstrap_process_mutex());
    auto it = vyx_bootstrap_processes().find(id);
    if (it == vyx_bootstrap_processes().end() || !it->second) { return -1; }
    return vyx_bootstrap_sample_process_resident(*it->second);
}

extern "C" VYX_RT_ABI int64_t vyx_bootstrap_process_peak_resident_bytes(int64_t id) {
    std::lock_guard<std::mutex> lock(vyx_bootstrap_process_mutex());
    auto it = vyx_bootstrap_processes().find(id);
    if (it == vyx_bootstrap_processes().end() || !it->second) { return -1; }
    auto& proc = *it->second;
    (void)vyx_bootstrap_sample_process_resident(proc);
    return proc.peak_resident_bytes;
}

// All samplers below require vyx_bootstrap_process_mutex(). Direct-process
// measurements and job-tree measurements are intentionally distinct: a root
// can finish while its descendants still own memory and consume CPU.
static int64_t vyx_bootstrap_sample_process_private(VyxBootstrapProcess& proc) {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (proc.pi.hProcess
        && vyx_rt_query_windows_process_memory(proc.pi.hProcess, counters)) {
        proc.peak_private_bytes = std::max(proc.peak_private_bytes,
            static_cast<int64_t>(counters.PeakPagefileUsage));
        return proc.finished ? 0 : static_cast<int64_t>(counters.PrivateUsage);
    }
    return proc.finished ? 0 : -1;
#else
    // VmRSS, VmData and private resident pages are not private commit.
    (void)proc;
    return -1;
#endif
}

static int64_t vyx_bootstrap_sample_process_cpu(VyxBootstrapProcess& proc) {
#ifdef _WIN32
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!proc.pi.hProcess
        || !GetProcessTimes(proc.pi.hProcess, &created, &exited, &kernel, &user)) {
        return -1;
    }
    const uint64_t kernel_ticks = (static_cast<uint64_t>(kernel.dwHighDateTime) << 32)
        | kernel.dwLowDateTime;
    const uint64_t user_ticks = (static_cast<uint64_t>(user.dwHighDateTime) << 32)
        | user.dwLowDateTime;
    if (kernel_ticks > UINT64_MAX - user_ticks) { return -1; }
    proc.cpu_time_us = std::max(proc.cpu_time_us,
        static_cast<int64_t>((kernel_ticks + user_ticks) / 10));
    return proc.cpu_time_us;
#else
    // A final direct-child CPU counter needs a platform-specific lifecycle
    // contract. Do not substitute wall time or cumulative waited-child time.
    (void)proc;
    return -1;
#endif
}

static int64_t vyx_bootstrap_sample_process_tree_private(VyxBootstrapProcess& proc) {
#ifdef _WIN32
    if (!proc.job) { return -1; }
    // The Windows SDK omits this info class. Microsoft's hcsshim uses the
    // same OS query/ABI for container job commit accounting:
    // https://github.com/microsoft/hcsshim/blob/main/internal/winapi/jobobject.go
    // https://github.com/microsoft/hcsshim/blob/main/internal/jobobject/jobobject.go
    // An OS that does not support it returns unknown; never sum working sets
    // or replace this tree observation with the root process's usage.
    constexpr auto job_object_memory_usage_information = static_cast<JOBOBJECTINFOCLASS>(28);
    struct JobMemoryUsageInformation {
        uint64_t job_memory;
        uint64_t peak_job_memory;
    };
    JobMemoryUsageInformation memory{};
    if (QueryInformationJobObject(proc.job, job_object_memory_usage_information,
                                  &memory, sizeof(memory), nullptr)) {
        if (memory.job_memory > INT64_MAX || memory.peak_job_memory > INT64_MAX) {
            return -1;
        }
        proc.tree_peak_private_bytes = std::max(proc.tree_peak_private_bytes,
            static_cast<int64_t>(memory.peak_job_memory));
        return static_cast<int64_t>(memory.job_memory);
    }
    // PeakJobMemoryUsed is also available through the public SDK even when
    // the OS cannot supply current job memory. Current remains unknown.
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    if (QueryInformationJobObject(proc.job, JobObjectExtendedLimitInformation,
                                  &limits, sizeof(limits), nullptr)
        && limits.PeakJobMemoryUsed <= static_cast<SIZE_T>(INT64_MAX)) {
        proc.tree_peak_private_bytes = std::max(proc.tree_peak_private_bytes,
            static_cast<int64_t>(limits.PeakJobMemoryUsed));
    }
    return -1;
#else
    (void)proc;
    return -1;
#endif
}

static int64_t vyx_bootstrap_sample_process_tree_cpu(VyxBootstrapProcess& proc) {
#ifdef _WIN32
    if (!proc.job) { return -1; }
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
    if (!QueryInformationJobObject(proc.job, JobObjectBasicAccountingInformation,
                                   &accounting, sizeof(accounting), nullptr)) {
        return -1;
    }
    const int64_t kernel = accounting.TotalKernelTime.QuadPart;
    const int64_t user = accounting.TotalUserTime.QuadPart;
    if (kernel < 0 || user < 0 || kernel > INT64_MAX - user) { return -1; }
    proc.tree_cpu_time_us = std::max(proc.tree_cpu_time_us, (kernel + user) / 10);
    return proc.tree_cpu_time_us;
#else
    (void)proc;
    return -1;
#endif
}

extern "C" VYX_RT_ABI int64_t vyx_bootstrap_process_private_bytes(int64_t id) {
    std::lock_guard<std::mutex> lock(vyx_bootstrap_process_mutex());
    auto it = vyx_bootstrap_processes().find(id);
    if (it == vyx_bootstrap_processes().end() || !it->second) { return -1; }
    return vyx_bootstrap_sample_process_private(*it->second);
}

extern "C" VYX_RT_ABI int64_t vyx_bootstrap_process_peak_private_bytes(int64_t id) {
    std::lock_guard<std::mutex> lock(vyx_bootstrap_process_mutex());
    auto it = vyx_bootstrap_processes().find(id);
    if (it == vyx_bootstrap_processes().end() || !it->second) { return -1; }
    auto& proc = *it->second;
    (void)vyx_bootstrap_sample_process_private(proc);
    return proc.peak_private_bytes;
}

extern "C" VYX_RT_ABI int64_t vyx_bootstrap_process_cpu_time_us(int64_t id) {
    std::lock_guard<std::mutex> lock(vyx_bootstrap_process_mutex());
    auto it = vyx_bootstrap_processes().find(id);
    if (it == vyx_bootstrap_processes().end() || !it->second) { return -1; }
    return vyx_bootstrap_sample_process_cpu(*it->second);
}

extern "C" VYX_RT_ABI int64_t vyx_bootstrap_process_tree_private_bytes(int64_t id) {
    std::lock_guard<std::mutex> lock(vyx_bootstrap_process_mutex());
    auto it = vyx_bootstrap_processes().find(id);
    if (it == vyx_bootstrap_processes().end() || !it->second) { return -1; }
    return vyx_bootstrap_sample_process_tree_private(*it->second);
}

extern "C" VYX_RT_ABI int64_t vyx_bootstrap_process_tree_peak_private_bytes(int64_t id) {
    std::lock_guard<std::mutex> lock(vyx_bootstrap_process_mutex());
    auto it = vyx_bootstrap_processes().find(id);
    if (it == vyx_bootstrap_processes().end() || !it->second) { return -1; }
    auto& proc = *it->second;
    (void)vyx_bootstrap_sample_process_tree_private(proc);
    return proc.tree_peak_private_bytes;
}

extern "C" VYX_RT_ABI int64_t vyx_bootstrap_process_tree_cpu_time_us(int64_t id) {
    std::lock_guard<std::mutex> lock(vyx_bootstrap_process_mutex());
    auto it = vyx_bootstrap_processes().find(id);
    if (it == vyx_bootstrap_processes().end() || !it->second) { return -1; }
    return vyx_bootstrap_sample_process_tree_cpu(*it->second);
}

extern "C" VYX_RT_ABI int64_t vyx_bootstrap_process_tree_active_count(int64_t id) {
    std::lock_guard<std::mutex> lock(vyx_bootstrap_process_mutex());
    auto it = vyx_bootstrap_processes().find(id);
    if (it == vyx_bootstrap_processes().end() || !it->second) { return -1; }
#ifdef _WIN32
    auto& proc = *it->second;
    if (!proc.job) { return -1; }
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
    if (!QueryInformationJobObject(proc.job, JobObjectBasicAccountingInformation,
                                   &accounting, sizeof(accounting), nullptr)) {
        return -1;
    }
    return static_cast<int64_t>(accounting.ActiveProcesses);
#else
    return -1;
#endif
}

extern "C" VYX_RT_ABI int32_t vyx_bootstrap_process_is_running(int64_t id) {
    std::lock_guard<std::mutex> lock(vyx_bootstrap_process_mutex());
    auto it = vyx_bootstrap_processes().find(id);
    if (it == vyx_bootstrap_processes().end() || !it->second) {
        return 0;
    }
    auto& proc = *it->second;
    if (proc.finished) {
        return 0;
    }
    // Capture before the POSIX wait can reap /proc, and before a Windows
    // completion transitions the handle to a finished observation.
    (void)vyx_bootstrap_sample_process_resident(proc);
#ifdef _WIN32
    DWORD wait_rc = WaitForSingleObject(proc.pi.hProcess, 0);
    if (wait_rc == WAIT_TIMEOUT) {
        return 1;
    }
    DWORD exit_code = 0;
    if (wait_rc == WAIT_OBJECT_0 && GetExitCodeProcess(proc.pi.hProcess, &exit_code)) {
        proc.exit_code = static_cast<int32_t>(exit_code);
    }
    proc.finished = true;
    return 0;
#else
    int status = 0;
    pid_t rc = -1;
    do {
        rc = waitpid(proc.pid, &status, WNOHANG);
    } while (rc < 0 && errno == EINTR);
    if (rc == 0) {
        return 1;
    }
    if (rc == proc.pid) {
        if (WIFEXITED(status)) {
            proc.exit_code = static_cast<int32_t>(WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            proc.exit_code = static_cast<int32_t>(128 + WTERMSIG(status));
        }
    } else if (rc < 0) {
        std::fprintf(stderr,
                     "error: cannot poll bootstrap child process "
                     "(pid=%lld errno=%d)\n",
                     static_cast<long long>(proc.pid),
                     errno);
    }
    proc.finished = true;
    return 0;
#endif
}

extern "C" VYX_RT_ABI int32_t vyx_bootstrap_process_exit_code(int64_t id) {
    std::lock_guard<std::mutex> lock(vyx_bootstrap_process_mutex());
    auto it = vyx_bootstrap_processes().find(id);
    if (it == vyx_bootstrap_processes().end() || !it->second) {
        return -1;
    }
    auto& proc = *it->second;
    if (!proc.finished) {
        return -1;
    }
    return proc.exit_code;
}

extern "C" VYX_RT_ABI int32_t vyx_bootstrap_process_wait(int64_t id) {
    for (;;) {
        if (!vyx_bootstrap_process_is_running(id)) {
            return vyx_bootstrap_process_exit_code(id);
        }
        vyx_bootstrap_sleep_ms(10);
    }
}

extern "C" VYX_RT_ABI int32_t vyx_bootstrap_process_cancel(int64_t id) {
    std::lock_guard<std::mutex> lock(vyx_bootstrap_process_mutex());
    auto it = vyx_bootstrap_processes().find(id);
    if (it == vyx_bootstrap_processes().end() || !it->second) {
        return 1;
    }
    auto& proc = *it->second;
#ifdef _WIN32
    bool terminate_ok = true;
    DWORD terminate_error = ERROR_SUCCESS;
    if (proc.job) {
        if (!TerminateJobObject(proc.job, 1)) {
            terminate_ok = false;
            terminate_error = GetLastError();
        }
    } else if (proc.pi.hProcess) {
        const DWORD initial_wait = WaitForSingleObject(proc.pi.hProcess, 0);
        if (initial_wait == WAIT_TIMEOUT) {
            if (!TerminateProcess(proc.pi.hProcess, 1)) {
                terminate_ok = false;
                terminate_error = GetLastError();
            }
        } else if (initial_wait == WAIT_FAILED) {
            terminate_ok = false;
            terminate_error = GetLastError();
        }
    } else {
        terminate_ok = false;
        terminate_error = ERROR_INVALID_HANDLE;
    }

    DWORD process_wait_error = ERROR_SUCCESS;
    const bool process_stopped = vyx_bootstrap_windows_wait_process(proc,
                                                                    5000,
                                                                    process_wait_error);
    DWORD job_wait_error = ERROR_SUCCESS;
    const bool job_stopped = vyx_bootstrap_windows_wait_job_empty(proc.job,
                                                                  5000,
                                                                  job_wait_error);
    if ((!terminate_ok && !job_stopped) || !process_stopped || !job_stopped) {
        std::fprintf(stderr,
                     "error: cannot cancel bootstrap child process "
                     "(id=%lld terminate=%lu process_wait=%lu job_wait=%lu)\n",
                     static_cast<long long>(id),
                     static_cast<unsigned long>(terminate_error),
                     static_cast<unsigned long>(process_wait_error),
                     static_cast<unsigned long>(job_wait_error));
        return 1;
    }
    return 0;
#else
    auto record_status = [&proc](int status) {
        if (WIFEXITED(status)) {
            proc.exit_code = static_cast<int32_t>(WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            proc.exit_code = static_cast<int32_t>(128 + WTERMSIG(status));
        } else {
            proc.exit_code = -1;
        }
        proc.finished = true;
    };
    auto poll_root = [&proc, &record_status]() -> bool {
        if (proc.finished) {
            return true;
        }
        int status = 0;
        pid_t wait_rc = -1;
        do {
            wait_rc = ::waitpid(proc.pid, &status, WNOHANG);
        } while (wait_rc < 0 && errno == EINTR);
        if (wait_rc == proc.pid) {
            record_status(status);
            return true;
        }
        if (wait_rc < 0 && errno == ECHILD) {
            proc.exit_code = -1;
            proc.finished = true;
            return true;
        }
        return false;
    };
    auto group_alive = [&proc]() -> bool {
        if (proc.pgid <= 0) {
            return false;
        }
        if (::kill(-proc.pgid, 0) == 0) {
            return true;
        }
        return errno == EPERM;
    };
    auto wait_until = [&poll_root, &group_alive](std::chrono::steady_clock::time_point deadline) {
        for (;;) {
            const bool root_stopped = poll_root();
            if (root_stopped && !group_alive()) {
                return true;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    };

    bool signal_ok = true;
    int signal_error = 0;
    if (proc.pgid > 0 && ::kill(-proc.pgid, SIGTERM) != 0 && errno != ESRCH) {
        signal_ok = false;
        signal_error = errno;
    }
    if (wait_until(std::chrono::steady_clock::now() + std::chrono::milliseconds(250))) {
        return signal_ok ? 0 : 1;
    }
    if (proc.pgid > 0 && ::kill(-proc.pgid, SIGKILL) != 0 && errno != ESRCH) {
        signal_ok = false;
        signal_error = errno;
    }
    if (!wait_until(std::chrono::steady_clock::now() + std::chrono::seconds(5))) {
        std::fprintf(stderr,
                     "error: cannot cancel bootstrap child process group "
                     "(id=%lld pgid=%lld errno=%d)\n",
                     static_cast<long long>(id),
                     static_cast<long long>(proc.pgid),
                     signal_error);
        return 1;
    }
    return signal_ok ? 0 : 1;
#endif
}

extern "C" VYX_RT_ABI void vyx_bootstrap_process_close(int64_t id) {
    std::lock_guard<std::mutex> lock(vyx_bootstrap_process_mutex());
    auto it = vyx_bootstrap_processes().find(id);
    if (it == vyx_bootstrap_processes().end()) {
        return;
    }
    auto proc = std::move(it->second);
    vyx_bootstrap_processes().erase(it);
    if (!proc) {
        return;
    }
#ifdef _WIN32
    // VyxBootstrapProcess owns and releases both handles. Cancellation is an
    // explicit operation so a successful launcher cannot lose descendants.
#else
    if (!proc->finished && proc->pid > 0) {
        int status = 0;
        pid_t wait_rc = -1;
        do {
            wait_rc = waitpid(proc->pid, &status, 0);
        } while (wait_rc < 0 && errno == EINTR);
        if (wait_rc == proc->pid) {
            if (WIFEXITED(status)) {
                proc->exit_code = static_cast<int32_t>(WEXITSTATUS(status));
            } else if (WIFSIGNALED(status)) {
                proc->exit_code = static_cast<int32_t>(128 + WTERMSIG(status));
            }
            proc->finished = true;
        }
    }
#endif
}

static std::string vyx_repl_trim(std::string s) {
    auto is_space = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    std::size_t first = 0;
    while (first < s.size() && is_space(static_cast<unsigned char>(s[first]))) {
        ++first;
    }
    std::size_t last = s.size();
    while (last > first && is_space(static_cast<unsigned char>(s[last - 1u]))) {
        --last;
    }
    return s.substr(first, last - first);
}

static bool vyx_repl_starts_with(const std::string& s, const char* prefix) {
    const std::size_t n = std::strlen(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

static bool vyx_repl_ends_with(const std::string& s, const char* suffix) {
    const std::size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

static bool vyx_repl_contains(const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
}

static void vyx_repl_strip_utf8_bom(std::string& s) {
    if (s.size() >= 3u
        && static_cast<unsigned char>(s[0]) == 0xEFu
        && static_cast<unsigned char>(s[1]) == 0xBBu
        && static_cast<unsigned char>(s[2]) == 0xBFu) {
        s.erase(0, 3u);
    }
}

static void vyx_repl_write_stdout(const std::string& text) {
    if (text.empty()) {
        return;
    }
#ifdef _WIN32
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != nullptr && out != INVALID_HANDLE_VALUE) {
        const char* data = text.data();
        std::size_t remaining = text.size();
        while (remaining > 0) {
            const DWORD chunk = static_cast<DWORD>(
                std::min<std::size_t>(remaining, 1u << 20));
            DWORD written = 0;
            if (!WriteFile(out, data, chunk, &written, nullptr) || written == 0) {
                break;
            }
            data += written;
            remaining -= written;
        }
        if (remaining == 0) {
            return;
        }
    }
#endif
    std::cout << text;
    std::cout.flush();
}

static void vyx_repl_write_stderr(const std::string& text) {
    if (text.empty()) {
        return;
    }
#ifdef _WIN32
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (err != nullptr && err != INVALID_HANDLE_VALUE) {
        const char* data = text.data();
        std::size_t remaining = text.size();
        while (remaining > 0) {
            const DWORD chunk = static_cast<DWORD>(
                std::min<std::size_t>(remaining, 1u << 20));
            DWORD written = 0;
            if (!WriteFile(err, data, chunk, &written, nullptr) || written == 0) {
                break;
            }
            data += written;
            remaining -= written;
        }
        if (remaining == 0) {
            return;
        }
    }
#endif
    std::cerr << text;
    std::cerr.flush();
}

static bool vyx_diag_lang_is_zh() {
    const char* raw = std::getenv("VYX_LANG");
    if (!raw || !raw[0]) {
        raw = std::getenv("LC_ALL");
    }
    if (!raw || !raw[0]) {
        raw = std::getenv("LANG");
    }
    if (!raw) {
        return false;
    }
    return (raw[0] == 'z' || raw[0] == 'Z')
        && (raw[1] == 'h' || raw[1] == 'H');
}

static const char* vyx_diag_level_label(const char* level) {
    if (!vyx_diag_lang_is_zh()) {
        return level;
    }
    if (std::strcmp(level, "note") == 0) {
        return "注意";
    }
    if (std::strcmp(level, "warning") == 0) {
        return "警告";
    }
    if (std::strcmp(level, "error") == 0) {
        return "错误";
    }
    if (std::strcmp(level, "fatal") == 0) {
        return "致命";
    }
    return "诊断";
}

static std::string vyx_repl_diag(const char* level,
                                 const char* code,
                                 const std::string& message) {
    return std::string("<repl>:1:1: ")
        + vyx_diag_level_label(level)
        + ": "
        + code
        + ": "
        + message
        + "\n";
}

static void vyx_repl_write_file_to_output(const std::filesystem::path& path,
                                          bool stderr_output) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return;
    }
    char buf[4096];
    while (in) {
        in.read(buf, sizeof(buf));
        const std::streamsize n = in.gcount();
        if (n <= 0) {
            break;
        }
        std::string chunk(buf, buf + n);
        if (stderr_output) {
            vyx_repl_write_stderr(chunk);
        } else {
            vyx_repl_write_stdout(chunk);
        }
    }
}

static void vyx_repl_clear_console() {
#ifdef _WIN32
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != nullptr && out != INVALID_HANDLE_VALUE) {
        CONSOLE_SCREEN_BUFFER_INFO csbi{};
        DWORD written = 0;
        const COORD home{0, 0};
        if (GetConsoleScreenBufferInfo(out, &csbi)) {
            const DWORD cells = static_cast<DWORD>(csbi.dwSize.X) *
                static_cast<DWORD>(csbi.dwSize.Y);
            FillConsoleOutputCharacterW(out, L' ', cells, home, &written);
            FillConsoleOutputAttribute(out, csbi.wAttributes, cells, home, &written);
            SetConsoleCursorPosition(out, home);
            return;
        }
    }
#endif
    vyx_repl_write_stdout("\x1b[2J\x1b[H");
}

static bool vyx_repl_ident_start(char c);

static bool vyx_repl_ident_char(char c) {
    const unsigned char uc = static_cast<unsigned char>(c);
    return std::isalnum(uc) || c == '_';
}

static bool vyx_repl_is_bare_identifier(const std::string& text) {
    if (text.empty() || !vyx_repl_ident_start(text[0])) {
        return false;
    }
    for (char c : text) {
        if (!vyx_repl_ident_char(c)) {
            return false;
        }
    }
    return true;
}

static bool vyx_repl_is_literal_keyword(const std::string& text) {
    return text == "true" || text == "false" || text == "null";
}

static std::string vyx_repl_first_code_line(const std::string& text) {
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find('\n', start);
        std::string line = vyx_repl_trim(text.substr(
            start,
            end == std::string::npos ? std::string::npos : end - start));
        if (vyx_repl_starts_with(line, "@[")) {
            const std::size_t close = line.find(']');
            if (close != std::string::npos
                && vyx_repl_trim(line.substr(close + 1u)).empty()) {
                if (end == std::string::npos) {
                    break;
                }
                start = end + 1u;
                continue;
            }
        }
        if (!line.empty()) {
            return line;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1u;
    }
    return {};
}

static std::string vyx_repl_strip_leading_attributes(std::string text) {
    text = vyx_repl_trim(std::move(text));
    for (;;) {
        if (!vyx_repl_starts_with(text, "@[")) {
            return text;
        }
        const std::size_t close = text.find(']');
        if (close == std::string::npos) {
            return text;
        }
        text = vyx_repl_trim(text.substr(close + 1u));
    }
}

static std::string vyx_repl_strip_leading_modifiers(std::string text) {
    text = vyx_repl_strip_leading_attributes(std::move(text));
    for (;;) {
        std::size_t pos = 0;
        while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) {
            ++pos;
        }
        std::size_t end = pos;
        while (end < text.size() && vyx_repl_ident_char(text[end])) {
            ++end;
        }
        if (end == pos) {
            return vyx_repl_trim(text.substr(pos));
        }
        const std::string word = text.substr(pos, end - pos);
        if (word == "public" || word == "private" || word == "protected"
            || word == "internal" || word == "unsafe" || word == "async"
            || word == "static" || word == "const") {
            text = vyx_repl_trim(text.substr(end));
            continue;
        }
        return vyx_repl_trim(text.substr(pos));
    }
}

static bool vyx_repl_is_top_level(const std::string& line) {
    const std::string first = vyx_repl_first_code_line(line);
    const std::string t = vyx_repl_strip_leading_modifiers(first);
    return vyx_repl_starts_with(t, "use ")
        || vyx_repl_starts_with(t, "import ")
        || vyx_repl_starts_with(t, "fn ")
        || vyx_repl_starts_with(t, "class ")
        || vyx_repl_starts_with(t, "struct ")
        || vyx_repl_starts_with(t, "enum ")
        || vyx_repl_starts_with(t, "trait ")
        || vyx_repl_starts_with(t, "impl ")
        || vyx_repl_starts_with(t, "extern ")
        || vyx_repl_starts_with(t, "type ")
        || vyx_repl_starts_with(t, "const ");
}

[[maybe_unused]] static bool vyx_repl_should_store_statement(const std::string& line) {
    const std::string t = vyx_repl_trim(line);
    if (vyx_repl_starts_with(t, "let ") || vyx_repl_starts_with(t, "var ")) {
        return true;
    }
    return vyx_repl_contains(t, "=")
        && !vyx_repl_contains(t, "==")
        && !vyx_repl_contains(t, "!=")
        && !vyx_repl_contains(t, "<=")
        && !vyx_repl_contains(t, ">=");
}

struct VyxReplScanState {
    int paren = 0;
    int brace = 0;
    int bracket = 0;
    bool in_string = false;
    bool in_multiline_string = false;
    bool escape = false;
    bool in_block_comment = false;
};

static void vyx_repl_scan_fragment(const std::string& text, VyxReplScanState& st) {
    bool in_line_comment = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        const char n = (i + 1u < text.size()) ? text[i + 1u] : '\0';
        if (in_line_comment) {
            if (c == '\n') {
                in_line_comment = false;
            }
            continue;
        }
        if (st.in_block_comment) {
            if (c == '*' && n == '/') {
                st.in_block_comment = false;
                ++i;
            }
            continue;
        }
        if (st.in_multiline_string) {
            if (c == '"' && n == '"'
                && i + 2u < text.size() && text[i + 2u] == '"') {
                st.in_multiline_string = false;
                i += 2u;
            }
            continue;
        }
        if (st.in_string) {
            if (st.escape) {
                st.escape = false;
                continue;
            }
            if (c == '\\') {
                st.escape = true;
                continue;
            }
            if (c == '"') {
                st.in_string = false;
            }
            continue;
        }
        if (c == '/' && n == '/') {
            in_line_comment = true;
            ++i;
            continue;
        }
        if (c == '/' && n == '*') {
            st.in_block_comment = true;
            ++i;
            continue;
        }
        if (c == '"' && n == '"'
            && i + 2u < text.size() && text[i + 2u] == '"') {
            st.in_multiline_string = true;
            i += 2u;
            continue;
        }
        if (c == '"') {
            st.in_string = true;
            continue;
        }
        if (c == '(') { ++st.paren; }
        else if (c == ')' && st.paren > 0) { --st.paren; }
        else if (c == '{') { ++st.brace; }
        else if (c == '}' && st.brace > 0) { --st.brace; }
        else if (c == '[') { ++st.bracket; }
        else if (c == ']' && st.bracket > 0) { --st.bracket; }
    }
}

static bool vyx_repl_balance_closed(const VyxReplScanState& st) {
    return st.paren == 0 && st.brace == 0 && st.bracket == 0
        && !st.in_string && !st.in_multiline_string
        && !st.escape && !st.in_block_comment;
}

static bool vyx_repl_ends_with_continuation_operator(const std::string& text) {
    const std::string t = vyx_repl_trim(text);
    if (t.empty()) {
        return false;
    }
    const char c = t.back();
    if (c == ',' || c == '.' || c == '+' || c == '-' || c == '*' || c == '/'
        || c == '%' || c == '&' || c == '|' || c == '^' || c == '='
        || c == '<' || c == '>' || c == '?' || c == ':') {
        return true;
    }
    return vyx_repl_ends_with(t, "&&")
        || vyx_repl_ends_with(t, "||")
        || vyx_repl_ends_with(t, "else")
        || vyx_repl_ends_with(t, "elif");
}

static bool vyx_repl_cell_complete(const std::string& text, bool force_multiline) {
    if (force_multiline) {
        return false;
    }
    const std::string t = vyx_repl_trim(text);
    if (t.empty()) {
        return true;
    }
    VyxReplScanState st;
    vyx_repl_scan_fragment(text, st);
    return vyx_repl_balance_closed(st)
        && !vyx_repl_ends_with_continuation_operator(t);
}

static bool vyx_repl_load_file(const std::string& path, std::string& text) {
    text.clear();
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size > 0) {
        text.resize(static_cast<std::size_t>(size));
        in.seekg(0, std::ios::beg);
        in.read(text.data(), size);
    }
    return true;
}

static std::vector<std::string> vyx_repl_split_loaded_cells(const std::string& text) {
    std::vector<std::string> cells;
    std::string current;
    bool force_multiline = false;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find('\n', start);
        std::string raw = text.substr(
            start,
            end == std::string::npos ? std::string::npos : end - start);
        if (!raw.empty() && raw.back() == '\r') {
            raw.pop_back();
        }
        const std::string line = vyx_repl_trim(raw);
        if (current.empty() && !force_multiline) {
            if (line.empty()) {
                if (end == std::string::npos) {
                    break;
                }
                start = end + 1u;
                continue;
            }
            if (vyx_repl_starts_with(line, "module ")) {
                if (end == std::string::npos) {
                    break;
                }
                start = end + 1u;
                continue;
            }
            if (line == ":{") {
                force_multiline = true;
                if (end == std::string::npos) {
                    break;
                }
                start = end + 1u;
                continue;
            }
        } else if (force_multiline && line == ":}") {
            cells.push_back(current);
            current.clear();
            force_multiline = false;
            if (end == std::string::npos) {
                break;
            }
            start = end + 1u;
            continue;
        }
        current += raw;
        current += "\n";
        if (vyx_repl_cell_complete(current, force_multiline)) {
            cells.push_back(current);
            current.clear();
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1u;
    }
    if (!vyx_repl_trim(current).empty()) {
        cells.push_back(current);
    }
    return cells;
}

struct VyxReplGlobal {
    std::string name;
    std::string type;
};

struct VyxReplTopDecl {
    std::string key;
    std::string text;
};

struct VyxReplVarDecl {
    bool valid = false;
    bool is_mutable = false;
    std::string name;
    std::string type;
    std::string init;
};

static bool vyx_repl_ident_start(char c) {
    const unsigned char uc = static_cast<unsigned char>(c);
    return std::isalpha(uc) || c == '_';
}

static std::string vyx_repl_mangle_part(std::string_view text) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size());
    for (unsigned char c : text) {
        const bool keep = (c >= '0' && c <= '9')
            || (c >= 'A' && c <= 'Z')
            || (c >= 'a' && c <= 'z');
        if (keep) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('_');
            out.push_back(kHex[(c >> 4) & 0x0f]);
            out.push_back(kHex[c & 0x0f]);
        }
    }
    return out;
}

static std::string vyx_repl_mangled_i32_noarg_fn(const std::string& name) {
    return "__vyx_F_" + vyx_repl_mangle_part(name) + "_R_i32_P_0";
}

static std::string vyx_repl_strip_trailing_semicolon(std::string text) {
    text = vyx_repl_trim(std::move(text));
    if (!text.empty() && text.back() == ';') {
        text.pop_back();
    }
    return vyx_repl_trim(std::move(text));
}

static bool vyx_repl_is_type_prefix_char(char c) {
    const unsigned char uc = static_cast<unsigned char>(c);
    return std::isalnum(uc) || c == '_' || c == '.' || c == ':' || c == '<' || c == '>'
        || c == ',' || c == ' ' || c == '\t';
}

static std::string vyx_repl_normalize_type_text(std::string text) {
    text = vyx_repl_trim(std::move(text));
    std::size_t pos = 0;
    while ((pos = text.find("::<", pos)) != std::string::npos) {
        text.replace(pos, 3u, "<");
        ++pos;
    }
    return text;
}

static bool vyx_repl_type_prefix_looks_like_type(const std::string& prefix) {
    const std::string t = vyx_repl_normalize_type_text(prefix);
    if (t.empty()) {
        return false;
    }
    const unsigned char first = static_cast<unsigned char>(t.front());
    if (std::isupper(first)) {
        return true;
    }
    if (t.find("::") != std::string::npos) {
        return true;
    }
    return false;
}

static std::string vyx_repl_top_level_cast_type(const std::string& init) {
    VyxReplScanState st;
    for (std::size_t i = 0; i + 4u <= init.size(); ++i) {
        vyx_repl_scan_fragment(init.substr(i, 1), st);
        if (!vyx_repl_balance_closed(st)) {
            continue;
        }
        if (i > 0 && std::isspace(static_cast<unsigned char>(init[i - 1u]))
            && init.compare(i, 4u, "as ") == 0) {
            return vyx_repl_trim(init.substr(i + 3u));
        }
    }
    return {};
}

static std::string vyx_repl_infer_literal_type(const std::string& raw_init) {
    std::string init = vyx_repl_strip_trailing_semicolon(raw_init);
    if (init.empty()) {
        return {};
    }
    const std::string cast_type = vyx_repl_top_level_cast_type(init);
    if (!cast_type.empty()) {
        return cast_type;
    }
    if (init == "true" || init == "false") {
        return "bool";
    }
    if (init.front() == '"' && init.back() == '"' && init.size() >= 2u) {
        return "string";
    }
    if (init == "null") {
        return "rawptr";
    }

    std::string lower = init;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto allDigitsFrom = [&](std::size_t begin, std::size_t end) {
        if (begin >= end) {
            return false;
        }
        for (std::size_t i = begin; i < end; ++i) {
            if (!std::isdigit(static_cast<unsigned char>(lower[i]))) {
                return false;
            }
        }
        return true;
    };
    std::size_t num_start = (lower[0] == '-' || lower[0] == '+') ? 1u : 0u;
    if (lower.size() > num_start) {
        std::string suffix;
        for (const char* s : {"i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64"}) {
            const std::size_t n = std::strlen(s);
            if (lower.size() > n && lower.compare(lower.size() - n, n, s) == 0) {
                suffix = s;
                break;
            }
        }
        const std::size_t body_end = suffix.empty() ? lower.size() : lower.size() - suffix.size();
        bool has_dot = false;
        bool has_exp = false;
        bool numeric = true;
        for (std::size_t i = num_start; i < body_end; ++i) {
            const char c = lower[i];
            if (std::isdigit(static_cast<unsigned char>(c)) || c == '_') {
                continue;
            }
            if (c == '.') {
                has_dot = true;
                continue;
            }
            if (c == 'e') {
                has_exp = true;
                continue;
            }
            if ((c == '-' || c == '+') && i > num_start && lower[i - 1u] == 'e') {
                continue;
            }
            numeric = false;
            break;
        }
        if (numeric && body_end > num_start) {
            if (has_dot || has_exp || lower == "nan" || lower == "inf") {
                if (lower.size() >= 3u && lower.substr(lower.size() - 3u) == "f32") {
                    return "f32";
                }
                return "f64";
            }
            if (!suffix.empty()) {
                return suffix;
            }
            if (allDigitsFrom(num_start, body_end)) {
                return "i32";
            }
        }
    }

    for (std::size_t factory_pos = 0; factory_pos < init.size(); ++factory_pos) {
        if (init[factory_pos] != '.') {
            continue;
        }
        if (factory_pos + 1u >= init.size() || !vyx_repl_ident_start(init[factory_pos + 1u])) {
            continue;
        }
        const std::size_t open = init.find('(', factory_pos + 1u);
        if (open == std::string::npos) {
            continue;
        }
        const std::string prefix = vyx_repl_trim(init.substr(0, factory_pos));
        bool ok = vyx_repl_type_prefix_looks_like_type(prefix);
        if (!ok) {
            for (char c : prefix) {
                if (!vyx_repl_is_type_prefix_char(c)) {
                    ok = false;
                    break;
                }
            }
            if (ok) {
                ok = vyx_repl_type_prefix_looks_like_type(prefix);
            }
        }
        if (ok) {
            return vyx_repl_normalize_type_text(prefix);
        }
    }

    const std::size_t brace_pos = init.find('{');
    if (brace_pos != std::string::npos && brace_pos > 0) {
        const std::string prefix = vyx_repl_trim(init.substr(0, brace_pos));
        bool ok = !prefix.empty() && vyx_repl_ident_start(prefix[0]);
        for (char c : prefix) {
            if (!vyx_repl_is_type_prefix_char(c)) {
                ok = false;
                break;
            }
        }
        if (ok) {
            return vyx_repl_normalize_type_text(prefix);
        }
    }

    return {};
}

static VyxReplVarDecl vyx_repl_parse_var_decl(const std::string& text) {
    VyxReplVarDecl out;
    std::string s = vyx_repl_strip_leading_modifiers(text);
    s = vyx_repl_strip_trailing_semicolon(std::move(s));
    std::size_t pos = 0;
    while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) {
        ++pos;
    }
    std::size_t word_end = pos;
    while (word_end < s.size() && vyx_repl_ident_char(s[word_end])) {
        ++word_end;
    }
    const std::string word = s.substr(pos, word_end - pos);
    if (word != "let" && word != "var") {
        return out;
    }
    out.is_mutable = (word == "var");
    pos = word_end;
    while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) {
        ++pos;
    }
    if (pos >= s.size() || !vyx_repl_ident_start(s[pos])) {
        return out;
    }
    std::size_t name_end = pos + 1u;
    while (name_end < s.size() && vyx_repl_ident_char(s[name_end])) {
        ++name_end;
    }
    out.name = s.substr(pos, name_end - pos);
    pos = name_end;
    while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) {
        ++pos;
    }
    if (pos < s.size() && s[pos] == ':') {
        ++pos;
        std::size_t ty_start = pos;
        VyxReplScanState st;
        while (pos < s.size()) {
            const char c = s[pos];
            if (c == '=' && vyx_repl_balance_closed(st)) {
                break;
            }
            vyx_repl_scan_fragment(s.substr(pos, 1), st);
            ++pos;
        }
        out.type = vyx_repl_trim(s.substr(ty_start, pos - ty_start));
    }
    while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) {
        ++pos;
    }
    if (pos < s.size() && s[pos] == '=') {
        out.init = vyx_repl_trim(s.substr(pos + 1u));
    }
    if (out.type.empty() && !out.init.empty()) {
        out.type = vyx_repl_infer_literal_type(out.init);
    }
    out.valid = !out.name.empty();
    return out;
}

static const VyxReplGlobal* vyx_repl_find_global(const std::vector<VyxReplGlobal>& globals,
                                                 const std::string& name) {
    for (const auto& g : globals) {
        if (g.name == name) {
            return &g;
        }
    }
    return nullptr;
}

static void vyx_repl_upsert_global(std::vector<VyxReplGlobal>& globals,
                                   const std::string& name,
                                   const std::string& type) {
    for (auto& g : globals) {
        if (g.name == name) {
            g.type = type;
            return;
        }
    }
    globals.push_back(VyxReplGlobal{name, type});
}

static bool vyx_repl_add_completion(std::vector<std::string>& out,
                                    const std::string& name) {
    if (name.empty()) {
        return false;
    }
    if (std::find(out.begin(), out.end(), name) != out.end()) {
        return false;
    }
    out.push_back(name);
    return true;
}

static std::string vyx_repl_decl_name_after_keyword(const std::string& text,
                                                    const char* keyword) {
    if (!vyx_repl_starts_with(text, keyword)) {
        return "";
    }
    std::size_t pos = std::strlen(keyword);
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) {
        ++pos;
    }
    if (pos >= text.size() || !vyx_repl_ident_start(text[pos])) {
        return "";
    }
    const std::size_t start = pos;
    while (pos < text.size() && vyx_repl_ident_char(text[pos])) {
        ++pos;
    }
    return text.substr(start, pos - start);
}

static std::string vyx_repl_decl_completion_name(const std::string& text) {
    const std::string first = vyx_repl_first_code_line(text);
    const std::string t = vyx_repl_strip_leading_modifiers(first);
    for (const char* keyword : {
             "fn ", "class ", "struct ", "enum ", "trait ", "type ", "const "
         }) {
        const std::string name = vyx_repl_decl_name_after_keyword(t, keyword);
        if (!name.empty()) {
            return name;
        }
    }
    return "";
}

static std::string vyx_repl_top_source_from_decls(
    const std::vector<VyxReplTopDecl>& decls) {
    std::string out;
    for (const auto& d : decls) {
        if (d.text.empty()) {
            continue;
        }
        out += d.text;
        if (out.empty() || out.back() != '\n') {
            out += "\n";
        }
    }
    return out;
}

static bool vyx_repl_upsert_top_decl(std::vector<VyxReplTopDecl>& decls,
                                     const std::string& key,
                                     const std::string& text) {
    if (!key.empty()) {
        for (auto& d : decls) {
            if (d.key == key) {
                d.text = text;
                return true;
            }
        }
    }
    decls.push_back(VyxReplTopDecl{key, text});
    return false;
}

static bool vyx_repl_delete_top_decl(std::vector<VyxReplTopDecl>& decls,
                                     const std::string& key) {
    if (key.empty()) {
        return false;
    }
    const auto old_size = decls.size();
    decls.erase(std::remove_if(decls.begin(),
                               decls.end(),
                               [&](const VyxReplTopDecl& d) {
                                   return d.key == key;
                               }),
                decls.end());
    return decls.size() != old_size;
}

static bool vyx_repl_delete_global(std::vector<VyxReplGlobal>& globals,
                                   const std::string& name) {
    if (name.empty()) {
        return false;
    }
    const auto old_size = globals.size();
    globals.erase(std::remove_if(globals.begin(),
                                 globals.end(),
                                 [&](const VyxReplGlobal& g) {
                                     return g.name == name;
                                 }),
                  globals.end());
    return globals.size() != old_size;
}

static void vyx_repl_rebuild_completions(
    std::vector<std::string>& completions,
    const std::vector<VyxReplTopDecl>& decls,
    const std::vector<VyxReplGlobal>& globals) {
    completions.clear();
    for (const auto& d : decls) {
        vyx_repl_add_completion(completions, d.key);
    }
    for (const auto& g : globals) {
        vyx_repl_add_completion(completions, g.name);
    }
}

static std::vector<std::string> vyx_repl_current_completions(
    const std::vector<std::string>& names) {
    std::vector<std::string> out;
    for (const char* cmd : {
             ":quit", ":exit", ":clear", ":help", ":load ", ":dump",
             ":export", ":export ", ":del ", ":{", ":}", ":complete "
         }) {
        vyx_repl_add_completion(out, cmd);
    }
    for (const char* builtin : {
             "print", "true", "false", "null"
         }) {
        vyx_repl_add_completion(out, builtin);
    }
    for (const auto& name : names) {
        vyx_repl_add_completion(out, name);
    }
    std::sort(out.begin(), out.end());
    return out;
}

static std::vector<std::string> vyx_repl_completion_matches(
    const std::vector<std::string>& candidates,
    const std::string& prefix) {
    std::vector<std::string> out;
    for (const auto& candidate : candidates) {
        if (prefix.empty() || candidate.rfind(prefix, 0) == 0) {
            out.push_back(candidate);
        }
    }
    return out;
}

static std::string vyx_repl_join_lines(const std::vector<std::string>& lines) {
    std::string out;
    for (const auto& line : lines) {
        out += line;
        out += "\n";
    }
    return out;
}

static std::string vyx_repl_extern_globals_source(const std::vector<VyxReplGlobal>& globals,
                                                  const std::string& skip_name) {
    std::string out;
    for (const auto& g : globals) {
        if (g.name.empty() || g.type.empty() || g.name == skip_name) {
            continue;
        }
        if (out.empty()) {
            out = "extern \"C\" {\n";
        }
        out += "var ";
        out += g.name;
        out += ": ";
        out += g.type;
        out += ";\n";
    }
    if (!out.empty()) {
        out += "}\n";
    }
    return out;
}

static std::string vyx_repl_prelude_source(const std::string& top_source,
                                           const std::vector<VyxReplGlobal>& globals,
                                           const std::string& skip_global) {
    std::string source = "module __vyx_repl;\nuse std.io;\n";
    source += vyx_repl_extern_globals_source(globals, skip_global);
    source += top_source;
    if (!top_source.empty() && top_source.back() != '\n') {
        source += "\n";
    }
    return source;
}

[[maybe_unused]] static std::string vyx_repl_validation_source(const std::string& top_source,
                                              const std::vector<VyxReplGlobal>& globals,
                                              const std::string& current_top) {
    std::string source = vyx_repl_prelude_source(top_source, globals, "");
    source += current_top;
    source += "\n";
    return source;
}

static std::string vyx_repl_exec_source(const std::string& top_source,
                                        const std::vector<VyxReplGlobal>& globals,
                                        const VyxReplVarDecl* var_decl,
                                        const std::string& current,
                                        bool is_statement,
                                        const std::string& cell_fn_name,
                                        const std::string& cell_entry_name,
                                        bool define_new_global) {
    std::string skip;
    if (var_decl && define_new_global) {
        skip = var_decl->name;
    }
    std::string source = vyx_repl_prelude_source(top_source, globals, skip);
    if (var_decl && define_new_global) {
        source += "public var ";
        source += var_decl->name;
        source += ": ";
        source += var_decl->type;
        source += ";\n";
    }
    source += "fn ";
    source += cell_fn_name;
    source += "() -> i32 {\n";
    if (var_decl) {
        if (!var_decl->init.empty()) {
            source += var_decl->name;
            source += " = ";
            source += var_decl->init;
            source += ";\n";
        }
    } else if (is_statement) {
        source += current;
        source += "\n";
    } else {
        source += "print(";
        source += current;
        source += ");\n";
    }
    source += "return 0;\n}\n";
    source += "public fn " + cell_entry_name + "() -> i32 { return " + cell_fn_name + "(); }\n";
    source += "fn main() -> i32 { return " + cell_entry_name + "(); }\n";
    return source;
}

static std::string vyx_repl_export_replay(
    const std::vector<std::string>& replay_cells) {
    std::string out;
    for (const auto& cell : replay_cells) {
        if (vyx_repl_trim(cell).empty()) {
            continue;
        }
        out += cell;
        if (out.empty() || out.back() != '\n') {
            out += "\n";
        }
        out += "\n";
    }
    return out;
}

static bool vyx_repl_export_to_file(const std::string& path,
                                    const std::string& source,
                                    std::string& err) {
    if (path.empty()) {
        err = "empty export path";
        return false;
    }
    std::filesystem::path out_path(path);
    std::error_code ec;
    const auto parent = out_path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            err = ec.message();
            return false;
        }
    }
    std::ofstream out(out_path, std::ios::binary);
    if (!out) {
        err = "failed to open export path";
        return false;
    }
    out << source;
    return true;
}

static bool vyx_repl_keep_temp() {
    const char* raw = std::getenv("VYX_REPL_KEEP_TEMP");
    return raw && raw[0] != '\0';
}

static uint64_t vyx_repl_pid() {
#ifdef _WIN32
    return static_cast<uint64_t>(GetCurrentProcessId());
#else
    return static_cast<uint64_t>(getpid());
#endif
}

static std::string vyx_repl_quote_arg(const std::string& arg) {
#ifdef _WIN32
    return vyx_bootstrap_windows_quote_arg(arg);
#else
    std::string out = "\"";
    for (char c : arg) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    out.push_back('"');
    return out;
#endif
}

static std::string vyx_repl_self_path(const char* self_exe) {
    std::string self = self_exe ? std::string(self_exe) : std::string();
    if (self.empty()) {
#ifdef _WIN32
        return "vyxc.exe";
#else
        return "vyxc";
#endif
    }
    std::error_code ec;
    std::filesystem::path p(self);
    if (std::filesystem::exists(p, ec)) {
        auto abs = std::filesystem::absolute(p, ec);
        if (!ec) {
            return abs.string();
        }
    }
    return self;
}

static int32_t vyx_repl_compile_cell(const std::string& self_exe,
                                     const std::string& compiler_args,
                                     const std::filesystem::path& path,
                                     std::filesystem::path& emit_path) {
    std::string cmd = vyx_repl_quote_arg(self_exe);
    emit_path = path;
    emit_path += ".ll";
    cmd += " --src=file ";
    cmd += vyx_repl_quote_arg(path.string());
    cmd += " --emit=ir";
    if (!compiler_args.empty()) {
        cmd += compiler_args;
    }
    cmd += " -o ";
    cmd += vyx_repl_quote_arg(emit_path.string());
    std::filesystem::path stdout_path = path;
    stdout_path += ".stdout";
    std::filesystem::path stderr_path = path;
    stderr_path += ".stderr";
    const std::string stdout_text_path = stdout_path.string();
    const std::string stderr_text_path = stderr_path.string();
    const int64_t pid = vyx_bootstrap_process_spawn(cmd.c_str(),
                                                    "",
                                                    stdout_text_path.c_str(),
                                                    stderr_text_path.c_str(),
                                                    "",
                                                    "");
    if (pid == 0) {
        vyx_repl_write_stdout(vyx_repl_diag(
            "fatal", "I0101", "failed to launch jit child"));
        return -1;
    }
    const int32_t rc = vyx_bootstrap_process_wait(pid);
    vyx_bootstrap_process_close(pid);
    vyx_repl_write_file_to_output(stdout_path, false);
    vyx_repl_write_file_to_output(stderr_path, true);
    if (!vyx_repl_keep_temp()) {
        std::error_code ec;
        std::filesystem::remove(stdout_path, ec);
        ec.clear();
        std::filesystem::remove(stderr_path, ec);
        ec.clear();
    }
    return rc;
}

static void vyx_repl_cleanup_paths(const std::vector<std::filesystem::path>& paths) {
    if (vyx_repl_keep_temp()) {
        return;
    }
    for (const auto& path : paths) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
}

static std::string vyx_repl_llvm_error_to_string(llvm::Error err) {
    if (!err) {
        return {};
    }
    std::string text;
    llvm::raw_string_ostream os(text);
    llvm::logAllUnhandledErrors(std::move(err), os, "");
    os.flush();
    return text.empty() ? "LLVM error" : text;
}

static std::string vyx_repl_source_diag_to_string(const llvm::SMDiagnostic& diag) {
    std::string text;
    llvm::raw_string_ostream os(text);
    diag.print("vyx repl jit", os);
    os.flush();
    return text;
}

static std::once_flag& vyx_repl_llvm_init_once() {
    static std::once_flag once;
    return once;
}

static void vyx_repl_ensure_llvm_ready() {
    std::call_once(vyx_repl_llvm_init_once(), [] {
        llvm::InitializeAllTargetInfos();
        llvm::InitializeAllTargets();
        llvm::InitializeAllTargetMCs();
        llvm::InitializeAllAsmParsers();
        llvm::InitializeAllAsmPrinters();
    });
}

static std::vector<std::string> vyx_repl_split_link_args(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    bool in_quote = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\\' && i + 1u < text.size() && text[i + 1u] == '"') {
            cur.push_back('"');
            ++i;
            continue;
        }
        if (c == '"') {
            in_quote = !in_quote;
            continue;
        }
        if (!in_quote && std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
            continue;
        }
        cur.push_back(c);
    }
    if (!cur.empty()) {
        out.push_back(cur);
    }
    return out;
}

static bool vyx_repl_path_explicit(const std::string& name) {
    return name.find('/') != std::string::npos
        || name.find('\\') != std::string::npos
        || name.find(':') != std::string::npos;
}

static bool vyx_repl_has_dynamic_library_suffix(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto ends_with = [&](const char* suffix) {
        const std::size_t n = std::strlen(suffix);
        return lower.size() >= n && lower.compare(lower.size() - n, n, suffix) == 0;
    };
    return ends_with(".dll") || ends_with(".so") || ends_with(".dylib");
}

static std::vector<std::string> vyx_repl_dynamic_library_names(const std::string& name) {
    std::vector<std::string> names;
    if (name.empty()) {
        return names;
    }
    if (vyx_repl_has_dynamic_library_suffix(name) || vyx_repl_path_explicit(name)) {
        names.push_back(name);
        return names;
    }
#ifdef _WIN32
    names.push_back(name + ".dll");
    names.push_back("lib" + name + ".dll");
#elif defined(__APPLE__)
    names.push_back("lib" + name + ".dylib");
    names.push_back(name + ".dylib");
    names.push_back("lib" + name + ".so");
#else
    names.push_back("lib" + name + ".so");
    names.push_back(name + ".so");
#endif
    names.push_back(name);
    return names;
}

class VyxReplJitSession {
public:
    static std::unique_ptr<VyxReplJitSession> create(const std::string& link_args,
                                                     std::string& err) {
        vyx_repl_ensure_llvm_ready();
        auto jit_expected = llvm::orc::LLJITBuilder().create();
        if (!jit_expected) {
            err = vyx_repl_llvm_error_to_string(jit_expected.takeError());
            return nullptr;
        }
        auto session = std::unique_ptr<VyxReplJitSession>(new VyxReplJitSession());
        session->jit_ = std::move(*jit_expected);

        auto generator_expected =
            llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
                session->jit_->getDataLayout().getGlobalPrefix());
        if (!generator_expected) {
            err = vyx_repl_llvm_error_to_string(generator_expected.takeError());
            return nullptr;
        }
        session->jit_->getMainJITDylib().addGenerator(std::move(*generator_expected));
        if (!session->loadDynamicLibraries(link_args, err)) {
            return nullptr;
        }
        return session;
    }

    bool addIRFile(const std::filesystem::path& ir_path,
                   const std::vector<std::string>& preserve_symbols,
                   const std::vector<std::string>& defined_globals,
                   const std::vector<std::string>& external_globals,
                   std::string& err,
                   const std::string& owned_symbol = std::string()) {
        auto ctx = std::make_unique<llvm::LLVMContext>();
        llvm::SMDiagnostic diag;
        auto mod = llvm::parseIRFile(ir_path.string(), diag, *ctx);
        if (!mod) {
            err = vyx_repl_source_diag_to_string(diag);
            return false;
        }

        normalizeMirGlobalsForCell(*mod, defined_globals, external_globals);
        if (!verifyModule(*mod, "before-repl-jit", err)) {
            return false;
        }
        internalizeForCell(*mod, preserve_symbols);
        if (!verifyModule(*mod, "after-repl-jit-internalize", err)) {
            return false;
        }

        llvm::orc::ThreadSafeModule tsm(std::move(mod), std::move(ctx));
        llvm::orc::ResourceTrackerSP tracker;
        if (!owned_symbol.empty()) {
            tracker = jit_->getMainJITDylib().createResourceTracker();
        }
        llvm::Error add_err = tracker
            ? jit_->addIRModule(tracker, std::move(tsm))
            : jit_->addIRModule(std::move(tsm));
        if (add_err) {
            err = vyx_repl_llvm_error_to_string(std::move(add_err));
            return false;
        }
        if (tracker) {
            owned_symbols_[owned_symbol] = std::move(tracker);
        }
        return true;
    }

    bool removeOwnedSymbol(const std::string& symbol, std::string& err) {
        auto it = owned_symbols_.find(symbol);
        if (it == owned_symbols_.end()) {
            return true;
        }
        if (auto remove_err = it->second->remove()) {
            err = vyx_repl_llvm_error_to_string(std::move(remove_err));
            return false;
        }
        owned_symbols_.erase(it);
        return true;
    }

    bool runI32(const std::string& symbol, int32_t& rc, std::string& err) {
        auto sym_expected = jit_->lookup(symbol);
        if (!sym_expected) {
            err = vyx_repl_llvm_error_to_string(sym_expected.takeError());
            return false;
        }
        using CellFn = int32_t();
        auto* fn = sym_expected->toPtr<CellFn>();
        if (!fn) {
            err = "JIT lookup returned null for " + symbol;
            return false;
        }
        rc = fn();
        return true;
    }

private:
    std::unique_ptr<llvm::orc::LLJIT> jit_;
    std::unordered_map<std::string, llvm::orc::ResourceTrackerSP> owned_symbols_;

    static bool verifyModule(llvm::Module& mod, const char* phase, std::string& err) {
        std::string text;
        llvm::raw_string_ostream os(text);
        if (llvm::verifyModule(mod, &os)) {
            os.flush();
            err = std::string("module verification failed at ")
                + (phase ? phase : "unknown") + ":\n" + text;
            return false;
        }
        return true;
    }

    static void internalizeForCell(llvm::Module& mod,
                                   const std::vector<std::string>& preserve_symbols) {
        std::set<std::string> preserve(preserve_symbols.begin(), preserve_symbols.end());

        llvm::LoopAnalysisManager LAM;
        llvm::FunctionAnalysisManager FAM;
        llvm::CGSCCAnalysisManager CGAM;
        llvm::ModuleAnalysisManager MAM;
        llvm::PassBuilder PB;
        PB.registerModuleAnalyses(MAM);
        PB.registerCGSCCAnalyses(CGAM);
        PB.registerFunctionAnalyses(FAM);
        PB.registerLoopAnalyses(LAM);
        PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

        llvm::ModulePassManager MPM;
        MPM.addPass(llvm::InternalizePass([&](const llvm::GlobalValue& gv) {
            return preserve.find(gv.getName().str()) != preserve.end();
        }));
        MPM.addPass(llvm::GlobalDCEPass());
        MPM.addPass(llvm::StripDeadPrototypesPass());
        MPM.run(mod, MAM);
    }

    static std::string mirGlobalUserName(const std::string& symbol) {
        static constexpr std::string_view kPrefix = "mir.global.";
        if (symbol.rfind(kPrefix, 0) != 0) {
            return {};
        }
        std::size_t pos = kPrefix.size();
        if (pos >= symbol.size() || !std::isdigit(static_cast<unsigned char>(symbol[pos]))) {
            return {};
        }
        while (pos < symbol.size()
            && std::isdigit(static_cast<unsigned char>(symbol[pos]))) {
            ++pos;
        }
        if (pos >= symbol.size() || symbol[pos] != '.') {
            return {};
        }
        std::string user = symbol.substr(pos + 1u);
        if (user.empty() || !vyx_repl_ident_start(user.front())) {
            return {};
        }
        for (char c : user) {
            if (!vyx_repl_ident_char(c)) {
                return {};
            }
        }
        return user;
    }

    static void normalizeMirGlobalsForCell(
        llvm::Module& mod,
        const std::vector<std::string>& defined_globals,
        const std::vector<std::string>& external_globals) {
        std::set<std::string> defined(defined_globals.begin(), defined_globals.end());
        std::set<std::string> external(external_globals.begin(), external_globals.end());
        if (defined.empty() && external.empty()) {
            return;
        }
        std::vector<llvm::GlobalVariable*> globals;
        for (llvm::GlobalVariable& gv : mod.globals()) {
            globals.push_back(&gv);
        }
        for (llvm::GlobalVariable* gv : globals) {
            if (!gv || !gv->hasName()) {
                continue;
            }
            const std::string user = mirGlobalUserName(gv->getName().str());
            if (user.empty()) {
                continue;
            }
            if (defined.find(user) != defined.end()) {
                gv->setName(user);
                gv->setLinkage(llvm::GlobalValue::ExternalLinkage);
            } else if (external.find(user) != external.end()) {
                gv->setName(user);
                gv->setInitializer(nullptr);
                gv->setLinkage(llvm::GlobalValue::ExternalLinkage);
                gv->setConstant(false);
            }
        }
    }

    bool loadDynamicLibraries(const std::string& link_args, std::string& err) {
        const std::vector<std::string> args = vyx_repl_split_link_args(link_args);
        std::vector<std::filesystem::path> search_paths;
        search_paths.emplace_back(".");
        std::vector<std::string> libs;
        for (std::size_t i = 0; i < args.size(); ++i) {
            const std::string& a = args[i];
            if ((a == "-L" || a == "--lib-path") && i + 1u < args.size()) {
                search_paths.emplace_back(args[++i]);
            } else if (a.rfind("-L", 0) == 0 && a.size() > 2u) {
                search_paths.emplace_back(a.substr(2u));
            } else if ((a == "-l" || a == "--link") && i + 1u < args.size()) {
                libs.push_back(args[++i]);
            } else if (a.rfind("-l", 0) == 0 && a.size() > 2u) {
                libs.push_back(a.substr(2u));
            }
        }

        for (const std::string& lib : libs) {
            bool loaded = false;
            std::string last_err;
            for (const auto& name : vyx_repl_dynamic_library_names(lib)) {
                std::vector<std::string> candidates;
                if (vyx_repl_path_explicit(name)) {
                    candidates.push_back(name);
                } else {
                    for (const auto& dir : search_paths) {
                        candidates.push_back((dir / name).string());
                    }
                    candidates.push_back(name);
                }
                for (const auto& candidate : candidates) {
                    std::error_code ec;
                    if (vyx_repl_path_explicit(candidate)
                        && !std::filesystem::exists(candidate, ec)) {
                        continue;
                    }
                    last_err.clear();
                    if (!llvm::sys::DynamicLibrary::LoadLibraryPermanently(candidate.c_str(),
                                                                            &last_err)) {
                        loaded = true;
                        break;
                    }
                }
                if (loaded) {
                    break;
                }
            }
            if (!loaded) {
                if (last_err.empty()) {
                    last_err = "no dynamic library candidate was found";
                }
                err = "JIT failed to load library '" + lib + "': " + last_err;
                return false;
            }
        }
        return true;
    }
};

struct VyxReplCell {
    std::string text;
    bool is_top = false;
    bool is_statement = false;
    bool is_var_decl = false;
    VyxReplVarDecl var_decl;
};

#ifdef _WIN32
static std::wstring vyx_repl_utf8_to_wide(const std::string& text) {
    if (text.empty()) {
        return L"";
    }
    const int needed = MultiByteToWideChar(CP_UTF8,
                                           MB_ERR_INVALID_CHARS,
                                           text.data(),
                                           static_cast<int>(text.size()),
                                           nullptr,
                                           0);
    if (needed <= 0) {
        std::wstring fallback;
        fallback.reserve(text.size());
        for (unsigned char c : text) {
            fallback.push_back(static_cast<wchar_t>(c));
        }
        return fallback;
    }
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8,
                        MB_ERR_INVALID_CHARS,
                        text.data(),
                        static_cast<int>(text.size()),
                        out.data(),
                        needed);
    return out;
}

static std::string vyx_repl_wide_to_utf8(const std::wstring& text) {
    if (text.empty()) {
        return "";
    }
    const int needed = WideCharToMultiByte(CP_UTF8,
                                           0,
                                           text.data(),
                                           static_cast<int>(text.size()),
                                           nullptr,
                                           0,
                                           nullptr,
                                           nullptr);
    if (needed <= 0) {
        std::string fallback;
        fallback.reserve(text.size());
        for (wchar_t c : text) {
            fallback.push_back(static_cast<char>(c & 0x7f));
        }
        return fallback;
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8,
                        0,
                        text.data(),
                        static_cast<int>(text.size()),
                        out.data(),
                        needed,
                        nullptr,
                        nullptr);
    return out;
}

static void vyx_repl_write_console_wide(HANDLE output, const std::wstring& text) {
    if (text.empty() || output == nullptr || output == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD mode = 0;
    if (GetConsoleMode(output, &mode)) {
        DWORD written = 0;
        WriteConsoleW(output,
                      text.data(),
                      static_cast<DWORD>(text.size()),
                      &written,
                      nullptr);
        return;
    }
    const std::string utf8 = vyx_repl_wide_to_utf8(text);
    DWORD written = 0;
    WriteFile(output,
              utf8.data(),
              static_cast<DWORD>(utf8.size()),
              &written,
              nullptr);
}

static bool vyx_repl_completion_word_char(wchar_t c) {
    return (c >= L'a' && c <= L'z')
        || (c >= L'A' && c <= L'Z')
        || (c >= L'0' && c <= L'9')
        || c == L'_' || c == L':' || c == L'.';
}
#endif

class VyxReplInput {
public:
    void setCompletionContext(std::string prompt,
                              std::vector<std::string> candidates) {
        completion_prompt_ = std::move(prompt);
        completion_candidates_ = std::move(candidates);
    }

    bool readLine(std::string& out) {
        out.clear();
#ifdef _WIN32
        if (!readLineWindows(out)) {
            return false;
        }
#else
        if (!std::getline(std::cin, out)) {
            return false;
        }
#endif
        vyx_repl_strip_utf8_bom(out);
        return true;
    }

private:
    std::string completion_prompt_;
    std::vector<std::string> completion_candidates_;
    std::vector<std::wstring> history_;

#ifdef _WIN32
    std::string pending_;
    bool eof_ = false;

    bool debugIo() const {
        const char* raw = std::getenv("VYX_REPL_IO_DEBUG");
        return raw && raw[0] != '\0';
    }

    bool readLineWindows(std::string& out) {
        HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
        if (input == nullptr || input == INVALID_HANDLE_VALUE) {
            if (debugIo()) {
                vyx_repl_write_stderr("[repl-io] invalid stdin handle\n");
            }
            return false;
        }
        DWORD console_mode = 0;
        if (GetFileType(input) == FILE_TYPE_CHAR
            && GetConsoleMode(input, &console_mode)) {
            return readConsoleLineWindows(input, out);
        }
        if (debugIo()) {
            vyx_repl_write_stderr(
                "[repl-io] stdin type=" + std::to_string(GetFileType(input))
                + " pending=" + std::to_string(pending_.size())
                + " eof=" + std::to_string(eof_ ? 1 : 0) + "\n");
        }
        for (;;) {
            const std::size_t newline = pending_.find('\n');
            if (newline != std::string::npos) {
                out = pending_.substr(0, newline);
                pending_.erase(0, newline + 1u);
                if (!out.empty() && out.back() == '\r') {
                    out.pop_back();
                }
                if (debugIo()) {
                    vyx_repl_write_stderr(
                        "[repl-io] line len=" + std::to_string(out.size())
                        + " text=" + out + "\n");
                }
                return true;
            }
            if (eof_) {
                if (pending_.empty()) {
                    return false;
                }
                out = pending_;
                pending_.clear();
                if (!out.empty() && out.back() == '\r') {
                    out.pop_back();
                }
                if (debugIo()) {
                    vyx_repl_write_stderr(
                        "[repl-io] final line len=" + std::to_string(out.size())
                        + " text=" + out + "\n");
                }
                return true;
            }

            char buf[4096];
            DWORD read = 0;
            const BOOL ok = ReadFile(input,
                                     buf,
                                     static_cast<DWORD>(sizeof(buf)),
                                     &read,
                                     nullptr);
            if (debugIo()) {
                vyx_repl_write_stderr(
                    "[repl-io] read ok=" + std::to_string(ok ? 1 : 0)
                    + " bytes=" + std::to_string(read)
                    + " err=" + std::to_string(GetLastError()) + "\n");
            }
            if (!ok || read == 0) {
                eof_ = true;
                continue;
            }
            pending_.append(buf, buf + read);
        }
    }

    void redrawConsoleLine(HANDLE output,
                           const COORD& origin,
                           const std::wstring& line,
                           std::size_t cursor,
                           std::size_t& rendered_len) {
        const std::size_t clear_len = std::max(rendered_len, line.size());
        SetConsoleCursorPosition(output, origin);
        if (clear_len > 0) {
            vyx_repl_write_console_wide(output,
                                        std::wstring(clear_len, L' '));
            SetConsoleCursorPosition(output, origin);
        }
        vyx_repl_write_console_wide(output, line);
        CONSOLE_SCREEN_BUFFER_INFO info;
        COORD pos = origin;
        if (GetConsoleScreenBufferInfo(output, &info)) {
            const SHORT width = static_cast<SHORT>(
                std::max<SHORT>(1, info.dwSize.X));
            const std::size_t absolute =
                static_cast<std::size_t>(std::max<SHORT>(0, origin.X))
                + std::min(cursor, line.size());
            pos.X = static_cast<SHORT>(absolute % width);
            pos.Y = static_cast<SHORT>(
                origin.Y + static_cast<SHORT>(absolute / width));
        } else {
            pos.X = static_cast<SHORT>(
                origin.X + static_cast<SHORT>(std::min(cursor, line.size())));
        }
        SetConsoleCursorPosition(output, pos);
        rendered_len = line.size();
    }

    void redrawConsolePromptAndLine(HANDLE output,
                                    COORD& origin,
                                    const std::wstring& line,
                                    std::size_t cursor,
                                    std::size_t& rendered_len) {
        vyx_repl_write_console_wide(output,
                                    vyx_repl_utf8_to_wide(completion_prompt_));
        CONSOLE_SCREEN_BUFFER_INFO info;
        if (GetConsoleScreenBufferInfo(output, &info)) {
            origin = info.dwCursorPosition;
        }
        rendered_len = 0;
        redrawConsoleLine(output, origin, line, cursor, rendered_len);
    }

    void completeConsoleLine(std::wstring& line,
                             std::size_t& cursor,
                             HANDLE output,
                             COORD& origin,
                             std::size_t& rendered_len) {
        if (cursor > line.size()) {
            cursor = line.size();
        }
        std::size_t start = cursor;
        while (start > 0 && vyx_repl_completion_word_char(line[start - 1u])) {
            --start;
        }
        const std::wstring wide_prefix = line.substr(start, cursor - start);
        const std::string prefix = vyx_repl_wide_to_utf8(wide_prefix);
        const auto matches = vyx_repl_completion_matches(completion_candidates_,
                                                         prefix);
        if (matches.empty()) {
            vyx_repl_write_console_wide(output, L"\a");
            return;
        }
        if (matches.size() == 1u) {
            const std::string& candidate = matches.front();
            if (candidate.size() <= prefix.size()) {
                return;
            }
            const std::wstring suffix =
                vyx_repl_utf8_to_wide(candidate.substr(prefix.size()));
            line.insert(cursor, suffix);
            cursor += suffix.size();
            redrawConsoleLine(output, origin, line, cursor, rendered_len);
            return;
        }
        std::vector<std::string> lines;
        for (const auto& match : matches) {
            lines.push_back(match);
        }
        const std::wstring listing = vyx_repl_utf8_to_wide(
            "\r\n" + vyx_repl_join_lines(lines));
        vyx_repl_write_console_wide(output, listing);
        redrawConsolePromptAndLine(output, origin, line, cursor, rendered_len);
    }

    void recordConsoleHistory(const std::wstring& line) {
        if (line.empty()) {
            return;
        }
        if (!history_.empty() && history_.back() == line) {
            return;
        }
        history_.push_back(line);
    }

    bool readConsoleLineWindows(HANDLE input, std::string& out) {
        HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
        std::wstring line;
        std::size_t cursor = 0;
        std::size_t rendered_len = 0;
        std::size_t history_index = history_.size();
        COORD origin{0, 0};
        CONSOLE_SCREEN_BUFFER_INFO info;
        if (GetConsoleScreenBufferInfo(output, &info)) {
            origin = info.dwCursorPosition;
        }
        for (;;) {
            INPUT_RECORD rec;
            DWORD read = 0;
            if (!ReadConsoleInputW(input, &rec, 1, &read) || read == 0) {
                return false;
            }
            if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) {
                continue;
            }
            const WORD key = rec.Event.KeyEvent.wVirtualKeyCode;
            const wchar_t ch = rec.Event.KeyEvent.uChar.UnicodeChar;
            const DWORD ctrl_state = rec.Event.KeyEvent.dwControlKeyState;
            const bool ctrl = (ctrl_state & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
            if (key == VK_RETURN) {
                vyx_repl_write_console_wide(output, L"\r\n");
                out = vyx_repl_wide_to_utf8(line);
                recordConsoleHistory(line);
                return true;
            }
            if (key == VK_TAB) {
                completeConsoleLine(line, cursor, output, origin, rendered_len);
                continue;
            }
            if (key == VK_LEFT) {
                if (cursor > 0) {
                    --cursor;
                    redrawConsoleLine(output, origin, line, cursor, rendered_len);
                }
                continue;
            }
            if (key == VK_RIGHT) {
                if (cursor < line.size()) {
                    ++cursor;
                    redrawConsoleLine(output, origin, line, cursor, rendered_len);
                }
                continue;
            }
            if (key == VK_HOME || (ctrl && (ch == L'a' || ch == L'A'))) {
                cursor = 0;
                redrawConsoleLine(output, origin, line, cursor, rendered_len);
                continue;
            }
            if (key == VK_END || (ctrl && (ch == L'e' || ch == L'E'))) {
                cursor = line.size();
                redrawConsoleLine(output, origin, line, cursor, rendered_len);
                continue;
            }
            if (key == VK_UP) {
                if (!history_.empty() && history_index > 0) {
                    --history_index;
                    line = history_[history_index];
                    cursor = line.size();
                    redrawConsoleLine(output, origin, line, cursor, rendered_len);
                }
                continue;
            }
            if (key == VK_DOWN) {
                if (history_index < history_.size()) {
                    ++history_index;
                    if (history_index == history_.size()) {
                        line.clear();
                    } else {
                        line = history_[history_index];
                    }
                    cursor = line.size();
                    redrawConsoleLine(output, origin, line, cursor, rendered_len);
                }
                continue;
            }
            if (key == VK_DELETE) {
                if (cursor < line.size()) {
                    line.erase(cursor, 1u);
                    redrawConsoleLine(output, origin, line, cursor, rendered_len);
                }
                continue;
            }
            if (key == VK_BACK) {
                if (cursor > 0) {
                    line.erase(cursor - 1u, 1u);
                    --cursor;
                    redrawConsoleLine(output, origin, line, cursor, rendered_len);
                }
                continue;
            }
            if (key == VK_ESCAPE) {
                line.clear();
                cursor = 0;
                redrawConsoleLine(output, origin, line, cursor, rendered_len);
                continue;
            }
            if (ch == 3) {
                return false;
            }
            if (ctrl && (ch == L'u' || ch == L'U')) {
                line.erase(0, cursor);
                cursor = 0;
                redrawConsoleLine(output, origin, line, cursor, rendered_len);
                continue;
            }
            if (ctrl && (ch == L'k' || ch == L'K')) {
                line.erase(cursor);
                redrawConsoleLine(output, origin, line, cursor, rendered_len);
                continue;
            }
            if (ch >= 32) {
                line.insert(cursor, 1u, ch);
                ++cursor;
                redrawConsoleLine(output, origin, line, cursor, rendered_len);
            }
        }
    }
#endif
};

static VyxReplCell vyx_repl_classify_cell(std::string text) {
    VyxReplCell cell;
    cell.text = vyx_repl_trim(std::move(text));
    cell.var_decl = vyx_repl_parse_var_decl(cell.text);
    cell.is_var_decl = cell.var_decl.valid;
    cell.is_top = !cell.is_var_decl && vyx_repl_is_top_level(cell.text);
    if (!cell.is_top && !cell.is_var_decl) {
        const std::string stripped = vyx_repl_strip_leading_modifiers(cell.text);
        const bool looks_if_expr = (vyx_repl_starts_with(stripped, "if ")
            || vyx_repl_starts_with(stripped, "if("))
            && vyx_repl_contains(stripped, "else")
            && !vyx_repl_ends_with(stripped, ";");
        cell.is_statement = !looks_if_expr
            && (vyx_repl_ends_with(cell.text, ";")
                || vyx_repl_ends_with(cell.text, "}"));
    }
    return cell;
}

static bool vyx_repl_missing_semicolon_after_known_statement_call(
    const std::string& text) {
    const std::string stripped = vyx_repl_strip_leading_modifiers(text);
    if (vyx_repl_ends_with(stripped, ";")) {
        return false;
    }
    if (!vyx_repl_starts_with(stripped, "print")) {
        return false;
    }
    std::size_t pos = 5u;
    while (pos < stripped.size()
           && std::isspace(static_cast<unsigned char>(stripped[pos]))) {
        ++pos;
    }
    return pos < stripped.size() && stripped[pos] == '(';
}

static void vyx_repl_print_help() {
    vyx_repl_write_stdout(
        "Vyx LLVM REPL\n"
        "  :quit / :exit  leave the session\n"
        "  :clear         reset the live JIT session and clear the console\n"
        "  clear / cls    clear the console\n"
        "  :help          show this help\n"
        "  :load <file>   compile one source file as a REPL cell\n"
        "  :dump          print declarations and live globals known to the session\n"
        "  :export [file] print or write replayable REPL input\n"
        "  :del <name>    delete a top-level declaration or live binding metadata\n"
        "  :complete <p>  print completion candidates for a prefix\n"
        "  :{             start manual multiline cell; close with :}\n"
        "Tab completes commands, builtins, top-level declarations, and live globals.\n"
        "Line editing: Left/Right/Home/End/Delete, Up/Down history, Ctrl+A/E/U/K.\n"
        "Top-level let/var cells become live JIT globals; later cells mutate the same storage.\n"
        "Balanced multiline input is accepted automatically.\n");
}

static bool vyx_repl_debug_io_enabled() {
    const char* raw = std::getenv("VYX_REPL_IO_DEBUG");
    return raw && raw[0] != '\0';
}

extern "C" VYX_RT_ABI int32_t vyx_bootstrap_repl_run(const char* self_exe,
                                                     const char* compiler_args) {
    const std::string self = vyx_repl_self_path(self_exe);
    const std::string forwarded_args = compiler_args ? std::string(compiler_args) : std::string();
    const bool keep_temp = vyx_repl_keep_temp();
    const uint64_t pid = vyx_repl_pid();
    const std::filesystem::path repl_dir = std::filesystem::path(".cache") / "repl";
    std::error_code ec;
    std::filesystem::create_directories(repl_dir, ec);

    std::string top_source;
    std::vector<VyxReplTopDecl> top_decls;
    std::vector<VyxReplGlobal> globals;
    std::vector<std::string> completion_names;
    std::vector<std::string> replay_cells;
    std::vector<std::filesystem::path> cell_paths;
    std::deque<std::string> queued_cells;
    int32_t counter = 0;
    VyxReplInput input;
    std::string jit_err;
    auto jit = VyxReplJitSession::create(forwarded_args, jit_err);
    if (!jit) {
        vyx_repl_write_stdout(vyx_repl_diag(
            "fatal", "I0101", "failed to create JIT session: " + jit_err));
        return 1;
    }

    vyx_repl_print_help();

    for (;;) {
        std::string cell_text;
        if (!queued_cells.empty()) {
            cell_text = queued_cells.front();
            queued_cells.pop_front();
        }
        bool force_multiline = false;
        if (cell_text.empty()) {
            for (;;) {
                const std::string prompt =
                    (cell_text.empty() && !force_multiline) ? "vyx> " : "...> ";
                vyx_repl_write_stdout(prompt);
                input.setCompletionContext(prompt,
                                           vyx_repl_current_completions(completion_names));
                std::string raw;
                if (!input.readLine(raw)) {
                    if (cell_text.empty()) {
                        vyx_repl_cleanup_paths(cell_paths);
                        return 0;
                    }
                    break;
                }
                const std::string line = vyx_repl_trim(raw);
                if (cell_text.empty() && !force_multiline) {
                    if (line.empty()) {
                        ++counter;
                        continue;
                    }
                    if (line == ":quit" || line == ":exit") {
                        vyx_repl_cleanup_paths(cell_paths);
                        return 0;
                    }
                    if (line == ":help") {
                        vyx_repl_print_help();
                        continue;
                    }
                    if (line == "clear" || line == "cls"
                        || line == ":cls" || line == ":clear-screen") {
                        vyx_repl_clear_console();
                        continue;
                    }
                    if (line == ":clear") {
                        top_source.clear();
                        top_decls.clear();
                        globals.clear();
                        completion_names.clear();
                        replay_cells.clear();
                        vyx_repl_cleanup_paths(cell_paths);
                        cell_paths.clear();
                        jit_err.clear();
                        jit = VyxReplJitSession::create(forwarded_args, jit_err);
                        if (!jit) {
                            vyx_repl_write_stdout(vyx_repl_diag(
                                "fatal", "I0101", "failed to reset JIT session: " + jit_err));
                            return 1;
                        }
                        vyx_repl_clear_console();
                        continue;
                    }
                    if (line == ":dump") {
                        vyx_repl_write_stdout("-- declarations --\n" + top_source);
                        vyx_repl_write_stdout("-- live globals --\n");
                        for (const auto& g : globals) {
                            vyx_repl_write_stdout(g.name + ": " + g.type + "\n");
                        }
                        continue;
                    }
                    if (line == ":export" || vyx_repl_starts_with(line, ":export ")) {
                        const std::string export_path = (line == ":export")
                            ? ""
                            : vyx_repl_trim(line.substr(8));
                        const std::string export_source =
                            vyx_repl_export_replay(replay_cells);
                        if (export_path.empty()) {
                            vyx_repl_write_stdout(export_source);
                        } else {
                            std::string export_err;
                            if (!vyx_repl_export_to_file(export_path,
                                                         export_source,
                                                         export_err)) {
                                vyx_repl_write_stdout(vyx_repl_diag(
                                    "error",
                                    "E0003",
                                    "failed to export `" + export_path + "`: " + export_err));
                            } else {
                                vyx_repl_write_stdout("exported " + export_path + "\n");
                            }
                        }
                        continue;
                    }
                    if (line == ":del" || vyx_repl_starts_with(line, ":del ")) {
                        const std::string delete_name = (line == ":del")
                            ? ""
                            : vyx_repl_trim(line.substr(5));
                        if (delete_name.empty()) {
                            vyx_repl_write_stdout(vyx_repl_diag(
                                "error", "E0003", "usage: :del <name>"));
                            continue;
                        }
                        const bool deleted_top =
                            vyx_repl_delete_top_decl(top_decls, delete_name);
                        const bool has_global =
                            vyx_repl_find_global(globals, delete_name) != nullptr;
                        if (has_global) {
                            std::string remove_err;
                            if (!jit->removeOwnedSymbol(delete_name, remove_err)) {
                                vyx_repl_write_stdout(vyx_repl_diag(
                                    "error",
                                    "I0101",
                                    "failed to delete live binding `" + delete_name
                                    + "`: " + remove_err));
                                continue;
                            }
                        }
                        const bool deleted_global = has_global
                            && vyx_repl_delete_global(globals, delete_name);
                        if (!deleted_top && !deleted_global) {
                            vyx_repl_write_stdout(vyx_repl_diag(
                                "error",
                                "E2000",
                                "no REPL definition named `" + delete_name + "`"));
                            continue;
                        }
                        top_source = vyx_repl_top_source_from_decls(top_decls);
                        vyx_repl_rebuild_completions(completion_names,
                                                     top_decls,
                                                     globals);
                        replay_cells.push_back(":del " + delete_name + "\n");
                        vyx_repl_write_stdout("deleted " + delete_name + "\n");
                        continue;
                    }
                    if (line == ":complete"
                        || vyx_repl_starts_with(line, ":complete ")) {
                        const std::string prefix = (line == ":complete")
                            ? ""
                            : vyx_repl_trim(line.substr(10));
                        const auto matches = vyx_repl_completion_matches(
                            vyx_repl_current_completions(completion_names),
                            prefix);
                        if (matches.empty()) {
                            vyx_repl_write_stdout("(no completions)\n");
                        } else {
                            vyx_repl_write_stdout(vyx_repl_join_lines(matches));
                        }
                        continue;
                    }
                    if (line == ":load" || vyx_repl_starts_with(line, ":load ")) {
                        const std::string load_path = (line == ":load")
                            ? ""
                            : vyx_repl_trim(line.substr(6));
                        if (load_path.empty()) {
                            vyx_repl_write_stdout(vyx_repl_diag(
                                "error", "E0003", "usage: :load <file>"));
                            continue;
                        }
                        std::string loaded;
                        if (!vyx_repl_load_file(load_path, loaded)) {
                            vyx_repl_write_stdout(vyx_repl_diag(
                                "error", "E0003", "failed to load `" + load_path + "`"));
                            continue;
                        }
                        const auto loaded_cells = vyx_repl_split_loaded_cells(loaded);
                        for (const auto& loaded_cell : loaded_cells) {
                            queued_cells.push_back(loaded_cell);
                        }
                        break;
                    }
                    if (line == ":{") {
                        force_multiline = true;
                        continue;
                    }
                } else if (force_multiline && line == ":}") {
                    break;
                }
                cell_text += raw;
                cell_text += "\n";
                if (vyx_repl_cell_complete(cell_text, force_multiline)) {
                    break;
                }
            }
        }

        VyxReplCell cell = vyx_repl_classify_cell(cell_text);
        if (cell.text.empty()) {
            ++counter;
            continue;
        }
        replay_cells.push_back(cell.text + "\n");
        if (vyx_repl_missing_semicolon_after_known_statement_call(cell.text)) {
            vyx_repl_write_stdout(vyx_repl_diag(
                "error",
                "E0002",
                "statement call requires ';': write `print(...);`"));
            ++counter;
            continue;
        }
        const std::string bare_ident = vyx_repl_trim(cell.text);
        if (vyx_repl_is_bare_identifier(bare_ident)
            && !vyx_repl_is_literal_keyword(bare_ident)
            && vyx_repl_find_global(globals, bare_ident) == nullptr) {
            if (bare_ident == "print") {
                vyx_repl_write_stdout(vyx_repl_diag(
                    "error",
                    "E0002",
                    "builtin function `print` requires a call: write `print(...);`"));
            } else {
                vyx_repl_write_stdout(vyx_repl_diag(
                    "error",
                    "E2000",
                    "unknown REPL value `" + bare_ident + "`"));
            }
            ++counter;
            continue;
        }
        std::string candidate_top = top_source;
        std::vector<VyxReplTopDecl> candidate_top_decls = top_decls;
        std::string candidate_top_key;
        if (cell.is_top) {
            candidate_top_key = vyx_repl_decl_completion_name(cell.text);
            vyx_repl_upsert_top_decl(candidate_top_decls,
                                     candidate_top_key,
                                     cell.text);
            candidate_top = vyx_repl_top_source_from_decls(candidate_top_decls);
        }
        if (cell.is_var_decl && cell.var_decl.type.empty()) {
            vyx_repl_write_stdout(vyx_repl_diag(
                "error",
                "E1000",
                "persistent variable `" + cell.var_decl.name
                + "` needs an explicit type for this initializer"));
            ++counter;
            continue;
        }
        const std::string cell_fn_name = "__vyx_repl_cell_"
            + std::to_string(pid)
            + "_"
            + std::to_string(counter);
        const std::string cell_entry_name = cell_fn_name + "_entry";
        const std::string cell_entry_symbol =
            vyx_repl_mangled_i32_noarg_fn(cell_entry_name);
        bool define_new_global = false;
        bool replace_global_type = false;
        std::vector<VyxReplGlobal> source_globals = globals;
        if (cell.is_var_decl) {
            const VyxReplGlobal* existing_global =
                vyx_repl_find_global(source_globals, cell.var_decl.name);
            if (existing_global != nullptr
                && existing_global->type != cell.var_decl.type) {
                replace_global_type = true;
                vyx_repl_delete_global(source_globals, cell.var_decl.name);
            }
            define_new_global =
                (vyx_repl_find_global(source_globals, cell.var_decl.name) == nullptr);
        }
        std::string source;
        if (cell.is_top) {
            source = vyx_repl_prelude_source(candidate_top, globals, "");
        } else if (cell.is_var_decl) {
            source = vyx_repl_exec_source(top_source,
                                          source_globals,
                                          &cell.var_decl,
                                          "",
                                          true,
                                          cell_fn_name,
                                          cell_entry_name,
                                          define_new_global);
        } else {
            source = vyx_repl_exec_source(top_source,
                                          globals,
                                          nullptr,
                                          cell.text,
                                          cell.is_statement,
                                          cell_fn_name,
                                          cell_entry_name,
                                          false);
        }
        const std::filesystem::path path = repl_dir / (
            std::string("cell_")
            + std::to_string(pid)
            + "_"
            + std::to_string(counter)
            + ".vyx");
        {
            std::ofstream out(path, std::ios::binary);
            if (!out) {
                vyx_repl_write_stdout(vyx_repl_diag(
                    "fatal",
                    "I0101",
                    "failed to write temp source `" + path.string() + "`"));
                return 1;
            }
            out << source;
        }
        cell_paths.push_back(path);

        if (vyx_repl_debug_io_enabled()) {
            vyx_repl_write_stderr("[repl-io] running cell "
                                  + path.string() + "\n");
        }
        std::filesystem::path ir_path;
        int32_t rc = vyx_repl_compile_cell(self,
                                           forwarded_args,
                                           path,
                                           ir_path);
        cell_paths.push_back(ir_path);
        if (vyx_repl_debug_io_enabled()) {
            vyx_repl_write_stderr("[repl-io] compile exit="
                                  + std::to_string(rc) + "\n");
        }
        if (rc == 0) {
            if (cell.is_top) {
                top_decls = candidate_top_decls;
                top_source = candidate_top;
                vyx_repl_rebuild_completions(completion_names,
                                             top_decls,
                                             globals);
            } else {
                std::vector<std::string> preserve;
                preserve.push_back(cell_entry_symbol);
                std::vector<std::string> defined_globals;
                std::vector<std::string> external_globals;
                const std::vector<VyxReplGlobal>& visible_globals =
                    cell.is_var_decl ? source_globals : globals;
                for (const auto& global : visible_globals) {
                    external_globals.push_back(global.name);
                    preserve.push_back(global.name);
                }
                if (cell.is_var_decl && define_new_global) {
                    preserve.push_back(cell.var_decl.name);
                    defined_globals.push_back(cell.var_decl.name);
                }
                std::string err;
                if (cell.is_var_decl && replace_global_type) {
                    if (!jit->removeOwnedSymbol(cell.var_decl.name, err)) {
                        vyx_repl_write_stdout(vyx_repl_diag(
                            "error",
                            "I0101",
                            "failed to replace live binding `" + cell.var_decl.name
                            + "`: " + err));
                        rc = 1;
                    } else {
                        vyx_repl_delete_global(globals, cell.var_decl.name);
                    }
                }
                const std::string owned_symbol =
                    (cell.is_var_decl && define_new_global)
                        ? cell.var_decl.name
                        : std::string();
                if (rc == 0) {
                    if (!jit->addIRFile(ir_path,
                                        preserve,
                                        defined_globals,
                                        external_globals,
                                        err,
                                        owned_symbol)) {
                        vyx_repl_write_stdout(vyx_repl_diag(
                            "error", "I0101", "jit add failed: " + err));
                        rc = 1;
                    } else {
                        int32_t run_rc = 0;
                        if (!jit->runI32(cell_entry_symbol, run_rc, err)) {
                            vyx_repl_write_stdout(vyx_repl_diag(
                                "error", "I0101", "jit run failed: " + err));
                            rc = 1;
                        } else {
                            rc = run_rc;
                        }
                    }
                }
                if (rc == 0 && cell.is_var_decl) {
                    if (define_new_global) {
                        vyx_repl_upsert_global(globals,
                                               cell.var_decl.name,
                                               cell.var_decl.type);
                    }
                    vyx_repl_rebuild_completions(completion_names,
                                                 top_decls,
                                                 globals);
                }
            }
        }
        if (keep_temp) {
            vyx_repl_write_stdout("note: kept temp source "
                                  + path.string() + "\n");
        }
        if (!keep_temp) {
            std::error_code rm_ec;
            std::filesystem::remove(ir_path, rm_ec);
        }
        if (rc != 0) {
            vyx_repl_write_stdout(vyx_repl_diag(
                "error", "I0101", "cell failed with exit " + std::to_string(rc)));
        }
        ++counter;
    }
}

extern "C" VYX_RT_ABI int32_t vyx_bootstrap_copy_file(const char* src,
                                                      const char* dst) {
    if (!src || !src[0] || !dst || !dst[0]) {
        return -1;
    }
    std::filesystem::path src_path(src);
    std::filesystem::path dst_path(dst);
    std::error_code ec;
    if (std::filesystem::equivalent(src_path, dst_path, ec) && !ec) {
        return 0;
    }
    ec.clear();
    const auto parent = dst_path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return -1;
        }
    }
    ec.clear();
    std::filesystem::copy_file(src_path,
                               dst_path,
                               std::filesystem::copy_options::overwrite_existing,
                               ec);
    return ec ? -1 : 0;
}

static bool vyx_bootstrap_skip_publish_name(const std::string& name) {
    return name == ".cache" || name == "out" || name == ".git"
        || name == ".vyx-registry" || name == ".vyx_cache";
}

extern "C" VYX_RT_ABI int32_t vyx_bootstrap_copy_tree(const char* src,
                                                      const char* dst) {
    if (!src || !src[0] || !dst || !dst[0]) {
        return -1;
    }
    const std::filesystem::path src_path(src);
    const std::filesystem::path dst_path(dst);
    std::error_code ec;
    if (!std::filesystem::exists(src_path, ec) || ec) {
        return -1;
    }
    ec.clear();
    if (std::filesystem::exists(dst_path, ec)
        && !ec
        && std::filesystem::equivalent(src_path, dst_path, ec)
        && !ec) {
        return 0;
    }
    ec.clear();
    std::filesystem::create_directories(dst_path, ec);
    if (ec) {
        return -1;
    }
    if (std::filesystem::is_regular_file(src_path)) {
        ec.clear();
        std::filesystem::copy_file(src_path,
                                   dst_path / src_path.filename(),
                                   std::filesystem::copy_options::overwrite_existing,
                                   ec);
        return ec ? -1 : 0;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             src_path, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (ec) {
            return -1;
        }
        const auto rel = std::filesystem::relative(entry.path(), src_path, ec);
        if (ec || rel.empty()) {
            continue;
        }
        bool skip = false;
        for (const auto& part : rel) {
            if (vyx_bootstrap_skip_publish_name(part.string())) {
                skip = true;
                break;
            }
        }
        if (skip) {
            continue;
        }
        const auto out = dst_path / rel;
        if (entry.is_directory()) {
            std::filesystem::create_directories(out, ec);
            if (ec) {
                return -1;
            }
            continue;
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        std::filesystem::create_directories(out.parent_path(), ec);
        if (ec) {
            return -1;
        }
        ec.clear();
        std::filesystem::copy_file(entry.path(),
                                   out,
                                   std::filesystem::copy_options::overwrite_existing,
                                   ec);
        if (ec) {
            return -1;
        }
    }
    return 0;
}

extern "C" VYX_RT_ABI int32_t vyx_string_starts_with(const char* a,
                                                     uint64_t      alen,
                                                     const char*   b,
                                                     uint64_t      blen) {
    VYX_BOOTSTRAP_RT_PROF(StartsWith);
    if (!a || !b) {
        return 0;
    }
    if (alen == 0) {
        alen = vyx_rt_known_string_len(a);
    }
    if (blen == 0) {
        blen = vyx_rt_known_string_len(b);
    }
    alen = vyx_rt_clip_string_len(a, alen);
    blen = vyx_rt_clip_string_len(b, blen);
    if (blen > alen) {
        return 0;
    }
    return std::memcmp(a, b, static_cast<size_t>(blen)) == 0 ? 1 : 0;
}

extern "C" VYX_RT_ABI int32_t vyx_string_ends_with(const char* a,
                                                   uint64_t      alen,
                                                   const char*   b,
                                                   uint64_t      blen) {
    VYX_BOOTSTRAP_RT_PROF(EndsWith);
    if (!a || !b) {
        return 0;
    }
    if (alen == 0) {
        alen = vyx_rt_known_string_len(a);
    }
    if (blen == 0) {
        blen = vyx_rt_known_string_len(b);
    }
    alen = vyx_rt_clip_string_len(a, alen);
    blen = vyx_rt_clip_string_len(b, blen);
    if (blen > alen) {
        return 0;
    }
    return std::memcmp(a + (alen - blen), b, static_cast<size_t>(blen)) == 0
               ? 1
               : 0;
}

extern "C" VYX_RT_ABI int32_t vyx_string_contains(const char* hay,
                                                   uint64_t hay_len,
                                                   const char* needle,
                                                   uint64_t needle_len) {
    VYX_BOOTSTRAP_RT_PROF(Contains);
    if (!hay || !needle) {
        return 0;
    }
    if (hay_len == 0) {
        hay_len = vyx_rt_known_string_len(hay);
    }
    if (needle_len == 0) {
        needle_len = vyx_rt_known_string_len(needle);
        if (needle_len == 0) {
            return 1;
        }
    }
    hay_len = vyx_rt_clip_string_len(hay, hay_len);
    needle_len = vyx_rt_clip_string_len(needle, needle_len);
    if (needle_len > hay_len) {
        return 0;
    }
    const auto nlen = static_cast<size_t>(needle_len);
    const auto hlen = static_cast<size_t>(hay_len);
    const unsigned char first = static_cast<unsigned char>(needle[0]);
    const char* cur = hay;
    const char* end = hay + hlen - nlen + 1;
    while (cur < end) {
        const auto remaining = static_cast<size_t>(end - cur);
        const void* hit = std::memchr(cur, first, remaining);
        if (!hit) {
            return 0;
        }
        cur = static_cast<const char*>(hit);
        if (nlen == 1 || std::memcmp(cur + 1, needle + 1, nlen - 1) == 0) {
            return 1;
        }
        ++cur;
    }
    return 0;
}

extern "C" VYX_RT_ABI int64_t vyx_string_index_of(const char* hay,
                                                  uint64_t hay_len,
                                                  const char* needle,
                                                  uint64_t needle_len) {
    if (!hay || !needle) {
        return -1;
    }
    if (needle_len == 0) {
        return 0;
    }
    if (needle_len > hay_len) {
        return -1;
    }
    const auto nlen = static_cast<size_t>(needle_len);
    const auto hlen = static_cast<size_t>(hay_len);
    const unsigned char first = static_cast<unsigned char>(needle[0]);
    const char* cur = hay;
    const char* end = hay + hlen - nlen + 1;
    while (cur < end) {
        const auto remaining = static_cast<size_t>(end - cur);
        const void* hit = std::memchr(cur, first, remaining);
        if (!hit) {
            return -1;
        }
        cur = static_cast<const char*>(hit);
        if (nlen == 1 || std::memcmp(cur + 1, needle + 1, nlen - 1) == 0) {
            return static_cast<int64_t>(cur - hay);
        }
        ++cur;
    }
    return -1;
}

extern "C" VYX_RT_ABI int32_t vyx_string_equals(const char* a, const char* b) {
    VYX_BOOTSTRAP_RT_PROF(StringEquals);
    if (a == b) {
        return 1;
    }
    if (!a || !b) {
        return 0;
    }
    if (a[0] != b[0]) {
        return 0;
    }
    if (a[1] != b[1]) {
        return 0;
    }
    if (a[1] != '\0') {
        if (a[2] != b[2]) {
            return 0;
        }
        if (a[2] != '\0' && a[3] != b[3]) {
            return 0;
        }
    }
    const auto alen = vyx_rt_known_string_len(a);
    const auto blen = vyx_rt_known_string_len(b);
    if (alen != blen) {
        return 0;
    }
    if (alen == 0) {
        return 1;
    }
    return std::memcmp(a, b, static_cast<size_t>(alen)) == 0 ? 1 : 0;
}

extern "C" VYX_RT_ABI int32_t vyx_string_compare(const char* a, const char* b) {
    VYX_BOOTSTRAP_RT_PROF(StringCompare);
    if (a == b) {
        return 0;
    }
    const auto alen = vyx_rt_known_string_len(a);
    const auto blen = vyx_rt_known_string_len(b);
    const auto n = std::min(alen, blen);
    if (n > 0) {
        if (!a) {
            return -1;
        }
        if (!b) {
            return 1;
        }
        const int cmp = std::memcmp(a, b, static_cast<size_t>(n));
        if (cmp != 0) {
            return cmp;
        }
    }
    if (alen < blen) {
        return -1;
    }
    if (alen > blen) {
        return 1;
    }
    return 0;
}

extern "C" VYX_RT_ABI int32_t vyx_string_equals_len(const char* a,
                                                    const char* b,
                                                    int64_t b_len) {
    VYX_BOOTSTRAP_RT_PROF(StringEqualsLen);
    if (b_len < 0) {
        b_len = 0;
    }
    if (!a || !b) {
        return b_len == 0 && vyx_rt_known_string_len(a) == 0 ? 1 : 0;
    }
    if (b_len > 0 && a[0] != b[0]) {
        return 0;
    }
    if (b_len > 1 && a[1] != b[1]) {
        return 0;
    }
    if (b_len > 2 && a[2] != b[2]) {
        return 0;
    }
    if (b_len > 3 && a[3] != b[3]) {
        return 0;
    }
    const auto alen = vyx_rt_known_string_len(a);
    const auto blen = static_cast<uint64_t>(b_len);
    if (alen != blen) {
        return 0;
    }
    if (blen == 0) {
        return 1;
    }
    return std::memcmp(a, b, static_cast<size_t>(blen)) == 0 ? 1 : 0;
}

extern "C" VYX_RT_ABI int32_t vyx_string_char_at(const char* s,
                                                 int32_t idx) {
    VYX_BOOTSTRAP_RT_PROF(CharAt);
    if (!s || idx < 0) {
        return 0;
    }
    auto n = static_cast<int32_t>(vyx_rt_known_string_len(s));
    if (idx >= n) {
        return 0;
    }
    return static_cast<unsigned char>(s[idx]);
}

extern "C" VYX_RT_ABI char* vyx_string_substring(const char* s,
                                                 int32_t start,
                                                 int32_t end) {
    VYX_BOOTSTRAP_RT_PROF(Substring);
    if (!s) {
        return vyx_rt_empty_cstr();
    }
    int32_t n = static_cast<int32_t>(vyx_rt_known_string_len(s));
    if (start < 0) {
        start = 0;
    }
    if (end < start) {
        end = start;
    }
    if (start > n) {
        start = n;
    }
    if (end > n) {
        end = n;
    }
    if (end <= start) {
        return vyx_rt_empty_cstr();
    }
    return vyx_rt_dup_bytes(s + start, static_cast<uint64_t>(end - start));
}

extern "C" VYX_RT_ABI char* vyx_string_substring_len(const char* s,
                                                     int64_t len,
                                                     int32_t start,
                                                     int32_t end) {
    if (!s) {
        return vyx_rt_empty_cstr();
    }
    if (len < 0) {
        len = 0;
    }
    int32_t n = len > INT32_MAX ? INT32_MAX : static_cast<int32_t>(len);
    if (start < 0) {
        start = 0;
    }
    if (end < start) {
        end = start;
    }
    if (start > n) {
        start = n;
    }
    if (end > n) {
        end = n;
    }
    if (end <= start) {
        return vyx_rt_empty_cstr();
    }
    return vyx_rt_dup_bytes(s + start, static_cast<uint64_t>(end - start));
}

extern "C" VYX_RT_ABI char* vyx_string_trim(const char* s) {
    VYX_BOOTSTRAP_RT_PROF(Trim);
    if (!s) {
        return vyx_rt_dup_malloc_bytes("", 0);
    }
    const unsigned char* first = reinterpret_cast<const unsigned char*>(s);
    const unsigned char* last = first + vyx_rt_known_string_len(s);
    while (first < last && std::isspace(*first)) {
        ++first;
    }
    while (last > first && std::isspace(*(last - 1))) {
        --last;
    }
    return vyx_rt_dup_malloc_bytes(reinterpret_cast<const char*>(first),
                                   static_cast<uint64_t>(last - first));
}

extern "C" VYX_RT_ABI char* vyx_string_clone(const char* s) {
    VYX_BOOTSTRAP_RT_PROF(Clone);
    const auto len = vyx_rt_known_string_len(s);
    if (len == 0) {
        return vyx_rt_empty_cstr();
    }
    return vyx_rt_dup_bytes(s, len);
}

extern "C" VYX_RT_ABI char* vyx_string_clone_len_abi(const char* s, int64_t len) {
    VYX_BOOTSTRAP_RT_PROF(Clone);
    if (len < 0) {
        len = 0;
    }
    // This is deliberately separate from vyx_string_clone. Old bootstrap
    // binaries consume that ABI as arena-or-shared storage; generated code
    // needs one independently releasable allocation in the runtime's own
    // allocator domain on every call, including an empty clone.
    return vyx_rt_dup_malloc_bytes(s, static_cast<uint64_t>(len));
}

extern "C" VYX_RT_ABI char* vyx_string_to_upper(const char* s) {
    VYX_BOOTSTRAP_RT_PROF(ToUpper);
    char* out = vyx_rt_dup_malloc_bytes(s, vyx_rt_known_string_len(s));
    if (!out) {
        return nullptr;
    }
    for (char* p = out; *p; ++p) {
        *p = static_cast<char>(std::toupper(static_cast<unsigned char>(*p)));
    }
    return out;
}

extern "C" VYX_RT_ABI char* vyx_string_to_lower(const char* s) {
    VYX_BOOTSTRAP_RT_PROF(ToLower);
    char* out = vyx_rt_dup_malloc_bytes(s, vyx_rt_known_string_len(s));
    if (!out) {
        return nullptr;
    }
    for (char* p = out; *p; ++p) {
        *p = static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
    }
    return out;
}

extern "C" VYX_RT_ABI char* vyx_string_to_upper_len_abi(const char* s, int64_t len) {
    if (!s || len <= 0) {
        char* out = vyx_string_alloc_abi(1);
        if (out) {
            out[0] = '\0';
            vyx_rt_note_string_len_cached(out, 0);
        }
        return out;
    }
    auto n = static_cast<uint64_t>(len);
    char* out = vyx_string_alloc_abi(static_cast<int64_t>(n + 1));
    if (!out) {
        return nullptr;
    }
    std::memcpy(out, s, static_cast<std::size_t>(n));
    out[n] = '\0';
    for (uint64_t i = 0; i < n; ++i) {
        out[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[i])));
    }
    vyx_rt_note_string_len_cached(out, n);
    return out;
}

extern "C" VYX_RT_ABI char* vyx_string_to_lower_len_abi(const char* s, int64_t len) {
    if (!s || len <= 0) {
        char* out = vyx_string_alloc_abi(1);
        if (out) {
            out[0] = '\0';
            vyx_rt_note_string_len_cached(out, 0);
        }
        return out;
    }
    auto n = static_cast<uint64_t>(len);
    char* out = vyx_string_alloc_abi(static_cast<int64_t>(n + 1));
    if (!out) {
        return nullptr;
    }
    std::memcpy(out, s, static_cast<std::size_t>(n));
    out[n] = '\0';
    for (uint64_t i = 0; i < n; ++i) {
        out[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(out[i])));
    }
    vyx_rt_note_string_len_cached(out, n);
    return out;
}

extern "C" VYX_RT_ABI char* vyx_string_replace(const char* s,
                                               const char* needle,
                                               const char* repl) {
    VYX_BOOTSTRAP_RT_PROF(Replace);
    if (!s) {
        return vyx_rt_dup_malloc_bytes("", 0);
    }
    if (!needle || needle[0] == '\0') {
        return vyx_rt_dup_malloc_bytes(s, vyx_rt_known_string_len(s));
    }
    if (!repl) {
        repl = "";
    }

    const auto slen = vyx_rt_known_string_len(s);
    const auto nlen = vyx_rt_known_string_len(needle);
    const auto rlen = vyx_rt_known_string_len(repl);
    if (nlen == 0 || nlen > slen) {
        return vyx_rt_dup_malloc_bytes(s, slen);
    }

    const auto find_hit = [&](const char* from) -> const char* {
        if (from < s) {
            return nullptr;
        }
        const uint64_t used = static_cast<uint64_t>(from - s);
        if (used + nlen > slen) {
            return nullptr;
        }
        const unsigned char first = static_cast<unsigned char>(needle[0]);
        const char* cur = from;
        const char* end = s + (slen - nlen + 1);
        const auto nlen_sz = static_cast<size_t>(nlen);
        while (cur < end) {
            const void* hit = std::memchr(cur, first, static_cast<size_t>(end - cur));
            if (!hit) {
                return nullptr;
            }
            cur = static_cast<const char*>(hit);
            if (nlen_sz == 1 || std::memcmp(cur + 1, needle + 1, nlen_sz - 1) == 0) {
                return cur;
            }
            ++cur;
        }
        return nullptr;
    };

    uint64_t count = 0;
    const char* scan = s;
    while (const char* hit = find_hit(scan)) {
        ++count;
        scan = hit + nlen;
    }
    if (count == 0) {
        return vyx_rt_dup_malloc_bytes(s, slen);
    }

    const uint64_t out_len = slen + count * rlen - count * nlen;
    char* out = static_cast<char*>(std::malloc(static_cast<std::size_t>(out_len + 1)));
    if (!out) {
        return nullptr;
    }

    char* dst = out;
    const char* cur = s;
    while (const char* hit = find_hit(cur)) {
        const auto prefix_len = static_cast<uint64_t>(hit - cur);
        if (prefix_len > 0) {
            std::memcpy(dst, cur, static_cast<size_t>(prefix_len));
            dst += prefix_len;
        }
        if (rlen > 0) {
            std::memcpy(dst, repl, static_cast<size_t>(rlen));
            dst += rlen;
        }
        cur = hit + nlen;
    }
    const auto tail_len = slen - static_cast<uint64_t>(cur - s);
    if (tail_len > 0) {
        std::memcpy(dst, cur, static_cast<size_t>(tail_len));
        dst += tail_len;
    }
    *dst = '\0';
    vyx_rt_note_string_len_cached(out, out_len);
    return out;
}

extern "C" VYX_RT_ABI char* vyx_string_concat(const char* a,
                                                const char* b) {
    VYX_BOOTSTRAP_RT_PROF(Concat);
    uint64_t alen = vyx_rt_known_string_len(a);
    uint64_t blen = vyx_rt_known_string_len(b);
    if (alen == 0 && blen == 0) {
        return vyx_rt_empty_cstr();
    }
    auto* out = vyx_rt_alloc_string_storage(alen + blen);
    if (!out) {
        return nullptr;
    }
    if (a && alen > 0) {
        std::memcpy(out, a, static_cast<size_t>(alen));
    }
    if (b && blen > 0) {
        std::memcpy(out + alen, b, static_cast<size_t>(blen));
    }
    out[alen + blen] = '\0';
    vyx_rt_note_string_len_cached(out, alen + blen);
    return out;
}

extern "C" VYX_RT_ABI char* vyx_string_append_assign_abi(const char* base,
                                                          int64_t base_len,
                                                          int64_t base_cap,
                                                          int64_t base_owned,
                                                          const char* suffix,
                                                          int64_t suffix_len,
                                                          int64_t* out_len,
                                                          int64_t* out_cap,
                                                          int64_t* out_owned) {
    VYX_BOOTSTRAP_RT_PROF(Concat);
    if (base_len < 0) {
        base_len = base ? static_cast<int64_t>(vyx_rt_known_string_len(base)) : 0;
    }
    if (suffix_len < 0) {
        suffix_len = suffix ? static_cast<int64_t>(vyx_rt_known_string_len(suffix)) : 0;
    }
    if (!suffix || suffix_len <= 0) {
        if (out_len) { *out_len = base_len; }
        if (out_cap) { *out_cap = base_cap; }
        if (out_owned) { *out_owned = base_owned; }
        return const_cast<char*>(base ? base : vyx_rt_empty_cstr());
    }

    const auto old_len = static_cast<uint64_t>(base_len > 0 ? base_len : 0);
    const auto add_len = static_cast<uint64_t>(suffix_len);
    const auto total = old_len + add_len;
    const auto needed_cap = total + 1u;
    uint64_t cap = base_cap > 0 ? static_cast<uint64_t>(base_cap) : old_len + 1u;
    while (cap < needed_cap) {
        cap = cap < 64u ? 64u : cap * 2u + 1u;
    }

    const bool suffix_aliases_base = base && suffix >= base && suffix < base + old_len;
    char* suffix_copy = nullptr;
    const char* src = suffix;
    if (suffix_aliases_base) {
        suffix_copy = static_cast<char*>(std::malloc(static_cast<std::size_t>(add_len + 1u)));
        if (!suffix_copy) { return nullptr; }
        std::memcpy(suffix_copy, suffix, static_cast<std::size_t>(add_len));
        suffix_copy[add_len] = '\0';
        src = suffix_copy;
    }

    char* data = nullptr;
    int64_t owned = 3;
    bool had_shared_len_record = false;
    if (base && base_owned == 1 && base_cap >= static_cast<int64_t>(needed_cap)) {
        data = const_cast<char*>(base);
        owned = 1;
    } else if (base && base_owned == 3 && base_cap >= static_cast<int64_t>(needed_cap)) {
        data = const_cast<char*>(base);
    } else if (base && base_owned == 3) {
        had_shared_len_record = vyx_rt_forget_string_len(base) != 0;
        auto* raw = static_cast<char*>(std::realloc(const_cast<char*>(base), static_cast<std::size_t>(cap)));
        if (!raw) {
            if (had_shared_len_record) { vyx_rt_note_string_len(base, old_len); }
            else { vyx_rt_note_string_len_cached(base, old_len); }
            if (suffix_copy) { std::free(suffix_copy); }
            return nullptr;
        }
        data = raw;
    } else {
        data = static_cast<char*>(std::malloc(static_cast<std::size_t>(cap)));
        if (!data) {
            if (suffix_copy) { std::free(suffix_copy); }
            return nullptr;
        }
        if (base && old_len > 0) { std::memcpy(data, base, static_cast<std::size_t>(old_len)); }
    }

    std::memcpy(data + old_len, src, static_cast<std::size_t>(add_len));
    data[total] = '\0';
    if (suffix_copy) { std::free(suffix_copy); }
    if (had_shared_len_record) { vyx_rt_note_string_len(data, total); }
    else {
        (void)vyx_rt_update_shared_string_len(data, total);
        vyx_rt_note_string_len_cached(data, total);
    }
    // This ABI borrows `base`. Legacy and self-hosted callers retain the
    // original str value and perform its drop according to the language
    // ownership path. Freeing a replaced malloc buffer here therefore races
    // that caller-side drop and corrupts the compiler on the next generation.
    // The in-module MIR append lowering has its own explicit release path;
    // keep this compatibility helper non-consuming as well.
    if (out_len) { *out_len = static_cast<int64_t>(total); }
    if (out_cap) { *out_cap = static_cast<int64_t>(cap); }
    if (out_owned) { *out_owned = owned; }
    return data;
}

struct VyxRtStringAbi {
    const char* ptr;
    int64_t len;
    int64_t cap;
    int64_t owned;
};

struct VyxRtDictAbi {
    void* data;
    int64_t len;
    int64_t cap;
    int64_t gen;
    bool destroyed;
};

static uint64_t vyx_rt_hash_i64_raw(int64_t val) {
    uint64_t h = static_cast<uint64_t>(val);
    h ^= h >> 30;
    h *= UINT64_C(13787848793156543929);
    h ^= h >> 27;
    h *= UINT64_C(10723151780598845931);
    h ^= h >> 31;
    return h;
}

static uint64_t vyx_rt_rotl64(uint64_t x, int r) {
    return (x << r) | (x >> (64 - r));
}

static uint64_t vyx_rt_hash_bytes_short_raw(const char* data, int64_t len) {
    uint64_t lo = 0;
    uint64_t hi = 0;
    if (len > 0) {
        const int64_t first = len > 8 ? 8 : len;
        std::memcpy(&lo, data, static_cast<std::size_t>(first));
        if (len > 8) {
            std::memcpy(&hi, data + 8, static_cast<std::size_t>(len - 8));
        }
    }
    uint64_t h = UINT64_C(11400714819323198485)
        ^ (static_cast<uint64_t>(len) * UINT64_C(14029467366897019727));
    h ^= lo * UINT64_C(13787848793156543929);
    h ^= vyx_rt_rotl64(hi * UINT64_C(10723151780598845931), 31);
    h ^= h >> 33;
    h *= UINT64_C(0xff51afd7ed558ccd);
    h ^= h >> 33;
    h *= UINT64_C(0xc4ceb9fe1a85ec53);
    h ^= h >> 33;
    return h;
}

static uint64_t vyx_rt_hash_bytes_key_raw(const char* data, int64_t len) {
    if (!data || len <= 16) {
        return vyx_rt_hash_bytes_short_raw(data ? data : "", len < 0 ? 0 : len);
    }
    uint64_t h = static_cast<uint64_t>(len) ^ UINT64_C(11400714819323198485);
    h ^= h >> 30;
    h *= UINT64_C(13787848793156543929);
    h ^= h >> 27;
    h *= UINT64_C(10723151780598845931);
    h ^= h >> 31;
    int64_t i = 0;
    while (i + 8 <= len) {
        uint64_t chunk = 0;
        std::memcpy(&chunk, data + i, 8);
        h ^= chunk;
        h ^= h >> 30;
        h *= UINT64_C(13787848793156543929);
        h ^= h >> 27;
        h *= UINT64_C(10723151780598845931);
        h ^= h >> 31;
        i += 8;
    }
    uint64_t tail = 0;
    uint32_t shift = 0;
    while (i < len) {
        tail |= static_cast<uint64_t>(static_cast<unsigned char>(data[i])) << shift;
        shift += 8;
        ++i;
    }
    if (shift > 0) {
        h ^= tail;
        h ^= h >> 30;
        h *= UINT64_C(13787848793156543929);
        h ^= h >> 27;
        h *= UINT64_C(10723151780598845931);
        h ^= h >> 31;
    }
    return h;
}

static int64_t vyx_rt_dict_ctrl_bytes(int64_t cap) {
    return (cap + 7) & ~int64_t{7};
}

static void vyx_rt_dict_i64_i64_alloc(VyxRtDictAbi* d, int64_t cap) {
    const int64_t ctrl_bytes = vyx_rt_dict_ctrl_bytes(cap);
    const int64_t total = ctrl_bytes + cap * 8 + cap * 8;
    d->data = std::malloc(static_cast<std::size_t>(total));
    if (d->data) {
        std::memset(d->data, 0, static_cast<std::size_t>(ctrl_bytes));
    }
    d->len = 0;
    d->cap = cap;
    d->destroyed = false;
}

static void vyx_rt_dict_str_i64_alloc(VyxRtDictAbi* d, int64_t cap) {
    const int64_t ctrl_bytes = vyx_rt_dict_ctrl_bytes(cap);
    const int64_t total = ctrl_bytes + cap * static_cast<int64_t>(sizeof(VyxRtStringAbi)) + cap * 8;
    d->data = std::malloc(static_cast<std::size_t>(total));
    if (d->data) {
        std::memset(d->data, 0, static_cast<std::size_t>(ctrl_bytes));
    }
    d->len = 0;
    d->cap = cap;
    d->destroyed = false;
}

static void vyx_rt_dict_i64_i64_insert_rehash(VyxRtDictAbi* d, int64_t key, int64_t val) {
    auto* ctrl = static_cast<unsigned char*>(d->data);
    const int64_t ctrl_bytes = vyx_rt_dict_ctrl_bytes(d->cap);
    auto* keys = reinterpret_cast<int64_t*>(ctrl + ctrl_bytes);
    auto* vals = reinterpret_cast<int64_t*>(ctrl + ctrl_bytes + d->cap * 8);
    int64_t idx = static_cast<int64_t>(vyx_rt_hash_i64_raw(key) & static_cast<uint64_t>(d->cap - 1));
    while (ctrl[idx] == 1) {
        idx = (idx + 1) & (d->cap - 1);
    }
    ctrl[idx] = 1;
    keys[idx] = key;
    vals[idx] = val;
    ++d->len;
}

static void vyx_rt_dict_i64_i64_grow(VyxRtDictAbi* d) {
    const int64_t old_cap = d->cap;
    void* old_data = d->data;
    const int64_t old_ctrl_bytes = vyx_rt_dict_ctrl_bytes(old_cap);
    auto* old_ctrl = static_cast<unsigned char*>(old_data);
    auto* old_keys = reinterpret_cast<int64_t*>(old_ctrl + old_ctrl_bytes);
    auto* old_vals = reinterpret_cast<int64_t*>(old_ctrl + old_ctrl_bytes + old_cap * 8);
    vyx_rt_dict_i64_i64_alloc(d, old_cap > 0 ? old_cap * 2 : 64);
    for (int64_t i = 0; i < old_cap; ++i) {
        if (old_ctrl[i] == 1) {
            vyx_rt_dict_i64_i64_insert_rehash(d, old_keys[i], old_vals[i]);
        }
    }
    std::free(old_data);
    ++d->gen;
}

extern "C" VYX_RT_ABI void vyx_dict_i64_i64_put(void* dict, int64_t key, int64_t val) {
    auto* d = static_cast<VyxRtDictAbi*>(dict);
    if (!d) { return; }
    if (!d->data || d->cap <= 0) {
        vyx_rt_dict_i64_i64_alloc(d, 64);
    }
    if (d->len * 10 >= d->cap * 7) {
        vyx_rt_dict_i64_i64_grow(d);
    }
    auto* ctrl = static_cast<unsigned char*>(d->data);
    const int64_t ctrl_bytes = vyx_rt_dict_ctrl_bytes(d->cap);
    auto* keys = reinterpret_cast<int64_t*>(ctrl + ctrl_bytes);
    auto* vals = reinterpret_cast<int64_t*>(ctrl + ctrl_bytes + d->cap * 8);
    int64_t idx = static_cast<int64_t>(vyx_rt_hash_i64_raw(key) & static_cast<uint64_t>(d->cap - 1));
    int64_t tombstone = -1;
    for (int64_t probes = 0; probes < d->cap; ++probes) {
        const unsigned char c = ctrl[idx];
        if (c == 0) {
            if (tombstone >= 0) { idx = tombstone; }
            ctrl[idx] = 1;
            keys[idx] = key;
            vals[idx] = val;
            ++d->len;
            ++d->gen;
            return;
        }
        if (c == 2) {
            if (tombstone < 0) { tombstone = idx; }
        } else if (keys[idx] == key) {
            vals[idx] = val;
            return;
        }
        idx = (idx + 1) & (d->cap - 1);
    }
    if (tombstone >= 0) {
        ctrl[tombstone] = 1;
        keys[tombstone] = key;
        vals[tombstone] = val;
        ++d->len;
        ++d->gen;
    }
}

extern "C" VYX_RT_ABI int32_t vyx_dict_i64_i64_try_get(void* dict, int64_t key, int64_t* out) {
    auto* d = static_cast<VyxRtDictAbi*>(dict);
    if (!d || !d->data || d->cap <= 0) { return 0; }
    auto* ctrl = static_cast<unsigned char*>(d->data);
    const int64_t ctrl_bytes = vyx_rt_dict_ctrl_bytes(d->cap);
    auto* keys = reinterpret_cast<int64_t*>(ctrl + ctrl_bytes);
    auto* vals = reinterpret_cast<int64_t*>(ctrl + ctrl_bytes + d->cap * 8);
    int64_t idx = static_cast<int64_t>(vyx_rt_hash_i64_raw(key) & static_cast<uint64_t>(d->cap - 1));
    for (int64_t probes = 0; probes < d->cap; ++probes) {
        const unsigned char c = ctrl[idx];
        if (c == 0) { return 0; }
        if (c == 1 && keys[idx] == key) {
            if (out) { *out = vals[idx]; }
            return 1;
        }
        idx = (idx + 1) & (d->cap - 1);
    }
    return 0;
}

[[maybe_unused]] static bool vyx_rt_string_key_eq(const VyxRtStringAbi& a, const char* key, int64_t len) {
    if (a.len != len) { return false; }
    if (len <= 0) { return true; }
    if (a.ptr == key) { return true; }
    if (!a.ptr || !key) { return false; }
    return std::memcmp(a.ptr, key, static_cast<std::size_t>(len)) == 0;
}

static constexpr int64_t VYX_RT_DICT_STR_INLINE16 = INT64_C(-0x565958494e4c3136);

static bool vyx_rt_dict_str_is_inline(const VyxRtStringAbi& s) {
    return s.owned == VYX_RT_DICT_STR_INLINE16;
}

static VyxRtStringAbi vyx_rt_dict_str_key_from_bytes(const char* key, int64_t len,
                                                     int64_t cap, int64_t owned) {
    VyxRtStringAbi out{key, len, cap, owned};
    if (key && len > 0 && len <= 16) {
        out.ptr = nullptr;
        out.cap = 0;
        out.owned = VYX_RT_DICT_STR_INLINE16;
        std::memcpy(&out.ptr, key, static_cast<std::size_t>(len > 8 ? 8 : len));
        if (len > 8) {
            std::memcpy(&out.cap, key + 8, static_cast<std::size_t>(len - 8));
        }
    }
    return out;
}

static void vyx_rt_dict_str_inline_bytes(const VyxRtStringAbi& s, char* out) {
    if (s.len <= 0) { return; }
    std::memcpy(out, &s.ptr, static_cast<std::size_t>(s.len > 8 ? 8 : s.len));
    if (s.len > 8) {
        std::memcpy(out + 8, &s.cap, static_cast<std::size_t>(s.len - 8));
    }
}

static uint64_t vyx_rt_dict_str_hash_key(const VyxRtStringAbi& s) {
    if (!vyx_rt_dict_str_is_inline(s)) {
        return vyx_rt_hash_bytes_key_raw(s.ptr, s.len);
    }
    char tmp[16];
    vyx_rt_dict_str_inline_bytes(s, tmp);
    return vyx_rt_hash_bytes_key_raw(tmp, s.len);
}

static bool vyx_rt_dict_str_eq_key(const VyxRtStringAbi& a, const char* key, int64_t len) {
    if (a.len != len) { return false; }
    if (len <= 0) { return true; }
    if (!vyx_rt_dict_str_is_inline(a)) {
        if (a.ptr == key) { return true; }
        if (!a.ptr || !key) { return false; }
        return std::memcmp(a.ptr, key, static_cast<std::size_t>(len)) == 0;
    }
    if (!key) { return false; }
    if (len <= 8) {
        return std::memcmp(&a.ptr, key, static_cast<std::size_t>(len)) == 0;
    }
    return std::memcmp(&a.ptr, key, 8) == 0
        && std::memcmp(&a.cap, key + 8, static_cast<std::size_t>(len - 8)) == 0;
}

static void vyx_rt_dict_str_i64_insert_rehash(VyxRtDictAbi* d, VyxRtStringAbi key, int64_t val) {
    auto* ctrl = static_cast<unsigned char*>(d->data);
    const int64_t ctrl_bytes = vyx_rt_dict_ctrl_bytes(d->cap);
    auto* keys = reinterpret_cast<VyxRtStringAbi*>(ctrl + ctrl_bytes);
    auto* vals = reinterpret_cast<int64_t*>(ctrl + ctrl_bytes + d->cap * static_cast<int64_t>(sizeof(VyxRtStringAbi)));
    int64_t idx = static_cast<int64_t>(vyx_rt_dict_str_hash_key(key) & static_cast<uint64_t>(d->cap - 1));
    while (ctrl[idx] == 1) {
        idx = (idx + 1) & (d->cap - 1);
    }
    ctrl[idx] = 1;
    keys[idx] = key;
    vals[idx] = val;
    ++d->len;
}

static void vyx_rt_dict_str_i64_grow(VyxRtDictAbi* d) {
    const int64_t old_cap = d->cap;
    void* old_data = d->data;
    const int64_t old_ctrl_bytes = vyx_rt_dict_ctrl_bytes(old_cap);
    auto* old_ctrl = static_cast<unsigned char*>(old_data);
    auto* old_keys = reinterpret_cast<VyxRtStringAbi*>(old_ctrl + old_ctrl_bytes);
    auto* old_vals = reinterpret_cast<int64_t*>(old_ctrl + old_ctrl_bytes + old_cap * static_cast<int64_t>(sizeof(VyxRtStringAbi)));
    vyx_rt_dict_str_i64_alloc(d, old_cap > 0 ? old_cap * 2 : 64);
    for (int64_t i = 0; i < old_cap; ++i) {
        if (old_ctrl[i] == 1) {
            vyx_rt_dict_str_i64_insert_rehash(d, old_keys[i], old_vals[i]);
        }
    }
    std::free(old_data);
    ++d->gen;
}

extern "C" VYX_RT_ABI void vyx_dict_str_i64_put(void* dict, const char* key, int64_t len,
                                                 int64_t cap, int64_t owned, int64_t val) {
    auto* d = static_cast<VyxRtDictAbi*>(dict);
    if (!d) { return; }
    if (!d->data || d->cap <= 0) {
        vyx_rt_dict_str_i64_alloc(d, 64);
    }
    if (d->len * 10 >= d->cap * 7) {
        vyx_rt_dict_str_i64_grow(d);
    }
    auto* ctrl = static_cast<unsigned char*>(d->data);
    const int64_t ctrl_bytes = vyx_rt_dict_ctrl_bytes(d->cap);
    auto* keys = reinterpret_cast<VyxRtStringAbi*>(ctrl + ctrl_bytes);
    auto* vals = reinterpret_cast<int64_t*>(ctrl + ctrl_bytes + d->cap * static_cast<int64_t>(sizeof(VyxRtStringAbi)));
    int64_t idx = static_cast<int64_t>(vyx_rt_hash_bytes_key_raw(key, len) & static_cast<uint64_t>(d->cap - 1));
    int64_t tombstone = -1;
    for (int64_t probes = 0; probes < d->cap; ++probes) {
        const unsigned char c = ctrl[idx];
        if (c == 0) {
            if (tombstone >= 0) { idx = tombstone; }
            ctrl[idx] = 1;
            keys[idx] = vyx_rt_dict_str_key_from_bytes(key, len, cap, owned);
            vals[idx] = val;
            ++d->len;
            ++d->gen;
            return;
        }
        if (c == 2) {
            if (tombstone < 0) { tombstone = idx; }
        } else if (vyx_rt_dict_str_eq_key(keys[idx], key, len)) {
            vals[idx] = val;
            return;
        }
        idx = (idx + 1) & (d->cap - 1);
    }
    if (tombstone >= 0) {
        ctrl[tombstone] = 1;
        keys[tombstone] = vyx_rt_dict_str_key_from_bytes(key, len, cap, owned);
        vals[tombstone] = val;
        ++d->len;
        ++d->gen;
    }
}

extern "C" VYX_RT_ABI void vyx_dict_str_i64_put_fast(void* dict, const char* key, int64_t len,
                                                      int64_t cap, int64_t owned, int64_t val) {
    vyx_dict_str_i64_put(dict, key, len, cap, owned, val);
}

extern "C" VYX_RT_ABI int32_t vyx_dict_str_i64_try_get(void* dict, const char* key, int64_t len,
                                                        int64_t* out) {
    auto* d = static_cast<VyxRtDictAbi*>(dict);
    if (!d || !d->data || d->cap <= 0) { return 0; }
    auto* ctrl = static_cast<unsigned char*>(d->data);
    const int64_t ctrl_bytes = vyx_rt_dict_ctrl_bytes(d->cap);
    auto* keys = reinterpret_cast<VyxRtStringAbi*>(ctrl + ctrl_bytes);
    auto* vals = reinterpret_cast<int64_t*>(ctrl + ctrl_bytes + d->cap * static_cast<int64_t>(sizeof(VyxRtStringAbi)));
    int64_t idx = static_cast<int64_t>(vyx_rt_hash_bytes_key_raw(key, len) & static_cast<uint64_t>(d->cap - 1));
    for (int64_t probes = 0; probes < d->cap; ++probes) {
        const unsigned char c = ctrl[idx];
        if (c == 0) { return 0; }
        if (c == 1 && vyx_rt_dict_str_eq_key(keys[idx], key, len)) {
            if (out) { *out = vals[idx]; }
            return 1;
        }
        idx = (idx + 1) & (d->cap - 1);
    }
    return 0;
}

extern "C" VYX_RT_ABI int32_t vyx_dict_str_i64_try_get_fast(void* dict, const char* key, int64_t len,
                                                             int64_t* out) {
    return vyx_dict_str_i64_try_get(dict, key, len, out);
}

static char* vyx_string_concat_many(const char* const* parts, const uint64_t* lens, std::size_t count) {
    uint64_t total = 0;
    for (std::size_t i = 0; i < count; ++i) {
        total += lens[i];
    }
    if (total == 0) {
        return vyx_rt_empty_cstr();
    }
    char* out = vyx_rt_alloc_string_storage(total);
    if (!out) {
        return nullptr;
    }
    char* dst = out;
    for (std::size_t i = 0; i < count; ++i) {
        if (parts[i] && lens[i] > 0) {
            std::memcpy(dst, parts[i], static_cast<std::size_t>(lens[i]));
            dst += lens[i];
        }
    }
    *dst = '\0';
    vyx_rt_note_string_len_cached(out, total);
    return out;
}

extern "C" VYX_RT_ABI char* vyx_string_concat3(const char* a,
                                                const char* b,
                                                const char* c) {
    const char* parts[3] = {a, b, c};
    uint64_t lens[3] = {
        vyx_rt_known_string_len(a),
        vyx_rt_known_string_len(b),
        vyx_rt_known_string_len(c),
    };
    return vyx_string_concat_many(parts, lens, 3);
}

extern "C" VYX_RT_ABI char* vyx_string_concat4(const char* a,
                                                const char* b,
                                                const char* c,
                                                const char* d) {
    const char* parts[4] = {a, b, c, d};
    uint64_t lens[4] = {
        vyx_rt_known_string_len(a),
        vyx_rt_known_string_len(b),
        vyx_rt_known_string_len(c),
        vyx_rt_known_string_len(d),
    };
    return vyx_string_concat_many(parts, lens, 4);
}

// f-string Lite helper: pairwise concatenation used by ex_FSTRING lowering.
// Kept as a distinct symbol from `vyx_string_concat` so the bootstrap IR
// shows `call @vyx_concat` (matches the BOOTSTRAP_GAP_PLAN P1.W1.F evidence
// requirement) without disturbing the existing `+`-operator concat path.
extern "C" VYX_RT_ABI char* vyx_concat(const char* a, const char* b) {
    return vyx_string_concat(a, b);
}

extern "C" VYX_RT_ABI int32_t str_equals(const char* a, const char* b) {
    VYX_BOOTSTRAP_RT_PROF(StrEquals);
    if (!a || !b) {
        return a == b ? 1 : 0;
    }
    return std::strcmp(a, b) == 0 ? 1 : 0;
}

// When statically linked into the bootstrap compiler, these symbols are
// already provided by the Vyx-emitted IR. Guard them so we don't get
// duplicate-symbol link errors.
#ifndef VYX_STATIC_LINK_BOOTSTRAP

extern "C" VYX_RT_ABI int64_t str_length(const char* s) {
    VYX_BOOTSTRAP_RT_PROF(StrLength);
    return static_cast<int64_t>(vyx_rt_known_string_len(s));
}

#endif // VYX_STATIC_LINK_BOOTSTRAP

static int64_t vyx_rt_i64_to_chars(char* out, int64_t v) {
    if (!out) {
        return 0;
    }
    char tmp[32];
    uint64_t u = 0;
    const bool neg = v < 0;
    if (neg) {
        u = uint64_t{0} - static_cast<uint64_t>(v);
    } else {
        u = static_cast<uint64_t>(v);
    }

    int pos = static_cast<int>(sizeof(tmp));
    do {
        const uint64_t q = u / 10;
        const uint64_t r = u - q * 10;
        tmp[--pos] = static_cast<char>('0' + r);
        u = q;
    } while (u != 0);
    if (neg) {
        tmp[--pos] = '-';
    }

    const int64_t len = static_cast<int64_t>(sizeof(tmp) - static_cast<std::size_t>(pos));
    std::memcpy(out, tmp + pos, static_cast<std::size_t>(len));
    out[len] = '\0';
    return len;
}

static int64_t vyx_rt_i64_decimal_len(int64_t v) {
    uint64_t u = 0;
    int64_t len = 1;
    if (v < 0) {
        u = uint64_t{0} - static_cast<uint64_t>(v);
        ++len;
    } else {
        u = static_cast<uint64_t>(v);
    }
    while (u >= 10) {
        u /= 10;
        ++len;
    }
    return len;
}

static int64_t vyx_rt_u64_to_chars(char* out, uint64_t v) {
    if (!out) {
        return 0;
    }
    char tmp[32];
    int pos = static_cast<int>(sizeof(tmp));
    do {
        const uint64_t q = v / 10;
        const uint64_t r = v - q * 10;
        tmp[--pos] = static_cast<char>('0' + r);
        v = q;
    } while (v != 0);

    const int64_t len = static_cast<int64_t>(sizeof(tmp) - static_cast<std::size_t>(pos));
    std::memcpy(out, tmp + pos, static_cast<std::size_t>(len));
    out[len] = '\0';
    return len;
}

static int64_t vyx_rt_u64_decimal_len(uint64_t v) {
    int64_t len = 1;
    while (v >= 10) {
        v /= 10;
        ++len;
    }
    return len;
}

extern "C" VYX_RT_ABI char* int_to_string(int64_t v) {
    VYX_BOOTSTRAP_RT_PROF(IntToString);
    char tmp[32];
    const int64_t n = vyx_rt_i64_to_chars(tmp, v);
    return vyx_rt_dup_malloc_bytes(tmp, static_cast<uint64_t>(n));
}

extern "C" VYX_RT_ABI char* uint_to_string(uint64_t v) {
    VYX_BOOTSTRAP_RT_PROF(IntToString);
    char tmp[32];
    const int64_t n = vyx_rt_u64_to_chars(tmp, v);
    return vyx_rt_dup_malloc_bytes(tmp, static_cast<uint64_t>(n));
}

extern "C" VYX_RT_ABI char* int_to_string_len_abi(int64_t v, int64_t* out_len) {
    VYX_BOOTSTRAP_RT_PROF(IntToString);
    char tmp[32];
    const int64_t n = vyx_rt_i64_to_chars(tmp, v);
    if (out_len) {
        *out_len = n;
    }
    return vyx_rt_dup_malloc_bytes(tmp, static_cast<uint64_t>(n));
}

extern "C" VYX_RT_ABI int64_t vyx_int_decimal_len_abi(int64_t v) {
    return vyx_rt_i64_decimal_len(v);
}

extern "C" VYX_RT_ABI int64_t vyx_int_to_string_into_abi(int64_t v, char* out) {
    if (!out) {
        return 0;
    }
    return vyx_rt_i64_to_chars(out, v);
}

extern "C" VYX_RT_ABI char* uint_to_string_len_abi(uint64_t v, int64_t* out_len) {
    VYX_BOOTSTRAP_RT_PROF(IntToString);
    char tmp[32];
    const int64_t n = vyx_rt_u64_to_chars(tmp, v);
    if (out_len) {
        *out_len = n;
    }
    return vyx_rt_dup_malloc_bytes(tmp, static_cast<uint64_t>(n));
}

extern "C" VYX_RT_ABI int64_t vyx_uint_decimal_len_abi(uint64_t v) {
    return vyx_rt_u64_decimal_len(v);
}

extern "C" VYX_RT_ABI int64_t vyx_uint_to_string_into_abi(uint64_t v, char* out) {
    if (!out) {
        return 0;
    }
    return vyx_rt_u64_to_chars(out, v);
}

struct VyxRtStringBuilderAbi {
    char* buf;
    int64_t len;
    int64_t cap;
    int64_t inline_words[8];
};

static void* vyx_rt_string_builder_object_alloc() {
    static thread_local char* cur = nullptr;
    static thread_local char* end = nullptr;
    constexpr std::size_t chunk_size = 1u << 20;
    constexpr std::size_t obj_size = sizeof(VyxRtStringBuilderAbi);
    if (!cur || static_cast<std::size_t>(end - cur) < obj_size) {
        cur = static_cast<char*>(std::malloc(chunk_size));
        if (!cur) { return nullptr; }
        end = cur + chunk_size;
    }
    void* out = cur;
    cur += obj_size;
    return out;
}

static void* vyx_rt_small_object_alloc(std::size_t size) {
    if (size == 0) { return nullptr; }
    size = (size + 15u) & ~std::size_t{15u};
    if (size > 512u) {
        return std::malloc(size);
    }
    static thread_local char* cur = nullptr;
    static thread_local char* end = nullptr;
    constexpr std::size_t chunk_size = 1u << 20;
    if (!cur || static_cast<std::size_t>(end - cur) < size) {
        cur = static_cast<char*>(std::malloc(chunk_size));
        if (!cur) { return nullptr; }
        end = cur + chunk_size;
    }
    void* out = cur;
    cur += size;
    return out;
}

extern "C" VYX_RT_ABI void* vyx_class_alloc_abi(int64_t size) {
    if (size <= 0) { return nullptr; }
    return vyx_rt_small_object_alloc(static_cast<std::size_t>(size));
}

extern "C" VYX_RT_ABI void* vyx_string_builder_create_abi() {
    auto* b = static_cast<VyxRtStringBuilderAbi*>(vyx_rt_string_builder_object_alloc());
    if (!b) { return nullptr; }
    b->buf = nullptr;
    b->len = 0;
    b->cap = 63;
    std::memset(b->inline_words, 0, sizeof(b->inline_words));
    return b;
}

static int64_t vyx_rt_string_builder_next_cap(int64_t current, int64_t needed) {
    int64_t cap = current;
    if (cap < 63) { cap = 63; }
    while (cap < needed) {
        cap = cap * 2 + 1;
    }
    return cap;
}

extern "C" VYX_RT_ABI void vyx_string_builder_append_i64_abi(void* builder, int64_t v) {
    auto* b = static_cast<VyxRtStringBuilderAbi*>(builder);
    if (!b) { return; }
    char tmp[32];
    const int64_t n = vyx_rt_i64_to_chars(tmp, v);
    const int64_t old_len = b->len;
    const int64_t needed = old_len + n + 1;
    if (needed >= b->cap) {
        const int64_t new_cap = vyx_rt_string_builder_next_cap(b->cap, needed);
        if (b->buf) {
            b->buf = static_cast<char*>(std::realloc(b->buf, static_cast<std::size_t>(new_cap)));
        } else {
            auto* new_buf = static_cast<char*>(std::malloc(static_cast<std::size_t>(new_cap)));
            if (new_buf && old_len > 0) {
                std::memcpy(new_buf, b->inline_words, static_cast<std::size_t>(old_len));
            }
            b->buf = new_buf;
        }
        b->cap = new_cap;
    }
    char* dst = b->buf ? b->buf : reinterpret_cast<char*>(b->inline_words);
    if (!dst) { return; }
    std::memcpy(dst + old_len, tmp, static_cast<std::size_t>(n));
    b->len = old_len + n;
}

extern "C" VYX_RT_ABI void vyx_string_builder_append_u64_abi(void* builder, uint64_t v) {
    auto* b = static_cast<VyxRtStringBuilderAbi*>(builder);
    if (!b) { return; }
    char tmp[32];
    const int64_t n = vyx_rt_u64_to_chars(tmp, v);
    const int64_t old_len = b->len;
    const int64_t needed = old_len + n + 1;
    if (needed >= b->cap) {
        const int64_t new_cap = vyx_rt_string_builder_next_cap(b->cap, needed);
        if (b->buf) {
            b->buf = static_cast<char*>(std::realloc(b->buf, static_cast<std::size_t>(new_cap)));
        } else {
            auto* new_buf = static_cast<char*>(std::malloc(static_cast<std::size_t>(new_cap)));
            if (new_buf && old_len > 0) {
                std::memcpy(new_buf, b->inline_words, static_cast<std::size_t>(old_len));
            }
            b->buf = new_buf;
        }
        b->cap = new_cap;
    }
    char* dst = b->buf ? b->buf : reinterpret_cast<char*>(b->inline_words);
    if (!dst) { return; }
    std::memcpy(dst + old_len, tmp, static_cast<std::size_t>(n));
    b->len = old_len + n;
}

extern "C" VYX_RT_ABI char* bool_to_string(int32_t v) {
    VYX_BOOTSTRAP_RT_PROF(BoolToString);
    return v ? vyx_rt_dup_malloc_bytes("true", 4)
             : vyx_rt_dup_malloc_bytes("false", 5);
}

static void vyx_rt_write_line(const char* s, uint64_t len) {
    if (!s) {
        std::fputc('\n', stdout);
        std::fflush(stdout);
        return;
    }
    if (len == 0) {
        len = vyx_rt_known_string_len(s);
    }
    if (len > 0) {
        std::fwrite(s, 1, static_cast<size_t>(len), stdout);
    }
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

extern "C" VYX_RT_ABI void print(const char* s, uint64_t len) {
    VYX_BOOTSTRAP_RT_PROF(Print);
    vyx_rt_write_line(s, len);
}

extern "C" VYX_RT_ABI void println(const char* s, uint64_t len) {
    VYX_BOOTSTRAP_RT_PROF(Print);
    vyx_rt_write_line(s, len);
}

extern "C" VYX_RT_ABI void vyx_rt_set_args(int32_t argc, char** argv) {
    vyx_rt_process_argc = argc >= 0 ? argc : 0;
    vyx_rt_process_argv = argv;
    vyx_rt_process_args_set = true;
}

extern "C" VYX_RT_ABI int32_t argCount() {
    VYX_BOOTSTRAP_RT_PROF(ArgCount);
    if (vyx_rt_process_args_set) {
        return vyx_rt_process_argc;
    }
#ifdef _WIN32
    return static_cast<int32_t>(__argc);
#else
    return 0;
#endif
}

extern "C" VYX_RT_ABI const char* getArg(int32_t idx) {
    VYX_BOOTSTRAP_RT_PROF(GetArg);
    int32_t argc = vyx_rt_process_argc;
    char** argv = vyx_rt_process_argv;
#ifdef _WIN32
    if (!vyx_rt_process_args_set) {
        argc = static_cast<int32_t>(__argc);
        argv = __argv;
    }
#endif
    if (idx < 0 || idx >= argc || !argv) {
        return vyx_rt_empty_cstr();
    }
    static thread_local std::vector<char*> cached_args;
    if (static_cast<std::size_t>(idx) >= cached_args.size()) {
        cached_args.resize(static_cast<std::size_t>(idx) + 1u, nullptr);
    }
    char*& cached = cached_args[static_cast<std::size_t>(idx)];
    if (!cached) {
        cached = vyx_rt_dup_cstr(argv[idx] ? argv[idx] : "");
    }
    return cached;
}

extern "C" VYX_RT_ABI char* readFile(const char* path) {
    VYX_BOOTSTRAP_RT_PROF(ReadFile);
    if (!path || !*path) {
        return vyx_rt_dup_bytes("", 0);
    }
    auto* fp = std::fopen(path, "rb");
    if (!fp) {
        return vyx_rt_dup_bytes("", 0);
    }
    if (std::fseek(fp, 0, SEEK_END) != 0) {
        std::fclose(fp);
        return vyx_rt_dup_bytes("", 0);
    }
    long size = std::ftell(fp);
    if (size < 0) {
        std::fclose(fp);
        return vyx_rt_dup_bytes("", 0);
    }
    std::rewind(fp);
    auto* buf = vyx_rt_alloc_string_storage(static_cast<uint64_t>(size));
    if (!buf) {
        std::fclose(fp);
        return nullptr;
    }
    size_t n = std::fread(buf, 1, static_cast<size_t>(size), fp);
    std::fclose(fp);
    buf[n] = '\0';
    auto* header = reinterpret_cast<VyxRtStringHeader*>(buf - sizeof(VyxRtStringHeader));
    header->len = static_cast<uint64_t>(n);
    vyx_rt_note_string_len_cached(buf, static_cast<uint64_t>(n));
    return buf;
}

extern "C" VYX_RT_ABI int64_t fileSize(const char* path) {
    VYX_BOOTSTRAP_RT_PROF(FileSize);
    if (!path || !*path) {
        return -1;
    }
    auto* fp = std::fopen(path, "rb");
    if (!fp) {
        return -1;
    }
    if (std::fseek(fp, 0, SEEK_END) != 0) {
        std::fclose(fp);
        return -1;
    }
    long size = std::ftell(fp);
    std::fclose(fp);
    return size < 0 ? -1 : static_cast<int64_t>(size);
}

extern "C" VYX_RT_ABI char* vyx_decode_string_escapes(const char* raw, int64_t n) {
    if (!raw || n <= 0) {
        return vyx_rt_empty_cstr();
    }
    char* out = vyx_string_alloc_abi(n + 1);
    if (!out) {
        return nullptr;
    }
    uint64_t w = 0;
    int64_t i = 0;
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(raw[i]);
        if (c == '\\' && i + 1 < n) {
            const unsigned char nx = static_cast<unsigned char>(raw[i + 1]);
            char ch = 0;
            int skip = 2;
            bool emit = true;
            if (nx == 'n') {
                ch = '\n';
            } else if (nx == 't') {
                ch = '\t';
            } else if (nx == 'r') {
                ch = '\r';
            } else if (nx == '\\') {
                ch = '\\';
            } else if (nx == '"') {
                ch = '"';
            } else if (nx == '\'') {
                ch = '\'';
            } else if (nx == '0') {
                emit = false;
            } else if (nx == '$') {
                ch = '$';
            } else {
                ch = static_cast<char>(c);
                skip = 1;
            }
            if (emit) {
                out[w++] = ch;
            }
            i += skip;
        } else {
            out[w++] = static_cast<char>(c);
            i += 1;
        }
    }
    out[w] = '\0';
    vyx_rt_note_string_len_cached(out, w);
    return out;
}

extern "C" VYX_RT_ABI char* from_cstr(const char* s) {
    VYX_BOOTSTRAP_RT_PROF(FromCstr);
    return vyx_rt_dup_cstr(s);
}

extern "C" VYX_RT_ABI char* from_cstr_len(const char* s, int64_t len) {
    VYX_BOOTSTRAP_RT_PROF(FromCstrLen);
    if (len < 0) {
        len = 0;
    }
    const auto n = static_cast<uint64_t>(len);
    if (n == 0) {
        return vyx_rt_empty_cstr();
    }
    if (char* adopted = vyx_rt_adopt_if_header_string(s, n)) {
        return adopted;
    }
    return vyx_rt_dup_bytes(s, n);
}

extern "C" VYX_RT_ABI char* from_cstr_view_len(const char* s, int64_t len) {
    VYX_BOOTSTRAP_RT_PROF(FromCstrViewLen);
    if (!s) {
        return vyx_rt_empty_cstr();
    }
    if (len < 0) {
        len = 0;
    }
    const auto n = static_cast<uint64_t>(len);
    if (n == 0) {
        return vyx_rt_empty_cstr();
    }
    if (char* adopted = vyx_rt_adopt_if_header_string(s, n)) {
        return adopted;
    }
    vyx_rt_note_string_len_cached(s, n);
    return const_cast<char*>(s);
}

extern "C" VYX_RT_ABI char* from_cstr_borrowed_cstr_len(const char* s, int64_t len) {
    if (!s) {
        return vyx_rt_empty_cstr();
    }
    if (len < 0) {
        len = 0;
    }
    const auto n = static_cast<uint64_t>(len);
    if (n == 0) {
        return vyx_rt_empty_cstr();
    }
    if (char* adopted = vyx_rt_adopt_if_header_string(s, n)) {
        return adopted;
    }
    vyx_rt_note_string_len_cached(s, n);
    return const_cast<char*>(s);
}

#ifndef VYX_STATIC_LINK_BOOTSTRAP

extern "C" VYX_RT_ABI void* to_rawptr(void* p) {
    VYX_BOOTSTRAP_RT_PROF(ToRawptr);
    return p;
}

extern "C" VYX_RT_ABI void* ptr_offset(void* p, int32_t offset) {
    VYX_BOOTSTRAP_RT_PROF(PtrOffset);
    if (!p) {
        return nullptr;
    }
    return static_cast<void*>(static_cast<char*>(p) + offset);
}

extern "C" VYX_RT_ABI int32_t ptr_read_i32(void* p) {
    VYX_BOOTSTRAP_RT_PROF(PtrReadI32);
    int32_t v = 0;
    if (p) {
        std::memcpy(&v, p, sizeof(v));
    }
    return v;
}

extern "C" VYX_RT_ABI int32_t ptr_read_u8(void* p) {
    VYX_BOOTSTRAP_RT_PROF(PtrReadU8);
    if (!p) {
        return 0;
    }
    return static_cast<int32_t>(*static_cast<unsigned char*>(p));
}

extern "C" VYX_RT_ABI int64_t ptr_read_i64(void* p) {
    VYX_BOOTSTRAP_RT_PROF(PtrReadI64);
    int64_t v = 0;
    if (p) {
        std::memcpy(&v, p, sizeof(v));
    }
    return v;
}

extern "C" VYX_RT_ABI void ptr_write_i32(void* p, int32_t v) {
    VYX_BOOTSTRAP_RT_PROF(PtrWriteI32);
    if (p) {
        std::memcpy(p, &v, sizeof(v));
    }
}

extern "C" VYX_RT_ABI void ptr_write_i64(void* p, int64_t v) {
    VYX_BOOTSTRAP_RT_PROF(PtrWriteI64);
    if (p) {
        std::memcpy(p, &v, sizeof(v));
    }
}

#endif // VYX_STATIC_LINK_BOOTSTRAP
