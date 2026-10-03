#include "CodeGenIncludes.h"
#include <algorithm>
#include <set>

namespace vyx {

llvm::Value* CodeGen::emitCall(const Expr& expr) {
    auto& call = static_cast<const CallExpr&>(expr);
    if (!call.callee) {
        diag_.error(call.location, "call expression has no callee");
        return nullptr;
    }

    if (call.callee && call.callee->kind == ExprKind::Identifier) {
        auto* ctResult = tryComptimeCall(call);
        if (ctResult) return ctResult;
    }

    // Dispatch to specialized builtin handlers
    if (call.callee->kind == ExprKind::Identifier) {
        auto& name = call.callee->as<IdentifierExpr>()->name;

        // Container constructors: Some/None/Ok/Err/ADT variants
        if (name == "Some" || name == "None" || name == "Ok" || name == "Err" ||
            name.find("::") != std::string::npos) {
            auto* result = emitBuiltinContainer(expr);
            if (result) return result;
        }

        // Memory/type intrinsics
        static const std::set<std::string> memBuiltins = {
            "sizeof", "alignof", "type_name", "alloc", "dealloc",
            "transmute", "from_cstr", "from_cstr_len", "from_cstr_view_len", "from_raw_string_parts", "to_rawptr", "typeinfo",
            "typeof"
        };
        if (memBuiltins.count(name)) {
            auto* result = emitBuiltinMemory(expr);
            if (result) return result;
        }

        // I/O and diagnostics
        if (name == "assert" || name == "panic" || name == "format") {
            auto* result = emitBuiltinIO(expr);
            if (result) return result;
        }

        // makeRef(val) → heap-allocate and wrap in __RefCounted struct
        if (name == "makeRef") {
            if (call.args.empty() || !call.args[0]) {
                diag_.error(call.location, "makeRef requires a value argument");
                return nullptr;
            }
            auto* val = emitExpr(*call.args[0]);
            if (!val) {
                diag_.error(call.location, "makeRef: argument did not produce a value");
                return nullptr;
            }
            auto* ptrTy = llvm::PointerType::getUnqual(*context_);
            auto* i64Ty = llvm::Type::getInt64Ty(*context_);
            auto* mallocFn = module_->getFunction("malloc");
            if (!mallocFn) {
                auto* fty = llvm::FunctionType::get(ptrTy, {i64Ty}, false);
                mallocFn = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, "malloc", *module_);
            }
            uint64_t origValSize = module_->getDataLayout().getTypeAllocSize(val->getType());
            uint64_t valSize = std::max<uint64_t>(8, origValSize);
            auto* rawMem = builder_->CreateCall(mallocFn,
                {llvm::ConstantInt::get(i64Ty, valSize)}, "ref.alloc");
            if (auto* memsetFn = module_->getFunction("memset")) {
                auto* i32Ty = llvm::Type::getInt32Ty(*context_);
                builder_->CreateCall(memsetFn, {
                    rawMem,
                    llvm::ConstantInt::get(i32Ty, 0),
                    llvm::ConstantInt::get(i64Ty, valSize)
                });
            }
            if (val->getType()->isIntegerTy() && valSize > origValSize) {
                unsigned targetBits = (unsigned)std::min<uint64_t>(64, valSize * 8);
                if (val->getType()->getIntegerBitWidth() < targetBits) {
                    val = builder_->CreateZExt(
                        val, llvm::IntegerType::get(*context_, targetBits), "makeRef.ext");
                }
            }
            builder_->CreateStore(val, rawMem);

            auto refIt = structTypes_.find("__RefCounted");
            llvm::StructType* refTy;
            if (refIt != structTypes_.end()) {
                refTy = refIt->second;
            } else {
                refTy = llvm::StructType::create(*context_, {ptrTy, i64Ty}, "__RefCounted");
                structTypes_["__RefCounted"] = refTy;
            }
            auto* fn = builder_->GetInsertBlock()->getParent();
            auto* alloca = createEntryBlockAlloca(fn, refTy, "ref.tmp");
            builder_->CreateStore(rawMem,
                builder_->CreateStructGEP(refTy, alloca, 0, "ref.ptr"));
            builder_->CreateStore(llvm::ConstantInt::get(i64Ty, 1),
                builder_->CreateStructGEP(refTy, alloca, 1, "ref.count"));
            return builder_->CreateLoad(refTy, alloca, "ref.val");
        }

    }

    // Functor direct call: identifier(args) where identifier is a struct variable
    // (the legacy `Event` multicast builtin — tagged in `containerTypes_` —
    // was removed in R5 step 10 along with `VyxTypeKind::Event`).
    if (call.callee->kind == ExprKind::Identifier) {
        auto varIt = namedValues_.find(call.callee->as<IdentifierExpr>()->name);
        if (varIt != namedValues_.end()) {
            // Functor: operator_call
            if (auto* stTy = llvm::dyn_cast<llvm::StructType>(getValuePtrType(varIt->second))) {
                std::string typeName = stTy->hasName() ? stTy->getName().str() : "";
                if (!typeName.empty()) {
                    std::string callFnName = typeName + ".operator_call";
                    auto fnIt = functions_.find(callFnName);
                    if (fnIt != functions_.end()) {
                        std::vector<llvm::Value*> callArgs = {varIt->second};
                        auto* opCallFnTy = fnIt->second->getFunctionType();
                        for (size_t ai = 0; ai < call.args.size(); ++ai) {
                            auto& arg = call.args[ai];
                            if (!arg) continue;
                            auto* av = emitExpr(*arg);
                            if (!av) {
                                diag_.error(call.location,
                                    "functor call: argument did not produce a value");
                                return nullptr;
                            }
                            // Param index is ai+1 because param 0 is self pointer
                            unsigned paramIdx = (unsigned)(ai + 1);
                            if (paramIdx < opCallFnTy->getNumParams()) {
                                auto* expectedTy = opCallFnTy->getParamType(paramIdx);
                                if (av->getType() != expectedTy && !expectedTy->isPointerTy())
                                    av = castToType(av, expectedTy);
                            }
                            callArgs.push_back(av);
                        }
                        return builder_->CreateCall(fnIt->second, callArgs, "call.result");
                    }
                }
            }
        }
    }

    // Handle method calls: obj.method(args)
    if (call.callee->kind == ExprKind::MemberAccess) {
        auto* result = emitMethodCall(expr);
        if (result) return result;
        // Fall through to regular member-function handling
    }

