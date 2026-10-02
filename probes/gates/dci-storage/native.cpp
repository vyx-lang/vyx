#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>

// Match the BigState declaration in dci_complex_abi's measured contract.
// The remaining exported classes in its header are unnecessary for this probe.
namespace abi_complex {
class BigState {
public:
    long long a, b, c, d;
    explicit BigState(int seed) noexcept;
    ~BigState() noexcept;
    long long sum() const noexcept;
};
static_assert(sizeof(BigState) == 32);
}

namespace {
struct Allocation { void* pointer; bool live; bool destroyed; };
Allocation allocations[4096]{};
int allocated = 0;
int released = 0;
int destroyed = 0;

[[noreturn]] void fail(const char* message) {
    std::fprintf(stderr, "dci_storage: %s\n", message);
    std::abort();
}

Allocation& live_allocation(void* pointer) {
    for (int i = allocated - 1; i >= 0; --i) {
        if (allocations[i].pointer == pointer && allocations[i].live) {
            return allocations[i];
        }
    }
    fail("unknown or already released storage");
}
}

extern "C" void* tracked_malloc(std::int64_t size) {
    if (size != sizeof(abi_complex::BigState) || allocated == 4096) {
        fail("unexpected allocation");
    }
    void* pointer = std::malloc(static_cast<std::size_t>(size));
    if (!pointer) { fail("allocation failed"); }
    allocations[allocated++] = {pointer, true, false};
    return pointer;
}

extern "C" void tracked_free(void* pointer) {
    auto& allocation = live_allocation(pointer);
    if (!allocation.destroyed) { fail("storage released before destructor"); }
    allocation.live = false;
    ++released;
    std::free(pointer);
}

extern "C" void* tracked_class_malloc(std::int64_t size) {
    return tracked_malloc(size);
}

extern "C" void* tracked_calloc(std::uint64_t count, std::uint64_t size) {
    if (count != 1) { fail("unexpected zeroed allocation"); }
    void* pointer = tracked_malloc(static_cast<std::int64_t>(size));
    std::memset(pointer, 0, static_cast<std::size_t>(size));
    return pointer;
}

extern "C" int tracked_live() {
    if (allocated != released) {
        std::fprintf(stderr, "allocated=%d destructed=%d released=%d live=%d\n",
                     allocated, destroyed, released, allocated - released);
    }
    return allocated - released;
}
extern "C" int tracked_balanced() {
    std::printf("allocated=%d destructed=%d released=%d\n", allocated, destroyed, released);
    return allocated > 2048 && allocated == released && allocated == destroyed;
}

namespace abi_complex {
BigState::BigState(int seed) noexcept
    : a(seed), b(seed + 1), c(seed + 2), d(seed + 3) {
    live_allocation(this);
}

BigState::~BigState() noexcept {
    auto& allocation = live_allocation(this);
    if (allocation.destroyed) { fail("destructor ran twice"); }
    allocation.destroyed = true;
    ++destroyed;
}

long long BigState::sum() const noexcept { return a + b + c + d; }
}
