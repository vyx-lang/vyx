#include "CodeGenIncludes.h"

namespace vyx {

void CodeGen::emitMatchStmt(const Stmt& stmt) {
    auto& ms = static_cast<const MatchStmt&>(stmt);
    auto* fn = builder_->GetInsertBlock()->getParent();
    auto* matchVal = ms.expr ? emitExpr(*ms.expr) : nullptr;
    if (!matchVal) return;

    // Helper: find ADT type from arm labels (uses ALL labels to disambiguate)
    auto findADTFromArms = [&]() -> std::pair<std::string, llvm::StructType*> {
        std::vector<std::string> armLabels;
        for (auto& arm : ms.arms) {
            if (!arm.isDefault && !arm.label.empty()) armLabels.push_back(arm.label);
        }
        if (armLabels.empty()) return {"", nullptr};

        // Collect all candidate ADT names that contain the first label
        std::vector<std::string> candidates;
        for (auto& [key, tagVal] : errorEnumValues_) {
            auto dotPos = key.rfind('.');
            if (dotPos != std::string::npos && key.substr(dotPos + 1) == armLabels[0]) {
                std::string adtName = key.substr(0, dotPos);
                auto stIt = structTypes_.find(adtName);
                if (stIt != structTypes_.end()) {
                    auto fnIt = structFieldNames_.find(adtName);
                    if (fnIt != structFieldNames_.end() && fnIt->second.size() >= 2 &&
                        fnIt->second[0] == "__tag" && fnIt->second[1] == "__data") {
                        candidates.push_back(adtName);
                    }
                }
            }
        }

        if (candidates.size() <= 1) {
            if (candidates.empty()) return {"", nullptr};
            auto stIt = structTypes_.find(candidates[0]);
            return {candidates[0], stIt->second};
        }

        // Disambiguate: find the ADT that contains ALL arm labels
        for (auto& cand : candidates) {
            bool allMatch = true;
            for (size_t li = 1; li < armLabels.size() && allMatch; ++li) {
                if (errorEnumValues_.find(cand + "." + armLabels[li]) == errorEnumValues_.end())
                    allMatch = false;
            }
            if (allMatch) {
                auto stIt = structTypes_.find(cand);
                return {cand, stIt->second};
            }
        }

        // Fallback: return first candidate
        auto stIt = structTypes_.find(candidates[0]);
        return {candidates[0], stIt->second};
    };

    // If matchVal is a pointer to an ADT struct (e.g. self in generic enum methods),
    // dereference it to get the struct value for proper pattern matching.
    if (matchVal->getType()->isPointerTy() && !currentClassName_.empty()) {
        auto stIt = structTypes_.find(currentClassName_);
        if (stIt != structTypes_.end()) {
            auto fnIt = structFieldNames_.find(currentClassName_);
            if (fnIt != structFieldNames_.end() && fnIt->second.size() >= 2 &&
                fnIt->second[0] == "__tag" && fnIt->second[1] == "__data") {
                matchVal = builder_->CreateLoad(stIt->second, matchVal, "match.deref");
            }
        }
    }

    // Handle match on pointer-to-ADT (Box<ADT> or raw pointer)
    if (matchVal->getType()->isPointerTy()) {
        auto [adtName, adtTy] = findADTFromArms();
        if (adtTy) {
            matchVal = builder_->CreateLoad(adtTy, matchVal, "match.ptr.adt");
        }
    }

    // Handle match on Ref<ADT> (__RefCounted wrapper)
    if (auto* wrapStTy = llvm::dyn_cast<llvm::StructType>(matchVal->getType())) {
        std::string stName = wrapStTy->hasName() ? wrapStTy->getName().str() : "";
        if (stName == "__RefCounted") {
            auto [adtName, adtTy] = findADTFromArms();
            if (adtTy) {
                auto* refAlloca = createEntryBlockAlloca(fn, wrapStTy, "match.ref.tmp");
                builder_->CreateStore(matchVal, refAlloca);
                auto* dataPtrField = builder_->CreateStructGEP(wrapStTy, refAlloca, 0, "ref.data.ptr");
                auto* rawPtr = builder_->CreateLoad(llvm::PointerType::getUnqual(*context_), dataPtrField, "ref.raw.ptr");
                matchVal = builder_->CreateLoad(adtTy, rawPtr, "match.ref.adt");
            }
        }
    }

    // Fix: match on integer from bad deref — re-emit as ADT if arms suggest it
    if (matchVal->getType()->isIntegerTy() && ms.expr && ms.expr->kind == ExprKind::UnaryOp) {
        auto* unE = ms.expr->as<const UnaryOpExpr>();
        if (unE->op == UnaryOp::Deref && unE->operand) {
            auto [adtName, adtTy] = findADTFromArms();
            if (adtTy) {
                auto* ptrVal = emitExpr(*unE->operand);
                if (ptrVal && ptrVal->getType()->isPointerTy()) {
                    matchVal = builder_->CreateLoad(adtTy, ptrVal, "match.fixderef.adt");
                }
            }
        }
    }

    // Fix: integer matchVal but arms reference a known ADT — reload from the
    // scrutinee's alloca using the ADT struct type. This covers patterns like
    //   let s = *box.get(i);    // s was emitted as int (ptrElemTypes missing)
    //   match (s) { case Variant(bind) => ... }
    // where the arm labels disambiguate which ADT to use.
    if (matchVal->getType()->isIntegerTy() && ms.expr && ms.expr->kind == ExprKind::Identifier) {
        auto [adtName, adtTy] = findADTFromArms();
        if (adtTy) {
            auto& ident = *ms.expr->as<const IdentifierExpr>();
            auto nvIt = namedValues_.find(ident.name);
            if (nvIt != namedValues_.end() && nvIt->second) {
                matchVal = builder_->CreateLoad(adtTy, nvIt->second, "match.adtreload");
            }
        }
    }


    // Handle match on Box<ADT> (raw pointer to heap-allocated ADT)
    if (matchVal->getType()->isPointerTy()) {
        auto [adtName, adtTy] = findADTFromArms();
        if (adtTy) {
            matchVal = builder_->CreateLoad(adtTy, matchVal, "match.box.adt");
        }
    }

    auto* mergeBB = llvm::BasicBlock::Create(*context_, "match.end", fn);

    llvm::BasicBlock* defaultBB = mergeBB;

    // Find default arm
    for (auto& arm : ms.arms) {
        if (arm.isDefault) {
            defaultBB = llvm::BasicBlock::Create(*context_, "match.default", fn);
            break;
        }
    }

    // Type-based pattern matching: case val: Type
    bool hasTypePatterns = false;
    for (auto& arm : ms.arms) {
        if (arm.typePattern) { hasTypePatterns = true; break; }
    }
    if (hasTypePatterns) {
        llvm::Type* matchLLVMType = matchVal->getType();
        bool anyMatched = false;

        for (size_t i = 0; i < ms.arms.size(); ++i) {
            auto& arm = ms.arms[i];
            if (arm.isDefault) continue;
            if (!arm.typePattern) continue;

            llvm::Type* patternType = toLLVMType(*arm.typePattern);
            bool typesMatch = (matchLLVMType == patternType);
            if (!typesMatch && matchLLVMType->isIntegerTy() && patternType->isIntegerTy()) {
                typesMatch = (matchLLVMType->getIntegerBitWidth() == patternType->getIntegerBitWidth());
            }
            if (!typesMatch && matchLLVMType->isStructTy() && patternType->isStructTy()) {
                typesMatch = (matchLLVMType == patternType);
            }

            if (typesMatch) {
                anyMatched = true;
                auto* caseBB = llvm::BasicBlock::Create(*context_, "match.type." + arm.label, fn);
                builder_->CreateBr(caseBB);
                builder_->SetInsertPoint(caseBB);

                if (!arm.label.empty()) {
                    auto* alloca = createEntryBlockAlloca(fn, matchLLVMType, arm.label);
                    builder_->CreateStore(matchVal, alloca);
                    namedValues_[arm.label] = alloca;
                }
                if (arm.body) emitStmt(*arm.body);
                if (builder_->GetInsertBlock() && !builder_->GetInsertBlock()->getTerminator())
                    builder_->CreateBr(mergeBB);
                break;
            }
        }

        if (!anyMatched) {
            bool hasDefault = false;
            for (auto& arm : ms.arms) {
                if (!arm.isDefault) continue;
                hasDefault = true;
                if (defaultBB != mergeBB) {
                    builder_->CreateBr(defaultBB);
                    builder_->SetInsertPoint(defaultBB);
                    if (arm.body) emitStmt(*arm.body);
                    if (builder_->GetInsertBlock() && !builder_->GetInsertBlock()->getTerminator())
                        builder_->CreateBr(mergeBB);
                } else {
                    builder_->CreateBr(mergeBB);
                }
                break;
            }
            if (!hasDefault && builder_->GetInsertBlock() && !builder_->GetInsertBlock()->getTerminator())
                builder_->CreateBr(mergeBB);
        }
        // If default BB was created but never used (type matched), remove it
        if (anyMatched && defaultBB != mergeBB && defaultBB->hasNPredecessors(0)) {
            defaultBB->eraseFromParent();
        }

        builder_->SetInsertPoint(mergeBB);
        return;
    }

    // Check if ANY non-default arm has a guard (need if/elif chain instead of switch)
    bool hasAnyGuard = false;
    for (auto& arm : ms.arms) {
        if (!arm.isDefault && arm.guardExpr) { hasAnyGuard = true; break; }
    }

    // For integer match with any guarded arm: use if/elif chain so that every
    // arm is tested in source order.  This also handles mixed patterns where
    // some arms have guards and others have plain value patterns (case 0 =>).
    if (matchVal->getType()->isIntegerTy() && hasAnyGuard) {
        for (size_t i = 0; i < ms.arms.size(); ++i) {
            auto& arm = ms.arms[i];
            if (arm.isDefault) continue;
            auto* caseBB = llvm::BasicBlock::Create(*context_, "match.guard." + arm.label, fn);
            auto* nextBB = llvm::BasicBlock::Create(*context_, "match.guard.next", fn);

            if (arm.guardExpr) {
                // Guarded arm: bind the label variable so the guard expression
                // can reference it, then branch based on the guard result.
                if (!arm.label.empty()) {
                    auto* alloca = createEntryBlockAlloca(fn, matchVal->getType(), arm.label);
                    builder_->CreateStore(matchVal, alloca);
                    namedValues_[arm.label] = alloca;
                }
                auto* guardVal = emitExpr(*arm.guardExpr);
                if (guardVal) {
                    auto* isTrue = coerceToBool(guardVal, "match.guard");
                    builder_->CreateCondBr(isTrue, caseBB, nextBB);
                } else {
                    builder_->CreateBr(caseBB);
                }
            } else if (arm.valuePattern) {
                // Value-pattern arm (e.g. case 0 =>): equality test.
                auto* patVal = emitExpr(*arm.valuePattern);
                if (patVal) {
                    // Widen/narrow constant to match scrutinee type.
                    if (patVal->getType() != matchVal->getType()) {
                        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(patVal)) {
                            patVal = llvm::ConstantInt::get(
                                llvm::cast<llvm::IntegerType>(matchVal->getType()),
                                ci->getSExtValue(), true);
                        }
                    }
                    auto* isEq = builder_->CreateICmpEQ(matchVal, patVal, "match.vpeq");
                    builder_->CreateCondBr(isEq, caseBB, nextBB);
                } else {
                    builder_->CreateBr(nextBB);
                }
            } else {
                // Plain binding arm (no guard, no value pattern): always matches,
                // bind the label and fall through unconditionally.
                if (!arm.label.empty()) {
                    auto* alloca = createEntryBlockAlloca(fn, matchVal->getType(), arm.label);
                    builder_->CreateStore(matchVal, alloca);
                    namedValues_[arm.label] = alloca;
                }
                builder_->CreateBr(caseBB);
                // nextBB is unreachable after an unconditional arm; leave it
                // for the default/merge connection below but it will be dead.
            }

            builder_->SetInsertPoint(caseBB);
            if (arm.body) emitStmt(*arm.body);
            if (builder_->GetInsertBlock() && !builder_->GetInsertBlock()->getTerminator())
                builder_->CreateBr(mergeBB);
            builder_->SetInsertPoint(nextBB);
        }
        bool hasDefault = false;
        for (auto& arm : ms.arms) {
            if (!arm.isDefault) continue;
            hasDefault = true;
            if (defaultBB != mergeBB) {
                if (builder_->GetInsertBlock() && !builder_->GetInsertBlock()->getTerminator())
                    builder_->CreateBr(defaultBB);
                builder_->SetInsertPoint(defaultBB);
            }
            if (arm.body) emitStmt(*arm.body);
            if (builder_->GetInsertBlock() && !builder_->GetInsertBlock()->getTerminator())
                builder_->CreateBr(mergeBB);
        }
        if (!hasDefault && defaultBB != mergeBB && defaultBB->hasNPredecessors(0)) {
            defaultBB->eraseFromParent();
        }
        if (builder_->GetInsertBlock() && !builder_->GetInsertBlock()->getTerminator())
            builder_->CreateBr(mergeBB);
        builder_->SetInsertPoint(mergeBB);
        return;
    }

