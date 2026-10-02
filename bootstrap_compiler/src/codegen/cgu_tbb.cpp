#define TBB_USE_EXCEPTIONS 0
#define __TBB_NO_IMPLICIT_LINKAGE 1

#include <oneapi/tbb/task_arena.h>
#include <oneapi/tbb/task_group.h>

#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <new>
#include <vector>

using VyxCguEmit = int32_t (*)(void*, void*, int64_t, int32_t);

struct VyxCguTbbPool {
    oneapi::tbb::task_arena arena;
    oneapi::tbb::task_group tasks;
    std::mutex mutex;
    std::condition_variable free_slot;
    std::vector<int32_t> results;
    VyxCguEmit emit;
    int32_t limit;
    int32_t in_flight = 0;
    bool finished = false;

    VyxCguTbbPool(int32_t units, int32_t workers, VyxCguEmit callback)
        : arena(workers), results(static_cast<std::size_t>(units), -1),
          emit(callback), limit(workers) {}
};

extern "C" void* vyx_tbb_cgu_pool_new(int32_t units, int32_t workers, void* emit) {
    if (units < 2 || workers < 1 || emit == nullptr) { return nullptr; }
    return new (std::nothrow) VyxCguTbbPool(
        units, workers, reinterpret_cast<VyxCguEmit>(emit));
}

// The permit covers both queued and executing modules. The producer may own
// one additional module while it lowers the next CGU.
extern "C" int32_t vyx_tbb_cgu_pool_submit(void* handle, int32_t unit, void* module,
                                            void* path, int64_t path_len, int32_t opt) {
    auto* pool = static_cast<VyxCguTbbPool*>(handle);
    if (pool == nullptr || module == nullptr || path == nullptr || unit < 1
        || static_cast<std::size_t>(unit) >= pool->results.size()) {
        return 1;
    }
    {
        std::unique_lock<std::mutex> lock(pool->mutex);
        pool->free_slot.wait(lock, [&] {
            return pool->finished || pool->in_flight < pool->limit;
        });
        if (pool->finished) { return 1; }
        ++pool->in_flight;
    }
    pool->arena.execute([&] {
        pool->tasks.run([pool, unit, module, path, path_len, opt] {
            const int32_t rc = pool->emit(module, path, path_len, opt);
            std::free(path);
            {
                std::lock_guard<std::mutex> lock(pool->mutex);
                pool->results[static_cast<std::size_t>(unit)] = rc;
                --pool->in_flight;
            }
            pool->free_slot.notify_one();
        });
    });
    return 0;
}

extern "C" void vyx_tbb_cgu_pool_finish(void* handle) {
    auto* pool = static_cast<VyxCguTbbPool*>(handle);
    if (pool == nullptr) { return; }
    {
        std::lock_guard<std::mutex> lock(pool->mutex);
        pool->finished = true;
    }
    pool->free_slot.notify_all();
    pool->arena.execute([&] { pool->tasks.wait(); });
}

extern "C" int32_t vyx_tbb_cgu_pool_unit_ok(void* handle, int32_t unit) {
    auto* pool = static_cast<VyxCguTbbPool*>(handle);
    if (pool == nullptr || unit < 1
        || static_cast<std::size_t>(unit) >= pool->results.size()) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(pool->mutex);
    return pool->results[static_cast<std::size_t>(unit)] == 0 ? 1 : 0;
}

extern "C" void vyx_tbb_cgu_pool_free(void* handle) {
    auto* pool = static_cast<VyxCguTbbPool*>(handle);
    if (pool == nullptr) { return; }
    vyx_tbb_cgu_pool_finish(pool);
    delete pool;
}
