#include "CodeGenIncludes.h"

namespace vyx {

void CodeGen::emitStmt(const Stmt& stmt) {
    if (!builder_->GetInsertBlock()) return;
    if (diag_.hasErrors()) return;
    if (++emitDepth_ > MaxEmitDepth) {
        --emitDepth_;
        diag_.error(stmt.location, "codegen statement depth exceeded ({})", MaxEmitDepth);
        return;
    }
    struct DepthGuard { int& d; ~DepthGuard() { --d; } } guard{emitDepth_};
    emitDebugLocation(stmt.location);

    switch (stmt.kind) {
        case StmtKind::Block:      emitBlock(stmt); break;
        case StmtKind::VarDecl:    emitVarDecl(stmt); break;
        case StmtKind::If:         emitIfStmt(stmt); break;
        case StmtKind::While:      emitWhileStmt(stmt); break;
        case StmtKind::For:        emitForStmt(stmt); break;
        case StmtKind::Return:     emitReturnStmt(stmt); break;
        case StmtKind::Assignment: emitAssignment(stmt); break;
        case StmtKind::Match:      emitMatchStmt(stmt); break;
        case StmtKind::ForEach: {
            auto& fe = static_cast<const ForEachStmt&>(stmt);
            if (!fe.collection || !fe.body) break;

            // Compile-time expansion: foreach (field in typeinfo(T).fields)
            if (fe.collection->kind == ExprKind::MemberAccess &&
                fe.collection->as<MemberAccessExpr>()->member == "fields" &&
                fe.collection->as<MemberAccessExpr>()->object &&
                fe.collection->as<MemberAccessExpr>()->object->kind == ExprKind::Call &&
                fe.collection->as<MemberAccessExpr>()->object->as<CallExpr>()->callee &&
                fe.collection->as<MemberAccessExpr>()->object->as<CallExpr>()->callee->kind == ExprKind::Identifier &&
                fe.collection->as<MemberAccessExpr>()->object->as<CallExpr>()->callee->as<IdentifierExpr>()->name == "typeinfo") {
                auto* tiCall = fe.collection->as<MemberAccessExpr>()->object->as<CallExpr>();
                if (!tiCall->args.empty() && tiCall->args[0] &&
                    tiCall->args[0]->kind == ExprKind::Identifier &&
                    tiCall->args[0]->as<IdentifierExpr>()->typeAnnotation) {
                    std::string typeName = tiCall->args[0]->as<IdentifierExpr>()->typeAnnotation->name;
                    auto fnIt = structFieldNames_.find(typeName);
                    if (fnIt != structFieldNames_.end()) {
                        auto& fieldNames = fnIt->second;
                        auto* fn = builder_->GetInsertBlock()->getParent();
                        auto* strTy = getOrCreateStringType();
                        for (size_t fi = 0; fi < fieldNames.size(); ++fi) {
                            auto* fieldNameVal = createStringValue(fieldNames[fi]);
                            auto* fieldAlloca = createEntryBlockAlloca(fn, strTy, fe.varName);
                            builder_->CreateStore(fieldNameVal, fieldAlloca);
                            auto savedVals = namedValues_;
                            namedValues_[fe.varName] = fieldAlloca;
                            emitStmt(*fe.body);
                            namedValues_ = savedVals;
                            if (builder_->GetInsertBlock() && builder_->GetInsertBlock()->getTerminator()) break;
                        }
                        break;
                    }
                }
            }

            auto* collVal = emitExpr(*fe.collection);
            if (!collVal) break;
            auto* fn = builder_->GetInsertBlock()->getParent();
            auto* i64Ty = llvm::Type::getInt64Ty(*context_);
            auto* ptrTy = llvm::PointerType::getUnqual(*context_);
            auto* idxAlloca = createEntryBlockAlloca(fn, i64Ty, "foreach.idx");
            builder_->CreateStore(llvm::ConstantInt::get(i64Ty, 0), idxAlloca);

            auto* collTy = collVal->getType();

            // String iteration: for c in str { }
            if (collTy == getOrCreateStringType()) {
                auto* strTy = getOrCreateStringType();
                auto* strAlloca = createEntryBlockAlloca(fn, strTy, "foreach.str");
                builder_->CreateStore(collVal, strAlloca);
                auto* strPtr = builder_->CreateLoad(ptrTy,
                    builder_->CreateStructGEP(strTy, strAlloca, 0), "str.ptr");
                auto* strLen = builder_->CreateLoad(i64Ty,
                    builder_->CreateStructGEP(strTy, strAlloca, 1), "str.len");
                auto* iterAlloca = createEntryBlockAlloca(fn, llvm::Type::getInt32Ty(*context_), fe.varName);

                auto* condBB = llvm::BasicBlock::Create(*context_, "str.cond", fn);
                auto* bodyBB = llvm::BasicBlock::Create(*context_, "str.body", fn);
                auto* endBB2 = llvm::BasicBlock::Create(*context_, "str.end", fn);
                loopStack_.push_back({endBB2, condBB, deferStack_.size(), blockAutoDropStack_.size()});
                builder_->CreateBr(condBB);
                builder_->SetInsertPoint(condBB);
                auto* idx = builder_->CreateLoad(i64Ty, idxAlloca);
                auto* inRange = builder_->CreateICmpSLT(idx, strLen);
                builder_->CreateCondBr(inRange, bodyBB, endBB2);
                builder_->SetInsertPoint(bodyBB);
                auto* charPtr = builder_->CreateGEP(llvm::Type::getInt8Ty(*context_), strPtr, idx, "str.char.ptr");
                auto* charVal = builder_->CreateLoad(llvm::Type::getInt8Ty(*context_), charPtr, "str.char");
                auto* charI32 = builder_->CreateZExt(charVal, llvm::Type::getInt32Ty(*context_));
                builder_->CreateStore(charI32, iterAlloca);
                auto savedVals = namedValues_;
                namedValues_[fe.varName] = iterAlloca;
                emitStmt(*fe.body);
                namedValues_ = savedVals;
                auto* curBB2 = builder_->GetInsertBlock();
                if (curBB2 && !curBB2->getTerminator()) {
                    auto* nextIdx = builder_->CreateAdd(builder_->CreateLoad(i64Ty, idxAlloca), llvm::ConstantInt::get(i64Ty, 1));
                    builder_->CreateStore(nextIdx, idxAlloca);
                    builder_->CreateBr(condBB);
                }
                builder_->SetInsertPoint(endBB2);
                loopStack_.pop_back();
                break;
            }

            // Fixed-size array: for x in [1,2,3] → compile-time unrolled index loop
            if (auto* arrTy = llvm::dyn_cast<llvm::ArrayType>(collTy)) {
                auto* arrAlloca = createEntryBlockAlloca(fn, arrTy, "foreach.arr");
                builder_->CreateStore(collVal, arrAlloca);
                auto* arrLen = llvm::ConstantInt::get(i64Ty, arrTy->getNumElements());
                auto* elemTy = arrTy->getElementType();
                auto* condBB = llvm::BasicBlock::Create(*context_, "arr.cond", fn);
                auto* bodyBB = llvm::BasicBlock::Create(*context_, "arr.body", fn);
                auto* endBB = llvm::BasicBlock::Create(*context_, "arr.end", fn);
                loopStack_.push_back({endBB, condBB, deferStack_.size(), blockAutoDropStack_.size()});
                builder_->CreateBr(condBB);
                builder_->SetInsertPoint(condBB);
                auto* idx = builder_->CreateLoad(i64Ty, idxAlloca);
                builder_->CreateCondBr(builder_->CreateICmpSLT(idx, arrLen), bodyBB, endBB);
                builder_->SetInsertPoint(bodyBB);
                auto* zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0);
                auto* gep = builder_->CreateGEP(arrTy, arrAlloca, {zero, idx}, "arr.elem.ptr");
                auto* elemVal = builder_->CreateLoad(elemTy, gep, "arr.elem");
                auto* iterAlloca = createEntryBlockAlloca(fn, elemTy, fe.varName);
                builder_->CreateStore(elemVal, iterAlloca);
                auto savedVals = namedValues_;
                namedValues_[fe.varName] = iterAlloca;
                emitStmt(*fe.body);
                namedValues_ = savedVals;
                if (auto* cb = builder_->GetInsertBlock(); cb && !cb->getTerminator()) {
                    builder_->CreateStore(
                        builder_->CreateAdd(builder_->CreateLoad(i64Ty, idxAlloca),
                            llvm::ConstantInt::get(i64Ty, 1)), idxAlloca);
                    builder_->CreateBr(condBB);
                }
                builder_->SetInsertPoint(endBB);
                loopStack_.pop_back();
                break;
            }

            if (collTy->isStructTy()) {
                auto* rangeTy = llvm::dyn_cast<llvm::StructType>(collTy);
                if (rangeTy && rangeTy->getNumElements() == 2 &&
                    rangeTy->getElementType(0)->isIntegerTy(64)) {
                    auto* rangeAlloca = createEntryBlockAlloca(fn, rangeTy, "range.tmp");
                    builder_->CreateStore(collVal, rangeAlloca);
                    auto* startPtr = builder_->CreateStructGEP(rangeTy, rangeAlloca, 0);
                    auto* startVal = builder_->CreateLoad(i64Ty, startPtr, "range.start");
                    auto* endPtr = builder_->CreateStructGEP(rangeTy, rangeAlloca, 1);
                    auto* endVal = builder_->CreateLoad(i64Ty, endPtr, "range.end");

                    auto* iterAlloca = createEntryBlockAlloca(fn, i64Ty, fe.varName);
                    builder_->CreateStore(startVal, iterAlloca);

                    auto* condBB = llvm::BasicBlock::Create(*context_, "range.cond", fn);
                    auto* bodyBB = llvm::BasicBlock::Create(*context_, "range.body", fn);
                    auto* endBB2 = llvm::BasicBlock::Create(*context_, "range.end", fn);
                    loopStack_.push_back({endBB2, condBB, deferStack_.size(), blockAutoDropStack_.size()});
                    builder_->CreateBr(condBB);
                    builder_->SetInsertPoint(condBB);
                    auto* curVal = builder_->CreateLoad(i64Ty, iterAlloca);
                    auto* inRange = builder_->CreateICmpSLT(curVal, endVal);
                    builder_->CreateCondBr(inRange, bodyBB, endBB2);
                    builder_->SetInsertPoint(bodyBB);
                    auto savedVals = namedValues_;
                    namedValues_[fe.varName] = iterAlloca;
                    emitStmt(*fe.body);
                    namedValues_ = savedVals;
                    auto* curBB2 = builder_->GetInsertBlock();
                    if (curBB2 && !curBB2->getTerminator()) {
                        auto* nextVal = builder_->CreateAdd(builder_->CreateLoad(i64Ty, iterAlloca), llvm::ConstantInt::get(i64Ty, 1));
                        builder_->CreateStore(nextVal, iterAlloca);
                        builder_->CreateBr(condBB);
                    }
                    builder_->SetInsertPoint(endBB2);
                    loopStack_.pop_back();
                    break;
                }
            }

            // iter() dispatch (general): any named-struct collection with an
            // iter() method. This covers `for x in self.children` (MemberAccess),
            // `for x in foo()` (Call), etc. — cases where the collection isn't
            // a plain Identifier. The Identifier-only path below handles a few
            // extra refinements (resolveClassName, destructuring iter state),
            // so we only take over when collTy is a struct whose base type has
            // iter() but not next() (i.e. it's a container, not an iterator).
            {
                std::string collTypeName;
                if (auto* stTy = llvm::dyn_cast<llvm::StructType>(collTy))
                    if (stTy->hasName()) collTypeName = stTy->getName().str();
                if (!collTypeName.empty() &&
                    fe.collection->kind != ExprKind::Identifier) {
                    auto* iterMethodFn = findClassMethod(collTypeName, "iter");
                    if (iterMethodFn) {
                        auto* collAlloca = createEntryBlockAlloca(fn, collTy, fe.varName + ".coll");
                        builder_->CreateStore(collVal, collAlloca);
                        auto* iterObj = builder_->CreateCall(iterMethodFn, {collAlloca}, "foriter.obj");
                        auto* iterObjAlloca = createEntryBlockAlloca(fn, iterObj->getType(), fe.varName + ".iter");
                        builder_->CreateStore(iterObj, iterObjAlloca);
                        std::string iterTypeName;
                        if (auto* stTy = llvm::dyn_cast<llvm::StructType>(iterObj->getType()))
                            if (stTy->hasName()) iterTypeName = stTy->getName().str();
                        auto* nxFn = findClassMethod(iterTypeName, "next");
                        if (nxFn) {
                            auto* retTy = nxFn->getReturnType();
                            auto* retStTy = llvm::dyn_cast<llvm::StructType>(retTy);
                            bool returnsResult = retStTy && retStTy->hasName() &&
                                                 isOptionOrResultSlotName(retStTy->getName());
                            if (returnsResult) {
                                auto* condBB = llvm::BasicBlock::Create(*context_, "foriter.cond", fn);
                                auto* bodyBB = llvm::BasicBlock::Create(*context_, "foriter.body", fn);
                                auto* endBB = llvm::BasicBlock::Create(*context_, "foriter.end", fn);
                                loopStack_.push_back({endBB, condBB, deferStack_.size(), blockAutoDropStack_.size()});
                                builder_->CreateBr(condBB);
                                builder_->SetInsertPoint(condBB);
                                auto* result = builder_->CreateCall(nxFn, {iterObjAlloca}, "foriter.result");
                                auto* resultTmp = createEntryBlockAlloca(fn, retTy, "foriter.result.tmp");
                                builder_->CreateStore(result, resultTmp);
                                auto* tagTy = llvm::cast<llvm::StructType>(retTy)->getElementType(0);
                                auto* tag = builder_->CreateLoad(tagTy,
                                    builder_->CreateStructGEP(retTy, resultTmp, 0), "foriter.tag");
                                builder_->CreateCondBr(
                                    builder_->CreateICmpEQ(tag, llvm::ConstantInt::get(tagTy, 0)),
                                    bodyBB, endBB);
                                builder_->SetInsertPoint(bodyBB);
                                auto* valFieldTy = llvm::cast<llvm::StructType>(retTy)->getElementType(1);
                                auto* dataPtr = builder_->CreateStructGEP(retTy, resultTmp, 1, "foriter.data.ptr");
                                // Element-type resolution: the payload slot is a
                                // byte array sized to hold the Option<T> payload.
                                // If the iterator base type is `Iterator<SomeClass>`
                                // and SomeClass has a registered struct type
                                // (e.g. user class Tree, mono-emitted Vec<Tree>),
                                // bind `child` as that struct so `child.field`
                                // resolves via structFieldNames_ instead of
                                // devolving to an i64 byte-blob load.
                                llvm::Type* loadTy = valFieldTy;
                                std::string innerElemName;
                                {
                                    auto lt = iterTypeName.find('<');
                                    auto gt = iterTypeName.rfind('>');
                                    if (lt != std::string::npos && gt != std::string::npos && gt > lt + 1)
                                        innerElemName = iterTypeName.substr(lt + 1, gt - lt - 1);
                                }
                                if (!innerElemName.empty()) {
                                    auto eit = structTypes_.find(innerElemName);
                                    if (eit != structTypes_.end()) {
                                        loadTy = eit->second;
                                    }
                                }
                                if (loadTy == valFieldTy && valFieldTy->isArrayTy()) {
                                    uint64_t sz = module_->getDataLayout().getTypeAllocSize(valFieldTy);
                                    if (sz <= 1) loadTy = llvm::Type::getInt8Ty(*context_);
                                    else if (sz <= 4) loadTy = llvm::Type::getInt32Ty(*context_);
                                    else loadTy = llvm::Type::getInt64Ty(*context_);
                                }
                                auto* elemVal = builder_->CreateLoad(loadTy, dataPtr, "foriter.val");
                                auto* elemAlloca = createEntryBlockAlloca(fn, loadTy, fe.varName);
                                builder_->CreateStore(elemVal, elemAlloca);
                                auto savedVals = namedValues_;
                                auto savedClassVarTypes = classVarTypes_;
                                namedValues_[fe.varName] = elemAlloca;
                                if (!innerElemName.empty() &&
                                    structTypes_.count(innerElemName)) {
                                    classVarTypes_[fe.varName] = innerElemName;
                                }
                                emitStmt(*fe.body);
                                namedValues_ = savedVals;
                                classVarTypes_ = savedClassVarTypes;
                                if (auto* cb = builder_->GetInsertBlock(); cb && !cb->getTerminator())
                                    builder_->CreateBr(condBB);
                                builder_->SetInsertPoint(endBB);
                                loopStack_.pop_back();
                                break;
                            }
                        }
                    }
                }
            }

            // iter() dispatch: call collection.iter() → loop via iterator's next()
            if (fe.collection->kind == ExprKind::Identifier) {
                std::string typeName = resolveClassName(*fe.collection,
                    fe.collection->as<IdentifierExpr>()->name);
                auto collIt0 = namedValues_.find(fe.collection->as<IdentifierExpr>()->name);
                if (collIt0 != namedValues_.end() && !typeName.empty()) {
                    auto* iterMethodFn = findClassMethod(typeName, "iter");
                    if (iterMethodFn) {
                        auto* iterObj = builder_->CreateCall(iterMethodFn, {collIt0->second}, "foriter.obj");
                        auto* iterObjAlloca = createEntryBlockAlloca(fn, iterObj->getType(), fe.varName + ".iter");
                        builder_->CreateStore(iterObj, iterObjAlloca);
                        std::string iterTypeName;
                        if (auto* stTy = llvm::dyn_cast<llvm::StructType>(iterObj->getType()))
                            if (stTy->hasName()) iterTypeName = stTy->getName().str();
                        auto* nxFn = findClassMethod(iterTypeName, "next");
                        if (nxFn) {
                            auto* retTy = nxFn->getReturnType();
                            auto* retStTy = llvm::dyn_cast<llvm::StructType>(retTy);
                            bool returnsResult = retStTy && retStTy->hasName() &&
                                                 isOptionOrResultSlotName(retStTy->getName());
                            if (returnsResult) {
                                // Rust-style: next() returns Result{tag, value}
                                auto* condBB = llvm::BasicBlock::Create(*context_, "foriter.cond", fn);
                                auto* bodyBB = llvm::BasicBlock::Create(*context_, "foriter.body", fn);
                                auto* endBB = llvm::BasicBlock::Create(*context_, "foriter.end", fn);
                                loopStack_.push_back({endBB, condBB, deferStack_.size(), blockAutoDropStack_.size()});
                                builder_->CreateBr(condBB);
                                builder_->SetInsertPoint(condBB);
                                auto* result = builder_->CreateCall(nxFn, {iterObjAlloca}, "foriter.result");
                                auto* resultTmp = createEntryBlockAlloca(fn, retTy, "foriter.result.tmp");
                                builder_->CreateStore(result, resultTmp);
                            auto* tagTy = llvm::cast<llvm::StructType>(retTy)->getElementType(0);
                            auto* tag = builder_->CreateLoad(tagTy,
                                builder_->CreateStructGEP(retTy, resultTmp, 0), "foriter.tag");
                            builder_->CreateCondBr(
                                builder_->CreateICmpEQ(tag, llvm::ConstantInt::get(tagTy, 0)),
                                bodyBB, endBB);
                                builder_->SetInsertPoint(bodyBB);
                                auto* valFieldTy2 = llvm::cast<llvm::StructType>(retTy)->getElementType(1);
                                auto* dataPtr2 = builder_->CreateStructGEP(retTy, resultTmp, 1, "foriter.data.ptr");
                                llvm::Type* loadTy2 = valFieldTy2;
                                if (valFieldTy2->isArrayTy()) {
                                    uint64_t sz = module_->getDataLayout().getTypeAllocSize(valFieldTy2);
                                    if (sz <= 1) loadTy2 = llvm::Type::getInt8Ty(*context_);
                                    else if (sz <= 4) loadTy2 = llvm::Type::getInt32Ty(*context_);
                                    else loadTy2 = llvm::Type::getInt64Ty(*context_);
                                }
                                auto* elemVal = builder_->CreateLoad(loadTy2, dataPtr2, "foriter.val");
                                auto* elemAlloca = createEntryBlockAlloca(fn, loadTy2, fe.varName);
                                builder_->CreateStore(elemVal, elemAlloca);
                                auto savedVals = namedValues_;
                                namedValues_[fe.varName] = elemAlloca;
                                // Destructuring: read named fields from iterator object
                                if (!fe.destructure.empty()) {
                                    auto sfIt = structFieldNames_.find(iterTypeName);
                                    if (sfIt == structFieldNames_.end()) {
                                        auto ltPos = iterTypeName.find('<');
                                        if (ltPos != std::string::npos)
                                            sfIt = structFieldNames_.find(iterTypeName.substr(0, ltPos));
                                    }
                                    if (sfIt != structFieldNames_.end()) {
                                        auto* iterStTy = llvm::dyn_cast<llvm::StructType>(
                                            getValuePtrType(iterObjAlloca));
                                        for (auto& dvar : fe.destructure) {
                                            for (size_t fi = 0; fi < sfIt->second.size(); ++fi) {
                                                if (sfIt->second[fi] == dvar && iterStTy &&
                                                    fi < iterStTy->getNumElements()) {
                                                    auto* fld = builder_->CreateLoad(
                                                        iterStTy->getElementType(fi),
                                                        builder_->CreateStructGEP(iterStTy, iterObjAlloca, fi),
                                                        "foriter." + dvar);
                                                    auto* dAlloca = createEntryBlockAlloca(
                                                        fn, fld->getType(), dvar);
                                                    builder_->CreateStore(fld, dAlloca);
                                                    namedValues_[dvar] = dAlloca;
                                                    break;
                                                }
                                            }
                                        }
                                    }
                                }
                                emitStmt(*fe.body);
                                namedValues_ = savedVals;
                                if (auto* cb = builder_->GetInsertBlock(); cb && !cb->getTerminator())
                                    builder_->CreateBr(condBB);
                                builder_->SetInsertPoint(endBB);
                                loopStack_.pop_back();
                                break;
                            }
                        }
                    }
                }
            }

            // Direct iterator: collection expression itself has next() → Result
            {
                std::string collTypeName;
                if (auto* stTy = llvm::dyn_cast<llvm::StructType>(collTy))
                    if (stTy->hasName()) collTypeName = stTy->getName().str();
                if (!collTypeName.empty()) {
                    auto* nxFn = findClassMethod(collTypeName, "next");
                    if (nxFn) {
                        auto* retTy = nxFn->getReturnType();
                        auto* retStTy = llvm::dyn_cast<llvm::StructType>(retTy);
                        if (retStTy && retStTy->hasName() &&
                            isOptionOrResultSlotName(retStTy->getName())) {
                            auto* iterObjAlloca = createEntryBlockAlloca(fn, collTy, fe.varName + ".iter");
                            builder_->CreateStore(collVal, iterObjAlloca);
                            auto* condBB = llvm::BasicBlock::Create(*context_, "diriter.cond", fn);
                            auto* bodyBB = llvm::BasicBlock::Create(*context_, "diriter.body", fn);
                            auto* endBB = llvm::BasicBlock::Create(*context_, "diriter.end", fn);
                            loopStack_.push_back({endBB, condBB, deferStack_.size(), blockAutoDropStack_.size()});
                            builder_->CreateBr(condBB);
                            builder_->SetInsertPoint(condBB);
                            auto* result = builder_->CreateCall(nxFn, {iterObjAlloca}, "diriter.result");
                            auto* resultTmp = createEntryBlockAlloca(fn, retTy, "diriter.result.tmp");
                            builder_->CreateStore(result, resultTmp);
                            auto* tagTy2 = llvm::cast<llvm::StructType>(retTy)->getElementType(0);
                            auto* tag = builder_->CreateLoad(tagTy2,
                                builder_->CreateStructGEP(retTy, resultTmp, 0), "diriter.tag");
                            builder_->CreateCondBr(
                                builder_->CreateICmpEQ(tag, llvm::ConstantInt::get(tagTy2, 0)),
                                bodyBB, endBB);
                            builder_->SetInsertPoint(bodyBB);
                            auto* dirValFieldTy = llvm::cast<llvm::StructType>(retTy)->getElementType(1);
                            auto* dataPtr = builder_->CreateStructGEP(retTy, resultTmp, 1, "diriter.data.ptr");
                            llvm::Type* loadTy = dirValFieldTy;
                            if (dirValFieldTy->isArrayTy()) {
                                uint64_t sz = module_->getDataLayout().getTypeAllocSize(dirValFieldTy);
                                if (sz <= 1) loadTy = llvm::Type::getInt8Ty(*context_);
                                else if (sz <= 4) loadTy = llvm::Type::getInt32Ty(*context_);
                                else loadTy = llvm::Type::getInt64Ty(*context_);
                            }
                            auto* elemVal = builder_->CreateLoad(loadTy, dataPtr, "diriter.val");
                            auto* elemAlloca = createEntryBlockAlloca(fn, loadTy, fe.varName);
                            builder_->CreateStore(elemVal, elemAlloca);
                            auto savedVals = namedValues_;
                            namedValues_[fe.varName] = elemAlloca;
                            if (!fe.destructure.empty()) {
                                auto sfIt = structFieldNames_.find(collTypeName);
                                if (sfIt == structFieldNames_.end()) {
                                    auto ltPos = collTypeName.find('<');
                                    if (ltPos != std::string::npos)
                                        sfIt = structFieldNames_.find(collTypeName.substr(0, ltPos));
                                }
                                if (sfIt != structFieldNames_.end()) {
                                    auto* iterStTy = llvm::dyn_cast<llvm::StructType>(collTy);
                                    for (auto& dvar : fe.destructure) {
                                        for (size_t fi = 0; fi < sfIt->second.size(); ++fi) {
                                            if (sfIt->second[fi] == dvar && iterStTy &&
                                                fi < iterStTy->getNumElements()) {
                                                auto* fld = builder_->CreateLoad(
                                                    iterStTy->getElementType(fi),
                                                    builder_->CreateStructGEP(iterStTy, iterObjAlloca, fi),
                                                    "diriter." + dvar);
                                                auto* dAlloca = createEntryBlockAlloca(fn, fld->getType(), dvar);
                                                builder_->CreateStore(fld, dAlloca);
                                                namedValues_[dvar] = dAlloca;
                                                break;
                                            }
                                        }
                                    }
                                }
                            }
                            emitStmt(*fe.body);
                            namedValues_ = savedVals;
                            if (auto* cb = builder_->GetInsertBlock(); cb && !cb->getTerminator())
                                builder_->CreateBr(condBB);
                            builder_->SetInsertPoint(endBB);
                            loopStack_.pop_back();
                        }
                    }
                }
            }

            break;
        }
        case StmtKind::ExprStmt: {
            auto& es = static_cast<const ExprStmt&>(stmt);
            if (es.localDecl) {
                if (es.localDecl->kind == DeclKind::Function)
                    emitFunctionDecl(*es.localDecl);
                else if (es.localDecl->kind == DeclKind::Struct)
                    emitStructDecl(*es.localDecl);
                else if (es.localDecl->kind == DeclKind::Class)
                    emitClassDecl(*es.localDecl);
            }
            if (es.expr) emitExpr(*es.expr);
            break;
        }
        case StmtKind::Break:
            if (!loopStack_.empty() && loopStack_.back().breakBB) {
                emitDeferredStmts(loopStack_.back().deferStackDepth);
                // P4-D follow-up (limit #5): drop every heap-owning local
                // declared in scopes between this `break` and the loop body
                // entry — the natural block-end drop is bypassed when we
                // jump straight to the loop's exit BB.
                size_t threshold = loopStack_.back().blockStackDepthAtEntry;
                for (size_t i = blockAutoDropStack_.size(); i > threshold; --i) {
                    const auto* drops = blockAutoDropStack_[i - 1];
                    if (!drops) continue;
                    for (auto& nm : *drops) emitDropForLocal(nm);
                }
                // Limit #2: ADT-payload drops along the same scope range.
                size_t adtThreshold = std::min(threshold, adtBlockStack_.size());
                for (size_t i = adtBlockStack_.size(); i > adtThreshold; --i) {
                    const auto* adts = adtBlockStack_[i - 1];
                    if (!adts) continue;
                    for (auto& a : *adts) emitAdtPayloadDrop(a.localName, a.innerHeapTypeName, a.variantTag);
                }
                builder_->CreateBr(loopStack_.back().breakBB);
            }
            break;
        case StmtKind::Continue:
            if (!loopStack_.empty() && loopStack_.back().continueBB) {
                emitDeferredStmts(loopStack_.back().deferStackDepth);
                size_t threshold = loopStack_.back().blockStackDepthAtEntry;
                for (size_t i = blockAutoDropStack_.size(); i > threshold; --i) {
                    const auto* drops = blockAutoDropStack_[i - 1];
                    if (!drops) continue;
                    for (auto& nm : *drops) emitDropForLocal(nm);
                }
                size_t adtThreshold = std::min(threshold, adtBlockStack_.size());
                for (size_t i = adtBlockStack_.size(); i > adtThreshold; --i) {
                    const auto* adts = adtBlockStack_[i - 1];
                    if (!adts) continue;
                    for (auto& a : *adts) emitAdtPayloadDrop(a.localName, a.innerHeapTypeName, a.variantTag);
                }
                builder_->CreateBr(loopStack_.back().continueBB);
            }
            break;
        case StmtKind::Unsafe:
            if (stmt.as<UnsafeStmt>()->body) emitBlock(*stmt.as<UnsafeStmt>()->body);
            break;
        default: break;
    }
}

