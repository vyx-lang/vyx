#include "CodeGenIncludes.h"

namespace vyx {

void CodeGen::generateDeriveMethods() {
    // Phase 8 (2026-04-23): Sema::synthesizeDeriveMethods now produces real
    // AST MethodDecls on every `@[derive(...)]`-annotated class/struct, so
    // the usual class-method emission path (emitFunctionDecl) handles the
    // LLVM generation. Running the legacy manual LLVM builder below would
    // emit duplicate function definitions (LLVM verifier rejects), so the
    // function is now a no-op kept only for the historical call site.
    fprintf(stderr, "[CodeGen] Phase: derive auto-generate methods (Sema-owned; CodeGen skipped)\n"); fflush(stderr);
    return;

    // ── Dead code retained for reference (not compiled) ──
#if 0
    // Process @[derive] attributes: auto-generate methods
    for (auto& decl : unit_->declarations) {
        if (!decl) continue;
        if (decl->kind != DeclKind::Struct && decl->kind != DeclKind::Class) continue;

        for (auto& [attrName, attrValue] : decl->attributes) {
            if (attrName != "derive") continue;

            auto stIt = structTypes_.find(decl->name);
            if (stIt == structTypes_.end()) continue;
            auto* stTy = stIt->second;
            auto& fields = structFieldNames_[decl->name];
            auto* ptrTy = llvm::PointerType::getUnqual(*context_);

            if (attrValue.find("Eq") != std::string::npos) {
                std::string eqName = decl->name + ".operator_eq";
                auto* eqTy = llvm::FunctionType::get(
                    llvm::Type::getInt8Ty(*context_), {ptrTy, ptrTy}, false);
                auto* eqFn = llvm::Function::Create(eqTy, llvm::Function::ExternalLinkage, eqName, *module_);
                eqFn->arg_begin()->setName("self");
                (eqFn->arg_begin() + 1)->setName("other");

                auto* entry = llvm::BasicBlock::Create(*context_, "entry", eqFn);
                builder_->SetInsertPoint(entry);

                llvm::Value* result = llvm::ConstantInt::getTrue(*context_);
                auto* strTy2 = getOrCreateStringType();
                for (size_t i = 0; i < fields.size(); ++i) {
                    auto* selfField = builder_->CreateStructGEP(stTy, &*eqFn->arg_begin(), i);
                    auto* otherField = builder_->CreateStructGEP(stTy, &*(eqFn->arg_begin() + 1), i);
                    auto* fieldTy = stTy->getElementType(i);
                    auto* a = builder_->CreateLoad(fieldTy, selfField);
                    auto* b = builder_->CreateLoad(fieldTy, otherField);
                    llvm::Value* eq;
                    if (fieldTy == strTy2) {
                        auto* aPtr = extractStringPtr(a);
                        auto* bPtr = extractStringPtr(b);
                        auto* strcmpFn = module_->getFunction("strcmp");
                        if (!strcmpFn) {
                            auto* fty = llvm::FunctionType::get(
                                llvm::Type::getInt32Ty(*context_),
                                {llvm::PointerType::getUnqual(*context_), llvm::PointerType::getUnqual(*context_)}, false);
                            strcmpFn = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, "strcmp", *module_);
                        }
                        auto* cmp = builder_->CreateCall(strcmpFn, {aPtr, bPtr});
                        eq = createSafeICmp(llvm::CmpInst::ICMP_EQ, cmp,
                            llvm::ConstantInt::get(cmp->getType(), 0));
                    } else if (fieldTy->isFloatingPointTy()) {
                        eq = builder_->CreateFCmpOEQ(a, b);
                    } else if (auto* nestedStTy = llvm::dyn_cast<llvm::StructType>(fieldTy)) {
                        std::string nestedName = nestedStTy->hasName() ? nestedStTy->getName().str() : "";
                        auto nestedEqIt = functions_.find(nestedName + ".operator_eq");
                        if (nestedEqIt != functions_.end()) {
                            auto* aAlloca2 = createEntryBlockAlloca(eqFn, nestedStTy, "eq.nested.a");
                            auto* bAlloca2 = createEntryBlockAlloca(eqFn, nestedStTy, "eq.nested.b");
                            builder_->CreateStore(a, aAlloca2);
                            builder_->CreateStore(b, bAlloca2);
                            eq = builder_->CreateCall(nestedEqIt->second, {aAlloca2, bAlloca2}, "eq.nested");
                        } else {
                            auto* memcmpFn2 = module_->getFunction("memcmp");
                            if (!memcmpFn2) {
                                auto* fty = llvm::FunctionType::get(
                                    llvm::Type::getInt32Ty(*context_),
                                    {llvm::PointerType::getUnqual(*context_), llvm::PointerType::getUnqual(*context_),
                                     llvm::Type::getInt64Ty(*context_)}, false);
                                memcmpFn2 = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, "memcmp", *module_);
                            }
                            auto* aA = createEntryBlockAlloca(eqFn, nestedStTy, "eq.a.st");
                            auto* bA = createEntryBlockAlloca(eqFn, nestedStTy, "eq.b.st");
                            builder_->CreateStore(a, aA);
                            builder_->CreateStore(b, bA);
                            uint64_t sz = module_->getDataLayout().getTypeAllocSize(nestedStTy);
                            auto* cmp = builder_->CreateCall(memcmpFn2,
                                {aA, bA, llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), sz)});
                            eq = createSafeICmp(llvm::CmpInst::ICMP_EQ, cmp,
                                llvm::ConstantInt::get(cmp->getType(), 0));
                        }
                    } else {
                        eq = builder_->CreateICmpEQ(a, b);
                    }
                    result = builder_->CreateAnd(result, eq);
                }
                builder_->CreateRet(builder_->CreateZExt(result, llvm::Type::getInt8Ty(*context_)));
                functions_[eqName] = eqFn;
            }

            if (attrValue.find("Clone") != std::string::npos) {
                std::string cloneName = decl->name + ".clone";
                auto* cloneTy = llvm::FunctionType::get(stTy, {ptrTy}, false);
                auto* cloneFn = llvm::Function::Create(cloneTy, llvm::Function::ExternalLinkage, cloneName, *module_);
                cloneFn->arg_begin()->setName("self");
                auto* cloneEntry = llvm::BasicBlock::Create(*context_, "entry", cloneFn);
                builder_->SetInsertPoint(cloneEntry);

                auto* strTy3 = getOrCreateStringType();
                bool hasStringFields = false;
                for (size_t ci = 0; ci < fields.size(); ++ci) {
                    if (stTy->getElementType(ci) == strTy3) { hasStringFields = true; break; }
                }

                if (hasStringFields) {
                    auto* resultAlloca = createEntryBlockAlloca(cloneFn, stTy, "clone.result");
                    auto* origVal = builder_->CreateLoad(stTy, &*cloneFn->arg_begin(), "clone.orig");
                    builder_->CreateStore(origVal, resultAlloca);

                    auto* i64Ty2 = llvm::Type::getInt64Ty(*context_);
                    auto* i8Ty2 = llvm::Type::getInt8Ty(*context_);
                    auto* ptrTy2 = llvm::PointerType::getUnqual(*context_);
                    auto* mallocFn2 = module_->getFunction("malloc");
                    auto* memcpyFn2 = module_->getFunction("memcpy");
                    if (!mallocFn2) {
                        auto* mty = llvm::FunctionType::get(ptrTy2, {i64Ty2}, false);
                        mallocFn2 = llvm::Function::Create(mty, llvm::Function::ExternalLinkage, "malloc", *module_);
                    }
                    if (!memcpyFn2) {
                        auto* mty = llvm::FunctionType::get(ptrTy2, {ptrTy2, ptrTy2, i64Ty2}, false);
                        memcpyFn2 = llvm::Function::Create(mty, llvm::Function::ExternalLinkage, "memcpy", *module_);
                    }

                    for (size_t ci = 0; ci < fields.size(); ++ci) {
                        if (stTy->getElementType(ci) != strTy3) continue;
                        auto* srcStrAlloca = createEntryBlockAlloca(cloneFn, strTy3, "clone.str.src");
                        auto* srcGep = builder_->CreateStructGEP(stTy, &*cloneFn->arg_begin(), ci);
                        auto* srcStr = builder_->CreateLoad(strTy3, srcGep);
                        builder_->CreateStore(srcStr, srcStrAlloca);
                        auto* srcPtr = builder_->CreateLoad(ptrTy2, builder_->CreateStructGEP(strTy3, srcStrAlloca, 0));
                        auto* srcLen = builder_->CreateLoad(i64Ty2, builder_->CreateStructGEP(strTy3, srcStrAlloca, 1));
                        auto* allocSz = builder_->CreateAdd(srcLen, llvm::ConstantInt::get(i64Ty2, 1));
                        auto* newBuf = builder_->CreateCall(mallocFn2, {allocSz}, "clone.str.buf");
                        builder_->CreateCall(memcpyFn2, {newBuf, srcPtr, srcLen});
                        auto* nullPos = builder_->CreateGEP(i8Ty2, newBuf, srcLen);
                        builder_->CreateStore(llvm::ConstantInt::get(i8Ty2, 0), nullPos);
                        auto* dstGep = builder_->CreateStructGEP(stTy, resultAlloca, ci);
                        builder_->CreateStore(newBuf, builder_->CreateStructGEP(strTy3, dstGep, 0));
                        builder_->CreateStore(srcLen, builder_->CreateStructGEP(strTy3, dstGep, 1));
                        builder_->CreateStore(allocSz, builder_->CreateStructGEP(strTy3, dstGep, 2));
                        builder_->CreateStore(llvm::ConstantInt::get(i64Ty2, 1), builder_->CreateStructGEP(strTy3, dstGep, 3));
                    }
                    builder_->CreateRet(builder_->CreateLoad(stTy, resultAlloca, "clone.deep"));
                } else {
                    auto* val = builder_->CreateLoad(stTy, &*cloneFn->arg_begin(), "clone.val");
                    builder_->CreateRet(val);
                }
                functions_[cloneName] = cloneFn;
            }

            if (attrValue.find("Debug") != std::string::npos) {
                auto* strTy = getOrCreateStringType();
                bool debugFieldsOk = true;
                for (size_t i = 0; i < fields.size(); ++i) {
                    auto* fieldTy = stTy->getElementType(i);
                    if (fieldTy->isIntegerTy() || fieldTy->isFloatingPointTy() || fieldTy == strTy)
                        continue;
                    diag_.error(decl->location,
                        "[derive(Debug)] cannot format field `{}` of `{}`: only integer, floating-point, and string fields are supported",
                        fields[i], decl->name);
                    debugFieldsOk = false;
                }
                if (!debugFieldsOk) continue;

                std::string toStrName = decl->name + ".toString";
                auto* toStrTy = llvm::FunctionType::get(strTy, {ptrTy}, false);
                auto* toStrFn = llvm::Function::Create(toStrTy, llvm::Function::ExternalLinkage, toStrName, *module_);
                toStrFn->arg_begin()->setName("self");
                auto* toStrEntry = llvm::BasicBlock::Create(*context_, "entry", toStrFn);
                builder_->SetInsertPoint(toStrEntry);

                auto* i64Ty = llvm::Type::getInt64Ty(*context_);
                auto* mallocFn = module_->getFunction("malloc");
                auto* snprintfFn = module_->getFunction("snprintf");
                if (!snprintfFn) {
                    auto* fty = llvm::FunctionType::get(
                        llvm::Type::getInt32Ty(*context_),
                        {llvm::PointerType::getUnqual(*context_), i64Ty, llvm::PointerType::getUnqual(*context_)}, true);
                    snprintfFn = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, "snprintf", *module_);
                }

                std::string fmt = decl->name + " { ";
                std::vector<llvm::Value*> fmtArgs;
                for (size_t i = 0; i < fields.size(); ++i) {
                    if (i > 0) fmt += ", ";
                    fmt += fields[i] + ": ";
                    auto* fieldTy = stTy->getElementType(i);
                    auto* gep = builder_->CreateStructGEP(stTy, &*toStrFn->arg_begin(), i);
                    llvm::Value* val = builder_->CreateLoad(fieldTy, gep);
                    if (fieldTy->isIntegerTy()) {
                        if (fieldTy->getIntegerBitWidth() > 32) fmt += "%lld";
                        else fmt += "%d";
                        if (fieldTy->getIntegerBitWidth() < 32)
                            val = numericCast(val, llvm::Type::getInt32Ty(*context_));
                    } else if (fieldTy->isFloatingPointTy()) {
                        fmt += "%f";
                        if (fieldTy->isFloatTy())
                            val = builder_->CreateFPExt(val, llvm::Type::getDoubleTy(*context_));
                    } else {
                        fmt += "%.*s";
                        auto* sAlloca = createEntryBlockAlloca(toStrFn, strTy, "dbg.str");
                        builder_->CreateStore(val, sAlloca);
                        auto* sLen = builder_->CreateLoad(i64Ty, builder_->CreateStructGEP(strTy, sAlloca, 1));
                        fmtArgs.push_back(builder_->CreateTrunc(sLen, llvm::Type::getInt32Ty(*context_)));
                        val = builder_->CreateLoad(ptrTy, builder_->CreateStructGEP(strTy, sAlloca, 0));
                    }
                    fmtArgs.push_back(val);
                }
                fmt += " }";

                if (!mallocFn) {
                    auto* mallocTy = llvm::FunctionType::get(ptrTy, {i64Ty}, false);
                    mallocFn = llvm::Function::Create(mallocTy, llvm::Function::ExternalLinkage, "malloc", *module_);
                }
                auto* fmtStr = getOrCreateString(fmt);
                auto* nullBuf = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context_));
                auto* zero64 = llvm::ConstantInt::get(i64Ty, 0);
                std::vector<llvm::Value*> sizeArgs = {nullBuf, zero64, fmtStr};
                sizeArgs.insert(sizeArgs.end(), fmtArgs.begin(), fmtArgs.end());
                auto* needed = builder_->CreateCall(snprintfFn, sizeArgs, "debug.needed");
                auto* needed64 = numericCast(needed, i64Ty);
                auto* bufSize = builder_->CreateAdd(needed64, llvm::ConstantInt::get(i64Ty, 1), "debug.bufsz");
                auto* buf = builder_->CreateCall(mallocFn, {bufSize}, "debug.buf");
                std::vector<llvm::Value*> snArgs = {buf, bufSize, fmtStr};
                snArgs.insert(snArgs.end(), fmtArgs.begin(), fmtArgs.end());
                builder_->CreateCall(snprintfFn, snArgs);

                auto* resultAlloca = createEntryBlockAlloca(toStrFn, strTy, "debug.str");
                builder_->CreateStore(buf, builder_->CreateStructGEP(strTy, resultAlloca, 0));
                builder_->CreateStore(needed64, builder_->CreateStructGEP(strTy, resultAlloca, 1));
                builder_->CreateStore(bufSize, builder_->CreateStructGEP(strTy, resultAlloca, 2));
                builder_->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), 1),
                    builder_->CreateStructGEP(strTy, resultAlloca, 3));
                builder_->CreateRet(builder_->CreateLoad(strTy, resultAlloca, "debug.result"));
                functions_[toStrName] = toStrFn;
            }

            if (attrValue.find("Hash") != std::string::npos) {
                bool hashFieldsOk = true;
                for (size_t i = 0; i < fields.size(); ++i) {
                    auto* fieldTy = stTy->getElementType(i);
                    if (fieldTy->isIntegerTy() || fieldTy->isFloatingPointTy()) continue;
                    diag_.error(decl->location,
                        "[derive(Hash)] cannot hash field `{}` of `{}`: only integer and floating-point fields are supported",
                        fields[i], decl->name);
                    hashFieldsOk = false;
                }
                if (!hashFieldsOk) continue;

                std::string hashName = decl->name + ".hash";
                auto* i64Ty = llvm::Type::getInt64Ty(*context_);
                auto* hashTy = llvm::FunctionType::get(i64Ty, {ptrTy}, false);
                auto* hashFn = llvm::Function::Create(hashTy, llvm::Function::ExternalLinkage, hashName, *module_);
                hashFn->arg_begin()->setName("self");
                auto* hashEntry = llvm::BasicBlock::Create(*context_, "entry", hashFn);
                builder_->SetInsertPoint(hashEntry);
                llvm::Value* hash = llvm::ConstantInt::get(i64Ty, 5381);
                for (size_t i = 0; i < fields.size(); ++i) {
                    auto* fieldTy = stTy->getElementType(i);
                    auto* gep = builder_->CreateStructGEP(stTy, &*hashFn->arg_begin(), i);
                    auto* val = builder_->CreateLoad(fieldTy, gep);
                    llvm::Value* val64 = val;
                    if (fieldTy->isFloatingPointTy())
                        val64 = builder_->CreateBitCast(
                            builder_->CreateFPExt(val, llvm::Type::getDoubleTy(*context_)), i64Ty);
                    else if (fieldTy->getIntegerBitWidth() < 64)
                        val64 = numericCast(val, i64Ty);
                    auto* shifted = builder_->CreateShl(hash, llvm::ConstantInt::get(i64Ty, 5));
                    hash = builder_->CreateAdd(shifted, hash);
                    hash = builder_->CreateXor(hash, val64);
                }
                builder_->CreateRet(hash);
                functions_[hashName] = hashFn;
            }

            if (attrValue.find("Ord") != std::string::npos) {
                bool ordFieldsOk = true;
                for (size_t i = 0; i < fields.size(); ++i) {
                    auto* fieldTy = stTy->getElementType(i);
                    if (fieldTy->isIntegerTy() || fieldTy->isFloatingPointTy()) continue;
                    diag_.error(decl->location,
                        "[derive(Ord)] cannot compare field `{}` of `{}`: only integer and floating-point fields are supported",
                        fields[i], decl->name);
                    ordFieldsOk = false;
                }
                if (!ordFieldsOk) continue;

                std::string cmpName = decl->name + ".operator_lt";
                auto* cmpTy = llvm::FunctionType::get(
                    llvm::Type::getInt8Ty(*context_), {ptrTy, ptrTy}, false);
                auto* cmpFn = llvm::Function::Create(cmpTy, llvm::Function::ExternalLinkage, cmpName, *module_);
                cmpFn->arg_begin()->setName("self");
                (cmpFn->arg_begin() + 1)->setName("other");
                auto* cmpEntry = llvm::BasicBlock::Create(*context_, "entry", cmpFn);
                builder_->SetInsertPoint(cmpEntry);
                auto* cmpMerge = llvm::BasicBlock::Create(*context_, "merge", cmpFn);
                for (size_t i = 0; i < fields.size(); ++i) {
                    auto* fieldTy = stTy->getElementType(i);
                    auto* selfGep = builder_->CreateStructGEP(stTy, &*cmpFn->arg_begin(), i);
                    auto* otherGep = builder_->CreateStructGEP(stTy, &*(cmpFn->arg_begin() + 1), i);
                    auto* a = builder_->CreateLoad(fieldTy, selfGep);
                    auto* b = builder_->CreateLoad(fieldTy, otherGep);
                    llvm::Value* lt;
                    if (fieldTy->isFloatingPointTy())
                        lt = builder_->CreateFCmpOLT(a, b);
                    else
                        lt = builder_->CreateICmpSLT(a, b);
                    auto* ltBB = llvm::BasicBlock::Create(*context_, "lt.true", cmpFn);
                    auto* nextBB = llvm::BasicBlock::Create(*context_, "lt.next", cmpFn);
                    builder_->CreateCondBr(lt, ltBB, nextBB);
                    builder_->SetInsertPoint(ltBB);
                    builder_->CreateRet(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context_), 1));
                    builder_->SetInsertPoint(nextBB);
                    llvm::Value* gt;
                    if (fieldTy->isFloatingPointTy())
                        gt = builder_->CreateFCmpOGT(a, b);
                    else
                        gt = builder_->CreateICmpSGT(a, b);
                    auto* gtBB = llvm::BasicBlock::Create(*context_, "gt.true", cmpFn);
                    auto* eqBB = llvm::BasicBlock::Create(*context_, "eq.next", cmpFn);
                    builder_->CreateCondBr(gt, gtBB, eqBB);
                    builder_->SetInsertPoint(gtBB);
                    builder_->CreateRet(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context_), 0));
                    builder_->SetInsertPoint(eqBB);
                    (void)eqBB;
                }
                builder_->CreateRet(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context_), 0));
                if (cmpMerge->hasNPredecessors(0)) cmpMerge->eraseFromParent();
                functions_[cmpName] = cmpFn;
            }
        }
    }
#endif
}

} // namespace vyx