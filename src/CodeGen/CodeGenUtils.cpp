#include "CodeGenIncludes.h"

#pragma warning(push, 0)
#include <llvm/Support/ErrorHandling.h>
#pragma warning(pop)

namespace vyx {

// R5 stage 2: Option / Result slot membership is driven solely by the
// lang-item registry. `__Result` / `__Result_N` are CodeGen-private
// layout aliases shared across every Result instantiation; treat them
// as result slots unconditionally regardless of registry state.
bool CodeGen::isOptionSlotName(llvm::StringRef name) const {
    if (!langItems_) return false;
    const Decl* lang = langItems_->find("option");
    if (!lang) return false;
    llvm::StringRef ln(lang->name);
    if (name == ln) return true;
    std::string pref = lang->name + "<";
    return name.starts_with(pref);
}

bool CodeGen::isResultSlotName(llvm::StringRef name) const {
    if (name.starts_with("__Result")) return true;
    if (!langItems_) return false;
    const Decl* lang = langItems_->find("result");
    if (!lang) return false;
    llvm::StringRef ln(lang->name);
    if (name == ln) return true;
    std::string pref = lang->name + "<";
    return name.starts_with(pref);
}

llvm::StructType* CodeGen::optionResultStructFromVyxType(const VyxTypePtr& type) {
    if (!type) return nullptr;
    if (!isOptionLike(*type) && !isResultLike(*type)) return nullptr;
    auto* lowered = toLLVMType(*type);
    auto* stTy = llvm::dyn_cast_or_null<llvm::StructType>(lowered);
    if (!stTy || stTy->isOpaque() || stTy->getNumElements() < 2 || !stTy->hasName())
        return nullptr;
    if (!isOptionOrResultSlotName(stTy->getName())) return nullptr;
    return stTy;
}

llvm::StructType* CodeGen::optionResultStructFromExpr(const Expr& expr) {
    return optionResultStructFromVyxType(expr.inferredType);
}

llvm::StructType* CodeGen::resolveOptionResultStructForExpr(
    const Expr& expr, bool allowReturnType) {
    if (auto* inferredTy = optionResultStructFromExpr(expr))
        return inferredTy;
    if (targetTypeHint_ && !targetTypeHint_->isOpaque() &&
        targetTypeHint_->getNumElements() >= 2 && targetTypeHint_->hasName() &&
        isOptionOrResultSlotName(targetTypeHint_->getName())) {
        return targetTypeHint_;
    }
    if (allowReturnType) {
        auto* fn = builder_->GetInsertBlock()->getParent();
        if (auto* rt = llvm::dyn_cast<llvm::StructType>(fn->getReturnType());
            rt && !rt->isOpaque() && rt->getNumElements() >= 2 && rt->hasName() &&
            isOptionOrResultSlotName(rt->getName())) {
            return rt;
        }
    }
    return nullptr;
}

llvm::StructType* CodeGen::variantPayloadOptionResultHint(
    const Expr& expr, llvm::StructType* containerTy, const std::string& variant) {
    if (containerTy && containerTy->hasName()) {
        auto byContainer = resultVariantInnerTypes_.find(containerTy->getName().str());
        if (byContainer != resultVariantInnerTypes_.end()) {
            auto byVariant = byContainer->second.find(variant);
            if (byVariant != byContainer->second.end()) {
                auto* stTy = llvm::dyn_cast_or_null<llvm::StructType>(byVariant->second);
                if (stTy && stTy->hasName() && isOptionOrResultSlotName(stTy->getName()))
                    return stTy;
            }
        }
    }

    VyxTypePtr payloadType;
    if (expr.inferredType) {
        if (variant == "Some") {
            payloadType = optionInner(*expr.inferredType);
        } else if (variant == "Ok") {
            payloadType = resultOk(*expr.inferredType);
        } else if (variant == "Err") {
            payloadType = resultErr(*expr.inferredType);
        }
    }
    if (auto* inferredPayload = optionResultStructFromVyxType(payloadType))
        return inferredPayload;
    return nullptr;
}

llvm::AllocaInst* CodeGen::createEntryBlockAlloca(llvm::Function* fn, llvm::Type* type, const std::string& name) {
    llvm::IRBuilder<> tmpBuilder(&fn->getEntryBlock(), fn->getEntryBlock().begin());
    auto* alloca = tmpBuilder.CreateAlloca(type, nullptr, name);
    alloca->setAlignment(module_->getDataLayout().getPrefTypeAlign(type));
    return alloca;
}

llvm::Type* CodeGen::getValuePtrType(llvm::Value* v) {
    if (auto* ai = llvm::dyn_cast<llvm::AllocaInst>(v)) return ai->getAllocatedType();
    if (auto* gv = llvm::dyn_cast<llvm::GlobalVariable>(v)) return gv->getValueType();
    if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(v)) return gep->getResultElementType();
    return nullptr;
}

llvm::Value* CodeGen::getOrCreateString(const std::string& str) {
    auto it = stringConstants_.find(str);
    if (it != stringConstants_.end()) {
        return it->second;
    }

    auto* strConst = builder_->CreateGlobalString(str, ".str");
    if (auto* gv = llvm::dyn_cast<llvm::GlobalVariable>(strConst)) {
        stringConstants_[str] = gv;
    }
    return strConst;
}

bool CodeGen::isSimpleExpr(const Expr& expr) {
    switch (expr.kind) {
        case ExprKind::IntLiteral:
        case ExprKind::FloatLiteral:
        case ExprKind::BoolLiteral:
        case ExprKind::CharLiteral:
        case ExprKind::Identifier:
            return true;
        case ExprKind::BinaryOp: {
            auto& bin = *expr.as<const BinaryOpExpr>();
            return bin.lhs && bin.rhs && isSimpleExpr(*bin.lhs) && isSimpleExpr(*bin.rhs);
        }
        case ExprKind::UnaryOp:
            return expr.as<const UnaryOpExpr>()->operand && isSimpleExpr(*expr.as<const UnaryOpExpr>()->operand);
        default:
            return false;
    }
}

