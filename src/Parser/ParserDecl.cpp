#include "Parser.h"
#include <cctype>
#include <iterator>
#include <map>

namespace vyx {

// ============================================================
//  where-clause parsing (shared by fn/struct/class/interface/impl)
// ============================================================

// ── const-predicate expression parsing (P2D-004) ─────────────────────────
//
// Parses a restricted arithmetic expression for use inside where-clause
// const predicates. Supports: IntLit, ConstIdent, BinOp(*,/,%,-), parens.
// The `+` operator is EXCLUDED from const_expr because `+` is the
// constraint separator in the where-clause grammar. Callers must read the
// `+` separator at the where-clause loop level, not here.
//
// Grammar (using direct token-index access, no Parser state machine):
//   const_expr  = const_mul { (`-`) const_mul }
//   const_mul   = const_atom { (`*`|`/`|`%`) const_atom }
//   const_atom  = IntLit | Identifier | `(` const_expr `)`

static ExprPtr cpAtom(DiagnosticsEngine&, const std::vector<Token>&, size_t&);
static ExprPtr cpMul (DiagnosticsEngine&, const std::vector<Token>&, size_t&);
static ExprPtr cpExpr(DiagnosticsEngine&, const std::vector<Token>&, size_t&);

static ExprPtr cpAtom(DiagnosticsEngine& diag,
                      const std::vector<Token>& toks, size_t& pos)
{
    if (pos >= toks.size()) {
        diag.error({}, "unexpected end of tokens in const predicate");
        return nullptr;
    }
    const auto& tok = toks[pos];

    if (tok.kind == TokenKind::IntLiteral) {
        ++pos;
        auto e = std::make_unique<IntLiteralExpr>();
        e->location = tok.location;
        e->value = std::stoll(std::string(tok.text));
        return e;
    }
    if (tok.kind == TokenKind::Identifier) {
        ++pos;
        auto e = std::make_unique<IdentifierExpr>();
        e->location = tok.location;
        e->name = std::string(tok.text);
        return e;
    }
    if (tok.kind == TokenKind::LParen) {
        auto parenLoc = tok.location;
        ++pos; // consume '('
        auto inner = cpExpr(diag, toks, pos);
        if (pos < toks.size() && toks[pos].kind == TokenKind::RParen) {
            ++pos; // consume ')'
        } else {
            diag.error(parenLoc, "expected ')' in const predicate expression");
        }
        return inner;
    }
    diag.error(tok.location,
        "expected integer literal or const identifier in where-clause predicate, got '{}'",
        std::string(tok.text));
    ++pos; // skip bad token
    return nullptr;
}

static ExprPtr cpMul(DiagnosticsEngine& diag,
                     const std::vector<Token>& toks, size_t& pos)
{
    auto left = cpAtom(diag, toks, pos);
    if (!left) return nullptr;

    while (pos < toks.size()) {
        TokenKind k = toks[pos].kind;
        BinaryOp op;
        if      (k == TokenKind::Star)    op = BinaryOp::Mul;
        else if (k == TokenKind::Slash)   op = BinaryOp::Div;
        else if (k == TokenKind::Percent) op = BinaryOp::Mod;
        else break;

        auto loc = toks[pos].location;
        ++pos;
        auto right = cpAtom(diag, toks, pos);
        if (!right) return left;
        left = ast::makeBinaryOp(loc, op, std::move(left), std::move(right));
    }
    return left;
}

static ExprPtr cpExpr(DiagnosticsEngine& diag,
                      const std::vector<Token>& toks, size_t& pos)
{
    auto left = cpMul(diag, toks, pos);
    if (!left) return nullptr;

    while (pos < toks.size() && toks[pos].kind == TokenKind::Minus) {
        auto loc = toks[pos].location;
        ++pos;
        auto right = cpMul(diag, toks, pos);
        if (!right) return left;
        left = ast::makeBinaryOp(loc, BinaryOp::Sub, std::move(left), std::move(right));
    }
    return left;
}

void Parser::parseWhereClauseInto(Decl& decl) {
    // Outer loop: constraints separated by `+` (const-pred style) or `,`
    // (trait-constraint style). Each branch handles its own continuation.
    while (true) {
        // ── IntLit-leading const predicate: `42 < N` (rare but valid) ─────
        if (check(TokenKind::IntLiteral)) {
            auto litTok = advance();
            auto litExpr = std::make_unique<IntLiteralExpr>();
            litExpr->location = litTok.location;
            litExpr->value = std::stoll(std::string(litTok.text));
            ExprPtr lhs = std::move(litExpr);

            while (true) {
                TokenKind k = peek().kind;
                BinaryOp arithOp;
                if      (k == TokenKind::Star)    arithOp = BinaryOp::Mul;
                else if (k == TokenKind::Slash)   arithOp = BinaryOp::Div;
                else if (k == TokenKind::Percent) arithOp = BinaryOp::Mod;
                else if (k == TokenKind::Minus)   arithOp = BinaryOp::Sub;
                else break;
                auto arithLoc = peek().location;
                advance();
                auto rhs2 = [&]() -> ExprPtr {
                    if (check(TokenKind::IntLiteral)) {
                        auto t = advance();
                        auto e = std::make_unique<IntLiteralExpr>();
                        e->location = t.location;
                        e->value = std::stoll(std::string(t.text));
                        return e;
                    }
                    if (check(TokenKind::Identifier)) {
                        auto t = advance();
                        auto e = std::make_unique<IdentifierExpr>();
                        e->location = t.location;
                        e->name = std::string(t.text);
                        return e;
                    }
                    diag_.error(peek().location, "expected const atom in where-clause predicate");
                    return nullptr;
                }();
                if (!rhs2) break;
                lhs = ast::makeBinaryOp(arithLoc, arithOp, std::move(lhs), std::move(rhs2));
            }

            BinaryOp cmpOp;
            if      (match(TokenKind::Less))         cmpOp = BinaryOp::Lt;
            else if (match(TokenKind::LessEqual))    cmpOp = BinaryOp::Lte;
            else if (match(TokenKind::Greater))      cmpOp = BinaryOp::Gt;
            else if (match(TokenKind::GreaterEqual)) cmpOp = BinaryOp::Gte;
            else if (match(TokenKind::EqualEqual))   cmpOp = BinaryOp::Eq;
            else if (match(TokenKind::BangEqual))    cmpOp = BinaryOp::Neq;
            else {
                diag_.error(peek().location,
                    "expected comparison operator in where-clause const predicate");
                if (!match(TokenKind::Comma) && !match(TokenKind::Plus)) break;
                continue;
            }

            size_t pos = current_;
            auto rhs = cpExpr(diag_, tokens_, pos);
            current_ = pos;
            if (!rhs) {
                if (!match(TokenKind::Comma) && !match(TokenKind::Plus)) break;
                continue;
            }

            Decl::ConstPredicate pred;
            pred.lhs = std::move(lhs);
            pred.rhs = std::move(rhs);
            pred.op  = cmpOp;
            decl.constPredicates.push_back(std::move(pred));

            if (!match(TokenKind::Comma) && !match(TokenKind::Plus)) break;
            continue;
        }

        // ── P2D-008: `typeof(IDENT) == "str"` runtime-receiver-type constraint ──
        // Syntax: `typeof(v) == "i32"`  /  `typeof(v) != "string"`
        // The argument to typeof() must be a plain identifier that names one of
        // this declaration's value parameters.  The expected value is a string
        // literal.  We detect the keyword-like identifier "typeof" followed by
        // '(' to distinguish it from a normal identifier.
        if (check(TokenKind::Identifier) && peek().text == "typeof" &&
            current_ + 1 < tokens_.size() &&
            tokens_[current_ + 1].is(TokenKind::LParen))
        {
            advance(); // consume "typeof"
            advance(); // consume '('
            auto argTok = expect(TokenKind::Identifier, "parameter name inside typeof()");
            std::string argName = std::string(argTok.text);
            expect(TokenKind::RParen, "')'");

            bool negate = false;
            bool hasOp = false;
            if (match(TokenKind::EqualEqual)) {
                negate = false; hasOp = true;
            } else if (match(TokenKind::BangEqual)) {
                negate = true; hasOp = true;
            }
            if (!hasOp) {
                diag_.error(peek().location,
                    "expected '==' or '!=' after 'typeof({})' in where-clause", argName);
                if (!match(TokenKind::Comma) && !match(TokenKind::Plus)) break;
                continue;
            }
            if (!check(TokenKind::StringLiteral)) {
                diag_.error(peek().location,
                    "expected string literal after 'typeof({}) {}' in where-clause",
                    argName, negate ? "!=" : "==");
                if (!match(TokenKind::Comma) && !match(TokenKind::Plus)) break;
                continue;
            }
            auto strTok = advance();
            Decl::TypeofConstraint tc;
            tc.paramName = argName;
            tc.expected  = std::string(strTok.stringValue);
            tc.negate    = negate;
            decl.typeofConstraints.push_back(std::move(tc));
            if (!match(TokenKind::Comma) && !match(TokenKind::Plus)) break;
            continue;
        }

        // ── P5-pack: `all<Ts>: Trait` / `any<Ts>: Trait` ──────────────────
        if (check(TokenKind::Identifier) &&
            (peek().text == "all" || peek().text == "any") &&
            current_ + 1 < tokens_.size() &&
            tokens_[current_ + 1].is(TokenKind::Less))
        {
            bool isAll = (peek().text == "all");
            advance(); // "all"/"any"
            advance(); // "<"
            auto packTok = expect(TokenKind::Identifier, "pack parameter name");
            std::string packName = std::string(packTok.text);
            if (!matchGenericClose())
                expect(TokenKind::Greater, "'>'");
            expect(TokenKind::Colon, "':'");
            do {
                auto traitTok = expect(TokenKind::Identifier, "trait name");
                Decl::PackConstraint pc;
                pc.packName  = packName;
                pc.traitName = std::string(traitTok.text);
                pc.kind      = isAll ? Decl::PackConstraint::All : Decl::PackConstraint::Any;
                decl.packConstraints.push_back(std::move(pc));
            } while (match(TokenKind::Plus));
            if (!match(TokenKind::Comma)) break;
            continue;
        }

        // ── Identifier-starting constraint ──────────────────────────────────
        if (!check(TokenKind::Identifier)) break;

        auto paramName = expect(TokenKind::Identifier, "parameter or type parameter");
        std::string pName = std::string(paramName.text);

        // P5-pack Feature 3: `Ts.len op intLiteral` — pack cardinality.
        if (check(TokenKind::Dot)) {
            size_t savedPos = current_;
            advance(); // '.'
            if (check(TokenKind::Identifier) && peek().text == "len") {
                advance(); // 'len'
                if (decl.kind == DeclKind::Function) {
                    auto* fn = decl.as<FunctionDecl>();
                    Refinement ref;
                    ref.paramName = pName + ".len";
                    if (match(TokenKind::BangEqual))        ref.op = BinaryOp::Neq;
                    else if (match(TokenKind::EqualEqual))  ref.op = BinaryOp::Eq;
                    else if (match(TokenKind::Less))        ref.op = BinaryOp::Lt;
                    else if (match(TokenKind::Greater))     ref.op = BinaryOp::Gt;
                    else if (match(TokenKind::LessEqual))   ref.op = BinaryOp::Lte;
                    else if (match(TokenKind::GreaterEqual)) ref.op = BinaryOp::Gte;
                    else {
                        diag_.error(peek().location,
                            "expected comparison operator after '{}.len'", pName);
                        if (!match(TokenKind::Comma) && !match(TokenKind::Plus)) break;
                        continue;
                    }
                    auto valTok = expect(TokenKind::IntLiteral, "integer literal");
                    ref.value = std::stoll(std::string(valTok.text));
                    fn->refinements.push_back(ref);
                } else {
                    diag_.error(peek().location,
                        "'{}.len' refinements are only allowed on function declarations",
                        pName);
                    if (!check(TokenKind::Comma) && !check(TokenKind::LBrace)) advance();
                    if (!check(TokenKind::Comma) && !check(TokenKind::LBrace)) advance();
                }
                if (!match(TokenKind::Comma) && !match(TokenKind::Plus)) break;
                continue;
            }
            current_ = savedPos;
        }

        // ── P3-Q: `IDENT :: IDENT == StringLit` → reflection constraint ──────
        // Syntax: `T::kind == "struct"` or `T::kind != "class"`
        // Only for type-parameter-like identifiers (starts with uppercase).
        {
            bool lhsUppercase = !pName.empty() &&
                std::isupper(static_cast<unsigned char>(pName[0]));
            bool handledAsReflect = false;
            if (lhsUppercase && check(TokenKind::ColonColon)) {
                size_t savedPos = current_;
                advance(); // consume ::
                if (check(TokenKind::Identifier)) {
                    std::string memberName = std::string(peek().text);
                    static const char* kReflectMembers[] = {
                        "kind", "name", "fields", "methods", nullptr
                    };
                    bool isReflectMember = false;
                    for (const char** rm = kReflectMembers; *rm; ++rm) {
                        if (memberName == *rm) { isReflectMember = true; break; }
                    }
                    if (isReflectMember) {
                        advance(); // consume member name
                        bool negate = false;
                        bool hasOp = false;
                        if (match(TokenKind::EqualEqual)) {
                            negate = false;
                            hasOp = true;
                        } else if (match(TokenKind::BangEqual)) {
                            negate = true;
                            hasOp = true;
                        }
                        if (!hasOp) {
                            diag_.error(peek().location,
                                "expected '==' or '!=' after '{}::{}'", pName, memberName);
                            // Mark as handled so we break out cleanly.
                            handledAsReflect = true;
                        } else if (!check(TokenKind::StringLiteral)) {
                            diag_.error(peek().location,
                                "expected string literal after '{}::{} {}'",
                                pName, memberName, negate ? "!=" : "==");
                            handledAsReflect = true;
                        } else {
                            auto strTok = advance();
                            Decl::ReflectConstraint rc;
                            rc.typeParam = pName;
                            rc.member    = memberName;
                            rc.expected  = std::string(strTok.stringValue);
                            rc.negate    = negate;
                            decl.reflectConstraints.push_back(std::move(rc));
                            handledAsReflect = true;
                        }
                    }
                }
                if (!handledAsReflect) {
                    // Not a reflection constraint (RHS is not a reflect-member name)
                    // — restore and fall through to other where-clause forms.
                    current_ = savedPos;
                }
            }
            if (handledAsReflect) {
                if (!match(TokenKind::Comma) && !match(TokenKind::Plus)) break;
                continue;
            }
        }

        // ── `IDENT :` → trait constraint ──────────────────────────────────
        if (match(TokenKind::Colon)) {
            std::vector<std::string> constraints;
            do {
                auto c = expect(TokenKind::Identifier, "constraint");
                std::string cname(c.text);
                // Optional turbofish on the trait name: `Iterator<T>`.
                // Consume-and-discard the type args — downstream constraint
                // matching operates on the bare trait name (which is what
                // interface declarations register under). Without this the
                // `<T>` bleeds out of the where-clause and is misparsed as
                // a subsequent top-level declaration.
                if (check(TokenKind::Less)) {
                    int depth = 0;
                    do {
                        auto tk = advance();
                        if (tk.kind == TokenKind::Less) ++depth;
                        else if (tk.kind == TokenKind::Greater) --depth;
                        else if (tk.kind == TokenKind::GreaterGreater) depth -= 2;
                    } while (current_ < tokens_.size() && depth > 0);
                }
                constraints.push_back(std::move(cname));
            } while (match(TokenKind::Plus));
            auto& existing = decl.genericConstraints[pName];
            existing.insert(existing.end(), constraints.begin(), constraints.end());
            // After trait constraint, separator is only `,`
            if (!match(TokenKind::Comma)) break;
            continue;
        }

        // Not `IDENT :` → must be a const_predicate starting with IDENT.
        // The leading identifier is the LHS; extend with arithmetic, then compare.
        auto lhsIdent = std::make_unique<IdentifierExpr>();
        lhsIdent->location = paramName.location;
        lhsIdent->name = pName;
        ExprPtr lhs = std::move(lhsIdent);

        // Extend LHS with trailing arithmetic (`*`, `/`, `%`, `-`)
        while (true) {
            TokenKind k = peek().kind;
            BinaryOp arithOp;
            if      (k == TokenKind::Star)    arithOp = BinaryOp::Mul;
            else if (k == TokenKind::Slash)   arithOp = BinaryOp::Div;
            else if (k == TokenKind::Percent) arithOp = BinaryOp::Mod;
            else if (k == TokenKind::Minus)   arithOp = BinaryOp::Sub;
            else break;
            auto arithLoc = peek().location;
            advance();
            auto rhs2 = [&]() -> ExprPtr {
                if (check(TokenKind::IntLiteral)) {
                    auto t = advance();
                    auto e = std::make_unique<IntLiteralExpr>();
                    e->location = t.location;
                    e->value = std::stoll(std::string(t.text));
                    return e;
                }
                if (check(TokenKind::Identifier)) {
                    auto t = advance();
                    auto e = std::make_unique<IdentifierExpr>();
                    e->location = t.location;
                    e->name = std::string(t.text);
                    return e;
                }
                diag_.error(peek().location, "expected const atom in where-clause predicate");
                return nullptr;
            }();
            if (!rhs2) break;
            lhs = ast::makeBinaryOp(arithLoc, arithOp, std::move(lhs), std::move(rhs2));
        }

        // Now look for comparison op.
        BinaryOp cmpOp = BinaryOp::Gt; // placeholder
        bool hasCmpOp = false;

        if      (match(TokenKind::EqualEqual))   { cmpOp = BinaryOp::Eq;  hasCmpOp = true; }
        else if (match(TokenKind::BangEqual))    { cmpOp = BinaryOp::Neq; hasCmpOp = true; }
        else if (match(TokenKind::LessEqual))    { cmpOp = BinaryOp::Lte; hasCmpOp = true; }
        else if (match(TokenKind::Less))         { cmpOp = BinaryOp::Lt;  hasCmpOp = true; }
        else if (match(TokenKind::GreaterEqual)) { cmpOp = BinaryOp::Gte; hasCmpOp = true; }
        else if (match(TokenKind::Greater))      { cmpOp = BinaryOp::Gt;  hasCmpOp = true; }

        if (hasCmpOp) {
            // Disambiguation: `T == i32` (type-equality) vs `N == 0` (const predicate).
            // Type equality/inequality: RHS is a type-name keyword or bare identifier
            //   that doesn't look like an integer expression.  Only applies when the
            //   LHS is a simple identifier (no arithmetic was performed on it).
            // Const predicate: RHS is an integer literal or arithmetic expression.
            bool lhsIsSimpleIdent =
                (lhs && lhs->kind == ExprKind::Identifier);
            // Type-equality disambiguation:
            //   `T == i32`       → type equality (legacy, RHS is a type-name keyword)
            //   `T == MyType`    → type equality (legacy, RHS is capitalized identifier)
            //   `N == 0`         → const predicate (RHS is integer literal)
            //   `N == M`         → const predicate (both are const param identifiers)
            //
            // Heuristic: if LHS is a simple identifier AND the RHS is a type-name
            // keyword or an Identifier that starts with an uppercase letter, treat
            // as type-equality.  Otherwise fall through to const predicate.
            // Note: this heuristic is not perfect but preserves the pre-P2D behavior.
            auto peekIsTypelikeIdent = [&]() -> bool {
                if (!check(TokenKind::Identifier)) return false;
                // Vyx convention: type names start with an uppercase letter.
                if (current_ < tokens_.size()) {
                    const auto& t = tokens_[current_];
                    if (!t.text.empty() && std::isupper(static_cast<unsigned char>(t.text[0])))
                        return true;
                }
                return false;
            };
            bool rhsIsTypeLike = peek().isTypeName() || peekIsTypelikeIdent();
            if (lhsIsSimpleIdent && rhsIsTypeLike &&
                (cmpOp == BinaryOp::Eq || cmpOp == BinaryOp::Neq)) {
                auto typeTok = advance();
                if (cmpOp == BinaryOp::Eq)
                    decl.typeEqualityConstraints[pName] = std::string(typeTok.text);
                else
                    decl.typeInequalityConstraints[pName] = std::string(typeTok.text);
                if (!match(TokenKind::Comma) && !match(TokenKind::Plus)) break;
                continue;
            }

            // ── New const predicate path ─────────────────────────────────────
            size_t pos = current_;
            auto rhs = cpExpr(diag_, tokens_, pos);
            current_ = pos;

            if (rhs) {
                Decl::ConstPredicate pred;
                pred.lhs = std::move(lhs);
                pred.rhs = std::move(rhs);
                pred.op  = cmpOp;
                decl.constPredicates.push_back(std::move(pred));
            }
            // Consume separator: `+` or `,`
            if (!match(TokenKind::Comma) && !match(TokenKind::Plus)) break;
            continue;
        }

        // No comparison op found — neither a trait constraint (`IDENT :`)
        // nor a const predicate (`IDENT CompareOp rhs`).
        diag_.error(peek().location,
            "expected ':' or comparison operator after '{}' in where clause; "
            "use `T: Trait` for trait bounds or `N > 0` for const predicates",
            pName);
        break;
    }
}

// ============================================================
//  Declarations
// ============================================================

DeclPtr Parser::parseDeclaration() {
    std::string docComment = peek().docComment;
    auto attrs = parseAttributes();
    auto appendAttrs = [&]() {
        auto more = parseAttributes();
        attrs.insert(attrs.end(),
                     std::make_move_iterator(more.begin()),
                     std::make_move_iterator(more.end()));
    };

    if (!attrs.empty() && check(TokenKind::LBrace)) {
        advance();
        while (!check(TokenKind::RBrace) && !isAtEnd()) {
            auto inner = parseDeclaration();
            if (inner) {
                for (auto& a : attrs)
                    inner->attributes.push_back(a);
                pendingDecls_.push_back(std::move(inner));
            }
        }
        expect(TokenKind::RBrace, "'}'");
        return nullptr;
    }

    bool isExport = false;
    if (match(TokenKind::KW_public)) {
        isExport = true;
    } else if (match(TokenKind::KW_export)) {
        if (check(TokenKind::KW_import)) {
            auto importDecl = parseImportDecl();
            if (importDecl) importDecl->isExport = true;
            if (!attrs.empty() && importDecl) importDecl->attributes = std::move(attrs);
            if (!docComment.empty() && importDecl) importDecl->docComment = std::move(docComment);
            return importDecl;
        }
        if (check(TokenKind::StringLiteral)) {
            auto loc = previous().location;
            auto abiTok = advance();
            auto decl = std::make_unique<ExternBlockDecl>();
            decl->location = loc;
            decl->externABI = abiTok.stringValue;
            decl->isExport = true;
            expect(TokenKind::LBrace, "'{'");
            while (!check(TokenKind::RBrace) && !isAtEnd()) {
                auto fn = parseFunctionDecl(true, false);
                if (fn) {
                    fn->externABI = decl->externABI;
                    fn->isExport = true;
                    decl->externDecls.push_back(std::move(fn));
                }
            }
            expect(TokenKind::RBrace, "'}'");
            if (!attrs.empty()) decl->attributes = std::move(attrs);
            if (!docComment.empty()) decl->docComment = std::move(docComment);
            return decl;
        }
        diag_.errorWithFix(peek().location,
            "replace 'export' with 'public'",
            "'export' is reserved for module operations (export import, export \"C\"); use 'public' for visibility");
        isExport = true;
    }
    appendAttrs();

    [[maybe_unused]] bool isStatic = false;
    if (match(TokenKind::KW_static)) isStatic = true;
    appendAttrs();

    bool isAsync = false;
    if (match(TokenKind::KW_async)) {
        isAsync = true;
        attrs.emplace_back("async", "");
    }
    appendAttrs();

    DeclPtr result = nullptr;

    if (check(TokenKind::KW_when)) {
        advance();
        expect(TokenKind::LParen, "'('");
        std::string condition;
        while (!check(TokenKind::RParen) && !isAtEnd()) {
            condition += std::string(peek().text);
            advance();
        }
        expect(TokenKind::RParen, "')'");

        bool condMet = false;
#ifdef _WIN32
        condMet = (condition == "windows" || condition == "win32");
#elif defined(__linux__)
        condMet = (condition == "linux");
#elif defined(__APPLE__)
        condMet = (condition == "macos" || condition == "apple");
#endif
        if (condition == "debug") condMet = true;

        if (condMet) {
            if (check(TokenKind::LBrace)) {
                advance();
                while (!check(TokenKind::RBrace) && !isAtEnd()) {
                    auto inner = parseDeclaration();
                    if (inner) pendingDecls_.push_back(std::move(inner));
                }
                expect(TokenKind::RBrace, "'}'");
            } else {
                result = parseDeclaration();
            }
        } else {
            if (check(TokenKind::LBrace)) {
                advance();
                int depth = 1;
                while (depth > 0 && !isAtEnd()) {
                    if (check(TokenKind::LBrace)) depth++;
                    if (check(TokenKind::RBrace)) depth--;
                    if (depth > 0) advance();
                }
                expect(TokenKind::RBrace, "'}'");
            }
        }
        return result;
    }

    if (check(TokenKind::KW_task)) {
        diag_.warning(peek().location, "'task fn' is deprecated; use '@[task] fn' instead");
        advance();
        result = parseFunctionDecl(isExport, false);
        if (result) result->attributes.emplace_back("task", "");
    } else if (check(TokenKind::KW_comptime)) {
        diag_.warning(peek().location, "'comptime fn' is deprecated; use '@[comptime] fn' instead");
        advance();
        if (check(TokenKind::KW_fn)) {
            result = parseFunctionDecl(isExport, isAsync);
            if (result) result->as<FunctionDecl>()->isComptime = true;
        } else {
            diag_.error(peek().location, "expected 'fn' after 'comptime'");
            synchronize();
            return nullptr;
        }
    } else if (check(TokenKind::KW_bench)) {
        diag_.warning(peek().location, "'bench fn' is deprecated; use '@[bench] fn' instead");
        advance();
        if (check(TokenKind::KW_fn)) {
            result = parseFunctionDecl(isExport, false);
            if (result) result->as<FunctionDecl>()->isBench = true;
        } else {
            diag_.error(peek().location, "expected 'fn' after 'bench'");
            synchronize();
            return nullptr;
        }
    } else if (check(TokenKind::KW_concept)) {
        auto loc = peek().location;
        advance();
        auto decl = std::make_unique<ConceptDecl>();
        decl->location = loc;
        decl->isExport = isExport;
        auto nameTok = expect(TokenKind::Identifier, "concept name");
        decl->name = std::string(nameTok.text);

        // C++20-style: `concept Name<T> [requires (params) { body }]`
        // Classic:    `concept Name = requires { body }` or `concept Name = A + B`
        if (check(TokenKind::Less)) {
            // Parse generic params into the decl (syntactically only; discarded by Sema).
            parseGenericParams(*decl);
            // Optional `requires (params) { body }` — skip tokens, capture nothing
            // since the semantic side is treated as a no-op.
            if (match(TokenKind::KW_requires)) {
                if (match(TokenKind::LParen)) {
                    int depth = 1;
                    while (depth > 0 && !isAtEnd()) {
                        if (check(TokenKind::LParen)) depth++;
                        else if (check(TokenKind::RParen)) { depth--; if (depth == 0) { advance(); break; } }
                        advance();
                    }
                }
                if (match(TokenKind::LBrace)) {
                    int depth = 1;
                    while (depth > 0 && !isAtEnd()) {
                        if (check(TokenKind::LBrace)) depth++;
                        else if (check(TokenKind::RBrace)) { depth--; if (depth == 0) { advance(); break; } }
                        advance();
                    }
                }
            } else if (check(TokenKind::LBrace)) {
                // Bare concept body: `concept Name<T> { ... }`
                advance();
                int depth = 1;
                while (depth > 0 && !isAtEnd()) {
                    if (check(TokenKind::LBrace)) depth++;
                    else if (check(TokenKind::RBrace)) { depth--; if (depth == 0) { advance(); break; } }
                    advance();
                }
            }
            match(TokenKind::Semicolon);
        } else {
            expect(TokenKind::Equal, "'='");
            if (check(TokenKind::KW_requires)) {
                advance();
                expect(TokenKind::LBrace, "'{'");
                while (!check(TokenKind::RBrace) && !isAtEnd()) {
                    if (check(TokenKind::KW_fn)) {
                        advance();
                        auto methodName = expect(TokenKind::Identifier, "method name");
                        decl->conceptRequires.push_back(std::string(methodName.text));
                        while (!check(TokenKind::Semicolon) && !check(TokenKind::RBrace) && !isAtEnd()) advance();
                        match(TokenKind::Semicolon);
                    } else {
                        advance();
                    }
                }
                expect(TokenKind::RBrace, "'}'");
            } else {
                do {
                    auto iface = expect(TokenKind::Identifier, "interface or concept name");
                    decl->conceptRequires.push_back(std::string(iface.text));
                } while (match(TokenKind::Plus));
            }
            match(TokenKind::Semicolon);
        }
        result = std::move(decl);
    } else if (check(TokenKind::KW_unsafe) && current_ + 1 < tokens_.size() && tokens_[current_ + 1].is(TokenKind::KW_fn)) {
        advance();
        result = parseFunctionDecl(isExport, isAsync);
        if (result) result->attributes.emplace_back("unsafe", "");
    } else if (check(TokenKind::KW_fn)) {
        result = parseFunctionDecl(isExport, isAsync);
    } else if (check(TokenKind::KW_struct)) {
        result = parseStructDecl(isExport);
    } else if (check(TokenKind::KW_class)) {
        result = parseClassDecl(isExport);
    } else if (check(TokenKind::KW_interface)) {
        result = parseInterfaceDecl(isExport);
    } else if (check(TokenKind::KW_error)) {
        result = parseErrorDecl(isExport);
    } else if (check(TokenKind::KW_macro)) {
        auto loc = peek().location;
        advance();
        auto decl = std::make_unique<MacroDecl>();
        decl->location = loc;
        decl->isExport = isExport;
        auto nameTok = expect(TokenKind::Identifier, "macro name");
        decl->name = std::string(nameTok.text);
        match(TokenKind::Bang);
        if (match(TokenKind::LParen)) {
            do {
                if (check(TokenKind::RParen)) break;
                auto p = expect(TokenKind::Identifier, "macro param");
                decl->macroParams.push_back(std::string(p.text));
            } while (match(TokenKind::Comma));
            expect(TokenKind::RParen, "')'");
        }
        expect(TokenKind::LBrace, "'{'");
        int depth = 1;
        std::string body;
        while (depth > 0 && !isAtEnd()) {
            if (check(TokenKind::LBrace)) depth++;
            if (check(TokenKind::RBrace)) { depth--; if (depth == 0) break; }
            body += std::string(peek().text) + " ";
            advance();
        }
        expect(TokenKind::RBrace, "'}'");
        decl->macroBody = body;
        result = std::move(decl);
    } else if (check(TokenKind::KW_enum)) {
        result = parseErrorDecl(isExport);
    } else if (check(TokenKind::KW_import) || check(TokenKind::KW_use)) {
        result = parseImportDecl();
    } else if (check(TokenKind::KW_module)) {
        // `module Foo.Bar;` (top-level form, file-wide) and
        // `module Foo.Bar { decls... }` (C#-style block form, scoped to
        // brace-balanced inner decls) are both handled by parseModuleDecl.
        result = parseModuleDecl();
    } else if (check(TokenKind::KW_impl)) {
        return parseImplBlock();
    } else if (check(TokenKind::KW_extern)) {
        result = parseExternBlock();
    } else if (check(TokenKind::KW_type) || check(TokenKind::KW_newtype)) {
        bool isNewtype = check(TokenKind::KW_newtype);
        auto loc = peek().location;
        advance();
        auto decl = std::make_unique<TypeAliasDecl>();
        decl->location = loc;
        decl->isExport = isExport;
        decl->isNewtype = isNewtype;
        auto nameTok = expect(TokenKind::Identifier, isNewtype ? "newtype name" : "type alias name");
        decl->name = std::string(nameTok.text);
        expect(TokenKind::Equal, "'='");
        decl->aliasType = parseType();
        match(TokenKind::Semicolon);
        result = std::move(decl);
    } else if (check(TokenKind::KW_const_kw) || check(TokenKind::KW_let) || check(TokenKind::KW_var)) {
        bool isVarMutable = check(TokenKind::KW_var);
        auto loc = peek().location;
        advance();
        auto decl = std::make_unique<GlobalVarDecl>();
        decl->location = loc;
        decl->isExport = isExport;
        decl->isMutableVar = isVarMutable;
        auto nameTok = expect(TokenKind::Identifier, "variable name");
        decl->name = std::string(nameTok.text);
        if (match(TokenKind::Colon)) {
            decl->varType = parseType();
        }
        if (match(TokenKind::Equal)) {
            auto initExpr = parseExpression();
            decl->initBody = ast::makeExprStmt(loc, std::move(initExpr));
        }
        match(TokenKind::Semicolon);
        result = std::move(decl);
    } else {
        diag_.error(peek().location, "expected declaration, got {}", tokenKindToString(peek().kind));
        synchronize();
        return nullptr;
    }

    if (result) {
        if (!attrs.empty()) result->attributes = std::move(attrs);
        if (!docComment.empty()) result->docComment = std::move(docComment);
        for (auto& [an, av] : result->attributes) {
            if (result->kind == DeclKind::Function) {
                auto* fn = result->as<FunctionDecl>();
                if (an == "comptime") fn->isComptime = true;
                if (an == "bench") fn->isBench = true;
                if (an == "async" || an == "task" || an == "coroutine" ||
                    an == "corountine") fn->isAsync = true;
            }
        }
    }
    return result;
}

DeclPtr Parser::parseFunctionDecl(bool isExport, bool isAsync) {
    auto loc = peek().location;
    expect(TokenKind::KW_fn, "'fn'");

    auto decl = std::make_unique<FunctionDecl>();
    decl->location = loc;
    decl->isExport = isExport;
    decl->isAsync = isAsync;

    decl->name = parseOperatorDeclName("function name");

    parseGenericParams(*decl);

    expect(TokenKind::LParen, "'('");
    pendingVariadicElementType_.clear();
    decl->params = parseParameterList();
    expect(TokenKind::RParen, "')'");

    // If any parameter used the `name: ...T` sugar, the generic param T
    // needs to be marked as variadic so Sema treats it as a type-pack.
    if (!pendingVariadicElementType_.empty()) {
        const std::string& elemT = pendingVariadicElementType_;
        // Only promote to variadic if T is one of the declared generic
        // params. (Don't touch built-in types.)
        for (auto& gp : decl->genericParams) {
            if (gp == elemT) {
                decl->isVariadicGeneric = true;
                break;
            }
        }
        pendingVariadicElementType_.clear();
    }

    if (match(TokenKind::Arrow)) {
        decl->returnType = parseType();
    }

    if (match(TokenKind::KW_where)) {
        parseWhereClauseInto(*decl);
    }

    // C++20-style trailing `requires Addable<T>` or `requires Addable<T> + Eq<T>`
    // clause — map it onto the existing `where T: Trait` mechanism. Each
    // `Name<TypeArg>` token-pair contributes a constraint (TypeArg : Name).
    // Bare `Name` (no generic args) is silently tolerated as a no-op
    // (we have no bare-type binding target for it).
    if (match(TokenKind::KW_requires)) {
        auto addTraitBound = [&](const std::string& typeParam, const std::string& traitName) {
            decl->genericConstraints[typeParam].push_back(traitName);
        };
        do {
            if (!check(TokenKind::Identifier)) {
                diag_.error(peek().location, "expected trait name after 'requires'");
                break;
            }
            auto traitTok = advance();
            std::string traitName = std::string(traitTok.text);
            if (match(TokenKind::Less)) {
                // Read first type arg as the constrained type param
                if (check(TokenKind::Identifier)) {
                    auto argTok = advance();
                    addTraitBound(std::string(argTok.text), traitName);
                }
                // Skip any further args
                int depth = 1;
                while (depth > 0 && !isAtEnd()) {
                    if (check(TokenKind::Less)) { depth++; advance(); continue; }
                    if (check(TokenKind::Greater)) { depth--; advance(); if (depth == 0) break; continue; }
                    if (check(TokenKind::GreaterGreater)) {
                        if (depth >= 2) { depth -= 2; advance(); if (depth == 0) break; continue; }
                        depth -= 1; advance(); if (depth == 0) break; continue;
                    }
                    advance();
                }
            }
        } while (match(TokenKind::Plus) || match(TokenKind::Comma));
    }

    if (check(TokenKind::LBrace)) {
        decl->body = parseBlock();
    } else {
        match(TokenKind::Semicolon);
    }

    return decl;
}

DeclPtr Parser::parseStructDecl(bool isExport) {
    auto loc = peek().location;
    expect(TokenKind::KW_struct, "'struct'");

    auto decl = std::make_unique<StructDecl>();
    decl->location = loc;
    decl->isExport = isExport;

    auto nameTok = expect(TokenKind::Identifier, "struct name");
    decl->name = std::string(nameTok.text);

    parseGenericParams(*decl);

    if (match(TokenKind::Colon)) {
        auto parent = expect(TokenKind::Identifier, "parent struct name");
        decl->parentName = std::string(parent.text);
    }

    if (match(TokenKind::KW_where)) {
        parseWhereClauseInto(*decl);
    }

    expect(TokenKind::LBrace, "'{'");
    while (!check(TokenKind::RBrace) && !isAtEnd()) {
        // Structs carry data only. If the user put a method here
        // (directly or after visibility/static/async/@[...]), report a
        // targeted error instead of letting parseFieldDecl emit the cryptic
        // "expected field name, got 'fn'" — Sema would then reject it
        // anyway (struct types have no method table in current codegen).
        size_t savedPos = current_;
        while (check(TokenKind::At)) {
            advance();
            if (check(TokenKind::LBracket)) {
                int d = 1; advance();
                while (!isAtEnd() && d > 0) {
                    if (check(TokenKind::LBracket)) d++;
                    else if (check(TokenKind::RBracket)) d--;
                    advance();
                }
            }
        }
        if (check(TokenKind::KW_public) || check(TokenKind::KW_private) ||
            check(TokenKind::KW_internal) || check(TokenKind::KW_protected)) advance();
        if (check(TokenKind::KW_static)) advance();
        if (check(TokenKind::KW_async)) advance();
        bool isMethodHere = check(TokenKind::KW_fn);
        current_ = savedPos;
        if (isMethodHere) {
            diag_.error(peek().location,
                "struct bodies may not contain methods; use 'class' for types "
                "with behaviour, or move this function to file scope");
            // Skip to next field/method/close brace to recover.
            int depth = 0;
            while (!isAtEnd()) {
                if (check(TokenKind::LBrace)) depth++;
                else if (check(TokenKind::RBrace)) { if (depth == 0) break; depth--; }
                advance();
            }
            continue;
        }

        auto field = parseFieldDecl();
        decl->fields.push_back(std::move(field));
    }
    expect(TokenKind::RBrace, "'}'");
    match(TokenKind::Semicolon);

    return decl;
}

DeclPtr Parser::parseClassDecl(bool isExport) {
    auto loc = peek().location;
    expect(TokenKind::KW_class, "'class'");

    auto decl = std::make_unique<ClassDecl>();
    decl->location = loc;
    decl->isExport = isExport;

    auto nameTok = expect(TokenKind::Identifier, "class name");
    decl->name = std::string(nameTok.text);

    parseGenericParams(*decl);

    if (match(TokenKind::Colon)) {
        // Parent / interface name optionally followed by turbofish-less generic
        // args: `class Bag : Container<i64>` or `class Pair : Pair<A, B>`. The
        // generic args are captured as a mangled string suffix appended to the
        // identifier ("Container<i64>"), preserving the existing
        // string-keyed lookup contract used by Sema/CodeGen for parent and
        // interface resolution.
        auto readNameWithGenerics = [&](const char* role) -> std::string {
            auto tok = expect(TokenKind::Identifier, role);
            std::string name(tok.text);
            if (check(TokenKind::Less)) {
                advance(); // consume '<'
                std::string args = "<";
                bool first = true;
                while (!check(TokenKind::Greater) && !isAtEnd()) {
                    if (!first) {
                        expect(TokenKind::Comma, "',' between type args");
                        args += ",";
                    }
                    first = false;
                    auto ty = parseType();
                    args += ty ? ty->name : std::string("?");
                }
                expect(TokenKind::Greater, "'>' to close generic args");
                args += ">";
                name += args;
            }
            return name;
        };
        decl->parentName = readNameWithGenerics("parent class or interface name");
        // Accept BOTH `,` and `+` as interface-list separators so users can
        // write `class Dog : Named + Countable` (trait-bound style) or
        // `class Dog : Named, Countable` (C#/inheritance style).
        while (match(TokenKind::Comma) || match(TokenKind::Plus)) {
            decl->interfaces.push_back(readNameWithGenerics("interface name"));
        }
    }

    if (match(TokenKind::KW_where)) {
        parseWhereClauseInto(*decl);
    }

    expect(TokenKind::LBrace, "'{'");
    while (!check(TokenKind::RBrace) && !isAtEnd()) {
        // P2-generics C5: `type Item = ConcreteType;` associated-type binding
        // inside a concrete class body (e.g. `class CountUp : Iterable`).
        // Also supports qualified form `type A::Item = X;` for disambiguation
        // when two traits both declare the same associated type name.
        // Populates ClassDecl::associatedTypes so Sema can verify interface
        // conformance and resolve `T::Item` when T is bound to this class.
        if (check(TokenKind::KW_type)) {
            advance();
            auto typeName = expect(TokenKind::Identifier, "associated type name");
            AssociatedType at;
            // Check for qualified `type TraitName::AssocName = X;`
            if (check(TokenKind::ColonColon)) {
                advance(); // consume ::
                at.qualifyingTrait = std::string(typeName.text);
                auto memberName = expect(TokenKind::Identifier, "associated type member name");
                at.name = std::string(memberName.text);
            } else {
                at.name = std::string(typeName.text);
            }
            if (match(TokenKind::Equal)) {
                at.defaultType = parseType();
            } else {
                diag_.error(peek().location,
                    "associated type '{}' in class body requires `= <type>` binding",
                    at.name);
            }
            match(TokenKind::Semicolon);
            decl->associatedTypes.push_back(std::move(at));
            continue;
        }

        // P5-traitconst: `const NAME: Type = expr;` associated-const binding.
        // Always requires an initializer in a class body; the value flows
        // into Sema's `T::NAME` resolution when T is bound to a trait that
        // declares the same const.
        if (check(TokenKind::KW_const_kw)) {
            auto cloc = peek().location;
            advance();
            auto nameTok = expect(TokenKind::Identifier, "const member name");
            expect(TokenKind::Colon, "':'");
            auto cty = parseType();
            ConstMember cm;
            cm.location = cloc;
            cm.name = std::string(nameTok.text);
            cm.type = std::move(cty);
            if (match(TokenKind::Equal)) {
                cm.defaultValue = parseExpression();
            } else {
                diag_.error(peek().location,
                    "const member '{}' in class body requires `= <expr>` initializer",
                    cm.name);
            }
            match(TokenKind::Semicolon);
            decl->constMembers.push_back(std::move(cm));
            continue;
        }

        auto methodAttrs = parseAttributes();
        auto appendMethodAttrs = [&]() {
            auto more = parseAttributes();
            methodAttrs.insert(methodAttrs.end(),
                               std::make_move_iterator(more.begin()),
                               std::make_move_iterator(more.end()));
        };

        Visibility vis = parseVisibility();
        appendMethodAttrs();

        bool isStaticMethod = match(TokenKind::KW_static);
        appendMethodAttrs();
        bool isAsyncMethod = match(TokenKind::KW_async);
        appendMethodAttrs();

        // After consuming `static`, peek ahead: if we see `identifier ':'`
        // (not `fn`), this is a static field declaration, not a static method.
        if (isStaticMethod && !check(TokenKind::KW_fn) && check(TokenKind::Identifier) &&
            current_ + 1 < tokens_.size() && tokens_[current_ + 1].is(TokenKind::Colon)) {
            auto field = parseFieldDecl();
            field.visibility = vis;
            field.isStatic = true;
            decl->fields.push_back(std::move(field));
        } else if (check(TokenKind::KW_fn) || isStaticMethod) {
            auto method = parseMethodDecl();
            method.visibility = vis;
            method.isStatic = isStaticMethod;
            method.isAsync = isAsyncMethod;
            method.attributes = std::move(methodAttrs);
            for (auto& [an, _av] : method.attributes) {
                if (an == "async" || an == "task" || an == "coroutine" ||
                    an == "corountine") {
                    method.isAsync = true;
                }
            }
            decl->methods.push_back(std::move(method));
        } else if (check(TokenKind::Identifier)) {
            auto field = parseFieldDecl();
            field.visibility = vis;
            decl->fields.push_back(std::move(field));
        } else {
            diag_.error(peek().location, "expected field or method in class body, got '{}'",
                std::string(peek().text));
            advance();
            if (diag_.errorCount() > 100) break;
        }
    }
    expect(TokenKind::RBrace, "'}'");
    match(TokenKind::Semicolon);

    return decl;
}

DeclPtr Parser::parseInterfaceDecl(bool isExport) {
    auto loc = peek().location;
    expect(TokenKind::KW_interface, "'interface'");

    auto decl = std::make_unique<InterfaceDecl>();
    decl->location = loc;
    decl->isExport = isExport;

    auto nameTok = expect(TokenKind::Identifier, "interface name");
    decl->name = std::string(nameTok.text);

    parseGenericParams(*decl);

    // Trait composition (2026-04-23):
    //   trait Num : Add + Sub + Mul + Div + Zero + One {}
    // Mirrors ClassDecl's `: Parent [,+ Iface]*` parser so the same
    // `parentName` / `interfaces` slots carry both class parents and
    // trait supertraits. Separator: `,` or `+` (trait-bound style is
    // more idiomatic for supertraits, so `+` is the expected form).
    if (match(TokenKind::Colon)) {
        auto readNameWithGenerics = [&](const char* role) -> std::string {
            auto tok = expect(TokenKind::Identifier, role);
            std::string name(tok.text);
            if (check(TokenKind::Less)) {
                advance(); // consume '<'
                std::string args = "<";
                bool first = true;
                while (!check(TokenKind::Greater) && !isAtEnd()) {
                    if (!first) {
                        expect(TokenKind::Comma, "',' between type args");
                        args += ",";
                    }
                    first = false;
                    auto ty = parseType();
                    args += ty ? ty->name : std::string("?");
                }
                expect(TokenKind::Greater, "'>' to close generic args");
                args += ">";
                name += args;
            }
            return name;
        };
        decl->parentName = readNameWithGenerics("supertrait name");
        while (match(TokenKind::Comma) || match(TokenKind::Plus)) {
            decl->interfaces.push_back(readNameWithGenerics("supertrait name"));
        }
    }

    if (match(TokenKind::KW_where)) {
        parseWhereClauseInto(*decl);
    }

    expect(TokenKind::LBrace, "'{'");
    while (!check(TokenKind::RBrace) && !isAtEnd()) {
        if (check(TokenKind::KW_type)) {
            advance();
            auto typeName = expect(TokenKind::Identifier, "associated type name");
            AssociatedType at;
            at.name = std::string(typeName.text);
            if (match(TokenKind::Equal)) {
                at.defaultType = parseType();
            }
            match(TokenKind::Semicolon);
            decl->associatedTypes.push_back(std::move(at));
        } else if (check(TokenKind::KW_const_kw)) {
            // Trait-body const requirement:
            //   const NAME: Type;            (requirement — impls must supply)
            //   const NAME: Type = <expr>;   (requirement with default value)
            auto cloc = peek().location;
            advance();
            auto nameTok = expect(TokenKind::Identifier, "const member name");
            expect(TokenKind::Colon, "':'");
            auto cty = parseType();
            ConstMember cm;
            cm.location = cloc;
            cm.name = std::string(nameTok.text);
            cm.type = std::move(cty);
            if (match(TokenKind::Equal)) {
                cm.defaultValue = parseExpression();
            }
            match(TokenKind::Semicolon);
            decl->constMembers.push_back(std::move(cm));
        } else {
            auto method = parseMethodDecl();
            decl->methods.push_back(std::move(method));
        }
    }
    expect(TokenKind::RBrace, "'}'");

    return decl;
}

DeclPtr Parser::parseErrorDecl(bool isExport) {
    auto loc = peek().location;
    if (!match(TokenKind::KW_error)) {
        expect(TokenKind::KW_enum, "'error' or 'enum'");
    }

    auto decl = std::make_unique<ErrorDefDecl>();
    decl->location = loc;
    decl->isExport = isExport;

    auto nameTok = expect(TokenKind::Identifier, "error name");
    decl->name = std::string(nameTok.text);

    if (check(TokenKind::Less)) {
        advance();
        while (!check(TokenKind::Greater) && !isAtEnd()) {
            auto param = expect(TokenKind::Identifier, "generic param");
            decl->genericParams.push_back(std::string(param.text));
            if (!match(TokenKind::Comma)) break;
        }
        expect(TokenKind::Greater, "'>'");
    }

    expect(TokenKind::LBrace, "'{'");
    while (!check(TokenKind::RBrace) && !isAtEnd()) {
        if (check(TokenKind::KW_fn) || check(TokenKind::KW_public) ||
            check(TokenKind::KW_private) || check(TokenKind::KW_static)) {
            auto method = parseMethodDecl();
            decl->methods.push_back(std::move(method));
        } else {
            auto variant = expect(TokenKind::Identifier, "error variant");
            decl->variants.push_back(std::string(variant.text));
            std::vector<TypePtr> types;
            if (match(TokenKind::LParen)) {
                if (!check(TokenKind::RParen)) {
                    types.push_back(parseType());
                    while (match(TokenKind::Comma)) {
                        types.push_back(parseType());
                    }
                }
                expect(TokenKind::RParen, "')'");
            }
            decl->variantTypes.push_back(std::move(types));
            match(TokenKind::Comma);
            match(TokenKind::Semicolon);
        }
    }
    expect(TokenKind::RBrace, "'}'");

    return decl;
}

DeclPtr Parser::parseImportDecl() {
    auto loc = peek().location;
    bool isUse = check(TokenKind::KW_use);
    if (!match(TokenKind::KW_import) && !match(TokenKind::KW_use)) {
        diag_.error(loc, "expected 'import' or 'use'");
        return nullptr;
    }

    auto decl = std::make_unique<ImportDecl>();
    decl->location = loc;

    if (isUse && check(TokenKind::Identifier)) {
        size_t savedPos = current_;
        auto firstIdent = advance();
        if (match(TokenKind::Equal)) {
            auto qualSeg1 = expect(TokenKind::Identifier, "module name");
            expect(TokenKind::ColonColon, "'::'");
            auto qualSeg2 = expect(TokenKind::Identifier, "function name");
            decl->importPath.push_back(std::string(qualSeg1.text));
            decl->importNames.push_back("__fn_alias:" + std::string(firstIdent.text) +
                "=" + std::string(qualSeg2.text));
            expect(TokenKind::Semicolon, "';'");
            return decl;
        }
        current_ = savedPos;
    }

    // Module path segments: accept Identifier or any soft-reserved name that
    // might legitimately be used as a module/filename (string / error / type
    // / module / interface / static / override). Without this,
    // `use std.string;` / `use std.error;` fail because `string`/`error`
    // tokenize as keywords.
    auto acceptPathSeg = [&](std::string_view diagLabel) -> Token {
        if (check(TokenKind::Identifier)) return advance();
        if (peek().isTypeName()) return advance();
        switch (peek().kind) {
            case TokenKind::KW_type:
            case TokenKind::KW_module:
            case TokenKind::KW_error:
            case TokenKind::KW_static:
            case TokenKind::KW_override:
            case TokenKind::KW_interface: {
                Token t = advance();
                t.kind = TokenKind::Identifier;
                return t;
            }
            default: break;
        }
        return expect(TokenKind::Identifier, diagLabel);
    };

    auto seg = acceptPathSeg("module path segment");
    decl->importPath.push_back(std::string(seg.text));

    while (!isAtEnd()) {
        if (match(TokenKind::ColonColon) || (isUse && match(TokenKind::Dot))) {
            if (check(TokenKind::LBrace)) break;
            if (check(TokenKind::Star)) {
                advance();
                decl->importNames.push_back("*");
                break;
            }
            auto next = acceptPathSeg("module path segment");
            decl->importPath.push_back(std::string(next.text));
        } else {
            break;
        }
    }

    if (match(TokenKind::LBrace)) {
        do {
            auto name = expect(TokenKind::Identifier, "import name");
            decl->importNames.push_back(std::string(name.text));
        } while (match(TokenKind::Comma));
        expect(TokenKind::RBrace, "'}'");
    }

    if (check(TokenKind::KW_as)) {
        advance();
        auto alias = expect(TokenKind::Identifier, "alias name");
        decl->importNames.clear();
        decl->importNames.push_back("__alias:" + std::string(alias.text));
    }

    expect(TokenKind::Semicolon, "';'");
    return decl;
}

DeclPtr Parser::parseModuleDecl() {
    auto loc = peek().location;
    expect(TokenKind::KW_module, "'module'");

    auto decl = std::make_unique<ImportDecl>();
    decl->location = loc;
    decl->isExport = true;

    auto acceptModuleSegment = [&]() -> std::string {
        if (check(TokenKind::Identifier) || peek().isTypeName()) {
            return std::string(advance().text);
        }
        // Accept soft-reserved words so a file may declare
        // `module std.error;` / `module std.string;` without renaming.
        switch (peek().kind) {
            case TokenKind::KW_type:
            case TokenKind::KW_module:
            case TokenKind::KW_error:
            case TokenKind::KW_static:
            case TokenKind::KW_override:
            case TokenKind::KW_interface:
                return std::string(advance().text);
            default: break;
        }
        diag_.error(peek().location, "expected module name segment, got {}", tokenKindToString(peek().kind));
        return "error";
    };
    std::string moduleName = acceptModuleSegment();
    while (match(TokenKind::Dot)) {
        moduleName += "." + acceptModuleSegment();
    }
    decl->importPath.push_back(moduleName);
    decl->importNames.push_back("__module");

    // Two forms:
    //   1. `module Foo.Bar;`            — file-wide (original semantics)
    //   2. `module Foo.Bar { decls... }` — C#-style block (scoped)
    // The block form emits a `__module Foo.Bar` sentinel, the inner decls,
    // then a `__module_end` sentinel. Inner type decls are renamed to
    // `Foo.Bar.Name` so they occupy a unique key in the symbol table and
    // a unique struct name at CodeGen (two `class Point` in disjoint
    // namespaces would otherwise collide as `%Point`). Inner USE sites of
    // a sibling short name (`return Point { ... }`) are resolved by Sema
    // via declModule_-aware lookup — no source-level rewriting needed.
    if (match(TokenKind::LBrace)) {
        size_t pendingStart = pendingDecls_.size();
        while (!check(TokenKind::RBrace) && !isAtEnd()) {
            auto inner = parseDeclaration();
            if (inner) pendingDecls_.push_back(std::move(inner));
        }
        expect(TokenKind::RBrace, "'}'");

        // Phase 1: collect every type decl's short name and pick its
        // fully-qualified target (`Foo.Bar.Point`). Nested `module A { module
        // B { ... } }` composes because the inner block has already renamed
        // its children to `B.Name` by the time this outer block sees them;
        // prepending `A.` yields the expected `A.B.Name`. `__module` /
        // `__module_end` sentinels emitted by inner blocks also get their
        // importPath prefixed so ImportResolver harvests `A.B` (not just
        // `B`) as a known inline namespace for dotted path collapsing.
        std::map<std::string, std::string> shortToQualified;
        for (size_t i = pendingStart; i < pendingDecls_.size(); ++i) {
            auto& pd = pendingDecls_[i];
            if (!pd) continue;
            switch (pd->kind) {
                case DeclKind::Class:
                case DeclKind::Struct:
                case DeclKind::Interface:
                case DeclKind::ErrorDef:
                case DeclKind::TypeAlias: {
                    std::string qualified = moduleName + "." + pd->name;
                    shortToQualified[pd->name] = qualified;
                    pd->name = std::move(qualified);
                    break;
                }
                case DeclKind::Import: {
                    auto* imp = pd->as<ImportDecl>();
                    if (!imp->importNames.empty() &&
                        imp->importNames[0] == "__module" &&
                        !imp->importPath.empty()) {
                        imp->importPath.back() =
                            moduleName + "." + imp->importPath.back();
                    }
                    break;
                }
                default:
                    break;
            }
        }

        // Phase 2: walk every inner decl and rewrite bare references to a
        // block-local short name into the corresponding qualified form so
        // downstream (Sema/Mono/CodeGen) never sees an ambiguous short
        // name. Skips decl->name itself (already renamed above) and
        // deliberately only rewrites names that appear in table — bare
        // identifiers that happen to coincide with an unrelated local
        // variable would be transformed too, but Sema rejects same-name
        // variable/type collisions so we don't run into that in practice.
        rewriteBlockShortNames(
            shortToQualified,
            pendingDecls_.begin() + pendingStart,
            pendingDecls_.end());

        auto endDecl = std::make_unique<ImportDecl>();
        endDecl->location = loc;
        endDecl->isExport = true;
        endDecl->importNames.push_back("__module_end");
        pendingDecls_.push_back(std::move(endDecl));
        return decl;
    }

    expect(TokenKind::Semicolon, "';'");
    return decl;
}

DeclPtr Parser::parseImplBlock() {
    auto loc = peek().location;
    expect(TokenKind::KW_impl, "'impl'");

    std::vector<std::string> implGenericParams;
    if (match(TokenKind::Less)) {
        do {
            auto gp = expect(TokenKind::Identifier, "generic parameter");
            implGenericParams.push_back(std::string(gp.text));
        } while (match(TokenKind::Comma));
        expect(TokenKind::Greater, "'>'");
    }

    auto className = expect(TokenKind::Identifier, "class name");
    std::string fullName = std::string(className.text);

    if (match(TokenKind::Less)) {
        fullName += "<";
        bool first = true;
        while (!check(TokenKind::Greater) && !isAtEnd()) {
            if (!first) fullName += ",";
            auto tp = expect(TokenKind::Identifier, "type parameter");
            fullName += std::string(tp.text);
            first = false;
            if (!check(TokenKind::Greater)) match(TokenKind::Comma);
        }
        expect(TokenKind::Greater, "'>'");
        fullName += ">";
    }

    auto decl = std::make_unique<ClassDecl>();
    decl->location = loc;
    decl->isImplBlock = true;
    decl->genericParams = std::move(implGenericParams);

    if (check(TokenKind::KW_for)) {
        advance();
        decl->interfaces.push_back(std::string(className.text));
        // `impl Trait for <Target>`: accept either an Identifier (class / struct /
        // user-type name) OR a primitive-keyword type token (i32 / i64 / f32 /
        // bool / char / rawptr / usize / isize / ...).  This matches Rust's
        // `impl Hash for i32` form.  The lexer tokenises `i32` as KW_i32 rather
        // than Identifier, so we can't just call expect(Identifier, ...).
        // NOTE: `string` is NOT a keyword (see Lexer/Token.h) so it's already
        // handled by the Identifier branch.
        auto tgt = peek();
        bool isPrimitiveTargetKw = false;
        switch (tgt.kind) {
            case TokenKind::KW_i8:   case TokenKind::KW_i16:
            case TokenKind::KW_i32:  case TokenKind::KW_i64:
            case TokenKind::KW_u8:   case TokenKind::KW_u16:
            case TokenKind::KW_u32:  case TokenKind::KW_u64:
            case TokenKind::KW_f32:  case TokenKind::KW_f64:
            case TokenKind::KW_bool: case TokenKind::KW_char:
            case TokenKind::KW_isize: case TokenKind::KW_usize:
            case TokenKind::KW_rawptr:
                isPrimitiveTargetKw = true;
                break;
            default:
                break;
        }
        if (isPrimitiveTargetKw) {
            advance();
            decl->name = std::string(tgt.text);
        } else {
            auto targetClass = expect(TokenKind::Identifier, "target type name");
            decl->name = std::string(targetClass.text);
        }

        // The target of an impl is nominal. Parse its generic spelling with
        // the normal declaration-generic parser so `impl Trait for Map<K, V>`
        // both reaches the following `where` / body and keeps K/V in scope for
        // the impl methods. The target's base name remains the semantic lookup
        // key; only its fresh parameters are propagated to the impl block.
        if (check(TokenKind::Less)) {
            ClassDecl targetShape;
            targetShape.name = decl->name;
            parseGenericParams(targetShape);
            for (auto& param : targetShape.genericParams) {
                bool alreadyDeclared = false;
                for (const auto& existing : decl->genericParams) {
                    if (existing == param) {
                        alreadyDeclared = true;
                        break;
                    }
                }
                if (!alreadyDeclared) decl->genericParams.push_back(std::move(param));
            }
        }
    } else {
        decl->name = std::string(className.text);
    }

    if (match(TokenKind::KW_where)) {
        parseWhereClauseInto(*decl);
    }

    expect(TokenKind::LBrace, "'{'");
    while (!check(TokenKind::RBrace) && !isAtEnd()) {
        // P2-generics C5: `type Item = ConcreteType;` associated-type binding.
        // Also supports qualified form `type A::Item = X;` for disambiguation
        // when the target implements multiple traits with the same assoc name.
        // Allowed only inside `impl Trait for Target` blocks; populates
        // ClassDecl::associatedTypes so Sema can resolve `T::Item` after
        // T is bound to Target.
        if (check(TokenKind::KW_type)) {
            advance();
            auto typeName = expect(TokenKind::Identifier, "associated type name");
            AssociatedType at;
            // Check for qualified `type TraitName::AssocName = X;`
            if (check(TokenKind::ColonColon)) {
                advance(); // consume ::
                at.qualifyingTrait = std::string(typeName.text);
                auto memberName = expect(TokenKind::Identifier, "associated type member name");
                at.name = std::string(memberName.text);
            } else {
                at.name = std::string(typeName.text);
            }
            if (match(TokenKind::Equal)) {
                at.defaultType = parseType();
            } else {
                diag_.error(peek().location,
                    "associated type '{}' in impl block requires `= <type>` binding",
                    at.name);
            }
            match(TokenKind::Semicolon);
            decl->associatedTypes.push_back(std::move(at));
            continue;
        }

        bool isPublic = match(TokenKind::KW_public);
        bool isStaticM = false;
        if (check(TokenKind::KW_static)) { advance(); isStaticM = true; }

        if (check(TokenKind::KW_fn)) {
            auto fnDecl = parseFunctionDecl(isPublic, false);
            if (fnDecl) {
                auto* fn = fnDecl->as<FunctionDecl>();
                MethodDecl method;
                method.name = fn->name;
                method.params = std::move(fn->params);
                method.returnType = std::move(fn->returnType);
                method.body = std::move(fn->body);
                method.isStatic = isStaticM;
                method.visibility = isPublic ? Visibility::Public : Visibility::Private;
                method.attributes = std::move(fnDecl->attributes);
                method.genericParams = std::move(fnDecl->genericParams);
                method.genericConstraints = std::move(fnDecl->genericConstraints);
                method.typeEqualityConstraints = std::move(fnDecl->typeEqualityConstraints);
                method.typeInequalityConstraints = std::move(fnDecl->typeInequalityConstraints);
                method.genericConstParams = std::move(fnDecl->genericConstParams);
                method.packConstraints = std::move(fnDecl->packConstraints);
                decl->methods.push_back(std::move(method));
            }
        } else {
            diag_.error(peek().location, "expected 'fn' in impl block");
            advance();
        }
    }
    expect(TokenKind::RBrace, "'}'");
    return decl;
}

DeclPtr Parser::parseExternBlock() {
    auto loc = peek().location;
    expect(TokenKind::KW_extern, "'extern'");

    auto decl = std::make_unique<ExternBlockDecl>();
    decl->location = loc;

    auto abi = expect(TokenKind::StringLiteral, "ABI string");
    decl->externABI = abi.stringValue;

    if (check(TokenKind::Identifier) && peek().text == "namespace") {
        advance();
        auto ns = expect(TokenKind::StringLiteral, "namespace name");
        decl->namespaceName = ns.stringValue;
    }

    expect(TokenKind::LBrace, "'{'");
    while (!check(TokenKind::RBrace) && !isAtEnd()) {
        // Per-declaration attributes inside the extern block: e.g.
        // `@[link_name("Sleep")] fn win_Sleep(ms: i32);` for renaming a
        // Windows API import. Collect and attach to the next function.
        auto innerAttrs = parseAttributes();
        if (check(TokenKind::KW_class)) {
            auto classDecl = parseClassDecl(false);
            if (classDecl) {
                classDecl->externABI = decl->externABI;
                classDecl->attributes.insert(classDecl->attributes.end(),
                    innerAttrs.begin(), innerAttrs.end());
                decl->externDecls.push_back(std::move(classDecl));
            }
        } else if (check(TokenKind::KW_struct)) {
            auto structDecl = parseStructDecl(false);
            if (structDecl) {
                structDecl->externABI = decl->externABI;
                structDecl->attributes.insert(structDecl->attributes.end(),
                    innerAttrs.begin(), innerAttrs.end());
                decl->externDecls.push_back(std::move(structDecl));
            }
        } else if (check(TokenKind::KW_enum)) {
            auto enumDecl = parseErrorDecl(false);
            if (enumDecl) {
                enumDecl->externABI = decl->externABI;
                enumDecl->attributes.insert(enumDecl->attributes.end(),
                    innerAttrs.begin(), innerAttrs.end());
                decl->externDecls.push_back(std::move(enumDecl));
            }
        } else {
            auto fn = parseFunctionDecl(false, false);
            if (fn) {
                fn->externABI = decl->externABI;
                fn->attributes.insert(fn->attributes.end(),
                    innerAttrs.begin(), innerAttrs.end());
            }
            decl->externDecls.push_back(std::move(fn));
        }
    }
    expect(TokenKind::RBrace, "'}'");

    return decl;
}

// ============================================================
//  Block-form namespace short-name rewriter
// ============================================================
//
// Walks every decl in `[begin, end)` (the decls produced directly by a
// `module Foo.Bar { ... }` block) and rewrites any bare reference to a
// type declared in the same block into its fully-qualified form. Visits:
//
//   - Every TypeAnnotation subtree that appears in field types, return
//     types, parameter types, generic args, var-decl types, cast target
//     types, match-arm type patterns, etc.
//   - Every IdentifierExpr name (so `Point.make()` — where `Point` is the
//     callee — becomes `Foo.Bar.Point.make()` and routes through CodeGen's
//     static-dispatch mangling intact).
//   - Every StructInitExpr.structName (so `Point { x: 1 }` becomes
//     `Foo.Bar.Point { x: 1 }` and hits the right symbol-table entry).
//   - ClassDecl.parentName / ClassDecl.interfaces so `class Sub : Base`
//     picks up an in-block `Base`.
//
// Does NOT rewrite identifiers that happen to match a block-local name
// but are plainly value identifiers (variables, parameters) — but since
// the rewrite table contains only TYPE short names (class/struct/
// interface/errordef/typealias) and those names never shadow a local
// variable in the same scope (Sema rejects the name collision later),
// this is a safe simplification.
namespace {

struct NsRewriter {
    const std::map<std::string, std::string>& table;

