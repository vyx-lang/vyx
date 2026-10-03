#include "Compiler.h"
#include "ProjectManager.h"
#include "ImportResolver.h"
#include "Linker.h"
#include "Common/Diagnostics.h"
#include "Lexer/Lexer.h"
#include "Parser/Parser.h"
#include "Sema/Sema.h"
#include "Mono/Monomorphize.h"
#include "CodeGen/CodeGen.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <set>
#include <map>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

static void printUsage(const char* prog) {
    std::cerr << "Vyx Compiler bl-2026-03-24\n"
              << "Usage: " << prog << " [options] <input.vyx>\n"
              << "       " << prog << " build              Build project using Vyx.toml\n"
              << "Options:\n"
              << "  --emit-ir     Output LLVM IR (.ll)\n"
              << "  --emit-obj    Output object file (.obj)\n"
              << "  --emit-exe    Link and produce executable (default)\n"
              << "  --emit-dll    Produce shared library (.dll)\n"
              << "  --emit-lib    Produce static library (.lib)\n"
              << "  -o <file>     Output file name\n"
              << "  -j <N>        Number of parallel compilation threads\n"
              << "  -Os           Optimize for code size\n"
              << "  -Oz           Aggressively optimize for code size\n"
              << "  --dump-tokens Print token stream (debug)\n"
              << "  --stop-after-parse  Exit 0 after successful parse (no imports/sema/codegen)\n"
              << "  --stop-after-sema   Exit 0 after successful semantic analysis (no mono/codegen)\n"
              << "  --coverage    Enable code coverage instrumentation\n"
              << "  -L <dir>      Add library search path\n"
              << "  -l <lib>      Link with library\n"
              << "  --lib-path <dir>  Add library search path (alias for -L)\n"
              << "  --link <lib>      Link with library (alias for -l)\n"
              << "  --max-errors N    Stop after N distinct template failures (default 100)\n";
}

// ============================================================
//  Incremental cache helpers
//  Uses tab-separated format to avoid Windows drive-letter colon conflicts
// ============================================================

static constexpr char CACHE_SEP = '\t';

static std::map<std::string, size_t> loadFunctionCache(const std::string& cachePath) {
    std::map<std::string, size_t> hashes;
    auto result = vyx::readSourceFile(cachePath);
    if (!result) return hashes;

    std::istringstream iss(*result);
    std::string line;
    while (std::getline(iss, line)) {
        auto sep = line.find(CACHE_SEP);
        if (sep != std::string::npos && sep > 0 && sep + 1 < line.size()) {
            const char* start = line.c_str() + sep + 1;
            char* end = nullptr;
            unsigned long long val = std::strtoull(start, &end, 10);
            if (end != start && end && *end == '\0') {
                hashes[line.substr(0, sep)] = val;
            }
        }
    }
    return hashes;
}

static void writeFunctionCache(const std::string& cachePath,
                                size_t compilerVersionHash,
                                size_t sourceHash,
                                const std::set<std::string>& importedFiles,
                                const fs::path& inputPath,
                                const std::string& source,
                                const vyx::TranslationUnit& unit) {
    std::ofstream cacheFile(cachePath);
    cacheFile << "__compiler" << CACHE_SEP << compilerVersionHash << '\n';
    cacheFile << "__whole_file" << CACHE_SEP << sourceHash << '\n';

    std::string canonInput = fs::canonical(inputPath).string();
    for (auto& imp : importedFiles) {
        if (imp != canonInput) {
            auto impResult = vyx::readSourceFile(imp);
            if (impResult)
                cacheFile << "__import" << CACHE_SEP << imp << CACHE_SEP
                          << std::hash<std::string>{}(*impResult) << '\n';
        }
    }

    for (auto& decl : unit.declarations) {
        if (decl && decl->kind == vyx::DeclKind::Function) {
            size_t funcStart = decl->location.line;
            std::string funcSrc;
            std::istringstream srcStream(source);
            std::string srcLine;
            size_t lineNum = 0;
            bool inFunc = false;
            while (std::getline(srcStream, srcLine)) {
                ++lineNum;
                if (lineNum >= funcStart) inFunc = true;
                if (inFunc) funcSrc += srcLine + "\n";
                if (inFunc && lineNum > funcStart + 500) break;
            }
            auto funcHash = std::hash<std::string>{}(funcSrc);
            cacheFile << decl->name << CACHE_SEP << funcHash << '\n';
        }
    }
}

