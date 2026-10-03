#include "Lexer.h"
#include <cctype>
#include <charconv>

namespace vyx {

Lexer::Lexer(std::string_view source, std::string_view filename, DiagnosticsEngine& diag)
    : source_(source), filename_(SourceLocation::intern(filename)), diag_(diag) {}

char Lexer::peek() const {
    if (isAtEnd()) return '\0';
    return source_[pos_];
}

char Lexer::peekNext() const {
    if (pos_ + 1 >= source_.size()) return '\0';
    return source_[pos_ + 1];
}

char Lexer::advance() {
    char c = source_[pos_++];
    if (c == '\n') { ++line_; col_ = 1; }
    else { ++col_; }
    return c;
}

bool Lexer::match(char expected) {
    if (isAtEnd() || source_[pos_] != expected) return false;
    advance();
    return true;
}

bool Lexer::isAtEnd() const {
    return pos_ >= source_.size();
}

SourceLocation Lexer::currentLocation() const {
    return {filename_, startLine_, startCol_, static_cast<uint32_t>(start_)};
}

Token Lexer::makeToken(TokenKind kind) const {
    Token tok;
    tok.kind = kind;
    tok.text = source_.substr(start_, pos_ - start_);
    tok.location = {filename_, startLine_, startCol_, static_cast<uint32_t>(start_)};
    return tok;
}

Token Lexer::makeToken(TokenKind kind, std::string_view text) const {
    Token tok;
    tok.kind = kind;
    tok.text = text;
    tok.location = {filename_, startLine_, startCol_, static_cast<uint32_t>(start_)};
    return tok;
}

void Lexer::skipWhitespaceAndComments() {
    pendingDocComment_.clear();
    while (!isAtEnd()) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance();
        } else if (c == '/' && peekNext() == '/') {
            bool isDocComment = (pos_ + 2 < source_.size() && source_[pos_ + 2] == '/');
            if (isDocComment) {
                advance(); advance(); advance();
                if (!isAtEnd() && peek() == ' ') advance();
                size_t textStart = pos_;
                while (!isAtEnd() && peek() != '\n') advance();
                std::string line(source_.substr(textStart, pos_ - textStart));
                if (!pendingDocComment_.empty()) pendingDocComment_ += "\n";
                pendingDocComment_ += line;
            } else {
                while (!isAtEnd() && peek() != '\n') advance();
            }
        } else if (c == '/' && peekNext() == '*') {
            advance(); advance();
            int depth = 1;
            while (!isAtEnd() && depth > 0) {
                if (peek() == '/' && peekNext() == '*') { advance(); advance(); ++depth; }
                else if (peek() == '*' && peekNext() == '/') { advance(); advance(); --depth; }
                else advance();
            }
        } else {
            break;
        }
    }
}

Token Lexer::scanIdentifierOrKeyword() {
    while (!isAtEnd() && (std::isalnum(peek()) || peek() == '_')) advance();

    std::string_view text = source_.substr(start_, pos_ - start_);

    if (text == "true" || text == "false") {
        Token tok = makeToken(TokenKind::BoolLiteral, text);
        tok.boolValue = (text == "true");
        return tok;
    }

    auto& keywords = getKeywordMap();
    auto it = keywords.find(text);
    if (it != keywords.end()) {
        return makeToken(it->second, text);
    }

    return makeToken(TokenKind::Identifier, text);
}

Token Lexer::scanNumber() {
    bool isFloat = false;
    bool isHex = false;
    bool isBin = false;
    bool isOct = false;

    if (source_[start_] == '0' && (peek() == 'x' || peek() == 'X')) {
        advance();
        isHex = true;
        while (!isAtEnd() && std::isxdigit(peek())) advance();
    } else if (source_[start_] == '0' && (peek() == 'b' || peek() == 'B')) {
        advance();
        isBin = true;
        while (!isAtEnd() && (peek() == '0' || peek() == '1')) advance();
    } else if (source_[start_] == '0' && (peek() == 'o' || peek() == 'O')) {
        advance();
        isOct = true;
        while (!isAtEnd() && peek() >= '0' && peek() <= '7') advance();
    } else {
        while (!isAtEnd() && std::isdigit(peek())) advance();
        if (peek() == '.' && std::isdigit(peekNext())) {
            isFloat = true;
            advance();
            while (!isAtEnd() && std::isdigit(peek())) advance();
        }
        if (peek() == 'e' || peek() == 'E') {
            isFloat = true;
            advance();
            if (peek() == '+' || peek() == '-') advance();
            while (!isAtEnd() && std::isdigit(peek())) advance();
        }
    }

    std::string_view text = source_.substr(start_, pos_ - start_);

    if (isFloat) {
        Token tok = makeToken(TokenKind::FloatLiteral, text);
        auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), tok.floatValue);
        if (ec != std::errc{}) {
            diag_.error(currentLocation(), "invalid float literal '{}'", text);
        }
        return tok;
    } else {
        Token tok = makeToken(TokenKind::IntLiteral, text);
        int base = isHex ? 16 : (isBin ? 2 : (isOct ? 8 : 10));
        std::string_view digits = (isHex || isBin || isOct) ? text.substr(2) : text;
        // Parse as uint64_t then reinterpret as int64_t so we can accept
        // the full 64-bit bit pattern — this covers both u64 literals above
        // INT64_MAX (e.g. 18446744073709551614) and the i64.min magnitude
        // 9223372036854775808 that unary minus later negates. The bit
        // pattern flows through to codegen unchanged.
        uint64_t raw = 0;
        auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), raw, base);
        if (ec == std::errc::result_out_of_range) {
            diag_.error(currentLocation(), "integer literal '{}' overflows u64", text);
        } else if (ec != std::errc{}) {
            diag_.error(currentLocation(), "invalid integer literal '{}'", text);
        }
        tok.intValue = static_cast<int64_t>(raw);
        return tok;
    }
}

