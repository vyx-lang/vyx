#include "CodeGenIncludes.h"

namespace vyx {

void CodeGen::enableCoverage() {
    emitCoverage_ = true;
}

std::optional<CodeGen::GenericArgs> CodeGen::parseGenericArgs(std::string_view mangledName) {
    auto ltPos = mangledName.find('<');
    if (ltPos == std::string_view::npos || mangledName.back() != '>') return std::nullopt;

    GenericArgs result;
    result.baseName = std::string(mangledName.substr(0, ltPos));

    auto inner = mangledName.substr(ltPos + 1, mangledName.size() - ltPos - 2);
    size_t start = 0;
    int depth = 0;
    for (size_t i = 0; i < inner.size(); ++i) {
        if (inner[i] == '<') ++depth;
        else if (inner[i] == '>') --depth;
        else if (inner[i] == ',' && depth == 0) {
            result.typeArgs.emplace_back(inner.substr(start, i - start));
            start = i + 1;
        }
    }
    result.typeArgs.emplace_back(inner.substr(start));
    return result;
}

void CodeGen::setupGenericTypeParams(const Decl& decl) {
    if (decl.genericParams.empty()) return;

    auto parsed = parseGenericArgs(decl.name);
    if (parsed && parsed->typeArgs.size() == decl.genericParams.size()) {
        for (size_t i = 0; i < decl.genericParams.size(); ++i) {
            TypeAnnotation ann;
            ann.kind = TypeAnnotationKind::Named;
            ann.name = parsed->typeArgs[i];
            genericTypeParams_[decl.genericParams[i]] = toLLVMType(ann);
            genericTypeParamNames_[decl.genericParams[i]] = parsed->typeArgs[i];
        }
    } else {
        for (auto& gp : decl.genericParams) {
            genericTypeParams_[gp] = llvm::Type::getInt64Ty(*context_);
            genericTypeParamNames_[gp] = "i64";
        }
    }
}

void CodeGen::emitCoverageIncrement(const std::string& funcName, int region) {
    if (!emitCoverage_) return;

    std::string counterName = "__vyx_cov_" + funcName + "_" + std::to_string(region);
    auto it = coverageCounters_.find(counterName);
    llvm::GlobalVariable* counter = nullptr;

    if (it != coverageCounters_.end()) {
        counter = it->second;
    } else {
        auto* i64Ty = llvm::Type::getInt64Ty(*context_);
        counter = new llvm::GlobalVariable(*module_, i64Ty, false,
            llvm::GlobalValue::InternalLinkage,
            llvm::ConstantInt::get(i64Ty, 0), counterName);
        coverageCounters_[counterName] = counter;
    }

    auto* i64Ty = llvm::Type::getInt64Ty(*context_);
    builder_->CreateAtomicRMW(llvm::AtomicRMWInst::Add, counter,
        llvm::ConstantInt::get(i64Ty, 1), llvm::MaybeAlign(),
        llvm::AtomicOrdering::Monotonic);
}

void CodeGen::enableDebugInfo(const std::string& filename) {
    emitDebug_ = true;
    debugBuilder_ = std::make_unique<llvm::DIBuilder>(*module_);
    debugFile_ = debugBuilder_->createFile(filename, ".");
    debugCU_ = debugBuilder_->createCompileUnit(
        llvm::dwarf::DW_LANG_C, debugFile_, "Vyx Compiler bl-2026-03-24", false, "", 0);
    module_->addModuleFlag(llvm::Module::Warning, "Debug Info Version",
        llvm::DEBUG_METADATA_VERSION);
#ifdef _WIN32
    module_->addModuleFlag(llvm::Module::Warning, "CodeView", 1);
#endif
}

void CodeGen::emitDebugLocation(const SourceLocation& loc) {
    if (!emitDebug_ || !debugCU_) return;
    if (loc.line == 0) return;
    auto* fn = builder_->GetInsertBlock() ? builder_->GetInsertBlock()->getParent() : nullptr;
    if (!fn || !fn->getSubprogram()) return;
    llvm::DIScope* scope = fn->getSubprogram();
    builder_->SetCurrentDebugLocation(
        llvm::DILocation::get(*context_, loc.line, loc.column, scope));
}

llvm::Function* CodeGen::getOrCreatePanicFn() {
    auto* fn = module_->getFunction("__vyx_panic");
    if (fn) return fn;

    auto* ptrTy = llvm::PointerType::getUnqual(*context_);
    auto* voidTy = llvm::Type::getVoidTy(*context_);
    auto* fnTy = llvm::FunctionType::get(voidTy, {ptrTy}, false);
    fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage, "__vyx_panic", *module_);
    fn->addFnAttr(llvm::Attribute::NoReturn);