// ============================================================
//  main
// ============================================================

enum class EmitMode { IR, Object, Executable, Run, DLL, StaticLib };

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    std::string firstArg = argv[1];

    // Project commands
    if (firstArg == "build")             return vyx::cmdBuild(argc, argv);

    // ================================================================
    //  Single-file compilation
    // ================================================================

    std::string inputFile;
    std::string outputFile;
    EmitMode emitMode = EmitMode::Executable;
    bool dumpTokens = false;
    bool stopAfterParse = false;
    bool stopAfterSema = false;
    bool keepObj = false;
    uint32_t maxErrors = 100;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--emit-ir")      { emitMode = EmitMode::IR; }
        else if (arg == "--emit-obj") { emitMode = EmitMode::Object; }
        else if (arg == "--emit-exe") { emitMode = EmitMode::Executable; }
        else if (arg == "--emit-dll") { emitMode = EmitMode::DLL; }
        else if (arg == "--emit-lib") { emitMode = EmitMode::StaticLib; }
        else if (arg == "--run" || arg == "run") { emitMode = EmitMode::Run; }
        else if (arg == "--dump-tokens") { dumpTokens = true; }
        else if (arg == "--stop-after-parse") { stopAfterParse = true; }
        else if (arg == "--stop-after-sema") { stopAfterSema = true; }
        else if (arg == "--keep-obj")   { keepObj = true; }
        else if (arg == "--max-errors" && i + 1 < argc) {
            char* end = nullptr;
            const char* v = argv[++i];
            unsigned long parsed = std::strtoul(v, &end, 10);
            if (end == v) { std::cerr << "warning: invalid --max-errors value, using 100\n"; maxErrors = 100; }
            else maxErrors = static_cast<uint32_t>(parsed);
        }
        else if (arg == "-O0" || arg == "-O1" || arg == "-O2" || arg == "-O3" || arg == "-Os" || arg == "-Oz") { }
        else if (arg == "-g" || arg == "--debug") { }
        else if (arg == "--coverage") { }
        else if (arg == "-o" && i + 1 < argc) { outputFile = argv[++i]; }
        else if (arg == "-j" && i + 1 < argc) { ++i; }
        else if (arg == "--target" && i + 1 < argc) { ++i; }
        else if (arg == "--sysroot" && i + 1 < argc) { ++i; }
        else if ((arg == "-L" || arg == "--lib-path") && i + 1 < argc) { ++i; }
        else if ((arg == "-l" || arg == "--link") && i + 1 < argc) { ++i; }
        else if (arg.size() > 2 && arg.substr(0, 2) == "-L") { }
        else if (arg.size() > 2 && arg.substr(0, 2) == "-l") { }
        else if (arg[0] != '-') { inputFile = arg; }
        else {
            std::cerr << "unknown option: " << arg << '\n';
            return 1;
        }
    }

    if (inputFile.empty()) {
        std::cerr << "error: no input file specified\n";
        return 1;
    }

    auto sourceResult = vyx::readSourceFile(inputFile);
    if (!sourceResult) {
        std::cerr << "error: " << sourceResult.error() << '\n';
        return 1;
    }
    auto& source = *sourceResult;

    // Incremental cache check
    fs::path inputPath(inputFile);
    auto sourceHash = std::hash<std::string>{}(source);
    // Cache key derives purely from the running binary's mtime+size (mixed in
    // below). We deliberately do NOT embed `__DATE__`/`__TIME__` because those
    // expand at TU-build-time and make every `vyxc.exe` rebuild produce a
    // different hash even when the rebuild is otherwise byte-identical, which
    // in turn makes main.cpp's object file nondeterministic. The mtime+size of
    // the vyxc.exe we are running already changes on every successful link, so
    // it captures the "compiler changed" signal without per-build drift.
    size_t compilerVersionHash = std::hash<std::string>{}("vyxc-1.0.0");
    {
        fs::path selfPath;
#ifdef _WIN32
        char buf[MAX_PATH];
        DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
        if (n > 0 && n < MAX_PATH) selfPath = buf;
#else
        std::error_code ecSym;
        selfPath = fs::read_symlink("/proc/self/exe", ecSym);
#endif
        if (!selfPath.empty() && fs::exists(selfPath)) {
            std::error_code ec;
            auto mtime = fs::last_write_time(selfPath, ec);
            if (!ec) {
                auto tp = mtime.time_since_epoch().count();
                auto sz = fs::file_size(selfPath, ec);
                if (!ec) {
                    std::string binKey = std::to_string(tp) + ":" + std::to_string(sz);
                    compilerVersionHash ^= std::hash<std::string>{}(binKey) +
                                           0x9e3779b9 + (compilerVersionHash << 6) +
                                           (compilerVersionHash >> 2);
                }
            }
        }
    }
    fs::path cacheDir = inputPath.parent_path() / ".cache";
    fs::create_directories(cacheDir);
    fs::path cachePath = cacheDir / (inputPath.stem().string() + ".vyx.cache");

    auto cachedFuncHashes = loadFunctionCache(cachePath.string());
    {
        auto compilerIt = cachedFuncHashes.find("__compiler");
        if (compilerIt != cachedFuncHashes.end() && compilerIt->second != compilerVersionHash)
            cachedFuncHashes.clear();

        auto wholeIt = cachedFuncHashes.find("__whole_file");
        if (wholeIt != cachedFuncHashes.end() && wholeIt->second != sourceHash) {
            cachedFuncHashes.clear();
        } else if (wholeIt != cachedFuncHashes.end() && wholeIt->second == sourceHash) {
            bool importsChanged = false;
            for (auto& [key, hash] : cachedFuncHashes) {
                if (!key.starts_with("__import")) continue;
                auto tabPos = key.find(CACHE_SEP);
                std::string importPath = (tabPos != std::string::npos) ? key.substr(tabPos + 1) : key.substr(8);
                if (fs::exists(importPath)) {
                    auto impResult = vyx::readSourceFile(importPath);
                    if (impResult && std::hash<std::string>{}(*impResult) != hash)
                        importsChanged = true;
                } else {
                    importsChanged = true;
                }
            }
            if (!importsChanged) {
                fs::path targetDir = inputPath.parent_path() / "target";
                fs::path outputPath = targetDir / (inputPath.stem().string() + ".exe");
                if (fs::exists(outputPath)) {
                    // If an explicit -o target was requested and it differs from the
                    // cached path, copy the cached exe so callers (e.g. test runners
                    // that use -o /tmp/...) always get an up-to-date binary at the
                    // requested location.
                    if (!outputFile.empty() && fs::path(outputFile) != outputPath) {
                        std::error_code ec;
                        fs::copy_file(outputPath, outputFile,
                                      fs::copy_options::overwrite_existing, ec);
                        if (ec) {
                            std::cerr << "warning: could not copy cached binary to '"
                                      << outputFile << "': " << ec.message() << '\n';
                        }
                    }
                    std::cerr << "up-to-date: " << inputFile << '\n';
                    return 0;
                }
            }
        }
    }
    std::string baseName = inputPath.stem().string();

    if (outputFile.empty()) {
        fs::path targetDir = inputPath.parent_path() / "target";
        switch (emitMode) {
            case EmitMode::IR:         outputFile = (cacheDir / (baseName + ".ll")).string(); break;
            case EmitMode::Object:     outputFile = (cacheDir / (baseName + ".obj")).string(); break;
            case EmitMode::Executable: outputFile = (targetDir / (baseName + ".exe")).string(); break;
            case EmitMode::Run:        outputFile = (cacheDir / (baseName + ".exe")).string(); break;
            case EmitMode::DLL:        outputFile = (targetDir / (baseName + ".dll")).string(); break;
            case EmitMode::StaticLib:  outputFile = (targetDir / (baseName + ".lib")).string(); break;
        }
        if (emitMode == EmitMode::Executable || emitMode == EmitMode::DLL || emitMode == EmitMode::StaticLib)
            fs::create_directories(targetDir);
    }

    vyx::DiagnosticsEngine diag;
    diag.setSource(source);

    vyx::Lexer lexer(source, inputFile, diag);
    auto tokens = lexer.tokenizeAll();

    if (dumpTokens) {
        for (auto& tok : tokens) {
            std::cerr << tok.location.toString() << " "
                      << static_cast<int>(tok.kind) << " '"
                      << tok.text << "'\n";
        }
    }
    if (diag.hasErrors()) {
        std::cerr << "lexer reported " << diag.errorCount() << " error(s)\n";
        return 1;
    }

    vyx::Parser parser(std::move(tokens), diag);
    auto unit = parser.parseTranslationUnit(inputFile);
    if (diag.hasErrors()) {
        std::cerr << "parser reported " << diag.errorCount() << " error(s)\n";
        return 1;
    }

    if (stopAfterParse) {
        return 0;
    }

    // Build module registry and resolve imports
    fs::path exeDir = fs::path(argv[0]).has_parent_path()
        ? fs::path(argv[0]).parent_path() : fs::current_path();

    vyx::ModuleRegistry modRegistry;
    modRegistry.scanDirectories(vyx::moduleSearchRoots(inputPath, exeDir));
    {
        auto depDir = fs::current_path() / ".vyx_deps";
        if (fs::is_directory(depDir)) modRegistry.scanDirectory(depDir);
    }

    vyx::ImportResolver resolver(diag, modRegistry);
    resolver.resolve(unit, inputPath);

    vyx::Sema sema(diag);
    sema.analyze(unit);
    if (diag.hasErrors()) {
        std::cerr << "semantic analysis reported " << diag.errorCount() << " error(s)\n";
        return 1;
    }

    if (stopAfterSema) {
        return 0;
    }

    // Monomorphization: instantiate all reachable generic uses before CodeGen.
    // Roots = all top-level Function decls (main + every exported/free function).
    {
        std::vector<const vyx::Decl*> monoRoots;
        monoRoots.reserve(unit.declarations.size());
        for (auto& decl : unit.declarations) {
            if (decl && decl->kind == vyx::DeclKind::Function)
                monoRoots.push_back(decl.get());
        }
        vyx::Monomorphize mono(diag);
        mono.setScheduler(&sema.getMonoSchedulerMut());
        // P2b Wave 3+: seed Mono with the assoc-type template registry so it
        // can resolve `T::Item` when T is bound to a generic class like Iter<i32>.
        mono.setAssocTemplateMap(sema.getAssocTemplateMap());
        // R5 step 3: hand Mono the lang-item registry so its VyxType →
        // TypeAnnotation reverse path names Option/Result via the registered
        // stdlib decls rather than the hard-coded literals.
        mono.setLangItems(&sema.getLangItems());
        // Error-recovery: pass --max-errors cap into Mono so it stops
        // enqueuing new analysis once N distinct failures are recorded.
        mono.setMaxErrors(maxErrors);
        mono.run(unit, monoRoots);
        if (diag.hasErrors()) {
            uint32_t distinct = mono.distinctFailureCount();
            uint32_t sites    = mono.totalFailureSiteCount();
            uint32_t collapsed = (sites > distinct) ? (sites - distinct) : 0;
            if (distinct > 0) {
                if (collapsed > 0) {
                    std::cerr << "error: template analysis completed with "
                              << distinct << " distinct failure(s) ("
                              << collapsed << " call site(s) collapsed via dedup)\n";
                } else {
                    std::cerr << "error: template analysis completed with "
                              << distinct << " distinct failure(s)\n";
                }
            } else {
                std::cerr << "monomorphization reported " << diag.errorCount() << " error(s)\n";
            }
            // Continue to CodeGen for maximal error harvest — CodeGen gracefully
            // skips monomorphs whose Sema/Mono entry is missing. This lets
            // users see downstream errors (e.g. call-site type mismatches) in
            // the same build without re-running after fixing generic failures.
            // We do NOT return 1 here; we fall through and let CodeGen run.
            // The driver returns non-zero only after CodeGen also completes.
        }
    }

    // Code generation
    std::string targetTriple;
    std::string sysroot;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--target" && i + 1 < argc) targetTriple = argv[i + 1];
        if (std::string(argv[i]) == "--sysroot" && i + 1 < argc) sysroot = argv[i + 1];
    }
    if (!targetTriple.empty() && sysroot.empty()) {
        if (targetTriple.find("linux") != std::string::npos) {
            for (auto& p : {"/usr/x86_64-linux-gnu", "/usr/aarch64-linux-gnu",
                            "/usr/lib/gcc-cross", "/usr/sysroot"}) {
                if (fs::exists(p)) { sysroot = p; break; }
            }
        }
        if (!sysroot.empty())
            std::cerr << "auto-detected sysroot: " << sysroot << '\n';
    }
    vyx::CodeGen codegen(diag, baseName, targetTriple);
    // R5 stage 2: expose stdlib lang-item registry so CodeGen recognises
    // Option / Result slots by registered decl rather than hard-coded names.
    codegen.setLangItems(&sema.getLangItems());

    bool debugMode = false, coverageMode = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-g" || a == "--debug") debugMode = true;
        if (a == "--coverage") coverageMode = true;
    }
    if (debugMode) codegen.enableDebugInfo(inputFile);
    if (coverageMode) codegen.enableCoverage();

    codegen.generate(unit);
    if (diag.hasErrors()) {
        std::cerr << "code generation reported " << diag.errorCount() << " error(s)\n";
        return 1;
    }

    int optLevel = 2;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "-Os") optLevel = 4;
        if (std::string(argv[i]) == "-Oz") optLevel = 5;
        if (std::string(argv[i]) == "-O0") optLevel = 0;
        if (std::string(argv[i]) == "-O1") optLevel = 1;
        if (std::string(argv[i]) == "-O3") optLevel = 3;
    }
    codegen.optimize(optLevel);

    // Collect user -L/-l flags
    std::vector<std::string> userLibPaths, userLinkLibs;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if ((a == "-L" || a == "--lib-path") && i + 1 < argc) userLibPaths.push_back(argv[++i]);
        else if (a.size() > 2 && a.substr(0, 2) == "-L") userLibPaths.push_back(a.substr(2));
        else if ((a == "-l" || a == "--link") && i + 1 < argc) userLinkLibs.push_back(argv[++i]);
        else if (a.size() > 2 && a.substr(0, 2) == "-l") userLinkLibs.push_back(a.substr(2));
    }

    switch (emitMode) {
        case EmitMode::IR:
            if (!codegen.emitIR(outputFile)) return 1;
            std::cerr << "wrote " << outputFile << '\n';
            break;

        case EmitMode::Object:
            if (!codegen.emitObject(outputFile)) return 1;
            std::cerr << "wrote " << outputFile << '\n';
            break;

        case EmitMode::Executable:
        case EmitMode::DLL:
        case EmitMode::StaticLib: {
            vyx::BuildTarget bt = vyx::BuildTarget::Executable;
            if (emitMode == EmitMode::DLL) bt = vyx::BuildTarget::SharedLib;
            if (emitMode == EmitMode::StaticLib) bt = vyx::BuildTarget::StaticLib;

            bool isLinux = targetTriple.find("linux") != std::string::npos;
            bool isMacOS = targetTriple.find("apple") != std::string::npos ||
                           targetTriple.find("darwin") != std::string::npos;
            std::string objExt = (isLinux || isMacOS) ? ".o" : ".obj";
            std::string objFile = (cacheDir / (baseName + objExt)).string();
            if (!codegen.emitObject(objFile)) return 1;

            // Honour user-specified `-o <path>` by feeding its directory and
            // stem into LinkJob; Linker::buildOutputPath uses outputDir+baseName
            // and would otherwise drop the .exe in cwd.
            fs::path outPath(outputFile);
            std::string outDir = outPath.has_parent_path() ? outPath.parent_path().string() : std::string{};
            std::string outBase = outPath.stem().string();
            if (outBase.empty()) outBase = baseName;
            if (!outDir.empty()) fs::create_directories(outDir);

            vyx::LinkJob job;
            job.type = bt;
            job.baseName = outBase;
            job.outputDir = outDir;
            job.objFiles = {objFile};
            job.linkLibs = userLinkLibs;
            job.libPaths = userLibPaths;
            job.targetTriple = targetTriple;
            job.compilerDir = fs::path(argv[0]).parent_path();

#ifdef _WIN32
            if (targetTriple.empty())
                job.linkLibs.push_back("ws2_32");
#endif

            // P3-B3: auto-link LLVM-C if the module references LLVM-C API
            // symbols. Lets `compiler.exe foo.vyx` self-host without
            // requiring a Vyx.toml-driven build (vyxc's project path
            // already injects -lLLVM-C via toml, but the single-file path
            // never reads toml — it has to detect the dependency from IR).
            bool needsLlvmC = codegen.referencesLlvmCRuntime();
            if (needsLlvmC) {
                bool already = false;
                for (auto& l : job.linkLibs) if (l == "LLVM-C") { already = true; break; }
                if (!already) job.linkLibs.push_back("LLVM-C");
                fs::path clangLibDir = job.compilerDir / "clang" / "lib";
                if (!fs::exists(clangLibDir / "LLVM-C.lib"))
                    clangLibDir = fs::current_path() / "clang" / "lib";
                if (!fs::exists(clangLibDir / "LLVM-C.lib"))
                    clangLibDir = job.compilerDir.parent_path() / "clang" / "lib";
                if (fs::exists(clangLibDir / "LLVM-C.lib")) {
                    bool havePath = false;
                    auto clangLibStr = clangLibDir.string();
                    for (auto& p : job.libPaths) if (p == clangLibStr) { havePath = true; break; }
                    if (!havePath) job.libPaths.push_back(clangLibStr);
                } else {
                    std::cerr << "warning: LLVM-C symbols referenced but LLVM-C.lib not found "
                                 "(searched compilerDir/clang/lib, cwd/clang/lib, parent/clang/lib)\n";
                }
            }

            if (vyx::Linker::link(job) != 0) return 1;
            std::cerr << "wrote " << outputFile << '\n';
            if (!keepObj) fs::remove(objFile);

            // P3-B3: drop LLVM-C.dll next to the produced exe so it can run
            // without PATH tweaks. The lib above resolves linker symbols at
            // build time; the dll is the runtime side of the same coin.
            if (needsLlvmC && bt == vyx::BuildTarget::Executable) {
                fs::path dllSrc = job.compilerDir / "clang" / "bin" / "LLVM-C.dll";
                if (!fs::exists(dllSrc)) dllSrc = fs::current_path() / "clang" / "bin" / "LLVM-C.dll";
                if (!fs::exists(dllSrc))
                    dllSrc = job.compilerDir.parent_path() / "clang" / "bin" / "LLVM-C.dll";
                if (fs::exists(dllSrc)) {
                    fs::path exePath(outputFile);
                    fs::path dllDst = exePath.parent_path().empty()
                        ? fs::path("LLVM-C.dll")
                        : exePath.parent_path() / "LLVM-C.dll";
                    std::error_code ec;
                    fs::copy_file(dllSrc, dllDst,
                                  fs::copy_options::overwrite_existing, ec);
                    if (ec) {
                        std::cerr << "warning: could not copy LLVM-C.dll to '"
                                  << dllDst.string() << "': " << ec.message() << '\n';
                    }
                }
            }

            writeFunctionCache(cachePath.string(), compilerVersionHash, sourceHash,
                               resolver.importedFiles(), inputPath, source, unit);
            break;
        }

        case EmitMode::Run: {
            std::string objFile = (cacheDir / (baseName + ".obj")).string();
            if (!codegen.emitObject(objFile)) return 1;

            auto clangExe = vyx::Linker::findClang(fs::path(argv[0]).parent_path());
#ifdef _WIN32
            std::string linkCmd = "\"\"" + clangExe.string() + "\" \"" + objFile +
                "\" -o \"" + outputFile + "\" -lws2_32 -Wl,/subsystem:console\"";
#else
            std::string linkCmd = clangExe.string() + " -no-pie " + objFile + " -o " + outputFile;
#endif
            int linkRet = std::system(linkCmd.c_str());
            if (linkRet != 0) { std::cerr << "link failed\n"; return 1; }

#ifdef _WIN32
            int runRet = std::system(("\"\"" + outputFile + "\"\"").c_str());
#else
            int runRet = std::system(outputFile.c_str());
#endif
            fs::remove(objFile);
            fs::remove(outputFile);
            return runRet;
        }
    }

    return 0;
}
