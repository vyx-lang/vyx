#pragma once
#include "SourceLocation.h"
#include <string>
#include <string_view>
#include <vector>
#include <utility>
#include <format>

namespace vyx {

enum class DiagLevel { Note, Warning, Error, Fatal };

// ── Instantiation chain support (Option-2 export) ────────────────────────
//
// One frame per active template instantiation. Mirrored here from Sema.h
// so that Monomorphize.cpp (and any future pass) can include
// Diagnostics.h alone and call emitInstantiationChainNotes without
// depending on the full Sema translation unit.
//
// Sema::InstantiationFrame is a type alias of this struct (defined in
// Sema.h) so existing code that refers to Sema::InstantiationFrame keeps
// compiling unchanged — both names refer to the same layout.
struct InstantiationFrame {
    std::string calleeName;       // Template being instantiated (fn/struct/class)
    SourceLocation site;          // Call / use site that triggered it
    std::vector<std::pair<std::string, std::string>> bindings; // T → ConcreteType
};

// DiagnosticsEngine is defined below; forward-declare so the free helper
// signature can reference it.
class DiagnosticsEngine;

// Free helper: emit compact "instantiation chain:" notes from `stack`
// using `diag`. If the stack is empty this is a no-op, so callers in
// non-generic error paths are safe to call it unconditionally.
void emitInstantiationChainNotes(DiagnosticsEngine& diag,
                                  SourceLocation siteLoc,
                                  const std::vector<InstantiationFrame>& stack);

struct DiagMessage {
    DiagLevel level;
    SourceLocation location;
    std::string message;
    std::string fixSuggestion;
    SourceLocation endLocation;
    bool isError = false;
    // P2D: structured diagnostic code (e.g. "P2D-001"). Empty for legacy
    // diagnostics. When non-empty, the renderer prints `level[CODE]: msg`
    // (rustc-style) instead of `level: msg`.
    std::string errorCode;
    // P2D: when true, the renderer prints just the location/level/message
    // line and SUPPRESSES the source-snippet + caret. Used by instantiation
    // chain notes so they form a compact list under the primary error.
    bool suppressSnippet = false;
};

class DiagnosticsEngine {
public:
    void setSource(std::string_view src) { source_ = src; }
    void setMaxErrors(uint32_t max) { maxErrors_ = max; }
    void enableColor(bool on) { useColor_ = on; }

    template <typename... Args>
    void report(DiagLevel level, SourceLocation loc, std::format_string<Args...> fmt, Args&&... args) {
        if (suppressed_) return;
        DiagMessage dm;
        dm.level = level;
        dm.location = loc;
        dm.message = std::format(fmt, std::forward<Args>(args)...);
        dm.isError = (level >= DiagLevel::Error);
        emitDiag(std::move(dm));
    }

    template <typename... Args>
    void errorWithFix(SourceLocation loc, const std::string& fix, std::format_string<Args...> fmt, Args&&... args) {
        if (suppressed_) return;
        DiagMessage dm;
        dm.level = DiagLevel::Error;
        dm.location = loc;
        dm.message = std::format(fmt, std::forward<Args>(args)...);
        dm.fixSuggestion = fix;
        dm.isError = true;
        emitDiag(std::move(dm));
    }

    template <typename... Args>
    void errorRange(SourceLocation start, SourceLocation end, std::format_string<Args...> fmt, Args&&... args) {
        if (suppressed_) return;
        DiagMessage dm;
        dm.level = DiagLevel::Error;
        dm.location = start;
        dm.endLocation = end;
        dm.message = std::format(fmt, std::forward<Args>(args)...);
        dm.isError = true;
        emitDiag(std::move(dm));
    }

    template <typename... Args>
    void error(SourceLocation loc, std::format_string<Args...> fmt, Args&&... args) {
        report(DiagLevel::Error, loc, fmt, std::forward<Args>(args)...);
    }

    // P2D-001 family: rustc-style structured error with explicit code.
    // Renders as `error[CODE]: msg`. Caller may chain helpers (errorCoded
    // + noteCoded + helpCoded) to build a multi-line industrial diagnostic
    // with instantiation chain attached as compact (snippet-suppressed)
    // notes.
    template <typename... Args>
    void errorCoded(std::string_view code, SourceLocation loc,
                    std::format_string<Args...> fmt, Args&&... args) {
        if (suppressed_) return;
        DiagMessage dm;
        dm.level = DiagLevel::Error;
        dm.location = loc;
        dm.message = std::format(fmt, std::forward<Args>(args)...);
        dm.errorCode = std::string(code);
        dm.isError = true;
        emitDiag(std::move(dm));
    }

    // Compact `note: ...` line with no source snippet — for instantiation
    // chain frames that should stack vertically without re-printing the
    // source line for every level.
    template <typename... Args>
    void noteCompact(SourceLocation loc, std::format_string<Args...> fmt, Args&&... args) {
        if (suppressed_) return;
        DiagMessage dm;
        dm.level = DiagLevel::Note;
        dm.location = loc;
        dm.message = std::format(fmt, std::forward<Args>(args)...);
        dm.suppressSnippet = true;
        emitDiag(std::move(dm));
    }

    // `help: ...` rendered as a compact note (no caret), aligned with the
    // primary error. Carries the GREEN `help:` prefix to be surfaced by the
    // renderer.
    template <typename... Args>
    void helpCompact(SourceLocation loc, std::format_string<Args...> fmt, Args&&... args) {
        if (suppressed_) return;
        DiagMessage dm;
        dm.level = DiagLevel::Note;
        dm.location = loc;
        dm.message = "help: " + std::format(fmt, std::forward<Args>(args)...);
        dm.suppressSnippet = true;
        emitDiag(std::move(dm));
    }

    template <typename... Args>
    void warning(SourceLocation loc, std::format_string<Args...> fmt, Args&&... args) {
        report(DiagLevel::Warning, loc, fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void note(SourceLocation loc, std::format_string<Args...> fmt, Args&&... args) {
        report(DiagLevel::Note, loc, fmt, std::forward<Args>(args)...);
    }

    [[nodiscard]] bool hasErrors() const { return errorCount_ > 0; }
    [[nodiscard]] uint32_t errorCount() const { return errorCount_; }
    bool isSuppressed() const { return suppressed_; }
    const std::vector<DiagMessage>& diagnostics() const { return diagnostics_; }

private:
    void emitDiag(DiagMessage&& dm);
    void printDiagnostic(const DiagMessage& diag);

    std::vector<DiagMessage> diagnostics_;
    uint32_t errorCount_ = 0;
    uint32_t maxErrors_ = 50;
    bool suppressed_ = false;
    bool useColor_ = true;
    std::string_view source_;
};

} // namespace vyx
