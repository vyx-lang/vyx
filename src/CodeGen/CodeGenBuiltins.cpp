#include "CodeGenIncludes.h"

namespace vyx {

// Some(value) / None / ADT variant construction
llvm::Value* CodeGen::emitBuiltinContainer(const Expr& expr) {
    auto& call = static_cast<const CallExpr&>(expr);
    if (!call.callee || call.callee->kind != ExprKind::Identifier) {
        diag_.error(call.location, "internal: container builtin requires identifier callee");
        return nullptr;
    }
    auto& name = call.callee->as<IdentifierExpr>()->name;
    auto typeAnnIsFunction = [](const TypePtr& ty) {
        return ty && ty->kind == TypeAnnotationKind::Function;
    };
    auto vyxTypeIsFunction = [](const VyxTypePtr& ty) {
        return ty && ty->kind == VyxTypeKind::Function;
    };
    auto inferredVariantPayloadIsFunction = [&](const std::string& variant) {
        if (!call.inferredType) return false;
        if (variant == "Some") {
            return vyxTypeIsFunction(optionInner(*call.inferredType));
        }
        if (variant == "Ok") {
            return vyxTypeIsFunction(resultOk(*call.inferredType));
        }
        if (variant == "Err") {
            return vyxTypeIsFunction(resultErr(*call.inferredType));
        }
        return false;
    };
    auto builtinVariantPayloadIsFunction = [&](const std::string& variant) {
        if (inferredVariantPayloadIsFunction(variant)) return true;
        auto* idCallee = call.callee->as<const IdentifierExpr>();
        if (variant == "Some" && !idCallee->callTypeArgs.empty()) {
            return typeAnnIsFunction(idCallee->callTypeArgs[0]);
        }
        if (variant == "Ok" && !idCallee->callTypeArgs.empty()) {
            return typeAnnIsFunction(idCallee->callTypeArgs[0]);
        }
        if (variant == "Err" && idCallee->callTypeArgs.size() >= 2) {
            return typeAnnIsFunction(idCallee->callTypeArgs[1]);
        }
        return false;
    };
    auto typeArgsVariantPayloadIsFunction = [&](const std::string& enumName,
                                                const std::string& variant,
                                                const IdentifierExpr* callee) {
        if (!callee) return false;
        const auto& typeArgs = callee->callTypeArgs;
        if (enumName == "Option" && variant == "Some" &&
            !typeArgs.empty()) {
            return typeAnnIsFunction(typeArgs[0]);
        }
        if (enumName == "Result" && variant == "Ok" &&
            !typeArgs.empty()) {
            return typeAnnIsFunction(typeArgs[0]);
        }
        if (enumName == "Result" && variant == "Err" &&
            typeArgs.size() >= 2) {
            return typeAnnIsFunction(typeArgs[1]);
        }
        return false;
    };
    auto wrapFnPayloadIfNeeded = [&](llvm::Value* argVal, const Expr* argExpr,
                                     bool expectsFunctionPayload) -> llvm::Value* {
        if (!argVal || !expectsFunctionPayload) return argVal;
        if (auto* rawFn = llvm::dyn_cast<llvm::Function>(argVal))
            return wrapRawFnAsFatPtr(rawFn);
        if (argExpr && argExpr->kind == ExprKind::Identifier) {
            auto& argName = argExpr->as<const IdentifierExpr>()->name;
            if (!closureFatPtrVars_.count(argName)) {
                auto fit = functions_.find(argName);
                if (fit != functions_.end())
                    return wrapRawFnAsFatPtr(fit->second);
            }
        }
        return argVal;
    };

    // Built-in Some(value) → ADT { tag=0, data=value }
    if (name == "Some") {
        auto* fn = builder_->GetInsertBlock()->getParent();
        llvm::StructType* stTy = resolveOptionResultStructForExpr(expr, true);
        if (!stTy) stTy = getOrCreateResultType();
        if (!stTy || stTy->isOpaque() || stTy->getNumElements() < 2) {
            diag_.error(call.location,
                "internal: Some(...) requires a materialized two-field Option layout");
            hadHardTypeError_ = true;
            return nullptr;
        }
        auto* alloca = createEntryBlockAlloca(fn, stTy, "option.some");
        auto* tagTy = stTy->getElementType(0);
        int someTag = 0;
        if (stTy->hasName() && isOptionSlotName(stTy->getName())) {
            auto evIt = errorEnumValues_.find(stTy->getName().str() + ".Some");
            if (evIt != errorEnumValues_.end()) someTag = evIt->second;
        }
        builder_->CreateStore(llvm::ConstantInt::get(tagTy, someTag),
            builder_->CreateStructGEP(stTy, alloca, 0, "some.tag"));
        auto* valPtr = builder_->CreateStructGEP(stTy, alloca, 1, "some.val");
        if (!call.args.empty() && call.args[0]) {
            auto* savedHint = targetTypeHint_;
            if (auto* payloadHint = variantPayloadOptionResultHint(expr, stTy, "Some"))
                targetTypeHint_ = payloadHint;
            else
                targetTypeHint_ = nullptr;
            auto* argVal = emitExpr(*call.args[0]);
            targetTypeHint_ = savedHint;
            if (!argVal) {
                diag_.error(call.location, "Some(...): argument did not produce a value");
                return nullptr;
            }
            argVal = wrapFnPayloadIfNeeded(argVal, call.args[0].get(),
                builtinVariantPayloadIsFunction("Some"));
            auto* destTy = stTy->getElementType(1);
            if (destTy->isArrayTy()) {
                // The payload field is a byte array (wide Option for large inner
                // types).  When argVal is a struct, write each field at its
                // compact byte offset rather than storing the struct as a whole.
                auto* i8Ty = llvm::Type::getInt8Ty(*context_);
                auto* i64Ty = llvm::Type::getInt64Ty(*context_);

                // Interface-box path: if the payload capacity suggests an
                // __iface_X pair (16 bytes) and the argument is a concrete
                // class registered with a *_vtable_* global, box it into a
                // {obj_ptr, vtable_ptr} pair first so that later
                // match-pattern extraction of `case Some(s)` yields a
                // well-formed interface value. Without this, Circle's
                // single f64 lands in the low 8 bytes and the iface's
                // vtable slot stays uninitialised — segfault on
                // `s.method()` later.
                bool boxedForIface = false;
                if (auto* argStTy = llvm::dyn_cast<llvm::StructType>(argVal->getType())) {
                    uint64_t payloadSz = destTy->getArrayNumElements();
                    uint64_t ptrSz = module_->getDataLayout().getPointerSize();
                    if (argStTy->hasName() && payloadSz == 2 * ptrSz &&
                        !argStTy->getName().starts_with("__iface_")) {
                        std::string concreteName = argStTy->getName().str();
                        std::string vtPrefix = concreteName + "_vtable_";
                        const llvm::GlobalVariable* vtMatch = nullptr;
                        std::string ifaceName;
                        for (auto& gv : module_->globals()) {
                            if (gv.getName().starts_with(vtPrefix)) {
                                vtMatch = &gv;
                                ifaceName = gv.getName().str().substr(vtPrefix.size());
                                break;
                            }
                        }
                        if (vtMatch) {
                            // Heap-allocate so the concrete value survives
                            // past the current frame. An alloca here would
                            // escape into a returned Option<Shape>, and the
                            // caller would dereference freed stack memory.
                            auto* ptrTy = llvm::PointerType::getUnqual(*context_);
                            uint64_t sz = module_->getDataLayout().getTypeAllocSize(argStTy);
                            if (sz < 1) sz = 8;
                            auto* mallocFn = module_->getFunction("malloc");
                            if (!mallocFn) {
                                auto* fty = llvm::FunctionType::get(ptrTy, {i64Ty}, false);
                                mallocFn = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, "malloc", *module_);
                            }
                            auto* heapObj = builder_->CreateCall(mallocFn,
                                {llvm::ConstantInt::get(i64Ty, sz)}, "opt.iface.heap");
                            builder_->CreateStore(argVal, heapObj);
                            auto* dst0 = builder_->CreateGEP(i8Ty, valPtr,
                                llvm::ConstantInt::get(i64Ty, 0), "opt.iface.dst0");
                            builder_->CreateStore(heapObj, dst0);
                            auto* dst1 = builder_->CreateGEP(i8Ty, valPtr,
                                llvm::ConstantInt::get(i64Ty, ptrSz), "opt.iface.dst1");
                            builder_->CreateStore(const_cast<llvm::GlobalVariable*>(vtMatch), dst1);
                            boxedForIface = true;
                        }
                    }
                }
                if (!boxedForIface) {
                    if (auto* argStTy = llvm::dyn_cast<llvm::StructType>(argVal->getType())) {
                        auto* argAlloca = createEntryBlockAlloca(
                            builder_->GetInsertBlock()->getParent(), argStTy, "some.inner.tmp");
                        builder_->CreateStore(argVal, argAlloca);
                        uint64_t payloadSz = destTy->getArrayNumElements();
                        uint64_t compactOffset = 0;
                        for (unsigned fi = 0; fi < argStTy->getNumElements(); ++fi) {
                            auto* fieldTy = argStTy->getElementType(fi);
                            uint64_t fieldSz = module_->getDataLayout().getTypeStoreSize(fieldTy);
                            if (compactOffset + fieldSz > payloadSz) {
                                diag_.error(call.location,
                                    "Some(...): payload struct is too large for destination slot");
                                hadHardTypeError_ = true;
                                return nullptr;
                            }
                            auto* srcPtr = builder_->CreateStructGEP(argStTy, argAlloca, fi, "src.fld");
                            auto* fieldVal = builder_->CreateLoad(fieldTy, srcPtr, "src.fld.val");
                            auto* dstPtr = builder_->CreateGEP(
                                i8Ty, valPtr,
                                llvm::ConstantInt::get(i64Ty, compactOffset), "dst.fld.ptr");
                            builder_->CreateStore(fieldVal, dstPtr);
                            compactOffset += fieldSz;
                        }
                    } else {
                        auto* dataPtr = builder_->CreateGEP(
                            i8Ty, valPtr, llvm::ConstantInt::get(i64Ty, 0));
                        builder_->CreateStore(argVal, dataPtr);
                    }
                }
            } else {
                builder_->CreateStore(castToType(argVal, destTy), valPtr);
            }
        } else {
            builder_->CreateStore(llvm::Constant::getNullValue(stTy->getElementType(1)), valPtr);
        }
        return builder_->CreateLoad(stTy, alloca, "some.result");
    }

