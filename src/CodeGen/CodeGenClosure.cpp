#include "CodeGenIncludes.h"
#include <set>

namespace vyx {

// Returns the named struct { ptr fn_ptr, ptr env_ptr } used as a fat closure pointer.
llvm::StructType* CodeGen::getOrCreateClosureFatPtrType() {
    const std::string name = "__closure_fat_ptr";
    auto it = structTypes_.find(name);
    if (it != structTypes_.end()) return it->second;
    auto* ptrTy = llvm::PointerType::getUnqual(*context_);
    auto* st = llvm::StructType::create(*context_, {ptrTy, ptrTy}, name);
    structTypes_[name] = st;
    return st;
}

// Synthesise (or reuse) a per-fn trampoline that prepends a discarded
// env_ptr slot, then wraps it into a heap { trampoline, null } fat pointer.
// The returned pointer has the same lifetime shape as capturing closures
// emitted below, so fn values can be stored in containers without carrying
// a dangling stack slot.
llvm::Value* CodeGen::wrapRawFnAsFatPtr(llvm::Function* rawFn) {
    if (!rawFn) return nullptr;
    std::string trampName = "__fn_fatptr_trampoline_" + rawFn->getName().str();
    auto* trampFn = module_->getFunction(trampName);
    if (!trampFn) {
        auto* rawFnTy = rawFn->getFunctionType();
        std::vector<llvm::Type*> trParamTys;
        trParamTys.push_back(llvm::PointerType::getUnqual(*context_));
        for (unsigned pi = 0; pi < rawFnTy->getNumParams(); ++pi)
            trParamTys.push_back(rawFnTy->getParamType(pi));
        auto* trFnTy = llvm::FunctionType::get(
            rawFnTy->getReturnType(), trParamTys, false);
        trampFn = llvm::Function::Create(
            trFnTy, llvm::Function::InternalLinkage,
            trampName, *module_);
        auto* entry = llvm::BasicBlock::Create(
            *context_, "entry", trampFn);
        llvm::IRBuilder<> tb(entry);
        std::vector<llvm::Value*> fwd;
        auto it = trampFn->arg_begin();
        ++it; // skip env slot
        for (; it != trampFn->arg_end(); ++it)
            fwd.push_back(&*it);
        if (rawFnTy->getReturnType()->isVoidTy()) {
            tb.CreateCall(rawFn, fwd);
            tb.CreateRetVoid();
        } else {
            auto* rv = tb.CreateCall(rawFn, fwd);
            tb.CreateRet(rv);
        }
    }
    auto* fatTy = getOrCreateClosureFatPtrType();
    auto* ptrTy = llvm::PointerType::getUnqual(*context_);
    auto* i64Ty = llvm::Type::getInt64Ty(*context_);
    auto* mallocFn = module_->getFunction("malloc");
    if (!mallocFn) {
        auto* mallocTy = llvm::FunctionType::get(ptrTy, {i64Ty}, false);
        mallocFn = llvm::Function::Create(
            mallocTy, llvm::Function::ExternalLinkage, "malloc", *module_);
    }
    uint64_t fatSize = module_->getDataLayout().getTypeAllocSize(fatTy);
    auto* fatMem = builder_->CreateCall(mallocFn,
        {llvm::ConstantInt::get(i64Ty, fatSize)}, "fn.fat.alloc");
    builder_->CreateStore(trampFn,
        builder_->CreateStructGEP(fatTy, fatMem, 0, "fn.fat.fnptr"));
    builder_->CreateStore(
        llvm::ConstantPointerNull::get(ptrTy),
        builder_->CreateStructGEP(fatTy, fatMem, 1, "fn.fat.envptr"));
    return fatMem;
}

llvm::Value* CodeGen::emitClosure(const Expr& expr) {
    auto* ce = expr.as<const ClosureExpr>();
    std::string closureName = "__closure_" + std::to_string(closureCounter_++);
    llvm::Type* retTy = llvm::Type::getInt32Ty(*context_);
    // First preference: explicit `-> T` annotation on the closure.
    if (ce->returnType) {
        if (ce->returnType->name != "unknown" && ce->returnType->name != "Unknown") {
            auto* inferred = toLLVMType(*ce->returnType);
            if (inferred && !inferred->isVoidTy()) retTy = inferred;
        }
    }
    // Second preference: Sema's inferred function type (written back into
    // `closureExpr->inferredType` at analysis time). Without this, a
    // bare `|| "hello"` would default retTy to i32 and truncate the
    // 32-byte %__String return struct to the low i32 of its ptr field,
    // leaving print() to read the garbage pointer value.
    if (retTy->isIntegerTy(32) && !ce->returnType && ce->inferredType &&
        ce->inferredType->kind == VyxTypeKind::Function &&
        ce->inferredType->returnType) {
        auto* inferred = toLLVMType(*ce->inferredType->returnType);
        if (inferred && !inferred->isVoidTy()) retTy = inferred;
    }

    auto* savedBlock = builder_->GetInsertBlock();
    auto savedPoint = builder_->GetInsertPoint();
    auto savedValues = namedValues_;
    auto savedDeferStack = deferStack_;
    auto savedBlockAutoDropStack = blockAutoDropStack_;
    auto savedAdtBlockStack = adtBlockStack_;
    auto savedLoopStack = loopStack_;

    struct CapturedVal { std::string name; llvm::Value* value; llvm::Type* type; bool byRef; };
    std::vector<CapturedVal> capturedValues;
    for (auto& cap : ce->captures) {
        // BUG-LV-09 fix: `[name = expr]` form introduces a fresh binding
        // whose value is computed by emitting `cap.moveExpr` in the OUTER
        // scope. The legacy code only consulted `savedValues.find(cap.name)`
        // and silently dropped the capture when the name didn't pre-exist,
        // leaving the closure body with an "undefined variable" reference.
        if (cap.moveExpr) {
            auto* val = emitExpr(*cap.moveExpr);
            if (val) {
                capturedValues.push_back({cap.name, val, val->getType(), false});
            }
            continue;
        }
        auto it = savedValues.find(cap.name);
        if (it != savedValues.end()) {
            auto* capTy = getValuePtrType(it->second);
            if (cap.byRef) {
                capturedValues.push_back({cap.name, it->second, llvm::PointerType::getUnqual(*context_), true});
            } else {
                auto* val = builder_->CreateLoad(capTy, it->second, cap.name + ".cap");
                capturedValues.push_back({cap.name, val, capTy, false});
            }
        }
    }

    // Auto-capture: when no explicit captures are listed (lambda syntax |params| body),
    // capture all outer scope variables so they're accessible inside the closure.
    if (ce->captures.empty() && !savedValues.empty()) {
        std::set<std::string> paramNames;
        for (auto& p : ce->params) paramNames.insert(p.name);

        for (auto& [name, alloca] : savedValues) {
            if (paramNames.count(name)) continue;
            if (functions_.count(name)) continue;
            auto* capTy = getValuePtrType(alloca);
            if (!capTy) continue;
            auto* val = builder_->CreateLoad(capTy, alloca, name + ".cap");
            capturedValues.push_back({name, val, capTy, false});
        }
    }

    // --- Determine whether we need a heap env (any captures exist) ---
    bool needsEnv = !capturedValues.empty();

    // Build the closure inner function.
    // If captures exist the first parameter is an opaque env_ptr (ptr).
    std::vector<llvm::Type*> paramTypes;
    auto* ptrTy = llvm::PointerType::getUnqual(*context_);
    if (needsEnv) paramTypes.push_back(ptrTy); // env_ptr as first param
    for (auto& p : ce->params) {
        paramTypes.push_back(p.type ? toLLVMType(*p.type) : llvm::Type::getInt32Ty(*context_));
    }

    auto* fnType = llvm::FunctionType::get(retTy, paramTypes, false);
    auto* closureFn = llvm::Function::Create(fnType, llvm::Function::InternalLinkage, closureName, *module_);

    // Name the env param (if present) and the user-visible params.
    {
        size_t argIdx = 0;
        for (auto& arg : closureFn->args()) {
            if (needsEnv && argIdx == 0) {
                arg.setName("env");
            } else {
                size_t pIdx = needsEnv ? argIdx - 1 : argIdx;
                if (pIdx < ce->params.size())
                    arg.setName(ce->params[pIdx].name);
            }
            ++argIdx;
        }
    }

    // Build env struct type: { cap0_type, cap1_type, ... }
    llvm::StructType* envStructTy = nullptr;
    if (needsEnv) {
        std::vector<llvm::Type*> envFieldTypes;
        for (auto& cv : capturedValues) envFieldTypes.push_back(cv.type);
        envStructTy = llvm::StructType::create(*context_, envFieldTypes, closureName + ".env");
    }

    // --- Emit closure inner function body ---
    auto* entry = llvm::BasicBlock::Create(*context_, "entry", closureFn);
    builder_->SetInsertPoint(entry);
    namedValues_.clear();
    deferStack_.clear();
    blockAutoDropStack_.clear();
    adtBlockStack_.clear();
    loopStack_.clear();

    // Unpack captured values from env struct (via the env_ptr first param).
    // namedValues_[cv.name] points directly at the env field so reads and
    // writes inside the closure body hit the shared env storage; without
    // this, writes landed on a throwaway local alloca and every invocation
    // started from the captured snapshot (FnMut semantics were broken).
    //
    // BUG-LV-08: byRef captures additionally need writeback. The env field
    // for a byRef capture stores a pointer to the OUTER alloca; loads/stores
    // inside the closure body, however, target a local mirror alloca so
    // call sites that GEP the captured slot continue to see a typed alloca.
    // To honour the `[&name]` semantics ("writes propagate to the outer"),
    // we collect (outerPtr, localAlloca, capTy) tuples and, after the body
    // is fully emitted, walk every ret terminator and insert a `store` of
    // the local mirror's current value back to the outer pointer just
    // before the ret. The mirror is kept up to date with the latest writes
    // because all closure-body stores still target the localAlloca.
    struct ByRefWriteback {
        llvm::Value* outerPtr;       // ptr (loaded from env field at entry)
        llvm::AllocaInst* localAlloca; // captured-type alloca holding the mirror
        llvm::Type* capTy;            // element type of localAlloca
        std::string name;
    };
    std::vector<ByRefWriteback> byRefWritebacks;

    if (needsEnv && envStructTy) {
        auto* envPtrArg = closureFn->arg_begin(); // first arg = env_ptr
        for (size_t i = 0; i < capturedValues.size(); ++i) {
            auto& cv = capturedValues[i];
            auto* fieldPtr = builder_->CreateStructGEP(envStructTy, envPtrArg,
                (unsigned)i, cv.name + ".envptr");
            if (cv.byRef) {
                // byRef capture: the env field holds a pointer to the
                // original outer alloca. Load the captured pointer once
                // and stash it in a ptr-typed entry alloca so that
                // getValuePtrType returns `ptr` for tools that poke at
                // the alloca directly. Loads/stores of cv.name still need
                // the actual element type (cv.type); call sites carry
                // that themselves.
                auto* ptrAlloca = createEntryBlockAlloca(closureFn, ptrTy, cv.name + ".ref");
                auto* outerPtr = builder_->CreateLoad(ptrTy, fieldPtr, cv.name + ".ptr");
                builder_->CreateStore(outerPtr, ptrAlloca);
                // Mirror the outer value into a local alloca so call sites
                // expecting a typed alloca continue to work. Writeback to
                // outerPtr is registered below and emitted before each ret.
                auto* capTy = cv.type ? cv.type : llvm::Type::getInt64Ty(*context_);
                auto* localAlloca = createEntryBlockAlloca(closureFn, capTy, cv.name);
                auto* val = builder_->CreateLoad(capTy, outerPtr, cv.name + ".deref");
                builder_->CreateStore(val, localAlloca);
                namedValues_[cv.name] = localAlloca;
                byRefWritebacks.push_back({outerPtr, localAlloca, capTy, cv.name});
            } else {
                namedValues_[cv.name] = fieldPtr;
            }
        }
    }

    // Allocas for user-visible parameters (skip env_ptr at idx 0).
    {
        size_t argIdx = 0;
        for (auto& arg : closureFn->args()) {
            if (needsEnv && argIdx == 0) { ++argIdx; continue; } // skip env_ptr
            auto* alloca = createEntryBlockAlloca(closureFn, arg.getType(), std::string(arg.getName()));
            builder_->CreateStore(&arg, alloca);
            namedValues_[std::string(arg.getName())] = alloca;
            ++argIdx;
        }
    }

    for (size_t ci = 0; ci < ce->params.size(); ++ci) {
        if (!ce->params[ci].type) continue;
        auto& cpType = *ce->params[ci].type;
        if ((cpType.name == "Ref" || cpType.name == "Scope") &&
            cpType.kind == TypeAnnotationKind::Generic) {
            auto& refSubs = static_cast<const GenericType&>(cpType).typeArgs;
            if (!refSubs.empty() && refSubs[0])
                refInnerTypeNames_[ce->params[ci].name] =
                    mangleTypeAnnotationNested(*refSubs[0]);
        }
        auto stIt = structTypes_.find(cpType.name);
        if (stIt != structTypes_.end()) {
            if (auto* stTy = llvm::dyn_cast<llvm::StructType>(stIt->second)) {
                if (stTy->hasName())
                    classVarTypes_[ce->params[ci].name] = stTy->getName().str();
            }
        }
    }

    llvm::Value* closureResult = nullptr;
    if (ce->singleExpr) {
        closureResult = emitExpr(*ce->singleExpr);
    } else if (ce->body) {
        emitBlock(*ce->body);
    }

    auto* lastBB = builder_->GetInsertBlock();
    if (lastBB && !lastBB->getTerminator()) {
        if (closureResult && closureResult->getType() == retTy)
            builder_->CreateRet(closureResult);
        else if (closureResult)
            builder_->CreateRet(castToType(closureResult, retTy));
        else
            builder_->CreateRet(llvm::Constant::getNullValue(retTy));
    }

    // BUG-LV-08: emit byRef writebacks before every ret in the closure body.
    // outerPtr and localAlloca are both materialised in the closure entry
    // block and therefore dominate every ret terminator below.
    if (!byRefWritebacks.empty()) {
        for (auto& bb : *closureFn) {
            auto* term = bb.getTerminator();
            auto* ret = llvm::dyn_cast_or_null<llvm::ReturnInst>(term);
            if (!ret) continue;
            llvm::IRBuilder<> wb(ret);
            for (auto& w : byRefWritebacks) {
                auto* curVal = wb.CreateLoad(w.capTy, w.localAlloca, w.name + ".wb.val");
                wb.CreateStore(curVal, w.outerPtr);
            }
        }
    }

    // --- Restore caller's insert point ---
    namedValues_ = std::move(savedValues);
    deferStack_ = std::move(savedDeferStack);
    blockAutoDropStack_ = std::move(savedBlockAutoDropStack);
    adtBlockStack_ = std::move(savedAdtBlockStack);
    loopStack_ = std::move(savedLoopStack);
    builder_->SetInsertPoint(savedBlock, savedPoint);
    functions_[closureName] = closureFn;

    if (!needsEnv) {
        // No captures: return bare function pointer (existing behaviour).
        return closureFn;
    }

    // --- Build heap env struct and populate captured values (in caller) ---
    auto* i64Ty = llvm::Type::getInt64Ty(*context_);
    auto* mallocFn = module_->getFunction("malloc");
    if (!mallocFn) {
        auto* mallocTy = llvm::FunctionType::get(ptrTy, {i64Ty}, false);
        mallocFn = llvm::Function::Create(mallocTy, llvm::Function::ExternalLinkage, "malloc", *module_);
    }

    uint64_t envSize = module_->getDataLayout().getTypeAllocSize(envStructTy);
    auto* envMem = builder_->CreateCall(mallocFn,
        {llvm::ConstantInt::get(i64Ty, envSize)}, closureName + ".env.alloc");
    // Store each captured value into the env struct.
    for (size_t i = 0; i < capturedValues.size(); ++i) {
        auto& cv = capturedValues[i];
        auto* fieldPtr = builder_->CreateStructGEP(envStructTy, envMem,
            (unsigned)i, cv.name + ".envfield");
        builder_->CreateStore(cv.value, fieldPtr);
    }

    // --- Build fat pointer struct { fn_ptr, env_ptr } on heap ---
    auto* fatPtrTy = getOrCreateClosureFatPtrType();
    uint64_t fatSize = module_->getDataLayout().getTypeAllocSize(fatPtrTy);
    auto* fatMem = builder_->CreateCall(mallocFn,
        {llvm::ConstantInt::get(i64Ty, fatSize)}, closureName + ".fat.alloc");
    // Store fn_ptr (element 0)
    auto* fnPtrField = builder_->CreateStructGEP(fatPtrTy, fatMem, 0, closureName + ".fat.fn");
    builder_->CreateStore(closureFn, fnPtrField);
    // Store env_ptr (element 1)
    auto* envPtrField = builder_->CreateStructGEP(fatPtrTy, fatMem, 1, closureName + ".fat.env");
    builder_->CreateStore(envMem, envPtrField);

    // Return the heap pointer to the fat struct.
    // The pointer type is compatible with the fn(...)->... return type (both ptr in LLVM).
    // emitVarDecl marks the receiving variable in closureFatPtrVars_ by inspecting
    // either the initExpr kind (direct closure) or the callee's return type annotation
    // (higher-order function returning a closure).
    (void)fatPtrTy; // used above for struct GEPs
    return fatMem;
}

llvm::Value* CodeGen::emitFailExpr(const Expr& expr) {
    auto* fe = expr.as<const FailExpr>();
    std::string key = fe->typeName + "." + fe->variant;
    auto it = errorEnumValues_.find(key);
    int errorCode = (it != errorEnumValues_.end()) ? it->second : -1;

    // BUG-LV-12: only the `fail E.V.with("msg")` debug-log syntax should
    // emit a printf here; the `fail E.V(payload)` syntax (isPayload=true)
    // carries the variant's actual payload field and must NOT be
    // pretty-printed (extractStringPtr would mis-cast a non-string payload
    // and crash). The payload is laid into the result struct further below.
    if (fe->message && !fe->isPayload) {
        auto* msgVal = emitExpr(*fe->message);
        if (msgVal) {
            auto* printfFn = getOrCreatePrintf();
            auto* fmt = getOrCreateString("error: %s.%s: %s\n");
            auto* typeStr = getOrCreateString(fe->typeName);
            auto* varStr = getOrCreateString(fe->variant);
            auto* msgPtr = extractStringPtr(msgVal);
            builder_->CreateCall(printfFn, {fmt, typeStr, varStr, msgPtr});
        }
    }

    auto* fn = builder_->GetInsertBlock()->getParent();
    auto* retTy = fn->getReturnType();

    // BUG-LV-07: `fail` is the fourth drop-injection point. Without
    // replaying scope-exit drops here, every owning local in the
    // function's open block scopes — Vec/Dict/String buffers, Ref<T>
    // refcount cells, user-class `fn drop()` payloads — is leaked on
    // the unwind path (e.g. `let local = probe.clone(); fail X.Y;`
    // would leave `probe`'s rc bumped but never decremented). Mirror
    // `emitReturnStmt`: flush defers, then walk the auto-drop stacks
    // BEFORE constructing the Result-shaped or scalar return.
    auto emitFailUnwindCleanup = [this]() {
        emitDeferredStmts(0);
        emitScopeExitDrops();
    };

    if (auto* stTy = llvm::dyn_cast<llvm::StructType>(retTy)) {
        bool isResult = stTy->hasName() && isResultSlotName(stTy->getName());
        if (isResult) {
            auto* alloca = createEntryBlockAlloca(fn, stTy, "result.fail");
            auto* tagPtr = builder_->CreateStructGEP(stTy, alloca, 0, "fail.tag");
            builder_->CreateStore(llvm::ConstantInt::get(stTy->getElementType(0), 1), tagPtr);
            auto* valPtr = builder_->CreateStructGEP(stTy, alloca, 1, "fail.val");
            auto* valFieldTy = stTy->getElementType(1);

            // BUG-LV-12: for `fail E.V(payload)`, materialise the inner
            // ADT instance ({ adtTag, adtData }) WITH its payload bytes
            // populated, then copy the whole ADT struct into the Result's
            // val field. This mirrors the IR shape produced by
            // `return Err(E.V(payload))` and is what `case V(p)` arms
            // expect. Without this, the val field's data area is undef
            // and any payload destructuring dereferences a NULL/garbage
            // pointer (the abort observed in BUG-LV-12).
            bool laidOutPayload = false;
            if (fe->isPayload && fe->message && valFieldTy->isArrayTy()) {
                auto adtIt = structTypes_.find(fe->typeName);
                if (adtIt != structTypes_.end()) {
                    auto* adtTy = adtIt->second;
                    auto* tmpAdt = createEntryBlockAlloca(fn, adtTy, "fail.adt.tmp");
                    auto* adtTagPtr = builder_->CreateStructGEP(adtTy, tmpAdt, 0, "fail.adt.tag");
                    builder_->CreateStore(
                        llvm::ConstantInt::get(adtTy->getElementType(0), errorCode),
                        adtTagPtr);

                    auto* payloadVal = emitExpr(*fe->message);
                    if (payloadVal) {
                        auto* adtDataPtr = builder_->CreateStructGEP(adtTy, tmpAdt, 1, "fail.adt.data");
                        auto& dl = module_->getDataLayout();
                        if (auto* payStTy = llvm::dyn_cast<llvm::StructType>(payloadVal->getType())) {
                            // Struct payload (string/Vec/etc.): per-field
                            // copy at compact offsets, matching how
                            // `Err(E.V(...))` lowers via emitBuiltinContainer.
                            auto* tmpPay = createEntryBlockAlloca(fn, payStTy, "fail.pay.tmp");
                            builder_->CreateStore(payloadVal, tmpPay);
                            auto* i8Ty = llvm::Type::getInt8Ty(*context_);
                            auto* i64Ty = llvm::Type::getInt64Ty(*context_);
                            uint64_t off = 0;
                            for (unsigned fi = 0; fi < payStTy->getNumElements(); ++fi) {
                                auto* fieldTy = payStTy->getElementType(fi);
                                uint64_t fieldSz = dl.getTypeStoreSize(fieldTy);
                                auto* srcP = builder_->CreateStructGEP(payStTy, tmpPay, fi, "fail.pay.src");
                                auto* fv = builder_->CreateLoad(fieldTy, srcP, "fail.pay.src.val");
                                auto* dstP = builder_->CreateGEP(
                                    i8Ty, adtDataPtr,
                                    llvm::ConstantInt::get(i64Ty, off), "fail.pay.dst");
                                builder_->CreateStore(fv, dstP);
                                off += fieldSz;
                            }
                        } else {
                            uint64_t fieldAlign = dl.getABITypeAlign(payloadVal->getType()).value();
                            auto* si = builder_->CreateStore(payloadVal, adtDataPtr);
                            si->setAlignment(llvm::Align(fieldAlign));
                        }
                    }

                    // Copy the populated ADT struct into the Result val
                    // (compact byte layout, mirroring Err(adt) flow).
                    auto* i8Ty = llvm::Type::getInt8Ty(*context_);
                    auto* i64Ty = llvm::Type::getInt64Ty(*context_);
                    uint64_t off = 0;
                    auto& dl = module_->getDataLayout();
                    for (unsigned fi = 0; fi < adtTy->getNumElements(); ++fi) {
                        auto* fieldTy = adtTy->getElementType(fi);
                        uint64_t fieldSz = dl.getTypeStoreSize(fieldTy);
                        auto* srcP = builder_->CreateStructGEP(adtTy, tmpAdt, fi, "fail.adt.src");
                        auto* fv = builder_->CreateLoad(fieldTy, srcP, "fail.adt.src.val");
                        auto* dstP = builder_->CreateGEP(
                            i8Ty, valPtr,
                            llvm::ConstantInt::get(i64Ty, off), "fail.adt.dst");
                        builder_->CreateStore(fv, dstP);
                        off += fieldSz;
                    }
                    laidOutPayload = true;
                }
            }

            if (!laidOutPayload) {
                if (valFieldTy->isArrayTy()) {
                    auto* errVal = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), errorCode, true);
                    builder_->CreateStore(errVal, valPtr);
                } else if (valFieldTy->isIntegerTy(64)) {
                    auto* errVal = llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), errorCode, true);
                    builder_->CreateStore(errVal, valPtr);
                } else {
                    builder_->CreateStore(llvm::Constant::getNullValue(valFieldTy), valPtr);
                }
            }
            auto* result = builder_->CreateLoad(stTy, alloca, "result.fail.val");
            // Drop owning locals on the unwind path BEFORE the ret.
            // Done after the result alloca is materialised so the
            // result struct itself isn't accidentally part of the
            // walked locals (it lives in entry-block scope, not in
            // any user block's autoDropLocals list).
            emitFailUnwindCleanup();
            builder_->CreateRet(result);
            auto* afterBB = llvm::BasicBlock::Create(*context_, "after.fail", fn);
            builder_->SetInsertPoint(afterBB);
            return result;
        }
    }

    auto* errResult = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), errorCode, true);
    if (!retTy->isVoidTy()) {
        emitFailUnwindCleanup();
        builder_->CreateRet(castToType(errResult, retTy));
        auto* afterBB = llvm::BasicBlock::Create(*context_, "after.fail", fn);
        builder_->SetInsertPoint(afterBB);
    } else {
        // Even when the surrounding function is void-returning, the
        // unwind cleanup still has to run so owning locals don't leak
        // through the (terminator-less) fall-through. Emit the cleanup
        // and a RetVoid so the basic block is properly terminated.
        emitFailUnwindCleanup();
        builder_->CreateRetVoid();
        auto* afterBB = llvm::BasicBlock::Create(*context_, "after.fail", fn);
        builder_->SetInsertPoint(afterBB);
    }
    return errResult;
}

