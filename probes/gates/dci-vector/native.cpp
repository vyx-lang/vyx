#include "vector.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <new>
#include <stdexcept>

namespace {
constexpr int limit = 16384;
struct HeapAllocation { void* pointer; std::size_t size; bool live; };
struct ObjectAllocation { void* pointer; void* buffer; bool live; bool observed; };
HeapAllocation heap[limit]{};
ObjectAllocation objects[limit]{};
int heap_allocated = 0, heap_released = 0, object_allocated = 0, object_released = 0;
int checked = 0;
bool tracking = false;
bool hooked = false;

[[noreturn]] void fail(const char* message) {
    std::fprintf(stderr, "dci_vector: %s\n", message);
    std::abort();
}
int heap_index(void* pointer, bool interior = false) {
    auto address = reinterpret_cast<std::uintptr_t>(pointer);
    for (int i = heap_allocated - 1; i >= 0; --i)
        if (heap[i].live) {
            auto start = reinterpret_cast<std::uintptr_t>(heap[i].pointer);
            if (start == address || (interior && address > start && address - start < heap[i].size)) return i;
        }
    return -1;
}
ObjectAllocation* object(void* pointer) {
    for (int i = object_allocated - 1; i >= 0; --i)
        if (objects[i].pointer == pointer && objects[i].live) return &objects[i];
    return nullptr;
}
struct ReferenceScope {
    bool previous = tracking;
    ReferenceScope() { tracking = false; }
    ~ReferenceScope() { tracking = previous; }
};

template <class T> void observe(std::vector<T>* value) {
    if (!hooked) return;
    auto* item = object(value);
    if (!item) fail("native reference check did not receive a live DCI object");
    item->buffer = value->data();
    item->observed = true;
    // MSVC aligns large vector buffers inside the underlying new allocation.
    if (value->capacity() && heap_index(item->buffer, true) < 0)
        fail("vector buffer was not allocated by the original standard library");
}
template <class T> int compare(std::vector<T>* actual, const std::vector<T>& expected,
                               int stage, int seed) {
    if (actual->size() != expected.size() || actual->capacity() != expected.capacity()) {
        std::fprintf(stderr, "stage=%d seed=%d size=%zu/%zu capacity=%zu/%zu\n", stage, seed,
                     actual->size(), expected.size(), actual->capacity(), expected.capacity());
        return 1;
    }
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (actual->at(i) != expected.at(i)) {
            std::fprintf(stderr, "stage=%d seed=%d element=%zu differs\n", stage, seed, i);
            return 2;
        }
    }
    observe(actual);
    ++checked;
    return 0;
}
bool balanced() {
    return !hooked || (object_allocated == object_released && heap_allocated == heap_released);
}
}

