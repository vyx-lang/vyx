param(
    [string]$RuntimePath = "",
    [string]$Clang = "",
    [switch]$CompileOnly
)

$ErrorActionPreference = "Stop"

$bootstrapRoot = Split-Path -Parent $PSScriptRoot
$repoRoot = Split-Path -Parent $bootstrapRoot
$isWindowsPlatform = if (Get-Variable -Name IsWindows -ErrorAction SilentlyContinue) {
    [bool]$IsWindows
} else {
    $env:OS -eq "Windows_NT"
}

if ([string]::IsNullOrWhiteSpace($RuntimePath)) {
    if ($isWindowsPlatform) {
        $RuntimePath = Join-Path $bootstrapRoot "out/vyx_compiler_backend.dll"
    } elseif ($IsMacOS) {
        $RuntimePath = Join-Path $bootstrapRoot "out/libvyx_compiler_backend.dylib"
    } else {
        $RuntimePath = Join-Path $bootstrapRoot "out/libvyx_compiler_backend.so"
    }
}
if ([string]::IsNullOrWhiteSpace($Clang)) {
    $clangName = if ($isWindowsPlatform) { "clang++.exe" } else { "clang++" }
    $Clang = Join-Path $repoRoot ("clang/bin/" + $clangName)
}

$RuntimePath = [System.IO.Path]::GetFullPath($RuntimePath)
$Clang = [System.IO.Path]::GetFullPath($Clang)
if (-not (Test-Path -LiteralPath $RuntimePath -PathType Leaf)) {
    throw "compiler backend is missing: $RuntimePath"
}
if (-not (Test-Path -LiteralPath $Clang -PathType Leaf)) {
    throw "clang++ is missing: $Clang"
}

$workRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("vyx-process-cancel-" + [Guid]::NewGuid().ToString("N"))
[void][System.IO.Directory]::CreateDirectory($workRoot)
$helperSource = Join-Path $workRoot "process_tree_helper.cpp"
$harnessSource = Join-Path $workRoot "process_cancel_harness.cpp"
$exeSuffix = if ($isWindowsPlatform) { ".exe" } else { "" }
$helperExe = Join-Path $workRoot ("process_tree_helper" + $exeSuffix)
$harnessExe = Join-Path $workRoot ("process_cancel_harness" + $exeSuffix)
$passed = $false

