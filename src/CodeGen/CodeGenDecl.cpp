#include "CodeGenIncludes.h"
#include <map>
#include <set>

namespace vyx {

void CodeGen::emitDecl(Decl& decl) {
    for (auto& [attrName, attrValue] : decl.attributes) {
        if (attrName == "platform") {
#ifdef _WIN32
            if (attrValue != "windows") return;
#elif __linux__
            if (attrValue != "linux") return;
#elif __APPLE__
            if (attrValue != "macos") return;
#endif
        }
    }
    switch (decl.kind) {
        case DeclKind::Function:
            if (auto* fn = decl.as<FunctionDecl>(); fn && fn->isAsync) {
                emitAsyncFunctionDecl(decl);
            } else {
                emitFunctionDecl(decl);
            }
            break;
        case DeclKind::Struct:      emitStructDecl(decl); break;
        case DeclKind::ExternBlock: emitExternBlock(decl); break;
        case DeclKind::GlobalVar: {
            auto& gvDecl = *decl.as<GlobalVarDecl>();
            llvm::Type* varType = llvm::Type::getInt32Ty(*context_);
            llvm::Constant* initVal = llvm::ConstantInt::get(varType, 0);
            bool isContainer = false;
            std::string containerKind;
            if (gvDecl.varType) {
                varType = toLLVMType(*gvDecl.varType);
                auto& rn = gvDecl.varType->name;
                auto& vtSubs = getTypeSubTypes(*gvDecl.varType);
                if (rn == "Vec") {
                    isContainer = true; containerKind = "Vec";
                    containerTypes_[decl.name] = "Vec";
                    if (!vtSubs.empty())
                        containerElemTypes_[decl.name] = toLLVMType(*vtSubs[0]);
                } else if (rn == "Dict") {
                    isContainer = true; containerKind = "Dict";
                    containerTypes_[decl.name] = "Dict";
                    if (vtSubs.size() >= 1)
                        containerElemTypes_[decl.name] = toLLVMType(*vtSubs[0]);
                    if (vtSubs.size() >= 2)
                        containerValTypes_[decl.name] = toLLVMType(*vtSubs[1]);
                }
            }
            Expr* gvInitExprRaw = nullptr;
            if (gvDecl.initBody && gvDecl.initBody->kind == StmtKind::ExprStmt) {
                gvInitExprRaw = static_cast<ExprStmt*>(gvDecl.initBody.get())->expr.get();
            }
            if (!isContainer && gvInitExprRaw &&
                gvInitExprRaw->kind == ExprKind::Call) {
                auto* callE = gvInitExprRaw->as<CallExpr>();
                if (callE->callee && callE->callee->kind == ExprKind::Identifier) {
                    auto* calId = callE->callee->as<IdentifierExpr>();
                    auto& cn = calId->name;
                    if (cn == "makeDict" || cn == "makeUnorderedMap") {
                        isContainer = true; containerKind = "Dict";
                        containerTypes_[decl.name] = "Dict";
                        auto& typeArgs = calId->callTypeArgs;
                        if (typeArgs.size() >= 1 && typeArgs[0])
                            containerElemTypes_[decl.name] = toLLVMType(*typeArgs[0]);
                        if (typeArgs.size() >= 2 && typeArgs[1])
                            containerValTypes_[decl.name] = toLLVMType(*typeArgs[1]);
                    } else if (cn == "makeVec") {
                        isContainer = true; containerKind = "Vec";
                        containerTypes_[decl.name] = "Vec";
                        auto& typeArgs = calId->callTypeArgs;
                        if (!typeArgs.empty() && typeArgs[0])
                            containerElemTypes_[decl.name] = toLLVMType(*typeArgs[0]);
                    } else if (cn == "makeSet" || cn == "makeUnorderedSet") {
                        isContainer = true; containerKind = "Set";
                        containerTypes_[decl.name] = "Set";
                    } else if (cn == "makeStack") {
                        isContainer = true; containerKind = "Stack";
                        containerTypes_[decl.name] = "Stack";
                    } else if (cn == "makeQueue") {
                        isContainer = true; containerKind = "Queue";
                        containerTypes_[decl.name] = "Queue";
                    }
                }
                if (isContainer) {
                    auto vecIt = structTypes_.find("__Vec");
                    if (vecIt != structTypes_.end()) varType = vecIt->second;
                    else {
                        auto* ptrTy = llvm::PointerType::getUnqual(*context_);
                        auto* i64Ty = llvm::Type::getInt64Ty(*context_);
                        varType = llvm::StructType::create(*context_, {ptrTy, i64Ty, i64Ty, i64Ty}, "__Vec");
                        structTypes_["__Vec"] = llvm::cast<llvm::StructType>(varType);
                    }
                }
            }
            if (gvInitExprRaw) {
                auto* initExpr = gvInitExprRaw;
                if (initExpr->kind == ExprKind::IntLiteral) {
                    initVal = llvm::ConstantInt::get(varType, initExpr->as<IntLiteralExpr>()->value, true);
                } else if (initExpr->kind == ExprKind::UnaryOp) {
                    auto* unary = initExpr->as<UnaryOpExpr>();
                    if (unary->op == UnaryOp::Neg && unary->operand && unary->operand->kind == ExprKind::IntLiteral) {
                        initVal = llvm::ConstantInt::get(varType, -unary->operand->as<IntLiteralExpr>()->value, true);
                    } else if (unary->op == UnaryOp::Neg && unary->operand && unary->operand->kind == ExprKind::FloatLiteral) {
                        varType = llvm::Type::getDoubleTy(*context_);
                        initVal = llvm::ConstantFP::get(varType, -unary->operand->as<FloatLiteralExpr>()->value);
                    }
                } else if (initExpr->kind == ExprKind::FloatLiteral) {
                    varType = llvm::Type::getDoubleTy(*context_);
                    initVal = llvm::ConstantFP::get(varType, initExpr->as<FloatLiteralExpr>()->value);
                } else if (initExpr->kind == ExprKind::NullLiteral) {
                    varType = llvm::PointerType::getUnqual(*context_);
                    initVal = llvm::ConstantPointerNull::get(
                        llvm::PointerType::getUnqual(*context_));
                } else if (initExpr->kind == ExprKind::BoolLiteral) {
                    varType = llvm::Type::getInt8Ty(*context_);
                    initVal = llvm::ConstantInt::get(varType, initExpr->as<BoolLiteralExpr>()->value ? 1 : 0);
                } else if (initExpr->kind == ExprKind::StringLiteral) {
                    // Bug fix: `var X: string = "literal";` (or `let X: string = ...;`)
                    // at module scope used to leave `varType` set to the `__String`
                    // struct type while `initVal` stayed as `ConstantInt(i32, 0)`,
                    // tripping `verifyModule failed: Global variable initializer
                    // type does not match global variable type`. Emit a real
                    // `{ptr, len, cap, owned=0}` constant struct backed by a private
                    // i8 array holding the literal bytes (NUL-terminated).
                    auto* strTy = getOrCreateStringType();
                    auto& bytes = initExpr->as<StringLiteralExpr>()->value;
                    auto* strBytes = llvm::ConstantDataArray::getString(
                        *context_, bytes, /*AddNull=*/true);
                    auto* strGV = new llvm::GlobalVariable(
                        *module_, strBytes->getType(), /*isConstant=*/true,
                        llvm::GlobalValue::PrivateLinkage, strBytes,
                        ".str.gv." + decl.name);
                    auto* ptrTy = llvm::PointerType::getUnqual(*context_);
                    auto* i64Ty = llvm::Type::getInt64Ty(*context_);
                    int64_t len = static_cast<int64_t>(bytes.size());
                    varType = strTy;
                    initVal = llvm::ConstantStruct::get(
                        strTy,
                        {llvm::ConstantExpr::getBitCast(strGV, ptrTy),
                         llvm::ConstantInt::get(i64Ty, len),
                         llvm::ConstantInt::get(i64Ty, len),
                         llvm::ConstantInt::get(i64Ty, 0)});
                }
            }
            if (isContainer)
                initVal = llvm::ConstantAggregateZero::get(varType);
            if (!initVal || initVal->getType() != varType) {
                if (varType->isPointerTy()) {
                    initVal = llvm::ConstantPointerNull::get(
                        llvm::cast<llvm::PointerType>(varType));
                } else if (varType->isAggregateType()) {
                    initVal = llvm::ConstantAggregateZero::get(varType);
                } else if (varType->isIntegerTy()) {
                    initVal = llvm::ConstantInt::get(varType, 0);
                } else if (varType->isFloatingPointTy()) {
                    initVal = llvm::ConstantFP::get(varType, 0.0);
                }
            }
            bool isConst = !gvDecl.isMutableVar;
            auto* gv = new llvm::GlobalVariable(*module_, varType, isConst,
                decl.isExport ? llvm::GlobalValue::ExternalLinkage : llvm::GlobalValue::InternalLinkage,
                initVal, decl.name);
            for (auto& [an, av] : decl.attributes) {
                if (an == "thread_local") {
                    gv->setThreadLocalMode(llvm::GlobalValue::GeneralDynamicTLSModel);
                }
                if (an == "volatile") {
                    volatileVars_.insert(decl.name);
                }
            }
            if (gvDecl.varType) {
                std::string typeName = gvDecl.varType->name;
                auto& gvtSubs = getTypeSubTypes(*gvDecl.varType);
                if (!gvtSubs.empty())
                    typeName = buildMangledClassName(typeName, gvtSubs);
                // TODO(P1c-C): Mono scan gap — if the type is absent from
                // structTypes_ here after a generic name, Mono missed emitting it;
                // no on-demand creation.
                if (structTypes_.count(typeName))
                    classVarTypes_[decl.name] = typeName;
            }
            (void)gv;
            break;
        }
        case DeclKind::TypeAlias: {
            auto& taDecl = *decl.as<TypeAliasDecl>();
            if (taDecl.aliasType) {
                if (taDecl.isNewtype) {
                    auto* underlyingTy = toLLVMType(*taDecl.aliasType);
                    auto* wrapperTy = llvm::StructType::create(*context_, {underlyingTy}, decl.name);
                    structTypes_[decl.name] = wrapperTy;
                    structFieldNames_[decl.name] = {"__value"};
                }
            }
            break;
        }
        default: break;
    }
}

void CodeGen::emitFunctionDecl(Decl& decl) {
    fprintf(stderr, "[CodeGen] emitFunctionDecl: %s\n", decl.name.c_str()); fflush(stderr);
    auto& fd = *decl.as<FunctionDecl>();
    auto* fn = functions_[decl.name];
    if (!fn || !fd.body) return;

    if (!fn->empty()) return; // already emitted

    bool isNoArgMain = false;
    if (decl.name == "main" && fd.params.empty()) {
        isNoArgMain = true;
        auto* i32Ty = llvm::Type::getInt32Ty(*context_);
        auto* ptrTy = llvm::PointerType::getUnqual(*context_);
        auto* cMainTy = llvm::FunctionType::get(i32Ty, {i32Ty, ptrTy}, false);
        fn->eraseFromParent();
        fn = llvm::Function::Create(cMainTy, llvm::Function::ExternalLinkage, "main", *module_);
        fn->arg_begin()->setName("argc");
        (fn->arg_begin() + 1)->setName("argv");
        functions_["main"] = fn;
    }

    for (auto& [attrName, attrValue] : decl.attributes) {
        if (attrName == "inline") {
            fn->addFnAttr(llvm::Attribute::AlwaysInline);
        } else if (attrName == "noinline") {
            fn->addFnAttr(llvm::Attribute::NoInline);
        } else if (attrName == "cold") {
            fn->addFnAttr(llvm::Attribute::Cold);
        } else if (attrName == "hot") {
            fn->addFnAttr(llvm::Attribute::Hot);
        } else if (attrName == "pure") {
            fn->addFnAttr(llvm::Attribute::ReadNone);
        } else if (attrName == "noreturn") {
            fn->addFnAttr(llvm::Attribute::NoReturn);
        } else if (attrName == "tailcall") {
            fn->addFnAttr("tailcall");
        } else if (attrName == "align" && !attrValue.empty()) {
            char* end = nullptr;
            unsigned long alignment = std::strtoul(attrValue.c_str(), &end, 10);
            if (end != attrValue.c_str() && alignment > 0) {
                fn->setAlignment(llvm::Align(alignment));
            }
        }
    }

    auto* entry = llvm::BasicBlock::Create(*context_, "entry", fn);
    builder_->SetInsertPoint(entry);

    if (emitDebug_ && debugBuilder_ && debugFile_) {
        auto* fnDI = debugBuilder_->createFunction(
            debugFile_, decl.name, decl.name, debugFile_,
            decl.location.line,
            debugBuilder_->createSubroutineType(debugBuilder_->getOrCreateTypeArray({})),
            decl.location.line, llvm::DINode::FlagZero,
            llvm::DISubprogram::SPFlagDefinition);
        fn->setSubprogram(fnDI);
        emitDebugLocation(decl.location);
    }

    emitCoverageIncrement(decl.name, 0);

    auto savedValues = namedValues_;
    auto savedContainerTypes = containerTypes_;
    auto savedContainerElemTypes = containerElemTypes_;
    auto savedContainerValTypes = containerValTypes_;
    // Free functions previously inherited stale side-maps (classVarTypes_,
    // interfaceVarTypes_, refInnerTypeNames_, …) from whichever function was
    // emitted just before.  That caused `var result = ""` in main() to
    // resolve as `Vec<i64>` — because an earlier std function (`unique()`
    // in collections.vyx:130) had `var result = Vec::<i64>.new()` and its
    // classVarTypes_["result"] wasn't cleared.  `result.contains("Fizz")`
    // then dispatched to `Vec<i64>.contains` instead of the inline strstr
    // path for strings.
    auto savedClassVarTypes = classVarTypes_;
    auto savedInterfaceVarTypes = interfaceVarTypes_;
    auto savedRefInnerTypeNames = refInnerTypeNames_;
    classVarTypes_.clear();
    interfaceVarTypes_.clear();
    refInnerTypeNames_.clear();
    namedValues_.clear();
    {
        std::map<std::string, std::string> globalCT;
        std::map<std::string, llvm::Type*> globalCET;
        std::map<std::string, llvm::Type*> globalCVT;
        for (auto& [k, v] : containerTypes_)
            if (module_->getGlobalVariable(k, true)) globalCT[k] = v;
        for (auto& [k, v] : containerElemTypes_)
            if (module_->getGlobalVariable(k, true)) globalCET[k] = v;
        for (auto& [k, v] : containerValTypes_)
            if (module_->getGlobalVariable(k, true)) globalCVT[k] = v;
        containerTypes_ = std::move(globalCT);
        containerElemTypes_ = std::move(globalCET);
        containerValTypes_ = std::move(globalCVT);
    }

    if (isNoArgMain) {
        auto* voidTy = llvm::Type::getVoidTy(*context_);
        auto* i32Ty2 = llvm::Type::getInt32Ty(*context_);
        auto* ptrTy2 = llvm::PointerType::getUnqual(*context_);

        auto* argcGlobal = module_->getGlobalVariable("_vyx_argc");
        if (!argcGlobal) {
            argcGlobal = new llvm::GlobalVariable(*module_, i32Ty2, false,
                llvm::GlobalValue::InternalLinkage,
                llvm::ConstantInt::get(i32Ty2, 0), "_vyx_argc");
        }
        auto* argvGlobal = module_->getGlobalVariable("_vyx_argv");
        if (!argvGlobal) {
            argvGlobal = new llvm::GlobalVariable(*module_, ptrTy2, false,
                llvm::GlobalValue::InternalLinkage,
                llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context_)), "_vyx_argv");
        }

