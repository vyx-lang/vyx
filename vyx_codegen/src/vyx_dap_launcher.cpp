#include "vyx_codegen.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <unistd.h>
#endif
#define VYX_RT_ABI VYX_API

// Delegate stdio directly: no duplicate payload, variable or target output cache.
extern "C" VYX_RT_ABI int32_t vyx_dap_run_adapter(const char* executable) {
    namespace fs = std::filesystem;
#ifdef _WIN32
    constexpr const char* adapter_name = "lldb-dap.exe";
    constexpr const char* codelldb_name = "codelldb.exe";
#else
    constexpr const char* adapter_name = "lldb-dap";
    constexpr const char* codelldb_name = "codelldb";
#endif
    std::vector<fs::path> candidates;
    const char* explicit_path = std::getenv("VYX_DAP_ADAPTER");
    if (!explicit_path || !*explicit_path) explicit_path = std::getenv("VYX_LLDB_DAP");
    if (explicit_path && *explicit_path) {
        candidates.emplace_back(fs::u8path(explicit_path));
    } else {
        std::error_code ec;
        fs::path exe = fs::absolute(fs::u8path(executable ? executable : ""), ec);
#ifdef _WIN32
        std::vector<wchar_t> module(32768);
        const DWORD n = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
        if (n && n < module.size()) exe = fs::path(module.data(), module.data() + n);
#endif
        candidates.push_back(exe.parent_path() / "debugger" / "adapter" / codelldb_name);
        candidates.push_back(exe.parent_path() / "_debug_adapter" / "adapter" / codelldb_name);
        candidates.push_back(exe.parent_path() / "clang" / "bin" / adapter_name);
        candidates.push_back(exe.parent_path() / adapter_name);
        if (const char* root = std::getenv("LLVM_ROOT"))
            candidates.push_back(fs::u8path(root) / "bin" / adapter_name);
        // Compatibility with older plugin configurations selecting the CLI.
        if (const char* cli = std::getenv("VYX_LLDB"))
            candidates.push_back(fs::u8path(cli).parent_path() / adapter_name);
        if (const char* raw_path = std::getenv("PATH")) {
            std::string path(raw_path);
#ifdef _WIN32
            constexpr char separator = ';';
#else
            constexpr char separator = ':';
#endif
            std::size_t begin = 0;
            while (begin <= path.size()) {
                auto end = path.find(separator, begin);
                if (end == std::string::npos) end = path.size();
                auto entry = path.substr(begin, end - begin);
                if (entry.size() >= 2 && entry.front() == '"' && entry.back() == '"')
                    entry = entry.substr(1, entry.size() - 2);
                if (!entry.empty()) candidates.push_back(fs::u8path(entry) / adapter_name);
                if (end == path.size()) break;
                begin = end + 1;
            }
        }
    }
    fs::path adapter;
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec)) { adapter = fs::absolute(candidate, ec); break; }
    }
    if (adapter.empty()) {
        std::fprintf(stderr, "error: debug adapter not found; use the complete Vyx SDK or set VYX_DAP_ADAPTER\n");
        return 2;
    }
#ifdef _WIN32
    // The SDK ships matching embeddable Python beside liblldb.
    if (adapter.stem() == "codelldb") {
        SetEnvironmentVariableW(L"PYTHONHOME", nullptr);
        SetEnvironmentVariableW(L"PYTHONPATH", nullptr);
    } else if (fs::is_regular_file(adapter.parent_path() / "python311.dll"))
        SetEnvironmentVariableW(L"PYTHONHOME", adapter.parent_path().c_str());
    const auto utf8 = adapter.u8string();
    const std::string command = "\"" + std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size()) + "\"";
    const int64_t child = vyx_bootstrap_process_spawn(command.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
    if (!child) {
        std::fprintf(stderr, "error: cannot start debug adapter\n");
        return 2;
    }
    const int32_t result = vyx_bootstrap_process_wait(child);
    vyx_bootstrap_process_close(child);
    return result;
#else
    if (adapter.stem() == "codelldb") { ::unsetenv("PYTHONHOME"); ::unsetenv("PYTHONPATH"); }
    // Replace the wrapper so EOF, signals and exit status belong to the adapter.
    std::string path = adapter.string();
    char* argv[] = {path.data(), nullptr};
    ::execv(argv[0], argv);
    std::fprintf(stderr, "error: cannot execute debug adapter (errno=%d)\n", errno);
    return 2;
#endif
}