    // For integer match without guards: use switch
    if (matchVal->getType()->isIntegerTy()) {
        auto* switchInst = builder_->CreateSwitch(matchVal, defaultBB, ms.arms.size());

        for (auto& arm : ms.arms) {
            if (arm.isDefault) continue;

            auto* caseBB = llvm::BasicBlock::Create(*context_, "match.case." + arm.label, fn);

            // If there's a value pattern
            if (arm.valuePattern) {
                auto* caseVal = emitExpr(*arm.valuePattern);
                if (caseVal) {
                    auto* ci = llvm::dyn_cast<llvm::ConstantInt>(caseVal);
                    if (ci && ci->getType() != matchVal->getType()) {
                        ci = llvm::ConstantInt::get(
                            llvm::cast<llvm::IntegerType>(matchVal->getType()),
                            ci->getSExtValue());
                    }
                    if (ci) switchInst->addCase(ci, caseBB);
                }
            } else if (!arm.label.empty() && !arm.guardExpr) {
                for (auto& d : unit_->declarations) {
                    if (!d || d->kind != DeclKind::ErrorDef) continue;
                    auto* errDecl = d->as<ErrorDefDecl>();
                    bool hasData = false;
                    for (auto& vt : errDecl->variantTypes) {
                        if (!vt.empty()) { hasData = true; break; }
                    }
                    if (hasData) continue;
                    auto evIt = errorEnumValues_.find(d->name + "." + arm.label);
                    if (evIt != errorEnumValues_.end()) {
                        auto* ci = llvm::ConstantInt::get(
                            llvm::cast<llvm::IntegerType>(matchVal->getType()),
                            evIt->second, true);
                        switchInst->addCase(ci, caseBB);
                        break;
                    }
                }
            }

            builder_->SetInsertPoint(caseBB);
            // Bind the label variable BEFORE guard evaluation
            if (!arm.label.empty() && !arm.valuePattern) {
                auto* bindAlloca = createEntryBlockAlloca(fn, matchVal->getType(), arm.label);
                builder_->CreateStore(matchVal, bindAlloca);
                namedValues_[arm.label] = bindAlloca;
            }
            auto armNeedsPayloadBindings = [](const MatchArm& a) {
                if (!a.bindingName.empty()) return true;
                if (!a.tupleBindings.empty()) return true;
                for (auto& np : a.nestedPatterns) {
                    if (!np.isTuple) {
                        if (!np.name.empty()) return true;
                    } else {
                        for (auto& c : np.children)
                            if (!c.name.empty()) return true;
                    }
                }
                return false;
            };
            if (armNeedsPayloadBindings(arm)) {
                diag_.error(ms.location,
                    "match on integer tag with payload bindings is not supported in codegen (scrutinee must be the ADT value type, not a bare tag); arm `{}`",
                    arm.label);
            }
            // Guard clause: check condition before executing body
            if (arm.guardExpr) {
                auto* guardVal = emitExpr(*arm.guardExpr);
                if (guardVal) {
                    auto* isTrue = coerceToBool(guardVal, "match.guard");
                    auto* guardPassBB = llvm::BasicBlock::Create(*context_, "match.guard.pass", fn);
                    builder_->CreateCondBr(isTrue, guardPassBB, defaultBB);
                    builder_->SetInsertPoint(guardPassBB);
                }
            }
            if (arm.body) emitStmt(*arm.body);
            if (!builder_->GetInsertBlock()->getTerminator())
                builder_->CreateBr(mergeBB);
        }

        // Emit default
        for (auto& arm : ms.arms) {
            if (!arm.isDefault) continue;
            builder_->SetInsertPoint(defaultBB);
            if (arm.body) emitStmt(*arm.body);
            if (!builder_->GetInsertBlock()->getTerminator())
                builder_->CreateBr(mergeBB);
        }
    } else if (matchVal->getType() == getOrCreateStringType()) {
        // String pattern matching: case "value" => ...
        auto* strTy = getOrCreateStringType();
        auto* ptrTy = llvm::PointerType::getUnqual(*context_);
        auto* i32Ty = llvm::Type::getInt32Ty(*context_);
        auto* matchAlloca = createEntryBlockAlloca(fn, strTy, "match.str");
        builder_->CreateStore(matchVal, matchAlloca);
        auto* matchPtr = builder_->CreateLoad(ptrTy,
            builder_->CreateStructGEP(strTy, matchAlloca, 0), "match.str.ptr");

        auto* strcmpFn = module_->getFunction("strcmp");
        if (!strcmpFn) {
            auto* fty = llvm::FunctionType::get(i32Ty, {ptrTy, ptrTy}, false);
            strcmpFn = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, "strcmp", *module_);
        }

