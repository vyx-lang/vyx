#include "Parser.h"
#include <cassert>
#include <functional>
#include <map>
#include <set>

namespace vyx {

// Render a TypeAnnotation into the same string form that VyxType::mangle()
// would produce — used by the parser to build full-specialization mangled
// names (`fn foo<i32>` → `foo<i32>`) without depending on Sema/Type.h.
//
// Coverage matches VyxType::mangle() for every annotation shape the parser
// can produce here: builtin primitive names, nested generics (`Vec<i32>`),
// pointers/references, tuples, function types and arrays. Keeps `name`
// verbatim for user-defined nominal types so `class Foo` stays `Foo` (the
// canonical mangler does the same). Falls back to `?` for null nodes —
// mirroring `mangleGeneric`'s null-arg sentinel.
static std::string typeAnnotationToMangle(const TypeAnnotation* ann) {
    if (!ann) return "?";
    switch (ann->kind) {
        case TypeAnnotationKind::Named:
            return ann->name;
        case TypeAnnotationKind::Generic: {
            auto* gt = ann->as<GenericType>();
            std::string r = ann->name + "<";
            for (size_t i = 0; i < gt->typeArgs.size(); ++i) {
                if (i > 0) r += ",";
                r += typeAnnotationToMangle(gt->typeArgs[i].get());
            }
            r += ">";
            return r;
        }
        case TypeAnnotationKind::Pointer: {
            auto* pt = ann->as<PointerType>();
            return "*" + typeAnnotationToMangle(pt->innerType.get());
        }
        case TypeAnnotationKind::Reference: {
            auto* rt = ann->as<ReferenceType>();
            return (rt->isMutable ? "&mut " : "&") +
                   typeAnnotationToMangle(rt->innerType.get());
        }
        case TypeAnnotationKind::Array: {
            auto* at = ann->as<ArrayType>();
            return "[" + typeAnnotationToMangle(at->elementType.get()) + "]";
        }
        case TypeAnnotationKind::Tuple: {
            auto* tt = ann->as<TupleType>();
            std::string r = "(";
            for (size_t i = 0; i < tt->elements.size(); ++i) {
                if (i > 0) r += ",";
                r += typeAnnotationToMangle(tt->elements[i].get());
            }
            r += ")";
            return r;
        }
        case TypeAnnotationKind::Function: {
            auto* ft = ann->as<FunctionType>();
            std::string r = "fn(";
            for (size_t i = 0; i < ft->paramTypes.size(); ++i) {
                if (i > 0) r += ",";
                r += typeAnnotationToMangle(ft->paramTypes[i].get());
            }
            r += ")->" + typeAnnotationToMangle(ft->returnType.get());
            return r;
        }
        case TypeAnnotationKind::Union: {
            auto* ut = ann->as<UnionType>();
            std::string r;
            for (size_t i = 0; i < ut->members.size(); ++i) {
                if (i > 0) r += "|";
                r += typeAnnotationToMangle(ut->members[i].get());
            }
            return r;
        }
        case TypeAnnotationKind::PackIndex: {
            // Variadic type-pack index annotations should never reach
            // here in a well-formed full-specialization name (Sema
            // expands them away during variadic instantiation), but
            // produce a defensive form so we don't drop the leading
            // '?' sentinel and accidentally collide with another
            // template's mangle.
            auto* pi = ann->as<PackIndexType>();
            return pi->packName + "[?]";
        }
    }
    return ann->name.empty() ? "?" : ann->name;
}

Parser::Parser(std::vector<Token> tokens, DiagnosticsEngine& diag)
    : tokens_(std::move(tokens)), diag_(diag) {}

// ============================================================
//  Token navigation
// ============================================================

const Token& Parser::peek() const { return tokens_[current_]; }
const Token& Parser::peekNext() const {
    if (current_ + 1 < tokens_.size()) return tokens_[current_ + 1];
    return tokens_.back();
}
const Token& Parser::previous() const { return tokens_[current_ - 1]; }

Token Parser::advance() {
    if (!isAtEnd()) ++current_;
    return tokens_[current_ - 1];
}

bool Parser::check(TokenKind kind) const { return peek().kind == kind; }

bool Parser::match(TokenKind kind) {
    if (check(kind)) { advance(); return true; }
    return false;
}

Token Parser::expect(TokenKind kind, std::string_view message) {
    if (check(kind)) { panicMode_ = false; return advance(); }
    if (!panicMode_) {
        diag_.error(peek().location, "expected {}, got {}", message, tokenKindToString(peek().kind));
        panicMode_ = true;
    }
    if (tryRecover(kind)) return advance();
    return peek();
}

Token Parser::expectIdentOrSoftKeyword(std::string_view message) {
    if (check(TokenKind::Identifier)) { panicMode_ = false; return advance(); }
    // Whitelist of keywords that may legitimately appear in
    // identifier-like positions (parameter names, field names, method
    // names, struct-init field names). These are context-dependent
    // reserved words, commonly used in C FFI bindings and domain APIs.
    switch (peek().kind) {
        case TokenKind::KW_type:
        case TokenKind::KW_module:
        case TokenKind::KW_error:
        case TokenKind::KW_override:
        // `string` is no longer a keyword — it lexes as Identifier, so no
        // case is needed here.  Keeping the comment so future readers know
        // why the list is shorter than the set of "types recognised as
        // soft-keywords" was historically.
        case TokenKind::KW_interface: {  // also covers `trait` / `protocol` aliases
            Token t = advance();
            // Normalize: expose it as Identifier-shaped to downstream code.
            t.kind = TokenKind::Identifier;
            panicMode_ = false;
            return t;
        }
        default: break;
    }
    if (!panicMode_) {
        diag_.error(peek().location, "expected {}, got {}", message, tokenKindToString(peek().kind));
        panicMode_ = true;
    }
    if (tryRecover(TokenKind::Identifier)) return advance();
    return peek();
}

std::string Parser::parseOperatorDeclName(std::string_view message) {
    auto name = expectIdentOrSoftKeyword(message);
    if (name.text != "operator") return std::string(name.text);

    if (match(TokenKind::LBracket)) {
        expect(TokenKind::RBracket, "']' after operator[");
        return "operator_index";
    }

    switch (peek().kind) {
        case TokenKind::Plus: case TokenKind::Minus:
        case TokenKind::Star: case TokenKind::Slash:
        case TokenKind::Percent: case TokenKind::Equal:
        case TokenKind::EqualEqual: case TokenKind::BangEqual:
        case TokenKind::Less: case TokenKind::Greater:
        case TokenKind::LessEqual: case TokenKind::GreaterEqual:
        case TokenKind::Amp: case TokenKind::Pipe:
        case TokenKind::Caret: case TokenKind::LessLess:
        case TokenKind::GreaterGreater: case TokenKind::PlusEqual:
        case TokenKind::MinusEqual: case TokenKind::StarEqual:
        case TokenKind::SlashEqual: case TokenKind::PercentEqual:
        case TokenKind::AmpEqual: case TokenKind::PipeEqual:
        case TokenKind::CaretEqual: case TokenKind::LessLessEqual:
        case TokenKind::GreaterGreaterEqual: case TokenKind::PlusPlus:
        case TokenKind::MinusMinus: case TokenKind::Bang:
        case TokenKind::Tilde:
            return "operator" + std::string(advance().text);
        default:
            return "operator";
    }
}

bool Parser::matchGenericClose() {
    if (check(TokenKind::Greater)) { advance(); return true; }
    if (check(TokenKind::GreaterGreater)) {
        tokens_[current_].kind = TokenKind::Greater;
        return true;
    }
    if (check(TokenKind::GreaterEqual)) {
        tokens_[current_].kind = TokenKind::Equal;
        return true;
    }
    if (check(TokenKind::GreaterGreaterEqual)) {
        tokens_[current_].kind = TokenKind::GreaterEqual;
        return true;
    }
    return false;
}

bool Parser::isAtEnd() const { return peek().kind == TokenKind::Eof; }

void Parser::synchronize() {
    panicMode_ = false;
    advance();
    while (!isAtEnd()) {
        if (previous().kind == TokenKind::Semicolon) return;
        if (previous().kind == TokenKind::RBrace) return;
        switch (peek().kind) {
            case TokenKind::KW_fn:
            case TokenKind::KW_let:
            case TokenKind::KW_var:
            case TokenKind::KW_struct:
            case TokenKind::KW_class:
            case TokenKind::KW_interface:
            case TokenKind::KW_macro:
            case TokenKind::KW_enum:
            case TokenKind::KW_error:
            case TokenKind::KW_import:
            case TokenKind::KW_export:
            case TokenKind::KW_return:
            case TokenKind::KW_for:
            case TokenKind::KW_while:
            case TokenKind::KW_foreach:
            case TokenKind::KW_match:
            case TokenKind::KW_if:
            case TokenKind::KW_public:
            case TokenKind::KW_module:
            case TokenKind::KW_use:
            case TokenKind::At:
                return;
            default:
                advance();
        }
    }
}

void Parser::synchronizeStatement() {
    panicMode_ = false;
    while (!isAtEnd()) {
        if (peek().kind == TokenKind::Semicolon) { advance(); return; }
        if (peek().kind == TokenKind::RBrace) return;
        switch (peek().kind) {
            case TokenKind::KW_let:
            case TokenKind::KW_var:
            case TokenKind::KW_return:
            case TokenKind::KW_if:
            case TokenKind::KW_while:
            case TokenKind::KW_for:
            case TokenKind::KW_foreach:
            case TokenKind::KW_match:
            case TokenKind::KW_defer:
            case TokenKind::KW_break:
            case TokenKind::KW_continue:
            case TokenKind::KW_unsafe:
                return;
            default:
                advance();
        }
    }
}

bool Parser::tryRecover(TokenKind expected) {
    size_t lookahead = current_;
    int maxLook = 3;
    while (lookahead < tokens_.size() && maxLook-- > 0) {
        if (tokens_[lookahead].kind == expected) {
            while (current_ < lookahead) advance();
            return true;
        }
        if (tokens_[lookahead].kind == TokenKind::Semicolon ||
            tokens_[lookahead].kind == TokenKind::RBrace ||
            tokens_[lookahead].kind == TokenKind::Eof)
            break;
        ++lookahead;
    }
    return false;
}

// ============================================================
//  Shared Helpers
// ============================================================

std::vector<std::pair<std::string, std::string>> Parser::parseAttributes() {
    std::vector<std::pair<std::string, std::string>> attrs;
    while (check(TokenKind::At)) {
        advance();
        expect(TokenKind::LBracket, "'['");
        Token attrName;
        if (check(TokenKind::Identifier) || check(TokenKind::KW_async) ||
            check(TokenKind::KW_static) || check(TokenKind::KW_unsafe) ||
            check(TokenKind::KW_comptime) || check(TokenKind::KW_task) ||
            check(TokenKind::KW_bench)) {
            attrName = advance();
        } else {
            attrName = expect(TokenKind::Identifier, "attribute name");
        }
        std::string attrValue;
        if (match(TokenKind::LParen)) {
            if (check(TokenKind::StringLiteral)) {
                attrValue = std::string(advance().stringValue);
            } else if (check(TokenKind::IntLiteral)) {
                attrValue = std::to_string(advance().intValue);
            } else if (check(TokenKind::Identifier)) {
                attrValue = std::string(advance().text);
                while (match(TokenKind::Comma)) {
                    if (check(TokenKind::Identifier))
                        attrValue += ", " + std::string(advance().text);
                }
            }
            expect(TokenKind::RParen, "')'");
        }
        expect(TokenKind::RBracket, "']'");
        attrs.emplace_back(std::string(attrName.text), std::move(attrValue));
    }
    return attrs;
}

Visibility Parser::parseVisibility() {
    if (match(TokenKind::KW_public)) return Visibility::Public;
    if (match(TokenKind::KW_private)) return Visibility::Private;
    if (match(TokenKind::KW_internal)) return Visibility::Internal;
    if (match(TokenKind::KW_protected)) return Visibility::Protected;
    return Visibility::Private;
}

void Parser::parseGenericParams(Decl& decl) {
    if (!match(TokenKind::Less)) return;
    if (match(TokenKind::Ellipsis)) decl.isVariadicGeneric = true;

    // ----------------------------------------------------------------
    // Peek-ahead phase: categorize the angle-bracket list as one of:
    //   (A) Pure generic-parameter list (`class Pair<A, B>`)
    //   (B) Full specialization (all args concrete: `class Pair<i32, i32>`)
    //   (C) Partial specialization (mix of fresh params and concrete/nested
    //       patterns: `class Pair<T, T>`, `class Pair<T, i32>`,
    //       `class Pair<Vec<T>, U>`).
    //
    // Rules:
    //   - If every arg is a single identifier with no following `<`/`::`/`*`
    //     and all those identifiers are *distinct*, it's (A).
    //   - If any arg is a concrete type (built-in keyword, `*`, `&`, `[`, `(`,
    //     `fn`, or identifier immediately followed by `<`/`::`) it is a
    //     specialization header.
    //     * If all args are concrete → (B) full specialization.
    //     * Otherwise → (C) partial specialization.
    //   - If any single-identifier arg appears *more than once* in the list
    //     (e.g. `<T, T>`) it is also a partial specialization.
    //
    // Lookahead is non-destructive.
    // ----------------------------------------------------------------
    size_t savedPos = current_;

    // Collect a lightweight summary of each top-level argument in the
    // angle-bracket list without fully parsing types yet.
    struct ArgKind { bool isBareIdent; std::string name; };
    std::vector<ArgKind> argKinds;
    bool anySpecToken = false; // any concrete-type signal seen

    {
        int depth = 1;
        bool firstInArg = true;
        ArgKind cur{true, ""};

        auto flushArg = [&]() {
            argKinds.push_back(cur);
            cur = {true, ""};
            firstInArg = true;
        };

        while (depth > 0 && !isAtEnd()) {
            const Token& tk = peek();

            if (tk.is(TokenKind::Less)) {
                ++depth;
                if (depth == 2) cur.isBareIdent = false; // nested generic
                advance();
                firstInArg = false;
                continue;
            }
            if (tk.is(TokenKind::Greater)) {
                --depth;
                if (depth == 0) { flushArg(); break; }
                advance();
                firstInArg = false;
                continue;
            }
            if (tk.is(TokenKind::GreaterGreater)) {
                if (depth >= 2) {
                    depth -= 2;
                    if (depth == 0) { flushArg(); break; }
                    advance();
                } else {
                    depth -= 1;
                    if (depth == 0) { flushArg(); break; }
                    advance();
                }
                firstInArg = false;
                continue;
            }
            if (depth == 1 && tk.is(TokenKind::Comma)) {
                flushArg();
                advance();
                continue;
            }
            // Skip `const N:` non-type parameter block at depth==1
            if (depth == 1 && tk.is(TokenKind::KW_const_kw)) {
                cur.isBareIdent = false;
                advance(); // const
                if (check(TokenKind::Identifier)) advance(); // name
                if (check(TokenKind::Colon)) advance(); // :
                while (!isAtEnd() && depth > 0) {
                    const Token& tt = peek();
                    if (tt.is(TokenKind::Less)) { ++depth; advance(); continue; }
                    if (tt.is(TokenKind::Greater)) break;
                    if (tt.is(TokenKind::GreaterGreater)) break;
                    if (depth == 1 && tt.is(TokenKind::Comma)) break;
                    advance();
                }
                continue;
            }
            // Concrete-type signals
            if (tk.isTypeName()) { cur.isBareIdent = false; anySpecToken = true; }
            else if (tk.isOneOf(TokenKind::Star, TokenKind::Amp,
                                TokenKind::LBracket, TokenKind::LParen,
                                TokenKind::KW_fn)) {
                cur.isBareIdent = false; anySpecToken = true;
            } else if (tk.is(TokenKind::Identifier)) {
                if (firstInArg) {
                    cur.name = std::string(tk.text);
                    // Look ahead one step for concrete-type lookahead
                    if (current_ + 1 < tokens_.size()) {
                        const Token& nx = tokens_[current_ + 1];
                        if (nx.isOneOf(TokenKind::Less, TokenKind::ColonColon, TokenKind::Star)) {
                            cur.isBareIdent = false; anySpecToken = true;
                        }
                    }
                } else {
                    // Identifier after first position in same arg → not bare
                    cur.isBareIdent = false;
                }
            }
            if (firstInArg) firstInArg = false;
            advance();
        }
        current_ = savedPos; // always rewind
    }

    // Determine category
    // Check for duplicate bare-ident names (e.g. <T, T>)
    bool hasDuplicateIdent = false;
    {
        std::map<std::string, int> identCount;
        for (auto& ak : argKinds)
            if (ak.isBareIdent && !ak.name.empty())
                ++identCount[ak.name];
        for (auto& [nm, cnt] : identCount)
            if (cnt > 1) { hasDuplicateIdent = true; break; }
    }

    bool isSpec = anySpecToken || hasDuplicateIdent;
    bool allConcrete = false;
    if (isSpec) {
        // A partial spec has at least one bare identifier slot that is
        // genuinely "still generic". If *all* slots are non-bare-ident
        // (and no duplicates), it's a full spec.
        allConcrete = true;
        for (auto& ak : argKinds)
            if (ak.isBareIdent) { allConcrete = false; break; }
        if (hasDuplicateIdent) allConcrete = false;
    }

    if (isSpec && allConcrete) {
        // ── (B) Full specialization ──────────────────────────────────────
        // (Identical to original code.)
        std::vector<TypePtr> specArgs;
        do {
            specArgs.push_back(parseType());
        } while (match(TokenKind::Comma));
        if (check(TokenKind::GreaterGreater)) {
            tokens_[current_].kind = TokenKind::Greater;
            auto extra = tokens_[current_];
            extra.kind = TokenKind::Greater;
            tokens_.insert(tokens_.begin() + current_ + 1, extra);
        }
        expect(TokenKind::Greater, "'>'");

        std::string mangled = decl.name + "<";
        for (size_t i = 0; i < specArgs.size(); ++i) {
            if (i > 0) mangled += ",";
            mangled += typeAnnotationToMangle(specArgs[i].get());
        }
        mangled += ">";

        decl.isFullSpecialization = true;
        decl.fullSpecBaseName = decl.name;
        decl.name = std::move(mangled);
        return;
    }

    if (isSpec) {
        // ── (C) Partial specialization ───────────────────────────────────
        //
        // Strategy:
        //   1. Walk the argument list, collecting TypeAnnotation nodes.
        //   2. For each slot, determine if the top-level token is a
        //      bare identifier that will serve as a fresh generic param.
        //      "Bare" means: the slot in argKinds has isBareIdent == true
        //      (single identifier, no following `<`/`::`/type-keyword).
        //   3. Collect the SET of unique fresh-param names across all slots
        //      (handles both `<T, T>` → {T} and `<T, i32>` → {T}).
        //   4. Store the pattern in `specializationPattern`, the fresh params
        //      in `genericParams`, and set the partial-spec flags.

        // First pass: parse all type annotations from the head
        std::vector<TypePtr> patternArgs;
        do {
            patternArgs.push_back(parseType());
        } while (match(TokenKind::Comma));
        if (check(TokenKind::GreaterGreater)) {
            tokens_[current_].kind = TokenKind::Greater;
            auto extra = tokens_[current_];
            extra.kind = TokenKind::Greater;
            tokens_.insert(tokens_.begin() + current_ + 1, extra);
        }
        expect(TokenKind::Greater, "'>'");

        // Build set of fresh generic params: any bare identifier in argKinds
        // that doesn't look like a concrete type name. We trust argKinds[i].
        // The order matters: maintain first-seen ordering in genericParams.
        std::vector<std::string> freshParams;
        std::map<std::string, bool> seen;
        for (auto& ak : argKinds) {
            if (ak.isBareIdent && !ak.name.empty() && !seen.count(ak.name)) {
                seen[ak.name] = true;
                freshParams.push_back(ak.name);
            }
        }

        decl.isPartialSpecialization = true;
        decl.partialSpecBaseName = decl.name;
        // Keep decl.name unchanged (primary base name) — Sema will find it
        // via partialSpecBaseName when enumerating candidates.
        decl.genericParams = std::move(freshParams);
        decl.specializationPattern = std::move(patternArgs);
        return;
    }

    // ── (A) Regular generic-parameter list ──────────────────────────────
    do {
        // Variadic pack `...Ts` can appear at any position in the list,
        // not just at the start. Setting isVariadicGeneric=true on first
        // encounter is enough — the generic-param vector still carries
        // the pack name.
        if (match(TokenKind::Ellipsis)) {
            decl.isVariadicGeneric = true;
            auto param = expect(TokenKind::Identifier, "pack parameter name");
            decl.genericParams.push_back(std::string(param.text));
            continue;
        }
        // Non-type template parameter: `const N: T`. The leading `const`
        // distinguishes it from a regular type parameter and the colon-typed
        // annotation gives the value type the call site must satisfy.
        if (match(TokenKind::KW_const_kw)) {
            auto param = expect(TokenKind::Identifier, "const parameter name");
            std::string paramName = std::string(param.text);
            expect(TokenKind::Colon, "':' after const parameter name");
            auto valueType = parseType();
            decl.genericParams.push_back(paramName);
            decl.genericConstParams[paramName] = std::move(valueType);
        } else {
            auto param = expect(TokenKind::Identifier, "generic parameter");
            std::string paramName = std::string(param.text);
            decl.genericParams.push_back(paramName);
            if (match(TokenKind::Colon)) {
                std::vector<std::string> constraints;
                do {
                    auto constraint = expect(TokenKind::Identifier, "constraint type");
                    constraints.push_back(std::string(constraint.text));
                } while (match(TokenKind::Plus));
                decl.genericConstraints[paramName] = std::move(constraints);
            }
        }
    } while (match(TokenKind::Comma));
    expect(TokenKind::Greater, "'>'");
}

// ============================================================
//  Translation Unit
// ============================================================

TranslationUnit Parser::parseTranslationUnit(std::string_view filename) {
    TranslationUnit unit;
    unit.filename = std::string(filename);
    while (!isAtEnd()) {
        if (diag_.errorCount() > 200) {
            diag_.error(peek().location, "too many errors, aborting");
            break;
        }
        size_t before = current_;
        auto decl = parseDeclaration();
        if (decl) {
            unit.declarations.push_back(std::move(decl));
        } else if (current_ == before && !isAtEnd()) {
            synchronize();
        }
        for (auto& pd : pendingDecls_)
            unit.declarations.push_back(std::move(pd));
        pendingDecls_.clear();
    }
    return unit;
}

// ============================================================
//  std-module auto-qualify (P4-A.2 ODR fix)
// ============================================================
//
// The block-form `module Foo.Bar { ... }` path in parseModuleDecl already
// renames every inner type decl to `Foo.Bar.Name` and rewrites bare
// references inside the block. The std library, however, uses the older
// file-scope form (`module std.collections;` at top, no block braces),
// so its type decls keep their bare short names and collide with
// user-declared types of the same name in the combined TU's symbol
// table / CodeGen struct-name map.
//
// Observed bug: `class StringBuilder { buf: rawptr; len: i64; cap: i64 }`
// in std/string.vyx and `struct StringBuilder { buf: string; count: i64 }`
// in a user test both register under the single key `StringBuilder`.
// CodeGen then emits one `%StringBuilder = type { ... }` — whichever was
// seen first wins — and inits against the OTHER source write through the
// wrong offsets. A `drop()` method synthesised for a heap-owning std
// container then calls `free()` on a non-pointer field of a user
// instance, crashing at runtime.
//
// Fix: scan the combined TU (user + imported decls) for every top-level
// file-scope `module std.X;` sentinel. Rename every type decl that sits
// at that module's immediate scope to `std.X.Name`, then do a SINGLE
// global `rewriteBlockShortNames` pass over the entire unit so every
// reference to a std short name (in any file) picks up the qualified
// form. Non-std file-scope modules (user's `module Foo;`) are left
// untouched to preserve existing flat-namespace semantics. Nested
// block-form `module ... { ... }` pairs have already been qualified by
// their own pass at parse time.
//
// Invoked from ImportResolver AFTER all imported files have been
// stitched into the user's translation unit. Cannot run at
// parseTranslationUnit time because that sees only one file at a time,
// so a reference to `Vec` from std/collections.vyx to
// std/vec.vyx's `Vec` wouldn't find the other file's rename table.
void Parser::autoQualifyStdModuleGlobal(TranslationUnit& unit) {
    if (unit.declarations.empty()) return;

    // Phase 1: build the global short→qualified table by scanning every
    // top-level file-scope `__module X.Y;` sentinel whose path starts
    // with `std.`. Rename the decls belonging to that module in-place.
    //
    // File-scope form has no matching `__module_end`. It opens at its
    // sentinel and closes at the next top-level `__module` (whether
    // file-scope OR block-start) or end-of-stream. Block-form sentinels
    // are balanced and bump blockDepth: we only consider decls at
    // blockDepth == 0 relative to the enclosing file-scope module for
    // auto-qualify — inner block decls have already been qualified by
    // parseModuleDecl's block path.
    // Reserved short names hard-coded in CodeGen/Mono (smart-pointer
    // intercepts, ADT-variant ctor dispatch, iterator-method routing,
    // container-layout assumptions). Renaming these to `std.X.Name`
    // would break every `name == "Box"` / `name == "Vec"` check in
    // downstream passes. Keep them at their short names — they can
    // still collide with user decls, but Sema enforces that collision
    // separately (see the `kReserved` list in SemaDecl.cpp). Full
    // disintermediation (teaching every CodeGen call site to accept
    // either name) is follow-up work; scoping this fix to non-reserved
    // short names is enough to close the reported ODR bug
    // (`StringBuilder` is NOT reserved in any CodeGen path).
    static const std::set<std::string> kReservedShortNames = {
        "Box", "Ref", "Scope", "Weak",
        "Vec", "Dict", "HashMap", "HashSet", "Set",
        "List", "Queue", "Stack", "Deque", "Slice",
        "Pair", "Tuple3", "Tuple4",
        "Iterator", "Iter", "Iterable",
        "Option", "Result",
    };

    std::map<std::string, std::string> shortToQualified;
    std::string currentFileScope;
    int blockDepth = 0;
    // Track whether we've already entered user-side territory — the first
    // non-imported decl marks the transition. At that point every imported
    // `__module X;` sentinel is stale (imports are just stitched in
    // top-of-unit, not delimited by their own end marker), so we must
    // clear currentFileScope or user's file-scope-less declarations
    // would inherit the last imported std module's scope and get
    // wrongly renamed (e.g. user's `struct StringBuilder` → `std.X.StringBuilder`).
    bool sawUserDecl = false;
    for (size_t i = 0; i < unit.declarations.size(); ++i) {
        auto& d = unit.declarations[i];
        if (!d) continue;
        if (!d->isImported && !sawUserDecl) {
            // First user-side decl: reset module scope. The user's own
            // `module X;` sentinel (if any) appears as a user decl itself
            // and will re-establish currentFileScope on the next iteration.
            currentFileScope.clear();
            blockDepth = 0;
            sawUserDecl = true;
        }
        if (d->kind == DeclKind::Import) {
            auto* imp = d->as<ImportDecl>();
            if (imp->importNames.empty()) continue;
            const auto& marker = imp->importNames[0];
            if (marker == "__module" && !imp->importPath.empty()) {
                // A `__module X` sentinel either opens a file-scope
                // module (no brace, no matching __module_end) or a
                // block-form module (brace open). We can't distinguish
                // here lexically, so use the following heuristic:
                //   - If blockDepth == 0 AND the NEXT occurrence of
                //     either `__module` or `__module_end` reachable
                //     forward is `__module` (or EOS), this one is
                //     file-scope.  Otherwise it's block-form.
                // Simpler encoding used by parseModuleDecl: block form
                // ALWAYS emits both `__module` AND `__module_end` for
                // its contents.  Peek forward until we hit one or the
                // other at the same depth.
                bool isBlock = false;
                int depth = 1;
                for (size_t j = i + 1; j < unit.declarations.size(); ++j) {
                    auto& d2 = unit.declarations[j];
                    if (!d2 || d2->kind != DeclKind::Import) continue;
                    auto* imp2 = d2->as<ImportDecl>();
                    if (imp2->importNames.empty()) continue;
                    const auto& m2 = imp2->importNames[0];
                    if (m2 == "__module") ++depth;
                    else if (m2 == "__module_end") {
                        --depth;
                        if (depth == 0) { isBlock = true; break; }
                    }
                }
                if (isBlock) {
                    ++blockDepth;
                } else {
                    currentFileScope = imp->importPath.back();
                    blockDepth = 0;
                }
                continue;
            }
            if (marker == "__module_end") {
                if (blockDepth > 0) --blockDepth;
                continue;
            }
            continue;
        }

        // Only touch decls that sit at the file-scope module's immediate
        // scope (blockDepth == 0) AND whose enclosing file-scope module
        // is a `std.` module.
        if (blockDepth > 0) continue;
        if (currentFileScope.empty()) continue;
        if (!currentFileScope.starts_with("std.")) continue;

        switch (d->kind) {
            case DeclKind::Class: {
                auto* cd = d->as<ClassDecl>();
                // `impl Trait for Target` is parsed as a ClassDecl with
                // `isImplBlock = true`; its `name` is the TARGET type
                // (e.g. `f64`, `Vec`), not a fresh declaration. We must
                // not rename it — the target lookup later (`symbols_.
                // lookupType(decl.name)`) needs the short name intact.
                // References to OTHER renamed types inside the impl's
                // method bodies are handled by the bulk rewrite below.
                if (cd->isImplBlock) break;
                if (d->name.find('.') != std::string::npos) continue;
                if (kReservedShortNames.count(d->name)) continue;
                std::string qualified = currentFileScope + "." + d->name;
                shortToQualified[d->name] = qualified;
                d->name = std::move(qualified);
                break;
            }
            case DeclKind::Struct:
            case DeclKind::Interface:
            case DeclKind::ErrorDef:
            case DeclKind::TypeAlias: {
                if (d->name.find('.') != std::string::npos) continue;
                if (kReservedShortNames.count(d->name)) continue;
                std::string qualified = currentFileScope + "." + d->name;
                shortToQualified[d->name] = qualified;
                d->name = std::move(qualified);
                break;
            }
            default:
                break;
        }
    }

    if (shortToQualified.empty()) return;

    // Phase 2: a single global pass over the whole TU rewrites every
    // bare short-name reference to its qualified form. Nested block
    // decls that already use the qualified form (from parseModuleDecl's
    // block pass) are unaffected — the rewrite table only has entries
    // for short names, and `NsRewriter` only rewrites when it finds an
    // exact table match.
    //
    // A crucial corollary: bare short-name references inside user-side
    // decls (files that live OUTSIDE any std module) will also be
    // rewritten by this pass. That's the intended behaviour — a user
    // who writes `Vec::<i64>.new()` with `use std.collections;` gets
    // their reference redirected to `std.collections.Vec`. The only
    // exception is when the user has declared a local type with the
    // same short name: rewriteBlockShortNames has no awareness of
    // shadowing, so the user's local reference would be incorrectly
    // mapped.
    //
    // Mitigation: build a set of user-declared type names FIRST, and
    // remove those entries from shortToQualified so the user's bare
    // references keep pointing at their own types. This preserves the
    // ODR fix (std and user types occupy distinct keys) without
    // breaking user code that names a type the same as a std type.
    std::set<std::string> userTypeNames;
    for (auto& d : unit.declarations) {
        if (!d || d->isImported) continue;
        switch (d->kind) {
            case DeclKind::Class:
                if (d->as<ClassDecl>()->isImplBlock) break;
                userTypeNames.insert(d->name);
                break;
            case DeclKind::Struct:
            case DeclKind::Interface:
            case DeclKind::ErrorDef:
            case DeclKind::TypeAlias:
                userTypeNames.insert(d->name);
                break;
            default:
                break;
        }
    }

    // Build the user-side rewrite table by dropping every short name that
    // matches a user-declared type. Inside user code the bare identifier
    // `StringBuilder` must keep pointing at the user's own struct; only
    // non-shadowed std names (`Vec`, `HashMap`, …) get re-qualified so
    // `use std.collections;` plus a bare `Vec::<i64>.new()` still works.
    std::map<std::string, std::string> userRewrite = shortToQualified;
    for (auto it = userRewrite.begin(); it != userRewrite.end(); ) {
        if (userTypeNames.count(it->first))
            it = userRewrite.erase(it);
        else
            ++it;
    }

    // Partition declarations into imported vs user-side ranges and rewrite
    // each with the appropriate table. Imported (std) decls use the FULL
    // shortToQualified table — every internal reference between std files
    // gets qualified (e.g. std.collections.Vec's methods referring to
    // `StringBuilder` stay pointed at `std.collections.StringBuilder`
    // even when the user ALSO has a `struct StringBuilder`). User decls
    // use the filtered `userRewrite` so their own short-name types keep
    // pointing at themselves.
    //
    // ImportResolver prepends every imported decl to unit.declarations
    // (see resolve() near the `allImported_.begin()` insert), so the
    // imported range forms a contiguous prefix. We split at the first
    // non-imported decl.
    auto firstUser = unit.declarations.end();
    for (auto it = unit.declarations.begin(); it != unit.declarations.end(); ++it) {
        if (*it && !(*it)->isImported) { firstUser = it; break; }
    }
    if (!shortToQualified.empty() && firstUser != unit.declarations.begin()) {
        rewriteBlockShortNames(
            shortToQualified,
            unit.declarations.begin(),
            firstUser);
    }
    if (!userRewrite.empty() && firstUser != unit.declarations.end()) {
        rewriteBlockShortNames(
            userRewrite,
            firstUser,
            unit.declarations.end());
    }
}

} // namespace vyx