    void structInitName(std::string& name) {
        auto it = table.find(name);
        if (it != table.end()) {
            name = it->second;
            return;
        }

        // Generic struct initialization is stored as a single mangled name
        // such as `PriorityQueue<T>`, rather than a GenericType annotation.
        // Qualify its base while preserving the argument spelling.
        auto genericStart = name.find('<');
        if (genericStart == std::string::npos) return;
        auto base = name.substr(0, genericStart);
        it = table.find(base);
        if (it != table.end()) name = it->second + name.substr(genericStart);
    }

    void typeAnn(TypeAnnotation* ta) {
        if (!ta) return;
        switch (ta->kind) {
            case TypeAnnotationKind::Named: {
                auto it = table.find(ta->name);
                if (it != table.end()) ta->name = it->second;
                break;
            }
            case TypeAnnotationKind::Pointer: {
                auto* p = ta->as<PointerType>();
                typeAnn(p->innerType.get());
                break;
            }
            case TypeAnnotationKind::Reference: {
                auto* r = ta->as<ReferenceType>();
                typeAnn(r->innerType.get());
                break;
            }
            case TypeAnnotationKind::Array: {
                auto* a = ta->as<ArrayType>();
                typeAnn(a->elementType.get());
                expr(a->size.get());
                break;
            }
            case TypeAnnotationKind::Tuple: {
                auto* t = ta->as<TupleType>();
                for (auto& e : t->elements) typeAnn(e.get());
                break;
            }
            case TypeAnnotationKind::Function: {
                auto* f = ta->as<FunctionType>();
                for (auto& pt : f->paramTypes) typeAnn(pt.get());
                typeAnn(f->returnType.get());
                break;
            }
            case TypeAnnotationKind::Generic: {
                auto* g = ta->as<GenericType>();
                auto it = table.find(g->name);
                if (it != table.end()) g->name = it->second;
                for (auto& arg : g->typeArgs) typeAnn(arg.get());
                for (auto& ae : g->argExprs) expr(ae.get());
                break;
            }
            case TypeAnnotationKind::Union: {
                auto* u = ta->as<UnionType>();
                for (auto& m : u->members) typeAnn(m.get());
                break;
            }
            default:
                break;
        }
    }

