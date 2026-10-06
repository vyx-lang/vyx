#include "exception_test.hpp"
#include <QtWidgets/QApplication>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>

struct QtCounterException { int mode; };
static int destroyed[8], destroyed_count, allocated, released, native_cleaned;
struct Allocation { void* pointer; bool live; };
static Allocation allocations[16];

static void fail(const char* message) {
    std::fprintf(stderr, "Qt shared unwind: %s\n", message);
    std::abort();
}

extern "C" void* tracked_malloc(std::uint64_t size) {
    if (allocated == 16) fail("too many DCI allocations");
    void* pointer = std::malloc(size);
    if (!pointer) fail("allocation failed");
    allocations[allocated++] = {pointer, true};
    return pointer;
}
extern "C" void* tracked_calloc(std::uint64_t count, std::uint64_t size) {
    if (count != 1) fail("unexpected DCI calloc count");
    void* pointer = tracked_malloc(size);
    std::memset(pointer, 0, size);
    return pointer;
}
extern "C" void tracked_free(void* pointer) {
    for (int index = allocated - 1; index >= 0; --index) {
        if (allocations[index].pointer == pointer && allocations[index].live) {
            allocations[index].live = false;
            ++released;
            std::free(pointer);
            return;
        }
    }
    fail("unknown allocation or repeated free");
}

void qt_counter_watch(QWidget* widget, int id) {
    QObject::connect(widget, &QObject::destroyed, [id] {
        if (destroyed_count == 8) fail("too many QObject destructions");
        destroyed[destroyed_count++] = id;
    });
}
int qt_counter_throw(int mode) {
    struct Guard { ~Guard() { ++native_cleaned; } } guard;
    if (mode != 0) throw QtCounterException{mode};
    return 42;
}
int qt_counter_call_visible(QWidget* widget) {
    widget->setVisible(true);
    return 42;
}

extern "C" int qt_counter_harness(void* callback, int mode) noexcept {
    int argc = 1;
    char name[] = "qt_shared_unwind";
    char* argv[] = {name, nullptr};
    QApplication app(argc, argv);
    allocated = released = destroyed_count = native_cleaned = 0;
    bool caught = false;
    int result = 0;
    try {
        result = reinterpret_cast<int (*)(int)>(callback)(mode);
    } catch (const QtCounterException& exception) {
        caught = exception.mode == mode;
    } catch (...) {
        fail("original exception type was not preserved");
    }
    if (caught != (mode != 0) || (!caught && result != 42)) fail("wrong propagation/result");
    const int expected_allocations = mode >= 2 ? 0 : 3;
    const int expected_objects = mode >= 2 ? 1 : 2;
    if (allocated != expected_allocations || released != allocated) {
        std::fprintf(stderr, "mode=%d allocated=%d released=%d destroyed=%d native=%d\n",
                     mode, allocated, released, destroyed_count, native_cleaned);
        fail("DCI storage leak");
    }
    if (destroyed_count != expected_objects) {
        std::fprintf(stderr, "mode=%d allocated=%d released=%d destroyed=%d expected=%d\n",
                     mode, allocated, released, destroyed_count, expected_objects);
        fail("QObject destruction mismatch");
    }
    if (mode == 1 && (destroyed[0] != 2 || destroyed[1] != 1)) fail("wrong unwind destruction order");
    if (mode >= 2 && destroyed[0] != 1) fail("factory object not destroyed");
    if (native_cleaned != 1) fail("native producer cleanup missing");
    std::printf("Qt shared unwind mode=%d caught=%d objects=%d storage=%d/%d native=1 OK\n",
                mode, caught, destroyed_count, allocated, released);
    return 0;
}
