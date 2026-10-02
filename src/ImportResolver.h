#pragma once

#include "Parser/AST.h"
#include "Common/Diagnostics.h"
#include "Common/Determinism.h"
#include "Lexer/Lexer.h"
#include "Parser/Parser.h"

#include <filesystem>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <functional>
#include <expected>

namespace vyx {

std::expected<std::string, std::string> readSourceFile(const std::string& path);

// Registry of `module xxx;` declarations found by scanning directories.
class ModuleRegistry {
public:
    void scanDirectory(const std::filesystem::path& dir);
    void scanDirectories(const std::vector<std::filesystem::path>& dirs);

    const std::map<std::string, std::vector<std::string>>& modules() const { return registry_; }

    std::vector<std::string> filesForModule(const std::string& moduleName) const;

private:
    std::map<std::string, std::vector<std::string>> registry_;
    std::set<std::string> scannedDirs_;
};

// Resolves imports for a TranslationUnit: loads only explicit `use xxx;`
// module imports and recursively follows their file imports.
class ImportResolver {
public:
    ImportResolver(DiagnosticsEngine& diag, const ModuleRegistry& registry);

    void resolve(TranslationUnit& unit, const std::filesystem::path& entryPath);

    const std::set<std::string>& importedFiles() const { return importedFiles_; }

private:
    void resolveUseImports(TranslationUnit& unit);
    void resolveFileImports(const std::vector<DeclPtr>& decls, const std::filesystem::path& fromPath);
    void importFile(const std::string& filePath);
    // Emit a diagnostic for every `use X.Y.Z;` statement in `unit` that
    // matched neither the module registry nor a file-path fallback. Runs
    // after both resolveUseImports and resolveFileImports so registered
    // fallbacks (e.g. `use std.option;` resolving via std/option.vyx even
    // though option.vyx declares `module std.core;`) are still accepted.
    void diagnoseUnknownImports(TranslationUnit& unit,
                                const std::filesystem::path& fromPath);

    // C#-style dotted access: walk AST, find `std.collections.Vec::<T>.new()`
    // patterns whose prefix matches an explicitly imported registered module
    // path, and rewrites the chain to drop the module prefix so subsequent
    // Sema / CodeGen sees the bare symbol `Vec::<T>.new()`.
    void rewriteDottedModulePaths(TranslationUnit& unit);

    DiagnosticsEngine& diag_;
    const ModuleRegistry& registry_;
    std::vector<DeclPtr> allImported_;
    std::vector<std::string> sourceBuffers_;
    std::set<std::string> importedFiles_;
    std::vector<std::string> importChain_;
};

// Ordered roots for an entry source. The owning project std/ directory is
// considered before the process working directory's std/ directory.
std::vector<std::filesystem::path> moduleSearchRoots(
    const std::filesystem::path& entryPath,
    const std::filesystem::path& executableDir = {});

} // namespace vyx
