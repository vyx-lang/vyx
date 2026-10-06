#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <iostream>
extern "C" void* vyx_tbb_cgu_pool_new(int, int, void*);
extern "C" int vyx_tbb_cgu_pool_submit(void*, int, void*, void*, long long, int);
extern "C" void vyx_tbb_cgu_pool_finish(void*);
extern "C" int vyx_tbb_cgu_pool_unit_ok(void*, int);
extern "C" void vyx_tbb_cgu_pool_free(void*);
std::atomic<int> completed{0};
int emit(void*, void*, long long, int) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    ++completed;
    return 0;
}
int main() {
    for (int workers : {1, 2, 4}) {
        completed = 0;
        void* pool = vyx_tbb_cgu_pool_new(33, workers, reinterpret_cast<void*>(&emit));
        assert(pool);
        for (int unit = 1; unit < 33; ++unit) {
            auto* path = static_cast<char*>(std::malloc(2));
            path[0] = 'x'; path[1] = 0;
            assert(vyx_tbb_cgu_pool_submit(pool, unit, pool, path, 1, 0) == 0);
        }
        vyx_tbb_cgu_pool_finish(pool);
        assert(completed == 32);
        for (int unit = 1; unit < 33; ++unit) assert(vyx_tbb_cgu_pool_unit_ok(pool, unit));
        vyx_tbb_cgu_pool_free(pool);
    }
    std::cout << "CGU pool OK: 1/2/4 workers, 32 bounded asynchronous submits each\n";
}