    void expr(Expr* e) {
        if (!e) return;
        switch (e->kind) {
            case ExprKind::Identifier: {
                auto* ie = e->as<IdentifierExpr>();
                auto it = table.find(ie->name);
                if (it != table.end()) ie->name = it->second;
                for (auto& ta : ie->callTypeArgs) typeAnn(ta.get());
                for (auto& ae : ie->callArgExprs) expr(ae.get());
                break;
            }
            case ExprKind::BinaryOp: {
                auto* b = e->as<BinaryOpExpr>();
                expr(b->lhs.get());
                expr(b->rhs.get());
                break;
            }
            case ExprKind::UnaryOp: {
                auto* u = e->as<UnaryOpExpr>();
                expr(u->operand.get());
                break;
            }
            case ExprKind::Call: {
                auto* c = e->as<CallExpr>();
                expr(c->callee.get());
                for (auto& a : c->args) expr(a.get());
                break;
            }
            case ExprKind::MemberAccess: {
                auto* ma = e->as<MemberAccessExpr>();
                expr(ma->object.get());
                for (auto& ta : ma->callTypeArgs) typeAnn(ta.get());
                break;
            }
            case ExprKind::Index: {
                auto* ix = e->as<IndexExpr>();
                expr(ix->object.get());
                expr(ix->indexExpr.get());
                break;
            }
            case ExprKind::Assignment: {
                auto* a = e->as<AssignmentExpr>();
                expr(a->lhs.get());
                expr(a->rhs.get());
                break;
            }
            case ExprKind::CompoundAssignment: {
                auto* a = e->as<CompoundAssignmentExpr>();
                expr(a->target.get());
                expr(a->value.get());
                break;
            }
            case ExprKind::Cast: {
                auto* c = e->as<CastExpr>();
                expr(c->operand.get());
                typeAnn(c->targetType.get());
                break;
            }
            case ExprKind::StructInit: {
                auto* si = e->as<StructInitExpr>();
                structInitName(si->structName);
                typeAnn(si->typeAnnotation.get());
                for (auto& [_n, v] : si->fieldInits) expr(v.get());
                expr(si->spreadBase.get());
                break;
            }
            case ExprKind::ArrayInit: {
                auto* ai = e->as<ArrayInitExpr>();
                for (auto& el : ai->elements) expr(el.get());
                expr(ai->repeatCount.get());
                break;
            }
            case ExprKind::TupleInit: {
                auto* ti = e->as<TupleInitExpr>();
                for (auto& el : ti->elements) expr(el.get());
                break;
            }
            case ExprKind::StringInterpolation: {
                auto* si = e->as<StringInterpExpr>();
                for (auto& p : si->parts) {
                    if (p.isExpr) expr(p.expr.get());
                }
                break;
            }
            case ExprKind::TryExpr: {
                auto* t = e->as<TryExpr>();
                expr(t->inner.get());
                break;
            }
            case ExprKind::Ternary: {
                auto* t = e->as<TernaryExpr>();
                expr(t->condition.get());
                expr(t->trueExpr.get());
                expr(t->falseExpr.get());
                break;
            }
            case ExprKind::Closure: {
                auto* c = e->as<ClosureExpr>();
                for (auto& cap : c->captures) expr(cap.moveExpr.get());
                for (auto& p : c->params) typeAnn(p.type.get());
                typeAnn(c->returnType.get());
                stmt(c->body.get());
                expr(c->singleExpr.get());
                break;
            }
            case ExprKind::AwaitExpr: {
                auto* a = e->as<AwaitExpr>();
                expr(a->inner.get());
                break;
            }
            case ExprKind::InlineAsm: {
                auto* ia = e->as<InlineAsmExpr>();
                for (auto& op : ia->operands) expr(op.get());
                break;
            }
            default:
                break;
        }
    }

