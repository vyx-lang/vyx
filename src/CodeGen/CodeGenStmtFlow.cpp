#include "CodeGenIncludes.h"

namespace vyx {

void CodeGen::emitIfStmt(const Stmt& stmt) {
    auto& ifS = static_cast<const IfStmt&>(stmt);
    auto* fn = builder_->GetInsertBlock()->getParent();

    auto* condVal = ifS.condition ? emitExpr(*ifS.condition) : nullptr;
    if (!condVal) return;
    condVal = coerceToBool(condVal, "ifcond");

    auto* thenBB = llvm::BasicBlock::Create(*context_, "then", fn);
    auto* elseBB = llvm::BasicBlock::Create(*context_, "else", fn);
    auto* mergeBB = llvm::BasicBlock::Create(*context_, "ifmerge", fn);

    if (ifS.elifBranches.empty() && !ifS.elseBranch) {
        builder_->CreateCondBr(condVal, thenBB, mergeBB);
        elseBB->eraseFromParent();

        builder_->SetInsertPoint(thenBB);
        if (ifS.thenBranch) emitStmt(*ifS.thenBranch);
        if (!builder_->GetInsertBlock()->getTerminator())
            builder_->CreateBr(mergeBB);
    } else {
        builder_->CreateCondBr(condVal, thenBB, elseBB);

        builder_->SetInsertPoint(thenBB);
        if (ifS.thenBranch) emitStmt(*ifS.thenBranch);
        if (!builder_->GetInsertBlock()->getTerminator())
            builder_->CreateBr(mergeBB);

        builder_->SetInsertPoint(elseBB);

        if (!ifS.elifBranches.empty()) {
            for (size_t i = 0; i < ifS.elifBranches.size(); ++i) {
                auto& [elifCond, elifBody] = ifS.elifBranches[i];
                auto* elifCondVal = elifCond ? emitExpr(*elifCond) : nullptr;
                if (elifCondVal) elifCondVal = coerceToBool(elifCondVal, "elifcond");

                auto* elifThenBB = llvm::BasicBlock::Create(*context_, "elif.then", fn);
                auto* elifNextBB = (i + 1 < ifS.elifBranches.size() || ifS.elseBranch)
                    ? llvm::BasicBlock::Create(*context_, "elif.next", fn)
                    : mergeBB;

                if (elifCondVal)
                    builder_->CreateCondBr(elifCondVal, elifThenBB, elifNextBB);

                builder_->SetInsertPoint(elifThenBB);
                if (elifBody) emitStmt(*elifBody);
                if (!builder_->GetInsertBlock()->getTerminator())
                    builder_->CreateBr(mergeBB);

                builder_->SetInsertPoint(elifNextBB);
            }

            if (ifS.elseBranch) {
                emitStmt(*ifS.elseBranch);
            }
            if (!builder_->GetInsertBlock()->getTerminator() &&
                builder_->GetInsertBlock() != mergeBB)
                builder_->CreateBr(mergeBB);
        } else if (ifS.elseBranch) {
            emitStmt(*ifS.elseBranch);
            if (!builder_->GetInsertBlock()->getTerminator())
                builder_->CreateBr(mergeBB);
        } else {
            builder_->CreateBr(mergeBB);
        }
    }

    if (mergeBB->hasNPredecessors(0)) {
        mergeBB->eraseFromParent();
    } else {
        builder_->SetInsertPoint(mergeBB);
    }
}

void CodeGen::emitWhileStmt(const Stmt& stmt) {
    auto& ws = static_cast<const WhileStmt&>(stmt);
    auto* fn = builder_->GetInsertBlock()->getParent();
    auto* condBB = llvm::BasicBlock::Create(*context_, "while.cond", fn);
    auto* bodyBB = llvm::BasicBlock::Create(*context_, "while.body", fn);
    auto* exitBB = llvm::BasicBlock::Create(*context_, "while.exit", fn);

    loopStack_.push_back({exitBB, condBB, deferStack_.size(), blockAutoDropStack_.size()});

    builder_->CreateBr(condBB);
    builder_->SetInsertPoint(condBB);

    auto* condVal = ws.condition ? emitExpr(*ws.condition) : nullptr;
    if (condVal) {
        condVal = coerceToBool(condVal, "whilecond");
        builder_->CreateCondBr(condVal, bodyBB, exitBB);
    }

    builder_->SetInsertPoint(bodyBB);
    if (ws.body) emitStmt(*ws.body);
    if (!builder_->GetInsertBlock()->getTerminator())
        builder_->CreateBr(condBB);

    loopStack_.pop_back();
    builder_->SetInsertPoint(exitBB);
}

void CodeGen::emitForStmt(const Stmt& stmt) {
    auto& fs = static_cast<const ForStmt&>(stmt);
    auto* fn = builder_->GetInsertBlock()->getParent();

    if (fs.init) emitStmt(*fs.init);

    auto* condBB = llvm::BasicBlock::Create(*context_, "for.cond", fn);
    auto* bodyBB = llvm::BasicBlock::Create(*context_, "for.body", fn);
    auto* stepBB = llvm::BasicBlock::Create(*context_, "for.step", fn);
    auto* exitBB = llvm::BasicBlock::Create(*context_, "for.exit", fn);

    loopStack_.push_back({exitBB, stepBB, deferStack_.size(), blockAutoDropStack_.size()});

    builder_->CreateBr(condBB);
    builder_->SetInsertPoint(condBB);

    if (fs.condition) {
        auto* condVal = emitExpr(*fs.condition);
        if (condVal) {
            condVal = coerceToBool(condVal, "forcond");
            builder_->CreateCondBr(condVal, bodyBB, exitBB);
        }
    } else {
        builder_->CreateBr(bodyBB);
    }

    builder_->SetInsertPoint(bodyBB);
    if (fs.body) emitStmt(*fs.body);
    if (!builder_->GetInsertBlock()->getTerminator())
        builder_->CreateBr(stepBB);

    builder_->SetInsertPoint(stepBB);
    if (fs.step) emitExpr(*fs.step);
    builder_->CreateBr(condBB);

    loopStack_.pop_back();
    builder_->SetInsertPoint(exitBB);
}

// P4-D follow-up (limit #5): walk every enclosing block scope and
// emit `.drop()` for its heap-owning locals BEFORE the function ret —
// an early `return` (or `fail`) deep in nested blocks would otherwise
// bypass the natural block-end drop emission. Sema already excludes
// `escapes` locals (i.e. `return a;` won't double-free `a`) from
// autoDropLocals.
//
// BUG-LV-07: this used to be a `return`-only lambda. Promoting it to a
// member function so `emitFailExpr` calls the *same* path closes the
// fourth drop-injection corner (out-of-scope / assign-overwrite / move
// / fail-unwind). The contract for callers is: call `emitDeferredStmts(0)`
// FIRST (defer bodies should run before destructors, mirroring Go), then
// `emitScopeExitDrops()`, then build the actual `CreateRet*`.
void CodeGen::emitScopeExitDrops() {
    if (!blockAutoDropStack_.empty()) {
        for (size_t i = blockAutoDropStack_.size(); i > 0; --i) {
            const auto* drops = blockAutoDropStack_[i - 1];
            if (!drops) continue;
            for (auto& nm : *drops) emitDropForLocal(nm);
        }
    }
    // Limit #2: ADT payload drops walked the same direction.
    for (size_t i = adtBlockStack_.size(); i > 0; --i) {
        const auto* adts = adtBlockStack_[i - 1];
        if (!adts) continue;
        for (auto& a : *adts) emitAdtPayloadDrop(a.localName, a.innerHeapTypeName, a.variantTag);
    }
}

void CodeGen::emitReturnStmt(const Stmt& stmt) {
    auto& rs = static_cast<const ReturnStmt&>(stmt);
    auto emitEnclosingBlockDrops = [this]() { emitScopeExitDrops(); };
    if (rs.expr) {
        auto* val = emitExpr(*rs.expr);
        auto* fn = builder_->GetInsertBlock()->getParent();

        if (auto* callInst = llvm::dyn_cast_or_null<llvm::CallInst>(val)) {
            if (callInst->getType() == fn->getReturnType() &&
                !callInst->getType()->isStructTy()) {
                if (fn->hasFnAttribute("tailcall"))
                    callInst->setTailCallKind(llvm::CallInst::TCK_MustTail);
                else
                    callInst->setTailCall(true);
            }
        }

        if (val && !fn->getReturnType()->isVoidTy()) {
            auto* retTy = fn->getReturnType();

            if (auto* stTy = llvm::dyn_cast<llvm::StructType>(retTy)) {
                if (stTy->hasName() &&
                    isOptionOrResultSlotName(stTy->getName()) &&
                    val->getType() != stTy && stTy->getNumElements() == 2) {
                    auto* alloca = createEntryBlockAlloca(fn, stTy, "result.ok");
                    auto* tagPtr = builder_->CreateStructGEP(stTy, alloca, 0, "ok.tag");
                    builder_->CreateStore(llvm::ConstantInt::get(stTy->getElementType(0), 0), tagPtr);
                    auto* valPtr = builder_->CreateStructGEP(stTy, alloca, 1, "ok.val");
                    auto* expectedTy = stTy->getElementType(1);
                    if (expectedTy->isArrayTy()) {
                        builder_->CreateStore(val, valPtr);
                    } else {
                        llvm::Value* stored = castToType(val, expectedTy);
                        builder_->CreateStore(stored, valPtr);
                    }
                    emitDeferredStmts(0);
                    emitEnclosingBlockDrops();
                    builder_->CreateRet(builder_->CreateLoad(stTy, alloca, "result.ok.val"));
                    return;
                }
            }

            if (auto* stTy = llvm::dyn_cast<llvm::StructType>(retTy)) {
                if (stTy->hasName() && stTy->getName().starts_with("__iface_") &&
                    val->getType() != retTy) {
                    auto ifaceName = stTy->getName().substr(8).str();
                    std::string concreteName;
                    if (rs.expr->kind == ExprKind::StructInit)
                        concreteName = rs.expr->as<StructInitExpr>()->structName;
                    else if (rs.expr->kind == ExprKind::Identifier) {
                        concreteName = resolveClassName(*rs.expr,
                            rs.expr->as<IdentifierExpr>()->name);
                    }
                    if (!concreteName.empty()) {
                        auto* objStorage = createEntryBlockAlloca(fn, val->getType(), "ret.iface.obj");
                        builder_->CreateStore(val, objStorage);
                        auto* ifaceAlloca = createEntryBlockAlloca(fn, stTy, "ret.iface");
                        builder_->CreateStore(objStorage,
                            builder_->CreateStructGEP(stTy, ifaceAlloca, 0));
                        auto* vtGlobal = module_->getGlobalVariable(
                            concreteName + "_vtable_" + ifaceName, true);
                        if (vtGlobal)
                            builder_->CreateStore(vtGlobal,
                                builder_->CreateStructGEP(stTy, ifaceAlloca, 1));
                        emitDeferredStmts(0);
                        emitEnclosingBlockDrops();
                        builder_->CreateRet(builder_->CreateLoad(stTy, ifaceAlloca, "ret.iface.val"));
                        return;
                    }
                }
            }

            if (val->getType()->isPointerTy() && retTy->isStructTy()) {
                val = builder_->CreateLoad(retTy, val, "ret.load");
            }
            val = castToType(val, retTy);
            emitDeferredStmts(0);
            emitEnclosingBlockDrops();
            builder_->CreateRet(val);
        } else {
            emitDeferredStmts(0);
            emitEnclosingBlockDrops();
            auto* fn2 = builder_->GetInsertBlock()->getParent();
            if (fn2->getReturnType()->isVoidTy()) {
                builder_->CreateRetVoid();
            } else {
                builder_->CreateRet(llvm::Constant::getNullValue(fn2->getReturnType()));
            }
        }
    } else {
        auto* fn = builder_->GetInsertBlock()->getParent();
        emitDeferredStmts(0);
        emitEnclosingBlockDrops();
        if (fn->getReturnType()->isVoidTy()) {
            builder_->CreateRetVoid();
        } else {
            builder_->CreateRet(llvm::Constant::getNullValue(fn->getReturnType()));
        }
    }
}

void CodeGen::emitAssignment(const Stmt& stmt) {
    auto& asgn = static_cast<const AssignStmt&>(stmt);
    // Interface reassignment: ifaceVar = newConcreteObj
    if (asgn.target && asgn.target->kind == ExprKind::Identifier) {
        std::string varName = asgn.target->as<IdentifierExpr>()->name;
        std::string ifaceName;
        if (asgn.target->inferredType &&
            asgn.target->inferredType->kind == VyxTypeKind::Interface) {
            ifaceName = asgn.target->inferredType->name;
        }
        if (ifaceName.empty()) {
            auto ifaceIt = interfaceVarTypes_.find(varName);
            if (ifaceIt != interfaceVarTypes_.end()) ifaceName = ifaceIt->second;
        }
        if (!ifaceName.empty() && asgn.value) {
            auto ifaceTyIt = structTypes_.find("__iface_" + ifaceName);
            if (ifaceTyIt != structTypes_.end()) {
                std::string concreteName;
                if (asgn.value->kind == ExprKind::StructInit) {
                    concreteName = asgn.value->as<StructInitExpr>()->structName;
                } else if (asgn.value->kind == ExprKind::Identifier) {
                    concreteName = resolveClassName(*asgn.value,
                        asgn.value->as<IdentifierExpr>()->name);
                }

                auto* val = emitExpr(*asgn.value);
                if (!val) return;

                auto* ifaceTy = ifaceTyIt->second;
                auto ifaceAllocaIt = namedValues_.find(varName);
                if (ifaceAllocaIt == namedValues_.end()) return;

                auto* fn = builder_->GetInsertBlock()->getParent();
                auto* objStorage = createEntryBlockAlloca(fn, val->getType(), varName + ".reassign");
                builder_->CreateStore(val, objStorage);
                builder_->CreateStore(objStorage,
                    builder_->CreateStructGEP(ifaceTy, ifaceAllocaIt->second, 0, "iface.obj.reassign"));

                if (!concreteName.empty()) {
                    std::string vtGlobalName = concreteName + "_vtable_" + ifaceName;
                    auto* vtGlobal = module_->getGlobalVariable(vtGlobalName, true);
                    if (vtGlobal) {
                        builder_->CreateStore(vtGlobal,
                            builder_->CreateStructGEP(ifaceTy, ifaceAllocaIt->second, 1, "iface.vtbl.reassign"));
                    }
                }
                return;
            }
        }
    }

    auto tryEmitStringSelfAppend = [&]() -> bool {
        if (!asgn.target || !asgn.value || asgn.target->kind != ExprKind::Identifier)
            return false;
        const std::string varName = asgn.target->as<IdentifierExpr>()->name;
        auto addrIt = namedValues_.find(varName);
        if (addrIt == namedValues_.end())
            return false;

        auto* strTy = getOrCreateStringType();
        auto* addr = addrIt->second;
        if (!addr || getValuePtrType(addr) != strTy)
            return false;

        auto isTargetIdentifier = [&](const Expr* e) {
            return e && e->kind == ExprKind::Identifier &&
                   e->as<IdentifierExpr>()->name == varName;
        };

        const Expr* rhs = asgn.value.get();
        if (!rhs || rhs->kind != ExprKind::BinaryOp)
            return false;
        auto* outer = rhs->as<BinaryOpExpr>();
        if (outer->op != BinaryOp::Add || !outer->lhs || !outer->rhs)
            return false;

        if (!isTargetIdentifier(outer->lhs.get()))
            return false;

        auto fnIt = functions_.find("str_append_assign");
        if (fnIt == functions_.end())
            return false;

        auto* current = builder_->CreateLoad(strTy, addr, "sappend.current");
        auto* suffix = emitExpr(*outer->rhs);
        if (!suffix || suffix->getType() != strTy)
            return false;

        auto* appended = builder_->CreateCall(fnIt->second, {current, suffix}, "str.append.assign");
        builder_->CreateStore(appended, addr);
        return true;
    };

    if (tryEmitStringSelfAppend())
        return;

    auto* val = asgn.value ? emitExpr(*asgn.value) : nullptr;
    if (!val) return;

    // Deref assignment: *ptr = val
    if (asgn.target && asgn.target->kind == ExprKind::UnaryOp &&
        asgn.target->as<UnaryOpExpr>()->op == UnaryOp::Deref && asgn.target->as<UnaryOpExpr>()->operand) {
        auto* operandE = asgn.target->as<UnaryOpExpr>()->operand.get();
        auto* ptrVal = emitExpr(*operandE);
        if (ptrVal && ptrVal->getType()->isPointerTy()) {
            llvm::Type* storeTy = val->getType();
            bool typedKnown = false;
            llvm::Type* inferredTy = inferredPointeeLLVM(*operandE);
            if (inferredTy) {
                storeTy = inferredTy;
                val = castToType(val, storeTy);
                typedKnown = true;
            } else if (operandE->kind == ExprKind::Identifier) {
                auto ptIt = ptrElemTypes_.find(operandE->as<IdentifierExpr>()->name);
                if (ptIt != ptrElemTypes_.end()) {
                    storeTy = ptIt->second;
                    val = castToType(val, storeTy);
                    typedKnown = true;
                }
            }
            if (storeTy != val->getType()) val = castToType(val, storeTy);
            if (!typedKnown && storeTy->isIntegerTy() && storeTy->getIntegerBitWidth() < 64) {
                val = castToType(val, llvm::Type::getInt64Ty(*context_));
            }
            builder_->CreateStore(val, ptrVal);
            return;
        }
    }

    // Member assignment: obj.field = val
    if (asgn.target && asgn.target->kind == ExprKind::MemberAccess) {
        auto* addr = getMemberAddress(*asgn.target);
        if (addr) {
            auto* fieldTy = getValuePtrType(addr);
            if (fieldTy && val->getType() != fieldTy) {
                val = castToType(val, fieldTy);
            }
            builder_->CreateStore(val, addr);
        }
        return;
    }

    // Array/struct index assignment: arr[i] = val
    if (asgn.target && asgn.target->kind == ExprKind::Index) {
        auto& idxExpr = static_cast<const IndexExpr&>(*asgn.target);
        if (idxExpr.object && idxExpr.indexExpr) {
            auto* arrAddr = getVariableAddress(*idxExpr.object);
            if (!arrAddr && idxExpr.object->kind == ExprKind::MemberAccess)
                arrAddr = getMemberAddress(*idxExpr.object);
            if (arrAddr) {
                auto* idxVal = emitExpr(*idxExpr.indexExpr);
                if (!idxVal) return;
                auto* arrTy = getValuePtrType(arrAddr);
                if (auto* arrayTy = llvm::dyn_cast<llvm::ArrayType>(arrTy)) {
                    auto* zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0);
                    auto* gep = builder_->CreateGEP(arrTy, arrAddr, {zero, idxVal}, "arr.idx.store");
                    val = castToType(val, arrayTy->getElementType());
                    auto* si = builder_->CreateStore(val, gep);
                    si->setVolatile(true);
                    return;
                }
                if (arrTy && arrTy->isPointerTy()) {
                    llvm::Type* elemTy = idxExpr.object ? inferredPointeeLLVM(*idxExpr.object) : nullptr;
                    if (!elemTy && idxExpr.object->kind == ExprKind::Identifier) {
                        auto ptIt = ptrElemTypes_.find(idxExpr.object->as<IdentifierExpr>()->name);
                        if (ptIt != ptrElemTypes_.end()) elemTy = ptIt->second;
                    }
                    if (elemTy) {
                        auto* ptrVal = builder_->CreateLoad(arrTy, arrAddr, "typed.ptr");
                        auto* idxExt = idxVal->getType()->isIntegerTy(64) ? idxVal
                            : builder_->CreateSExt(idxVal, llvm::Type::getInt64Ty(*context_));
                        auto* gep = builder_->CreateGEP(elemTy, ptrVal, idxExt, "typed.store.ptr");
                        builder_->CreateStore(castToType(val, elemTy), gep);
                        return;
                    }
                }
                // operator[] set: call TypeName.operator_index_set(self, idx, val)
                if (auto* stTy = llvm::dyn_cast<llvm::StructType>(arrTy)) {
                    std::string typeName = stTy->hasName() ? stTy->getName().str() : "";
                    if (typeName.empty() && idxExpr.object->kind == ExprKind::Identifier) {
                        typeName = resolveClassName(*idxExpr.object,
                            idxExpr.object->as<IdentifierExpr>()->name);
                    }
                    if (!typeName.empty()) {
                        auto* opFn = findClassMethod(typeName, "operator_index_set");
                        if (opFn) {
                            auto* opSetFnTy = opFn->getFunctionType();
                            // Cast index to expected type (param 1)
                            llvm::Value* idxArg = idxVal;
                            if (opSetFnTy->getNumParams() > 1) {
                                auto* idxParamTy = opSetFnTy->getParamType(1);
                                if (!idxParamTy->isPointerTy() && idxArg->getType() != idxParamTy)
                                    idxArg = castToType(idxArg, idxParamTy);
                            }
                            // Cast value to expected type (param 2)
                            if (opSetFnTy->getNumParams() > 2)
                                val = castToType(val, opSetFnTy->getParamType(2));
                            builder_->CreateCall(opFn, {arrAddr, idxArg, val});
                            return;
                        }
                    }
                    // Direct Vec-like struct write when no operator_index_set
                    if (stTy->getNumElements() >= 3) {
                        auto* ptrTy = llvm::PointerType::getUnqual(*context_);
                        auto* i64Ty = llvm::Type::getInt64Ty(*context_);
                        auto* dataField = builder_->CreateStructGEP(stTy, arrAddr, 0, "vec.set.data.ptr");
                        auto* data = builder_->CreateLoad(ptrTy, dataField, "vec.set.data");
                        auto* idxExt = idxVal->getType()->isIntegerTy(64) ? idxVal
                            : builder_->CreateSExt(idxVal, i64Ty);
                        llvm::Type* elemTy = idxExpr.object ? inferredContainerElemLLVM(*idxExpr.object) : nullptr;
                        if (!elemTy && idxExpr.object->kind == ExprKind::Identifier) {
                            auto etIt = containerElemTypes_.find(idxExpr.object->as<IdentifierExpr>()->name);
                            if (etIt != containerElemTypes_.end()) elemTy = etIt->second;
                        }
                        if (!elemTy) elemTy = i64Ty;
                        auto* elemPtr = builder_->CreateGEP(elemTy, data, idxExt, "vec.set.elem.ptr");
                        val = castToType(val, elemTy);
                        builder_->CreateStore(val, elemPtr);
                        return;
                    }
                }
            }
        }
    }

