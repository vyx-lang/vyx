#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <cstdint>

namespace vyx {
namespace dap {

struct Source {
    std::string name;
    std::string path;
};

struct Breakpoint {
    int id = 0;
    bool verified = false;
    int line = 0;
    Source source;
};

struct StackFrame {
    int id = 0;
    std::string name;
    Source source;
    int line = 0;
    int column = 0;
};

struct Scope {
    std::string name;
    int variablesReference = 0;
    bool expensive = false;
};

struct Variable {
    std::string name;
    std::string value;
    std::string type;
    int variablesReference = 0;
};

struct BreakpointInfo {
    std::string sourcePath;
    int line = 0;
    int id = 0;
};

class DAPServer {
public:
    DAPServer();
    void run();

private:
    std::string readMessage();
    void sendEvent(const std::string& event, const std::string& bodyJson = "");
    void sendResponse(int seq, int requestSeq, const std::string& command,
                      bool success, const std::string& bodyJson = "");
    void sendErrorResponse(int seq, int requestSeq, const std::string& command,
                           const std::string& message);

    void handleRequest(const std::string& json);

    void handleInitialize(int seq, const std::string& args);
    void handleLaunch(int seq, const std::string& args);
    void handleAttach(int seq, const std::string& args);
    void handleSetBreakpoints(int seq, const std::string& args);
    void handleConfigurationDone(int seq);
    void handleThreads(int seq);
    void handleStackTrace(int seq, const std::string& args);
    void handleScopes(int seq, const std::string& args);
    void handleVariables(int seq, const std::string& args);
    void handleContinue(int seq, const std::string& args);
    void handleNext(int seq, const std::string& args);
    void handleStepIn(int seq, const std::string& args);
    void handleStepOut(int seq, const std::string& args);
    void handlePause(int seq, const std::string& args);
    void handleDisconnect(int seq, const std::string& args);
    void handleEvaluate(int seq, const std::string& args);

    // GDB/LLDB MI interface
    bool launchDebugger(const std::string& program, const std::vector<std::string>& args);
    std::string sendDebuggerCommand(const std::string& cmd);
    void readDebuggerOutput();
    void killDebugger();

    // JSON helpers
    static std::string jsonString(const std::string& s);
    static std::string jsonInt(int n);
    static std::string jsonBool(bool b);
    static std::string extractString(const std::string& json, const std::string& key);
    static int extractInt(const std::string& json, const std::string& key);

    int seq_ = 1;
    int nextBreakpointId_ = 1;
    int nextVarRef_ = 1000;
    bool initialized_ = false;
    bool running_ = false;
    bool terminated_ = false;

    std::string debuggerPath_;
    std::string programPath_;

    std::vector<BreakpointInfo> breakpoints_;
    std::vector<StackFrame> cachedFrames_;
    std::unordered_map<int, std::vector<Variable>> variablesByRef_;

#ifdef _WIN32
    void* debuggerProcess_ = nullptr;
    void* debuggerStdinWrite_ = nullptr;
    void* debuggerStdoutRead_ = nullptr;
#else
    int debuggerPid_ = -1;
    int toDebugger_[2] = {-1, -1};
    int fromDebugger_[2] = {-1, -1};
#endif
};

} // namespace dap
} // namespace vyx
