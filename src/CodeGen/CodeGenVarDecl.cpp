#include "CodeGenIncludes.h"
#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace vyx {

void CodeGen::emitVarDecl(const Stmt& stmt) {
    auto& vd = static_cast<const VarDeclStmt&>(stmt);
    auto* fn = builder_->GetInsertBlock()->getParent();
    auto methodReturnsContainerElement = [](const std::string& member) {
        return member == "get" ||
               member == "get_unchecked" ||
               member == "pop" ||
               member == "peek" ||
               member == "first" ||
               member == "last" ||
               member == "remove" ||
               member == "removeAt";
    };
    auto functionReturnLLVMType = [&](const VyxTypePtr& fnTy) -> llvm::Type* {
        llvm::Type* retTy = llvm::Type::getInt64Ty(*context_);
        if (fnTy && fnTy->kind == VyxTypeKind::Function && fnTy->returnType) {
            auto* inferred = toLLVMType(*fnTy->returnType);
            if (inferred && !inferred->isVoidTy()) retTy = inferred;
        }
        return retTy;
    };
    auto unwrapCallFunctionReturnType = [&](const Expr* initExpr) -> llvm::Type* {
        if (!initExpr || initExpr->kind != ExprKind::Call) return nullptr;
        auto* call = initExpr->as<const CallExpr>();
        if (!call->callee || call->callee->kind != ExprKind::MemberAccess) return nullptr;
        auto* ma = call->callee->as<const MemberAccessExpr>();
        if (!ma->object) return nullptr;
        const bool unwrapsValue =
            ma->member == "unwrap" ||
            ma->member == "unwrap_or" ||
            ma->member == "unwrapOr";
        const bool unwrapsErr = ma->member == "unwrap_err";
        if (!unwrapsValue && !unwrapsErr) return nullptr;
        if (!ma->object->inferredType) return nullptr;

        VyxTypePtr payload;
        if (unwrapsErr) {
            payload = resultErr(*ma->object->inferredType);
        } else if (auto inner = optionInner(*ma->object->inferredType)) {
            payload = inner;
        } else {
            payload = resultOk(*ma->object->inferredType);
        }
        if (!payload || payload->kind != VyxTypeKind::Function) return nullptr;
        return functionReturnLLVMType(payload);
    };
    auto maybeRetainRefCopy = [&](llvm::AllocaInst* dstAlloca) {
        if (!dstAlloca) return;
        bool copiesExistingRef =
            vd.initExpr &&
            (vd.initExpr->kind == ExprKind::Identifier ||
             vd.initExpr->kind == ExprKind::MemberAccess);
        if (!copiesExistingRef) return;
        auto rcIt = structTypes_.find("__RefCounted");
        if (rcIt == structTypes_.end() ||
            dstAlloca->getAllocatedType() != rcIt->second) {
            return;
        }

        auto* refTy = rcIt->second;
        auto* i64Ty = llvm::Type::getInt64Ty(*context_);
        auto* ptrTy = llvm::PointerType::getUnqual(*context_);
        auto* rcFieldPtr = builder_->CreateStructGEP(refTy, dstAlloca, 1);
        auto* rcHeader = builder_->CreateLoad(ptrTy, rcFieldPtr, "ref.copy.rc.ptr");
        auto* fn2 = builder_->GetInsertBlock()->getParent();
        auto* retainBB = llvm::BasicBlock::Create(*context_, "ref.copy.retain", fn2);
        auto* contBB = llvm::BasicBlock::Create(*context_, "ref.copy.cont", fn2);
        auto* notNull = builder_->CreateICmpNE(
            rcHeader,
            llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy)),
            "ref.copy.rc.notnull");
        builder_->CreateCondBr(notNull, retainBB, contBB);
        builder_->SetInsertPoint(retainBB);
        auto* strong = builder_->CreateLoad(i64Ty, rcHeader, "ref.copy.strong");
        auto* newStrong = builder_->CreateAdd(
            strong, llvm::ConstantInt::get(i64Ty, 1), "ref.copy.strong.inc");
        builder_->CreateStore(newStrong, rcHeader);
        builder_->CreateBr(contBB);
        builder_->SetInsertPoint(contBB);
    };

    // Tuple destructuring: `let (a, b) = expr;`
    if (!vd.tupleBindings.empty()) {
        if (!vd.initExpr) {
            diag_.error(stmt.location, "tuple destructuring requires an initializer");
            return;
        }
        auto* initVal = emitExpr(*vd.initExpr);
        if (!initVal) {
            diag_.error(stmt.location, "tuple destructuring: initializer did not produce a value");
            return;
        }
        auto* tupTy = llvm::dyn_cast<llvm::StructType>(initVal->getType());
        if (!tupTy) {
            // Store to a temp alloca and treat its type as the tuple type
            auto* tmpAlloca = createEntryBlockAlloca(fn, initVal->getType(), "__tuple_dest_tmp");
            builder_->CreateStore(initVal, tmpAlloca);
            for (size_t i = 0; i < vd.tupleBindings.size(); ++i) {
                auto* elemAlloca = createEntryBlockAlloca(fn, initVal->getType(), vd.tupleBindings[i]);
                builder_->CreateStore(initVal, elemAlloca);
                namedValues_[vd.tupleBindings[i]] = elemAlloca;
            }
            return;
        }
        // Store the tuple value to a temp alloca, then GEP each element
        auto* tmpAlloca = createEntryBlockAlloca(fn, tupTy, "__tuple_dest_tmp");
        builder_->CreateStore(initVal, tmpAlloca);
        size_t n = std::min(vd.tupleBindings.size(), (size_t)tupTy->getNumElements());
        for (size_t i = 0; i < n; ++i) {
            auto* elemTy = tupTy->getElementType(i);
            auto* gep = builder_->CreateStructGEP(tupTy, tmpAlloca, i, vd.tupleBindings[i] + ".ptr");
            auto* elemVal = builder_->CreateLoad(elemTy, gep, vd.tupleBindings[i] + ".val");
            auto* elemAlloca = createEntryBlockAlloca(fn, elemTy, vd.tupleBindings[i]);
            builder_->CreateStore(elemVal, elemAlloca);
            namedValues_[vd.tupleBindings[i]] = elemAlloca;
        }
        return;
    }

    llvm::Type* varType = llvm::Type::getInt32Ty(*context_);
    if (vd.varType) {
        varType = toLLVMType(*vd.varType);

        if (vd.varType->name == "u8" || vd.varType->name == "u16" ||
            vd.varType->name == "u32" || vd.varType->name == "u64") {
            unsignedVars_.insert(vd.varName);
        }

        if (vd.varType->kind == TypeAnnotationKind::Pointer && vd.varType->as<PointerType>()->innerType) {
            ptrElemTypes_[vd.varName] = toLLVMType(*vd.varType->as<PointerType>()->innerType);
        }

        if (vd.varType->name == "Dict" && containerTypes_.find(vd.varName) == containerTypes_.end()) {
            containerTypes_[vd.varName] = "Dict";
            if (vd.varType->as<GenericType>()->typeArgs.size() >= 1)
                containerElemTypes_[vd.varName] = toLLVMType(*vd.varType->as<GenericType>()->typeArgs[0]);
            if (vd.varType->as<GenericType>()->typeArgs.size() >= 2)
                containerValTypes_[vd.varName] = toLLVMType(*vd.varType->as<GenericType>()->typeArgs[1]);
        } else if (vd.varType->name == "Vec" && containerTypes_.find(vd.varName) == containerTypes_.end()) {
            containerTypes_[vd.varName] = "Vec";
            if (!vd.varType->as<GenericType>()->typeArgs.empty())
                containerElemTypes_[vd.varName] = toLLVMType(*vd.varType->as<GenericType>()->typeArgs[0]);
        }

        // Interface boxing: let x: IFace = ConcreteClass{...}
        if (vd.initExpr && structTypes_.count(vd.varType->name + "_vtable")) {
            std::string ifaceName = vd.varType->name;
            std::string concreteName;
            if (vd.initExpr->kind == ExprKind::StructInit) {
                concreteName = vd.initExpr->as<StructInitExpr>()->structName;
            } else if (vd.initExpr->kind == ExprKind::Identifier) {
                concreteName = resolveClassName(*vd.initExpr,
                    vd.initExpr->as<IdentifierExpr>()->name);
            }

            auto* initVal = emitExpr(*vd.initExpr);
            if (!initVal) { namedValues_[vd.varName] = nullptr; return; }

            // When the init expression is a call (e.g. `Circle.new(...)`)
            // neither of the AST-kind branches above recovers a name, so
            // we'd skip the vtable store and leave field 1 (the vtable
            // pointer) uninitialised — later dispatch reads garbage and
            // segfaults. Fall back to the concrete LLVM struct name.
            if (concreteName.empty()) {
                if (auto* initSt = llvm::dyn_cast<llvm::StructType>(initVal->getType())) {
                    if (initSt->hasName()) concreteName = initSt->getName().str();
                }
            }

            auto* ptrTy = llvm::PointerType::getUnqual(*context_);
            auto ifaceTyIt = structTypes_.find("__iface_" + ifaceName);
            llvm::StructType* ifaceTy = nullptr;
            if (ifaceTyIt != structTypes_.end()) {
                ifaceTy = ifaceTyIt->second;
            } else {
                ifaceTy = llvm::StructType::create(*context_, {ptrTy, ptrTy}, "__iface_" + ifaceName);
                structTypes_["__iface_" + ifaceName] = ifaceTy;
            }

            auto* ifaceAlloca = createEntryBlockAlloca(fn, ifaceTy, vd.varName);
            auto* objStorage = createEntryBlockAlloca(fn, initVal->getType(), vd.varName + ".concrete");
            builder_->CreateStore(initVal, objStorage);
            builder_->CreateStore(objStorage,
                builder_->CreateStructGEP(ifaceTy, ifaceAlloca, 0, "iface.obj"));

            if (!concreteName.empty()) {
                std::string vtGlobalName = concreteName + "_vtable_" + ifaceName;
                auto* vtGlobal = module_->getGlobalVariable(vtGlobalName, true);
                if (vtGlobal) {
                    builder_->CreateStore(vtGlobal,
                        builder_->CreateStructGEP(ifaceTy, ifaceAlloca, 1, "iface.vtbl"));
                }
            }

            namedValues_[vd.varName] = ifaceAlloca;
            interfaceVarTypes_[vd.varName] = ifaceName;
            return;
        }
    } else if (vd.initExpr) {
        if (vd.initExpr->kind == ExprKind::Call && vd.initExpr->as<CallExpr>()->callee &&
            vd.initExpr->as<CallExpr>()->callee->kind == ExprKind::Identifier) {
            auto& calleeName = vd.initExpr->as<CallExpr>()->callee->as<IdentifierExpr>()->name;
            if (calleeName == "alloc" && !vd.initExpr->as<CallExpr>()->callee->as<IdentifierExpr>()->callTypeArgs.empty()) {
                auto& typeArgs = vd.initExpr->as<CallExpr>()->callee->as<IdentifierExpr>()->callTypeArgs;
                if (typeArgs[0]) {
                    ptrElemTypes_[vd.varName] = toLLVMType(*typeArgs[0]);
                }
            }
            if (calleeName == "makeMutex")
                containerTypes_[vd.varName] = "Mutex";
            else if (calleeName == "makeChannel")
                containerTypes_[vd.varName] = "Channel";
            else if (containerTypes_.find(vd.varName) == containerTypes_.end()) {
                for (auto& decl : unit_->declarations) {
                    if (!decl || decl->kind != DeclKind::Function || decl->name != calleeName) continue;
                    if (decl->as<FunctionDecl>()->returnType) {
                        auto& rn = decl->as<FunctionDecl>()->returnType->name;
                        if (rn == "Vec") {
                            containerTypes_[vd.varName] = "Vec";
                            if (!decl->as<FunctionDecl>()->returnType->as<GenericType>()->typeArgs.empty())
                                containerElemTypes_[vd.varName] = toLLVMType(*decl->as<FunctionDecl>()->returnType->as<GenericType>()->typeArgs[0]);
                        } else if (rn == "Dict") {
                            containerTypes_[vd.varName] = "Dict";
                            if (decl->as<FunctionDecl>()->returnType->as<GenericType>()->typeArgs.size() >= 1)
                                containerElemTypes_[vd.varName] = toLLVMType(*decl->as<FunctionDecl>()->returnType->as<GenericType>()->typeArgs[0]);
                            if (decl->as<FunctionDecl>()->returnType->as<GenericType>()->typeArgs.size() >= 2)
                                containerValTypes_[vd.varName] = toLLVMType(*decl->as<FunctionDecl>()->returnType->as<GenericType>()->typeArgs[1]);
                        }
                    }
                    break;
                }
            }
        }
        // Auto-boxing for Any type: let a: Any = 42; → box as {tag, value}
        if (vd.varType && vd.varType->name == "Any" && vd.initExpr) {
            auto* initVal = emitExpr(*vd.initExpr);
            if (!initVal) { namedValues_[vd.varName] = nullptr; return; }
            auto* anyTy = getOrCreateAnyType();
            auto* fn = builder_->GetInsertBlock()->getParent();
            auto* anyAlloca = createEntryBlockAlloca(fn, anyTy, vd.varName);
            auto* tagPtr = builder_->CreateStructGEP(anyTy, anyAlloca, 0, "any.tag");
            auto* valPtr = builder_->CreateStructGEP(anyTy, anyAlloca, 1, "any.val");
            int8_t tag = 0;
            llvm::Value* boxed = llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), 0);
            if (initVal->getType()->isIntegerTy(32)) {
                tag = 1;
                boxed = builder_->CreateSExt(initVal, llvm::Type::getInt64Ty(*context_));
            } else if (initVal->getType()->isIntegerTy(64)) {
                tag = 2; boxed = initVal;
            } else if (initVal->getType()->isDoubleTy()) {
                tag = 3;
                boxed = builder_->CreateBitCast(initVal, llvm::Type::getInt64Ty(*context_));
            } else if (initVal->getType() == getOrCreateStringType()) {
                tag = 4;
                boxed = builder_->CreatePtrToInt(
                    extractStringPtr(initVal), llvm::Type::getInt64Ty(*context_));
            } else if (initVal->getType()->isIntegerTy(8)) {
                tag = 5;
                boxed = builder_->CreateZExt(initVal, llvm::Type::getInt64Ty(*context_));
            } else if (initVal->getType()->isPointerTy()) {
                tag = 6;
                boxed = builder_->CreatePtrToInt(initVal, llvm::Type::getInt64Ty(*context_));
            } else if (initVal->getType()->isStructTy()) {
                tag = 7;
                auto* structTy = initVal->getType();
                auto* mallocFn = module_->getFunction("malloc");
                if (mallocFn) {
                    uint64_t sz = module_->getDataLayout().getTypeAllocSize(structTy);
                    auto* mem = builder_->CreateCall(mallocFn,
                        {llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), sz)}, "any.struct");
                    builder_->CreateStore(initVal, mem);
                    boxed = builder_->CreatePtrToInt(mem, llvm::Type::getInt64Ty(*context_));
                }
            } else if (initVal->getType()->isFloatTy()) {
                tag = 3;
                auto* ext = builder_->CreateFPExt(initVal, llvm::Type::getDoubleTy(*context_));
                boxed = builder_->CreateBitCast(ext, llvm::Type::getInt64Ty(*context_));
            } else if (initVal->getType()->isIntegerTy()) {
                tag = 1;
                boxed = builder_->CreateSExt(initVal, llvm::Type::getInt64Ty(*context_));
            }
            builder_->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context_), tag), tagPtr);
            builder_->CreateStore(boxed, valPtr);
            namedValues_[vd.varName] = anyAlloca;
            return;
        }
        if (vd.initExpr->kind == ExprKind::StructInit) {
            // Prefer the Sema-inferred concrete instantiation name (e.g. "Cache<i64,i64>")
            // over the bare struct name from the AST ("Cache") so that findClassMethod
            // can locate the monomorphised methods registered under the mangled key.
            std::string structClassName = vd.initExpr->as<StructInitExpr>()->structName;
            if (vd.initExpr->inferredType &&
                vd.initExpr->inferredType->kind == VyxTypeKind::Class &&
                vd.initExpr->inferredType->name.find('<') != std::string::npos) {
                structClassName = vd.initExpr->inferredType->name;
            }
            classVarTypes_[vd.varName] = structClassName;
        }
        if (vd.initExpr->kind == ExprKind::Call && vd.initExpr->as<CallExpr>()->callee &&
            vd.initExpr->as<CallExpr>()->callee->kind == ExprKind::Identifier) {
            auto& callName = vd.initExpr->as<CallExpr>()->callee->as<IdentifierExpr>()->name;
            // Compile-time intrinsics (sizeof/alignof/alloc/transmute/...) carry type
            // arguments as computation inputs, not class template parameters. Skip
            // the ctor-detection pass so `sizeof<i8>` isn't mangled and handed to
            // `getOrCreateGenericStructType`.
            static const std::set<std::string> kComptimeIntrinsics = {
                "sizeof", "alignof", "alloc", "transmute",
                "type_name", "typeinfo", "dealloc",
                "from_cstr", "from_cstr_len", "from_cstr_view_len", "from_raw_string_parts", "to_rawptr",
            };
            if (!kComptimeIntrinsics.count(callName)) {
                // Full template specialization fast-path: `let m = marker::<i32>(123);`
                // has the same shape as a templated ctor call (`Foo::<i32>(...)`),
                // but `marker` is a function, not a class. If the mangled name
                // (`marker<i32>`) or the bare callee name already resolves to a
                // known non-method function, skip the ctor-detection path so we
                // never try to instantiate a struct named `marker<i32>` and
                // misclassify the variable as a class instance.
                auto& ctorTypeArgs = vd.initExpr->as<CallExpr>()->callee->as<IdentifierExpr>()->callTypeArgs;
                bool isKnownPlainFunction = false;
                if (!ctorTypeArgs.empty()) {
                    std::string mangledFn = buildMangledClassName(callName, ctorTypeArgs);
                    if (!mangledFn.empty() &&
                        mangledFn.find('.') == std::string::npos &&
                        functions_.find(mangledFn) != functions_.end()) {
                        isKnownPlainFunction = true;
                    }
                }
                if (!isKnownPlainFunction &&
                    functions_.find(callName) != functions_.end() &&
                    !structTypes_.count(callName)) {
                    isKnownPlainFunction = true;
                }
                if (!isKnownPlainFunction) {
                    std::string ctorClass = callName;
                    if (!ctorTypeArgs.empty()) {
                        std::string mangled = buildMangledClassName(callName, ctorTypeArgs);
                        // TODO(P1c-C): Mono scan gap — if mangled is absent from
                        // structTypes_ here, Mono missed emitting it; no on-demand creation.
                        if (structTypes_.count(mangled))
                            ctorClass = mangled;
                    }
                    if (functions_.find(ctorClass + ".__ctor") != functions_.end() ||
                        functions_.find(ctorClass + ".__ctor_" + std::to_string(vd.initExpr->as<CallExpr>()->args.size())) != functions_.end() ||
                        functions_.find(callName + ".__ctor") != functions_.end() ||
                        functions_.find(callName + ".__ctor_" + std::to_string(vd.initExpr->as<CallExpr>()->args.size())) != functions_.end()) {
                        classVarTypes_[vd.varName] = ctorClass;
                    }
                }
            }
        }
        if (vd.initExpr->kind == ExprKind::Call && vd.initExpr->as<CallExpr>()->callee &&
            vd.initExpr->as<CallExpr>()->callee->kind == ExprKind::MemberAccess &&
            vd.initExpr->as<CallExpr>()->callee->as<MemberAccessExpr>()->object) {
            auto* maObj = vd.initExpr->as<CallExpr>()->callee->as<MemberAccessExpr>()->object.get();
            std::string typeName;
            static const std::vector<TypePtr> emptyTA;
            static const std::vector<ExprPtr> emptyAE;
            const std::vector<TypePtr>* typeArgsPtr = &emptyTA;
            const std::vector<ExprPtr>* argExprsPtr = &emptyAE;
            if (maObj->kind == ExprKind::Identifier) {
                typeName = maObj->as<IdentifierExpr>()->name;
                typeArgsPtr = &maObj->as<IdentifierExpr>()->callTypeArgs;
                argExprsPtr = &maObj->as<IdentifierExpr>()->callArgExprs;
            } else if (maObj->kind == ExprKind::SelfExpr) {
                typeName = "self";
            }
            auto& methodName = vd.initExpr->as<CallExpr>()->callee->as<MemberAccessExpr>()->member;
            std::string resolvedName = buildMangledClassName(typeName, *typeArgsPtr, *argExprsPtr);
            // TODO(P1c-C): Mono scan gap — if resolvedName is absent from
            // structTypes_ here, Mono missed emitting it; no on-demand creation.
            if (!structTypes_.count(resolvedName))
                resolvedName = typeName;
            std::string funcName = resolvedName + "." + methodName;
            auto fnIt2 = functions_.find(funcName);
            if (fnIt2 == functions_.end()) fnIt2 = functions_.find(typeName + "." + methodName);
            if (fnIt2 != functions_.end() && fnIt2->second) {
                auto* retTy = fnIt2->second->getReturnType();
                if (retTy->isVoidTy() && fnIt2->second->arg_size() > 0) {
                    auto sretAttr2 = fnIt2->second->getAttributes().getParamAttr(0, llvm::Attribute::StructRet);
                    if (sretAttr2.isValid()) {
                        retTy = sretAttr2.getValueAsType();
                    }
                }
                bool usedRetType = false;
                if (auto* stTy = llvm::dyn_cast<llvm::StructType>(retTy)) {
                    if (stTy->hasName()) {
                        std::string retName = stTy->getName().str();
                        if (structTypes_.count(retName)) {
                            classVarTypes_[vd.varName] = retName;
                            usedRetType = true;
                        } else if (retName.find('<') != std::string::npos) {
                            // TODO(P1c-C): Mono scan gap — if retName is absent from
                            // structTypes_ here, Mono missed emitting it; no on-demand creation.
                            if (structTypes_.count(retName)) {
                                classVarTypes_[vd.varName] = retName;
                                usedRetType = true;
                            }
                        }
                    }
                }
                if (!usedRetType && structTypes_.count(resolvedName)) {
                    classVarTypes_[vd.varName] = resolvedName;
                }
            }
        }
        // Map builtin container constructors to std class types
        // Uses mangled name (e.g. "Vec<i32>") when type args are present
        if (vd.initExpr->kind == ExprKind::Call && vd.initExpr->as<CallExpr>()->callee &&
            vd.initExpr->as<CallExpr>()->callee->kind == ExprKind::Identifier &&
            classVarTypes_.find(vd.varName) == classVarTypes_.end()) {
            auto& callName = vd.initExpr->as<CallExpr>()->callee->as<IdentifierExpr>()->name;
            static const std::map<std::string, std::string> builtinToClass = {
                {"makeRef", "Ref"}, {"makeMutex", "Mutex"},
                {"makeChannel", "Channel"}, {"makeScope", "Scope"},
            };
            auto bIt = builtinToClass.find(callName);
            if (bIt != builtinToClass.end()) {
                std::string className = buildMangledClassName(
                    bIt->second, vd.initExpr->as<CallExpr>()->callee->as<IdentifierExpr>()->callTypeArgs);
                // TODO(P1c-C): Mono scan gap — if className is absent from
                // structTypes_ here, Mono missed emitting it; no on-demand creation.
                if (!structTypes_.count(className))
                    className = bIt->second;
                classVarTypes_[vd.varName] = className;
            }
        }
        // Track Ref<T> inner type when initialized via Ref::<T>.new(...)
        if (vd.initExpr->kind == ExprKind::Call && vd.initExpr->as<CallExpr>()->callee &&
            vd.initExpr->as<CallExpr>()->callee->kind == ExprKind::MemberAccess) {
            auto& maCallee = *vd.initExpr->as<CallExpr>()->callee->as<MemberAccessExpr>();
            if (maCallee.member == "new" && maCallee.object &&
                maCallee.object->kind == ExprKind::Identifier) {
                auto& objIdent = *maCallee.object->as<IdentifierExpr>();
                if (objIdent.name == "Ref" && !objIdent.callTypeArgs.empty()) {
                    std::string innerTypeName =
                        mangleTypeAnnotationNested(*objIdent.callTypeArgs[0]);
                    refInnerTypeNames_[vd.varName] = innerTypeName;
                }
            }
            // P4-D.2: propagate inner type through Ref<T>-returning methods
            // such as `r2 = r1.clone()` or `opt_upgrade_result`.  Without
            // this, `r2.count()` falls through to auto-deref and
            // catastrophically calls the method with `self = data_ptr`
            // instead of `self = ref_struct_ptr` — fatal for any method
            // that reads `self.rc`.
            if ((maCallee.member == "clone") && maCallee.object &&
                maCallee.object->kind == ExprKind::Identifier) {
                auto& srcIdent = *maCallee.object->as<IdentifierExpr>();
                auto srcIt = refInnerTypeNames_.find(srcIdent.name);
                if (srcIt != refInnerTypeNames_.end()) {
                    refInnerTypeNames_[vd.varName] = srcIt->second;
                }
            }
        }

        // Detect class type from return value: if the called function returns a registered struct type
        if (vd.initExpr->kind == ExprKind::Call && vd.initExpr->as<CallExpr>()->callee &&
            vd.initExpr->as<CallExpr>()->callee->kind == ExprKind::Identifier &&
            classVarTypes_.find(vd.varName) == classVarTypes_.end()) {
            auto& callName = vd.initExpr->as<CallExpr>()->callee->as<IdentifierExpr>()->name;
            auto fnIt = functions_.find(callName);
            if (fnIt != functions_.end()) {
                auto* retTy = fnIt->second->getReturnType();
                // For sret functions, the "return type" is the struct type from sret attribute
                if (retTy->isVoidTy() && fnIt->second->arg_size() > 0) {
                    auto sretAttr = fnIt->second->getAttributes().getParamAttr(0, llvm::Attribute::StructRet);
                    if (sretAttr.isValid()) {
                        retTy = sretAttr.getValueAsType();
                    }
                }
                if (auto* stTy = llvm::dyn_cast<llvm::StructType>(retTy)) {
                    if (stTy->hasName()) {
                        std::string retName = stTy->getName().str();
                        if (structTypes_.count(retName) && structFieldNames_.count(retName)) {
                            classVarTypes_[vd.varName] = retName;
                        }
                    }
                }
            }
        }
        // Auto-call init() constructor after struct init
        bool hasInit = false;
        llvm::Function* initFn = nullptr;
        if (vd.initExpr->kind == ExprKind::StructInit) {
            initFn = findClassMethod(vd.initExpr->as<StructInitExpr>()->structName, "init");
            hasInit = initFn != nullptr;
        }
        auto* initVal = emitExpr(*vd.initExpr);
        bool hasExplicitType = (vd.varType != nullptr);
        if (initVal && !hasExplicitType) varType = initVal->getType();

        // Fat closure detection: emitClosure returns an llvm::Function* for bare closures
        // (no captures) and a malloc CallInst ptr for capturing closures (fat ptr).
        // Additionally, a higher-order function returning fn(...)→... also yields a fat ptr.
        //   Case 1: Direct closure literal — check by whether initVal is llvm::Function*.
        //   Case 2: Call to function with fn return type — check AST return type annotation.
        if (initVal && vd.initExpr && vd.initExpr->kind == ExprKind::Closure) {
            if (!llvm::dyn_cast_or_null<llvm::Function>(initVal)) {
                // Pull the closure's return type out of the ClosureExpr's
                // inferred function type (set by Sema). Lets call-sites use
                // the correct LLVM type instead of defaulting to i64.
                llvm::Type* retTy = llvm::Type::getInt64Ty(*context_);
                auto* ce = vd.initExpr->as<const ClosureExpr>();
                if (ce && ce->inferredType &&
                    ce->inferredType->kind == VyxTypeKind::Function &&
                    ce->inferredType->returnType) {
                    auto* inferred = toLLVMType(*ce->inferredType->returnType);
                    if (inferred && !inferred->isVoidTy()) retTy = inferred;
                }
                closureFatPtrVars_[vd.varName] = retTy;
            }
        } else if (vd.initExpr && vd.initExpr->kind == ExprKind::Call && unit_) {
            auto& callExpr2 = *vd.initExpr->as<CallExpr>();
            if (callExpr2.callee && callExpr2.callee->kind == ExprKind::Identifier) {
                auto& callee2Name = callExpr2.callee->as<IdentifierExpr>()->name;
                bool registered = false;
                for (auto& dd : unit_->declarations) {
                    if (!dd || dd->kind != DeclKind::Function || dd->name != callee2Name) continue;
                    auto* fd2 = dd->as<FunctionDecl>();
                    if (fd2->returnType &&
                        fd2->returnType->kind == TypeAnnotationKind::Function) {
                        // Unknown return type for higher-order fn results
                        // — fall back to i64 here; correct lowering for
                        // those needs a deeper pass.
                        closureFatPtrVars_[vd.varName] =
                            llvm::Type::getInt64Ty(*context_);
                        registered = true;
                    }
                    break;
                }
                // Callee not a top-level function — check if it's itself a
                // closure-typed variable (`let add5 = cadd(5);` where `cadd`
                // was registered earlier as a closure returning a closure).
                // Without this, `add5` stays unregistered and later call
                // sites look up a non-existent function symbol.
                if (!registered &&
                    closureFatPtrVars_.count(callee2Name)) {
                    // Chained closure: calling cadd yields another closure
                    // (the returned fn type carries its own return type, but
                    // we don't track it precisely here — i64 is the safe
                    // default used elsewhere for opaque fn returns).
                    closureFatPtrVars_[vd.varName] =
                        llvm::Type::getInt64Ty(*context_);
                }
            }
        }

        if (auto* closureFn = llvm::dyn_cast_or_null<llvm::Function>(initVal)) {
            functions_[vd.varName] = closureFn;
            varType = llvm::PointerType::getUnqual(*context_);
        }
        // LV-17: vars initialized from a function-typed expression that is
        // already materialized as a fat-pointer heap handle (ptr) must be
        // registered in closureFatPtrVars_ so later `f(...)` calls route via
        // CodeGenCall's `{fn_ptr, env_ptr}` path instead of raw indirect_call.
        // Typical shape: `let g = vec_of_fn.get(i); g(x)` where `get(i)`
        // returns a pointer to `__closure_fat_ptr`.
        if (initVal && !llvm::isa<llvm::Function>(initVal)) {
            bool fnTyped = false;
            llvm::Type* retTy = llvm::Type::getInt64Ty(*context_);
            if (vd.varType && vd.varType->kind == TypeAnnotationKind::Function) {
                fnTyped = true;
                auto& ft = static_cast<const FunctionType&>(*vd.varType);
                if (ft.returnType) {
                    auto* infer = toLLVMType(*ft.returnType);
                    if (infer && !infer->isVoidTy()) retTy = infer;
                }
            } else if (vd.initExpr && vd.initExpr->inferredType &&
                       vd.initExpr->inferredType->kind == VyxTypeKind::Function) {
                fnTyped = true;
                if (vd.initExpr->inferredType->returnType) {
                    auto* infer = toLLVMType(*vd.initExpr->inferredType->returnType);
                    if (infer && !infer->isVoidTy()) retTy = infer;
                }
            } else if (auto* unwrapRetTy = unwrapCallFunctionReturnType(vd.initExpr.get())) {
                fnTyped = true;
                retTy = unwrapRetTy;
            }
            if (fnTyped) closureFatPtrVars_[vd.varName] = retTy;
        }

        auto* alloca = createEntryBlockAlloca(fn, varType, vd.varName);
        if (initVal) {
            if (hasExplicitType && varType->isStructTy() &&
                !initVal->getType()->isStructTy()) {
                auto* memsetFn = module_->getFunction("memset");
                if (memsetFn) {
                    uint64_t sz = module_->getDataLayout().getTypeAllocSize(varType);
                    builder_->CreateCall(memsetFn, {
                        alloca,
                        llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0),
                        llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), sz)
                    });
                }
            } else {
                builder_->CreateStore(initVal, alloca);
            }
        }
        namedValues_[vd.varName] = alloca;
        // Track Box<T>/Ref<T>/Scope<T>.new() inits — this branch handles
        // class-method-returning-struct inits (which is where Box.new /
        // Ref.new land) and returns early at line ~766 before the general
        // smart-pointer tracking further down can run.
        if (vd.initExpr && vd.initExpr->kind == ExprKind::Call) {
            auto* callE = vd.initExpr->as<CallExpr>();
            if (callE->callee && callE->callee->kind == ExprKind::MemberAccess) {
                auto* ma = callE->callee->as<MemberAccessExpr>();
                if (ma->member == "new" && ma->object &&
                    ma->object->kind == ExprKind::Identifier) {
                    auto* obj = ma->object->as<IdentifierExpr>();
                    bool isSmartPtr = (obj->name == "Box" || obj->name == "Ref" ||
                                       obj->name == "Scope");
                    if (isSmartPtr &&
                        !obj->callTypeArgs.empty() && obj->callTypeArgs[0] &&
                        refInnerTypeNames_.find(vd.varName) == refInnerTypeNames_.end()) {
                        refInnerTypeNames_[vd.varName] =
                            mangleTypeAnnotationNested(*obj->callTypeArgs[0]);
                    }
                }
            }
        }
        // Propagate interface tag when the inferred init type is an
        // `__iface_X` struct (e.g. `let s = shapes.get(0)` where
        // `shapes` is a `Vec<Shape>`). Without this, later `s.method()`
        // calls miss the vtable-dispatch path and fall into the
        // "search all struct fields" fallback in CodeGenExprAccess,
        // which silently reinterprets the iface value as a vtable
        // struct and jumps into garbage memory.
        if (varType && varType->isStructTy()) {
            auto* stTy = llvm::cast<llvm::StructType>(varType);
            if (stTy->hasName() && stTy->getName().starts_with("__iface_")) {
                interfaceVarTypes_[vd.varName] = stTy->getName().str().substr(8);
            }
        }
        if (lastCollectedElemType_) {
            containerElemTypes_[vd.varName] = lastCollectedElemType_;
            containerTypes_[vd.varName] = "Vec";
            lastCollectedElemType_ = nullptr;
        }
        if (hasInit && initFn) {
            builder_->CreateCall(initFn, {alloca});
        }
        // Auto-track struct types for method dispatch,
        // upgrade base name → mangled name when return type is generic
        {
            auto cvIt = classVarTypes_.find(vd.varName);
            bool needsTrack = (cvIt == classVarTypes_.end());
            if (auto* stTy = llvm::dyn_cast<llvm::StructType>(varType)) {
                if (stTy->hasName()) {
                    std::string tyName = stTy->getName().str();
                    if (isInternalStructType(tyName))
                        needsTrack = false;
                    if (!needsTrack && cvIt != classVarTypes_.end() &&
                        tyName.find('<') != std::string::npos && cvIt->second != tyName)
                        needsTrack = true;
                    if (needsTrack && structTypes_.count(tyName))
                        classVarTypes_[vd.varName] = tyName;
                }
            }
        }
        // Propagate type info from MemberAccess init (e.g. let unit_ref = self.unit)
        if (vd.initExpr->kind == ExprKind::MemberAccess && unit_) {
            auto& maInit = *vd.initExpr->as<MemberAccessExpr>();
            std::string objType;
            if (maInit.object && maInit.object->kind == ExprKind::SelfExpr && !currentClassName_.empty())
                objType = currentClassName_;
            else if (maInit.object && maInit.object->kind == ExprKind::Identifier) {
                auto& idn = maInit.object->as<IdentifierExpr>()->name;
                objType = resolveClassName(*maInit.object, idn);
                if (objType.empty()) objType = resolveRefInner(*maInit.object, idn);
            }
            if (!objType.empty()) {
                for (auto& d : unit_->declarations) {
                    if (!d || d->name != objType) continue;
                    std::vector<FieldDecl>* flds = nullptr;
                    if (d->kind == DeclKind::Class) flds = &d->as<ClassDecl>()->fields;
                    else if (d->kind == DeclKind::Struct) flds = &d->as<StructDecl>()->fields;
                    if (flds) {
                        for (auto& f : *flds) {
                            if (f.name != maInit.member || !f.type) continue;
                            if ((f.type->name == "Ref" || f.type->name == "Scope" || f.type->name == "Box") &&
                                f.type->kind == TypeAnnotationKind::Generic) {
                                auto& fSubs = static_cast<const GenericType&>(*f.type).typeArgs;
                                if (!fSubs.empty() && fSubs[0] &&
                                    refInnerTypeNames_.find(vd.varName) == refInnerTypeNames_.end()) {
                                    refInnerTypeNames_[vd.varName] =
                                        mangleTypeAnnotationNested(*fSubs[0]);
                                }
                            } else if (f.type->kind == TypeAnnotationKind::Named ||
                                       f.type->kind == TypeAnnotationKind::Generic) {
                                std::string mn = mangleTypeAnnotation(*f.type);
                                if (structTypes_.count(mn) && classVarTypes_.find(vd.varName) == classVarTypes_.end())
                                    classVarTypes_[vd.varName] = mn;
                                else if (structTypes_.count(f.type->name) && classVarTypes_.find(vd.varName) == classVarTypes_.end())
                                    classVarTypes_[vd.varName] = f.type->name;
                            }
                        }
                    }
                    break;
                }
            }
        }
        // Copy type info from source variable (e.g. let b = a)
        if (vd.initExpr->kind == ExprKind::Identifier) {
            auto& srcName = vd.initExpr->as<IdentifierExpr>()->name;
            if (refInnerTypeNames_.find(vd.varName) == refInnerTypeNames_.end()) {
                auto srcRef = refInnerTypeNames_.find(srcName);
                if (srcRef != refInnerTypeNames_.end())
                    refInnerTypeNames_[vd.varName] = srcRef->second;
            }
            if (classVarTypes_.find(vd.varName) == classVarTypes_.end()) {
                auto srcCv = classVarTypes_.find(srcName);
                if (srcCv != classVarTypes_.end())
                    classVarTypes_[vd.varName] = srcCv->second;
            }
            if (containerTypes_.find(vd.varName) == containerTypes_.end()) {
                auto srcCt = containerTypes_.find(srcName);
                if (srcCt != containerTypes_.end()) {
                    containerTypes_[vd.varName] = srcCt->second;
                    auto srcElem = containerElemTypes_.find(srcName);
                    if (srcElem != containerElemTypes_.end())
                        containerElemTypes_[vd.varName] = srcElem->second;
                }
            }
        }
        // Track Ref<T>/Box<T> inner type for auto-deref member access
        if (refInnerTypeNames_.find(vd.varName) == refInnerTypeNames_.end()) {
            auto rcIt3 = structTypes_.find("__RefCounted");
            bool isRef3 = (rcIt3 != structTypes_.end() && varType == rcIt3->second) ||
                          (varType && varType->isPointerTy());
            if (isRef3 && vd.initExpr->kind == ExprKind::Call && unit_) {
                std::string calledFn;
                auto& ce = *vd.initExpr->as<CallExpr>();
                if (ce.callee) {
                    if (ce.callee->kind == ExprKind::Identifier)
                        calledFn = ce.callee->as<IdentifierExpr>()->name;
                    else if (ce.callee->kind == ExprKind::MemberAccess) {
                        auto& mac = *ce.callee->as<MemberAccessExpr>();
                        std::string oc;
                        if (mac.object && mac.object->kind == ExprKind::SelfExpr && !currentClassName_.empty())
                            oc = currentClassName_;
                        else if (mac.object && mac.object->kind == ExprKind::Identifier) {
                            auto& idn = mac.object->as<IdentifierExpr>()->name;
                            oc = resolveClassName(*mac.object, idn);
                            if (oc.empty()) oc = resolveRefInner(*mac.object, idn);
                        } else if (mac.object && mac.object->kind == ExprKind::MemberAccess) {
                            auto& im = *mac.object->as<MemberAccessExpr>();
                            std::string pc;
                            if (im.object && im.object->kind == ExprKind::SelfExpr && !currentClassName_.empty()) pc = currentClassName_;
                            else if (im.object && im.object->kind == ExprKind::Identifier) {
                                pc = resolveClassName(*im.object,
                                    im.object->as<IdentifierExpr>()->name);
                            }
                            if (!pc.empty()) {
                                for (auto& dd : unit_->declarations) {
                                    if (!dd || dd->name != pc) continue;
                                    std::vector<FieldDecl>* flds2 = nullptr;
                                    if (dd->kind == DeclKind::Class) flds2 = &dd->as<ClassDecl>()->fields;
                                    else if (dd->kind == DeclKind::Struct) flds2 = &dd->as<StructDecl>()->fields;
                                    if (flds2) {
                                        for (auto& f2 : *flds2) {
                                            if (f2.name == im.member && f2.type) {
                                                oc = f2.type->name;
                                                if ((f2.type->name == "Ref" || f2.type->name == "Scope") &&
                                                    f2.type->kind == TypeAnnotationKind::Generic) {
                                                    auto& fs = static_cast<const GenericType&>(*f2.type).typeArgs;
                                                    if (!fs.empty() && fs[0]) oc = mangleTypeAnnotation(*fs[0]);
                                                }
                                            }
                                        }
                                    }
                                    break;
                                }
                            }
                        }
                        if (!oc.empty()) calledFn = oc + "." + mac.member;
                        else calledFn = mac.member;
                    }
                }
                // Track Vec<T>.get() element type from container field annotation
                if (varType && varType->isPointerTy() && ce.callee &&
                    ce.callee->kind == ExprKind::MemberAccess) {
                    auto& getMac = *ce.callee->as<MemberAccessExpr>();
                    if (methodReturnsContainerElement(getMac.member) &&
                        getMac.object && getMac.object->kind == ExprKind::MemberAccess) {
                        auto& fieldMa = *getMac.object->as<MemberAccessExpr>();
                        std::string pClass;
                        if (fieldMa.object && fieldMa.object->kind == ExprKind::Identifier) {
                            pClass = resolveClassName(*fieldMa.object,
                                fieldMa.object->as<IdentifierExpr>()->name);
                        } else if (fieldMa.object && fieldMa.object->kind == ExprKind::SelfExpr && !currentClassName_.empty()) {
                            pClass = currentClassName_;
                        }
                        if (!pClass.empty()) {
                            for (auto& dd2 : unit_->declarations) {
                                if (!dd2 || dd2->name != pClass) continue;
                                std::vector<FieldDecl>* flds = nullptr;
                                if (dd2->kind == DeclKind::Class) flds = &dd2->as<ClassDecl>()->fields;
                                else if (dd2->kind == DeclKind::Struct) flds = &dd2->as<StructDecl>()->fields;
                                if (flds) {
                                    for (auto& f : *flds) {
                                        if (f.name == fieldMa.member && f.type &&
                                            (f.type->name == "Vec" || f.type->name == "Dict") &&
                                            f.type->kind == TypeAnnotationKind::Generic) {
                                            auto& vecSubs = static_cast<const GenericType&>(*f.type).typeArgs;
                                            if (!vecSubs.empty() && vecSubs[0]) {
                                                std::string elemMn = mangleTypeAnnotation(*vecSubs[0]);
                                                if (elemMn == "rawptr" && vecSubs[0]->name == "Box" &&
                                                    vecSubs[0]->kind == TypeAnnotationKind::Generic) {
                                                    auto& boxArgs = static_cast<const GenericType&>(*vecSubs[0]).typeArgs;
                                                    if (!boxArgs.empty() && boxArgs[0]) {
                                                        std::string innerMn = mangleTypeAnnotation(*boxArgs[0]);
                                                        if (structTypes_.count(innerMn))
                                                            refInnerTypeNames_[vd.varName] = innerMn;
                                                    }
                                                } else if (elemMn != "rawptr" && structTypes_.count(elemMn)) {
                                                    refInnerTypeNames_[vd.varName] = elemMn;
                                                }
                                            }
                                        }
                                    }
                                }
                                break;
                            }
                        }
                    }
                    if (methodReturnsContainerElement(getMac.member) &&
                        getMac.object && getMac.object->kind == ExprKind::Identifier &&
                        refInnerTypeNames_.find(vd.varName) == refInnerTypeNames_.end()) {
                        auto& varId = *getMac.object->as<IdentifierExpr>();
                        std::string cn = resolveClassName(*getMac.object, varId.name);
                        if (!cn.empty()) {
                            auto ltPos = cn.find('<');
                            if (ltPos != std::string::npos) {
                                std::string base = cn.substr(0, ltPos);
                                if (base == "Vec" || base == "Dict") {
                                    auto gtPos = cn.rfind('>');
                                    if (gtPos != std::string::npos && gtPos > ltPos) {
                                        std::string innerName = cn.substr(ltPos + 1, gtPos - ltPos - 1);
                                        // LV-17: `let f = vec_of_fn.get(i)` with no explicit type loses
                                        // function-ness at codegen time (initVal is only `ptr`), then call
                                        // sites route to raw indirect_call and treat the fat-ptr handle as
                                        // code address. Recover by marking the result as closure-fat-pointer
                                        // when container element looks like `fn(...)->R`.
                                        if (innerName.rfind("fn(", 0) == 0) {
                                            llvm::Type* retTy = llvm::Type::getInt64Ty(*context_);
                                            auto arrowPos = innerName.rfind(")->");
                                            if (arrowPos != std::string::npos) {
                                                std::string retName = innerName.substr(arrowPos + 3);
                                                retName.erase(std::remove_if(retName.begin(), retName.end(),
                                                    [](unsigned char c) { return std::isspace(c); }),
                                                    retName.end());
                                                if (!retName.empty()) {
                                                    TypeAnnotation ra;
                                                    ra.kind = TypeAnnotationKind::Named;
                                                    ra.name = retName;
                                                    auto* infer = toLLVMType(ra);
                                                    if (infer && !infer->isVoidTy()) retTy = infer;
                                                }
                                            }
                                            closureFatPtrVars_[vd.varName] = retTy;
                                        }
                                        if (innerName.size() > 4 && innerName.substr(0, 4) == "Box<" && innerName.back() == '>')
                                            innerName = innerName.substr(4, innerName.size() - 5);
                                        if (structTypes_.count(innerName))
                                            refInnerTypeNames_[vd.varName] = innerName;
                                    }
                                }
                            }
                        }
                    }
                }
                if (!calledFn.empty()) {
                    auto trackRet = [&](const TypeAnnotation* rt) {
                        if (!rt) return;
                        if ((rt->name == "Ref" || rt->name == "Scope" || rt->name == "Box") &&
                            rt->kind == TypeAnnotationKind::Generic) {
                            auto& rs = static_cast<const GenericType&>(*rt).typeArgs;
                            if (!rs.empty() && rs[0])
                                refInnerTypeNames_[vd.varName] =
                                    mangleTypeAnnotationNested(*rs[0]);
                        } else if (rt->kind == TypeAnnotationKind::Named) {
                            std::string mn = mangleTypeAnnotation(*rt);
                            if (structTypes_.count(mn))
                                refInnerTypeNames_[vd.varName] = mn;
                        }
                    };
                    for (auto& dd : unit_->declarations) {
                        if (!dd) continue;
                        if (dd->kind == DeclKind::Function && dd->name == calledFn) {
                            trackRet(dd->as<FunctionDecl>()->returnType.get());
                            break;
                        }
                        if (dd->kind == DeclKind::Class) {
                            auto dp = calledFn.find('.');
                            if (dp != std::string::npos && dd->name == calledFn.substr(0, dp)) {
                                for (auto& m : dd->as<ClassDecl>()->methods)
                                    if (m.name == calledFn.substr(dp + 1)) trackRet(m.returnType.get());
                                break;
                            }
                        }
                    }
                }
            }
        }
        // Infer container element types from classVarTypes_ (e.g. var v = Vec::<i32>.new())
        {
            auto cvIt = classVarTypes_.find(vd.varName);
            if (cvIt != classVarTypes_.end() && containerTypes_.find(vd.varName) == containerTypes_.end()) {
                auto& cn = cvIt->second;
                auto ltPos = cn.find('<');
                if (ltPos != std::string::npos) {
                    std::string base = cn.substr(0, ltPos);
                    auto gtPos = cn.rfind('>');
                    if (gtPos != std::string::npos && gtPos > ltPos) {
                        std::string innerName = cn.substr(ltPos + 1, gtPos - ltPos - 1);
                        if (base == "Vec") {
                            containerTypes_[vd.varName] = "Vec";
                            TypeAnnotation ta;
                            ta.kind = TypeAnnotationKind::Named;
                            ta.name = innerName;
                            containerElemTypes_[vd.varName] = toLLVMType(ta);
                        } else if (base == "Dict") {
                            containerTypes_[vd.varName] = "Dict";
                            auto commaPos = innerName.find(',');
                            if (commaPos != std::string::npos) {
                                TypeAnnotation ka;
                                ka.kind = TypeAnnotationKind::Named;
                                ka.name = innerName.substr(0, commaPos);
                                containerElemTypes_[vd.varName] = toLLVMType(ka);
                                TypeAnnotation va;
                                va.kind = TypeAnnotationKind::Named;
                                va.name = innerName.substr(commaPos + 1);
                                containerValTypes_[vd.varName] = toLLVMType(va);
                            }
                        }
                    }
                }
            }
        }
        maybeRetainRefCopy(alloca);
        return;
    }

    llvm::AllocaInst* alloca = nullptr;

    if (vd.initExpr) {
        if (auto* stHint = llvm::dyn_cast<llvm::StructType>(varType))
            targetTypeHint_ = stHint;
        if (auto* arrHint = llvm::dyn_cast<llvm::ArrayType>(varType))
            targetArrayElemHint_ = arrHint->getElementType();
        auto* initVal = emitExpr(*vd.initExpr);
        targetTypeHint_ = nullptr;
        targetArrayElemHint_ = nullptr;
        // Uniform fn-type ABI: `let f: fn(...)->R = bare_fn_ident;` stores
        // a {trampoline, null_env} fat pointer into f's slot, so later
        // `apply(f, x)` call sites — which pass the fat-ptr address into
        // a callee that extracts fn+env from it — see a well-formed fat
        // pointer. Without this, the slot holds only the raw fn pointer,
        // the callee reads 8 bytes of the fn's code as "env", and crashes
        // on call.
        if (vd.varType &&
            vd.varType->kind == TypeAnnotationKind::Function &&
            initVal && llvm::isa<llvm::Function>(initVal)) {
            auto* rawFn = llvm::cast<llvm::Function>(initVal);
            auto* fatAlloca = wrapRawFnAsFatPtr(rawFn);
            auto& ft = static_cast<const FunctionType&>(*vd.varType);
            llvm::Type* retTy = llvm::Type::getInt64Ty(*context_);
            if (ft.returnType) {
                auto* infer = toLLVMType(*ft.returnType);
                if (infer && !infer->isVoidTy()) retTy = infer;
            }
            closureFatPtrVars_[vd.varName] = retTy;
            initVal = fatAlloca;
        }
        // LV-17 companion: function-typed vars initialized from a pointer value
        // (e.g. Vec<fn>.get(i), higher-order returns) are fat-pointer handles.
        // Mark them so `f(...)` goes through closureFatPtrVars_ dispatch.
        if (vd.varType &&
            vd.varType->kind == TypeAnnotationKind::Function &&
            initVal && !llvm::isa<llvm::Function>(initVal)) {
            auto& ft = static_cast<const FunctionType&>(*vd.varType);
            llvm::Type* retTy = llvm::Type::getInt64Ty(*context_);
            if (ft.returnType) {
                auto* infer = toLLVMType(*ft.returnType);
                if (infer && !infer->isVoidTy()) retTy = infer;
            }
            closureFatPtrVars_[vd.varName] = retTy;
        }
        if (initVal) {
            if (!vd.varType && initVal->getType() != varType &&
                (initVal->getType()->isStructTy() || initVal->getType()->isPointerTy())) {
                varType = initVal->getType();
            }
            alloca = createEntryBlockAlloca(fn, varType, vd.varName);
            if (varType->isStructTy() && initVal->getType()->isIntegerTy()) {
                auto* memsetFn = module_->getFunction("memset");
                if (memsetFn) {
                    auto sz = module_->getDataLayout().getTypeAllocSize(varType);
                    builder_->CreateCall(memsetFn, {
                        alloca,
                        llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0),
                        llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), sz)
                    });
                }
            } else {
                initVal = castToType(initVal, varType);
                builder_->CreateStore(initVal, alloca);
            }
        } else if (!alloca) {
            alloca = createEntryBlockAlloca(fn, varType, vd.varName);
        }
    } else {
        alloca = createEntryBlockAlloca(fn, varType, vd.varName);
    }

    namedValues_[vd.varName] = alloca;

    // Track Ref<T>/Scope<T>/Box<T> inner type from explicit type annotation.
    // `let x: Box<Num> = ...` records "Num" so `x.deref()` can load the
    // typed value back instead of a truncated 8-byte slice.
    if (vd.varType &&
        (vd.varType->name == "Ref" || vd.varType->name == "Scope" ||
         vd.varType->name == "Box") &&
        vd.varType->kind == TypeAnnotationKind::Generic) {
        auto& refSubs = static_cast<const GenericType&>(*vd.varType).typeArgs;
        if (!refSubs.empty() && refSubs[0]) {
            refInnerTypeNames_[vd.varName] =
                mangleTypeAnnotationNested(*refSubs[0]);
        }
    }
    // Also track when init is `Box::<T>.new(...)` (no explicit annotation) —
    // reached via the general-VarDecl path for Ref; mirrors the tracking at
    // the early-return class-method branch above.
    if (vd.initExpr && vd.initExpr->kind == ExprKind::Call) {
        auto* callE = vd.initExpr->as<CallExpr>();
        if (callE->callee && callE->callee->kind == ExprKind::MemberAccess) {
            auto* ma = callE->callee->as<MemberAccessExpr>();
            if (ma->member == "new" && ma->object &&
                ma->object->kind == ExprKind::Identifier) {
                auto* obj = ma->object->as<IdentifierExpr>();
                bool isSmartPtr = (obj->name == "Box" || obj->name == "Ref" ||
                                   obj->name == "Scope");
                if (isSmartPtr &&
                    !obj->callTypeArgs.empty() && obj->callTypeArgs[0] &&
                    refInnerTypeNames_.find(vd.varName) == refInnerTypeNames_.end()) {
                    refInnerTypeNames_[vd.varName] =
                        mangleTypeAnnotationNested(*obj->callTypeArgs[0]);
                }
            }
        }
    }

    // Track Ref<T>/ptr inner type when init loads from struct field or method call
    if (vd.initExpr && refInnerTypeNames_.find(vd.varName) == refInnerTypeNames_.end()) {
        auto rcIt2 = structTypes_.find("__RefCounted");
        bool isRefOrPtr = (rcIt2 != structTypes_.end() && varType == rcIt2->second) ||
                          (varType && varType->isPointerTy());
        
        if (isRefOrPtr) {
            // If init is MemberAccess (e.g. self.inner), look up field type from struct definition
            if (vd.initExpr->kind == ExprKind::MemberAccess) {
                auto& ma = *vd.initExpr->as<MemberAccessExpr>();
                std::string objTypeName;
                if (ma.object && ma.object->kind == ExprKind::SelfExpr && !currentClassName_.empty())
                    objTypeName = currentClassName_;
                else if (ma.object && ma.object->kind == ExprKind::Identifier) {
                    objTypeName = resolveClassName(*ma.object,
                        ma.object->as<IdentifierExpr>()->name);
                }
                if (!objTypeName.empty()) {
                    auto ftIt = structFieldTypeNames_.find(objTypeName);
                    auto fnIt = structFieldNames_.find(objTypeName);
                    if (ftIt != structFieldTypeNames_.end() && fnIt != structFieldNames_.end()) {
                        for (size_t fi = 0; fi < fnIt->second.size() && fi < ftIt->second.size(); ++fi) {
                            if (fnIt->second[fi] == ma.member) {
                                std::string fieldTypeName = ftIt->second[fi];
                                // fieldTypeName might be "__RefCounted" but we need inner type
                                // Look in actual declarations for the field type annotation
                                if (unit_) {
                                    for (auto& d : unit_->declarations) {
                                        if (!d) continue;
                                        if ((d->kind == DeclKind::Class || d->kind == DeclKind::Struct) &&
                                            d->name == objTypeName) {
                                            const auto& fields = (d->kind == DeclKind::Class)
                                                ? d->as<const ClassDecl>()->fields
                                                : d->as<const StructDecl>()->fields;
                                            for (auto& f : fields) {
                                                if (f.name == ma.member && f.type) {
                                                    if ((f.type->name == "Ref" || f.type->name == "Scope") &&
                                                        f.type->kind == TypeAnnotationKind::Generic) {
                                                        auto& fSubs = static_cast<const GenericType&>(*f.type).typeArgs;
                                                        if (!fSubs.empty() && fSubs[0]) {
                                                            refInnerTypeNames_[vd.varName] =
                                                                mangleTypeAnnotationNested(*fSubs[0]);
                                                        }
                                                    }
                                                }
                                            }
                                            break;
                                        }
                                    }
                                }
                                break;
                            }
                        }
                    }
                }
            }
            // If init is Identifier (e.g. let b = a), copy inner type from source
            else if (vd.initExpr->kind == ExprKind::Identifier) {
                auto srcIt = refInnerTypeNames_.find(vd.initExpr->as<IdentifierExpr>()->name);
                if (srcIt != refInnerTypeNames_.end()) {
                    refInnerTypeNames_[vd.varName] = srcIt->second;
                }
            }
            // If init is a function/method call returning __RefCounted, find the Ref<T> inner type
            // by looking up the function declaration's return type annotation in the AST
            else if (vd.initExpr->kind == ExprKind::Call && unit_) {
                std::string calledFnName;
                auto& callExpr = *vd.initExpr->as<CallExpr>();
                if (callExpr.callee) {
                    if (callExpr.callee->kind == ExprKind::Identifier)
                        calledFnName = callExpr.callee->as<IdentifierExpr>()->name;
                    else if (callExpr.callee->kind == ExprKind::MemberAccess) {
                        auto& maCal = *callExpr.callee->as<MemberAccessExpr>();
                        std::string objClass;
                        if (maCal.object && maCal.object->kind == ExprKind::SelfExpr && !currentClassName_.empty())
                            objClass = currentClassName_;
                        else if (maCal.object && maCal.object->kind == ExprKind::Identifier) {
                            auto& idn = maCal.object->as<IdentifierExpr>()->name;
                            objClass = resolveClassName(*maCal.object, idn);
                            if (objClass.empty()) objClass = resolveRefInner(*maCal.object, idn);
                        }
                        // Handle nested member access: self.field.method() or obj.field.method()
                        else if (maCal.object && maCal.object->kind == ExprKind::MemberAccess) {
                            auto& innerMa = *maCal.object->as<MemberAccessExpr>();
                            std::string parentClass;
                            if (innerMa.object && innerMa.object->kind == ExprKind::SelfExpr && !currentClassName_.empty())
                                parentClass = currentClassName_;
                            else if (innerMa.object && innerMa.object->kind == ExprKind::Identifier) {
                                auto& idn = innerMa.object->as<IdentifierExpr>()->name;
                                parentClass = resolveClassName(*innerMa.object, idn);
                                if (parentClass.empty()) parentClass = resolveRefInner(*innerMa.object, idn);
                            }
                            if (!parentClass.empty() && unit_) {
                                for (auto& decl2 : unit_->declarations) {
                                    if (!decl2 || decl2->name != parentClass) continue;
                                    std::vector<FieldDecl>* flds = nullptr;
                                    if (decl2->kind == DeclKind::Class) flds = &decl2->as<ClassDecl>()->fields;
                                    else if (decl2->kind == DeclKind::Struct) flds = &decl2->as<StructDecl>()->fields;
                                    if (flds) {
                                        for (auto& f : *flds) {
                                            if (f.name == innerMa.member && f.type) {
                                                objClass = f.type->name;
                                                if ((f.type->name == "Ref" || f.type->name == "Scope") &&
                                                    f.type->kind == TypeAnnotationKind::Generic) {
                                                    auto& fSubs = static_cast<const GenericType&>(*f.type).typeArgs;
                                                    if (!fSubs.empty() && fSubs[0])
                                                        objClass = mangleTypeAnnotation(*fSubs[0]);
                                                }
                                            }
                                        }
                                    }
                                    break;
                                }
                            }
                        }
                        if (!objClass.empty()) calledFnName = objClass + "." + maCal.member;
                        else calledFnName = maCal.member;
                    }
                }
                if (!calledFnName.empty()) {
                    bool found = false;
                    auto tryTrackRetType = [&](const TypeAnnotation* retType) {
                        if (!retType) return;
                        if ((retType->name == "Ref" || retType->name == "Scope" || retType->name == "Box") &&
                            retType->kind == TypeAnnotationKind::Generic) {
                            auto& rSubs = static_cast<const GenericType&>(*retType).typeArgs;
                            if (!rSubs.empty() && rSubs[0]) {
                                refInnerTypeNames_[vd.varName] =
                                    mangleTypeAnnotationNested(*rSubs[0]);
                                found = true;
                            }
                        }
                    };
                    for (auto& d : unit_->declarations) {
                        if (!d) continue;
                        // Check standalone functions
                        if (d->kind == DeclKind::Function && d->name == calledFnName) {
                            tryTrackRetType(d->as<FunctionDecl>()->returnType.get());
                            break;
                        }
                        // Check class/struct methods
                        if (d->kind == DeclKind::Class) {
                            auto dotPos = calledFnName.find('.');
                            if (dotPos != std::string::npos) {
                                std::string className = calledFnName.substr(0, dotPos);
                                std::string methodName = calledFnName.substr(dotPos + 1);
                                if (d->name == className) {
                                    auto& methods = d->as<ClassDecl>()->methods;
                                    for (auto& m : methods) {
                                        if (m.name == methodName)
                                            tryTrackRetType(m.returnType.get());
                                    }
                                    break;
                                }
                            }
                        }
                        
                    }
                    if (!found && !calledFnName.empty()) {
                        auto fnIt = functions_.find(calledFnName);
                        if (fnIt != functions_.end() && fnIt->second->getReturnType()->isPointerTy()) {
                            for (auto& d2 : unit_->declarations) {
                                if (!d2) continue;
                                auto fnDotPos = calledFnName.find('.');
                                if (fnDotPos == std::string::npos) {
                                    if (d2->kind == DeclKind::Function && d2->name == calledFnName) {
                                        tryTrackRetType(d2->as<FunctionDecl>()->returnType.get());
                                        break;
                                    }
                                } else {
                                    std::string cn = calledFnName.substr(0, fnDotPos);
                                    std::string mn = calledFnName.substr(fnDotPos + 1);
                                    if (d2->kind == DeclKind::Class && d2->name == cn) {
                                        for (auto& m2 : d2->as<ClassDecl>()->methods)
                                            if (m2.name == mn) tryTrackRetType(m2.returnType.get());
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // Explicit annotations should override any stale function-local tracking.
    if (vd.varType) {
        if (auto* stTy = llvm::dyn_cast<llvm::StructType>(varType);
            stTy && stTy->hasName()) {
            classVarTypes_[vd.varName] = stTy->getName().str();
        } else {
            std::string annName = vd.varType->name;
            if (!annName.empty() && structTypes_.count(annName)) {
                classVarTypes_[vd.varName] = annName;
            }
            if (vd.varType->kind == TypeAnnotationKind::Generic &&
                !annName.empty() && !vd.varType->as<GenericType>()->typeArgs.empty()) {
                std::string mangled = mangleTypeAnnotation(*vd.varType);
                if (structTypes_.count(mangled)) {
                    classVarTypes_[vd.varName] = mangled;
                }
            }
        }
    }

    // Auto-retain for Ref<T> copies: `let r2 = r` / `let r2 = self.field`
    // increments the shared header's strong count. Fresh constructors such
    // as `Ref::<T>.new(...)` already return a handle with strong=1, and
    // `clone()` performs its own retain, so this is deliberately limited to
    // plain existing-value copies.
    maybeRetainRefCopy(alloca);

    // Auto-RAII disabled: Vyx lacks move semantics, so auto-destroy on scope
    // exit causes use-after-free when values are shared via Dict.put/Vec.push.
    // Rely on explicit destroy() calls or process exit for cleanup.

    if (vd.elseBranch) {
        auto* fn = builder_->GetInsertBlock()->getParent();
        auto* val = builder_->CreateLoad(alloca->getAllocatedType(), alloca, vd.varName + ".val");
        llvm::Value* isElse = nullptr;
        if (auto* stTy = llvm::dyn_cast<llvm::StructType>(val->getType())) {
            if (isOptionOrResultSlotName(stTy->getName())) {
                auto* tmpAlloca = createEntryBlockAlloca(fn, stTy, "let.else.opt");
                builder_->CreateStore(val, tmpAlloca);
                auto* tagPtr = builder_->CreateStructGEP(stTy, tmpAlloca, 0, "let.else.tag.ptr");
                auto* leTagTy = stTy->getElementType(0);
                auto* tag = builder_->CreateLoad(leTagTy, tagPtr, "let.else.tag");
                isElse = builder_->CreateICmpEQ(tag,
                    llvm::ConstantInt::get(leTagTy, 1), "let.else.isNone");
            }
        }
        if (!isElse) {
            if (val->getType()->isPointerTy())
                isElse = builder_->CreateICmpEQ(val, llvm::Constant::getNullValue(val->getType()), "let.else.check");
            else
                isElse = builder_->getInt1(false);
        }
        auto* elseBB = llvm::BasicBlock::Create(*context_, "let.else", fn);
        auto* contBB = llvm::BasicBlock::Create(*context_, "let.cont", fn);
        builder_->CreateCondBr(isElse, elseBB, contBB);
        builder_->SetInsertPoint(elseBB);
        emitBlock(*vd.elseBranch);
        if (builder_->GetInsertBlock() && !builder_->GetInsertBlock()->getTerminator())
            builder_->CreateBr(contBB);
        builder_->SetInsertPoint(contBB);
    }
}

} // namespace vyx