    if (call.callee->kind == ExprKind::Identifier) {
        auto& cn = call.callee->as<IdentifierExpr>()->name;
        std::string ctorKey = cn + ".__ctor_" + std::to_string(call.args.size());
        auto ctorIt = functions_.find(ctorKey);
        if (ctorIt == functions_.end()) ctorIt = functions_.find(cn + ".__ctor");
        if (ctorIt != functions_.end()) {
            auto* ptrTy = llvm::PointerType::getUnqual(*context_);
            auto* i64Ty = llvm::Type::getInt64Ty(*context_);
            uint64_t classSize = 8;
            auto sizeIt = structFieldNames_.find(cn + ".__size");
            if (sizeIt != structFieldNames_.end() && !sizeIt->second.empty())
                classSize = std::stoull(sizeIt->second[0]);

            auto& mTriple2 = module_->getTargetTriple();
            bool useMsvc3 = mTriple2.getTriple().empty() ||
                (mTriple2.isOSWindows() && mTriple2.getEnvironment() != llvm::Triple::GNU &&
                 mTriple2.getEnvironment() != llvm::Triple::Cygnus);
            std::string newName = useMsvc3 ? "??2@YAPEAX_K@Z" : "_Znwm";
            auto* newFn = module_->getFunction(newName);
            if (!newFn) {
                auto* newTy = llvm::FunctionType::get(ptrTy, {i64Ty}, false);
                newFn = llvm::Function::Create(newTy, llvm::Function::ExternalLinkage, newName, *module_);
            }
            auto* mem = builder_->CreateCall(newFn, {llvm::ConstantInt::get(i64Ty, classSize)}, "cpp.new");

            std::vector<llvm::Value*> ctorArgs = {mem};
            auto* ctorFn = ctorIt->second;
            for (size_t i = 0; i < call.args.size(); ++i) {
                auto* argVal = emitExpr(*call.args[i]);
                if (argVal && i + 1 < ctorFn->getFunctionType()->getNumParams())
                    argVal = castToType(argVal, ctorFn->getFunctionType()->getParamType(i + 1));
                if (argVal) ctorArgs.push_back(argVal);
            }
            builder_->CreateCall(ctorFn, ctorArgs);
            return mem;
        }
    }