try {
    @'
#include <chrono>
#include <fstream>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/types.h>
#include <unistd.h>
#endif

static int spawn_leaf(const char* self, const char* marker) {
#ifdef _WIN32
    std::string command = std::string("\"") + self + "\" --leaf \"" + marker + "\"";
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        return 1;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 0;
#else
    const pid_t pid = fork();
    if (pid < 0) { return 1; }
    if (pid == 0) {
        execl(self, self, "--leaf", marker, static_cast<char*>(nullptr));
        _exit(127);
    }
    return 0;
#endif
}

int main(int argc, char** argv) {
    if (argc != 3) { return 64; }
    const std::string mode = argv[1];
    const char* marker = argv[2];
    if (mode == "--leaf" || mode == "--self-marker") {
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        std::ofstream out(marker, std::ios::binary | std::ios::trunc);
        out << "written\n";
        return out ? 0 : 1;
    }
    if (mode == "--root-wait" || mode == "--root-exit") {
        if (spawn_leaf(argv[0], marker) != 0) { return 2; }
        if (mode == "--root-exit") { return 0; }
        std::this_thread::sleep_for(std::chrono::seconds(30));
        return 0;
    }
    return 65;
}
'@ | Set-Content -LiteralPath $helperSource -Encoding ascii

    @'
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

using SpawnFn = int64_t (*)(const char*, const char*, const char*, const char*, const char*, const char*);
using WaitFn = int32_t (*)(int64_t);
using CancelFn = int32_t (*)(int64_t);
using CloseFn = void (*)(int64_t);

static std::string quote(const std::string& value) {
    return "\"" + value + "\"";
}

int main(int argc, char** argv) {
    if (argc != 4) { return 64; }
#ifdef _WIN32
    HMODULE library = LoadLibraryA(argv[1]);
    if (!library) { return 65; }
    auto symbol = [library](const char* name) -> void* {
        return reinterpret_cast<void*>(GetProcAddress(library, name));
    };
#else
    void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        std::cerr << dlerror() << "\n";
        return 65;
    }
    auto symbol = [library](const char* name) -> void* { return dlsym(library, name); };
#endif
    const auto spawn = reinterpret_cast<SpawnFn>(symbol("vyx_bootstrap_process_spawn"));
    const auto wait_process = reinterpret_cast<WaitFn>(symbol("vyx_bootstrap_process_wait"));
    const auto cancel = reinterpret_cast<CancelFn>(symbol("vyx_bootstrap_process_cancel"));
    const auto close_process = reinterpret_cast<CloseFn>(symbol("vyx_bootstrap_process_close"));
    if (!spawn || !wait_process || !cancel || !close_process) { return 66; }

#ifdef _WIN32
    const char* original_disable_value = std::getenv("VYX_BOOTSTRAP_DISABLE_JOB_OBJECT");
    const bool had_disable_value = original_disable_value != nullptr;
    const std::string saved_disable_value = had_disable_value ? original_disable_value : "";
    SetEnvironmentVariableA("VYX_BOOTSTRAP_DISABLE_JOB_OBJECT", nullptr);
#endif

    const std::string helper = argv[2];
    const std::filesystem::path root = argv[3];
    const auto run = [&](const char* mode, const std::filesystem::path& marker) {
        const std::string command = quote(helper) + " " + mode + " " + quote(marker.string());
        return spawn(command.c_str(), "", "", "", "", "");
    };

    const auto cancelled_marker = root / "cancelled.marker";
    std::filesystem::remove(cancelled_marker);
    int64_t id = run("--root-wait", cancelled_marker);
    if (id <= 0) { return 67; }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    if (cancel(id) != 0) { return 68; }
    close_process(id);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    if (std::filesystem::exists(cancelled_marker)) { return 69; }

    const auto completed_marker = root / "completed.marker";
    std::filesystem::remove(completed_marker);
    id = run("--root-exit", completed_marker);
    if (id <= 0 || wait_process(id) != 0) { return 70; }
    close_process(id);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    if (!std::filesystem::exists(completed_marker)) { return 71; }

#ifdef _WIN32
    const auto fallback_marker = root / "fallback.marker";
    std::filesystem::remove(fallback_marker);
    SetEnvironmentVariableA("VYX_BOOTSTRAP_DISABLE_JOB_OBJECT", "1");
    id = run("--self-marker", fallback_marker);
    if (id <= 0) { return 72; }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    if (cancel(id) != 0) { return 73; }
    close_process(id);
    SetEnvironmentVariableA("VYX_BOOTSTRAP_DISABLE_JOB_OBJECT",
                            had_disable_value ? saved_disable_value.c_str() : nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    if (std::filesystem::exists(fallback_marker)) { return 74; }
#endif

    std::cout << "process_cancel OK\n";
    return 0;
}
'@ | Set-Content -LiteralPath $harnessSource -Encoding ascii

    & $Clang -std=c++17 -O0 $helperSource -o $helperExe
    if ($LASTEXITCODE -ne 0) { throw "helper compilation failed: exit=$LASTEXITCODE" }
    $harnessArgs = @("-std=c++17", "-O0", $harnessSource, "-o", $harnessExe)
    if (-not $isWindowsPlatform -and -not $IsMacOS) { $harnessArgs += "-ldl" }
    & $Clang @harnessArgs
    if ($LASTEXITCODE -ne 0) { throw "harness compilation failed: exit=$LASTEXITCODE" }

    if ($CompileOnly) {
        $passed = $true
        Write-Host "process_cancel compile PASS"
        return
    }

    & $harnessExe $RuntimePath $helperExe $workRoot
    if ($LASTEXITCODE -ne 0) { throw "process cancellation regression failed: exit=$LASTEXITCODE work_root=$workRoot" }
    $passed = $true
    Write-Host "process_cancel PASS runtime=$RuntimePath"
} finally {
    if ($passed -and (Test-Path -LiteralPath $workRoot)) {
        Remove-Item -LiteralPath $workRoot -Recurse -Force
    } elseif (Test-Path -LiteralPath $workRoot) {
        Write-Host "process_cancel retained_work_root=$workRoot"
    }
}
