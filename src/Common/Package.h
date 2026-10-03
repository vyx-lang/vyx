#pragma once
#include "SemVer.h"
#include <string>
#include <vector>
#include <map>
#include <optional>

namespace vyx {

enum class BuildTarget { Executable, SharedLib, StaticLib };

struct Dependency {
    std::string name;
    std::string source;     // "github.com/user/repo" or local path
    std::string version;    // "0.1.0" or "main" (branch) or tag
    std::string localPath;  // resolved local path after install

    bool isGitHub() const { return source.find("github.com") != std::string::npos; }

    std::string gitUrl() const {
        if (source.find("https://") == 0 || source.find("git@") == 0) return source;
        return "https://" + source + ".git";
    }

    std::string installDir() const {
        return ".vyx_deps/" + name;
    }
};

struct TargetConfig {
    std::string name;
    BuildTarget type = BuildTarget::Executable;
    std::string entry;
    std::vector<std::string> sources;
    std::vector<std::string> linkLibs;
    std::vector<std::string> libPaths;
    std::vector<std::string> includePaths;
    std::vector<std::string> cflags;
    std::vector<std::string> cxxflags;
    std::vector<std::string> dependsOn;
    std::vector<std::string> prebuild;
    std::vector<std::string> postbuild;
    std::string outputDir;
    std::vector<std::string> linkOrder;
    std::string cCompiler;
    std::string cxxCompiler;
    std::string ar;
};

struct PackageConfig {
    std::string name;
    std::string version;
    std::string entryPoint = "main.vyx";
    int threads = 0; // 0 = auto (hardware concurrency)
    std::vector<TargetConfig> targets;
    std::map<std::string, std::string> dependencies;
    std::vector<Dependency> deps;
    std::map<std::string, std::string> registries;
    std::vector<std::string> prebuild;
    std::vector<std::string> postbuild;
    std::string outputDir = "target";
    std::string cacheDir = ".cache";
    std::map<std::string, std::string> scripts;

    std::string defaultRegistry() const {
        auto it = registries.find("default");
        return it != registries.end() ? it->second : "https://registry.vyx.dev";
    }

    int getThreadCount() const;
    static std::optional<PackageConfig> load(const std::string& path,
                                             const std::string& targetTriple = "");
    std::string toString() const;
};

} // namespace vyx
