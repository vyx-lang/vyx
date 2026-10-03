#include "Linker.h"

#include <cstdlib>
#include <iostream>

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

fs::path Linker::findLldLink(const fs::path& compilerDir) {
#ifdef _WIN32
    fs::path p = compilerDir / "clang" / "bin" / "lld-link.exe";
    if (fs::exists(p)) return p;
    p = fs::current_path() / "clang" / "bin" / "lld-link.exe";
    if (fs::exists(p)) return p;
    return "lld-link.exe";
#else
    fs::path p = compilerDir / "clang" / "bin" / "ld.lld";
    if (fs::exists(p)) return p;
    p = fs::current_path() / "clang" / "bin" / "ld.lld";
    if (fs::exists(p)) return p;
    return "ld.lld";
#endif
}

fs::path Linker::findLlvmAr(const fs::path& compilerDir) {
#ifdef _WIN32
    fs::path p = compilerDir / "clang" / "bin" / "llvm-ar.exe";
    if (fs::exists(p)) return p;
    p = fs::current_path() / "clang" / "bin" / "llvm-ar.exe";
    if (fs::exists(p)) return p;
    return "llvm-ar.exe";
#else
    fs::path p = compilerDir / "clang" / "bin" / "llvm-ar";
    if (fs::exists(p)) return p;
    p = fs::current_path() / "clang" / "bin" / "llvm-ar";
    if (fs::exists(p)) return p;
    return "llvm-ar";
#endif
}

fs::path Linker::findClang(const fs::path& compilerDir) {
#ifdef _WIN32
    fs::path p = compilerDir / "clang" / "bin" / "clang.exe";
    if (fs::exists(p)) return p;
    p = fs::current_path() / "clang" / "bin" / "clang.exe";
    if (fs::exists(p)) return p;
    return "clang.exe";
#else
    fs::path p = compilerDir / "clang" / "bin" / "clang";
    if (fs::exists(p)) return p;
    p = fs::current_path() / "clang" / "bin" / "clang";
    if (fs::exists(p)) return p;
    return "clang";
#endif
}

std::string Linker::buildOutputPath(const LinkJob& job) {
    std::string outDir = job.outputDir;
    if (!outDir.empty() && outDir.back() != '/' && outDir.back() != '\\')
        outDir += '/';

    bool isLinux  = job.targetTriple.find("linux")  != std::string::npos;
    bool isMacOS  = job.targetTriple.find("apple")  != std::string::npos ||
                    job.targetTriple.find("darwin") != std::string::npos;
    bool isNative = job.targetTriple.empty();

    switch (job.type) {
        case BuildTarget::Executable:
#ifdef _WIN32
            if (isNative || (!isLinux && !isMacOS))
                return outDir + job.baseName + ".exe";
#endif
            return outDir + job.baseName;

        case BuildTarget::SharedLib:
#ifdef _WIN32
            if (isNative || (!isLinux && !isMacOS))
                return outDir + job.baseName + ".dll";
#endif
            if (isMacOS) return outDir + "lib" + job.baseName + ".dylib";
            return outDir + "lib" + job.baseName + ".so";

        case BuildTarget::StaticLib:
#ifdef _WIN32
            if (isNative || (!isLinux && !isMacOS))
                return outDir + job.baseName + ".lib";
#endif
            return outDir + "lib" + job.baseName + ".a";
    }
    return outDir + job.baseName;
}

std::string Linker::normalizeLinkLibName(const std::string& lib) {
    std::string name = lib;
    if (name.empty()) return name;
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos)
        name = fs::path(name).filename().string();

    if (name.rfind("lib", 0) == 0 && name.size() > 3)
        name.erase(0, 3);

    while (true) {
        auto dot = name.rfind('.');
        if (dot == std::string::npos) break;
        std::string suffix = name.substr(dot);
        if (suffix == ".dll" || suffix == ".lib" || suffix == ".a" ||
            suffix == ".so" || suffix == ".dylib" || suffix == ".dll.a") {
            name = name.substr(0, dot);
            continue;
        }
        break;
    }

    auto stripSuffix = [&](const char* suffix) {
        std::string s(suffix);
        if (name.size() >= s.size() && name.compare(name.size() - s.size(), s.size(), s) == 0) {
            name.resize(name.size() - s.size());
            return true;
        }
        return false;
    };
    stripSuffix(".dylib") || stripSuffix(".so") || stripSuffix(".a") || stripSuffix(".lib") || stripSuffix(".dll");
    return name;
}

