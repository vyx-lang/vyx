#include "vyx_codegen.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"
#endif

namespace {

int copy_out(char* buf, int32_t cap, const std::string& text) {
    if (!buf || cap <= 0) {
        return -1;
    }
    const int32_t n = static_cast<int32_t>(text.size());
    const int32_t use = n < cap - 1 ? n : cap - 1;
    std::memcpy(buf, text.data(), static_cast<size_t>(use));
    buf[use] = 0;
    return use;
}

#ifndef _WIN32

struct LineEntry {
    std::string file;
    int32_t line = 0;
    uint64_t addr = 0;
};

struct Breakpoint {
    std::string file;
    int32_t line = 0;
    uint64_t addr = 0;
    long orig = 0;
    bool planted = false;
};

struct Engine {
    pid_t pid = -1;
    std::string exe;
    std::vector<LineEntry> lines;
    std::vector<Breakpoint> breaks;
    bool running = false;
    bool stopped = false;
    std::string last_reason;
};

Engine g_eng;

std::string basename_of(const std::string& path) {
    auto pos = path.find_last_of("/\\");
    if (pos == std::string::npos) {
        return path;
    }
    return path.substr(pos + 1);
}

bool file_matches(const std::string& want, const std::string& have) {
    if (want.empty() || have.empty()) {
        return false;
    }
    if (want == have) {
        return true;
    }
    return basename_of(want) == basename_of(have);
}

int load_dwarf(const char* exe) {
    g_eng.lines.clear();
    auto buf_or = llvm::MemoryBuffer::getFile(exe);
    if (!buf_or) {
        return -1;
    }
    auto obj_or = llvm::object::ObjectFile::createObjectFile((*buf_or)->getMemBufferRef());
    if (!obj_or) {
        llvm::consumeError(obj_or.takeError());
        return -1;
    }
    auto ctx = llvm::DWARFContext::create(**obj_or);
    if (!ctx) {
        return -1;
    }
    for (const auto& cu : ctx->compile_units()) {
        if (!cu) {
            continue;
        }
        const auto* table = ctx->getLineTableForUnit(cu.get());
        if (!table) {
            continue;
        }
        for (const auto& row : table->Rows) {
            if (!row.IsStmt || row.Address.Address == 0 || row.Line == 0) {
                continue;
            }
            std::string file;
            if (table->getFileNameByIndex(
                    row.File,
                    cu->getCompilationDir(),
                    llvm::DILineInfoSpecifier::FileLineInfoKind::AbsoluteFilePath,
                    file)) {
                LineEntry e;
                e.file = file;
                e.line = static_cast<int32_t>(row.Line);
                e.addr = row.Address.Address;
                g_eng.lines.push_back(e);
            }
        }
    }
    return g_eng.lines.empty() ? -1 : 0;
}

uint64_t addr_for(const std::string& file, int32_t line) {
    uint64_t best = 0;
    for (const auto& e : g_eng.lines) {
        if (e.line == line && file_matches(file, e.file)) {
            return e.addr;
        }
        if (best == 0 && e.line >= line && file_matches(file, e.file)) {
            best = e.addr;
        }
    }
    return best;
}

int plant(Breakpoint& bp) {
    if (g_eng.pid <= 0 || bp.addr == 0) {
        return -1;
    }
    errno = 0;
    long word = ptrace(PTRACE_PEEKTEXT, g_eng.pid, reinterpret_cast<void*>(bp.addr), nullptr);
    if (word == -1 && errno != 0) {
        return -1;
    }
    bp.orig = word;
    long patched = (word & ~0xffL) | 0xccL;
    if (ptrace(PTRACE_POKETEXT, g_eng.pid, reinterpret_cast<void*>(bp.addr),
               reinterpret_cast<void*>(patched)) != 0) {
        return -1;
    }
    bp.planted = true;
    return 0;
}

int restore(Breakpoint& bp) {
    if (!bp.planted || g_eng.pid <= 0) {
        return 0;
    }
    if (ptrace(PTRACE_POKETEXT, g_eng.pid, reinterpret_cast<void*>(bp.addr),
               reinterpret_cast<void*>(bp.orig)) != 0) {
        return -1;
    }
    bp.planted = false;
    return 0;
}

int read_regs(user_regs_struct& regs) {
    return ptrace(PTRACE_GETREGS, g_eng.pid, nullptr, &regs);
}

int write_regs(const user_regs_struct& regs) {
    return ptrace(PTRACE_SETREGS, g_eng.pid, nullptr, const_cast<user_regs_struct*>(&regs));
}

Breakpoint* hit_at(uint64_t rip) {
    for (auto& bp : g_eng.breaks) {
        if (bp.planted && (bp.addr == rip || bp.addr + 1 == rip)) {
            return &bp;
        }
    }
    return nullptr;
}

int wait_once(std::string& reason) {
    int status = 0;
    if (waitpid(g_eng.pid, &status, 0) < 0) {
        reason = "error";
        return -1;
    }
    if (WIFEXITED(status) || WIFSIGNALED(status)) {
        g_eng.running = false;
        g_eng.stopped = false;
        reason = "exited";
        return 0;
    }
    if (!WIFSTOPPED(status)) {
        reason = "error";
        return -1;
    }
    user_regs_struct regs{};
    if (read_regs(regs) != 0) {
        reason = "error";
        return -1;
    }
    uint64_t rip = static_cast<uint64_t>(regs.rip);
    Breakpoint* bp = hit_at(rip);
    if (bp == nullptr) {
        bp = hit_at(rip - 1);
    }
    if (bp != nullptr) {
        restore(*bp);
        if (static_cast<uint64_t>(regs.rip) != bp->addr) {
            regs.rip = static_cast<unsigned long>(bp->addr);
            write_regs(regs);
        }
        reason = "breakpoint";
        ptrace(PTRACE_SINGLESTEP, g_eng.pid, nullptr, nullptr);
        int st2 = 0;
        waitpid(g_eng.pid, &st2, 0);
        plant(*bp);
    } else {
        reason = "step";
    }
    g_eng.stopped = true;
    return 0;
}

#endif

}  // namespace

