#include "CodeGenIncludes.h"

namespace vyx {

llvm::Value* CodeGen::emitExpr(const Expr& expr) {
    if (diag_.hasErrors()) return nullptr;
    if (++emitDepth_ > MaxEmitDepth) {
        --emitDepth_;
        diag_.error(expr.location, "codegen recursion depth exceeded ({})", MaxEmitDepth);
        return nullptr;
    }
    struct DepthGuard { int& d; ~DepthGuard() { --d; } } guard{emitDepth_};
    emitDebugLocation(expr.location);
    switch (expr.kind) {
        case ExprKind::IntLiteral: {
            auto* e = expr.as<const IntLiteralExpr>();
            llvm::Type* litTy = llvm::Type::getInt32Ty(*context_);
            if (e->value > INT32_MAX || e->value < INT32_MIN) {
                litTy = llvm::Type::getInt64Ty(*context_);
            }
            return llvm::ConstantInt::get(litTy, e->value, true);
        }

        case ExprKind::FloatLiteral:
            return llvm::ConstantFP::get(llvm::Type::getDoubleTy(*context_), expr.as<const FloatLiteralExpr>()->value);

        case ExprKind::BoolLiteral:
            return llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context_), expr.as<const BoolLiteralExpr>()->value ? 1 : 0);

        case ExprKind::StringLiteral:
            return createStringValue(expr.as<const StringLiteralExpr>()->value);

