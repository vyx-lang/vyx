#include "CodeGenIncludes.h"

namespace vyx {

void CodeGen::generatePredeclareCRuntime() {
    fprintf(stderr, "[CodeGen] Phase: pre-declare C runtime\n"); fflush(stderr);
    // Pre-declare commonly used C runtime functions to avoid repeated getOrCreate patterns
    auto* ptrTy = llvm::PointerType::getUnqual(*context_);
    auto* i32Ty = llvm::Type::getInt32Ty(*context_);
    auto* i64Ty = llvm::Type::getInt64Ty(*context_);
    auto* voidTy = llvm::Type::getVoidTy(*context_);
    auto declare = [&](const char* name, llvm::FunctionType* ty) {
        if (!module_->getFunction(name))
            llvm::Function::Create(ty, llvm::Function::ExternalLinkage, name, *module_);
    };
    declare("malloc", llvm::FunctionType::get(ptrTy, {i64Ty}, false));
    declare("free", llvm::FunctionType::get(voidTy, {ptrTy}, false));
    declare("memset", llvm::FunctionType::get(ptrTy, {ptrTy, i32Ty, i64Ty}, false));
    declare("memcpy", llvm::FunctionType::get(ptrTy, {ptrTy, ptrTy, i64Ty}, false));
    declare("memcmp", llvm::FunctionType::get(i32Ty, {ptrTy, ptrTy, i64Ty}, false));
    declare("strlen", llvm::FunctionType::get(i64Ty, {ptrTy}, false));
    declare("strcmp", llvm::FunctionType::get(i32Ty, {ptrTy, ptrTy}, false));
    {
        auto* exitTy = llvm::FunctionType::get(voidTy, {i32Ty}, false);
        if (!module_->getFunction("exit")) {
            auto* fn = llvm::Function::Create(exitTy, llvm::Function::ExternalLinkage, "exit", *module_);
            fn->addFnAttr(llvm::Attribute::NoReturn);
        }
    }
    declare("printf", llvm::FunctionType::get(i32Ty, {ptrTy}, true));
    declare("snprintf", llvm::FunctionType::get(i32Ty, {ptrTy, i64Ty, ptrTy}, true));
}

void CodeGen::generateFirstPassDeclareTypes() {
    fprintf(stderr, "[CodeGen] Phase: first pass - declare struct/class types\n"); fflush(stderr);
    // First pass: declare struct/class types
    for (auto& decl : unit_->declarations) {
        if (!decl) continue;
        if (decl->kind == DeclKind::Struct) {
            emitStructDecl(*decl);
        }
        if (decl->kind == DeclKind::Class) {
            setupGenericTypeParams(*decl);
            emitStructDecl(*decl);
            genericTypeParams_.clear();
        }
    }
}

void CodeGen::generatePredeclareSmartPtrRuntime() {
    fprintf(stderr, "[CodeGen] Phase: declare runtime helpers (smart pointers)\n"); fflush(stderr);
    auto* i8PtrTy = llvm::PointerType::getUnqual(*context_);
    auto* i64Ty = llvm::Type::getInt64Ty(*context_);
    if (!module_->getFunction("malloc")) {
        auto* mallocTy = llvm::FunctionType::get(i8PtrTy, {i64Ty}, false);
        auto* mallocFn = llvm::Function::Create(mallocTy, llvm::Function::ExternalLinkage, "malloc", *module_);
        functions_["malloc"] = mallocFn;
    }
    if (!module_->getFunction("free")) {
        auto* freeTy = llvm::FunctionType::get(llvm::Type::getVoidTy(*context_), {i8PtrTy}, false);
        auto* freeFn = llvm::Function::Create(freeTy, llvm::Function::ExternalLinkage, "free", *module_);
        functions_["free"] = freeFn;
    }
}

void CodeGen::generateRegisterErrorEnums() {
    fprintf(stderr, "[CodeGen] Phase: error enum / ADT tagged unions\n"); fflush(stderr);

    // Eagerly register the runtime `__String` LLVM layout (ptr+len+cap+owned
    // = 32 bytes) and its `string` alias. `getOrCreateStringType` installs
    // both `structTypes_["__String"]` and `structTypes_["string"]` pointing
    // at the same llvm::StructType, plus the 4-slot field-name vector under
    // both keys. Doing this BEFORE the ADT payload-width loop below
    // guarantees that user-generic-enum variants like `Either<i32, string>`
    // see the true 32-byte width when `toLLVMType(Class{name="string"})`
    // runs — previously the lookup missed, fell through to opaque pointer,
    // and the wide-payload branch skipped the variant via its `isPointerTy`
    // guard, silently picking the 16-byte `__Result` layout. Registering
    // here also makes the hardcoded `name == "string"` special-case in
    // `toLLVMType(VyxType)` unnecessary — the structTypes_ lookup for
    // `Class{name="string"}` now always succeeds.
    (void)getOrCreateStringType();

    // Generic enum tagged-union dispatch aliases: Option<T>/Result<T,E> share
    // the runtime `__Result` LLVM struct (i32 tag + i64 payload). Without
    // these aliases, `match (self)` inside Mono-synthesised enum methods
    // (Option.map<U>, Result.and_then<U>, ...) can't find a structFieldNames_
    // entry to trigger the pointer-deref branch in CodeGenMatch.cpp:62-71,
    // and the body's `case Some(v)` arm never extracts the payload.
    // Registering the aliases here makes the alias key resolve to the same
    // struct that Option/Result vars carry, so the existing isResultStruct
    // path takes over for the dispatch. Variant tag values (Some=0, None=1,
    // Ok=0, Err=1) match the hand-written constructor convention used by
    // CodeGenStmt's Some/None/Ok/Err emission.
    auto* resultTy = getOrCreateResultType();
    // R5 stage 2: register the shared __Result layout alias under
    // whatever name the stdlib supplied via `@[lang_item("option")]` /
    // `@[lang_item("result")]`. No bootstrap fallback to the literal
    // names — a user who skips stdlib gets no alias registration.
    std::vector<std::pair<std::string, std::vector<std::string>>> enumAliases;
    if (langItems_) {
        if (const Decl* l = langItems_->find("option"))
            enumAliases.push_back({l->name, {"Some", "None"}});
        if (const Decl* l = langItems_->find("result"))
            enumAliases.push_back({l->name, {"Ok", "Err"}});
    }
    for (auto& [enumName, variants] : enumAliases) {
        if (!structTypes_.count(enumName)) {
            structTypes_[enumName] = resultTy;
            structFieldNames_[enumName] = {"__tag", "__data"};
        }
        for (size_t i = 0; i < variants.size(); ++i) {
            std::string key = enumName + "." + variants[i];
            if (!errorEnumValues_.count(key))
                errorEnumValues_[key] = static_cast<int>(i);
        }
    }

    int errorBase = -100;
    for (auto& decl : unit_->declarations) {
        if (!decl || decl->kind != DeclKind::ErrorDef) continue;
        if (!decl->genericParams.empty()) continue;

        auto* errDecl = decl->as<ErrorDefDecl>();
        bool hasDataVariants = false;
        for (auto& vt : errDecl->variantTypes) {
            if (!vt.empty()) { hasDataVariants = true; break; }
        }

        if (hasDataVariants) {
            auto& dl = module_->getDataLayout();
            uint64_t maxDataSize = 0;
            for (size_t i = 0; i < errDecl->variantTypes.size(); ++i) {
                uint64_t variantSize = 0;
                for (auto& t : errDecl->variantTypes[i]) {
                    variantSize += dl.getTypeAllocSize(toLLVMType(*t));
                }
                if (variantSize > maxDataSize) maxDataSize = variantSize;
            }
            if (maxDataSize == 0) maxDataSize = 8;

            auto* tagTy = llvm::Type::getInt32Ty(*context_);
            auto* dataTy = llvm::ArrayType::get(llvm::Type::getInt8Ty(*context_), maxDataSize);
            auto* adtTy = llvm::StructType::create(*context_, {tagTy, dataTy}, decl->name);
            structTypes_[decl->name] = adtTy;
            structFieldNames_[decl->name] = {"__tag", "__data"};
            structFieldNames_[decl->name + ".__max_data_size"] = {std::to_string(maxDataSize)};
        }

        bool isCRepr = false;
        for (auto& [an, av] : decl->attributes) {
            if (an == "c_repr" || an == "c_enum") isCRepr = true;
        }

        for (size_t i = 0; i < errDecl->variants.size(); ++i) {
            std::string key = decl->name + "." + errDecl->variants[i];
            if (hasDataVariants || isCRepr) {
                errorEnumValues_[key] = static_cast<int>(i);
            } else {
                errorEnumValues_[key] = errorBase - static_cast<int>(i);
            }
        }
        if (!hasDataVariants) {
            errorBase -= static_cast<int>(errDecl->variants.size());
        }
    }
}

void CodeGen::generateEmitErrorDefMethods() {
    fprintf(stderr, "[CodeGen] Phase: emit enum methods\n"); fflush(stderr);
    for (auto& decl : unit_->declarations) {
        if (!decl || decl->kind != DeclKind::ErrorDef) continue;
        if (!decl->genericParams.empty()) continue;
        auto* errMethods = decl->as<ErrorDefDecl>();
        if (errMethods->methods.empty()) continue;
        for (auto& method : errMethods->methods) {
            std::string mangledName = decl->name + "." + method.name;

            llvm::Type* retTy = method.returnType
                ? toLLVMType(*method.returnType)
                : llvm::Type::getVoidTy(*context_);

            size_t selfOff = (!method.isStatic && !method.params.empty() &&
                               method.params[0].name == "self") ? 1 : 0;

            std::vector<llvm::Type*> paramTypes;
            if (!method.isStatic) {
                paramTypes.push_back(llvm::Type::getInt64Ty(*context_));
            }
            for (size_t pi = selfOff; pi < method.params.size(); ++pi) {
                auto& p = method.params[pi];
                paramTypes.push_back(p.type ? toLLVMType(*p.type) : llvm::Type::getInt32Ty(*context_));
            }

            auto* fnType = llvm::FunctionType::get(retTy, paramTypes, false);
            auto* fn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, mangledName, *module_);

            if (!method.isStatic) {
                fn->arg_begin()->setName("self");
            }
            size_t idx = method.isStatic ? 0 : 1;
            for (auto it = fn->arg_begin() + (method.isStatic ? 0 : 1); it != fn->arg_end(); ++it, ++idx) {
                size_t paramIdx = (method.isStatic ? idx : idx - 1) + selfOff;
                if (paramIdx < method.params.size())
                    it->setName(method.params[paramIdx].name);
            }

            functions_[mangledName] = fn;

            if (!method.body) continue;

            auto* entry = llvm::BasicBlock::Create(*context_, "entry", fn);
            builder_->SetInsertPoint(entry);

            auto savedValues = namedValues_;
            namedValues_.clear();

            if (!method.isStatic) {
                auto* selfAlloca = createEntryBlockAlloca(fn, llvm::Type::getInt64Ty(*context_), "self");
                builder_->CreateStore(fn->arg_begin(), selfAlloca);
                namedValues_["self"] = selfAlloca;
            }

            idx = method.isStatic ? 0 : 1;
            for (auto it = fn->arg_begin() + (method.isStatic ? 0 : 1); it != fn->arg_end(); ++it, ++idx) {
                auto* alloca = createEntryBlockAlloca(fn, it->getType(), std::string(it->getName()));
                builder_->CreateStore(&*it, alloca);
                namedValues_[std::string(it->getName())] = alloca;
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
        }
    }
}

void CodeGen::generateCreateInterfaceVtables() {
    fprintf(stderr, "[CodeGen] Phase: vtable / interface\n"); fflush(stderr);
    for (auto& decl : unit_->declarations) {
        if (!decl || decl->kind != DeclKind::Interface) continue;

        std::vector<llvm::Type*> vtableFieldTypes;
        std::vector<std::string> vtableFieldNames;
        auto* fnPtrTy = llvm::PointerType::getUnqual(*context_);

        for (auto& method : decl->as<InterfaceDecl>()->methods) {
            if (!method.genericParams.empty()) continue;
            vtableFieldTypes.push_back(fnPtrTy);
            vtableFieldNames.push_back(method.name);
        }

        if (!vtableFieldTypes.empty()) {
            auto* vtableTy = llvm::StructType::create(*context_, vtableFieldTypes, decl->name + "_vtable");
            structTypes_[decl->name + "_vtable"] = vtableTy;
            structFieldNames_[decl->name + "_vtable"] = std::move(vtableFieldNames);
            // Eagerly register the fat-pointer interface struct `__iface_X`
            // so later codegen paths (e.g. `Box::<dyn X>.new` special-case)
            // can detect dyn-trait contexts via a simple structTypes_ lookup
            // without racing the lazy on-demand creators in CodeGenTypeMap /
            // CodeGenVarDecl.
            std::string ifaceKey = "__iface_" + decl->name;
            if (!structTypes_.count(ifaceKey)) {
                auto* ptrTy = llvm::PointerType::getUnqual(*context_);
                auto* ifaceTy = llvm::StructType::create(*context_, {ptrTy, ptrTy}, ifaceKey);
                structTypes_[ifaceKey] = ifaceTy;
            }
        }
    }
}

void CodeGen::generateEmitDefaultTraitMethods() {
    fprintf(stderr, "[CodeGen] Phase: emit default trait method implementations\n"); fflush(stderr);
    // Default method bodies are cloned by Sema (analyzeClassDecl) into every
    // implementing class's method list.  Each implementing class emits its own
    // copy with the correct `currentClassName_` via emitClassDecl.
    //
    // Here we only forward-declare the interface-level function (no body) so
    // that generatePopulateInstanceVtables can reference it as a fallback
    // pointer.  We do NOT emit a body because the body may contain `self.X()`
    // calls that require knowing the concrete class at codegen time; emitting
    // the body here with no currentClassName_ set causes "undefined function ''"
    // errors for those dispatches.
    auto signatureMentionsSelfAssoc = [](const TypeAnnotation* ann) -> bool {
        if (!ann) return false;
        if (ann->kind == TypeAnnotationKind::Dependent) {
            const auto* dt = static_cast<const DependentType*>(ann);
            if (dt->baseName == "Self") return true;
        }
        return false;
    };
    for (auto& decl : unit_->declarations) {
        if (!decl || decl->kind != DeclKind::Interface) continue;
        for (auto& method : decl->as<InterfaceDecl>()->methods) {
            if (!method.body) continue;
            std::string mangledName = decl->name + "." + method.name;
            if (functions_.count(mangledName)) continue;

            // BUG-LV-14: skip the trait-level forward declaration when the
            // signature references `Self::Assoc`. The trait alone has no
            // bound `Self`, so `toLLVMType` cannot lower the dependent
            // type. Per-class copies emitted by emitClassDecl carry the
            // bound `Self = ClassName` and produce the real definition;
            // the vtable populator falls back to those copies via the
            // primary `ClassName.method` lookup, so the absent trait fwd
            // decl is harmless in correctly-implemented programs.
            if (signatureMentionsSelfAssoc(method.returnType.get())) continue;
            bool paramSelfAssoc = false;
            for (auto& p : method.params) {
                if (signatureMentionsSelfAssoc(p.type.get())) {
                    paramSelfAssoc = true;
                    break;
                }
            }
            if (paramSelfAssoc) continue;

            llvm::Type* retTy = method.returnType
                ? toLLVMType(*method.returnType)
                : llvm::Type::getVoidTy(*context_);

            size_t ifaceSelfOff = (!method.params.empty() && method.params[0].name == "self") ? 1 : 0;
            std::vector<llvm::Type*> paramTypes = {llvm::PointerType::getUnqual(*context_)};
            for (size_t pi = ifaceSelfOff; pi < method.params.size(); ++pi) {
                auto& p = method.params[pi];
                paramTypes.push_back(p.type ? toLLVMType(*p.type) : llvm::Type::getInt32Ty(*context_));
            }

            auto* fnType = llvm::FunctionType::get(retTy, paramTypes, false);
            auto* fn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, mangledName, *module_);
            fn->arg_begin()->setName("self");
            functions_[mangledName] = fn;
            // Body is intentionally omitted here; the concrete per-class copy
            // (ClassName.methodName) emitted by emitClassDecl carries the body.
        }
    }
}