std::string CodeGen::llvmTypeToString(llvm::Type* ty) {
    if (!ty) llvm_unreachable("CodeGen::llvmTypeToString(null)");
    if (ty == getOrCreateStringType()) return "string";
    if (ty->isIntegerTy(8)) return "i8";
    if (ty->isIntegerTy(16)) return "i16";
    if (ty->isIntegerTy(32)) return "i32";
    if (ty->isIntegerTy(64)) return "i64";
    if (ty->isFloatTy()) return "f32";
    if (ty->isDoubleTy()) return "f64";
    if (ty->isPointerTy()) return "rawptr";
    if (ty->isVoidTy()) return "void";
    if (auto* st = llvm::dyn_cast<llvm::StructType>(ty)) {
        if (st->hasName()) return st->getName().str();
    }
    return "i64";
}

bool CodeGen::isInternalStructType(const std::string& name) {
    if (name.empty()) return false;
    if (name == "__String") return false;
    if (name.starts_with("__")) return true;
    if (name.starts_with("DictEntry")) return true;
    return false;
}

// Walk the translation unit for a ClassDecl whose bare name matches the
// base portion of `structName` (everything before the first `<`) and
// check whether its `interfaces` list mentions `Deref`.  The interface
// string may appear either as the bare identifier `"Deref"` or in its
// mangled generic form `"Deref<T>"` — accept both.  Returning true opts
// the class into the heap-spill coercion path in `castToType` and into
// the smart-pointer → rawptr routing in `toLLVMType`.
//
// Search order:
//   1. Scan `unit_->declarations` — catches user classes and stdlib
//      classes pulled in via explicit `use std.ref;`.
//   2. Fall back to lang-item registry slots `"box"` / `"ref"` — catches
//      the canonical stdlib smart pointers even when the current TU did
//      not `use std.ref` transitively (e.g. a file that only
//      `use std.collections;` and relies on `Box<T>` / `Ref<T>` via
//      lang-item resolution in Sema).
bool CodeGen::classDeclaresDeref(llvm::StringRef structName) const {
    if (structName.empty()) return false;
    auto lt = structName.find('<');
    llvm::StringRef base = (lt == llvm::StringRef::npos)
        ? structName : structName.substr(0, lt);
    auto baseOf = [](llvm::StringRef s) {
        auto p = s.find('<');
        return p == llvm::StringRef::npos ? s : s.substr(0, p);
    };
    auto declHasDeref = [&](const ClassDecl* cd) {
        if (!cd) return false;
        // ParserDecl stores the first name after `:` in `parentName` and any
        // `, name` / `+ name` continuation in `interfaces`.  Either slot may
        // carry the `Deref` trait; accept a `Deref` or `Deref<...>` match in
        // both.
        if (!cd->parentName.empty() && baseOf(cd->parentName) == "Deref")
            return true;
        for (auto& iface : cd->interfaces) {
            if (baseOf(iface) == "Deref") return true;
        }
        return false;
    };
    bool foundClass = false;
    if (unit_) {
        for (auto& d : unit_->declarations) {
            if (!d || d->kind != DeclKind::Class) continue;
            if (d->name != base) continue;
            foundClass = true;
            auto* cd = d->as<const ClassDecl>();
            if (declHasDeref(cd)) return true;
            // Found the owning class decl — no need to keep scanning even
            // if it doesn't declare Deref (names are unique per unit).
            break;
        }
    }
    // Lang-item fallback: the canonical stdlib smart-pointer classes
    // (`Box<T>`, `Ref<T>`) are registered under the `"box"` / `"ref"`
    // slots at Sema time.  Consult the registry when unit_->declarations
    // lookup misses — this handles TUs that pull in Box/Ref transitively
    // via other stdlib imports (e.g. a file that only `use std.collections;`
    // but whose Sema scan populated the `box` slot through another path).
    if (!foundClass && langItems_) {
        for (const char* slot : {"box", "ref"}) {
            const Decl* l = langItems_->find(slot);
            if (!l || l->name != base) continue;
            foundClass = true;
            if (auto* cd = l->as<const ClassDecl>(); declHasDeref(cd))
                return true;
            break;
        }
    }
    // Canonical-name fallback: when neither unit_->declarations nor the
    // lang-item registry knows about this class (common when a user file
    // uses `Box<T>` via Sema's implicit prelude resolution but the TU
    // was built without a direct `use std.ref;`), preserve the historical
    // `starts_with("Box<") | "Ref<" | "Scope<"` behaviour so CodeGen still
    // routes these to rawptr / heap-spill.  The hardcoded fallback only
    // fires when the trait-driven lookup above couldn't locate the class;
    // user-defined `class MyBox<T> : Deref<T>` continues to win through
    // the trait check.
    if (!foundClass) {
        if (base == "Box" || base == "Ref" || base == "Scope") return true;
    }
    return false;
}