        case ExprKind::CharLiteral: {
            auto& val = expr.as<const CharLiteralExpr>()->value;
            return llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_),
                val.empty() ? 0 : static_cast<uint32_t>(val[0]));
        }

        case ExprKind::NullLiteral:
            return llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context_));

        case ExprKind::Identifier: {
            auto* ident = expr.as<const IdentifierExpr>();
            // Built-in None → creates appropriate None value based on return type context
            if (ident->name == "None") {
                auto noneIt = namedValues_.find("None");
                if (noneIt == namedValues_.end()) {
                    auto* fn = builder_->GetInsertBlock()->getParent();
                    llvm::StructType* rt = resolveOptionResultStructForExpr(expr, true);
                    if (rt && rt->hasName()) {
                        auto name = rt->getName();
                        auto* alloca = createEntryBlockAlloca(fn, rt, "option.none");
                        if (isOptionOrResultSlotName(name)) {
                            auto* tagTy = rt->getElementType(0);
                            auto* tagPtr = builder_->CreateStructGEP(rt, alloca, 0, "none.tag");
                            int noneTag = 1;
                            if (isOptionSlotName(name)) {
                                auto evIt = errorEnumValues_.find(name.str() + ".None");
                                if (evIt != errorEnumValues_.end()) noneTag = evIt->second;
                            }
                            builder_->CreateStore(llvm::ConstantInt::get(tagTy, noneTag), tagPtr);
                            auto* valPtr = builder_->CreateStructGEP(rt, alloca, 1, "none.val");
                            builder_->CreateStore(
                                llvm::Constant::getNullValue(rt->getElementType(1)), valPtr);
                            return builder_->CreateLoad(rt, alloca, "none.result");
                        }
                    }
                    auto* stTy = getOrCreateResultType();
                    auto* alloca = createEntryBlockAlloca(fn, stTy, "option.none");
                    builder_->CreateStore(
                        llvm::ConstantInt::get(stTy->getElementType(0), 1),
                        builder_->CreateStructGEP(stTy, alloca, 0));
                    builder_->CreateStore(
                        llvm::Constant::getNullValue(stTy->getElementType(1)),
                        builder_->CreateStructGEP(stTy, alloca, 1));
                    return builder_->CreateLoad(stTy, alloca, "none.result");
                }
            }
            auto it = namedValues_.find(ident->name);
            if (it != namedValues_.end()) {
                auto* li = builder_->CreateLoad(getValuePtrType(it->second), it->second, ident->name);
                if (volatileVars_.count(ident->name))
                    li->setVolatile(true);
                return li;
            }
            auto fit = functions_.find(ident->name);
            if (fit != functions_.end()) return fit->second;
            auto* gv = module_->getGlobalVariable(ident->name, true);
            if (gv) {
                auto* li = builder_->CreateLoad(gv->getValueType(), gv, ident->name);
                if (volatileVars_.count(ident->name))
                    li->setVolatile(true);
                return li;
            }
            if (structTypes_.count(ident->name))
                return llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0);
            diag_.error(expr.location, "undefined variable '{}'", ident->name);
            return llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0);
        }

        case ExprKind::BinaryOp: {
            auto* binExpr = expr.as<const BinaryOpExpr>();
            if (binExpr->op == BinaryOp::Pipe && binExpr->lhs && binExpr->rhs &&
                binExpr->rhs->kind == ExprKind::Call) {
                auto* rhsCall = binExpr->rhs->as<const CallExpr>();
                if (rhsCall->callee) {
                    auto* pipeInput = emitExpr(*binExpr->lhs);
                    if (pipeInput) {
                        std::string funcName;
                        if (rhsCall->callee->kind == ExprKind::Identifier) {
                            funcName = rhsCall->callee->as<const IdentifierExpr>()->name;
                        } else if (rhsCall->callee->kind == ExprKind::MemberAccess) {
                            auto* ma = rhsCall->callee->as<const MemberAccessExpr>();
                            if (ma->object && ma->object->kind == ExprKind::Identifier)
                                funcName = ma->object->as<const IdentifierExpr>()->name + "." + ma->member;
                        }

                        auto fnIt = functions_.find(funcName);
                        if (fnIt != functions_.end()) {
                            std::vector<llvm::Value*> args;
                            auto* targetFn = fnIt->second;
                            llvm::Value* castInput = pipeInput;
                            if (targetFn->arg_size() > 0) {
                                auto* expectedTy = targetFn->getArg(0)->getType();
                                castInput = castToType(pipeInput, expectedTy);
                            }
                            args.push_back(castInput);
                            for (size_t ai = 0; ai < rhsCall->args.size(); ++ai) {
                                if (!rhsCall->args[ai]) continue;
                                auto* argVal = emitExpr(*rhsCall->args[ai]);
                                if (argVal && ai + 1 < targetFn->arg_size()) {
                                    argVal = castToType(argVal, targetFn->getArg(ai + 1)->getType());
                                }
                                args.push_back(argVal);
                            }
                            return builder_->CreateCall(targetFn, args, "pipe");
                        }

                        if (rhsCall->callee->kind == ExprKind::Identifier) {
                            auto varIt = namedValues_.find(funcName);
                            if (varIt != namedValues_.end()) {
                                auto* closureFn = builder_->CreateLoad(getValuePtrType(varIt->second),
                                    varIt->second, funcName + ".ptr");
                                if (closureFn->getType()->isPointerTy()) {
                                    std::vector<llvm::Value*> args = {pipeInput};
                                    std::vector<llvm::Type*> argTypes = {pipeInput->getType()};
                                    llvm::Type* retTy = pipeInput->getType();
                                    auto* fnTy = llvm::FunctionType::get(retTy, argTypes, false);
                                    return builder_->CreateCall(fnTy, closureFn, args, "pipe.closure");
                                }
                            }
                        }
                    }
                }
            }
            return emitBinaryOp(expr);
        }
        case ExprKind::UnaryOp:
            return emitUnaryOp(expr);
        case ExprKind::Call:
            return emitCall(expr);
        case ExprKind::MemberAccess:
            return emitMemberAccess(expr);
        case ExprKind::StringInterpolation:
            return emitStringInterpolation(expr);
        case ExprKind::CompoundAssignment:
            return emitCompoundAssignment(expr);
        case ExprKind::Index:
            return emitIndex(expr);

        case ExprKind::StructInit: {
            auto* siExpr = expr.as<const StructInitExpr>();
            std::string resolvedName = siExpr->structName;
            if (!currentClassName_.empty()) {
                auto ltPos = currentClassName_.find('<');
                if (ltPos != std::string::npos) {
                    std::string currentBase = currentClassName_.substr(0, ltPos);
                    if (siExpr->structName == currentBase) {
                        resolvedName = currentClassName_;
                    }
                }
            }
            if (!genericTypeParams_.empty() && resolvedName.find('<') == std::string::npos && unit_) {
                for (auto& d : unit_->declarations) {
                    if (!d || (d->kind != DeclKind::Class && d->kind != DeclKind::Struct)) continue;
                    if (d->name != resolvedName || d->genericParams.empty()) continue;
                    std::string mangled = resolvedName + "<";
                    for (size_t gi = 0; gi < d->genericParams.size(); ++gi) {
                        if (gi > 0) mangled += ",";
                        auto gpIt = genericTypeParams_.find(d->genericParams[gi]);
                        auto gnIt = genericTypeParamNames_.find(d->genericParams[gi]);
                        if (gnIt != genericTypeParamNames_.end()) {
                            mangled += gnIt->second;
                        } else if (gpIt != genericTypeParams_.end()) {
                            mangled += llvmTypeToString(gpIt->second);
                        } else mangled += "i64";
                    }
                    mangled += ">";
                    // TODO(P1c-C): Mono scan gap — if mangled is absent from
                    // structTypes_ here, Mono missed emitting it; no on-demand creation.
                    if (structTypes_.count(mangled)) resolvedName = mangled;
                    break;
                }
            }
            auto sit = structTypes_.find(resolvedName);
            if (sit == structTypes_.end()) {
                diag_.error(expr.location, "undefined struct '{}'", siExpr->structName);
                return llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0);
            }
            auto* structTy = sit->second;
            auto* fn = builder_->GetInsertBlock()->getParent();
            auto* alloca = createEntryBlockAlloca(fn, structTy, "struct.tmp");

            auto* memsetFn = module_->getFunction("memset");
            if (memsetFn) {
                uint64_t sz = module_->getDataLayout().getTypeAllocSize(structTy);
                builder_->CreateCall(memsetFn, {
                    alloca,
                    llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0),
                    llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), sz)
                });
            }

            if (siExpr->spreadBase) {
                auto* baseVal = emitExpr(*siExpr->spreadBase);
                if (baseVal) builder_->CreateStore(baseVal, alloca);
            }

            auto& fieldNames = structFieldNames_[resolvedName];
            for (auto& [name, valExpr] : siExpr->fieldInits) {
                int idx = -1;
                for (size_t i = 0; i < fieldNames.size(); ++i) {
                    if (fieldNames[i] == name) { idx = static_cast<int>(i); break; }
                }
                if (idx >= 0 && valExpr) {
                    auto* fieldTy = structTy->getElementType(idx);
                    auto* savedHint = targetTypeHint_;
                    if (auto* fieldStTy = llvm::dyn_cast<llvm::StructType>(fieldTy))
                        targetTypeHint_ = fieldStTy;
                    auto* val = emitExpr(*valExpr);
                    targetTypeHint_ = savedHint;
                    auto* gep = builder_->CreateStructGEP(structTy, alloca, idx, name);
                    if (val) builder_->CreateStore(castToType(val, fieldTy), gep);
                }
            }
            return builder_->CreateLoad(structTy, alloca, "struct.val");
        }

        case ExprKind::ArrayInit: {
            auto* arrInit = expr.as<const ArrayInitExpr>();
            if (arrInit->elements.empty()) {
                auto* emptyArrTy = llvm::ArrayType::get(llvm::Type::getInt64Ty(*context_), 0);
                return llvm::ConstantArray::get(emptyArrTy, {});
            }

            bool hasSpread = false;
            for (size_t si = 0; si < arrInit->elementIsSpread.size(); ++si)
                if (arrInit->elementIsSpread[si]) { hasSpread = true; break; }

            if (hasSpread) {
                auto* i64Ty = llvm::Type::getInt64Ty(*context_);
                auto* ptrTy = llvm::PointerType::getUnqual(*context_);
                auto* fn = builder_->GetInsertBlock()->getParent();
                auto* vecTy = llvm::StructType::get(*context_, {ptrTy, i64Ty, i64Ty});
                auto* mallocFn = module_->getFunction("malloc");
                if (!mallocFn) {
                    auto* mty = llvm::FunctionType::get(ptrTy, {i64Ty}, false);
                    mallocFn = llvm::Function::Create(mty, llvm::Function::ExternalLinkage, "malloc", *module_);
                }
                auto* resultVec = createEntryBlockAlloca(fn, vecTy, "spread.vec");
                auto* initMem = builder_->CreateCall(mallocFn, {llvm::ConstantInt::get(i64Ty, 128)}, "spread.data");
                builder_->CreateStore(initMem, builder_->CreateStructGEP(vecTy, resultVec, 0));
                builder_->CreateStore(llvm::ConstantInt::get(i64Ty, 0), builder_->CreateStructGEP(vecTy, resultVec, 1));
                builder_->CreateStore(llvm::ConstantInt::get(i64Ty, 16), builder_->CreateStructGEP(vecTy, resultVec, 2));

                llvm::Type* elemTy = i64Ty;
                for (size_t i = 0; i < arrInit->elements.size(); ++i) {
                    bool isSpread = i < arrInit->elementIsSpread.size() && arrInit->elementIsSpread[i];
                    auto* val = emitExpr(*arrInit->elements[i]);
                    if (!val) {
                        diag_.error(expr.location, "array spread: element did not produce a value");
                        return nullptr;
                    }
                    if (i == 0 && !isSpread) elemTy = val->getType();

                    auto* reallocFn = module_->getFunction("realloc");
                    if (!reallocFn) {
                        auto* fty = llvm::FunctionType::get(ptrTy, {ptrTy, i64Ty}, false);
                        reallocFn = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, "realloc", *module_);
                    }

                    auto emitSpreadGrow = [&]() {
                        auto* resLenPtr = builder_->CreateStructGEP(vecTy, resultVec, 1);
                        auto* resLen = builder_->CreateLoad(i64Ty, resLenPtr, "sp.len");
                        auto* resCapPtr = builder_->CreateStructGEP(vecTy, resultVec, 2);
                        auto* resCap = builder_->CreateLoad(i64Ty, resCapPtr, "sp.cap");
                        auto* needGrow = builder_->CreateICmpSGE(resLen, resCap);
                        auto* growBB = llvm::BasicBlock::Create(*context_, "spread.grow", fn);
                        auto* contBB = llvm::BasicBlock::Create(*context_, "spread.cont", fn);
                        builder_->CreateCondBr(needGrow, growBB, contBB);
                        builder_->SetInsertPoint(growBB);
                        auto* newCap = builder_->CreateMul(resCap, llvm::ConstantInt::get(i64Ty, 2));
                        auto* elemSize = llvm::ConstantInt::get(i64Ty, module_->getDataLayout().getTypeAllocSize(elemTy));
                        auto* newSize = builder_->CreateMul(newCap, elemSize);
                        auto* resDataPtr = builder_->CreateStructGEP(vecTy, resultVec, 0);
                        auto* oldData = builder_->CreateLoad(ptrTy, resDataPtr);
                        auto* newData = builder_->CreateCall(reallocFn, {oldData, newSize}, "spread.realloc");
                        builder_->CreateStore(newData, resDataPtr);
                        builder_->CreateStore(newCap, resCapPtr);
                        builder_->CreateBr(contBB);
                        builder_->SetInsertPoint(contBB);
                    };

                    if (isSpread && val->getType()->isStructTy()) {
                        auto* srcAlloca = createEntryBlockAlloca(fn, val->getType(), "spread.src");
                        builder_->CreateStore(val, srcAlloca);
                        auto* srcData = builder_->CreateLoad(ptrTy, builder_->CreateStructGEP(
                            llvm::cast<llvm::StructType>(val->getType()), srcAlloca, 0));
                        auto* srcLen = builder_->CreateLoad(i64Ty, builder_->CreateStructGEP(
                            llvm::cast<llvm::StructType>(val->getType()), srcAlloca, 1));
                        auto* idxA = createEntryBlockAlloca(fn, i64Ty, "spread.i");
                        builder_->CreateStore(llvm::ConstantInt::get(i64Ty, 0), idxA);
                        auto* loopBB = llvm::BasicBlock::Create(*context_, "spread.loop", fn);
                        auto* bodyBB = llvm::BasicBlock::Create(*context_, "spread.body", fn);
                        auto* endBB = llvm::BasicBlock::Create(*context_, "spread.end", fn);
                        builder_->CreateBr(loopBB);
                        builder_->SetInsertPoint(loopBB);
                        auto* idx = builder_->CreateLoad(i64Ty, idxA);
                        builder_->CreateCondBr(builder_->CreateICmpSLT(idx, srcLen), bodyBB, endBB);
                        builder_->SetInsertPoint(bodyBB);
                        emitSpreadGrow();
                        auto* srcElem = builder_->CreateLoad(elemTy,
                            builder_->CreateGEP(elemTy, srcData, idx));
                        auto* resLenPtr = builder_->CreateStructGEP(vecTy, resultVec, 1);
                        auto* resLen = builder_->CreateLoad(i64Ty, resLenPtr);
                        auto* resData = builder_->CreateLoad(ptrTy, builder_->CreateStructGEP(vecTy, resultVec, 0));
                        auto* dest = builder_->CreateGEP(elemTy, resData, resLen);
                        builder_->CreateStore(srcElem, dest);
                        builder_->CreateStore(builder_->CreateAdd(resLen, llvm::ConstantInt::get(i64Ty, 1)), resLenPtr);
                        builder_->CreateStore(builder_->CreateAdd(idx, llvm::ConstantInt::get(i64Ty, 1)), idxA);
                        builder_->CreateBr(loopBB);
                        builder_->SetInsertPoint(endBB);
                    } else {
                        emitSpreadGrow();
                        auto* resLenPtr = builder_->CreateStructGEP(vecTy, resultVec, 1);
                        auto* resLen = builder_->CreateLoad(i64Ty, resLenPtr);
                        auto* resData = builder_->CreateLoad(ptrTy, builder_->CreateStructGEP(vecTy, resultVec, 0));
                        auto* dest = builder_->CreateGEP(elemTy, resData, resLen);
                        llvm::Value* stored = val;
                        if (stored->getType() != elemTy) stored = castToType(stored, elemTy);
                        builder_->CreateStore(stored, dest);
                        builder_->CreateStore(builder_->CreateAdd(resLen, llvm::ConstantInt::get(i64Ty, 1)), resLenPtr);
                    }
                }
                lastCollectedElemType_ = elemTy;
                return builder_->CreateLoad(vecTy, resultVec, "spread.result");
            }

            auto* firstVal = emitExpr(*arrInit->elements[0]);
            if (!firstVal) {
                diag_.error(expr.location, "array initializer: first element did not produce a value");
                return nullptr;
            }
            auto* elemTy = targetArrayElemHint_ ? targetArrayElemHint_ : firstVal->getType();

            size_t arrSize = arrInit->elements.size();
            if (arrInit->repeatCount) {
                if (arrInit->repeatCount->kind == ExprKind::IntLiteral)
                    arrSize = static_cast<size_t>(arrInit->repeatCount->as<const IntLiteralExpr>()->value);
            }

            auto* arrTy = llvm::ArrayType::get(elemTy, arrSize);
            auto* fn = builder_->GetInsertBlock()->getParent();
            auto* alloca = createEntryBlockAlloca(fn, arrTy, "arr.tmp");

            auto* zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0);
            if (arrInit->repeatCount) {
                for (size_t i = 0; i < arrSize; ++i) {
                    auto* idx = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), i);
                    auto* gep = builder_->CreateGEP(arrTy, alloca, {zero, idx}, "arr.elem");
                    builder_->CreateStore(castToType(firstVal, elemTy), gep);
                }
            } else {
                for (size_t i = 0; i < arrInit->elements.size(); ++i) {
                    auto* val = (i == 0) ? firstVal : emitExpr(*arrInit->elements[i]);
                    if (!val) {
                        diag_.error(expr.location, "array initializer: element {} did not produce a value", i);
                        return nullptr;
                    }
                    auto* idx = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), i);
                    auto* gep = builder_->CreateGEP(arrTy, alloca, {zero, idx}, "arr.elem");
                    builder_->CreateStore(castToType(val, elemTy), gep);
                }
            }
            return builder_->CreateLoad(arrTy, alloca, "arr.val");
        }

        case ExprKind::TupleInit: {
            auto* tupInit = expr.as<const TupleInitExpr>();
            if (tupInit->elements.empty()) {
                auto* emptyTupleTy = llvm::StructType::get(*context_, {});
                return llvm::UndefValue::get(emptyTupleTy);
            }

            std::vector<llvm::Value*> vals;
            std::vector<llvm::Type*> types;
            for (size_t ti = 0; ti < tupInit->elements.size(); ++ti) {
                auto& elem = tupInit->elements[ti];
                auto* v = emitExpr(*elem);
                if (!v) {
                    diag_.error(expr.location, "tuple literal: element {} did not produce a value", ti);
                    return nullptr;
                }
                vals.push_back(v);
                types.push_back(v->getType());
            }
            auto* tupleTy = llvm::StructType::get(*context_, types);
            auto* fn = builder_->GetInsertBlock()->getParent();
            auto* alloca = createEntryBlockAlloca(fn, tupleTy, "tuple.tmp");
            for (size_t i = 0; i < vals.size(); ++i) {
                auto* gep = builder_->CreateStructGEP(tupleTy, alloca, i);
                builder_->CreateStore(vals[i], gep);
            }
            return builder_->CreateLoad(tupleTy, alloca, "tuple.val");
        }

        case ExprKind::Cast: {
            auto* castE = expr.as<const CastExpr>();
            if (!castE->operand) {
                diag_.error(castE->location, "cast: missing operand expression");
                return nullptr;
            }
            auto* val = emitExpr(*castE->operand);
            if (!val) {
                diag_.error(castE->location, "cast: operand did not produce a value");
                return nullptr;
            }
            if (!castE->targetType) {
                diag_.error(castE->location, "cast: missing target type");
                return nullptr;
            }
            // Any unboxing: anyVal as i32 → extract i32 from __Any
            if (val->getType() == getOrCreateAnyType()) {
                auto* anyTy = getOrCreateAnyType();
                auto* i64Ty = llvm::Type::getInt64Ty(*context_);
                auto* fn = builder_->GetInsertBlock()->getParent();
                auto* anyAlloca = createEntryBlockAlloca(fn, anyTy, "cast.any");
                builder_->CreateStore(val, anyAlloca);
                auto* rawVal = builder_->CreateLoad(i64Ty,
                    builder_->CreateStructGEP(anyTy, anyAlloca, 1), "cast.raw");
                auto* targetTy = toLLVMType(*castE->targetType);
                if (targetTy->isIntegerTy(32))
                    return builder_->CreateTrunc(rawVal, targetTy, "cast.i32");
                if (targetTy->isIntegerTy(64))
                    return rawVal;
                if (targetTy->isDoubleTy())
                    return builder_->CreateBitCast(rawVal, targetTy, "cast.f64");
                if (targetTy->isFloatTy()) {
                    auto* d = builder_->CreateBitCast(rawVal, llvm::Type::getDoubleTy(*context_));
                    return builder_->CreateFPTrunc(d, targetTy, "cast.f32");
                }
                if (targetTy->isPointerTy())
                    return builder_->CreateIntToPtr(rawVal, targetTy, "cast.ptr");
                return builder_->CreateTrunc(rawVal, targetTy);
            }
            if (auto* srcStTy = llvm::dyn_cast<llvm::StructType>(val->getType())) {
                if (srcStTy->hasName() && srcStTy->getName().starts_with("__iface_")) {
                    auto* targetTy = toLLVMType(*castE->targetType);
                    if (targetTy->isStructTy()) {
                        auto* fn = builder_->GetInsertBlock()->getParent();
                        auto* ptrTy = llvm::PointerType::getUnqual(*context_);
                        auto* ifaceAlloca = createEntryBlockAlloca(fn, srcStTy, "downcast.iface");
                        builder_->CreateStore(val, ifaceAlloca);
                        auto* objPtr = builder_->CreateLoad(ptrTy,
                            builder_->CreateStructGEP(srcStTy, ifaceAlloca, 0), "downcast.obj");
                        auto* result = builder_->CreateLoad(targetTy, objPtr, "downcast.val");
                        if (castE->operand->kind == ExprKind::Identifier) {
                            classVarTypes_[castE->operand->as<const IdentifierExpr>()->name + ".downcast"] =
                                castE->targetType->name;
                        }
                        return result;
                    }
                }
            }

            auto* targetTy = toLLVMType(*castE->targetType);
            if (targetTy->isIntegerTy() && val->getType()->isStructTy()) {
                return val;
            }
            if (val->getType()->isIntegerTy() && targetTy->isIntegerTy() &&
                val->getType()->getIntegerBitWidth() < targetTy->getIntegerBitWidth() &&
                castE->operand && isUnsignedExpr(*castE->operand)) {
                return builder_->CreateZExt(val, targetTy, "uzext");
            }
            return numericCast(val, targetTy);
        }

        case ExprKind::Closure:
            return emitClosure(expr);

        case ExprKind::FailExpr:
            return emitFailExpr(expr);

        case ExprKind::TryExpr:
            return emitTryExpr(expr);

        case ExprKind::AwaitExpr:
            return emitAwaitExpr(expr);

        case ExprKind::InlineAsm: {
            auto* asmE = expr.as<const InlineAsmExpr>();
            std::vector<llvm::Type*> paramTypes;
            std::vector<llvm::Value*> argsV;
            for (auto& op : asmE->operands) {
                auto* v = emitExpr(*op);
                if (v) {
                    paramTypes.push_back(v->getType());
                    argsV.push_back(v);
                }
            }
            llvm::Type* retTy = llvm::Type::getVoidTy(*context_);
            if (!asmE->constraints.empty() && asmE->constraints[0] == '=') {
                retTy = llvm::Type::getInt64Ty(*context_);
            }
            auto* asmFnTy = llvm::FunctionType::get(retTy, paramTypes, false);
            auto* inlineAsm = llvm::InlineAsm::get(asmFnTy, asmE->asmTemplate,
                asmE->constraints, asmE->hasSideEffects);
            auto* call = builder_->CreateCall(asmFnTy, inlineAsm, argsV);
            if (retTy->isVoidTy())
                return llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0);
            return call;
        }

        case ExprKind::SelfExpr: {
            auto selfIt = namedValues_.find("self");
            if (selfIt != namedValues_.end()) {
                return builder_->CreateLoad(getValuePtrType(selfIt->second), selfIt->second, "self.ptr");
            }
            diag_.error(expr.location, "`self` is only valid inside class methods");
            return nullptr;
        }

        case ExprKind::Ternary: {
            auto* ternE = expr.as<const TernaryExpr>();
            if (!ternE->condition || !ternE->trueExpr || !ternE->falseExpr) {
                diag_.error(ternE->location, "ternary expression is missing condition or branch");
                return nullptr;
            }
            auto* cond = emitExpr(*ternE->condition);
            if (!cond) {
                diag_.error(ternE->location, "ternary: condition did not produce a value");
                return nullptr;
            }
            cond = coerceToBool(cond, "ternary.cond");

            if (isSimpleExpr(*ternE->trueExpr) && isSimpleExpr(*ternE->falseExpr)) {
                auto* trueVal = emitExpr(*ternE->trueExpr);
                auto* falseVal = emitExpr(*ternE->falseExpr);
                if (!trueVal || !falseVal) {
                    diag_.error(ternE->location, "ternary: branch expression did not produce a value");
                    return nullptr;
                }
                if (trueVal->getType() != falseVal->getType()) {
                    llvm::Type* commonTy = nullptr;
                    auto* trueTy = trueVal->getType();
                    auto* falseTy = falseVal->getType();
                    if (trueTy->isIntegerTy() && falseTy->isIntegerTy())
                        commonTy = trueTy->getIntegerBitWidth() >= falseTy->getIntegerBitWidth() ? trueTy : falseTy;
                    else if (trueTy->isFloatingPointTy() && falseTy->isFloatingPointTy())
                        commonTy = trueTy->getPrimitiveSizeInBits() >= falseTy->getPrimitiveSizeInBits() ? trueTy : falseTy;
                    else if (trueTy->isFloatingPointTy() && falseTy->isIntegerTy())
                        commonTy = trueTy;
                    else if (trueTy->isIntegerTy() && falseTy->isFloatingPointTy())
                        commonTy = falseTy;
                    else if (trueTy->isPointerTy() && falseTy->isPointerTy())
                        commonTy = trueTy;
                    if (commonTy) {
                        trueVal = castToType(trueVal, commonTy);
                        falseVal = castToType(falseVal, commonTy);
                    } else {
                        diag_.error(ternE->location, "ternary: incompatible branch value types");
                        return nullptr;
                    }
                }
                return builder_->CreateSelect(cond, trueVal, falseVal, "ternary.val");
            }

            auto* fn = builder_->GetInsertBlock()->getParent();
            auto* thenBB = llvm::BasicBlock::Create(*context_, "ternary.then", fn);
            auto* elseBB = llvm::BasicBlock::Create(*context_, "ternary.else", fn);
            auto* mergeBB = llvm::BasicBlock::Create(*context_, "ternary.merge", fn);
            builder_->CreateCondBr(cond, thenBB, elseBB);

            builder_->SetInsertPoint(thenBB);
            auto* trueVal = emitExpr(*ternE->trueExpr);
            auto* thenEnd = builder_->GetInsertBlock();

            builder_->SetInsertPoint(elseBB);
            auto* falseVal = emitExpr(*ternE->falseExpr);
            auto* elseEnd = builder_->GetInsertBlock();

            if (!trueVal || !falseVal) {
                if (!thenEnd->getTerminator()) builder_->SetInsertPoint(thenEnd), builder_->CreateBr(mergeBB);
                if (!elseEnd->getTerminator()) builder_->SetInsertPoint(elseEnd), builder_->CreateBr(mergeBB);
                builder_->SetInsertPoint(mergeBB);
                diag_.error(ternE->location, "ternary: branch expression did not produce a value");
                return nullptr;
            }

            auto* trueTy = trueVal->getType();
            auto* falseTy = falseVal->getType();
            if (trueTy != falseTy) {
                llvm::Type* commonTy = nullptr;
                if (trueTy->isIntegerTy() && falseTy->isIntegerTy()) {
                    commonTy = trueTy->getIntegerBitWidth() >= falseTy->getIntegerBitWidth() ? trueTy : falseTy;
                } else if (trueTy->isFloatingPointTy() && falseTy->isFloatingPointTy()) {
                    commonTy = trueTy->getPrimitiveSizeInBits() >= falseTy->getPrimitiveSizeInBits() ? trueTy : falseTy;
                } else if (trueTy->isFloatingPointTy() && falseTy->isIntegerTy()) {
                    commonTy = trueTy;
                } else if (trueTy->isIntegerTy() && falseTy->isFloatingPointTy()) {
                    commonTy = falseTy;
                } else if (trueTy->isPointerTy() && falseTy->isPointerTy()) {
                    commonTy = trueTy;
                }

                if (commonTy) {
                    builder_->SetInsertPoint(thenEnd);
                    if (thenEnd->getTerminator()) thenEnd->getTerminator()->eraseFromParent();
                    trueVal = castToType(trueVal, commonTy);
                    thenEnd = builder_->GetInsertBlock();
                    builder_->SetInsertPoint(elseEnd);
                    if (elseEnd->getTerminator()) elseEnd->getTerminator()->eraseFromParent();
                    falseVal = castToType(falseVal, commonTy);
                    elseEnd = builder_->GetInsertBlock();
                } else {
                    diag_.error(ternE->location, "ternary: incompatible branch value types");
                    if (!thenEnd->getTerminator()) builder_->SetInsertPoint(thenEnd), builder_->CreateBr(mergeBB);
                    if (!elseEnd->getTerminator()) builder_->SetInsertPoint(elseEnd), builder_->CreateBr(mergeBB);
                    builder_->SetInsertPoint(mergeBB);
                    return nullptr;
                }
            }

            if (!thenEnd->getTerminator()) { builder_->SetInsertPoint(thenEnd); builder_->CreateBr(mergeBB); }
            if (!elseEnd->getTerminator()) { builder_->SetInsertPoint(elseEnd); builder_->CreateBr(mergeBB); }
            builder_->SetInsertPoint(mergeBB);
            auto* phi = builder_->CreatePHI(trueVal->getType(), 2, "ternary.val");
            phi->addIncoming(trueVal, thenEnd);
            phi->addIncoming(falseVal, elseEnd);
            return phi;
        }

        // F: sizeof...(pack) — should be resolved to IntLiteralExpr by Sema/Mono,
        // but if it somehow survives emit a constant 0 so we don't ICE.
        case ExprKind::SizeofPack:
            return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), 0);

        // P3-Q: TypeReflect should be fully lowered to StringLiteralExpr by
        // Sema/Mono before CodeGen sees it.  If one somehow survives (e.g.
        // a non-generic function using T::name on an unresolved param), emit
        // an empty string constant and report a diagnostic so the user knows.
        case ExprKind::TypeReflect: {
            auto* re = expr.as<const TypeReflectExpr>();
            diag_.error(expr.location,
                "internal: TypeReflect '{}::{}' was not lowered by Sema/Mono; "
                "reflection is only valid inside generic functions",
                re->typeParam, re->member);
            return createStringValue("");
        }

        default:
            diag_.error(expr.location, "unhandled expression kind in codegen ({})", static_cast<int>(expr.kind));
            return nullptr;
    }
}

} // namespace vyx
