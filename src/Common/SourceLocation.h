#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <format>
#include <deque>

namespace vyx {

struct SourceLocation {
    std::string_view filename;
    uint32_t line   = 1;
    uint32_t column = 1;
    uint32_t offset = 0;

    // Intern a filename — the returned string_view is valid for the program's lifetime.
    // Filenames are deduplicated; repeated calls with the same name return the same view.
    static std::string_view intern(std::string_view name) {
        static std::deque<std::string> pool;
        for (const auto& s : pool) {
            if (s == name) return s;
        }
        pool.emplace_back(name);
        return pool.back();
    }

    std::string toString() const {
        return std::format("{}:{}:{}", filename, line, column);
    }
};

struct SourceRange {
    SourceLocation begin;
    SourceLocation end;
};

} // namespace vyx
