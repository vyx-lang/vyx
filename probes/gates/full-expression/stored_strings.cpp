#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

struct Allocation { void* pointer; bool live; };
static Allocation allocations[32];
static int allocated, released;

extern "C" void* payload_allocate(int id) {
    if (allocated == 32) std::abort();
    auto* pointer = static_cast<char*>(std::malloc(33));
    if (!pointer) std::abort();
    std::memset(pointer, 'a' + id, 32);
    pointer[32] = '\0';
    allocations[allocated++] = {pointer, true};
    return pointer;
}

// Only the Vyx IR's free calls are redirected here; these native allocations
// remain ordinary malloc allocations and retain the production ownership tag.
extern "C" void tracked_free(void* pointer) {
    if (!pointer) return;
    for (int i = allocated - 1; i >= 0; --i) {
        if (allocations[i].pointer != pointer) continue;
        if (!allocations[i].live) {
            std::fputs("stored string freed twice\n", stderr);
            std::abort();
        }
        allocations[i].live = false;
        ++released;
        break;
    }
    std::free(pointer);
}

extern "C" void payload_expect(int expected_allocated, int expected_released) {
    if (allocated == expected_allocated && released == expected_released) return;
    std::fprintf(stderr, "payloads allocated=%d released=%d expected=%d/%d\n",
                 allocated, released, expected_allocated, expected_released);
    std::exit(1);
}
