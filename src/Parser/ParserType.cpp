#include "Parser.h"

namespace vyx {

// ============================================================
//  Fields, Methods, Parameters
// ============================================================

FieldDecl Parser::parseFieldDecl() {
    FieldDecl field;
    field.location = peek().location;
    field.visibility = parseVisibility();

    // `static let NAME: T = …` form (struct-level constant). Consume the
    // modifiers so the field name check doesn't trip on them. The `static`
    // distinction is not yet preserved downstream — struct-level constants
    // are accepted syntactically and emitted as regular fields.
    match(TokenKind::KW_static);
    if (check(TokenKind::KW_let) || check(TokenKind::KW_const_kw)) advance();

    if (check(TokenKind::Identifier) && peek().text == "weak") {
        advance();
        field.isWeak = true;
    }

    std::vector<std::string> names;
    auto firstName = expectIdentOrSoftKeyword("field name");
    names.push_back(std::string(firstName.text));

    while (match(TokenKind::Comma)) {
        auto next = expectIdentOrSoftKeyword("field name");
        names.push_back(std::string(next.text));
    }

    expect(TokenKind::Colon, "':'");
    auto type = parseType();

    ExprPtr defaultVal = nullptr;
    if (match(TokenKind::Equal)) {
        defaultVal = parseExpression();
    }
    match(TokenKind::Semicolon);

    field.name = names[0];
    field.type = std::move(type);
    field.defaultValue = std::move(defaultVal);
    if (names.size() > 1) {
        field.extraNames.assign(names.begin() + 1, names.end());
    }
    return field;
}

MethodDecl Parser::parseMethodDecl() {
    MethodDecl method;
    method.location = peek().location;

    method.isStatic = match(TokenKind::KW_static);
    method.isAsync = match(TokenKind::KW_async);
    expect(TokenKind::KW_fn, "'fn'");

    method.name = parseOperatorDeclName("method name");

    if (match(TokenKind::Less)) {
        do {
            auto tp = expect(TokenKind::Identifier, "type parameter");
            method.genericParams.push_back(std::string(tp.text));
        } while (match(TokenKind::Comma));
        expect(TokenKind::Greater, "'>'");
    }

    expect(TokenKind::LParen, "'('");
    method.params = parseParameterList();
    expect(TokenKind::RParen, "')'");

    if (match(TokenKind::Arrow)) {
        method.returnType = parseType();
    }

    if (match(TokenKind::KW_override)) {
        method.isOverride = true;
    }

    if (match(TokenKind::KW_where)) {
        do {
            auto tpName = expect(TokenKind::Identifier, "type parameter");
            std::string tp = std::string(tpName.text);
            if (match(TokenKind::EqualEqual) && (check(TokenKind::Identifier) || peek().isTypeName())) {
                auto typeTok = advance();
                method.typeEqualityConstraints[tp] = std::string(typeTok.text);
            } else if (match(TokenKind::BangEqual) && (check(TokenKind::Identifier) || peek().isTypeName())) {
                auto typeTok = advance();
                method.typeInequalityConstraints[tp] = std::string(typeTok.text);
            } else if (match(TokenKind::Colon)) {
                std::vector<std::string> traits;
                do {
                    auto c = expect(TokenKind::Identifier, "constraint");
                    traits.push_back(std::string(c.text));
                } while (match(TokenKind::Plus));
                auto& existing = method.genericConstraints[tp];
                existing.insert(existing.end(), traits.begin(), traits.end());
            }
        } while (match(TokenKind::Comma));
    }

    if (check(TokenKind::LBrace)) {
        method.body = parseBlock();
    } else {
        expect(TokenKind::Semicolon, "';' or '{'");
    }

    return method;
}

ParamDecl Parser::parseParameter() {
    ParamDecl param;

    if (match(TokenKind::Ellipsis)) {
        auto name = expectIdentOrSoftKeyword("parameter name");
        param.name = "..." + std::string(name.text);
        expect(TokenKind::Colon, "':'");
        param.type = parseType();
        return param;
    }

    // Explicit `self` receiver: accepted inside method/interface parameter
    // lists so users can write `fn foo(self) -> ...` Rust-style. The `self`
    // keyword has no explicit type — Sema binds it to the enclosing class
    // type via `currentClassType_`. `&self` / `&mut self` forms are also
    // accepted; they desugar to an unannotated `self` param with the
    // appropriate mutRef flag.
    if (check(TokenKind::KW_self)) {
        advance();
        param.name = "self";
        return param;
    }
    if (check(TokenKind::Amp) &&
        current_ + 1 < tokens_.size() &&
        (tokens_[current_ + 1].is(TokenKind::KW_self) ||
         (tokens_[current_ + 1].is(TokenKind::KW_mut) &&
          current_ + 2 < tokens_.size() &&
          tokens_[current_ + 2].is(TokenKind::KW_self)))) {
        advance(); // &
        param.isMutRef = match(TokenKind::KW_mut);
        expect(TokenKind::KW_self, "'self'");
        param.name = "self";
        return param;
    }

    auto name = expectIdentOrSoftKeyword("parameter name");
    param.name = std::string(name.text);
    expect(TokenKind::Colon, "':'");

    // Sugar form: `name: ...T` (named variadic with homogeneous element
    // type). Rewrite to equivalent `...name: T` and remember T so the
    // enclosing parseFunctionDecl can mark the generic param variadic.
    if (check(TokenKind::Ellipsis)) {
        advance();
        param.name = "..." + param.name;
        param.type = parseType();
        if (param.type) {
            pendingVariadicElementType_ = param.type->name;
        }
        return param;
    }

    if (match(TokenKind::Amp)) {
        param.isMutRef = match(TokenKind::KW_mut);
        param.type = parseType();
        auto ref = std::make_unique<ReferenceType>();
        ref->isMutable = param.isMutRef;
        ref->innerType = std::move(param.type);
        ref->location = name.location;
        param.type = std::move(ref);
    } else {
        param.type = parseType();
    }

    if (match(TokenKind::Equal)) {
        param.defaultValue = parseExpression();
    }

    return param;
}

std::vector<ParamDecl> Parser::parseParameterList() {
    std::vector<ParamDecl> params;
    if (check(TokenKind::RParen)) return params;

    params.push_back(parseParameter());
    while (match(TokenKind::Comma)) {
        params.push_back(parseParameter());
    }
    return params;
}

// ============================================================
//  Type Parsing
// ============================================================

TypePtr Parser::parseType() {
    auto first = parseBaseType();
    if (match(TokenKind::Question)) {
        // `T?` sugar → GenericType("Option", [T]).  The parser runs before
        // Sema so it can't consult the lang-item registry; "Option" here is
        // a fixed convention anchor that stdlib is required to expose as
        // `@[lang_item("option")] enum Option<T>`.  Sema's type resolution
        // then treats this GenericType like any other user-written
        // `Option<T>` annotation — registry-driven downstream.
        auto opt = std::make_unique<GenericType>();
        opt->location = first->location;
        opt->name = "Option";
        opt->typeArgs.push_back(std::move(first));
        return opt;
    }
    if (!check(TokenKind::Pipe)) return first;

    auto loc = first->location;
    auto unionType = std::make_unique<UnionType>();
    unionType->location = loc;
    unionType->members.push_back(std::move(first));
    while (match(TokenKind::Pipe)) {
        unionType->members.push_back(parseBaseType());
    }
    return unionType;
}

TypePtr Parser::parseBaseType() {
    auto loc = peek().location;

    if (match(TokenKind::Star)) {
        auto t = std::make_unique<PointerType>();
        t->location = loc;
        t->innerType = parseType();
        return t;
    }

    if (match(TokenKind::Amp)) {
        auto t = std::make_unique<ReferenceType>();
        t->location = loc;
        t->isMutable = match(TokenKind::KW_mut);
        t->innerType = parseType();
        return t;
    }

    if (match(TokenKind::LBracket)) {
        auto t = std::make_unique<ArrayType>();
        t->location = loc;
        // Three accepted shapes:
        //   1. `[N; T]` — leading literal/identifier size followed by `;`
        //      then element type. Legacy form used by `[8; i32]` and the
        //      Rust-style `[N; T]` (size first when explicit `;` follows).
        //   2. `[N]T`   — size in brackets, element type follows. Used by
        //      const-generic samples (`[N]T`, `[Cap]T`) where `N`/`Cap` is
        //      a non-type template parameter that must keep its identifier
        //      shape until Mono substitutes the bound integer.
        //   3. `[T]` / `[T; N]` — legacy element-first form (dynamic slice
        //      or fixed-size with trailing `;` size).
        // Disambiguation: try the size-first paths whenever the head token
        // is a literal or an Identifier *not* followed by an angle bracket
        // / namespace separator (those would belong to a generic-type
        // element). Otherwise fall through to element-first parsing.
        bool sizeFirst = false;
        if (check(TokenKind::IntLiteral)) {
            sizeFirst = true;
        } else if (check(TokenKind::Identifier) && current_ + 1 < tokens_.size()) {
            const Token& nx = tokens_[current_ + 1];
            // Identifier followed by `;` (legacy `N; T`) or `]` (const-generic
            // `N]T`) is unambiguously a size, not a NamedType element.
            if (nx.is(TokenKind::Semicolon) || nx.is(TokenKind::RBracket))
                sizeFirst = true;
        }
        if (sizeFirst) {
            t->size = parseExpression();
            if (match(TokenKind::Semicolon)) {
                t->elementType = parseType();
                expect(TokenKind::RBracket, "']'");
            } else {
                expect(TokenKind::RBracket, "']'");
                t->elementType = parseType();
            }
        } else {
            t->elementType = parseType();
            if (match(TokenKind::Semicolon)) {
                t->size = parseExpression();
            }
            expect(TokenKind::RBracket, "']'");
        }
        return t;
    }

    if (check(TokenKind::Identifier) && peek().text == "dyn") {
        advance();
        return parseBaseType();
    }

    if (check(TokenKind::KW_fn)) {
        advance();
        auto t = std::make_unique<FunctionType>();
        t->location = loc;
        expect(TokenKind::LParen, "'('");
        if (!check(TokenKind::RParen)) {
            t->paramTypes.push_back(parseType());
            while (match(TokenKind::Comma)) {
                t->paramTypes.push_back(parseType());
            }
        }
        expect(TokenKind::RParen, "')'");
        if (match(TokenKind::Arrow)) {
            t->returnType = parseType();
        }
        return t;
    }

    if (match(TokenKind::LParen)) {
        auto t = std::make_unique<TupleType>();
        t->location = loc;
        t->elements.push_back(parseType());
        while (match(TokenKind::Comma)) {
            t->elements.push_back(parseType());
        }
        expect(TokenKind::RParen, "')'");
        return t;
    }

    if (peek().isTypeName() || check(TokenKind::Identifier)) {
        auto tok = advance();
        std::string name = std::string(tok.text);

        // C#-style fully-qualified namespace access: `Foo.Bar.Point`.
        // Accumulate dotted segments into the NamedType string so downstream
        // Sema (which registers block-form `module Foo.Bar { class Point }`
        // as a symbol keyed on `"Foo.Bar.Point"`) can resolve the reference.
        // Stops as soon as the next token isn't `. Ident` — this way the
        // `.member` access of an identifier expression isn't swallowed here
        // (type-position parsing is only reachable from contexts that won't
        // confuse the two).
        while (check(TokenKind::Dot) && current_ + 1 < tokens_.size() &&
               (tokens_[current_ + 1].kind == TokenKind::Identifier ||
                tokens_[current_ + 1].isTypeName())) {
            advance(); // consume '.'
            auto segTok = advance();
            name += ".";
            name += std::string(segTok.text);
        }

        // P2-generics C5: dependent associated type `T::Item` / `T::Output`.
        // Recognised when an identifier is followed by `::Ident` and NOT by
        // `::<` (turbofish) — the latter is a generic instantiation and is
        // handled by the existing branch below.
        if (check(TokenKind::ColonColon) && current_ + 1 < tokens_.size() &&
            tokens_[current_ + 1].kind == TokenKind::Identifier) {
            advance(); // consume '::'
            auto memberTok = expect(TokenKind::Identifier, "associated type name");
            auto dt = std::make_unique<DependentType>();
            dt->location = loc;
            dt->baseName = name;
            dt->memberName = std::string(memberTok.text);
            dt->name = name + "::" + dt->memberName;
            return dt;
        }

        bool hasTurbofish = false;
        if (check(TokenKind::ColonColon) && current_ + 1 < tokens_.size() &&
            tokens_[current_ + 1].kind == TokenKind::Less) {
            advance();
            hasTurbofish = true;
        }
        if (hasTurbofish || match(TokenKind::Less)) {
            if (hasTurbofish) advance();
            auto t = std::make_unique<GenericType>();
            t->location = loc;
            t->name = std::move(name);
            // Non-type template-argument detection: when an angle-bracket slot
            // begins with an integer / negation / parenthesised expression we
            // route through `parseExpression` and park the result in `argExprs`
            // (typeArgs slot stays nullptr to preserve positional alignment).
            // Type slots flow through `parseType` as before.
            auto parseOneArg = [&]() {
                bool wantsConstExpr =
                    check(TokenKind::IntLiteral) ||
                    check(TokenKind::FloatLiteral) ||
                    check(TokenKind::StringLiteral) ||
                    check(TokenKind::CharLiteral) ||
                    check(TokenKind::KW_true) ||
                    check(TokenKind::KW_false) ||
                    check(TokenKind::Minus);
                if (wantsConstExpr) {
                    t->typeArgs.push_back(nullptr);
                    if (t->argExprs.size() < t->typeArgs.size() - 1)
                        t->argExprs.resize(t->typeArgs.size() - 1);
                    t->argExprs.push_back(parseExpression());
                } else {
                    t->typeArgs.push_back(parseType());
                    if (!t->argExprs.empty())
                        t->argExprs.push_back(nullptr);
                }
            };
            parseOneArg();
            while (match(TokenKind::Comma)) {
                parseOneArg();
            }
            if (check(TokenKind::GreaterGreater)) {
                tokens_[current_].kind = TokenKind::Greater;
                auto extra = tokens_[current_];
                extra.kind = TokenKind::Greater;
                tokens_.insert(tokens_.begin() + current_ + 1, extra);
            }
            expect(TokenKind::Greater, "'>'");
            return t;
        }

        // P2 C6: variadic type-pack indexing in type position — `Ts[N]`.
        // We only consume the `[ IntLiteral ]` suffix when the bracket
        // immediately follows an identifier/type-name. The `[T; N]` /
        // `[T]` shapes are handled earlier (when `[` is the first token
        // of a base type) so they never reach this branch.
        if (check(TokenKind::LBracket) && current_ + 1 < tokens_.size() &&
            tokens_[current_ + 1].kind == TokenKind::IntLiteral) {
            advance(); // '['
            auto idxExpr = parseExpression();
            expect(TokenKind::RBracket, "']'");
            auto pi = std::make_unique<PackIndexType>();
            pi->location = loc;
            pi->name = name;
            pi->packName = std::move(name);
            pi->indexExpr = std::move(idxExpr);
            return pi;
        }

        return ast::makeNamedType(loc, std::move(name));
    }

    diag_.error(loc, "expected type, got {}", tokenKindToString(peek().kind));
    return ast::makeNamedType(loc, "error");
}

} // namespace vyx

