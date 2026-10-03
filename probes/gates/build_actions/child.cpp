#define NOMINMAX
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
static std::string quote(const std::string& value) { return "\"" + value + "\""; }
static void write(const fs::path& path, const std::string& value) {
    std::ofstream stream(path, std::ios::binary);
    stream << value;
}

int main(int argc, char** argv) {
    if (argc != 5) { return 60; }
    const std::string mode = argv[1];
    const std::string id = argv[2];
    const std::string output = argv[3];
    const DWORD delay = static_cast<DWORD>(std::stoul(argv[4]));
    if (mode == "descendant") {
        Sleep(delay);
        write(output, "complete " + id);
        write("end_" + id + ".time", std::to_string(GetTickCount64()));
        write("descendant_completed_" + id, "unexpected if batch failed");
        return 0;
    }
    write("root_" + id + ".pid", std::to_string(GetCurrentProcessId()));
    write("start_" + id + ".time", std::to_string(GetTickCount64()));
    if (mode == "success") {
        Sleep(delay);
        write(output, "complete " + id);
        write("end_" + id + ".time", std::to_string(GetTickCount64()));
        return 0;
    }
    if (mode != "tree" && mode != "failure" && mode != "tree_success") { return 61; }
    char executable[MAX_PATH]{};
    if (!GetModuleFileNameA(nullptr, executable, MAX_PATH)) { return 62; }
    const DWORD child_delay = mode == "tree_success" ? delay : 30000;
    std::string command = quote(executable) + " descendant " + id + " "
        + quote(output) + " " + std::to_string(child_delay);
    STARTUPINFOA start{};
    start.cb = sizeof(start);
    PROCESS_INFORMATION process{};
    if (!CreateProcessA(executable, command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &start, &process)) { return 63; }
    write("descendant_" + id + ".pid", std::to_string(process.dwProcessId));
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (mode == "failure") {
        Sleep(delay);
        write(output, "failed partial output");
        return 17;
    }
    // Deliberately exit before the descendant produces output. The real
    // scheduler must retain its CPU token and delay artifact publication.
    return 0;
}