char Lexer::parseEscapeChar() {
    advance(); // consume backslash
    char c = peek();
    advance();
    switch (c) {
        case 'n':  return '\n';
        case 't':  return '\t';
        case 'r':  return '\r';
        case '\\': return '\\';
        case '"':  return '"';
        case '\'': return '\'';
        case '0':  return '\0';
        case '$':  return '$';
        default:
            diag_.error(currentLocation(), "unknown escape sequence '\\{}'", std::string(1, c));
            return c;
    }
}

Token Lexer::scanString() {
    std::string value;
    while (!isAtEnd() && peek() != '"') {
        if (peek() == '\\') {
            value += parseEscapeChar();
        } else if (peek() == '\n') {
            diag_.error(currentLocation(), "unterminated string literal");
            break;
        } else {
            value += advance();
        }
    }

    if (isAtEnd()) {
        diag_.error(currentLocation(), "unterminated string literal");
    } else {
        advance();
    }

    Token tok = makeToken(TokenKind::StringLiteral);
    tok.stringValue = std::move(value);
    return tok;
}

Token Lexer::scanChar() {
    std::string value;
    if (peek() == '\\') {
        value += parseEscapeChar();
    } else {
        value += advance();
    }

    if (peek() != '\'') {
        diag_.error(currentLocation(), "unterminated character literal");
    } else {
        advance();
    }

    Token tok = makeToken(TokenKind::CharLiteral);
    tok.stringValue = std::move(value);
    return tok;
}

Token Lexer::scanMultilineString() {
    advance(); advance(); // consume remaining two quotes of """
    std::string value;
    while (!isAtEnd()) {
        if (peek() == '"' && pos_ + 1 < source_.size() && source_[pos_ + 1] == '"' &&
            pos_ + 2 < source_.size() && source_[pos_ + 2] == '"') {
            advance(); advance(); advance();

            if (!value.empty() && value[0] == '\n') value.erase(0, 1);
            if (!value.empty() && value.back() == '\n') value.pop_back();

            size_t minIndent = std::string::npos;
            size_t p = 0;
            while (p < value.size()) {
                size_t indent = 0;
                while (p < value.size() && (value[p] == ' ' || value[p] == '\t')) { indent++; p++; }
                if (p < value.size() && value[p] != '\n') {
                    if (indent < minIndent) minIndent = indent;
                }
                while (p < value.size() && value[p] != '\n') p++;
                if (p < value.size()) p++;
            }

            if (minIndent > 0 && minIndent != std::string::npos) {
                std::string dedented;
                p = 0;
                while (p < value.size()) {
                    size_t skip = 0;
                    while (skip < minIndent && p + skip < value.size() &&
                           (value[p + skip] == ' ' || value[p + skip] == '\t')) skip++;
                    p += skip;
                    while (p < value.size()) {
                        dedented += value[p];
                        if (value[p] == '\n') { p++; break; }
                        p++;
                    }
                }
                value = std::move(dedented);
            }

            Token tok = makeToken(TokenKind::StringLiteral);
            tok.stringValue = std::move(value);
            return tok;
        }
        value += advance();
    }

    diag_.error(currentLocation(), "unterminated multi-line string");
    return makeToken(TokenKind::Invalid);
}

