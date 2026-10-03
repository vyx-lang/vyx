#include "Regex.h"
#include <cctype>

namespace vyx {

// ============================================================
//  Simple Regex Matcher (Thompson-style NFA simulation)
// ============================================================

SimpleRegexMatcher::SimpleRegexMatcher(std::string_view pattern) {
    std::string_view p = pattern;

    // Check for flags at end: /pattern/i
    // Already stripped by caller

    // Check anchors
    if (!p.empty() && p.front() == '^') {
        anchorStart_ = true;
        p = p.substr(1);
    }
    if (!p.empty() && p.back() == '$') {
        anchorEnd_ = true;
        p = p.substr(0, p.size() - 1);
    }

    pattern_ = std::string(p);
}

bool SimpleRegexMatcher::match(std::string_view text) const {
    if (anchorStart_) {
        return matchHere(pattern_, text);
    }

    // Try matching at every position
    for (size_t i = 0; i <= text.size(); ++i) {
        if (matchHere(pattern_, text.substr(i))) {
            return true;
        }
    }
    return false;
}

bool SimpleRegexMatcher::matchHere(std::string_view pattern, std::string_view text) const {
    if (pattern.empty()) {
        if (anchorEnd_) return text.empty();
        return true;
    }

    // Handle quantifiers
    if (pattern.size() >= 2 && pattern[1] == '*') {
        return matchStar(pattern[0], pattern.substr(2), text);
    }
    if (pattern.size() >= 2 && pattern[1] == '+') {
        return matchPlus(pattern[0], pattern.substr(2), text);
    }
    if (pattern.size() >= 2 && pattern[1] == '?') {
        // Optional: try with and without
        if (!text.empty() && (pattern[0] == '.' || pattern[0] == text[0])) {
            if (matchHere(pattern.substr(2), text.substr(1))) return true;
        }
        return matchHere(pattern.substr(2), text);
    }

    // Handle character classes [a-z]
    if (pattern[0] == '[') {
        size_t close = pattern.find(']');
        if (close == std::string_view::npos) return false;

        bool negated = false;
        size_t start = 1;
        if (pattern[1] == '^') { negated = true; start = 2; }

        if (text.empty()) return false;

        bool inClass = false;
        for (size_t i = start; i < close; ++i) {
            if (i + 2 < close && pattern[i + 1] == '-') {
                if (text[0] >= pattern[i] && text[0] <= pattern[i + 2]) {
                    inClass = true;
                }
                i += 2;
            } else {
                if (text[0] == pattern[i]) inClass = true;
            }
        }

        if (negated) inClass = !inClass;
        if (!inClass) return false;
        return matchHere(pattern.substr(close + 1), text.substr(1));
    }

    // Handle escape sequences
    if (pattern[0] == '\\' && pattern.size() >= 2) {
        if (text.empty()) return false;
        char expected = pattern[1];
        switch (expected) {
            case 'd': if (!std::isdigit(text[0])) return false; break;
            case 'w': if (!std::isalnum(text[0]) && text[0] != '_') return false; break;
            case 's': if (!std::isspace(text[0])) return false; break;
            default: if (text[0] != expected) return false; break;
        }
        return matchHere(pattern.substr(2), text.substr(1));
    }

    // Match single char
    if (!text.empty() && (pattern[0] == '.' || pattern[0] == text[0])) {
        return matchHere(pattern.substr(1), text.substr(1));
    }

    return false;
}

bool SimpleRegexMatcher::matchStar(char c, std::string_view pattern, std::string_view text) const {
    // Match zero or more of c
    for (size_t i = 0; ; ++i) {
        if (matchHere(pattern, text.substr(i))) return true;
        if (i >= text.size()) break;
        if (c != '.' && text[i] != c) break;
    }
    return false;
}

bool SimpleRegexMatcher::matchPlus(char c, std::string_view pattern, std::string_view text) const {
    // Match one or more of c
    if (text.empty()) return false;
    if (c != '.' && text[0] != c) return false;
    return matchStar(c, pattern, text.substr(1));
}

// ============================================================
//  Compile-time DFA generation (simplified)
// ============================================================

std::vector<uint8_t> RegexCompiler::compileToDFA(std::string_view pattern) {
    // Generate a bytecode representation for the pattern
    // Bytecodes:
    //  0x01 cc       - match literal char cc
    //  0x02          - match any char (.)
    //  0x03 lo hi    - match char in range [lo, hi]
    //  0x04 off      - jump (relative offset)
    //  0x05 off      - split (try both paths)
    //  0x06          - match (success)

    std::vector<uint8_t> code;

    for (size_t i = 0; i < pattern.size(); ++i) {
        char c = pattern[i];

        if (c == '.') {
            code.push_back(0x02); // any char
        } else if (c == '[') {
            // Character class
            size_t close = pattern.find(']', i);
            if (close != std::string_view::npos) {
                for (size_t j = i + 1; j < close; ++j) {
                    if (j + 2 < close && pattern[j + 1] == '-') {
                        code.push_back(0x03);
                        code.push_back(static_cast<uint8_t>(pattern[j]));
                        code.push_back(static_cast<uint8_t>(pattern[j + 2]));
                        j += 2;
                    } else {
                        code.push_back(0x01);
                        code.push_back(static_cast<uint8_t>(pattern[j]));
                    }
                }
                i = close;
            }
        } else if (c == '\\' && i + 1 < pattern.size()) {
            code.push_back(0x01);
            code.push_back(static_cast<uint8_t>(pattern[++i]));
        } else if (c == '*' || c == '+' || c == '?') {
            // Quantifiers modify the previous instruction
            // Simplified: just add a loop marker
            code.push_back(0x05);
            code.push_back(c == '*' ? 0 : (c == '+' ? 1 : 2));
        } else if (c == '^' || c == '$') {
            code.push_back(0x04);
            code.push_back(c == '^' ? 1 : 2);
        } else {
            code.push_back(0x01);
            code.push_back(static_cast<uint8_t>(c));
        }
    }

    code.push_back(0x06); // match success
    return code;
}

} // namespace vyx