llvm::Value* CodeGen::castToType(llvm::Value* val, llvm::Type* targetType) {
    if (!val || val->getType() == targetType) return val;

    auto* srcType = val->getType();

    if (srcType->isStructTy() && targetType->isPointerTy()) {
        // R5 phase 6 (2026-04-23): any smart-pointer class whose LLVM
        // layout collapses to an opaque pointer must heap-allocate its
        // struct body when coerced to that pointer.  Methods like
        // `Box<T>.deref` / `Box<T>.get` treat `self` as a pointer to
        // the `{ data: rawptr }` body and read the data field via
        // `load ptr, ptr %self`.  When `Box { data: d }` is returned as
        // `Box<T>` (ptr), spilling the struct onto a stack alloca and
        // returning its address hands the caller a dangling pointer —
        // the alloca is freed the moment the callee returns and the
        // caller's `.deref()` reads garbage stack memory (Bug D:
        // nested_box_generic runtime).  Heap-allocate the struct instead
        // so the returned pointer stays valid across the call.
        //
        // Eligibility: the owning `ClassDecl` declares the `Deref<T>`
        // trait (std/ref.vyx).  This opts in `Box<T>` (stdlib) and any
        // user-defined `class MyBox<T> : Deref<T>` on equal footing — no
        // hard-coded `"Box<"` prefix test.
        if (auto* srcSt = llvm::dyn_cast<llvm::StructType>(srcType)) {
            if (srcSt->hasName() && classDeclaresDeref(srcSt->getName())) {
                auto* i64Ty = llvm::Type::getInt64Ty(*context_);
                uint64_t sz = module_->getDataLayout().getTypeAllocSize(srcSt);
                auto* mallocFn = module_->getFunction("malloc");
                if (!mallocFn) {
                    auto* fty = llvm::FunctionType::get(
                        llvm::PointerType::getUnqual(*context_), {i64Ty}, false);
                    mallocFn = llvm::Function::Create(fty,
                        llvm::Function::ExternalLinkage, "malloc", *module_);
                }
                auto* mem = builder_->CreateCall(mallocFn,
                    {llvm::ConstantInt::get(i64Ty, sz)}, "box.struct");
                builder_->CreateStore(val, mem);
                return mem;
            }
        }
        auto* fn = builder_->GetInsertBlock()->getParent();
        auto* tmp = createEntryBlockAlloca(fn, srcType, "cast.s2p");
        builder_->CreateStore(val, tmp);
        return tmp;
    }

    if (srcType->isPointerTy() && targetType->isStructTy()) {
        return builder_->CreateLoad(targetType, val, "cast.p2s");
    }

    if (srcType->isStructTy() && targetType->isStructTy()) {
        auto* srcSt = llvm::cast<llvm::StructType>(srcType);
        auto* targetSt = llvm::cast<llvm::StructType>(targetType);
        const Decl* stringDecl = langItems_ ? langItems_->find("string") : nullptr;
        const bool isRegisteredString =
            stringDecl && stringDecl->kind == DeclKind::Class &&
            srcSt->hasName() && srcSt->getName() == stringDecl->name;

        // `std.string.String` uses SSO: its `ptr` is null while bytes live
        // in trailing inline fields. Primitive `string` is only the 32-byte
        // `{ptr, len, cap, owned}` ABI form, so generic prefix truncation
        // would return a null pointer. Materialize an independent ABI value
        // for exactly the registered string lang-item; arbitrary classes do
        // not enter this path.
        if (isRegisteredString && targetSt == getOrCreateStringType() &&
            srcSt->getNumElements() >= 5 &&
            srcSt->getElementType(0)->isPointerTy() &&
            srcSt->getElementType(1)->isIntegerTy(64)) {
            auto* fn = builder_->GetInsertBlock()->getParent();
            auto* ptrTy = llvm::PointerType::getUnqual(*context_);
            auto* i8Ty = llvm::Type::getInt8Ty(*context_);
            auto* i64Ty = llvm::Type::getInt64Ty(*context_);

            auto* srcAlloca = createEntryBlockAlloca(fn, srcSt, "cast.string.source");
            builder_->CreateStore(val, srcAlloca);
            auto* sourcePtr = builder_->CreateLoad(
                ptrTy, builder_->CreateStructGEP(srcSt, srcAlloca, 0),
                "cast.string.ptr");
            auto* sourceLen = builder_->CreateLoad(
                i64Ty, builder_->CreateStructGEP(srcSt, srcAlloca, 1),
                "cast.string.len");
            auto* inlinePtr = builder_->CreateStructGEP(
                srcSt, srcAlloca, 4, "cast.string.inline");
            auto* hasHeapBuffer = builder_->CreateICmpNE(
                sourcePtr,
                llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy)),
                "cast.string.heap");
            auto* sourceData = builder_->CreateSelect(
                hasHeapBuffer, sourcePtr, inlinePtr, "cast.string.data");
            auto* byteCount = builder_->CreateAdd(
                sourceLen, llvm::ConstantInt::get(i64Ty, 1), "cast.string.bytes");

            auto* mallocFn = module_->getFunction("malloc");
            if (!mallocFn) {
                auto* mallocTy = llvm::FunctionType::get(ptrTy, {i64Ty}, false);
                mallocFn = llvm::Function::Create(
                    mallocTy, llvm::Function::ExternalLinkage, "malloc", *module_);
            }
            auto* buffer = builder_->CreateCall(mallocFn, {byteCount}, "cast.string.copy");

            auto* memcpyFn = module_->getFunction("memcpy");
            if (!memcpyFn) {
                auto* memcpyTy = llvm::FunctionType::get(
                    ptrTy, {ptrTy, ptrTy, i64Ty}, false);
                memcpyFn = llvm::Function::Create(
                    memcpyTy, llvm::Function::ExternalLinkage, "memcpy", *module_);
            }
            builder_->CreateCall(memcpyFn, {buffer, sourceData, sourceLen});
            auto* nul = builder_->CreateGEP(i8Ty, buffer, sourceLen, "cast.string.nul");
            builder_->CreateStore(llvm::ConstantInt::get(i8Ty, 0), nul);

            auto* dstAlloca = createEntryBlockAlloca(fn, targetSt, "cast.string.abi");
            builder_->CreateStore(buffer, builder_->CreateStructGEP(targetSt, dstAlloca, 0));
            builder_->CreateStore(sourceLen, builder_->CreateStructGEP(targetSt, dstAlloca, 1));
            builder_->CreateStore(byteCount, builder_->CreateStructGEP(targetSt, dstAlloca, 2));
            builder_->CreateStore(llvm::ConstantInt::get(i64Ty, 1),
                builder_->CreateStructGEP(targetSt, dstAlloca, 3));
            return builder_->CreateLoad(targetSt, dstAlloca, "cast.string.abi.value");
        }
    }

    if (srcType->isStructTy() && targetType->isStructTy() && srcType != targetType) {
        if (auto* srcSt = llvm::dyn_cast<llvm::StructType>(srcType)) {
            if (srcSt->hasName() && srcSt->getName() == "__RefCounted") {
                auto* dataPtr = builder_->CreateExtractValue(val, {0}, "ref.deref.ptr");
                return builder_->CreateLoad(targetType, dataPtr, "ref.deref");
            }
        }
    }

    if (targetType->isStructTy()) {
        auto* targetSt = llvm::cast<llvm::StructType>(targetType);
        if (targetSt->hasName() && targetSt->getNumElements() >= 2) {
            auto nm = targetSt->getName();
            if (isOptionOrResultSlotName(nm)) {
                if (srcType->isIntegerTy() || (srcType->isStructTy() && srcType != targetType)) {
                    auto* fn = builder_->GetInsertBlock()->getParent();
                    auto* alloca = createEntryBlockAlloca(fn, targetSt, "cast.opt");
                    auto* tagTy = targetSt->getElementType(0);
                    llvm::Value* tagVal = nullptr;
                    if (srcType->isStructTy()) {
                        auto* srcSt = llvm::cast<llvm::StructType>(srcType);
                        auto* srcAlloca = createEntryBlockAlloca(fn, srcSt, "cast.src");
                        builder_->CreateStore(val, srcAlloca);
                        auto* srcTag = builder_->CreateLoad(
                            srcSt->getElementType(0),
                            builder_->CreateStructGEP(srcSt, srcAlloca, 0), "cast.stag");
                        tagVal = (srcTag->getType() == tagTy)
                            ? srcTag
                            : builder_->CreateIntCast(srcTag, tagTy, false, "cast.ttag");
                    } else {
                        tagVal = (val->getType() == tagTy)
                            ? val
                            : builder_->CreateIntCast(val, tagTy, false, "cast.ttag");
                    }
                    builder_->CreateStore(tagVal,
                        builder_->CreateStructGEP(targetSt, alloca, 0));
                    // Copy payload bytes across Result struct sizes. Without
                    // this, `?` error propagation from a Result<Vec<i64>,
                    // string> into a Result<i64, string> would zero out the
                    // destination payload and the outer Err's string
                    // payload would be lost. Memcpy up to min(src, dst)
                    // payload size; excess destination bytes are zeroed.
                    auto* dstPayloadField = builder_->CreateStructGEP(
                        targetSt, alloca, 1);
                    auto* dstPayloadTy = targetSt->getElementType(1);
                    uint64_t dstPayloadSz = module_->getDataLayout()
                        .getTypeAllocSize(dstPayloadTy);
                    if (srcType->isStructTy()) {
                        auto* srcSt = llvm::cast<llvm::StructType>(srcType);
                        if (srcSt->getNumElements() >= 2) {
                            auto* srcPayloadTy = srcSt->getElementType(1);
                            uint64_t srcPayloadSz = module_->getDataLayout()
                                .getTypeAllocSize(srcPayloadTy);
                            // Re-stash val into a fresh src alloca so we have
                            // a pointer to read payload bytes from. (The earlier
                            // srcAlloca above is defined only in the srcType-is-
                            // struct branch; reuse its pattern locally.)
                            auto* srcAllocaPl = createEntryBlockAlloca(fn, srcSt, "cast.src.pl");
                            builder_->CreateStore(val, srcAllocaPl);
                            auto* srcPayloadPtr = builder_->CreateStructGEP(
                                srcSt, srcAllocaPl, 1);
                            uint64_t copySz = std::min(srcPayloadSz, dstPayloadSz);
                            auto* memcpyFn = module_->getFunction("memcpy");
                            auto* ptrTy = llvm::PointerType::getUnqual(*context_);
                            auto* i64Ty = llvm::Type::getInt64Ty(*context_);
                            if (!memcpyFn) {
                                auto* fty = llvm::FunctionType::get(
                                    ptrTy, {ptrTy, ptrTy, i64Ty}, false);
                                memcpyFn = llvm::Function::Create(
                                    fty, llvm::Function::ExternalLinkage,
                                    "memcpy", *module_);
                            }
                            // Zero destination payload first so any trailing
                            // bytes beyond the copy are clean.
                            builder_->CreateStore(
                                llvm::Constant::getNullValue(dstPayloadTy),
                                dstPayloadField);
                            if (copySz > 0) {
                                builder_->CreateCall(memcpyFn, {
                                    dstPayloadField, srcPayloadPtr,
                                    llvm::ConstantInt::get(i64Ty, copySz)
                                });
                            }
                        } else {
                            builder_->CreateStore(
                                llvm::Constant::getNullValue(dstPayloadTy),
                                dstPayloadField);
                        }
                    } else {
                        builder_->CreateStore(
                            llvm::Constant::getNullValue(dstPayloadTy),
                            dstPayloadField);
                    }
                    return builder_->CreateLoad(targetSt, alloca, "cast.opt");
                }
            }
        }
    }

    if (srcType->isStructTy() && targetType->isStructTy() && srcType != targetType) {
        uint64_t srcSz = module_->getDataLayout().getTypeAllocSize(srcType);
        uint64_t dstSz = module_->getDataLayout().getTypeAllocSize(targetType);
        auto* fn = builder_->GetInsertBlock()->getParent();
        if (srcSz == dstSz) {
            auto* srcAlloca = createEntryBlockAlloca(fn, srcType, "cast.s2s.src");
            builder_->CreateStore(val, srcAlloca);
            return builder_->CreateLoad(targetType, srcAlloca, "cast.s2s.v");
        }
        if (srcSz >= dstSz) {
            auto* srcAlloca = createEntryBlockAlloca(fn, srcType, "cast.s2s.big");
            builder_->CreateStore(val, srcAlloca);
            return builder_->CreateLoad(targetType, srcAlloca, "cast.s2s.trunc");
        }
        auto* dstAlloca = createEntryBlockAlloca(fn, targetType, "cast.s2s.pad");
        builder_->CreateStore(llvm::Constant::getNullValue(targetType), dstAlloca);
        auto* srcAlloca = createEntryBlockAlloca(fn, srcType, "cast.s2s.small");
        builder_->CreateStore(val, srcAlloca);
        auto* i64Ty = llvm::Type::getInt64Ty(*context_);
        auto* ptrTy = llvm::PointerType::getUnqual(*context_);
        auto* memcpyFn = module_->getFunction("memcpy");
        if (!memcpyFn) {
            auto* fty = llvm::FunctionType::get(ptrTy, {ptrTy, ptrTy, i64Ty}, false);
            memcpyFn = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, "memcpy", *module_);
        }
        builder_->CreateCall(memcpyFn, {dstAlloca, srcAlloca,
            llvm::ConstantInt::get(i64Ty, srcSz)});
        return builder_->CreateLoad(targetType, dstAlloca, "cast.s2s.pad.v");
    }

    if (srcType->isIntegerTy() && targetType->isStructTy()) {
        auto* fn = builder_->GetInsertBlock()->getParent();
        auto* alloca = createEntryBlockAlloca(fn, targetType, "cast.i2s");
        builder_->CreateStore(llvm::Constant::getNullValue(targetType), alloca);
        return builder_->CreateLoad(targetType, alloca, "cast.i2s.v");
    }

    return numericCast(val, targetType);
}