    // Built-in Ok(value) / Err(value) → ADT { tag=0/1, data=value }
    if (name == "Ok" || name == "Err") {
        auto* fn = builder_->GetInsertBlock()->getParent();
        llvm::StructType* stTy = resolveOptionResultStructForExpr(expr, true);
        if (!stTy) stTy = getOrCreateResultType();

        std::string label = (name == "Ok") ? std::string("result.ok") : std::string("result.err");
        auto* alloca = createEntryBlockAlloca(fn, stTy, label);
        auto* tagTy = stTy->getElementType(0);
        int tagVal = (name == "Ok") ? 0 : 1;
        if (stTy->hasName() && isResultSlotName(stTy->getName())) {
            auto evIt = errorEnumValues_.find(stTy->getName().str() + "." + name);
            if (evIt != errorEnumValues_.end()) tagVal = evIt->second;
        }
        builder_->CreateStore(llvm::ConstantInt::get(tagTy, tagVal),
            builder_->CreateStructGEP(stTy, alloca, 0, label + ".tag"));
        auto* valPtr = builder_->CreateStructGEP(stTy, alloca, 1, label + ".val");
        if (!call.args.empty() && call.args[0]) {
            auto* savedHint = targetTypeHint_;
            if (auto* payloadHint = variantPayloadOptionResultHint(expr, stTy, name))
                targetTypeHint_ = payloadHint;
            else
                targetTypeHint_ = nullptr;
            auto* argVal = emitExpr(*call.args[0]);
            targetTypeHint_ = savedHint;
            if (!argVal) {
                diag_.error(call.location, "{}(...): argument did not produce a value", name);
                return nullptr;
            }
            argVal = wrapFnPayloadIfNeeded(argVal, call.args[0].get(),
                builtinVariantPayloadIsFunction(name));
            auto* destTy = stTy->getElementType(1);
            if (destTy->isArrayTy()) {
                auto* i8Ty = llvm::Type::getInt8Ty(*context_);
                auto* i64Ty = llvm::Type::getInt64Ty(*context_);
                // Mirror Some()'s interface-box fast path: when the
                // payload slot is 2*ptr wide and the arg is a concrete
                // class with a known vtable for some interface, box it
                // into a proper {obj_ptr, vtable_ptr} pair so later
                // `case Ok(s) => s.method()` matches correctly.
                bool boxedForIface = false;
                if (auto* argStTy = llvm::dyn_cast<llvm::StructType>(argVal->getType())) {
                    uint64_t payloadSz = destTy->getArrayNumElements();
                    uint64_t ptrSz = module_->getDataLayout().getPointerSize();
                    if (argStTy->hasName() && payloadSz == 2 * ptrSz &&
                        !argStTy->getName().starts_with("__iface_")) {
                        std::string concreteName = argStTy->getName().str();
                        std::string vtPrefix = concreteName + "_vtable_";
                        const llvm::GlobalVariable* vtMatch = nullptr;
                        for (auto& gv : module_->globals()) {
                            if (gv.getName().starts_with(vtPrefix)) {
                                vtMatch = &gv;
                                break;
                            }
                        }
                        if (vtMatch) {
                            auto* ptrTy = llvm::PointerType::getUnqual(*context_);
                            uint64_t sz = module_->getDataLayout().getTypeAllocSize(argStTy);
                            if (sz < 1) sz = 8;
                            auto* mallocFn = module_->getFunction("malloc");
                            if (!mallocFn) {
                                auto* fty = llvm::FunctionType::get(ptrTy, {i64Ty}, false);
                                mallocFn = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, "malloc", *module_);
                            }
                            auto* heapObj = builder_->CreateCall(mallocFn,
                                {llvm::ConstantInt::get(i64Ty, sz)}, label + ".iface.heap");
                            builder_->CreateStore(argVal, heapObj);
                            auto* dst0 = builder_->CreateGEP(i8Ty, valPtr,
                                llvm::ConstantInt::get(i64Ty, 0), label + ".iface.dst0");
                            builder_->CreateStore(heapObj, dst0);
                            auto* dst1 = builder_->CreateGEP(i8Ty, valPtr,
                                llvm::ConstantInt::get(i64Ty, ptrSz), label + ".iface.dst1");
                            builder_->CreateStore(const_cast<llvm::GlobalVariable*>(vtMatch), dst1);
                            boxedForIface = true;
                        }
                    }
                }
                if (!boxedForIface) {
                if (auto* argStTy = llvm::dyn_cast<llvm::StructType>(argVal->getType())) {
                    auto* argAlloca = createEntryBlockAlloca(
                        builder_->GetInsertBlock()->getParent(), argStTy, label + ".inner.tmp");
                    builder_->CreateStore(argVal, argAlloca);
                    uint64_t payloadSz = destTy->getArrayNumElements();
                    uint64_t compactOffset = 0;
                    for (unsigned fi = 0; fi < argStTy->getNumElements(); ++fi) {
                        auto* fieldTy = argStTy->getElementType(fi);
                        uint64_t fieldSz = module_->getDataLayout().getTypeStoreSize(fieldTy);
                        if (compactOffset + fieldSz > payloadSz) {
                            diag_.error(call.location,
                                "{}(...): payload struct is too large for destination slot", name);
                            hadHardTypeError_ = true;
                            return nullptr;
                        }
                        auto* srcPtr = builder_->CreateStructGEP(argStTy, argAlloca, fi, "src.fld");
                        auto* fieldVal = builder_->CreateLoad(fieldTy, srcPtr, "src.fld.val");
                        auto* dstPtr = builder_->CreateGEP(
                            i8Ty, valPtr, llvm::ConstantInt::get(i64Ty, compactOffset), "dst.fld.ptr");
                        builder_->CreateStore(fieldVal, dstPtr);
                        compactOffset += fieldSz;
                    }
                } else {
                    auto* dataPtr = builder_->CreateGEP(
                        i8Ty, valPtr, llvm::ConstantInt::get(i64Ty, 0));
                    builder_->CreateStore(argVal, dataPtr);
                }
                }  // closes `if (!boxedForIface)` above
            } else {
                builder_->CreateStore(castToType(argVal, destTy), valPtr);
            }
        } else {
            builder_->CreateStore(llvm::Constant::getNullValue(stTy->getElementType(1)), valPtr);
        }
        return builder_->CreateLoad(stTy, alloca, label + ".result");
    }

    // Built-in None → ADT { tag=1, data=0 }
    if (name == "None") {
        auto noneIt = namedValues_.find("None");
        if (noneIt != namedValues_.end()) {
            llvm::Type* vpt = getValuePtrType(noneIt->second);
            if (!vpt) {
                diag_.error(call.location, "`None`: invalid binding for identifier");
                return nullptr;
            }
            return builder_->CreateLoad(vpt, noneIt->second, "None.var");
        }
        llvm::StructType* stTy = resolveOptionResultStructForExpr(expr, true);
        if (stTy) {
            auto* fn = builder_->GetInsertBlock()->getParent();
            auto* alloca = createEntryBlockAlloca(fn, stTy, "none.alloca");
            auto* tagTy = stTy->getElementType(0);
            int noneTag = 1;
            if (stTy->hasName()) {
                auto evIt = errorEnumValues_.find(stTy->getName().str() + ".None");
                if (evIt != errorEnumValues_.end()) noneTag = evIt->second;
            }
            builder_->CreateStore(llvm::ConstantInt::get(tagTy, noneTag),
                builder_->CreateStructGEP(stTy, alloca, 0, "none.tag"));
            builder_->CreateStore(llvm::Constant::getNullValue(stTy->getElementType(1)),
                builder_->CreateStructGEP(stTy, alloca, 1, "none.data"));
            return builder_->CreateLoad(stTy, alloca, "none.result");
        }
        stTy = getOrCreateResultType();
        llvm::Value* result = llvm::UndefValue::get(stTy);
        result = builder_->CreateInsertValue(result,
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 1), 0, "none.tag");
        result = builder_->CreateInsertValue(result,
            llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), 0), 1, "none.val");
        return result;
    }

    // ADT variant construction: EnumName::Variant(args)
    auto sep = name.find("::");
    if (sep != std::string::npos) {
        std::string enumName = name.substr(0, sep);
        std::string sourceEnumName = enumName;
        std::string variantName = name.substr(sep + 2);
        // Turbofish-tagged ADT variant (generic enum, e.g. from the Sema
        // desugar of `Either::<i32,string>.Left(42)`). Build the mangled
        // concrete name and use that for the struct lookup so the newly
        // on-demand-registered `Either<i32,string>` entry is found.
        auto* idCalleeEx = call.callee && call.callee->kind == ExprKind::Identifier
                            ? call.callee->as<const IdentifierExpr>() : nullptr;
        if (idCalleeEx && !idCalleeEx->callTypeArgs.empty()) {
            std::string mangled = enumName + "<";
            for (size_t ti = 0; ti < idCalleeEx->callTypeArgs.size(); ++ti) {
                if (ti > 0) mangled += ",";
                if (idCalleeEx->callTypeArgs[ti])
                    mangled += mangleTypeAnnotationNested(*idCalleeEx->callTypeArgs[ti]);
                else
                    mangled += "?";
            }
            mangled += ">";
            auto stIt2 = structTypes_.find(mangled);
            if (stIt2 != structTypes_.end()) {
                enumName = mangled;
            }
        }
        const bool firstPayloadIsFunction =
            inferredVariantPayloadIsFunction(variantName) ||
            typeArgsVariantPayloadIsFunction(sourceEnumName, variantName, idCalleeEx);
        auto stIt = structTypes_.find(enumName);
        auto evIt = errorEnumValues_.find(enumName + "." + variantName);
        if (stIt != structTypes_.end() && evIt != errorEnumValues_.end()) {
            auto* adtTy = stIt->second;
            auto* fn = builder_->GetInsertBlock()->getParent();
            auto* alloca = createEntryBlockAlloca(fn, adtTy, "adt.tmp");

            auto* tagPtr = builder_->CreateStructGEP(adtTy, alloca, 0, "adt.tag");
            builder_->CreateStore(
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), evIt->second), tagPtr);

            if (!call.args.empty()) {
                auto* dataPtr = builder_->CreateStructGEP(adtTy, alloca, 1, "adt.data");
                auto& dl = module_->getDataLayout();
                uint64_t offset = 0;
                for (size_t i = 0; i < call.args.size(); ++i) {
                    if (!call.args[i]) {
                        diag_.error(call.location, "ADT constructor `{}`: missing argument at index {}", name,
                            i);
                        return nullptr;
                    }
                    auto* argVal = emitExpr(*call.args[i]);
                    if (!argVal) {
                        diag_.error(call.location, "ADT constructor `{}`: argument {} did not produce a value",
                            name, i);
                        return nullptr;
                    }
                    argVal = wrapFnPayloadIfNeeded(argVal, call.args[i].get(),
                        firstPayloadIsFunction && i == 0);
                    uint64_t fieldAlign = dl.getABITypeAlign(argVal->getType()).value();
                    offset = (offset + fieldAlign - 1) & ~(fieldAlign - 1);
                    auto* destPtr = builder_->CreateGEP(
                        llvm::Type::getInt8Ty(*context_), dataPtr,
                        llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), offset), "adt.field");
                    auto* si = builder_->CreateStore(argVal, destPtr);
                    si->setAlignment(llvm::Align(fieldAlign));
                    offset += dl.getTypeAllocSize(argVal->getType());
                }
            }

            return builder_->CreateLoad(adtTy, alloca, "adt.val");
        }
        diag_.error(call.location, "ADT constructor `{}`: type `{}` or variant `{}` is unknown to codegen",
            name, enumName, variantName);
        return nullptr;
    }

    return nullptr;
}

