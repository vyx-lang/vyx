// Included after verbatim production runtime excerpts by run.ps1.
// The child is a real process: touch committed pages and observe the OS counters.
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

static void mark(const std::filesystem::path& path) {
    std::ofstream(path, std::ios::binary) << "ready\n";
}

static bool await_marker(const std::filesystem::path& path) {
    for (int i = 0; i < 3000; ++i) {
        if (std::filesystem::exists(path)) { return true; }
        Sleep(10);
    }
    return false;
}

int main(int argc, char** argv) {
    const bool child = argc == 3 && std::string(argv[1]) == "--child";
    const std::filesystem::path root(child ? argv[2] : argv[1]);
    constexpr SIZE_T allocation_size = 64u * 1024u * 1024u;
    if (child) {
        mark(root / "ready");
        if (!await_marker(root / "allocate")) { return 2; }
        auto* pages = static_cast<volatile unsigned char*>(
            VirtualAlloc(nullptr, allocation_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!pages) { return 3; }
        for (SIZE_T i = 0; i < allocation_size; i += 4096u) { pages[i] = 1; }
        mark(root / "allocated");
        if (!await_marker(root / "exit")) { return 4; }
        VirtualFree(const_cast<unsigned char*>(pages), 0, MEM_RELEASE);
        return 0;
    }

    if (vyx_bootstrap_process_resident_bytes(-1) != -1
        || vyx_bootstrap_process_peak_resident_bytes(-1) != -1) {
        std::fprintf(stderr, "invalid handle must be unknown\n");
        return 5;
    }
    std::string command = "\"" + std::string(argv[0]) + "\" --child \"" + root.string() + "\"";
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    auto proc = std::make_unique<VyxBootstrapProcess>();
    if (!CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &proc->pi)) {
        std::fprintf(stderr, "CreateProcess failed: %lu\n", GetLastError());
        return 6;
    }
    const HANDLE handle = proc->pi.hProcess;
    constexpr int64_t id = 1;
    {
        std::lock_guard<std::mutex> lock(vyx_bootstrap_process_mutex());
        vyx_bootstrap_processes().emplace(id, std::move(proc));
    }
    auto fail = [&](const char* message) {
        std::fprintf(stderr, "%s\n", message);
        TerminateProcess(handle, 1);
        WaitForSingleObject(handle, 5000);
        vyx_bootstrap_process_close(id);
        return 7;
    };
    if (!await_marker(root / "ready")) { return fail("child never became ready"); }
    const auto before = vyx_bootstrap_process_resident_bytes(id);
    mark(root / "allocate");
    if (!await_marker(root / "allocated")) { return fail("child never allocated"); }
    const auto after = vyx_bootstrap_process_resident_bytes(id);
    const auto peak = vyx_bootstrap_process_peak_resident_bytes(id);
    std::printf("before_bytes=%lld after_bytes=%lld peak_bytes=%lld\n",
                static_cast<long long>(before), static_cast<long long>(after),
                static_cast<long long>(peak));
    if (before <= 0 || after < before + 32ll * 1024ll * 1024ll || peak < after) {
        return fail("resident/peak observation did not reflect committed pages");
    }
    mark(root / "exit");
    if (WaitForSingleObject(handle, 10000) != WAIT_OBJECT_0) { return fail("child did not exit"); }
    if (vyx_bootstrap_process_is_running(id) != 0
        || vyx_bootstrap_process_resident_bytes(id) != 0
        || vyx_bootstrap_process_peak_resident_bytes(id) < peak) {
        return fail("finished handle lost peak or retained current usage");
    }
    // Query concurrently with close. Both operations must use the same mutex.
    std::atomic<bool> done{false};
    std::atomic<bool> wrong{false};
    std::thread reader([&]() {
        while (!done.load()) {
            const auto current = vyx_bootstrap_process_resident_bytes(id);
            const auto high = vyx_bootstrap_process_peak_resident_bytes(id);
            if (current != 0 && current != -1) { wrong.store(true); }
            if (high != -1 && high < peak) { wrong.store(true); }
        }
    });
    Sleep(10);
    vyx_bootstrap_process_close(id);
    Sleep(10);
    done.store(true);
    reader.join();
    if (wrong.load() || vyx_bootstrap_process_resident_bytes(id) != -1
        || vyx_bootstrap_process_peak_resident_bytes(id) != -1) {
        std::fprintf(stderr, "closed handle or concurrent close observation failed\n");
        return 8;
    }
    std::puts("pipeline_native_memory: OK");
    return 0;
}