llvm::Value* CodeGen::numericCast(llvm::Value* val, llvm::Type* targetType) {
    if (!val || val->getType() == targetType) return val;

    auto* srcType = val->getType();
    if (srcType->isIntegerTy() && targetType->isFloatingPointTy()) return builder_->CreateSIToFP(val, targetType, "itof");
    if (srcType->isFloatingPointTy() && targetType->isIntegerTy()) return builder_->CreateFPToSI(val, targetType, "ftoi");
    if (srcType->isFloatingPointTy() && targetType->isFloatingPointTy()) {
        if (srcType->getPrimitiveSizeInBits() < targetType->getPrimitiveSizeInBits())
            return builder_->CreateFPExt(val, targetType, "fext");
        return builder_->CreateFPTrunc(val, targetType, "ftrunc");
    }
    if (srcType->isIntegerTy() && targetType->isIntegerTy()) {
        if (srcType->getIntegerBitWidth() < targetType->getIntegerBitWidth()) {
            if (srcType->isIntegerTy(1)) return builder_->CreateZExt(val, targetType, "zext");
            return builder_->CreateSExt(val, targetType, "sext");
        }
        if (srcType->getIntegerBitWidth() > targetType->getIntegerBitWidth())
            return builder_->CreateTrunc(val, targetType, "trunc");
    }
    if (srcType->isPointerTy() && targetType->isPointerTy()) return val;
    if (srcType->isIntegerTy() && targetType->isPointerTy()) {
        auto* ext = val;
        if (srcType->getIntegerBitWidth() < 64)
            ext = builder_->CreateSExt(val, llvm::Type::getInt64Ty(*context_), "cast.ext");
        return builder_->CreateIntToPtr(ext, targetType, "cast.itoptr");
    }
    if (srcType->isPointerTy() && targetType->isIntegerTy()) return builder_->CreatePtrToInt(val, targetType, "cast.ptrtoi");
    if (srcType->isStructTy() && targetType->isIntegerTy()) {
        auto* stTy = llvm::cast<llvm::StructType>(srcType);
        auto* fn = builder_->GetInsertBlock()->getParent();
        auto* alloca = createEntryBlockAlloca(fn, stTy, "struct.cast");
        builder_->CreateStore(val, alloca);
        if (stTy->getNumElements() > 0) {
            auto* firstElem = stTy->getElementType(0);
            auto* field = builder_->CreateStructGEP(stTy, alloca, 0);
            auto* loaded = builder_->CreateLoad(firstElem, field, "struct.field0");
            if (firstElem->isPointerTy()) return builder_->CreatePtrToInt(loaded, targetType, "struct.ptrtoi");
            if (firstElem->isIntegerTy()) return numericCast(loaded, targetType);
        }
    }
    return val;
}