        for (size_t i = 0; i < ms.arms.size(); ++i) {
            auto& arm = ms.arms[i];
            if (arm.isDefault) continue;

            auto* caseBB = llvm::BasicBlock::Create(*context_, "match.str." + arm.label, fn);
            auto* nextBB = (i + 1 < ms.arms.size() || defaultBB != mergeBB)
                ? llvm::BasicBlock::Create(*context_, "match.str.next", fn) : mergeBB;

            if (arm.valuePattern) {
                auto* patternVal = emitExpr(*arm.valuePattern);
                if (patternVal) {
                    auto* patternPtr = extractStringPtr(patternVal);
                    auto* cmpResult = builder_->CreateCall(strcmpFn, {matchPtr, patternPtr}, "str.cmp");
                    auto* isMatch = builder_->CreateICmpEQ(cmpResult,
                        llvm::ConstantInt::get(i32Ty, 0), "str.eq");
                    builder_->CreateCondBr(isMatch, caseBB, nextBB);
                }
            } else if (!arm.label.empty()) {
                auto* labelStr = getOrCreateString(arm.label);
                auto* cmpResult = builder_->CreateCall(strcmpFn, {matchPtr, labelStr}, "str.cmp");
                auto* isMatch = builder_->CreateICmpEQ(cmpResult,
                    llvm::ConstantInt::get(i32Ty, 0), "str.eq");
                builder_->CreateCondBr(isMatch, caseBB, nextBB);
            } else {
                builder_->CreateBr(caseBB);
            }

            builder_->SetInsertPoint(caseBB);
            if (!arm.bindingName.empty()) {
                auto* bindAlloca = createEntryBlockAlloca(fn, strTy, arm.bindingName);
                builder_->CreateStore(matchVal, bindAlloca);
                namedValues_[arm.bindingName] = bindAlloca;
            }
            if (arm.body) emitStmt(*arm.body);
            if (builder_->GetInsertBlock() && !builder_->GetInsertBlock()->getTerminator())
                builder_->CreateBr(mergeBB);

            if (nextBB != mergeBB)
                builder_->SetInsertPoint(nextBB);
        }

