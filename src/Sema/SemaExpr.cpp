#include "Sema.h"
#include "SemaCloneHelpers.h"
#include "../Common/StringUtils.h"
#include <algorithm>
#include <functional>
#include <set>

namespace vyx {

// Public entry point: runs the real analysis and stores the inferred type
// on the AST node so Codegen can consult expr.inferredType directly.
VyxTypePtr Sema::analyzeExpr(Expr& expr) {
    auto t = analyzeExprImpl(expr);
    // Preserve existing inferredType if analyzer returned null (e.g. error path),
    // otherwise record the freshly inferred type.
    if (t) expr.inferredType = t;
    return t;
}

VyxTypePtr Sema::analyzeExprImpl(Expr& expr) {
    switch (expr.kind) {
        case ExprKind::IntLiteral:
            return types::makeInt(64, true);
        case ExprKind::FloatLiteral:
            return types::makeFloat(64);
        case ExprKind::BoolLiteral:
            return types::makeBool();
        case ExprKind::StringLiteral:
            return types::makeString();
        case ExprKind::StringInterpolation: {
            // Analyze every embedded `${expr}` — critical for generic / variadic
            // calls inside interpolations (`"val=${sum_homo(a,b,c)}"`), whose
            // instantiations would otherwise never be registered. Without this
            // walk, `instantiateTemplateIfNeeded` never fires for the inner
            // call, no monomorph lands in `pendingInstantiations_`, and the
            // call-site identifier stays un-mangled — yielding a linker
            // "undefined symbol" at the base name.
            auto* si = expr.as<StringInterpExpr>();
            for (auto& part : si->parts) {
                if (part.isExpr && part.expr) analyzeExpr(*part.expr);
            }
            return types::makeString();
        }
        case ExprKind::CharLiteral:
            return types::makeChar();
        case ExprKind::NullLiteral:
            return types::makeRawPtr();

        case ExprKind::Identifier: {
            auto* idExpr = expr.as<IdentifierExpr>();
            // Reject turbofish applied to a class/struct/errordef that has
            // zero generic parameters. Without this, `HashMap::<i64,i64>.new()`
            // silently creates a mangled instance whose body is the non-
            // generic template's — which then produces a layout-mismatched
            // struct at codegen and segfaults at runtime.
            if (!idExpr->callTypeArgs.empty() && unit_) {
                for (auto& d : unit_->declarations) {
                    if (!d) continue;
                    if (d->name != idExpr->name) continue;
                    if (d->kind != DeclKind::Class &&
                        d->kind != DeclKind::Struct &&
                        d->kind != DeclKind::ErrorDef) continue;
                    if (d->genericParams.empty()) {
                        diag_.error(expr.location,
                            "turbofish '::<...>' applied to non-generic {} '{}'; "
                            "remove the type arguments",
                            d->kind == DeclKind::Class    ? "class"
                          : d->kind == DeclKind::Struct   ? "struct"
                                                          : "error-def",
                            idExpr->name);
                        return types::makeUnknown();
                    }
                    break;
                }
            }
            auto* sym = symbols_.lookup(idExpr->name);
            if (!sym) {
                auto typeRef = symbols_.lookupType(idExpr->name);
                if (typeRef) {
                    return typeRef;
                }
                if (idExpr->typeAnnotation && idExpr->typeAnnotation->kind == TypeAnnotationKind::Generic) {
                    auto resolved = resolveType(*idExpr->typeAnnotation);
                    if (resolved && resolved->kind != VyxTypeKind::Unknown) {
                        return resolved;
                    }
                }
                // Turbofish: `Vec::<T>` / `Dict::<K,V>` / `Array::<i32, 16>` produces an
                // Identifier with `callTypeArgs` (and optionally `callArgExprs` for const
                // non-type params). Synthesize a Generic annotation so the type-factory path
                // (Vec/Dict/Set/Queue/Stack/Option/Result/...) and const-generic path can
                // resolve the concrete instantiation.
                if (!idExpr->callTypeArgs.empty()) {
                    GenericType genAnn;
                    genAnn.name = idExpr->name;
                    genAnn.location = expr.location;
                    for (size_t ai = 0; ai < idExpr->callTypeArgs.size(); ++ai) {
                        auto& ta = idExpr->callTypeArgs[ai];
                        // Null slot: const-param position — push nullptr so indices align.
                        genAnn.typeArgs.push_back(ta ? cloneTypeAnnotation(*ta) : nullptr);
                    }
                    // Propagate const-arg expressions so SemaResolve can evaluate them.
                    if (!idExpr->callArgExprs.empty()) {
                        for (auto& ae : idExpr->callArgExprs)
                            genAnn.argExprs.push_back(ae ? cloneExpr(*ae) : nullptr);
                    }
                    auto resolved = resolveType(genAnn);
                    if (resolved && resolved->kind != VyxTypeKind::Unknown) {
                        return resolved;
                    }
                }
                // Suggestion set: only the true language intrinsics. Library
                // symbols live in modules and are discovered via the user's
                // `use` directives rather than hard-coded here.
                static const char* commonNames[] = {
                    "print", "assert", "assert_eq", "panic",
                    "sizeof", "alignof", "alloc", "dealloc", "transmute",
                    "type_name", "typeinfo", "format",
                    "from_cstr", "from_cstr_len", "from_cstr_view_len", "from_raw_string_parts", "to_rawptr",
                };
                std::string suggestion;
                int bestDist = 3;
                for (auto* name : commonNames) {
                    int d = levenshteinDistance(idExpr->name, name);
                    if (d < bestDist) { bestDist = d; suggestion = name; }
                }
                // Const generic parameter used as a value (e.g. `return N;` in
                // `fn f<const N: i64>() -> i64`).  The Sema body analysis runs
                // before Mono substitutes the literal, so we just need to accept
                // the identifier and return its annotated type (always i64/i32/usize).
                if (activeConstGenericParams_.count(idExpr->name)) {
                    return types::makeInt(64, true);
                }

                // Async builtin namespaces: Promise is a compiler-level object
                // whose methods (all, race) are lowered directly in CodeGen.
                // Returning Unknown here lets analyzeMemberAccess/analyzeCall
                // pass through without spurious "undefined symbol" errors.
                static const std::set<std::string> kAsyncBuiltinNS = {
                    "Promise",
                    "Task",
                };
                if (kAsyncBuiltinNS.count(idExpr->name)) {
                    return types::makeUnknown();
                }

                if (!suggestion.empty())
                    diag_.error(expr.location, "undefined symbol '{}', did you mean '{}'?", idExpr->name, suggestion);
                else if (idExpr->name.find("_") != std::string::npos) {
                    std::string base = idExpr->name.substr(0, idExpr->name.rfind('_'));
                    diag_.error(expr.location, "undefined symbol '{}' (variadic arg index may be out of range for '{}')",
                        idExpr->name, base);
                } else
                    diag_.error(expr.location, "undefined symbol '{}'", idExpr->name);
                return types::makeUnknown();
            }
            if (sym->isMoved) {
                // P4-B V-MOVE-001: structured diagnostic pointing at both the
                // use site (primary) and the move site (compact note).
                diag_.errorCoded("V-MOVE-001", expr.location,
                    "use of moved value '{}' — ownership has been transferred",
                    idExpr->name);
                if (sym->moveLocation.line > 0) {
                    diag_.noteCompact(sym->moveLocation,
                        "value '{}' was moved here", idExpr->name);
                }
                diag_.helpCompact(expr.location,
                    "insert '.clone()' at the move site to keep '{}' valid, "
                    "or use '&{}' to borrow", idExpr->name, idExpr->name);
            }
            if (sym->type && isRefLike(*sym->type) && sym->isBorrowed) {
                diag_.warning(expr.location, "Ref<T> value '{}' is borrowed — ensure borrow ends before move", idExpr->name);
            }
            // Function identifier used as a value (first-class fn-ptr):
            // return the function type, not the return type. Without this,
            // `let f: fn(i32)->i32 = id_i32;` sees RHS as i32 and rejects
            // the assignment. Callers of `sym->type` that expect the
            // return type (e.g. call dispatch) already have the Call
            // expression path which uses sym->returnType directly.
            if (sym->isFunction && !idExpr->callTypeArgs.empty() == false) {
                auto fnT = std::make_shared<VyxType>();
                fnT->kind = VyxTypeKind::Function;
                fnT->name = "fn";
                fnT->paramTypes = sym->paramTypes;
                fnT->returnType = sym->returnType;
                return fnT;
            }
            return sym->type;
        }

        case ExprKind::BinaryOp:
            return analyzeBinaryOp(expr);
        case ExprKind::UnaryOp:
            return analyzeUnaryOp(expr);
        case ExprKind::Call:
            return analyzeCall(expr);
        case ExprKind::MemberAccess: {
            auto result = analyzeMemberAccess(expr);
            auto* maExpr = expr.as<MemberAccessExpr>();
            if (maExpr->object) {
                VyxTypePtr objType = nullptr;
                if (maExpr->object->kind == ExprKind::Identifier) {
                    auto* sym = symbols_.lookup(maExpr->object->as<IdentifierExpr>()->name);
                    if (sym) objType = sym->type;
                }
                // Allow both camelCase and snake_case for the Option query /
                // unwrap methods; plus `.map`, `.and_then`/`andThen`,
                // `.or_else`/`orElse`, `.filter` which are real Option methods
                // defined in std/option.vyx. Without allowing them here the
                // Option null-guard diagnostic fires spuriously.
                static const std::set<std::string> kOptionMethods = {
                    "isSome", "isNone", "is_some", "is_none",
                    "unwrap", "unwrapOr", "unwrap_or",
                    "map", "and_then", "andThen",
                    "or_else", "orElse", "filter"
                };
                if (objType && isOptionLike(*objType) &&
                    !kOptionMethods.count(maExpr->member)) {
                    auto inner = optionInner(*objType);
                    diag_.error(expr.location,
                        "cannot access '{}' on nullable type '{}?'; use 'if let', '.unwrap()' or '\?\?' first",
                        maExpr->member, inner ? inner->toString() : "?");
                }
            }
            return result;
        }
        case ExprKind::Index: {
            auto idxResult = analyzeIndex(expr);
            auto* idxExpr = expr.as<IndexExpr>();
            if (idxExpr->object) {
                VyxTypePtr objType = nullptr;
                if (idxExpr->object->kind == ExprKind::Identifier) {
                    auto* sym = symbols_.lookup(idxExpr->object->as<IdentifierExpr>()->name);
                    if (sym) objType = sym->type;
                }
                if (objType && isOptionLike(*objType)) {
                    auto inner = optionInner(*objType);
                    diag_.error(expr.location,
                        "cannot index into nullable type '{}?'; use 'if let', '.unwrap()' or '\?\?' first",
                        inner ? inner->toString() : "?");
                }
                if (!inUnsafe_ && objType &&
                    (objType->kind == VyxTypeKind::RawPtr || objType->kind == VyxTypeKind::Pointer)) {
                    diag_.error(expr.location, "raw pointer indexing requires 'unsafe' block");
                }
            }
            return idxResult;
        }

        case ExprKind::Assignment: {
            auto* assignExpr = expr.as<AssignmentExpr>();
            if (!assignExpr->lhs)
                diag_.error(expr.location, "assignment is missing left-hand side");
            else
                (void)analyzeExpr(*assignExpr->lhs);
            VyxTypePtr rhsType = nullptr;
            if (!assignExpr->rhs)
                diag_.error(expr.location, "assignment is missing right-hand side");
            else
                rhsType = analyzeExpr(*assignExpr->rhs);
            if (rhsType && isRefLike(*rhsType)) {
                if (assignExpr->rhs && assignExpr->rhs->kind == ExprKind::MemberAccess) {
                    auto* maRhs = assignExpr->rhs->as<MemberAccessExpr>();
                    if (maRhs->member == "move" && maRhs->object) {
                        if (maRhs->object->kind == ExprKind::Identifier) {
                            auto* moveSym = symbols_.lookup(maRhs->object->as<IdentifierExpr>()->name);
                            if (moveSym) moveSym->isMoved = true;
                        }
                    }
                }
            }
            return rhsType ? rhsType : types::makeUnknown();
        }

        case ExprKind::CompoundAssignment: {
            auto* compExpr = expr.as<CompoundAssignmentExpr>();
            if (!compExpr->target)
                diag_.error(expr.location, "compound assignment is missing target expression");
            if (!compExpr->value)
                diag_.error(expr.location, "compound assignment is missing value expression");
            auto targetType = compExpr->target ? analyzeExpr(*compExpr->target) : types::makeUnknown();
            if (compExpr->value) analyzeExpr(*compExpr->value);
            return targetType;
        }

        case ExprKind::StructInit: {
            auto* siExpr = expr.as<StructInitExpr>();
            auto type = symbols_.lookupType(siExpr->structName);
            if (!type && siExpr->structName.find('<') != std::string::npos) {
                auto ltPos = siExpr->structName.find('<');
                std::string baseName = siExpr->structName.substr(0, ltPos);
                GenericType genAnn;
                genAnn.name = baseName;
                genAnn.location = expr.location;
                std::string inner = siExpr->structName.substr(ltPos + 1, siExpr->structName.size() - ltPos - 2);
                size_t start = 0;
                int depth = 0;
                for (size_t i = 0; i < inner.size(); ++i) {
                    if (inner[i] == '<') ++depth;
                    else if (inner[i] == '>') --depth;
                    else if (inner[i] == ',' && depth == 0) {
                        auto sub = std::make_unique<NamedType>();
                        sub->name = inner.substr(start, i - start);
                        genAnn.typeArgs.push_back(std::move(sub));
                        start = i + 1;
                    }
                }
                auto sub = std::make_unique<NamedType>();
                sub->name = inner.substr(start);
                genAnn.typeArgs.push_back(std::move(sub));
                type = resolveType(genAnn);
            }
            if (!type) {
                diag_.error(expr.location, "undefined type '{}'", siExpr->structName);
                return types::makeUnknown();
            }
            if (!inUnsafe_ && externClassNames_.count(siExpr->structName)) {
                diag_.warning(expr.location,
                    "extern C++ class '{}' used outside 'unsafe' block — consider wrapping in unsafe {{ }}",
                    siExpr->structName);
            }
            // Analyze all field-init expressions first so their types are
            // available for generic-parameter inference below.
            std::vector<std::pair<std::string, VyxTypePtr>> analyzedFields;
            analyzedFields.reserve(siExpr->fieldInits.size());
            for (auto& [name, val] : siExpr->fieldInits) {
                VyxTypePtr valType = val ? analyzeExpr(*val) : nullptr;
                analyzedFields.emplace_back(name, valType);
            }
            // If the struct name has no '<' but the looked-up type is a bare
            // generic class template (methods have Generic/Unknown return types),
            // attempt to infer the type parameters from the field initializer
            // types and re-resolve to a concrete monomorphised instance.
            // Do NOT attempt inference when inside a generic template body
            // (activeGenericParams_ non-empty): the field types would still
            // reference unbound type params, causing incorrect i64 fallback
            // inference that poisons the template method return-type check.
            if (siExpr->structName.find('<') == std::string::npos &&
                activeGenericParams_.empty()) {
                if (auto inferred = inferGenericStructInit(
                        siExpr->structName, analyzedFields, expr.location)) {
                    type = inferred;
                }
            }
            for (size_t fi = 0; fi < siExpr->fieldInits.size(); ++fi) {
                auto& [name, val] = siExpr->fieldInits[fi];
                auto& valType = analyzedFields[fi].second;
                if (val) {
                    bool fieldFound = false;
                    for (auto& f : type->fields) {
                        if (f.name == name) {
                            fieldFound = true;
                            if (valType && f.type &&
                                f.type->kind != VyxTypeKind::Generic &&
                                f.type->kind != VyxTypeKind::Unknown &&
                                !isAssignable(*f.type, *valType, val->location)) {
                                diag_.warning(expr.location, "type mismatch for field '{}': expected '{}', got '{}'",
                                    name, f.type->toString(), valType->toString());
                            }
                            break;
                        }
                    }
                    if (!fieldFound) {
                        diag_.error(expr.location, "struct '{}' has no field '{}'", siExpr->structName, name);
                    }
                }
            }
            return type;
        }

        case ExprKind::ArrayInit: {
            auto* arrExpr = expr.as<ArrayInitExpr>();
            VyxTypePtr elemType = nullptr;
            for (auto& elem : arrExpr->elements) {
                if (elem) {
                    auto t = analyzeExpr(*elem);
                    if (!elemType) elemType = t;
                }
            }
            if (!elemType) {
                if (arrExpr->elements.empty())
                    diag_.error(expr.location, "array literal is empty (cannot infer element type)");
                else
                    diag_.error(expr.location, "array literal has no valid element expressions");
                elemType = types::makeUnknown();
            }
            // `[v; N]` repeat form: the array length is the evaluated repeat
            // count, not the literal element count (which is always 1 for this
            // shape). When N is a constant-foldable int literal we emit the
            // repeated size here; otherwise fall through to the literal element
            // count so template-time analysis still produces a well-formed
            // Array type (size=0 is the standard "unresolved" marker).
            int arrSize = static_cast<int>(arrExpr->elements.size());
            if (arrExpr->repeatCount &&
                arrExpr->repeatCount->kind == ExprKind::IntLiteral) {
                arrSize = static_cast<int>(
                    arrExpr->repeatCount->as<IntLiteralExpr>()->value);
            }
            return types::makeArray(elemType, arrSize);
        }

        case ExprKind::SelfExpr:
            // `self` in method bodies resolves to the enclosing class type,
            // tracked by analyzeClassDecl around each method body. Outside a
            // class method, `self` is an error — but we stay quiet here since
            // the parser / earlier analysis is expected to have flagged it.
            return currentClassType_ ? currentClassType_ : types::makeUnknown();

        case ExprKind::TupleInit: {
            auto* tupExpr = expr.as<TupleInitExpr>();
            auto tupleType = std::make_shared<VyxType>();
            tupleType->kind = VyxTypeKind::Tuple;
            for (size_t ti = 0; ti < tupExpr->elements.size(); ++ti) {
                if (!tupExpr->elements[ti]) {
                    diag_.error(expr.location, "tuple literal: missing element at position {}", ti);
                    continue;
                }
                tupleType->tupleTypes.push_back(analyzeExpr(*tupExpr->elements[ti]));
            }
            if (tupleType->tupleTypes.empty())
                diag_.error(expr.location, "tuple literal has no elements");
            return tupleType;
        }

        case ExprKind::Cast: {
            auto* castExpr = expr.as<CastExpr>();
            if (castExpr->operand) analyzeExpr(*castExpr->operand);
            if (castExpr->targetType) return resolveType(*castExpr->targetType);
            diag_.error(expr.location, "cast expression has no target type");
            return types::makeUnknown();
        }

        case ExprKind::FailExpr: {
            auto* failExpr = expr.as<FailExpr>();
            if (!failExpr->typeName.empty()) {
                auto errType = symbols_.lookupType(failExpr->typeName);
                if (!errType) {
                    diag_.warning(expr.location, "unknown error type '{}'", failExpr->typeName);
                }
            }
            // BUG-LV-12: analyze the payload/message sub-expression so its
            // `inferredType` is populated for codegen (string/int/struct
            // construction relies on this for proper layout).
            if (failExpr->message) {
                analyzeExpr(*failExpr->message);
            }
            if (currentReturnType_ && isResultLike(*currentReturnType_)) {
                return currentReturnType_;
            }
            return currentReturnType_ ? currentReturnType_ : types::makeUnknown();
        }

        case ExprKind::AwaitExpr: {
            auto* awaitExpr = expr.as<AwaitExpr>();
            if (awaitExpr->inner) {
                auto innerType = analyzeExpr(*awaitExpr->inner);
                if (innerType && isAsyncLike(*innerType)) {
                    if (auto result = asyncInner(*innerType)) {
                        return result;
                    }
                }
                return innerType ? innerType : types::makeUnknown();
            }
            return types::makeVoid();
        }

        case ExprKind::TryExpr: {
            auto* tryE = expr.as<TryExpr>();
            if (tryE->inner) {
                auto innerType = analyzeExpr(*tryE->inner);
                if (innerType) {
                    if (auto ok = resultOk(*innerType)) return ok;
                    if (auto inner = optionInner(*innerType)) return inner;
                }
                // R5 stage 1 续: accept `?` on a user enum tagged as
                // `@[lang_item("result")]` / `@[lang_item("option")]`.
                // Such a type arrives here as VyxTypeKind::ErrorType (user
                // ADT) rather than the built-in Result/Optional kinds, so
                // the two clauses above miss it. When the registry has a
                // matching slot and the inner's decl-name matches the
                // registered decl, extract the payload type from the
                // template's first variant (the "Ok" / "Some" arm by
                // convention).
                if (innerType && innerType->kind == VyxTypeKind::ErrorType) {
                    auto* resultDecl = langItems_.find("result");
                    auto* optionDecl = langItems_.find("option");
                    const Decl* langDecl = nullptr;
                    if (resultDecl && resultDecl->name == innerType->name) {
                        langDecl = resultDecl;
                    } else if (optionDecl && optionDecl->name == innerType->name) {
                        langDecl = optionDecl;
                    }
                    if (langDecl && langDecl->kind == DeclKind::ErrorDef) {
                        auto* ed = langDecl->as<ErrorDefDecl>();
                        // First variant (by convention: Ok / Some) carries
                        // the payload we unwrap. If the ADT has no payload
                        // types or the first variant is empty, fall through
                        // to the default (innerType).
                        if (!ed->variantTypes.empty() && !ed->variantTypes[0].empty() &&
                            ed->variantTypes[0][0]) {
                            // Build a substitution from the enum's generic
                            // params to the concrete paramTypes on the
                            // call site's VyxType.
                            GenericSubstitution subst;
                            for (size_t i = 0; i < langDecl->genericParams.size() &&
                                               i < innerType->paramTypes.size(); ++i) {
                                if (innerType->paramTypes[i])
                                    subst.add(langDecl->genericParams[i],
                                              innerType->paramTypes[i]);
                            }
                            auto resolved = templateResolver_.substituteType(
                                *ed->variantTypes[0][0], subst);
                            if (resolved && resolved->kind != VyxTypeKind::Unknown)
                                return resolved;
                        }
                    }
                }
                return innerType;
            }
            diag_.error(expr.location, "try expression has no operand");
            return types::makeUnknown();
        }

        case ExprKind::Ternary: {
            auto* ternExpr = expr.as<TernaryExpr>();
            if (!ternExpr->condition)
                diag_.error(expr.location, "ternary expression is missing condition");
            if (!ternExpr->trueExpr)
                diag_.error(expr.location, "ternary expression is missing true branch");
            if (!ternExpr->falseExpr)
                diag_.error(expr.location, "ternary expression is missing false branch");
            VyxTypePtr condType;
            if (ternExpr->condition) {
                condType = analyzeExpr(*ternExpr->condition);
                if (condType && condType->kind != VyxTypeKind::Bool &&
                    condType->kind != VyxTypeKind::Integer &&
                    condType->kind != VyxTypeKind::Pointer &&
                    condType->kind != VyxTypeKind::RawPtr &&
                    !isOptionLike(*condType) &&
                    condType->kind != VyxTypeKind::Unknown) {
                    diag_.warning(expr.location,
                        "ternary condition has type '{}', expected a boolean or testable type",
                        condType->toString());
                }
            }
            VyxTypePtr trueType = ternExpr->trueExpr ? analyzeExpr(*ternExpr->trueExpr) : types::makeUnknown();
            VyxTypePtr falseType = ternExpr->falseExpr ? analyzeExpr(*ternExpr->falseExpr) : types::makeUnknown();

            if (trueType && falseType &&
                trueType->kind != VyxTypeKind::Unknown &&
                falseType->kind != VyxTypeKind::Unknown) {
                if (trueType->isEqual(*falseType)) {
                    return trueType;
                }
                auto common = commonType(*trueType, *falseType);
                if (common) {
                    if (!trueType->isNumeric() || !falseType->isNumeric()) {
                        if (!isAssignable(*trueType, *falseType, expr.location) &&
                            !isAssignable(*falseType, *trueType, expr.location)) {
                            diag_.warning(expr.location,
                                "ternary branches have incompatible types '{}' and '{}', result type is '{}'",
                                trueType->toString(), falseType->toString(), common->toString());
                        }
                    }
                    return common;
                }
            }
            return trueType;
        }

        case ExprKind::Closure: {
            auto* closureExpr = expr.as<ClosureExpr>();
            auto fnType = std::make_shared<VyxType>();
            fnType->kind = VyxTypeKind::Function;
            fnType->name = "fn";

            // BUG-LV-09 fix: a `[name = expr]` capture introduces a fresh
            // binding `name` whose type is dictated by `expr` evaluated in
            // the enclosing scope. Without analysing `cap.moveExpr` first,
            // the symbol enters the closure env with `<unknown>` and CodeGen
            // later fails with "cannot map type '<unknown>' to LLVM type".
            // We therefore pre-resolve every `moveExpr` in the OUTER scope
            // (before pushScope) so identifiers in the expression resolve
            // against the captures' surroundings, then fall back to the
            // outer-symbol lookup for the legacy `[name]` (no `=`) form.
            std::vector<VyxTypePtr> capMoveExprTypes(closureExpr->captures.size());
            for (size_t ci = 0; ci < closureExpr->captures.size(); ++ci) {
                auto& cap = closureExpr->captures[ci];
                if (cap.moveExpr)
                    capMoveExprTypes[ci] = analyzeExpr(*cap.moveExpr);
            }

            symbols_.pushScope();

            for (size_t ci = 0; ci < closureExpr->captures.size(); ++ci) {
                auto& cap = closureExpr->captures[ci];
                Symbol capSym;
                capSym.name = cap.name;
                if (capMoveExprTypes[ci]) {
                    capSym.type = capMoveExprTypes[ci];
                } else {
                    auto* outerSym = symbols_.lookup(cap.name);
                    capSym.type = outerSym ? outerSym->type : types::makeUnknown();
                }
                capSym.isConst = !cap.byRef;
                capSym.isMutable = cap.byRef;
                symbols_.declare(cap.name, std::move(capSym));
            }

            for (auto& cp : closureExpr->params) {
                VyxTypePtr paramTy = cp.type ? resolveType(*cp.type) : types::makeUnknown();
                fnType->paramTypes.push_back(paramTy);
                Symbol paramSym;
                paramSym.name = cp.name;
                paramSym.type = paramTy;
                symbols_.declare(cp.name, std::move(paramSym));
            }

            VyxTypePtr declaredRetType = closureExpr->returnType
                ? resolveType(*closureExpr->returnType) : nullptr;

            auto savedReturn = currentReturnType_;
            if (declaredRetType)
                currentReturnType_ = declaredRetType;

            VyxTypePtr bodyRetType = types::makeVoid();
            if (closureExpr->singleExpr) {
                bodyRetType = analyzeExpr(*closureExpr->singleExpr);
            } else if (closureExpr->body) {
                analyzeStmt(*closureExpr->body);
                if (closureExpr->body->kind == StmtKind::Block) {
                    auto* block = closureExpr->body->as<BlockStmt>();
                    if (!block->statements.empty()) {
                        // Recover the body's return type from the already-
                        // analysed last statement. We must NOT re-call
                        // analyzeExpr here: the block's own scope (holding
                        // `let v = ...;` bindings) has already been popped
                        // by analyzeBlock, so a fresh analyzeExpr would
                        // fail with "undefined symbol 'v'" on any return
                        // that references a local.
                        auto& lastStmt = block->statements.back();
                        if (lastStmt && lastStmt->kind == StmtKind::Return) {
                            auto* rs = lastStmt->as<ReturnStmt>();
                            if (rs->expr && rs->expr->inferredType)
                                bodyRetType = rs->expr->inferredType;
                        } else if (lastStmt && lastStmt->kind == StmtKind::ExprStmt) {
                            auto* es = lastStmt->as<ExprStmt>();
                            if (es->expr && es->expr->inferredType)
                                bodyRetType = es->expr->inferredType;
                        }
                    }
                }
            }

            symbols_.popScope();
            currentReturnType_ = savedReturn;

            if (declaredRetType)
                fnType->returnType = declaredRetType;
            else
                fnType->returnType = bodyRetType ? bodyRetType : types::makeVoid();
            // Stash the fully-inferred function type on the closure expr
            // so CodeGen can pull the correct return type instead of
            // defaulting to i32 (which truncates struct returns — notably
            // `|| "hello"` where the inferred return is `string`).
            closureExpr->inferredType = fnType;
            return fnType;
        }

        case ExprKind::InlineAsm: {
            auto* asmE = expr.as<InlineAsmExpr>();
            for (auto& op : asmE->operands) {
                if (op) (void)analyzeExpr(*op);
            }
            if (!asmE->constraints.empty() && asmE->constraints[0] == '=')
                return types::makeInt(64, true);
            return types::makeVoid();
        }

        // P5-pack: pack fold expression — `args...+`
        // Sema defers full type resolution to Mono (which expands the pack).
        // We verify here that the identifier names a known parameter/variable
        // and return the inferred element type (or Unknown for generic packs).
        case ExprKind::PackFold: {
            auto* pfe = expr.as<PackFoldExpr>();
            // Just look up the pack variable; type resolution happens at Mono time.
            auto packType = symbols_.lookupType(pfe->packName);
            if (!packType) {
                // May be a value-level parameter, not a type; that's OK at this stage.
                // Return Unknown — Mono will substitute concrete types.
                return types::makeUnknown();
            }
            if (packType->isPack() && !packType->tupleTypes.empty())
                return packType->tupleTypes[0]; // rough element type
            return types::makeUnknown();
        }

        // F: sizeof...(pack) — compile-time pack-length.
        // Returns i64. The actual integer constant is substituted at
        // createVariadicInstance time (Sema) or Mono time; here we just
        // ensure the expression gets the correct inferred type.
        case ExprKind::SizeofPack:
            return types::makeInt(64, true);

        // P3-Q: compile-time type reflection — T::kind / T::name / T::fields / T::methods
        // At Sema time, just verify that T names a known generic parameter (or skip
        // silently if it's unresolvable — Mono will substitute).  Always returns string
        // because kind/name are strings and fields/methods expand at Mono time but the
        // loop variable binding is checked later (the expression itself is "string").
        case ExprKind::TypeReflect: {
            auto* re = expr.as<TypeReflectExpr>();
            // P2D-009: invalid reflection member.
            // The parser stores invalid members as "<invalid>:actualName" to
            // distinguish them from the valid set {kind, name, fields, methods}.
            // (Post the 2026-04-21 parser fix, only TRUE reflection-intent
            // spellings reach this arm — `Op::Variant` on errordefs is now
            // parsed as a plain MemberAccess.)
            if (!re->member.empty() && re->member[0] == '<') {
                std::string badMember = re->member;
                auto colonPos = badMember.find(':');
                if (colonPos != std::string::npos)
                    badMember = badMember.substr(colonPos + 1);
                diag_.errorCoded("P2D-009", expr.location,
                    "unknown reflection member '{}' on type parameter '{}'; "
                    "valid members are 'name', 'kind', 'fields', 'methods'",
                    badMember, re->typeParam);
                return types::makeUnknown();
            }
            // Intentionally permissive: if typeParam is not in activeGenericParams_
            // we still return string so downstream code compiles. Mono will error
            // if the parameter is unbound at instantiation time.
            return types::makeString();
        }

        default:
            diag_.error(expr.location, "internal: unhandled expression kind in semantic analysis ({})",
                static_cast<int>(expr.kind));
            return types::makeUnknown();
    }
}

} // namespace vyx