extern "C" VYX_API int32_t vyx_dap_engine_available(void) {
#ifdef _WIN32
    return 0;
#else
    return 1;
#endif
}

extern "C" VYX_API int32_t vyx_dap_launch(const char* exe, const char* cwd) {
#ifdef _WIN32
    (void)exe;
    (void)cwd;
    return -1;
#else
    vyx_dap_disconnect();
    if (!exe || !exe[0]) {
        return -1;
    }
    g_eng.exe = exe;
    if (load_dwarf(exe) != 0) {
        // DWARF is optional for launch; breakpoints may still fail to verify.
    }
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        if (cwd && cwd[0]) {
            if (chdir(cwd) != 0) {
                _exit(127);
            }
        }
        ptrace(PTRACE_TRACEME, 0, nullptr, nullptr);
        execl(exe, exe, nullptr);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return -1;
    }
    g_eng.pid = pid;
    g_eng.running = true;
    g_eng.stopped = true;
    g_eng.last_reason = "entry";
    ptrace(PTRACE_SETOPTIONS, pid, nullptr, PTRACE_O_EXITKILL);
    return 0;
#endif
}

extern "C" VYX_API int32_t vyx_dap_set_breakpoint(const char* file, int32_t line) {
#ifdef _WIN32
    (void)file;
    (void)line;
    return 0;
#else
    if (!file || line <= 0) {
        return 0;
    }
    Breakpoint bp;
    bp.file = file;
    bp.line = line;
    bp.addr = addr_for(file, line);
    if (bp.addr != 0 && g_eng.pid > 0) {
        plant(bp);
    }
    g_eng.breaks.push_back(bp);
    return bp.planted || bp.addr != 0 ? 1 : 0;
#endif
}

