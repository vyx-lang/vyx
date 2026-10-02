#include "Diagnostics.h"
#include <iostream>

namespace vyx {

void DiagnosticsEngine::emitDiag(DiagMessage&& dm) {
    if (dm.isError) {
        ++errorCount_;
        if (maxErrors_ > 0 && errorCount_ >= maxErrors_) suppressed_ = true;
    }
    diagnostics_.push_back(std::move(dm));
    printDiagnostic(diagnostics_.back());
}

void DiagnosticsEngine::printDiagnostic(const DiagMessage& diag) {
    static constexpr const char* RESET  = "\033[0m";
    static constexpr const char* BOLD   = "\033[1m";
    static constexpr const char* RED    = "\033[31m";
    static constexpr const char* YELLOW = "\033[33m";
    static constexpr const char* CYAN   = "\033[36m";
    static constexpr const char* GREEN  = "\033[32m";

    auto color = [this](const char* ansi) -> const char* {
        return useColor_ ? ansi : "";
    };

    const char* levelStr = "";
    const char* levelColor = "";
    switch (diag.level) {
        case DiagLevel::Note:    levelStr = "note";    levelColor = CYAN;   break;
        case DiagLevel::Warning: levelStr = "warning"; levelColor = YELLOW; break;
        case DiagLevel::Error:   levelStr = "error";   levelColor = RED;    break;
        case DiagLevel::Fatal:   levelStr = "fatal";   levelColor = RED;    break;
    }

    std::cerr << color(BOLD) << diag.location.toString() << ": "
              << color(levelColor) << levelStr;
    if (!diag.errorCode.empty()) {
        std::cerr << color(levelColor) << "[" << diag.errorCode << "]";
    }
    std::cerr << color(RESET) << color(BOLD) << ": "
              << diag.message
              << color(RESET) << '\n';

    if (diag.suppressSnippet) {
        if (!diag.fixSuggestion.empty()) {
            std::cerr << "       = " << color(GREEN) << "help: "
                      << color(RESET) << diag.fixSuggestion << '\n';
        }
        return;
    }

    if (diag.location.line > 0 && !source_.empty()) {
        uint32_t lineNum = 0;
        size_t pos = 0, lineStart = 0;
        while (pos <= source_.size()) {
            if (pos == source_.size() || source_[pos] == '\n') {
                ++lineNum;
                if (lineNum == diag.location.line) {
                    std::string line(source_.substr(lineStart, pos - lineStart));
                    if (!line.empty() && line.back() == '\r') line.pop_back();

                    std::string numStr = std::to_string(lineNum);
                    std::string gutter(numStr.size() + 2, ' ');

                    std::cerr << "  " << color(CYAN) << lineNum << color(RESET) << " | " << line << '\n';

                    uint32_t col = diag.location.column > 0 ? diag.location.column - 1 : 0;
                    uint32_t spanLen = 1;
                    if (diag.endLocation.line == diag.location.line && diag.endLocation.column > diag.location.column)
                        spanLen = diag.endLocation.column - diag.location.column;

                    std::cerr << gutter << "| " << std::string(col, ' ')
                              << color(levelColor) << "^" << std::string(spanLen > 1 ? spanLen - 1 : 0, '~')
                              << color(RESET) << '\n';

                    if (!diag.fixSuggestion.empty()) {
                        std::cerr << gutter << "= " << color(GREEN) << "help: "
                                  << color(RESET) << diag.fixSuggestion << '\n';
                    }
                    break;
                }
                lineStart = pos + 1;
            }
            ++pos;
        }
    }
}

// ── Option-2 free helper ─────────────────────────────────────────────────
//
// Emit "instantiation chain:" notes from `stack` into `diag`. Called from
// Sema (existing Sema::emitInstantiationChainNotes delegates here), from
// Monomorphize (new Mono-side chain notes), and from any future pass that
// holds its own InstantiationFrame stack. If the stack is empty this is a
// no-op so non-generic error paths are safe to call it unconditionally.
void emitInstantiationChainNotes(DiagnosticsEngine& diag,
                                  SourceLocation siteLoc,
                                  const std::vector<InstantiationFrame>& stack) {
    if (stack.empty()) return;
    diag.noteCompact(siteLoc, "instantiation chain:");
    size_t idx = 1;
    for (auto& frame : stack) {
        std::string args;
        for (auto& [tp, ct] : frame.bindings) {
            if (!args.empty()) args += ", ";
            args += tp + "=" + ct;
        }
        if (args.empty()) {
            diag.noteCompact(frame.site,
                "  {}. {} instantiated at {}",
                idx, frame.calleeName, frame.site.toString());
        } else {
            diag.noteCompact(frame.site,
                "  {}. {} with [{}] instantiated at {}",
                idx, frame.calleeName, args, frame.site.toString());
        }
        ++idx;
    }
}

} // namespace vyx