    void stmt(Stmt* s) {
        if (!s) return;
        switch (s->kind) {
            case StmtKind::VarDecl: {
                auto* vd = s->as<VarDeclStmt>();
                typeAnn(vd->varType.get());
                expr(vd->initExpr.get());
                stmt(vd->elseBranch.get());
                break;
            }
            case StmtKind::ExprStmt: {
                auto* es = s->as<ExprStmt>();
                expr(es->expr.get());
                if (es->localDecl) decl(es->localDecl.get());
                break;
            }
            case StmtKind::Return: {
                auto* rs = s->as<ReturnStmt>();
                expr(rs->expr.get());
                break;
            }
            case StmtKind::If: {
                auto* is_ = s->as<IfStmt>();
                expr(is_->condition.get());
                stmt(is_->thenBranch.get());
                for (auto& [c, b] : is_->elifBranches) {
                    expr(c.get());
                    stmt(b.get());
                }
                stmt(is_->elseBranch.get());
                break;
            }
            case StmtKind::While: {
                auto* w = s->as<WhileStmt>();
                expr(w->condition.get());
                stmt(w->body.get());
                break;
            }
            case StmtKind::For: {
                auto* f = s->as<ForStmt>();
                stmt(f->init.get());
                expr(f->condition.get());
                expr(f->step.get());
                stmt(f->body.get());
                break;
            }
            case StmtKind::ForEach: {
                auto* fe = s->as<ForEachStmt>();
                expr(fe->collection.get());
                stmt(fe->body.get());
                break;
            }
            case StmtKind::Block: {
                auto* b = s->as<BlockStmt>();
                for (auto& sub : b->statements) stmt(sub.get());
                break;
            }
            case StmtKind::Match: {
                auto* m = s->as<MatchStmt>();
                expr(m->expr.get());
                for (auto& arm : m->arms) {
                    typeAnn(arm.typePattern.get());
                    expr(arm.valuePattern.get());
                    expr(arm.guardExpr.get());
                    stmt(arm.body.get());
                }
                break;
            }
            case StmtKind::Assignment: {
                auto* as = s->as<AssignStmt>();
                expr(as->target.get());
                expr(as->value.get());
                break;
            }
            case StmtKind::Defer: {
                auto* d = s->as<DeferStmt>();
                stmt(d->body.get());
                break;
            }
            case StmtKind::StaticAssert: {
                auto* sa = s->as<StaticAssertStmt>();
                expr(sa->expr.get());
                break;
            }
            case StmtKind::Unsafe: {
                auto* us = s->as<UnsafeStmt>();
                stmt(us->body.get());
                break;
            }
            default:
                break;
        }
    }