extern "C" VYX_API int32_t vyx_dap_clear_breakpoints(void) {
#ifdef _WIN32
    return 0;
#else
    for (auto& bp : g_eng.breaks) {
        restore(bp);
    }
    g_eng.breaks.clear();
    return 0;
#endif
}

extern "C" VYX_API int32_t vyx_dap_continue(void) {
#ifdef _WIN32
    return -1;
#else
    if (g_eng.pid <= 0) {
        return -1;
    }
    g_eng.stopped = false;
    return ptrace(PTRACE_CONT, g_eng.pid, nullptr, nullptr);
#endif
}

extern "C" VYX_API int32_t vyx_dap_step_over(void) {
#ifdef _WIN32
    return -1;
#else
    if (g_eng.pid <= 0) {
        return -1;
    }
    g_eng.stopped = false;
    return ptrace(PTRACE_SINGLESTEP, g_eng.pid, nullptr, nullptr);
#endif
}

extern "C" VYX_API int32_t vyx_dap_step_in(void) {
    return vyx_dap_step_over();
}

extern "C" VYX_API int32_t vyx_dap_step_out(void) {
    return vyx_dap_continue();
}

extern "C" VYX_API int32_t vyx_dap_pause(void) {
#ifdef _WIN32
    return -1;
#else
    if (g_eng.pid <= 0) {
        return -1;
    }
    return kill(g_eng.pid, SIGSTOP);
#endif
}

extern "C" VYX_API int32_t vyx_dap_wait_stop(char* reason, int32_t cap) {
#ifdef _WIN32
    (void)reason;
    (void)cap;
    return -1;
#else
    std::string why;
    if (wait_once(why) != 0) {
        return copy_out(reason, cap, "error");
    }
    g_eng.last_reason = why;
    return copy_out(reason, cap, why);
#endif
}

extern "C" VYX_API int32_t vyx_dap_stack_json(char* buf, int32_t cap) {
#ifdef _WIN32
    return copy_out(buf, cap, "{\"stackFrames\":[],\"totalFrames\":0}");
#else
    if (g_eng.pid <= 0) {
        return copy_out(buf, cap, "{\"stackFrames\":[],\"totalFrames\":0}");
    }
    user_regs_struct regs{};
    if (read_regs(regs) != 0) {
        return copy_out(buf, cap, "{\"stackFrames\":[],\"totalFrames\":0}");
    }
    std::string file = g_eng.exe;
    int32_t line = 1;
    uint64_t rip = static_cast<uint64_t>(regs.rip);
    for (const auto& e : g_eng.lines) {
        if (e.addr <= rip) {
            file = e.file;
            line = e.line;
        }
    }
    std::string json = "{\"stackFrames\":[{\"id\":0,\"name\":\"main\",\"line\":";
    json += std::to_string(line);
    json += ",\"column\":1,\"source\":{\"path\":\"";
    for (char c : file) {
        if (c == '\\' || c == '"') {
            json.push_back('\\');
        }
        json.push_back(c);
    }
    json += "\"}}],\"totalFrames\":1}";
    return copy_out(buf, cap, json);
#endif
}

extern "C" VYX_API int32_t vyx_dap_vars_json(int32_t frame, char* buf, int32_t cap) {
    (void)frame;
    return copy_out(buf, cap, "[]");
}

extern "C" VYX_API int32_t vyx_dap_evaluate(const char* expr, char* buf, int32_t cap) {
    if (!expr) {
        return copy_out(buf, cap, "");
    }
    return copy_out(buf, cap, expr);
}

extern "C" VYX_API void vyx_dap_disconnect(void) {
#ifdef _WIN32
    return;
#else
    if (g_eng.pid > 0) {
        kill(g_eng.pid, SIGKILL);
        int status = 0;
        waitpid(g_eng.pid, &status, 0);
    }
    g_eng.pid = -1;
    g_eng.running = false;
    g_eng.stopped = false;
    g_eng.breaks.clear();
    g_eng.lines.clear();
    g_eng.exe.clear();
#endif
}