bool CodeGen::isUnsignedExpr(const Expr& expr) {
    if (expr.kind == ExprKind::Identifier) {
        return unsignedVars_.count(expr.as<const IdentifierExpr>()->name) > 0;
    }
    if (expr.kind == ExprKind::Cast) {
        auto* castE = expr.as<const CastExpr>();
        if (castE->targetType) {
            auto& n = castE->targetType->name;
            return n == "u8" || n == "u16" || n == "u32" || n == "u64";
        }
    }
    // Arithmetic / bitwise expressions inherit unsignedness from their
    // operands: if either side is unsigned, treat the result as unsigned.
    // Without this, `${u & 0xFF}` / `${u + 1}` for a u64 `u` would display
    // as signed even though every sensible reading is unsigned.
    if (expr.kind == ExprKind::BinaryOp) {
        auto* binE = expr.as<const BinaryOpExpr>();
        if ((binE->lhs && isUnsignedExpr(*binE->lhs)) ||
            (binE->rhs && isUnsignedExpr(*binE->rhs))) {
            return true;
        }
    }
    if (expr.kind == ExprKind::UnaryOp) {
        auto* unE = expr.as<const UnaryOpExpr>();
        if (unE->operand && isUnsignedExpr(*unE->operand)) return true;
    }
    return false;
}

llvm::Value* CodeGen::createSafeICmp(llvm::CmpInst::Predicate pred, llvm::Value* lhs, llvm::Value* rhs, const llvm::Twine& name) {
    if (lhs->getType() != rhs->getType()) {
        if (lhs->getType()->isIntegerTy() && rhs->getType()->isIntegerTy()) {
            unsigned lBits = lhs->getType()->getIntegerBitWidth();
            unsigned rBits = rhs->getType()->getIntegerBitWidth();
            if (lBits < rBits)
                lhs = builder_->CreateSExt(lhs, rhs->getType(), "icmp.widen.l");
            else
                rhs = builder_->CreateSExt(rhs, lhs->getType(), "icmp.widen.r");
        } else {
            rhs = castToType(rhs, lhs->getType());
        }
    }
    // LLVM's ICmp asserts operands are int/ptr (or their vector forms). When
    // a generic method body like `Iterator<T>.min()` is monomorphised for a
    // struct T (e.g. Vec<i32>), the user-level `val < best` reduces to an
    // ICmp on struct values, which would crash LLVM. The body is never
    // actually called for such T's at runtime, so emit a safe `false`
    // placeholder that keeps the IR well-formed; a proper Ord-bound Sema
    // check is the right long-term fix.
    auto* lhsTy = lhs->getType();
    if (!lhsTy->isIntOrIntVectorTy() && !lhsTy->isPtrOrPtrVectorTy()) {
        return llvm::ConstantInt::getFalse(*context_);
    }
    return builder_->CreateICmp(pred, lhs, rhs, name);
}

llvm::Value* CodeGen::coerceToBool(llvm::Value* val, const llvm::Twine& name) {
    auto* i1Ty = llvm::Type::getInt1Ty(*context_);
    if (!val) return llvm::ConstantInt::getFalse(i1Ty);
    auto* ty = val->getType();
    if (ty->isIntegerTy(1)) return val;
    if (ty->isIntegerTy()) return createSafeICmp(llvm::CmpInst::ICMP_NE, val, llvm::ConstantInt::get(ty, 0), name);
    if (ty->isPointerTy()) return createSafeICmp(llvm::CmpInst::ICMP_NE, val, llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ty)), name);
    if (ty->isFloatingPointTy()) return builder_->CreateFCmpONE(val, llvm::ConstantFP::get(ty, 0.0), name);
    return llvm::ConstantInt::getTrue(i1Ty);
}

