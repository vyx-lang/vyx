#include "Sema.h"
#include "SemaCloneHelpers.h"
#include "../Common/StringUtils.h"
#include <algorithm>
#include <functional>
#include <unordered_map>
#include <set>
#include <map>

namespace vyx {

// Defined in SemaGeneric.cpp. Evaluates IntLit / Identifier(env) / BinaryOp arith
// to an int64, returning false on any unsupported node or unbound name.
bool evalConstExprInt(const Expr* expr,
                      const std::map<std::string, int64_t>& env,
                      int64_t& out);

VyxTypePtr Sema::analyzeBinaryOp(Expr& expr) {
    auto* binExpr = expr.as<BinaryOpExpr>();

    if (binExpr->op == BinaryOp::MatchOp && binExpr->rhs &&
        binExpr->rhs->kind == ExprKind::Identifier &&
        binExpr->rhs->as<IdentifierExpr>()->name.starts_with("__typecheck_")) {
        if (binExpr->lhs) analyzeExpr(*binExpr->lhs);
        return types::makeBool();
    }

    if (!binExpr->lhs || !binExpr->rhs) {
        diag_.error(expr.location, "binary operator: missing left or right operand");
        return types::makeUnknown();
    }
    auto lhsType = analyzeExpr(*binExpr->lhs);
    auto rhsType = analyzeExpr(*binExpr->rhs);
    if (!lhsType || !rhsType) {
        diag_.error(expr.location, "binary operator: operand did not yield a type");
        return types::makeUnknown();
    }

    if (binExpr->op != BinaryOp::NullCoalesce && binExpr->op != BinaryOp::MatchOp) {
        if (lhsType && isOptionLike(*lhsType) &&
            binExpr->op != BinaryOp::Eq && binExpr->op != BinaryOp::Neq) {
            auto inner = optionInner(*lhsType);
            diag_.error(expr.location,
                "cannot use operator on nullable type '{}?'; unwrap or use '\?\?' first",
                inner ? inner->toString() : "?");
        }
        if (rhsType && isOptionLike(*rhsType) &&
            binExpr->op != BinaryOp::Eq && binExpr->op != BinaryOp::Neq) {
            auto inner = optionInner(*rhsType);
            diag_.error(expr.location,
                "cannot use operator on nullable type '{}?'; unwrap or use '\?\?' first",
                inner ? inner->toString() : "?");
        }
    }

    if (lhsType && (lhsType->kind == VyxTypeKind::Struct || lhsType->kind == VyxTypeKind::Class)) {
        static const std::unordered_map<int, std::string> opMethodNames = {
            {(int)BinaryOp::Add, "operator_add"}, {(int)BinaryOp::Sub, "operator_sub"},
            {(int)BinaryOp::Mul, "operator_mul"}, {(int)BinaryOp::Div, "operator_div"},
            {(int)BinaryOp::Mod, "operator_mod"},
            {(int)BinaryOp::Eq, "operator_eq"}, {(int)BinaryOp::Neq, "operator_neq"},
            {(int)BinaryOp::Lt, "operator_lt"}, {(int)BinaryOp::Gt, "operator_gt"},
            {(int)BinaryOp::Lte, "operator_lte"}, {(int)BinaryOp::Gte, "operator_gte"},
            {(int)BinaryOp::And, "operator_and"}, {(int)BinaryOp::Or, "operator_or"},
            {(int)BinaryOp::BitAnd, "operator_band"}, {(int)BinaryOp::BitOr, "operator_bor"},
            {(int)BinaryOp::BitXor, "operator_bxor"},
            {(int)BinaryOp::Shl, "operator_shl"}, {(int)BinaryOp::Shr, "operator_shr"},
        };
        auto opIt = opMethodNames.find((int)binExpr->op);
        if (opIt != opMethodNames.end()) {
            for (auto& method : lhsType->methods) {
                if (method.name == opIt->second && method.returnType) {
                    return method.returnType;
                }
            }
        }
    }

    switch (binExpr->op) {
        case BinaryOp::Add:
        case BinaryOp::Sub:
        case BinaryOp::Mul:
        case BinaryOp::Div:
        case BinaryOp::Mod:
            if (isStringType(*lhsType) && binExpr->op == BinaryOp::Add)
                return types::makeString();
            return commonType(*lhsType, *rhsType);

        case BinaryOp::Eq:
        case BinaryOp::Neq:
        case BinaryOp::Lt:
        case BinaryOp::Lte:
        case BinaryOp::Gt:
        case BinaryOp::Gte:
        case BinaryOp::MatchOp:
            return types::makeBool();

        case BinaryOp::And:
        case BinaryOp::Or:
            return types::makeBool();

        case BinaryOp::BitAnd:
        case BinaryOp::BitOr:
        case BinaryOp::BitXor:
        case BinaryOp::Shl:
        case BinaryOp::Shr:
            return lhsType;

        case BinaryOp::RangeOp:
            return types::makeRange();

        case BinaryOp::Pipe:
            if (binExpr->rhs) return analyzeExpr(*binExpr->rhs);
            return lhsType;

        case BinaryOp::NullCoalesce:
            if (lhsType && isOptionLike(*lhsType)) {
                auto inner = optionInner(*lhsType);
                if (inner) return inner;
            }
            return lhsType;
    }
    diag_.error(expr.location, "internal: unhandled binary operator ({})", static_cast<int>(binExpr->op));
    return types::makeUnknown();
}

VyxTypePtr Sema::analyzeUnaryOp(Expr& expr) {
    auto* unExpr = expr.as<UnaryOpExpr>();
    if (!unExpr->operand) {
        diag_.error(expr.location, "unary operator: missing operand");
        return types::makeUnknown();
    }
    auto operandType = analyzeExpr(*unExpr->operand);
    if (!operandType) {
        diag_.error(expr.location, "unary operator: operand did not yield a type");
        return types::makeUnknown();
    }

    switch (unExpr->op) {
        case UnaryOp::Neg:
        case UnaryOp::BitNot:
        case UnaryOp::PreInc:
        case UnaryOp::PreDec:
        case UnaryOp::PostInc:
        case UnaryOp::PostDec:
            return operandType;
        case UnaryOp::Not:
            return types::makeBool();
        case UnaryOp::Ref:
            if (unExpr->operand && unExpr->operand->kind == ExprKind::Identifier) {
                auto* sym = symbols_.lookup(unExpr->operand->as<IdentifierExpr>()->name);
                if (sym) {
                    if (sym->isMoved) {
                        diag_.error(expr.location, "cannot borrow moved value '{}'", unExpr->operand->as<IdentifierExpr>()->name);
                    } else if (sym->mutBorrowCount > 0) {
                        diag_.error(expr.location, "cannot take shared reference to '{}' while it has an active mutable borrow",
                            unExpr->operand->as<IdentifierExpr>()->name);
                    } else {
                        sym->borrowCount++;
                        sym->isBorrowed = true;
                        sym->borrowScopeDepth = symbols_.currentScopeDepth();
                        symbols_.trackBorrow(unExpr->operand->as<IdentifierExpr>()->name, symbols_.currentScopeDepth(), false);
                    }
                }
            }
            return types::makeReference(operandType, false);
        case UnaryOp::MutRef:
            if (unExpr->operand && unExpr->operand->kind == ExprKind::Identifier) {
                auto* sym = symbols_.lookup(unExpr->operand->as<IdentifierExpr>()->name);
                if (sym) {
                    // borrowCount is a total (shared + mut) bookkeeping
                    // counter; the shared-only count is `borrowCount -
                    // mutBorrowCount`. Prioritise the mut-collision
                    // diagnostic so a previous `&mut x` produces "second
                    // mutable reference" rather than "N shared borrow(s)".
                    size_t sharedOnly = sym->borrowCount >= sym->mutBorrowCount
                        ? sym->borrowCount - sym->mutBorrowCount : 0;
                    if (sym->isMoved) {
                        diag_.error(expr.location, "cannot take mutable reference to moved value '{}'",
                            unExpr->operand->as<IdentifierExpr>()->name);
                    } else if (sym->mutBorrowCount > 0) {
                        diag_.error(expr.location, "cannot take second mutable reference to '{}' — only one &mut allowed at a time",
                            unExpr->operand->as<IdentifierExpr>()->name);
                    } else if (sharedOnly > 0) {
                        diag_.error(expr.location, "cannot take mutable reference to '{}' while it has {} active shared borrow(s)",
                            unExpr->operand->as<IdentifierExpr>()->name, sharedOnly);
                    } else if (!sym->isMutable && !sym->isFunction) {
                        diag_.error(expr.location, "cannot take mutable reference to immutable variable '{}'",
                            unExpr->operand->as<IdentifierExpr>()->name);
                    } else {
                        sym->isBorrowed = true;
                        sym->borrowCount++;
                        sym->mutBorrowCount++;
                        sym->borrowScopeDepth = symbols_.currentScopeDepth();
                        symbols_.trackBorrow(unExpr->operand->as<IdentifierExpr>()->name, symbols_.currentScopeDepth(), true);
                    }
                }
            }
            return types::makeReference(operandType, true);
        case UnaryOp::Deref:
            if (operandType->kind == VyxTypeKind::RawPtr) {
                if (!inUnsafe_)
                    diag_.error(expr.location, "dereference of raw pointer requires 'unsafe' block");
                return types::makeUnknown();
            }
            if (operandType->kind == VyxTypeKind::Pointer) {
                if (!inUnsafe_)
                    diag_.error(expr.location, "dereference of pointer requires 'unsafe' block");
                if (!operandType->pointeeType) {
                    diag_.error(expr.location, "dereference: pointer has no pointee type");
                    return types::makeUnknown();
                }
                return operandType->pointeeType;
            }
            if (operandType->kind == VyxTypeKind::Reference) {
                if (!operandType->pointeeType) {
                    diag_.error(expr.location, "dereference: reference has no pointee type");
                    return types::makeUnknown();
                }
                return operandType->pointeeType;
            }
            diag_.error(expr.location, "cannot dereference non-pointer type '{}'", operandType->toString());
            return types::makeUnknown();
    }
    diag_.error(expr.location, "internal: unhandled unary operator ({})", static_cast<int>(unExpr->op));
    return types::makeUnknown();
}

VyxTypePtr Sema::analyzeCall(Expr& expr) {
    auto* callExpr = expr.as<CallExpr>();
    if (!callExpr->callee) {
        diag_.error(expr.location, "call expression has no callee");
        return types::makeUnknown();
    }

    // Desugar `Option::<T>.some(x)` / `Option::<T>.none()` /
    // `Result::<T,E>.ok(v)` / `Result::<T,E>.err(e)` into the prelude
    // constructor form (`Some(x)` / `None` / `Ok(v)` / `Err(e)`). The
    // static-factory spelling mirrors user expectations from languages
    // like Swift / Rust (`Option::Some(x)`) but Vyx represents these via
    // prelude-injected zero/one-arg symbols. Turbofish type args are
    // preserved on the rewritten identifier so CodeGen can still see
    // payload ABI-sensitive types such as fn(...) fat handles.
    if (callExpr->callee->kind == ExprKind::MemberAccess) {
        auto* ma = callExpr->callee->as<MemberAccessExpr>();
        if (ma->object && ma->object->kind == ExprKind::Identifier) {
            const auto& baseName = ma->object->as<IdentifierExpr>()->name;
            const char* ctor = nullptr;
            // R5 stage 2: lang-item registry is authoritative — no
            // bootstrap-era string fallback. A user-defined
            // `@[lang_item("option")] enum Maybe<T>` gets
            // `Maybe::<T>.some(x)` desugared the same way.
            auto slotNameIs = [&](const char* slot) {
                const Decl* l = langItems_.find(slot);
                return l && baseName == l->name;
            };
            if (slotNameIs("option")) {
                if (ma->member == "some") ctor = "Some";
                else if (ma->member == "none") ctor = "None";
            } else if (slotNameIs("result")) {
                if (ma->member == "ok") ctor = "Ok";
                else if (ma->member == "err") ctor = "Err";
            }
            if (ctor) {
                auto id = std::make_unique<IdentifierExpr>();
                id->location = ma->location;
                id->name = ctor;
                auto* origObj = ma->object->as<IdentifierExpr>();
                for (auto& ta : origObj->callTypeArgs)
                    id->callTypeArgs.push_back(ta ? cloneTypeAnnotation(*ta) : nullptr);
                callExpr->callee = std::move(id);
            }
            // User-defined ADT variant constructor: `Either::<i32, string>.Left(42)`
            // or `Either.Left(42)`. Rewrite to `Either::Left(42)` (Identifier
            // with `::` separator) so the existing ADT-constructor path in
            // analyzeCall's Identifier branch + CodeGen's emitBuiltinAdt
            // handle it uniformly. Turbofish args are consumed; the concrete
            // ADT struct gets registered on-demand by CodeGen's toLLVMType
            // (see CodeGenTypeMap's generic-ErrorDef branch).
            if (!ctor) {
                auto typeRef = symbols_.lookupType(baseName);
                if (typeRef && typeRef->kind == VyxTypeKind::ErrorType) {
                    // Verify the member is actually a variant name of this
                    // enum. Fall back to the generic member-access path
                    // (static methods on the ADT) when it isn't.
                    bool isVariant = false;
                    if (unit_) {
                        for (auto& d : unit_->declarations) {
                            if (!d || d->kind != DeclKind::ErrorDef) continue;
                            if (d->name != baseName) continue;
                            auto* ed = d->as<ErrorDefDecl>();
                            for (auto& v : ed->variants) {
                                if (v == ma->member) { isVariant = true; break; }
                            }
                            break;
                        }
                    }
                    if (isVariant) {
                        auto id = std::make_unique<IdentifierExpr>();
                        id->location = ma->location;
                        id->name = baseName + "::" + ma->member;
                        // Carry the turbofish args through onto the new
                        // Identifier so CodeGen's ADT builtin can look up
                        // the concrete mangled struct (`Either<i32,string>`).
                        auto* origObj = ma->object->as<IdentifierExpr>();
                        for (auto& ta : origObj->callTypeArgs)
                            id->callTypeArgs.push_back(ta ? cloneTypeAnnotation(*ta) : nullptr);
                        callExpr->callee = std::move(id);
                    }
                }
            }
        }
    }

    // Method-generic type-arg inference: if the user wrote `r.map(closure)`
    // on an Option/Result receiver without an explicit turbofish, pull the
    // closure's explicit return-type annotation and fold it into the
    // MemberAccess's callTypeArgs so Mono can instantiate the correct
    // `Result<T,E>.map<U>` / `Option<T>.map<U>`. Scoped to the simple
    // single-U pattern — not a full inference engine.
    if (callExpr->callee->kind == ExprKind::MemberAccess) {
        auto* ma = callExpr->callee->as<MemberAccessExpr>();
        if (ma->callTypeArgs.empty() && callExpr->args.size() == 1 &&
            callExpr->args[0] && callExpr->args[0]->kind == ExprKind::Closure) {
            // Only fire for well-known one-arg single-U method-generics
            // on Option/Result (map, filter don't change U for filter —
            // skip filter; map is the primary case).
            static const std::set<std::string> kSingleUMethods = {
                "map", "and_then", "andThen", "or_else", "orElse", "map_err", "mapErr"
            };
            if (kSingleUMethods.count(ma->member)) {
                auto* closure = callExpr->args[0]->as<ClosureExpr>();
                if (closure->returnType) {
                    ma->callTypeArgs.push_back(cloneTypeAnnotation(*closure->returnType));
                }
            }
        }
    }

    if (callExpr->callee->kind == ExprKind::Identifier) {
        auto& name = callExpr->callee->as<IdentifierExpr>()->name;
        if (!inUnsafe_ && (name == "alloc" || name == "dealloc" || name == "transmute")) {
            diag_.error(expr.location, "call to '{}' requires 'unsafe' block", name);
        }
        auto* calleeSym = symbols_.lookup(name);
        if (!inUnsafe_ && calleeSym && calleeSym->isUnsafe) {
            diag_.error(expr.location, "call to unsafe function '{}' requires 'unsafe' block", name);
        }
    }

    // Named parameter reordering
    bool hasNamedArgs = !callExpr->argNames.empty() &&
        std::any_of(callExpr->argNames.begin(), callExpr->argNames.end(),
            [](const std::string& n) { return !n.empty(); });

    if (hasNamedArgs && callExpr->callee->kind == ExprKind::Identifier) {
        const Decl* targetDecl = nullptr;
        if (unit_) {
            for (auto& d : unit_->declarations) {
                if (d && d->kind == DeclKind::Function &&
                    d->name == callExpr->callee->as<IdentifierExpr>()->name) {
                    targetDecl = d.get();
                    break;
                }
            }
        }
        if (targetDecl) {
            auto* fnDecl = targetDecl->as<FunctionDecl>();
            if (!fnDecl->params.empty()) {
                size_t paramCount = fnDecl->params.size();
                std::vector<ExprPtr> reordered(paramCount);
                std::vector<bool> filled(paramCount, false);

                for (size_t i = 0; i < callExpr->args.size() && i < callExpr->argNames.size(); ++i) {
                    if (!callExpr->argNames[i].empty()) {
                        for (size_t pi = 0; pi < paramCount; ++pi) {
                            if (fnDecl->params[pi].name == callExpr->argNames[i]) {
                                reordered[pi] = std::move(callExpr->args[i]);
                                filled[pi] = true;
                                break;
                            }
                        }
                    } else {
                        for (size_t pi = 0; pi < paramCount; ++pi) {
                            if (!filled[pi]) {
                                reordered[pi] = std::move(callExpr->args[i]);
                                filled[pi] = true;
                                break;
                            }
                        }
                    }
                }
                for (size_t pi = 0; pi < paramCount; ++pi) {
                    if (!filled[pi] && fnDecl->params[pi].defaultValue) {
                        reordered[pi] = cloneExpr(*fnDecl->params[pi].defaultValue);
                        filled[pi] = true;
                    }
                }
                callExpr->args = std::move(reordered);
                callExpr->argNames.clear();
            }
        }
    }

    // Analyze arguments and collect types
    std::vector<VyxTypePtr> analyzedArgTypes;
    for (size_t ai = 0; ai < callExpr->args.size(); ++ai) {
        auto& arg = callExpr->args[ai];
        if (!arg) {
            diag_.error(expr.location, "call: missing argument at position {}", ai);
            analyzedArgTypes.push_back(types::makeUnknown());
            continue;
        }
        auto argType = analyzeExpr(*arg);
        analyzedArgTypes.push_back(argType ? argType : types::makeUnknown());
        if (!argType) {
            diag_.error(expr.location, "call: argument {} did not yield a type", ai);
        }
        if (arg->kind == ExprKind::Identifier) {
            auto* idArg = arg->as<IdentifierExpr>();
            if (!idArg->typeAnnotation && argType && argType->kind != VyxTypeKind::Unknown) {
                idArg->typeAnnotation = convertTypeToAnnotation(argType);
            }
        }
    }

    if (callExpr->callee->kind == ExprKind::Identifier) {
        auto* idCallee = callExpr->callee->as<IdentifierExpr>();
        auto resolveCallTypeArg = [&](size_t idx) -> VyxTypePtr {
            if (idx >= idCallee->callTypeArgs.size() || !idCallee->callTypeArgs[idx])
                return nullptr;
            return resolveType(*idCallee->callTypeArgs[idx]);
        };
        if (idCallee->name == "Some") {
            auto inner = resolveCallTypeArg(0);
            if (inner && inner->kind != VyxTypeKind::Unknown)
                return types::makeOptional(inner);
        } else if (idCallee->name == "None") {
            auto inner = resolveCallTypeArg(0);
            if (inner && inner->kind != VyxTypeKind::Unknown)
                return types::makeOptional(inner);
        } else if (idCallee->name == "Ok") {
            auto ok = resolveCallTypeArg(0);
            auto err = resolveCallTypeArg(1);
            if (ok && ok->kind != VyxTypeKind::Unknown &&
                err && err->kind != VyxTypeKind::Unknown)
                return types::makeResult(ok, err);
        } else if (idCallee->name == "Err") {
            auto ok = resolveCallTypeArg(0);
            auto err = resolveCallTypeArg(1);
            if (ok && ok->kind != VyxTypeKind::Unknown &&
                err && err->kind != VyxTypeKind::Unknown)
                return types::makeResult(ok, err);
        }
    }

    // ── P4-B: mark heap-owning by-value arguments moved ───────────────────
    // `foo(a)` where `a`'s type is heap-owning AND the callee's param at
    // position `ai` is NOT `&T` / `&mut T` transfers ownership into the
    // callee. We restrict the scope to free-function calls with a resolved
    // callee symbol — method calls (MemberAccess callee) have too many
    // dispatch/generic caveats to analyse cheaply here, and would produce
    // false positives on e.g. `a.clone()` / `a.len()`.
    // Escape hatch: if the arg expression is itself a Call whose callee is
    // `.clone()`, there is no local name to mark, so this naturally skips.
    if (callExpr->callee->kind == ExprKind::Identifier) {
        auto* idCalleePeek = callExpr->callee->as<IdentifierExpr>();
        auto* calleeSymPeek = symbols_.lookup(idCalleePeek->name);
        if (calleeSymPeek && calleeSymPeek->isFunction && !calleeSymPeek->borrowsArgs) {
            const auto& fnParams = calleeSymPeek->paramTypes;
            for (size_t ai = 0; ai < callExpr->args.size(); ++ai) {
                auto& arg = callExpr->args[ai];
                if (!arg || arg->kind != ExprKind::Identifier) continue;
                auto argType = ai < analyzedArgTypes.size() ? analyzedArgTypes[ai] : nullptr;
                if (!argType || !isHeapOwningType(*argType)) continue;
                // Skip if the parameter is a reference — borrow, not move.
                if (ai < fnParams.size() && fnParams[ai] &&
                    fnParams[ai]->kind == VyxTypeKind::Reference) {
                    continue;
                }
                auto* srcSym = symbols_.lookup(arg->as<IdentifierExpr>()->name);
                if (srcSym && !isStringType(*argType) &&
                    (!srcSym->isConst || isMoveOnlyHeapOwningType(*argType))) {
                    srcSym->isMoved = true;
                    srcSym->moveLocation = arg->location;
                }
            }
        }
    }

    // Method-call counterpart of the move checker above. The previous
    // implementation intentionally skipped MemberAccess callees to avoid
    // false positives on `a.clone()`; however that also let real by-value
    // method transfers leak through (e.g. `c.push_many(src, ...)`), causing
    // both caller and callee bindings to auto-drop the same heap payload.
    //
    // Rules:
    //   * If method signature includes an explicit `self` parameter and it is
    //     by-value, mark the receiver identifier moved.
    //   * For regular call arguments, mark heap-owning identifier args moved
    //     when the corresponding parameter is by-value (non-reference).
    //   * Keep `.clone()` as an escape hatch: receiver is never move-marked
    //     for clone-like methods even when the signature looks by-value.
    if (callExpr->callee->kind == ExprKind::MemberAccess) {
        auto* ma = callExpr->callee->as<MemberAccessExpr>();
        VyxTypePtr objType;
        if (ma && ma->object) {
            if (ma->object->kind == ExprKind::Identifier) {
                const auto& objectName = ma->object->as<IdentifierExpr>()->name;
                if (activeGenericParams_.contains(objectName)) {
                    // A generic parameter in `T::method()` is a static receiver,
                    // not a value expression. Keep the move checker from
                    // diagnosing it as an undefined local before normal member
                    // analysis handles the call.
                    objType = types::makeGeneric(objectName);
                }
            }
            if (!objType) objType = analyzeExpr(*ma->object);
        }
        if (ma && objType) {
            std::vector<VyxTypePtr> methodParams;
            size_t chosenParamCount = 0;
            for (auto& method : objType->methods) {
                if (method.name != ma->member) continue;
                // Prefer exact arity first, then explicit-self form.
                if (method.paramTypes.size() == callExpr->args.size() ||
                    method.paramTypes.size() == callExpr->args.size() + 1) {
                    methodParams = method.paramTypes;
                    chosenParamCount = method.paramTypes.size();
                    if (method.paramTypes.size() == callExpr->args.size()) break;
                } else if (methodParams.empty()) {
                    methodParams = method.paramTypes;
                    chosenParamCount = method.paramTypes.size();
                }
            }

            bool hasExplicitSelf =
                !methodParams.empty() && chosenParamCount == callExpr->args.size() + 1;
            auto isCloneLike = [](const std::string& member) {
                return member == "clone" || member == "clone_deep";
            };
            auto isBorrowLikeStaticFactory = [&](const MemberAccessExpr* memberAccess) {
                if (!memberAccess || !memberAccess->object ||
                    memberAccess->object->kind != ExprKind::Identifier) {
                    return false;
                }
                const std::string& clsName = memberAccess->object->as<IdentifierExpr>()->name;
                // Legacy hardcode for `Weak.of(...)` retained for backward
                // compatibility — Weak's of() arrives without the
                // `@[borrow_args]` attribute on its AST node.
                if (memberAccess->member == "of" && clsName == "Weak") return true;
                // BUG-LV-19 fix — honour `@[borrow_args]` on static-method
                // factories (e.g. `Slice::<T>.from_vec(v)` borrows `v` rather
                // than moving it).  Without this, V-MOVE-001 fires the
                // moment the caller writes `let s = Slice.from_vec(v); v.len();`.
                if (!unit_) return false;
                auto baseName = [](const std::string& n) {
                    auto lt = n.find('<');
                    return lt == std::string::npos ? n : n.substr(0, lt);
                };
                std::string targetClassBase = baseName(clsName);
                for (auto& d : unit_->declarations) {
                    if (!d || d->kind != DeclKind::Class) continue;
                    if (baseName(d->name) != targetClassBase) continue;
                    auto* cd = d->as<const ClassDecl>();
                    for (auto& m : cd->methods) {
                        if (m.name != memberAccess->member) continue;
                        for (auto& [an, av] : m.attributes) {
                            if (an == "borrow_args") return true;
                        }
                    }
                }
                return false;
            };

            if (hasExplicitSelf &&
                ma->object && ma->object->kind == ExprKind::Identifier &&
                !isCloneLike(ma->member) &&
                isHeapOwningType(*objType)) {
                auto* selfParam = methodParams[0].get();
                bool selfByRef = selfParam && selfParam->kind == VyxTypeKind::Reference;
                if (!selfByRef) {
                    auto* recvSym = symbols_.lookup(ma->object->as<IdentifierExpr>()->name);
                    if (recvSym && !isStringType(*objType) &&
                        (!recvSym->isConst || isMoveOnlyHeapOwningType(*objType))) {
                        recvSym->isMoved = true;
                        recvSym->moveLocation = ma->object->location;
                    }
                }
            }

            size_t base = hasExplicitSelf ? 1 : 0;
            for (size_t ai = 0; ai < callExpr->args.size(); ++ai) {
                auto& arg = callExpr->args[ai];
                if (!arg || arg->kind != ExprKind::Identifier) continue;
                if (isBorrowLikeStaticFactory(ma)) continue;
                auto argType = ai < analyzedArgTypes.size() ? analyzedArgTypes[ai] : nullptr;
                if (!argType || !isHeapOwningType(*argType)) continue;
                size_t pi = ai + base;
                if (pi < methodParams.size() && methodParams[pi] &&
                    methodParams[pi]->kind == VyxTypeKind::Reference) {
                    continue;
                }
                auto* srcSym = symbols_.lookup(arg->as<IdentifierExpr>()->name);
                if (srcSym && !isStringType(*argType) &&
                    (!srcSym->isConst || isMoveOnlyHeapOwningType(*argType))) {
                    srcSym->isMoved = true;
                    srcSym->moveLocation = arg->location;
                }
            }
        }
    }

    if (callExpr->callee->kind == ExprKind::Identifier &&
        (callExpr->callee->as<IdentifierExpr>()->name == "spawn" ||
         callExpr->callee->as<IdentifierExpr>()->name == "startThread")) {
        for (auto& arg : callExpr->args) {
            if (!arg) continue;
            if (arg->kind == ExprKind::Closure) {
                auto* closureArg = arg->as<ClosureExpr>();
                for (auto& cap : closureArg->captures) {
                    auto* capSym = symbols_.lookup(cap.name);
                    if (!capSym || !capSym->type) continue;
                    if (isRefLike(*capSym->type)) {
                        auto inner = refInner(*capSym->type);
                        diag_.warning(expr.location,
                            "capturing Ref<{}> in {} — Ref<T> is not Send; use Channel or Mutex for thread safety",
                            inner ? inner->toString() : "?",
                            callExpr->callee->as<IdentifierExpr>()->name);
                    }
                    if (cap.byRef) {
                        diag_.error(expr.location,
                            "capturing '{}' by reference in {} is unsafe — use 'move' capture or Channel<T>",
                            cap.name, callExpr->callee->as<IdentifierExpr>()->name);
                    }
                }
            }
        }
    }

    if (callExpr->callee->kind == ExprKind::Identifier) {
        auto* idCallee = callExpr->callee->as<IdentifierExpr>();
        auto aliasIt = functionAliases_.find(idCallee->name);
        if (aliasIt != functionAliases_.end()) {
            idCallee->name = aliasIt->second;
        }

        // Phase 9: free function overload resolution. Skip when the group has
        // exactly one candidate — the legacy single-symbol dispatch is more
        // permissive about argument-type checking (it does NO arity / type
        // matching at this stage; the body re-checks via implicit
        // conversions during emit), and forcing strict checking here would
        // regress every callsite whose arg type Sema can't fully infer
        // (e.g. local `var` with deferred type, generic instantiations
        // touched mid-flight). Overload selection is what we actually need
        // when the group has > 1 distinct entries.
        //
        // Phase 9b: when a mixed group has both non-generic AND generic
        // siblings, the non-generic claims the unmangled slot in `symbols_`.
        // A call like `min::<i32>(5, 7)` would find the non-generic sym,
        // skip the `sym->isGeneric` branch, and produce a codegen-time
        // arity mismatch. We now re-examine the overload group in the
        // turbofish case too, preferring the generic sibling whose arity
        // matches the turbofish's type-arg count. Similarly, when overload
        // resolution picks a generic winner for an un-turbofish'd call,
        // we capture the decl in `pickedGenericOverloadDecl` and route
        // through the same template instantiation path below.
        const Decl* pickedGenericOverloadDecl = nullptr;
        bool hasMultipleOverloads = false;
        {
            auto ogPeek = overloadGroups_.find(idCallee->name);
            if (ogPeek != overloadGroups_.end() && ogPeek->second.size() > 1) {
                hasMultipleOverloads = true;
            }
        }
        if (hasMultipleOverloads && !idCallee->callTypeArgs.empty()) {
            // Turbofish on a mixed overload set: pick the generic sibling
            // whose generic-param arity matches the turbofish. The non-
            // generic entries cannot consume turbofish type-args (their
            // genericParams is empty) so they're never the right target.
            auto ogIt = overloadGroups_.find(idCallee->name);
            if (ogIt != overloadGroups_.end()) {
                for (auto& cand : ogIt->second) {
                    if (!cand.isGeneric || !cand.decl) continue;
                    if (cand.decl->genericParams.size() != idCallee->callTypeArgs.size())
                        continue;
                    pickedGenericOverloadDecl = cand.decl;
                    break;
                }
            }
        }
        if (hasMultipleOverloads && idCallee->callTypeArgs.empty()) {
            auto ogIt = overloadGroups_.find(idCallee->name);
            if (ogIt != overloadGroups_.end() && !ogIt->second.empty()) {
                auto& candidates = ogIt->second;
                // Score every candidate vs. analyzedArgTypes. Scoring rule:
                //   exact-match params  → +2 each
                //   implicit-conversion → +1 each
                //   generic-placeholder → +0 (still a match)
                //   no-match            → candidate dropped
                // Non-generic candidates outrank generic siblings unless
                // every non-generic was filtered. Highest total wins;
                // ties → ambiguous-overload error.
                struct Scored {
                    const OverloadEntry* entry;
                    int score;
                    bool isGeneric;
                };
                std::vector<Scored> nonGen, gen;
                auto isImplicitlyConvertible = [&](const VyxType& from, const VyxType& to) -> bool {
                    // Identical: handled by exact-match. We only get here for
                    // the "+1 not +2" case. Numeric conversions are allowed
                    // here with the same broad semantics as ordinary call-site
                    // casts: exact overloads still win, but a lone i32
                    // overload may accept an unsuffixed i64 literal/value and
                    // downstream checking can warn for narrowing.
                    if (from.kind == VyxTypeKind::Integer && to.kind == VyxTypeKind::Integer) {
                        if (from.isSigned == to.isSigned) return true;
                    }
                    if (from.kind == VyxTypeKind::Float && to.kind == VyxTypeKind::Float) {
                        if (from.bitWidth <= to.bitWidth) return true;
                    }
                    if (from.kind == VyxTypeKind::Integer && to.kind == VyxTypeKind::Float) {
                        return true;
                    }
                    return false;
                };
                for (auto& cand : candidates) {
                    if (cand.paramTypes.size() != analyzedArgTypes.size()) {
                        // Variadic-aware match: a generic candidate whose last
                        // param is a sugar-variadic pack may accept any arg
                        // count. Conservative: skip arity-mismatched non-
                        // generic; treat generic as still a candidate when its
                        // param count matches or differs.
                        if (!cand.isGeneric) continue;
                    }
                    int score = 0;
                    bool viable = true;
                    size_t cmpN = std::min(cand.paramTypes.size(), analyzedArgTypes.size());
                    for (size_t i = 0; i < cmpN; ++i) {
                        const auto& argT = analyzedArgTypes[i];
                        const auto& parT = cand.paramTypes[i];
                        if (!argT || !parT) { score += 0; continue; }
                        if (parT->kind == VyxTypeKind::Unknown) {
                            // Generic placeholder.
                            score += 0;
                            continue;
                        }
                        if (argT->isEqual(*parT)) {
                            score += 2;
                        } else if (isImplicitlyConvertible(*argT, *parT)) {
                            score += 1;
                        } else {
                            viable = false;
                            break;
                        }
                    }
                    if (!viable) continue;
                    Scored s{&cand, score, cand.isGeneric};
                    if (cand.isGeneric) gen.push_back(s);
                    else nonGen.push_back(s);
                }
                std::vector<Scored>& pool = !nonGen.empty() ? nonGen : gen;
                if (!pool.empty()) {
                    std::sort(pool.begin(), pool.end(),
                        [](const Scored& a, const Scored& b) { return a.score > b.score; });
                    if (pool.size() >= 2 && pool[0].score == pool[1].score) {
                        // Ambiguous — emit a clear error listing the
                        // top-scoring candidates.
                        std::string msg = "ambiguous call to '" + idCallee->name +
                            "' — multiple overloads match equally well:";
                        for (auto& c : pool) {
                            if (c.score != pool[0].score) break;
                            msg += "\n  candidate: " + idCallee->name +
                                typeSignatureString(c.entry->paramTypes);
                        }
                        diag_.error(expr.location, "{}", msg);
                        return types::makeUnknown();
                    }
                    const OverloadEntry* picked = pool[0].entry;
                    if (picked->isGeneric && picked->decl) {
                        // Generic winner: capture the decl and route through
                        // the template-instantiation path below. Do NOT
                        // rewrite idCallee->name — the generic decl keeps
                        // its unmangled name (the monomorph's mangled name
                        // gets assigned during instantiation below).
                        pickedGenericOverloadDecl = picked->decl;
                    } else if (!picked->mangledName.empty()) {
                        // Rewrite the callee identifier to the picked
                        // non-generic overload's mangled symbol. CodeGen
                        // looks up `idCallee->name` in functions_, which
                        // now points at the unique symbol.
                        idCallee->name = picked->mangledName;
                    }
                } else if (!candidates.empty()) {
                    // No viable overload at all → emit a helpful diagnostic
                    // listing every candidate. Mirrors C++'s "no matching
                    // function for call" pattern.
                    std::string msg = "no matching overload for call to '" +
                        idCallee->name + "' with argument types " +
                        typeSignatureString(analyzedArgTypes) + "; candidates:";
                    for (auto& c : candidates) {
                        msg += "\n  " + idCallee->name +
                            typeSignatureString(c.paramTypes) +
                            (c.isGeneric ? " [generic]" : "");
                    }
                    diag_.error(expr.location, "{}", msg);
                    return types::makeUnknown();
                }
            }
        }

        auto* sym = symbols_.lookup(idCallee->name);
        if (sym && sym->isImported && !sym->isExported && !inImportedScope_) {
            diag_.error(expr.location,
                "'{}' is private in its module; mark it 'public' to use from other modules",
                idCallee->name);
            return types::makeUnknown();
        }
        if (sym && ambiguousFunctions_.count(idCallee->name)) {
            auto modIt = functionSourceModule_.find(idCallee->name);
            diag_.error(expr.location,
                "'{}' exists in multiple imported modules; use qualified name (e.g., {}::{})",
                idCallee->name,
                modIt != functionSourceModule_.end() ? modIt->second : "module",
                idCallee->name);
            return types::makeUnknown();
        }
        if (!sym) {
            auto typeRef = symbols_.lookupType(idCallee->name);
            if (typeRef && (typeRef->kind == VyxTypeKind::Class || typeRef->kind == VyxTypeKind::Struct)) {
                return typeRef;
            }
            auto& calleeName = idCallee->name;
            auto sep = calleeName.find("::");
            if (sep != std::string::npos) {
                std::string enumName = calleeName.substr(0, sep);
                auto enumType = symbols_.lookupType(enumName);
                if (enumType && enumType->kind == VyxTypeKind::ErrorType) {
                    return enumType;
                }
                std::string modulePrefix = calleeName.substr(0, sep);
                std::string unqualified = calleeName.substr(sep + 2);
                auto* unqualSym = symbols_.lookup(unqualified);
                if (unqualSym) {
                    auto modIt = functionSourceModule_.find(unqualified);
                    if (modIt != functionSourceModule_.end() && modIt->second != modulePrefix) {
                        diag_.error(expr.location,
                            "'{}' belongs to module '{}', not '{}'",
                            unqualified, modIt->second, modulePrefix);
                        return types::makeUnknown();
                    }
                    idCallee->name = unqualified;
                    return analyzeCall(expr);
                }
            }
            std::string suggestion;
            int bestDist = 4;
            if (unit_) {
                for (auto& d : unit_->declarations) {
                    if (d && d->kind == DeclKind::Function) {
                        int dist = levenshteinDistance(idCallee->name, d->name);
                        if (dist < bestDist) { bestDist = dist; suggestion = d->name; }
                    }
                }
            }
            if (!suggestion.empty()) {
                diag_.errorWithFix(expr.location,
                    "replace with '" + suggestion + "'",
                    "undefined function '{}', did you mean '{}'?",
                    idCallee->name, suggestion);
            } else {
                diag_.error(expr.location, "undefined function '{}'", idCallee->name);
            }
            return types::makeUnknown();
        }

        if (unit_) {
            for (auto& d : unit_->declarations) {
                if (d && d->kind == DeclKind::Function && d->name == idCallee->name) {
                    for (auto& [an, av] : d->attributes) {
                        if (an == "deprecated") {
                            std::string msg = av.empty()
                                ? "function '" + d->name + "' is deprecated"
                                : "function '" + d->name + "' is deprecated: " + av;
                            diag_.warning(expr.location, "{}", msg);
                        }
                    }
                    break;
                }
            }
        }
        
        if ((sym->isFunction && sym->isGeneric) || pickedGenericOverloadDecl) {
            const Decl* genericDeclPtr = pickedGenericOverloadDecl;
            if (!genericDeclPtr) {
                for (size_t di = 0; di < unit_->declarations.size(); ++di) {
                    auto& d = unit_->declarations[di];
                    if (d && d->kind == DeclKind::Function &&
                        d->name == sym->name && !d->genericParams.empty()) {
                        genericDeclPtr = d.get();
                        break;
                    }
                }
            }

            if (genericDeclPtr) {
                auto* genFnDecl = genericDeclPtr->as<FunctionDecl>();
                std::vector<VyxTypePtr> argTypes;
                // Turbofish-derived generic-param bindings used SOLELY for
                // mangling. Inference and constraint-checking continue to
                // consume `argTypes` (param-substituted form) unchanged.
                std::vector<VyxTypePtr> mangleArgs;
                bool hasMangleOverride = false;
                if (!idCallee->callTypeArgs.empty() &&
                    idCallee->callTypeArgs.size() == genericDeclPtr->genericParams.size()) {
                    std::map<std::string, VyxTypePtr> typeMap;
                    for (size_t i = 0; i < genericDeclPtr->genericParams.size(); ++i) {
                        // Null slot: const-param position (e.g. `Array::<i32, 16>` where `16`
                        // is in callArgExprs, not callTypeArgs). Skip — type map only needs
                        // type bindings; const bindings are forwarded through resolveType's
                        // Generic annotation path.
                        if (!idCallee->callTypeArgs[i]) continue;
                        typeMap[genericDeclPtr->genericParams[i]] = resolveType(*idCallee->callTypeArgs[i]);
                    }
                    // Build mangleArgs from turbofish bindings directly so the
                    // instantiation key reflects T → i64 (not the substituted
                    // parameter type which would yield `<Container>` for
                    // `fn f<T>(c: Container<T>)`).
                    for (size_t i = 0; i < genericDeclPtr->genericParams.size(); ++i) {
                        if (idCallee->callTypeArgs[i]) {
                            mangleArgs.push_back(resolveType(*idCallee->callTypeArgs[i]));
                            hasMangleOverride = true;
                        } else {
                            mangleArgs.push_back(types::makeUnknown());
                        }
                    }
                    // Push the template's generic params so resolveType on
                    // compound annotations (e.g. `fn(T) -> T`, `Option<T>`)
                    // treats T as Generic rather than "undefined type". The
                    // fast `typeMap` lookup above handles bare `T`; the
                    // resolveType fallback handles wrappers.
                    std::vector<std::string> pushedGenericParams;
                    pushedGenericParams.reserve(genericDeclPtr->genericParams.size());
                    for (auto& gp : genericDeclPtr->genericParams) {
                        if (activeGenericParams_.insert(gp).second)
                            pushedGenericParams.push_back(gp);
                    }
                    // Sugar-form variadic: `fn foo<T>(args: ...T)` has ONE
                    // template param (`...args`) but the actual call passes
                    // N args. We need `argTypes.size() == N` so that
                    // `createVariadicInstance` synthesizes N concrete params
                    // (`args_0`, `args_1`, ...). Detect the sugar-variadic
                    // param by its name prefix (`...`) and expand the
                    // homogeneous pack using the turbofish-substituted
                    // element type, one per remaining actual call arg.
                    size_t callArgIdx = 0;
                    size_t nCallArgs = analyzedArgTypes.size();
                    for (auto& p : genFnDecl->params) {
                        bool isVariadicSugarParam =
                            p.name.size() > 3 && p.name.substr(0, 3) == "...";
                        if (isVariadicSugarParam && p.type) {
                            // Substituted element type (T -> i64 via typeMap).
                            VyxTypePtr elemTy;
                            auto it = typeMap.find(p.type->name);
                            if (it != typeMap.end()) elemTy = it->second;
                            else elemTy = resolveType(*p.type);
                            // Expand: one entry per remaining call arg. This
                            // drives createVariadicInstance's per-arg
                            // parameter synthesis (args_0..args_{N-1}) so the
                            // CodeGen forward-decl and the call site agree on
                            // arity.
                            while (callArgIdx < nCallArgs) {
                                argTypes.push_back(elemTy);
                                ++callArgIdx;
                            }
                        } else if (p.type) {
                            auto it = typeMap.find(p.type->name);
                            if (it != typeMap.end()) argTypes.push_back(it->second);
                            else argTypes.push_back(resolveType(*p.type));
                            if (callArgIdx < nCallArgs) ++callArgIdx;
                        } else {
                            argTypes.push_back(types::makeUnknown());
                            if (callArgIdx < nCallArgs) ++callArgIdx;
                        }
                    }
                    for (auto& gp : pushedGenericParams)
                        activeGenericParams_.erase(gp);
                } else {
                    argTypes = analyzedArgTypes;
                }

                std::string mangledName = TemplateResolver::mangleTemplateName(
                    genericDeclPtr->name, hasMangleOverride ? mangleArgs : argTypes);
                // Suffix with const-arg values so distinct instantiations of
                // the same generic with different non-type args get distinct
                // monomorphs — e.g. `capacity<i64>#8` vs `capacity<i64>#4`.
                if (!idCallee->callArgExprs.empty() && !genericDeclPtr->genericConstParams.empty()) {
                    std::map<std::string, int64_t> emptyEnvM;
                    for (size_t i = 0; i < idCallee->callArgExprs.size(); ++i) {
                        auto& ae = idCallee->callArgExprs[i];
                        int64_t cv = 0;
                        if (ae && evalConstExprInt(ae.get(), emptyEnvM, cv)) {
                            mangledName += "#" + std::to_string(cv);
                        }
                    }
                }

                if (instantiatedReturnTypes_.find(mangledName) == instantiatedReturnTypes_.end()) {
                    instantiateTemplateIfNeeded(expr, genericDeclPtr, argTypes,
                        hasMangleOverride ? &mangleArgs : nullptr);
                }

                idCallee->name = mangledName;

                auto retIt = instantiatedReturnTypes_.find(mangledName);
                if (retIt != instantiatedReturnTypes_.end()) {
                    return retIt->second;
                }
                return types::makeUnknown();
            }
        }

        if (sym->isFunction && sym->returnType) {
            return sym->returnType;
        }
        if (sym->type && sym->type->kind == VyxTypeKind::Function && sym->type->returnType) {
            return sym->type->returnType;
        }
        return sym->type ? sym->type : types::makeUnknown();
    }

    if (callExpr->callee->kind == ExprKind::MemberAccess) {
        auto memberType = analyzeMemberAccess(*callExpr->callee);
        return memberType;
    }

    // Generic fallback: any other expression can be a callee as long as it
    // yields a value of function type. Covers `pipe.get(i)(x)`, closure
    // returned from another call, `(fn() -> T { ... })()` immediate invoke,
    // etc. Analyze the callee, and if it has a function type, return the
    // function's return type; otherwise treat as Unknown so downstream
    // CodeGen can try to dispatch.
    auto calleeType = analyzeExpr(*callExpr->callee);
    if (calleeType && calleeType->kind == VyxTypeKind::Function && calleeType->returnType) {
        return calleeType->returnType;
    }
    return types::makeUnknown();
}

VyxTypePtr Sema::analyzeMemberAccess(Expr& expr) {
    auto* maExpr = expr.as<MemberAccessExpr>();
    if (!maExpr->object) {
        diag_.error(expr.location, "member access: missing object expression");
        return types::makeUnknown();
    }
    VyxTypePtr objType;
    if (maExpr->object->kind == ExprKind::Identifier) {
        const auto& objectName = maExpr->object->as<IdentifierExpr>()->name;
        if (activeGenericParams_.contains(objectName)) {
            // `T::method()` uses a type parameter as a static-method receiver,
            // not as a runtime value. Preserve that distinction so it does not
            // fall through Identifier analysis as an undefined symbol.
            objType = types::makeGeneric(objectName);
        }
    }
    if (!objType) objType = analyzeExpr(*maExpr->object);
    if (!objType) {
        diag_.error(expr.location, "member access: object expression did not yield a type");
        return types::makeUnknown();
    }
    auto smartPtrInnerForMember = [&](const VyxType& t) -> VyxTypePtr {
        if (auto inner = smartPtrInner(t)) return inner;
        if (!isSmartPtrLike(t)) return nullptr;
        auto lt = t.name.find('<');
        if (lt == std::string::npos || t.name.empty() || t.name.back() != '>')
            return nullptr;
        auto innerName = t.name.substr(lt + 1, t.name.size() - lt - 2);
        if (auto registered = symbols_.lookupType(innerName))
            return registered;
        if (innerName.find('<') == std::string::npos)
            return resolveNamedType(innerName, expr.location);
        return nullptr;
    };

    // Turbofish on the object: `Box2::<i32>.of(...)` / `MyClass::<T>.method()`.
    // When the object is an Identifier with callTypeArgs and objType is still
    // the base-template Class (name has no '<'), look up a concrete instance
    // that was already registered by Sema's resolveType under the mangled
    // name and swap it in. Avoids re-entering resolveType (which would also
    // run through builtin-container fast-paths that change the VyxTypeKind).
    // Only activated for genuine user classes (names not aliased to a
    // builtin container kind) so Vec / Dict / Set / Queue / Stack / Option /
    // Result continue to return their dedicated VyxTypeKind.
    if (maExpr->object->kind == ExprKind::Identifier &&
        (objType->kind == VyxTypeKind::Class || objType->kind == VyxTypeKind::Struct)) {
        auto* objId = maExpr->object->as<IdentifierExpr>();
        // R5 stage 2: `Option` and `Result` used to live here so
        // `Option::<T>.Some(v)` would keep the built-in VyxTypeKind.
        // They've been removed — when stdlib provides them as real
        // enum decls we want the user-class shell path to run. If
        // stdlib is absent the shell lookup falls through to the
        // ErrorType path and produces the same diagnostics as before.
        static const std::set<std::string> kBuiltinAliases = {
            "Vec", "Dict", "UnorderedMap", "Set", "UnorderedSet",
            "Stack", "Queue",
            "Ref", "Scope", "Box", "Delegate", "Event",
        };
        if (!objId->callTypeArgs.empty() &&
            objType->name.find('<') == std::string::npos &&
            !kBuiltinAliases.count(objId->name) && unit_) {
            // Build the mangled name and look it up directly.
            std::string mangled = objId->name + "<";
            for (size_t i = 0; i < objId->callTypeArgs.size(); ++i) {
                if (i > 0) mangled += ",";
                if (objId->callTypeArgs[i]) {
                    auto resolved = templateResolver_.resolveTypeAnnotation(
                        *objId->callTypeArgs[i]);
                    mangled += resolved ? resolved->toString() : "?";
                } else {
                    mangled += "?";
                }
            }
            mangled += ">";
            // Look up the already-registered mangled type. If present, use
            // it. Otherwise synthesise a lightweight instance Class type by
            // substituting the class-level generic params into the base
            // template's method signatures — WITHOUT calling resolveType
            // which would also re-enqueue instantiateClassTemplate and
            // re-analyse fields. We only need method return types; field
            // analysis happens later through the regular channel.
            auto instType = symbols_.lookupType(mangled);
            if (instType && !instType->methods.empty()) {
                objType = instType;
            } else {
                for (auto& d : unit_->declarations) {
                    if (!d || d->name != objId->name) continue;
                    if (d->kind != DeclKind::Class && d->kind != DeclKind::Struct) continue;
                    if (d->genericParams.size() != objId->callTypeArgs.size()) continue;
                    std::vector<VyxTypePtr> concreteTypes;
                    concreteTypes.reserve(objId->callTypeArgs.size());
                    for (auto& ta : objId->callTypeArgs) {
                        concreteTypes.push_back(ta
                            ? templateResolver_.resolveTypeAnnotation(*ta)
                            : nullptr);
                    }
                    GenericSubstitution subst;
                    for (size_t i = 0; i < d->genericParams.size(); ++i) {
                        if (concreteTypes[i])
                            subst.add(d->genericParams[i], concreteTypes[i]);
                    }
                    auto shell = std::make_shared<VyxType>();
                    shell->kind = (d->kind == DeclKind::Struct)
                        ? VyxTypeKind::Struct : VyxTypeKind::Class;
                    shell->name = mangled;
                    shell->paramTypes = concreteTypes;
                    if (d->kind == DeclKind::Class) {
                        for (auto& method : d->as<ClassDecl>()->methods) {
                            VyxType::MethodSig sig;
                            sig.name = method.name;
                            sig.isPublic = (method.visibility == Visibility::Public);
                            if (method.returnType) {
                                auto rt = templateResolver_.substituteType(
                                    *method.returnType, subst);
                                sig.returnType = rt;
                            } else {
                                sig.returnType = types::makeVoid();
                            }
                            for (auto& param : method.params) {
                                if (param.type) {
                                    sig.paramTypes.push_back(
                                        templateResolver_.substituteType(*param.type, subst));
                                } else {
                                    sig.paramTypes.push_back(types::makeUnknown());
                                }
                            }
                            shell->methods.push_back(std::move(sig));
                        }
                    }
                    objType = shell;
                    break;
                }
            }
        }
    }

    if (maExpr->object && maExpr->object->kind == ExprKind::Identifier &&
        iteratingCollections_.count(maExpr->object->as<IdentifierExpr>()->name)) {
        static const std::set<std::string> mutMethods = {
            "push", "pop", "insert", "remove", "clear",
            "add", "put", "shrink_to_fit", "reserve"
        };
        if (mutMethods.count(maExpr->member)) {
            diag_.error(expr.location,
                "cannot call '{}' on '{}' while iterating over it — iterator invalidation",
                maExpr->member, maExpr->object->as<IdentifierExpr>()->name);
        }
    }

    if (isOptionLike(*objType)) {
        // Accept both camelCase and snake_case query forms — std/option.vyx
        // registers both; Sema fast-paths return types here so method calls
        // on Option don't have to walk the full dispatch table.
        if (maExpr->member == "isSome" || maExpr->member == "isNone" ||
            maExpr->member == "is_some" || maExpr->member == "is_none") {
            return types::makeBool();
        }
        auto optInner = optionInner(*objType);
        if ((maExpr->member == "unwrap" ||
             maExpr->member == "unwrapOr" || maExpr->member == "unwrap_or") &&
            optInner) {
            return optInner;
        }
        // Higher-order methods on Option: map/and_then/or_else/filter.
        // We return Unknown so Sema doesn't emit "no member" warnings;
        // the actual instantiated return type is resolved at Mono /
        // CodeGen via the std enum-method dispatch.
        static const std::set<std::string> kOptionHighOrder = {
            "map", "and_then", "andThen", "or_else", "orElse", "filter"
        };
        if (kOptionHighOrder.count(maExpr->member)) {
            return types::makeUnknown();
        }
    }

    // Symmetric Result<T,E> fast-path: mirror Option's handling so the
    // same Sema shortcuts apply (`.is_ok` / `.isOk`, `.unwrap`, `.map`,
    // `.and_then`, `.map_err`, etc.) without walking the full dispatch.
    if (isResultLike(*objType)) {
        if (maExpr->member == "isOk" || maExpr->member == "isErr" ||
            maExpr->member == "is_ok" || maExpr->member == "is_err") {
            return types::makeBool();
        }
        auto okTy = resultOk(*objType);
        auto errTy = resultErr(*objType);
        if ((maExpr->member == "unwrap" ||
             maExpr->member == "unwrapOr" || maExpr->member == "unwrap_or") &&
            okTy) {
            return okTy;
        }
        if (maExpr->member == "unwrap_err" && errTy) {
            return errTy;
        }
        static const std::set<std::string> kResultHighOrder = {
            "map", "map_err", "mapErr",
            "and_then", "andThen", "or_else", "orElse"
        };
        if (kResultHighOrder.count(maExpr->member)) {
            return types::makeUnknown();
        }
    }

    if (maExpr->member == "move" && maExpr->object && maExpr->object->kind == ExprKind::Identifier) {
        auto* sym = symbols_.lookup(maExpr->object->as<IdentifierExpr>()->name);
        if (sym) {
            sym->isMoved = true;
            if (sym->isBorrowed) {
                diag_.error(expr.location, "cannot move '{}' while it is borrowed",
                    maExpr->object->as<IdentifierExpr>()->name);
            }
        }
    }

    // Smart-pointer deref is type-level sugar over the lang_item classes, but
    // the class method table can still carry the un-substituted generic `T`.
    // Prefer the canonical Class{name="Box<X>", paramTypes=[X]} payload here
    // so chained calls like `Box<Box<i32>>.deref().deref()` keep the first
    // call's inferredType as `Box<i32>` instead of falling back to generic T.
    if (maExpr->member == "deref") {
        if (auto inner = smartPtrInnerForMember(*objType)) {
            return inner;
        }
    }

    if (objType->kind == VyxTypeKind::Struct || objType->kind == VyxTypeKind::Class) {
        if (maExpr->object && maExpr->object->kind == ExprKind::Identifier && unit_) {
            auto* objId = maExpr->object->as<IdentifierExpr>();
            if (!objId->callTypeArgs.empty()) {
                for (auto& d : unit_->declarations) {
                    if (!d || d->name != objId->name) continue;
                    if (d->kind != DeclKind::Class && d->kind != DeclKind::Struct) continue;
                    if (d->genericParams.size() != objId->callTypeArgs.size()) continue;

                    const auto& methods = (d->kind == DeclKind::Class)
                        ? d->as<ClassDecl>()->methods
                        : d->as<StructDecl>()->methods;
                    for (auto& method : methods) {
                        if (method.name != maExpr->member) continue;
                        std::vector<VyxTypePtr> receiverArgs;
                        receiverArgs.reserve(objId->callTypeArgs.size());
                        bool allReceiverArgsConcrete = true;
                        for (auto& ta : objId->callTypeArgs) {
                            VyxTypePtr resolved = ta ? resolveType(*ta) : nullptr;
                            if (!resolved || resolved->kind == VyxTypeKind::Unknown ||
                                resolved->kind == VyxTypeKind::Generic) {
                                allReceiverArgsConcrete = false;
                                break;
                            }
                            receiverArgs.push_back(std::move(resolved));
                        }
                        if (!allReceiverArgsConcrete) continue;

                        bool isSelfAccess = maExpr->object->kind == ExprKind::SelfExpr;
                        bool enforceVisibility = (d->kind == DeclKind::Class);
                        if (enforceVisibility && method.visibility != Visibility::Public &&
                            !isSelfAccess && !inImportedScope_) {
                            diag_.error(expr.location, "method '{}' of '{}' is private",
                                maExpr->member, objType->toString());
                        }

                        GenericSubstitution subst;
                        for (size_t i = 0; i < d->genericParams.size(); ++i) {
                            subst.add(d->genericParams[i], receiverArgs[i]);
                        }
                        if (!method.returnType) return types::makeVoid();
                        auto rt = templateResolver_.substituteType(*method.returnType, subst);
                        if (rt && rt->kind == VyxTypeKind::Class &&
                            rt->methods.empty() && rt->name.find('<') != std::string::npos) {
                            auto reAnnot = convertTypeToAnnotation(rt);
                            if (reAnnot) {
                                auto reResolved = resolveType(*reAnnot);
                                if (reResolved && reResolved->kind != VyxTypeKind::Unknown)
                                    return reResolved;
                            }
                        }
                        return rt ? rt : types::makeUnknown();
                    }
                }
            }
        }
        if (maExpr->member == "new") {
            // Static factory `Vec::<i64>.new()` / `Dict::<K,V>.new()`: when the
            // receiver Identifier carries turbofish callTypeArgs, splice them
            // back into the returned type so downstream inference sees the
            // concrete instantiation (`Vec<i64>` / `Dict<K,V>`) instead of
            // the bare template name. Without this, `let v = Vec::<i64>.new()`
            // assigns v the type Class{name="Vec"} and any subsequent
            // `v.iter()` can't substitute T into the method's return type.
            if (maExpr->object && maExpr->object->kind == ExprKind::Identifier) {
                auto* objId = maExpr->object->as<IdentifierExpr>();
                // R5 phase 4d: after Box / Ref / Scope pivoted to Class-kind,
                // the formerly-split "shadow path" is no longer needed.  Every
                // `Name::<args>.new()` turbofish goes through the same
                // resolveTypeAnnotation path which produces the canonical
                // Class{name="Name<..>", paramTypes=[..]}.
                if (!objId->callTypeArgs.empty()) {
                    // IMPORTANT: use the identifier's *original* name (objId->name)
                    // rather than objType->name. objType may have been rewritten by
                    // the shell-path above to the already-mangled form
                    // ("Container<i64>"); if we then re-mangle with the type args
                    // we produce a double-mangled name ("Container<i64><i64>")
                    // that Mono can never match against the base template.
                    const std::string& baseName = objId->name;
                    GenericType gt;
                    gt.kind = TypeAnnotationKind::Generic;
                    gt.name = baseName;
                    for (auto& ta : objId->callTypeArgs)
                        if (ta) gt.typeArgs.push_back(cloneTypeAnnotation(*ta));
                    auto resolved = templateResolver_.resolveTypeAnnotation(gt);
                    if (resolved && resolved->kind != VyxTypeKind::Unknown)
                        return resolved;
                }
            }
            return objType;
        }
        bool isSelfAccess = (maExpr->object && maExpr->object->kind == ExprKind::SelfExpr);
        bool enforceVisibility = (objType->kind == VyxTypeKind::Class);
        for (auto& field : objType->fields) {
            if (field.name == maExpr->member) {
                if (enforceVisibility && !field.isPublic && !isSelfAccess && !inImportedScope_) {
                    diag_.error(expr.location, "field '{}' of '{}' is private",
                        maExpr->member, objType->toString());
                }
                return field.type;
            }
        }
        for (auto& method : objType->methods) {
            if (method.name == maExpr->member) {
                if (enforceVisibility && !method.isPublic && !isSelfAccess && !inImportedScope_) {
                    diag_.error(expr.location, "method '{}' of '{}' is private",
                        maExpr->member, objType->toString());
                }
                return method.returnType ? method.returnType : types::makeVoid();
            }
        }
        if (auto inner = smartPtrInnerForMember(*objType)) {
            auto derefType = inner;
            if (derefType &&
                (derefType->kind == VyxTypeKind::Class ||
                 derefType->kind == VyxTypeKind::Struct) &&
                derefType->fields.empty() && derefType->methods.empty()) {
                if (auto registered = symbols_.lookupType(derefType->name))
                    derefType = registered;
            }
            if (derefType &&
                (derefType->kind == VyxTypeKind::Class ||
                 derefType->kind == VyxTypeKind::Struct)) {
                bool derefEnforceVisibility =
                    derefType->kind == VyxTypeKind::Class;
                for (auto& field : derefType->fields) {
                    if (field.name == maExpr->member) {
                        if (derefEnforceVisibility && !field.isPublic && !inImportedScope_) {
                            diag_.error(expr.location, "field '{}' of '{}' is private",
                                maExpr->member, derefType->toString());
                        }
                        return field.type;
                    }
                }
                for (auto& method : derefType->methods) {
                    if (method.name == maExpr->member) {
                        if (derefEnforceVisibility && !method.isPublic && !inImportedScope_) {
                            diag_.error(expr.location, "method '{}' of '{}' is private",
                                maExpr->member, derefType->toString());
                        }
                        return method.returnType ? method.returnType : types::makeVoid();
                    }
                }
            }
        }
        if (!objType->methods.empty() || !objType->fields.empty()) {
            diag_.error(expr.location, "type '{}' has no member '{}'", objType->toString(), maExpr->member);
        }
    }

    // std container kinds (Vec/Dict/Set/...) lower to dedicated VyxType kinds
    // (DynArray/Dict/Set/...) at type-resolution time, but their methods live
    // on the user-defined `class Vec<T>`/`class Dict<K,V>`/... in std. Look
    // those up by name and, when found, substitute the receiver's element/key/
    // value types into the matching method's return type so chained calls
    // like `let it = v.iter()` get a concrete `Iterator<i64>` instead of
    // Unknown — without which Mono's method-generic dispatch on `it.map<U>()`
    // can't fire.
    auto stdContainerLookup = [&](const std::string& className,
                                   const std::vector<std::string>& classGenericParams,
                                   const std::vector<VyxTypePtr>& classArgs) -> VyxTypePtr {
        if (!unit_) return nullptr;
        for (auto& d : unit_->declarations) {
            if (!d || d->kind != DeclKind::Class) continue;
            if (d->name != className) continue;
            if (d->genericParams.size() != classArgs.size()) continue;
            auto* cd = d->as<ClassDecl>();
            for (auto& method : cd->methods) {
                if (method.name != maExpr->member) continue;
                if (!method.returnType) return types::makeVoid();
                GenericSubstitution subst;
                for (size_t i = 0; i < classGenericParams.size() && i < classArgs.size(); ++i)
                    if (classArgs[i]) subst.add(classGenericParams[i], classArgs[i]);
                auto rt = templateResolver_.substituteType(*method.returnType, subst);
                if (!rt || rt->kind == VyxTypeKind::Unknown) {
                    rt = templateResolver_.resolveTypeAnnotation(*method.returnType);
                }
                // Re-resolve the substituted type so the resulting Class entry
                // gets its methods populated (substituteType only walks names,
                // it doesn't trigger class-template instantiation). Without
                // this `let it = v.iter()` would set it: Class{name="Iterator<i64>"}
                // with empty methods, and the next `it.map<U>` couldn't see
                // map's signature.
                if (rt && rt->kind == VyxTypeKind::Class && rt->methods.empty() &&
                    rt->name.find('<') != std::string::npos) {
                    auto reAnnot = convertTypeToAnnotation(rt);
                    if (reAnnot) {
                        auto reResolved = resolveType(*reAnnot);
                        if (reResolved && !reResolved->methods.empty())
                            return reResolved;
                    }
                }
                return rt;
            }
            return nullptr;
        }
        return nullptr;
    };
    if (isVecLike(*objType)) {
        if (auto elem = vecElement(*objType)) {
            if (auto rt = stdContainerLookup("Vec", {"T"}, {elem}))
                return rt;
        }
    }
    if (isDictLike(*objType) && objType->paramTypes.size() == 2 &&
        objType->paramTypes[0] && objType->paramTypes[1]) {
        if (auto rt = stdContainerLookup("Dict", {"K", "V"},
                                         {objType->paramTypes[0], objType->paramTypes[1]}))
            return rt;
    }
    if (isElementContainerLike(*objType)) {
        if (auto elem = containerElement(*objType)) {
            // UnorderedSet maps to stdlib class `HashSet<T>`, not `UnorderedSet<T>`.
            const char* base = isSetLike(*objType) ? "Set"
                            : isUnorderedSetLike(*objType) ? "HashSet"
                            : isStackLike(*objType) ? "Stack" : "Queue";
            if (auto rt = stdContainerLookup(base, {"T"}, {elem}))
                return rt;
        }
    }
    // Smart-pointer wrappers Box<T> / Ref<T> / Scope<T> follow the
    // same pattern: methods live on the stdlib `class Box<T>` / `class
    // Ref<T>` / `class Scope<T>` and take T for substitution.  Without
    // this hook a call like `box.deref()` on a Box<T> never finds the
    // class method and Sema returns Unknown, which downstream CodeGen
    // lowers to "no member 'deref'".
    if (auto inner = smartPtrInnerForMember(*objType)) {
        const char* base = isBoxLike(*objType)   ? "Box"
                         : isRefLike(*objType)   ? "Ref"
                         : isScopeLike(*objType) ? "Scope"
                                                 : nullptr;
        if (base) {
            if (auto rt = stdContainerLookup(base, {"T"}, {inner}))
                return rt;
        }
    }

    // Primitive-target trait-impl dispatch (2026-04-23): `x.hash()` where
    // `x: i32` and the user wrote `impl Hashable for i32 { fn hash() -> i64 }`.
    // primitiveMethodImpls_ is populated by analyzeClassDecl; here we look up
    // the method for the receiver's canonical primitive name and return its
    // declared return type, so Sema-level type checking of generic bodies
    // written against trait bounds (`fn f<T: Hashable>(x: T)` → `x.hash()`)
    // sees `i64` instead of `Unknown`.
    //
    // For Class-kind "string" (R5: String is a Class with name="string")
    // the scan earlier in this function already ran against objType->methods
    // and returned; we only reach here for bare primitives (Integer / Float /
    // Bool / Char / RawPtr).  But we also honour the Class-kind string case
    // here for good measure — if the String class doesn't declare `hash`
    // but the user wrote `impl Hashable for string`, this fallback finds it.
    {
        std::string primName;
        switch (objType->kind) {
            case VyxTypeKind::Integer:
                if (objType->isSizeType)
                    primName = objType->isSigned ? "isize" : "usize";
                else
                    primName = (objType->isSigned ? "i" : "u") +
                                std::to_string(objType->bitWidth);
                break;
            case VyxTypeKind::Float:
                primName = "f" + std::to_string(objType->bitWidth);
                break;
            case VyxTypeKind::Bool:   primName = "bool";   break;
            case VyxTypeKind::Char:   primName = "char";   break;
            case VyxTypeKind::RawPtr: primName = "rawptr"; break;
            case VyxTypeKind::Class:
                if (isStringType(*objType)) primName = "string";
                break;
            default: break;
        }
        if (!primName.empty()) {
            auto primIt = primitiveMethodImpls_.find(primName);
            if (primIt != primitiveMethodImpls_.end()) {
                auto methIt = primIt->second.find(maExpr->member);
                if (methIt != primIt->second.end()) {
                    const MethodDecl* md = methIt->second;
                    if (md && md->returnType)
                        return resolveType(*md->returnType);
                    return types::makeVoid();
                }
            }
        }
    }

    // Other kinds (Enum/ErrorType, String, Array, Pointer, Reference, Optional,
    // Generic, Unknown from unresolved generics, etc.) intentionally return
    // Unknown here and let the downstream stage (CodeGen and builtin helpers)
    // resolve the real member/method semantics.
    return types::makeUnknown();
}

VyxTypePtr Sema::analyzeIndex(Expr& expr) {
    auto* idxExpr = expr.as<IndexExpr>();
    if (!idxExpr->object || !idxExpr->indexExpr) {
        diag_.error(expr.location, "index expression: missing object or index");
        return types::makeUnknown();
    }
    auto objType = analyzeExpr(*idxExpr->object);
    if (!objType) {
        diag_.error(expr.location, "index: object expression did not yield a type");
        return types::makeUnknown();
    }
    analyzeExpr(*idxExpr->indexExpr);

    if (objType->kind == VyxTypeKind::Array && objType->elementType)
        return objType->elementType;
    if (isStringType(*objType)) {
        if (idxExpr->indexExpr && idxExpr->indexExpr->kind == ExprKind::BinaryOp &&
            idxExpr->indexExpr->as<BinaryOpExpr>()->op == BinaryOp::RangeOp) {
            return types::makeString();
        }
        return types::makeChar();
    }

    if (objType->kind == VyxTypeKind::Struct || objType->kind == VyxTypeKind::Class) {
        for (auto& method : objType->methods) {
            if (method.name == "operator_index" && method.returnType)
                return method.returnType;
        }
    }
    // Container indexing for Vec/Dict/Set/Array-of-unknown and generics is resolved by CodeGen.
    return types::makeUnknown();
}
} // namespace vyx
