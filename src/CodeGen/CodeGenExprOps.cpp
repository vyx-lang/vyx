#include "CodeGenIncludes.h"

namespace vyx {

llvm::Value* CodeGen::emitBinaryOp(const Expr& expr) {
    auto* binE = expr.as<const BinaryOpExpr>();
    // Type guard: x is Type.Variant → check ADT tag
    if (binE->op == BinaryOp::MatchOp && binE->rhs &&
        binE->rhs->kind == ExprKind::Identifier &&
        binE->rhs->as<const IdentifierExpr>()->name.starts_with("__typecheck_")) {
        auto* lhs = binE->lhs ? emitExpr(*binE->lhs) : nullptr;
        auto* i8Ty = llvm::Type::getInt8Ty(*context_);
        if (!lhs) {
            diag_.error(binE->location, "match guard: left-hand expression produced no value");
            return nullptr;
        }
        std::string checkName = binE->rhs->as<const IdentifierExpr>()->name.substr(12);
        auto evIt = errorEnumValues_.find(checkName);
        if (evIt != errorEnumValues_.end() && lhs->getType()->isStructTy()) {
            auto* stTy = llvm::cast<llvm::StructType>(lhs->getType());
            auto fnIt = structFieldNames_.find(stTy->hasName() ? stTy->getName().str() : "");
            bool isADT = fnIt != structFieldNames_.end() && fnIt->second.size() >= 2 &&
                         fnIt->second[0] == "__tag";
            if (isADT) {
                auto* fn = builder_->GetInsertBlock()->getParent();
                auto* adtAlloca = createEntryBlockAlloca(fn, stTy, "is.adt");
                builder_->CreateStore(lhs, adtAlloca);
                auto* tagPtr = builder_->CreateStructGEP(stTy, adtAlloca, 0, "is.tag.ptr");
                auto* tagVal = builder_->CreateLoad(llvm::Type::getInt32Ty(*context_), tagPtr, "is.tag");
                return builder_->CreateZExt(builder_->CreateICmpEQ(tagVal,
                    llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), evIt->second), "is.match"),
                    i8Ty, "is.bool");
            }
        }
        if (auto* lhsStTy = llvm::dyn_cast<llvm::StructType>(lhs->getType());
            lhsStTy && lhsStTy->hasName() &&
            isOptionOrResultSlotName(lhsStTy->getName())) {
            auto* fn = builder_->GetInsertBlock()->getParent();
            auto* alloca = createEntryBlockAlloca(fn, lhsStTy, "is.opt");
            builder_->CreateStore(lhs, alloca);
            auto* tagTy = lhsStTy->getElementType(0);
            auto* tag = builder_->CreateLoad(tagTy,
                builder_->CreateStructGEP(lhsStTy, alloca, 0), "is.tag");
            if (checkName == "Some" || checkName == "Ok")
                return builder_->CreateZExt(
                    builder_->CreateICmpEQ(tag, llvm::ConstantInt::get(tagTy, 0)),
                    i8Ty, "is.bool");
            if (checkName == "None" || checkName == "Err")
                return builder_->CreateZExt(
                    builder_->CreateICmpEQ(tag, llvm::ConstantInt::get(tagTy, 1)),
                    i8Ty, "is.bool");
        }
        return llvm::ConstantInt::get(i8Ty, 0);
    }

    if (binE->op == BinaryOp::And || binE->op == BinaryOp::Or) {
        auto* fn = builder_->GetInsertBlock()->getParent();
        auto* lhsVal = binE->lhs ? emitExpr(*binE->lhs) : nullptr;
        if (!lhsVal) return llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context_), 0);
        auto* lhsBool = coerceToBool(lhsVal, "sc.l");
        bool isAnd = (binE->op == BinaryOp::And);
        auto* lhsEndBB = builder_->GetInsertBlock();
        auto* evalRhsBB = llvm::BasicBlock::Create(*context_, isAnd ? "and.rhs" : "or.rhs", fn);
        auto* scDoneBB = llvm::BasicBlock::Create(*context_, "sc.done", fn);
        if (isAnd)
            builder_->CreateCondBr(lhsBool, evalRhsBB, scDoneBB);
        else
            builder_->CreateCondBr(lhsBool, scDoneBB, evalRhsBB);

        builder_->SetInsertPoint(evalRhsBB);
        auto* rhsVal = binE->rhs ? emitExpr(*binE->rhs) : nullptr;
        llvm::Value* rhsBool = llvm::ConstantInt::getFalse(*context_);
        if (rhsVal) {
            rhsBool = coerceToBool(rhsVal, "sc.r");
        }
        auto* rhsEndBB = builder_->GetInsertBlock();
        bool rhsFallsThrough = rhsEndBB && !rhsEndBB->getTerminator();
        if (rhsFallsThrough)
            builder_->CreateBr(scDoneBB);

        builder_->SetInsertPoint(scDoneBB);
        auto* phi = builder_->CreatePHI(llvm::Type::getInt1Ty(*context_), 2, "sc.phi");
        phi->addIncoming(llvm::ConstantInt::get(llvm::Type::getInt1Ty(*context_), isAnd ? 0 : 1), lhsEndBB);
        if (rhsFallsThrough)
            phi->addIncoming(rhsBool, rhsEndBB);
        return builder_->CreateZExt(phi, llvm::Type::getInt8Ty(*context_), "sc.val");
    }

    auto exprIsString = [](const Expr* e) {
        return e && e->inferredType &&
               e->inferredType->kind == VyxTypeKind::Class &&
               e->inferredType->name == "string";
    };
    auto exprIsInteger = [](const Expr* e) {
        return e && e->inferredType &&
               e->inferredType->kind == VyxTypeKind::Integer;
    };
    auto toStringIntegerReceiver = [&](const Expr* e) -> const Expr* {
        if (!e || e->kind != ExprKind::Call) return nullptr;
        auto* call = e->as<const CallExpr>();
        if (!call->args.empty() || !call->callee || call->callee->kind != ExprKind::MemberAccess)
            return nullptr;
        auto* ma = call->callee->as<const MemberAccessExpr>();
        if (ma->member != "toString" || !ma->object) return nullptr;
        const Expr* obj = ma->object.get();
        return exprIsInteger(obj) ? obj : nullptr;
    };

    if (binE->op == BinaryOp::Add && binE->lhs && binE->rhs && exprIsString(binE->lhs.get())) {
        if (const Expr* rhsNumberExpr = toStringIntegerReceiver(binE->rhs.get())) {
            if (binE->lhs->kind == ExprKind::BinaryOp) {
                auto* inner = binE->lhs->as<const BinaryOpExpr>();
                if (inner->op == BinaryOp::Add && inner->lhs && inner->rhs &&
                    exprIsString(inner->lhs.get()) && inner->rhs->kind == ExprKind::StringLiteral) {
                    auto* base = emitExpr(*inner->lhs);
                    auto* rhsNumber = emitExpr(*rhsNumberExpr);
                    if (!base || !rhsNumber) {
                        diag_.error(expr.location, "binary operation has null operand");
                        return llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0);
                    }
                    if (base->getType() == getOrCreateStringType() && rhsNumber->getType()->isIntegerTy()) {
                        auto* fn = builder_->GetInsertBlock()->getParent();
                        auto* strTy = getOrCreateStringType();
                        auto* i8Ty = llvm::Type::getInt8Ty(*context_);
                        auto* i64Ty = llvm::Type::getInt64Ty(*context_);
                        auto* ptrTy = llvm::PointerType::getUnqual(*context_);

                        auto* toCharsFn = getOrCreateI64ToCharsFunction();
                        auto* mallocFn = module_->getFunction("malloc");
                        if (!mallocFn) {
                            auto* mty = llvm::FunctionType::get(ptrTy, {i64Ty}, false);
                            mallocFn = llvm::Function::Create(mty, llvm::Function::ExternalLinkage, "malloc", *module_);
                        }
                        auto* memcpyFn = module_->getFunction("memcpy");
                        if (!memcpyFn) {
                            auto* mty = llvm::FunctionType::get(ptrTy, {ptrTy, ptrTy, i64Ty}, false);
                            memcpyFn = llvm::Function::Create(mty, llvm::Function::ExternalLinkage, "memcpy", *module_);
                        }

                        auto* baseAlloca = createEntryBlockAlloca(fn, strTy, "concatlitfmt.base");
                        builder_->CreateStore(base, baseAlloca);
                        auto* basePtr = builder_->CreateLoad(ptrTy, builder_->CreateStructGEP(strTy, baseAlloca, 0));
                        auto* baseLen = builder_->CreateLoad(i64Ty, builder_->CreateStructGEP(strTy, baseAlloca, 1));

                        const auto& lit = inner->rhs->as<const StringLiteralExpr>()->value;
                        auto* litPtr = getOrCreateString(lit);
                        auto* litLen = llvm::ConstantInt::get(i64Ty, static_cast<uint64_t>(lit.size()));

                        auto* tmpTy = llvm::ArrayType::get(i8Ty, 32);
                        auto* tmpAlloca = createEntryBlockAlloca(fn, tmpTy, "tostr.stack");
                        auto* zero64 = llvm::ConstantInt::get(i64Ty, 0);
                        auto* tmpPtr = builder_->CreateInBoundsGEP(tmpTy, tmpAlloca, {zero64, zero64}, "tostr.stack.ptr");

                        llvm::Value* arg = rhsNumber;
                        if (!rhsNumber->getType()->isIntegerTy(64))
                            arg = builder_->CreateIntCast(rhsNumber, i64Ty, true);
                        auto* rhsLen = builder_->CreateCall(toCharsFn, {tmpPtr, arg}, "tostr.n64");
                        auto* prefixLen = builder_->CreateAdd(baseLen, litLen, "concatlit.prefix.len");
                        auto* totalLen = builder_->CreateAdd(prefixLen, rhsLen, "concatlitfmt.len");
                        auto* allocSize = builder_->CreateAdd(totalLen, llvm::ConstantInt::get(i64Ty, 1));
                        auto* out = builder_->CreateCall(mallocFn, {allocSize}, "concatlitfmt.buf");
                        builder_->CreateCall(memcpyFn, {out, basePtr, baseLen});
                        if (!lit.empty()) {
                            auto* litDst = builder_->CreateGEP(i8Ty, out, baseLen);
                            builder_->CreateCall(memcpyFn, {litDst, litPtr, litLen});
                        }
                        auto* numDst = builder_->CreateGEP(i8Ty, out, prefixLen);
                        builder_->CreateCall(memcpyFn, {numDst, tmpPtr, rhsLen});
                        auto* nul = builder_->CreateGEP(i8Ty, out, totalLen);
                        builder_->CreateStore(llvm::ConstantInt::get(i8Ty, 0), nul);

                        auto* resAlloca = createEntryBlockAlloca(fn, strTy, "concatlitfmt.res");
                        builder_->CreateStore(out, builder_->CreateStructGEP(strTy, resAlloca, 0));
                        builder_->CreateStore(totalLen, builder_->CreateStructGEP(strTy, resAlloca, 1));
                        builder_->CreateStore(allocSize, builder_->CreateStructGEP(strTy, resAlloca, 2));
                        builder_->CreateStore(llvm::ConstantInt::get(i64Ty, 1),
                            builder_->CreateStructGEP(strTy, resAlloca, 3));
                        return builder_->CreateLoad(strTy, resAlloca, "concatlitfmt.val");
                    }
                }
            }

            auto* lhs = emitExpr(*binE->lhs);
            auto* rhsNumber = emitExpr(*rhsNumberExpr);
            if (!lhs || !rhsNumber) {
                diag_.error(expr.location, "binary operation has null operand");
                return llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0);
            }
            if (lhs->getType() == getOrCreateStringType() && rhsNumber->getType()->isIntegerTy()) {
                auto* fn = builder_->GetInsertBlock()->getParent();
                auto* strTy = getOrCreateStringType();
                auto* i8Ty = llvm::Type::getInt8Ty(*context_);
                auto* i64Ty = llvm::Type::getInt64Ty(*context_);
                auto* ptrTy = llvm::PointerType::getUnqual(*context_);

                auto* toCharsFn = getOrCreateI64ToCharsFunction();
                auto* mallocFn = module_->getFunction("malloc");
                if (!mallocFn) {
                    auto* mty = llvm::FunctionType::get(ptrTy, {i64Ty}, false);
                    mallocFn = llvm::Function::Create(mty, llvm::Function::ExternalLinkage, "malloc", *module_);
                }
                auto* memcpyFn = module_->getFunction("memcpy");
                if (!memcpyFn) {
                    auto* mty = llvm::FunctionType::get(ptrTy, {ptrTy, ptrTy, i64Ty}, false);
                    memcpyFn = llvm::Function::Create(mty, llvm::Function::ExternalLinkage, "memcpy", *module_);
                }

                auto* lhsAlloca = createEntryBlockAlloca(fn, strTy, "concatfmt.l");
                builder_->CreateStore(lhs, lhsAlloca);
                auto* lhsPtr = builder_->CreateLoad(ptrTy, builder_->CreateStructGEP(strTy, lhsAlloca, 0));
                auto* lhsLen = builder_->CreateLoad(i64Ty, builder_->CreateStructGEP(strTy, lhsAlloca, 1));

                auto* tmpTy = llvm::ArrayType::get(i8Ty, 32);
                auto* tmpAlloca = createEntryBlockAlloca(fn, tmpTy, "tostr.stack");
                auto* zero64 = llvm::ConstantInt::get(i64Ty, 0);
                auto* tmpPtr = builder_->CreateInBoundsGEP(tmpTy, tmpAlloca, {zero64, zero64}, "tostr.stack.ptr");

                llvm::Value* arg = rhsNumber;
                if (!rhsNumber->getType()->isIntegerTy(64))
                    arg = builder_->CreateIntCast(rhsNumber, i64Ty, true);
                auto* rhsLen = builder_->CreateCall(toCharsFn, {tmpPtr, arg}, "tostr.n64");
                auto* totalLen = builder_->CreateAdd(lhsLen, rhsLen, "concatfmt.len");
                auto* allocSize = builder_->CreateAdd(totalLen, llvm::ConstantInt::get(i64Ty, 1));
                auto* out = builder_->CreateCall(mallocFn, {allocSize}, "concatfmt.buf");
                builder_->CreateCall(memcpyFn, {out, lhsPtr, lhsLen});
                auto* dst = builder_->CreateGEP(i8Ty, out, lhsLen);
                builder_->CreateCall(memcpyFn, {dst, tmpPtr, rhsLen});
                auto* nul = builder_->CreateGEP(i8Ty, out, totalLen);
                builder_->CreateStore(llvm::ConstantInt::get(i8Ty, 0), nul);

                auto* resAlloca = createEntryBlockAlloca(fn, strTy, "concatfmt.res");
                builder_->CreateStore(out, builder_->CreateStructGEP(strTy, resAlloca, 0));
                builder_->CreateStore(totalLen, builder_->CreateStructGEP(strTy, resAlloca, 1));
                builder_->CreateStore(allocSize, builder_->CreateStructGEP(strTy, resAlloca, 2));
                builder_->CreateStore(llvm::ConstantInt::get(i64Ty, 1),
                    builder_->CreateStructGEP(strTy, resAlloca, 3));
                return builder_->CreateLoad(strTy, resAlloca, "concatfmt.val");
            }
        }
    }

    auto* lhs = binE->lhs ? emitExpr(*binE->lhs) : nullptr;
    auto* rhs = binE->rhs ? emitExpr(*binE->rhs) : nullptr;
    if (!lhs || !rhs) {
        diag_.error(expr.location, "binary operation has null operand");
        return llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0);
    }

    // Pointer arithmetic: ptr + n
    if ((binE->op == BinaryOp::Add || binE->op == BinaryOp::Sub) &&
        lhs->getType()->isPointerTy() && rhs->getType()->isIntegerTy()) {
        if (!builder_->GetInsertBlock()) return lhs;
        llvm::Type* gepElemTy = nullptr;
        if (binE->lhs) gepElemTy = inferredPointeeLLVM(*binE->lhs);
        if (!gepElemTy && binE->lhs && binE->lhs->kind == ExprKind::Identifier) {
            auto ptIt = ptrElemTypes_.find(binE->lhs->as<const IdentifierExpr>()->name);
            if (ptIt != ptrElemTypes_.end()) gepElemTy = ptIt->second;
        }
        if (!gepElemTy) gepElemTy = llvm::Type::getInt8Ty(*context_);
        auto* offset = rhs;
        if (binE->op == BinaryOp::Sub)
            offset = builder_->CreateNeg(rhs, "ptr.neg");
        return builder_->CreateGEP(gepElemTy, lhs, offset, "ptr.arith");
    }

    // Pointer difference: ptr - ptr → element count
    if (binE->op == BinaryOp::Sub &&
        lhs->getType()->isPointerTy() && rhs->getType()->isPointerTy()) {
        auto* i64Ty = llvm::Type::getInt64Ty(*context_);
        auto* lInt = builder_->CreatePtrToInt(lhs, i64Ty);
        auto* rInt = builder_->CreatePtrToInt(rhs, i64Ty);
        return builder_->CreateSub(lInt, rInt, "ptr.diff");
    }

    // Operator overloading for struct/class types
    if (lhs->getType() == rhs->getType()) {
        if (auto* stTy = llvm::dyn_cast<llvm::StructType>(lhs->getType())) {
            static const std::unordered_map<int, std::string> opNames = {
                {(int)BinaryOp::Add, "operator_add"}, {(int)BinaryOp::Sub, "operator_sub"},
                {(int)BinaryOp::Mul, "operator_mul"}, {(int)BinaryOp::Div, "operator_div"},
                {(int)BinaryOp::Mod, "operator_mod"},
                {(int)BinaryOp::Eq, "operator_eq"}, {(int)BinaryOp::Neq, "operator_neq"},
                {(int)BinaryOp::Lt, "operator_lt"}, {(int)BinaryOp::Gt, "operator_gt"},
                {(int)BinaryOp::Lte, "operator_lte"}, {(int)BinaryOp::Gte, "operator_gte"},
                {(int)BinaryOp::And, "operator_and"}, {(int)BinaryOp::Or, "operator_or"},
                {(int)BinaryOp::BitAnd, "operator_band"}, {(int)BinaryOp::BitOr, "operator_bor"},
                {(int)BinaryOp::BitXor, "operator_bxor"},
                {(int)BinaryOp::Shl, "operator_shl"}, {(int)BinaryOp::Shr, "operator_shr"},
            };
            auto opIt = opNames.find((int)binE->op);
            if (opIt != opNames.end()) {
                std::string typeName = stTy->getName().str();
                std::string methodName = typeName + "." + opIt->second;
                auto fnIt = functions_.find(methodName);
                if (fnIt != functions_.end()) {
                    auto* opFn = fnIt->second;
                    auto* fn = builder_->GetInsertBlock()->getParent();
                    auto* lAlloca = createEntryBlockAlloca(fn, stTy, "op.l");
                    builder_->CreateStore(lhs, lAlloca);
                    std::vector<llvm::Value*> callArgs;
                    callArgs.push_back(lAlloca);
                    if (opFn->arg_size() > 1) {
                        auto* paramTy = opFn->getFunctionType()->getParamType(1);
                        if (paramTy->isPointerTy()) {
                            auto* rAlloca = createEntryBlockAlloca(fn, stTy, "op.r");
                            builder_->CreateStore(rhs, rAlloca);
                            callArgs.push_back(rAlloca);
                        } else {
                            callArgs.push_back(rhs);
                        }
                    }
                    return builder_->CreateCall(opFn, callArgs, "op.result");
                }
            }
        }
    }

    // Mixed-type operator overload: LHS is struct/class, RHS is different type (e.g. Vec2 + i64)
    if (lhs->getType() != rhs->getType()) {
        if (auto* stTy = llvm::dyn_cast<llvm::StructType>(lhs->getType())) {
            if (stTy->hasName() && stTy->getName() != "__String") {
                static const std::unordered_map<int, std::string> mixedOpNames = {
                    {(int)BinaryOp::Add, "operator_add"}, {(int)BinaryOp::Sub, "operator_sub"},
                    {(int)BinaryOp::Mul, "operator_mul"}, {(int)BinaryOp::Div, "operator_div"},
                    {(int)BinaryOp::Mod, "operator_mod"},
                    {(int)BinaryOp::BitAnd, "operator_band"}, {(int)BinaryOp::BitOr, "operator_bor"},
                    {(int)BinaryOp::BitXor, "operator_bxor"},
                    {(int)BinaryOp::Shl, "operator_shl"}, {(int)BinaryOp::Shr, "operator_shr"},
                };
                auto opIt = mixedOpNames.find((int)binE->op);
                if (opIt != mixedOpNames.end()) {
                    std::string typeName = stTy->getName().str();
                    std::string methodName = typeName + "." + opIt->second;
                    auto fnIt = functions_.find(methodName);
                    if (fnIt != functions_.end()) {
                        auto* opFn = fnIt->second;
                        if (opFn->arg_size() >= 2) {
                            auto* fn = builder_->GetInsertBlock()->getParent();
                            auto* lAlloca = createEntryBlockAlloca(fn, stTy, "mop.l");
                            builder_->CreateStore(lhs, lAlloca);
                            std::vector<llvm::Value*> callArgs = {lAlloca};
                            auto* param1Ty = opFn->getFunctionType()->getParamType(1);
                            llvm::Value* rhsArg = rhs;
                            if (param1Ty->isPointerTy()) {
                                auto* rAlloca = createEntryBlockAlloca(fn, rhs->getType(), "mop.r");
                                builder_->CreateStore(rhs, rAlloca);
                                callArgs.push_back(rAlloca);
                            } else if (rhsArg->getType() != param1Ty) {
                                rhsArg = castToType(rhsArg, param1Ty);
                                callArgs.push_back(rhsArg);
                            } else {
                                callArgs.push_back(rhsArg);
                            }
                            return builder_->CreateCall(opFn, callArgs, "mop.result");
                        }
                    }
                }
            }
        }
    }

    auto* strTy = getOrCreateStringType();

    // string + non-string: convert RHS to string, then concatenate
    if (binE->op == BinaryOp::Add && lhs->getType() == strTy && rhs->getType() != strTy) {
        auto* fn = builder_->GetInsertBlock()->getParent();
        auto* ptrTy = llvm::PointerType::getUnqual(*context_);
        auto* i64Ty = llvm::Type::getInt64Ty(*context_);
        auto* i32Ty = llvm::Type::getInt32Ty(*context_);
        auto* snprintfFn = module_->getFunction("snprintf");
        if (!snprintfFn) {
            auto* fty = llvm::FunctionType::get(i32Ty, {ptrTy, i64Ty, ptrTy}, true);
            snprintfFn = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, "snprintf", *module_);
        }
        auto* mallocFn2 = module_->getFunction("malloc");
        if (!mallocFn2) {
            auto* mty = llvm::FunctionType::get(ptrTy, {i64Ty}, false);
            mallocFn2 = llvm::Function::Create(mty, llvm::Function::ExternalLinkage, "malloc", *module_);
        }
        auto* tmpBuf = builder_->CreateCall(mallocFn2, {llvm::ConstantInt::get(i64Ty, 32)}, "tostr.buf");
        llvm::Value* fmtStr;
        llvm::Value* arg = rhs;
        if (rhs->getType()->isIntegerTy()) {
            unsigned bits = rhs->getType()->getIntegerBitWidth();
            if (bits < 64) arg = builder_->CreateSExt(rhs, i64Ty, "tostr.ext");
            fmtStr = getOrCreateString("%lld");
        } else if (rhs->getType()->isFloatingPointTy()) {
            if (!rhs->getType()->isDoubleTy()) arg = builder_->CreateFPExt(rhs, llvm::Type::getDoubleTy(*context_));
            fmtStr = getOrCreateString("%g");
        } else if (rhs->getType()->isPointerTy()) {
            fmtStr = getOrCreateString("%p");
        } else {
            diag_.error(binE->location,
                "string concatenation: unsupported RHS type for implicit formatting (only integer, float, and pointer are supported)");
            return nullptr;
        }
        auto* wrote = builder_->CreateCall(snprintfFn, {tmpBuf, llvm::ConstantInt::get(i64Ty, 32), fmtStr, arg}, "tostr.n");
        auto* wrote64 = builder_->CreateSExt(wrote, i64Ty, "tostr.n64");
        auto* rhsStrAlloca = createEntryBlockAlloca(fn, strTy, "tostr.rhs");
        builder_->CreateStore(tmpBuf, builder_->CreateStructGEP(strTy, rhsStrAlloca, 0));
        builder_->CreateStore(wrote64, builder_->CreateStructGEP(strTy, rhsStrAlloca, 1));
        builder_->CreateStore(llvm::ConstantInt::get(i64Ty, 32), builder_->CreateStructGEP(strTy, rhsStrAlloca, 2));
        builder_->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), 1),
            builder_->CreateStructGEP(strTy, rhsStrAlloca, 3));
        rhs = builder_->CreateLoad(strTy, rhsStrAlloca, "tostr.val");
    }

    // non-string + string: convert LHS to string, then concatenate
    if (binE->op == BinaryOp::Add && lhs->getType() != strTy && rhs->getType() == strTy) {
        auto* fn = builder_->GetInsertBlock()->getParent();
        auto* ptrTy = llvm::PointerType::getUnqual(*context_);
        auto* i64Ty = llvm::Type::getInt64Ty(*context_);
        auto* i32Ty = llvm::Type::getInt32Ty(*context_);
        auto* snprintfFn = module_->getFunction("snprintf");
        if (!snprintfFn) {
            auto* fty = llvm::FunctionType::get(i32Ty, {ptrTy, i64Ty, ptrTy}, true);
            snprintfFn = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, "snprintf", *module_);
        }
        auto* mallocFn2 = module_->getFunction("malloc");
        if (!mallocFn2) {
            auto* mty = llvm::FunctionType::get(ptrTy, {i64Ty}, false);
            mallocFn2 = llvm::Function::Create(mty, llvm::Function::ExternalLinkage, "malloc", *module_);
        }
        auto* tmpBuf = builder_->CreateCall(mallocFn2, {llvm::ConstantInt::get(i64Ty, 32)}, "tostr.buf");
        llvm::Value* fmtStr;
        llvm::Value* arg = lhs;
        if (lhs->getType()->isIntegerTy()) {
            unsigned bits = lhs->getType()->getIntegerBitWidth();
            if (bits < 64) arg = builder_->CreateSExt(lhs, i64Ty, "tostr.ext");
            fmtStr = getOrCreateString("%lld");
        } else if (lhs->getType()->isFloatingPointTy()) {
            if (!lhs->getType()->isDoubleTy()) arg = builder_->CreateFPExt(lhs, llvm::Type::getDoubleTy(*context_));
            fmtStr = getOrCreateString("%g");
        } else if (lhs->getType()->isPointerTy()) {
            fmtStr = getOrCreateString("%p");
        } else {
            diag_.error(binE->location,
                "string concatenation: unsupported LHS type for implicit formatting (only integer, float, and pointer are supported)");
            return nullptr;
        }
        auto* wrote = builder_->CreateCall(snprintfFn, {tmpBuf, llvm::ConstantInt::get(i64Ty, 32), fmtStr, arg}, "tostr.n");
        auto* wrote64 = builder_->CreateSExt(wrote, i64Ty, "tostr.n64");
        auto* lhsStrAlloca = createEntryBlockAlloca(fn, strTy, "tostr.lhs");
        builder_->CreateStore(tmpBuf, builder_->CreateStructGEP(strTy, lhsStrAlloca, 0));
        builder_->CreateStore(wrote64, builder_->CreateStructGEP(strTy, lhsStrAlloca, 1));
        builder_->CreateStore(llvm::ConstantInt::get(i64Ty, 32), builder_->CreateStructGEP(strTy, lhsStrAlloca, 2));
        builder_->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), 1),
            builder_->CreateStructGEP(strTy, lhsStrAlloca, 3));
        lhs = builder_->CreateLoad(strTy, lhsStrAlloca, "tostr.val");
    }

    if (binE->op == BinaryOp::Add && lhs->getType() == strTy && rhs->getType() == strTy) {
        auto fnIt = functions_.find("str_concat");
        if (fnIt == functions_.end()) {
            diag_.error(binE->location,
                "string '+' requires std.string::str_concat; import the source-backed std string module");
            return nullptr;
        }
        return builder_->CreateCall(fnIt->second, {lhs, rhs}, "str.concat");
    }

    // String * int: repeat string N times
    if (binE->op == BinaryOp::Mul &&
        ((lhs->getType() == strTy && rhs->getType()->isIntegerTy()) ||
         (lhs->getType()->isIntegerTy() && rhs->getType() == strTy))) {
        llvm::Value* strVal = lhs->getType() == strTy ? lhs : rhs;
        llvm::Value* countVal = lhs->getType() == strTy ? rhs : lhs;
        auto* fn = builder_->GetInsertBlock()->getParent();
        auto* i64Ty = llvm::Type::getInt64Ty(*context_);
        auto* ptrTy = llvm::PointerType::getUnqual(*context_);
        auto* sAlloca = createEntryBlockAlloca(fn, strTy, "strmul.s");
        builder_->CreateStore(strVal, sAlloca);
        auto* srcPtr = builder_->CreateLoad(ptrTy, builder_->CreateStructGEP(strTy, sAlloca, 0));
        auto* srcLen = builder_->CreateLoad(i64Ty, builder_->CreateStructGEP(strTy, sAlloca, 1));
        auto* count64 = castToType(countVal, i64Ty);
        auto* totalLen = builder_->CreateMul(srcLen, count64, "strmul.total");
        auto* mallocFn = module_->getFunction("malloc");
        if (!mallocFn) {
            auto* mty = llvm::FunctionType::get(ptrTy, {i64Ty}, false);
            mallocFn = llvm::Function::Create(mty, llvm::Function::ExternalLinkage, "malloc", *module_);
        }
        auto* allocSz = builder_->CreateAdd(totalLen, llvm::ConstantInt::get(i64Ty, 1));
        auto* buf = builder_->CreateCall(mallocFn, {allocSz}, "strmul.buf");
        auto* memcpyFn = module_->getFunction("memcpy");
        if (!memcpyFn) {
            auto* mty = llvm::FunctionType::get(ptrTy, {ptrTy, ptrTy, i64Ty}, false);
            memcpyFn = llvm::Function::Create(mty, llvm::Function::ExternalLinkage, "memcpy", *module_);
        }
        auto* iAlloca = createEntryBlockAlloca(fn, i64Ty, "strmul.i");
        builder_->CreateStore(llvm::ConstantInt::get(i64Ty, 0), iAlloca);
        auto* loopBB = llvm::BasicBlock::Create(*context_, "strmul.loop", fn);
        auto* bodyBB = llvm::BasicBlock::Create(*context_, "strmul.body", fn);
        auto* endBB = llvm::BasicBlock::Create(*context_, "strmul.end", fn);
        builder_->CreateBr(loopBB);
        builder_->SetInsertPoint(loopBB);
        auto* idx = builder_->CreateLoad(i64Ty, iAlloca);
        builder_->CreateCondBr(builder_->CreateICmpSLT(idx, count64), bodyBB, endBB);
        builder_->SetInsertPoint(bodyBB);
        auto* offset = builder_->CreateMul(idx, srcLen);
        auto* dest = builder_->CreateGEP(llvm::Type::getInt8Ty(*context_), buf, offset);
        builder_->CreateCall(memcpyFn, {dest, srcPtr, srcLen});
        builder_->CreateStore(builder_->CreateAdd(idx, llvm::ConstantInt::get(i64Ty, 1)), iAlloca);
        builder_->CreateBr(loopBB);
        builder_->SetInsertPoint(endBB);
        auto* nullPos = builder_->CreateGEP(llvm::Type::getInt8Ty(*context_), buf, totalLen);
        builder_->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context_), 0), nullPos);
        auto* resAlloca = createEntryBlockAlloca(fn, strTy, "strmul.res");
        builder_->CreateStore(buf, builder_->CreateStructGEP(strTy, resAlloca, 0));
        builder_->CreateStore(totalLen, builder_->CreateStructGEP(strTy, resAlloca, 1));
        builder_->CreateStore(allocSz, builder_->CreateStructGEP(strTy, resAlloca, 2));
        builder_->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), 1),
            builder_->CreateStructGEP(strTy, resAlloca, 3));

        return builder_->CreateLoad(strTy, resAlloca, "strmul.val");
    }

    // Ensure same type (skip for struct types — handled by operator_eq dispatch)
    if (lhs->getType() != rhs->getType() && !lhs->getType()->isStructTy() && !rhs->getType()->isStructTy()) {
        if (lhs->getType()->isFloatingPointTy() || rhs->getType()->isFloatingPointTy()) {
            auto* doubleTy = llvm::Type::getDoubleTy(*context_);
            lhs = numericCast(lhs, doubleTy);
            rhs = numericCast(rhs, doubleTy);
        } else {
            rhs = castToType(rhs, lhs->getType());
        }
    }

    bool isFloat = lhs->getType()->isFloatingPointTy();
    if (auto* vecTy = llvm::dyn_cast<llvm::FixedVectorType>(lhs->getType()))
        isFloat = vecTy->getElementType()->isFloatingPointTy();

    if (auto* lhsStTy = llvm::dyn_cast<llvm::StructType>(lhs->getType())) {
        if (lhsStTy->hasName() && lhsStTy->getName() != "__String") {
            static const std::unordered_map<int, std::string> binOpNames = {
                {(int)BinaryOp::Add, "operator_add"}, {(int)BinaryOp::Sub, "operator_sub"},
                {(int)BinaryOp::Mul, "operator_mul"}, {(int)BinaryOp::Div, "operator_div"},
                {(int)BinaryOp::Mod, "operator_mod"},
                {(int)BinaryOp::Eq, "operator_eq"}, {(int)BinaryOp::Neq, "operator_neq"},
                {(int)BinaryOp::Lt, "operator_lt"}, {(int)BinaryOp::Gt, "operator_gt"},
                {(int)BinaryOp::Lte, "operator_lte"}, {(int)BinaryOp::Gte, "operator_gte"},
                {(int)BinaryOp::BitAnd, "operator_band"}, {(int)BinaryOp::BitOr, "operator_bor"},
                {(int)BinaryOp::BitXor, "operator_bxor"},
                {(int)BinaryOp::Shl, "operator_shl"}, {(int)BinaryOp::Shr, "operator_shr"},
            };
            // For Neq: prefer operator_neq; fall back to inverting operator_eq
            int lookupOp = (int)binE->op;
            auto opIt = binOpNames.find(lookupOp);
            if (opIt != binOpNames.end()) {
                std::string opName = lhsStTy->getName().str() + "." + opIt->second;
                auto fnIt = functions_.find(opName);
                // If operator_neq is not defined, fall back to operator_eq + XOR
                bool neqFallback = false;
                if (fnIt == functions_.end() && binE->op == BinaryOp::Neq) {
                    opName = lhsStTy->getName().str() + ".operator_eq";
                    fnIt = functions_.find(opName);
                    neqFallback = true;
                }
                if (fnIt != functions_.end()) {
                    auto* fn = builder_->GetInsertBlock()->getParent();
                    auto* lAlloca = createEntryBlockAlloca(fn, lhsStTy, "op.l");
                    auto* rAlloca = createEntryBlockAlloca(fn, lhsStTy, "op.r");
                    builder_->CreateStore(lhs, lAlloca);
                    builder_->CreateStore(rhs, rAlloca);
                    auto* result = builder_->CreateCall(fnIt->second, {lAlloca, rAlloca}, "op.result");
                    if (neqFallback) {
                        return builder_->CreateXor(result,
                            llvm::ConstantInt::get(result->getType(), 1), "neq.result");
                    }
                    return result;
                }
            }
        }
    }

    auto* strTy2 = getOrCreateStringType();
    if (lhs->getType() == strTy2 && rhs->getType() == strTy2 &&
        (binE->op == BinaryOp::Eq || binE->op == BinaryOp::Neq ||
         binE->op == BinaryOp::Lt || binE->op == BinaryOp::Gt ||
         binE->op == BinaryOp::Lte || binE->op == BinaryOp::Gte)) {
        auto* fn = builder_->GetInsertBlock()->getParent();
        auto* ptrTy = llvm::PointerType::getUnqual(*context_);
        auto* i64Ty2 = llvm::Type::getInt64Ty(*context_);
        auto* lAlloca = createEntryBlockAlloca(fn, strTy2, "scmp.l");
        auto* rAlloca = createEntryBlockAlloca(fn, strTy2, "scmp.r");
        builder_->CreateStore(lhs, lAlloca);
        builder_->CreateStore(rhs, rAlloca);
        auto* lPtr = builder_->CreateLoad(ptrTy, builder_->CreateStructGEP(strTy2, lAlloca, 0));
        auto* lLen = builder_->CreateLoad(i64Ty2, builder_->CreateStructGEP(strTy2, lAlloca, 1));
        auto* rPtr = builder_->CreateLoad(ptrTy, builder_->CreateStructGEP(strTy2, rAlloca, 0));
        auto* rLen = builder_->CreateLoad(i64Ty2, builder_->CreateStructGEP(strTy2, rAlloca, 1));

        auto* memcmpFn = module_->getFunction("memcmp");
        if (!memcmpFn) {
            auto* mty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context_), {ptrTy, ptrTy, i64Ty2}, false);
            memcmpFn = llvm::Function::Create(mty, llvm::Function::ExternalLinkage, "memcmp", *module_);
        }
        auto* minLen = builder_->CreateSelect(builder_->CreateICmpSLT(lLen, rLen), lLen, rLen, "scmp.minlen");
        auto* cmpResult = builder_->CreateCall(memcmpFn, {lPtr, rPtr, minLen}, "scmp.cmp");
        auto* cmpIsZero = createSafeICmp(llvm::CmpInst::ICMP_EQ, cmpResult,
            llvm::ConstantInt::get(cmpResult->getType(), 0));
        auto* lenDiff = builder_->CreateSub(lLen, rLen, "scmp.lendiff");
        auto* lenCmp = builder_->CreateTrunc(lenDiff, llvm::Type::getInt32Ty(*context_), "scmp.lencmp");
        auto* finalCmp = builder_->CreateSelect(cmpIsZero, lenCmp, cmpResult, "scmp.final");

        auto* i8Bool = llvm::Type::getInt8Ty(*context_);
        auto* zero32 = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0);
        switch (binE->op) {
            case BinaryOp::Eq:
                return builder_->CreateZExt(builder_->CreateICmpEQ(finalCmp, zero32, "seq"), i8Bool, "cmp.bool");
            case BinaryOp::Neq:
                return builder_->CreateZExt(builder_->CreateICmpNE(finalCmp, zero32, "sne"), i8Bool, "cmp.bool");
            case BinaryOp::Lt:
                return builder_->CreateZExt(builder_->CreateICmpSLT(finalCmp, zero32, "slt"), i8Bool, "cmp.bool");
            case BinaryOp::Gt:
                return builder_->CreateZExt(builder_->CreateICmpSGT(finalCmp, zero32, "sgt"), i8Bool, "cmp.bool");
            case BinaryOp::Lte:
                return builder_->CreateZExt(builder_->CreateICmpSLE(finalCmp, zero32, "sle"), i8Bool, "cmp.bool");
            case BinaryOp::Gte:
                return builder_->CreateZExt(builder_->CreateICmpSGE(finalCmp, zero32, "sge"), i8Bool, "cmp.bool");
            default: break;
        }
    }

    // Align integer types for binary operations to prevent ICmp type mismatch
    if (lhs->getType()->isIntegerTy() && rhs->getType()->isIntegerTy() &&
        lhs->getType() != rhs->getType()) {
        unsigned lBits = lhs->getType()->getIntegerBitWidth();
        unsigned rBits = rhs->getType()->getIntegerBitWidth();
        bool lhsUnsigned = binE->lhs && isUnsignedExpr(*binE->lhs);
        bool rhsUnsigned = binE->rhs && isUnsignedExpr(*binE->rhs);
        if (lBits < rBits)
            lhs = (lhsUnsigned ? builder_->CreateZExt(lhs, rhs->getType(), "widen.l")
                               : builder_->CreateSExt(lhs, rhs->getType(), "widen.l"));
        else
            rhs = (rhsUnsigned ? builder_->CreateZExt(rhs, lhs->getType(), "widen.r")
                               : builder_->CreateSExt(rhs, lhs->getType(), "widen.r"));
        isFloat = false;
    }

    switch (binE->op) {
        case BinaryOp::Add:
            if (isFloat) return builder_->CreateFAdd(lhs, rhs, "fadd");
            if (emitDebug_ && lhs->getType()->isIntegerTy()) {
                auto* intrinsic = llvm::Intrinsic::getOrInsertDeclaration(
                    module_.get(), llvm::Intrinsic::sadd_with_overflow, {lhs->getType()});
                auto* result = builder_->CreateCall(intrinsic, {lhs, rhs}, "add.ov");
                auto* val = builder_->CreateExtractValue(result, 0, "add.val");
                auto* overflow = builder_->CreateExtractValue(result, 1, "add.overflow");
                auto* curFn = builder_->GetInsertBlock()->getParent();
                auto* panicBB = llvm::BasicBlock::Create(*context_, "add.panic", curFn);
                auto* okBB = llvm::BasicBlock::Create(*context_, "add.ok", curFn);
                builder_->CreateCondBr(overflow, panicBB, okBB);
                builder_->SetInsertPoint(panicBB);
                emitPanicCall(getOrCreateString("PANIC: integer overflow in addition"));
                builder_->SetInsertPoint(okBB);
                return val;
            }
            return builder_->CreateAdd(lhs, rhs, "add");
        case BinaryOp::Sub:
            if (isFloat) return builder_->CreateFSub(lhs, rhs, "fsub");
            if (emitDebug_ && lhs->getType()->isIntegerTy()) {
                auto* intrinsic = llvm::Intrinsic::getOrInsertDeclaration(
                    module_.get(), llvm::Intrinsic::ssub_with_overflow, {lhs->getType()});
                auto* result = builder_->CreateCall(intrinsic, {lhs, rhs}, "sub.ov");
                auto* val = builder_->CreateExtractValue(result, 0, "sub.val");
                auto* overflow = builder_->CreateExtractValue(result, 1, "sub.overflow");
                auto* curFn = builder_->GetInsertBlock()->getParent();
                auto* panicBB = llvm::BasicBlock::Create(*context_, "sub.panic", curFn);
                auto* okBB = llvm::BasicBlock::Create(*context_, "sub.ok", curFn);
                builder_->CreateCondBr(overflow, panicBB, okBB);
                builder_->SetInsertPoint(panicBB);
                emitPanicCall(getOrCreateString("PANIC: integer overflow in subtraction"));
                builder_->SetInsertPoint(okBB);
                return val;
            }
            return builder_->CreateSub(lhs, rhs, "sub");
        case BinaryOp::Mul:
            if (isFloat) return builder_->CreateFMul(lhs, rhs, "fmul");
            if (emitDebug_ && lhs->getType()->isIntegerTy()) {
                auto* intrinsic = llvm::Intrinsic::getOrInsertDeclaration(
                    module_.get(), llvm::Intrinsic::smul_with_overflow, {lhs->getType()});
                auto* result = builder_->CreateCall(intrinsic, {lhs, rhs}, "mul.ov");
                auto* val = builder_->CreateExtractValue(result, 0, "mul.val");
                auto* overflow = builder_->CreateExtractValue(result, 1, "mul.overflow");
                auto* curFn = builder_->GetInsertBlock()->getParent();
                auto* panicBB = llvm::BasicBlock::Create(*context_, "mul.panic", curFn);
                auto* okBB = llvm::BasicBlock::Create(*context_, "mul.ok", curFn);
                builder_->CreateCondBr(overflow, panicBB, okBB);
                builder_->SetInsertPoint(panicBB);
                emitPanicCall(getOrCreateString("PANIC: integer overflow in multiplication"));
                builder_->SetInsertPoint(okBB);
                return val;
            }
            return builder_->CreateMul(lhs, rhs, "mul");
        case BinaryOp::Div:
            if (isFloat) return builder_->CreateFDiv(lhs, rhs, "fdiv");
            {
                auto* curFn = builder_->GetInsertBlock()->getParent();
                bool lhsUnsigned = binE->lhs && isUnsignedExpr(*binE->lhs);
                auto* iTy = rhs->getType();
                auto* zero = llvm::ConstantInt::get(iTy, 0);
                auto* divZero = builder_->CreateICmpEQ(rhs, zero, "div.zero");
                llvm::Value* trap = divZero;
                // INT_MIN / -1 is UB in LLVM's sdiv (and in C/C++).  Panic
                // instead of silently yielding a poison value that the
                // optimizer turns into whatever it feels like. Only
                // applies to signed division; unsigned can't hit the
                // overflow case.
                if (!lhsUnsigned) {
                    unsigned bw = iTy->getIntegerBitWidth();
                    auto* negOne = llvm::ConstantInt::get(iTy, -1);
                    auto* intMin = llvm::ConstantInt::get(
                        iTy, llvm::APInt::getSignedMinValue(bw));
                    auto* isNegOne = builder_->CreateICmpEQ(rhs, negOne, "div.neg1");
                    auto* isMin    = builder_->CreateICmpEQ(lhs, intMin, "div.min");
                    auto* overflow = builder_->CreateAnd(isNegOne, isMin, "div.ov");
                    trap = builder_->CreateOr(divZero, overflow, "div.trap");
                }
                auto* panicBB = llvm::BasicBlock::Create(*context_, "div.panic", curFn);
                auto* okBB = llvm::BasicBlock::Create(*context_, "div.ok", curFn);
                builder_->CreateCondBr(trap, panicBB, okBB);
                builder_->SetInsertPoint(panicBB);
                emitPanicCall(getOrCreateString(
                    "PANIC: integer division by zero or overflow (INT_MIN / -1)"));
                builder_->SetInsertPoint(okBB);
                if (lhsUnsigned) return builder_->CreateUDiv(lhs, rhs, "udiv");
                return builder_->CreateSDiv(lhs, rhs, "sdiv");
            }
        case BinaryOp::Mod:
            if (isFloat) return builder_->CreateFRem(lhs, rhs, "fmod");
            {
                auto* curFn = builder_->GetInsertBlock()->getParent();
                bool lhsUnsigned = binE->lhs && isUnsignedExpr(*binE->lhs);
                auto* iTy = rhs->getType();
                auto* zero = llvm::ConstantInt::get(iTy, 0);
                auto* modZero = builder_->CreateICmpEQ(rhs, zero, "mod.zero");
                llvm::Value* trap = modZero;
                // srem on (INT_MIN, -1) is UB in LLVM too (standard value is
                // 0 but LLVM treats the divide-overflow identically). Match
                // the Div guard.
                if (!lhsUnsigned) {
                    unsigned bw = iTy->getIntegerBitWidth();
                    auto* negOne = llvm::ConstantInt::get(iTy, -1);
                    auto* intMin = llvm::ConstantInt::get(
                        iTy, llvm::APInt::getSignedMinValue(bw));
                    auto* isNegOne = builder_->CreateICmpEQ(rhs, negOne, "mod.neg1");
                    auto* isMin    = builder_->CreateICmpEQ(lhs, intMin, "mod.min");
                    auto* overflow = builder_->CreateAnd(isNegOne, isMin, "mod.ov");
                    trap = builder_->CreateOr(modZero, overflow, "mod.trap");
                }
                auto* panicBB = llvm::BasicBlock::Create(*context_, "mod.panic", curFn);
                auto* okBB = llvm::BasicBlock::Create(*context_, "mod.ok", curFn);
                builder_->CreateCondBr(trap, panicBB, okBB);
                builder_->SetInsertPoint(panicBB);
                emitPanicCall(getOrCreateString(
                    "PANIC: integer modulo by zero or overflow (INT_MIN % -1)"));
                builder_->SetInsertPoint(okBB);
                if (lhsUnsigned) return builder_->CreateURem(lhs, rhs, "umod");
                return builder_->CreateSRem(lhs, rhs, "smod");
            }

        case BinaryOp::Eq:
        case BinaryOp::Neq:
        case BinaryOp::Lt:
        case BinaryOp::Lte:
        case BinaryOp::Gt:
        case BinaryOp::Gte: {
            if (!isFloat && lhs->getType() != rhs->getType() &&
                lhs->getType()->isIntegerTy() && rhs->getType()->isIntegerTy()) {
                unsigned bL = lhs->getType()->getIntegerBitWidth();
                unsigned bR = rhs->getType()->getIntegerBitWidth();
                auto* wider = bL > bR ? lhs->getType() : rhs->getType();
                if (bL < bR) lhs = builder_->CreateSExt(lhs, wider, "cmp.ext.l");
                else         rhs = builder_->CreateSExt(rhs, wider, "cmp.ext.r");
            }
            if (!isFloat && lhs->getType() != rhs->getType()) {
                rhs = castToType(rhs, lhs->getType());
            }

            if (llvm::isa<llvm::StructType>(lhs->getType())) {
                auto* i8Bool = llvm::Type::getInt8Ty(*context_);
                llvm::CmpInst::Predicate pred;
                switch (binE->op) {
                    case BinaryOp::Eq:  pred = llvm::CmpInst::ICMP_EQ;  break;
                    case BinaryOp::Neq: pred = llvm::CmpInst::ICMP_NE;  break;
                    case BinaryOp::Lt:  pred = llvm::CmpInst::ICMP_SLT; break;
                    case BinaryOp::Lte: pred = llvm::CmpInst::ICMP_SLE; break;
                    case BinaryOp::Gt:  pred = llvm::CmpInst::ICMP_SGT; break;
                    case BinaryOp::Gte: pred = llvm::CmpInst::ICMP_SGE; break;
                    default: pred = llvm::CmpInst::ICMP_EQ; break;
                }
                auto* cmpResult = createSafeICmp(pred, lhs, rhs, "st.cmp");
                return builder_->CreateZExt(cmpResult, i8Bool, "cmp.bool");
            }

            {
                auto* i8Bool = llvm::Type::getInt8Ty(*context_);
                llvm::Value* cmpResult = nullptr;
                switch (binE->op) {
                    case BinaryOp::Eq:
                        cmpResult = isFloat ? builder_->CreateFCmpOEQ(lhs, rhs, "feq")
                                            : builder_->CreateICmpEQ(lhs, rhs, "eq");
                        break;
                    case BinaryOp::Neq:
                        // IEEE 754: NaN != NaN must be true; use unordered-not-equal.
                        cmpResult = isFloat ? builder_->CreateFCmpUNE(lhs, rhs, "fne")
                                            : builder_->CreateICmpNE(lhs, rhs, "ne");
                        break;
                    case BinaryOp::Lt:
                        cmpResult = isFloat ? builder_->CreateFCmpOLT(lhs, rhs, "flt")
                                            : builder_->CreateICmpSLT(lhs, rhs, "lt");
                        break;
                    case BinaryOp::Lte:
                        cmpResult = isFloat ? builder_->CreateFCmpOLE(lhs, rhs, "fle")
                                            : builder_->CreateICmpSLE(lhs, rhs, "le");
                        break;
                    case BinaryOp::Gt:
                        cmpResult = isFloat ? builder_->CreateFCmpOGT(lhs, rhs, "fgt")
                                            : builder_->CreateICmpSGT(lhs, rhs, "gt");
                        break;
                    case BinaryOp::Gte:
                        cmpResult = isFloat ? builder_->CreateFCmpOGE(lhs, rhs, "fge")
                                            : builder_->CreateICmpSGE(lhs, rhs, "ge");
                        break;
                    default: break;
                }
                if (cmpResult)
                    return builder_->CreateZExt(cmpResult, i8Bool, "cmp.bool");
            }
        }

        case BinaryOp::And:
            return builder_->CreateAnd(lhs, rhs, "and");
        case BinaryOp::Or:
            return builder_->CreateOr(lhs, rhs, "or");

        case BinaryOp::BitAnd: return builder_->CreateAnd(lhs, rhs, "band");
        case BinaryOp::BitOr:  return builder_->CreateOr(lhs, rhs, "bor");
        case BinaryOp::BitXor: return builder_->CreateXor(lhs, rhs, "bxor");
        case BinaryOp::Shl: {
            // Shift by ≥ LHS bit width is undefined in LLVM (and in C/C++).
            // `let a: i64 = 1 << 40` with default-i32 literals emitted `shl
            // i32 1, 40` which LLVM folded to garbage. When we can tell the
            // amount is too large, promote lhs to i64 so the semantics are
            // the obvious "this is a 64-bit shift".
            unsigned lhsBits = lhs->getType()->getIntegerBitWidth();
            if (auto* rhsConst = llvm::dyn_cast<llvm::ConstantInt>(rhs)) {
                uint64_t amt = rhsConst->getZExtValue();
                if (amt >= lhsBits && lhsBits < 64) {
                    auto* i64Ty = llvm::Type::getInt64Ty(*context_);
                    bool lhsUnsigned = binE->lhs && isUnsignedExpr(*binE->lhs);
                    lhs = lhsUnsigned
                        ? builder_->CreateZExt(lhs, i64Ty, "shl.widen.l")
                        : builder_->CreateSExt(lhs, i64Ty, "shl.widen.l");
                    rhs = builder_->CreateZExt(rhsConst, i64Ty, "shl.widen.r");
                }
            }
            return builder_->CreateShl(lhs, rhs, "shl");
        }
        case BinaryOp::Shr: {
            unsigned lhsBits = lhs->getType()->getIntegerBitWidth();
            if (auto* rhsConst = llvm::dyn_cast<llvm::ConstantInt>(rhs)) {
                uint64_t amt = rhsConst->getZExtValue();
                if (amt >= lhsBits && lhsBits < 64) {
                    auto* i64Ty = llvm::Type::getInt64Ty(*context_);
                    bool lhsUnsigned = binE->lhs && isUnsignedExpr(*binE->lhs);
                    lhs = lhsUnsigned
                        ? builder_->CreateZExt(lhs, i64Ty, "shr.widen.l")
                        : builder_->CreateSExt(lhs, i64Ty, "shr.widen.l");
                    rhs = builder_->CreateZExt(rhsConst, i64Ty, "shr.widen.r");
                }
            }
            if (binE->lhs && isUnsignedExpr(*binE->lhs))
                return builder_->CreateLShr(lhs, rhs, "lshr");
            return builder_->CreateAShr(lhs, rhs, "shr");
        }

        case BinaryOp::Pipe:
            return lhs;

        case BinaryOp::NullCoalesce: {
            if (auto* stTy = llvm::dyn_cast<llvm::StructType>(lhs->getType())) {
                if (stTy->hasName() &&
                    isOptionOrResultSlotName(stTy->getName())) {
                    auto* fn = builder_->GetInsertBlock()->getParent();
                    auto* i64Ty2 = llvm::Type::getInt64Ty(*context_);
                    auto* alloca = createEntryBlockAlloca(fn, stTy, "coalesce.opt");
                    builder_->CreateStore(lhs, alloca);
                    auto* tagPtr = builder_->CreateStructGEP(stTy, alloca, 0, "coalesce.tag.ptr");
                    auto* coalTagTy = stTy->getElementType(0);
                    auto* tag = builder_->CreateLoad(coalTagTy, tagPtr, "coalesce.tag");
                    auto* isNone = builder_->CreateICmpEQ(tag,
                        llvm::ConstantInt::get(coalTagTy, 1), "coalesce.isNone");

                    auto* someBB = llvm::BasicBlock::Create(*context_, "coalesce.some", fn);
                    auto* noneBB = llvm::BasicBlock::Create(*context_, "coalesce.none", fn);
                    auto* mergeBB = llvm::BasicBlock::Create(*context_, "coalesce.merge", fn);
                    builder_->CreateCondBr(isNone, noneBB, someBB);

                    builder_->SetInsertPoint(someBB);
                    auto* valPtr = builder_->CreateStructGEP(stTy, alloca, 1, "coalesce.val.ptr");
                    auto* targetTy = rhs->getType();
                    llvm::Value* someResult;
                    if (targetTy->isStructTy()) {
                        someResult = builder_->CreateLoad(targetTy, valPtr, "coalesce.struct");
                    } else {
                        auto* valFieldTy = stTy->getElementType(1);
                        llvm::Type* loadTy = i64Ty2;
                        if (valFieldTy->isArrayTy()) {
                            uint64_t sz = module_->getDataLayout().getTypeAllocSize(valFieldTy);
                            if (sz <= 1) loadTy = llvm::Type::getInt8Ty(*context_);
                            else if (sz <= 4) loadTy = llvm::Type::getInt32Ty(*context_);
                            else loadTy = i64Ty2;
                        }
                        auto* rawVal = builder_->CreateLoad(loadTy, valPtr, "coalesce.raw");
                        if (targetTy->isIntegerTy()) {
                            if (rawVal->getType() == targetTy)
                                someResult = rawVal;
                            else if (rawVal->getType()->isIntegerTy() &&
                                     rawVal->getType()->getIntegerBitWidth() > targetTy->getIntegerBitWidth())
                                someResult = builder_->CreateTrunc(rawVal, targetTy, "coalesce.trunc");
                            else
                                someResult = builder_->CreateSExt(rawVal, targetTy, "coalesce.ext");
                        } else if (targetTy->isDoubleTy()) {
                            someResult = builder_->CreateBitCast(rawVal, targetTy, "coalesce.f64");
                        } else if (targetTy->isFloatTy()) {
                            auto* d = builder_->CreateBitCast(rawVal, llvm::Type::getDoubleTy(*context_));
                            someResult = builder_->CreateFPTrunc(d, targetTy, "coalesce.f32");
                        } else if (targetTy->isPointerTy()) {
                            someResult = builder_->CreateIntToPtr(rawVal, targetTy, "coalesce.ptr");
                        } else {
                            someResult = rawVal;
                            if (someResult->getType() != targetTy)
                                someResult = castToType(someResult, targetTy);
                        }
                    }
                    auto* someEnd = builder_->GetInsertBlock();
                    builder_->CreateBr(mergeBB);

                    builder_->SetInsertPoint(noneBB);
                    auto* noneEnd = builder_->GetInsertBlock();
                    builder_->CreateBr(mergeBB);

                    builder_->SetInsertPoint(mergeBB);
                    auto* phi = builder_->CreatePHI(targetTy, 2, "coalesce.result");
                    phi->addIncoming(someResult, someEnd);
                    phi->addIncoming(rhs, noneEnd);
                    return phi;
                }
            }
            if (lhs->getType()->isPointerTy()) {
                auto* isNull = builder_->CreateICmpEQ(lhs, llvm::Constant::getNullValue(lhs->getType()), "is.null");
                return builder_->CreateSelect(isNull, rhs, lhs, "coalesce");
            }
            return lhs;
        }

        case BinaryOp::RangeOp: {
            auto* fn = builder_->GetInsertBlock()->getParent();
            auto* rangeTy = llvm::StructType::get(*context_,
                {llvm::Type::getInt64Ty(*context_), llvm::Type::getInt64Ty(*context_)});
            auto* alloca = createEntryBlockAlloca(fn, rangeTy, "range");
            auto* startPtr = builder_->CreateStructGEP(rangeTy, alloca, 0);
            auto* endPtr = builder_->CreateStructGEP(rangeTy, alloca, 1);
            auto* lhsExt = lhs->getType()->isIntegerTy(64) ? lhs
                : builder_->CreateSExt(lhs, llvm::Type::getInt64Ty(*context_));
            auto* rhsExt = rhs->getType()->isIntegerTy(64) ? rhs
                : builder_->CreateSExt(rhs, llvm::Type::getInt64Ty(*context_));
            builder_->CreateStore(lhsExt, startPtr);
            builder_->CreateStore(rhsExt, endPtr);
            return builder_->CreateLoad(rangeTy, alloca, "range.val");
        }

        default:
            diag_.error(expr.location, "unhandled binary operator in codegen ({})", static_cast<int>(binE->op));
            return llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0);
    }
}