llvm::Function* CodeGen::getOrCreatePrintf() {
    auto* fn = module_->getFunction("printf");
    if (fn) return fn;

    auto* fnType = llvm::FunctionType::get(
        llvm::Type::getInt32Ty(*context_),
        {llvm::PointerType::getUnqual(*context_)},
        true);
    return llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, "printf", *module_);
}

llvm::Function* CodeGen::getOrCreateI64ToCharsFunction() {
    if (auto* existing = module_->getFunction("__vyx_i64_to_chars"))
        return existing;

    auto* i8Ty = llvm::Type::getInt8Ty(*context_);
    auto* i64Ty = llvm::Type::getInt64Ty(*context_);
    auto* ptrTy = llvm::PointerType::getUnqual(*context_);
    auto* fnTy = llvm::FunctionType::get(i64Ty, {ptrTy, i64Ty}, false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                      "__vyx_i64_to_chars", *module_);
    fn->addFnAttr(llvm::Attribute::AlwaysInline);

    auto ip = builder_->saveIP();
    auto argIt = fn->arg_begin();
    auto* out = &*argIt++;
    out->setName("out");
    auto* value = &*argIt;
    value->setName("v");

    auto* entryBB = llvm::BasicBlock::Create(*context_, "entry", fn);
    auto* loopBB = llvm::BasicBlock::Create(*context_, "digits.loop", fn);
    auto* afterBB = llvm::BasicBlock::Create(*context_, "digits.done", fn);
    auto* negBB = llvm::BasicBlock::Create(*context_, "digits.sign", fn);
    auto* copyBB = llvm::BasicBlock::Create(*context_, "digits.copy", fn);

    auto* zero64 = llvm::ConstantInt::get(i64Ty, 0);
    auto* one64 = llvm::ConstantInt::get(i64Ty, 1);
    auto* ten64 = llvm::ConstantInt::get(i64Ty, 10);
    auto* thirtyTwo = llvm::ConstantInt::get(i64Ty, 32);
    auto* tmpTy = llvm::ArrayType::get(i8Ty, 32);

    builder_->SetInsertPoint(entryBB);
    auto* tmp = builder_->CreateAlloca(tmpTy, nullptr, "digits");
    auto* isNeg = builder_->CreateICmpSLT(value, zero64, "is.neg");
    builder_->CreateBr(loopBB);

    builder_->SetInsertPoint(loopBB);
    auto* nPhi = builder_->CreatePHI(i64Ty, 2, "n");
    auto* posPhi = builder_->CreatePHI(i64Ty, 2, "pos");
    nPhi->addIncoming(value, entryBB);
    posPhi->addIncoming(thirtyTwo, entryBB);

    auto* q = builder_->CreateSDiv(nPhi, ten64, "q");
    auto* r = builder_->CreateSRem(nPhi, ten64, "r");
    auto* rNeg = builder_->CreateICmpSLT(r, zero64, "r.neg");
    auto* negR = builder_->CreateSub(zero64, r, "r.abs.neg");
    auto* rAbs = builder_->CreateSelect(rNeg, negR, r, "r.abs");
    auto* digit64 = builder_->CreateAdd(rAbs, llvm::ConstantInt::get(i64Ty, '0'), "digit64");
    auto* digit8 = builder_->CreateTrunc(digit64, i8Ty, "digit8");
    auto* posNext = builder_->CreateSub(posPhi, one64, "pos.next");
    auto* digitPtr = builder_->CreateInBoundsGEP(tmpTy, tmp, {zero64, posNext}, "digit.ptr");
    builder_->CreateStore(digit8, digitPtr);
    auto* more = builder_->CreateICmpNE(q, zero64, "more");
    builder_->CreateCondBr(more, loopBB, afterBB);
    nPhi->addIncoming(q, loopBB);
    posPhi->addIncoming(posNext, loopBB);

    builder_->SetInsertPoint(afterBB);
    builder_->CreateCondBr(isNeg, negBB, copyBB);

    builder_->SetInsertPoint(negBB);
    auto* signPos = builder_->CreateSub(posNext, one64, "sign.pos");
    auto* signPtr = builder_->CreateInBoundsGEP(tmpTy, tmp, {zero64, signPos}, "sign.ptr");
    builder_->CreateStore(llvm::ConstantInt::get(i8Ty, '-'), signPtr);
    builder_->CreateBr(copyBB);

    builder_->SetInsertPoint(copyBB);
    auto* startPhi = builder_->CreatePHI(i64Ty, 2, "start");
    startPhi->addIncoming(posNext, afterBB);
    startPhi->addIncoming(signPos, negBB);
    auto* len = builder_->CreateSub(thirtyTwo, startPhi, "len");
    auto* startPtr = builder_->CreateInBoundsGEP(tmpTy, tmp, {zero64, startPhi}, "start.ptr");
    auto* memcpyFn = module_->getFunction("memcpy");
    if (!memcpyFn) {
        auto* mty = llvm::FunctionType::get(ptrTy, {ptrTy, ptrTy, i64Ty}, false);
        memcpyFn = llvm::Function::Create(mty, llvm::Function::ExternalLinkage, "memcpy", *module_);
    }
    builder_->CreateCall(memcpyFn, {out, startPtr, len});
    auto* nulPtr = builder_->CreateGEP(i8Ty, out, len, "nul.ptr");
    builder_->CreateStore(llvm::ConstantInt::get(i8Ty, 0), nulPtr);
    builder_->CreateRet(len);

    builder_->restoreIP(ip);
    return fn;
}

