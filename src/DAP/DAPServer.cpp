#include "DAPServer.h"
#include <iostream>
#include <algorithm>
#include <cstring>
#include <filesystem>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#endif

namespace fs = std::filesystem;

namespace vyx {
namespace dap {

DAPServer::DAPServer() {
#ifdef _WIN32
    debuggerProcess_ = nullptr;
    debuggerStdinWrite_ = nullptr;
    debuggerStdoutRead_ = nullptr;
#endif
}

// ============================================================
//  JSON helpers
// ============================================================

std::string DAPServer::jsonString(const std::string& s) {
    std::string escaped;
    escaped.reserve(s.size() + 2);
    escaped += '"';
    for (char c : s) {
        switch (c) {
            case '"':  escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:   escaped += c; break;
        }
    }
    escaped += '"';
    return escaped;
}

std::string DAPServer::jsonInt(int n) { return std::to_string(n); }
std::string DAPServer::jsonBool(bool b) { return b ? "true" : "false"; }

std::string DAPServer::extractString(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return "";
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return "";
    auto end = json.find('"', pos + 1);
    while (end != std::string::npos && json[end - 1] == '\\') end = json.find('"', end + 1);
    if (end == std::string::npos) return "";
    return json.substr(pos + 1, end - pos - 1);
}

int DAPServer::extractInt(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return 0;
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return 0;
    ++pos;
    while (pos < json.size() && json[pos] == ' ') ++pos;
    std::string num;
    while (pos < json.size() && (isdigit(json[pos]) || json[pos] == '-')) {
        num += json[pos++];
    }
    return num.empty() ? 0 : std::stoi(num);
}

// ============================================================
//  DAP message I/O (Content-Length header protocol)
// ============================================================

std::string DAPServer::readMessage() {
    std::string header;
    while (true) {
        int c = std::cin.get();
        if (c == EOF) return "";
        header += static_cast<char>(c);
        if (header.size() >= 4 && header.substr(header.size() - 4) == "\r\n\r\n") break;
    }

    int contentLength = 0;
    auto pos = header.find("Content-Length: ");
    if (pos != std::string::npos) {
        contentLength = std::stoi(header.substr(pos + 16));
    }
    if (contentLength <= 0) return "";

    std::string body(contentLength, '\0');
    std::cin.read(body.data(), contentLength);
    return body;
}

void DAPServer::sendEvent(const std::string& event, const std::string& bodyJson) {
    std::string msg = "{\"seq\":" + jsonInt(seq_++) +
        ",\"type\":\"event\",\"event\":" + jsonString(event);
    if (!bodyJson.empty()) msg += ",\"body\":" + bodyJson;
    msg += "}";

    std::string header = "Content-Length: " + std::to_string(msg.size()) + "\r\n\r\n";
    std::cout << header << msg;
    std::cout.flush();
}

void DAPServer::sendResponse(int seq, int requestSeq, const std::string& command,
                              bool success, const std::string& bodyJson) {
    std::string msg = "{\"seq\":" + jsonInt(seq_++) +
        ",\"type\":\"response\",\"request_seq\":" + jsonInt(requestSeq) +
        ",\"success\":" + jsonBool(success) +
        ",\"command\":" + jsonString(command);
    if (!bodyJson.empty()) msg += ",\"body\":" + bodyJson;
    msg += "}";

    std::string header = "Content-Length: " + std::to_string(msg.size()) + "\r\n\r\n";
    std::cout << header << msg;
    std::cout.flush();
}

void DAPServer::sendErrorResponse(int seq, int requestSeq, const std::string& command,
                                   const std::string& message) {
    std::string msg = "{\"seq\":" + jsonInt(seq_++) +
        ",\"type\":\"response\",\"request_seq\":" + jsonInt(requestSeq) +
        ",\"success\":false,\"command\":" + jsonString(command) +
        ",\"message\":" + jsonString(message) + "}";

    std::string header = "Content-Length: " + std::to_string(msg.size()) + "\r\n\r\n";
    std::cout << header << msg;
    std::cout.flush();
}

// ============================================================
//  Main run loop
// ============================================================

void DAPServer::run() {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    while (!terminated_) {
        std::string msg = readMessage();
        if (msg.empty()) break;
        handleRequest(msg);
    }

    killDebugger();
}

void DAPServer::handleRequest(const std::string& json) {
    std::string type = extractString(json, "type");
    std::string command = extractString(json, "command");
    int requestSeq = extractInt(json, "seq");

    auto argsPos = json.find("\"arguments\"");
    std::string args = "{}";
    if (argsPos != std::string::npos) {
        auto braceStart = json.find('{', argsPos);
        if (braceStart != std::string::npos) {
            int depth = 0;
            size_t end = braceStart;
            for (; end < json.size(); ++end) {
                if (json[end] == '{') depth++;
                else if (json[end] == '}') { depth--; if (depth == 0) break; }
            }
            args = json.substr(braceStart, end - braceStart + 1);
        }
    }

    if (command == "initialize")         handleInitialize(requestSeq, args);
    else if (command == "launch")        handleLaunch(requestSeq, args);
    else if (command == "attach")        handleAttach(requestSeq, args);
    else if (command == "setBreakpoints") handleSetBreakpoints(requestSeq, args);
    else if (command == "configurationDone") handleConfigurationDone(requestSeq);
    else if (command == "threads")       handleThreads(requestSeq);
    else if (command == "stackTrace")    handleStackTrace(requestSeq, args);
    else if (command == "scopes")        handleScopes(requestSeq, args);
    else if (command == "variables")     handleVariables(requestSeq, args);
    else if (command == "continue")      handleContinue(requestSeq, args);
    else if (command == "next")          handleNext(requestSeq, args);
    else if (command == "stepIn")        handleStepIn(requestSeq, args);
    else if (command == "stepOut")       handleStepOut(requestSeq, args);
    else if (command == "pause")         handlePause(requestSeq, args);
    else if (command == "disconnect")    handleDisconnect(requestSeq, args);
    else if (command == "evaluate")      handleEvaluate(requestSeq, args);
    else {
        sendResponse(seq_, requestSeq, command, true);
    }
}

// ============================================================
//  DAP request handlers
// ============================================================

void DAPServer::handleInitialize(int seq, const std::string&) {
    std::string body = "{"
        "\"supportsConfigurationDoneRequest\":true,"
        "\"supportsFunctionBreakpoints\":false,"
        "\"supportsConditionalBreakpoints\":false,"
        "\"supportsEvaluateForHovers\":true,"
        "\"supportsStepBack\":false,"
        "\"supportsSetVariable\":false,"
        "\"supportsRestartFrame\":false,"
        "\"supportsGotoTargetsRequest\":false,"
        "\"supportsCompletionsRequest\":false,"
        "\"supportsTerminateRequest\":true,"
        "\"supportsSteppingGranularity\":false"
    "}";
    sendResponse(seq_, seq, "initialize", true, body);
    sendEvent("initialized");
    initialized_ = true;
}

void DAPServer::handleLaunch(int seq, const std::string& args) {
    programPath_ = extractString(args, "program");

    if (programPath_.empty()) {
        sendErrorResponse(seq_, seq, "launch", "No program specified");
        return;
    }

    if (!fs::exists(programPath_)) {
        sendErrorResponse(seq_, seq, "launch", "Program not found: " + programPath_);
        return;
    }

    // Find GDB or LLDB
    debuggerPath_ = extractString(args, "debuggerPath");
    if (debuggerPath_.empty()) {
#ifdef _WIN32
        for (auto& candidate : {"gdb.exe", "lldb.exe"}) {
            std::string whichCmd = std::string("where.exe ") + candidate + " 2>NUL";
            if (std::system(whichCmd.c_str()) == 0) {
                debuggerPath_ = candidate;
                break;
            }
        }
#else
        for (auto& candidate : {"gdb", "lldb"}) {
            std::string whichCmd = std::string("which ") + candidate + " 2>/dev/null";
            if (std::system(whichCmd.c_str()) == 0) {
                debuggerPath_ = candidate;
                break;
            }
        }
#endif
    }

    if (debuggerPath_.empty()) {
        sendErrorResponse(seq_, seq, "launch", "No debugger (GDB/LLDB) found. Install GDB or LLDB and add to PATH.");
        return;
    }

    std::vector<std::string> launchArgs;
    if (!launchDebugger(programPath_, launchArgs)) {
        sendErrorResponse(seq_, seq, "launch", "Failed to start debugger");
        return;
    }

    // Set breakpoints that were registered before launch
    for (auto& bp : breakpoints_) {
        std::string cmd = "-break-insert " + bp.sourcePath + ":" + std::to_string(bp.line);
        sendDebuggerCommand(cmd);
    }

    sendResponse(seq_, seq, "launch", true);
}

void DAPServer::handleAttach(int seq, const std::string&) {
    sendErrorResponse(seq_, seq, "attach", "Attach mode not supported yet");
}

void DAPServer::handleSetBreakpoints(int seq, const std::string& args) {
    std::string sourcePath = extractString(args, "path");

    // Parse breakpoint lines from "lines" array
    std::vector<int> lines;
    auto linesPos = args.find("\"lines\"");
    if (linesPos == std::string::npos) linesPos = args.find("\"breakpoints\"");
    if (linesPos != std::string::npos) {
        auto arrStart = args.find('[', linesPos);
        auto arrEnd = args.find(']', arrStart);
        if (arrStart != std::string::npos && arrEnd != std::string::npos) {
            std::string arrContent = args.substr(arrStart + 1, arrEnd - arrStart - 1);
            // Parse line numbers from breakpoints array objects or plain numbers
            size_t pos = 0;
            while (pos < arrContent.size()) {
                auto linePos = arrContent.find("\"line\"", pos);
                if (linePos != std::string::npos) {
                    auto colonPos = arrContent.find(':', linePos + 6);
                    if (colonPos != std::string::npos) {
                        std::string num;
                        size_t numStart = colonPos + 1;
                        while (numStart < arrContent.size() && arrContent[numStart] == ' ') numStart++;
                        while (numStart < arrContent.size() && isdigit(arrContent[numStart]))
                            num += arrContent[numStart++];
                        if (!num.empty()) lines.push_back(std::stoi(num));
                    }
                    pos = linePos + 6;
                } else {
                    // Try plain numbers
                    while (pos < arrContent.size() && !isdigit(arrContent[pos])) pos++;
                    std::string num;
                    while (pos < arrContent.size() && isdigit(arrContent[pos]))
                        num += arrContent[pos++];
                    if (!num.empty()) lines.push_back(std::stoi(num));
                }
            }
        }
    }

    // Remove old breakpoints for this source
    breakpoints_.erase(
        std::remove_if(breakpoints_.begin(), breakpoints_.end(),
            [&](const BreakpointInfo& bp) { return bp.sourcePath == sourcePath; }),
        breakpoints_.end());

    std::string bpArray = "[";
    for (size_t i = 0; i < lines.size(); ++i) {
        BreakpointInfo bp;
        bp.sourcePath = sourcePath;
        bp.line = lines[i];
        bp.id = nextBreakpointId_++;
        breakpoints_.push_back(bp);

        // If debugger is running, set the breakpoint now
        if (running_) {
            std::string cmd = "-break-insert " + sourcePath + ":" + std::to_string(lines[i]);
            sendDebuggerCommand(cmd);
        }

        if (i > 0) bpArray += ",";
        bpArray += "{\"id\":" + jsonInt(bp.id) +
            ",\"verified\":true,\"line\":" + jsonInt(lines[i]) +
            ",\"source\":{\"path\":" + jsonString(sourcePath) + "}}";
    }
    bpArray += "]";

    sendResponse(seq_, seq, "setBreakpoints", true,
        "{\"breakpoints\":" + bpArray + "}");
}

void DAPServer::handleConfigurationDone(int seq) {
    sendResponse(seq_, seq, "configurationDone", true);

    // Start executing the program
    if (running_) {
        sendDebuggerCommand("-exec-run");
    }
}

void DAPServer::handleThreads(int seq) {
    sendResponse(seq_, seq, "threads", true,
        "{\"threads\":[{\"id\":1,\"name\":\"main\"}]}");
}

void DAPServer::handleStackTrace(int seq, const std::string&) {
    cachedFrames_.clear();

    std::string output = sendDebuggerCommand("-stack-list-frames");

    // Parse GDB MI output for frames
    // Format: frame={level="0",addr="0x...",func="main",file="test.vyx",line="10"}
    size_t pos = 0;
    int frameId = 0;
    while ((pos = output.find("frame={", pos)) != std::string::npos) {
        StackFrame frame;
        frame.id = frameId++;

        auto funcPos = output.find("func=\"", pos);
        if (funcPos != std::string::npos) {
            auto funcEnd = output.find('"', funcPos + 6);
            frame.name = output.substr(funcPos + 6, funcEnd - funcPos - 6);
        }

        auto filePos = output.find("file=\"", pos);
        if (filePos != std::string::npos) {
            auto fileEnd = output.find('"', filePos + 6);
            frame.source.path = output.substr(filePos + 6, fileEnd - filePos - 6);
            frame.source.name = fs::path(frame.source.path).filename().string();
        }

        auto linePos = output.find("line=\"", pos);
        if (linePos != std::string::npos) {
            auto lineEnd = output.find('"', linePos + 6);
            frame.line = std::stoi(output.substr(linePos + 6, lineEnd - linePos - 6));
        }

        cachedFrames_.push_back(frame);
        pos += 7;
    }

    if (cachedFrames_.empty()) {
        cachedFrames_.push_back({0, "<unknown>", {"<unknown>", ""}, 0, 0});
    }

    std::string framesJson = "[";
    for (size_t i = 0; i < cachedFrames_.size(); ++i) {
        auto& f = cachedFrames_[i];
        if (i > 0) framesJson += ",";
        framesJson += "{\"id\":" + jsonInt(f.id) +
            ",\"name\":" + jsonString(f.name) +
            ",\"line\":" + jsonInt(f.line) +
            ",\"column\":" + jsonInt(f.column);
        if (!f.source.path.empty()) {
            framesJson += ",\"source\":{\"name\":" + jsonString(f.source.name) +
                ",\"path\":" + jsonString(f.source.path) + "}";
        }
        framesJson += "}";
    }
    framesJson += "]";

    sendResponse(seq_, seq, "stackTrace", true,
        "{\"stackFrames\":" + framesJson + ",\"totalFrames\":" + jsonInt((int)cachedFrames_.size()) + "}");
}

void DAPServer::handleScopes(int seq, const std::string& args) {
    int frameId = extractInt(args, "frameId");

    int localRef = nextVarRef_++;
    int argRef = nextVarRef_++;

    // Query variables from debugger
    variablesByRef_.clear();
    std::string localOutput = sendDebuggerCommand("-stack-list-locals --simple-values");
    std::string argOutput = sendDebuggerCommand("-stack-list-arguments --simple-values 0 0");

    auto parseVars = [&](const std::string& output) -> std::vector<Variable> {
        std::vector<Variable> vars;
        size_t pos = 0;
        while ((pos = output.find("{name=\"", pos)) != std::string::npos) {
            Variable v;
            auto nameEnd = output.find('"', pos + 7);
            v.name = output.substr(pos + 7, nameEnd - pos - 7);

            auto typePos = output.find("type=\"", pos);
            if (typePos != std::string::npos && typePos < output.find('}', pos)) {
                auto typeEnd = output.find('"', typePos + 6);
                v.type = output.substr(typePos + 6, typeEnd - typePos - 6);
            }

            auto valPos = output.find("value=\"", pos);
            if (valPos != std::string::npos && valPos < output.find('}', pos)) {
                auto valEnd = output.find('"', valPos + 7);
                v.value = output.substr(valPos + 7, valEnd - valPos - 7);
            }

            vars.push_back(v);
            pos = nameEnd + 1;
        }
        return vars;
    };

    variablesByRef_[localRef] = parseVars(localOutput);
    variablesByRef_[argRef] = parseVars(argOutput);

    std::string scopesJson = "[{\"name\":\"Locals\",\"variablesReference\":" +
        jsonInt(localRef) + ",\"expensive\":false},"
        "{\"name\":\"Arguments\",\"variablesReference\":" +
        jsonInt(argRef) + ",\"expensive\":false}]";

    sendResponse(seq_, seq, "scopes", true, "{\"scopes\":" + scopesJson + "}");
    (void)frameId;
}

void DAPServer::handleVariables(int seq, const std::string& args) {
    int varRef = extractInt(args, "variablesReference");

    std::string varsJson = "[";
    auto it = variablesByRef_.find(varRef);
    if (it != variablesByRef_.end()) {
        for (size_t i = 0; i < it->second.size(); ++i) {
            auto& v = it->second[i];
            if (i > 0) varsJson += ",";
            varsJson += "{\"name\":" + jsonString(v.name) +
                ",\"value\":" + jsonString(v.value) +
                ",\"type\":" + jsonString(v.type) +
                ",\"variablesReference\":0}";
        }
    }
    varsJson += "]";

    sendResponse(seq_, seq, "variables", true, "{\"variables\":" + varsJson + "}");
}

void DAPServer::handleContinue(int seq, const std::string&) {
    sendDebuggerCommand("-exec-continue");
    sendResponse(seq_, seq, "continue", true, "{\"allThreadsContinued\":true}");
}

void DAPServer::handleNext(int seq, const std::string&) {
    sendDebuggerCommand("-exec-next");
    sendResponse(seq_, seq, "next", true);
    readDebuggerOutput();
    sendEvent("stopped", "{\"reason\":\"step\",\"threadId\":1,\"allThreadsStopped\":true}");
}

void DAPServer::handleStepIn(int seq, const std::string&) {
    sendDebuggerCommand("-exec-step");
    sendResponse(seq_, seq, "stepIn", true);
    readDebuggerOutput();
    sendEvent("stopped", "{\"reason\":\"step\",\"threadId\":1,\"allThreadsStopped\":true}");
}

void DAPServer::handleStepOut(int seq, const std::string&) {
    sendDebuggerCommand("-exec-finish");
    sendResponse(seq_, seq, "stepOut", true);
    readDebuggerOutput();
    sendEvent("stopped", "{\"reason\":\"step\",\"threadId\":1,\"allThreadsStopped\":true}");
}

void DAPServer::handlePause(int seq, const std::string&) {
    sendDebuggerCommand("-exec-interrupt");
    sendResponse(seq_, seq, "pause", true);
    sendEvent("stopped", "{\"reason\":\"pause\",\"threadId\":1,\"allThreadsStopped\":true}");
}

void DAPServer::handleDisconnect(int seq, const std::string&) {
    killDebugger();
    sendResponse(seq_, seq, "disconnect", true);
    terminated_ = true;
}

void DAPServer::handleEvaluate(int seq, const std::string& args) {
    std::string expression = extractString(args, "expression");
    std::string cmd = "-data-evaluate-expression " + expression;
    std::string output = sendDebuggerCommand(cmd);

    std::string value = "?";
    auto valPos = output.find("value=\"");
    if (valPos != std::string::npos) {
        auto valEnd = output.find('"', valPos + 7);
        value = output.substr(valPos + 7, valEnd - valPos - 7);
    }

    sendResponse(seq_, seq, "evaluate", true,
        "{\"result\":" + jsonString(value) + ",\"variablesReference\":0}");
}

// ============================================================
//  Debugger process management
// ============================================================

bool DAPServer::launchDebugger(const std::string& program, const std::vector<std::string>&) {
    bool isLldb = debuggerPath_.find("lldb") != std::string::npos;
    std::string cmdLine = debuggerPath_ + (isLldb ? " --batch -o \"" : " --interpreter=mi --quiet ");
    if (isLldb) {
        cmdLine = debuggerPath_ + " -batch";
    } else {
        cmdLine = debuggerPath_ + " --interpreter=mi --quiet " + program;
    }

#ifdef _WIN32
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE stdinRead, stdinWrite, stdoutRead, stdoutWrite;
    CreatePipe(&stdinRead, &stdinWrite, &sa, 0);
    SetHandleInformation(stdinWrite, HANDLE_FLAG_INHERIT, 0);
    CreatePipe(&stdoutRead, &stdoutWrite, &sa, 0);
    SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {};
    si.cb = sizeof(STARTUPINFOA);
    si.hStdInput = stdinRead;
    si.hStdOutput = stdoutWrite;
    si.hStdError = stdoutWrite;
    si.dwFlags |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(nullptr, const_cast<char*>(cmdLine.c_str()),
                        nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
        return false;
    }

    CloseHandle(stdinRead);
    CloseHandle(stdoutWrite);
    CloseHandle(pi.hThread);

    debuggerProcess_ = pi.hProcess;
    debuggerStdinWrite_ = stdinWrite;
    debuggerStdoutRead_ = stdoutRead;
#else
    pipe(toDebugger_);
    pipe(fromDebugger_);

    debuggerPid_ = fork();
    if (debuggerPid_ == 0) {
        close(toDebugger_[1]);
        close(fromDebugger_[0]);
        dup2(toDebugger_[0], STDIN_FILENO);
        dup2(fromDebugger_[1], STDOUT_FILENO);
        dup2(fromDebugger_[1], STDERR_FILENO);
        close(toDebugger_[0]);
        close(fromDebugger_[1]);
        if (isLldb) {
            execlp(debuggerPath_.c_str(), debuggerPath_.c_str(), "-batch", nullptr);
        } else {
            execlp(debuggerPath_.c_str(), debuggerPath_.c_str(),
                   "--interpreter=mi", "--quiet", program.c_str(), nullptr);
        }
        _exit(1);
    }
    close(toDebugger_[0]);
    close(fromDebugger_[1]);
#endif

    running_ = true;
    readDebuggerOutput();
    return true;
}

std::string DAPServer::sendDebuggerCommand(const std::string& cmd) {
    if (!running_) return "";

    std::string fullCmd = cmd + "\n";
#ifdef _WIN32
    DWORD written;
    WriteFile(debuggerStdinWrite_, fullCmd.c_str(), (DWORD)fullCmd.size(), &written, nullptr);
#else
    write(toDebugger_[1], fullCmd.c_str(), fullCmd.size());
#endif

    return "";
}

void DAPServer::readDebuggerOutput() {
    if (!running_) return;

#ifdef _WIN32
    char buf[4096];
    DWORD bytesRead;
    DWORD available;
    if (PeekNamedPipe(debuggerStdoutRead_, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
        ReadFile(debuggerStdoutRead_, buf, std::min(available, (DWORD)sizeof(buf) - 1), &bytesRead, nullptr);
        buf[bytesRead] = '\0';

        std::string output(buf, bytesRead);
        if (output.find("*stopped") != std::string::npos) {
            if (output.find("reason=\"breakpoint-hit\"") != std::string::npos) {
                sendEvent("stopped", "{\"reason\":\"breakpoint\",\"threadId\":1,\"allThreadsStopped\":true}");
            } else if (output.find("reason=\"exited") != std::string::npos ||
                       output.find("reason=\"signal") != std::string::npos) {
                sendEvent("terminated");
                running_ = false;
            }
        }
    }
#else
    char buf[4096];
    ssize_t n = read(fromDebugger_[0], buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        std::string output(buf, n);
        if (output.find("*stopped") != std::string::npos) {
            if (output.find("reason=\"breakpoint-hit\"") != std::string::npos) {
                sendEvent("stopped", "{\"reason\":\"breakpoint\",\"threadId\":1,\"allThreadsStopped\":true}");
            } else if (output.find("reason=\"exited") != std::string::npos) {
                sendEvent("terminated");
                running_ = false;
            }
        }
    }
#endif
}

void DAPServer::killDebugger() {
#ifdef _WIN32
    if (debuggerProcess_) {
        sendDebuggerCommand("-gdb-exit");
        WaitForSingleObject(debuggerProcess_, 2000);
        TerminateProcess(debuggerProcess_, 0);
        CloseHandle(debuggerProcess_);
        CloseHandle(debuggerStdinWrite_);
        CloseHandle(debuggerStdoutRead_);
        debuggerProcess_ = nullptr;
    }
#else
    if (debuggerPid_ > 0) {
        sendDebuggerCommand("-gdb-exit");
        int status;
        waitpid(debuggerPid_, &status, WNOHANG);
        kill(debuggerPid_, SIGTERM);
        close(toDebugger_[1]);
        close(fromDebugger_[0]);
        debuggerPid_ = -1;
    }
#endif
    running_ = false;
}

} // namespace dap
} // namespace vyx