void CodeGen::generatePreCreateClassStructs() {
    fprintf(stderr, "[CodeGen] Phase: pre-create class struct types\n"); fflush(stderr);
    for (auto& decl : unit_->declarations) {
        if (!decl || decl->kind != DeclKind::Class) continue;
        emitStructDecl(*decl);
    }
}

void CodeGen::generateEmitGlobalVars() {
    fprintf(stderr, "[CodeGen] Phase: global variables\n"); fflush(stderr);
    for (auto& decl : unit_->declarations) {
        if (!decl || decl->kind != DeclKind::GlobalVar) continue;
        emitDecl(*decl);
    }
}

void CodeGen::generateClassMethodsForwardDeclare() {
    fprintf(stderr, "[CodeGen] Phase: class methods pass 1 (forward-declare)\n"); fflush(stderr);
    for (auto& decl : unit_->declarations) {
        if (!decl) continue;
        if (decl->kind != DeclKind::Class) continue;
        // See generateClassMethodsEmitBodies: skip generic class templates
        // (their forward declarations are emitted on demand by the
        // pending-generic-class instantiation phase). Full specializations
        // have empty `genericParams` so they fall through here normally.
        if (!decl->genericParams.empty() && !decl->isFullSpecialization) continue;
        bool skipPlat = false;
        for (auto& [an, av] : decl->attributes) {
            if (an == "platform") {
#ifdef _WIN32
                if (av != "windows") skipPlat = true;
#elif __linux__
                if (av != "linux") skipPlat = true;
#elif __APPLE__
                if (av != "macos") skipPlat = true;
#endif
            }
        }
        if (!skipPlat) emitClassDecl(*decl, /*declareOnly=*/true);
    }
}

