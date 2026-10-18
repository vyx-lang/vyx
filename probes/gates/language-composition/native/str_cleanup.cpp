#include <cstdlib>
#include <cstdio>
#include <unordered_set>

static std::unordered_set<void*> live;
static unsigned long long allocated = 0, released = 0, cloned = 0;
extern "C" void* vyx_string_clone_len_abi(const void*, long long);
extern "C" void* vyx_string_alloc_abi(long long);
extern "C" void vyx_free_runtime_string(void*);
extern "C" void vyx_free_pooled_string(void*);
extern "C" void* probe_clone(const void* source, long long length) {
    void* pointer = vyx_string_clone_len_abi(source, length);
    if (pointer) { live.insert(pointer); ++allocated; ++cloned; }
    return pointer;
}
extern "C" void* probe_string_alloc(long long size) {
    void* pointer = vyx_string_alloc_abi(size);
    if (pointer) { live.insert(pointer); ++allocated; ++cloned; }
    return pointer;
}
extern "C" void probe_runtime_free(void* pointer) {
    if (live.erase(pointer)) { ++released; }
    vyx_free_runtime_string(pointer);
}
extern "C" void probe_pooled_free(void* pointer) {
    if (live.erase(pointer)) { ++released; }
    vyx_free_pooled_string(pointer);
}
extern "C" void* probe_malloc(std::size_t size) {
    void* pointer = std::malloc(size);
    if (pointer) { live.insert(pointer); ++allocated; }
    return pointer;
}
extern "C" void* probe_calloc(std::size_t count, std::size_t size) {
    void* pointer = std::calloc(count, size);
    if (pointer) { live.insert(pointer); ++allocated; }
    return pointer;
}
extern "C" void probe_free(void* pointer) {
    if (live.erase(pointer)) { ++released; }
    std::free(pointer);
}
extern "C" int probe_entry(int, char**);
int main(int argc, char** argv) {
    const long long expected_clones = argc > 1 ? std::strtoll(argv[1], nullptr, 10) : 4;
    const int code = probe_entry(argc, argv);
    std::printf("tracked=%llu/%llu live=%zu clones=%llu\n", allocated, released, live.size(), cloned);
    return code != 0 ? code : ((expected_clones < 0 || cloned == static_cast<unsigned long long>(expected_clones))
        && live.empty() && allocated == released ? 0 : 9);
}
