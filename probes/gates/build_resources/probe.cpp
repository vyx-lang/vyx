#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
constexpr int64_t MiB = 1024 * 1024;
static std::string quote(const std::string& s) { return "\"" + s + "\""; }
static void mark(const fs::path& p) { std::ofstream(p) << "ready"; }
template <class Predicate> static bool until(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) { return false; }
        Sleep(10);
    }
    return true;
}
static bool wait_mark(const fs::path& p) { return until([&] { return fs::exists(p); }); }
static void burn_cpu(unsigned millis) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(millis);
    uint64_t value = 13;
    while (std::chrono::steady_clock::now() < end) {
        for (unsigned i = 0; i < 50000; ++i) { value = value * 1664525 + 1013904223; }
        std::atomic_signal_fence(std::memory_order_seq_cst);
    }
    std::ofstream("NUL") << value;
}
static void* allocate(unsigned size_mb) {
    auto* data = static_cast<unsigned char*>(VirtualAlloc(nullptr, size_mb * MiB,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (data) {
        for (int64_t i = 0; i < size_mb * MiB; i += 4096) { data[i] = 42; }
    }
    return data;
}
static int child_main(const std::string& mode, const fs::path& dir, const std::string& exe) {
    if (mode == "--descendant") {
        if (!allocate(64)) { return 40; }
        burn_cpu(500);
        mark(dir / "descendant_ready");
        return wait_mark(dir / "descendant_exit") ? 0 : 41;
    }
    if (!allocate(16)) { return 42; }
    mark(dir / "root_ready");
    if (!wait_mark(dir / "root_grow")) { return 43; }
    if (!allocate(32)) { return 44; }
    burn_cpu(250);
    if (mode == "--root") {
        std::string command = quote(exe) + " --descendant " + quote(dir.string());
        STARTUPINFOA si{}; si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessA(exe.c_str(), command.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) { return 45; }
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        if (!wait_mark(dir / "descendant_ready")) { return 46; }
    }
    mark(dir / "grown_ready");
    return wait_mark(dir / "root_exit") ? 0 : 47;
}

using Query = int64_t (*)(int64_t);
struct Api {
    int64_t (*spawn)(const char*, const char*, const char*, const char*, const char*, const char*);
    int32_t (*running)(int64_t);
    int32_t (*wait)(int64_t);
    int32_t (*cancel)(int64_t);
    void (*close)(int64_t);
    int64_t (*commit)();
    int64_t (*physical)();
    Query rss, peak_rss, bytes, peak, cpu, tree_bytes, tree_peak, tree_cpu, tree_active;
};
template <class T> static bool load(HMODULE dll, T& fn, const char* name) {
    fn = reinterpret_cast<T>(GetProcAddress(dll, name));
    if (!fn) { std::fprintf(stderr, "missing export %s\n", name); }
    return fn != nullptr;
}
static bool load_api(HMODULE dll, Api& api) {
    return load(dll, api.spawn, "vyx_bootstrap_process_spawn")
        && load(dll, api.running, "vyx_bootstrap_process_is_running")
        && load(dll, api.wait, "vyx_bootstrap_process_wait")
        && load(dll, api.cancel, "vyx_bootstrap_process_cancel")
        && load(dll, api.close, "vyx_bootstrap_process_close")
        && load(dll, api.commit, "vyx_rt_system_available_commit_bytes")
        && load(dll, api.physical, "vyx_rt_system_available_physical_bytes")
        && load(dll, api.rss, "vyx_bootstrap_process_resident_bytes")
        && load(dll, api.peak_rss, "vyx_bootstrap_process_peak_resident_bytes")
        && load(dll, api.bytes, "vyx_bootstrap_process_private_bytes")
        && load(dll, api.peak, "vyx_bootstrap_process_peak_private_bytes")
        && load(dll, api.cpu, "vyx_bootstrap_process_cpu_time_us")
        && load(dll, api.tree_bytes, "vyx_bootstrap_process_tree_private_bytes")
        && load(dll, api.tree_peak, "vyx_bootstrap_process_tree_peak_private_bytes")
        && load(dll, api.tree_cpu, "vyx_bootstrap_process_tree_cpu_time_us")
        && load(dll, api.tree_active, "vyx_bootstrap_process_tree_active_count");
}
struct ManagedChild {
    Api& api;
    int64_t id;
    ~ManagedChild() {
        if (id > 0) { (void)api.cancel(id); api.close(id); }
    }
};
#define REQUIRE(condition, code) do { if (!(condition)) { \
    std::fprintf(stderr, "FAIL %d: %s\n", code, #condition); return code; } } while (false)

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]).rfind("--", 0) == 0) {
        return child_main(argv[1], argv[2], fs::absolute(argv[0]).string());
    }
    if (argc != 3 && argc != 4) { return 2; }
    const bool no_job = argc == 4 && std::string(argv[3]) == "--no-job";
    const HMODULE dll = LoadLibraryA(argv[1]);
    REQUIRE(dll, 3);
    Api api{};
    REQUIRE(load_api(dll, api), 4);
    const std::vector<Query> queries{api.rss, api.peak_rss, api.bytes, api.peak, api.cpu,
        api.tree_bytes, api.tree_peak, api.tree_cpu, api.tree_active};
    for (auto query : queries) { REQUIRE(query(-1) == -1 && query(0) == -1, 5); }
    REQUIRE(api.commit() > 0 && api.physical() > 0, 6);

    const fs::path dir = fs::path(argv[2]) / (no_job ? "direct" : "tree");
    fs::create_directories(dir);
    const std::string command = quote(fs::absolute(argv[0]).string())
        + (no_job ? " --single " : " --root ") + quote(dir.string());
    const std::string log = (dir / "child.log").string();
    ManagedChild child{api, api.spawn(command.c_str(), "", log.c_str(), log.c_str(), "", "")};
    REQUIRE(child.id > 0, 7);
    REQUIRE(wait_mark(dir / "root_ready"), 8);
    const auto initial = api.bytes(child.id);
    const auto initial_rss = api.rss(child.id);
    REQUIRE(initial >= 16 * MiB && initial_rss >= 16 * MiB, 9);
    mark(dir / "root_grow");
    REQUIRE(wait_mark(dir / "grown_ready"), 10);
    const auto grown = api.bytes(child.id);
    const auto direct_peak = api.peak(child.id);
    const auto cpu = api.cpu(child.id);
    REQUIRE(grown >= initial + 30 * MiB, 11);
    REQUIRE(direct_peak >= grown && cpu >= 50000, 12);
    const auto tree_bytes = api.tree_bytes(child.id);
    const auto tree_peak = api.tree_peak(child.id);
    const auto tree_cpu = api.tree_cpu(child.id);
    std::printf("mode=%s system_commit=%lld physical=%lld initial=%lld grown=%lld "
                "direct_peak=%lld direct_cpu_us=%lld tree=%lld tree_peak=%lld tree_cpu_us=%lld\n",
                no_job ? "direct" : "tree", api.commit(), api.physical(), initial, grown,
                direct_peak, cpu, tree_bytes, tree_peak, tree_cpu);
    if (no_job) {
        REQUIRE(tree_bytes == -1 && tree_peak == -1 && tree_cpu == -1
                && api.tree_active(child.id) == -1, 13);
    } else {
        std::printf("initial_tree_active=%lld\n", api.tree_active(child.id));
        // Windows may also put console-host helper processes in the job.
        REQUIRE(api.tree_active(child.id) >= 2, 25);
        REQUIRE(tree_bytes >= grown + 60 * MiB, 14);
        REQUIRE(tree_peak >= tree_bytes && tree_cpu >= cpu + 100000, 15);
    }

    mark(dir / "root_exit");
    REQUIRE(until([&] { return !api.running(child.id); }), 16);
    REQUIRE(api.wait(child.id) == 0, 17);
    REQUIRE(api.bytes(child.id) == 0 && api.rss(child.id) == 0, 18);
    REQUIRE(api.peak(child.id) >= direct_peak && api.cpu(child.id) >= cpu, 19);
    if (!no_job) {
        const auto orphan_current = api.tree_bytes(child.id);
        REQUIRE(api.tree_active(child.id) >= 1, 26);
        REQUIRE(orphan_current >= 64 * MiB, 20);
        REQUIRE(api.tree_peak(child.id) >= tree_peak && api.tree_cpu(child.id) >= tree_cpu, 21);
        std::printf("root_finished=1 descendant_current=%lld direct_current=%lld\n",
                    orphan_current, api.bytes(child.id));
        mark(dir / "descendant_exit");
        REQUIRE(until([&] { return api.tree_active(child.id) == 0; }), 22);
        const auto final_current = api.tree_bytes(child.id);
        std::printf("tree_active=0 after_descendant_exit_tree_current=%lld\n", final_current);
        // Windows can retain a small commit charge for terminated process/job
        // bookkeeping while handles remain open. Report it honestly; lifecycle
        // completion is ActiveProcesses == 0, never a synthetic memory zero.
        REQUIRE(final_current >= 0 && final_current < orphan_current - 60 * MiB, 27);
        REQUIRE(api.tree_peak(child.id) >= tree_peak && api.tree_cpu(child.id) >= tree_cpu, 23);
        const auto final_peak = api.tree_peak(child.id);
        const auto final_cpu = api.tree_cpu(child.id);
        for (unsigned i = 0; i < 64; ++i) {
            REQUIRE(api.tree_peak(child.id) == final_peak && api.tree_cpu(child.id) == final_cpu, 28);
        }
    }

    std::atomic<bool> stop{false};
    std::atomic<unsigned> read_rounds{0};
    const auto closed_id = child.id;
    std::thread racing_reader([&] {
        while (!stop.load()) {
            for (auto query : queries) { (void)query(closed_id); }
            ++read_rounds;
        }
    });
    while (read_rounds.load() == 0) { std::this_thread::yield(); }
    api.close(child.id);
    child.id = 0;
    stop.store(true);
    racing_reader.join();
    for (auto query : queries) { REQUIRE(query(closed_id) == -1, 24); }
    std::puts("build resource observations OK");
    return 0;
}
