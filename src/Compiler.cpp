#include "Compiler.h"
#include "ImportResolver.h"
#include "Common/Diagnostics.h"
#include "Lexer/Lexer.h"
#include "Parser/Parser.h"
#include "Sema/Sema.h"
#include "Mono/Monomorphize.h"
#include "CodeGen/CodeGen.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>

namespace fs = std::filesystem;

namespace vyx {

std::expected<void, std::string> compileFile(const std::string& inputFile,
                                              const std::string& outputObj,
                                              bool isEntry) {
    auto sourceResult = readSourceFile(inputFile);
    if (!sourceResult)
        return std::unexpected(sourceResult.error());
    auto& source = *sourceResult;

    DiagnosticsEngine diag;
    diag.setSource(source);

    Lexer lexer(source, inputFile, diag);
    auto tokens = lexer.tokenizeAll();
    if (diag.hasErrors())
        return std::unexpected("lexer errors in " + inputFile);

    Parser parser(std::move(tokens), diag);
    auto unit = parser.parseTranslationUnit(inputFile);
    if (diag.hasErrors())
        return std::unexpected("parser errors in " + inputFile);

    fs::path inputPath = fs::absolute(inputFile);
    ModuleRegistry modRegistry;
    modRegistry.scanDirectories(moduleSearchRoots(inputPath));
    {
        auto depDir = fs::current_path() / ".vyx_deps";
        if (fs::is_directory(depDir)) modRegistry.scanDirectory(depDir);
    }

    ImportResolver resolver(diag, modRegistry);
    resolver.resolve(unit, inputPath);

    Sema sema(diag);
    sema.analyze(unit);
    if (diag.hasErrors())
        return std::unexpected("semantic errors in " + inputFile);

    // Monomorphize reachable generic instantiations before CodeGen. Without
    // this step, generic classes like `Vec<i64>` and `Iterator<i64>` never
    // get concrete struct entries and CodeGen fails with
    // "generic struct type not found (Mono scan gap?)".  Mirrors the main.cpp
    // single-file path — project-mode (`vyxc build`) used to skip Mono
    // entirely.
    {
        std::vector<const vyx::Decl*> monoRoots;
        monoRoots.reserve(unit.declarations.size());
        for (auto& decl : unit.declarations) {
            if (decl && decl->kind == vyx::DeclKind::Function)
                monoRoots.push_back(decl.get());
        }
        Monomorphize mono(diag);
        mono.setScheduler(&sema.getMonoSchedulerMut());
        mono.setAssocTemplateMap(sema.getAssocTemplateMap());
        // R5 step 3: hand Mono the lang-item registry so its VyxType →
        // TypeAnnotation reverse path names Option/Result via the registered
        // stdlib decls rather than the hard-coded literals.
        mono.setLangItems(&sema.getLangItems());
        mono.run(unit, monoRoots);
        if (diag.hasErrors())
            return std::unexpected("monomorphization errors in " + inputFile);
    }

    std::string moduleName = fs::path(inputFile).stem().string();
    CodeGen codegen(diag, moduleName);
    // R5 stage 2: hand CodeGen the stdlib lang-item registry so its layout /
    // match / `?` paths can recognise `Option` / `Result` by the registered
    // stdlib decl rather than hard-coded `"Option<"` / `"Result<"` prefixes.
    codegen.setLangItems(&sema.getLangItems());
    codegen.generate(unit);
    if (diag.hasErrors())
        return std::unexpected("codegen errors in " + inputFile);
    codegen.optimize(2);

    if (!codegen.emitObject(outputObj))
        return std::unexpected("failed to emit object for " + inputFile);

    std::cerr << "  compiled " << inputFile << " -> " << outputObj << '\n';
    return {};
}

// ============================================================
//  Dependency Graph
// ============================================================

void DependencyGraph::addDependency(const std::string& file, const std::string& dependsOn) {
    deps_[file].push_back(dependsOn);
    reverseDeps_[dependsOn].push_back(file);
    allFiles_.insert(file);
    allFiles_.insert(dependsOn);
}

std::vector<std::string> DependencyGraph::getDependencies(const std::string& file) const {
    auto it = deps_.find(file);
    return it != deps_.end() ? it->second : std::vector<std::string>{};
}

std::vector<std::string> DependencyGraph::getDependents(const std::string& file) const {
    auto it = reverseDeps_.find(file);
    return it != reverseDeps_.end() ? it->second : std::vector<std::string>{};
}

std::vector<std::string> DependencyGraph::topologicalOrder() const {
    std::map<std::string, int> inDegree;
    for (auto& f : allFiles_) inDegree[f] = 0;
    for (auto& [file, depList] : deps_) {
        inDegree[file] += static_cast<int>(depList.size());
    }

    std::vector<std::string> order;
    std::vector<std::string> queue;
    for (auto& [f, d] : inDegree) {
        if (d == 0) queue.push_back(f);
    }

    while (!queue.empty()) {
        auto cur = queue.back();
        queue.pop_back();
        order.push_back(cur);
        auto revIt = reverseDeps_.find(cur);
        if (revIt != reverseDeps_.end()) {
            for (auto& dependent : revIt->second) {
                if (--inDegree[dependent] == 0)
                    queue.push_back(dependent);
            }
        }
    }
    return order;
}

bool DependencyGraph::hasCycle() const {
    return topologicalOrder().size() < allFiles_.size();
}

void DependencyGraph::clear() {
    deps_.clear();
    reverseDeps_.clear();
    allFiles_.clear();
}

// ============================================================
//  Compile Cache
// ============================================================

CompileCache::CompileCache(const std::string& cacheDir) : cacheDir_(cacheDir) {
    fs::create_directories(cacheDir);
    loadTimestamps();
}

bool CompileCache::isUpToDate(const std::string& sourceFile, const std::string& objFile) const {
    if (!fs::exists(objFile)) return false;
    auto srcIt = timestamps_.find(sourceFile);
    if (srcIt == timestamps_.end()) return false;
    auto srcTime = fs::last_write_time(sourceFile);
    auto cachedTime = static_cast<uint64_t>(srcTime.time_since_epoch().count());
    return cachedTime == srcIt->second;
}

void CompileCache::recordCompilation(const std::string& sourceFile, const std::string& objFile) {
    auto srcTime = fs::last_write_time(sourceFile);
    timestamps_[sourceFile] = static_cast<uint64_t>(srcTime.time_since_epoch().count());
    saveTimestamps();
}

std::vector<std::string> CompileCache::getStaleFiles(const std::vector<std::string>& sourceFiles) const {
    std::vector<std::string> stale;
    for (auto& src : sourceFiles) {
        std::string objFile = (fs::path(cacheDir_) / (fs::path(src).stem().string() + ".obj")).string();
        if (!isUpToDate(src, objFile))
            stale.push_back(src);
    }
    return stale;
}

void CompileCache::loadTimestamps() {
    std::string tsFile = (fs::path(cacheDir_) / ".vyx_cache").string();
    std::ifstream f(tsFile);
    if (!f.is_open()) return;
    std::string line;
    while (std::getline(f, line)) {
        auto sep = line.find('\t');
        if (sep == std::string::npos) continue;
        auto end = line.find('\t', sep + 1);
        std::string value = end == std::string::npos
            ? line.substr(sep + 1)
            : line.substr(sep + 1, end - sep - 1);
        uint64_t ts = 0;
        auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), ts);
        if (ec == std::errc{} && ptr == value.data() + value.size()) {
            timestamps_[line.substr(0, sep)] = ts;
        }
    }
}

void CompileCache::saveTimestamps() const {
    std::string tsFile = (fs::path(cacheDir_) / ".vyx_cache").string();
    std::ofstream f(tsFile);
    for (auto& [file, ts] : timestamps_) {
        f << file << '\t' << ts << '\n';
    }
}

} // namespace vyx
