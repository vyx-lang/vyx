#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return 3;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    limits.ProcessMemoryLimit = 256ull * 1024 * 1024;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) return 4;
    std::string command = std::string("\"") + argv[1] + "\" " + argv[2];
    std::vector<char> buffer(command.begin(), command.end());
    buffer.push_back('\0');
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION child{};
    if (!CreateProcessA(nullptr, buffer.data(), nullptr, nullptr, TRUE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child)) return 5;
    if (!AssignProcessToJobObject(job, child.hProcess)) {
        TerminateProcess(child.hProcess, 6);
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
        CloseHandle(job);
        return 6;
    }
    ResumeThread(child.hThread);
    CloseHandle(child.hThread);
    if (WaitForSingleObject(child.hProcess, 30000) != WAIT_OBJECT_0) {
        TerminateProcess(child.hProcess, 7);
        CloseHandle(child.hProcess);
        CloseHandle(job);
        return 7;
    }
    DWORD status = 0;
    GetExitCodeProcess(child.hProcess, &status);
    CloseHandle(child.hProcess);
    CloseHandle(job);
    std::printf("allocation failure child exit=%lu (expected 1)\n", status);
    return status == 1 ? 0 : 8;
}