void CodeGen::emitBlock(const Stmt& block) {
    auto& bs = static_cast<const BlockStmt&>(block);
    auto savedClassVars = classVarTypes_;
    std::vector<std::string> blockClassVars;
    deferStack_.push_back({});

    // P4-D follow-up (limit #5): publish this block's auto-drop list so a
    // `return` / `break` / `continue` deeper in the body can walk every
    // enclosing scope and emit the drop calls before its terminator.
    blockAutoDropStack_.push_back(&bs.autoDropLocals);
    // Companion stack for ADT-payload drops (limit #2).
    adtBlockStack_.push_back(&bs.autoDropAdtPayloads);

    for (auto& s : bs.statements) {
        if (s) {
            if (s->kind == StmtKind::Defer) {
                if (s->as<DeferStmt>()->body) deferStack_.back().push_back(s->as<DeferStmt>()->body.get());
                continue;
            }
            emitStmt(*s);
            for (auto& [name, cls] : classVarTypes_) {
                if (savedClassVars.find(name) == savedClassVars.end()) {
                    blockClassVars.push_back(name);
                }
            }
            // Track Ref<T> variables for auto-release
            auto rcIt = structTypes_.find("__RefCounted");
            if (rcIt != structTypes_.end()) {
                for (auto& [name, val] : namedValues_) {
                    if (savedClassVars.count(name) || classVarTypes_.count(name)) continue;
                    auto* ty = getValuePtrType(val);
                    if (ty == rcIt->second) {
                        classVarTypes_[name] = "__RefCounted";
                        blockClassVars.push_back(name);
                    }
                }
            }
        }
        if (builder_->GetInsertBlock() && builder_->GetInsertBlock()->getTerminator())
            break;
    }

    // Execute deferred statements in reverse order (LIFO) for natural block exit
    auto* curBB2 = builder_->GetInsertBlock();
    if (curBB2 && !curBB2->getTerminator()) {
        // Snapshot the frame before iterating (see emitDeferredStmts for
        // the iterator-invalidation rationale — nested blocks push to
        // deferStack_ and can realloc the outer vector).
        auto deferSnap = deferStack_.back();
        for (auto it = deferSnap.rbegin(); it != deferSnap.rend(); ++it) {
            emitStmt(**it);
        }
    }
    deferStack_.pop_back();

    // P4-C scope-aware auto-drop: emit `.drop()` calls for every heap-owning
    // local that Sema's analyzeBlock identified as "alive at block end"
    // (not moved, not escaped via return). Runs BEFORE the block's natural
    // terminator; skipped when the block already produced a terminator
    // (early return / break / continue — those have already been intercepted
    // by emitReturnStmt / Break / Continue which walk blockAutoDropStack_
    // and drop every enclosing scope's locals first).
    auto* dropBB = builder_->GetInsertBlock();
    if (dropBB && !dropBB->getTerminator()) {
        for (auto& localName : bs.autoDropLocals) {
            emitDropForLocal(localName);
        }
        // P4-D follow-up (limit #2): tag-conditional payload drops for
        // ADT locals (Option<H>/Result<H,_>/Result<_,H> with heap-owning H).
        for (auto& adt : bs.autoDropAdtPayloads) {
            emitAdtPayloadDrop(adt.localName, adt.innerHeapTypeName, adt.variantTag);
        }
    }

    // Pop this block's auto-drop list off the enclosing-scope stack.
    if (!blockAutoDropStack_.empty() && blockAutoDropStack_.back() == &bs.autoDropLocals) {
        blockAutoDropStack_.pop_back();
    }
    if (!adtBlockStack_.empty() && adtBlockStack_.back() == &bs.autoDropAdtPayloads) {
        adtBlockStack_.pop_back();
    }

    // Restore: remove block-scoped class vars from tracking
    for (auto& name : blockClassVars) {
        classVarTypes_.erase(name);
    }
}

