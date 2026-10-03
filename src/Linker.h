#pragma once

#include "Common/Package.h"

#include <filesystem>
#include <string>
#include <vector>

namespace vyx {

// Unified linker abstraction that handles platform-specific details.
struct LinkJob {
    BuildTarget type = BuildTarget::Executable;
    std::string baseName;
    std::string outputDir;
    std::vector<std::string> objFiles;
    std::vector<std::string> linkLibs;
    std::vector<std::string> libPaths;
    std::vector<std::string> linkOrder;
    std::string targetTriple;
    std::string sysroot;
    std::string cxxCompiler;
    std::string ar;
    std::filesystem::path compilerDir;
};

class Linker {
public:
    static std::filesystem::path findLldLink(const std::filesystem::path& compilerDir);
    static std::filesystem::path findLlvmAr(const std::filesystem::path& compilerDir);
    static std::filesystem::path findClang(const std::filesystem::path& compilerDir);

    static std::string buildOutputPath(const LinkJob& job);
    static std::string normalizeLinkLibName(const std::string& lib);
    static std::string buildLinkCommand(const LinkJob& job);
    static int link(const LinkJob& job);
};

} // namespace vyx