std::string Linker::buildLinkCommand(const LinkJob& job) {
    std::string outputFile = buildOutputPath(job);
    std::string allObjs;
    for (auto& obj : job.objFiles) allObjs += " " + quoteArg(obj);

    auto& libsToLink = job.linkOrder.empty() ? job.linkLibs : job.linkOrder;

    bool isNative = job.targetTriple.empty();
    bool isLinux  = isNative || job.targetTriple.find("linux")  != std::string::npos;
    bool isMacOS  = job.targetTriple.find("apple")  != std::string::npos ||
                    job.targetTriple.find("darwin") != std::string::npos;
    bool isWasm   = job.targetTriple.find("wasm")   != std::string::npos;
    bool isAndroid= job.targetTriple.find("android")!= std::string::npos;
    bool isArm    = job.targetTriple.find("aarch64")!= std::string::npos ||
                    job.targetTriple.find("arm")    != std::string::npos;

    // Static library: always use llvm-ar
    if (job.type == BuildTarget::StaticLib) {
        auto ar = job.ar.empty() ? findLlvmAr(job.compilerDir).string() : job.ar;
        return quoteArg(ar) + " rcs " + quoteArg(outputFile) + allObjs;
    }

    // Platform-specific linking
#ifdef _WIN32
    if (job.targetTriple.empty() || (!isLinux && !isMacOS && !isWasm && !isAndroid)) {
        auto lld = findLldLink(job.compilerDir);
        std::string extraLibs;
        for (auto& lib : libsToLink) extraLibs += " /defaultlib:" + lib;
        for (auto& path : job.libPaths) extraLibs += " /libpath:\"" + path + "\"";

        std::string cmd = lld.string()
            + " /defaultlib:libcmt /defaultlib:libucrt"
            + " /defaultlib:libvcruntime /defaultlib:legacy_stdio_definitions"
            + " /defaultlib:kernel32";

        if (job.type == BuildTarget::Executable) {
            cmd += " /entry:mainCRTStartup /subsystem:console /STACK:67108864";
        } else {
            cmd += " /dll";
            fs::path implib = fs::path(outputFile).replace_extension(".lib");
            cmd += " /implib:" + quoteArg(implib.string());
        }

        cmd += extraLibs + allObjs + " /out:" + outputFile;
        return cmd;
    }
#endif

    std::string extraFlags;
    for (auto& path : job.libPaths) extraFlags += " -L" + quoteArg(path);
    for (auto& lib : libsToLink) extraFlags += " -l" + normalizeLinkLibName(lib);

    if (isWasm) {
        return "wasm-ld" + allObjs + " -o " + quoteArg(outputFile) +
               " --no-entry --export-all" + extraFlags;
    }

    std::string cxx = job.cxxCompiler.empty() ? "clang++" : job.cxxCompiler;
    std::string cmd = quoteArg(cxx);
    if (!job.targetTriple.empty()) cmd += " --target=" + job.targetTriple;
    if (!job.sysroot.empty()) cmd += " --sysroot=" + quoteArg(job.sysroot);
    if (job.type == BuildTarget::SharedLib) cmd += " -shared";
    if (job.type == BuildTarget::Executable && isLinux) cmd += " -no-pie";
    cmd += allObjs + " -o " + quoteArg(outputFile);
    if (isAndroid) cmd += " -ldl -lm";
    cmd += extraFlags;
    return cmd;
}

int Linker::link(const LinkJob& job) {
    std::string cmd = buildLinkCommand(job);
    std::string outputFile = buildOutputPath(job);

#ifdef _WIN32
    // Windows file-lock workaround: a running .exe can be renamed but not
    // overwritten.  If the output is a locked executable (e.g. self-hosted
    // compiler rebuilding itself), rename it out of the way first.
    std::string oldFile = outputFile + ".old";
    bool didRename = false;
    if (fs::exists(outputFile)) {
        std::error_code ec;
        fs::remove(oldFile, ec);
        fs::rename(outputFile, oldFile, ec);
        if (!ec) {
            didRename = true;
        } else {
            // rename failed — target is not locked, just proceed normally
        }
    }
#endif

    std::cerr << "linking: " << job.baseName << " -> " << outputFile << '\n';
    if (std::getenv("VYX_DEBUG_LINK")) {
        std::cerr << "link cmd: " << cmd << '\n';
    }
    int ret = std::system(cmd.c_str());

#ifdef _WIN32
    if (didRename) {
        std::error_code ec;
        if (ret == 0) {
            fs::remove(oldFile, ec);
        } else {
            // Link failed — restore the original file
            fs::remove(outputFile, ec);
            fs::rename(oldFile, outputFile, ec);
        }
    }
#endif

    if (ret != 0) {
        std::cerr << "link failed for '" << job.baseName << "' (exit " << ret << ")\n";
    }
    return ret;
}

} // namespace vyx