// alloc/dealloc/transmute/sizeof/alignof/type_name/from_cstr/to_rawptr/typeinfo
llvm::Value* CodeGen::emitBuiltinMemory(const Expr& expr) {
    auto& call = static_cast<const CallExpr&>(expr);
    if (!call.callee || call.callee->kind != ExprKind::Identifier) {
        diag_.error(call.location, "internal: memory intrinsic requires identifier callee");
        return nullptr;
    }
    auto& name = call.callee->as<IdentifierExpr>()->name;
    auto& typeArgs = call.callee->as<IdentifierExpr>()->callTypeArgs;

    if (name == "sizeof") {
        if (!typeArgs.empty() && typeArgs[0]) {
            auto* ty = toLLVMType(*typeArgs[0]);
            uint64_t sz = module_->getDataLayout().getTypeAllocSize(ty).getFixedValue();
            return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), sz);
        }
        diag_.error(call.location, "sizeof requires a type argument (e.g. sizeof::<T>())");
        return nullptr;
    }

    if (name == "alignof") {
        if (!typeArgs.empty() && typeArgs[0]) {
            auto* ty = toLLVMType(*typeArgs[0]);
            uint64_t align = module_->getDataLayout().getABITypeAlign(ty).value();
            return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), align);
        }
        diag_.error(call.location, "alignof requires a type argument (e.g. alignof::<T>())");
        return nullptr;
    }

    if (name == "type_name") {
        if (!typeArgs.empty() && typeArgs[0]) {
            std::string tname = typeArgs[0]->name;
            auto gpIt = genericTypeParams_.find(tname);
            if (gpIt != genericTypeParams_.end()) {
                tname = llvmTypeToString(gpIt->second);
            }
            return createStringValue(tname);
        }
        diag_.error(call.location, "type_name requires a type argument (e.g. type_name::<T>())");
        return nullptr;
    }

    // typeof(expr) — runtime-free compile-time type introspection.
    // Surpasses C++ typeid().name() (which is runtime + implementation-defined).
    // Returns a Vyx string literal with the canonical VyxType name of the
    // argument expression: `typeof(42) == "i64"`, `typeof("hi") == "string"`.
    if (name == "typeof") {
        if (call.args.empty() || !call.args[0]) {
            diag_.error(call.location, "typeof requires an expression argument");
            return nullptr;
        }
        std::string tname;
        // Prefer Sema-inferred type (most precise); fall back to the LLVM
        // value's LLVM type name for late-resolved exprs.
        if (call.args[0]->inferredType) {
            tname = call.args[0]->inferredType->toString();
        } else {
            auto* val = emitExpr(*call.args[0]);
            tname = val ? llvmTypeToString(val->getType()) : "<unknown>";
        }
        return createStringValue(tname);
    }

    if (name == "alloc") {
        if (typeArgs.empty() || !typeArgs[0]) {
            diag_.error(call.location, "alloc requires an element type argument (e.g. alloc::<T>(n))");
            return nullptr;
        }
        auto* i64Ty = llvm::Type::getInt64Ty(*context_);
        auto* ptrTy = llvm::PointerType::getUnqual(*context_);
        auto* mallocFn = module_->getFunction("malloc");
        if (!mallocFn) {
            auto* mty = llvm::FunctionType::get(ptrTy, {i64Ty}, false);
            mallocFn = llvm::Function::Create(mty, llvm::Function::ExternalLinkage, "malloc", *module_);
        }
        auto* ty = toLLVMType(*typeArgs[0]);
        uint64_t elemSize = module_->getDataLayout().getTypeAllocSize(ty).getFixedValue();
        llvm::Value* totalSize = llvm::ConstantInt::get(i64Ty, elemSize);
        if (!call.args.empty() && call.args[0]) {
            auto* count = emitExpr(*call.args[0]);
            if (!count) {
                diag_.error(call.location, "alloc: count expression did not produce a value");
                return nullptr;
            }
            if (!count->getType()->isIntegerTy(64))
                count = builder_->CreateSExt(count, i64Ty);
            totalSize = builder_->CreateMul(count, llvm::ConstantInt::get(i64Ty, elemSize));
        }
        return builder_->CreateCall(mallocFn, {totalSize}, "alloc.ptr");
    }

    if (name == "dealloc") {
        if (!call.args.empty() && call.args[0]) {
            auto* ptr = emitExpr(*call.args[0]);
            if (ptr) {
                auto* ptrTy = llvm::PointerType::getUnqual(*context_);
                auto* freeFn = module_->getFunction("free");
                if (!freeFn) {
                    auto* fty = llvm::FunctionType::get(llvm::Type::getVoidTy(*context_), {ptrTy}, false);
                    freeFn = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, "free", *module_);
                }
                if (!ptr->getType()->isPointerTy())
                    ptr = builder_->CreateIntToPtr(ptr, ptrTy);
                return builder_->CreateCall(freeFn, {ptr});
            }
        }
        diag_.error(call.location, "dealloc() requires a pointer argument");
        return nullptr;
    }

    if (name == "transmute") {
        if (!call.args.empty() && call.args[0] && typeArgs.size() >= 2 && typeArgs[1]) {
            auto* val = emitExpr(*call.args[0]);
            if (!val) {
                diag_.error(call.location, "transmute: value argument did not produce a value");
                return nullptr;
            }
            auto* targetTy = toLLVMType(*typeArgs[1]);
            auto* srcTy = val->getType();
            if (srcTy == targetTy) return val;
            if (srcTy->isPointerTy() && targetTy->isIntegerTy())
                return builder_->CreatePtrToInt(val, targetTy, "transmute");
            if (srcTy->isIntegerTy() && targetTy->isPointerTy())
                return builder_->CreateIntToPtr(val, targetTy, "transmute");
            if (srcTy->isFloatingPointTy() && targetTy->isIntegerTy())
                return builder_->CreateBitCast(val, targetTy, "transmute");
            if (srcTy->isIntegerTy() && targetTy->isFloatingPointTy())
                return builder_->CreateBitCast(val, targetTy, "transmute");
            return builder_->CreateBitCast(val, targetTy, "transmute");
        }
        diag_.error(call.location, "transmute requires value argument and type parameters: transmute::<From, To>(val)");
        return nullptr;
    }

    if (name == "from_cstr") {
        if (!call.args.empty() && call.args[0]) {
            auto* cPtr = emitExpr(*call.args[0]);
            if (!cPtr) {
                diag_.error(call.location, "from_cstr: argument did not produce a value");
                return nullptr;
            }
            auto* ptrTy = llvm::PointerType::getUnqual(*context_);
            auto* i64Ty = llvm::Type::getInt64Ty(*context_);
            if (cPtr->getType()->isStructTy()) {
                auto* strTy2 = getOrCreateStringType();
                if (cPtr->getType() == strTy2) {
                    auto* fn2 = builder_->GetInsertBlock()->getParent();
                    auto* tmp = createEntryBlockAlloca(fn2, strTy2, "cstr.extract");
                    builder_->CreateStore(cPtr, tmp);
                    cPtr = builder_->CreateLoad(ptrTy, builder_->CreateStructGEP(strTy2, tmp, 0));
                }
            } else if (cPtr->getType()->isIntegerTy()) {
                cPtr = builder_->CreateIntToPtr(cPtr, ptrTy);
            }
            auto* strTy = getOrCreateStringType();
            auto* fn = builder_->GetInsertBlock()->getParent();
            auto* alloca = createEntryBlockAlloca(fn, strTy, "cstr.vyx");
            auto* memsetFn3 = module_->getFunction("memset");
            if (memsetFn3) {
                uint64_t sz = module_->getDataLayout().getTypeAllocSize(strTy);
                builder_->CreateCall(memsetFn3, {
                    alloca,
                    llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0),
                    llvm::ConstantInt::get(i64Ty, sz)
                });
            }
            builder_->CreateStore(cPtr, builder_->CreateStructGEP(strTy, alloca, 0));
            auto* strlenFn = module_->getFunction("strlen");
            if (!strlenFn) {
                auto* slty = llvm::FunctionType::get(i64Ty, {ptrTy}, false);
                strlenFn = llvm::Function::Create(slty, llvm::Function::ExternalLinkage, "strlen", *module_);
            }
            auto* len = builder_->CreateCall(strlenFn, {cPtr}, "cstr.len");
            builder_->CreateStore(len, builder_->CreateStructGEP(strTy, alloca, 1));
            builder_->CreateStore(len, builder_->CreateStructGEP(strTy, alloca, 2));
            builder_->CreateStore(llvm::ConstantInt::get(i64Ty, 0), builder_->CreateStructGEP(strTy, alloca, 3));
            return builder_->CreateLoad(strTy, alloca, "cstr.str");
        }
        diag_.error(call.location, "from_cstr requires a pointer argument");
        return nullptr;
    }

    if (name == "from_cstr_len" || name == "from_cstr_view_len") {
        if (call.args.size() < 2 || !call.args[0] || !call.args[1]) {
            diag_.error(call.location, "{} requires pointer and length arguments", name);
            return nullptr;
        }
        auto* cPtr = emitExpr(*call.args[0]);
        auto* lenRaw = emitExpr(*call.args[1]);
        if (!cPtr || !lenRaw) {
            diag_.error(call.location, "{}: argument did not produce a value", name);
            return nullptr;
        }
        auto* ptrTy = llvm::PointerType::getUnqual(*context_);
        auto* i64Ty = llvm::Type::getInt64Ty(*context_);
        if (cPtr->getType()->isStructTy()) {
            auto* strTy2 = getOrCreateStringType();
            if (cPtr->getType() == strTy2) {
                auto* fn2 = builder_->GetInsertBlock()->getParent();
                auto* tmp = createEntryBlockAlloca(fn2, strTy2, "cstrlen.extract");
                builder_->CreateStore(cPtr, tmp);
                cPtr = builder_->CreateLoad(ptrTy, builder_->CreateStructGEP(strTy2, tmp, 0));
            }
        } else if (cPtr->getType()->isIntegerTy()) {
            cPtr = builder_->CreateIntToPtr(cPtr, ptrTy);
        }
        auto* n = lenRaw->getType()->isIntegerTy(64) ? lenRaw : builder_->CreateSExt(lenRaw, i64Ty);
        auto* fn = builder_->GetInsertBlock()->getParent();
        auto* strTy = getOrCreateStringType();
        auto* alloca = createEntryBlockAlloca(fn, strTy, "cstrlen.vyx");
        auto* memsetFn = module_->getFunction("memset");
        if (memsetFn) {
            uint64_t sz = module_->getDataLayout().getTypeAllocSize(strTy);
            builder_->CreateCall(memsetFn, {
                alloca,
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0),
                llvm::ConstantInt::get(i64Ty, sz)
            });
        }
        auto* plusOne = builder_->CreateAdd(n, llvm::ConstantInt::get(i64Ty, 1));
        llvm::Value* buf = cPtr;
        llvm::Value* cap = n;
        if (name == "from_cstr_len") {
            auto* mallocFn = module_->getFunction("malloc");
            if (!mallocFn) {
                auto* mty = llvm::FunctionType::get(ptrTy, {i64Ty}, false);
                mallocFn = llvm::Function::Create(mty, llvm::Function::ExternalLinkage, "malloc", *module_);
            }
            buf = builder_->CreateCall(mallocFn, {plusOne}, "cstrlen.buf");
            auto* i8Ty = llvm::Type::getInt8Ty(*context_);
            auto* memcpyFn = module_->getFunction("memcpy");
            if (!memcpyFn) {
                auto* mty = llvm::FunctionType::get(ptrTy, {ptrTy, ptrTy, i64Ty}, false);
                memcpyFn = llvm::Function::Create(mty, llvm::Function::ExternalLinkage, "memcpy", *module_);
            }
            builder_->CreateCall(memcpyFn, {buf, cPtr, n});
            auto* nullAt = builder_->CreateGEP(i8Ty, buf, n);
            builder_->CreateStore(llvm::ConstantInt::get(i8Ty, 0), nullAt);
            cap = plusOne;
        }
        builder_->CreateStore(buf, builder_->CreateStructGEP(strTy, alloca, 0));
        builder_->CreateStore(n, builder_->CreateStructGEP(strTy, alloca, 1));
        builder_->CreateStore(cap, builder_->CreateStructGEP(strTy, alloca, 2));
        builder_->CreateStore(llvm::ConstantInt::get(i64Ty, 0), builder_->CreateStructGEP(strTy, alloca, 3));
        return builder_->CreateLoad(strTy, alloca, "cstrlen.str");
    }

    if (name == "from_raw_string_parts") {
        if (call.args.size() < 4 || !call.args[0] || !call.args[1] || !call.args[2] || !call.args[3]) {
            diag_.error(call.location, "from_raw_string_parts requires ptr, len, cap, and owned arguments");
            return nullptr;
        }
        auto* ptr = emitExpr(*call.args[0]);
        auto* len = emitExpr(*call.args[1]);
        auto* cap = emitExpr(*call.args[2]);
        auto* owned = emitExpr(*call.args[3]);
        if (!ptr || !len || !cap || !owned) {
            diag_.error(call.location, "from_raw_string_parts: argument did not produce a value");
            return nullptr;
        }
        auto* ptrTy = llvm::PointerType::getUnqual(*context_);
        auto* i64Ty = llvm::Type::getInt64Ty(*context_);
        if (ptr->getType()->isIntegerTy())
            ptr = builder_->CreateIntToPtr(ptr, ptrTy);
        else if (ptr->getType() != ptrTy)
            ptr = castToType(ptr, ptrTy);
        if (!len->getType()->isIntegerTy(64)) len = builder_->CreateSExtOrTrunc(len, i64Ty);
        if (!cap->getType()->isIntegerTy(64)) cap = builder_->CreateSExtOrTrunc(cap, i64Ty);
        if (!owned->getType()->isIntegerTy(64)) owned = builder_->CreateSExtOrTrunc(owned, i64Ty);

        auto* fn = builder_->GetInsertBlock()->getParent();
        auto* strTy = getOrCreateStringType();
        auto* alloca = createEntryBlockAlloca(fn, strTy, "rawstr.parts");
        builder_->CreateStore(ptr, builder_->CreateStructGEP(strTy, alloca, 0));
        builder_->CreateStore(len, builder_->CreateStructGEP(strTy, alloca, 1));
        builder_->CreateStore(cap, builder_->CreateStructGEP(strTy, alloca, 2));
        builder_->CreateStore(owned, builder_->CreateStructGEP(strTy, alloca, 3));
        return builder_->CreateLoad(strTy, alloca, "rawstr.val");
    }

    if (name == "to_rawptr") {
        if (!call.args.empty() && call.args[0]) {
            auto* val = emitExpr(*call.args[0]);
            if (!val) {
                diag_.error(call.location, "to_rawptr: argument did not produce a value");
                return nullptr;
            }
            if (val->getType()->isPointerTy()) return val;
            if (val->getType() == getOrCreateStringType()) {
                auto* fn = builder_->GetInsertBlock()->getParent();
                auto* strTy = getOrCreateStringType();
                auto* tmp = createEntryBlockAlloca(fn, strTy, "str.tmp");
                builder_->CreateStore(val, tmp);
                return builder_->CreateLoad(llvm::PointerType::getUnqual(*context_),
                    builder_->CreateStructGEP(strTy, tmp, 0, "str.ptr.gep"), "str.ptr");
            }
            if (val->getType()->isIntegerTy())
                return builder_->CreateIntToPtr(val, llvm::PointerType::getUnqual(*context_));
        }
        diag_.error(call.location, "to_rawptr requires a pointer, string, or integer argument");
        return nullptr;
    }

    if (name == "typeinfo") {
        if (!call.args.empty() && call.args[0] && call.args[0]->kind == ExprKind::Identifier && call.args[0]->as<IdentifierExpr>()->typeAnnotation) {
            auto& ann = *call.args[0]->as<IdentifierExpr>()->typeAnnotation;
            std::string typeName = ann.name;
            if (typeName.empty()) {
                diag_.error(call.location, "typeinfo: type annotation has no name");
                return nullptr;
            }
            int typeSize = 0;
            int fieldCount = 0;
            auto sit = structTypes_.find(typeName);
            if (sit != structTypes_.end()) {
                typeSize = static_cast<int>(module_->getDataLayout().getTypeAllocSize(sit->second).getFixedValue());
                fieldCount = sit->second->getNumElements();
            } else {
                TypeAnnotation tann;
                tann.kind = TypeAnnotationKind::Named;
                tann.name = typeName;
                auto* llvmTy = toLLVMType(tann);
                if (llvmTy && !llvmTy->isVoidTy())
                    typeSize = static_cast<int>(module_->getDataLayout().getTypeAllocSize(llvmTy).getFixedValue());
            }
            std::string info = typeName + " (size=" + std::to_string(typeSize) +
                ", fields=" + std::to_string(fieldCount) + ")";
            return createStringValue(info);
        }
        diag_.error(call.location, "typeinfo requires an identifier argument with an explicit type annotation");
        return nullptr;
    }

    diag_.error(call.location, "internal: unhandled memory intrinsic `{}`", name);
    return nullptr;
}

