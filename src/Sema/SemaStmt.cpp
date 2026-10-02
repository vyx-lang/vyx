#include "Sema.h"
#include "SemaCloneHelpers.h"
#include "../Common/StringUtils.h"
#include <algorithm>
#include <functional>
#include <set>

namespace vyx {

static bool isHashMapWrapperLike(const VyxType& t) {
    if (t.kind != VyxTypeKind::Class) return false;
    return (t.name.size() > 8 && t.name.compare(0, 8, "HashMap<") == 0) ||
           (t.name.size() > 10 && t.name.compare(0, 10, "StringMap<") == 0) ||
           (t.name.size() > 15 && t.name.compare(0, 15, "ConcurrentDict<") == 0);
}

static bool hasAutoDropPayload(const VyxType& t) {
    if (isHeapOwningType(t)) return true;
    if (auto inner = optionInner(t)) {
        return isHeapOwningType(*inner);
    }
    if (isResultLike(t)) {
        if (auto okT = resultOk(t)) {
            if (isHeapOwningType(*okT)) return true;
        }
        if (auto errT = resultErr(t)) {
            if (isHeapOwningType(*errT)) return true;
        }
    }
    return false;
}

static bool isBorrowedContainerElementResult(const Expr* expr) {
    if (!expr || expr->kind != ExprKind::Call) return false;
    auto* call = expr->as<CallExpr>();
    if (!call->callee || call->callee->kind != ExprKind::MemberAccess) return false;
    auto* ma = call->callee->as<MemberAccessExpr>();
    if (!ma || !ma->object) return false;
    auto objType = ma->object->inferredType;
    if (!objType) return false;
    if (isVecLike(*objType)) {
        return ma->member == "get" || ma->member == "get_unchecked" ||
               ma->member == "first" || ma->member == "last";
    }
    if (isMapLike(*objType) || isHashMapWrapperLike(*objType)) {
        return ma->member == "get" || ma->member == "try_get";
    }
    return false;
}

// ============================================================
//  Statement analysis
// ============================================================

void Sema::analyzeStmt(Stmt& stmt) {
    switch (stmt.kind) {
        case StmtKind::Block:      analyzeBlock(stmt); break;
        case StmtKind::VarDecl:    analyzeVarDecl(stmt); break;
        case StmtKind::If:         analyzeIfStmt(stmt); break;
        case StmtKind::While:      analyzeWhileStmt(stmt); break;
        case StmtKind::For:        analyzeForStmt(stmt); break;
        case StmtKind::Return:     analyzeReturnStmt(stmt); break;
        case StmtKind::Break:
        case StmtKind::Continue:   break;
        case StmtKind::ExprStmt: {
            auto* es = stmt.as<ExprStmt>();
            if (!es->expr)
                diag_.error(stmt.location, "expression statement has no expression");
            else
                analyzeExpr(*es->expr);
            break;
        }
        case StmtKind::Assignment: {
            auto* as = stmt.as<AssignStmt>();
            if (!as->target)
                diag_.error(stmt.location, "assignment statement is missing target");
            else
                analyzeExpr(*as->target);
            if (!as->value)
                diag_.error(stmt.location, "assignment statement is missing value");
            if (as->value) {
                auto valType = analyzeExpr(*as->value);
                // P4-B: mark the source moved when the assignment RHS is a
                // bare heap-owning identifier. Clone escape hatch is handled
                // below — a CallExpr RHS whose callee is `.clone()` does not
                // enter this branch since `as->value->kind != Identifier`.
                if (as->value->kind == ExprKind::Identifier && valType) {
                    if (isHeapOwningType(*valType)) {
                        auto* srcSym = symbols_.lookup(as->value->as<IdentifierExpr>()->name);
                        if (srcSym && !isStringType(*valType) &&
                            (!srcSym->isConst || isMoveOnlyHeapOwningType(*valType))) {
                            srcSym->isMoved = true;
                            srcSym->moveLocation = as->value->location;
                        }
                    }
                }
                // P4-D follow-up (limit #1): when the target is a bare
                // identifier bound to a still-live heap-owning value,
                // signal CodeGen to drop the old binding before the store.
                // Skip if the target is itself moved/escaped (the old data
                // is conceptually gone) or if RHS is the same identifier
                // (`a = a` is a no-op for ownership purposes).
                if (as->target && as->target->kind == ExprKind::Identifier) {
                    auto* tgtSym = symbols_.lookup(as->target->as<IdentifierExpr>()->name);
                    if (tgtSym && tgtSym->type && !tgtSym->isConst &&
                        isHeapOwningType(*tgtSym->type) &&
                        !tgtSym->isMoved && !tgtSym->escapes) {
                        bool sameIdent =
                            as->value->kind == ExprKind::Identifier &&
                            as->value->as<IdentifierExpr>()->name ==
                                as->target->as<IdentifierExpr>()->name;
                        if (!sameIdent) as->dropOldLvalue = true;
                    }
                }
            }
            break;
        }
        case StmtKind::Match: {
            auto* ms = stmt.as<MatchStmt>();
            if (!ms->expr)
                diag_.error(stmt.location, "match statement is missing scrutinee expression");
            VyxTypePtr matchExprType = ms->expr ? analyzeExpr(*ms->expr) : nullptr;
            // Pattern/scrutinee type compatibility: reject `Some/None` arms
            // when the scrutinee is not an Option, and `Ok/Err` arms when the
            // scrutinee is not a Result/ErrorType. Without this, mismatched
            // matches fall through to CodeGen's bare-integer path producing
            // a cryptic "match on integer tag with payload bindings" error
            // or, worse, silently "work" by extracting a garbage payload.
            if (matchExprType) {
                auto k = matchExprType->kind;
                bool isPrimitive = k == VyxTypeKind::Integer ||
                                   k == VyxTypeKind::Bool    ||
                                   k == VyxTypeKind::Float   ||
                                   k == VyxTypeKind::Char    ||
                                   isStringType(*matchExprType);
                // Class scrutinees: `Some/None/Ok/Err` are only valid when the
                // class is the Option or Result tagged-union (their method
                // bodies are compiled as classes but the variants are
                // Some/None/Ok/Err). For any other class, these arm labels
                // are an error — the user most likely tried to match on a
                // plain object as if it were an ADT. Error-def types use
                // VyxTypeKind::ErrorType with `errorVariants` listed, which
                // the exhaustiveness check below already validates, so we
                // don't need to cross-check labels there.
                bool isClassNonAdt = false;
                if (k == VyxTypeKind::Class || k == VyxTypeKind::Struct) {
                    const std::string& nm = matchExprType->name;
                    // R5 stage 2: lang-item registry is the sole source of
                    // truth — the old `"Option"` / `"Result"` string
                    // fallbacks are gone along with the bootstrap path.
                    auto matchesLang = [&](const char* slot) {
                        const Decl* l = langItems_.find(slot);
                        if (!l) return false;
                        if (nm == l->name) return true;
                        std::string pref = l->name + "<";
                        return nm.compare(0, pref.size(), pref) == 0;
                    };
                    bool looksLikeOption = matchesLang("option");
                    bool looksLikeResult = matchesLang("result");
                    if (!looksLikeOption && !looksLikeResult) {
                        isClassNonAdt = true;
                    }
                }
                if (isPrimitive || isClassNonAdt) {
                    for (auto& arm : ms->arms) {
                        if (arm.isDefault || arm.label.empty()) continue;
                        const std::string& lbl = arm.label;
                        if (lbl == "Some" || lbl == "None" ||
                            lbl == "Ok"   || lbl == "Err") {
                            diag_.error(stmt.location,
                                "match arm '{}' requires an ADT scrutinee "
                                "(Option<T>/Result<T>/error-def), got '{}'",
                                lbl, matchExprType->toString());
                            break;
                        }
                    }
                }
            }
            {
                bool hasDefault = false;
                auto armPayloadType = [&](const MatchArm& arm) -> VyxTypePtr {
                    if (!matchExprType) return nullptr;
                    if (arm.label == "Some") return optionInner(*matchExprType);
                    if (arm.label == "Ok" || arm.label == "ok") return resultOk(*matchExprType);
                    if (arm.label == "Err" || arm.label == "fail") return resultErr(*matchExprType);
                    return nullptr;
                };
                auto addBindingAutoDrop = [&](BlockStmt* body, const std::string& name, Symbol* sym) {
                    if (!body || !sym || !sym->type) return;
                    if (sym->isMoved || sym->escapes) return;
                    if (isHeapOwningType(*sym->type)) {
                        if (std::find(body->autoDropLocals.begin(),
                                      body->autoDropLocals.end(),
                                      name) == body->autoDropLocals.end()) {
                            body->autoDropLocals.push_back(name);
                        }
                        return;
                    }
                    if (auto inner = optionInner(*sym->type)) {
                        if (isHeapOwningType(*inner) && !inner->name.empty()) {
                            body->autoDropAdtPayloads.push_back({name, inner->name, 0});
                        }
                        return;
                    }
                    if (isResultLike(*sym->type)) {
                        if (auto okT = resultOk(*sym->type)) {
                            if (isHeapOwningType(*okT) && !okT->name.empty()) {
                                body->autoDropAdtPayloads.push_back({name, okT->name, 0});
                            }
                        }
                        if (auto errT = resultErr(*sym->type)) {
                            if (isHeapOwningType(*errT) && !errT->name.empty()) {
                                body->autoDropAdtPayloads.push_back({name, errT->name, 1});
                            }
                        }
                    }
                };
                for (auto& arm : ms->arms) {
                    if (arm.isDefault) hasDefault = true;
                    if (arm.body) {
                        symbols_.pushScope();
                        if (!arm.bindingName.empty()) {
                            Symbol bindSym;
                            bindSym.name = arm.bindingName;
                            bindSym.type = armPayloadType(arm);
                            if (!bindSym.type) bindSym.type = types::makeUnknown();
                            symbols_.declare(arm.bindingName, std::move(bindSym));
                        }
                        for (auto& tb : arm.tupleBindings) {
                            Symbol tbSym;
                            tbSym.name = tb;
                            tbSym.type = types::makeUnknown();
                            symbols_.declare(tb, std::move(tbSym));
                        }
                        analyzeStmt(*arm.body);
                        if (!arm.bindingName.empty() &&
                            arm.body->kind == StmtKind::Block) {
                            auto* bindSym = symbols_.lookup(arm.bindingName);
                            addBindingAutoDrop(arm.body->as<BlockStmt>(),
                                arm.bindingName, bindSym);
                        }
                        symbols_.popScope();
                    }
                }
                // P4-B: a bare heap-owning identifier scrutinee transfers
                // ownership into the match — mark moved. We do not attempt
                // to recognise `match &a { ... }` (borrow) here; the scrutinee
                // in that case is a UnaryOp/Ref expression, not a plain
                // Identifier, so it naturally skips this branch.
                if (ms->expr && ms->expr->kind == ExprKind::Identifier && matchExprType) {
                    if (isHeapOwningType(*matchExprType)) {
                        auto* srcSym = symbols_.lookup(ms->expr->as<IdentifierExpr>()->name);
                        if (srcSym && !isStringType(*matchExprType) &&
                            (!srcSym->isConst || isMoveOnlyHeapOwningType(*matchExprType))) {
                            srcSym->isMoved = true;
                            srcSym->moveLocation = ms->expr->location;
                        }
                    }
                }
                if (!hasDefault) {
                    VyxTypePtr matchType = matchExprType;
                    if (matchType && matchType->kind == VyxTypeKind::ErrorType && !matchType->errorVariants.empty()) {
                        std::set<std::string> coveredVariants;
                        for (auto& arm : ms->arms) {
                            if (!arm.label.empty()) coveredVariants.insert(arm.label);
                        }
                        std::vector<std::string> missing;
                        for (auto& variant : matchType->errorVariants) {
                            if (!coveredVariants.contains(variant)) missing.push_back(variant);
                        }
                        if (!missing.empty()) {
                            std::string missingStr;
                            for (size_t i = 0; i < missing.size(); ++i) {
                                if (i > 0) missingStr += ", ";
                                missingStr += missing[i];
                            }
                            diag_.error(stmt.location,
                                "non-exhaustive match on '{}': missing variant(s): {}",
                                matchType->toString(), missingStr);
                        }
                    } else if (matchType && matchType->kind == VyxTypeKind::Bool) {
                        bool hasTrue = false, hasFalse = false;
                        for (auto& arm : ms->arms) {
                            if (arm.label == "true") hasTrue = true;
                            if (arm.label == "false") hasFalse = true;
                        }
                        if (!hasTrue || !hasFalse) {
                            diag_.warning(stmt.location, "non-exhaustive match on bool: add 'default' or cover true/false");
                        }
                    } else {
                        diag_.warning(stmt.location, "match without 'default' may not be exhaustive");
                    }
                }
            }
            break;
        }
        case StmtKind::ForEach: {
            auto* fe = stmt.as<ForEachStmt>();
            if (!fe->collection)
                diag_.error(stmt.location, "for-each is missing collection expression");
            VyxTypePtr elemType = types::makeUnknown();
            std::string collName;
            if (fe->collection) {
                auto collType = analyzeExpr(*fe->collection);
                if (fe->collection->kind == ExprKind::Identifier)
                    collName = fe->collection->as<IdentifierExpr>()->name;
                if (collType && collType->kind == VyxTypeKind::Array && collType->elementType) {
                    elemType = collType->elementType;
                } else if (collType && collType->kind == VyxTypeKind::Range) {
                    elemType = types::makeInt(64, true);
                } else if (collType && isVecLike(*collType)) {
                    if (auto elem = vecElement(*collType))
                        elemType = elem;
                } else if (collType && (collType->kind == VyxTypeKind::Struct || collType->kind == VyxTypeKind::Class)) {
                    for (auto& m : collType->methods) {
                        if (m.name == "next" && m.returnType) {
                            // If next() returns Option<T>, the for-loop binder is T
                            // (the runtime auto-unwraps Some(v)→v and exits on None)
                            if (auto inner = optionInner(*m.returnType)) {
                                elemType = inner;
                            } else {
                                elemType = m.returnType;
                            }
                            break;
                        }
                    }
                }
            }
            if (fe->body) {
                symbols_.pushScope();
                if (!collName.empty()) iteratingCollections_.insert(collName);
                Symbol iterSym;
                iterSym.name = fe->varName;
                iterSym.type = elemType;
                symbols_.declare(fe->varName, std::move(iterSym));
                for (auto& dvar : fe->destructure) {
                    Symbol dsym;
                    dsym.name = dvar;
                    dsym.type = types::makeUnknown();
                    symbols_.declare(dvar, std::move(dsym));
                }
                analyzeStmt(*fe->body);
                if (!collName.empty()) iteratingCollections_.erase(collName);
                symbols_.popScope();
            }
            break;
        }
        case StmtKind::Defer: {
            auto* ds = stmt.as<DeferStmt>();
            if (ds->body) analyzeStmt(*ds->body);
            break;
        }
        case StmtKind::Unsafe: {
            auto* us = stmt.as<UnsafeStmt>();
            auto savedUnsafe = inUnsafe_;
            inUnsafe_ = true;
            if (us->body) analyzeStmt(*us->body);
            inUnsafe_ = savedUnsafe;
            break;
        }
        case StmtKind::StaticAssert: {
            auto* sa = stmt.as<StaticAssertStmt>();
            if (sa->expr) {
                bool condResult = false;
                if (sa->expr->kind == ExprKind::BoolLiteral) {
                    condResult = sa->expr->as<BoolLiteralExpr>()->value;
                } else if (sa->expr->kind == ExprKind::IntLiteral) {
                    condResult = sa->expr->as<IntLiteralExpr>()->value != 0;
                } else if (sa->expr->kind == ExprKind::BinaryOp) {
                    auto* binExpr = sa->expr->as<BinaryOpExpr>();
                    if (binExpr->lhs && binExpr->rhs &&
                        binExpr->lhs->kind == ExprKind::IntLiteral &&
                        binExpr->rhs->kind == ExprKind::IntLiteral) {
                        int64_t l = binExpr->lhs->as<IntLiteralExpr>()->value;
                        int64_t r = binExpr->rhs->as<IntLiteralExpr>()->value;
                        switch (binExpr->op) {
                            case BinaryOp::Eq:  condResult = (l == r); break;
                            case BinaryOp::Neq: condResult = (l != r); break;
                            case BinaryOp::Lt:  condResult = (l < r); break;
                            case BinaryOp::Lte: condResult = (l <= r); break;
                            case BinaryOp::Gt:  condResult = (l > r); break;
                            case BinaryOp::Gte: condResult = (l >= r); break;
                            default:
                                diag_.warning(stmt.location, "static_assert: non-comparison operator not evaluated at compile time");
                                condResult = true;
                                break;
                        }
                    } else {
                        diag_.warning(stmt.location, "static_assert: non-constant expression cannot be fully evaluated at compile time");
                        condResult = true;
                    }
                } else {
                    diag_.warning(stmt.location, "static_assert: expression type not evaluable at compile time — assumed true");
                    condResult = true;
                }
                if (!condResult) {
                    std::string msg = sa->message.empty()
                        ? "static assertion failed"
                        : "static assertion failed: " + sa->message;
                    diag_.error(stmt.location, "{}", msg);
                }
            }
            break;
        }
        default:
            diag_.error(stmt.location, "internal: unhandled statement kind in semantic analysis ({})",
                static_cast<int>(stmt.kind));
            break;
    }
}

void Sema::analyzeBlock(Stmt& block) {
    auto* bs = block.as<BlockStmt>();
    symbols_.pushScope();
    for (auto& s : bs->statements) {
        if (s) analyzeStmt(*s);
    }
    // P4-C: collect heap-owning locals that are still alive at block end
    // (not moved, not escaped via return) so CodeGen can emit their
    // `.drop()` calls before the block's natural terminator. Runs before
    // popScope while current_ still points at this block's scope.
    bs->autoDropLocals.clear();
    bs->autoDropAdtPayloads.clear();
    symbols_.forEachLocalSymbol([&](const std::string& name, Symbol& sym) {
        if (sym.isFunction || sym.isImported) return;
        if (!sym.type) return;
        if (sym.isMoved) return;
        if (sym.escapes) return;
        if (isHeapOwningType(*sym.type)) {
            bs->autoDropLocals.push_back(name);
            return;
        }
        // P4-D follow-up (limit #2): Option<H> / Result<H,_> / Result<_,H>
        // where H is a heap-owning class need a tag-conditional drop for
        // the carried H even though the ADT itself doesn't have a `.drop`.
        // We record the inner type name here; CodeGen reads the tag at
        // runtime and dispatches to `H.drop` only when the active variant
        // matches.
        if (auto inner = optionInner(*sym.type)) {
            if (isHeapOwningType(*inner) && !inner->name.empty()) {
                bs->autoDropAdtPayloads.push_back({name, inner->name, 0});
            }
            return;
        }
        if (isResultLike(*sym.type)) {
            if (auto okT = resultOk(*sym.type)) {
                if (isHeapOwningType(*okT) && !okT->name.empty()) {
                    bs->autoDropAdtPayloads.push_back({name, okT->name, 0});
                }
            }
            if (auto errT = resultErr(*sym.type)) {
                if (isHeapOwningType(*errT) && !errT->name.empty()) {
                    bs->autoDropAdtPayloads.push_back({name, errT->name, 1});
                }
            }
        }
    });
    symbols_.popScope();
}

void Sema::analyzeVarDecl(Stmt& stmt) {
    auto* vd = stmt.as<VarDeclStmt>();

    // Tuple destructuring: `let (a, b) = expr;`
    if (!vd->tupleBindings.empty()) {
        VyxTypePtr initType = nullptr;
        if (vd->initExpr) {
            initType = analyzeExpr(*vd->initExpr);
        } else {
            diag_.error(stmt.location, "tuple destructuring requires an initializer");
            return;
        }
        // Determine per-element types (use tuple element types when available)
        size_t n = vd->tupleBindings.size();
        for (size_t i = 0; i < n; ++i) {
            VyxTypePtr elemType = types::makeUnknown();
            if (initType && initType->kind == VyxTypeKind::Tuple && i < initType->tupleTypes.size()) {
                elemType = initType->tupleTypes[i];
            }
            Symbol sym;
            sym.name = vd->tupleBindings[i];
            sym.type = elemType;
            sym.isConst = vd->isConst;
            sym.isMutable = !vd->isConst;
            sym.declLocation = stmt.location;
            sym.scopeDepth = symbols_.currentScopeDepth();
            if (!symbols_.declare(vd->tupleBindings[i], std::move(sym))) {
                diag_.error(stmt.location, "redeclaration of '{}'", vd->tupleBindings[i]);
            }
        }
        return;
    }

    VyxTypePtr declaredType = nullptr;
    if (vd->varType) {
        declaredType = resolveType(*vd->varType);
    }

    VyxTypePtr initType = nullptr;
    if (vd->initExpr) {
        initType = analyzeExpr(*vd->initExpr);
    }

    VyxTypePtr finalType;
    if (declaredType) {
        finalType = declaredType;
        if (initType && !isAssignable(*declaredType, *initType,
                vd->initExpr ? vd->initExpr->location : stmt.location)) {
            diag_.error(stmt.location, "cannot assign '{}' to variable of type '{}'",
                        initType->toString(), declaredType->toString());
        }
    } else if (initType) {
        finalType = initType;
    } else {
        diag_.error(stmt.location, "variable '{}' requires a type annotation or initializer", vd->varName);
        finalType = types::makeUnknown();
    }

    Symbol sym;
    sym.name = vd->varName;
    sym.type = finalType;
    sym.isConst = vd->isConst;
    sym.isMutable = !vd->isConst;
    sym.declLocation = stmt.location;
    sym.scopeDepth = symbols_.currentScopeDepth();
    if (finalType && hasAutoDropPayload(*finalType) &&
        isBorrowedContainerElementResult(vd->initExpr.get())) {
        // Vec<T>.get/Dict<K,V>.get expose borrowed views of container slots.
        // The binding may be used like a value, but it must not auto-drop the
        // slot's backing storage at scope exit.
        sym.escapes = true;
    }

    if (finalType && finalType->kind == VyxTypeKind::Reference && vd->initExpr) {
        if (vd->initExpr->kind == ExprKind::UnaryOp) {
            auto* unaryE = vd->initExpr->as<UnaryOpExpr>();
            if (unaryE->operand && unaryE->operand->kind == ExprKind::Identifier) {
                auto* srcSym = symbols_.lookup(unaryE->operand->as<IdentifierExpr>()->name);
                if (srcSym) {
                    // Do NOT bump `borrowCount` here — analyzeUnaryOp's Ref /
                    // MutRef path has already incremented it when the inner
                    // `&x` / `&mut x` was analysed. Double-counting leaks
                    // through into the mut-while-shared diagnostic ("N active
                    // shared borrow(s)") as 2 per single `&x` binding.
                    // We still record the lifetime-/moved-value checks and
                    // stamp the borrow scope depth onto the new binding.
                    srcSym->isBorrowed = true;
                    if (srcSym->scopeDepth > sym.scopeDepth) {
                        diag_.error(stmt.location,
                            "reference to '{}' would outlive the referenced value (ref scope={}, source scope={})",
                            unaryE->operand->as<IdentifierExpr>()->name, sym.scopeDepth, srcSym->scopeDepth);
                    }
                    if (srcSym->isMoved) {
                        diag_.error(stmt.location,
                            "cannot borrow '{}' — value has been moved", unaryE->operand->as<IdentifierExpr>()->name);
                    }
                    sym.borrowScopeDepth = srcSym->scopeDepth;
                }
            }
        }
    }

    // ── P4-B: ownership transfer / shared retain on `let|var c = a` ───
    // Two regimes, both routed through the RHS-is-Identifier check:
    //
    //   1. Shared-ownership smart pointers (Ref<T>, Scope<T>) — C++
    //      shared_ptr style. Bind-by-name implicitly retains: the AST
    //      `let c = a;` is rewritten to `let c = a.clone();` so the new
    //      binding owns its own refcount bump and the source `a` stays
    //      live. Fixes the `let c = a;` double-free that arose when the
    //      P4-D auto-drop pass dropped both shallow-copied handles.
    //
    //   2. Unique-ownership heap types (Box/Vec/Dict/Set/Stack/Queue/
    //      String/user class with `fn drop()`) — keep move-on-bind.
    //      Source `a` becomes invalid; further use is diagnosed.
    //
    // Escape hatch (still honoured for unique types): explicit
    // `.clone()` on the RHS (`var b = a.clone()`) is already a CallExpr
    // so it never enters this Identifier branch.
    if (vd->initExpr && vd->initExpr->kind == ExprKind::Identifier && finalType) {
        if (isRefLike(*finalType) || isScopeLike(*finalType)) {
            // Shared-ownership: rewrite `let/var c = a;` → `let/var c = a.clone();`.
            // The new MemberAccess callee form deliberately bypasses
            // `analyzeCall`'s P4-B by-value-arg move marking (which is
            // restricted to Identifier callees), so `a` is not flagged
            // moved and continues to participate in auto-drop.
            auto loc = vd->initExpr->location;
            auto memAccess = std::make_unique<MemberAccessExpr>();
            memAccess->location = loc;
            memAccess->object = std::move(vd->initExpr);
            memAccess->member = "clone";
            auto callExpr = std::make_unique<CallExpr>();
            callExpr->location = loc;
            callExpr->callee = std::move(memAccess);
            vd->initExpr = std::move(callExpr);
            (void)analyzeExpr(*vd->initExpr);
        } else if (isHeapOwningType(*finalType)) {
            auto* srcSym = symbols_.lookup(vd->initExpr->as<IdentifierExpr>()->name);
            if (srcSym && !isStringType(*finalType) &&
                (!srcSym->isConst || isMoveOnlyHeapOwningType(*finalType))) {
                srcSym->isMoved = true;
                srcSym->moveLocation = vd->initExpr->location;
            }
        }
    }

    if (vd->isComptime) {
        if (!vd->initExpr) {
            diag_.error(stmt.location, "'const' requires an initializer (compile-time evaluated)");
        } else if (vd->initExpr->kind != ExprKind::IntLiteral &&
                   vd->initExpr->kind != ExprKind::FloatLiteral &&
                   vd->initExpr->kind != ExprKind::BoolLiteral &&
                   vd->initExpr->kind != ExprKind::StringLiteral &&
                   vd->initExpr->kind != ExprKind::BinaryOp) {
            diag_.warning(stmt.location, "'const' initializer should be a compile-time constant expression");
        }
    }

    if (!symbols_.declare(vd->varName, std::move(sym))) {
        diag_.error(stmt.location, "redeclaration of '{}'", vd->varName);
    }
}

void Sema::analyzeIfStmt(Stmt& stmt) {
    auto* is = stmt.as<IfStmt>();
    if (is->condition) {
        auto condType = analyzeExpr(*is->condition);
        if (condType && !condType->isBoolType() && !condType->isNumeric()) {
            diag_.warning(stmt.location, "condition should be a boolean expression");
        }
    }
    if (is->thenBranch) analyzeStmt(*is->thenBranch);
    for (auto& [cond, body] : is->elifBranches) {
        if (cond) analyzeExpr(*cond);
        if (body) analyzeStmt(*body);
    }
    if (is->elseBranch) analyzeStmt(*is->elseBranch);
}

void Sema::analyzeWhileStmt(Stmt& stmt) {
    auto* ws = stmt.as<WhileStmt>();
    if (ws->condition) analyzeExpr(*ws->condition);
    if (ws->body) analyzeStmt(*ws->body);
}

void Sema::analyzeForStmt(Stmt& stmt) {
    auto* fs = stmt.as<ForStmt>();
    symbols_.pushScope();
    if (fs->init) analyzeStmt(*fs->init);
    if (fs->condition) analyzeExpr(*fs->condition);
    if (fs->step) analyzeExpr(*fs->step);
    if (fs->body) analyzeStmt(*fs->body);
    symbols_.popScope();
}

void Sema::analyzeReturnStmt(Stmt& stmt) {
    auto* rs = stmt.as<ReturnStmt>();
    if (rs->expr) {
        auto retType = analyzeExpr(*rs->expr);
        const bool isUnannotatedVoidReturn =
            currentReturnAnnotation_ == nullptr && currentReturnType_ &&
            currentReturnType_->kind == VyxTypeKind::Void;
        if (!isUnannotatedVoidReturn && currentReturnType_ && retType &&
            !isAssignable(*currentReturnType_, *retType, rs->expr->location)) {
            // Auto-wrap: inside `fn() -> Result<T,E>` / `fn() -> Option<T>`,
            // `return <e>` where <e>: T implicitly becomes `return Ok(<e>)` /
            // `return Some(<e>)`. Mirrors the existing `fail E;` shortcut
            // (which implicitly wraps into `Err(E)`) so success and failure
            // paths are symmetric — both sides of the sum type can use bare
            // expressions, no manual ceremony. The `@[auto_wrap]` attribute
            // is preserved as a no-op for backward compatibility with
            // anything that already opted in explicitly.
            bool wrapped = false;
            if (currentReturnAnnotation_ &&
                currentReturnAnnotation_->kind == TypeAnnotationKind::Generic) {
                const auto* gt = currentReturnAnnotation_->as<GenericType>();
                std::string ctorName;
                const TypeAnnotation* payloadAnn = nullptr;
                // R5 stage 2: auto-wrap is driven solely by the lang-item
                // registry. Units that want `return x` → `Ok(x)` / `Some(x)`
                // sugar must `use` a module that registers the Result /
                // Option lang items (i.e. std.core). No bootstrap fallback.
                auto matchesSlot = [&](const char* slot) -> bool {
                    const Decl* lang = langItems_.find(slot);
                    return lang && lang->name == currentReturnAnnotation_->name;
                };
                if (matchesSlot("result") && !gt->typeArgs.empty()) {
                    payloadAnn = gt->typeArgs[0].get();
                    ctorName = "Ok";
                } else if (matchesSlot("option") && !gt->typeArgs.empty()) {
                    payloadAnn = gt->typeArgs[0].get();
                    ctorName = "Some";
                }
                if (payloadAnn) {
                    auto payloadType = resolveType(*payloadAnn);
                    if (payloadType &&
                        isAssignable(*payloadType, *retType, rs->expr->location)) {
                        auto ctor = std::make_unique<IdentifierExpr>();
                        ctor->name = ctorName;
                        ctor->location = rs->expr->location;
                        auto call = std::make_unique<CallExpr>();
                        call->callee = std::move(ctor);
                        call->args.push_back(std::move(rs->expr));
                        call->location = stmt.location;
                        rs->expr = std::move(call);
                        retType = currentReturnType_;
                        wrapped = true;
                    }
                }
            }
            if (!wrapped) {
                diag_.error(stmt.location, "cannot return '{}' from function returning '{}'",
                            retType->toString(), currentReturnType_->toString());
            }
        }
        if (retType && retType->kind == VyxTypeKind::Reference && rs->expr->kind == ExprKind::UnaryOp) {
            auto* unaryE = rs->expr->as<UnaryOpExpr>();
            if (unaryE->operand && unaryE->operand->kind == ExprKind::Identifier) {
                auto* sym = symbols_.lookup(unaryE->operand->as<IdentifierExpr>()->name);
                if (sym && !sym->isFunction) {
                    diag_.error(stmt.location,
                        "cannot return reference to local variable '{}'",
                        unaryE->operand->as<IdentifierExpr>()->name);
                }
            }
        }
        if (rs->expr->kind == ExprKind::Identifier) {
            auto* sym = symbols_.lookup(rs->expr->as<IdentifierExpr>()->name);
            if (sym) {
                sym->escapes = true;
                // P4-B: `return a` where `a` is heap-owning conceptually
                // transfers ownership out of the function. We deliberately
                // DO NOT flip `isMoved` here because Sema is path-
                // insensitive: marking moved on an early-return inside an
                // if-branch bleeds the state into the unaffected branch
                // (`if (...) return result; result.push(...);`), producing
                // cascades of false positives in the stdlib. Tracking
                // `sym->escapes` is sufficient for any lifetime / escape
                // analysis layered on top; the actual double-use
                // diagnostic on a normal return path is vacuous because
                // the function terminates.
            }
        }
    }
}

} // namespace vyx
