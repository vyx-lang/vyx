#include "Package.h"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

namespace vyx {

namespace fs = std::filesystem;

int PackageConfig::getThreadCount() const {
    if (threads > 0) return threads;
    int hw = static_cast<int>(std::thread::hardware_concurrency());
    return hw > 0 ? hw : 1;
}

static size_t firstNonSpace(const std::string& s) {
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c != ' ' && c != '\t') return i;
    }
    return std::string::npos;
}

static void parseCommandList(const std::string& value, std::vector<std::string>& out) {
    std::string cmd;
    bool inQuote = false;
    for (char c : value) {
        if (c == '"') { inQuote = !inQuote; continue; }
        if (c == ',' && !inQuote) {
            auto s = firstNonSpace(cmd);
            if (s != std::string::npos) out.push_back(cmd.substr(s));
            cmd.clear();
        } else {
            cmd += c;
        }
    }
    auto s = firstNonSpace(cmd);
    if (s != std::string::npos) out.push_back(cmd.substr(s));
}

static std::string lowerCopy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

static bool pathHasLlvmHeaders(const fs::path& root) {
    std::error_code ec;
    return fs::exists(root / "include" / "llvm" / "BinaryFormat" / "Dwarf.h", ec);
}

static std::string llvmRootFromLlvmDir(const std::string& llvmDir) {
    if (llvmDir.empty()) return "";
    fs::path p = fs::path(llvmDir).lexically_normal();
    if (p.filename().string() == "llvm" && p.has_parent_path()) p = p.parent_path();
    if (p.filename().string() == "cmake" && p.has_parent_path()) p = p.parent_path();
    if (p.filename().string() == "lib" && p.has_parent_path()) p = p.parent_path();
    if (pathHasLlvmHeaders(p)) return p.string();
    return "";
}

static std::string readLlvmRootFromCache(const fs::path& cachePath) {
    std::ifstream in(cachePath);
    if (!in.is_open()) return "";
    std::string line;
    while (std::getline(in, line)) {
        constexpr const char* prefix = "LLVM_DIR:";
        if (line.rfind(prefix, 0) == 0) {
            auto eq = line.find('=');
            if (eq != std::string::npos) {
                return llvmRootFromLlvmDir(line.substr(eq + 1));
            }
        }
    }
    return "";
}

static std::string inferLlvmRoot() {
    if (const char* root = std::getenv("LLVM_ROOT")) {
        if (*root && pathHasLlvmHeaders(root)) return root;
    }
    if (const char* dir = std::getenv("LLVM_DIR")) {
        auto root = llvmRootFromLlvmDir(dir);
        if (!root.empty()) return root;
    }

    std::vector<fs::path> cacheCandidates = {
        fs::current_path() / "CMakeCache.txt",
        fs::current_path() / "cmake-build-debug" / "CMakeCache.txt",
        fs::current_path() / "build" / "CMakeCache.txt",
        fs::current_path() / ".." / "cmake-build-debug" / "CMakeCache.txt",
        fs::current_path() / ".." / "build" / "CMakeCache.txt",
    };
    for (const auto& cache : cacheCandidates) {
        auto root = readLlvmRootFromCache(cache);
        if (!root.empty()) return root;
    }

    std::vector<fs::path> rootCandidates = {
        fs::current_path() / "clang",
        fs::current_path() / ".." / "clang",
        fs::current_path() / ".." / ".." / "clang",
    };
    for (const auto& root : rootCandidates) {
        auto norm = root.lexically_normal();
        if (pathHasLlvmHeaders(norm)) return norm.string();
    }
    return "";
}

static std::string envValue(const std::string& name) {
    if (name == "LLVM_ROOT") {
        auto root = inferLlvmRoot();
        if (!root.empty()) return root;
    }
    if (const char* value = std::getenv(name.c_str())) return value;
    return "";
}

static bool isIdentChar(char c) {
    unsigned char uc = static_cast<unsigned char>(c);
    return std::isalnum(uc) || c == '_';
}

static std::string expandEnvRefs(const std::string& text) {
    std::string out;
    for (size_t i = 0; i < text.size();) {
        if (text[i] == '$') {
            if (i + 1 < text.size() && text[i + 1] == '{') {
                size_t end = text.find('}', i + 2);
                if (end != std::string::npos) {
                    auto value = envValue(text.substr(i + 2, end - (i + 2)));
                    if (!value.empty()) {
                        out += value;
                        i = end + 1;
                        continue;
                    }
                }
            } else if (i + 1 < text.size() && isIdentChar(text[i + 1])) {
                size_t end = i + 1;
                while (end < text.size() && isIdentChar(text[end])) ++end;
                auto value = envValue(text.substr(i + 1, end - (i + 1)));
                if (!value.empty()) {
                    out += value;
                    i = end;
                    continue;
                }
            }
        } else if (text[i] == '%') {
            size_t end = text.find('%', i + 1);
            if (end != std::string::npos) {
                auto value = envValue(text.substr(i + 1, end - (i + 1)));
                if (!value.empty()) {
                    out += value;
                    i = end + 1;
                    continue;
                }
            }
        }
        out += text[i++];
    }
    return out;
}