llvm::Value* CodeGen::emitPrintCall(const std::vector<llvm::Value*>& args) {
    auto* printf = getOrCreatePrintf();
    auto* i32Ty = llvm::Type::getInt32Ty(*context_);
    auto* i64Ty = llvm::Type::getInt64Ty(*context_);
    auto* ptrTy = llvm::PointerType::getUnqual(*context_);
    auto* strTy = getOrCreateStringType();

    auto doFlush = [&]() {
        auto* ff = module_->getFunction("fflush");
        if (!ff) {
            auto* ft = llvm::FunctionType::get(i32Ty, {ptrTy}, false);
            ff = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "fflush", *module_);
        }
        builder_->CreateCall(ff, {llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy))});
    };

    if (args.empty()) {
        auto* nl = getOrCreateString("\n");
        auto* r = builder_->CreateCall(printf, {nl}, "print");
        doFlush();
        return r;
    }

    if (args.size() == 1 && args[0]->getType()->isPointerTy()) {
        auto* fmt = getOrCreateString("%s\n");
        return builder_->CreateCall(printf, {fmt, args[0]}, "print");
    }

    if (args.size() == 1 && args[0]->getType() == strTy) {
        auto* fn = builder_->GetInsertBlock()->getParent();
        auto* fmt = getOrCreateString("%.*s\n");
        auto* alloca = createEntryBlockAlloca(fn, strTy, "pr.str");
        builder_->CreateStore(args[0], alloca);
        auto* len = builder_->CreateLoad(i64Ty, builder_->CreateStructGEP(strTy, alloca, 1));
        auto* len32 = builder_->CreateTrunc(len, i32Ty);
        auto* rawPtr = builder_->CreateLoad(ptrTy, builder_->CreateStructGEP(strTy, alloca, 0));
        auto* r2 = builder_->CreateCall(printf, {fmt, len32, rawPtr}, "print");
        doFlush();
        return r2;
    }

    if (args.size() == 1) {
        auto* val = args[0];
        if (val->getType()->isIntegerTy()) {
            if (val->getType()->getIntegerBitWidth() == 1) {
                auto* fn = builder_->GetInsertBlock()->getParent();
                auto* trueBB = llvm::BasicBlock::Create(*context_, "print.true", fn);
                auto* falseBB = llvm::BasicBlock::Create(*context_, "print.false", fn);
                auto* mergeBB = llvm::BasicBlock::Create(*context_, "print.merge", fn);

                builder_->CreateCondBr(val, trueBB, falseBB);
                builder_->SetInsertPoint(trueBB);
                builder_->CreateCall(printf, {getOrCreateString("true\n")});
                builder_->CreateBr(mergeBB);
                builder_->SetInsertPoint(falseBB);
                builder_->CreateCall(printf, {getOrCreateString("false\n")});
                builder_->CreateBr(mergeBB);
                builder_->SetInsertPoint(mergeBB);
                return llvm::ConstantInt::get(i32Ty, 0);
            }
            if (val->getType()->getIntegerBitWidth() > 32) {
                auto* fmt64 = getOrCreateString("%lld\n");
                return builder_->CreateCall(printf, {fmt64, val}, "print");
            }
            auto* fmt = getOrCreateString("%d\n");
            if (val->getType()->getIntegerBitWidth() < 32) {
                val = builder_->CreateSExt(val, i32Ty);
            }
            return builder_->CreateCall(printf, {fmt, val}, "print");
        }
        if (val->getType()->isFloatingPointTy()) {
            auto* fmt = getOrCreateString("%f\n");
            if (val->getType()->isFloatTy()) {
                val = builder_->CreateFPExt(val, llvm::Type::getDoubleTy(*context_));
            }
            return builder_->CreateCall(printf, {fmt, val}, "print");
        }
    }

    auto* fn = builder_->GetInsertBlock()->getParent();
    std::vector<llvm::Value*> printfArgs;
    std::string fmtStr;
    for (auto* arg : args) {
        if (arg->getType() == strTy) {
            fmtStr += "%.*s";
            auto* alloca = createEntryBlockAlloca(fn, strTy, "pr.str");
            builder_->CreateStore(arg, alloca);
            auto* len = builder_->CreateLoad(i64Ty, builder_->CreateStructGEP(strTy, alloca, 1));
            printfArgs.push_back(builder_->CreateTrunc(len, i32Ty));
            printfArgs.push_back(builder_->CreateLoad(ptrTy, builder_->CreateStructGEP(strTy, alloca, 0)));
        } else if (arg->getType()->isPointerTy()) {
            fmtStr += "%s";
            printfArgs.push_back(arg);
        } else if (arg->getType()->isIntegerTy()) {
            fmtStr += (arg->getType()->getIntegerBitWidth() > 32) ? "%lld" : "%d";
            printfArgs.push_back(arg);
        } else if (arg->getType()->isFloatingPointTy()) {
            fmtStr += "%f";
            printfArgs.push_back(arg);
        }
    }
    fmtStr += "\n";
    printfArgs.insert(printfArgs.begin(), getOrCreateString(fmtStr));
    return builder_->CreateCall(printf, printfArgs, "print");
}

llvm::Value* CodeGen::getVariableAddress(const Expr& expr) {
    if (expr.kind == ExprKind::Identifier) {
        auto& name = expr.as<const IdentifierExpr>()->name;
        auto it = namedValues_.find(name);
        if (it != namedValues_.end()) return it->second;
        // Fall through to module-level globals so `counter += 1` on a
        // `var counter: i32 = 0;` at file scope resolves. Previously
        // only the function-local `namedValues_` was consulted and the
        // compound-assign path errored with "could not resolve address
        // of target".
        if (auto* gv = module_->getGlobalVariable(name, true))
            return gv;
    }
    // Field / nested member access: `self.pos += 1`, `obj.field += ...`,
    // `obj.nested.deep += ...`. getMemberAddress walks the whole chain
    // and returns a GEP into the underlying alloca, which compound
    // assign can load/store against. Without this fallback the compound
    // assign codegen errored with "could not resolve address of target".
    if (expr.kind == ExprKind::MemberAccess) {
        if (auto* addr = getMemberAddress(expr)) return addr;
    }
    return nullptr;
}

