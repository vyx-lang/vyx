#include "CodeGenIncludes.h"
#include "../Sema/TemplateResolver.h"
#include <set>

namespace vyx {

// Backend-level name translations applied AFTER canonical mangling.
// These are LLVM/runtime concerns — how opaque user types lower to concrete
// storage — not part of the generic mangling contract.
//
// Note: `string` is intentionally NOT translated. The `__String` LLVM struct
// is registered under both `__String` and `string` keys in structTypes_
// (see getOrCreateStringType), so the canonical name `string` resolves
// directly without rewriting. This keeps mangled compounds like `Vec<string>`
// aligned with what Mono produces.
static std::string applyBackendNameTranslation(const std::string& mangled) {
    if (mangled == "Ref" || mangled == "Scope") return "__RefCounted";
    if (mangled == "Box") return "rawptr";
    return mangled;
}

std::string CodeGen::resolveAndMangleTypeAnnotation(const TypeAnnotation& ann) {
    // Shortcut the bare-name fast path — handles both the legacy special
    // cases and the common `T` → concrete remap from `genericTypeParamNames_`.
    auto& subTypes = getTypeSubTypes(ann);
    if (subTypes.empty()) {
        if (ann.name == "Ref" || ann.name == "Scope") return "__RefCounted";
        if (ann.name == "Box") return "rawptr";
        auto gnIt = genericTypeParamNames_.find(ann.name);
        if (gnIt != genericTypeParamNames_.end())
            return applyBackendNameTranslation(gnIt->second);
        return ann.name;
    }

    // Build a TypeEnv from the CodeGen-level generic param bindings. This
    // lets the canonical substituter see the same remapping the legacy
    // implementation did via direct string lookup.
    TypeEnv env;
    for (auto& [k, v] : genericTypeParamNames_) {
        env.bind(k, types::makeClass(v));
    }

    // Resolve the AST annotation into a structured VyxType, substitute,
    // then mangle through the canonical path. `mangle()` is idempotent and
    // structural, eliminating the `Outer<Inner<...>>` slicing bug the old
    // code had string-level guards for.
    auto resolved = TemplateResolver::resolveTypeAnnotation(ann);
    auto substituted = substituteType(resolved, env);
    return applyBackendNameTranslation(substituted ? substituted->mangle() : ann.name);
}

std::string CodeGen::buildMangledClassName(const std::string& baseName,
                                           const std::vector<TypePtr>& typeArgs) {
    static const std::vector<ExprPtr> emptyArgExprs;
    return buildMangledClassName(baseName, typeArgs, emptyArgExprs);
}

std::string CodeGen::buildMangledClassName(const std::string& baseName,
                                           const std::vector<TypePtr>& typeArgs,
                                           const std::vector<ExprPtr>& callArgExprs) {
    if (typeArgs.empty()) return baseName;
    // If baseName is already a fully-mangled generic form (e.g. `Vec<i64>`),
    // treat typeArgs as a no-op rewrap guard. The canonical mangler would
    // otherwise produce `Vec<i64><...>` — structurally valid but semantically
    // a double wrap.
    if (baseName.size() >= 2 &&
        baseName.find('<') != std::string::npos &&
        baseName.back() == '>') {
        return baseName;
    }
    // Build the mangled name directly, emitting const-generic slots as their
    // integer value. mangleGeneric() on VyxTypePtr can't express this shape —
    // a type slot is either a type or it collapses to i64 — so we bypass it
    // whenever at least one slot is a const-generic integer.
    bool hasConstSlot = false;
    for (size_t i = 0; i < typeArgs.size(); ++i) {
        if (!typeArgs[i] && i < callArgExprs.size() && callArgExprs[i] &&
            callArgExprs[i]->kind == ExprKind::IntLiteral) {
            hasConstSlot = true;
            break;
        }
    }
    if (hasConstSlot) {
        std::string out = baseName + "<";
        TypeEnv env;
        for (auto& [k, v] : genericTypeParamNames_) env.bind(k, types::makeClass(v));
        for (size_t i = 0; i < typeArgs.size(); ++i) {
            if (i > 0) out += ",";
            if (!typeArgs[i]) {
                // Const-generic slot: emit the IntLiteral value directly.
                if (i < callArgExprs.size() && callArgExprs[i] &&
                    callArgExprs[i]->kind == ExprKind::IntLiteral) {
                    out += std::to_string(
                        callArgExprs[i]->as<IntLiteralExpr>()->value);
                } else {
                    // Fallback for pre-parsed nullptr slots without IntLit:
                    // same i64 placeholder as the type-only path used. Better
                    // than silently dropping the slot.
                    out += "i64";
                }
                continue;
            }
            auto resolved = TemplateResolver::resolveTypeAnnotation(*typeArgs[i]);
            auto concrete = substituteType(resolved, env);
            out += concrete ? concrete->toString() : "?";
        }
        out += ">";
        return out;
    }
    std::vector<VyxTypePtr> resolvedArgs;
    resolvedArgs.reserve(typeArgs.size());
    TypeEnv env;
    for (auto& [k, v] : genericTypeParamNames_) env.bind(k, types::makeClass(v));
    for (auto& a : typeArgs) {
        if (!a) {
            resolvedArgs.push_back(types::makeInt(64, true));
            continue;
        }
        auto resolved = TemplateResolver::resolveTypeAnnotation(*a);
        resolvedArgs.push_back(substituteType(resolved, env));
    }
    return mangleGeneric(baseName, resolvedArgs);
}

llvm::Function* CodeGen::findClassMethod(const std::string& className,
                                         const std::string& methodName) {
    std::string fullName = className + "." + methodName;
    auto it = functions_.find(fullName);
    if (it != functions_.end()) return it->second;
    return nullptr;
}

llvm::Function* CodeGen::findClassInstanceMethod(const std::string& className,
                                                 const std::string& methodName) {
    if (auto* exact = findClassMethod(className, methodName))
        return exact;
    if (!unit_) return nullptr;

    auto baseName = [](const std::string& name) {
        auto lt = name.find('<');
        return lt == std::string::npos ? name : name.substr(0, lt);
    };
    auto methodBaseName = baseName(methodName);

    auto findClassDecl = [&](const std::string& name) -> const ClassDecl* {
        const ClassDecl* importedMatch = nullptr;
        const std::string base = baseName(name);
        for (auto& d : unit_->declarations) {
            if (!d || d->kind != DeclKind::Class) continue;
            if (d->name != name && d->name != base) continue;
            auto* cd = d->as<const ClassDecl>();
            if (!d->isImported) return cd;
            if (!importedMatch) importedMatch = cd;
        }
        return importedMatch;
    };

    std::set<std::string> seen;
    std::string cursor = className;
    while (seen.insert(cursor).second) {
        auto* cd = findClassDecl(cursor);
        if (!cd || cd->parentName.empty()) break;

        const std::string& parentName = cd->parentName;
        auto* parentDecl = findClassDecl(parentName);
        if (!parentDecl) break;

        bool hasNonStaticMethod = false;
        for (auto& method : parentDecl->methods) {
            if (!method.isStatic && method.name == methodBaseName) {
                hasNonStaticMethod = true;
                break;
            }
        }
        if (hasNonStaticMethod) {
            if (auto* inherited = findClassMethod(parentName, methodName))
                return inherited;
        }
        cursor = parentName;
    }
    return nullptr;
}

// Primitive-target impl detection (mirrors Sema::isPrimitiveTypeName).
// Keep in sync with that table: both are consulted by the `impl Trait for
// <prim>` feature (2026-04-23) and they must agree on the name space.
static bool codegenIsPrimitiveImplTarget(const std::string& name) {
    static const std::set<std::string> kPrim = {
        "i8", "i16", "i32", "i64",
        "u8", "u16", "u32", "u64",
        "f32", "f64",
        "bool", "char",
        "rawptr", "isize", "usize",
        "str",
        "string",
    };
    return kPrim.count(name) > 0;
}

// Map a primitive impl-target name to the LLVM value type CodeGen uses for
// that primitive.  Used when emitting `impl Trait for <prim>` method bodies
// so `self` arrives by value (not behind a pointer).  Mirrors the type
// arithmetic in CodeGenTypeMap.cpp's toLLVMType for the built-ins we care
// about.  Returns nullptr for non-primitive names.
llvm::Type* CodeGen::primitiveImplSelfLLVMType(const std::string& name) {
    if (name == "i8"  || name == "u8")  return llvm::Type::getInt8Ty(*context_);
    if (name == "i16" || name == "u16") return llvm::Type::getInt16Ty(*context_);
    if (name == "i32" || name == "u32" || name == "char")
        return llvm::Type::getInt32Ty(*context_);
    if (name == "i64" || name == "u64" || name == "isize" || name == "usize")
        return llvm::Type::getInt64Ty(*context_);
    if (name == "f32") return llvm::Type::getFloatTy(*context_);
    if (name == "f64") return llvm::Type::getDoubleTy(*context_);
    if (name == "bool") return llvm::Type::getInt1Ty(*context_);
    if (name == "rawptr") return llvm::PointerType::getUnqual(*context_);
    if (name == "str" || name == "string") {
        // `string` is a two-slot struct `{ ptr, i64 }` (see CodeGenTypeMap).
        auto it = structTypes_.find(name);
        if (it == structTypes_.end()) it = structTypes_.find("string");
        if (it != structTypes_.end()) return it->second;
        return nullptr;
    }
    return nullptr;
}

void CodeGen::emitClassDecl(const Decl& decl, bool declareOnly) {
    auto& cls = *decl.as<const ClassDecl>();
    auto savedClassName = currentClassName_;
    auto savedGenericParams = genericTypeParams_;
    auto savedGenericParamNames = genericTypeParamNames_;
    currentClassName_ = decl.name;

    setupGenericTypeParams(decl);

    // Primitive-target impl block — `impl Hashable for i32 { ... }`.
    // Emit each method as a free function mangled `i32.hash` so the
    // existing CodeGenMethodCall dispatch (`typeName + "." + method`)
    // finds it via resolveClassName returning "i32" for i32 receivers.
    // `self` is passed by value rather than by pointer; the primitive has
    // no struct layout to skip emitStructDecl for.
    const bool isPrimImpl = cls.isImplBlock && codegenIsPrimitiveImplTarget(decl.name);

    if (!cls.isImplBlock) {
        emitStructDecl(decl);
    }

    // BUG-LV-02 fix — synthesize per-subclass copies of every non-overridden
    // inherited method so that `self.X()` inside a parent body resolves
    // through the subclass's `currentClassName_` and lands on the most-
    // derived implementation. Without this, a Dog instance dispatched into
    // the parent's `Animal::greet` would static-call `Animal::kind`, losing
    // polymorphism (and corrupting reads when the layout offset on `self`
    // is read against the wrong class). Re-using the parent's MethodDecl
    // AST is safe: CodeGen only reads from it, and `currentClassName_` is
    // the only thing that needs to differ between the original copy on the
    // parent and the synthesized copy on the child.
    std::vector<const MethodDecl*> inheritedMethods;
    if (!cls.isImplBlock && !cls.parentName.empty() && unit_) {
        std::set<std::string> ownNames;
        for (auto& m : cls.methods) ownNames.insert(m.name);
        std::set<std::string> visited;
        std::string cursor = cls.parentName;
        while (!cursor.empty() && visited.insert(cursor).second) {
            const ClassDecl* parentDecl = nullptr;
            for (auto& d : unit_->declarations) {
                if (d && d->kind == DeclKind::Class && d->name == cursor) {
                    parentDecl = d->as<const ClassDecl>();
                    break;
                }
            }
            if (!parentDecl) break;
            for (auto& pm : parentDecl->methods) {
                if (pm.isStatic) continue;            // statics are not inherited at the dispatch level
                if (!pm.genericParams.empty()) continue; // method-level generics: skip (Mono pre-instantiates)
                if (!ownNames.insert(pm.name).second) continue;
                inheritedMethods.push_back(&pm);
            }
            cursor = parentDecl->parentName;
        }
    }

    // Forward-declare the class's own methods plus a synthetic copy of each
    // non-overridden inherited method (BUG-LV-02). The lambda body matches
    // the original loop body verbatim; the only thing that varies between
    // the "own" and "inherited" passes is which MethodDecl AST is consumed.
    auto emitMethodForwardDecl = [&](const MethodDecl& method) {
        if (!method.genericParams.empty()) return;
        std::string mangledName = decl.name + "." + method.name;
        if (functions_.count(mangledName)) return;

        llvm::Type* retTy = method.returnType
            ? toLLVMType(*method.returnType)
            : llvm::Type::getVoidTy(*context_);

        // Explicit-self receiver (Rust-style `fn foo(self, ...)`) is parsed
        // into method.params[0] with name "self" and no type annotation.
        // Codegen models the receiver as the implicit `self` pointer added
        // below, so skip the param entry to avoid generating a bogus extra
        // i32 arg (which would otherwise shadow `self` in namedValues_ and
        // produce `load i32, ptr %self` for every field access).
        size_t selfParamOffset = (!method.isStatic && !method.params.empty() &&
                                   method.params[0].name == "self") ? 1 : 0;

        std::vector<llvm::Type*> paramTypes;
        if (!method.isStatic) {
            // Primitive-target impl: pass self by value so the call site
            // (which has the primitive value, not an alloca pointer) can
            // hand it over directly.
            if (isPrimImpl) {
                auto* selfTy = primitiveImplSelfLLVMType(decl.name);
                paramTypes.push_back(selfTy ? selfTy : llvm::PointerType::getUnqual(*context_));
            } else {
                paramTypes.push_back(llvm::PointerType::getUnqual(*context_));
            }
        }
        for (size_t pi = selfParamOffset; pi < method.params.size(); ++pi) {
            auto& p = method.params[pi];
            paramTypes.push_back(p.type ? toLLVMType(*p.type) : llvm::Type::getInt32Ty(*context_));
        }

        auto* fnType = llvm::FunctionType::get(retTy, paramTypes, false);
        auto* fn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, mangledName, *module_);

        if (!method.isStatic) fn->arg_begin()->setName("self");
        size_t idx = method.isStatic ? 0 : 1;
        for (auto& arg : llvm::make_range(
                fn->arg_begin() + (method.isStatic ? 0 : 1), fn->arg_end())) {
            size_t paramIdx = (method.isStatic ? idx : idx - 1) + selfParamOffset;
            if (paramIdx < method.params.size())
                arg.setName(method.params[paramIdx].name);
            ++idx;
        }

        for (auto& [attrName, attrValue] : method.attributes) {
            if (attrName == "noinline") fn->addFnAttr(llvm::Attribute::NoInline);
            else if (attrName == "inline") fn->addFnAttr(llvm::Attribute::AlwaysInline);
            else if (attrName == "align" && !attrValue.empty()) {
                char* end = nullptr;
                unsigned long alignment = std::strtoul(attrValue.c_str(), &end, 10);
                if (end != attrValue.c_str() && alignment > 0)
                    fn->setAlignment(llvm::Align(alignment));
            }
        }

        functions_[mangledName] = fn;
    };

