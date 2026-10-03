#define TBB_USE_EXCEPTIONS 0
#define __TBB_NO_IMPLICIT_LINKAGE 1

#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/task_arena.h>
#include <oneapi/tbb/task_group.h>

#include <stdint.h>

using tbb::blocked_range;
using tbb::parallel_for;
using tbb::task_arena;
using tbb::task_group;

typedef void (*vyx_tbb_job)(void* ctx, int64_t index);

struct VyxTbbRangeBody {
    vyx_tbb_job fn;
    void* ctx;
    void operator()(const blocked_range<int64_t>& range) const {
        int64_t i = range.begin();
        while (i != range.end()) {
            fn(ctx, i);
            i += 1;
        }
    }
};

struct VyxTbbIndexJob {
    vyx_tbb_job fn;
    void* ctx;
    int64_t index;
    void operator()() const {
        fn(ctx, index);
    }
};

extern "C" int32_t vyx_tbb_max_concurrency(void) {
    return (int32_t)tbb::this_task_arena::max_concurrency();
}

extern "C" int32_t vyx_tbb_parallel_for(int64_t n, void* fn, void* ctx) {
    if (n <= 0) { return 0; }
    if (fn == NULL) { return 1; }
    VyxTbbRangeBody body;
    body.fn = (vyx_tbb_job)(uintptr_t)fn;
    body.ctx = ctx;
    parallel_for(blocked_range<int64_t>(0, n), body);
    return 0;
}

extern "C" int32_t vyx_tbb_task_group_run_n(int64_t n, void* fn, void* ctx) {
    if (n <= 0) { return 0; }
    if (fn == NULL) { return 1; }
    task_group group;
    int64_t i = 0;
    while (i < n) {
        VyxTbbIndexJob job;
        job.fn = (vyx_tbb_job)(uintptr_t)fn;
        job.ctx = ctx;
        job.index = i;
        group.run(job);
        i += 1;
    }
    group.wait();
    return 0;
}

extern "C" void* vyx_tbb_arena_new(int32_t nthreads) {
    task_arena* arena = new task_arena();
    if (nthreads > 0) {
        arena->initialize(nthreads);
    } else {
        arena->initialize();
    }
    return arena;
}

extern "C" int32_t vyx_tbb_arena_max_concurrency(void* arena) {
    if (arena == NULL) { return 0; }
    return (int32_t)static_cast<task_arena*>(arena)->max_concurrency();
}

extern "C" int32_t vyx_tbb_arena_parallel_for(void* arena, int64_t n, void* fn, void* ctx) {
    if (arena == NULL) { return 1; }
    if (n <= 0) { return 0; }
    if (fn == NULL) { return 1; }
    VyxTbbRangeBody body;
    body.fn = (vyx_tbb_job)(uintptr_t)fn;
    body.ctx = ctx;
    static_cast<task_arena*>(arena)->execute([&] {
        parallel_for(blocked_range<int64_t>(0, n), body);
    });
    return 0;
}

extern "C" void vyx_tbb_arena_free(void* arena) {
    delete static_cast<task_arena*>(arena);
}