llvm::Value* CodeGen::emitTryExpr(const Expr& expr) {
    auto* te = expr.as<const TryExpr>();
    if (!te->inner) return nullptr;

    auto* val = emitExpr(*te->inner);
    if (!val) return nullptr;

    auto* fn = builder_->GetInsertBlock()->getParent();

    if (auto* stTy = llvm::dyn_cast<llvm::StructType>(val->getType())) {
        bool isResult = stTy->hasName() && isResultSlotName(stTy->getName());
        if (isResult) {
            auto* alloca = createEntryBlockAlloca(fn, stTy, "try.result");
            builder_->CreateStore(val, alloca);

            auto* tagPtr = builder_->CreateStructGEP(stTy, alloca, 0, "try.tag.ptr");
            auto* tagTy = stTy->getElementType(0);
            auto* tag = builder_->CreateLoad(tagTy, tagPtr, "try.tag");
            auto* isErr = builder_->CreateICmpEQ(tag,
                llvm::ConstantInt::get(tagTy, 1), "try.iserr");

            auto* errBB = llvm::BasicBlock::Create(*context_, "try.err", fn);
            auto* okBB = llvm::BasicBlock::Create(*context_, "try.ok", fn);
            builder_->CreateCondBr(isErr, errBB, okBB);

            builder_->SetInsertPoint(errBB);
            // Flush deferred statements before early-return on Err so
            // scope-exit cleanups fire the same way they would on an
            // explicit `return`. Without this, `defer` bodies are skipped
            // entirely when `try` short-circuits on the Err branch.
            if (!deferStack_.empty())
                emitDeferredStmts(deferStack_.size() - 1);
            if (fn->getReturnType()->isVoidTy()) {
                builder_->CreateRetVoid();
            } else {
                builder_->CreateRet(castToType(val, fn->getReturnType()));
            }

            builder_->SetInsertPoint(okBB);
            auto* valPtr = builder_->CreateStructGEP(stTy, alloca, 1, "try.ok.ptr");
            auto* elemTy = stTy->getElementType(1);
            llvm::Type* loadTy = elemTy;
            // Prefer the per-variant Ok type when registered (Result<T,E>
            // with widened payload). Without this, an Ok(i64) hidden inside
            // a 32-byte __Result_32 payload would load as the full [32 x i8]
            // array and cast-truncate at call sites, corrupting the value.
            // Check the inner expression's canonical Vyx mangled name FIRST
            // since the LLVM struct name is shared across multiple
            // Result<T,E> monomorphs with the same max payload size.
            {
                std::string scrutineeMangled;
                if (te->inner && te->inner->inferredType) {
                    scrutineeMangled = te->inner->inferredType->mangle();
                }
                if (!scrutineeMangled.empty()) {
                    auto rvIt = resultVariantInnerTypes_.find(scrutineeMangled);
                    if (rvIt != resultVariantInnerTypes_.end()) {
                        auto okIt = rvIt->second.find("Ok");
                        if (okIt == rvIt->second.end()) okIt = rvIt->second.find("Some");
                        if (okIt != rvIt->second.end()) loadTy = okIt->second;
                    }
                }
                if (loadTy == elemTy && stTy->hasName()) {
                    auto rvIt = resultVariantInnerTypes_.find(stTy->getName().str());
                    if (rvIt != resultVariantInnerTypes_.end()) {
                        auto okIt = rvIt->second.find("Ok");
                        if (okIt == rvIt->second.end()) okIt = rvIt->second.find("Some");
                        if (okIt != rvIt->second.end()) loadTy = okIt->second;
                    }
                }
            }
            if (loadTy == elemTy && elemTy->isArrayTy()) {
                auto& dl = module_->getDataLayout();
                loadTy = (dl.getTypeAllocSize(elemTy) >= 8)
                    ? llvm::Type::getInt64Ty(*context_)
                    : llvm::Type::getInt32Ty(*context_);
            }
            // When Ok's type is a struct (like Vec/string), the array
            // payload stores fields compactly; do a per-field load into
            // a properly-typed alloca rather than a monolithic struct
            // load (which would hit SROA alignment surprises).
            if (elemTy->isArrayTy() && loadTy->isStructTy()) {
                auto* innerStTy = llvm::cast<llvm::StructType>(loadTy);
                auto* tmpAlloca = createEntryBlockAlloca(fn, loadTy, "try.ok.tmp");
                auto* i8Ty = llvm::Type::getInt8Ty(*context_);
                auto* i64Ty = llvm::Type::getInt64Ty(*context_);
                uint64_t compactOffset = 0;
                for (unsigned fi = 0; fi < innerStTy->getNumElements(); ++fi) {
                    auto* fieldTy = innerStTy->getElementType(fi);
                    uint64_t fieldSz = module_->getDataLayout().getTypeStoreSize(fieldTy);
                    auto* bytePtr = builder_->CreateGEP(
                        i8Ty, valPtr, llvm::ConstantInt::get(i64Ty, compactOffset),
                        "try.ok.field.byte.ptr");
                    auto* fieldVal = builder_->CreateLoad(fieldTy, bytePtr, "try.ok.field.val");
                    auto* dstPtr = builder_->CreateStructGEP(
                        innerStTy, tmpAlloca, fi, "try.ok.field.dst.ptr");
                    builder_->CreateStore(fieldVal, dstPtr);
                    compactOffset += fieldSz;
                }
                return builder_->CreateLoad(loadTy, tmpAlloca, "try.ok.val");
            }
            return builder_->CreateLoad(loadTy, valPtr, "try.ok.val");
        }
    }

    if (val->getType()->isIntegerTy()) {
        auto* zero = llvm::ConstantInt::get(val->getType(), 0);
        auto* isErr = builder_->CreateICmpSLT(val, zero, "try.iserr");

        auto* errBB = llvm::BasicBlock::Create(*context_, "try.err", fn);
        auto* okBB = llvm::BasicBlock::Create(*context_, "try.ok", fn);
        builder_->CreateCondBr(isErr, errBB, okBB);

        builder_->SetInsertPoint(errBB);
        // Same defer-flush as the Result-shaped branch above.
        if (!deferStack_.empty())
            emitDeferredStmts(deferStack_.size() - 1);
        if (fn->getReturnType()->isVoidTy()) {
            builder_->CreateRetVoid();
        } else {
            builder_->CreateRet(castToType(val, fn->getReturnType()));
        }

        builder_->SetInsertPoint(okBB);
    }

    return val;
}

} // namespace vyx