    for (auto& method : cls.methods) emitMethodForwardDecl(method);
    for (auto* mp : inheritedMethods)  emitMethodForwardDecl(*mp);

    if (declareOnly) {
        currentClassName_ = savedClassName;
        genericTypeParams_ = savedGenericParams;
        genericTypeParamNames_ = savedGenericParamNames;
        return;
    }

    // Mono clears genericParams on every concrete instance it emits, so a
    // non-empty genericParams here means this is still an uninstantiated
    // template — skip body emission entirely.
    if (!decl.genericParams.empty()) {
        currentClassName_ = savedClassName;
        genericTypeParams_ = savedGenericParams;
        genericTypeParamNames_ = savedGenericParamNames;
        return;
    }

    auto emitMethodBody = [&](const MethodDecl& method) {
        if (!method.genericParams.empty()) return;
        std::string mangledName = decl.name + "." + method.name;
        auto fnIt = functions_.find(mangledName);
        auto* fn = (fnIt != functions_.end()) ? fnIt->second : nullptr;
        if (!fn || !method.body) return;
        if (!fn->empty()) return;

        auto* entry = llvm::BasicBlock::Create(*context_, "entry", fn);
        builder_->SetInsertPoint(entry);
        if (emitDebug_ && debugBuilder_ && debugFile_) {
            auto* fnDI = debugBuilder_->createFunction(
                debugFile_, mangledName, mangledName, debugFile_,
                method.location.line,
                debugBuilder_->createSubroutineType(debugBuilder_->getOrCreateTypeArray({})),
                method.location.line, llvm::DINode::FlagZero,
                llvm::DISubprogram::SPFlagDefinition);
            fn->setSubprogram(fnDI);
            emitDebugLocation(method.location);
        }

        auto savedValues = namedValues_;
        auto savedSideMaps = snapshotVarScopeSideMaps();
        namedValues_.clear();
        clearVarScopeSideMaps();

        if (!method.isStatic) {
            if (isPrimImpl) {
                // Primitive `self`: alloca of the primitive's value type so
                // loads of `self` produce the value directly, matching how
                // the user's `fn hash() -> i64 { return hash_i32(self); }`
                // body expects `self: i32`.
                auto* selfTy = primitiveImplSelfLLVMType(decl.name);
                if (!selfTy) selfTy = llvm::Type::getInt64Ty(*context_);
                auto* selfAlloca = createEntryBlockAlloca(fn, selfTy, "self");
                builder_->CreateStore(fn->arg_begin(), selfAlloca);
                namedValues_["self"] = selfAlloca;
                // Do NOT set classVarTypes_["self"] for primitives — there is
                // no matching structTypes_ entry and the instance-dispatch
                // path would mis-route subsequent `self.foo()` calls.
            } else {
                auto* selfPtrTy = llvm::PointerType::getUnqual(*context_);
                auto* selfAlloca = createEntryBlockAlloca(fn, selfPtrTy, "self");
                builder_->CreateStore(fn->arg_begin(), selfAlloca);
                namedValues_["self"] = selfAlloca;
                classVarTypes_["self"] = decl.name;
            }
        }

        size_t bodySelfParamOffset = (!method.isStatic && !method.params.empty() &&
                                        method.params[0].name == "self") ? 1 : 0;
        size_t argIdx = method.isStatic ? 0 : 1;
        for (auto& arg : llvm::make_range(
                fn->arg_begin() + (method.isStatic ? 0 : 1), fn->arg_end())) {
            size_t paramIdx = (method.isStatic ? argIdx : argIdx - 1) + bodySelfParamOffset;
            std::string paramName = std::string(arg.getName());
            if (paramIdx < method.params.size()) paramName = method.params[paramIdx].name;
            auto* alloca = createEntryBlockAlloca(fn, arg.getType(), paramName);
            builder_->CreateStore(&arg, alloca);
            namedValues_[paramName] = alloca;
            if (paramIdx < method.params.size()) {
                trackParamType(method.params[paramIdx]);
                // Fn-typed method parameters arrive as fat pointers (fn+env)
                // from the call site (see emitVarDecl's closure lowering).
                // Register them in closureFatPtrVars_ so calls to the param
                // extract fn+env from the fat struct rather than calling the
                // fat-pointer address directly as machine code.
                auto& p = method.params[paramIdx];
                if (p.type && p.type->kind == TypeAnnotationKind::Function) {
                    auto& ft = static_cast<const FunctionType&>(*p.type);
                    llvm::Type* retTy = llvm::Type::getInt64Ty(*context_);
                    if (ft.returnType) {
                        auto* infer = toLLVMType(*ft.returnType);
                        if (infer && !infer->isVoidTy()) retTy = infer;
                    }
                    closureFatPtrVars_[p.name] = retTy;
                }
            }
            ++argIdx;
        }

        emitBlock(*method.body);

        auto* lastBB = builder_->GetInsertBlock();
        if (lastBB && !lastBB->getTerminator()) {
            if (fn->getReturnType()->isVoidTy()) builder_->CreateRetVoid();
            else builder_->CreateRet(llvm::Constant::getNullValue(fn->getReturnType()));
        }

        if (llvm::verifyFunction(*fn, &llvm::errs())) {
            diag_.error(CodeGen::codegenInternalSourceLocation(), "verification failed for function '{}'", fn->getName().str());
        }
        namedValues_ = std::move(savedValues);
        restoreVarScopeSideMaps(std::move(savedSideMaps));
    };