void CodeGen::generateClassMethodsEmitBodies() {
    fprintf(stderr, "[CodeGen] Phase: class methods pass 2 (emit bodies)\n"); fflush(stderr);
    for (auto& decl : unit_->declarations) {
        if (!decl) continue;
        if (decl->kind != DeclKind::Class) continue;
        // Generic class templates (`class Holder<T> { ... }`) cannot be
        // codegen'd directly: their bodies refer to template parameters
        // (`T`, ...) that are only bound during monomorphisation. The
        // monomorphizer clones a per-instantiation `Holder<i32>`, etc.
        // ClassDecl into the unit and the pending-generic-class phase
        // emits them. Full template specializations are not generic
        // (parser clears `genericParams` and stamps `isFullSpecialization`)
        // so they fall through to `emitClassDecl` here, exactly like a
        // hand-written non-generic class — which is what we want.
        if (!decl->genericParams.empty() && !decl->isFullSpecialization) continue;
        bool skipPlat = false;
        for (auto& [an, av] : decl->attributes) {
            if (an == "platform") {
#ifdef _WIN32
                if (av != "windows") skipPlat = true;
#elif __linux__
                if (av != "linux") skipPlat = true;
#elif __APPLE__
                if (av != "macos") skipPlat = true;
#endif
            }
        }
        if (!skipPlat) emitClassDecl(*decl);
    }
}