static std::string activePlatformSuffix(const std::string& targetTriple) {
    auto t = lowerCopy(targetTriple);
    if (t.find("android") != std::string::npos) return "_android";
    if (t.find("linux") != std::string::npos) return "_linux";
    if (t.find("apple") != std::string::npos || t.find("darwin") != std::string::npos) return "_apple";
#ifdef _WIN32
    return "_windows";
#elif defined(__APPLE__)
    return "_apple";
#elif defined(__ANDROID__)
    return "_android";
#else
    return "_linux";
#endif
}

static bool stripMatchingPlatformSuffix(std::string& key, const std::string& targetTriple) {
    static constexpr const char* suffixes[] = {"_windows", "_linux", "_android", "_apple"};
    for (auto* suffix : suffixes) {
        std::string s = suffix;
        if (key.size() > s.size() && key.compare(key.size() - s.size(), s.size(), s) == 0) {
            if (s != activePlatformSuffix(targetTriple)) return false;
            key.resize(key.size() - s.size());
            return true;
        }
    }
    return true;
}

static void parseItemList(const std::string& value, std::vector<std::string>& out) {
    std::string raw = value;
    auto start = raw.find_first_not_of(" \t");
    auto end = raw.find_last_not_of(" \t");
    if (start == std::string::npos) return;
    raw = raw.substr(start, end - start + 1);
    if (raw.size() >= 2 && raw.front() == '[' && raw.back() == ']')
        raw = raw.substr(1, raw.size() - 2);

    std::string item;
    bool inQuote = false;
    auto flush = [&]() {
        auto s = item.find_first_not_of(" \t");
        auto e = item.find_last_not_of(" \t");
        if (s == std::string::npos) {
            item.clear();
            return;
        }
        std::string trimmed = item.substr(s, e - s + 1);
        if (trimmed.size() >= 2 && trimmed.front() == '"' && trimmed.back() == '"')
            trimmed = trimmed.substr(1, trimmed.size() - 2);
        if (!trimmed.empty()) out.push_back(expandEnvRefs(trimmed));
        item.clear();
    };

    for (char c : raw) {
        if (c == '"') {
            inQuote = !inQuote;
            item += c;
        } else if (!inQuote && (c == ',' || c == ' ' || c == '\t')) {
            flush();
        } else {
            item += c;
        }
    }
    flush();
}

