#include "Sema.h"
#include "SemaCloneHelpers.h"
#include "../Common/StringUtils.h"
#include <algorithm>
#include <functional>
#include <map>

namespace vyx {
// ============================================================
//  Type resolution
// ============================================================

VyxTypePtr Sema::resolveType(const TypeAnnotation& annotation) {
    switch (annotation.kind) {
        case TypeAnnotationKind::Named:
            return resolveNamedType(annotation.name, annotation.location);

        case TypeAnnotationKind::Pointer: {
            auto* pt = annotation.as<PointerType>();
            if (!pt->innerType) {
                diag_.error(annotation.location, "pointer type requires an inner type (use `*T`)");
                return types::makePointer(types::makeUnknown());
            }
            return types::makePointer(resolveType(*pt->innerType));
        }

        case TypeAnnotationKind::Reference: {
            auto* rt = annotation.as<ReferenceType>();
            if (!rt->innerType) {
                diag_.error(annotation.location, "reference type requires an inner type (use `&T` / `&mut T`)");
                return types::makeReference(types::makeUnknown(), rt->isMutable);
            }
            return types::makeReference(resolveType(*rt->innerType), rt->isMutable);
        }

        case TypeAnnotationKind::Array: {
            auto* at = annotation.as<ArrayType>();
            if (!at->elementType) {
                diag_.error(annotation.location, "array type requires an element type (e.g. `[T; n]` or `[T]`)");
            }
            auto elem = at->elementType ? resolveType(*at->elementType) : types::makeUnknown();
            int size = 0;
            if (at->size && at->size->kind == ExprKind::IntLiteral) {
                size = static_cast<int>(at->size->as<IntLiteralExpr>()->value);
            }
            return types::makeArray(elem, size);
        }

        case TypeAnnotationKind::Tuple: {
            auto* tt = annotation.as<TupleType>();
            auto t = std::make_shared<VyxType>();
            t->kind = VyxTypeKind::Tuple;
            for (size_t ti = 0; ti < tt->elements.size(); ++ti) {
                auto& elem = tt->elements[ti];
                if (!elem) {
                    diag_.error(annotation.location, "tuple type: missing element type at position {}", ti);
                    t->tupleTypes.push_back(types::makeUnknown());
                    continue;
                }
                t->tupleTypes.push_back(resolveType(*elem));
            }
            return t;
        }

        case TypeAnnotationKind::Function: {
            auto* ft = annotation.as<FunctionType>();
            auto t = std::make_shared<VyxType>();
            t->kind = VyxTypeKind::Function;
            for (size_t pi = 0; pi < ft->paramTypes.size(); ++pi) {
                auto& pt = ft->paramTypes[pi];
                if (!pt) {
                    diag_.error(annotation.location, "function type: missing parameter type at position {}", pi);
                    t->paramTypes.push_back(types::makeUnknown());
                } else {
                    t->paramTypes.push_back(resolveType(*pt));
                }
            }
            if (ft->returnType) {
                t->returnType = resolveType(*ft->returnType);
            }
            return t;
        }

        case TypeAnnotationKind::Generic: {
            auto* gt = annotation.as<GenericType>();
            auto& typeArgs = gt->typeArgs;

            // Some parser paths preserve a bare type parameter as
            // GenericType{name=T,args=[]} instead of Named.  Keep it
            // equivalent to resolveNamedType's active-generic handling so
            // method-level generics inside impl/interface signatures (for
            // example `fn fmap<A>(x: Vec<A>)`) do not fall through to the
            // user-template lookup and report "undefined type A".
            if (typeArgs.empty()) {
                if (annotation.name == "Self" ||
                    activeGenericParams_.contains(annotation.name) ||
                    activeConstGenericParams_.contains(annotation.name)) {
                    auto t = std::make_shared<VyxType>();
                    t->kind = VyxTypeKind::Generic;
                    t->name = annotation.name;
                    return t;
                }
            }

            auto resolveTypeArgChecked = [&](size_t idx) -> VyxTypePtr {
                if (idx >= typeArgs.size()) {
                    diag_.error(annotation.location,
                        "internal: generic type `{}`: type argument index {} out of range",
                        annotation.name, idx);
                    return types::makeUnknown();
                }
                if (!typeArgs[idx]) {
                    diag_.error(annotation.location,
                        "generic type `{}`: missing type argument at position {}",
                        annotation.name, idx);
                    return types::makeUnknown();
                }
                return resolveType(*typeArgs[idx]);
            };

            // R5 stage 2: Option / Result are stdlib decls — no built-in
            // fallback. The user-class instantiation loop below handles
            // them as regular generic ErrorDef types. A TU that fails to
            // `use std.core;` (or equivalent) will hit "undefined type"
            // here, which is correct — there's no "bootstrap without
            // stdlib" mode to paper over the missing import.

            // Remaining generic types (`Vec<T>`, `Dict<K,V>`, user classes,
            // ...) must come from user-visible `class X<T>` / `struct X<T>`
            // declarations that the caller has explicitly imported. The
            // lookup over `unit_->declarations` below is the sole source
            // of truth for those.

            // ── Full template specialization fast-path ───────────────────
            // If the user wrote `class Vec<i32> { ... }` / `fn foo<i32>()`,
            // the parser collapsed that into a plain non-generic decl whose
            // `name` already equals the mangled key (`Vec<i32>`) and whose
            // `isFullSpecialization` flag is set. Pass 1 registered its
            // VyxType under that same name, and `analyzeClassDecl` filled
            // in the field/method shape (the decl's `genericParams` is
            // empty so it follows the regular non-generic analysis path).
            //
            // Build the same mangled key from the call-site type-arguments
            // and look it up directly: on a hit we return the user-defined
            // VyxType verbatim and mark the mangled name as "already
            // instantiated", which tells Mono's processOne() to also skip
            // its template-clone path. This guarantees `Vec<i32>` use
            // sites resolve to the hand-written body — never to a generated
            // monomorph of the `Vec<T>` template (if one happens to coexist
            // in the same TU).
            if (unit_) {
                std::string fullSpecKey = annotation.name + "<";
                for (size_t i = 0; i < typeArgs.size(); ++i) {
                    if (i > 0) fullSpecKey += ",";
                    if (!typeArgs[i]) {
                        // Const-param slot: emit the integer value if available.
                        if (i < gt->argExprs.size() && gt->argExprs[i] &&
                            gt->argExprs[i]->kind == ExprKind::IntLiteral) {
                            fullSpecKey += std::to_string(
                                gt->argExprs[i]->as<IntLiteralExpr>()->value);
                        } else {
                            fullSpecKey += "?";
                        }
                    } else {
                        auto ct = resolveTypeArgChecked(i);
                        fullSpecKey += ct ? ct->toString() : "?";
                    }
                }
                fullSpecKey += ">";
                for (auto& d : unit_->declarations) {
                    if (!d || !d->isFullSpecialization) continue;
                    if (d->kind != DeclKind::Class &&
                        d->kind != DeclKind::Struct) continue;
                    if (d->name != fullSpecKey) continue;
                    auto specType = symbols_.lookupType(fullSpecKey);
                    if (!specType) {
                        // Pass 1 should have registered this; fall through
                        // to template-clone path as a defensive fallback.
                        break;
                    }
                    instantiatedClassTemplates_.insert(fullSpecKey);
                    return specType;
                }
            }

            // ── P2b: Partial template specialization candidate selection ─────
            // Before instantiating the primary template, check whether any
            // partial specialization matches the concrete argument list.  If
            // exactly one matches (or one is strictly more specific than all
            // others), use that decl's body instead.
            //
            // We first build the concrete VyxTypePtr list from typeArgs so the
            // pattern-matcher can compare them.  This mirrors the primary-
            // template path below but resolves types eagerly so we have
            // concrete names available for matching.
            if (unit_) {
                std::vector<VyxTypePtr> concreteArgsForPartial;
                concreteArgsForPartial.reserve(typeArgs.size());
                for (size_t i = 0; i < typeArgs.size(); ++i) {
                    if (typeArgs[i])
                        concreteArgsForPartial.push_back(resolveTypeArgChecked(i));
                    else
                        concreteArgsForPartial.push_back(nullptr);
                }

                auto partialMatch = selectPartialSpec(
                    annotation.name, concreteArgsForPartial, annotation.location);

                if (partialMatch.decl) {
                    const Decl* psDecl = partialMatch.decl;
                    // Build mangled name using the *concrete* args (same as primary)
                    std::string mangledName = annotation.name + "<";
                    for (size_t i = 0; i < concreteArgsForPartial.size(); ++i) {
                        if (i > 0) mangledName += ",";
                        mangledName += concreteArgsForPartial[i]
                            ? concreteArgsForPartial[i]->toString() : "?";
                    }
                    mangledName += ">";

                    auto existingType = symbols_.lookupType(mangledName);
                    if (existingType) return existingType;

                    // Create the VyxType shell for the specialization instance
                    auto instType = std::make_shared<VyxType>();
                    instType->kind = (psDecl->kind == DeclKind::Struct)
                        ? VyxTypeKind::Struct : VyxTypeKind::Class;
                    instType->name = mangledName;

                    // Build substitution from the pattern-match bindings.
                    // The bindings map fresh param names → VyxTypePtr.
                    GenericSubstitution subst;
                    for (auto& [fp, ty] : partialMatch.bindings)
                        subst.add(fp, ty);

                    // Push fresh params into active set so field/method type
                    // resolution doesn't emit "undefined type" errors.
                    std::vector<std::string> tempPushed;
                    for (auto& gp : psDecl->genericParams) {
                        if (activeGenericParams_.insert(gp).second)
                            tempPushed.push_back(gp);
                    }

                    auto& fields = (psDecl->kind == DeclKind::Struct)
                        ? psDecl->as<StructDecl>()->fields
                        : psDecl->as<ClassDecl>()->fields;
                    for (auto& field : fields) {
                        VyxType::Field f;
                        f.name = field.name;
                        f.isPublic = (field.visibility == Visibility::Public);
                        if (field.type) {
                            f.type = templateResolver_.substituteType(*field.type, subst);
                            if (f.type->kind == VyxTypeKind::Generic)
                                f.type = resolveType(*field.type);
                        }
                        instType->fields.push_back(f);
                        for (auto& extra : field.extraNames) {
                            VyxType::Field ef = f;
                            ef.name = extra;
                            instType->fields.push_back(ef);
                        }
                    }
                    if (psDecl->kind == DeclKind::Class) {
                        auto* cd = psDecl->as<ClassDecl>();
                        for (auto& method : cd->methods) {
                            VyxType::MethodSig sig;
                            sig.name = method.name;
                            sig.isPublic = (method.visibility == Visibility::Public);
                            if (method.returnType) {
                                sig.returnType = templateResolver_.substituteType(
                                    *method.returnType, subst);
                                if (sig.returnType->kind == VyxTypeKind::Generic)
                                    sig.returnType = resolveType(*method.returnType);
                            } else {
                                sig.returnType = types::makeVoid();
                            }
                            for (auto& param : method.params) {
                                if (param.type) {
                                    auto pt = templateResolver_.substituteType(*param.type, subst);
                                    sig.paramTypes.push_back(pt);
                                } else {
                                    sig.paramTypes.push_back(types::makeUnknown());
                                }
                            }
                            instType->methods.push_back(std::move(sig));
                        }
                    }

                    for (auto& gp : tempPushed) activeGenericParams_.erase(gp);

                    symbols_.registerType(mangledName, instType);

                    // Check concreteness before queuing codegen
                    bool allConcrete = true;
                    for (auto& ct : concreteArgsForPartial) {
                        if (!ct || ct->kind == VyxTypeKind::Generic ||
                            ct->kind == VyxTypeKind::Unknown) {
                            allConcrete = false;
                            break;
                        }
                    }
                    if (allConcrete &&
                        instantiatedClassTemplates_.insert(mangledName).second) {
                        // Build a concreteTypes vector aligned with psDecl->genericParams
                        // (the partial spec's fresh params), not the use-site arity.
                        std::vector<VyxTypePtr> psConcreteTypes;
                        psConcreteTypes.reserve(psDecl->genericParams.size());
                        for (auto& fp : psDecl->genericParams) {
                            auto it = partialMatch.bindings.find(fp);
                            psConcreteTypes.push_back(
                                (it != partialMatch.bindings.end()) ? it->second : nullptr);
                        }
                        instantiateClassTemplate(*psDecl, mangledName,
                                                 psConcreteTypes,
                                                 annotation.location, {});
                    }

                    return instType;
                }
            }

            if (unit_) {
                for (size_t di = 0; di < unit_->declarations.size(); ++di) {
                    auto& d = unit_->declarations[di];
                    if (d && (d->kind == DeclKind::Struct || d->kind == DeclKind::Class) &&
                        d->name == annotation.name && !d->genericParams.empty() &&
                        d->genericParams.size() == typeArgs.size()) {

                        // Build mangled name and collect concrete types.  Const
                        // parameters (in d->genericConstParams) have a null typeArgs
                        // slot and carry their integer value in argExprs; emit them
                        // as integer literals in the mangle so distinct N values
                        // land in different symbol-table buckets.
                        std::string mangledName = annotation.name + "<";
                        std::vector<VyxTypePtr> concreteTypes;  // type-only slots
                        std::map<std::string, int64_t> constArgValues;
                        for (size_t i = 0; i < typeArgs.size(); ++i) {
                            const std::string& pname = d->genericParams[i];
                            bool isConstParam =
                                d->genericConstParams.find(pname) != d->genericConstParams.end();
                            if (i > 0) mangledName += ",";
                            if (isConstParam) {
                                // Const slot: evaluate the expression from argExprs[i].
                                int64_t cv = 0;
                                bool cvFound = false;
                                if (i < gt->argExprs.size() && gt->argExprs[i]) {
                                    if (gt->argExprs[i]->kind == ExprKind::IntLiteral) {
                                        cv = gt->argExprs[i]->as<IntLiteralExpr>()->value;
                                        cvFound = true;
                                    } else {
                                        // Non-literal passed to a const param — error.
                                        diag_.error(annotation.location,
                                            "non-type template parameter '{}' requires an integer literal",
                                            pname);
                                    }
                                } else if (i < typeArgs.size() && typeArgs[i]) {
                                    // A type annotation was provided where an integer is expected.
                                    // Exception: if the NamedType refers to a const-generic param
                                    // of `d` itself (e.g. `Buffer<T, N>` inside a method of
                                    // `class Buffer<T, const N: i64>`), treat it as a symbolic
                                    // pass-through placeholder — the actual integer value is
                                    // unknown at template-analysis time and will be substituted
                                    // by instantiateClassTemplate / Mono.
                                    // Also handle the case where N is in activeConstGenericParams_
                                    // (pushed by instantiateTemplateIfNeeded for function templates
                                    // that use N in a return-type annotation).
                                    // We intentionally do NOT match against activeGenericParams_
                                    // here: using a *type* param (T) in a const slot is a real
                                    // user error even during template analysis.
                                    const bool isSymbolicConstParam =
                                        (typeArgs[i]->kind == TypeAnnotationKind::Named) &&
                                        (d->genericConstParams.count(typeArgs[i]->name) > 0 ||
                                         activeConstGenericParams_.count(typeArgs[i]->name) > 0);
                                    if (isSymbolicConstParam) {
                                        // Emit the param name verbatim into the mangled name so
                                        // distinct symbolic uses stay distinct from each other.
                                        // The instance will remain non-concrete and won't be
                                        // enqueued for codegen until Mono resolves N.
                                        mangledName += typeArgs[i]->name;
                                        concreteTypes.push_back(nullptr);
                                        continue;
                                    }
                                    diag_.error(annotation.location,
                                        "non-type template parameter '{}' requires an integer literal, not a type",
                                        pname);
                                } else if (!cvFound) {
                                    // No argument at all for a required const param.
                                    diag_.error(annotation.location,
                                        "missing argument for non-type template parameter '{}'", pname);
                                }
                                mangledName += std::to_string(cv);
                                constArgValues[pname] = cv;
                                concreteTypes.push_back(nullptr); // placeholder
                            } else {
                                auto ct = resolveTypeArgChecked(i);
                                concreteTypes.push_back(ct);
                                mangledName += ct ? ct->toString() : "?";
                            }
                        }
                        mangledName += ">";

                        auto existingType = symbols_.lookupType(mangledName);
                        if (existingType) return existingType;

                        auto instType = std::make_shared<VyxType>();
                        instType->kind = (d->kind == DeclKind::Struct) ? VyxTypeKind::Struct : VyxTypeKind::Class;
                        instType->name = mangledName;

                        // Build type-only substitution (const params are handled by constArgValues).
                        // concreteTypes[gi] aligns with d->genericParams[gi] one-to-one;
                        // const-param slots hold nullptr.
                        GenericSubstitution subst;
                        for (size_t gi = 0; gi < d->genericParams.size() && gi < concreteTypes.size(); ++gi) {
                            const std::string& pn = d->genericParams[gi];
                            bool isCp = d->genericConstParams.count(pn) > 0;
                            if (!isCp && concreteTypes[gi]) {
                                subst.add(pn, concreteTypes[gi]);
                            }
                        }

                        auto& fields = (d->kind == DeclKind::Struct)
                            ? d->as<StructDecl>()->fields
                            : d->as<ClassDecl>()->fields;

                        // Fallback `resolveType(*field.type)` / `resolveType(*method.returnType)`
                        // below walks the *original* (un-substituted) annotation, so any
                        // class-level generic parameter name (e.g. `T` in `Vec<T>`) must be
                        // recognised as Generic — not "undefined type". The active set at the
                        // call site belongs to *this resolveType caller* (e.g. Dict's `K,V` when
                        // analysing `Dict.keys()`'s body), so we temporarily augment it with
                        // `d->genericParams` for the duration of the fallback. Inserts are
                        // undone after the loop; we skip params already present so we don't
                        // accidentally erase a caller-owned name (rare shadowing case).
                        std::vector<std::string> tempPushed;
                        tempPushed.reserve(d->genericParams.size());
                        for (auto& gp : d->genericParams) {
                            if (activeGenericParams_.insert(gp).second)
                                tempPushed.push_back(gp);
                        }

                        for (auto& field : fields) {
                            VyxType::Field f;
                            f.name = field.name;
                            f.isPublic = (field.visibility == Visibility::Public);
                            if (field.type) {
                                f.type = templateResolver_.substituteType(*field.type, subst);
                                if (f.type->kind == VyxTypeKind::Generic)
                                    f.type = resolveType(*field.type);
                            }
                            instType->fields.push_back(f);
                            for (auto& extra : field.extraNames) {
                                VyxType::Field ef = f;
                                ef.name = extra;
                                instType->fields.push_back(ef);
                            }
                        }

                        if (d->kind == DeclKind::Class) {
                            auto* classD = d->as<ClassDecl>();
                            for (auto& method : classD->methods) {
                                VyxType::MethodSig sig;
                                sig.name = method.name;
                                sig.isPublic = (method.visibility == Visibility::Public);
                                // Method-level generics (e.g. `fn map<U>(...) -> Vec<U>`) are
                                // also visible inside their own signature. Push them around
                                // this method only; the class-level push above already covers `T`.
                                std::vector<std::string> mPushed;
                                mPushed.reserve(method.genericParams.size());
                                for (auto& mgp : method.genericParams) {
                                    if (activeGenericParams_.insert(mgp).second)
                                        mPushed.push_back(mgp);
                                }
                                if (method.returnType) {
                                    sig.returnType = templateResolver_.substituteType(*method.returnType, subst);
                                    if (sig.returnType->kind == VyxTypeKind::Generic)
                                        sig.returnType = resolveType(*method.returnType);
                                } else {
                                    sig.returnType = types::makeVoid();
                                }
                                for (auto& param : method.params) {
                                    if (param.type) {
                                        auto paramT = templateResolver_.substituteType(*param.type, subst);
                                        if (paramT->kind != VyxTypeKind::Generic)
                                            sig.paramTypes.push_back(paramT);
                                        else
                                            sig.paramTypes.push_back(resolveType(*param.type));
                                    } else {
                                        sig.paramTypes.push_back(types::makeUnknown());
                                    }
                                }
                                for (auto& mgp : mPushed) activeGenericParams_.erase(mgp);
                                instType->methods.push_back(std::move(sig));
                            }
                        }

                        for (auto& gp : tempPushed) activeGenericParams_.erase(gp);

                        // A partial instantiation (at least one typeArg is still
                        // a method-level generic parameter or failed to resolve)
                        // must not trigger full codegen of a concrete class:
                        // its mangled name still contains unsubstituted type
                        // parameters, and `instantiateClassTemplate` would
                        // enqueue a pending class whose codegen pass later
                        // rejects it with "still has unresolved type parameter
                        // X -> X". However we *do* still register it into the
                        // symbol table under its mangled name so that later
                        // `resolveNamedType("Iterator<T>")` lookups inside the
                        // same enclosing scope can find the shape (e.g. when a
                        // method returns `Iterator<T>` and another expression
                        // re-resolves that annotation via the Named path).
                        // allConcrete: every *type* slot must be concrete.
                        // const-param slots are `nullptr` in concreteTypes; skip them.
                        bool allConcrete = true;
                        for (size_t gi = 0; gi < d->genericParams.size(); ++gi) {
                            bool isCp = d->genericConstParams.count(d->genericParams[gi]) > 0;
                            if (isCp) continue;
                            const VyxTypePtr& ct = concreteTypes[gi];
                            if (!ct || ct->kind == VyxTypeKind::Generic ||
                                ct->kind == VyxTypeKind::Unknown) {
                                allConcrete = false;
                                break;
                            }
                        }

                        symbols_.registerType(mangledName, instType);

                        if (allConcrete &&
                            instantiatedClassTemplates_.insert(mangledName).second) {
                            instantiateClassTemplate(*d, mangledName, concreteTypes,
                                                     annotation.location, constArgValues);
                        }

                        return instType;
                    }
                }
            }

            // P2 C6: Variadic generic class/struct template (e.g. `class Pair<...Ts>`).
            // Usage `Pair<i32, string>` produces typeArgs.size() == 2 but
            // genericParams.size() == 1 (just the pack name). Match this
            // separately: collect all typeArgs as the pack's concrete members,
            // mangle as `Pair<i32,string>`, and trigger instantiation.
            if (unit_ && !typeArgs.empty()) {
                for (size_t di = 0; di < unit_->declarations.size(); ++di) {
                    auto& d = unit_->declarations[di];
                    if (!d) continue;
                    if (d->kind != DeclKind::Struct && d->kind != DeclKind::Class) continue;
                    if (d->name != annotation.name) continue;
                    if (!d->isVariadicGeneric) continue;
                    if (d->genericParams.size() != 1) continue;

                    // Build mangled name from all type args.
                    std::string mangledName = annotation.name + "<";
                    std::vector<VyxTypePtr> concreteTypes;
                    for (size_t i = 0; i < typeArgs.size(); ++i) {
                        if (i > 0) mangledName += ",";
                        if (typeArgs[i]) {
                            auto ct = resolveType(*typeArgs[i]);
                            concreteTypes.push_back(ct);
                            mangledName += ct ? ct->toString() : "?";
                        } else {
                            concreteTypes.push_back(nullptr);
                            mangledName += "?";
                        }
                    }
                    mangledName += ">";

                    auto existingType = symbols_.lookupType(mangledName);
                    if (existingType) return existingType;

                    auto instType = std::make_shared<VyxType>();
                    instType->kind = (d->kind == DeclKind::Struct) ? VyxTypeKind::Struct : VyxTypeKind::Class;
                    instType->name = mangledName;

                    // Populate instType->fields so that member-access type-checking
                    // (`p.a`) can resolve field types inline. For PackIndexType
                    // annotations, substitute directly from concreteTypes[index].
                    // For other annotation kinds, fall through to resolveType.
                    auto& varFields = (d->kind == DeclKind::Struct)
                        ? d->as<StructDecl>()->fields
                        : d->as<ClassDecl>()->fields;
                    for (auto& field : varFields) {
                        VyxType::Field f;
                        f.name = field.name;
                        f.isPublic = (field.visibility == Visibility::Public);
                        if (field.type) {
                            if (field.type->kind == TypeAnnotationKind::PackIndex) {
                                auto* pi = field.type->as<PackIndexType>();
                                if (pi->indexExpr &&
                                    pi->indexExpr->kind == ExprKind::IntLiteral) {
                                    int64_t idx = pi->indexExpr->as<IntLiteralExpr>()->value;
                                    if (idx >= 0 && static_cast<size_t>(idx) < concreteTypes.size()
                                        && concreteTypes[static_cast<size_t>(idx)]) {
                                        f.type = concreteTypes[static_cast<size_t>(idx)];
                                    } else {
                                        f.type = types::makeUnknown();
                                    }
                                } else {
                                    f.type = types::makeUnknown();
                                }
                            } else {
                                // Non-pack-index field: use pack name as generic so
                                // activeGenericParams_ recognises it during resolution.
                                activeGenericParams_.insert(d->genericParams[0]);
                                f.type = resolveType(*field.type);
                                activeGenericParams_.erase(d->genericParams[0]);
                            }
                        }
                        instType->fields.push_back(f);
                        for (auto& extra : field.extraNames) {
                            VyxType::Field ef = f;
                            ef.name = extra;
                            instType->fields.push_back(ef);
                        }
                    }

                    symbols_.registerType(mangledName, instType);

                    if (instantiatedClassTemplates_.insert(mangledName).second) {
                        instantiateClassTemplate(*d, mangledName, concreteTypes,
                                                 annotation.location, {});
                    }
                    return instType;
                }
            }

            // ── Generic ErrorDef (e.g. stdlib Result<T,E> / Option<T>, user
            // `enum Either<L,R>`) ────────────────────────────────────────────
            // Without this branch, a `Result<i32, string>` return annotation
            // falls to resolveNamedType("Result", ...) which drops the type
            // args and yields bare `ErrorType{name="Result", paramTypes=[]}`.
            // Downstream Mono uses `recvType->paramTypes` to compute the
            // per-instantiation receiver (e.g. `Result<i32,string>` → wide
            // `%__Result_32` struct); with paramTypes empty, the method body
            // falls back to the shared 16-byte `%__Result` struct and
            // mis-reads the payload for wide-E variants. Preserving
            // paramTypes here lets requestMethodInstantiation pick the
            // parameterised receiver and CodeGenMatch dereference the
            // correct per-instantiation layout.
            //
            // The helpers `isResultLike` / `isOptionLike` accept both
            // `VyxTypeKind::Class` (Sema factory form) and ErrorType (this
            // path) to keep downstream dispatch uniform.
            if (unit_) {
                for (auto& d : unit_->declarations) {
                    if (!d || d->kind != DeclKind::ErrorDef) continue;
                    if (d->name != annotation.name) continue;
                    if (d->genericParams.size() != typeArgs.size()) continue;
                    std::string mangledName = annotation.name + "<";
                    std::vector<VyxTypePtr> concreteTypes;
                    concreteTypes.reserve(typeArgs.size());
                    for (size_t i = 0; i < typeArgs.size(); ++i) {
                        if (i > 0) mangledName += ",";
                        auto ct = resolveTypeArgChecked(i);
                        concreteTypes.push_back(ct);
                        mangledName += ct ? ct->toString() : "?";
                    }
                    mangledName += ">";
                    if (auto existing = symbols_.lookupType(mangledName))
                        return existing;
                    auto instType = std::make_shared<VyxType>();
                    instType->kind = VyxTypeKind::ErrorType;
                    instType->name = mangledName;
                    instType->paramTypes = concreteTypes;
                    // Copy the enum's variant names so `isResultLike` /
                    // `isOptionLike` helpers (which inspect t->name prefix)
                    // continue to work AND `.errorVariants` stays populated
                    // for exhaustiveness checks if any consumer cares.
                    auto* ed = d->as<ErrorDefDecl>();
                    instType->errorVariants = ed->variants;
                    symbols_.registerType(mangledName, instType);
                    return instType;
                }
            }

            // ── Builtin container fallbacks ──────────────────────────────────
            // If the user-defined class/struct lookup above found nothing (the
            // container template is a compiler-builtin, not declared in source),
            // map well-known container names to their intrinsic VyxType kinds
            // exactly as TemplateResolver::resolveTypeAnnotation does.  Without
            // these fallbacks `Box<Node>`, `Vec<i32>`, etc. fall through to
            // resolveNamedType which emits "undefined type 'Box'" because the
            // builtins are never placed in unit_->declarations.
            {
                const std::string& base = annotation.name;
                const size_t nArgs = typeArgs.size();
                if (base == "Vec" && nArgs == 1)
                    return types::makeDynArray(resolveTypeArgChecked(0));
                if (base == "Box" && nArgs == 1)
                    return types::makeBoxPtr(resolveTypeArgChecked(0));
                if (base == "Ref" && nArgs == 1)
                    return types::makeRefPtr(resolveTypeArgChecked(0));
                if (base == "Scope" && nArgs == 1)
                    return types::makeScopePtr(resolveTypeArgChecked(0));
                if (base == "Stack" && nArgs == 1)
                    return types::makeStack(resolveTypeArgChecked(0));
                if (base == "Queue" && nArgs == 1)
                    return types::makeQueue(resolveTypeArgChecked(0));
                if (base == "Set" && nArgs == 1)
                    return types::makeSet(resolveTypeArgChecked(0));
                if (base == "UnorderedSet" && nArgs == 1)
                    return types::makeUnorderedSet(resolveTypeArgChecked(0));
                if (base == "Dict" && nArgs == 2)
                    return types::makeDict(resolveTypeArgChecked(0), resolveTypeArgChecked(1));
                if (base == "UnorderedMap" && nArgs == 2)
                    return types::makeUnorderedMap(resolveTypeArgChecked(0), resolveTypeArgChecked(1));
                if (base == "Delegate" && nArgs == 1)
                    return types::makeDelegate(resolveTypeArgChecked(0));
                if (base == "Event" && nArgs == 1)
                    return types::makeEvent(resolveTypeArgChecked(0));
            }

            return resolveNamedType(annotation.name, annotation.location);
        }

        case TypeAnnotationKind::Union: {
            auto* ut = annotation.as<UnionType>();
            std::vector<VyxTypePtr> members;
            for (size_t ui = 0; ui < ut->members.size(); ++ui) {
                auto& member = ut->members[ui];
                if (!member) {
                    diag_.error(annotation.location, "union type: missing member type at position {}", ui);
                    continue;
                }
                members.push_back(resolveType(*member));
            }
            return types::makeUnion(std::move(members));
        }

        case TypeAnnotationKind::PackIndex: {
            // P2 C6: variadic type-pack indexing must be erased by
            // SemaGeneric's variadic instantiation pass before any
            // concrete type resolution. Reaching here means we're
            // resolving a `Ts[i]` annotation in a non-variadic
            // context (e.g. an unrelated function or a stale clone).
            // Emit a precise diagnostic and degrade to Unknown so we
            // can keep checking the rest of the program.
            auto* pi = annotation.as<PackIndexType>();
            diag_.error(annotation.location,
                "type-pack index '{}[...]' used outside a variadic instantiation",
                pi->packName);
            return types::makeUnknown();
        }

        case TypeAnnotationKind::Dependent: {
            // P2-generics C5: `T::Item` — resolve a dependent associated type.
            //
            // Resolution order:
            //   1. Check implAssocByTarget_ for "BaseName::MemberName". This
            //      fires when T has been fully bound to a concrete class that
            //      provided `type Item = X;` in its body or impl block. The
            //      key uses `baseName` directly when T itself is concrete
            //      (e.g. `CountUp::Item`).
            //   1.5 Generic class template instantiation (P2b Wave 3+):
            //      If baseName looks like a monomorphised name "Iter<i32>",
            //      strip the angle-bracket suffix to get "Iter", look up
            //      implAssocByTemplate_["Iter"]["Item"] → raw annotation T,
            //      then substitute T → i32 using the class instantiation's
            //      type arguments parsed from the mangled name.
            //   2. Fall back to implAssocByTrait_ for the interface default.
            //   3. If T is still an active generic parameter return a Generic
            //      placeholder named "T::Item" so downstream Mono / Sema can
            //      substitute it at instantiation time.
            //   4. Otherwise emit a diagnostic and return Unknown.
            auto* dt = annotation.as<DependentType>();
            const std::string key = dt->baseName + "::" + dt->memberName;

            // 1. Concrete target binding (from a class body or impl block).
            auto targetIt = implAssocByTarget_.find(key);
            if (targetIt != implAssocByTarget_.end() && targetIt->second) {
                return targetIt->second;
            }

            // 1.5 Generic-class-template instantiation: "Iter<i32>::Item"
            // baseName may be a monomorphised name like "Iter<i32>".
            // Strip to base and try implAssocByTemplate_.
            {
                auto lt = dt->baseName.find('<');
                if (lt != std::string::npos) {
                    std::string baseName = dt->baseName.substr(0, lt);
                    auto tmplIt = implAssocByTemplate_.find(baseName);
                    if (tmplIt != implAssocByTemplate_.end()) {
                        // Look up the assoc-type annotation for this member.
                        auto memberIt = tmplIt->second.find(dt->memberName);
                        if (memberIt != tmplIt->second.end() && memberIt->second) {
                            // We have the raw annotation (e.g. `T`).
                            // Parse the concrete type args from the mangled baseName.
                            // Find the template decl to get the param names.
                            const TypeAnnotation* rawAnn = memberIt->second;
                            if (unit_) {
                                for (auto& d : unit_->declarations) {
                                    if (!d || d->name != baseName) continue;
                                    if (d->kind != DeclKind::Class &&
                                        d->kind != DeclKind::Struct) continue;
                                    if (d->genericParams.empty()) continue;
                                    // Build a substitution from the monomorphised args.
                                    // The args are encoded inside "<...>" in baseName.
                                    // Use the same logic as instantiateClassTemplate:
                                    // parse the args string.
                                    std::string argsStr = dt->baseName.substr(lt + 1);
                                    if (!argsStr.empty() && argsStr.back() == '>')
                                        argsStr.pop_back();
                                    // Split on comma (depth-aware for nested generics).
                                    std::vector<std::string> argNames;
                                    {
                                        int depth = 0;
                                        std::string cur;
                                        for (char c : argsStr) {
                                            if (c == '<') { ++depth; cur += c; }
                                            else if (c == '>') { --depth; cur += c; }
                                            else if (c == ',' && depth == 0) {
                                                argNames.push_back(cur); cur.clear();
                                            } else { cur += c; }
                                        }
                                        if (!cur.empty()) argNames.push_back(cur);
                                    }
                                    // Build substitution: param[i] -> lookupType(argNames[i])
                                    GenericSubstitution subst;
                                    for (size_t i = 0; i < d->genericParams.size() &&
                                                       i < argNames.size(); ++i) {
                                        auto argType = symbols_.lookupType(argNames[i]);
                                        if (!argType) {
                                            // Built-in primitive names won't be in the
                                            // symbol table; construct them on the fly.
                                            argType = resolveNamedType(argNames[i],
                                                                       annotation.location);
                                        }
                                        if (argType) subst.add(d->genericParams[i], argType);
                                    }
                                    auto result = templateResolver_.substituteType(*rawAnn, subst);
                                    if (result && result->kind != VyxTypeKind::Generic) {
                                        return result;
                                    }
                                    // If still generic (e.g. partially-bound), fall through.
                                    break;
                                }
                            }
                        }
                    }
                }
            }

            // 2. Interface default.
            auto traitIt = implAssocByTrait_.find(key);
            if (traitIt != implAssocByTrait_.end() && traitIt->second) {
                return traitIt->second;
            }

            // 3. Still-generic base parameter: defer to Mono time.
            if (activeGenericParams_.contains(dt->baseName)) {
                auto t = std::make_shared<VyxType>();
                t->kind = VyxTypeKind::Generic;
                t->name = key; // e.g. "T::Item"
                return t;
            }

            // 3b. BUG-LV-14: `Self::Item` referenced from inside a trait
            // default-method body (or its clone into an implementing class)
            // — Self may not be in `activeGenericParams_` if we're walking
            // a body cloned onto a concrete class. First try resolving via
            // the enclosing class (currentClassType_) → its concrete assoc
            // binding; if that fails, treat as a still-generic placeholder
            // so Mono substitutes when the trait is bound.
            if (dt->baseName == "Self") {
                if (currentClassType_) {
                    const std::string concKey =
                        currentClassType_->name + "::" + dt->memberName;
                    auto cIt = implAssocByTarget_.find(concKey);
                    if (cIt != implAssocByTarget_.end() && cIt->second) {
                        return cIt->second;
                    }
                }
                auto t = std::make_shared<VyxType>();
                t->kind = VyxTypeKind::Generic;
                t->name = key; // e.g. "Self::Item"
                return t;
            }

            // 4. Completely unresolvable — emit diagnostic.
            diag_.error(annotation.location,
                "cannot resolve associated type '{}' on '{}': "
                "no binding found in any impl block or class body",
                dt->memberName, dt->baseName);
            return types::makeUnknown();
        }
    }
    diag_.error(annotation.location, "internal: unsupported type annotation kind in resolveType ({})",
        static_cast<int>(annotation.kind));
    return types::makeUnknown();
}

VyxTypePtr Sema::resolveNamedType(const std::string& name, SourceLocation loc) {
    if (name == "_") return types::makeUnknown();
    if (name == "fn") {
        auto t = std::make_shared<VyxType>();
        t->kind = VyxTypeKind::Function;
        t->name = "fn";
        return t;
    }
    // `Self` inside a trait / class / interface body: treat as an opaque
    // generic standing in for the implementing type. Mirrors Rust's `Self`.
    // Sema downstream already handles Generic widely; Mono-time binding
    // happens when the trait is instantiated for a concrete receiver
    // (e.g. `fma::<i32>` → T=i32 → Self=i32 for any impl on i32).
    if (name == "Self") {
        auto t = std::make_shared<VyxType>();
        t->kind = VyxTypeKind::Generic;
        t->name = "Self";
        return t;
    }
    if (activeGenericParams_.contains(name)) {
        auto t = std::make_shared<VyxType>();
        t->kind = VyxTypeKind::Generic;
        t->name = name;
        return t;
    }
    // Const-generic parameter used in type-name position (e.g. inside a
    // turbofish `::<N>` where N is the caller's const param). Treat it as
    // a symbolic integer type so `resolveType` doesn't fail with
    // "undefined type 'N'". The actual integer value is resolved later
    // via `substConstIdentsInStmt` / Mono's env-lookup.
    if (activeConstGenericParams_.contains(name)) {
        auto t = std::make_shared<VyxType>();
        t->kind = VyxTypeKind::Generic;
        t->name = name;
        return t;
    }
    auto type = symbols_.lookupType(name);
    if (type) return type;

    auto aliasIt = importAliases_.find(name);
    if (aliasIt != importAliases_.end()) {
        type = symbols_.lookupType(aliasIt->second);
        if (type) return type;
    }

    auto importIt = importedSymbols_.find(name);
    if (importIt != importedSymbols_.end()) {
        type = symbols_.lookupType(importIt->second);
        if (type) return type;
    }

    if (!currentModule_.empty()) {
        type = symbols_.lookupType(currentModule_ + "." + name);
        if (type) return type;
    }

    std::string suggestion;
    int bestDist = 3;
    static const char* builtins[] = {"i8","i16","i32","i64","u8","u16","u32","u64",
        "f32","f64","bool","char","str","string","void","rawptr","isize","usize"};
    for (auto* b : builtins) {
        int d = levenshteinDistance(name, b);
        if (d < bestDist) { bestDist = d; suggestion = b; }
    }

    if (!suggestion.empty()) {
        diag_.error(loc, "undefined type '{}', did you mean '{}'?", name, suggestion);
    } else {
        diag_.error(loc, "undefined type '{}'", name);
    }
    return types::makeUnknown();
}

// ============================================================
//  Type checking utilities
// ============================================================

bool Sema::isRegisteredStringLangItem(const VyxType& type) const {
    if (type.kind != VyxTypeKind::Class) return false;

    const Decl* stringDecl = langItems_.find("string");
    return stringDecl && stringDecl->kind == DeclKind::Class &&
           type.name == stringDecl->name;
}

bool Sema::isAssignable(const VyxType& target, const VyxType& source, SourceLocation at) {
    if (target.kind == VyxTypeKind::Unknown || source.kind == VyxTypeKind::Unknown) return true;
    if (target.kind == VyxTypeKind::Generic || source.kind == VyxTypeKind::Generic) return true;
    if (target.isEqual(source)) return true;

    // The registered stdlib String class shares the leading ABI layout of the
    // primitive `string`; do not grant this conversion to arbitrary classes.
    if (isStringType(target) && isRegisteredStringLangItem(source)) return true;

    // Fixed arrays: inside a template body the size expression may be a
    // const-generic identifier (e.g. `[N]T`) that hasn't been substituted
    // yet, so `resolveType` returned arraySize=0. Be lenient when at least
    // one side is a size-0 placeholder and the element type matches —
    // Mono/instantiateClassTemplate will enforce the real size at the
    // concrete instantiation site.
    if (target.kind == VyxTypeKind::Array && source.kind == VyxTypeKind::Array &&
        (target.arraySize == 0 || source.arraySize == 0) &&
        (!activeConstGenericParams_.empty() || !activeGenericParams_.empty())) {
        if (target.elementType && source.elementType &&
            isAssignable(*target.elementType, *source.elementType, at))
            return true;
        if (!target.elementType || !source.elementType)
            return true;
    }

    if (!target.name.empty() && !source.name.empty()) {
        auto tlt = target.name.find('<');
        auto slt = source.name.find('<');
        std::string tBase = (tlt != std::string::npos) ? target.name.substr(0, tlt) : target.name;
        std::string sBase = (slt != std::string::npos) ? source.name.substr(0, slt) : source.name;
        if (!tBase.empty() && tBase == sBase) {
            std::string tArgs = (tlt != std::string::npos) ? target.name.substr(tlt) : "";
            std::string sArgs = (slt != std::string::npos) ? source.name.substr(slt) : "";
            if (tArgs == sArgs || tArgs.empty() || sArgs.empty()) return true;
        }
    }

    bool targetIsPtr = target.kind == VyxTypeKind::Pointer || target.kind == VyxTypeKind::RawPtr;
    bool sourceIsPtr = source.kind == VyxTypeKind::Pointer || source.kind == VyxTypeKind::RawPtr ||
                       source.kind == VyxTypeKind::Reference;
    if (targetIsPtr && sourceIsPtr) return true;

    if (isResultLike(target)) {
        if (auto okTy = resultOk(target)) {
            if (isAssignable(*okTy, source, at)) return true;
        }
    }

    if (target.kind == VyxTypeKind::Union) {
        for (auto& member : target.tupleTypes) {
            if (member && isAssignable(*member, source, at)) return true;
        }
    }

    if (target.kind == VyxTypeKind::Interface && source.kind == VyxTypeKind::Class) {
        for (auto& iface : source.interfaceTypes) {
            if (iface && iface->name == target.name) return true;
        }
        for (auto& method : target.methods) {
            bool found = false;
            for (auto& sm : source.methods) {
                if (sm.name != method.name) continue;
                if (sm.paramTypes.size() != method.paramTypes.size()) continue;
                bool sigMatch = true;
                for (size_t i = 0; i < sm.paramTypes.size(); ++i) {
                    if (sm.paramTypes[i] && method.paramTypes[i] &&
                        !sm.paramTypes[i]->isEqual(*method.paramTypes[i])) {
                        sigMatch = false;
                        break;
                    }
                }
                if (sigMatch && sm.returnType && method.returnType &&
                    !sm.returnType->isEqual(*method.returnType)) {
                    sigMatch = false;
                }
                if (sigMatch) { found = true; break; }
            }
            if (!found) {
                diag_.error(semaInternalSourceLocation(),
                    "class '{}' does not satisfy interface '{}': missing or mismatched method '{}'",
                    source.name, target.name, method.name);
                return false;
            }
        }
        return true;
    }

    if (target.isIntegral() && source.isIntegral()) {
        if (target.bitWidth < source.bitWidth) {
            diag_.warning(pickAssignWarnLoc(at), "implicit narrowing conversion from '{}' to '{}'",
                source.toString(), target.toString());
        }
        return true;
    }
    if (target.isFloatingPoint() && source.isFloatingPoint()) {
        if (target.bitWidth < source.bitWidth) {
            diag_.warning(pickAssignWarnLoc(at), "implicit narrowing conversion from '{}' to '{}'",
                source.toString(), target.toString());
        }
        return true;
    }
    if (target.isFloatingPoint() && source.isNumeric()) return true;

    return false;
}

VyxTypePtr Sema::commonType(const VyxType& a, const VyxType& b) {
    if (a.kind == VyxTypeKind::Unknown) return std::make_shared<VyxType>(b);
    if (b.kind == VyxTypeKind::Unknown) return std::make_shared<VyxType>(a);
    if (a.isEqual(b)) return std::make_shared<VyxType>(a);

    if (a.isFloatingPoint() && b.isIntegral()) return std::make_shared<VyxType>(a);
    if (b.isFloatingPoint() && a.isIntegral()) return std::make_shared<VyxType>(b);

    if (a.isIntegral() && b.isIntegral()) {
        return a.bitWidth >= b.bitWidth ? std::make_shared<VyxType>(a) : std::make_shared<VyxType>(b);
    }

    return std::make_shared<VyxType>(a);
}

bool Sema::checkNumericOp(const VyxType& type, SourceLocation loc) {
    if (!type.isNumeric()) {
        diag_.error(loc, "expected numeric type, got '{}'", type.toString());
        return false;
    }
    return true;
}

// ============================================================
//  Generic struct-init type inference
// ============================================================

// inferGenericStructInit: when the user writes `Cache { has_value: true, value: 42 }`
// without explicit turbofish type args, attempt to infer each generic parameter
// by matching field names against the class template's declared field types.
//
// Algorithm:
//   1. Find the primary template decl for `structName` in the translation unit.
//   2. For each template parameter P, look for a declared field whose type
//      annotation is the bare name P (e.g. `public value: V`).
//   3. If a matching field was supplied in the struct-init, use the analyzed
//      type of that field init expression as the concrete binding for P.
//   4. For parameters that cannot be inferred (no field references them), use
//      i64 as a safe default so the instantiation is concrete and codegen can
//      emit a complete type layout.
//   5. Build a GenericType annotation with the resolved args and call
//      resolveType() to trigger normal class-template instantiation.
//
// Returns nullptr when `structName` is not a generic template (caller keeps
// the bare type it already has).
VyxTypePtr Sema::inferGenericStructInit(
    const std::string& structName,
    const std::vector<std::pair<std::string, VyxTypePtr>>& fieldTypes,
    SourceLocation loc)
{
    if (!unit_) {
        return nullptr;
    }

    // Step 1: find the primary template decl (non-specialization, non-empty genericParams).
    const Decl* tmpl = nullptr;
    for (auto& d : unit_->declarations) {
        if (!d) continue;
        if (d->name != structName) continue;
        if (d->genericParams.empty()) continue;
        if (d->kind != DeclKind::Class && d->kind != DeclKind::Struct) continue;
        if (d->isFullSpecialization || d->isPartialSpecialization) continue;
        tmpl = d.get();
        break;
    }
    if (!tmpl) {
        return nullptr;
    }

    // Step 2: build fieldName → generic-param-name map from the template's field decls.
    // Only bare NamedType annotations that match a generic param are tracked.
    std::map<std::string, std::string> fieldToParam;
    {
        const std::vector<FieldDecl>* flds = nullptr;
        if (tmpl->kind == DeclKind::Class)
            flds = &tmpl->as<ClassDecl>()->fields;
        else
            flds = &tmpl->as<StructDecl>()->fields;

        for (auto& field : *flds) {
            if (!field.type) continue;
            if (field.type->kind != TypeAnnotationKind::Named) continue;
            const std::string& typeName = field.type->name;
            for (auto& gp : tmpl->genericParams) {
                if (typeName == gp) {
                    fieldToParam[field.name] = gp;
                    // Also handle extra alias names (e.g. `x, y, z: T`)
                    for (auto& extra : field.extraNames)
                        fieldToParam[extra] = gp;
                    break;
                }
            }
        }
    }

    // Step 3: infer concrete types for each param from the provided field inits.
    std::map<std::string, VyxTypePtr> inferred;
    for (auto& [fname, ftype] : fieldTypes) {
        auto it = fieldToParam.find(fname);
        if (it == fieldToParam.end()) continue;
        if (!ftype || ftype->kind == VyxTypeKind::Unknown ||
            ftype->kind == VyxTypeKind::Generic) continue;
        // Don't overwrite an already-inferred binding (first win).
        if (inferred.count(it->second) == 0)
            inferred[it->second] = ftype;
    }

    // Step 4 & 5: build GenericType annotation and resolve.
    auto gt = std::make_unique<GenericType>();
    gt->kind = TypeAnnotationKind::Generic;
    gt->name = structName;
    gt->location = loc;

    for (auto& gp : tmpl->genericParams) {
        auto iit = inferred.find(gp);
        TypePtr argAnn;
        if (iit != inferred.end()) {
            argAnn = convertTypeToAnnotation(iit->second);
        }
        if (!argAnn) {
            // Unbound param: default to i64 so the instantiation is fully concrete.
            auto fallback = std::make_unique<NamedType>();
            fallback->name = "i64";
            fallback->location = loc;
            argAnn = std::move(fallback);
        }
        gt->typeArgs.push_back(std::move(argAnn));
    }

    return resolveType(*gt);
}

// ============================================================
//  Template instantiation
// ============================================================


// ============================================================
//  Type annotation conversion
// ============================================================

TypePtr Sema::convertTypeToAnnotation(const VyxTypePtr& vyxType) {
    if (!vyxType) return nullptr;

    switch (vyxType->kind) {
        case VyxTypeKind::Void:
        case VyxTypeKind::Bool:
        case VyxTypeKind::Char:
        case VyxTypeKind::Integer:
        case VyxTypeKind::Float:
        case VyxTypeKind::Struct:
        case VyxTypeKind::Class:
        case VyxTypeKind::Interface: {
            // A Class VyxType whose mangled `name` still carries an
            // unresolved type parameter (e.g. `Iterator<U>` where U is a
            // method-level generic produced by TemplateResolver at partial
            // instantiation time) cannot be looked up as a concrete type by
            // CodeGen. Emit a `rawptr` fallback so `toLLVMType` treats the
            // slot as an opaque pointer; the concrete Iterator<U> layout is
            // irrelevant until the method is instantiated with concrete U.
            auto containsUnresolvedParam = [](const std::string& s) -> bool {
                auto lt = s.find('<');
                if (lt == std::string::npos) return false;
                size_t n = s.size();
                for (size_t i = lt + 1; i < n; ++i) {
                    if (!std::isupper(static_cast<unsigned char>(s[i]))) continue;
                    bool leftBoundary = (s[i - 1] == '<' || s[i - 1] == ',');
                    bool rightBoundary = (i + 1 < n) && (s[i + 1] == '>' || s[i + 1] == ',');
                    if (leftBoundary && rightBoundary) return true;
                }
                return false;
            };
            if (vyxType->kind == VyxTypeKind::Class &&
                containsUnresolvedParam(vyxType->name)) {
                auto ann = std::make_unique<NamedType>();
                ann->name = "rawptr";
                return ann;
            }
            // When a Class/Struct/Interface has a mangled name like
            // "Iterator<Vec<i32>>" (produced by TemplateResolver::substituteType
            // for user-defined generic classes), decompose it into a structured
            // GenericType AST node so Sema's resolveType can process it through
            // the Generic branch (which performs class template instantiation)
            // instead of through resolveNamedType which would fail with
            // "undefined type 'Iterator<Vec<i32>>'".
            //
            // The mini-parser splits the mangled name depth-aware on commas
            // to handle nested generics like "Foo<Bar<i32>,Baz<u64>>".
            if ((vyxType->kind == VyxTypeKind::Class ||
                 vyxType->kind == VyxTypeKind::Struct ||
                 vyxType->kind == VyxTypeKind::Interface) &&
                vyxType->name.find('<') != std::string::npos) {
                auto lt = vyxType->name.find('<');
                std::string baseName = vyxType->name.substr(0, lt);
                // Strip surrounding '<' ... '>'
                std::string inner = vyxType->name.substr(lt + 1);
                if (!inner.empty() && inner.back() == '>') inner.pop_back();
                // Depth-aware comma split
                std::vector<std::string> argTokens;
                {
                    int depth = 0;
                    std::string cur;
                    for (char c : inner) {
                        if (c == '<') { ++depth; cur += c; }
                        else if (c == '>') { --depth; cur += c; }
                        else if (c == ',' && depth == 0) {
                            argTokens.push_back(cur); cur.clear();
                        } else { cur += c; }
                    }
                    if (!cur.empty()) argTokens.push_back(cur);
                }
                if (!argTokens.empty()) {
                    // Recursively build annotation for each arg token.
                    std::function<TypePtr(const std::string&)> buildAnn =
                        [&](const std::string& token) -> TypePtr {
                        auto tlt = token.find('<');
                        if (tlt == std::string::npos) {
                            auto n = std::make_unique<NamedType>();
                            n->name = token;
                            return n;
                        }
                        std::string tBase = token.substr(0, tlt);
                        std::string tInner = token.substr(tlt + 1);
                        if (!tInner.empty() && tInner.back() == '>') tInner.pop_back();
                        std::vector<std::string> tArgs;
                        {
                            int d = 0; std::string tc;
                            for (char c : tInner) {
                                if (c == '<') { ++d; tc += c; }
                                else if (c == '>') { --d; tc += c; }
                                else if (c == ',' && d == 0) { tArgs.push_back(tc); tc.clear(); }
                                else tc += c;
                            }
                            if (!tc.empty()) tArgs.push_back(tc);
                        }
                        auto g = std::make_unique<GenericType>();
                        g->name = tBase;
                        for (auto& ta : tArgs) g->typeArgs.push_back(buildAnn(ta));
                        return g;
                    };
                    auto g = std::make_unique<GenericType>();
                    g->name = baseName;
                    for (auto& tok : argTokens) g->typeArgs.push_back(buildAnn(tok));
                    return g;
                }
            }
            auto ann = std::make_unique<NamedType>();
            ann->name = vyxType->name;
            return ann;
        }
        case VyxTypeKind::RawPtr:
        case VyxTypeKind::Pointer: {
            auto ann = std::make_unique<PointerType>();
            if (vyxType->pointeeType) ann->innerType = convertTypeToAnnotation(vyxType->pointeeType);
            return ann;
        }
        case VyxTypeKind::Reference: {
            auto ann = std::make_unique<ReferenceType>();
            ann->isMutable = vyxType->isMutable;
            if (vyxType->pointeeType) ann->innerType = convertTypeToAnnotation(vyxType->pointeeType);
            return ann;
        }
        case VyxTypeKind::Array: {
            auto ann = std::make_unique<ArrayType>();
            if (vyxType->elementType) ann->elementType = convertTypeToAnnotation(vyxType->elementType);
            if (vyxType->arraySize > 0) ann->size = ast::makeIntLiteral({}, vyxType->arraySize);
            return ann;
        }
        case VyxTypeKind::Tuple: {
            auto ann = std::make_unique<TupleType>();
            for (const auto& elemType : vyxType->tupleTypes) {
                ann->elements.push_back(convertTypeToAnnotation(elemType));
            }
            return ann;
        }
        case VyxTypeKind::Function: {
            auto ann = std::make_unique<FunctionType>();
            ann->name = "fn";
            for (const auto& paramType : vyxType->paramTypes) {
                ann->paramTypes.push_back(convertTypeToAnnotation(paramType));
            }
            if (vyxType->returnType) ann->returnType = convertTypeToAnnotation(vyxType->returnType);
            return ann;
        }
        // ── Container kinds: Option/Result/Vec/Dict/Stack/Queue/Set/Ref/
        //    Scope/Box/Delegate/Event all arrive as Class-kind with the mangled
        //    name `Foo<args>` and paramTypes holding the decomposed args. They
        //    flow through the Class branch above which unpacks the mangled
        //    name back into a structured GenericType annotation. After R5
        //    phase 4 batch 4, no dedicated kinds remain for containers.
        case VyxTypeKind::Union: {
            auto ann = std::make_unique<UnionType>();
            for (const auto& member : vyxType->tupleTypes) {
                ann->members.push_back(convertTypeToAnnotation(member));
            }
            return ann;
        }
        case VyxTypeKind::Generic: {
            // P2-generics C5: a Generic VyxType whose name contains "::"
            // is a dependent associated-type placeholder ("T::Item"). Convert
            // back to a DependentType annotation so substituteType can handle
            // it correctly in recursive instantiation scenarios.
            auto sep = vyxType->name.find("::");
            if (sep != std::string::npos) {
                auto ann = std::make_unique<DependentType>();
                ann->name = vyxType->name;
                ann->baseName = vyxType->name.substr(0, sep);
                ann->memberName = vyxType->name.substr(sep + 2);
                return ann;
            }
            // Unresolved generic type parameter ("T", "U"): emit a NamedType.
            // A GenericType with empty typeArgs would later go through the
            // mangleGeneric fallback and produce `T<>`, which then mismatches
            // the spelling in class bodies / return types. NamedType round-trips
            // cleanly: resolveTypeAnnotation detects the name via isGenericName
            // (or the caller-visible genericParams list) and re-emits the
            // correct VyxTypeKind::Generic.
            auto ann = std::make_unique<NamedType>();
            ann->name = vyxType->name;
            return ann;
        }
        case VyxTypeKind::ErrorType:
        case VyxTypeKind::Unknown:
        default: {
            auto ann = std::make_unique<NamedType>();
            ann->name = vyxType->name.empty() ? "unknown" : vyxType->name;
            return ann;
        }
    }
}

TypePtr Sema::cloneTypeAnnotation(const TypeAnnotation& src) {
    return vyx::cloneTypeAnnotation(src);
}
} // namespace vyx