llvm::StructType* CodeGen::getOrCreateStringType() {
    auto it = structTypes_.find("__String");
    if (it != structTypes_.end()) return it->second;
    auto* ty = llvm::StructType::create(*context_,
        {llvm::PointerType::getUnqual(*context_), llvm::Type::getInt64Ty(*context_),
         llvm::Type::getInt64Ty(*context_), llvm::Type::getInt64Ty(*context_)},
        "__String");
    // Register under both the LLVM-internal name (`__String`) and the
    // canonical Vyx name (`string`) so generic mangled keys produced by Mono
    // (`Vec<string>`, `Dict<string,V>`) line up with CodeGen lookups without
    // any per-site `string→__String` translation. The two keys point at the
    // same llvm::StructType — there is no separate string struct.
    structTypes_["__String"] = ty;
    structTypes_["str"]      = ty;
    structTypes_["string"]   = ty;
    structFieldNames_["__String"] = {"ptr", "len", "cap", "owned"};
    structFieldNames_["str"]      = {"ptr", "len", "cap", "owned"};
    structFieldNames_["string"]   = {"ptr", "len", "cap", "owned"};
    return ty;
}

llvm::Value* CodeGen::createStringValue(const std::string& str) {
    auto* fn = builder_->GetInsertBlock()->getParent();
    auto* strTy = getOrCreateStringType();
    auto* rawPtr = getOrCreateString(str);
    auto* alloca = createEntryBlockAlloca(fn, strTy, "str.tmp");
    auto* memsetFn2 = module_->getFunction("memset");
    if (memsetFn2) {
        uint64_t sz = module_->getDataLayout().getTypeAllocSize(strTy);
        builder_->CreateCall(memsetFn2, {
            alloca, llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0),
            llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), sz)
        });
    }
    builder_->CreateStore(rawPtr, builder_->CreateStructGEP(strTy, alloca, 0, "str.ptr"));
    builder_->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), str.size()),
        builder_->CreateStructGEP(strTy, alloca, 1, "str.len"));
    builder_->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), 0),
        builder_->CreateStructGEP(strTy, alloca, 2, "str.cap"));
    builder_->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), 0),
        builder_->CreateStructGEP(strTy, alloca, 3, "str.owned"));
    return builder_->CreateLoad(strTy, alloca, "str.val");
}

llvm::Value* CodeGen::extractStringPtr(llvm::Value* strVal) {
    if (strVal->getType()->isPointerTy()) return strVal;
    auto* strTy = getOrCreateStringType();
    if (strVal->getType() == strTy) {
        auto* fn = builder_->GetInsertBlock()->getParent();
        auto* alloca = createEntryBlockAlloca(fn, strTy, "str.extract");
        builder_->CreateStore(strVal, alloca);
        auto* ptrField = builder_->CreateStructGEP(strTy, alloca, 0, "str.ptr.extract");
        return builder_->CreateLoad(llvm::PointerType::getUnqual(*context_), ptrField, "str.cptr");
    }
    return strVal;
}

llvm::Value* CodeGen::emitHashValue(llvm::Value* key) {
    auto* i64Ty = llvm::Type::getInt64Ty(*context_);
    if (key->getType()->isIntegerTy()) {
        auto* factor = llvm::ConstantInt::get(i64Ty, 2654435761ULL);
        auto* keyExt = key->getType()->isIntegerTy(64) ? key : builder_->CreateSExt(key, i64Ty);
        return builder_->CreateMul(keyExt, factor, "hash");
    }
    if (key->getType()->isPointerTy()) {
        auto* keyInt = builder_->CreatePtrToInt(key, i64Ty, "hash.ptr");
        auto* factor = llvm::ConstantInt::get(i64Ty, 2654435761ULL);
        return builder_->CreateMul(keyInt, factor, "hash");
    }
    return key;
}

llvm::StructType* CodeGen::getOrCreateAnyType() {
    auto it = structTypes_.find("__Any");
    if (it != structTypes_.end()) return it->second;
    auto* ty = llvm::StructType::create(*context_,
        {llvm::Type::getInt8Ty(*context_), llvm::Type::getInt64Ty(*context_)},
        "__Any");
    structTypes_["__Any"] = ty;
    return ty;
}

llvm::StructType* CodeGen::getOrCreateResultType() {
    auto it = structTypes_.find("__Result");
    if (it != structTypes_.end()) return it->second;
    auto* ty = llvm::StructType::create(*context_,
        {llvm::Type::getInt32Ty(*context_), llvm::Type::getInt64Ty(*context_)},
        "__Result");
    structTypes_["__Result"] = ty;
    // NOTE: do NOT register structFieldNames_["__Result"] here.
    // If we did, emitMatchStmt would enter the isADT path for __Result-typed
    // match values and look up errorEnumValues_["__Result.Some"] (which is
    // never registered, since Option<T> is a generic ErrorDef skipped by Mono).
    // That causes the Some arm to be silently skipped and the default to always
    // fire.  The isResultStruct path (triggered when no structFieldNames_ entry
    // exists) correctly dispatches Some/None and binds the payload.
    return ty;
}

llvm::StructType* CodeGen::getOrCreateResultTypeForInner(llvm::Type* innerTy) {
    // For inner types that fit in 8 bytes, use the shared __Result layout.
    if (!innerTy) return getOrCreateResultType();
    uint64_t innerSz = module_->getDataLayout().getTypeAllocSize(innerTy);
    if (innerSz <= 8) return getOrCreateResultType();

    // For larger inner types (e.g. Vec<T>=24 bytes), create a per-size struct
    // __Result_<N> { i32 tag; [N x i8] payload } so the payload is never
    // truncated. The name encodes the size so distinct large types that happen
    // to have the same byte count share one struct, which is safe.
    std::string name = "__Result_" + std::to_string(innerSz);
    auto it = structTypes_.find(name);
    if (it != structTypes_.end()) return it->second;

    auto* i8Ty  = llvm::Type::getInt8Ty(*context_);
    auto* i32Ty = llvm::Type::getInt32Ty(*context_);
    auto* arrTy = llvm::ArrayType::get(i8Ty, innerSz);
    auto* ty    = llvm::StructType::create(*context_, {i32Ty, arrTy}, name);
    structTypes_[name] = ty;
    // Map the inner type so CodeGenMatch can recover it for payload loads.
    resultInnerTypes_[name] = innerTy;
    return ty;
}

SourceLocation CodeGen::codegenInternalSourceLocation() {
    static const SourceLocation loc = [] {
        SourceLocation l;
        l.filename = SourceLocation::intern("<codegen>");
        l.line = 0;
        l.column = 0;
        return l;
    }();
    return loc;
}

} // namespace vyx
