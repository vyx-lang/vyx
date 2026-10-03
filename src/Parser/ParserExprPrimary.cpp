#include "Parser.h"

#include <cstdlib>
#include <functional>

namespace vyx {

ExprPtr Parser::parsePrimary() {
    auto loc = peek().location;

    // `fn`-literal in expression position: `fn(x: T, y: U) -> R { ... }`
    // or the return-type-less form `fn(x: T) { ... }`. Lowered to a
    // capture-less ClosureExpr so existing closure codegen / mono paths
    // pick it up uniformly.
    //
    // Disambiguation: at statement start, `fn` introduces a declaration
    // (`fn foo(...) -> T { ... }`), but parseStatement routes decl-starting
    // tokens before calling parseExpression, so reaching parsePrimary with
    // `fn` implies an expression-position literal. We still guard with
    // `peekNext().kind == LParen` so a stray `fn Bar` (mis-placed decl)
    // falls through to the generic error path instead of being silently
    // reinterpreted as a closure.
    if (check(TokenKind::KW_fn) && peekNext().kind == TokenKind::LParen) {
        advance(); // consume `fn`
        auto e = std::make_unique<ClosureExpr>();
        e->location = loc;
        expect(TokenKind::LParen, "'('");
        if (!check(TokenKind::RParen)) {
            do {
                ClosureParam cp;
                auto pname = expect(TokenKind::Identifier, "parameter name");
                cp.name = std::string(pname.text);
                if (match(TokenKind::Colon)) {
                    cp.type = parseType();
                }
                e->params.push_back(std::move(cp));
            } while (match(TokenKind::Comma));
        }
        expect(TokenKind::RParen, "')'");
        if (match(TokenKind::Arrow)) {
            e->returnType = parseType();
        }
        if (check(TokenKind::LBrace)) {
            e->body = parseBlock();
        } else {
            // Expression-bodied form `fn(x) expr` isn't part of the
            // canonical grammar, but accept it symmetrically with `|x| expr`
            // so call-site ergonomics match the `|...|` form.
            e->singleExpr = parseExpression();
        }
        return e;
    }

    // Lambda: `|params| expr_or_block` OR zero-arg `|| expr_or_block`.
    // The lexer tokenises `||` as PipePipe (logical OR) — at expression-start
    // position that's unambiguously an empty closure-param list since logical
    // OR can't appear as a prefix operator.
    if (check(TokenKind::Pipe) || check(TokenKind::PipePipe)) {
        bool zeroArg = check(TokenKind::PipePipe);
        advance();
        auto e = std::make_unique<ClosureExpr>();
        e->location = loc;
        if (!zeroArg) {
            if (!check(TokenKind::Pipe)) {
                do {
                    ClosureParam param;
                    auto nameTok = expect(TokenKind::Identifier, "lambda parameter");
                    param.name = std::string(nameTok.text);
                    if (match(TokenKind::Colon)) {
                        param.type = parseBaseType();
                    }
                    e->params.push_back(std::move(param));
                } while (match(TokenKind::Comma));
            }
            expect(TokenKind::Pipe, "'|'");
        }
        if (match(TokenKind::Arrow)) {
            e->returnType = parseType();
        }
        if (check(TokenKind::LBrace)) {
            e->body = parseBlock();
        } else {
            e->singleExpr = parseExpression();
        }
        return e;
    }

    if (check(TokenKind::IntLiteral)) {
        auto tok = advance();
        return ast::makeIntLiteral(loc, tok.intValue);
    }

    if (check(TokenKind::FloatLiteral)) {
        auto tok = advance();
        return ast::makeFloatLiteral(loc, tok.floatValue);
    }

    if (check(TokenKind::BoolLiteral)) {
        auto tok = advance();
        return ast::makeBoolLiteral(loc, tok.boolValue);
    }

    if (check(TokenKind::StringLiteral)) {
        auto tok = advance();
        return parseStringInterpolation(tok);
    }

    if (check(TokenKind::CharLiteral)) {
        auto tok = advance();
        auto e = std::make_unique<CharLiteralExpr>();
        e->location = loc;
        e->value = tok.stringValue;
        return e;
    }

    if (match(TokenKind::KW_null)) {
        auto e = std::make_unique<NullLiteralExpr>();
        e->location = loc;
        return e;
    }

    if (match(TokenKind::KW_self)) {
        auto e = std::make_unique<SelfExpr>();
        e->location = loc;
        return e;
    }

    // inline asm
    if (check(TokenKind::KW_asm)) {
        auto asmLoc = advance().location;
        expect(TokenKind::LParen, "'('");
        auto templateTok = expect(TokenKind::StringLiteral, "asm template string");
        auto e = std::make_unique<InlineAsmExpr>();
        e->location = asmLoc;
        e->asmTemplate = std::string(templateTok.stringValue);
        std::string constraints;
        while (match(TokenKind::Comma)) {
            if (check(TokenKind::StringLiteral)) {
                auto cstr = advance();
                if (!constraints.empty()) constraints += ",";
                constraints += std::string(cstr.stringValue);
            } else {
                e->operands.push_back(parseExpression());
            }
        }
        e->constraints = constraints;
        expect(TokenKind::RParen, "')'");
        return e;
    }

    // if-else expression
    if (check(TokenKind::KW_if)) {
        auto ifLoc = advance().location;
        expect(TokenKind::LParen, "'('");
        auto cond = parseExpression();
        expect(TokenKind::RParen, "')'");
        ExprPtr trueVal;
        if (check(TokenKind::LBrace)) {
            advance();
            trueVal = parseExpression();
            match(TokenKind::Semicolon);
            expect(TokenKind::RBrace, "'}'");
        } else {
            trueVal = parseExpression();
        }
        ExprPtr falseVal;
        if (match(TokenKind::KW_else)) {
            if (check(TokenKind::LBrace)) {
                advance();
                falseVal = parseExpression();
                match(TokenKind::Semicolon);
                expect(TokenKind::RBrace, "'}'");
            } else if (check(TokenKind::KW_if)) {
                falseVal = parsePrimary();
            } else {
                falseVal = parseExpression();
            }
        } else if (check(TokenKind::KW_elif)) {
            tokens_[current_].kind = TokenKind::KW_if;
            falseVal = parsePrimary();
        } else {
            falseVal = ast::makeIntLiteral(ifLoc, 0);
        }
        auto e = std::make_unique<TernaryExpr>();
        e->location = ifLoc;
        e->condition = std::move(cond);
        e->trueExpr = std::move(trueVal);
        e->falseExpr = std::move(falseVal);
        return e;
    }

    // when expression
    if (check(TokenKind::KW_when)) {
        auto whenLoc = advance().location;
        expect(TokenKind::LBrace, "'{'");

        struct WhenArm { ExprPtr cond; ExprPtr val; bool isElse = false; };
        std::vector<WhenArm> arms;
        while (!check(TokenKind::RBrace) && !isAtEnd()) {
            WhenArm arm;
            if (match(TokenKind::KW_else)) {
                arm.isElse = true;
                expect(TokenKind::FatArrow, "'=>'");
                arm.val = parseExpression();
                match(TokenKind::Semicolon);
                arms.push_back(std::move(arm));
                break;
            }
            arm.cond = parseExpression();
            expect(TokenKind::FatArrow, "'=>'");
            arm.val = parseExpression();
            match(TokenKind::Semicolon);
            arms.push_back(std::move(arm));
        }
        expect(TokenKind::RBrace, "'}'");

        ExprPtr result = ast::makeIntLiteral(whenLoc, 0);
        for (int i = static_cast<int>(arms.size()) - 1; i >= 0; --i) {
            if (arms[i].isElse) {
                result = std::move(arms[i].val);
            } else {
                auto e = std::make_unique<TernaryExpr>();
                e->location = whenLoc;
                e->condition = std::move(arms[i].cond);
                e->trueExpr = std::move(arms[i].val);
                e->falseExpr = std::move(result);
                result = std::move(e);
            }
        }
        return result;
    }

    // try prefix operator — `try expr` where expr: Result<T,E>
    // `try` is not a keyword token; match it as an identifier named "try".
    if (check(TokenKind::Identifier) && peek().text == "try") {
        auto tryLoc = advance().location; // consume 'try'
        auto operand = parseUnary(); // parse operand (handles calls, postfix, etc.)
        auto e = std::make_unique<TryExpr>();
        e->location = tryLoc;
        e->inner = std::move(operand);
        return e;
    }

    // sizeof...(pack) — compile-time pack-length query
    // Syntax: sizeof ... ( identifier )
    if (check(TokenKind::Identifier) && peek().text == "sizeof" &&
        current_ + 1 < tokens_.size() &&
        tokens_[current_ + 1].is(TokenKind::Ellipsis)) {
        auto sizeofLoc = advance().location; // consume 'sizeof'
        advance(); // consume '...'
        expect(TokenKind::LParen, "'(' after 'sizeof...'");
        auto identTok = expect(TokenKind::Identifier, "pack name");
        expect(TokenKind::RParen, "')' after pack name in 'sizeof...'");
        auto e = std::make_unique<SizeofPackExpr>();
        e->location = sizeofLoc;
        e->packName = std::string(identTok.text);
        return e;
    }

    // Primitive type keyword as LHS of `::` for static method calls
    // (e.g. `i32::zero()`, `bool::default_value()`, `f64::one()`).
    //
    // A primitive-type keyword by itself in value position is already accepted
    // further down via `peek().isTypeName()` — it decays to an IdentifierExpr
    // so `typeinfo(i32)` works.  That branch, however, runs *after* the
    // Identifier branch, so a `KW_i32` token followed by `::` never reaches
    // the identifier-`::` handler: parsePrimary returns the bare IdentifierExpr
    // and the outer parser then sees an unexpected `::`.
    //
    // To keep `i32::zero()` uniform with `T::zero()` (where T was bound to i32
    // at instantiation time — see Phase 8's `hasStaticMethodForConcreteType`
    // rewrite in lowerTypeReflectInExpr), we rewrite `<prim>::<member>` here
    // into a MemberAccess on the primitive-named Identifier, exactly like
    // the `Type::Variant` path below.  Sema's analyzeMemberAccess then routes
    // through primitiveMethodImpls_ the same way `i32.zero()` does.
    //
    // Does NOT fire for `i32` in type-annotation position (that path goes
    // through parseType, not parsePrimary), nor for `i32(value)` casts
    // (those are handled downstream once the bare `i32` identifier reaches
    // parsePostfix's LParen branch).
    if (peek().isTypeName() && current_ + 1 < tokens_.size() &&
        tokens_[current_ + 1].is(TokenKind::ColonColon)) {
        auto tok = advance();      // consume the type keyword
        advance();                 // consume `::`
        // Accept an Identifier OR another type keyword as the member name
        // (defensive — `i32::i64` has no sane meaning, but `Nested::i32` etc
        // should still parse cleanly; we just forward to MemberAccess).
        std::string memberName;
        if (check(TokenKind::Identifier) || peek().isTypeName()) {
            memberName = std::string(advance().text);
        } else {
            diag_.error(peek().location,
                "expected member name after '{}::', got {}",
                tok.text, tokenKindToString(peek().kind));
            memberName = "<error>";
        }
        auto idExpr = ast::makeIdentifier(loc, std::string(tok.text));
        return ast::makeMemberAccess(loc, std::move(idExpr), std::move(memberName));
    }

    // Identifier
    if (check(TokenKind::Identifier)) {
        auto tok = advance();

        // C#-style fully-qualified struct init: `A.B.Point { field: val }`.
        // If the next tokens form `.Ident(.Ident)* {` — and the brace is
        // followed by a struct-init-looking field (`Ident :`) or an empty
        // `{}` — collapse the dotted chain into a single qualified name
        // and parse the rest as StructInit. Without this the chain parses
        // as MemberAccess and the `{ field: value }` tail fails.
        if (check(TokenKind::Dot) && current_ + 1 < tokens_.size() &&
            tokens_[current_ + 1].kind == TokenKind::Identifier) {
            size_t savedPos = current_;
            std::string qualified = std::string(tok.text);
            while (check(TokenKind::Dot) && current_ + 1 < tokens_.size() &&
                   tokens_[current_ + 1].kind == TokenKind::Identifier) {
                advance(); // '.'
                auto seg = advance();
                qualified += ".";
                qualified += std::string(seg.text);
            }
            // Peek for `{` followed by struct-init shape.
            bool isStructInit = false;
            if (check(TokenKind::LBrace) && current_ + 1 < tokens_.size()) {
                auto& nx = tokens_[current_ + 1];
                if (nx.is(TokenKind::RBrace)) {
                    isStructInit = true;
                } else if (nx.is(TokenKind::Identifier) &&
                           current_ + 2 < tokens_.size() &&
                           tokens_[current_ + 2].is(TokenKind::Colon)) {
                    isStructInit = true;
                }
            }
            if (isStructInit) {
                advance(); // consume `{`
                auto si = std::make_unique<StructInitExpr>();
                si->location = loc;
                si->structName = std::move(qualified);
                if (!check(TokenKind::RBrace)) {
                    do {
                        if (check(TokenKind::RBrace)) break;
                        if (check(TokenKind::DotDot)) {
                            advance();
                            si->spreadBase = parseExpression();
                            break;
                        }
                        auto fieldName = expect(TokenKind::Identifier, "field name");
                        expect(TokenKind::Colon, "':'");
                        auto fieldVal = parseExpression();
                        si->fieldInits.emplace_back(std::string(fieldName.text),
                                                     std::move(fieldVal));
                    } while (match(TokenKind::Comma));
                }
                expect(TokenKind::RBrace, "'}'");
                return si;
            }
            // Not a struct init — fall back: restore tokens and let the
            // normal expression/member-access parser handle the chain.
            current_ = savedPos;
        }

        if (match(TokenKind::ColonColon)) {
            // P3-Q: T::kind / T::name / T::fields / T::methods
            // Heuristic: LHS starts with uppercase → treat as type parameter.
            if (check(TokenKind::Identifier)) {
                static const char* kReflectMembers[] = {
                    "kind", "name", "fields", "methods", nullptr
                };
                std::string_view rhsText = peek().text;
                bool isReflectMember = false;
                for (const char** rm = kReflectMembers; *rm; ++rm) {
                    if (rhsText == *rm) { isReflectMember = true; break; }
                }
                bool lhsUppercase = !tok.text.empty() &&
                    std::isupper(static_cast<unsigned char>(tok.text[0]));
                // Lowercase primitive-type names (currently only `string`,
                // which is lexed as Identifier rather than a keyword — see
                // Token.h note) also act as a type-name LHS for static
                // method dispatch: `string::default_value()` must resolve
                // through primitiveMethodImpls_["string"]["default_value"]
                // just like `i32::zero()`.  We treat those identically to
                // the uppercase `Type::Variant` case below.
                static const char* kLowercasePrimTypes[] = {
                    "string", nullptr
                };
                bool lhsIsLowercasePrim = false;
                for (const char** p = kLowercasePrimTypes; *p; ++p) {
                    if (tok.text == *p) { lhsIsLowercasePrim = true; break; }
                }
                if (lhsUppercase || lhsIsLowercasePrim) {
                    auto memberTok = advance(); // consume member name
                    if (isReflectMember && lhsUppercase) {
                        // Reflection members only fire for uppercase LHS —
                        // `string::kind` etc. doesn't make sense.
                        return ast::makeTypeReflect(loc,
                            std::string(tok.text),
                            std::string(memberTok.text));
                    }
                    // Not a reflection member — treat `Type::Variant` as a
                    // MemberAccess on the type identifier. Downstream Sema
                    // handles `ErrorType.Variant` (user `error` decls) via
                    // analyzeMemberAccess; `class::fn` static-method calls
                    // likewise go through the same path. This makes
                    // `Op::Add` a natural alias for `Op.Add`.
                    auto idExpr = ast::makeIdentifier(loc, std::string(tok.text));
                    return ast::makeMemberAccess(loc,
                        std::move(idExpr),
                        std::string(memberTok.text));
                }
            }
            if (check(TokenKind::Less)) {
                advance();
                auto e = ast::makeIdentifier(loc, std::string(tok.text));
                // Parse each turbofish argument: integer literals (and negated
                // literals) are non-type (const) generic arguments that go into
                // callArgExprs; type annotations go into callTypeArgs. This mirrors
                // GenericType::argExprs in the type-position parser so downstream
                // Sema can evaluate integer slots without treating them as types.
                // Mini const-expression parser for turbofish slots. Accepts
                //   atom = IntLit | FloatLit | `-` atom | Identifier
                //   mul  = atom (( `*` | `/` | `%` ) atom)*
                //   expr = mul  (( `+` | `-` ) mul)*
                // Does NOT call parseExpression() because that would consume
                // `>` as a comparison and break the turbofish close.
                std::function<ExprPtr()> parseConstAtom;
                std::function<ExprPtr()> parseConstMul;
                std::function<ExprPtr()> parseConstExpr;
                parseConstAtom = [&]() -> ExprPtr {
                    SourceLocation aloc = peek().location;
                    if (check(TokenKind::Minus)) {
                        advance();
                        auto inner = parseConstAtom();
                        if (inner && inner->kind == ExprKind::IntLiteral) {
                            auto* il = inner->as<IntLiteralExpr>();
                            il->value = -il->value;
                            return inner;
                        }
                        if (inner && inner->kind == ExprKind::FloatLiteral) {
                            auto* fl = inner->as<FloatLiteralExpr>();
                            fl->value = -fl->value;
                            return inner;
                        }
                        // Generic unary-neg: wrap 0 - inner.
                        auto zero = std::make_unique<IntLiteralExpr>();
                        zero->location = aloc;
                        zero->value = 0;
                        return ast::makeBinaryOp(aloc, BinaryOp::Sub,
                            std::move(zero), std::move(inner));
                    }
                    if (check(TokenKind::IntLiteral)) {
                        auto t = advance();
                        auto lit = std::make_unique<IntLiteralExpr>();
                        lit->location = aloc;
                        lit->value = std::stoll(std::string(t.text));
                        return lit;
                    }
                    if (check(TokenKind::FloatLiteral)) {
                        auto t = advance();
                        auto lit = std::make_unique<FloatLiteralExpr>();
                        lit->location = aloc;
                        lit->value = std::stod(std::string(t.text));
                        return lit;
                    }
                    if (check(TokenKind::Identifier)) {
                        auto t = advance();
                        auto id = std::make_unique<IdentifierExpr>();
                        id->location = aloc;
                        id->name = std::string(t.text);
                        return id;
                    }
                    return nullptr;
                };
                parseConstMul = [&]() -> ExprPtr {
                    auto lhs = parseConstAtom();
                    if (!lhs) return nullptr;
                    while (check(TokenKind::Star) || check(TokenKind::Slash) ||
                           check(TokenKind::Percent)) {
                        SourceLocation opLoc = peek().location;
                        BinaryOp op = check(TokenKind::Star) ? BinaryOp::Mul
                                    : check(TokenKind::Slash) ? BinaryOp::Div
                                    : BinaryOp::Mod;
                        advance();
                        auto rhs = parseConstAtom();
                        if (!rhs) return lhs;
                        lhs = ast::makeBinaryOp(opLoc, op, std::move(lhs), std::move(rhs));
                    }
                    return lhs;
                };
                parseConstExpr = [&]() -> ExprPtr {
                    auto lhs = parseConstMul();
                    if (!lhs) return nullptr;
                    while (check(TokenKind::Plus) || check(TokenKind::Minus)) {
                        SourceLocation opLoc = peek().location;
                        BinaryOp op = check(TokenKind::Plus) ? BinaryOp::Add : BinaryOp::Sub;
                        advance();
                        auto rhs = parseConstMul();
                        if (!rhs) return lhs;
                        lhs = ast::makeBinaryOp(opLoc, op, std::move(lhs), std::move(rhs));
                    }
                    return lhs;
                };

                auto parseOneTfArg = [&]() {
                    // Lookahead to decide type-arg vs const-expr arg. A slot
                    // that starts with an IntLit / FloatLit / `-` is const.
                    // A slot that starts with an Identifier followed by a
                    // binary op (`-`, `+`, `*`, `/`, `%`) is ALSO const —
                    // that's the `<T, N-1>` case. Otherwise treat as type.
                    bool startsWithLit = check(TokenKind::IntLiteral) ||
                                         check(TokenKind::FloatLiteral);
                    bool startsWithNegLit = check(TokenKind::Minus) &&
                        current_ + 1 < tokens_.size() &&
                        (tokens_[current_ + 1].is(TokenKind::IntLiteral) ||
                         tokens_[current_ + 1].is(TokenKind::FloatLiteral));
                    bool identFollowedByArith = check(TokenKind::Identifier) &&
                        current_ + 1 < tokens_.size() &&
                        (tokens_[current_ + 1].is(TokenKind::Plus) ||
                         tokens_[current_ + 1].is(TokenKind::Minus) ||
                         tokens_[current_ + 1].is(TokenKind::Star) ||
                         tokens_[current_ + 1].is(TokenKind::Slash) ||
                         tokens_[current_ + 1].is(TokenKind::Percent));
                    bool wantsConstExpr = startsWithLit || startsWithNegLit || identFollowedByArith;

                    if (wantsConstExpr) {
                        e->callTypeArgs.push_back(nullptr);
                        if (e->callArgExprs.size() < e->callTypeArgs.size() - 1)
                            e->callArgExprs.resize(e->callTypeArgs.size() - 1);
                        auto expr = parseConstExpr();
                        if (!expr) {
                            diag_.error(peek().location,
                                "expected const expression in turbofish slot");
                            e->callArgExprs.push_back(nullptr);
                        } else {
                            e->callArgExprs.push_back(std::move(expr));
                        }
                    } else {
                        // A bare identifier here may be either (a) a real type
                        // or (b) a const generic param reference (e.g. `inner::<N>()`
                        // inside a generic `outer<const N>`). Capture both:
                        // NamedType in callTypeArgs, IdentifierExpr in callArgExprs.
                        bool couldBeConstIdent = check(TokenKind::Identifier) &&
                            current_ + 1 < tokens_.size() &&
                            (tokens_[current_ + 1].is(TokenKind::Comma) ||
                             tokens_[current_ + 1].is(TokenKind::Greater) ||
                             tokens_[current_ + 1].is(TokenKind::GreaterGreater));
                        SourceLocation identLoc;
                        std::string identName;
                        if (couldBeConstIdent) {
                            identLoc = peek().location;
                            identName = std::string(peek().text);
                        }
                        e->callTypeArgs.push_back(parseType());
                        if (couldBeConstIdent) {
                            auto id = std::make_unique<IdentifierExpr>();
                            id->location = identLoc;
                            id->name = identName;
                            if (e->callArgExprs.size() < e->callTypeArgs.size() - 1)
                                e->callArgExprs.resize(e->callTypeArgs.size() - 1);
                            e->callArgExprs.push_back(std::move(id));
                        } else if (!e->callArgExprs.empty()) {
                            e->callArgExprs.push_back(nullptr);
                        }
                    }
                };
                parseOneTfArg();
                while (match(TokenKind::Comma)) parseOneTfArg();
                if (!matchGenericClose())
                    expect(TokenKind::Greater, "'>'");

                // Turbofish struct init: Type::<A, B> { field: value }
                if (check(TokenKind::LBrace)) {
                    size_t savedPos = current_;
                    advance();
                    bool isStructInit = false;
                    if (check(TokenKind::RBrace)) {
                        isStructInit = true;
                    } else if (check(TokenKind::Identifier)) {
                        size_t savedIdent = current_;
                        advance();
                        if (check(TokenKind::Colon)) isStructInit = true;
                        current_ = savedIdent;
                    }
                    current_ = savedPos;

                    if (isStructInit) {
                        std::string mangledName = std::string(tok.text) + "<";
                        for (size_t ti = 0; ti < e->callTypeArgs.size(); ++ti) {
                            if (ti > 0) mangledName += ",";
                            if (e->callTypeArgs[ti]) mangledName += e->callTypeArgs[ti]->name;
                        }
                        mangledName += ">";

                        advance(); // consume {
                        auto si = std::make_unique<StructInitExpr>();
                        si->location = loc;
                        si->structName = mangledName;
                        if (!check(TokenKind::RBrace)) {
                            do {
                                // Allow trailing comma before `}`.
                                if (check(TokenKind::RBrace)) break;
                                if (check(TokenKind::DotDot)) {
                                    advance();
                                    si->spreadBase = parseExpression();
                                    break;
                                }
                                auto fieldName = expect(TokenKind::Identifier, "field name");
                                expect(TokenKind::Colon, "':'");
                                auto fieldVal = parseExpression();
                                si->fieldInits.emplace_back(std::string(fieldName.text), std::move(fieldVal));
                            } while (match(TokenKind::Comma));
                        }
                        expect(TokenKind::RBrace, "'}'");
                        return si;
                    }
                }

                return e;
            }
            std::string qualName = std::string(tok.text);
            auto memberTok = expect(TokenKind::Identifier, "qualified name");
            qualName += "::" + std::string(memberTok.text);
            return ast::makeIdentifier(loc, std::move(qualName));
        }

        // Empty struct init: Name {}
        if (check(TokenKind::LBrace) && current_ + 1 < tokens_.size() &&
            tokens_[current_ + 1].is(TokenKind::RBrace)) {
            advance();
            advance();
            auto e = std::make_unique<StructInitExpr>();
            e->location = loc;
            e->structName = std::string(tok.text);
            return e;
        }

        // Struct init: Name { field: val }
        if (check(TokenKind::LBrace) && peekNext().kind == TokenKind::Identifier) {
            size_t saved = current_;
            advance();
            if (check(TokenKind::Identifier)) {
                auto saved2 = current_;
                advance();
                if (check(TokenKind::Colon)) {
                    current_ = saved;
                    advance();
                    auto e = std::make_unique<StructInitExpr>();
                    e->location = loc;
                    e->structName = std::string(tok.text);

                    do {
                        // Allow trailing comma before `}`.
                        if (check(TokenKind::RBrace)) break;
                        if (check(TokenKind::DotDot)) {
                            advance();
                            e->spreadBase = parsePrimary();
                            break;
                        }
                        auto fieldName = expect(TokenKind::Identifier, "field name");
                        expect(TokenKind::Colon, "':'");
                        auto val = parseExpression();
                        e->fieldInits.emplace_back(std::string(fieldName.text), std::move(val));
                    } while (match(TokenKind::Comma));

                    expect(TokenKind::RBrace, "'}'");
                    return e;
                }
                current_ = saved2;
            }
            current_ = saved;
        }

        return ast::makeIdentifier(loc, std::string(tok.text));
    }

    // fail expression
    if (match(TokenKind::KW_fail)) {
        auto e = std::make_unique<FailExpr>();
        e->location = loc;
        auto typeName = expect(TokenKind::Identifier, "error type");
        e->typeName = std::string(typeName.text);
        if (match(TokenKind::Dot)) {
            auto variant = expect(TokenKind::Identifier, "error variant");
            e->variant = std::string(variant.text);
            if (match(TokenKind::Dot)) {
                auto withTok = expect(TokenKind::Identifier, "'with'");
                if (withTok.text == "with") {
                    expect(TokenKind::LParen, "'('");
                    e->message = parseExpression();
                    expect(TokenKind::RParen, "')'");
                }
            } else if (check(TokenKind::LParen)) {
                advance();
                e->message = parseExpression();
                e->isPayload = true;
                expect(TokenKind::RParen, "')'");
            }
        }
        return e;
    }

    // Closure: [captures](params) => expr_or_block
    if (check(TokenKind::LBracket) && !check(TokenKind::IntLiteral)) {
        size_t saved = current_;
        advance();

        bool isClosure = false;
        if (check(TokenKind::Identifier) || check(TokenKind::Amp) || check(TokenKind::RBracket)) {
            size_t innerSaved = current_;
            int depth = 1;
            while (!isAtEnd() && depth > 0) {
                if (check(TokenKind::LBracket)) ++depth;
                if (check(TokenKind::RBracket)) --depth;
                if (depth > 0) advance();
            }
            if (check(TokenKind::RBracket)) {
                advance();
                if (check(TokenKind::LParen)) {
                    isClosure = true;
                }
            }
            current_ = innerSaved;
        }

        current_ = saved;

        if (isClosure) {
            auto e = std::make_unique<ClosureExpr>();
            e->location = loc;

            expect(TokenKind::LBracket, "'['");
            if (!check(TokenKind::RBracket)) {
                do {
                    CaptureItem cap;
                    if (match(TokenKind::Amp)) {
                        cap.byRef = true;
                        auto name = expect(TokenKind::Identifier, "capture name");
                        cap.name = std::string(name.text);
                    } else {
                        auto name = expect(TokenKind::Identifier, "capture name");
                        cap.name = std::string(name.text);
                        if (match(TokenKind::Equal)) {
                            cap.moveExpr = parseExpression();
                            cap.move = true;
                        }
                    }
                    e->captures.push_back(std::move(cap));
                } while (match(TokenKind::Comma));
            }
            expect(TokenKind::RBracket, "']'");

            expect(TokenKind::LParen, "'('");
            if (!check(TokenKind::RParen)) {
                do {
                    ClosureParam cp;
                    auto pname = expect(TokenKind::Identifier, "parameter name");
                    cp.name = std::string(pname.text);
                    if (match(TokenKind::Colon)) {
                        cp.type = parseType();
                    }
                    e->params.push_back(std::move(cp));
                } while (match(TokenKind::Comma));
            }
            expect(TokenKind::RParen, "')'");

            expect(TokenKind::FatArrow, "'=>'");

            if (check(TokenKind::LBrace)) {
                e->body = parseBlock();
            } else {
                e->singleExpr = parseExpression();
            }

            return e;
        }
    }

    // Parenthesized expression or tuple
    if (match(TokenKind::LParen)) {
        auto first = parseExpression();
        if (match(TokenKind::Comma)) {
            auto e = std::make_unique<TupleInitExpr>();
            e->location = loc;
            e->elements.push_back(std::move(first));
            e->elements.push_back(parseExpression());
            while (match(TokenKind::Comma)) {
                e->elements.push_back(parseExpression());
            }
            expect(TokenKind::RParen, "')'");
            return e;
        }
        expect(TokenKind::RParen, "')'");
        return first;
    }

    // Array literal
    if (match(TokenKind::LBracket)) {
        auto e = std::make_unique<ArrayInitExpr>();
        e->location = loc;
        if (!check(TokenKind::RBracket)) {
            bool isSpread = match(TokenKind::Ellipsis);
            e->elements.push_back(parseExpression());
            e->elementIsSpread.push_back(isSpread);
            if (match(TokenKind::Semicolon)) {
                e->repeatCount = parseExpression();
            } else {
                while (match(TokenKind::Comma)) {
                    if (check(TokenKind::RBracket)) break;
                    isSpread = match(TokenKind::Ellipsis);
                    e->elements.push_back(parseExpression());
                    e->elementIsSpread.push_back(isSpread);
                }
            }
        }
        expect(TokenKind::RBracket, "']'");
        return e;
    }

    // Type keyword in value position: accept as a bare identifier whose
    // name is the keyword's spelling. Lets `typeinfo(i64)` /
    // `typeinfo(string)` / `sizeof::<f64>` reference primitive types by
    // keyword; CodeGen / Sema then treat the name like any user-defined
    // type. Only fires when the next token is a type keyword (not an
    // arbitrary keyword like `if`), so it doesn't mask the real "expected
    // expression" errors.
    if (peek().isTypeName()) {
        auto tok = advance();
        auto ident = std::make_unique<IdentifierExpr>();
        ident->location = loc;
        ident->name = std::string(tok.text);
        return ident;
    }

    diag_.error(loc, "expected expression, got {}", tokenKindToString(peek().kind));
    advance();
    return ast::makeIntLiteral(loc, 0);
}

ExprPtr Parser::parseStringInterpolation(const Token& tok) {
    const std::string& raw = tok.stringValue;

    bool hasInterp = false;
    for (size_t i = 0; i + 1 < raw.size(); ++i) {
        if (raw[i] == '$' && raw[i + 1] == '{') {
            hasInterp = true;
            break;
        }
    }

    if (!hasInterp) {
        return ast::makeStringLiteral(tok.location, raw);
    }

    auto e = std::make_unique<StringInterpExpr>();
    e->location = tok.location;

    size_t i = 0;
    while (i < raw.size()) {
        if (i + 1 < raw.size() && raw[i] == '$' && raw[i + 1] == '{') {
            i += 2;
            std::string exprStr;
            int depth = 1;
            while (i < raw.size() && depth > 0) {
                if (raw[i] == '{') ++depth;
                else if (raw[i] == '}') { if (--depth == 0) { ++i; break; } }
                exprStr += raw[i++];
            }

            Lexer interpLexer(exprStr, tok.location.filename, diag_);
            auto interpTokens = interpLexer.tokenizeAll();
            Parser interpParser(std::move(interpTokens), diag_);

            InterpPart part;
            part.isExpr = true;
            part.expr = interpParser.parseExpression();
            e->parts.push_back(std::move(part));
        } else {
            std::string text;
            while (i < raw.size()) {
                if (i + 1 < raw.size() && raw[i] == '$' && raw[i + 1] == '{') break;
                text += raw[i++];
            }
            InterpPart part;
            part.isExpr = false;
            part.text = std::move(text);
            e->parts.push_back(std::move(part));
        }
    }

    return e;
}
} // namespace vyx
