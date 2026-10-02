#include "Parser.h"
#include <functional>

namespace vyx {

// ============================================================
//  Statements
// ============================================================

StmtPtr Parser::parseStatement() {
    if (check(TokenKind::KW_let))    return parseVarDecl(true);
    if (check(TokenKind::KW_var))    return parseVarDecl(false);
    if (check(TokenKind::KW_const_kw)) {
        auto s = parseVarDecl(true);
        if (s) s->as<VarDeclStmt>()->isComptime = true;
        return s;
    }
    if (check(TokenKind::KW_if))     return parseIfStatement();
    if (check(TokenKind::KW_while))  return parseWhileStatement();
    if (check(TokenKind::KW_for))    return parseForStatement();
    if (check(TokenKind::KW_foreach)){
        diag_.warning(peek().location, "'foreach' is deprecated; use 'for (x in collection)' instead");
        return parseForEachStatement();
    }
    if (check(TokenKind::KW_defer)) {
        auto loc = advance().location;
        auto s = std::make_unique<DeferStmt>();
        s->location = loc;
        // Accept either `defer { ... }` (block) or `defer stmt;` (single statement)
        if (check(TokenKind::LBrace))
            s->body = parseBlock();
        else
            s->body = parseStatement();
        return s;
    }
    if (check(TokenKind::KW_unsafe)) {
        auto loc = advance().location;
        auto s = std::make_unique<UnsafeStmt>();
        s->location = loc;
        s->body = parseBlock();
        return s;
    }
    if (check(TokenKind::At) && current_ + 3 < tokens_.size() &&
        tokens_[current_ + 1].is(TokenKind::LBracket) &&
        tokens_[current_ + 2].is(TokenKind::KW_unsafe) &&
        tokens_[current_ + 3].is(TokenKind::RBracket)) {
        auto loc = peek().location;
        advance(); advance(); advance(); advance();
        auto s = std::make_unique<UnsafeStmt>();
        s->location = loc;
        s->body = parseBlock();
        return s;
    }
    if (check(TokenKind::KW_static_assert)) {
        auto loc = advance().location;
        expect(TokenKind::LParen, "'('");
        auto s = std::make_unique<StaticAssertStmt>();
        s->location = loc;
        s->expr = parseExpression();
        if (match(TokenKind::Comma)) {
            if (check(TokenKind::StringLiteral)) {
                s->message = std::string(advance().stringValue);
            }
        }
        expect(TokenKind::RParen, "')'");
        match(TokenKind::Semicolon);
        return s;
    }
    if (check(TokenKind::KW_return)) return parseReturnStatement();
    if (check(TokenKind::KW_yield)) {
        auto loc = advance().location;
        if (match(TokenKind::KW_return)) {
            auto s = std::make_unique<ReturnStmt>();
            s->location = loc;
            if (!check(TokenKind::Semicolon)) {
                s->expr = parseExpression();
            }
            expect(TokenKind::Semicolon, "';'");
            return s;
        }
        auto e = std::make_unique<AwaitExpr>();
        e->location = loc;
        if (!check(TokenKind::Semicolon)) {
            e->inner = parseExpression();
        }
        expect(TokenKind::Semicolon, "';'");
        return ast::makeExprStmt(loc, std::move(e));
    }
    if (check(TokenKind::KW_match))  return parseMatchStatement();

    if (check(TokenKind::KW_fn)) {
        auto fnDecl = parseFunctionDecl(false, false);
        if (fnDecl) {
            auto s = std::make_unique<ExprStmt>();
            s->location = fnDecl->location;
            s->localDecl = std::move(fnDecl);
            return s;
        }
    }
    if (check(TokenKind::KW_struct)) {
        auto decl = parseStructDecl(false);
        if (decl) {
            auto s = std::make_unique<ExprStmt>();
            s->location = decl->location;
            s->localDecl = std::move(decl);
            return s;
        }
    }
    if (check(TokenKind::KW_class)) {
        auto decl = parseClassDecl(false);
        if (decl) {
            auto s = std::make_unique<ExprStmt>();
            s->location = decl->location;
            s->localDecl = std::move(decl);
            return s;
        }
    }

    if (check(TokenKind::LBrace))    return parseBlock();

    if (check(TokenKind::KW_break)) {
        auto loc = advance().location;
        expect(TokenKind::Semicolon, "';'");
        auto s = std::make_unique<BreakStmt>();
        s->location = loc;
        return s;
    }
    if (check(TokenKind::KW_continue)) {
        auto loc = advance().location;
        expect(TokenKind::Semicolon, "';'");
        auto s = std::make_unique<ContinueStmt>();
        s->location = loc;
        return s;
    }

    auto loc = peek().location;
    auto expr = parseExpression();

    if (match(TokenKind::Equal)) {
        auto value = parseExpression();
        expect(TokenKind::Semicolon, "';'");
        auto s = std::make_unique<AssignStmt>();
        s->location = loc;
        s->target = std::move(expr);
        s->value = std::move(value);
        return s;
    }

    CompoundOp cop{};
    bool isCompound = false;
    if (check(TokenKind::PlusEqual))              { cop = CompoundOp::AddEq; isCompound = true; }
    else if (check(TokenKind::MinusEqual))        { cop = CompoundOp::SubEq; isCompound = true; }
    else if (check(TokenKind::StarEqual))         { cop = CompoundOp::MulEq; isCompound = true; }
    else if (check(TokenKind::SlashEqual))        { cop = CompoundOp::DivEq; isCompound = true; }
    else if (check(TokenKind::PercentEqual))      { cop = CompoundOp::ModEq; isCompound = true; }
    else if (check(TokenKind::LessLessEqual))     { cop = CompoundOp::ShlEq; isCompound = true; }
    else if (check(TokenKind::GreaterGreaterEqual)) { cop = CompoundOp::ShrEq; isCompound = true; }
    else if (check(TokenKind::AmpEqual))          { cop = CompoundOp::BandEq; isCompound = true; }
    else if (check(TokenKind::PipeEqual))         { cop = CompoundOp::BorEq; isCompound = true; }
    else if (check(TokenKind::CaretEqual))        { cop = CompoundOp::BxorEq; isCompound = true; }

    if (isCompound) {
        advance();
        auto value = parseExpression();
        expect(TokenKind::Semicolon, "';'");
        auto ce = std::make_unique<CompoundAssignmentExpr>();
        ce->location = loc;
        ce->op = cop;
        ce->target = std::move(expr);
        ce->value = std::move(value);
        return ast::makeExprStmt(loc, std::move(ce));
    }

    if (!check(TokenKind::RBrace)) {
        expect(TokenKind::Semicolon, "';'");
    }
    return ast::makeExprStmt(loc, std::move(expr));
}

StmtPtr Parser::parseBlock() {
    auto loc = peek().location;
    expect(TokenKind::LBrace, "'{'");
    std::vector<StmtPtr> stmts;
    while (!check(TokenKind::RBrace) && !isAtEnd()) {
        size_t before = current_;
        auto stmt = parseStatement();
        if (stmt) {
            stmts.push_back(std::move(stmt));
        } else if (current_ == before && !isAtEnd()) {
            synchronizeStatement();
        }
    }
    expect(TokenKind::RBrace, "'}'");
    return ast::makeBlock(loc, std::move(stmts));
}

StmtPtr Parser::parseVarDecl(bool isConst) {
    auto loc = peek().location;
    advance();

    if (match(TokenKind::LBrace)) {
        std::vector<std::string> names;
        names.push_back(std::string(expect(TokenKind::Identifier, "field name").text));
        while (match(TokenKind::Comma)) {
            if (check(TokenKind::RBrace)) break;
            names.push_back(std::string(expect(TokenKind::Identifier, "field name").text));
        }
        expect(TokenKind::RBrace, "'}'");
        expect(TokenKind::Equal, "'='");
        auto init = parseExpression();
        expect(TokenKind::Semicolon, "';'");

        auto block = std::make_unique<BlockStmt>();
        block->location = loc;

        block->statements.push_back(
            ast::makeVarDecl(loc, isConst, "__destructure_tmp", nullptr, std::move(init)));

        for (auto& name : names) {
            auto access = ast::makeMemberAccess(loc,
                ast::makeIdentifier(loc, "__destructure_tmp"), name);
            block->statements.push_back(
                ast::makeVarDecl(loc, isConst, name, nullptr, std::move(access)));
        }
        return block;
    }

    if (match(TokenKind::LParen)) {
        std::vector<std::string> names;
        names.push_back(std::string(expect(TokenKind::Identifier, "variable name").text));
        while (match(TokenKind::Comma)) {
            names.push_back(std::string(expect(TokenKind::Identifier, "variable name").text));
        }
        expect(TokenKind::RParen, "')'");
        expect(TokenKind::Equal, "'='");
        auto init = parseExpression();
        expect(TokenKind::Semicolon, "';'");

        auto vd = std::make_unique<VarDeclStmt>();
        vd->location = loc;
        vd->isConst = isConst;
        vd->tupleBindings = std::move(names);
        vd->initExpr = std::move(init);
        return vd;
    }

    auto name = expect(TokenKind::Identifier, "variable name");

    TypePtr type = nullptr;
    if (match(TokenKind::Colon)) {
        type = parseType();
    }

    ExprPtr init = nullptr;
    if (match(TokenKind::Equal)) {
        init = parseExpression();
    }

    if (match(TokenKind::KW_else)) {
        auto elseBlock = parseBlock();
        auto varDecl = ast::makeVarDecl(loc, isConst, std::string(name.text), std::move(type), std::move(init));
        varDecl->elseBranch = std::move(elseBlock);
        return varDecl;
    }

    expect(TokenKind::Semicolon, "';'");
    return ast::makeVarDecl(loc, isConst, std::string(name.text), std::move(type), std::move(init));
}

StmtPtr Parser::parseIfStatement() {
    auto loc = peek().location;
    expect(TokenKind::KW_if, "'if'");

    auto s = std::make_unique<IfStmt>();
    s->location = loc;

    expect(TokenKind::LParen, "'('");
    if (check(TokenKind::KW_let)) {
        advance();
        auto bindName = expect(TokenKind::Identifier, "binding name");
        expect(TokenKind::Equal, "'='");
        auto initExpr = parseExpression();
        expect(TokenKind::RParen, "')'");

        auto outerBlock = std::make_unique<BlockStmt>();
        outerBlock->location = loc;
        outerBlock->statements.push_back(
            ast::makeVarDecl(loc, true, "__iflet_tmp", nullptr, std::move(initExpr)));

        auto someCheck = ast::makeIdentifier(loc, "__typecheck_Some");
        auto condExpr = ast::makeBinaryOp(loc, BinaryOp::MatchOp,
            ast::makeIdentifier(loc, "__iflet_tmp"), std::move(someCheck));

        auto innerIf = std::make_unique<IfStmt>();
        innerIf->location = loc;
        innerIf->condition = std::move(condExpr);

        auto thenBlock = std::make_unique<BlockStmt>();
        thenBlock->location = loc;
        auto unwrapCall = ast::makeCall(loc,
            ast::makeMemberAccess(loc, ast::makeIdentifier(loc, "__iflet_tmp"), "unwrap"), {});
        thenBlock->statements.push_back(
            ast::makeVarDecl(loc, true, std::string(bindName.text), nullptr, std::move(unwrapCall)));
        auto innerBody = parseBlock();
        thenBlock->statements.push_back(std::move(innerBody));
        innerIf->thenBranch = std::move(thenBlock);

        if (match(TokenKind::KW_else)) {
            innerIf->elseBranch = parseBlock();
        }

        outerBlock->statements.push_back(std::move(innerIf));
        s->condition = ast::makeBoolLiteral(loc, true);
        s->thenBranch = std::move(outerBlock);
        return s;
    }
    s->condition = parseExpression();
    expect(TokenKind::RParen, "')'");
    s->thenBranch = parseBlock();

    while (match(TokenKind::KW_elif)) {
        expect(TokenKind::LParen, "'('");
        auto elifCond = parseExpression();
        expect(TokenKind::RParen, "')'");
        auto elifBody = parseBlock();
        s->elifBranches.emplace_back(std::move(elifCond), std::move(elifBody));
    }

    if (match(TokenKind::KW_else)) {
        // `else if (...)` is sugar for `elif (...)` — accept both forms.
        if (check(TokenKind::KW_if)) {
            advance(); // consume `if`
            expect(TokenKind::LParen, "'('");
            auto condExpr = parseExpression();
            expect(TokenKind::RParen, "')'");
            auto body = parseBlock();
            s->elifBranches.emplace_back(std::move(condExpr), std::move(body));
            // Chain further `else if` / `elif` / `else` branches.
            while (true) {
                if (match(TokenKind::KW_elif)) {
                    expect(TokenKind::LParen, "'('");
                    auto c = parseExpression();
                    expect(TokenKind::RParen, "')'");
                    auto b = parseBlock();
                    s->elifBranches.emplace_back(std::move(c), std::move(b));
                    continue;
                }
                if (match(TokenKind::KW_else)) {
                    if (check(TokenKind::KW_if)) {
                        advance();
                        expect(TokenKind::LParen, "'('");
                        auto c = parseExpression();
                        expect(TokenKind::RParen, "')'");
                        auto b = parseBlock();
                        s->elifBranches.emplace_back(std::move(c), std::move(b));
                        continue;
                    }
                    s->elseBranch = parseBlock();
                }
                break;
            }
        } else {
            s->elseBranch = parseBlock();
        }
    }
    return s;
}

StmtPtr Parser::parseWhileStatement() {
    auto loc = peek().location;
    expect(TokenKind::KW_while, "'while'");
    auto s = std::make_unique<WhileStmt>();
    s->location = loc;
    expect(TokenKind::LParen, "'('");
    s->condition = parseExpression();
    expect(TokenKind::RParen, "')'");
    s->body = parseBlock();
    return s;
}

StmtPtr Parser::parseForStatement() {
    auto loc = peek().location;
    expect(TokenKind::KW_for, "'for'");

    // for x in collection { }
    if (check(TokenKind::Identifier) && current_ + 1 < tokens_.size() &&
        tokens_[current_ + 1].kind == TokenKind::KW_in) {
        auto s = std::make_unique<ForEachStmt>();
        s->location = loc;
        auto varTok = advance();
        s->varName = std::string(varTok.text);
        advance();
        s->collection = parseExpression();
        s->body = parseBlock();
        return s;
    }

    // for ([key, value] in expr) { }
    if (check(TokenKind::LParen) && current_ + 1 < tokens_.size() &&
        tokens_[current_ + 1].kind == TokenKind::LBracket) {
        advance(); // (
        advance(); // [
        auto s = std::make_unique<ForEachStmt>();
        s->location = loc;
        while (!check(TokenKind::RBracket) && !isAtEnd()) {
            auto tok = expect(TokenKind::Identifier, "destructure variable");
            s->destructure.push_back(std::string(tok.text));
            if (check(TokenKind::Comma)) advance();
        }
        expect(TokenKind::RBracket, "']'");
        if (!s->destructure.empty())
            s->varName = s->destructure[0];
        expect(TokenKind::KW_in, "'in'");
        s->collection = parseExpression();
        expect(TokenKind::RParen, "')'");
        s->body = parseBlock();
        return s;
    }

    // for (x in collection) { }
    if (check(TokenKind::LParen) && current_ + 1 < tokens_.size() &&
        tokens_[current_ + 1].kind == TokenKind::Identifier &&
        current_ + 2 < tokens_.size() &&
        tokens_[current_ + 2].kind == TokenKind::KW_in) {
        advance();
        auto s = std::make_unique<ForEachStmt>();
        s->location = loc;
        auto varTok = advance();
        s->varName = std::string(varTok.text);
        advance();
        s->collection = parseExpression();
        expect(TokenKind::RParen, "')'");
        s->body = parseBlock();
        return s;
    }

    // C-style for
    expect(TokenKind::LParen, "'('");
    auto s = std::make_unique<ForStmt>();
    s->location = loc;

    if (check(TokenKind::KW_let) || check(TokenKind::KW_var)) {
        s->init = parseVarDecl(check(TokenKind::KW_let));
    } else if (!check(TokenKind::Semicolon)) {
        auto initExpr = parseExpression();
        expect(TokenKind::Semicolon, "';'");
        s->init = ast::makeExprStmt(loc, std::move(initExpr));
    } else {
        expect(TokenKind::Semicolon, "';'");
    }

    if (!check(TokenKind::Semicolon)) {
        s->condition = parseExpression();
    }
    expect(TokenKind::Semicolon, "';'");

    if (!check(TokenKind::RParen)) {
        auto stepExpr = parseExpression();
        CompoundOp cop{};
        bool isCompound = false;
        if (check(TokenKind::PlusEqual))         { cop = CompoundOp::AddEq; isCompound = true; }
        else if (check(TokenKind::MinusEqual))   { cop = CompoundOp::SubEq; isCompound = true; }
        else if (check(TokenKind::StarEqual))    { cop = CompoundOp::MulEq; isCompound = true; }
        else if (check(TokenKind::SlashEqual))   { cop = CompoundOp::DivEq; isCompound = true; }

        if (isCompound) {
            advance();
            auto value = parseExpression();
            auto ce = std::make_unique<CompoundAssignmentExpr>();
            ce->location = stepExpr->location;
            ce->op = cop;
            ce->target = std::move(stepExpr);
            ce->value = std::move(value);
            s->step = std::move(ce);
        } else if (check(TokenKind::Equal)) {
            advance();
            auto value = parseExpression();
            auto ae = std::make_unique<AssignmentExpr>();
            ae->location = stepExpr->location;
            ae->lhs = std::move(stepExpr);
            ae->rhs = std::move(value);
            s->step = std::move(ae);
        } else {
            s->step = std::move(stepExpr);
        }
    }

    expect(TokenKind::RParen, "')'");
    s->body = parseBlock();
    return s;
}

StmtPtr Parser::parseForEachStatement() {
    auto loc = peek().location;
    expect(TokenKind::KW_foreach, "'foreach'");
    auto s = std::make_unique<ForEachStmt>();
    s->location = loc;
    expect(TokenKind::LParen, "'('");
    auto varName = expect(TokenKind::Identifier, "iterator variable");
    s->varName = std::string(varName.text);
    expect(TokenKind::KW_in, "'in'");
    s->collection = parseExpression();
    expect(TokenKind::RParen, "')'");
    s->body = parseBlock();
    return s;
}

StmtPtr Parser::parseReturnStatement() {
    auto loc = peek().location;
    expect(TokenKind::KW_return, "'return'");
    ExprPtr expr = nullptr;
    if (!check(TokenKind::Semicolon)) {
        expr = parseExpression();
    }
    expect(TokenKind::Semicolon, "';'");
    return ast::makeReturn(loc, std::move(expr));
}

StmtPtr Parser::parseMatchStatement() {
    auto loc = peek().location;
    expect(TokenKind::KW_match, "'match'");

    auto s = std::make_unique<MatchStmt>();
    s->location = loc;

    expect(TokenKind::LParen, "'('");
    s->expr = parseExpression();
    expect(TokenKind::RParen, "')'");

    expect(TokenKind::LBrace, "'{'");
    while (!check(TokenKind::RBrace) && !isAtEnd()) {
        MatchArm arm;
        arm.location = peek().location;

        if (match(TokenKind::KW_case)) {
            if (check(TokenKind::LParen)) {
                advance();
                // Each inner element of a `case (...)` pattern is:
                //   - a nested tuple `(...)`  → isTuple=true
                //   - a literal like `0`      → isLiteral=true, literalExpr set
                //   - an identifier binding   → name set
                std::function<NestedPattern(Parser&)> parsePat;
                parsePat = [&parsePat](Parser& self) -> NestedPattern {
                    NestedPattern pat;
                    if (self.check(TokenKind::LParen)) {
                        self.advance();
                        pat.isTuple = true;
                        while (!self.check(TokenKind::RParen) && !self.isAtEnd()) {
                            pat.children.push_back(parsePat(self));
                            if (!self.check(TokenKind::RParen)) self.match(TokenKind::Comma);
                        }
                        self.expect(TokenKind::RParen, "')'");
                    } else if (self.check(TokenKind::IntLiteral) ||
                               self.check(TokenKind::StringLiteral) ||
                               self.check(TokenKind::BoolLiteral) ||
                               self.check(TokenKind::CharLiteral) ||
                               self.check(TokenKind::FloatLiteral)) {
                        pat.isLiteral = true;
                        // Stash the literal text in `name` as a copy-safe
                        // fallback (copy ctor resets literalExpr to null).
                        if (self.check(TokenKind::IntLiteral)) {
                            pat.name = std::string(self.peek().text);
                        }
                        pat.literalExpr = self.parsePrimary();
                    } else {
                        auto name = self.expect(TokenKind::Identifier, "binding name");
                        pat.name = std::string(name.text);
                    }
                    return pat;
                };
                while (!check(TokenKind::RParen) && !isAtEnd()) {
                    NestedPattern pat = parsePat(*this);
                    // tupleBindings carries identifier names for downstream
                    // Sema/CodeGen binding; literal slots push an empty name
                    // placeholder so indices stay aligned with nestedPatterns.
                    if (!pat.isTuple && !pat.name.empty() && !pat.isLiteral)
                        arm.tupleBindings.push_back(pat.name);
                    else
                        arm.tupleBindings.push_back("");
                    arm.nestedPatterns.push_back(std::move(pat));
                    if (!check(TokenKind::RParen)) match(TokenKind::Comma);
                }
                expect(TokenKind::RParen, "')'");
                arm.label = "__tuple";
            } else if (check(TokenKind::IntLiteral) || check(TokenKind::FloatLiteral) ||
                check(TokenKind::StringLiteral) || check(TokenKind::BoolLiteral) ||
                check(TokenKind::CharLiteral)) {
                arm.valuePattern = parsePrimary();
                if (arm.valuePattern && arm.valuePattern->kind == ExprKind::StringLiteral)
                    arm.label = arm.valuePattern->as<StringLiteralExpr>()->value;
                else if (arm.valuePattern && arm.valuePattern->kind == ExprKind::IntLiteral)
                    arm.label = std::to_string(arm.valuePattern->as<IntLiteralExpr>()->value);
                else
                    arm.label = "";
            } else {
                std::string labelStr;
                if (check(TokenKind::KW_fail)) {
                    labelStr = "fail";
                    advance();
                } else {
                    auto label = expect(TokenKind::Identifier, "case label");
                    labelStr = std::string(label.text);
                }
                arm.label = labelStr;

                if (match(TokenKind::LParen)) {
                    std::function<NestedPattern()> parseNestedBind;
                    parseNestedBind = [&]() -> NestedPattern {
                        NestedPattern pat;
                        auto name = expect(TokenKind::Identifier, "binding or variant name");
                        pat.name = std::string(name.text);
                        if (match(TokenKind::LParen)) {
                            pat.isTuple = true;
                            while (!check(TokenKind::RParen) && !isAtEnd()) {
                                pat.children.push_back(parseNestedBind());
                                if (!check(TokenKind::RParen)) match(TokenKind::Comma);
                            }
                            expect(TokenKind::RParen, "')'");
                        }
                        return pat;
                    };

                    auto firstPat = parseNestedBind();
                    if (firstPat.isTuple || check(TokenKind::Comma)) {
                        arm.nestedPatterns.push_back(firstPat);
                        if (!firstPat.isTuple)
                            arm.tupleBindings.push_back(firstPat.name);
                        while (match(TokenKind::Comma)) {
                            auto nextPat = parseNestedBind();
                            arm.nestedPatterns.push_back(nextPat);
                            if (!nextPat.isTuple)
                                arm.tupleBindings.push_back(nextPat.name);
                        }
                    } else {
                        arm.bindingName = firstPat.name;
                    }
                    expect(TokenKind::RParen, "')'");
                } else if (match(TokenKind::Colon)) {
                    arm.typePattern = parseType();
                }
            }

            if (match(TokenKind::KW_if)) {
                arm.guardExpr = parseExpression();
            }
            expect(TokenKind::FatArrow, "'=>'");
            arm.body = check(TokenKind::LBrace) ? parseBlock() : parseStatement();
        } else if (match(TokenKind::KW_default) || match(TokenKind::KW_else)) {
            arm.isDefault = true;
            expect(TokenKind::FatArrow, "'=>'");
            arm.body = check(TokenKind::LBrace) ? parseBlock() : parseStatement();
        } else {
            advance();
        }

        s->arms.push_back(std::move(arm));
    }
    expect(TokenKind::RBrace, "'}'");

    return s;
}

} // namespace vyx