    auto* entry = llvm::BasicBlock::Create(*context_, "entry", fn);
    auto savedIP = builder_->saveIP();
    builder_->SetInsertPoint(entry);

    auto* printfFn = getOrCreatePrintf();
    auto* msg = fn->getArg(0);
    builder_->CreateCall(printfFn, {msg});

    // Always print a newline + "backtrace:" header so messages without a
    // trailing \n (like `panic("Vec.get out of bounds")`) don't produce
    // "Vec.get out of bounds  backtrace:" on one line. When the message
    // already ends in \n this makes for one blank line before backtrace
    // — we've stripped the \n from the compiler-emitted panic literals
    // above, but user-written panic() messages may still have one.
    builder_->CreateCall(printfFn, {getOrCreateString("\n  backtrace:\n")});

#ifdef _WIN32
    auto* i16Ty = llvm::Type::getInt16Ty(*context_);
    auto* i32Ty = llvm::Type::getInt32Ty(*context_);
    auto* i64Ty = llvm::Type::getInt64Ty(*context_);

    auto* captureStackBackTraceFnTy = llvm::FunctionType::get(
        i16Ty, {llvm::Type::getInt32Ty(*context_), llvm::Type::getInt32Ty(*context_), ptrTy, ptrTy}, false);
    auto* captureStackFn = module_->getOrInsertFunction("RtlCaptureStackBackTrace", captureStackBackTraceFnTy).getCallee();

    auto* stackBuf = builder_->CreateAlloca(ptrTy, llvm::ConstantInt::get(i32Ty, 32), "bt.buf");
    auto* nFrames = builder_->CreateCall(
        captureStackBackTraceFnTy, captureStackFn,
        {llvm::ConstantInt::get(i32Ty, 1), llvm::ConstantInt::get(i32Ty, 32),
         stackBuf, llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context_))},
        "bt.n");
    auto* nFramesExt = builder_->CreateZExt(nFrames, i32Ty, "bt.n32");

    auto* idxAlloca = builder_->CreateAlloca(i32Ty, nullptr, "bt.idx");
    builder_->CreateStore(llvm::ConstantInt::get(i32Ty, 0), idxAlloca);

    auto* loopBB = llvm::BasicBlock::Create(*context_, "bt.loop", fn);
    auto* bodyBB = llvm::BasicBlock::Create(*context_, "bt.body", fn);
    auto* endBB = llvm::BasicBlock::Create(*context_, "bt.end", fn);
    builder_->CreateBr(loopBB);

    builder_->SetInsertPoint(loopBB);
    auto* idx = builder_->CreateLoad(i32Ty, idxAlloca, "bt.i");
    auto* cmp = builder_->CreateICmpSLT(idx, nFramesExt, "bt.cmp");
    builder_->CreateCondBr(cmp, bodyBB, endBB);

    builder_->SetInsertPoint(bodyBB);
    auto* framePtr = builder_->CreateGEP(ptrTy, stackBuf, idx, "bt.frame.ptr");
    auto* frame = builder_->CreateLoad(ptrTy, framePtr, "bt.frame");
    auto* frameAsInt = builder_->CreatePtrToInt(frame, i64Ty, "bt.addr");
    builder_->CreateCall(printfFn, {getOrCreateString("    [%d] 0x%llx\n"), idx, frameAsInt});
    auto* nextIdx = builder_->CreateAdd(idx, llvm::ConstantInt::get(i32Ty, 1));
    builder_->CreateStore(nextIdx, idxAlloca);
    builder_->CreateBr(loopBB);

    builder_->SetInsertPoint(endBB);
#endif

    auto* exitFn = module_->getFunction("exit");
    if (!exitFn) {
        auto* exitTy = llvm::FunctionType::get(voidTy, {llvm::Type::getInt32Ty(*context_)}, false);
        exitFn = llvm::Function::Create(exitTy, llvm::Function::ExternalLinkage, "exit", *module_);
        exitFn->addFnAttr(llvm::Attribute::NoReturn);
    }
    builder_->CreateCall(exitFn, {llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 1)});
    builder_->CreateUnreachable();

    builder_->restoreIP(savedIP);
    return fn;
}

void CodeGen::emitPanicCall(llvm::Value* msgStr) {
    auto* panicFn = getOrCreatePanicFn();
    builder_->CreateCall(panicFn, {msgStr});
    builder_->CreateUnreachable();
}

} // namespace vyx