bool CodeGen::emitDropForLocal(const std::string& localName) {
    auto nvIt = namedValues_.find(localName);
    if (nvIt == namedValues_.end()) return false;
    auto* alloca = nvIt->second;
    if (!alloca) return false;
    auto* allocaTy = getValuePtrType(alloca);
    auto* structTy = allocaTy ? llvm::dyn_cast<llvm::StructType>(allocaTy) : nullptr;
    if (!structTy || !structTy->hasName()) return false;
    std::string typeName = structTy->getName().str();
    auto dropIt = functions_.find(typeName + ".drop");
    // P4-D.2: `Ref<T>` allocas lower to the shared `__RefCounted` struct,
    // so the literal name lookup above misses the real drop. Route through
    // any `Ref<*>.drop` since they all share the __RefCounted self layout.
    if (dropIt == functions_.end() && typeName == "__RefCounted") {
        std::string prefix = "Ref<";
        std::string suffix = ">.drop";
        for (auto it = functions_.begin(); it != functions_.end(); ++it) {
            auto& fn_name = it->first;
            if (fn_name.size() > prefix.size() + suffix.size() &&
                fn_name.compare(0, prefix.size(), prefix) == 0 &&
                fn_name.compare(fn_name.size() - suffix.size(),
                                suffix.size(), suffix) == 0) {
                dropIt = it;
                break;
            }
        }
    }
    if (dropIt == functions_.end()) return false;
    builder_->CreateCall(dropIt->second, {alloca});
    return true;
}