void CodeGen::generateFreeFunctionBodies() {
    fprintf(stderr, "[CodeGen] Phase: third pass - emit function bodies (emitFunctionDecl)\n"); fflush(stderr);
    for (auto& decl : unit_->declarations) {
        if (!decl) continue;
        if (decl->kind == DeclKind::Function) {
            if (!decl->genericParams.empty()) continue;
            emitFunctionDecl(*decl);
        }
    }
}

void CodeGen::generateFinalizeModule() {
    fprintf(stderr, "[CodeGen] Phase: verify module (no silent linkage downgrades)\n"); fflush(stderr);
    for (auto& fn : *module_) {
        if (fn.isDeclaration() && fn.hasInternalLinkage()) {
            diag_.error(CodeGen::codegenInternalSourceLocation(), "internal linkage function `{}` has no definition after codegen",
                fn.getName().str());
        }
    }

    if (emitDebug_ && debugBuilder_) {
        debugBuilder_->finalize();
    }
}

void CodeGen::generate(const TranslationUnit& unit) {
    if (diag_.hasErrors()) return;
    unit_ = &unit;

    generatePredeclareCRuntime();
    generateFirstPassDeclareTypes();
    // Phase 8 (2026-04-23): `@[derive(...)]` synthesis now runs in Sema
    // (see Sema::synthesizeDeriveMethods) so the injected methods go through
    // the normal class-method emission path. The old CodeGen-level pass is
    // neutered below to avoid emitting duplicate LLVM functions.
    generateDeriveMethods();
    generatePredeclareSmartPtrRuntime();
    generateRegisterErrorEnums();
    generateEmitErrorDefMethods();
    generateCreateInterfaceVtables();
    generateEmitDefaultTraitMethods();
    generatePreCreateClassStructs();
    generateForwardDeclareFunctionsAndExtern();
    generateEmitGlobalVars();
    generateClassMethodsForwardDeclare();
    generateClassMethodsEmitBodies();
    generatePopulateInstanceVtables();
    generateFreeFunctionBodies();
    generateInstantiatePendingGenericClasses();
    generateFinalizeModule();

    fprintf(stderr, "[CodeGen] Phase: generate() COMPLETE\n"); fflush(stderr);
}

} // namespace vyx
