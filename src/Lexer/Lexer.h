#pragma once
#include "Token.h"
#include "../Common/Diagnostics.h"
#include <string>
#include <string_view>
#include <vector>

namespace vyx {

class Lexer {
public:
    Lexer(std::string_view source, std::string_view filename, DiagnosticsEngine& diag);

    Token nextToken();
    std::vector<Token> tokenizeAll();

private:
    char peek() const;
    char peekNext() const;
    char advance();
    bool match(char expected);
    bool isAtEnd() const;

    void skipWhitespaceAndComments();
    Token scanToken();
    Token makeToken(TokenKind kind) const;
    Token makeToken(TokenKind kind, std::string_view text) const;

    Token scanIdentifierOrKeyword();
    Token scanNumber();
    Token scanString();
    Token scanMultilineString();
    Token scanChar();
    char parseEscapeChar();

    SourceLocation currentLocation() const;

    std::string_view source_;
    std::string_view filename_;
    DiagnosticsEngine& diag_;

    size_t pos_    = 0;
    size_t start_  = 0;
    uint32_t line_ = 1;
    uint32_t col_  = 1;
    uint32_t startLine_ = 1;
    uint32_t startCol_  = 1;
    std::string pendingDocComment_;
};

} // namespace vyx
