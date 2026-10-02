#define _CRT_SECURE_NO_WARNINGS
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>

struct Guard { std::int64_t id, b, c, d; };
static_assert(sizeof(Guard) == 32);
struct Failure { int mode; };
#ifndef _WIN32
// This probe does not inspect process arguments. Its Linux link needs only
// the generated entry's args ABI, not the platform's complete Vyx runtime.
extern "C" void vyx_rt_set_args(int, char**) {}
// Native Vyx class storage uses the class arena, not DCI's malloc domain.
extern "C" void* vyx_class_alloc_abi(std::int64_t size) {
    alignas(16) static unsigned char arena[4096];
    static std::size_t offset;
    if (size < 0 || offset + size > sizeof(arena)) std::abort();
    auto* result = arena + offset;
    offset += (size + 15) & ~15;
    return result;
}
#endif
struct Allocation { void* ptr; bool live, complete, failed; };
static Allocation allocations[64];
static int allocated, released, trace[64], trace_size, native_cleaned;

[[noreturn]] static void fail(const char* text) {
    std::fprintf(stderr, "shared unwind: %s\n", text);
    std::abort();
}
static Allocation& live(void* ptr) {
    for (int i = allocated - 1; i >= 0; --i)
        if (allocations[i].ptr == ptr && allocations[i].live) return allocations[i];
    fail("unknown or released object");
}
extern "C" void* tracked_malloc(std::uint64_t size) {
    if (size != sizeof(Guard) || allocated == 64) fail("unexpected allocation");
    void* ptr = std::malloc(size);
    if (!ptr) fail("allocation failed");
    allocations[allocated++] = {ptr, true, false, false};
    return ptr;
}
extern "C" void* tracked_calloc(std::uint64_t count, std::uint64_t size) {
    if (count != 1) fail("unexpected calloc");
    void* ptr = tracked_malloc(size);
    std::memset(ptr, 0, size);
    return ptr;
}
extern "C" void tracked_free(void* ptr) {
    auto& item = live(ptr);
    if (item.complete) fail("free before destructor");
    item.live = false;
    ++released;
    std::free(ptr);
}
struct NativeGuard { ~NativeGuard() noexcept { ++native_cleaned; } };
extern "C" Guard* guard_construct(Guard* ptr, int id) {
    auto& item = live(ptr);
    if (id < 0) {
        NativeGuard member;
        item.failed = true;
        throw Failure{2};
    }
    ptr->id = id;
    item.complete = true;
    return ptr;
}
extern "C" void guard_destroy(Guard* ptr) {
    auto& item = live(ptr);
    if (!item.complete || item.failed) fail("destructor on incomplete object");
    item.complete = false;
    trace[trace_size++] = static_cast<int>(ptr->id);
    if (ptr->id == 11) throw Failure{10};
    if (ptr->id == 8 && std::getenv("DCI_EH_DOUBLE_THROW")) {
        std::fputs("SECOND_EXCEPTION_THROWN\n", stderr);
        std::fflush(stderr);
        throw Failure{999};
    }
}
extern "C" int contract_throw(int mode) {
    if (mode == 0) return 20;
    NativeGuard guard;
    throw Failure{mode};
}
extern "C" void record_drop(int id) noexcept { trace[trace_size++] = id; }
extern "C" int harness(void* callback, int mode) noexcept {
    if (std::getenv("DCI_EH_DOUBLE_THROW")) mode = 8;
    allocated = released = trace_size = native_cleaned = 0;
    bool caught = false;
    int result = -1;
    try {
        NativeGuard guard;
        result = reinterpret_cast<int (*)(int)>(callback)(mode);
    } catch (const Failure& ex) {
        if (ex.mode == 999) return 77;
        caught = ex.mode == mode;
    } catch (...) { fail("exception identity changed"); }
    static const int expected[][7] = {
        {1,2,3,0}, {2,1,3,0}, {1,3,0}, {1,2,3,0},
        {2,4,1,3,0}, {5,5,2,1,3,0}, {6,2,1,3,0}, {7,2,1,3,0}, {8,2,1,3,0}, {9,2,1,3,0}, {1,11,2,3,0}
    };
    const bool should_throw = mode != 0 && mode != 3;
    if (caught != should_throw) fail("propagation did not reach native catcher");
    if (!should_throw && result != (mode == 0 ? 20 : 30)) fail("normal return value");
    if (allocated != released) {
        std::fprintf(stderr, "mode=%d allocated=%d released=%d destructors=%d\n",
                     mode, allocated, released, trace_size);
        return 10 + mode;
    }
    int n = 0;
    while (expected[mode][n]) {
        if (n >= trace_size || trace[n] != expected[mode][n]) {
            std::fprintf(stderr, "mode=%d trace:", mode);
            for (int i = 0; i < trace_size; ++i) std::fprintf(stderr, " %d", trace[i]);
            std::fputc('\n', stderr);
            fail("cleanup order");
        }
        ++n;
    }
    if (n != trace_size) fail("extra destructor");
    if (native_cleaned != (should_throw && mode != 10 ? 2 : 1)) fail("native stack cleanup");
    std::printf("mode=%d caught=%d allocated=%d released=%d destructors=%d OK\n",
                mode, caught, allocated, released, trace_size);
    return 0;
}

extern "C" std::int64_t inline_create(std::int64_t seed) noexcept { return seed; }
extern "C" void inline_destroy(std::int64_t* value) noexcept { record_drop(static_cast<int>(*value)); }
extern "C" int inline_harness(void* callback) noexcept {
    allocated = released = trace_size = native_cleaned = 0;
    bool caught = false;
    try { reinterpret_cast<int (*)(int)>(callback)(1); }
    catch (const Failure& ex) { caught = ex.mode == 1; }
    catch (...) { fail("inline exception identity changed"); }
    if (!caught || trace_size != 1 || trace[0] != 10 || native_cleaned != 1
        || allocated != 0 || released != 0) fail("inline stack object cleanup");
    std::puts("inline stack cleanup OK");
    return 0;
}