        if (builder_->GetInsertBlock() && !builder_->GetInsertBlock()->getTerminator()) {
            if (defaultBB != mergeBB)
                builder_->CreateBr(defaultBB);
            else
                builder_->CreateBr(mergeBB);
        }

        for (auto& arm : ms.arms) {
            if (!arm.isDefault) continue;
            if (defaultBB != mergeBB) {
                builder_->SetInsertPoint(defaultBB);
                if (arm.body) emitStmt(*arm.body);
                if (builder_->GetInsertBlock() && !builder_->GetInsertBlock()->getTerminator())
                    builder_->CreateBr(mergeBB);
            }
        }

        builder_->SetInsertPoint(mergeBB);
        return;
    } else if (auto* matchStTy = llvm::dyn_cast<llvm::StructType>(matchVal->getType())) {
        auto fnIt = structFieldNames_.find(matchStTy->hasName() ? matchStTy->getName().str() : "");
        bool isADT = fnIt != structFieldNames_.end() && fnIt->second.size() >= 2 &&
                     fnIt->second[0] == "__tag" && fnIt->second[1] == "__data";

        if (isADT) {
            std::string enumName = matchStTy->getName().str();
            auto* adtAlloca = createEntryBlockAlloca(fn, matchStTy, "match.adt");
            builder_->CreateStore(matchVal, adtAlloca);
            auto* tagPtr = builder_->CreateStructGEP(matchStTy, adtAlloca, 0, "adt.tag.ptr");
            auto* tagVal = builder_->CreateLoad(llvm::Type::getInt32Ty(*context_), tagPtr, "adt.tag");
            auto* dataPtr = builder_->CreateStructGEP(matchStTy, adtAlloca, 1, "adt.data.ptr");

            auto* switchInst = builder_->CreateSwitch(tagVal, defaultBB, ms.arms.size());

            for (size_t i = 0; i < ms.arms.size(); ++i) {
                auto& arm = ms.arms[i];
                if (arm.isDefault) continue;

                auto evIt = errorEnumValues_.find(enumName + "." + arm.label);
                if (evIt == errorEnumValues_.end()) {
                    if (arm.label == "ok") evIt = errorEnumValues_.find(enumName + ".Ok");
                    else if (arm.label == "fail" || arm.label == "err") evIt = errorEnumValues_.find(enumName + ".Err");
                    else if (arm.label == "some") evIt = errorEnumValues_.find(enumName + ".Some");
                    else if (arm.label == "none") evIt = errorEnumValues_.find(enumName + ".None");
                }
                if (evIt == errorEnumValues_.end()) continue;

                auto* caseBB = llvm::BasicBlock::Create(*context_, "match.adt." + arm.label, fn);
                switchInst->addCase(
                    llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), evIt->second), caseBB);

                builder_->SetInsertPoint(caseBB);

                // Bind ADT variant data fields (with nested pattern support)
                auto bindADTFields = [&](const std::vector<std::string>& bindings,
                                         const std::vector<NestedPattern>& nestedPats) {
                    if (bindings.empty() && arm.bindingName.empty() && nestedPats.empty()) return;
                    if (!unit_) return;
                    std::string enumBaseName = enumName;
                    if (auto pos = enumBaseName.find('<'); pos != std::string::npos)
                        enumBaseName = enumBaseName.substr(0, pos);
                    for (auto& d : unit_->declarations) {
                        if (!d || d->kind != DeclKind::ErrorDef || d->name != enumBaseName) continue;
                        auto savedGP = genericTypeParams_;
                        auto savedGPNames = genericTypeParamNames_;
                        if (!d->genericParams.empty()) {
                            auto ltPos = enumName.find('<');
                            if (ltPos != std::string::npos) {
                                std::string inner = enumName.substr(ltPos + 1, enumName.size() - ltPos - 2);
                                std::vector<std::string> tpStrs;
                                size_t start2 = 0; int depth2 = 0;
                                for (size_t ci = 0; ci < inner.size(); ++ci) {
                                    if (inner[ci] == '<') ++depth2;
                                    else if (inner[ci] == '>') --depth2;
                                    else if (inner[ci] == ',' && depth2 == 0) {
                                        tpStrs.push_back(inner.substr(start2, ci - start2));
                                        start2 = ci + 1;
                                    }
                                }
                                tpStrs.push_back(inner.substr(start2));
                                for (size_t gi = 0; gi < d->genericParams.size() && gi < tpStrs.size(); ++gi) {
                                    TypeAnnotation ca; ca.kind = TypeAnnotationKind::Named; ca.name = tpStrs[gi];
                                    genericTypeParams_[d->genericParams[gi]] = toLLVMType(ca);
                                    genericTypeParamNames_[d->genericParams[gi]] = tpStrs[gi];
                                }
                            }
                        }
                        for (size_t vi = 0; vi < d->as<ErrorDefDecl>()->variants.size(); ++vi) {
                            std::string resolvedLabel = arm.label;
                            if (resolvedLabel == "ok") resolvedLabel = "Ok";
                            else if (resolvedLabel == "fail" || resolvedLabel == "err") resolvedLabel = "Err";
                            else if (resolvedLabel == "some") resolvedLabel = "Some";
                            else if (resolvedLabel == "none") resolvedLabel = "None";
                            if (d->as<ErrorDefDecl>()->variants[vi] != arm.label && d->as<ErrorDefDecl>()->variants[vi] != resolvedLabel) continue;
                            if (d->as<ErrorDefDecl>()->variantTypes[vi].empty()) break;
                            uint64_t offset = 0;
                            size_t numFields = d->as<ErrorDefDecl>()->variantTypes[vi].size();
                            size_t numBindings = bindings.empty()
                                ? (nestedPats.empty() ? 1 : nestedPats.size())
                                : bindings.size();
                            for (size_t bi = 0; bi < numBindings && bi < numFields; ++bi) {
                                auto* bindTy = toLLVMType(*d->as<ErrorDefDecl>()->variantTypes[vi][bi]);
                                auto* fieldPtr = builder_->CreateGEP(
                                    llvm::Type::getInt8Ty(*context_), dataPtr,
                                    llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), offset), "adt.bind.ptr");
                                auto* bindVal = builder_->CreateLoad(bindTy, fieldPtr, "adt.field");

                                // Nested pattern: destructure inner struct
                                if (bi < nestedPats.size() && nestedPats[bi].isTuple) {
                                    auto& nested = nestedPats[bi];
                                    if (auto* innerStTy = llvm::dyn_cast<llvm::StructType>(bindTy)) {
                                        auto* innerAlloca = createEntryBlockAlloca(fn, innerStTy, nested.name + ".tmp");
                                        builder_->CreateStore(bindVal, innerAlloca);
                                        std::string innerName = innerStTy->hasName() ? innerStTy->getName().str() : nested.name;
                                        auto sfnIt = structFieldNames_.find(innerName);
                                        for (size_t ci = 0; ci < nested.children.size(); ++ci) {
                                            if (sfnIt != structFieldNames_.end() && ci < sfnIt->second.size()) {
                                                auto* gep = builder_->CreateStructGEP(innerStTy, innerAlloca, ci, nested.children[ci].name);
                                                auto* childVal = builder_->CreateLoad(innerStTy->getElementType(ci), gep, nested.children[ci].name);
                                                auto* childAlloca = createEntryBlockAlloca(fn, innerStTy->getElementType(ci), nested.children[ci].name);
                                                builder_->CreateStore(childVal, childAlloca);
                                                namedValues_[nested.children[ci].name] = childAlloca;
                                            } else if (ci < innerStTy->getNumElements()) {
                                                auto* gep = builder_->CreateStructGEP(innerStTy, innerAlloca, ci, nested.children[ci].name);
                                                auto* childVal = builder_->CreateLoad(innerStTy->getElementType(ci), gep, nested.children[ci].name);
                                                auto* childAlloca = createEntryBlockAlloca(fn, innerStTy->getElementType(ci), nested.children[ci].name);
                                                builder_->CreateStore(childVal, childAlloca);
                                                namedValues_[nested.children[ci].name] = childAlloca;
                                            }
                                        }
                                    }
                                } else {
                                    std::string bName;
                                    if (!bindings.empty()) bName = bindings[bi];
                                    else if (bi < nestedPats.size()) bName = nestedPats[bi].name;
                                    else bName = arm.bindingName;
                                    auto* bindAlloca = createEntryBlockAlloca(fn, bindTy, bName);
                                    builder_->CreateStore(bindVal, bindAlloca);
                                    namedValues_[bName] = bindAlloca;
                                    if (auto* stTy = llvm::dyn_cast<llvm::StructType>(bindTy)) {
                                        if (stTy->hasName())
                                            classVarTypes_[bName] = stTy->getName().str();
                                    }
                                    if (bindTy->isPointerTy() && !bName.empty()) {
                                        auto& variantTypeAnn = *d->as<ErrorDefDecl>()->variantTypes[vi][bi];
                                        // BUG-LV-04 — when the bound variant is `Box<T>` (or any
                                        // smart-pointer collapsed to a bare ptr), tag the binder
                                        // with the canonical class name so subsequent method
                                        // dispatch routes through the real `Box<T>.deref` body
                                        // (which knows to chase Box.data → heap → load T).  Without
                                        // this, CodeGenMethodCall.cpp's deref shortcut treats
                                        // `boxPtr` as a direct heap-T pointer and emits a single
                                        // `load i64, ptr %boxPtr`, which actually reads Box.data
                                        // (the inner pointer bytes) — yielding a garbage address
                                        // instead of the dereferenced value.
                                        if (variantTypeAnn.name == "Box" &&
                                            variantTypeAnn.kind == TypeAnnotationKind::Generic) {
                                            std::string boxMangled = mangleTypeAnnotationNested(variantTypeAnn);
                                            if (structTypes_.count(boxMangled)) {
                                                classVarTypes_[bName] = boxMangled;
                                            }
                                        }
                                        std::string resolvedInner;
                                        if (variantTypeAnn.name == "Box" && variantTypeAnn.kind == TypeAnnotationKind::Generic) {
                                            auto& boxArgs = static_cast<const GenericType&>(variantTypeAnn).typeArgs;
                                            if (!boxArgs.empty() && boxArgs[0])
                                                resolvedInner = mangleTypeAnnotation(*boxArgs[0]);
                                        } else if (variantTypeAnn.kind == TypeAnnotationKind::Named) {
                                            auto gpIt = genericTypeParamNames_.find(variantTypeAnn.name);
                                            if (gpIt != genericTypeParamNames_.end()) {
                                                std::string gpName = gpIt->second;
                                                if (gpName.size() > 4 && gpName.substr(0, 4) == "Box<" && gpName.back() == '>')
                                                    resolvedInner = gpName.substr(4, gpName.size() - 5);
                                                else if (structTypes_.count(gpName))
                                                    resolvedInner = gpName;
                                            }
                                        }
                                        // Fallback: search source field declarations for Box<T> info
                                        if (resolvedInner.empty() && ms.expr) {
                                            const MemberAccessExpr* fieldMa = nullptr;
                                            if (ms.expr->kind == ExprKind::MemberAccess)
                                                fieldMa = ms.expr->as<const MemberAccessExpr>();
                                            if (fieldMa && fieldMa->object) {
                                                std::string ownerType;
                                                if (fieldMa->object->kind == ExprKind::Identifier) {
                                                    ownerType = resolveClassName(*fieldMa->object,
                                                        fieldMa->object->as<const IdentifierExpr>()->name);
                                                } else if (fieldMa->object->kind == ExprKind::SelfExpr && !currentClassName_.empty()) {
                                                    ownerType = currentClassName_;
                                                }
                                                if (!ownerType.empty()) {
                                                    for (auto& dd : unit_->declarations) {
                                                        if (!dd || dd->name != ownerType) continue;
                                                        std::vector<FieldDecl>* flds = nullptr;
                                                        if (dd->kind == DeclKind::Class) flds = &dd->as<ClassDecl>()->fields;
                                                        else if (dd->kind == DeclKind::Struct) flds = &dd->as<StructDecl>()->fields;
                                                        if (!flds) break;
                                                        for (auto& f : *flds) {
                                                            if (f.name != fieldMa->member || !f.type) continue;
                                                            // R5 stage 2: registry-only match for option/result
                                                            // slot names — no bootstrap fallback.
                                                            auto isOptResAnnName = [&](const std::string& n) {
                                                                if (!langItems_) return false;
                                                                if (auto* l = langItems_->find("option"); l && l->name == n) return true;
                                                                if (auto* l = langItems_->find("result"); l && l->name == n) return true;
                                                                return false;
                                                            };
                                                            if (f.type->kind == TypeAnnotationKind::Generic &&
                                                                isOptResAnnName(f.type->name)) {
                                                                auto& outerArgs = static_cast<const GenericType&>(*f.type).typeArgs;
                                                                for (auto& oa : outerArgs) {
                                                                    if (oa && oa->name == "Box" && oa->kind == TypeAnnotationKind::Generic) {
                                                                        auto& boxArgs = static_cast<const GenericType&>(*oa).typeArgs;
                                                                        if (!boxArgs.empty() && boxArgs[0])
                                                                            resolvedInner = mangleTypeAnnotation(*boxArgs[0]);
                                                                    }
                                                                }
                                                            }
                                                            break;
                                                        }
                                                        break;
                                                    }
                                                }
                                            }
                                        }
                                        if (!resolvedInner.empty() && structTypes_.count(resolvedInner)) {
                                            refInnerTypeNames_[bName] = resolvedInner;
                                        }
                                    }
                                }
                                offset += module_->getDataLayout().getTypeAllocSize(bindTy);
                            }
                            break;
                        }
                        genericTypeParams_ = savedGP;
                        genericTypeParamNames_ = savedGPNames;
                        break;
                    }
                };
                bindADTFields(arm.tupleBindings.empty() && !arm.bindingName.empty()
                    ? std::vector<std::string>{} : arm.tupleBindings,
                    arm.nestedPatterns);

                if (arm.body) emitStmt(*arm.body);
                if (builder_->GetInsertBlock() && !builder_->GetInsertBlock()->getTerminator())
                    builder_->CreateBr(mergeBB);
            }

            for (auto& arm : ms.arms) {
                if (!arm.isDefault) continue;
                builder_->SetInsertPoint(defaultBB);
                if (arm.body) emitStmt(*arm.body);
                if (builder_->GetInsertBlock() && !builder_->GetInsertBlock()->getTerminator())
                    builder_->CreateBr(mergeBB);
            }

            builder_->SetInsertPoint(mergeBB);
            return;
        }
    }

    if (!matchVal->getType()->isIntegerTy()) {
        bool isResultStruct = false;
        llvm::AllocaInst* resultAlloca = nullptr;
        // Scrutinee's canonical Vyx type name (e.g. "Result<i64,string>").
        // Needed when per-instantiation variant types must be recovered from
        // a shared LLVM struct like __Result_32 that backs multiple distinct
        // Result<T,E> monomorphs.
        std::string scrutineeMangledName;
        if (ms.expr && ms.expr->inferredType) {
            scrutineeMangledName = ms.expr->inferredType->mangle();
        }
        if (auto* stTy = llvm::dyn_cast<llvm::StructType>(matchVal->getType())) {
            if (isOptionOrResultSlotName(stTy->getName())) {
                isResultStruct = true;
                resultAlloca = createEntryBlockAlloca(fn, stTy, "match.result");
                builder_->CreateStore(matchVal, resultAlloca);
            }
        }

        for (size_t i = 0; i < ms.arms.size(); ++i) {
            auto& arm = ms.arms[i];
            if (arm.isDefault) continue;

            auto* caseBB = llvm::BasicBlock::Create(*context_, "match.case." + arm.label, fn);
            auto* nextBB = (i + 1 < ms.arms.size() || defaultBB != mergeBB)
                ? llvm::BasicBlock::Create(*context_, "match.next", fn) : mergeBB;

            if (isResultStruct && resultAlloca) {
                auto* stTy = llvm::cast<llvm::StructType>(getValuePtrType(resultAlloca));
                auto* tagTy = stTy->getElementType(0);
                auto* tagPtr = builder_->CreateStructGEP(stTy, resultAlloca, 0, "match.tag.ptr");
                auto* tag = builder_->CreateLoad(tagTy, tagPtr, "match.tag");
                if (arm.label == "ok" || arm.label == "Ok" || arm.label == "Some") {
                    builder_->CreateCondBr(
                        builder_->CreateICmpEQ(tag, llvm::ConstantInt::get(tagTy, 0)),
                        caseBB, nextBB);
                } else if (arm.label == "fail" || arm.label == "Err" || arm.label == "None") {
                    builder_->CreateCondBr(
                        builder_->CreateICmpEQ(tag, llvm::ConstantInt::get(tagTy, 1)),
                        caseBB, nextBB);
                } else {
                    // Enum variant matching.
                    // For tagged-union ADTs (`{__tag, __data}` shape used
                    // by Option/Result and all user `enum X { V(…), … }`
                    // lowerings), the variant discriminator lives in the
                    // tag slot (index 0) and the payload lives in the
                    // data slot (index 1).  Earlier this branch always
                    // read slot 1 and compared against the enum-value
                    // map, which is correct only for c_repr flat enums
                    // whose storage is a single scalar — for tagged
                    // unions it reinterprets the payload bytes as the
                    // discriminator and mis-routes every case.
                    // Detect the shape by LLVM struct name OR scrutinee
                    // canonical name registered in structFieldNames_.
                    auto enumIt = errorEnumValues_.find(arm.label);
                    if (enumIt == errorEnumValues_.end()) {
                        builder_->CreateBr(caseBB);
                    } else {
                        bool taggedUnion = false;
                        auto looksLikeTagUnion = [&](const std::string& key) {
                            auto fnIt2 = structFieldNames_.find(key);
                            return fnIt2 != structFieldNames_.end() &&
                                   fnIt2->second.size() >= 2 &&
                                   fnIt2->second[0] == "__tag" &&
                                   fnIt2->second[1] == "__data";
                        };
                        if (stTy->hasName()) {
                            std::string n = stTy->getName().str();
                            if (looksLikeTagUnion(n)) taggedUnion = true;
                            // Shared __Result / __Result_N structs back
                            // multiple Vyx tagged unions; any shared
                            // Result/Option/user-generic-enum receiver
                            // uses the tag slot too. Treat the LLVM
                            // name alone as a sufficient signal.
                            if (!taggedUnion &&
                                (n == "__Result" || n.rfind("__Result_", 0) == 0)) {
                                taggedUnion = true;
                            }
                        }
                        if (!taggedUnion && !scrutineeMangledName.empty() &&
                            looksLikeTagUnion(scrutineeMangledName)) {
                            taggedUnion = true;
                        }
                        if (taggedUnion) {
                            auto* expected = llvm::ConstantInt::get(tagTy, enumIt->second, true);
                            auto* isMatch = builder_->CreateICmpEQ(tag, expected, "match.tag.eq");
                            builder_->CreateCondBr(isMatch, caseBB, nextBB);
                        } else {
                            auto* valPtr = builder_->CreateStructGEP(stTy, resultAlloca, 1, "match.val.ptr");
                            auto* val = builder_->CreateLoad(llvm::Type::getInt64Ty(*context_), valPtr);
                            auto* expected = llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), enumIt->second, true);
                            auto* isMatch = builder_->CreateICmpEQ(val, expected);
                            builder_->CreateCondBr(isMatch, caseBB, nextBB);
                        }
                    }
                }
            } else if (arm.label == "ok" && matchVal->getType()->isIntegerTy()) {
                auto* zero = llvm::ConstantInt::get(matchVal->getType(), 0);
                auto* isOk = builder_->CreateICmpSGE(matchVal, zero, "match.isok");
                builder_->CreateCondBr(isOk, caseBB, nextBB);
            } else if (arm.label == "fail" && matchVal->getType()->isIntegerTy()) {
                auto* zero = llvm::ConstantInt::get(matchVal->getType(), 0);
                auto* isFail = builder_->CreateICmpSLT(matchVal, zero, "match.isfail");
                builder_->CreateCondBr(isFail, caseBB, nextBB);
            } else if (arm.label == "__tuple" && matchVal->getType()->isStructTy()) {
                // Tuple-literal pattern: `case (0, x) =>` — compare each
                // literal slot; let binding slots pass through. The parser
                // stores either an IntLiteral expression or a constant in
                // the literal slot. Copies of the arm reset literalExpr to
                // null, so fall back to parsing the literal out of the
                // `name` field if it happens to be a digit string (defensive).
                auto* tupTy = llvm::cast<llvm::StructType>(matchVal->getType());
                auto* precheckAlloca = createEntryBlockAlloca(fn, tupTy, "match.tup.pre");
                builder_->CreateStore(matchVal, precheckAlloca);
                llvm::Value* cond = llvm::ConstantInt::getTrue(*context_);
                bool hasLiteral = false;
                for (size_t ti = 0; ti < arm.nestedPatterns.size() && ti < tupTy->getNumElements(); ++ti) {
                    auto& np = arm.nestedPatterns[ti];
                    if (!np.isLiteral) continue;
                    hasLiteral = true;
                    auto* slotGep = builder_->CreateStructGEP(tupTy, precheckAlloca, ti, "tup.slot");
                    auto* slotVal = builder_->CreateLoad(tupTy->getElementType(ti), slotGep, "tup.slot.val");
                    llvm::Value* litVal = nullptr;
                    if (np.literalExpr) {
                        litVal = emitExpr(*np.literalExpr);
                        if (litVal) litVal = castToType(litVal, slotVal->getType());
                    }
                    if (!litVal && !np.name.empty()) {
                        // Fallback: parse the stored stringified literal via
                        // strtoll (exceptions are disabled in this TU).
                        const char* s = np.name.c_str();
                        char* end = nullptr;
                        int64_t v = std::strtoll(s, &end, 10);
                        if (end != s && *end == '\0') {
                            litVal = llvm::ConstantInt::get(slotVal->getType(), v, true);
                        }
                    }
                    if (litVal) {
                        auto* slotEq = createSafeICmp(llvm::CmpInst::ICMP_EQ, slotVal, litVal, "tup.eq");
                        cond = builder_->CreateAnd(cond, slotEq, "tup.all.eq");
                    }
                }
                if (hasLiteral) {
                    builder_->CreateCondBr(cond, caseBB, nextBB);
                } else {
                    builder_->CreateBr(caseBB);
                }
            } else {
                builder_->CreateBr(caseBB);
            }

            builder_->SetInsertPoint(caseBB);
            // Tuple destructuring: bind each element (skipping literal slots
            // which have empty names in tupleBindings).
            if (!arm.tupleBindings.empty() && matchVal->getType()->isStructTy()) {
                auto* tupTy = llvm::cast<llvm::StructType>(matchVal->getType());
                auto* tupAlloca = createEntryBlockAlloca(fn, tupTy, "match.tup");
                builder_->CreateStore(matchVal, tupAlloca);
                for (size_t ti = 0; ti < arm.tupleBindings.size() && ti < tupTy->getNumElements(); ++ti) {
                    const std::string& bname = arm.tupleBindings[ti];
                    if (bname.empty()) continue;   // literal slot, no binding
                    auto* elemGep = builder_->CreateStructGEP(tupTy, tupAlloca, ti, bname);
                    auto* elemVal = builder_->CreateLoad(tupTy->getElementType(ti), elemGep);
                    auto* elemAlloca = createEntryBlockAlloca(fn, tupTy->getElementType(ti), bname);
                    builder_->CreateStore(elemVal, elemAlloca);
                    namedValues_[bname] = elemAlloca;
                }
            }
            if (!arm.bindingName.empty()) {
                if (isResultStruct && resultAlloca) {
                    auto* stTy = llvm::cast<llvm::StructType>(getValuePtrType(resultAlloca));
                    auto* valPtr = builder_->CreateStructGEP(stTy, resultAlloca, 1, "bind.val.ptr");
                    auto* valFieldTy = stTy->getElementType(1);
                    llvm::Type* loadTy = valFieldTy;
                    // Per-variant payload resolution. Result<T, E> with T ≠ E
                    // stores both variants in the same slot (shared i64
                    // payload for narrow types, or [N x i8] for wide types),
                    // so the bound name's load type must come from which
                    // arm matched — not from the struct-level inner type.
                    // Prefer the scrutinee's canonical Vyx mangled name
                    // (`Result<i64,string>`) over the LLVM struct name
                    // (`__Result_32`) because the latter is shared across
                    // multiple Result<T,E> instantiations with the same
                    // max-sized payload but different variant types.
                    llvm::Type* variantTy = nullptr;
                    if (!arm.label.empty()) {
                        if (!scrutineeMangledName.empty()) {
                            auto rvIt = resultVariantInnerTypes_.find(scrutineeMangledName);
                            if (rvIt != resultVariantInnerTypes_.end()) {
                                auto vIt = rvIt->second.find(arm.label);
                                if (vIt != rvIt->second.end()) variantTy = vIt->second;
                            }
                        }
                        if (!variantTy && stTy->hasName()) {
                            auto rvIt = resultVariantInnerTypes_.find(stTy->getName().str());
                            if (rvIt != resultVariantInnerTypes_.end()) {
                                auto vIt = rvIt->second.find(arm.label);
                                if (vIt != rvIt->second.end()) variantTy = vIt->second;
                            }
                        }
                    }
                    if (variantTy) {
                        loadTy = variantTy;
                    } else if (valFieldTy->isArrayTy()) {
                        // Wide Option struct: payload is [N x i8]. Look up the
                        // concrete inner type recorded by getOrCreateResultTypeForInner
                        // so we can do a properly typed load rather than truncating
                        // the value to a scalar integer.
                        llvm::Type* innerTy = nullptr;
                        if (stTy->hasName()) {
                            auto riIt = resultInnerTypes_.find(stTy->getName().str());
                            if (riIt != resultInnerTypes_.end()) innerTy = riIt->second;
                        }
                        if (innerTy) {
                            loadTy = innerTy;
                        } else {
                            // Fallback for unexpected array payload sizes.
                            uint64_t sz = module_->getDataLayout().getTypeAllocSize(valFieldTy);
                            if (sz <= 1) loadTy = llvm::Type::getInt8Ty(*context_);
                            else if (sz <= 4) loadTy = llvm::Type::getInt32Ty(*context_);
                            else loadTy = llvm::Type::getInt64Ty(*context_);
                        }
                    }
                    // When the payload type is a struct (e.g. inner Option<i64> =
                    // __Result), do NOT load it as a monolithic value from the byte
                    // array.  LLVM SROA uses the struct's ABI-aligned field offsets
                    // (i64 at offset 8 in {i32,i64}) when turning the alloca load
                    // into extractvalue operations, but the bytes in the array were
                    // stored compactly (i64 at offset 4, immediately after i32).
                    // Instead, load each field at its compact byte offset and store
                    // into a properly-typed alloca so the inner match can then use
                    // normal struct GEPs.
                    auto* alloca = createEntryBlockAlloca(fn, loadTy, arm.bindingName);
                    if (valFieldTy->isArrayTy() && loadTy->isStructTy()) {
                        auto* innerStTy = llvm::cast<llvm::StructType>(loadTy);
                        auto* i8Ty = llvm::Type::getInt8Ty(*context_);
                        auto* i64Ty = llvm::Type::getInt64Ty(*context_);
                        uint64_t compactOffset = 0;
                        for (unsigned fi = 0; fi < innerStTy->getNumElements(); ++fi) {
                            auto* fieldTy = innerStTy->getElementType(fi);
                            uint64_t fieldSz = module_->getDataLayout().getTypeStoreSize(fieldTy);
                            auto* bytePtr = builder_->CreateGEP(
                                i8Ty, valPtr,
                                llvm::ConstantInt::get(i64Ty, compactOffset),
                                "field.byte.ptr");
                            auto* fieldVal = builder_->CreateLoad(fieldTy, bytePtr, "field.val");
                            auto* fieldPtr = builder_->CreateStructGEP(
                                innerStTy, alloca, fi, "field.dst.ptr");
                            builder_->CreateStore(fieldVal, fieldPtr);
                            compactOffset += fieldSz;
                        }
                    } else {
                        auto* val = builder_->CreateLoad(loadTy, valPtr);
                        builder_->CreateStore(val, alloca);
                    }
                    namedValues_[arm.bindingName] = alloca;
                    // Track struct type name for subsequent method calls on bound var.
                    if (auto* bindStTy = llvm::dyn_cast<llvm::StructType>(loadTy)) {
                        if (bindStTy->hasName()) {
                            auto nm = bindStTy->getName();
                            if (nm.starts_with("__iface_")) {
                                // Bound name is an interface value — route
                                // subsequent `.method()` calls through the
                                // vtable-dispatch path instead of the
                                // struct-field-access fallback (which would
                                // reinterpret {data_ptr, vtable_ptr} as a
                                // flat vtable and segfault).
                                interfaceVarTypes_[arm.bindingName] = nm.substr(8).str();
                            } else {
                                classVarTypes_[arm.bindingName] = nm.str();
                            }
                        }
                    }
                    if (ms.expr && ms.expr->kind == ExprKind::Identifier) {
                        auto* bindStTy = llvm::dyn_cast<llvm::StructType>(loadTy);
                        bool payloadOwnsDrop = false;
                        if (bindStTy && bindStTy->hasName()) {
                            std::string typeName = bindStTy->getName().str();
                            payloadOwnsDrop = functions_.count(typeName + ".drop") > 0;
                            if (!payloadOwnsDrop && typeName == "__RefCounted") {
                                for (const auto& [fnName, _] : functions_) {
                                    if (fnName.rfind("Ref<", 0) == 0 &&
                                        fnName.size() > 6 &&
                                        fnName.compare(fnName.size() - 6, 6, ">.drop") == 0) {
                                        payloadOwnsDrop = true;
                                        break;
                                    }
                                }
                            }
                        }
                        if (payloadOwnsDrop) {
                            const auto& srcName = ms.expr->as<IdentifierExpr>()->name;
                            auto srcIt = namedValues_.find(srcName);
                            if (srcIt != namedValues_.end()) {
                                auto* srcTy = getValuePtrType(srcIt->second);
                                auto* srcStTy = srcTy ? llvm::dyn_cast<llvm::StructType>(srcTy) : nullptr;
                                if (srcStTy && srcStTy->getNumElements() >= 2) {
                                    auto* tagTy = srcStTy->getElementType(0);
                                    auto* tagPtr = builder_->CreateStructGEP(
                                        srcStTy, srcIt->second, 0, "match.src.moved.tag");
                                    builder_->CreateStore(
                                        llvm::ConstantInt::get(tagTy, -1, true), tagPtr);
                                }
                            }
                        }
                    }
                } else {
                    auto* alloca = createEntryBlockAlloca(fn, matchVal->getType(), arm.bindingName);
                    builder_->CreateStore(matchVal, alloca);
                    namedValues_[arm.bindingName] = alloca;
                }
            }

            if (arm.guardExpr) {
                auto* guardVal = emitExpr(*arm.guardExpr);
                if (guardVal) {
                    auto* isTrue = coerceToBool(guardVal, "match.guard");
                    auto* guardPassBB = llvm::BasicBlock::Create(*context_, "match.guard.pass", fn);
                    builder_->CreateCondBr(isTrue, guardPassBB, nextBB);
                    builder_->SetInsertPoint(guardPassBB);
                }
            }

            if (arm.body) emitStmt(*arm.body);
            if (!builder_->GetInsertBlock()->getTerminator())
                builder_->CreateBr(mergeBB);

            if (nextBB != mergeBB)
                builder_->SetInsertPoint(nextBB);
        }

        // Connect trailing nextBB to defaultBB or mergeBB
        if (builder_->GetInsertBlock() && !builder_->GetInsertBlock()->getTerminator()) {
            if (defaultBB != mergeBB)
                builder_->CreateBr(defaultBB);
            else
                builder_->CreateBr(mergeBB);
        }

        // Default
        for (auto& arm : ms.arms) {
            if (!arm.isDefault) continue;
            if (defaultBB != mergeBB) {
                builder_->SetInsertPoint(defaultBB);
                if (arm.body) emitStmt(*arm.body);
                if (!builder_->GetInsertBlock()->getTerminator())
                    builder_->CreateBr(mergeBB);
            }
        }

        if (builder_->GetInsertBlock() && !builder_->GetInsertBlock()->getTerminator())
            builder_->CreateBr(mergeBB);
    }

    builder_->SetInsertPoint(mergeBB);
}

} // namespace vyx