Token Lexer::scanToken() {
    char c = advance();

    switch (c) {
        case '(': return makeToken(TokenKind::LParen);
        case ')': return makeToken(TokenKind::RParen);
        case '{': return makeToken(TokenKind::LBrace);
        case '}': return makeToken(TokenKind::RBrace);
        case '[': return makeToken(TokenKind::LBracket);
        case ']': return makeToken(TokenKind::RBracket);
        case ',': return makeToken(TokenKind::Comma);
        case ';': return makeToken(TokenKind::Semicolon);
        case '~': return makeToken(TokenKind::Tilde);
        case '@': return makeToken(TokenKind::At);
        case '?':
            if (match('?')) return makeToken(TokenKind::QuestionQuestion);
            return makeToken(TokenKind::Question);
        case '^':
            if (match('=')) return makeToken(TokenKind::CaretEqual);
            return makeToken(TokenKind::Caret);

        case '.':
            if (match('.')) {
                if (match('.')) return makeToken(TokenKind::Ellipsis);
                if (match('=')) return makeToken(TokenKind::DotDotEqual);
                return makeToken(TokenKind::DotDot);
            }
            return makeToken(TokenKind::Dot);

        case ':':
            if (match(':')) return makeToken(TokenKind::ColonColon);
            return makeToken(TokenKind::Colon);

        case '+':
            if (match('+')) return makeToken(TokenKind::PlusPlus);
            if (match('=')) return makeToken(TokenKind::PlusEqual);
            return makeToken(TokenKind::Plus);

        case '-':
            if (match('-')) return makeToken(TokenKind::MinusMinus);
            if (match('>')) return makeToken(TokenKind::Arrow);
            if (match('=')) return makeToken(TokenKind::MinusEqual);
            return makeToken(TokenKind::Minus);

        case '*':
            if (match('=')) return makeToken(TokenKind::StarEqual);
            return makeToken(TokenKind::Star);

        case '/':
            if (match('=')) return makeToken(TokenKind::SlashEqual);
            return makeToken(TokenKind::Slash);

        case '%':
            if (match('=')) return makeToken(TokenKind::PercentEqual);
            return makeToken(TokenKind::Percent);

        case '=':
            if (match('=')) return makeToken(TokenKind::EqualEqual);
            if (match('>')) return makeToken(TokenKind::FatArrow);
            if (match('~')) return makeToken(TokenKind::MatchOp);
            return makeToken(TokenKind::Equal);

        case '!':
            if (match('=')) return makeToken(TokenKind::BangEqual);
            return makeToken(TokenKind::Bang);

        case '<':
            if (match('<')) {
                if (match('=')) return makeToken(TokenKind::LessLessEqual);
                return makeToken(TokenKind::LessLess);
            }
            if (match('=')) return makeToken(TokenKind::LessEqual);
            return makeToken(TokenKind::Less);

        case '>':
            if (match('>')) {
                if (match('=')) return makeToken(TokenKind::GreaterGreaterEqual);
                return makeToken(TokenKind::GreaterGreater);
            }
            if (match('=')) return makeToken(TokenKind::GreaterEqual);
            return makeToken(TokenKind::Greater);

        case '&':
            if (match('&')) return makeToken(TokenKind::AmpAmp);
            if (match('=')) return makeToken(TokenKind::AmpEqual);
            return makeToken(TokenKind::Amp);

        case '|':
            if (match('|')) return makeToken(TokenKind::PipePipe);
            if (match('>')) return makeToken(TokenKind::PipeArrow);
            if (match('=')) return makeToken(TokenKind::PipeEqual);
            return makeToken(TokenKind::Pipe);

        case '"':
            if (peek() == '"' && pos_ + 1 < source_.size() && source_[pos_ + 1] == '"') {
                return scanMultilineString();
            }
            return scanString();

        case '\'':
            return scanChar();

        default:
            if (std::isalpha(c) || c == '_') return scanIdentifierOrKeyword();
            if (std::isdigit(c)) return scanNumber();
            diag_.error(currentLocation(), "unexpected character '{}'", std::string(1, c));
            return makeToken(TokenKind::Invalid);
    }
}

Token Lexer::nextToken() {
    skipWhitespaceAndComments();
    start_ = pos_;
    startLine_ = line_;
    startCol_ = col_;
    if (isAtEnd()) return makeToken(TokenKind::Eof);
    auto tok = scanToken();
    if (!pendingDocComment_.empty()) {
        tok.docComment = std::move(pendingDocComment_);
        pendingDocComment_.clear();
    }
    return tok;
}

std::vector<Token> Lexer::tokenizeAll() {
    std::vector<Token> tokens;
    while (true) {
        Token tok = nextToken();
        tokens.push_back(tok);
        if (tok.is(TokenKind::Eof)) break;
    }
    return tokens;
}

} // namespace vyx
