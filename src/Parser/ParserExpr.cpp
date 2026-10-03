#include "Parser.h"

#include <cstdlib>

namespace vyx {

ExprPtr Parser::parseExpression() {
    return parseAssignment();
}

ExprPtr Parser::parseAssignment() {
    auto expr = parseBinaryExpr(0);

    if (match(TokenKind::Question)) {
        if (check(TokenKind::Semicolon) || check(TokenKind::RParen) ||
            check(TokenKind::RBrace) || check(TokenKind::Comma) || check(TokenKind::Eof)) {
            auto e = std::make_unique<TryExpr>();
            e->location = expr->location;
            e->inner = std::move(expr);
            return e;
        }
        auto trueExpr = parseExpression();
        expect(TokenKind::Colon, "':'");
        auto falseExpr = parseExpression();
        auto e = std::make_unique<TernaryExpr>();
        e->location = expr->location;
        e->condition = std::move(expr);
        e->trueExpr = std::move(trueExpr);
        e->falseExpr = std::move(falseExpr);
        return e;
    }

    return expr;
}

int Parser::getPrecedence(TokenKind kind) const {
    switch (kind) {
        case TokenKind::PipePipe:       return 1;
        case TokenKind::AmpAmp:         return 2;
        case TokenKind::Pipe:           return 3;
        case TokenKind::Caret:          return 4;
        case TokenKind::Amp:            return 5;
        case TokenKind::EqualEqual:
        case TokenKind::BangEqual:      return 6;
        case TokenKind::Less:
        case TokenKind::LessEqual:
        case TokenKind::Greater:
        case TokenKind::GreaterEqual:   return 7;
        case TokenKind::MatchOp:        return 7;
        case TokenKind::DotDot:          return 0;
        case TokenKind::DotDotEqual:     return 0;
        case TokenKind::PipeArrow:      return 0;
        case TokenKind::QuestionQuestion: return 1;
        case TokenKind::LessLess:
        case TokenKind::GreaterGreater: return 8;
        case TokenKind::Plus:
        case TokenKind::Minus:          return 9;
        case TokenKind::Star:
        case TokenKind::Slash:
        case TokenKind::Percent:        return 10;
        default: return -1;
    }
}

BinaryOp Parser::tokenToBinaryOp(TokenKind kind) const {
    switch (kind) {
        case TokenKind::Plus:           return BinaryOp::Add;
        case TokenKind::Minus:          return BinaryOp::Sub;
        case TokenKind::Star:           return BinaryOp::Mul;
        case TokenKind::Slash:          return BinaryOp::Div;
        case TokenKind::Percent:        return BinaryOp::Mod;
        case TokenKind::EqualEqual:     return BinaryOp::Eq;
        case TokenKind::BangEqual:      return BinaryOp::Neq;
        case TokenKind::Less:           return BinaryOp::Lt;
        case TokenKind::LessEqual:      return BinaryOp::Lte;
        case TokenKind::Greater:        return BinaryOp::Gt;
        case TokenKind::GreaterEqual:   return BinaryOp::Gte;
        case TokenKind::AmpAmp:         return BinaryOp::And;
        case TokenKind::PipePipe:       return BinaryOp::Or;
        case TokenKind::Amp:            return BinaryOp::BitAnd;
        case TokenKind::Pipe:           return BinaryOp::BitOr;
        case TokenKind::Caret:          return BinaryOp::BitXor;
        case TokenKind::LessLess:       return BinaryOp::Shl;
        case TokenKind::GreaterGreater: return BinaryOp::Shr;
        case TokenKind::MatchOp:        return BinaryOp::MatchOp;
        case TokenKind::DotDot:         return BinaryOp::RangeOp;
        case TokenKind::DotDotEqual:   return BinaryOp::RangeOp;
        case TokenKind::PipeArrow:     return BinaryOp::Pipe;
        case TokenKind::QuestionQuestion: return BinaryOp::NullCoalesce;
        default:
            diag_.error(tokens_[current_].location,
                "unexpected token in binary expression ({})", static_cast<int>(kind));
            std::abort();
    }
}

ExprPtr Parser::parseBinaryExpr(int minPrecedence) {
    auto left = parseUnary();

    // Type guard: `expr is Type.Variant`
    if (check(TokenKind::KW_is)) {
        auto isLoc = peek().location;
        advance();
        auto typeName = expect(TokenKind::Identifier, "type name");
        std::string fullName = std::string(typeName.text);
        if (match(TokenKind::Dot)) {
            auto variant = expect(TokenKind::Identifier, "variant name");
            fullName += "." + std::string(variant.text);
        }
        auto rhs = ast::makeIdentifier(isLoc, "__typecheck_" + fullName);
        left = ast::makeBinaryOp(isLoc, BinaryOp::MatchOp, std::move(left), std::move(rhs));
    }

    while (true) {
        int prec = getPrecedence(peek().kind);
        if (prec < minPrecedence) break;

        auto loc = peek().location;
        auto opKind = peek().kind;
        advance();

        auto right = parseBinaryExpr(prec + 1);

        if (opKind == TokenKind::DotDotEqual) {
            right = ast::makeBinaryOp(loc, BinaryOp::Add, std::move(right), ast::makeIntLiteral(loc, 1));
        }

        auto op = tokenToBinaryOp(opKind);
        auto binOp = ast::makeBinaryOp(loc, op, std::move(left), std::move(right));

        // Chain comparison: `a < b < c` → `a < b && b < c`
        if ((op == BinaryOp::Lt || op == BinaryOp::Gt || op == BinaryOp::Lte || op == BinaryOp::Gte) &&
            (check(TokenKind::Less) || check(TokenKind::Greater) ||
             check(TokenKind::LessEqual) || check(TokenKind::GreaterEqual))) {
            auto chainLoc = peek().location;
            auto chainOpKind = peek().kind;
            advance();
            auto chainRight = parseBinaryExpr(prec + 1);
            // NOTE: assumes middle operand is an identifier for copy
            std::string middleName;
            if (binOp->rhs && binOp->rhs->kind == ExprKind::Identifier) {
                middleName = binOp->rhs->as<IdentifierExpr>()->name;
            }
            if (!middleName.empty()) {
                auto middleCopy = ast::makeIdentifier(binOp->rhs->location, middleName);
                auto chainCmp = ast::makeBinaryOp(chainLoc, tokenToBinaryOp(chainOpKind),
                    std::move(middleCopy), std::move(chainRight));
                left = ast::makeBinaryOp(loc, BinaryOp::And, std::move(binOp), std::move(chainCmp));
            } else {
                left = std::move(binOp);
            }
        } else {
            left = std::move(binOp);
        }
    }

    return left;
}

ExprPtr Parser::parseUnary() {
    auto loc = peek().location;

    if (match(TokenKind::KW_await)) {
        auto inner = parseUnary();
        auto e = std::make_unique<AwaitExpr>();
        e->location = loc;
        e->inner = std::move(inner);
        return e;
    }

    if (match(TokenKind::Minus))     return ast::makeUnaryOp(loc, UnaryOp::Neg, parseUnary());
    if (match(TokenKind::Bang))      return ast::makeUnaryOp(loc, UnaryOp::Not, parseUnary());
    if (match(TokenKind::Tilde))     return ast::makeUnaryOp(loc, UnaryOp::BitNot, parseUnary());
    if (match(TokenKind::PlusPlus))  return ast::makeUnaryOp(loc, UnaryOp::PreInc, parseUnary());
    if (match(TokenKind::MinusMinus))return ast::makeUnaryOp(loc, UnaryOp::PreDec, parseUnary());

    if (match(TokenKind::Amp)) {
        if (match(TokenKind::KW_mut))
            return ast::makeUnaryOp(loc, UnaryOp::MutRef, parseUnary());
        return ast::makeUnaryOp(loc, UnaryOp::Ref, parseUnary());
    }

    if (match(TokenKind::Star)) return ast::makeUnaryOp(loc, UnaryOp::Deref, parseUnary());

    auto expr = parsePrimary();
    return parsePostfix(std::move(expr));
}

ExprPtr Parser::parsePostfix(ExprPtr expr) {
    while (true) {
        auto loc = peek().location;

        // Turbofish `::<...>` after a MemberAccess: `std.collections.Vec::<T>`
        // Primary-identifier `Vec::<T>` is handled in parsePrimary; this
        // postfix form covers the namespace-chained case, where the leading
        // expression is already a MemberAccessExpr by the time we get here.
        if (expr->kind == ExprKind::MemberAccess &&
            check(TokenKind::ColonColon) && current_ + 1 < tokens_.size() &&
            tokens_[current_ + 1].is(TokenKind::Less)) {
            advance(); // consume `::`
            advance(); // consume `<`
            std::vector<TypePtr> typeArgs;
            typeArgs.push_back(parseType());
            while (match(TokenKind::Comma)) typeArgs.push_back(parseType());
            if (!matchGenericClose())
                expect(TokenKind::Greater, "'>'");
            auto* memberExpr = expr->as<MemberAccessExpr>();
            for (auto& ta : typeArgs)
                if (ta) memberExpr->callTypeArgs.push_back(std::move(ta));
            continue;
        }

        // Generic expression: ident<T1,T2>( or ident<T1,T2>. or ident<T1,T2>{
        if (check(TokenKind::Less) && expr->kind == ExprKind::Identifier) {
            size_t saved = current_;
            advance();
            int depth = 1;
            enum class GenericFollow { None, Call, MemberOrInit } follow = GenericFollow::None;
            while (!isAtEnd() && depth > 0) {
                if (check(TokenKind::Less)) depth++;
                else if (check(TokenKind::Greater)) { depth--; if (depth == 0) break; }
                else if (check(TokenKind::Semicolon) || check(TokenKind::LBrace)) break;
                advance();
            }
            if (depth == 0 && check(TokenKind::Greater)) {
                advance();
                if (check(TokenKind::LParen)) follow = GenericFollow::Call;
                else if (check(TokenKind::Dot) || check(TokenKind::LBrace)) follow = GenericFollow::MemberOrInit;
            }
            current_ = saved;

            if (follow != GenericFollow::None) {
                advance();
                std::vector<TypePtr> typeArgs;
                typeArgs.push_back(parseType());
                while (match(TokenKind::Comma)) typeArgs.push_back(parseType());
                if (!matchGenericClose())
                    expect(TokenKind::Greater, "'>'");

                auto* identExpr = expr->as<IdentifierExpr>();

                if (follow == GenericFollow::Call) {
                    if (!typeArgs.empty()) identExpr->typeAnnotation = std::move(typeArgs[0]);
                    for (auto& ta : typeArgs)
                        if (ta) identExpr->callTypeArgs.push_back(std::move(ta));
                } else {
                    std::string baseName = identExpr->name;
                    std::string mangledName = baseName + "<";
                    for (size_t i = 0; i < typeArgs.size(); ++i) {
                        if (i > 0) mangledName += ",";
                        if (typeArgs[i]) mangledName += typeArgs[i]->name;
                    }
                    mangledName += ">";
                    identExpr->name = mangledName;

                    auto ann = std::make_unique<GenericType>();
                    ann->location = loc;
                    ann->name = baseName;
                    for (auto& ta : typeArgs) {
                        if (ta) ann->typeArgs.push_back(std::move(ta));
                    }
                    identExpr->typeAnnotation = std::move(ann);

                    // Generic struct init: Pair<i32,string> { field: val }
                    if (check(TokenKind::LBrace)) {
                        size_t savedStruct = current_;
                        advance();
                        bool isStructInit = false;
                        if (check(TokenKind::RBrace)) {
                            isStructInit = true;
                        } else if (check(TokenKind::Identifier)) {
                            auto savedIdent = current_;
                            advance();
                            if (check(TokenKind::Colon)) isStructInit = true;
                            current_ = savedIdent;
                        }
                        current_ = savedStruct;

                        if (isStructInit) {
                            advance();
                            auto e = std::make_unique<StructInitExpr>();
                            e->location = loc;
                            e->structName = mangledName;
                            if (!check(TokenKind::RBrace)) {
                                do {
                                    auto fieldName = expect(TokenKind::Identifier, "field name");
                                    expect(TokenKind::Colon, "':'");
                                    auto val = parseExpression();
                                    e->fieldInits.emplace_back(std::string(fieldName.text), std::move(val));
                                } while (match(TokenKind::Comma));
                            }
                            expect(TokenKind::RBrace, "'}'");
                            expr = std::move(e);
                            continue;
                        }
                    }
                }
            }
        }

        // Function call: expr(args)
        if (match(TokenKind::LParen)) {
            std::vector<ExprPtr> args;
            std::vector<std::string> argNames;
            if (!check(TokenKind::RParen)) {
                auto parseArg = [&]() {
                    if (check(TokenKind::Identifier) && current_ + 1 < tokens_.size() &&
                        tokens_[current_ + 1].is(TokenKind::Colon) &&
                        (current_ + 2 >= tokens_.size() || !tokens_[current_ + 2].is(TokenKind::Colon))) {
                        auto nameTok = advance();
                        advance();
                        argNames.push_back(std::string(nameTok.text));
                    } else {
                        argNames.push_back("");
                    }
                    args.push_back(parseExpression());
                };
                parseArg();
                while (match(TokenKind::Comma)) {
                    parseArg();
                }
            }
            expect(TokenKind::RParen, "')'");
            auto callExpr = ast::makeCall(loc, std::move(expr), std::move(args));
            callExpr->argNames = std::move(argNames);

            if (check(TokenKind::LBrace) && !check(TokenKind::Semicolon)) {
                auto closureBody = parseBlock();
                auto closure = std::make_unique<ClosureExpr>();
                closure->location = loc;
                closure->body = std::move(closureBody);
                callExpr->args.push_back(std::move(closure));
            }

            expr = std::move(callExpr);
            continue;
        }

        // Member access: expr.member
        if (match(TokenKind::Dot)) {
            if (check(TokenKind::IntLiteral)) {
                auto idx = advance();
                expr = ast::makeMemberAccess(loc, std::move(expr), std::to_string(idx.intValue));
            } else {
                auto member = expect(TokenKind::Identifier, "member name");
                auto memberExpr = ast::makeMemberAccess(loc, std::move(expr), std::string(member.text));

                // Method-level generic args: `.method<U>(` or `.method::<U>(`
                // Consume an optional turbofish `::` so that `obj.method::<U>(args)`
                // is accepted in addition to the bare `obj.method<U>(args)` form.
                bool hasTurbofish = false;
                if (check(TokenKind::ColonColon) && current_ + 1 < tokens_.size() &&
                    tokens_[current_ + 1].is(TokenKind::Less)) {
                    hasTurbofish = true;
                    advance(); // consume `::`
                }
                if (check(TokenKind::Less)) {
                    size_t saved = current_;
                    advance();
                    int depth = 1;
                    bool isGenericCall = false;
                    while (!isAtEnd() && depth > 0) {
                        if (check(TokenKind::Less)) depth++;
                        else if (check(TokenKind::Greater)) { depth--; if (depth == 0) break; }
                        else if (check(TokenKind::Semicolon) || check(TokenKind::LBrace)) break;
                        advance();
                    }
                    if (depth == 0 && check(TokenKind::Greater)) {
                        advance();
                        if (check(TokenKind::LParen)) isGenericCall = true;
                    }
                    current_ = saved;
                    // If we consumed `::` above but lookahead says this isn't a
                    // generic call (e.g. `<` was actually a comparison), back up
                    // past the `::` we already consumed.
                    if (hasTurbofish && !isGenericCall) { --current_; }

                    if (isGenericCall) {
                        advance();
                        std::vector<TypePtr> typeArgs;
                        typeArgs.push_back(parseType());
                        while (match(TokenKind::Comma)) typeArgs.push_back(parseType());
                        if (!matchGenericClose())
                            expect(TokenKind::Greater, "'>'");
                        for (auto& ta : typeArgs)
                            if (ta) memberExpr->callTypeArgs.push_back(std::move(ta));
                    }
                } else if (hasTurbofish) {
                    // Consumed `::` but no `<` followed — back up.
                    --current_;
                }

                expr = std::move(memberExpr);
            }
            continue;
        }

        // Typed-pointer member access.  The AST intentionally uses the same
        // MemberAccessExpr node as `.`; sema decides whether the receiver is
        // a typed pointer/reference and applies the corresponding ABI.
        if (match(TokenKind::Arrow)) {
            auto member = expect(TokenKind::Identifier, "member name after '->'");
            expr = ast::makeMemberAccess(loc, std::move(expr), std::string(member.text));
            continue;
        }

        // Index: expr[index]
        if (match(TokenKind::LBracket)) {
            auto idx = parseExpression();
            expect(TokenKind::RBracket, "']'");
            auto e = std::make_unique<IndexExpr>();
            e->location = loc;
            e->object = std::move(expr);
            e->indexExpr = std::move(idx);
            expr = std::move(e);
            continue;
        }

        if (match(TokenKind::PlusPlus)) {
            expr = ast::makeUnaryOp(loc, UnaryOp::PostInc, std::move(expr));
            continue;
        }
        if (match(TokenKind::MinusMinus)) {
            expr = ast::makeUnaryOp(loc, UnaryOp::PostDec, std::move(expr));
            continue;
        }

        // P5-pack: pack fold expression `ident...op`
        // Recognised only when the current expr is a bare identifier AND
        // the next two tokens are `...` followed by a binary operator.
        if (expr->kind == ExprKind::Identifier && check(TokenKind::Ellipsis)) {
            // Peek two ahead to confirm a binary operator follows `...`.
            // (We don't consume yet so we can fall through cleanly.)
            size_t nextPos = current_ + 1; // position after `...`
            if (nextPos < tokens_.size()) {
                TokenKind nextKind = tokens_[nextPos].kind;
                BinaryOp foldOp = BinaryOp::Add;
                bool validOp = true;
                switch (nextKind) {
                    case TokenKind::Plus:    foldOp = BinaryOp::Add;    break;
                    case TokenKind::Minus:   foldOp = BinaryOp::Sub;    break;
                    case TokenKind::Star:    foldOp = BinaryOp::Mul;    break;
                    case TokenKind::Slash:   foldOp = BinaryOp::Div;    break;
                    case TokenKind::Percent: foldOp = BinaryOp::Mod;    break;
                    case TokenKind::AmpAmp:  foldOp = BinaryOp::And;    break;
                    case TokenKind::PipePipe:foldOp = BinaryOp::Or;     break;
                    case TokenKind::Amp:     foldOp = BinaryOp::BitAnd; break;
                    case TokenKind::Pipe:    foldOp = BinaryOp::BitOr;  break;
                    case TokenKind::Caret:   foldOp = BinaryOp::BitXor; break;
                    default: validOp = false; break;
                }
                if (validOp) {
                    advance(); // consume `...`
                    advance(); // consume the operator token
                    const auto& identName = expr->as<IdentifierExpr>()->name;
                    auto foldExpr = std::make_unique<PackFoldExpr>();
                    foldExpr->location = loc;
                    foldExpr->packName = identName;
                    foldExpr->op       = foldOp;
                    expr = std::move(foldExpr);
                    continue;
                }
            }
        }

        // Type cast: expr as Type
        if (match(TokenKind::KW_as)) {
            auto targetType = parseType();
            auto e = std::make_unique<CastExpr>();
            e->location = loc;
            e->operand = std::move(expr);
            e->targetType = std::move(targetType);
            expr = std::move(e);
            continue;
        }

        // Try postfix: `expr?` participating in a larger binary expression.
        //
        // Vyx already accepts the bare `expr?` form at parseAssignment top
        // level (followed by `;`, `)`, `}`, `,`, EOF). That layer cannot see
        // the `?` when it sits in the *middle* of a binary expression — e.g.
        // `parse_low(a)? + parse_low(b)?` — because parseBinaryExpr returns
        // the LHS to parseAssignment only after consuming the `+ b?` chain.
        // To make `?` a true postfix unary that left-binds tighter than any
        // binary operator (so it composes with `+`, `==`, `&&`, `??`, etc.)
        // we recognise it here whenever the token following `?` cannot start
        // an expression — i.e. it's a binary operator or a closer. Ternary
        // (`cond ? trueExpr : falseExpr`) is preserved because the trueExpr
        // start never matches either condition: it's an identifier / literal
        // / opening bracket, never a binop and never a closer, so we leave
        // the `?` untouched for parseAssignment to pick up.
        if (check(TokenKind::Question) && current_ + 1 < tokens_.size()) {
            TokenKind next = tokens_[current_ + 1].kind;
            bool isCloser = (next == TokenKind::Semicolon || next == TokenKind::RParen ||
                             next == TokenKind::RBrace   || next == TokenKind::Comma  ||
                             next == TokenKind::RBracket || next == TokenKind::Eof    ||
                             next == TokenKind::Colon);
            bool isBinOp  = (getPrecedence(next) >= 0);
            if (isCloser || isBinOp) {
                auto qLoc = peek().location;
                advance();
                auto e = std::make_unique<TryExpr>();
                e->location = qLoc;
                e->inner = std::move(expr);
                expr = std::move(e);
                continue;
            }
        }

        break;
    }
    return expr;
}

} // namespace vyx
