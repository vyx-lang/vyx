#pragma once

// POSIX child startup shared by the compiler runtime and its native regression.
// Keep allocation, environment construction and argv storage before fork().
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <paths.h>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include <csignal>
#if defined(__APPLE__)
#include <crt_externs.h>
#else
extern char** environ;
#endif

namespace vyx_bootstrap_posix {

struct SpawnError {
    const char* operation = nullptr;
    int number = 0;
};

// Keep internal descriptors away from stdin/stdout/stderr, even when the caller
// closed one of them. All descriptors must close on a successful exec.
inline bool private_fd(int& fd) {
    int result;
    do {
        result = fd < 3 ? ::fcntl(fd, F_DUPFD_CLOEXEC, 3)
                        : ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    } while (result < 0 && errno == EINTR);
    if (result < 0) { return false; }
    if (fd < 3) { ::close(fd); fd = result; }
    return true;
}

inline pid_t spawn(const std::vector<std::string>& argv, const std::string& cwd,
                   int out_fd, int err_fd, const char* env_name,
                   const char* env_value, SpawnError& error) {
    error = {};
    if (argv.empty() || argv[0].empty()) {
        error = {"argv", EINVAL}; return -1;
    }
    const bool override_env = env_name && env_name[0];
    if (override_env && std::strchr(env_name, '=')) {
        error = {"environment", EINVAL}; return -1;
    }
    std::vector<std::string> environment;
#if defined(__APPLE__)
    char** inherited = *_NSGetEnviron();
#else
    char** inherited = environ;
#endif
    const std::string prefix = override_env ? std::string(env_name) + "=" : "";
    for (char** entry = inherited; entry && *entry; ++entry) {
        if (!override_env || std::strncmp(*entry, prefix.c_str(), prefix.size()) != 0) {
            environment.emplace_back(*entry);
        }
    }
    if (override_env) { environment.emplace_back(prefix + (env_value ? env_value : "")); }
    std::vector<char*> envp;
    for (auto& entry : environment) { envp.push_back(entry.data()); }
    envp.push_back(nullptr);
    std::vector<char*> cargv;
    for (const auto& entry : argv) { cargv.push_back(const_cast<char*>(entry.c_str())); }
    cargv.push_back(nullptr);

    // Resolve PATH entries before fork, but leave relative paths relative to
    // the child's working directory, as execvp does. Preserve its script and
    // EACCES behavior without calling allocating libc helpers in the child.
    std::vector<std::string> paths;
    if (argv[0].find('/') != std::string::npos) {
        paths.push_back(argv[0]);
    } else {
        std::string search;
        bool has_path = false;
        for (const auto& entry : environment) {
            if (entry.compare(0, 5, "PATH=") == 0) {
                search = entry.substr(5); has_path = true; break;
            }
        }
        if (!has_path) {
#if defined(__ANDROID__)
            // bionic does not expose confstr; use its public exec default.
            search = _PATH_DEFPATH;
#else
            const auto count = ::confstr(_CS_PATH, nullptr, 0);
            if (count == 0) { error = {"default PATH", EINVAL}; return -1; }
            std::vector<char> buffer(count);
            if (::confstr(_CS_PATH, buffer.data(), count) != count) {
                error = {"default PATH", EINVAL}; return -1;
            }
            search = buffer.data();
#endif
        }
        std::size_t start = 0;
        for (;;) {
            const auto end = search.find(':', start);
            const auto directory = search.substr(start, end == std::string::npos ? end : end - start);
            paths.push_back(directory.empty() ? argv[0] : directory + "/" + argv[0]);
            if (end == std::string::npos) { break; }
            start = end + 1;
        }
    }
    const char* shell = "/bin/sh";
#if defined(__ANDROID__)
    // Match bionic on pre-28 devices as well as Termux's /bin/sh mapping.
    if (::access(shell, F_OK) != 0) { shell = "/system/bin/sh"; }
#endif
    std::vector<char*> shell_argv{const_cast<char*>(shell), nullptr};
    shell_argv.insert(shell_argv.end(), cargv.begin() + 1, cargv.end());

    enum Stage { group = 1, directory, standard_output, standard_error, execute };
    struct ChildError { int stage; int number; };
    int channel[2];
    // Serialize pipe/fcntl/fork so another invocation cannot inherit a pipe
    // before CLOEXEC is set (Darwin has no pipe2). Never lock in the child.
    static std::mutex launch_mutex;
    std::unique_lock<std::mutex> launch_lock(launch_mutex);
    if (::pipe(channel) != 0) { error = {"startup pipe", errno}; return -1; }
    if (!private_fd(channel[0]) || !private_fd(channel[1])) {
        const int number = errno;
        ::close(channel[0]); ::close(channel[1]);
        error = {"startup pipe CLOEXEC", number}; return -1;
    }
    const pid_t pid = ::fork();
    if (pid < 0) {
        const int number = errno;
        ::close(channel[0]); ::close(channel[1]);
        error = {"fork", number}; return -1;
    }
    if (pid == 0) {
        ::close(channel[0]);
        const auto fail = [&](Stage stage, int number) {
            const ChildError failure{stage, number};
            const char* data = reinterpret_cast<const char*>(&failure);
            std::size_t remaining = sizeof(failure);
            while (remaining) {
                const auto written = ::write(channel[1], data, remaining);
                if (written < 0 && errno == EINTR) { continue; }
                if (written <= 0) { break; }
                data += written; remaining -= static_cast<std::size_t>(written);
            }
            ::_exit(stage == execute ? 127 : 126);
        };
        int result;
        // One owner, one transition. Parent/child concurrent setpgid used to
        // make a child die with 126 before redirecting its diagnostic streams.
        do { result = ::setpgid(0, 0); } while (result != 0 && errno == EINTR);
        if (result != 0) { fail(group, errno); }
        if (!cwd.empty() && ::chdir(cwd.c_str()) != 0) { fail(directory, errno); }
        if (out_fd >= 0) {
            do { result = ::dup2(out_fd, STDOUT_FILENO); } while (result < 0 && errno == EINTR);
            if (result < 0) { fail(standard_output, errno); }
        }
        if (err_fd >= 0) {
            do { result = ::dup2(err_fd, STDERR_FILENO); } while (result < 0 && errno == EINTR);
            if (result < 0) { fail(standard_error, errno); }
        }
        if (out_fd >= 3) { ::close(out_fd); }
        if (err_fd >= 3 && err_fd != out_fd) { ::close(err_fd); }
        int execute_error = ENOENT;
        bool denied = false;
        for (const auto& path : paths) {
            ::execve(path.c_str(), cargv.data(), envp.data());
            const int number = errno;
            if (number == ENOEXEC) {
                shell_argv[1] = const_cast<char*>(path.c_str());
                ::execve(shell, shell_argv.data(), envp.data());
                fail(execute, errno);
            }
            if (number == EACCES) { denied = true; }
            else if (number != ENOENT && number != ENOTDIR) { fail(execute, number); }
            execute_error = number;
        }
        fail(execute, denied ? EACCES : execute_error);
    }
    ::close(channel[1]);
    launch_lock.unlock();
    ChildError failure{};
    std::size_t received = 0;
    int read_error = 0;
    while (received < sizeof(failure)) {
        const auto count = ::read(channel[0], reinterpret_cast<char*>(&failure) + received,
                                  sizeof(failure) - received);
        if (count < 0 && errno == EINTR) { continue; }
        if (count < 0) { read_error = errno; break; }
        if (count == 0) { break; }
        received += static_cast<std::size_t>(count);
    }
    ::close(channel[0]);
    if (received || read_error) {
        if (read_error || received != sizeof(failure)) {
            // Startup status is unknown: do not return an unverified group.
            (void)::kill(-pid, SIGKILL); (void)::kill(pid, SIGKILL);
            error = {"startup status", read_error ? read_error : EIO};
        } else {
            const char* operations[] = {"unknown", "setpgid", "chdir", "stdout dup2", "stderr dup2", "execve"};
            error = {failure.stage >= group && failure.stage <= execute
                         ? operations[failure.stage] : operations[0], failure.number};
        }
        int status;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        return -1;
    }
    // EOF means exec closed the status fd. The process group is ready before
    // the handle is published, including for immediate timeout cancellation.
    return pid;
}

} // namespace vyx_bootstrap_posix