    for (auto& method : cls.methods) emitMethodBody(method);
    for (auto* mp : inheritedMethods)  emitMethodBody(*mp);

    currentClassName_ = savedClassName;
    genericTypeParams_ = savedGenericParams;
    genericTypeParamNames_ = savedGenericParamNames;
}

void CodeGen::instantiateGenericClass(const Decl& genericDecl,
    const std::vector<llvm::Type*>& concreteTypes,
    const std::vector<std::string>& typeNames) {
    if (genericDecl.genericParams.size() != concreteTypes.size()) return;

    std::string mangledName = genericDecl.name + "<";
    for (size_t i = 0; i < typeNames.size(); ++i) {
        if (i > 0) mangledName += ",";
        mangledName += typeNames[i];
    }
    mangledName += ">";

    if (instantiatedClasses_.count(mangledName)) return;
    instantiatedClasses_.insert(mangledName);

    auto savedClassName = currentClassName_;
    auto savedGenericParams = genericTypeParams_;
    auto savedGenericParamNames = genericTypeParamNames_;
    currentClassName_ = mangledName;

    for (size_t i = 0; i < genericDecl.genericParams.size(); ++i) {
        genericTypeParams_[genericDecl.genericParams[i]] = concreteTypes[i];
        genericTypeParamNames_[genericDecl.genericParams[i]] = typeNames[i];
    }

    const auto& genFields = (genericDecl.kind == DeclKind::Class)
        ? genericDecl.as<const ClassDecl>()->fields
        : genericDecl.as<const StructDecl>()->fields;
    const auto& genMethods = genericDecl.as<const ClassDecl>()->methods;

    if (!structTypes_.count(mangledName)) {
        std::vector<llvm::Type*> fieldTypes;
        std::vector<std::string> fieldNames;
        for (auto& field : genFields) {
            if (field.type) {
                fieldTypes.push_back(toLLVMType(*field.type));
                fieldNames.push_back(field.name);
            }
        }
        auto* stTy = llvm::StructType::create(*context_, fieldTypes, mangledName);
        structTypes_[mangledName] = stTy;
        structFieldNames_[mangledName] = fieldNames;
    }

    for (auto& method : genMethods) {
        // Method-level generic params (e.g. `map<U>` on `Iterator<T>`) stay
        // unbound at the class-monomorph step — they are instantiated
        // separately per call-site U. Attempting to emit them here would
        // mean resolving `Iterator<U>` or `sizeof::<U>()` with U still a
        // bare type-parameter name, which mangles to "U<>" and errors out
        // in structTypes_ lookup. Skip them; Mono-synthesised method
        // instantiations (`Iterator<i64>.map<f64>`) carry the bound U
        // and emit via the standalone FunctionDecl path.
        if (!method.genericParams.empty()) continue;

        std::string methodName = mangledName + "." + method.name;
        if (functions_.count(methodName)) continue;

        llvm::Type* retTy = method.returnType
            ? toLLVMType(*method.returnType)
            : llvm::Type::getVoidTy(*context_);

        size_t selfOff = (!method.isStatic && !method.params.empty() &&
                           method.params[0].name == "self") ? 1 : 0;

        std::vector<llvm::Type*> paramTypes;
        if (!method.isStatic) {
            paramTypes.push_back(llvm::PointerType::getUnqual(*context_));
        }
        for (size_t pi = selfOff; pi < method.params.size(); ++pi) {
            auto& p = method.params[pi];
            paramTypes.push_back(p.type ? toLLVMType(*p.type) : llvm::Type::getInt32Ty(*context_));
        }

        auto* fnType = llvm::FunctionType::get(retTy, paramTypes, false);
        auto* fn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, methodName, *module_);
        if (!method.isStatic) fn->arg_begin()->setName("self");

        functions_[methodName] = fn;
    }

    for (auto& method : genMethods) {
        if (!method.genericParams.empty()) continue;
        std::string methodName = mangledName + "." + method.name;
        auto* fn = functions_[methodName];
        if (!fn || !method.body || !fn->empty()) continue;

        auto* entry = llvm::BasicBlock::Create(*context_, "entry", fn);
        builder_->SetInsertPoint(entry);
        if (emitDebug_ && debugBuilder_ && debugFile_) {
            auto* fnDI = debugBuilder_->createFunction(
                debugFile_, methodName, methodName, debugFile_,
                method.location.line,
                debugBuilder_->createSubroutineType(debugBuilder_->getOrCreateTypeArray({})),
                method.location.line, llvm::DINode::FlagZero,
                llvm::DISubprogram::SPFlagDefinition);
            fn->setSubprogram(fnDI);
            emitDebugLocation(method.location);
        }
        auto savedValues = namedValues_;
        namedValues_.clear();

        if (!method.isStatic) {
            auto* selfPtrTy = llvm::PointerType::getUnqual(*context_);
            auto* selfAlloca = createEntryBlockAlloca(fn, selfPtrTy, "self");
            builder_->CreateStore(fn->arg_begin(), selfAlloca);
            namedValues_["self"] = selfAlloca;
            classVarTypes_["self"] = mangledName;
        }

        size_t selfOffBody = (!method.isStatic && !method.params.empty() &&
                               method.params[0].name == "self") ? 1 : 0;
        size_t argIdx = method.isStatic ? 0 : 1;
        for (auto& arg : llvm::make_range(
                fn->arg_begin() + (method.isStatic ? 0 : 1), fn->arg_end())) {
            size_t paramIdx = (method.isStatic ? argIdx : argIdx - 1) + selfOffBody;
            if (paramIdx < method.params.size()) {
                auto* alloca = createEntryBlockAlloca(fn, arg.getType(), method.params[paramIdx].name);
                builder_->CreateStore(&arg, alloca);
                namedValues_[method.params[paramIdx].name] = alloca;
                trackParamType(method.params[paramIdx]);
            }
            ++argIdx;
        }

        if (method.body) emitStmt(*method.body);

        if (builder_->GetInsertBlock() && !builder_->GetInsertBlock()->getTerminator()) {
            if (fn->getReturnType()->isVoidTy())
                builder_->CreateRetVoid();
            else
                builder_->CreateRet(llvm::Constant::getNullValue(fn->getReturnType()));
        }

        if (llvm::verifyFunction(*fn, &llvm::errs())) {
            diag_.error(CodeGen::codegenInternalSourceLocation(), "verification failed for function '{}'", fn->getName().str());
        }
        namedValues_ = std::move(savedValues);
    }

    currentClassName_ = savedClassName;
    genericTypeParams_ = savedGenericParams;
    genericTypeParamNames_ = savedGenericParamNames;
}

} // namespace vyx