bool CodeGen::emitAdtPayloadDrop(const std::string& localName,
                                 const std::string& innerHeapTypeName,
                                 int variantTag) {
    auto nvIt = namedValues_.find(localName);
    if (nvIt == namedValues_.end()) return false;
    auto* alloca = nvIt->second;
    if (!alloca) return false;
    auto* allocaTy = getValuePtrType(alloca);
    auto* structTy = allocaTy ? llvm::dyn_cast<llvm::StructType>(allocaTy) : nullptr;
    if (!structTy || structTy->getNumElements() < 2) return false;
    // Resolve inner drop function — try a literal `H.drop` first, then
    // (for Ref<T>) walk Ref<*>.drop entries since all Ref instantiations
    // share the __RefCounted layout.
    auto dropIt = functions_.find(innerHeapTypeName + ".drop");
    if (dropIt == functions_.end()) {
        // Strip generic args ("Ref<i64>" -> "Ref"-prefix scan).
        auto lt = innerHeapTypeName.find('<');
        std::string base = lt == std::string::npos
            ? innerHeapTypeName
            : innerHeapTypeName.substr(0, lt);
        std::string prefix = base + "<";
        std::string suffix = ">.drop";
        for (auto it = functions_.begin(); it != functions_.end(); ++it) {
            auto& fn_name = it->first;
            if (fn_name.size() > prefix.size() + suffix.size() &&
                fn_name.compare(0, prefix.size(), prefix) == 0 &&
                fn_name.compare(fn_name.size() - suffix.size(),
                                suffix.size(), suffix) == 0) {
                dropIt = it;
                break;
            }
        }
    }
    if (dropIt == functions_.end()) return false;

    auto* fn = builder_->GetInsertBlock()->getParent();
    auto* tagTy = structTy->getElementType(0);
    auto* tagPtr = builder_->CreateStructGEP(structTy, alloca, 0, "adt.tag.ptr");
    auto* tagVal = builder_->CreateLoad(tagTy, tagPtr, "adt.tag");
    auto* expectedTag = llvm::ConstantInt::get(tagTy, variantTag);
    auto* cond = builder_->CreateICmpEQ(tagVal, expectedTag, "adt.tag.eq");
    auto* dropBB = llvm::BasicBlock::Create(*context_, "adt.payload.drop", fn);
    auto* skipBB = llvm::BasicBlock::Create(*context_, "adt.payload.skip", fn);
    builder_->CreateCondBr(cond, dropBB, skipBB);
    builder_->SetInsertPoint(dropBB);
    // Inner type's `self` is a pointer to its struct; the payload slot
    // (field 1) starts at offset 8 in the ADT layout. Either the slot
    // type matches Inner's struct directly (when sizeof(Inner) ≤ 8 and
    // shared __Result is used) or it's a [N x i8] byte array sized to
    // sizeof(Inner). Either way, the address GEP into field 1 is the
    // correct `self` for the inner drop.
    auto* payloadPtr = builder_->CreateStructGEP(structTy, alloca, 1, "adt.payload.ptr");
    builder_->CreateCall(dropIt->second, {payloadPtr});
    builder_->CreateBr(skipBB);
    builder_->SetInsertPoint(skipBB);
    return true;
}

void CodeGen::emitDeferredStmts(size_t fromDepth) {
    for (size_t i = deferStack_.size(); i > fromDepth; --i) {
        // Copy the frame by value: `emitStmt(...)` below can descend into
        // a nested block (defer body is frequently `{ ... }`) which in
        // turn calls `deferStack_.push_back({})`. If the push reallocates
        // the outer vector, the `defers` reference below dangles and the
        // reverse iterator reads past the end into freed memory — the
        // symptom is a runaway emission loop where every other ptr is
        // garbage, eventually segfaulting the compiler.
        auto defers = deferStack_[i - 1];
        for (auto it = defers.rbegin(); it != defers.rend(); ++it) {
            emitStmt(**it);
        }
    }
}

} // namespace vyx