// print/format/panic/assert/assert_eq
llvm::Value* CodeGen::emitBuiltinIO(const Expr& expr) {
    auto& call = static_cast<const CallExpr&>(expr);
    if (!call.callee || call.callee->kind != ExprKind::Identifier) {
        diag_.error(call.location, "internal: I/O builtin requires identifier callee");
        return nullptr;
    }
    auto& name = call.callee->as<IdentifierExpr>()->name;

    if (name == "assert") {
        if (call.args.empty() || !call.args[0]) {
            diag_.error(call.location, "assert requires a boolean condition argument");
            return nullptr;
        }
        auto* condVal = emitExpr(*call.args[0]);
        if (!condVal) {
            diag_.error(call.location, "assert: condition expression produced no value");
            return nullptr;
        }
        auto* fn = builder_->GetInsertBlock()->getParent();
        auto* isTrue = coerceToBool(condVal, "assert.cond");
        auto* passBB = llvm::BasicBlock::Create(*context_, "assert.pass", fn);
        auto* failBB = llvm::BasicBlock::Create(*context_, "assert.fail", fn);
        builder_->CreateCondBr(isTrue, passBB, failBB);
        builder_->SetInsertPoint(failBB);
        auto* printfFn = getOrCreatePrintf();
        auto* msg = getOrCreateString("ASSERTION FAILED\n");
        builder_->CreateCall(printfFn, {msg});
        auto* exitFn = module_->getFunction("exit");
        if (!exitFn) {
            auto* exitTy = llvm::FunctionType::get(llvm::Type::getVoidTy(*context_), {llvm::Type::getInt32Ty(*context_)}, false);
            exitFn = llvm::Function::Create(exitTy, llvm::Function::ExternalLinkage, "exit", *module_);
        }
        builder_->CreateCall(exitFn, {llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 1)});
        builder_->CreateUnreachable();
        builder_->SetInsertPoint(passBB);
        return llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0);
    }

    if (name == "panic") {
        if (!call.args.empty()) {
            auto* msgVal = emitExpr(*call.args[0]);
            if (msgVal) {
                auto* msgPtr = extractStringPtr(msgVal);
                emitPanicCall(msgPtr);
            } else {
                emitPanicCall(getOrCreateString("PANIC: unrecoverable error"));
            }
        } else {
            emitPanicCall(getOrCreateString("PANIC: unrecoverable error"));
        }
        return llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0);
    }

    if (name == "format") {
        if (!call.args.empty()) {
            auto* fmtVal = emitExpr(*call.args[0]);
            if (fmtVal) {
                auto* i64Ty = llvm::Type::getInt64Ty(*context_);
                auto* ptrTy = llvm::PointerType::getUnqual(*context_);
                auto* fn = builder_->GetInsertBlock()->getParent();
                auto* strTy = getOrCreateStringType();
                auto* fmtPtr = extractStringPtr(fmtVal);
                auto* mallocFn = module_->getFunction("malloc");
                if (!mallocFn) {
                    auto* mty = llvm::FunctionType::get(ptrTy, {i64Ty}, false);
                    mallocFn = llvm::Function::Create(mty, llvm::Function::ExternalLinkage, "malloc", *module_);
                }
                auto* snprintfFn = module_->getFunction("snprintf");
                if (!snprintfFn) {
                    auto* sty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context_), {ptrTy, i64Ty, ptrTy}, true);
                    snprintfFn = llvm::Function::Create(sty, llvm::Function::ExternalLinkage, "snprintf", *module_);
                }
                std::vector<llvm::Value*> extraArgs;
                for (size_t i = 1; i < call.args.size(); ++i) {
                    if (!call.args[i]) continue;
                    auto* v = emitExpr(*call.args[i]);
                    if (!v) {
                        diag_.error(call.location, "format: argument {} did not produce a value", i);
                        return nullptr;
                    }
                    if (v->getType() == strTy) v = extractStringPtr(v);
                    extraArgs.push_back(v);
                }
                auto* nullPtr = llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy));
                std::vector<llvm::Value*> measureArgs = {nullPtr, llvm::ConstantInt::get(i64Ty, 0), fmtPtr};
                measureArgs.insert(measureArgs.end(), extraArgs.begin(), extraArgs.end());
                auto* needed = builder_->CreateCall(snprintfFn, measureArgs, "fmt.needed");
                auto* needed64 = builder_->CreateSExt(needed, i64Ty);
                auto* allocSize = builder_->CreateAdd(needed64, llvm::ConstantInt::get(i64Ty, 1));
                auto* buf = builder_->CreateCall(mallocFn, {allocSize}, "fmt.buf");
                std::vector<llvm::Value*> fmtArgs = {buf, allocSize, fmtPtr};
                fmtArgs.insert(fmtArgs.end(), extraArgs.begin(), extraArgs.end());
                builder_->CreateCall(snprintfFn, fmtArgs, "fmt.len");
                auto* resultAlloca = createEntryBlockAlloca(fn, strTy, "fmt.str");
                builder_->CreateStore(buf, builder_->CreateStructGEP(strTy, resultAlloca, 0));
                builder_->CreateStore(needed64, builder_->CreateStructGEP(strTy, resultAlloca, 1));
                builder_->CreateStore(allocSize, builder_->CreateStructGEP(strTy, resultAlloca, 2));
                builder_->CreateStore(llvm::ConstantInt::get(i64Ty, 1), builder_->CreateStructGEP(strTy, resultAlloca, 3));
                return builder_->CreateLoad(strTy, resultAlloca, "fmt.result");
            }
            diag_.error(call.location, "format: format string argument did not produce a value");
            return nullptr;
        }
        diag_.error(call.location, "format requires at least a format string argument");
        return nullptr;
    }

    diag_.error(call.location, "internal: unhandled I/O builtin `{}`", name);
    return nullptr;
}

} // namespace vyx
