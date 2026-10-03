#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include <map>

namespace vyx {

class CompileCache {
public:
    explicit CompileCache(const std::string& cacheDir);
    bool isUpToDate(const std::string& sourceFile, const std::string& objFile) const;
    void recordCompilation(const std::string& sourceFile, const std::string& objFile);
    std::vector<std::string> getStaleFiles(const std::vector<std::string>& sourceFiles) const;

private:
    std::string cacheDir_;
    std::map<std::string, uint64_t> timestamps_;
    void loadTimestamps();
    void saveTimestamps() const;
};

} // namespace vyx
