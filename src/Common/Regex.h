#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <cstdint>

namespace vyx {

enum class RegexNodeKind {
    Literal,    // exact character
    AnyChar,    // .
    CharClass,  // [a-z]
    Repeat,     // *, +, ?
    Sequence,   // ab
    Alternation,// a|b
    Group,      // (a)
    Anchor,     // ^ $
};

struct RegexNode {
    RegexNodeKind kind;
    char ch = 0;
    bool negated = false;     // [^...] negated class
    std::vector<std::pair<char, char>> ranges; // for char class
    std::unique_ptr<RegexNode> left;
    std::unique_ptr<RegexNode> right;
    int32_t minRepeat = 0;    // 0 for *, 1 for +
    int32_t maxRepeat = -1;   // -1 for unlimited
    bool isStart = false;     // ^
    bool isEnd = false;       // $
};

class RegexCompiler {
public:
    static std::unique_ptr<RegexNode> parse(std::string_view pattern);
    static bool match(const RegexNode& node, std::string_view text);
    static std::vector<uint8_t> compileToDFA(std::string_view pattern);

private:
    static std::unique_ptr<RegexNode> parseExpr(std::string_view& s);
    static std::unique_ptr<RegexNode> parseTerm(std::string_view& s);
    static std::unique_ptr<RegexNode> parseAtom(std::string_view& s);
    static std::unique_ptr<RegexNode> parseCharClass(std::string_view& s);

    static bool matchNode(const RegexNode& node, std::string_view text, size_t& pos);
    static bool matchRepeat(const RegexNode& node, std::string_view text, size_t& pos);
};

class SimpleRegexMatcher {
public:
    explicit SimpleRegexMatcher(std::string_view pattern);
    bool match(std::string_view text) const;

private:
    bool matchHere(std::string_view pattern, std::string_view text) const;
    bool matchStar(char c, std::string_view pattern, std::string_view text) const;
    bool matchPlus(char c, std::string_view pattern, std::string_view text) const;

    std::string pattern_;
    bool anchorStart_ = false;
    bool anchorEnd_ = false;
    [[maybe_unused]] bool caseInsensitive_ = false;
};

} // namespace vyx
