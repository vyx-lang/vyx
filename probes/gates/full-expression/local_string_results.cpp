#include <cstdint>
#include <cstdio>
#include <cstdlib>

extern "C" {
void* vyx_string_alloc_abi(std::int64_t);
void* vyx_string_clone_len_abi(const char*, std::int64_t);
void* vyx_string_to_upper_len_abi(const char*, std::int64_t);
void* vyx_string_to_lower_len_abi(const char*, std::int64_t);
void* vyx_string_replace(const char*, const char*, const char*);
void* vyx_string_trim(const char*);
void* vyx_runtime_malloc(std::uint64_t);
void vyx_runtime_free(void*);
void vyx_free_pooled_string(void*);
void vyx_free_runtime_string(void*);
}

struct Allocation { void* pointer; bool live; };
static Allocation ledger[128];
static int allocated, released, previous_checkpoint;

static void* observe(void* pointer) {
    if (!pointer || allocated == 128) std::abort();
    for (int i = allocated - 1; i >= 0; --i)
        if (ledger[i].pointer == pointer && ledger[i].live) std::abort();
    ledger[allocated++] = {pointer, true};
    return pointer;
}

static void release(void* pointer) {
    if (!pointer) return;
    for (int i = allocated - 1; i >= 0; --i) {
        if (ledger[i].pointer != pointer) continue;
        if (!ledger[i].live) {
            std::fputs("local string result released twice\n", stderr);
            std::abort();
        }
        ledger[i].live = false;
        ++released;
        return;
    }
}

// Redirect only IR call sites. Each wrapper delegates to the original runtime
// allocator/releaser, preserving its allocation domain and ownership tags.
extern "C" void* track_vyx_string_alloc_abi(std::int64_t n) {
    return observe(vyx_string_alloc_abi(n));
}
extern "C" void* track_vyx_string_clone_len_abi(const char* p, std::int64_t n) {
    return observe(vyx_string_clone_len_abi(p, n));
}
extern "C" void* track_vyx_string_to_upper_len_abi(const char* p, std::int64_t n) {
    return observe(vyx_string_to_upper_len_abi(p, n));
}
extern "C" void* track_vyx_string_to_lower_len_abi(const char* p, std::int64_t n) {
    return observe(vyx_string_to_lower_len_abi(p, n));
}
extern "C" void* track_vyx_string_replace(const char* p, const char* old, const char* replacement) {
    return observe(vyx_string_replace(p, old, replacement));
}
extern "C" void* track_vyx_string_trim(const char* p) { return observe(vyx_string_trim(p)); }
extern "C" void* track_vyx_runtime_malloc(std::uint64_t n) {
    return observe(vyx_runtime_malloc(n));
}
extern "C" void track_vyx_runtime_free(void* p) { release(p); vyx_runtime_free(p); }
extern "C" void track_vyx_free_pooled_string(void* p) { release(p); vyx_free_pooled_string(p); }
extern "C" void track_vyx_free_runtime_string(void* p) { release(p); vyx_free_runtime_string(p); }

extern "C" void allocation_checkpoint(int stage) {
    if (allocated != released || allocated <= previous_checkpoint) {
        std::fprintf(stderr, "stage=%d allocated=%d released=%d previous=%d\n",
                     stage, allocated, released, previous_checkpoint);
        std::exit(1);
    }
    previous_checkpoint = allocated;
}