        auto* setArgsFn = module_->getFunction("_vyx_set_args_internal");
        if (!setArgsFn) {
            auto* fty = llvm::FunctionType::get(voidTy, {i32Ty2, ptrTy2}, false);
            setArgsFn = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, "_vyx_set_args_internal", *module_);
        }
        if (setArgsFn->empty()) {
            auto* saBB = llvm::BasicBlock::Create(*context_, "entry", setArgsFn);
            llvm::IRBuilder<> saBuilder(saBB);
            saBuilder.CreateStore(&*setArgsFn->arg_begin(), argcGlobal);
            saBuilder.CreateStore(&*(setArgsFn->arg_begin() + 1), argvGlobal);
            saBuilder.CreateRetVoid();
        }
        builder_->CreateCall(setArgsFn, {fn->arg_begin(), fn->arg_begin() + 1});

        {
            auto* gacFn = module_->getFunction("_vyx_get_argc_internal");
            if (!gacFn) {
                auto* gacFty = llvm::FunctionType::get(i32Ty2, {}, false);
                gacFn = llvm::Function::Create(gacFty, llvm::Function::ExternalLinkage, "_vyx_get_argc_internal", *module_);
            }
            if (gacFn->empty()) {
                auto* gacBB = llvm::BasicBlock::Create(*context_, "entry", gacFn);
                llvm::IRBuilder<> gacBuilder(gacBB);
                gacBuilder.CreateRet(gacBuilder.CreateLoad(i32Ty2, argcGlobal, "argc"));
            }
        }
        {
            auto* strTy = getOrCreateStringType();
            auto* i64Ty3 = llvm::Type::getInt64Ty(*context_);
            auto* gaFn = module_->getFunction("_vyx_get_arg_internal");
            if (!gaFn) {
                auto* gaFty = llvm::FunctionType::get(strTy, {i32Ty2}, false);
                gaFn = llvm::Function::Create(gaFty, llvm::Function::ExternalLinkage, "_vyx_get_arg_internal", *module_);
            }
            if (gaFn->empty()) {
                auto* gaBB = llvm::BasicBlock::Create(*context_, "entry", gaFn);
                llvm::IRBuilder<> gaBuilder(gaBB);
                auto* argv = gaBuilder.CreateLoad(ptrTy2, argvGlobal, "argv");
                auto* idx = gaBuilder.CreateSExt(&*gaFn->arg_begin(), i64Ty3, "idx64");
                auto* elemPtr = gaBuilder.CreateGEP(ptrTy2, argv, idx, "arg.ptr");
                auto* cstr = gaBuilder.CreateLoad(ptrTy2, elemPtr, "arg.cstr");
                auto* strlenFn = module_->getFunction("strlen");
                if (!strlenFn) {
                    auto* fty = llvm::FunctionType::get(i64Ty3, {ptrTy2}, false);
                    strlenFn = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, "strlen", *module_);
                }
                auto* slen = gaBuilder.CreateCall(strlenFn, {cstr}, "arg.len");
                auto* fn2 = gaBuilder.GetInsertBlock()->getParent();
                auto* result = createEntryBlockAlloca(fn2, strTy, "arg.str");
                gaBuilder.CreateStore(cstr, gaBuilder.CreateStructGEP(strTy, result, 0));
                gaBuilder.CreateStore(slen, gaBuilder.CreateStructGEP(strTy, result, 1));
                gaBuilder.CreateStore(gaBuilder.CreateAdd(slen, llvm::ConstantInt::get(i64Ty3, 1)),
                    gaBuilder.CreateStructGEP(strTy, result, 2));
                gaBuilder.CreateStore(llvm::ConstantInt::get(i64Ty3, 0),
                    gaBuilder.CreateStructGEP(strTy, result, 3));
                gaBuilder.CreateRet(gaBuilder.CreateLoad(strTy, result, "arg.result"));
            }
        }
    }
    if (isNoArgMain) {
    } else {
        for (auto& arg : fn->args()) {
            auto* alloca = createEntryBlockAlloca(fn, arg.getType(), std::string(arg.getName()));
            builder_->CreateStore(&arg, alloca);
            namedValues_[std::string(arg.getName())] = alloca;
        }
    }

    for (size_t pi = 0; pi < fd.params.size(); ++pi) {
        trackParamType(fd.params[pi]);
    }

    // Fn-typed parameters uniformly arrive as fat pointers (fn+env).
    // Register them in closureFatPtrVars_ so calls to them go through
    // the fat-pointer extract path in CodeGenCall — previously only
    // local let-bound closures were tracked, leaving call sites to
    // fn-typed params (e.g. `fn apply(f: fn(T)->U) { f(x); }`) to
    // call the fat pointer itself as a function pointer, which crashed
    // at runtime because the fat pointer points at a {fn,env} struct,
    // not at machine code.
    for (auto& p : fd.params) {
        if (!p.type) continue;
        if (p.type->kind != TypeAnnotationKind::Function) continue;
        auto& ft = static_cast<const FunctionType&>(*p.type);
        llvm::Type* retTy = llvm::Type::getInt64Ty(*context_);
        if (ft.returnType) {
            auto* infer = toLLVMType(*ft.returnType);
            if (infer && !infer->isVoidTy()) retTy = infer;
        }
        closureFatPtrVars_[p.name] = retTy;
    }

    std::set<std::string> paramNames;
    for (auto& p : fd.params) paramNames.insert(p.name);

    for (auto& ref : fd.refinements) {
        auto it = namedValues_.find(ref.paramName);
        if (it != namedValues_.end()) {
            auto* paramVal = builder_->CreateLoad(getValuePtrType(it->second), it->second, ref.paramName + ".val");
            auto* constVal = llvm::ConstantInt::get(paramVal->getType(), ref.value);
            llvm::Value* cond = nullptr;
            switch (ref.op) {
                case BinaryOp::Neq: cond = builder_->CreateICmpEQ(paramVal, constVal); break;
                case BinaryOp::Eq:  cond = builder_->CreateICmpNE(paramVal, constVal); break;
                case BinaryOp::Gt:  cond = builder_->CreateICmpSLE(paramVal, constVal); break;
                case BinaryOp::Gte: cond = builder_->CreateICmpSLT(paramVal, constVal); break;
                case BinaryOp::Lt:  cond = builder_->CreateICmpSGE(paramVal, constVal); break;
                case BinaryOp::Lte: cond = builder_->CreateICmpSGT(paramVal, constVal); break;
                default: continue;
            }
            auto* panicBB = llvm::BasicBlock::Create(*context_, "refine.fail", fn);
            auto* okBB = llvm::BasicBlock::Create(*context_, "refine.ok", fn);
            builder_->CreateCondBr(cond, panicBB, okBB);
            builder_->SetInsertPoint(panicBB);
            std::string msg = "PANIC: refinement constraint violated — " + ref.paramName +
                " failed constraint\n";
            emitPanicCall(getOrCreateString(msg));
            builder_->SetInsertPoint(okBB);
        }
    }

    // Track interface-typed parameters for vtable dispatch.
    // Generic interfaces (`Container<T>` after Mono substitutes T=i64 →
    // `Container<i64>`) share a single base-name vtable (`Container_vtable`)
    // since interface-method signatures are uniform across instantiations
    // (vtable carries opaque ptr-typed function slots). Fall back to the
    // bare base name when `<full-name>_vtable` doesn't exist.
    for (auto& p : fd.params) {
        if (!p.type) continue;
        if (structTypes_.count(p.type->name + "_vtable")) {
            interfaceVarTypes_[p.name] = p.type->name;
            continue;
        }
        auto lt = p.type->name.find('<');
        if (lt != std::string::npos) {
            std::string base = p.type->name.substr(0, lt);
            if (structTypes_.count(base + "_vtable")) {
                interfaceVarTypes_[p.name] = base;
            }
        }
    }

    // Mono-synthesised enum-method instantiation (e.g. `Option.map<i64>`):
    // the body is `match (self) { case Some(v) => ... }` where self is a
    // `*__self_opaque` pointer to the receiver's `__Result` struct. Set
    // `currentClassName_` to the enum's name (registered as a struct alias
    // pointing at `__Result` in generateRegisterErrorEnums) so the
    // pointer-deref branch in CodeGenMatch.cpp:62-71 triggers and the body
    // can pattern-match the tagged union correctly. Without this, match
    // would treat self as an opaque pointer and emit `f(self_ptr)` instead
    // of unwrapping the payload.
    std::string savedClassName = currentClassName_;
    // When the method-instantiation request supplied a fully-parameterised
    // receiver (syntheticClassReceiver, e.g. "Result<i32,string>"), prefer
    // it over the bare enum name so match/self-deref lookups land on the
    // per-instantiation LLVM struct (`%__Result_32` for wide-payload
    // Result) instead of the shared `%__Result`. The parameterised alias
    // is registered by CodeGenTypeMap.cpp alongside the base alias that
    // generateRegisterErrorEnums installs.
    if (!fd.syntheticClassReceiver.empty()) {
        currentClassName_ = fd.syntheticClassReceiver;
    } else if (!fd.syntheticEnumReceiver.empty()) {
        currentClassName_ = fd.syntheticEnumReceiver;
    }
    struct ClassNameGuard {
        std::string& cur;
        std::string saved;
        ~ClassNameGuard() { cur = std::move(saved); }
    } classNameGuard{currentClassName_, std::move(savedClassName)};

    std::function<void(Stmt&)> convertLastExprToReturn = [&](Stmt& block) {
        if (block.kind != StmtKind::Block) return;
        auto& stmts = static_cast<BlockStmt&>(block).statements;
        if (stmts.empty()) return;
        auto& lastStmt = stmts.back();
        if (!lastStmt) return;

        if (lastStmt->kind == StmtKind::ExprStmt && lastStmt->as<ExprStmt>()->expr) {
            lastStmt->kind = StmtKind::Return;
            return;
        }

        if (lastStmt->kind == StmtKind::If) {
            auto* ifS = lastStmt->as<IfStmt>();
            if (ifS->thenBranch) convertLastExprToReturn(*ifS->thenBranch);
            for (auto& [cond, body] : ifS->elifBranches) {
                if (body) convertLastExprToReturn(*body);
            }
            if (ifS->elseBranch) convertLastExprToReturn(*ifS->elseBranch);
            return;
        }

        if (lastStmt->kind == StmtKind::Block) {
            convertLastExprToReturn(*lastStmt);
            return;
        }

        if (lastStmt->kind == StmtKind::Match) {
            auto& matchArms = lastStmt->as<MatchStmt>()->arms;
            if (!matchArms.empty()) {
                for (auto& arm : matchArms) {
                    if (arm.body) convertLastExprToReturn(*arm.body);
                }
            }
        }
    };

    bool hasReturnType = !fn->getReturnType()->isVoidTy();
    if (hasReturnType) convertLastExprToReturn(*fd.body);

    bool borrowsArgs = false;
    for (auto& [an, _av] : decl.attributes) {
        if (an == "borrow_args") {
            borrowsArgs = true;
            break;
        }
    }

    // Emit block body, tracking last expression value for implicit return.
    // Push a defer scope so that `defer` statements in the function body are
    // collected here and emitted (LIFO) before every exit path.
    deferStack_.push_back({});
    // P4-D follow-up (limit #5): publish the function body's auto-drop list
    // so an explicit `return` deep inside nested blocks can drop function-
    // scope heap-owning locals before the function ret. Without this push,
    // emitReturnStmt's `emitEnclosingBlockDrops()` walks an empty stack —
    // the function body's autoDropLocals are otherwise only consulted at
    // implicit fall-through (below), missing every early-return path.
    blockAutoDropStack_.push_back(&fd.body->as<BlockStmt>()->autoDropLocals);
    // Mirror push for ADT-payload tracking (limit #2).
    adtBlockStack_.push_back(&fd.body->as<BlockStmt>()->autoDropAdtPayloads);

    // P4-D follow-up (limit #5): heap-owning by-value parameters take
    // ownership at call boundary (move-by-value). The callee must drop
    // them on every exit path. Sema's `forEachLocalSymbol` only sees
    // body-block scope and skips fn-decl-scope params, so collect them
    // here and stack-publish so emitReturnStmt walks them too. Skip:
    //   * `self` (class method receiver — caller retains ownership);
    //   * reference / borrow-typed params (non-owning);
    //   * fn-typed params (closures, drop is the caller's job).
    std::vector<std::string> paramAutoDropLocals;
    for (auto& p : fd.params) {
        if (borrowsArgs) break;
        if (p.name.empty() || p.name == "self") continue;
        if (!p.type) continue;
        // Reference / borrow types are non-owning.
        if (p.type->kind == TypeAnnotationKind::Reference) continue;
        // Function-typed params are not heap-owning at the call boundary.
        if (p.type->kind == TypeAnnotationKind::Function) continue;
        // Resolve the param's struct type via namedValues_ alloca; if it
        // matches a heap-owning class (drop method registered) we own it.
        auto nvIt = namedValues_.find(p.name);
        if (nvIt == namedValues_.end()) continue;
        auto* allocaTy = getValuePtrType(nvIt->second);
        auto* structTy = allocaTy ? llvm::dyn_cast<llvm::StructType>(allocaTy) : nullptr;
        if (!structTy || !structTy->hasName()) continue;
        std::string typeName = structTy->getName().str();
        bool hasDrop = functions_.count(typeName + ".drop") > 0;
        // __RefCounted is the shared LLVM struct under every Ref<T>; any
        // Ref<*>.drop in the function table works (mirrors emitDropForLocal).
        if (!hasDrop && typeName == "__RefCounted") {
            for (auto& [fn_name, _] : functions_) {
                if (fn_name.size() > 4 && fn_name.compare(0, 4, "Ref<") == 0 &&
                    fn_name.size() > 6 &&
                    fn_name.compare(fn_name.size() - 6, 6, ">.drop") == 0) {
                    hasDrop = true;
                    break;
                }
            }
        }
        if (hasDrop) paramAutoDropLocals.push_back(p.name);
    }
    blockAutoDropStack_.push_back(&paramAutoDropLocals);
    llvm::Value* lastExprVal = nullptr;
    for (auto& s : fd.body->as<BlockStmt>()->statements) {
        if (!s) continue;
        if (builder_->GetInsertBlock() && builder_->GetInsertBlock()->getTerminator())
            break;

        if (s->kind == StmtKind::Defer) {
            // Collect deferred statement; it will be run at scope exit.
            if (s->as<DeferStmt>()->body) deferStack_.back().push_back(s->as<DeferStmt>()->body.get());
            continue;
        }
        if (s->kind == StmtKind::ExprStmt && s->as<ExprStmt>()->expr) {
            lastExprVal = emitExpr(*s->as<ExprStmt>()->expr);
        } else {
            emitStmt(*s);
            lastExprVal = nullptr;
        }
    }

    // P4-C scope-aware auto-drop at function body exit. The function body
    // IS a BlockStmt whose Sema pass populated `autoDropLocals` with heap-
    // owning locals that are neither moved nor escaped. Emit `.drop()`
    // for each on implicit fall-through; explicit `return` paths are
    // handled at the `return` site by emitReturnStmt walking
    // blockAutoDropStack_. Also drop heap-owning by-value params (limit
    // #5 follow-up) since they were moved into the callee at call time.
    auto* insertBB = builder_->GetInsertBlock();
    if (insertBB && !insertBB->getTerminator()) {
        auto* bodyBlock = fd.body->as<BlockStmt>();
        for (auto& localName : bodyBlock->autoDropLocals) {
            if (paramNames.count(localName) || localName == "self") continue;
            emitDropForLocal(localName);
        }
        for (auto& paramName : paramAutoDropLocals) {
            emitDropForLocal(paramName);
        }
        // P4-D follow-up (limit #2): ADT-payload drops on natural fall-through.
        for (auto& adt : bodyBlock->autoDropAdtPayloads) {
            emitAdtPayloadDrop(adt.localName, adt.innerHeapTypeName, adt.variantTag);
        }
    }

    auto* lastBB = builder_->GetInsertBlock();
    if (lastBB && !lastBB->getTerminator()) {
        // Emit deferred statements (LIFO) before implicit fall-through return.
        // (Explicit `return` statements already call emitDeferredStmts via emitReturnStmt.)
        emitDeferredStmts(deferStack_.size() - 1);
        auto* retTy = fn->getReturnType();
        if (retTy->isVoidTy()) {
            builder_->CreateRetVoid();
        } else if (lastExprVal) {
            // Implicit Ok wrapping for Result return types
            if (auto* stTy = llvm::dyn_cast<llvm::StructType>(retTy)) {
                if (stTy->getName() == "__Result" && lastExprVal->getType() != stTy) {
                    auto* alloca = createEntryBlockAlloca(fn, stTy, "result.ok");
                    auto* tagPtr = builder_->CreateStructGEP(stTy, alloca, 0);
                    builder_->CreateStore(llvm::ConstantInt::get(stTy->getElementType(0), 0), tagPtr);
                    auto* valPtr = builder_->CreateStructGEP(stTy, alloca, 1);
                    llvm::Value* stored = lastExprVal;
                    auto* i64Ty2 = llvm::Type::getInt64Ty(*context_);
                    if (stored->getType()->isIntegerTy() && stored->getType()->getIntegerBitWidth() < 64)
                        stored = numericCast(stored, i64Ty2);
                    else if (stored->getType()->isFloatingPointTy())
                        stored = builder_->CreateBitCast(
                            builder_->CreateFPExt(stored, llvm::Type::getDoubleTy(*context_)), i64Ty2);
                    else if (stored->getType()->isPointerTy())
                        stored = builder_->CreatePtrToInt(stored, i64Ty2);
                    else if (stored->getType()->isStructTy()) {
                        auto* mem = builder_->CreateCall(module_->getFunction("malloc"),
                            {llvm::ConstantInt::get(i64Ty2,
                                module_->getDataLayout().getTypeAllocSize(stored->getType()))}, "ok.box");
                        builder_->CreateStore(stored, mem);
                        stored = builder_->CreatePtrToInt(mem, i64Ty2);
                    }
                    builder_->CreateStore(stored, valPtr);
                    builder_->CreateRet(builder_->CreateLoad(stTy, alloca));
                } else {
                    builder_->CreateRet(lastExprVal);
                }
            } else {
                builder_->CreateRet(castToType(lastExprVal, retTy));
            }
        } else {
            builder_->CreateRet(llvm::Constant::getNullValue(retTy));
        }
    }

    // Pop the function-level defer scope pushed before the body loop.
    if (!deferStack_.empty()) deferStack_.pop_back();
    // P4-D follow-up (limit #5): pop the param + body auto-drop entries
    // pushed before the body loop. Defensive: only pop if our top is
    // still ours (emitBlock balances nested pushes/pops, but defer-body
    // re-emission could have introduced spurious entries).
    if (!blockAutoDropStack_.empty() &&
        blockAutoDropStack_.back() == &paramAutoDropLocals) {
        blockAutoDropStack_.pop_back();
    }
    if (!blockAutoDropStack_.empty() &&
        blockAutoDropStack_.back() == &fd.body->as<BlockStmt>()->autoDropLocals) {
        blockAutoDropStack_.pop_back();
    }
    if (!adtBlockStack_.empty() &&
        adtBlockStack_.back() == &fd.body->as<BlockStmt>()->autoDropAdtPayloads) {
        adtBlockStack_.pop_back();
    }

    if (llvm::verifyFunction(*fn, &llvm::errs())) {
        diag_.error(CodeGen::codegenInternalSourceLocation(), "verification failed for function '{}'", fn->getName().str());
    }
    namedValues_ = std::move(savedValues);
    for (auto& [k, v] : containerTypes_) {
        if (!savedContainerTypes.count(k) && module_->getGlobalVariable(k, true))
            savedContainerTypes[k] = v;
    }
    for (auto& [k, v] : containerElemTypes_) {
        if (!savedContainerElemTypes.count(k) && module_->getGlobalVariable(k, true))
            savedContainerElemTypes[k] = v;
    }
    for (auto& [k, v] : containerValTypes_) {
        if (!savedContainerValTypes.count(k) && module_->getGlobalVariable(k, true))
            savedContainerValTypes[k] = v;
    }
    containerTypes_ = std::move(savedContainerTypes);
    containerElemTypes_ = std::move(savedContainerElemTypes);
    containerValTypes_ = std::move(savedContainerValTypes);
    classVarTypes_ = std::move(savedClassVarTypes);
    interfaceVarTypes_ = std::move(savedInterfaceVarTypes);
    refInnerTypeNames_ = std::move(savedRefInnerTypeNames);
}

} // namespace vyx