    // Track container types from assignment: dict = makeDict::<K,V>()
    if (asgn.target && asgn.target->kind == ExprKind::Identifier &&
        asgn.value && asgn.value->kind == ExprKind::Call && asgn.value->as<CallExpr>()->callee) {
        auto& varName = asgn.target->as<IdentifierExpr>()->name;
        auto& callee = *asgn.value->as<CallExpr>()->callee;
        if (callee.kind == ExprKind::Identifier) {
            auto& cn = callee.as<IdentifierExpr>()->name;
            if (cn == "makeDict" || cn == "makeUnorderedMap") {
                containerTypes_[varName] = "Dict";
                auto& typeArgs = callee.as<IdentifierExpr>()->callTypeArgs;
                if (typeArgs.size() >= 1 && typeArgs[0])
                    containerElemTypes_[varName] = toLLVMType(*typeArgs[0]);
                if (typeArgs.size() >= 2 && typeArgs[1])
                    containerValTypes_[varName] = toLLVMType(*typeArgs[1]);
            } else if (cn == "makeVec") {
                containerTypes_[varName] = "Vec";
                auto& typeArgs = callee.as<IdentifierExpr>()->callTypeArgs;
                if (!typeArgs.empty() && typeArgs[0])
                    containerElemTypes_[varName] = toLLVMType(*typeArgs[0]);
            }
        }
    }

    auto* addr = asgn.target ? getVariableAddress(*asgn.target) : nullptr;
    if (addr) {
        // P4-D follow-up (limit #1): drop the old value of `target` before
        // overwriting it. Sema has already gated this on heap-ownership,
        // not-moved, not-escaped, and target!=value to avoid double-drop /
        // self-drop. The RHS has been fully evaluated above, so it cannot
        // observe the dropped state.
        if (asgn.dropOldLvalue && asgn.target->kind == ExprKind::Identifier) {
            emitDropForLocal(asgn.target->as<IdentifierExpr>()->name);
        }
        val = castToType(val, getValuePtrType(addr));
        builder_->CreateStore(val, addr);
    } else if (asgn.target && asgn.target->kind == ExprKind::Identifier) {
        auto* gv = module_->getGlobalVariable(asgn.target->as<IdentifierExpr>()->name, true);
        if (gv) {
            val = castToType(val, gv->getValueType());
            builder_->CreateStore(val, gv);
        }
    }
}



} // namespace vyx
