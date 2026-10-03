#pragma once

#include <string>
#include <vector>
#include <set>
#include <map>

namespace vyx {

class DependencyGraph {
public:
    void addDependency(const std::string& file, const std::string& dependsOn);
    std::vector<std::string> getDependencies(const std::string& file) const;
    std::vector<std::string> getDependents(const std::string& file) const;
    std::vector<std::string> topologicalOrder() const;
    bool hasCycle() const;
    void clear();

private:
    std::map<std::string, std::vector<std::string>> deps_;
    std::map<std::string, std::vector<std::string>> reverseDeps_;
    std::set<std::string> allFiles_;
};

} // namespace vyx