std::optional<PackageConfig> PackageConfig::load(const std::string& path,
                                                 const std::string& targetTriple) {
    std::ifstream file(path);
    if (!file.is_open()) return std::nullopt;

    PackageConfig config;
    std::string line;
    std::string currentSection;
    TargetConfig currentTarget;
    bool inTarget = false;

    while (std::getline(file, line)) {
        size_t start = firstNonSpace(line);
        if (start == std::string::npos) continue;
        line = line.substr(start);

        if (line[0] == '#') continue;

        if (line[0] == '[') {
            if (inTarget && !currentTarget.name.empty()) {
                config.targets.push_back(std::move(currentTarget));
                currentTarget = {};
            }
            size_t end = line.find(']');
            if (end != std::string::npos) {
                currentSection = line.substr(1, end - 1);
            }
            inTarget = currentSection.find("target.") == 0;
            if (inTarget) {
                currentTarget.name = currentSection.substr(7);
            }
            continue;
        }

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);

        while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
            key.pop_back();
        start = firstNonSpace(value);
        if (start != std::string::npos) value = value.substr(start);
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        value = expandEnvRefs(value);
        if (!stripMatchingPlatformSuffix(key, targetTriple)) continue;

        if (currentSection == "package") {
            if (key == "name") config.name = value;
            else if (key == "version") config.version = value;
            else if (key == "entry") config.entryPoint = value;
        } else if (currentSection == "build") {
            if (key == "threads") {
                int v = 0;
                std::from_chars(value.c_str(), value.c_str() + value.size(), v);
                config.threads = v;
            }
            else if (key == "output_dir") config.outputDir = value;
            else if (key == "cache_dir") config.cacheDir = value;
            else if (key == "prebuild") parseCommandList(value, config.prebuild);
            else if (key == "postbuild") parseCommandList(value, config.postbuild);
        } else if (currentSection == "registries") {
            config.registries[key] = value;
        } else if (currentSection == "scripts") {
            config.scripts[key] = value;
        } else if (currentSection == "dependencies") {
            config.dependencies[key] = value;
            Dependency dep;
            dep.name = key;
            size_t atPos = value.find('@');
            if (atPos != std::string::npos) {
                dep.source = value.substr(0, atPos);
                dep.version = value.substr(atPos + 1);
            } else {
                dep.source = value;
                dep.version = "main";
            }
            config.deps.push_back(std::move(dep));
        } else if (inTarget) {
            if (key == "type") {
                if (value == "shared" || value == "dylib") currentTarget.type = BuildTarget::SharedLib;
                else if (value == "static" || value == "staticlib") currentTarget.type = BuildTarget::StaticLib;
                else currentTarget.type = BuildTarget::Executable;
            } else if (key == "entry") {
                currentTarget.entry = value;
            } else if (key == "output_dir") {
                currentTarget.outputDir = value;
            } else if (key == "cc") {
                currentTarget.cCompiler = value;
            } else if (key == "cxx") {
                currentTarget.cxxCompiler = value;
            } else if (key == "ar") {
                currentTarget.ar = value;
            } else if (key == "prebuild") {
                parseCommandList(value, currentTarget.prebuild);
            } else if (key == "postbuild") {
                parseCommandList(value, currentTarget.postbuild);
            } else if (key == "sources" || key == "libs" || key == "lib_paths"
                       || key == "depends_on" || key == "link_order"
                       || key == "include_paths" || key == "cflags" || key == "cxxflags") {
                // Multi-line array support: if value starts with '[' but doesn't
                // end with ']', keep reading lines until we find the closing ']'.
                std::string fullValue = value;
                {
                    std::string trimmed = fullValue;
                    auto ts = trimmed.find_first_not_of(" \t");
                    if (ts != std::string::npos) trimmed = trimmed.substr(ts);
                    if (!trimmed.empty() && trimmed.front() == '[' && trimmed.back() != ']') {
                        std::string nextLine;
                        while (std::getline(file, nextLine)) {
                            auto ns = firstNonSpace(nextLine);
                            if (ns == std::string::npos) continue;
                            std::string trimNext = nextLine.substr(ns);
                            if (trimNext[0] == '#') continue;
                            fullValue += " " + trimNext;
                            if (trimNext.find(']') != std::string::npos) break;
                        }
                    }
                }
                std::vector<std::string>* vec = nullptr;
                if (key == "sources") vec = &currentTarget.sources;
                else if (key == "libs") vec = &currentTarget.linkLibs;
                else if (key == "depends_on") vec = &currentTarget.dependsOn;
                else if (key == "link_order") vec = &currentTarget.linkOrder;
                else if (key == "include_paths") vec = &currentTarget.includePaths;
                else if (key == "cflags") vec = &currentTarget.cflags;
                else if (key == "cxxflags") vec = &currentTarget.cxxflags;
                else vec = &currentTarget.libPaths;
                parseItemList(fullValue, *vec);
            }
        }
    }

    if (inTarget && !currentTarget.name.empty()) {
        config.targets.push_back(std::move(currentTarget));
    }

    if (config.targets.empty()) {
        TargetConfig defaultTarget;
        defaultTarget.name = config.name;
        defaultTarget.type = BuildTarget::Executable;
        defaultTarget.entry = config.entryPoint;
        config.targets.push_back(std::move(defaultTarget));
    }

    return config;
}

std::string PackageConfig::toString() const {
    std::string result;
    result += "[package]\n";
    result += "name = \"" + name + "\"\n";
    result += "version = \"" + version + "\"\n";
    result += "entry = \"" + entryPoint + "\"\n";
    if (threads > 0 || !outputDir.empty() || !cacheDir.empty() || !prebuild.empty() || !postbuild.empty()) {
        result += "\n[build]\n";
        if (threads > 0) result += "threads = " + std::to_string(threads) + "\n";
        if (!outputDir.empty()) result += "output_dir = \"" + outputDir + "\"\n";
        if (!cacheDir.empty() && cacheDir != ".cache") result += "cache_dir = \"" + cacheDir + "\"\n";
        for (auto& cmd : prebuild) result += "prebuild = \"" + cmd + "\"\n";
        for (auto& cmd : postbuild) result += "postbuild = \"" + cmd + "\"\n";
    }
    for (auto& target : targets) {
        result += "\n[target." + target.name + "]\n";
        switch (target.type) {
            case BuildTarget::Executable: result += "type = \"executable\"\n"; break;
            case BuildTarget::SharedLib:  result += "type = \"shared\"\n"; break;
            case BuildTarget::StaticLib:  result += "type = \"static\"\n"; break;
        }
        if (!target.entry.empty()) result += "entry = \"" + target.entry + "\"\n";
        if (!target.outputDir.empty()) result += "output_dir = \"" + target.outputDir + "\"\n";
        for (auto& cmd : target.prebuild) result += "prebuild = \"" + cmd + "\"\n";
        for (auto& cmd : target.postbuild) result += "postbuild = \"" + cmd + "\"\n";
    }
    if (!scripts.empty()) {
        result += "\n[scripts]\n";
        for (auto& [k, v] : scripts) {
            result += k + " = \"" + v + "\"\n";
        }
    }
    if (!dependencies.empty()) {
        result += "\n[dependencies]\n";
        for (auto& [k, v] : dependencies) {
            result += k + " = \"" + v + "\"\n";
        }
    }
    return result;
}

} // namespace vyx