llvm::Value* CodeGen::emitUnaryOp(const Expr& expr) {
    auto* unE = expr.as<const UnaryOpExpr>();
    auto* operand = unE->operand ? emitExpr(*unE->operand) : nullptr;
    if (!operand) {
        diag_.error(expr.location, "unary operation has null operand");
        return llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0);
    }

    if (auto* stTy = llvm::dyn_cast<llvm::StructType>(operand->getType())) {
        static const std::unordered_map<int, std::string> unaryOpNames = {
            {(int)UnaryOp::Neg, "operator_neg"}, {(int)UnaryOp::Not, "operator_not"},
            {(int)UnaryOp::BitNot, "operator_bnot"},
            {(int)UnaryOp::PreInc, "operator_inc"}, {(int)UnaryOp::PostInc, "operator_inc"},
            {(int)UnaryOp::PreDec, "operator_dec"}, {(int)UnaryOp::PostDec, "operator_dec"},
        };
        auto opIt = unaryOpNames.find((int)unE->op);
        if (opIt != unaryOpNames.end()) {
            std::string typeName = stTy->hasName() ? stTy->getName().str() : "";
            if (!typeName.empty()) {
                std::string opFnName = typeName + "." + opIt->second;
                auto fnIt = functions_.find(opFnName);
                if (fnIt != functions_.end()) {
                    if (unE->operand && unE->operand->kind == ExprKind::Identifier) {
                        auto* addr = getVariableAddress(*unE->operand);
                        if (addr) return builder_->CreateCall(fnIt->second, {addr}, "unary.result");
                    }
                }
            }
        }
    }

    switch (unE->op) {
        case UnaryOp::Neg:
            return operand->getType()->isFloatingPointTy()
                ? builder_->CreateFNeg(operand, "fneg")
                : builder_->CreateNeg(operand, "neg");
        case UnaryOp::Not: {
            auto* operandBool = coerceToBool(operand, "not.cond");
            auto* notVal = builder_->CreateNot(operandBool, "not");
            return builder_->CreateZExt(notVal, llvm::Type::getInt8Ty(*context_), "not.bool");
        }
        case UnaryOp::BitNot:
            return builder_->CreateNot(operand, "bnot");
        case UnaryOp::PreInc:
        case UnaryOp::PostInc: {
            if (unE->operand && unE->operand->kind == ExprKind::Identifier) {
                auto* addr = getVariableAddress(*unE->operand);
                if (addr) {
                    auto* one = llvm::ConstantInt::get(operand->getType(), 1);
                    auto* inc = builder_->CreateAdd(operand, one, "inc");
                    builder_->CreateStore(inc, addr);
                    return (unE->op == UnaryOp::PostInc) ? operand : inc;
                }
            }
            return operand;
        }
        case UnaryOp::PreDec:
        case UnaryOp::PostDec: {
            if (unE->operand && unE->operand->kind == ExprKind::Identifier) {
                auto* addr = getVariableAddress(*unE->operand);
                if (addr) {
                    auto* one = llvm::ConstantInt::get(operand->getType(), 1);
                    auto* dec = builder_->CreateSub(operand, one, "dec");
                    builder_->CreateStore(dec, addr);
                    return (unE->op == UnaryOp::PostDec) ? operand : dec;
                }
            }
            return operand;
        }
        case UnaryOp::Ref:
        case UnaryOp::MutRef: {
            if (unE->operand && unE->operand->kind == ExprKind::Identifier) {
                auto* addr = getVariableAddress(*unE->operand);
                if (addr) return addr;
            }
            if (unE->operand && unE->operand->kind == ExprKind::Index) {
                auto* idxExpr = unE->operand->as<const IndexExpr>();
                if (idxExpr->object && idxExpr->object->kind == ExprKind::Identifier && idxExpr->indexExpr) {
                    auto* arrAddr = getVariableAddress(*idxExpr->object);
                    if (arrAddr) {
                        auto* idxVal = emitExpr(*idxExpr->indexExpr);
                        auto* arrTy = getValuePtrType(arrAddr);
                        if (auto* arrayTy = llvm::dyn_cast<llvm::ArrayType>(arrTy)) {
                            auto* zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0);
                            return builder_->CreateGEP(arrayTy, arrAddr, {zero, idxVal}, "arr.ref.ptr");
                        }
                        if (auto* stTy = llvm::dyn_cast<llvm::StructType>(arrTy)) {
                            if (stTy->getNumElements() >= 3) {
                                auto* dataField = builder_->CreateStructGEP(stTy, arrAddr, 0);
                                auto* data = builder_->CreateLoad(
                                    llvm::PointerType::getUnqual(*context_), dataField);
                                auto* idxExt = idxVal->getType()->isIntegerTy(64) ? idxVal
                                    : builder_->CreateSExt(idxVal, llvm::Type::getInt64Ty(*context_));
                                llvm::Type* elemTy = idxExpr->object ? inferredContainerElemLLVM(*idxExpr->object) : nullptr;
                                if (!elemTy) {
                                    auto etIt = containerElemTypes_.find(idxExpr->object->as<const IdentifierExpr>()->name);
                                    if (etIt != containerElemTypes_.end()) elemTy = etIt->second;
                                }
                                if (!elemTy) elemTy = llvm::Type::getInt64Ty(*context_);
                                return builder_->CreateGEP(elemTy, data, idxExt, "vec.ref.ptr");
                            }
                        }
                    }
                }
            }
            if (unE->operand && unE->operand->kind == ExprKind::MemberAccess) {
                auto* addr = getMemberAddress(*unE->operand);
                if (addr) return addr;
            }
            return operand;
        }
        case UnaryOp::Deref: {
            if (operand->getType()->isPointerTy()) {
                llvm::Type* loadTy = nullptr;
                if (unE->operand) loadTy = inferredPointeeLLVM(*unE->operand);
                if (!loadTy && unE->operand && unE->operand->kind == ExprKind::Identifier) {
                    auto& idName = unE->operand->as<const IdentifierExpr>()->name;
                    auto ptIt = ptrElemTypes_.find(idName);
                    if (ptIt != ptrElemTypes_.end()) {
                        loadTy = ptIt->second;
                    } else {
                        std::string inner = resolveRefInner(*unE->operand, idName);
                        if (!inner.empty()) {
                            auto stIt = structTypes_.find(inner);
                            if (stIt != structTypes_.end())
                                loadTy = stIt->second;
                        }
                    }
                }
                if (!loadTy) loadTy = llvm::Type::getInt64Ty(*context_);
                else if (unE->operand && unE->operand->kind == ExprKind::MemberAccess && unit_) {
                    auto& dma = *unE->operand->as<const MemberAccessExpr>();
                    std::string ownerType;
                    if (dma.object && dma.object->kind == ExprKind::SelfExpr && !currentClassName_.empty())
                        ownerType = currentClassName_;
                    else if (dma.object && dma.object->kind == ExprKind::Identifier) {
                        auto& idn = dma.object->as<const IdentifierExpr>()->name;
                        ownerType = resolveClassName(*dma.object, idn);
                        if (ownerType.empty()) ownerType = resolveRefInner(*dma.object, idn);
                    }
                    if (!ownerType.empty()) {
                        for (auto& dd : unit_->declarations) {
                            if (!dd || dd->name != ownerType) continue;
                            std::vector<FieldDecl>* flds = nullptr;
                            if (dd->kind == DeclKind::Class) flds = &dd->as<ClassDecl>()->fields;
                            else if (dd->kind == DeclKind::Struct) flds = &dd->as<StructDecl>()->fields;
                            if (flds) {
                                for (auto& f : *flds) {
                                    if (f.name != dma.member || !f.type) continue;
                                    if (f.type->name == "Box" && f.type->kind == TypeAnnotationKind::Generic) {
                                        auto& boxArgs = static_cast<const GenericType&>(*f.type).typeArgs;
                                        if (!boxArgs.empty() && boxArgs[0]) {
                                            std::string innerMn = mangleTypeAnnotation(*boxArgs[0]);
                                            auto stIt = structTypes_.find(innerMn);
                                            if (stIt != structTypes_.end()) loadTy = stIt->second;
                                        }
                                    } else if (f.type->kind == TypeAnnotationKind::Pointer && f.type->as<PointerType>()->innerType) {
                                        std::string innerMn = mangleTypeAnnotation(*f.type->as<PointerType>()->innerType);
                                        auto stIt = structTypes_.find(innerMn);
                                        if (stIt != structTypes_.end()) loadTy = stIt->second;
                                    }
                                }
                            }
                            break;
                        }
                    }
                }
                else if (unE->operand && unE->operand->kind == ExprKind::Call) {
                    auto& callE = *unE->operand->as<const CallExpr>();
                    if (callE.callee && callE.callee->kind == ExprKind::MemberAccess) {
                        auto& ma = *callE.callee->as<const MemberAccessExpr>();
                        if (ma.member == "get" || ma.member == "pop" || ma.member == "peek") {
                            std::string containerType;
                            if (ma.object && ma.object->kind == ExprKind::Identifier) {
                                auto& idn = ma.object->as<const IdentifierExpr>()->name;
                                containerType = resolveClassName(*ma.object, idn);
                                if (containerType.empty())
                                    containerType = resolveRefInner(*ma.object, idn);
                            }
                            else if (ma.object && ma.object->kind == ExprKind::MemberAccess && unit_) {
                                auto& fieldMa = *ma.object->as<const MemberAccessExpr>();
                                std::string pClass;
                                if (fieldMa.object && fieldMa.object->kind == ExprKind::Identifier) {
                                    auto& idn = fieldMa.object->as<const IdentifierExpr>()->name;
                                    pClass = resolveClassName(*fieldMa.object, idn);
                                    if (pClass.empty() || pClass == "__RefCounted") {
                                        auto inner = resolveRefInner(*fieldMa.object, idn);
                                        if (!inner.empty()) pClass = inner;
                                    }
                                } else if (fieldMa.object && fieldMa.object->kind == ExprKind::SelfExpr && !currentClassName_.empty()) {
                                    pClass = currentClassName_;
                                }
                                // Handle chained member access: a.b.field.get() where a.b resolves through classes
                                else if (fieldMa.object && fieldMa.object->kind == ExprKind::MemberAccess && unit_) {
                                    auto& innerMa = *fieldMa.object->as<const MemberAccessExpr>();
                                    std::string innerClass;
                                    if (innerMa.object && innerMa.object->kind == ExprKind::Identifier) {
                                        auto& idn = innerMa.object->as<const IdentifierExpr>()->name;
                                        innerClass = resolveClassName(*innerMa.object, idn);
                                        if (innerClass.empty())
                                            innerClass = resolveRefInner(*innerMa.object, idn);
                                    } else if (innerMa.object && innerMa.object->kind == ExprKind::SelfExpr && !currentClassName_.empty()) {
                                        innerClass = currentClassName_;
                                    }
                                    if (!innerClass.empty()) {
                                        for (auto& dd2 : unit_->declarations) {
                                            if (!dd2 || dd2->name != innerClass) continue;
                                            std::vector<FieldDecl>* flds2 = nullptr;
                                            if (dd2->kind == DeclKind::Class) flds2 = &dd2->as<ClassDecl>()->fields;
                                            else if (dd2->kind == DeclKind::Struct) flds2 = &dd2->as<StructDecl>()->fields;
                                            if (flds2) {
                                                for (auto& f2 : *flds2) {
                                                    if (f2.name == innerMa.member && f2.type) {
                                                        // Unwrap Ref<T>/Scope<T> to get inner type
                                                        if (f2.type->kind == TypeAnnotationKind::Generic &&
                                                            (f2.type->name == "Ref" || f2.type->name == "Scope")) {
                                                            auto& refArgs = static_cast<const GenericType&>(*f2.type).typeArgs;
                                                            if (!refArgs.empty() && refArgs[0]) {
                                                                std::string innerTn = mangleTypeAnnotation(*refArgs[0]);
                                                                if (structTypes_.count(innerTn)) pClass = innerTn;
                                                                else if (structTypes_.count(refArgs[0]->name)) pClass = refArgs[0]->name;
                                                            }
                                                        } else {
                                                            std::string resolvedFieldType = mangleTypeAnnotation(*f2.type);
                                                            if (structTypes_.count(resolvedFieldType)) {
                                                                pClass = resolvedFieldType;
                                                            } else if (f2.type->kind == TypeAnnotationKind::Named && structTypes_.count(f2.type->name)) {
                                                                pClass = f2.type->name;
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                            break;
                                        }
                                    }
                                }
                                if (!pClass.empty()) {
                                    for (auto& dd : unit_->declarations) {
                                        if (!dd || dd->name != pClass) continue;
                                        std::vector<FieldDecl>* flds = nullptr;
                                        if (dd->kind == DeclKind::Class) flds = &dd->as<ClassDecl>()->fields;
                                        else if (dd->kind == DeclKind::Struct) flds = &dd->as<StructDecl>()->fields;
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
                                                                auto innerStIt = structTypes_.find(innerMn);
                                                                if (innerStIt != structTypes_.end())
                                                                    loadTy = innerStIt->second;
                                                            }
                                                        } else if (elemMn != "rawptr" && structTypes_.count(elemMn)) {
                                                            loadTy = structTypes_[elemMn];
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                        break;
                                    }
                                }
                            }
                            if (loadTy == llvm::Type::getInt64Ty(*context_) && !containerType.empty()) {
                                auto ltPos = containerType.find('<');
                                if (ltPos != std::string::npos) {
                                    std::string base = containerType.substr(0, ltPos);
                                    if (base == "Vec" || base == "Dict") {
                                        auto gtPos = containerType.rfind('>');
                                        if (gtPos != std::string::npos && gtPos > ltPos) {
                                            std::string innerName = containerType.substr(ltPos + 1, gtPos - ltPos - 1);
                                            if (innerName.size() > 4 && innerName.substr(0, 4) == "Box<" && innerName.back() == '>')
                                                innerName = innerName.substr(4, innerName.size() - 5);
                                            auto stIt = structTypes_.find(innerName);
                                            if (stIt != structTypes_.end())
                                                loadTy = stIt->second;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                return builder_->CreateLoad(loadTy, operand, "deref");
            }
            return operand;
        }
        default:
            diag_.error(expr.location, "internal: unhandled unary operator in codegen ({})",
                static_cast<int>(unE->op));
            return llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0);
    }
}


llvm::Value* CodeGen::emitCompoundAssignment(const Expr& expr) {
    auto* caE = expr.as<const CompoundAssignmentExpr>();
    if (!caE->target || !caE->value) {
        diag_.error(caE->location, "compound assignment: missing target or right-hand side");
        return nullptr;
    }

    auto* addr = getVariableAddress(*caE->target);
    if (!addr) {
        diag_.error(caE->location, "compound assignment: could not resolve address of target");
        return nullptr;
    }

    auto* currentVal = builder_->CreateLoad(getValuePtrType(addr), addr);
    auto* rhsVal = emitExpr(*caE->value);
    if (!rhsVal) {
        diag_.error(caE->location, "compound assignment: right-hand side did not produce a value");
        return nullptr;
    }

    // Legacy `evt += handler` (subscribe) and `evt -= handler` (unsub)
    // compound-assignment sugar for builtin Event was removed in R5 step 10
    // alongside `VyxTypeKind::Event` and the `containerTypes_[v] = "Event"`
    // tag. Event now lives in stdlib as `class Event` (if ever provided) or
    // via user code; += / -= would go through the operator overload path
    // below just like any other class.

    // Operator overload for struct/class types: += calls operator_add, etc.
    if (auto* stTy = llvm::dyn_cast<llvm::StructType>(getValuePtrType(addr))) {
        static const std::unordered_map<int, std::string> compoundOpNames = {
            {(int)CompoundOp::AddEq, "operator_add"}, {(int)CompoundOp::SubEq, "operator_sub"},
            {(int)CompoundOp::MulEq, "operator_mul"}, {(int)CompoundOp::DivEq, "operator_div"},
            {(int)CompoundOp::ModEq, "operator_mod"},
            {(int)CompoundOp::ShlEq, "operator_shl"}, {(int)CompoundOp::ShrEq, "operator_shr"},
            {(int)CompoundOp::BandEq, "operator_band"}, {(int)CompoundOp::BorEq, "operator_bor"},
            {(int)CompoundOp::BxorEq, "operator_bxor"},
        };
        auto opIt = compoundOpNames.find((int)caE->op);
        if (opIt != compoundOpNames.end()) {
            std::string typeName = stTy->hasName() ? stTy->getName().str() : "";
            if (!typeName.empty()) {
                std::string opFnName = typeName + "." + opIt->second;
                auto fnIt = functions_.find(opFnName);
                if (fnIt != functions_.end()) {
                    auto* result = builder_->CreateCall(fnIt->second, {addr, rhsVal}, "compound.result");
                    builder_->CreateStore(result, addr);
                    return result;
                }
            }
        }
    }

    // String += : concatenate and store back (with old buffer free)
    auto* strTy = getOrCreateStringType();
    if (caE->op == CompoundOp::AddEq &&
        currentVal->getType() == strTy && rhsVal->getType() == strTy) {
        auto* fn = builder_->GetInsertBlock()->getParent();
        auto* i8Ty = llvm::Type::getInt8Ty(*context_);
        auto* i64Ty2 = llvm::Type::getInt64Ty(*context_);
        auto* ptrTy2 = llvm::PointerType::getUnqual(*context_);

        auto* lAlloca = createEntryBlockAlloca(fn, strTy, "strcat.l");
        builder_->CreateStore(currentVal, lAlloca);
        auto* rAlloca = createEntryBlockAlloca(fn, strTy, "strcat.r");
        builder_->CreateStore(rhsVal, rAlloca);

        auto* lPtr = builder_->CreateLoad(ptrTy2, builder_->CreateStructGEP(strTy, lAlloca, 0));
        auto* lLen = builder_->CreateLoad(i64Ty2, builder_->CreateStructGEP(strTy, lAlloca, 1));
        auto* rPtr = builder_->CreateLoad(ptrTy2, builder_->CreateStructGEP(strTy, rAlloca, 0));
        auto* rLen = builder_->CreateLoad(i64Ty2, builder_->CreateStructGEP(strTy, rAlloca, 1));

        auto* totalLen = builder_->CreateAdd(lLen, rLen, "strcat.len");
        auto* mallocFn2 = module_->getFunction("malloc");
        if (!mallocFn2) {
            auto* mty = llvm::FunctionType::get(ptrTy2, {i64Ty2}, false);
            mallocFn2 = llvm::Function::Create(mty, llvm::Function::ExternalLinkage, "malloc", *module_);
        }
        auto* freeFn = module_->getFunction("free");
        if (!freeFn) {
            auto* fty = llvm::FunctionType::get(llvm::Type::getVoidTy(*context_), {ptrTy2}, false);
            freeFn = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, "free", *module_);
        }
        auto* memcpyFn = module_->getFunction("memcpy");
        if (!memcpyFn) {
            auto* mty = llvm::FunctionType::get(ptrTy2, {ptrTy2, ptrTy2, i64Ty2}, false);
            memcpyFn = llvm::Function::Create(mty, llvm::Function::ExternalLinkage, "memcpy", *module_);
        }
        auto* neededCap = builder_->CreateAdd(totalLen, llvm::ConstantInt::get(i64Ty2, 1));
        auto* ownedFlag = builder_->CreateLoad(llvm::Type::getInt64Ty(*context_),
            builder_->CreateStructGEP(strTy, addr, 3), "str.owned");
        auto* wasOwned = builder_->CreateICmpEQ(ownedFlag,
            llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), 1), "was.owned");

        // Always allocate new buffer to avoid state corruption in elif chains
        auto* newCap = builder_->CreateMul(neededCap, llvm::ConstantInt::get(i64Ty2, 2));
        auto* buf = builder_->CreateCall(mallocFn2, {newCap}, "strcat.buf");
        builder_->CreateCall(memcpyFn, {buf, lPtr, lLen});
        auto* dest2 = builder_->CreateGEP(i8Ty, buf, lLen);
        builder_->CreateCall(memcpyFn, {dest2, rPtr, rLen});
        auto* nullTerm = builder_->CreateGEP(i8Ty, buf, totalLen);
        builder_->CreateStore(llvm::ConstantInt::get(i8Ty, 0), nullTerm);
        // Free old if owned
        auto* freeBB = llvm::BasicBlock::Create(*context_, "strcat.free", fn);
        auto* storeBB = llvm::BasicBlock::Create(*context_, "strcat.store", fn);
        builder_->CreateCondBr(wasOwned, freeBB, storeBB);
        builder_->SetInsertPoint(freeBB);
        auto* oldBuf = builder_->CreateLoad(ptrTy2, builder_->CreateStructGEP(strTy, addr, 0));
        builder_->CreateCall(freeFn, {oldBuf});
        builder_->CreateBr(storeBB);
        builder_->SetInsertPoint(storeBB);
        builder_->CreateStore(buf, builder_->CreateStructGEP(strTy, addr, 0));
        builder_->CreateStore(newCap, builder_->CreateStructGEP(strTy, addr, 2));
        builder_->CreateStore(totalLen, builder_->CreateStructGEP(strTy, addr, 1));
        builder_->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), 1),
            builder_->CreateStructGEP(strTy, addr, 3));
        return builder_->CreateLoad(strTy, addr, "strcat.val");
    }

    rhsVal = castToType(rhsVal, currentVal->getType());
    bool isFloat = currentVal->getType()->isFloatingPointTy();
    llvm::Value* result = nullptr;

    switch (caE->op) {
        case CompoundOp::AddEq:
            result = isFloat ? builder_->CreateFAdd(currentVal, rhsVal) : builder_->CreateAdd(currentVal, rhsVal);
            break;
        case CompoundOp::SubEq:
            result = isFloat ? builder_->CreateFSub(currentVal, rhsVal) : builder_->CreateSub(currentVal, rhsVal);
            break;
        case CompoundOp::MulEq:
            result = isFloat ? builder_->CreateFMul(currentVal, rhsVal) : builder_->CreateMul(currentVal, rhsVal);
            break;
        case CompoundOp::DivEq:
            if (isFloat) {
                result = builder_->CreateFDiv(currentVal, rhsVal);
            } else {
                auto* curFn = builder_->GetInsertBlock()->getParent();
                auto* divZero = builder_->CreateICmpEQ(rhsVal, llvm::ConstantInt::get(rhsVal->getType(), 0));
                auto* panicBB = llvm::BasicBlock::Create(*context_, "diveq.panic", curFn);
                auto* okBB = llvm::BasicBlock::Create(*context_, "diveq.ok", curFn);
                builder_->CreateCondBr(divZero, panicBB, okBB);
                builder_->SetInsertPoint(panicBB);
                emitPanicCall(getOrCreateString("PANIC: integer division by zero"));
                builder_->SetInsertPoint(okBB);
                result = builder_->CreateSDiv(currentVal, rhsVal);
            }
            break;
        case CompoundOp::ModEq:
            if (isFloat) {
                result = builder_->CreateFRem(currentVal, rhsVal);
            } else {
                auto* curFn = builder_->GetInsertBlock()->getParent();
                auto* modZero = builder_->CreateICmpEQ(rhsVal, llvm::ConstantInt::get(rhsVal->getType(), 0));
                auto* panicBB = llvm::BasicBlock::Create(*context_, "modeq.panic", curFn);
                auto* okBB = llvm::BasicBlock::Create(*context_, "modeq.ok", curFn);
                builder_->CreateCondBr(modZero, panicBB, okBB);
                builder_->SetInsertPoint(panicBB);
                emitPanicCall(getOrCreateString("PANIC: integer modulo by zero"));
                builder_->SetInsertPoint(okBB);
                result = builder_->CreateSRem(currentVal, rhsVal);
            }
            break;
        case CompoundOp::ShlEq:
            result = builder_->CreateShl(currentVal, rhsVal);
            break;
        case CompoundOp::ShrEq:
            if (caE->target && isUnsignedExpr(*caE->target))
                result = builder_->CreateLShr(currentVal, rhsVal);
            else
                result = builder_->CreateAShr(currentVal, rhsVal);
            break;
        case CompoundOp::BandEq:
            result = builder_->CreateAnd(currentVal, rhsVal);
            break;
        case CompoundOp::BorEq:
            result = builder_->CreateOr(currentVal, rhsVal);
            break;
        case CompoundOp::BxorEq:
            result = builder_->CreateXor(currentVal, rhsVal);
            break;
    }

    if (result) builder_->CreateStore(result, addr);
    return result;
}

} // namespace vyx