    if (call.callee->kind == ExprKind::Identifier && call.callee->as<IdentifierExpr>()->name == "assert_eq") {
        if (call.args.size() < 2 || !call.args[0] || !call.args[1]) {
            diag_.error(call.location, "assert_eq requires two value arguments");
            return nullptr;
        }
        {
            auto* lhs = emitExpr(*call.args[0]);
            auto* rhs = emitExpr(*call.args[1]);
            if (!lhs || !rhs) {
                diag_.error(call.location, "assert_eq: argument did not produce a value");
                return nullptr;
            }
            {
                auto* fn = builder_->GetInsertBlock()->getParent();
                if (lhs->getType()->isStructTy() && rhs->getType()->isStructTy() &&
                    lhs->getType() != rhs->getType()) {
                    diag_.error(call.location,
                        "assert_eq: operands have different struct types; comparison is not defined");
                    return nullptr;
                }
                if (lhs->getType() != rhs->getType()) {
                    rhs = castToType(rhs, lhs->getType());
                }
                llvm::Value* isEq;
                if (lhs->getType()->isPointerTy() && rhs->getType()->isIntegerTy()) {
                    lhs = builder_->CreatePtrToInt(lhs, rhs->getType());
                } else if (lhs->getType()->isIntegerTy() && rhs->getType()->isPointerTy()) {
                    rhs = builder_->CreatePtrToInt(rhs, lhs->getType());
                }
                if (lhs->getType()->isStructTy() && rhs->getType()->isStructTy()) {
                    auto* lhsStTy = llvm::cast<llvm::StructType>(lhs->getType());
                    std::string opName = lhsStTy->hasName() ? lhsStTy->getName().str() + ".operator_eq" : "";
                    auto fnIt = opName.empty() ? functions_.end() : functions_.find(opName);
                    if (fnIt != functions_.end()) {
                        auto* lAlloca = createEntryBlockAlloca(fn, lhsStTy, "assert_eq.l");
                        auto* rAlloca = createEntryBlockAlloca(fn, lhsStTy, "assert_eq.r");
                        builder_->CreateStore(lhs, lAlloca);
                        builder_->CreateStore(rhs, rAlloca);
                        isEq = builder_->CreateCall(fnIt->second, {lAlloca, rAlloca}, "assert_eq.struct");
                    } else {
                        auto* ptrTy = llvm::PointerType::getUnqual(*context_);
                        auto* i64Ty = llvm::Type::getInt64Ty(*context_);
                        auto* i32Ty = llvm::Type::getInt32Ty(*context_);
                        // Special case: __String → compare content via memcmp on ptr/len
                        auto* strTy2 = getOrCreateStringType();
                        if (lhsStTy == strTy2) {
                            auto* lAlloca2 = createEntryBlockAlloca(fn, strTy2, "aeq.sl");
                            auto* rAlloca2 = createEntryBlockAlloca(fn, strTy2, "aeq.sr");
                            builder_->CreateStore(lhs, lAlloca2);
                            builder_->CreateStore(rhs, rAlloca2);
                            auto* lPtr = builder_->CreateLoad(ptrTy, builder_->CreateStructGEP(strTy2, lAlloca2, 0));
                            auto* lLen = builder_->CreateLoad(i64Ty, builder_->CreateStructGEP(strTy2, lAlloca2, 1));
                            auto* rPtr = builder_->CreateLoad(ptrTy, builder_->CreateStructGEP(strTy2, rAlloca2, 0));
                            auto* rLen = builder_->CreateLoad(i64Ty, builder_->CreateStructGEP(strTy2, rAlloca2, 1));
                            auto* lenEq = builder_->CreateICmpEQ(lLen, rLen, "aeq.leneq");
                            auto* memcmpFn = module_->getFunction("memcmp");
                            if (!memcmpFn) {
                                auto* fty = llvm::FunctionType::get(i32Ty, {ptrTy, ptrTy, i64Ty}, false);
                                memcmpFn = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, "memcmp", *module_);
                            }
                            auto* cmpResult = builder_->CreateCall(memcmpFn, {lPtr, rPtr, lLen}, "aeq.memcmp");
                            auto* contentEq = createSafeICmp(llvm::CmpInst::ICMP_EQ, cmpResult,
                                llvm::ConstantInt::get(cmpResult->getType(), 0), "aeq.ceq");
                            isEq = builder_->CreateAnd(lenEq, contentEq, "aeq.streq");
                        } else {
                            // Generic struct: compare full bytes via memcmp
                            auto* lAlloca2 = createEntryBlockAlloca(fn, lhsStTy, "assert_eq.l");
                            auto* rAlloca2 = createEntryBlockAlloca(fn, lhsStTy, "assert_eq.r");
                            builder_->CreateStore(lhs, lAlloca2);
                            builder_->CreateStore(rhs, rAlloca2);
                            auto* memcmpFn = module_->getFunction("memcmp");
                            if (!memcmpFn) {
                                auto* fty = llvm::FunctionType::get(i32Ty, {ptrTy, ptrTy, i64Ty}, false);
                                memcmpFn = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, "memcmp", *module_);
                            }
                            uint64_t sz = module_->getDataLayout().getTypeAllocSize(lhsStTy);
                            auto* cmpResult = builder_->CreateCall(memcmpFn,
                                {lAlloca2, rAlloca2, llvm::ConstantInt::get(i64Ty, sz)}, "assert_eq.memcmp");
                            isEq = createSafeICmp(llvm::CmpInst::ICMP_EQ, cmpResult,
                                llvm::ConstantInt::get(cmpResult->getType(), 0), "assert_eq.cmp");
                        }
                    }
                } else if (lhs->getType()->isFloatingPointTy()) {
                    isEq = builder_->CreateFCmpOEQ(lhs, rhs, "assert_eq.cmp");
                } else {
                    if (lhs->getType() != rhs->getType() &&
                        lhs->getType()->isIntegerTy() && rhs->getType()->isIntegerTy()) {
                        unsigned bL = lhs->getType()->getIntegerBitWidth();
                        unsigned bR = rhs->getType()->getIntegerBitWidth();
                        if (bL < bR) lhs = builder_->CreateSExt(lhs, rhs->getType(), "aeq.ext");
                        else         rhs = builder_->CreateSExt(rhs, lhs->getType(), "aeq.ext");
                    }
                    isEq = builder_->CreateICmpEQ(lhs, rhs, "assert_eq.cmp");
                }
                auto* passBB = llvm::BasicBlock::Create(*context_, "aeq.pass", fn);
                auto* failBB = llvm::BasicBlock::Create(*context_, "aeq.fail", fn);
                builder_->CreateCondBr(isEq, passBB, failBB);
                builder_->SetInsertPoint(failBB);
                auto* printfFn = getOrCreatePrintf();
                auto* msg = getOrCreateString("ASSERT_EQ FAILED\n");
                builder_->CreateCall(printfFn, {msg});
                auto* exitFn = module_->getFunction("exit");
                if (!exitFn) {
                    auto* exitTy = llvm::FunctionType::get(llvm::Type::getVoidTy(*context_), {llvm::Type::getInt32Ty(*context_)}, false);
                    exitFn = llvm::Function::Create(exitTy, llvm::Function::ExternalLinkage, "exit", *module_);
                }
                builder_->CreateCall(exitFn, {llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 1)});
                builder_->CreateUnreachable();
                builder_->SetInsertPoint(passBB);
            }
            }
        return llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0);
    }

    // getArg: moved to std/args.vyx
    if (call.callee->kind == ExprKind::Identifier && call.callee->as<IdentifierExpr>()->name == "getArg") {
        if (call.args.empty() || !call.args[0]) {
            diag_.error(call.location, "getArg requires a argv index argument");
            return nullptr;
        }
        auto* idx = emitExpr(*call.args[0]);
        if (!idx) {
            diag_.error(call.location, "getArg: index expression did not produce a value");
            return nullptr;
        }
        auto* ptrTy = llvm::PointerType::getUnqual(*context_);
        auto* i64Ty = llvm::Type::getInt64Ty(*context_);
        auto argvIt = namedValues_.find("argv");
        if (argvIt != namedValues_.end()) {
            auto* argv = builder_->CreateLoad(ptrTy, argvIt->second, "argv.ptr");
            auto* idx64 = builder_->CreateSExt(idx, i64Ty);
            auto* strPtrPtr = builder_->CreateGEP(ptrTy, argv, idx64, "argv.elem.ptr");
            auto* cStr = builder_->CreateLoad(ptrTy, strPtrPtr, "argv.cstr");
            auto* fn = builder_->GetInsertBlock()->getParent();
            auto* strTy = getOrCreateStringType();
            auto* alloca = createEntryBlockAlloca(fn, strTy, "arg.str");
            builder_->CreateStore(cStr, builder_->CreateStructGEP(strTy, alloca, 0));
            auto* strlenFn = module_->getFunction("strlen");
            if (!strlenFn) {
                auto* fty = llvm::FunctionType::get(i64Ty, {ptrTy}, false);
                strlenFn = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, "strlen", *module_);
            }
            auto* len = builder_->CreateCall(strlenFn, {cStr}, "arg.len");
            builder_->CreateStore(len, builder_->CreateStructGEP(strTy, alloca, 1));
            builder_->CreateStore(len, builder_->CreateStructGEP(strTy, alloca, 2));
            builder_->CreateStore(llvm::ConstantInt::get(i64Ty, 0), builder_->CreateStructGEP(strTy, alloca, 3));
            return builder_->CreateLoad(strTy, alloca, "arg.val");
        }
        // Fallback: runtime argv table (when argc/argv are not in scope as locals)
        auto* strTy = getOrCreateStringType();
        auto* getArgFn = module_->getFunction("_vyx_get_arg_internal");
        if (!getArgFn) {
            auto* i32Ty = llvm::Type::getInt32Ty(*context_);
            auto* fty = llvm::FunctionType::get(strTy, {i32Ty}, false);
            getArgFn = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, "_vyx_get_arg_internal", *module_);
        }
        auto* idx32 = idx;
        if (idx->getType() != llvm::Type::getInt32Ty(*context_))
            idx32 = builder_->CreateTrunc(idx, llvm::Type::getInt32Ty(*context_));
        return builder_->CreateCall(getArgFn, {idx32}, "arg.val");
    }

    // getArgs() → convert C argc/argv to Vec<string>
    if (call.callee->kind == ExprKind::Identifier && call.callee->as<IdentifierExpr>()->name == "getArgs") {
        auto* ptrTy = llvm::PointerType::getUnqual(*context_);
        auto* i64Ty = llvm::Type::getInt64Ty(*context_);
        auto* i32Ty = llvm::Type::getInt32Ty(*context_);
        auto* fn = builder_->GetInsertBlock()->getParent();

        auto argcIt = namedValues_.find("argc");
        auto argvIt = namedValues_.find("argv");
        if (argcIt == namedValues_.end() || argvIt == namedValues_.end()) {
            auto vecIt = structTypes_.find("__Vec");
            llvm::StructType* vecTy;
            if (vecIt != structTypes_.end()) vecTy = vecIt->second;
            else {
                vecTy = llvm::StructType::create(*context_,
                    {ptrTy, i64Ty, i64Ty}, "__Vec");
                structTypes_["__Vec"] = vecTy;
            }
            auto* alloca = createEntryBlockAlloca(fn, vecTy, "empty.args");
            auto* nullPtr = llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy));
            builder_->CreateStore(nullPtr, builder_->CreateStructGEP(vecTy, alloca, 0));
            builder_->CreateStore(llvm::ConstantInt::get(i64Ty, 0), builder_->CreateStructGEP(vecTy, alloca, 1));
            builder_->CreateStore(llvm::ConstantInt::get(i64Ty, 0), builder_->CreateStructGEP(vecTy, alloca, 2));
            return builder_->CreateLoad(vecTy, alloca, "empty.args.val");
        }

        auto* argc = builder_->CreateLoad(i32Ty, argcIt->second, "argc.val");
        auto* argv = builder_->CreateLoad(ptrTy, argvIt->second, "argv.ptr");
        auto* argc64 = builder_->CreateSExt(argc, i64Ty, "argc64");

        // Allocate Vec<string>: {ptr data, i64 len, i64 cap}
        auto vecIt2 = structTypes_.find("__Vec");
        llvm::StructType* vecTy;
        if (vecIt2 != structTypes_.end()) vecTy = vecIt2->second;
        else {
            vecTy = llvm::StructType::create(*context_, {ptrTy, i64Ty, i64Ty}, "__Vec");
            structTypes_["__Vec"] = vecTy;
        }
        auto* strTy = getOrCreateStringType();
        auto* strSize = llvm::ConstantInt::get(i64Ty, module_->getDataLayout().getTypeAllocSize(strTy));
        auto* totalSize = builder_->CreateMul(argc64, strSize, "args.total");
        auto* mallocFn = module_->getFunction("malloc");
        if (!mallocFn) {
            auto* fty = llvm::FunctionType::get(ptrTy, {i64Ty}, false);
            mallocFn = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, "malloc", *module_);
        }
        auto* dataBuf = builder_->CreateCall(mallocFn, {totalSize}, "args.data");
        auto* strlenFn = module_->getFunction("strlen");
        if (!strlenFn) {
            auto* fty = llvm::FunctionType::get(i64Ty, {ptrTy}, false);
            strlenFn = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, "strlen", *module_);
        }

        // Loop: for i = 0..argc, copy argv[i] as Vyx string into Vec
        auto* idxAlloca = createEntryBlockAlloca(fn, i64Ty, "args.idx");
        builder_->CreateStore(llvm::ConstantInt::get(i64Ty, 0), idxAlloca);
        auto* loopBB = llvm::BasicBlock::Create(*context_, "args.loop", fn);
        auto* bodyBB = llvm::BasicBlock::Create(*context_, "args.body", fn);
        auto* doneBB = llvm::BasicBlock::Create(*context_, "args.done", fn);
        builder_->CreateBr(loopBB);
        builder_->SetInsertPoint(loopBB);
        auto* idx = builder_->CreateLoad(i64Ty, idxAlloca);
        builder_->CreateCondBr(builder_->CreateICmpSLT(idx, argc64), bodyBB, doneBB);
        builder_->SetInsertPoint(bodyBB);
        auto* cStrPtr = builder_->CreateGEP(ptrTy, argv, idx, "argv.i.ptr");
        auto* cStr = builder_->CreateLoad(ptrTy, cStrPtr, "argv.i.cstr");
        auto* len = builder_->CreateCall(strlenFn, {cStr}, "argv.i.len");
        auto* destPtr = builder_->CreateGEP(strTy, dataBuf, idx, "args.dest");
        builder_->CreateStore(cStr, builder_->CreateStructGEP(strTy, destPtr, 0));
        builder_->CreateStore(len, builder_->CreateStructGEP(strTy, destPtr, 1));
        builder_->CreateStore(len, builder_->CreateStructGEP(strTy, destPtr, 2));
        builder_->CreateStore(llvm::ConstantInt::get(i64Ty, 0), builder_->CreateStructGEP(strTy, destPtr, 3));
        builder_->CreateStore(builder_->CreateAdd(idx, llvm::ConstantInt::get(i64Ty, 1)), idxAlloca);
        builder_->CreateBr(loopBB);

        builder_->SetInsertPoint(doneBB);
        auto* vecAlloca = createEntryBlockAlloca(fn, vecTy, "args.vec");
        builder_->CreateStore(dataBuf, builder_->CreateStructGEP(vecTy, vecAlloca, 0));
        builder_->CreateStore(argc64, builder_->CreateStructGEP(vecTy, vecAlloca, 1));
        builder_->CreateStore(argc64, builder_->CreateStructGEP(vecTy, vecAlloca, 2));
        return builder_->CreateLoad(vecTy, vecAlloca, "args.result");
    }

    // argCount() → return the argc parameter from main, or fall back to internal helper
    if (call.callee->kind == ExprKind::Identifier && call.callee->as<IdentifierExpr>()->name == "argCount") {
        auto argcIt = namedValues_.find("argc");
        if (argcIt != namedValues_.end()) {
            return builder_->CreateLoad(llvm::Type::getInt32Ty(*context_), argcIt->second, "argc.val");
        }
        auto* i32Ty = llvm::Type::getInt32Ty(*context_);
        auto* getArgcFn = module_->getFunction("_vyx_get_argc_internal");
        if (!getArgcFn) {
            auto* fty = llvm::FunctionType::get(i32Ty, {}, false);
            getArgcFn = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, "_vyx_get_argc_internal", *module_);
        }
        return builder_->CreateCall(getArgcFn, {}, "argc.val");
    }

    // Handle built-in print()
    if (call.callee->kind == ExprKind::Identifier && call.callee->as<IdentifierExpr>()->name == "print") {
        // Special case: print with string interpolation
        if (call.args.size() == 1 && call.args[0] &&
            call.args[0]->kind == ExprKind::StringInterpolation) {
            // Build format string with trailing newline
            auto* interpExpr = call.args[0]->as<StringInterpExpr>();
            std::string fmtStr;
            std::vector<llvm::Value*> printfArgs;
            auto* strTyPrintShared = getOrCreateStringType();
            // Phase 7 (2026-04-23): same Display-trait dispatch as
            // emitStringInterpolation — when `<Type>.display` is available
            // (primitives via `impl Display for <prim>` in std/fmt.vyx, user
            // classes via regular `impl Display`), call it and print the
            // returned string via `%.*s`; otherwise fall through to the
            // hardcoded switch so types without Display keep working.
            auto classNameForDisplayP = [&](const Expr& e, llvm::Value* v) -> std::string {
                std::string nm = inferredClassName(e);
                if (!nm.empty() && findClassMethod(nm, "display")) return nm;
                if (v) {
                    auto* vt = v->getType();
                    if (vt == strTyPrintShared) {
                        if (findClassMethod("string", "display")) return "string";
                    } else if (vt->isIntegerTy(1) || vt->isIntegerTy(8)) {
                        // bool at most use-sites is i8; i1 only in limited
                        // pass-through contexts.  Skip for unsigned so u8
                        // doesn't mis-route through `bool.display`.
                        if (!isUnsignedExpr(e) && findClassMethod("bool", "display"))
                            return "bool";
                    } else if (vt->isIntegerTy(32)) {
                        if (!isUnsignedExpr(e) && findClassMethod("i32", "display"))
                            return "i32";
                    } else if (vt->isIntegerTy(64)) {
                        if (!isUnsignedExpr(e) && findClassMethod("i64", "display"))
                            return "i64";
                    } else if (vt->isFloatTy()) {
                        if (findClassMethod("f32", "display")) return "f32";
                    } else if (vt->isDoubleTy()) {
                        if (findClassMethod("f64", "display")) return "f64";
                    }
                }
                return {};
            };
            for (auto& part : interpExpr->parts) {
                if (part.isExpr && part.expr) {
                    auto* val = emitExpr(*part.expr);
                    if (val) {
                        // String-identity short-circuit for `${s}` where s
                        // is already a string (Display is identity there).
                        if (val->getType() == strTyPrintShared) {
                            fmtStr += "%.*s";
                            auto* fn2 = builder_->GetInsertBlock()->getParent();
                            auto* strTy2 = strTyPrintShared;
                            auto* tmpA = createEntryBlockAlloca(fn2, strTy2, "pi.str");
                            builder_->CreateStore(val, tmpA);
                            auto* sLen = builder_->CreateLoad(llvm::Type::getInt64Ty(*context_),
                                builder_->CreateStructGEP(strTy2, tmpA, 1));
                            printfArgs.push_back(builder_->CreateTrunc(sLen, llvm::Type::getInt32Ty(*context_)));
                            val = builder_->CreateLoad(llvm::PointerType::getUnqual(*context_),
                                builder_->CreateStructGEP(strTy2, tmpA, 0));
                            printfArgs.push_back(val);
                            continue;
                        }
                        std::string dispCls = classNameForDisplayP(*part.expr, val);
                        if (!dispCls.empty()) {
                            llvm::Function* dispFn = findClassMethod(dispCls, "display");
                            if (dispFn && dispFn->getFunctionType()->getNumParams() >= 1) {
                                auto* param0Ty = dispFn->getFunctionType()->getParamType(0);
                                auto* fn2 = builder_->GetInsertBlock()->getParent();
                                llvm::Value* selfArg = nullptr;
                                if (param0Ty->isPointerTy()) {
                                    auto* tmpSelf = createEntryBlockAlloca(fn2, val->getType(), "disp.self");
                                    builder_->CreateStore(val, tmpSelf);
                                    selfArg = tmpSelf;
                                } else if (param0Ty == val->getType()) {
                                    selfArg = val;
                                } else if (param0Ty->isIntegerTy() && val->getType()->isIntegerTy()) {
                                    // Width fixup — widen or narrow across
                                    // int widths.  Narrow (trunc) is needed
                                    // for bool whose value repr is i8 while
                                    // `impl Display for bool` self is i1.
                                    unsigned pbw = param0Ty->getIntegerBitWidth();
                                    unsigned vbw = val->getType()->getIntegerBitWidth();
                                    if (pbw > vbw) {
                                        selfArg = isUnsignedExpr(*part.expr)
                                            ? builder_->CreateZExt(val, param0Ty)
                                            : builder_->CreateSExt(val, param0Ty);
                                    } else {
                                        selfArg = builder_->CreateTrunc(val, param0Ty);
                                    }
                                }
                                if (selfArg) {
                                    auto* retStr = builder_->CreateCall(dispFn, {selfArg}, "disp.ret");
                                    fmtStr += "%.*s";
                                    auto* strTy2 = strTyPrintShared;
                                    auto* tmpA = createEntryBlockAlloca(fn2, strTy2, "pi.disp.str");
                                    builder_->CreateStore(retStr, tmpA);
                                    auto* sLen = builder_->CreateLoad(llvm::Type::getInt64Ty(*context_),
                                        builder_->CreateStructGEP(strTy2, tmpA, 1));
                                    printfArgs.push_back(builder_->CreateTrunc(sLen, llvm::Type::getInt32Ty(*context_)));
                                    auto* sPtr = builder_->CreateLoad(llvm::PointerType::getUnqual(*context_),
                                        builder_->CreateStructGEP(strTy2, tmpA, 0));
                                    printfArgs.push_back(sPtr);
                                    continue;
                                }
                            }
                        }
                        // Fallback hardcoded path for types without Display.
                        if (val->getType()->isIntegerTy()) {
                            bool unsignedPart = isUnsignedExpr(*part.expr);
                            if (val->getType()->getIntegerBitWidth() > 32) {
                                fmtStr += unsignedPart ? "%llu" : "%lld";
                            } else {
                                fmtStr += unsignedPart ? "%u" : "%d";
                                if (val->getType()->getIntegerBitWidth() < 32) {
                                    val = unsignedPart
                                        ? builder_->CreateZExt(val, llvm::Type::getInt32Ty(*context_))
                                        : builder_->CreateSExt(val, llvm::Type::getInt32Ty(*context_));
                                }
                            }
                        } else if (val->getType()->isFloatingPointTy()) {
                            fmtStr += "%f";
                            if (val->getType()->isFloatTy())
                                val = builder_->CreateFPExt(val, llvm::Type::getDoubleTy(*context_));
                        } else if (val->getType()->isPointerTy()) {
                            fmtStr += "%s";
                        } else if (val->getType()->isStructTy()) {
                            fmtStr += "%s";
                            auto* fn = builder_->GetInsertBlock()->getParent();
                            auto* tmp = createEntryBlockAlloca(fn, val->getType(), "print.struct.tmp");
                            builder_->CreateStore(val, tmp);
                            auto* stTy = llvm::cast<llvm::StructType>(val->getType());
                            std::string debugName = stTy->getName().str() + ".toString";
                            auto fnIt = functions_.find(debugName);
                            if (fnIt != functions_.end()) {
                                auto* strVal = builder_->CreateCall(fnIt->second, {tmp}, "print.struct.str");
                                val = extractStringPtr(strVal);
                            } else {
                                val = getOrCreateString("<struct>");
                            }
                        }
                        printfArgs.push_back(val);
                    }
                } else {
                    // Escape `%` so printf doesn't eat literal percent chars.
                    for (char c : part.text) {
                        if (c == '%') fmtStr += "%%";
                        else fmtStr += c;
                    }
                }
            }
            fmtStr += "\n";
            auto* fmtVal = getOrCreateString(fmtStr);
            std::vector<llvm::Value*> allArgs = {fmtVal};
            allArgs.insert(allArgs.end(), printfArgs.begin(), printfArgs.end());
            auto* printResult = builder_->CreateCall(getOrCreatePrintf(), allArgs, "print.interp");
            auto* i32Ty = llvm::Type::getInt32Ty(*context_);
            auto* ptrTy = llvm::PointerType::getUnqual(*context_);
            auto* ff = module_->getFunction("fflush");
            if (!ff) {
                auto* ft = llvm::FunctionType::get(i32Ty, {ptrTy}, false);
                ff = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "fflush", *module_);
            }
            builder_->CreateCall(ff, {llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy))});
            return printResult;
        }

        fprintf(stderr, "[CodeGen] emitCallExpr: print() call with %zu args\n", call.args.size()); fflush(stderr);
        std::vector<llvm::Value*> printArgs;
        for (auto& arg : call.args) {
            if (arg) {
                auto* v = emitExpr(*arg);
                if (v) printArgs.push_back(v);
            }
        }
        return emitPrintCall(printArgs);
    }

    llvm::Function* calleeFunc = nullptr;
    std::string calleeName;
    std::vector<llvm::Value*> cachedArgValues(call.args.size(), nullptr);

    if (call.callee->kind == ExprKind::Identifier) {
        calleeName = call.callee->as<IdentifierExpr>()->name;
        auto it = functions_.find(calleeName);
        if (it != functions_.end()) {
            calleeFunc = it->second;
        } else {
            calleeFunc = module_->getFunction(calleeName);
        }
        if (!calleeFunc) {
            auto sep = calleeName.find("::");
            if (sep != std::string::npos) {
                std::string unqual = calleeName.substr(sep + 2);
                auto it2 = functions_.find(unqual);
                if (it2 != functions_.end()) {
                    calleeFunc = it2->second;
                    calleeName = unqual;
                } else {
                    calleeFunc = module_->getFunction(unqual);
                    if (calleeFunc) calleeName = unqual;
                }
            }
        }

        // Check namedValues_ for function pointer variables (closures) BEFORE fallback.
        // Fat-pointer closures (capturing closures returned by higher-order functions)
        // are stored as a ptr to a heap-allocated __closure_fat_ptr { fn_ptr, env_ptr }.
        // The inner closure function takes env_ptr as its first (hidden) argument.
        if (!calleeFunc && closureFatPtrVars_.count(calleeName)) {
            auto varIt = namedValues_.find(calleeName);
            if (varIt != namedValues_.end()) {
                auto* ptrTy = llvm::PointerType::getUnqual(*context_);
                auto* fatPtrTy = getOrCreateClosureFatPtrType();
                // Load the heap pointer to the fat struct from the local alloca.
                auto* heapPtr = builder_->CreateLoad(ptrTy, varIt->second, calleeName + ".heap");
                // Extract fn_ptr (field 0) and env_ptr (field 1) from the heap fat struct.
                auto* fnPtrAddr = builder_->CreateStructGEP(fatPtrTy, heapPtr, 0, calleeName + ".fnptr.addr");
                auto* envPtrAddr = builder_->CreateStructGEP(fatPtrTy, heapPtr, 1, calleeName + ".envptr.addr");
                auto* fnPtr = builder_->CreateLoad(ptrTy, fnPtrAddr, calleeName + ".fnptr");
                auto* envPtr = builder_->CreateLoad(ptrTy, envPtrAddr, calleeName + ".envptr");
                // Build argument list: env_ptr first, then user args.
                std::vector<llvm::Value*> argsV = {envPtr};
                std::vector<llvm::Type*> argTypes = {ptrTy};
                // Pull the closure's return type recorded when the fat
                // pointer was built (via emitVarDecl). Previously this
                // was hard-coded to i64, which silently truncated struct
                // returns (notably %__String) to the low 8 bytes.
                llvm::Type* retTy = llvm::Type::getInt64Ty(*context_);
                auto mapIt = closureFatPtrVars_.find(calleeName);
                if (mapIt != closureFatPtrVars_.end() && mapIt->second)
                    retTy = mapIt->second;
                for (auto& arg : call.args) {
                    auto* v = emitExpr(*arg);
                    if (v) {
                        argsV.push_back(v);
                        argTypes.push_back(v->getType());
                    }
                }
                auto* fnTy = llvm::FunctionType::get(retTy, argTypes, false);
                return builder_->CreateCall(fnTy, fnPtr, argsV, calleeName + ".fat.call");
            }
        }

        if (!calleeFunc) {
            auto varIt = namedValues_.find(calleeName);
            if (varIt != namedValues_.end()) {
                auto* ptrVal = builder_->CreateLoad(getValuePtrType(varIt->second), varIt->second, calleeName + ".ptr");
                if (ptrVal->getType()->isPointerTy()) {
                    std::vector<llvm::Value*> argsV;
                    std::vector<llvm::Type*> argTypes;
                    llvm::Type* retTy = llvm::Type::getInt64Ty(*context_);
                    for (auto& arg : call.args) {
                        auto* v = emitExpr(*arg);
                        if (v) {
                            argsV.push_back(v);
                            argTypes.push_back(v->getType());
                        }
                    }
                    auto* fnTy = llvm::FunctionType::get(retTy, argTypes, false);
                    return builder_->CreateCall(fnTy, ptrVal, argsV, "indirect_call");
                }
            }
        }

        if (!calleeFunc) {
            llvm::Type* retTy = llvm::Type::getInt64Ty(*context_);

            std::vector<llvm::Type*> paramTypes;
            for (size_t i = 0; i < call.args.size(); ++i) {
                const auto& arg = call.args[i];
                if (arg && arg->kind == ExprKind::Identifier && arg->as<IdentifierExpr>()->typeAnnotation) {
                    paramTypes.push_back(toLLVMType(*arg->as<IdentifierExpr>()->typeAnnotation));
                } else {
                    auto* inferredArg = arg ? emitExpr(*arg) : nullptr;
                    cachedArgValues[i] = inferredArg;
                    paramTypes.push_back(inferredArg ? inferredArg->getType() : llvm::Type::getInt32Ty(*context_));
                }
            }

            auto* fnType = llvm::FunctionType::get(retTy, paramTypes, false);
            calleeFunc = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, calleeName, *module_);
            functions_[calleeName] = calleeFunc;
        }
    }

    if (!calleeFunc && call.callee->kind != ExprKind::Identifier) {
        auto* calleeVal = emitExpr(*call.callee);
        if (calleeVal && calleeVal->getType()->isPointerTy()) {
            const VyxType* fnInfo =
                (call.callee->inferredType &&
                 call.callee->inferredType->kind == VyxTypeKind::Function)
                    ? call.callee->inferredType.get()
                    : nullptr;
            llvm::Type* retTy = llvm::Type::getInt64Ty(*context_);
            if (fnInfo && fnInfo->returnType) {
                auto* inferredRet = toLLVMType(*fnInfo->returnType);
                if (inferredRet && !inferredRet->isVoidTy()) retTy = inferredRet;
            }

            std::vector<llvm::Value*> argsV;
            std::vector<llvm::Type*> argTypes;
            for (size_t ai = 0; ai < call.args.size(); ++ai) {
                auto& arg = call.args[ai];
                if (arg) {
                    auto* v = emitExpr(*arg);
                    if (v) {
                        if (fnInfo && ai < fnInfo->paramTypes.size() &&
                            fnInfo->paramTypes[ai]) {
                            auto* expectedTy = toLLVMType(*fnInfo->paramTypes[ai]);
                            if (expectedTy && !expectedTy->isVoidTy())
                                v = castToType(v, expectedTy);
                        }
                        argsV.push_back(v);
                        argTypes.push_back(v->getType());
                    }
                }
            }
            if (fnInfo && !llvm::isa<llvm::Function>(calleeVal)) {
                auto* ptrTy = llvm::PointerType::getUnqual(*context_);
                auto* fatPtrTy = getOrCreateClosureFatPtrType();
                auto* fnPtrAddr = builder_->CreateStructGEP(
                    fatPtrTy, calleeVal, 0, "iife.fnptr.addr");
                auto* envPtrAddr = builder_->CreateStructGEP(
                    fatPtrTy, calleeVal, 1, "iife.envptr.addr");
                auto* fnPtr = builder_->CreateLoad(ptrTy, fnPtrAddr, "iife.fnptr");
                auto* envPtr = builder_->CreateLoad(ptrTy, envPtrAddr, "iife.envptr");
                argsV.insert(argsV.begin(), envPtr);
                argTypes.insert(argTypes.begin(), ptrTy);
                auto* fnTy = llvm::FunctionType::get(retTy, argTypes, false);
                return builder_->CreateCall(fnTy, fnPtr, argsV, "iife.fat.call");
            }
            auto* fnTy = llvm::FunctionType::get(retTy, argTypes, false);
            return builder_->CreateCall(fnTy, calleeVal, argsV, "iife_call");
        }
    }

    if (!calleeFunc) {
        // When the callee is a MemberAccess (obj.method(...)), `calleeName`
        // stays empty and the bare "undefined function ''" is useless.
        // Reconstruct a readable "obj.method" form so the user sees which
        // call couldn't be dispatched — usually it means the class or
        // receiver type doesn't define that method (or it has a different
        // signature than what we're looking for).
        std::string displayName = calleeName;
        if (displayName.empty() && call.callee && call.callee->kind == ExprKind::MemberAccess) {
            auto* ma = call.callee->as<const MemberAccessExpr>();
            std::string objName = "<expr>";
            if (ma->object) {
                if (ma->object->kind == ExprKind::Identifier) {
                    objName = ma->object->as<const IdentifierExpr>()->name;
                } else if (ma->object->kind == ExprKind::SelfExpr) {
                    objName = "self";
                }
            }
            displayName = objName + "." + ma->member;
        }
        diag_.error(call.location, "undefined function '{}'", displayName);
        return llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0);
    }

    // Pre-compute fn-typed param mask from the callee's FunctionDecl so we
    // can uniformly wrap raw fn identifiers / plain fn-ptrs into fat
    // pointers before passing — the callee body reads these as
    // {fn_ptr, env_ptr} (see closureFatPtrVars_ registration in
    // CodeGenDecl / CodeGenClass).
    std::vector<bool> calleeParamIsFnTyped;
    if (!calleeName.empty() && unit_) {
        for (auto& d : unit_->declarations) {
            if (!d || d->kind != DeclKind::Function || d->name != calleeName) continue;
            auto* fd = d->as<FunctionDecl>();
            calleeParamIsFnTyped.resize(fd->params.size(), false);
            for (size_t pi = 0; pi < fd->params.size(); ++pi) {
                calleeParamIsFnTyped[pi] = fd->params[pi].type &&
                    fd->params[pi].type->kind == TypeAnnotationKind::Function;
            }
            break;
        }
    }

    std::vector<llvm::Value*> argsV;
    for (size_t i = 0; i < call.args.size(); ++i) {
        auto* savedHint = targetTypeHint_;
        if (i < calleeFunc->getFunctionType()->getNumParams()) {
            if (auto* pStTy = llvm::dyn_cast<llvm::StructType>(
                    calleeFunc->getFunctionType()->getParamType(i))) {
                if (pStTy->hasName()) {
                    auto nm = pStTy->getName();
                    if (isOptionOrResultSlotName(nm))
                        targetTypeHint_ = pStTy;
                }
            }
        }
        auto* argVal = (i < cachedArgValues.size() && cachedArgValues[i]) ? cachedArgValues[i] : emitExpr(*call.args[i]);
        targetTypeHint_ = savedHint;
        if (argVal) {
            // Raw-fn → fat-ptr wrap: callee's fn-typed params are read via
            // the closureFatPtrVars_ fat-ptr extract path. If the arg is
            // either a bare llvm::Function* (e.g. `apply(id_i32, 7)`) or
            // a plain ptr loaded from a fn-typed local that stores only
            // the raw fn pointer (e.g. `let f1: fn(i32)->i32 = id_i32;
            // apply(f1, 7);`), synthesise a trampoline and wrap into a
            // {fn_ptr, null_env} fat-ptr. Without this the callee
            // extracts fn_ptr from what is actually the first bytes of
            // the fn's code and segfaults.
            if (i < calleeParamIsFnTyped.size() && calleeParamIsFnTyped[i]) {
                if (auto* rawFn = llvm::dyn_cast<llvm::Function>(argVal)) {
                    argVal = wrapRawFnAsFatPtr(rawFn);
                } else if (call.args[i] && call.args[i]->kind == ExprKind::Identifier) {
                    // Local variable holding a raw fn ptr: look up the
                    // Function it aliases and wrap that. A ptr loaded
                    // from a fn-typed var that is NOT registered in
                    // closureFatPtrVars_ points at raw machine code,
                    // not a fat struct — treat it like a bare Function.
                    auto& nm = call.args[i]->as<IdentifierExpr>()->name;
                    if (!closureFatPtrVars_.count(nm)) {
                        if (auto fit = functions_.find(nm); fit != functions_.end()) {
                            argVal = wrapRawFnAsFatPtr(fit->second);
                        }
                    }
                }
            }
            // Interface boxing for call arguments: concrete struct → {obj_ptr, vtable_ptr}
            if (i < calleeFunc->getFunctionType()->getNumParams()) {
                auto* paramTy = calleeFunc->getFunctionType()->getParamType(i);
                if (auto* paramStTy = llvm::dyn_cast<llvm::StructType>(paramTy)) {
                    if (paramStTy->hasName() && paramStTy->getName().starts_with("__iface_") &&
                        !argVal->getType()->isStructTy()) {
                        // Skip if already an interface pair
                    } else if (paramStTy->hasName() && paramStTy->getName().starts_with("__iface_") &&
                        argVal->getType()->isStructTy() &&
                        argVal->getType() != paramStTy) {
                        auto ifaceName = paramStTy->getName().substr(8).str();
                        std::string concreteName;
                        if (call.args[i]->kind == ExprKind::StructInit)
                            concreteName = call.args[i]->as<StructInitExpr>()->structName;
                        else if (call.args[i]->kind == ExprKind::Identifier) {
                            concreteName = resolveClassName(*call.args[i],
                                call.args[i]->as<IdentifierExpr>()->name);
                        }
                        // Fallback: when the arg is a call expression like
                        // `Circle.new(1.0)`, neither of the above branches
                        // matches, but the LLVM struct type of argVal
                        // carries the concrete class name directly.
                        // Without this fallback, iface boxing silently
                        // skipped, a zero-padded iface value reached the
                        // callee, and later dispatch followed a NULL
                        // vtable pointer.
                        if (concreteName.empty()) {
                            if (auto* argSt = llvm::dyn_cast<llvm::StructType>(argVal->getType())) {
                                if (argSt->hasName()) {
                                    concreteName = argSt->getName().str();
                                }
                            }
                        }
                        // Never re-box an already-boxed interface value —
                        // `let c: Shape = Circle.of(...); describe(c)` must
                        // pass `c` through as-is. Double-boxing treated the
                        // existing {obj_ptr, vtbl_ptr} pair as a concrete
                        // class, stored it into iface.obj_ptr, and left
                        // iface.vtbl_ptr uninitialized → segfault on
                        // dispatch.
                        if (!concreteName.empty() &&
                            concreteName.rfind("__iface_", 0) == 0) {
                            concreteName.clear();
                        }

                        if (!concreteName.empty()) {
                            auto* fn2 = builder_->GetInsertBlock()->getParent();
                            auto* objStorage = createEntryBlockAlloca(fn2, argVal->getType(), "iface.arg.obj");
                            builder_->CreateStore(argVal, objStorage);
                            auto* ifaceAlloca = createEntryBlockAlloca(fn2, paramStTy, "iface.arg");
                            builder_->CreateStore(objStorage,
                                builder_->CreateStructGEP(paramStTy, ifaceAlloca, 0));
                            std::string vtName = concreteName + "_vtable_" + ifaceName;
                            auto* vtGlobal = module_->getGlobalVariable(vtName, true);
                            if (vtGlobal) {
                                builder_->CreateStore(vtGlobal,
                                    builder_->CreateStructGEP(paramStTy, ifaceAlloca, 1));
                            }
                            argVal = builder_->CreateLoad(paramStTy, ifaceAlloca, "iface.arg.val");
                        }
                    }
                }
                // Auto-deref Ref<T> → T: extract data pointer from __RefCounted, load target struct
                if (argVal->getType()->isStructTy() && paramTy->isStructTy() &&
                    argVal->getType() != paramTy) {
                    if (auto* argSt = llvm::dyn_cast<llvm::StructType>(argVal->getType())) {
                        if (argSt->hasName() && argSt->getName() == "__RefCounted") {
                            auto* dataPtr = builder_->CreateExtractValue(argVal, {0}, "ref.arg.ptr");
                            argVal = builder_->CreateLoad(paramTy, dataPtr, "ref.arg.deref");
                        }
                    }
                }
                // Auto-deref Box<T> → T: pointer arg to struct param
                if (argVal->getType()->isPointerTy() && paramTy->isStructTy()) {
                    argVal = builder_->CreateLoad(paramTy, argVal, "box.deref");
                } else if (argVal->getType()->isStructTy() && paramTy->isIntegerTy(64)) {
                    uint64_t sz = module_->getDataLayout().getTypeAllocSize(argVal->getType());
                    if (sz == 8) {
                        auto* fn2 = builder_->GetInsertBlock()->getParent();
                        auto* tmpAlloca = createEntryBlockAlloca(fn2, argVal->getType(), "coerce.struct");
                        builder_->CreateStore(argVal, tmpAlloca);
                        argVal = builder_->CreateLoad(llvm::Type::getInt64Ty(*context_), tmpAlloca, "coerce.i64");
                    }
                } else if (argVal->getType()->isIntegerTy(64) && paramTy->isStructTy()) {
                    uint64_t sz = module_->getDataLayout().getTypeAllocSize(paramTy);
                    if (sz == 8) {
                        auto* fn2 = builder_->GetInsertBlock()->getParent();
                        auto* tmpAlloca = createEntryBlockAlloca(fn2, paramTy, "coerce.tostruct");
                        builder_->CreateStore(argVal, tmpAlloca);
                        argVal = builder_->CreateLoad(paramTy, tmpAlloca, "coerce.struct");
                    } else {
                        argVal = castToType(argVal, paramTy);
                    }
                } else if (argVal->getType()->isStructTy() && paramTy->isStructTy() &&
                           argVal->getType() != paramTy) {
                    // Tuple / struct arg with per-element type mismatch
                    // (e.g. `(i32, i32)` constructed from int literals passed
                    // to a `(i64, i64)` parameter). Rebuild the struct by
                    // extracting each element, casting to the corresponding
                    // target slot, and reconstructing the target-typed
                    // aggregate. Without this, LLVM's struct-to-struct
                    // bitcast packs two i32 into the low i64 slot and
                    // garbages the data.
                    auto* srcSt = llvm::cast<llvm::StructType>(argVal->getType());
                    auto* dstSt = llvm::cast<llvm::StructType>(paramTy);
                    if (srcSt->getNumElements() == dstSt->getNumElements()) {
                        auto* fn2 = builder_->GetInsertBlock()->getParent();
                        auto* tmp = createEntryBlockAlloca(fn2, dstSt, "tuple.coerce");
                        for (unsigned ei = 0; ei < srcSt->getNumElements(); ++ei) {
                            auto* srcElem = builder_->CreateExtractValue(argVal, {ei}, "tup.src.el");
                            auto* casted = castToType(srcElem, dstSt->getElementType(ei));
                            builder_->CreateStore(casted,
                                builder_->CreateStructGEP(dstSt, tmp, ei, "tup.dst.el"));
                        }
                        argVal = builder_->CreateLoad(dstSt, tmp, "tuple.coerced");
                    } else {
                        argVal = castToType(argVal, paramTy);
                    }
                } else if (!(argVal->getType()->isStructTy() && paramTy->isIntegerTy())) {
                    argVal = castToType(argVal, paramTy);
                }
                if (argVal->getType() != paramTy)
                    argVal = castToType(argVal, paramTy);
            }
            argsV.push_back(argVal);
        }
    }

    if (argsV.size() < calleeFunc->getFunctionType()->getNumParams() && unit_) {
        for (auto& d : unit_->declarations) {
            if (!d || d->kind != DeclKind::Function || d->name != calleeName) continue;
            auto* fd = d->as<FunctionDecl>();
            for (size_t i = argsV.size(); i < calleeFunc->getFunctionType()->getNumParams() && i < fd->params.size(); ++i) {
                if (fd->params[i].defaultValue) {
                    auto* defVal = emitExpr(*fd->params[i].defaultValue);
                    if (defVal) {
                        defVal = castToType(defVal, calleeFunc->getFunctionType()->getParamType(i));
                        argsV.push_back(defVal);
                    }
                }
            }
            break;
        }
    }

    if (calleeFunc->getReturnType()->isVoidTy()) {
        auto* voidCall = builder_->CreateCall(calleeFunc, argsV);
        for (auto* arg : argsV) {
            if (arg && arg->getType()->isPointerTy()) {
                auto* stripped = arg->stripPointerCasts();
                if (llvm::isa<llvm::AllocaInst>(stripped) || llvm::isa<llvm::AllocaInst>(arg)) {
                    voidCall->setTailCallKind(llvm::CallInst::TCK_NoTail);
                    break;
                }
            }
        }
        return nullptr;
    }

    auto* callResult = builder_->CreateCall(calleeFunc, argsV, "call");

    if (callResult->getType()->isStructTy()) {
        callResult->setTailCallKind(llvm::CallInst::TCK_NoTail);
    } else {
        for (auto* arg : argsV) {
            if (arg && arg->getType()->isPointerTy()) {
                auto* stripped = arg->stripPointerCasts();
                if (llvm::isa<llvm::AllocaInst>(stripped) || llvm::isa<llvm::AllocaInst>(arg)) {
                    callResult->setTailCallKind(llvm::CallInst::TCK_NoTail);
                    break;
                }
            }
        }
    }

    return callResult;
}

} // namespace vyx
