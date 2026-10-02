#include "ProjectManager.h"
#include "Compiler.h"
#include "ImportResolver.h"
#include "Linker.h"
#include "Common/Diagnostics.h"
#include "Common/Package.h"
#include "Lexer/Lexer.h"
#include "Parser/Parser.h"
#include "Sema/Sema.h"
#include "Mono/Monomorphize.h"
#include "CodeGen/CodeGen.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <unordered_map>
#include <map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace vyx {

static std::string quoteArg(const std::string& arg) {
    if (arg.find_first_of(" \t\"") == std::string::npos) return arg;
    std::string out = "\"";
    for (char c : arg) {
        if (c == '"') out += "\\\"";
        else out += c;
    }
    out += "\"";
    return out;
}

void printVyxUsage(const char* prog) {
    std::cerr << "Vyx Compiler bl-2026-03-27\n"
              << "Usage: " << prog << " <command> [options]\n\n"
              << "Commands:\n"
              << "  build              Build project using Vyx.toml\n"
              << "\nUse 'vyxc <file.vyx>' for direct compilation.\n";
}

// ============================================================
//  cmdBuild — unified project build
// ============================================================

int cmdBuild(int argc, char* argv[]) {
    std::string targetTriple;
    std::string sysroot;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--target" && i + 1 < argc) {
            targetTriple = argv[++i];
        } else if (arg.rfind("--target=", 0) == 0) {
            targetTriple = arg.substr(9);
        } else if (arg == "--sysroot" && i + 1 < argc) {
            sysroot = argv[++i];
        } else if (arg.rfind("--sysroot=", 0) == 0) {
            sysroot = arg.substr(10);
        }
    }

    auto config = PackageConfig::load("Vyx.toml", targetTriple);
    if (!config) {
        std::cerr << "error: no Vyx.toml found in current directory\n";
        return 1;
    }

    int numThreads = config->getThreadCount();
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-j" && i + 1 < argc) {
            numThreads = std::stoi(argv[++i]);
        }
    }

    std::cerr << "building " << config->name << " v" << config->version
              << " (" << numThreads << " thread(s))\n";

    fs::path buildCacheDir = config->cacheDir;
    fs::create_directories(buildCacheDir);

    for (auto& cmd : config->prebuild) {
        std::cerr << "  [prebuild] " << cmd << '\n';
        if (std::system(cmd.c_str()) != 0) {
            std::cerr << "error: prebuild command failed: " << cmd << '\n';
            return 1;
        }
    }

    // Topological sort of targets
    std::map<std::string, size_t> targetIndex;
    for (size_t i = 0; i < config->targets.size(); ++i)
        targetIndex[config->targets[i].name] = i;

    std::vector<size_t> buildOrder;
    std::unordered_set<size_t> visited, inStack;
    std::function<bool(size_t)> topoVisit = [&](size_t idx) -> bool {
        if (inStack.count(idx)) {
            std::cerr << "error: circular dependency involving target '"
                      << config->targets[idx].name << "'\n";
            return false;
        }
        if (visited.count(idx)) return true;
        inStack.insert(idx);
        for (auto& dep : config->targets[idx].dependsOn) {
            auto it = targetIndex.find(dep);
            if (it != targetIndex.end()) {
                if (!topoVisit(it->second)) return false;
            }
        }
        inStack.erase(idx);
        visited.insert(idx);
        buildOrder.push_back(idx);
        return true;
    };
    for (size_t i = 0; i < config->targets.size(); ++i) {
        if (!topoVisit(i)) return 1;
    }

    std::map<std::string, std::string> builtOutputs;

    // Depth-based parallel grouping
    std::unordered_map<size_t, int> targetDepth;
    for (auto idx : buildOrder) {
        int maxDep = 0;
        for (auto& dep : config->targets[idx].dependsOn) {
            auto it = targetIndex.find(dep);
            if (it != targetIndex.end() && targetDepth.count(it->second))
                maxDep = std::max(maxDep, targetDepth[it->second] + 1);
        }
        targetDepth[idx] = maxDep;
    }

    int maxDepth = 0;
    for (auto& [idx, d] : targetDepth) maxDepth = std::max(maxDepth, d);

    for (int depth = 0; depth <= maxDepth; ++depth) {
        std::vector<size_t> levelTargets;
        for (auto idx : buildOrder)
            if (targetDepth[idx] == depth) levelTargets.push_back(idx);

        if (levelTargets.size() > 1 && numThreads > 1)
            std::cerr << "  building " << levelTargets.size()
                      << " independent targets in parallel (depth=" << depth << ")\n";

        for (auto idx : levelTargets) {
            auto& target = config->targets[idx];

            for (auto& cmd : target.prebuild) {
                std::cerr << "  [prebuild:" << target.name << "] " << cmd << '\n';
                if (std::system(cmd.c_str()) != 0) {
                    std::cerr << "error: prebuild command failed for target '"
                              << target.name << "': " << cmd << '\n';
                    return 1;
                }
            }

            // Auto-inject lib paths/libs from depends_on
            std::vector<std::string> depVyiFiles;
            for (auto& dep : target.dependsOn) {
                auto outIt = builtOutputs.find(dep);
                if (outIt != builtOutputs.end()) {
                    fs::path outPath = outIt->second;
                    std::string dir = outPath.parent_path().string();
                    if (dir.empty()) dir = ".";
                    if (std::find(target.libPaths.begin(), target.libPaths.end(), dir) == target.libPaths.end())
                        target.libPaths.push_back(dir);
                    std::string libName = Linker::normalizeLinkLibName(outPath.filename().string());
                    if (std::find(target.linkLibs.begin(), target.linkLibs.end(), libName) == target.linkLibs.end())
                        target.linkLibs.push_back(libName);
                    std::string vyiPath = outPath.stem().string() + ".vyi";
                    if (fs::exists(vyiPath)) depVyiFiles.push_back(vyiPath);
                }
            }

            std::string entryFile = target.entry.empty() ? config->entryPoint : target.entry;
            if (!fs::exists(entryFile)) {
                std::cerr << "error: entry file '" << entryFile << "' not found\n";
                return 1;
            }

            // Collect Vyx source files
            std::vector<std::string> sourceFiles;
            sourceFiles.push_back(entryFile);
            for (auto& src : target.sources) {
                auto srcExt = fs::path(src).extension().string();
                if (srcExt == ".c" || srcExt == ".cpp" || srcExt == ".cc" || srcExt == ".cxx")
                    continue;
                if (fs::exists(src) && std::find(sourceFiles.begin(), sourceFiles.end(), src) == sourceFiles.end())
                    sourceFiles.push_back(src);
            }

            // Compile C/C++ sources
            std::vector<std::string> cppObjFiles;
            for (auto& src : target.sources) {
                fs::path srcPath(src);
                auto ext = srcPath.extension().string();
                if (ext == ".c" || ext == ".cpp" || ext == ".cc" || ext == ".cxx") {
#ifdef _WIN32
                    std::string objName = (buildCacheDir / (srcPath.stem().string() + "_c.obj")).string();
#else
                    std::string objName = (buildCacheDir / (srcPath.stem().string() + "_c.o")).string();
#endif
                    bool isCFile = (ext == ".c");
                    std::string clangExe = isCFile
                        ? (target.cCompiler.empty() ? "clang" : target.cCompiler)
                        : (target.cxxCompiler.empty() ? "clang++" : target.cxxCompiler);
                    std::string cmd = quoteArg(clangExe) + " -c \"" + src + "\" -o \"" + objName + "\" -O2";
                    if (!targetTriple.empty()) cmd += " --target=" + targetTriple;
                    if (!sysroot.empty()) cmd += " --sysroot=" + quoteArg(sysroot);
                    cmd += isCFile ? " -std=c11" : " -std=c++17";
                    // include_paths
                    for (auto& inc : target.includePaths)
                        cmd += " -I \"" + inc + "\"";
                    // cflags / cxxflags
                    for (auto& flag : (isCFile ? target.cflags : target.cxxflags))
                        cmd += " " + flag;
                    std::cerr << "  compiling " << (isCFile ? "C" : "C++") << ": " << src << '\n';
                    if (std::system(cmd.c_str()) != 0) {
                        std::cerr << "error: failed to compile " << src << '\n';
                        return 1;
                    }
                    cppObjFiles.push_back(objName);
                }
            }

            // Compile Vyx files
            std::vector<std::string> objFiles;
            std::atomic<bool> hasError{false};
            CompileCache cache(buildCacheDir.string());

            if (numThreads > 1 && sourceFiles.size() > 1) {
                // Parallel compilation
                std::vector<std::thread> threads;
                for (auto& srcFile : sourceFiles) {
                    std::string objFile = (buildCacheDir / (fs::path(srcFile).stem().string() + ".obj")).string();
                    objFiles.push_back(objFile);
                }
                int skipped = 0;
                std::mutex mtx;
                for (size_t i = 0; i < sourceFiles.size(); ++i) {
                    if (cache.isUpToDate(sourceFiles[i], objFiles[i])) {
                        std::cerr << "  cached " << sourceFiles[i] << '\n';
                        skipped++;
                        continue;
                    }
                    threads.emplace_back([&, i]() {
                        auto result = compileFile(sourceFiles[i], objFiles[i], i == 0);
                        if (!result) {
                            std::cerr << "error: " << result.error() << '\n';
                            hasError.store(true);
                        } else {
                            std::lock_guard<std::mutex> lock(mtx);
                            cache.recordCompilation(sourceFiles[i], objFiles[i]);
                        }
                    });
                }
                for (auto& t : threads) t.join();
                if (hasError.load()) { std::cerr << "compilation failed\n"; return 1; }
                if (skipped > 0)
                    std::cerr << "  " << skipped << " file(s) up-to-date, skipped\n";
            } else {
                // Single-threaded unified compilation
                auto sourceResult = readSourceFile(entryFile);
                if (!sourceResult) {
                    std::cerr << "error: " << sourceResult.error() << '\n';
                    return 1;
                }

                DiagnosticsEngine diag;
                diag.setSource(*sourceResult);
                Lexer lexer(*sourceResult, entryFile, diag);
                auto tokens = lexer.tokenizeAll();
                if (diag.hasErrors()) { std::cerr << "lex errors\n"; return 1; }
                Parser parser(std::move(tokens), diag);
                auto unit = parser.parseTranslationUnit(entryFile);
                if (diag.hasErrors()) { std::cerr << "parse errors\n"; return 1; }

                // Import .vyi interfaces from dependencies
                for (auto& vyiPath : depVyiFiles) {
                    auto vyiResult = readSourceFile(vyiPath);
                    if (vyiResult && !vyiResult->empty()) {
                        Lexer vyiLex(*vyiResult, vyiPath, diag);
                        auto vyiToks = vyiLex.tokenizeAll();
                        Parser vyiPar(std::move(vyiToks), diag);
                        auto vyiUnit = vyiPar.parseTranslationUnit(vyiPath);
                        for (auto& d : vyiUnit.declarations) {
                            if (d) unit.declarations.insert(unit.declarations.begin(), std::move(d));
                        }
                        std::cerr << "  imported interface " << vyiPath << '\n';
                    }
                }

                // Build module registry and resolve imports
                ModuleRegistry modRegistry;
                fs::path entryPath = fs::absolute(entryFile);
                modRegistry.scanDirectories(moduleSearchRoots(entryPath));

                ImportResolver resolver(diag, modRegistry);
                resolver.resolve(unit, entryPath);

                Sema sema(diag);
                sema.analyze(unit);
                if (diag.hasErrors()) { std::cerr << "sema errors\n"; return 1; }

                // Project-mode unified compilation was skipping Mono entirely,
                // so generic classes like `Vec<i64>` / `Iterator<i64>` never
                // got their concrete struct entries and CodeGen errored with
                // "generic struct type not found".  Mirror main.cpp's seed.
                {
                    std::vector<const Decl*> monoRoots;
                    monoRoots.reserve(unit.declarations.size());
                    for (auto& d : unit.declarations) {
                        if (d && d->kind == DeclKind::Function)
                            monoRoots.push_back(d.get());
                    }
                    Monomorphize mono(diag);
                    mono.setScheduler(&sema.getMonoSchedulerMut());
                    mono.setAssocTemplateMap(sema.getAssocTemplateMap());
                    // R5 step 3: share the lang-item registry with Mono so its
                    // VyxType → TypeAnnotation reverse path picks up the
                    // registered Option/Result slot names (not hard-coded).
                    mono.setLangItems(&sema.getLangItems());
                    mono.run(unit, monoRoots);
                    if (diag.hasErrors()) { std::cerr << "mono errors\n"; return 1; }
                }

                CodeGen codegen(diag, target.name);
                codegen.setLangItems(&sema.getLangItems());
                codegen.generate(unit);
                if (diag.hasErrors()) { std::cerr << "codegen errors\n"; return 1; }
                codegen.optimize(2);

                std::string objFile = (buildCacheDir / (target.name + ".obj")).string();
                if (!codegen.emitObject(objFile)) return 1;
                objFiles.push_back(objFile);
            }

            // Link
            LinkJob job;
            job.type = target.type;
            job.baseName = target.name;
            job.outputDir = target.outputDir.empty() ? config->outputDir : target.outputDir;
            job.linkLibs = target.linkLibs;
            job.libPaths = target.libPaths;
            job.linkOrder = target.linkOrder;
            job.targetTriple = targetTriple;
            job.sysroot = sysroot;
            job.cxxCompiler = target.cxxCompiler;
            job.ar = target.ar;
            job.compilerDir = fs::path(argv[0]).parent_path();
            job.objFiles = objFiles;
            for (auto& obj : cppObjFiles) job.objFiles.push_back(obj);

            if (!job.outputDir.empty())
                fs::create_directories(job.outputDir);

            if (Linker::link(job) != 0) return 1;

            std::string outputFile = Linker::buildOutputPath(job);
            std::cerr << "  built " << outputFile << '\n';
            builtOutputs[target.name] = outputFile;

            for (auto& cmd : target.postbuild) {
                std::cerr << "  [postbuild:" << target.name << "] " << cmd << '\n';
                if (std::system(cmd.c_str()) != 0) {
                    std::cerr << "error: postbuild command failed for target '"
                              << target.name << "': " << cmd << '\n';
                    return 1;
                }
            }

        }
    }

    for (auto& cmd : config->postbuild) {
        std::cerr << "  [postbuild] " << cmd << '\n';
        if (std::system(cmd.c_str()) != 0) {
            std::cerr << "error: postbuild command failed: " << cmd << '\n';
            return 1;
        }
    }

    std::cerr << "build complete!\n";
    return 0;
}

} // namespace vyx