// Track allocations made by the genuine std::allocator. Reference vectors are
// built while tracking is suspended, so their allocations cannot hide a leak.
void* operator new(std::size_t size) {
    void* pointer = std::malloc(size ? size : 1);
    if (!pointer) throw std::bad_alloc();
    if (tracking) {
        if (heap_allocated == limit) fail("standard library allocation ledger exhausted");
        heap[heap_allocated++] = {pointer, size ? size : 1, true};
    }
    return pointer;
}
void operator delete(void* pointer) noexcept {
    if (!pointer) return;
    int index = heap_index(pointer);
    if (index >= 0) { heap[index].live = false; ++heap_released; }
    std::free(pointer);
}
void operator delete(void* pointer, std::size_t) noexcept { ::operator delete(pointer); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void* pointer) noexcept { ::operator delete(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { ::operator delete(pointer); }

extern "C" void* tracked_malloc(std::uint64_t size) {
    if (!tracking || (size != sizeof(std::vector<int>) && size != sizeof(std::vector<double>)))
        fail("unexpected consumer allocation");
    if (object_allocated == limit) fail("DCI object ledger exhausted");
    void* pointer = std::malloc(static_cast<std::size_t>(size));
    if (!pointer) fail("DCI object allocation failed");
    objects[object_allocated++] = {pointer, nullptr, true, false};
    hooked = true;
    return pointer;
}
extern "C" void* tracked_calloc(std::uint64_t count, std::uint64_t size) {
    if (count != 1) fail("unexpected consumer calloc");
    void* pointer = tracked_malloc(size);
    std::memset(pointer, 0, static_cast<std::size_t>(size));
    return pointer;
}
extern "C" void tracked_free(void* pointer) {
    auto* item = object(pointer);
    if (!item) fail("unknown or already released DCI object");
    if (!item->observed) fail("DCI object was released without a native reference check");
    if (item->buffer && heap_index(item->buffer, true) >= 0)
        fail("DCI object storage released before its vector destructor released the buffer");
    item->live = false;
    ++object_released;
    std::free(pointer);
}

extern "C" int vector_check_i32(void* pointer, int stage, int seed) {
    ReferenceScope reference;
    std::vector<int> expected{1,2,3,4,5};
    if (stage >= 1) expected.reserve(32);
    if (stage >= 2) for (int i = 0; i < 4096; ++i) expected.push_back(seed + i * 3);
    if (stage >= 3) {
        for (std::size_t i = 0; i < expected.size(); i += 17) expected.at(i) += seed + 7;
        *expected.data() += 11;
    }
    if (stage >= 4) for (int i = 0; i < 1024; ++i) expected.pop_back();
    if (stage >= 5) expected.clear();
    if (stage >= 6) for (int i = 0; i < 128; ++i) expected.push_back(seed - i);
    return compare(static_cast<std::vector<int>*>(pointer), expected, stage, seed);
}
extern "C" int vector_check_f64(void* pointer, int stage, int seed) {
    ReferenceScope reference;
    std::vector<double> expected{0.5,1.5,2.5};
    if (stage >= 1) expected.reserve(16);
    if (stage >= 2) for (int i = 0; i < 2048; ++i) expected.push_back(seed + i * 0.25);
    if (stage >= 3) {
        for (std::size_t i = 0; i < expected.size(); i += 19) expected.at(i) += seed + 0.5;
        *expected.data() += 0.25;
    }
    if (stage >= 4) for (int i = 0; i < 512; ++i) expected.pop_back();
    if (stage >= 5) expected.clear();
    if (stage >= 6) for (int i = 0; i < 64; ++i) expected.push_back(seed - i * 0.5);
    return compare(static_cast<std::vector<double>*>(pointer), expected, stage, seed);
}
extern "C" int vector_note_i32(void* pointer) { observe(static_cast<std::vector<int>*>(pointer)); return 0; }
extern "C" int vector_note_f64(void* pointer) { observe(static_cast<std::vector<double>*>(pointer)); return 0; }

extern "C" int vector_harness(void* stress, void* throwing) {
    object_allocated = object_released = heap_allocated = heap_released = checked = 0;
    hooked = false;
    tracking = true;
    for (int seed = 0; seed < 64; ++seed) {
        int result = reinterpret_cast<int (*)(int)>(stress)(seed);
        if (result) return result;
        if (!balanced()) fail("normal return leaked an object or standard library allocation");
    }
    for (int mode = 0; mode < 2; ++mode) {
        bool caught = false;
        try { reinterpret_cast<int (*)(int)>(throwing)(mode); }
        catch (const std::out_of_range&) { caught = true; }
        catch (...) { fail("standard library exception identity changed"); }
        if (!caught) fail("std::vector::at did not propagate std::out_of_range");
        if (!balanced()) fail("exception propagation leaked an object or standard library allocation");
    }
    tracking = false;
    if (checked != 64 * 14) fail("not all native reference phases ran");
    if (hooked && object_allocated != 132) fail("unexpected DCI object count");
    std::printf("vector reference comparisons=%d pushes=%d exceptions=2", checked, 64 * (4096+128+2048+64));
    if (hooked) std::printf(" objects=%d/%d std_allocations=%d/%d", object_allocated,
                            object_released, heap_allocated, heap_released);
    std::puts(" OK");
    return 0;
}
