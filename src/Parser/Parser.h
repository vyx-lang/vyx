#pragma once
#include "AST.h"
#include "../Lexer/Lexer.h"
#include "../Common/Diagnostics.h"
#include <vector>
#include <map>

namespace vyx {

class Parser {
public:
    Parser(std::vector<Token> tokens, DiagnosticsEngine& diag);

    TranslationUnit parseTranslationUnit(std::string_view filename);

    // Post-link pass (called by ImportResolver after every import has
    // been stitched into the user's TU): auto-qualify every top-level
    // type decl that sits inside a file-scope `module std.X;` header.
    // Renames `Vec` → `std.collections.Vec`, `StringBuilder` →
    // `std.string.StringBuilder`, etc., then rewrites every bare-short-
    // name reference across the whole TU to match — EXCEPT references
    // inside user-side decls that also declare a same-named type
    // (user's ODR-equivalent decl keeps its short name and wins the
    // lookup). See Parser.cpp for the rationale (P4-A.2 fix).
    static void autoQualifyStdModuleGlobal(TranslationUnit& unit);

private:
    // Token navigation
    const Token& peek() const;
    const Token& peekNext() const;
    const Token& previous() const;
    Token advance();
    bool check(TokenKind kind) const;
    bool match(TokenKind kind);
    Token expect(TokenKind kind, std::string_view message);
    // Parameter / field / method names accept Identifier plus a fixed set
    // of "soft" reserved words — names like `type`, `module`, `error`,
    // `static` that are keywords in some positions but legitimate
    // identifiers elsewhere (e.g. FFI bindings for C headers that use
    // these tokens as param/field names). Returns the underlying token;
    // caller may treat its text as an identifier.
    Token expectIdentOrSoftKeyword(std::string_view message);
    // Parses either a regular identifier or a source-level operator spelling,
    // normalizing aliases such as `operator[]` to the host's canonical name.
    std::string parseOperatorDeclName(std::string_view message);
    bool matchGenericClose();
    bool isAtEnd() const;

    // Shared helpers (deduplicated)
    std::vector<std::pair<std::string, std::string>> parseAttributes();
    Visibility parseVisibility();
    void parseGenericParams(Decl& decl);
    // Parse a `where T: A + B, U: C, …` clause (and equality/inequality/refinement
    // forms) directly into `decl.genericConstraints` / typeEqualityConstraints /
    // typeInequalityConstraints / refinements. Caller must check `KW_where` first
    // (and consume nothing if absent). No-op for declarations that don't carry a
    // refinements vector (Refinement only stored on FunctionDecl).
    void parseWhereClauseInto(Decl& decl);

    // Declarations
    DeclPtr parseDeclaration();
    DeclPtr parseFunctionDecl(bool isExport, bool isAsync);
    DeclPtr parseStructDecl(bool isExport);
    DeclPtr parseClassDecl(bool isExport);
    DeclPtr parseInterfaceDecl(bool isExport);
    DeclPtr parseErrorDecl(bool isExport);
    DeclPtr parseImportDecl();
    // Parses both the file-wide `module Foo.Bar;` form and the C#-style
    // block form `module Foo.Bar { ... }`. In the block form, inner decls
    // are queued via pendingDecls_ flanked by a `__module Foo.Bar` sentinel
    // and a matching `__module_end` sentinel so Sema's declModule_ rebuild
    // pops back to the enclosing namespace at the closing brace.
    DeclPtr parseModuleDecl();
    DeclPtr parseImplBlock();
    DeclPtr parseExternBlock();

    // Block-form namespace support (C#-style `module Foo.Bar { ... }`):
    // walk over every decl/stmt/expr/type-annotation inside a namespace
    // block and rewrite bare references to block-local type decls into
    // their fully-qualified form (`Foo.Bar.Point` instead of `Point`).
    // This makes the block's inner short-name references uniquely keyed
    // for downstream Sema/Mono/CodeGen. `shortToQualified` maps each
    // unqualified type name declared in this block to its `Foo.Bar.X`
    // rewrite target.
    static void rewriteBlockShortNames(
        const std::map<std::string, std::string>& shortToQualified,
        const std::vector<DeclPtr>::iterator& begin,
        const std::vector<DeclPtr>::iterator& end);


    // Struct / Class internals
    FieldDecl parseFieldDecl();
    MethodDecl parseMethodDecl();
    ParamDecl parseParameter();
    std::vector<ParamDecl> parseParameterList();

    // Types
    TypePtr parseType();
    TypePtr parseBaseType();

    // Statements
    StmtPtr parseStatement();
    StmtPtr parseBlock();
    StmtPtr parseVarDecl(bool isConst);
    StmtPtr parseIfStatement();
    StmtPtr parseWhileStatement();
    StmtPtr parseForStatement();
    StmtPtr parseForEachStatement();
    StmtPtr parseReturnStatement();
    StmtPtr parseMatchStatement();

    // Expressions (Pratt parser)
    ExprPtr parseExpression();
    ExprPtr parseAssignment();
    ExprPtr parseBinaryExpr(int minPrecedence);
    ExprPtr parseUnary();
    ExprPtr parsePostfix(ExprPtr expr);
    ExprPtr parsePrimary();
    ExprPtr parseStringInterpolation(const Token& tok);

    int getPrecedence(TokenKind kind) const;
    BinaryOp tokenToBinaryOp(TokenKind kind) const;

    // Error recovery
    void synchronize();
    void synchronizeStatement();
    bool tryRecover(TokenKind expected);

    std::vector<Token> tokens_;
    DiagnosticsEngine& diag_;
    size_t current_ = 0;
    bool panicMode_ = false;
    std::vector<DeclPtr> pendingDecls_;

    // Set by parseParameter when it encounters the sugar form
    // `name: ...T` (named variadic with homogeneous element type). The type
    // identifier is recorded so the enclosing parseFunctionDecl can mark the
    // corresponding generic param variadic after parameter-list parsing.
    // Empty when no such param was seen.
    std::string pendingVariadicElementType_;
};

} // namespace vyx
