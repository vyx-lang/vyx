// Compile the production launcher directly; no copy of its implementation.
// Interpose only setpgid to exercise startup error reporting deterministically.
#include <cerrno>
#include <csignal>
#include <unistd.h>
static volatile sig_atomic_t group_fault = 0;
static int test_setpgid(pid_t pid, pid_t pgid) {
    if (group_fault == 1) { group_fault = 0; errno = EINTR; return -1; }
    if (group_fault == 2) { errno = EPERM; return -1; }
    return ::setpgid(pid, pgid);
}
#define setpgid test_setpgid
#include "bootstrap_process_posix.hpp"
#undef setpgid
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

namespace fs = std::filesystem;
using vyx_bootstrap_posix::spawn;
using vyx_bootstrap_posix::SpawnError;

static int wait_for(pid_t pid) {
    int status;
    pid_t result;
    do { result = ::waitpid(pid, &status, 0); } while (result < 0 && errno == EINTR);
    return result == pid && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int output(const fs::path& path) {
    int fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0600);
    if (fd >= 0 && !vyx_bootstrap_posix::private_fd(fd)) { ::close(fd); return -1; }
    return fd;
}

static std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "--fast") { return ::getpgrp() == ::getpid() ? 0 : 20; }
    if (mode == "--sleep") { for (;;) { ::pause(); } }
    if (mode == "--child") {
        if (::getpgrp() != ::getpid() || argc != 3 || std::string(argv[2]) != "a b : \" c") { return 21; }
        const char* value = std::getenv("VYX_STARTUP_TEST");
        if (!value || std::string(value) != "child only") { return 22; }
        std::printf("%s\n", fs::current_path().string().c_str());
        std::fprintf(stderr, "child stderr\n");
        return 7;
    }
    const auto self = fs::canonical(argv[0]);
    if (mode == "--closed-stdio") {
        ::close(0); ::close(1); ::close(2);
        SpawnError error;
        const pid_t pid = spawn({self.string(), "--fast"}, "", -1, -1, "", "", error);
        return pid > 0 && wait_for(pid) == 0 ? 0 : 23;
    }
    if (argc != 2) { return 2; }
    const auto root = fs::canonical(argv[1]);
    int passed = 0;
    auto check = [&](const char* name, bool valid) {
        std::printf("%s: %s\n", name, valid ? "PASS" : "FAIL");
        if (valid) { ++passed; }
    };
    SpawnError error;
    int out = output(root / "stdout");
    int err = output(root / "stderr");
    if (out < 0 || err < 0) { return 3; }
    ::setenv("VYX_STARTUP_TEST", "parent", 1);
    auto pid = spawn({self.string(), "--child", "a b : \" c"}, root.string(), out, err,
                     "VYX_STARTUP_TEST", "child only", error);
    ::close(out); ::close(err);
    check("argv-cwd-environment-redirection", pid > 0 && wait_for(pid) == 7
          && read(root / "stdout") == root.string() + "\n"
          && read(root / "stderr") == "child stderr\n"
          && std::string(std::getenv("VYX_STARTUP_TEST")) == "parent");

    fs::create_directories(root / "tools");
    fs::create_symlink(self, root / "tools/child");
    pid = spawn({"child", "--fast"}, root.string(), -1, -1, "PATH", "tools", error);
    check("PATH-override-relative-to-child-cwd", pid > 0 && wait_for(pid) == 0);

    std::ofstream(root / "script") << "printf shell-script; exit 7\n";
    fs::permissions(root / "script", fs::perms::owner_all);
    out = output(root / "script.stdout");
    pid = spawn({"./script"}, root.string(), out, -1, "", "", error);
    ::close(out);
    check("execvp-script-compatibility", pid > 0 && wait_for(pid) == 7
          && read(root / "script.stdout") == "shell-script");

    pid = spawn({"./missing"}, root.string(), -1, -1, "", "", error);
    check("missing-executable-is-startup-error", pid == -1 && error.number == ENOENT
          && std::string(error.operation) == "execve");
    std::ofstream(root / "tools/denied") << "not executable";
    fs::permissions(root / "tools/denied", fs::perms::owner_read);
    pid = spawn({"denied"}, root.string(), -1, -1, "PATH", "tools:missing", error);
    check("PATH-preserves-permission-error", pid == -1 && error.number == EACCES);
    pid = spawn({self.string(), "--fast"}, (root / "missing").string(), -1, -1, "", "", error);
    check("invalid-cwd-is-startup-error", pid == -1 && error.number == ENOENT
          && std::string(error.operation) == "chdir");
    pid = spawn({self.string(), "--fast"}, "", 1000000, -1, "", "", error);
    check("invalid-redirection-is-startup-error", pid == -1 && error.number == EBADF
          && std::string(error.operation) == "stdout dup2");
    pid = spawn({self.string(), "--fast"}, "", -1, -1, "INVALID=NAME", "x", error);
    check("invalid-environment-is-startup-error", pid == -1 && error.number == EINVAL);

    group_fault = 1;
    pid = spawn({self.string(), "--fast"}, "", -1, -1, "", "", error);
    group_fault = 0;
    check("interrupted-setpgid", pid > 0 && wait_for(pid) == 0);
    group_fault = 2;
    pid = spawn({self.string(), "--fast"}, "", -1, -1, "", "", error);
    group_fault = 0;
    check("setpgid-error-is-reported", pid == -1 && error.number == EPERM
          && std::string(error.operation) == "setpgid");

    pid = spawn({self.string(), "--sleep"}, "", -1, -1, "", "", error);
    bool group_ready = pid > 0 && ::getpgid(pid) == pid;
    int status = 0;
    if (pid > 0) {
        const int killed = ::kill(-pid, SIGKILL);
        const auto waited = ::waitpid(pid, &status, 0);
        group_ready = group_ready && killed == 0 && waited == pid
                      && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;
    }
    check("group-ready-for-immediate-cancellation", group_ready);
    pid = spawn({self.string(), "--closed-stdio"}, "", -1, -1, "", "", error);
    check("closed-standard-descriptors", pid > 0 && wait_for(pid) == 0);

    std::atomic<int> failed{0};
    std::vector<std::thread> threads;
    for (int thread = 0; thread < 4; ++thread) {
        threads.emplace_back([&] {
            for (int i = 0; i < 128; ++i) {
                SpawnError failure;
                const auto child = spawn({self.string(), "--fast"}, "", -1, -1, "", "", failure);
                if (child <= 0 || wait_for(child) != 0) { ++failed; }
            }
        });
    }
    for (auto& thread : threads) { thread.join(); }
    check("512-concurrent-child-startups", failed == 0);
    errno = 0;
    check("all-startup-failures-reaped", ::waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD);
    std::printf("%d/14 process startup checks passed\n", passed);
    return passed == 14 ? 0 : 1;
}
