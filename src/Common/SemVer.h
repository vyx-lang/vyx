#pragma once
#include <string>
#include <compare>
#include <charconv>

namespace vyx {

struct SemVer {
    int major = 0, minor = 0, patch = 0;

    static SemVer parse(const std::string& str) {
        SemVer v;
        const char* p = str.c_str();
        const char* end = p + str.size();
        auto [ptr1, ec1] = std::from_chars(p, end, v.major);
        if (ec1 != std::errc{} || ptr1 >= end || *ptr1 != '.') return v;
        auto [ptr2, ec2] = std::from_chars(ptr1 + 1, end, v.minor);
        if (ec2 != std::errc{} || ptr2 >= end || *ptr2 != '.') return v;
        std::from_chars(ptr2 + 1, end, v.patch);
        return v;
    }

    bool isCompatible(const SemVer& required) const {
        if (major != required.major) return false;
        if (minor < required.minor) return false;
        if (minor == required.minor && patch < required.patch) return false;
        return true;
    }

    // ^1.2.3 means >=1.2.3 <2.0.0 (major-compatible)
    bool satisfiesCaret(const SemVer& req) const {
        if (req.major == 0) {
            if (req.minor == 0) return major == 0 && minor == 0 && patch == req.patch;
            return major == 0 && minor == req.minor && patch >= req.patch;
        }
        return major == req.major && !(*this < req);
    }

    // ~1.2.3 means >=1.2.3 <1.3.0 (minor-compatible)
    bool satisfiesTilde(const SemVer& req) const {
        return major == req.major && minor == req.minor && patch >= req.patch;
    }

    static bool satisfiesRange(const SemVer& ver, const std::string& range) {
        if (range.empty()) return true;
        if (range[0] == '^') return ver.satisfiesCaret(parse(range.substr(1)));
        if (range[0] == '~') return ver.satisfiesTilde(parse(range.substr(1)));
        if (range.starts_with(">=")) {
            auto req = parse(range.substr(2));
            return !(ver < req);
        }
        return ver.isCompatible(parse(range));
    }

    auto operator<=>(const SemVer&) const = default;

    std::string toString() const {
        return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
    }
};

} // namespace vyx
