#include "Sema.h"
#include "SemaCloneHelpers.h"
#include "../Common/StringUtils.h"
#include <algorithm>
#include <functional>
#include <memory>
#include <map>
#include <set>
#include <string_view>

namespace vyx {
// ============================================================
//  Declaration analysis
// ============================================================

static bool hasAttribute(const Decl& decl, std::string_view name) {
    for (const auto& [attrName, _] : decl.attributes) {
        if (attrName == name) return true;
    }
    return false;
}

void Sema::checkMethodGenericShadowing(const Decl& outerDecl, const MethodDecl& method) {
    if (outerDecl.genericParams.empty() || method.genericParams.empty()) return;

    std::set<std::string> outerGenerics(
        outerDecl.genericParams.begin(), outerDecl.genericParams.end());

    const char* outerKindStr = "declaration";
    switch (outerDecl.kind) {
        case DeclKind::Class:     outerKindStr = "class";     break;
        case DeclKind::Struct:    outerKindStr = "struct";    break;
        case DeclKind::Interface: outerKindStr = "interface"; break;
        case DeclKind::ErrorDef:  outerKindStr = "errordef";  break;
        default: break;
    }

    for (auto& mgp : method.genericParams) {
        if (!outerGenerics.contains(mgp)) continue;
        diag_.error(method.location,
            "method generic parameter '{}' shadows generic parameter of enclosing "
            "{} '{}'; rename the method parameter (e.g. 'fn {}<U>(...)') to avoid "
            "ambiguous type substitution between the class-level and method-level '{}'",
            mgp, outerKindStr, outerDecl.name, method.name, mgp);
    }
}

void Sema::analyzeDecl(Decl& decl) {
    switch (decl.kind) {
        case DeclKind::Function:  analyzeFunctionDecl(decl); break;
        case DeclKind::Struct:    analyzeStructDecl(decl); break;
        case DeclKind::Class:     analyzeClassDecl(decl); break;
        case DeclKind::Interface: {
            auto* ifaceDecl = decl.as<InterfaceDecl>();
            auto type = symbols_.lookupType(decl.name);
            if (type) {
                // P2-generics C5: push associated type names as opaque generic
                // parameters so that method signatures referencing them
                // (e.g. `fn next(self) -> Option<Item>`) resolve to Generic("Item")
                // instead of triggering "undefined type 'Item'" errors.
                for (auto& at : ifaceDecl->associatedTypes) {
                    activeGenericParams_.insert(at.name);
                }
                // Also push `Self` so dependent-type references like
                // `Self::Item` resolve to Generic("Self::Item") via step 3 of
                // resolveType's Dependent branch, and get substituted at
                // trait-impl check / Mono time when Self is bound to the
                // implementing class.
                const bool pushedSelfForIface =
                    activeGenericParams_.insert("Self").second;
                // Generic interface parameters (`interface Container<T> { ... }`):
                // push T into the active scope so method signatures referencing
                // T resolve to Generic("T") instead of "undefined type 'T'".
                // Mirrors the ErrorDef handler below.
                for (auto& gp : decl.genericParams) {
                    activeGenericParams_.insert(gp);
                }

                for (auto& method : ifaceDecl->methods) {
                    checkMethodGenericShadowing(decl, method);
                    // Push method-level generic params (e.g. `fn ratio<T>(...)`
                    // inside a trait) so resolveType sees `T` as a
                    // Generic-param, not "undefined type". Pop after the
                    // signature is resolved so trait-level generics stay
                    // the only visible set for the next method.
                    std::vector<std::string> mPushed;
                    for (auto& mgp : method.genericParams) {
                        if (activeGenericParams_.insert(mgp).second)
                            mPushed.push_back(mgp);
                    }
                    VyxType::MethodSig sig;
                    sig.name = method.name;
                    sig.returnType = method.returnType ? resolveType(*method.returnType) : types::makeVoid();
                    for (auto& param : method.params) {
                        sig.paramTypes.push_back(param.type ? resolveType(*param.type) : types::makeUnknown());
                    }
                    for (auto& mgp : mPushed) activeGenericParams_.erase(mgp);
                    type->methods.push_back(std::move(sig));
                }

                // Pop the associated type names + generic params — they are
                // scoped to this interface and should not persist into other decls.
                for (auto& at : ifaceDecl->associatedTypes) {
                    activeGenericParams_.erase(at.name);
                }
                for (auto& gp : decl.genericParams) {
                    activeGenericParams_.erase(gp);
                }
                if (pushedSelfForIface) activeGenericParams_.erase("Self");
                // Default-body analysis deferred to the dedicated pass.
            }
            break;
        }
        case DeclKind::ErrorDef: {
            auto* errDecl = decl.as<ErrorDefDecl>();
            auto type = symbols_.lookupType(decl.name);
            if (type) {
                if (!decl.genericParams.empty()) {
                    for (auto& gp : decl.genericParams) activeGenericParams_.insert(gp);
                    std::set<std::string> gpSet(decl.genericParams.begin(), decl.genericParams.end());
                    auto resolveGenericType = [&](const TypeAnnotation& ann) -> VyxTypePtr {
                        if (gpSet.contains(ann.name)) {
                            auto t = std::make_shared<VyxType>();
                            t->kind = VyxTypeKind::Generic;
                            t->name = ann.name;
                            return t;
                        }
                        if (ann.kind == TypeAnnotationKind::Generic &&
                            !ann.as<GenericType>()->typeArgs.empty()) return types::makeUnknown();
                        return resolveType(ann);
                    };
                    for (auto& method : errDecl->methods) {
                        checkMethodGenericShadowing(decl, method);
                        for (auto& mgp : method.genericParams) activeGenericParams_.insert(mgp);
                        VyxType::MethodSig sig;
                        sig.name = method.name;
                        sig.isPublic = true;
                        sig.returnType = method.returnType
                            ? resolveGenericType(*method.returnType) : types::makeVoid();
                        for (auto& param : method.params)
                            sig.paramTypes.push_back(param.type ? resolveGenericType(*param.type) : types::makeUnknown());
                        type->methods.push_back(std::move(sig));
                        for (auto& mgp : method.genericParams) activeGenericParams_.erase(mgp);
                    }
                    for (auto& gp : decl.genericParams) activeGenericParams_.erase(gp);
                } else {
                    for (auto& method : errDecl->methods) {
                        VyxType::MethodSig sig;
                        sig.name = method.name;
                        sig.isPublic = true;
                        sig.returnType = method.returnType ? resolveType(*method.returnType) : types::makeVoid();
                        for (auto& param : method.params)
                            sig.paramTypes.push_back(param.type ? resolveType(*param.type) : types::makeUnknown());
                        type->methods.push_back(std::move(sig));
                    }
                }
                // Body analysis deferred to the dedicated method-bodies pass
                // run after ALL decl signatures (fields, nested types, etc.)
                // are registered. See Sema::analyze().
            }
            break;
        }
        case DeclKind::Import: {
            auto* imp = decl.as<ImportDecl>();
            if (!imp->importNames.empty() && imp->importNames[0] == "__module" && !imp->importPath.empty()) {
                if (!decl.isImported) {
                    currentModule_ = imp->importPath[0];
                }
            }
            for (auto& name : imp->importNames) {
                if (name.starts_with("__alias:")) {
                    std::string alias = name.substr(8);
                    std::string fullPath;
                    for (size_t i = 0; i < imp->importPath.size(); ++i) {
                        if (i > 0) fullPath += ".";
                        fullPath += imp->importPath[i];
                    }
                    importAliases_[alias] = fullPath;
                }
            }
            break;
        }
        case DeclKind::TypeAlias: {
            auto* ta = decl.as<TypeAliasDecl>();
            if (ta->aliasType) {
                if (ta->isNewtype) {
                    auto type = std::make_shared<VyxType>();
                    type->name = decl.name;
                    type->kind = VyxTypeKind::Struct;
                    symbols_.registerType(decl.name, type);
                } else {
                    auto resolvedType = resolveType(*ta->aliasType);
                    symbols_.registerType(decl.name, resolvedType);
                }
            }
            break;
        }
        case DeclKind::ExternBlock: {
            auto* eb = decl.as<ExternBlockDecl>();
            for (auto& child : eb->externDecls) {
                if (!child) continue;
                if (child->kind == DeclKind::Function) {
                    analyzeFunctionDecl(*child);
                } else if (child->kind == DeclKind::Class || child->kind == DeclKind::Struct) {
                    externClassNames_.insert(child->name);
                }
            }
            break;
        }
        case DeclKind::GlobalVar: {
            auto* gv = decl.as<GlobalVarDecl>();
            VyxTypePtr varType = nullptr;
            if (gv->varType) {
                varType = resolveType(*gv->varType);
            }
            if (gv->initBody && gv->initBody->kind == StmtKind::ExprStmt) {
                auto* es = gv->initBody->as<ExprStmt>();
                if (es->expr) {
                    auto initType = analyzeExpr(*es->expr);
                    if (!varType) varType = initType;
                }
            }
            if (!varType) varType = types::makeUnknown();
            Symbol sym;
            sym.name = decl.name;
            sym.type = varType;
            sym.isConst = !gv->isMutableVar;
            sym.declLocation = decl.location;
            symbols_.declare(decl.name, std::move(sym));
            break;
        }
        case DeclKind::Concept: {
            auto* conceptDecl = decl.as<ConceptDecl>();
            auto type = std::make_shared<VyxType>();
            type->kind = VyxTypeKind::Interface;
            type->name = decl.name;
            for (auto& req : conceptDecl->conceptRequires) {
                auto existingType = symbols_.lookupType(req);
                if (existingType && existingType->kind == VyxTypeKind::Interface) {
                    for (auto& m : existingType->methods) {
                        type->methods.push_back(m);
                    }
                } else {
                    VyxType::MethodSig sig;
                    sig.name = req;
                    sig.isPublic = true;
                    sig.returnType = types::makeUnknown();
                    type->methods.push_back(std::move(sig));
                }
            }
            symbols_.registerType(decl.name, type);
            break;
        }
        case DeclKind::Macro:
            break;
    }
}

void Sema::analyzeFunctionDecl(Decl& decl) {
    auto* fnDecl = decl.as<FunctionDecl>();

    if (fnDecl->body && fnDecl->body->kind == StmtKind::Block &&
        fnDecl->body->as<BlockStmt>()->statements.empty()) {
        diag_.warning(decl.location,
            "unimplemented empty function body: {}", decl.name);
    }

    if (!decl.genericParams.empty()) {
        // ODR: generic overloads collide when name + arity match in the same
        // namespace (the bodies can differ, but the call site cannot resolve
        // between them). Non-generic pre-registration has already run; this
        // path handles only the generic decls that Pass 2 skipped.
        const std::string& ns = moduleOfDecl(decl);
        if (checkFunctionRedefinition(ns, decl, /*paramTypes=*/{},
                                       /*isGeneric=*/true,
                                       /*arity=*/decl.genericParams.size())) {
            return;
        }
        VyxTypePtr retType = types::makeUnknown();
        Symbol fnSym;
        fnSym.name = decl.name;
        fnSym.isFunction = true;
        fnSym.returnType = retType;
        fnSym.type = retType;
        fnSym.declLocation = decl.location;
        fnSym.isGeneric = true;
        fnSym.genericArity = decl.genericParams.size();
        fnSym.paramCount = fnDecl->params.size();
        fnSym.borrowsArgs = hasAttribute(decl, "borrow_args");
        symbols_.declare(decl.name, fnSym);

        // Phase 9: also record the generic overload in overloadGroups_ so a
        // call site can prefer a non-generic exact match over a generic
        // candidate when both names coincide. Generic decls keep their
        // unmangled name (they go through Mono's existing instantiation
        // path); we record paramTypes from the raw annotations as Unknown
        // so candidate scoring counts them last.
        OverloadEntry entry;
        entry.decl = &decl;
        entry.paramTypes.assign(fnDecl->params.size(), types::makeUnknown());
        entry.mangledName.clear();
        entry.isGeneric = true;
        overloadGroups_[decl.name].push_back(std::move(entry));
        return;
    }

    VyxTypePtr retType = fnDecl->returnType
        ? resolveType(*fnDecl->returnType)
        : types::makeVoid();

    Symbol fnSym;
    fnSym.name = decl.name;
    fnSym.isFunction = true;
    fnSym.returnType = retType;
    fnSym.type = retType;
    fnSym.declLocation = decl.location;
    fnSym.isImported = decl.isImported;
    fnSym.isExported = decl.isExport;
    fnSym.borrowsArgs = hasAttribute(decl, "borrow_args");

    for (auto& param : fnDecl->params) {
        if (param.type) {
            // `void` is not a first-class type, so LLVM rejects it as a
            // parameter with the cryptic "Function arguments must have
            // first-class types!" verifier failure late in codegen.
            // Catch it at Sema time with a clear message — the common
            // cause is an auto-generated C binding that wrote `void`
            // where it meant `rawptr` (i.e. `void*`).
            if (param.type->kind == TypeAnnotationKind::Named &&
                param.type->name == "void") {
                diag_.error(param.type->location,
                    "parameter '{}' has type 'void'; did you mean 'rawptr'? "
                    "(`void*` in C-header bindings should be `rawptr`)",
                    param.name);
            }
            fnSym.paramTypes.push_back(resolveType(*param.type));
        } else {
            fnSym.paramTypes.push_back(types::makeUnknown());
        }
    }

    bool isUnsafeFn = false;
    bool isAutoWrapFn = false;
    for (auto& [an, av] : decl.attributes) {
        if (an == "unsafe") isUnsafeFn = true;
        else if (an == "auto_wrap") isAutoWrapFn = true;
    }
    fnSym.isUnsafe = isUnsafeFn;
    symbols_.declare(decl.name, fnSym);
    if (decl.isImported && !currentModule_.empty()) {
        std::string shortModule = currentModule_;
        auto lastDot = shortModule.rfind('.');
        if (lastDot != std::string::npos) shortModule = shortModule.substr(lastDot + 1);
        functionSourceModule_[decl.name] = shortModule;
    }

    if (fnDecl->isComptime) {
        comptimeFunctions_[decl.name] = &decl;
    }

    if (!fnDecl->body) return;

    symbols_.pushScope();
    auto savedReturn = currentReturnType_;
    auto savedReturnAnn = currentReturnAnnotation_;
    auto savedImportedScope = inImportedScope_;
    auto savedUnsafe = inUnsafe_;
    auto savedAutoWrap = currentFnAutoWrap_;
    currentReturnType_ = retType;
    currentReturnAnnotation_ = fnDecl->returnType.get();
    inImportedScope_ = decl.isImported;
    if (isUnsafeFn) inUnsafe_ = true;
    currentFnAutoWrap_ = isAutoWrapFn;

    for (size_t i = 0; i < fnDecl->params.size(); ++i) {
        std::string paramName = fnDecl->params[i].name;
        Symbol paramSym;
        paramSym.name = paramName;
        paramSym.type = fnSym.paramTypes[i];
        paramSym.isConst = !fnDecl->params[i].isMutRef;
        paramSym.declLocation = decl.location;
        symbols_.declare(paramName, std::move(paramSym));
    }

    analyzeStmt(*fnDecl->body);

    for (auto& [an, av] : decl.attributes) {
        if (an == "tailcall" && fnDecl->body) {
            bool hasTailCall = false;
            std::function<void(const Stmt&)> checkTail = [&](const Stmt& s) {
                if (s.kind == StmtKind::Return) {
                    auto* rs = s.as<ReturnStmt>();
                    if (rs->expr && rs->expr->kind == ExprKind::Call) {
                        auto* call = rs->expr->as<CallExpr>();
                        if (call->callee && call->callee->kind == ExprKind::Identifier &&
                            call->callee->as<IdentifierExpr>()->name == decl.name) {
                            hasTailCall = true;
                        }
                    }
                }
                if (s.kind == StmtKind::Block) {
                    for (auto& sub : s.as<BlockStmt>()->statements) { if (sub) checkTail(*sub); }
                }
                if (s.kind == StmtKind::If) {
                    auto* ifS = s.as<IfStmt>();
                    if (ifS->thenBranch) checkTail(*ifS->thenBranch);
                    if (ifS->elseBranch) checkTail(*ifS->elseBranch);
                }
            };
            checkTail(*fnDecl->body);
            if (!hasTailCall) {
                diag_.warning(decl.location,
                    "@[tailcall] function '{}' has no tail-recursive call to itself; stack overflow risk remains",
                    decl.name);
            }
        }
    }

    currentReturnType_ = savedReturn;
    currentReturnAnnotation_ = savedReturnAnn;
    inImportedScope_ = savedImportedScope;
    inUnsafe_ = savedUnsafe;
    currentFnAutoWrap_ = savedAutoWrap;
    symbols_.popScope();
}

void Sema::analyzeStructDecl(Decl& decl) {
    auto* sd = decl.as<StructDecl>();
    auto type = symbols_.lookupType(decl.name);
    if (!type) {
        diag_.error(decl.location, "internal: struct '{}' missing from symbol table during analysis", decl.name);
        return;
    }

    if (!decl.genericParams.empty()) {
        for (auto& gp : decl.genericParams) activeGenericParams_.insert(gp);
        std::set<std::string> gpSet(decl.genericParams.begin(), decl.genericParams.end());
        for (auto& field : sd->fields) {
            if (!field.type) continue;
            VyxType::Field f;
            f.name = field.name;
            f.isPublic = (field.visibility == Visibility::Public);
            if (gpSet.contains(field.type->name))
                f.type = std::make_shared<VyxType>(VyxType{.kind = VyxTypeKind::Generic, .name = field.type->name});
            else if (field.type->kind == TypeAnnotationKind::Generic &&
                     !field.type->as<GenericType>()->typeArgs.empty())
                f.type = types::makeUnknown();
            else
                f.type = resolveType(*field.type);
            type->fields.push_back(f);
            for (auto& extra : field.extraNames) {
                VyxType::Field ef = f;
                ef.name = extra;
                type->fields.push_back(ef);
            }
        }
        for (auto& gp : decl.genericParams) activeGenericParams_.erase(gp);
        return;
    }

    if (!sd->parentName.empty()) {
        auto parentType = symbols_.lookupType(sd->parentName);
        if (parentType) {
            for (auto& pf : parentType->fields) {
                type->fields.push_back(pf);
            }
        }
    }

    for (auto& field : sd->fields) {
        if (field.type) {
            auto resolvedType = resolveType(*field.type);
            VyxType::Field f;
            f.name = field.name;
            f.type = resolvedType;
            f.isPublic = (field.visibility == Visibility::Public);
            type->fields.push_back(f);
            for (auto& extra : field.extraNames) {
                VyxType::Field ef;
                ef.name = extra;
                ef.type = resolvedType;
                ef.isPublic = f.isPublic;
                type->fields.push_back(ef);
            }
        }
    }
}

void Sema::analyzeClassDecl(Decl& decl) {
    auto* cd = decl.as<ClassDecl>();

    // Phase 8: mirror the matching trait method's `isStatic` onto an impl
    // method that omits the `static` keyword.
    //
    // Rationale: Vyx's existing convention is "no `self` param → method
    // takes self implicitly" (e.g. `fn hash() -> i64` in std/hash.vyx
    // uses `self` in its body and is dispatched as `x.hash()`). We do
    // NOT want to change that convention blindly.  But when the trait
    // explicitly marked a method `static fn`, the impl should inherit
    // the static status even if the user forgot the keyword — otherwise
    // the signatures mismatch (trait says static; impl emits an implicit
    // self pointer) and CodeGen verification fails at the call site.
    //
    // Applies to any impl block (primitive or class target). Only
    // promotes when the trait's declaration itself carries `isStatic = true`.
    if (cd->isImplBlock) {
        for (auto& m : cd->methods) {
            if (m.isStatic) continue;
            bool traitIsStatic = false;
            for (auto& traitName : cd->interfaces) {
                if (!unit_) break;
                for (auto& d : unit_->declarations) {
                    if (!d || d->kind != DeclKind::Interface) continue;
                    if (d->name != traitName) continue;
                    auto* iface = d->as<InterfaceDecl>();
                    for (auto& im : iface->methods) {
                        if (im.name != m.name) continue;
                        if (im.isStatic) { traitIsStatic = true; break; }
                    }
                    if (traitIsStatic) break;
                }
                if (traitIsStatic) break;
            }
            if (traitIsStatic) m.isStatic = true;
        }
    }

    // ── Primitive-target impl block (2026-04-23) ────────────────────────────
    // `impl Hashable for i32 { fn hash() -> i64 { ... } }`
    // The target is a primitive (not a nominal class), so the usual
    // "treat cd->name as a class and extend type->methods" path below does
    // not apply — mutating the shared primitive VyxType would leak methods
    // into every i32 use-site and confuse isEqual/toString/etc.  Register
    // the methods in a side-map and record trait-satisfaction so both
    // member-access dispatch and verifyMethodConstraints can find them.
    //
    // Coherence is NOT enforced here; a second `impl Hashable for i32`
    // silently overwrites the first.  Orphan checking is future work.
    if (cd->isImplBlock && isPrimitiveTypeName(decl.name)) {
        for (auto& m : cd->methods) {
            primitiveMethodImpls_[decl.name][m.name] = &m;
        }
        // PLAN_SEMA_ROOT_FIX — S3: trait-satisfaction is now owned
        // exclusively by Phase A discovery in `analyze()` and the
        // FactBase is frozen by the time we reach this Pass-1 callback.
        // We keep populating `primitiveMethodImpls_` here because
        // CodeGen and `verifyMethodConstraints`' method-name walk still
        // consult that side-map; migrating it is later-stage work.
        // Method bodies are type-checked in Pass 2 (analyzeAllMethodBodies)
        // since decl->name is registered in symbols_ for every primitive and
        // that pass will reach this decl via the DeclKind::Class branch.
        return;
    }

    if (cd->isImplBlock) {
        auto type = symbols_.lookupType(decl.name);
        if (!type) {
            diag_.error(decl.location, "impl block for unknown type '{}'", decl.name);
            return;
        }
    }

    // Reserved smart-pointer / built-in container names. A user class
    // with the same name silently shadows CodeGen's hard-coded dispatch
    // (e.g. `Box.new` / `Box.get` / `Box.set` intercepts), producing
    // miscompiled code at call sites. Catch the collision up-front.
    // std's own `class Box<T>` / `class Ref<T>` / `class Scope<T>` sit
    // in std/ref.vyx and are marked isImported, so the user file is the
    // only one that should trip this check.
    if (!cd->isImplBlock && !decl.isImported) {
        static const char* kReserved[] = {
            "Box", "Ref", "Scope", "Weak", nullptr
        };
        for (const char** rn = kReserved; *rn; ++rn) {
            if (decl.name == *rn) {
                diag_.error(decl.location,
                    "class name '{}' is reserved for std smart-pointer types; "
                    "pick a different name (or `impl {}` to extend the std class).",
                    decl.name, decl.name);
                return;
            }
        }
    }

    // P2-generics C5: register associated-type bindings so `T::Item` can be
    // resolved when T is bound to this concrete class at instantiation time.
    // This runs before field/method analysis so that method signatures that
    // reference `T::Item` see the binding.
    //
    // For regular class bodies:   `class CountUp : Iterable { type Item = i64; }`
    // For impl blocks:            `impl Iterable for CountUp { type Item = i64; }`
    // Both store their bindings in ClassDecl::associatedTypes.
    //
    // Edge-case handling (P2b Wave 3+):
    //   1. Generic class template (e.g. `class Iter<T> : Iterable`):
    //      DO NOT register "Iter<T>::Item" -> unresolved T in implAssocByTarget_.
    //      Instead, record the RAW annotation pointer in implAssocByTemplate_
    //      so the Dependent/Mono lookup can substitute at instantiation time.
    //   2. Multi-trait disambiguation: if the class has two qualifying traits
    //      that both declare the same assoc name, bare `type Item = X` is
    //      ambiguous. Detect and diagnose.

    // Build set of all traits implemented by this class (parentName + interfaces).
    // Used for ambiguity detection.
    std::vector<std::string> allTraits;
    if (!cd->parentName.empty()) {
        auto pt = symbols_.lookupType(cd->parentName);
        if (pt && pt->kind == VyxTypeKind::Interface) allTraits.push_back(cd->parentName);
    }
    for (auto& iface : cd->interfaces) allTraits.push_back(iface);

    // For each assoc-type declared by more than one implemented trait under the
    // same bare name, record which traits claim it so we can check for ambiguity.
    // key = assoc name → list of trait names that declare it.
    std::map<std::string, std::vector<std::string>> assocNameToTraits;
    for (auto& traitName : allTraits) {
        auto traitIt = traitAssocTypes_.find(traitName);
        if (traitIt == traitAssocTypes_.end()) continue;
        for (auto& assocName : traitIt->second) {
            assocNameToTraits[assocName].push_back(traitName);
        }
    }

    const bool isGenericTemplate = !decl.genericParams.empty();

    for (auto& at : cd->associatedTypes) {
        if (!at.defaultType) continue;

        // ── Edge case 1: generic class template ─────────────────────────
        // For `class Iter<T> : Iterable { type Item = T; }`, the annotation
        // `T` in `at.defaultType` is still a raw generic name — resolveType
        // would succeed and return Generic("T"), but registering
        // "Iter<T>::Item" → Generic("T") in implAssocByTarget_ is useless
        // because lookup at mono time uses "Iter<i32>::Item", which won't match.
        // Instead, record the raw annotation pointer in implAssocByTemplate_
        // so the resolution path can substitute using the instantiation env.
        if (isGenericTemplate) {
            // Register in the template registry: base name → assoc name → annotation.
            // Always store under the bare assoc name so `implAssocByTemplate_["Iter"]["Item"]`
            // is always populated.  For qualified bindings (`type A::Item = T;`) also
            // store under the qualified key `"A::Item"` so trait-specific lookups work.
            implAssocByTemplate_[decl.name][at.name] = at.defaultType.get();
            if (!at.qualifyingTrait.empty()) {
                const std::string qualKey = at.qualifyingTrait + "::" + at.name;
                implAssocByTemplate_[decl.name][qualKey] = at.defaultType.get();
            }
            continue; // skip implAssocByTarget_ for generic templates
        }

        // ── Edge case 2: multi-trait ambiguity check ─────────────────────
        // If more than one implemented trait declares an assoc type with the
        // same bare name AND this binding is unqualified, it is ambiguous.
        if (at.qualifyingTrait.empty()) {
            auto ambigIt = assocNameToTraits.find(at.name);
            if (ambigIt != assocNameToTraits.end() && ambigIt->second.size() > 1) {
                // Build a human-readable list of clashing trait names.
                std::string traitList;
                for (size_t ti = 0; ti < ambigIt->second.size(); ++ti) {
                    if (ti > 0) traitList += "' and '";
                    traitList += ambigIt->second[ti];
                }
                diag_.error(decl.location,
                    "associated type '{}' is ambiguous in class '{}': "
                    "declared by '{}'; qualify with trait name, e.g. `type {}::{} = X;`",
                    at.name, decl.name, traitList,
                    ambigIt->second[0], at.name);
                continue;
            }
        }

        // ── Normal concrete class: resolve and register ──────────────────
        // Push generic params temporarily so that any class-level generics
        // referenced in the binding annotation resolve to Generic() rather
        // than "undefined type".
        for (auto& gp : decl.genericParams) activeGenericParams_.insert(gp);
        auto resolved = resolveType(*at.defaultType);
        for (auto& gp : decl.genericParams) activeGenericParams_.erase(gp);

        // Determine the effective registration key.  For qualified bindings
        // (`type A::Item = i32;`) we register under the qualifying trait's
        // perspective as well so `A::Item` resolves when accessed via trait A.
        const std::string targetKey = decl.name + "::" + at.name;
        implAssocByTarget_[targetKey] = resolved;
        // Also register in the symbol table under the same key so that
        // direct-use sites `CountUp::Item` (Named path via resolveNamedType)
        // can find the type without going through the Dependent path.
        symbols_.registerType(targetKey, resolved);

        // If this is a qualified binding (`type A::Item = i32;`), also register
        // under the trait-qualified key so code using `A::Item` directly finds it.
        if (!at.qualifyingTrait.empty()) {
            const std::string qualKey = decl.name + "::" + at.qualifyingTrait + "::" + at.name;
            implAssocByTarget_[qualKey] = resolved;
            symbols_.registerType(qualKey, resolved);
        }
    }

    auto type = symbols_.lookupType(decl.name);
    if (!type) {
        diag_.error(decl.location, "internal: class '{}' missing from symbol table during analysis", decl.name);
        return;
    }

    if (!decl.genericParams.empty()) {
        for (auto& gp : decl.genericParams) activeGenericParams_.insert(gp);
        std::set<std::string> gpSet(decl.genericParams.begin(), decl.genericParams.end());

        auto resolveGenericFieldType = [&](const TypeAnnotation& ann) -> VyxTypePtr {
            if (gpSet.contains(ann.name)) {
                auto t = std::make_shared<VyxType>();
                t->kind = VyxTypeKind::Generic;
                t->name = ann.name;
                return t;
            }
            if (ann.kind == TypeAnnotationKind::Generic &&
                !ann.as<GenericType>()->typeArgs.empty()) return types::makeUnknown();
            return resolveType(ann);
        };

        for (auto& field : cd->fields) {
            if (!field.type) continue;
            VyxType::Field f;
            f.name = field.name;
            f.type = resolveGenericFieldType(*field.type);
            f.isPublic = (field.visibility == Visibility::Public);
            type->fields.push_back(f);
            for (auto& extra : field.extraNames) {
                VyxType::Field ef = f;
                ef.name = extra;
                type->fields.push_back(ef);
            }
        }

        for (auto& method : cd->methods) {
            checkMethodGenericShadowing(decl, method);
            for (auto& mgp : method.genericParams) activeGenericParams_.insert(mgp);
            VyxType::MethodSig sig;
            sig.name = method.name;
            sig.isPublic = (method.visibility == Visibility::Public);
            sig.returnType = method.returnType
                ? resolveGenericFieldType(*method.returnType)
                : types::makeVoid();
            for (auto& param : method.params)
                sig.paramTypes.push_back(param.type ? resolveGenericFieldType(*param.type) : types::makeUnknown());
            type->methods.push_back(std::move(sig));
            for (auto& mgp : method.genericParams) activeGenericParams_.erase(mgp);
        }

        for (auto& gp : decl.genericParams) activeGenericParams_.erase(gp);
        return;
    }

    if (!cd->parentName.empty()) {
        auto parentType = symbols_.lookupType(cd->parentName);
        if (parentType) {
            for (auto& pf : parentType->fields) {
                type->fields.push_back(pf);
            }
            for (auto& pm : parentType->methods) {
                type->methods.push_back(pm);
            }
        }
    }

    for (auto& field : cd->fields) {
        if (field.type) {
            auto resolvedType = resolveType(*field.type);
            bool updated = false;
            for (auto& ef : type->fields) {
                if (ef.name == field.name) {
                    ef.type = resolvedType;
                    ef.isPublic = (field.visibility == Visibility::Public);
                    updated = true;
                    break;
                }
            }
            if (!updated) {
                VyxType::Field f;
                f.name = field.name;
                f.type = resolvedType;
                f.isPublic = (field.visibility == Visibility::Public);
                type->fields.push_back(std::move(f));
            }
        }
    }

    for (auto& method : cd->methods) {
        for (auto& mgp : method.genericParams) activeGenericParams_.insert(mgp);
        bool updated = false;
        for (auto& em : type->methods) {
            if (em.name == method.name) {
                em.isPublic = (method.visibility == Visibility::Public);
                em.returnType = method.returnType ? resolveType(*method.returnType) : types::makeVoid();
                updated = true;
                break;
            }
        }
        if (!updated) {
            VyxType::MethodSig sig;
            sig.name = method.name;
            sig.isPublic = (method.visibility == Visibility::Public);
            sig.returnType = method.returnType ? resolveType(*method.returnType) : types::makeVoid();
            for (auto& param : method.params) {
                sig.paramTypes.push_back(param.type ? resolveType(*param.type) : types::makeUnknown());
            }
            type->methods.push_back(std::move(sig));
        }
        for (auto& mgp : method.genericParams) activeGenericParams_.erase(mgp);
    }

    for (auto& ifaceName : cd->interfaces) {
        auto ifaceType = symbols_.lookupType(ifaceName);
        if (!ifaceType) {
            diag_.error(decl.location, "undefined interface '{}'", ifaceName);
            continue;
        }

        const Decl* ifaceDeclPtr = nullptr;
        if (unit_) {
            for (auto& d : unit_->declarations) {
                if (d && d->kind == DeclKind::Interface && d->name == ifaceName) {
                    ifaceDeclPtr = d.get();
                    break;
                }
            }
        }

        // P2-generics C5: verify that the class provides bindings for every
        // associated type declared on the interface (unless the interface
        // supplies a default and the class accepts it).
        if (ifaceDeclPtr) {
            auto* ifaceD = ifaceDeclPtr->as<InterfaceDecl>();
            for (auto& assocDecl : ifaceD->associatedTypes) {
                // Does this class provide a binding?
                bool classBinds = false;
                for (auto& at : cd->associatedTypes) {
                    if (at.name == assocDecl.name) { classBinds = true; break; }
                }
                // Also check implAssocByTarget_ in case an impl block wired it.
                if (!classBinds) {
                    const std::string tgtKey = decl.name + "::" + assocDecl.name;
                    if (implAssocByTarget_.count(tgtKey)) classBinds = true;
                }
                // Has the interface declared a default?
                bool hasDefault = (assocDecl.defaultType != nullptr);
                if (!classBinds && !hasDefault) {
                    diag_.error(decl.location,
                        "class '{}' implements interface '{}' but does not bind associated type '{}'",
                        decl.name, ifaceName, assocDecl.name);
                }
            }
        }

        for (auto& reqMethod : ifaceType->methods) {
            bool found = false;
            for (auto& classMethod : type->methods) {
                if (classMethod.name == reqMethod.name) { found = true; break; }
            }
            if (!found) {
                bool hasDefault = false;
                if (ifaceDeclPtr) {
                    auto* ifaceD = ifaceDeclPtr->as<InterfaceDecl>();
                    for (auto& m : ifaceD->methods) {
                        if (m.name == reqMethod.name && m.body) {
                            hasDefault = true;
                            MethodDecl defaultMethod;
                            defaultMethod.name = m.name;
                            defaultMethod.visibility = Visibility::Public;
                            defaultMethod.isOverride = true;
                            defaultMethod.isAsync = m.isAsync;
                            defaultMethod.isStatic = m.isStatic;
                            defaultMethod.location = m.location;
                            if (m.returnType) defaultMethod.returnType = cloneTypeAnnotation(*m.returnType);
                            for (auto& p : m.params) {
                                ParamDecl np;
                                np.name = p.name;
                                np.isMutRef = p.isMutRef;
                                if (p.type) np.type = cloneTypeAnnotation(*p.type);
                                defaultMethod.params.push_back(std::move(np));
                            }
                            if (m.body) defaultMethod.body = cloneStatement(*m.body);
                            cd->methods.push_back(std::move(defaultMethod));

                            VyxType::MethodSig sig;
                            sig.name = m.name;
                            sig.isPublic = true;
                            sig.returnType = reqMethod.returnType;
                            sig.paramTypes = reqMethod.paramTypes;
                            type->methods.push_back(std::move(sig));
                            break;
                        }
                    }
                }
                if (!hasDefault) {
                    diag_.error(decl.location, "class '{}' does not implement method '{}' required by interface '{}'",
                        decl.name, reqMethod.name, ifaceName);
                }
            }
        }
    }

    // P2-generics C5: also check parentName when it resolves to an interface
    // (the `class C : Iterable { ... }` syntax puts the trait in parentName).
    // Also clone default method bodies into the class, mirroring what is done
    // for `cd->interfaces` above — without this, `class En : Greeter` where
    // `Greeter` has a default-body method would leave `hello` unimplemented.
    if (!cd->parentName.empty()) {
        auto parentIfaceType = symbols_.lookupType(cd->parentName);
        if (parentIfaceType && parentIfaceType->kind == VyxTypeKind::Interface) {
            const Decl* ifaceDeclPtr = nullptr;
            if (unit_) {
                for (auto& d : unit_->declarations) {
                    if (d && d->kind == DeclKind::Interface && d->name == cd->parentName) {
                        ifaceDeclPtr = d.get();
                        break;
                    }
                }
            }
            if (ifaceDeclPtr) {
                auto* ifaceD = ifaceDeclPtr->as<InterfaceDecl>();
                for (auto& assocDecl : ifaceD->associatedTypes) {
                    bool classBinds = false;
                    for (auto& at : cd->associatedTypes) {
                        if (at.name == assocDecl.name) { classBinds = true; break; }
                    }
                    if (!classBinds) {
                        const std::string tgtKey = decl.name + "::" + assocDecl.name;
                        if (implAssocByTarget_.count(tgtKey)) classBinds = true;
                    }
                    bool hasDefault = (assocDecl.defaultType != nullptr);
                    if (!classBinds && !hasDefault) {
                        diag_.error(decl.location,
                            "class '{}' implements interface '{}' but does not bind associated type '{}'",
                            decl.name, cd->parentName, assocDecl.name);
                    }
                }

                // Clone default method bodies for methods that the class hasn't
                // overridden (same logic as the cd->interfaces loop above).
                // NOTE: type->methods already contains the interface methods
                // (copied unconditionally at the parentName block above), so
                // we check cd->methods (the class's OWN declared methods) rather
                // than type->methods to detect missing implementations.
                for (auto& reqMethod : parentIfaceType->methods) {
                    bool ownImpl = false;
                    for (auto& classMethod : cd->methods) {
                        if (classMethod.name == reqMethod.name) { ownImpl = true; break; }
                    }
                    if (!ownImpl) {
                        bool hasDefault = false;
                        for (auto& m : ifaceD->methods) {
                            if (m.name == reqMethod.name && m.body) {
                                hasDefault = true;
                                MethodDecl defaultMethod;
                                defaultMethod.name = m.name;
                                defaultMethod.visibility = Visibility::Public;
                                defaultMethod.isOverride = true;
                                defaultMethod.isAsync = m.isAsync;
                                defaultMethod.isStatic = m.isStatic;
                                defaultMethod.location = m.location;
                                if (m.returnType) defaultMethod.returnType = cloneTypeAnnotation(*m.returnType);
                                for (auto& p : m.params) {
                                    ParamDecl np;
                                    np.name = p.name;
                                    np.isMutRef = p.isMutRef;
                                    if (p.type) np.type = cloneTypeAnnotation(*p.type);
                                    defaultMethod.params.push_back(std::move(np));
                                }
                                if (m.body) defaultMethod.body = cloneStatement(*m.body);
                                cd->methods.push_back(std::move(defaultMethod));
                                // type->methods already has the sig (from the
                                // parentType copy at lines 594-603); no duplicate.
                                break;
                            }
                        }
                        if (!hasDefault) {
                            diag_.error(decl.location,
                                "class '{}' does not implement method '{}' required by interface '{}'",
                                decl.name, reqMethod.name, cd->parentName);
                        }
                    }
                }
            }
        }
    }
}

void Sema::injectStructuralTraitDefaults(TranslationUnit& unit) {
    // Collect interface decls (potential traits providing defaults).
    struct TraitInfo {
        const Decl* decl = nullptr;
        const InterfaceDecl* iface = nullptr;
        VyxTypePtr type;
        // Required method names (those WITHOUT a body in the trait source).
        std::set<std::string> required;
        // Default method names (those WITH a body in the trait source).
        std::vector<const MethodDecl*> defaults;
    };
    std::vector<TraitInfo> traits;
    // Pure-requirement traits (no default-body methods) — tracked separately
    // so we can still link classes that structurally satisfy them onto
    // `cd->interfaces`, enabling vtable emission for `Box<dyn Trait>` /
    // `Vec<dyn Trait>` dispatch without perturbing the default-injection
    // path for traits that DO carry bodies.
    std::vector<TraitInfo> pureTraits;
    for (auto& d : unit.declarations) {
        if (!d || d->kind != DeclKind::Interface) continue;
        // Generic traits not handled by this pass — their signatures carry
        // unbound generic params that would mislead structural matching.
        if (!d->genericParams.empty()) continue;
        auto* ifaceD = d->as<InterfaceDecl>();
        if (!ifaceD) continue;
        TraitInfo ti;
        ti.decl  = d.get();
        ti.iface = ifaceD;
        ti.type  = symbols_.lookupType(d->name);
        if (!ti.type) continue;
        for (auto& m : ifaceD->methods) {
            if (m.body) ti.defaults.push_back(&m);
            else        ti.required.insert(m.name);
        }
        if (ti.defaults.empty()) {
            // Pure-requirement trait. Only useful for dyn-dispatch linking if
            // it actually has at least one required method (marker traits
            // degenerate to "always match" which we refuse — see below).
            if (ti.required.empty()) continue;
            pureTraits.push_back(std::move(ti));
            continue;
        }
        traits.push_back(std::move(ti));
    }
    if (traits.empty() && pureTraits.empty()) return;

    for (auto& d : unit.declarations) {
        if (!d || d->kind != DeclKind::Class) continue;
        auto* cd = d->as<ClassDecl>();
        if (!cd) continue;
        // `impl Trait for Type` shares the target type's method table, but is
        // not an independent concrete class.  Structural linking here would
        // otherwise attach every matching trait to each extension block and
        // make CodeGen try to build vtables from methods owned by another
        // declaration.
        if (cd->isImplBlock) continue;
        if (!d->genericParams.empty()) continue; // skip generic class templates
        auto classType = symbols_.lookupType(d->name);
        if (!classType) continue;

        // Build a set of method names the class already has (either declared
        // itself OR picked up via earlier default-injection through an
        // explicit `: Trait` bound).
        std::set<std::string> classMethods;
        for (auto& m : classType->methods) classMethods.insert(m.name);
        for (auto& m : cd->methods) classMethods.insert(m.name);

        // Already-linked explicit traits — don't re-process.
        std::set<std::string> linkedTraits;
        if (!cd->parentName.empty()) linkedTraits.insert(cd->parentName);
        for (auto& iface : cd->interfaces) linkedTraits.insert(iface);

        for (auto& ti : traits) {
            if (linkedTraits.count(ti.decl->name)) continue;

            // Structural-match gate: class must satisfy every required
            // (bodyless) method name on the trait. If the trait has zero
            // required methods this degenerates to "always match", which
            // is undesirable for marker traits — guard that case.
            if (ti.required.empty()) continue;
            bool allMet = true;
            for (auto& req : ti.required) {
                if (!classMethods.count(req)) { allMet = false; break; }
            }
            if (!allMet) continue;

            // Inject each default method the class hasn't declared.
            for (auto* m : ti.defaults) {
                if (classMethods.count(m->name)) continue;
                MethodDecl clone;
                clone.name       = m->name;
                clone.visibility = Visibility::Public;
                clone.isOverride = true;
                clone.isAsync    = m->isAsync;
                clone.isStatic   = m->isStatic;
                clone.location   = m->location;
                clone.genericParams = m->genericParams;
                clone.genericConstraints = m->genericConstraints;
                if (m->returnType) clone.returnType = cloneTypeAnnotation(*m->returnType);
                for (auto& p : m->params) {
                    ParamDecl np;
                    np.name     = p.name;
                    np.isMutRef = p.isMutRef;
                    if (p.type) np.type = cloneTypeAnnotation(*p.type);
                    clone.params.push_back(std::move(np));
                }
                if (m->body) clone.body = cloneStatement(*m->body);
                cd->methods.push_back(std::move(clone));

                // Mirror onto classType->methods so trait-bound checks see
                // it via `concreteType->methods` (see SemaGeneric.cpp ~946).
                VyxType::MethodSig sig;
                sig.name       = m->name;
                sig.isPublic   = true;
                // Find the signature on the trait's VyxType for accurate
                // param/return types.
                for (auto& ms : ti.type->methods) {
                    if (ms.name == m->name) {
                        sig.returnType = ms.returnType;
                        sig.paramTypes = ms.paramTypes;
                        break;
                    }
                }
                classType->methods.push_back(std::move(sig));
                classMethods.insert(m->name);
            }

            // Intentionally do NOT push the trait into `cd->interfaces`
            // here. That would trigger vtable synthesis for a trait the user
            // didn't explicitly implement, and generateInterfaceVtables
            // references trait forward-declares whose (void* self, void*
            // other) signature doesn't match the class's method signatures,
            // producing a malformed module that later crashes inside
            // `Module::print`. The trait-bound check (SemaGeneric.cpp ~946)
            // inspects `concreteType->methods` directly, which is already
            // populated above — so `T: Sized` still resolves.
            linkedTraits.insert(ti.decl->name);
        }
    }

    // Pure-requirement trait linking pass: when a class structurally provides
    // every required method on a trait whose declarations are bodyless (the
    // common "trait Greeter { fn greet(self) -> string; }" shape),
    // register the trait on `cd->interfaces`. Unlike the default-injection
    // pass above, this is safe because:
    //   1. generateEmitDefaultTraitMethods skips bodyless methods, so no
    //      forward-declare with a spurious `(ptr,ptr)` shape is ever emitted.
    //   2. generatePopulateInstanceVtables can populate the vtable directly
    //      from the class's concrete method (`ClassName.methodName`).
    // Without this, `Box<dyn Greeter>` / `Vec<dyn Greeter>` callers emit
    // `__iface_Greeter` pairs whose vtable slot stays NULL, producing a
    // runtime segfault when the fat pointer is dispatched.
    if (!pureTraits.empty()) {
        for (auto& d : unit.declarations) {
            if (!d || d->kind != DeclKind::Class) continue;
            auto* cd = d->as<ClassDecl>();
            if (!cd) continue;
            if (cd->isImplBlock) continue;
            if (!d->genericParams.empty()) continue;
            auto classType = symbols_.lookupType(d->name);
            if (!classType) continue;

            std::set<std::string> classMethods;
            for (auto& m : classType->methods) classMethods.insert(m.name);
            for (auto& m : cd->methods) classMethods.insert(m.name);

            std::set<std::string> linkedTraits;
            if (!cd->parentName.empty()) linkedTraits.insert(cd->parentName);
            for (auto& iface : cd->interfaces) linkedTraits.insert(iface);

            for (auto& ti : pureTraits) {
                if (linkedTraits.count(ti.decl->name)) continue;
                bool allMet = true;
                for (auto& req : ti.required) {
                    if (!classMethods.count(req)) { allMet = false; break; }
                }
                if (!allMet) continue;
                cd->interfaces.push_back(ti.decl->name);
                linkedTraits.insert(ti.decl->name);
            }
        }
    }
}

void Sema::analyzeAllMethodBodies(TranslationUnit& unit) {
    // Second pass: with every decl's signature / field layout / nested type
    // registered, walk the unit once more and analyse each method body.
    // Generic params are re-pushed/popped per decl so resolveType treats
    // `T` as an opaque Generic and not "undefined type T".
    for (auto& decl : unit.declarations) {
        if (!decl) continue;

        // Push the owning decl's generic params for the duration of its
        // body analysis. Without this, `T` inside a generic class method
        // resolves to "undefined type" via SemaResolve.
        auto pushGenerics = [&](std::vector<std::string>& gps) {
            for (auto& gp : gps) activeGenericParams_.insert(gp);
        };
        auto popGenerics = [&](std::vector<std::string>& gps) {
            for (auto& gp : gps) activeGenericParams_.erase(gp);
        };

        switch (decl->kind) {
            case DeclKind::Class: {
                auto* cd = decl->as<ClassDecl>();
                auto classType = symbols_.lookupType(decl->name);
                if (!classType) break;
                pushGenerics(decl->genericParams);
                analyzeMethodBodiesGeneric(*decl, cd->methods, classType);
                popGenerics(decl->genericParams);
                break;
            }
            case DeclKind::Interface: {
                auto* id = decl->as<InterfaceDecl>();
                auto type = symbols_.lookupType(decl->name);
                if (!type) break;
                pushGenerics(decl->genericParams);
                analyzeMethodBodiesGeneric(*decl, id->methods, type);
                popGenerics(decl->genericParams);
                break;
            }
            case DeclKind::ErrorDef: {
                auto* ed = decl->as<ErrorDefDecl>();
                auto type = symbols_.lookupType(decl->name);
                if (!type) break;
                pushGenerics(decl->genericParams);
                analyzeMethodBodiesGeneric(*decl, ed->methods, type);
                popGenerics(decl->genericParams);
                break;
            }
            default:
                break;
        }
    }
}

void Sema::analyzeMethodBodiesGeneric(Decl& decl,
                                       std::vector<MethodDecl>& methods,
                                       VyxTypePtr selfType) {
    // Snapshot analysis state so we can restore on exit. Same restore
    // discipline as analyzeFunctionDecl.
    auto savedClassType   = currentClassType_;
    auto savedReturn      = currentReturnType_;
    auto savedReturnAnn   = currentReturnAnnotation_;
    auto savedUnsafe      = inUnsafe_;
    auto savedAutoWrap    = currentFnAutoWrap_;
    auto savedImported    = inImportedScope_;

    currentClassType_ = selfType;
    // Methods defined in an imported module must be treated as belonging to
    // that module when resolving calls — otherwise their calls to private
    // helpers in the same module (e.g. `__popcount64`, `_dict_hash_*`)
    // trigger a cross-module visibility error. Mirrors the same toggle
    // analyzeFunctionDecl applies to free-function bodies.
    inImportedScope_ = decl.isImported;

    // If the enclosing decl is a concrete instantiation of a generic
    // (its name contains '<', e.g. "Vec<i32>"), manufacture a synthetic
    // InstantiationFrame so any type errors discovered inside method
    // bodies carry a helpful "instantiation chain:" note.  The frame is
    // pushed onto `instantiationStack_` here and popped when the
    // guard goes out of scope at the end of the method loop (one frame
    // per class, not per method — the method name is encoded in the
    // `calleeName` field of each per-method sub-frame added below).
    //
    // For non-instantiated (still-generic) decls the stack is left
    // unchanged; `emitInstantiationChainNotes` is a no-op when the
    // stack is empty so existing non-generic code is unaffected.
    std::unique_ptr<InstantiationGuard> outerClassGuard;
    bool isConcreteInstantiation = decl.name.find('<') != std::string::npos;
    if (isConcreteInstantiation && instantiationStack_.empty()) {
        // Determine the class-level type bindings from the mangled name.
        // We parse them from the name string as a best-effort since the
        // original `GenericSubstitution` is not retained on the decl.
        InstantiationFrame classFrame;
        classFrame.calleeName = decl.name;
        classFrame.site       = decl.location;
        // Bindings are not available from the mangled name without a full
        // de-mangler; leave them empty — the calleeName already encodes the
        // concrete types in a human-readable form (e.g. "Vec<i32>").
        outerClassGuard = std::make_unique<InstantiationGuard>(*this, std::move(classFrame));
    }

    for (auto& method : methods) {
        if (!method.body) continue;       // abstract / extern / iface-no-default sig-only
        if (method.body->kind == StmtKind::Block &&
            method.body->as<BlockStmt>()->statements.empty()) {
            diag_.warning(method.location,
                "unimplemented empty function body: {}", method.name);
        }

        // Method-level generic params are visible within the body.
        for (auto& mgp : method.genericParams) activeGenericParams_.insert(mgp);

        // For concrete instantiations, also push a per-method frame so the
        // chain note reads:
        //   1. Vec<i32> instantiated at ...
        //   2.   method push(val: i32) at ...
        // The frame is popped automatically when methodGuard goes out of scope.
        std::unique_ptr<InstantiationGuard> methodGuard;
        if (isConcreteInstantiation) {
            InstantiationFrame mf;
            mf.calleeName = decl.name + "::" + method.name;
            mf.site       = method.location;
            methodGuard = std::make_unique<InstantiationGuard>(*this, std::move(mf));
        }

        symbols_.pushScope();

        // Non-type generic parameters of the *enclosing decl* (`const N: usize`)
        // need to be visible as constant-typed identifiers within the body so
        // expressions like `arr[N - 1]` or `if i < N { ... }` resolve N to
        // its declared value type. The actual integer value is unknown at
        // template-analysis time; Mono substitutes the bound value when it
        // emits the concrete instantiation.
        for (auto& [pname, ptype] : decl.genericConstParams) {
            Symbol cs;
            cs.name = pname;
            cs.type = ptype ? resolveType(*ptype) : types::makeISize();
            cs.isConst = true;
            cs.declLocation = decl.location;
            symbols_.declare(pname, std::move(cs));
        }

        VyxTypePtr retType = method.returnType
            ? resolveType(*method.returnType)
            : types::makeVoid();
        currentReturnType_       = retType;
        currentReturnAnnotation_ = method.returnType.get();

        bool methodAutoWrap = false;
        for (auto& [an, av] : method.attributes) {
            if (an == "unsafe")         inUnsafe_ = true;
            else if (an == "auto_wrap") methodAutoWrap = true;
        }
        currentFnAutoWrap_ = methodAutoWrap;

        // `self` binding for instance methods. Interface / errordef methods
        // without a selfType (caller passed nullptr) skip this, since they're
        // declaration-only signatures with static-like default bodies.
        if (!method.isStatic && selfType) {
            Symbol selfSym;
            selfSym.name = "self";
            selfSym.type = selfType;
            selfSym.isConst = false;
            selfSym.declLocation = method.location;
            symbols_.declare("self", std::move(selfSym));
        }

        for (auto& param : method.params) {
            Symbol ps;
            ps.name = param.name;
            ps.type = param.type ? resolveType(*param.type) : types::makeUnknown();
            ps.isConst = !param.isMutRef;
            ps.declLocation = method.location;
            symbols_.declare(param.name, std::move(ps));
        }

        // Snapshot error count so we can detect whether body analysis
        // produced new diagnostics.  If errors were emitted AND we are
        // inside a concrete generic instantiation, append the chain note.
        uint32_t errsBefore = diag_.errorCount();
        analyzeStmt(*method.body);
        uint32_t errsAfter = diag_.errorCount();

        // Emit instantiation chain notes when body analysis produced new
        // errors inside a concrete instantiation context.  The stack
        // already contains the class + method frames pushed above, so the
        // chain note includes both levels.
        if (errsAfter > errsBefore && isConcreteInstantiation) {
            emitInstantiationChainNotes(method.location);
        }

        symbols_.popScope();
        inUnsafe_ = savedUnsafe;

        for (auto& mgp : method.genericParams) activeGenericParams_.erase(mgp);
        // methodGuard pops the per-method frame here (if any).
    }

    currentClassType_      = savedClassType;
    currentReturnType_     = savedReturn;
    currentReturnAnnotation_ = savedReturnAnn;
    currentFnAutoWrap_     = savedAutoWrap;
    inUnsafe_              = savedUnsafe;
    inImportedScope_       = savedImported;
}

// ============================================================
//  Phase 8: @[derive(Trait1, Trait2, ...)] auto-synthesis
// ============================================================
//
// For each class / struct declaration carrying an `@[derive(...)]` attribute,
// generate the corresponding impl methods from the declaration's fields and
// push them onto `cd->methods` before Sema's per-decl pass analyzes the type.
//
// Strategy: the injected MethodDecls are real AST nodes (same shape as if the
// user had written the impl by hand) so analyzeClassDecl registers their
// signatures in VyxType::methods and analyzeAllMethodBodies type-checks their
// bodies. No CodeGen hook required — the bodies compile through the normal
// emitFunctionDecl path.
//
// Trait coverage:
//   Eq       → fn operator_eq(other: Self) -> bool
//                 body: return self.f1 == other.f1 && self.f2 == other.f2 && ...;
//                 (for string fields uses str_equals)
//   Clone    → fn clone() -> Self
//                 body: return Self { f1: self.f1, ... };
//                 (for string fields uses str_dup to deep-copy)
//   Hashable → fn hash() -> i64
//                 body: var h: i64 = 0; h = h ^ hash_i32(self.f1); ...; return h;
//   Ord      → fn compare(other: Self) -> i32
//                 body: lexicographic if/else chain, primitive-<  only
//   Copy     → no method synthesis (marker trait; semantic enforcement deferred)
//   Debug    → fn debug() -> string           (MVP: deferred, emits diagnostic)
//   Display  → fn display() -> string         (MVP: deferred, emits diagnostic)
//
// Field-type eligibility: the MVP only derives traits on classes whose fields
// are primitives (i8..i64 / u8..u64 / f32/f64 / bool / char / string). Nested
// class fields emit a diagnostic pointing at the offending field rather than
// silently producing a miscompile.
namespace {

using namespace ast;

// Shallow heuristic: returns true when the TypeAnnotation is a plain
// `NamedType` whose name matches a stdlib primitive. We deliberately
// reject pointer/array/generic wrappers — their trait impls would need a
// deeper Sema analysis that is out of scope for Phase 8.
bool isDerivePrimitiveField(const TypeAnnotation* ann) {
    if (!ann) return false;
    if (ann->kind != TypeAnnotationKind::Named) return false;
    static const std::set<std::string> kPrim = {
        "i8", "i16", "i32", "i64",
        "u8", "u16", "u32", "u64",
        "f32", "f64",
        "bool", "char",
        "rawptr", "isize", "usize",
        "str",
        "string",
    };
    return kPrim.count(ann->name) > 0;
}

bool isStringField(const TypeAnnotation* ann) {
    return ann && ann->kind == TypeAnnotationKind::Named &&
           (ann->name == "str" || ann->name == "string");
}

// Parse an `@[derive(A, B, C)]` attribute value (the parser concatenated the
// identifiers with ", " separators) into a vector of trait names.
std::vector<std::string> splitDeriveList(const std::string& raw) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : raw) {
        if (c == ',' || c == ' ' || c == '\t') {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

ExprPtr makeSelfExpr(SourceLocation loc) {
    auto e = std::make_unique<SelfExpr>();
    e->location = loc;
    return e;
}

ExprPtr makeSelfFieldAccess(SourceLocation loc, const std::string& field) {
    return makeMemberAccess(loc, makeSelfExpr(loc), field);
}

ExprPtr makeOtherFieldAccess(SourceLocation loc, const std::string& field) {
    return makeMemberAccess(loc, makeIdentifier(loc, "other"), field);
}

} // namespace

void Sema::synthesizeDeriveMethods(TranslationUnit& unit) {
    for (auto& declPtr : unit.declarations) {
        if (!declPtr) continue;
        Decl* decl = declPtr.get();
        if (decl->kind != DeclKind::Class && decl->kind != DeclKind::Struct) continue;

        // Collect derived trait names from every `@[derive(...)]` attribute on
        // this decl. Multiple derive attributes accumulate.
        std::vector<std::string> derived;
        for (auto& [attrName, attrValue] : decl->attributes) {
            if (attrName != "derive") continue;
            auto names = splitDeriveList(attrValue);
            for (auto& n : names) derived.push_back(n);
        }
        if (derived.empty()) continue;

        // Reference the fields + methods in-place. Structs and classes have
        // parallel layouts but distinct AST nodes, so we branch once and take
        // a reference.
        std::vector<FieldDecl>* fieldsPtr = nullptr;
        std::vector<MethodDecl>* methodsPtr = nullptr;
        if (decl->kind == DeclKind::Class) {
            auto* cd = decl->as<ClassDecl>();
            // Skip impl blocks — impl blocks can't host `@[derive]` (the
            // attribute targets the defining class).
            if (cd->isImplBlock) continue;
            fieldsPtr = &cd->fields;
            methodsPtr = &cd->methods;
        } else {
            auto* sd = decl->as<StructDecl>();
            fieldsPtr = &sd->fields;
            methodsPtr = &sd->methods;
        }
        auto& fields = *fieldsPtr;
        auto& methods = *methodsPtr;

        // Build a set of existing method names so user-provided impls override
        // auto-generation (first-writer-wins policy).
        std::set<std::string> existingMethods;
        for (auto& m : methods) existingMethods.insert(m.name);

        // Field-type eligibility check: for each requested trait, verify every
        // field uses a primitive type. Emit a diagnostic pointing at the first
        // offending field and skip the synthesis for that trait only. Copy is
        // a marker trait so no check is required.
        auto verifyFieldsPrimitive = [&](const std::string& traitName) -> bool {
            for (auto& f : fields) {
                if (f.isStatic) continue;
                if (!isDerivePrimitiveField(f.type.get())) {
                    std::string fieldTypeStr = "<unresolved>";
                    if (f.type) fieldTypeStr = f.type->name.empty() ? "<anonymous>" : f.type->name;
                    diag_.error(f.location,
                        "@[derive({})] on '{}' cannot use field '{}' of type '{}': "
                        "only primitive field types (integers, floats, bool, char, string) "
                        "are supported in the MVP derive",
                        traitName, decl->name, f.name, fieldTypeStr);
                    return false;
                }
            }
            return true;
        };

        SourceLocation synthLoc = decl->location;

        // ───── derive(Eq) → fn operator_eq(other: Self) -> bool ─────
        auto synthesizeEq = [&]() {
            if (existingMethods.count("operator_eq")) return;
            if (!verifyFieldsPrimitive("Eq")) return;

            MethodDecl m;
            m.name = "operator_eq";
            m.visibility = Visibility::Public;
            m.location = synthLoc;
            m.returnType = makeNamedType(synthLoc, "bool");
            ParamDecl p;
            p.name = "other";
            p.type = makeNamedType(synthLoc, decl->name);
            m.params.push_back(std::move(p));

            // If no fields, body is `return true;`.
            ExprPtr condition;
            for (auto& f : fields) {
                if (f.isStatic) continue;
                ExprPtr term;
                if (isStringField(f.type.get())) {
                    // `str_equals(self.f, other.f)`
                    std::vector<ExprPtr> args;
                    args.push_back(makeSelfFieldAccess(synthLoc, f.name));
                    args.push_back(makeOtherFieldAccess(synthLoc, f.name));
                    term = makeCall(synthLoc,
                                    makeIdentifier(synthLoc, "str_equals"),
                                    std::move(args));
                } else {
                    term = makeBinaryOp(synthLoc, BinaryOp::Eq,
                                        makeSelfFieldAccess(synthLoc, f.name),
                                        makeOtherFieldAccess(synthLoc, f.name));
                }
                condition = condition
                    ? makeBinaryOp(synthLoc, BinaryOp::And, std::move(condition), std::move(term))
                    : std::move(term);
            }
            if (!condition) condition = makeBoolLiteral(synthLoc, true);

            std::vector<StmtPtr> stmts;
            stmts.push_back(makeReturn(synthLoc, std::move(condition)));
            m.body = makeBlock(synthLoc, std::move(stmts));

            methods.push_back(std::move(m));
            existingMethods.insert("operator_eq");
        };

        // ───── derive(Clone) → fn clone() -> Self ─────
        auto synthesizeClone = [&]() {
            if (existingMethods.count("clone")) return;
            if (!verifyFieldsPrimitive("Clone")) return;

            MethodDecl m;
            m.name = "clone";
            m.visibility = Visibility::Public;
            m.location = synthLoc;
            m.returnType = makeNamedType(synthLoc, decl->name);

            // Build `return Self { f: self.f, ... };` — for string fields use
            // `str_dup(self.f)` to produce an independent buffer.
            auto initExpr = std::make_unique<StructInitExpr>();
            initExpr->location = synthLoc;
            initExpr->structName = decl->name;
            for (auto& f : fields) {
                if (f.isStatic) continue;
                ExprPtr v;
                if (isStringField(f.type.get())) {
                    std::vector<ExprPtr> args;
                    args.push_back(makeSelfFieldAccess(synthLoc, f.name));
                    v = makeCall(synthLoc,
                                 makeIdentifier(synthLoc, "str_dup"),
                                 std::move(args));
                } else {
                    v = makeSelfFieldAccess(synthLoc, f.name);
                }
                initExpr->fieldInits.emplace_back(f.name, std::move(v));
            }

            std::vector<StmtPtr> stmts;
            stmts.push_back(makeReturn(synthLoc, std::move(initExpr)));
            m.body = makeBlock(synthLoc, std::move(stmts));

            methods.push_back(std::move(m));
            existingMethods.insert("clone");
        };

        // ───── derive(Hashable) → fn hash() -> i64 ─────
        auto synthesizeHash = [&]() {
            if (existingMethods.count("hash")) return;
            if (!verifyFieldsPrimitive("Hashable")) return;

            MethodDecl m;
            m.name = "hash";
            m.visibility = Visibility::Public;
            m.location = synthLoc;
            m.returnType = makeNamedType(synthLoc, "i64");

            std::vector<StmtPtr> stmts;
            // `var h: i64 = 0;`
            stmts.push_back(makeVarDecl(synthLoc,
                /*isConst=*/false,
                /*name=*/"h",
                /*type=*/makeNamedType(synthLoc, "i64"),
                /*init=*/makeIntLiteral(synthLoc, 0)));

            // For each field: `h = h ^ hash_XXX(self.f);`
            for (auto& f : fields) {
                if (f.isStatic) continue;
                const std::string& ftn = f.type ? f.type->name : std::string{};
                std::string hashFn;
                if (ftn == "i64" || ftn == "u64" || ftn == "isize" || ftn == "usize")
                    hashFn = "hash_i64";
                else if (ftn == "str" || ftn == "string")
                    hashFn = "hash_string";
                else if (ftn == "f32" || ftn == "f64" || ftn == "bool" || ftn == "char" ||
                         ftn == "i8" || ftn == "i16" || ftn == "i32" ||
                         ftn == "u8" || ftn == "u16" || ftn == "u32")
                    hashFn = "hash_i64"; // cast-up path
                else
                    hashFn = "hash_i64"; // fallback (shouldn't trigger — guarded above)

                // Value to hash: for non-i64/string primitives we widen via an
                // explicit `as i64` cast so the call site typechecks.
                ExprPtr selfF = makeSelfFieldAccess(synthLoc, f.name);
                if (hashFn == "hash_i64" && ftn != "i64" && ftn != "u64" &&
                    ftn != "isize" && ftn != "usize") {
                    auto cast = std::make_unique<CastExpr>();
                    cast->location = synthLoc;
                    cast->operand = std::move(selfF);
                    cast->targetType = makeNamedType(synthLoc, "i64");
                    selfF = std::move(cast);
                }

                std::vector<ExprPtr> args;
                args.push_back(std::move(selfF));
                auto call = makeCall(synthLoc,
                                     makeIdentifier(synthLoc, hashFn),
                                     std::move(args));
                // `h = h ^ call;` — emit as AssignStmt (a top-level statement)
                // rather than wrapping AssignmentExpr in an ExprStmt: CodeGen
                // routes assignments through emitAssignment / StmtKind::Assignment
                // and has no codepath for ExprKind::Assignment used as an
                // expression statement.
                auto xorExpr = makeBinaryOp(synthLoc, BinaryOp::BitXor,
                                            makeIdentifier(synthLoc, "h"),
                                            std::move(call));
                auto assignStmt = std::make_unique<AssignStmt>();
                assignStmt->location = synthLoc;
                assignStmt->target = makeIdentifier(synthLoc, "h");
                assignStmt->value = std::move(xorExpr);
                stmts.push_back(std::move(assignStmt));
            }

            stmts.push_back(makeReturn(synthLoc, makeIdentifier(synthLoc, "h")));
            m.body = makeBlock(synthLoc, std::move(stmts));

            methods.push_back(std::move(m));
            existingMethods.insert("hash");
        };

        // ───── derive(Ord) → fn compare(other: Self) -> i32 ─────
        auto synthesizeOrd = [&]() {
            if (existingMethods.count("compare")) return;
            if (!verifyFieldsPrimitive("Ord")) return;
            // String fields can't use the inlined `<` / `>` compare; we'd
            // need str_compare. Reject them up front with a clear message.
            for (auto& f : fields) {
                if (f.isStatic) continue;
                if (isStringField(f.type.get())) {
                    diag_.error(f.location,
                        "@[derive(Ord)] on '{}': string field '{}' is not yet supported "
                        "(string comparison requires str_compare; use a manual impl)",
                        decl->name, f.name);
                    return;
                }
            }

            MethodDecl m;
            m.name = "compare";
            m.visibility = Visibility::Public;
            m.location = synthLoc;
            m.returnType = makeNamedType(synthLoc, "i32");
            ParamDecl p;
            p.name = "other";
            p.type = makeNamedType(synthLoc, decl->name);
            m.params.push_back(std::move(p));

            // Body: sequence of
            //   if (self.f < other.f) { return -1; }
            //   if (self.f > other.f) { return 1; }
            // for each field, followed by `return 0;`.
            std::vector<StmtPtr> stmts;
            for (auto& f : fields) {
                if (f.isStatic) continue;

                auto mkCmp = [&](BinaryOp op, int64_t retVal) -> StmtPtr {
                    auto cond = makeBinaryOp(synthLoc, op,
                                             makeSelfFieldAccess(synthLoc, f.name),
                                             makeOtherFieldAccess(synthLoc, f.name));
                    std::vector<StmtPtr> thenStmts;
                    // `return -1;` requires a UnaryOp::Neg over IntLiteral(1)
                    // for negative values since IntLiteral stores unsigned.
                    ExprPtr retExpr;
                    if (retVal < 0) {
                        retExpr = makeUnaryOp(synthLoc, UnaryOp::Neg,
                                              makeIntLiteral(synthLoc, -retVal));
                    } else {
                        retExpr = makeIntLiteral(synthLoc, retVal);
                    }
                    // The return must be `i32`, but the bare integer literal
                    // defaults to `i64`. Wrap in a cast.
                    auto cast = std::make_unique<CastExpr>();
                    cast->location = synthLoc;
                    cast->operand = std::move(retExpr);
                    cast->targetType = makeNamedType(synthLoc, "i32");
                    thenStmts.push_back(makeReturn(synthLoc, std::move(cast)));
                    auto block = makeBlock(synthLoc, std::move(thenStmts));
                    auto ifStmt = std::make_unique<IfStmt>();
                    ifStmt->location = synthLoc;
                    ifStmt->condition = std::move(cond);
                    ifStmt->thenBranch = std::move(block);
                    return ifStmt;
                };

                stmts.push_back(mkCmp(BinaryOp::Lt, -1));
                stmts.push_back(mkCmp(BinaryOp::Gt, 1));
            }

            // Final `return 0;` (as i32).
            auto zeroLit = makeIntLiteral(synthLoc, 0);
            auto zeroCast = std::make_unique<CastExpr>();
            zeroCast->location = synthLoc;
            zeroCast->operand = std::move(zeroLit);
            zeroCast->targetType = makeNamedType(synthLoc, "i32");
            stmts.push_back(makeReturn(synthLoc, std::move(zeroCast)));

            m.body = makeBlock(synthLoc, std::move(stmts));
            methods.push_back(std::move(m));
            existingMethods.insert("compare");
        };

        // Dispatch over requested derives. Unknown traits emit an error so
        // typos are surfaced rather than silently ignored. Copy is a marker
        // trait (no method to synthesize).
        for (auto& traitName : derived) {
            if (traitName == "Eq") {
                synthesizeEq();
            } else if (traitName == "Clone") {
                synthesizeClone();
            } else if (traitName == "Hashable" || traitName == "Hash") {
                // Accept both spellings; stdlib uses "Hashable".
                synthesizeHash();
            } else if (traitName == "Ord") {
                synthesizeOrd();
            } else if (traitName == "Copy") {
                // Marker only; nothing to synthesize.
            } else if (traitName == "Debug" || traitName == "Display") {
                // MVP scope decision (2026-04-23): Debug/Display synthesis is
                // deferred because it requires calling `.display()` / `.debug()`
                // on each field (which primitives don't yet carry) plus a
                // snprintf-style string concat that Vyx doesn't have a clean
                // AST-only expression for. Emit a diagnostic so the caller
                // knows to write the impl manually for now.
                diag_.error(decl->location,
                    "@[derive({})] on '{}' is not yet implemented in the MVP; "
                    "synthesis for '{}' will land in a follow-up patch — "
                    "write a manual `fn {}() -> string {{ ... }}` for now",
                    traitName, decl->name, traitName,
                    traitName == "Debug" ? "debug" : "display");
            } else {
                diag_.error(decl->location,
                    "@[derive({})] on '{}': unknown trait name; supported traits "
                    "are Eq, Ord, Clone, Copy, Hashable (aka Hash)",
                    traitName, decl->name);
            }
        }
    }
}

} // namespace vyx