    void paramList(std::vector<ParamDecl>& params) {
        for (auto& p : params) {
            typeAnn(p.type.get());
            expr(p.defaultValue.get());
        }
    }

    void decl(Decl* d) {
        if (!d) return;
        switch (d->kind) {
            case DeclKind::Function: {
                auto* fd = d->as<FunctionDecl>();
                paramList(fd->params);
                typeAnn(fd->returnType.get());
                stmt(fd->body.get());
                break;
            }
            case DeclKind::Class: {
                auto* cd = d->as<ClassDecl>();
                if (!cd->parentName.empty()) {
                    auto it = table.find(cd->parentName);
                    if (it != table.end()) cd->parentName = it->second;
                }
                for (auto& iface : cd->interfaces) {
                    auto it = table.find(iface);
                    if (it != table.end()) iface = it->second;
                }
                for (auto& f : cd->fields) {
                    typeAnn(f.type.get());
                    expr(f.defaultValue.get());
                }
                for (auto& m : cd->methods) {
                    paramList(m.params);
                    typeAnn(m.returnType.get());
                    stmt(m.body.get());
                }
                break;
            }
            case DeclKind::Struct: {
                auto* sd = d->as<StructDecl>();
                if (!sd->parentName.empty()) {
                    auto it = table.find(sd->parentName);
                    if (it != table.end()) sd->parentName = it->second;
                }
                for (auto& f : sd->fields) {
                    typeAnn(f.type.get());
                    expr(f.defaultValue.get());
                }
                for (auto& m : sd->methods) {
                    paramList(m.params);
                    typeAnn(m.returnType.get());
                    stmt(m.body.get());
                }
                break;
            }
            case DeclKind::Interface: {
                auto* ifd = d->as<InterfaceDecl>();
                if (!ifd->parentName.empty()) {
                    auto it = table.find(ifd->parentName);
                    if (it != table.end()) ifd->parentName = it->second;
                }
                for (auto& iface : ifd->interfaces) {
                    auto it = table.find(iface);
                    if (it != table.end()) iface = it->second;
                }
                for (auto& m : ifd->methods) {
                    paramList(m.params);
                    typeAnn(m.returnType.get());
                    stmt(m.body.get());
                }
                break;
            }
            case DeclKind::ErrorDef: {
                auto* ed = d->as<ErrorDefDecl>();
                for (auto& variantPayload : ed->variantTypes) {
                    for (auto& pt : variantPayload) typeAnn(pt.get());
                }
                for (auto& m : ed->methods) {
                    paramList(m.params);
                    typeAnn(m.returnType.get());
                    stmt(m.body.get());
                }
                break;
            }
            case DeclKind::TypeAlias: {
                auto* ta = d->as<TypeAliasDecl>();
                typeAnn(ta->aliasType.get());
                break;
            }
            case DeclKind::GlobalVar: {
                auto* gv = d->as<GlobalVarDecl>();
                typeAnn(gv->varType.get());
                stmt(gv->initBody.get());
                break;
            }
            default:
                break;
        }
    }
};

} // namespace

/*static*/ void Parser::rewriteBlockShortNames(
    const std::map<std::string, std::string>& shortToQualified,
    const std::vector<DeclPtr>::iterator& begin,
    const std::vector<DeclPtr>::iterator& end)
{
    if (shortToQualified.empty()) return;
    NsRewriter r{shortToQualified};
    for (auto it = begin; it != end; ++it) {
        if (*it) r.decl(it->get());
    }
}
} // namespace vyx

