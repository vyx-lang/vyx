#include "CodeGenIncludes.h"
#include "../Sema/TemplateResolver.h"
#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace vyx {

llvm::Value* CodeGen::emitMethodCall(const Expr& expr) {
    auto* call = expr.as<const CallExpr>();
    if (!call->callee || call->callee->kind != ExprKind::MemberAccess) return nullptr;

    auto& memberExpr = *call->callee->as<MemberAccessExpr>();
    if (!memberExpr.object) return nullptr;

    auto compactTypeName = [](std::string name) {
        name.erase(std::remove_if(name.begin(), name.end(),
            [](unsigned char c) { return std::isspace(c); }), name.end());
        return name;
    };
    auto isVecOfFnType = [&](const std::string& typeName) {
        auto compact = compactTypeName(typeName);
        return compact.rfind("Vec<fn(", 0) == 0;
    };
    auto vecMethodStoresFnElement = [&](const std::string& receiverType,
                                        size_t argIdx) {
        if (!isVecOfFnType(receiverType)) return false;
        if ((memberExpr.member == "push" ||
             memberExpr.member == "push_unchecked") &&
            argIdx == 0) {
            return true;
        }
        if ((memberExpr.member == "set" ||
             memberExpr.member == "set_unchecked") &&
            argIdx == 1) {
            return true;
        }
        if (memberExpr.member == "insert" && argIdx == 1) {
            return true;
        }
        return false;
    };
    auto wrapRawFnValue = [&](llvm::Value* v, const Expr* arg) -> llvm::Value* {
        if (!v) return v;
        if (auto* rawFn = llvm::dyn_cast<llvm::Function>(v))
            return wrapRawFnAsFatPtr(rawFn);
        if (arg && arg->kind == ExprKind::Identifier) {
            auto& nm = arg->as<const IdentifierExpr>()->name;
            if (!closureFatPtrVars_.count(nm)) {
                auto fit = functions_.find(nm);
                if (fit != functions_.end())
                    return wrapRawFnAsFatPtr(fit->second);
            }
        }
        return v;
    };

    // Builtin operator-method dispatch on primitives.
    //
    // Generic code written against arithmetic traits (`fn f<T>(a: T, b: T) -> T
    // where T: Add + Mul { return a.op_mul(b).op_add(c); }`) instantiated at
    // T=i32/i64/f64/etc. needs `x.op_add(y)` to mean `x + y`. Without this
    // lowering, CodeGenCall reports "undefined function 'x.op_add'" because
    // primitives carry no method table.
    //
    // Mapping mirrors std.arith:
    //   op_add/op_sub/op_mul/op_div → integer/float arithmetic
    //   op_lt/op_le/op_gt/op_ge/op_eq/op_ne → comparison (returns i1)
    // Runs before the Promise handlers since those are unrelated.
    if (call->args.size() == 1 && call->args[0]) {
        static const std::set<std::string> kOpMethods = {
            "op_add", "op_sub", "op_mul", "op_div", "op_mod",
            "op_lt", "op_le", "op_gt", "op_ge", "op_eq", "op_ne",
        };
        if (kOpMethods.count(memberExpr.member)) {
            auto* lhs = emitExpr(*memberExpr.object);
            auto* rhs = emitExpr(*call->args[0]);
            if (lhs && rhs) {
                auto* lty = lhs->getType();
                auto* rty = rhs->getType();
                bool bothInt = lty->isIntegerTy() && rty->isIntegerTy();
                bool bothFP  = lty->isFloatingPointTy() && rty->isFloatingPointTy();
                if (bothInt || bothFP) {
                    if (bothInt && lty != rty) {
                        // Promote the narrower operand.
                        auto lb = lty->getIntegerBitWidth();
                        auto rb = rty->getIntegerBitWidth();
                        if (lb < rb) lhs = builder_->CreateSExt(lhs, rty);
                        else if (rb < lb) rhs = builder_->CreateSExt(rhs, lty);
                    }
                    auto& m = memberExpr.member;
                    if (bothInt) {
                        if (m == "op_add") return builder_->CreateAdd(lhs, rhs, "op.add");
                        if (m == "op_sub") return builder_->CreateSub(lhs, rhs, "op.sub");
                        if (m == "op_mul") return builder_->CreateMul(lhs, rhs, "op.mul");
                        if (m == "op_div") return builder_->CreateSDiv(lhs, rhs, "op.div");
                        if (m == "op_mod") return builder_->CreateSRem(lhs, rhs, "op.mod");
                        if (m == "op_lt") return builder_->CreateICmpSLT(lhs, rhs, "op.lt");
                        if (m == "op_le") return builder_->CreateICmpSLE(lhs, rhs, "op.le");
                        if (m == "op_gt") return builder_->CreateICmpSGT(lhs, rhs, "op.gt");
                        if (m == "op_ge") return builder_->CreateICmpSGE(lhs, rhs, "op.ge");
                        if (m == "op_eq") return builder_->CreateICmpEQ(lhs, rhs, "op.eq");
                        if (m == "op_ne") return builder_->CreateICmpNE(lhs, rhs, "op.ne");
                    } else { // bothFP
                        if (m == "op_add") return builder_->CreateFAdd(lhs, rhs, "op.fadd");
                        if (m == "op_sub") return builder_->CreateFSub(lhs, rhs, "op.fsub");
                        if (m == "op_mul") return builder_->CreateFMul(lhs, rhs, "op.fmul");
                        if (m == "op_div") return builder_->CreateFDiv(lhs, rhs, "op.fdiv");
                        if (m == "op_mod") return builder_->CreateFRem(lhs, rhs, "op.fmod");
                        if (m == "op_lt") return builder_->CreateFCmpOLT(lhs, rhs, "op.flt");
                        if (m == "op_le") return builder_->CreateFCmpOLE(lhs, rhs, "op.fle");
                        if (m == "op_gt") return builder_->CreateFCmpOGT(lhs, rhs, "op.fgt");
                        if (m == "op_ge") return builder_->CreateFCmpOGE(lhs, rhs, "op.fge");
                        if (m == "op_eq") return builder_->CreateFCmpOEQ(lhs, rhs, "op.feq");
                        if (m == "op_ne") return builder_->CreateFCmpONE(lhs, rhs, "op.fne");
                    }
                }
            }
        }
    }

    // Promise.all(p1, p2, ...) → await all promises, return tuple of results
    if (memberExpr.object->kind == ExprKind::Identifier &&
        memberExpr.object->as<IdentifierExpr>()->name == "Promise" && memberExpr.member == "all") {
        auto* i64Ty = llvm::Type::getInt64Ty(*context_);
        auto* fn = builder_->GetInsertBlock()->getParent();
        std::vector<llvm::Value*> results;
        for (auto& arg : call->args) {
            auto* val = emitExpr(*arg);
            if (val) results.push_back(val);
        }
        // Return as tuple struct
        if (results.size() == 2) {
            auto* tupTy = llvm::StructType::get(*context_, {results[0]->getType(), results[1]->getType()});
            auto* alloca = createEntryBlockAlloca(fn, tupTy, "promise.all");
            builder_->CreateStore(results[0], builder_->CreateStructGEP(tupTy, alloca, 0));
            builder_->CreateStore(results[1], builder_->CreateStructGEP(tupTy, alloca, 1));
            return builder_->CreateLoad(tupTy, alloca, "promise.all.val");
        }
        return results.empty() ? llvm::ConstantInt::get(i64Ty, 0) : results.back();
    }

    // Promise.race(p1, p2) → return the result of the first promise that completes
    if (memberExpr.object->kind == ExprKind::Identifier &&
        memberExpr.object->as<IdentifierExpr>()->name == "Promise" && memberExpr.member == "race") {
        if (!call->args.empty()) {
            return emitExpr(*call->args[0]);
        }
        return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), 0);
    }

    // Ref.new(val) → unified Ref<T> construction (replaces makeRef).
    //
    // P4-D.2 layout: the second `__RefCounted` slot holds a POINTER to a
    // heap-allocated 16-byte refcount header whose two i64 cells are
    // [strong_count=1][weak_count=0].  This matches the stdlib `Ref<T>`
    // methods (`count`, `weak_count`, `clone`, `drop`) which all treat
    // `self.rc` as a `rawptr` and memcpy through it.  Earlier iterations
    // stored the count INLINE (as an immediate i64 in slot 1), which made
    // `rc` undereferenceable — fatal for `Ref<T>.count()` / `Weak<T>`.
    if (memberExpr.object->kind == ExprKind::Identifier &&
        memberExpr.object->as<IdentifierExpr>()->name == "Ref" && memberExpr.member == "new") {
        if (!call->args.empty()) {
            auto* val = emitExpr(*call->args[0]);
            if (val) {
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
                // Zero the slot so a narrow LLVM store (e.g. i32 literal passed
                // to Ref::<i64>.new(7)) does not leave high bytes undefined;
                // stdlib get()/deref memcpy sizeof(T) and would otherwise read
                // garbage for the widened T.
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
                            val, llvm::IntegerType::get(*context_, targetBits), "ref.new.ext");
                    }
                }
                builder_->CreateStore(val, rawMem);

                // Allocate the 2-slot refcount header: [strong=1][weak=0].
                auto* rcMem = builder_->CreateCall(mallocFn,
                    {llvm::ConstantInt::get(i64Ty, 16)}, "ref.rc");
                builder_->CreateStore(llvm::ConstantInt::get(i64Ty, 1), rcMem);
                auto* weakSlot = builder_->CreateGEP(
                    llvm::Type::getInt8Ty(*context_), rcMem,
                    llvm::ConstantInt::get(i64Ty, 8), "ref.rc.weak");
                builder_->CreateStore(llvm::ConstantInt::get(i64Ty, 0), weakSlot);

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
                // Second slot is i64-typed in the struct; store the rc-header
                // pointer via ptrtoint so downstream pointer-loading code
                // (`%rc.val = load ptr ...`) sees the correct bytes.
                auto* rcAsI64 = builder_->CreatePtrToInt(rcMem, i64Ty, "ref.rc.i64");
                builder_->CreateStore(rcAsI64,
                    builder_->CreateStructGEP(refTy, alloca, 1, "ref.rcptr"));
                return builder_->CreateLoad(refTy, alloca, "ref.val");
            }
        }
        return llvm::Constant::getNullValue(llvm::PointerType::getUnqual(*context_));
    }

    // Weak.of(ref) is a borrow-like downgrade, not an ownership transfer.
    // Lower it directly so the Ref argument is not passed through the
    // by-value std method path, where callee-side auto-drop would decrement
    // the strong count and invalidate the caller's still-live Ref.
    if (memberExpr.object->kind == ExprKind::Identifier &&
        memberExpr.object->as<IdentifierExpr>()->name == "Weak" &&
        memberExpr.member == "of") {
        if (call->args.size() != 1 || !call->args[0]) {
            diag_.error(expr.location, "Weak.of requires one Ref<T> argument");
            return nullptr;
        }

        auto* ptrTy = llvm::PointerType::getUnqual(*context_);
        auto* i64Ty = llvm::Type::getInt64Ty(*context_);
        llvm::Value* dataPtr = nullptr;
        llvm::Value* rcRaw = nullptr;

        if (call->args[0]->kind == ExprKind::Identifier) {
            auto* refAddr = getVariableAddress(*call->args[0]);
            auto* refTy = refAddr ? getValuePtrType(refAddr) : nullptr;
            if (auto* refStTy = llvm::dyn_cast_or_null<llvm::StructType>(refTy)) {
                if (refStTy->getNumElements() >= 2) {
                    dataPtr = builder_->CreateLoad(
                        ptrTy,
                        builder_->CreateStructGEP(refStTy, refAddr, 0, "weak.of.data.ptr"),
                        "weak.of.data");
                    auto* rcSlotTy = refStTy->getElementType(1);
                    rcRaw = builder_->CreateLoad(
                        rcSlotTy,
                        builder_->CreateStructGEP(refStTy, refAddr, 1, "weak.of.rc.ptr"),
                        "weak.of.rc.raw");
                }
            }
        }

        if (!dataPtr || !rcRaw) {
            auto* refVal = emitExpr(*call->args[0]);
            if (auto* refStTy = refVal ? llvm::dyn_cast<llvm::StructType>(refVal->getType()) : nullptr) {
                if (refStTy->getNumElements() >= 2) {
                    dataPtr = builder_->CreateExtractValue(refVal, {0}, "weak.of.data");
                    rcRaw = builder_->CreateExtractValue(refVal, {1}, "weak.of.rc.raw");
                }
            }
        }

        if (!dataPtr || !rcRaw) {
            diag_.error(expr.location, "Weak.of expects a Ref<T> value");
            return nullptr;
        }

        llvm::Value* rcPtr = rcRaw;
        if (!rcPtr->getType()->isPointerTy())
            rcPtr = builder_->CreateIntToPtr(rcPtr, ptrTy, "weak.of.rc");
        if (dataPtr->getType() != ptrTy)
            dataPtr = castToType(dataPtr, ptrTy);

        auto* weakSlot = builder_->CreateGEP(
            llvm::Type::getInt8Ty(*context_), rcPtr,
            llvm::ConstantInt::get(i64Ty, 8), "weak.of.weak.slot");
        auto* oldWeak = builder_->CreateLoad(i64Ty, weakSlot, "weak.of.weak");
        auto* newWeak = builder_->CreateAdd(
            oldWeak, llvm::ConstantInt::get(i64Ty, 1), "weak.of.weak.inc");
        builder_->CreateStore(newWeak, weakSlot);

        std::string weakTypeName = "Weak";
        auto* weakId = memberExpr.object->as<IdentifierExpr>();
        if (!weakId->callTypeArgs.empty())
            weakTypeName = buildMangledClassName("Weak", weakId->callTypeArgs);

        llvm::StructType* weakTy = nullptr;
        auto weakIt = structTypes_.find(weakTypeName);
        if (weakIt != structTypes_.end()) {
            weakTy = weakIt->second;
        } else {
            weakTy = llvm::StructType::create(*context_, {ptrTy, ptrTy}, weakTypeName);
            structTypes_[weakTypeName] = weakTy;
            structFieldNames_[weakTypeName] = {"data", "rc"};
        }

        auto* fn = builder_->GetInsertBlock()->getParent();
        auto* out = createEntryBlockAlloca(fn, weakTy, "weak.of.tmp");
        builder_->CreateStore(dataPtr,
            builder_->CreateStructGEP(weakTy, out, 0, "weak.of.out.data"));
        builder_->CreateStore(rcPtr,
            builder_->CreateStructGEP(weakTy, out, 1, "weak.of.out.rc"));
        return builder_->CreateLoad(weakTy, out, "weak.of.val");
    }

    // Box<T> operations: Box.new(val), box.get(), box.set(val), box.drop()
    // Skip builtin Box.new when the call is Box::<T>.new() and "Box<T>" is a
    // user-defined struct (structTypes_ has the mangled key). In that case fall
    // BUG-LV-03 root fix — `lang_item("box")` is *always* lowered through the
    // hardcoded inline path below, even when `use std.ref;` brings the
    // `class Box<T>` declaration into the unit. Routing through the
    // monomorphised `Box<T>.new` function for `T = dyn Trait` skips the
    // concrete-to-fat-pointer boxing performed at lines 322-365, which is
    // what produces the silent crash logged as BUG-LV-03 (see
    // docs/LANG_VALIDATION_BUGS.md). The legacy `_boxUserClassCheck` was the
    // dual-track gate that flipped between hardcoded vs class-method paths
    // depending on whether `Box<T>` was registered in `structTypes_` —
    // exactly the duplicate representation the bug describes. Eliminating it
    // collapses both paths into the single canonical lowering.
    if (memberExpr.object->kind == ExprKind::Identifier &&
        memberExpr.object->as<IdentifierExpr>()->name == "Box" && memberExpr.member == "new") {
        if (!call->args.empty()) {
            auto* val = emitExpr(*call->args[0]);
            if (val) {
                auto* ptrTy = llvm::PointerType::getUnqual(*context_);
                auto* i64Ty = llvm::Type::getInt64Ty(*context_);
                auto* mallocFn = module_->getFunction("malloc");
                if (!mallocFn) {
                    auto* fty = llvm::FunctionType::get(ptrTy, {i64Ty}, false);
                    mallocFn = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, "malloc", *module_);
                }
                // Box<dyn Trait> special case: the user spelled
                // `Box::<dyn Trait>.new(concrete)` so the Box payload must be
                // a 16-byte `__iface_Trait` fat pointer, not the concrete
                // class's raw bytes. Without this detour the `.deref()` path
                // loads the first 16 bytes of the class (e.g. En.name's
                // string_ptr + length) and interprets them as {obj, vtable},
                // yielding an instant segfault on the first dispatch.
                //
                // Shape:
                //   objStorage = malloc(sizeof(Concrete)); store val → objStorage
                //   iface      = { objStorage, ConcreteClass_vtable_Trait }
                //   box.alloc  = malloc(16); store iface → box.alloc
                //
                // Gated on (a) the Box identifier carrying a type arg that
                // resolves to an `__iface_X` struct, AND (b) the concrete val
                // being a struct whose `<concreteName>_vtable_<traitName>`
                // global exists (emitted by generatePopulateInstanceVtables
                // when the class explicitly or structurally implements the
                // trait).
                auto& idExpr = *memberExpr.object->as<IdentifierExpr>();
                std::string ifaceName;
                if (!idExpr.callTypeArgs.empty() && idExpr.callTypeArgs[0]) {
                    auto& ta = *idExpr.callTypeArgs[0];
                    if (!ta.name.empty() &&
                        structTypes_.count("__iface_" + ta.name)) {
                        ifaceName = ta.name;
                    }
                }
                if (!ifaceName.empty()) {
                    std::string concreteName;
                    if (auto* vSt = llvm::dyn_cast<llvm::StructType>(val->getType())) {
                        if (vSt->hasName()) concreteName = vSt->getName().str();
                    }
                    std::string vtName = concreteName + "_vtable_" + ifaceName;
                    auto* vtGlobal = concreteName.empty()
                        ? nullptr
                        : module_->getGlobalVariable(vtName, true);
                    auto ifaceStIt = structTypes_.find("__iface_" + ifaceName);
                    if (vtGlobal && ifaceStIt != structTypes_.end() &&
                        val->getType()->isStructTy()) {
                        auto* ifaceStTy = ifaceStIt->second;
                        // Heap-allocate and spill the concrete instance so
                        // the fat pointer's obj slot carries a stable address.
                        uint64_t concBytes = module_->getDataLayout().getTypeAllocSize(val->getType());
                        if (concBytes < 8) concBytes = 8;
                        auto* objStorage = builder_->CreateCall(mallocFn,
                            {llvm::ConstantInt::get(i64Ty, concBytes)}, "box.dyn.obj");
                        builder_->CreateStore(val, objStorage);
                        // Build the iface pair in a stack slot, then blit it
                        // into the Box's heap payload. Using a stack slot
                        // keeps the two stores independent so LLVM's mem2reg
                        // can SROA them cleanly.
                        auto* fn2 = builder_->GetInsertBlock()->getParent();
                        auto* ifaceSlot = createEntryBlockAlloca(fn2, ifaceStTy, "box.dyn.iface");
                        builder_->CreateStore(objStorage,
                            builder_->CreateStructGEP(ifaceStTy, ifaceSlot, 0));
                        builder_->CreateStore(vtGlobal,
                            builder_->CreateStructGEP(ifaceStTy, ifaceSlot, 1));
                        auto* ifaceVal = builder_->CreateLoad(ifaceStTy, ifaceSlot, "box.dyn.iface.val");
                        auto* boxMem = builder_->CreateCall(mallocFn,
                            {llvm::ConstantInt::get(i64Ty, 16)}, "box.alloc");
                        builder_->CreateStore(ifaceVal, boxMem);
                        return boxMem;
                    }
                }
                uint64_t allocBytes = module_->getDataLayout().getTypeAllocSize(val->getType());
                if (allocBytes < 8) allocBytes = 8;
                auto* mem = builder_->CreateCall(mallocFn,
                    {llvm::ConstantInt::get(i64Ty, allocBytes)}, "box.alloc");
                llvm::Value* stored = val;
                if (val->getType()->isIntegerTy() && val->getType()->getIntegerBitWidth() < 64)
                    stored = builder_->CreateSExt(val, i64Ty);
                else if (val->getType()->isPointerTy())
                    stored = builder_->CreatePtrToInt(val, i64Ty);
                else if (val->getType()->isFloatingPointTy())
                    stored = builder_->CreateBitCast(
                        builder_->CreateFPExt(val, llvm::Type::getDoubleTy(*context_)), i64Ty);
                builder_->CreateStore(stored, mem);
                return mem;
            }
        }
        return llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context_));
    }
    if ((memberExpr.member == "get" || memberExpr.member == "deref") &&
        memberExpr.object) {
        // Box<T>.deref() / Box<T>.get(): load the T value out of the
        // Box's heap payload. Two receiver shapes:
        //   - Identifier receiver (`b.deref()`): look up the alloca.
        //     The tracked `refInnerTypeNames_` tells us T's LLVM layout.
        //   - Call-result receiver (`v.get(i).deref()`): emit the
        //     object expression; if it yields a pointer value *and*
        //     Sema tagged its inferredType as BoxPtr (box-like), load
        //     the inner T out via inferredType->pointeeType.
        if (memberExpr.object->kind == ExprKind::Identifier) {
            auto& objName = memberExpr.object->as<IdentifierExpr>()->name;
            // BUG-LV-03 — companion to the `Box.new` collapse above. When the
            // receiver is tagged as `Box<T>` (the lang_item("box") class is
            // the only thing that ever lowers via the hardcoded
            // malloc-and-cast path), force the inline get/deref path so the
            // load layout matches what `Box::<T>.new` actually returned. The
            // class-method dispatch below assumes `b` is a pointer to a Box
            // class struct (`{data: rawptr}`); under the unified hardcoded
            // path `b` is the raw `data` pointer itself, and the extra
            // indirection turns the load into a wild-pointer dereference.
            auto cvIt = classVarTypes_.find(objName);
            bool boxLangItem = false;
            if (cvIt != classVarTypes_.end()) {
                const std::string& cn = cvIt->second;
                if (cn == "Box" || (cn.size() >= 4 && cn.substr(0, 4) == "Box<"))
                    boxLangItem = true;
            }
            if ((!classVarTypes_.count(objName) && !containerTypes_.count(objName)) || boxLangItem) {
                auto nIt = namedValues_.find(objName);
                if (nIt != namedValues_.end()) {
                    auto* allocaTy = getValuePtrType(nIt->second);
                    if (allocaTy->isPointerTy()) {
                        auto* boxPtr = builder_->CreateLoad(allocaTy, nIt->second, "box.ptr");
                        if (memberExpr.member == "deref" &&
                            memberExpr.object->inferredType &&
                            isSmartPtrLike(*memberExpr.object->inferredType)) {
                            if (auto inner = smartPtrInner(*memberExpr.object->inferredType)) {
                                auto smartName = [](const std::string& n) {
                                    return (n.size() > 4 && n.compare(0, 4, "Box<") == 0) ||
                                           (n.size() > 4 && n.compare(0, 4, "Ref<") == 0) ||
                                           (n.size() > 6 && n.compare(0, 6, "Scope<") == 0);
                                };
                                auto* ptrTy = llvm::PointerType::getUnqual(*context_);
                                llvm::Type* innerTy =
                                    (isSmartPtrLike(*inner) || smartName(inner->name))
                                        ? static_cast<llvm::Type*>(ptrTy)
                                        : toLLVMType(*inner);
                                if (innerTy) {
                                    return builder_->CreateLoad(innerTy, boxPtr,
                                        "box.inferred.deref");
                                }
                            }
                        }
                        auto innerIt = refInnerTypeNames_.find(objName);
                        if (innerIt != refInnerTypeNames_.end()) {
                            auto stIt = structTypes_.find(innerIt->second);
                            if (stIt != structTypes_.end()) {
                                return builder_->CreateLoad(stIt->second, boxPtr, "box.deref");
                            }
                            // `Box<dyn Trait>`: refInnerTypeNames_ records the
                            // bare trait name (parser strips `dyn`), so the
                            // `structTypes_["Greeter"]` lookup above misses
                            // even though the iface struct `__iface_Greeter`
                            // has been registered by generateCreateInterfaceVtables.
                            // Retry via the `__iface_` prefix so .deref()
                            // materialises a 16-byte fat pointer instead of
                            // falling through to the 8-byte `box.val` load.
                            auto ifaceInnerIt = structTypes_.find("__iface_" +
                                innerIt->second);
                            if (ifaceInnerIt != structTypes_.end()) {
                                return builder_->CreateLoad(ifaceInnerIt->second,
                                    boxPtr, "box.iface.deref");
                            }
                            // Box<Box<T>> / Box<Ref<T>> / Box<Scope<T>>:
                            // the inner type is itself a smart pointer lowered
                            // as an opaque pointer, so structTypes_ won't have
                            // an entry.  Load the heap slot as a pointer so the
                            // chained .deref() sees an LLVM ptr value and the
                            // chain-call handler fires correctly.
                            {
                                const auto& inn = innerIt->second;
                                bool innerIsSmartPtr =
                                    (inn.size() > 4 && inn.compare(0, 4, "Box<") == 0) ||
                                    (inn.size() > 4 && inn.compare(0, 4, "Ref<") == 0) ||
                                    (inn.size() > 6 && inn.compare(0, 6, "Scope<") == 0);
                                if (innerIsSmartPtr) {
                                    auto* ptrTy = llvm::PointerType::getUnqual(*context_);
                                    return builder_->CreateLoad(ptrTy, boxPtr, "box.nested.deref");
                                }
                            }
                        }
                        // Scan-gap recovery: when `bx` is `Box<dyn Trait>`,
                        // Sema tags the call with Interface{name="Trait"}.
                        // Load the full `__iface_Trait` struct from the Box
                        // payload so downstream interface dispatch can
                        // recover obj/vtable pointers — without this the
                        // default i64 load below truncates the 16-byte iface
                        // slot and any subsequent chained `.method()` either
                        // misses dispatch or segfaults at runtime. Guarded
                        // by Sema tagging, which currently fires only when
                        // std.ref is explicitly imported so Box's class can
                        // be found; the industrial box_dyn_trait test still
                        // falls through to the i64 load with just
                        // `use std.collections`.
                        if (expr.inferredType &&
                            expr.inferredType->kind == VyxTypeKind::Interface &&
                            !expr.inferredType->name.empty()) {
                            auto ifaceIt = structTypes_.find("__iface_" +
                                expr.inferredType->name);
                            if (ifaceIt != structTypes_.end()) {
                                return builder_->CreateLoad(ifaceIt->second,
                                    boxPtr, "box.iface.deref");
                            }
                        }
                        auto* i64Ty = llvm::Type::getInt64Ty(*context_);
                        return builder_->CreateLoad(i64Ty, boxPtr, "box.val");
                    }
                }
            }
        } else if (memberExpr.object->inferredType &&
                   isSmartPtrLike(*memberExpr.object->inferredType)) {
            // Chain-call receiver is a Box/Ref/Scope returned by an earlier
            // call (or field access). Box<T> variables lower to the raw
            // payload pointer, while Ref/Scope values still carry a struct
            // whose first field is the payload pointer.
            //
            // The inner type T is recovered from the receiver's
            // `inferredType.paramTypes[0]` when Sema set it; otherwise we
            // parse the mangled class name (`Box<i32>` → `i32`) and lower
            // it through the TypeAnnotation overload.  The pre-existing
            // default-to-i64 fallback is retained as a last-ditch.
            auto* recvVal = emitExpr(*memberExpr.object);
            if (recvVal && recvVal->getType()->isPointerTy()) {
                auto* ptrTy = llvm::PointerType::getUnqual(*context_);
                auto smartName = [](const std::string& n) {
                    return (n.size() > 4 && n.compare(0, 4, "Box<") == 0) ||
                           (n.size() > 4 && n.compare(0, 4, "Ref<") == 0) ||
                           (n.size() > 6 && n.compare(0, 6, "Scope<") == 0);
                };
                auto boxName = [](const std::string& n) {
                    return n.size() > 4 && n.compare(0, 4, "Box<") == 0;
                };
                auto inner = smartPtrInner(*memberExpr.object->inferredType);
                bool receiverIsBox = isBoxLike(*memberExpr.object->inferredType) ||
                                     boxName(memberExpr.object->inferredType->name);
                // Box<dyn Trait> detour: Box payloads are already raw payload
                // pointers, so the full 16-byte fat pointer is a direct load.
                {
                    if (receiverIsBox && inner && !inner->name.empty()) {
                        auto ifaceDirect = structTypes_.find("__iface_" + inner->name);
                        if (ifaceDirect != structTypes_.end()) {
                            return builder_->CreateLoad(ifaceDirect->second,
                                recvVal, "box.chain.iface.deref");
                        }
                    }
                }
                llvm::Value* dataPtr = recvVal;
                if (!receiverIsBox) {
                    dataPtr = builder_->CreateLoad(ptrTy, recvVal,
                                                   "box.chain.data");
                }
                llvm::Type* innerLLVM = nullptr;
                if (inner) {
                    innerLLVM = (isSmartPtrLike(*inner) || smartName(inner->name))
                        ? static_cast<llvm::Type*>(ptrTy)
                        : toLLVMType(*inner);
                    // Unsubstituted generic `T` defaults to i64 via
                    // toLLVMType; that silently hides Mono-scan gaps, so
                    // fall through to the name-parse path instead.
                    if (innerLLVM == llvm::Type::getInt64Ty(*context_) &&
                        inner->kind == VyxTypeKind::Generic) {
                        innerLLVM = nullptr;
                    }
                }
                if (!innerLLVM) {
                    const std::string& mangled =
                        memberExpr.object->inferredType->name;
                    auto parsed = parseGenericArgs(mangled);
                    if (parsed && !parsed->typeArgs.empty()) {
                        auto ann = std::make_unique<NamedType>();
                        ann->name = parsed->typeArgs[0];
                        ann->location = memberExpr.location;
                        innerLLVM = toLLVMType(*ann);
                    }
                }
                if (!innerLLVM) innerLLVM = llvm::Type::getInt64Ty(*context_);
                return builder_->CreateLoad(innerLLVM, dataPtr,
                                             "box.chain.deref");
            }
        } else if (memberExpr.member == "deref" &&
                   memberExpr.object->kind == ExprKind::Call) {
            // Scan-gap recovery for `self.<field>.get(i).deref()` in a
            // generic class method: Sema's generic-field shortcut leaves the
            // chain's inferredType Unknown, so the BoxPtr branch above
            // can't fire. Reconstruct the Box inner type structurally from
            // the field's declared annotation (`Vec<Box<X>>.get(i).deref()`
            // → X). The shape we recognise is
            //   self.field.get(i).deref()
            // where self.field's declared type is `Vec<Box<X>>` on
            // currentClassName_ — the common case behind the industrial
            // `recursive_tree` failure.
            auto* getCall = memberExpr.object->as<CallExpr>();
            std::string innerStructName;
            if (getCall && getCall->callee &&
                getCall->callee->kind == ExprKind::MemberAccess) {
                auto* getMa = getCall->callee->as<MemberAccessExpr>();
                if (getMa->member == "get" && getMa->object &&
                    getMa->object->kind == ExprKind::MemberAccess &&
                    !currentClassName_.empty() && unit_) {
                    auto* innerMa = getMa->object->as<MemberAccessExpr>();
                    if (innerMa->object &&
                        innerMa->object->kind == ExprKind::SelfExpr) {
                        for (auto& d : unit_->declarations) {
                            if (!d || d->name != currentClassName_) continue;
                            const std::vector<FieldDecl>* flds = nullptr;
                            if (d->kind == DeclKind::Class)
                                flds = &d->as<ClassDecl>()->fields;
                            else if (d->kind == DeclKind::Struct)
                                flds = &d->as<StructDecl>()->fields;
                            if (!flds) break;
                            for (auto& f : *flds) {
                                if (f.name != innerMa->member || !f.type) continue;
                                if (f.type->kind != TypeAnnotationKind::Generic) break;
                                auto* cGt = f.type->as<GenericType>();
                                if (cGt->name != "Vec" || cGt->typeArgs.empty() ||
                                    !cGt->typeArgs[0]) break;
                                auto* elemAnn = cGt->typeArgs[0].get();
                                if (elemAnn->kind != TypeAnnotationKind::Generic) break;
                                auto* eGt = elemAnn->as<GenericType>();
                                if (eGt->name != "Box" || eGt->typeArgs.empty() ||
                                    !eGt->typeArgs[0]) break;
                                innerStructName = mangleTypeAnnotation(*eGt->typeArgs[0]);
                                break;
                            }
                            break;
                        }
                    }
                }
            }
            if (!innerStructName.empty()) {
                auto stIt = structTypes_.find(innerStructName);
                if (stIt != structTypes_.end()) {
                    auto* recvVal = emitExpr(*memberExpr.object);
                    if (recvVal && recvVal->getType()->isPointerTy()) {
                        return builder_->CreateLoad(stIt->second, recvVal,
                                                     "box.chain.deref");
                    }
                }
            }
        }
    }
    if (memberExpr.object->kind == ExprKind::Identifier && memberExpr.member == "set") {
        auto& objName2 = memberExpr.object->as<IdentifierExpr>()->name;
        if (!classVarTypes_.count(objName2) && !containerTypes_.count(objName2)) {
        if (!call->args.empty()) {
            auto nIt = namedValues_.find(objName2);
            if (nIt != namedValues_.end()) {
                auto* allocaTy = getValuePtrType(nIt->second);
                if (allocaTy->isPointerTy()) {
                    auto* boxPtr = builder_->CreateLoad(allocaTy, nIt->second, "box.ptr");
                    auto* val = emitExpr(*call->args[0]);
                    if (val && boxPtr) {
                        auto* i64Ty = llvm::Type::getInt64Ty(*context_);
                        llvm::Value* stored = val;
                        if (val->getType()->isIntegerTy() && val->getType()->getIntegerBitWidth() < 64)
                            stored = builder_->CreateSExt(val, i64Ty);
                        else if (val->getType()->isPointerTy())
                            stored = builder_->CreatePtrToInt(val, i64Ty);
                        else if (val->getType()->isFloatingPointTy())
                            stored = builder_->CreateBitCast(
                                builder_->CreateFPExt(val, llvm::Type::getDoubleTy(*context_)), i64Ty);
                        builder_->CreateStore(stored, boxPtr);
                    }
                    return val;
                }
            }
        }
        }
    }
    // Raw-pointer `.drop()` shortcut.  Legacy convenience for code that
    // stores a bare `rawptr` in a local and wants `free + null-out` via
    // `p.drop()`.  Skipped when:
    //   - the local is tracked as a class/container value (normal dispatch
    //     should find a typed `Drop::drop` impl), OR
    //   - a user-defined `<TypeName>.drop` function exists in functions_
    //     (Sema resolved this to a real method; don't clobber it with a
    //     raw-ptr free that would corrupt typed heap owners — this was
    //     the Front-4-revert root cause when std.String.drop was added
    //     and this handler intercepted before the user method dispatch).
    if (memberExpr.object->kind == ExprKind::Identifier && memberExpr.member == "drop") {
        auto& objName3 = memberExpr.object->as<IdentifierExpr>()->name;
        if (!classVarTypes_.count(objName3) && !containerTypes_.count(objName3)) {
        auto nIt = namedValues_.find(objName3);
        if (nIt != namedValues_.end()) {
            auto* allocaTy = getValuePtrType(nIt->second);
            // P4-A.1 guard: if the alloca is a concrete named struct type AND
            // a user-defined `<TypeName>.drop` function is registered, bail
            // out to normal method dispatch so the user's Drop impl runs.
            bool hasUserDrop = false;
            if (auto* structTy = allocaTy ? llvm::dyn_cast<llvm::StructType>(allocaTy) : nullptr) {
                if (structTy->hasName()) {
                    std::string typeName = structTy->getName().str();
                    if (functions_.count(typeName + ".drop")) {
                        hasUserDrop = true;
                    }
                }
            }
            if (!hasUserDrop && allocaTy && allocaTy->isPointerTy()) {
                auto* ptrTy = llvm::PointerType::getUnqual(*context_);
                auto* boxPtr = builder_->CreateLoad(allocaTy, nIt->second, "box.ptr");
                auto* freeFn = module_->getFunction("free");
                if (!freeFn) {
                    auto* fty = llvm::FunctionType::get(
                        llvm::Type::getVoidTy(*context_), {ptrTy}, false);
                    freeFn = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, "free", *module_);
                }
                builder_->CreateCall(freeFn, {boxPtr});
                builder_->CreateStore(
                    llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy)),
                    nIt->second);
                return llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0);
            }
        }
        }
    }

    // Static method dispatch: `ClassName.method()` / `ClassName::<T,N>.method()`.
    // Applies when the receiver IdentifierExpr names a type (not a variable)
    // and the call targets an arbitrary user-defined static method. Previously
    // this path was gated to `member == "new"` only, which silently routed
    // `Buffer::<i32, 16>.make()` to the instance-access fallback that can't
    // handle type-names. We now lift that gate and dispatch any static method
    // whose mangled form resolves in `functions_`. The named-constructor
    // fallback below still applies for `new` specifically.
    if (memberExpr.object->kind == ExprKind::Identifier) {
        auto* objId = memberExpr.object->as<IdentifierExpr>();
        std::string typeName = objId->name;
        auto genericReceiverIt = genericTypeParamNames_.find(typeName);
        const bool isGenericTypeParamReceiver =
            genericReceiverIt != genericTypeParamNames_.end();
        std::string resolvedTypeName =
            isGenericTypeParamReceiver ? genericReceiverIt->second : typeName;
        // Gate: the object must actually be a type name, not a value variable.
        // Presence of turbofish args (callTypeArgs / callArgExprs) OR absence
        // of a named variable/global binding indicates type-reference usage.
        bool isTypeRef =
            isGenericTypeParamReceiver ||
            !objId->callTypeArgs.empty() || !objId->callArgExprs.empty() ||
            (namedValues_.find(typeName) == namedValues_.end() &&
             module_->getGlobalVariable(typeName, true) == nullptr &&
             structTypes_.count(resolvedTypeName) > 0);

        if (isTypeRef && memberExpr.member != "new") {
            std::string mangledClassName = resolvedTypeName;
            if (!objId->callTypeArgs.empty()) {
                // Build mangled name using BOTH callTypeArgs and callArgExprs —
                // Mono emits the class under `mangleGenericMixed` which splices
                // const-literal values into the mangled form (e.g.
                // `Buffer<i32,16>`). If we used `buildMangledClassName(typeArgs)`
                // alone, const slots would be nullptr and collapse to a default
                // i64, producing `Buffer<i32,i64>` which never resolves.
                bool hasConstSlot = false;
                for (auto& ae : objId->callArgExprs)
                    if (ae) { hasConstSlot = true; break; }
                if (!hasConstSlot) {
                    mangledClassName = buildMangledClassName(resolvedTypeName, objId->callTypeArgs);
                } else {
                    size_t n = std::max(objId->callTypeArgs.size(),
                                        objId->callArgExprs.size());
                    std::string r = resolvedTypeName + "<";
                    for (size_t i = 0; i < n; ++i) {
                        if (i) r += ",";
                        const TypeAnnotation* ta =
                            i < objId->callTypeArgs.size() ? objId->callTypeArgs[i].get() : nullptr;
                        const Expr* ae =
                            i < objId->callArgExprs.size() ? objId->callArgExprs[i].get() : nullptr;
                        if (ae && ae->kind == ExprKind::IntLiteral) {
                            r += std::to_string(ae->as<IntLiteralExpr>()->value);
                        } else if (ta) {
                            TypeEnv env;
                            for (auto& [k, v] : genericTypeParamNames_)
                                env.bind(k, types::makeClass(v));
                            auto resolved = TemplateResolver::resolveTypeAnnotation(*ta);
                            auto sub = substituteType(resolved, env);
                            r += sub ? sub->mangle() : ta->name;
                        } else {
                            r += "?";
                        }
                    }
                    r += ">";
                    mangledClassName = r;
                }
            }
            std::string resolvedMember = memberExpr.member;
            if (!memberExpr.callTypeArgs.empty()) {
                resolvedMember = buildMangledClassName(
                    memberExpr.member, memberExpr.callTypeArgs);
            }
            auto* staticFn = findClassMethod(mangledClassName, resolvedMember);
            if (staticFn) {
                std::vector<llvm::Value*> argsV;
                for (size_t argIdx = 0; argIdx < call->args.size(); ++argIdx) {
                    auto& arg = call->args[argIdx];
                    if (arg) {
                        auto* val = emitExpr(*arg);
                        if (val) {
                            size_t paramIdx = argsV.size();
                            if (!memberExpr.callTypeArgs.empty() &&
                                paramIdx < staticFn->getFunctionType()->getNumParams() &&
                                staticFn->getFunctionType()->getParamType(paramIdx)->isPointerTy()) {
                                val = wrapRawFnValue(val, arg.get());
                            }
                            if (paramIdx < staticFn->getFunctionType()->getNumParams())
                                val = castToType(val, staticFn->getFunctionType()->getParamType(paramIdx));
                            argsV.push_back(val);
                        }
                    }
                }
                if (staticFn->getReturnType()->isVoidTy())
                    return builder_->CreateCall(staticFn, argsV);
                return builder_->CreateCall(staticFn, argsV, "static.call");
            }
        }
    }

    // Static method dispatch: ClassName.new() / ClassName::<T>.new()
    // Check for user-defined static new() before falling through to named constructor
    if (memberExpr.member == "new" && memberExpr.object->kind == ExprKind::Identifier) {
        std::string typeName = memberExpr.object->as<IdentifierExpr>()->name;

        std::string mangledClassName = typeName;
        auto* idE = memberExpr.object->as<IdentifierExpr>();
        if (!idE->callTypeArgs.empty()) {
            mangledClassName = buildMangledClassName(typeName, idE->callTypeArgs, idE->callArgExprs);
        }

        // Try user-defined static new() method (mangled name first, then base name)
        auto* newFn = findClassMethod(mangledClassName, "new");
        if (newFn) {
            std::vector<llvm::Value*> argsV;
            for (auto& arg : call->args) {
                if (arg) {
                    auto* val = emitExpr(*arg);
                    if (val) {
                        size_t paramIdx = argsV.size();
                        if (paramIdx < newFn->getFunctionType()->getNumParams())
                            val = castToType(val, newFn->getFunctionType()->getParamType(paramIdx));
                        argsV.push_back(val);
                    }
                }
            }
            if (newFn->getReturnType()->isVoidTy())
                return builder_->CreateCall(newFn, argsV);
            return builder_->CreateCall(newFn, argsV, "new.call");
        }

        // Fallback: named constructor StructName.new(args) → StructName { field0: arg0, ... }
        // Prefer the mangled name (e.g. `Vec<i32>`) when available; turbofish calls like
        // `Dict::<string,i32>.new()` register the struct under the mangled key.
        // Mono is authoritative for generic struct emission.  If mangledClassName
        // is absent from structTypes_ here, the Mono scan phase missed it (e.g.
        // a StructInit call site not yet covered by scanExprForRequests).
        // No on-demand synthesis is performed; the missing-struct diagnostic fires
        // downstream when the LLVM struct lookup fails.
        auto stIt = structTypes_.find(mangledClassName);
        if (stIt == structTypes_.end()) stIt = structTypes_.find(typeName);
        if (stIt != structTypes_.end()) {
            auto* structTy = stIt->second;
            auto& fields = structFieldNames_[stIt->first];
            auto* fn = builder_->GetInsertBlock()->getParent();
            auto* alloca = createEntryBlockAlloca(fn, structTy, "new.tmp");
            auto* ptrTy = llvm::PointerType::getUnqual(*context_);
            auto* i64Ty = llvm::Type::getInt64Ty(*context_);
            uint64_t structSize = module_->getDataLayout().getTypeAllocSize(structTy);
            auto* memsetFn = module_->getFunction("memset");
            if (!memsetFn) {
                auto* fty = llvm::FunctionType::get(ptrTy, {ptrTy, llvm::Type::getInt32Ty(*context_), i64Ty}, false);
                memsetFn = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, "memset", *module_);
            }
            builder_->CreateCall(memsetFn, {alloca,
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0),
                llvm::ConstantInt::get(i64Ty, structSize)});
            for (size_t i = 0; i < call->args.size() && i < fields.size(); ++i) {
                if (!call->args[i]) continue;
                auto* val = emitExpr(*call->args[i]);
                if (val) {
                    val = castToType(val, structTy->getElementType(i));
                    auto* gep = builder_->CreateStructGEP(structTy, alloca, i);
                    builder_->CreateStore(val, gep);
                }
            }
            return builder_->CreateLoad(structTy, alloca, "new.val");
        }
    }

    // self.method() dispatch inside class methods (Mono pre-instantiates all
    // generic methods, so a plain mangled-name lookup is sufficient here).
    // If the call carries method-level type arguments (turbofish notation
    // `self.method::<U>(args)`), build the mangled method name
    // "method<U1,U2,...>" and look that up instead. Mono emits a standalone
    // FunctionDecl under "ClassMono.method<U>" so findClassMethod("ClassMono",
    // "method<U>") resolves to the correct concrete body.
    if (memberExpr.object->kind == ExprKind::SelfExpr && !currentClassName_.empty()) {
        // Build the resolved method name: plain or mangled with type args.
        std::string resolvedMemberName = memberExpr.member;
        if (!memberExpr.callTypeArgs.empty()) {
            resolvedMemberName = buildMangledClassName(memberExpr.member, memberExpr.callTypeArgs);
        }
        llvm::Function* calledFn = findClassInstanceMethod(currentClassName_, resolvedMemberName);
        if (!calledFn && !memberExpr.callTypeArgs.empty()) {
            // Mono scan missed this method-level generic instantiation.
            diag_.error(expr.location,
                "codegen: method '{}::{}' with type args not found — Mono scan gap",
                currentClassName_, resolvedMemberName);
        }
        if (calledFn) {
            auto* selfAlloca = namedValues_["self"];
            if (selfAlloca) {
                auto* selfPtr = builder_->CreateLoad(
                    getValuePtrType(selfAlloca), selfAlloca, "self.ptr");
                std::vector<llvm::Value*> argsV = {selfPtr};
                for (auto& arg : call->args) {
                    if (arg) {
                        auto* val = emitExpr(*arg);
                        if (val) {
                            size_t paramIdx = argsV.size();
                            if (paramIdx < calledFn->getFunctionType()->getNumParams()) {
                                val = castToType(val, calledFn->getFunctionType()->getParamType(paramIdx));
                            }
                            argsV.push_back(val);
                        }
                    }
                }
                if (calledFn->getReturnType()->isVoidTy())
                    return builder_->CreateCall(calledFn, argsV);
                return builder_->CreateCall(calledFn, argsV, "self.call");
            }
        }
    }

    // Chained class-method dispatch: `expr.method(...)` where expr itself is
    // a non-Identifier (Call result, MemberAccess result, etc.). The standard
    // Identifier path below can't handle these, so we stage the receiver into
    // a temp alloca, derive className from its struct type, and dispatch via
    // findClassMethod. This unblocks chains like
    //   v.iter().filter(f).map::<U>(g).collect()
    // where each step's receiver is the previous step's call result.
    if (memberExpr.object->kind != ExprKind::Identifier &&
        memberExpr.object->inferredType &&
        (memberExpr.object->inferredType->kind == VyxTypeKind::Class ||
         memberExpr.object->inferredType->kind == VyxTypeKind::Struct)) {
        // Bug D1 fix: when the receiver is a field access (MemberAccess with a
        // SelfExpr or Identifier parent), GEP into the actual field alloca instead
        // of loading a temporary copy.  Mutations inside the called method (e.g.
        // self.counter = ...) are then visible to the caller.
        llvm::Value* recvAlloca = nullptr;
        llvm::StructType* recvStTy = nullptr;

        if (memberExpr.object->kind == ExprKind::MemberAccess) {
            auto& innerMa2 = *memberExpr.object->as<MemberAccessExpr>();
            // Determine the parent struct class name.
            std::string parentClass2;
            if (innerMa2.object && innerMa2.object->kind == ExprKind::SelfExpr && !currentClassName_.empty())
                parentClass2 = currentClassName_;
            else if (innerMa2.object && innerMa2.object->kind == ExprKind::Identifier)
                parentClass2 = resolveClassName(*innerMa2.object,
                    innerMa2.object->as<IdentifierExpr>()->name);

            if (!parentClass2.empty()) {
                auto fnIt3 = structFieldNames_.find(parentClass2);
                auto stIt3 = structTypes_.find(parentClass2);
                if (fnIt3 != structFieldNames_.end() && stIt3 != structTypes_.end()) {
                    auto& fldNames3 = fnIt3->second;
                    auto* parentStructTy3 = stIt3->second;
                    size_t fieldIdx3 = SIZE_MAX;
                    for (size_t i = 0; i < fldNames3.size(); ++i) {
                        if (fldNames3[i] == innerMa2.member) { fieldIdx3 = i; break; }
                    }
                    if (fieldIdx3 != SIZE_MAX) {
                        llvm::Value* parentPtr3 = nullptr;
                        if (innerMa2.object->kind == ExprKind::SelfExpr) {
                            auto selfIt3 = namedValues_.find("self");
                            if (selfIt3 != namedValues_.end())
                                parentPtr3 = builder_->CreateLoad(
                                    getValuePtrType(selfIt3->second), selfIt3->second, "self.ptr");
                        } else if (innerMa2.object->kind == ExprKind::Identifier) {
                            auto& idN3 = innerMa2.object->as<IdentifierExpr>()->name;
                            auto nvIt3 = namedValues_.find(idN3);
                            if (nvIt3 != namedValues_.end()) {
                                auto* allocaTy3 = getValuePtrType(nvIt3->second);
                                if (allocaTy3 == parentStructTy3)
                                    parentPtr3 = nvIt3->second;
                                else if (allocaTy3->isPointerTy())
                                    parentPtr3 = builder_->CreateLoad(allocaTy3, nvIt3->second, "ref.self");
                            }
                        }
                        if (parentPtr3) {
                            recvAlloca = builder_->CreateStructGEP(
                                parentStructTy3, parentPtr3, (unsigned)fieldIdx3, "field.ptr");
                            // Derive the struct type from the GEP result element type.
                            auto* gepElemTy = parentStructTy3->getElementType((unsigned)fieldIdx3);
                            recvStTy = llvm::dyn_cast<llvm::StructType>(gepElemTy);
                            if (!recvStTy) {
                                // Not a struct element — fall through to the load path.
                                recvAlloca = nullptr;
                            }
                        }
                    }
                }
            }
        }

        if (!recvAlloca) {
            // General/fallback path: load the receiver into a temp alloca.
            auto* recvVal = emitExpr(*memberExpr.object);
            if (recvVal) {
                auto* fn = builder_->GetInsertBlock()->getParent();
                if (recvVal->getType()->isPointerTy()) {
                    recvAlloca = recvVal;
                    auto sit = structTypes_.find(memberExpr.object->inferredType->name);
                    if (sit != structTypes_.end())
                        recvStTy = sit->second;
                } else if (auto* st = llvm::dyn_cast<llvm::StructType>(recvVal->getType())) {
                    recvStTy = st;
                    recvAlloca = createEntryBlockAlloca(fn, st, "chain.tmp");
                    builder_->CreateStore(recvVal, recvAlloca);
                }
            }
        }

        if (recvAlloca && recvStTy && recvStTy->hasName()) {
            std::string className = recvStTy->getName().str();
            std::string resolvedMember = memberExpr.member;
            if (!memberExpr.callTypeArgs.empty()) {
                resolvedMember = buildMangledClassName(memberExpr.member, memberExpr.callTypeArgs);
            }
            auto* methodFn = findClassInstanceMethod(className, resolvedMember);
            if (methodFn) {
                std::vector<llvm::Value*> argsV = {recvAlloca};
                for (size_t argIdx = 0; argIdx < call->args.size(); ++argIdx) {
                    auto& arg = call->args[argIdx];
                    if (!arg) continue;
                    auto* v = emitExpr(*arg);
                    if (v) {
                        size_t pi = argsV.size();
                        // Raw-fn → fat-ptr wrap (mirrors the Identifier-receiver
                        // path): generic method bodies read a `fn(T)->U` param
                        // as a `{fn_ptr, env_ptr}` fat pointer and call it via
                        // `fn_ptr(env_ptr, args...)`. When the caller passes a
                        // bare function name (`v.iter().map::<i32>(dbl)`) we
                        // get an `llvm::Function*` whose signature has no env
                        // slot. Without this wrap the callee loads the first
                        // 8 bytes of the function body as a pointer and jumps
                        // to garbage — the classic chain-arg segfault that
                        // broke `static_method_chain`.
                        if (!memberExpr.callTypeArgs.empty() &&
                            pi < methodFn->getFunctionType()->getNumParams() &&
                            methodFn->getFunctionType()->getParamType(pi)->isPointerTy()) {
                            v = wrapRawFnValue(v, arg.get());
                        }
                        if (pi < methodFn->getFunctionType()->getNumParams())
                            v = castToType(v, methodFn->getFunctionType()->getParamType(pi));
                        argsV.push_back(v);
                    }
                }
                if (methodFn->getReturnType()->isVoidTy()) {
                    builder_->CreateCall(methodFn, argsV);
                    return llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0);
                }
                return builder_->CreateCall(methodFn, argsV, "chain.call");
            }
        }
    }

    // Class method dispatch (check Sema-inferred type first, fallback to classVarTypes_)
    if (memberExpr.object->kind == ExprKind::Identifier) {
        auto& idn = memberExpr.object->as<IdentifierExpr>()->name;
        std::string className = resolveClassName(*memberExpr.object, idn);
        // Fallback: if sema/classVarTypes_ are silent but the identifier is a
        // tracked container (Dict/Vec/Set/Stack/Queue/Mutex/Channel/Event/Delegate),
        // use the container kind as the class name so findClassMethod can locate
        // methods like Dict.set / Vec.push on the std container class.
        if (className.empty()) {
            auto ctFb = containerTypes_.find(idn);
            if (ctFb != containerTypes_.end() && !ctFb->second.empty()) {
                className = ctFb->second;
            }
        }
        // Primitive-target impl fallback (2026-04-23): string vars get
        // classified as `__String` (the LLVM struct's internal name, see
        // trackParamType in CodeGenCore.cpp).  When the user wrote
        // `impl Hashable for string { fn hash() ... }`, the method function
        // is mangled `string.hash`, not `__String.hash`.  If the impl
        // mangle resolves, swap the key.  Same story for any primitive
        // alias — keep this conservative (only remap when we'd otherwise
        // miss).
        if (className == "__String" && findClassMethod("string", memberExpr.member)) {
            className = "string";
        }
        // When className is still empty (e.g. generic receiver whose
        // inferredType is Generic and no mapping survived to CodeGen), try
        // to recover from the alloca's LLVM value type.  Only for primitives
        // whose LLVM shape uniquely names the source type (i32/i64/…, and
        // the shared __String struct).  Guarded by an existence check so
        // non-impl receivers still fall through to existing paths.
        if (className.empty()) {
            auto nvIt = namedValues_.find(idn);
            if (nvIt != namedValues_.end() && nvIt->second) {
                auto* storedTy = getValuePtrType(nvIt->second);
                if (storedTy) {
                    if (storedTy->isIntegerTy()) {
                        unsigned bits = storedTy->getIntegerBitWidth();
                        if (bits == 8)  className = "i8";
                        else if (bits == 16) className = "i16";
                        else if (bits == 32) className = "i32";
                        else if (bits == 64) className = "i64";
                        else if (bits == 1)  className = "bool";
                    } else if (storedTy->isFloatTy())  className = "f32";
                    else if (storedTy->isDoubleTy()) className = "f64";
                    else if (auto* stTy = llvm::dyn_cast<llvm::StructType>(storedTy)) {
                        if (stTy->hasName() && stTy->getName() == "__String")
                            className = "string";
                    }
                    if (!className.empty() &&
                        !findClassMethod(className, memberExpr.member)) {
                        className.clear();
                    }
                }
            }
        }
        if (!className.empty()) {
            // Mono pre-instantiates all generic methods.  For method-level
            // generic calls (`obj.method::<U>(args)`) Mono emits a standalone
            // FunctionDecl under "ClassName.method<U>", so we build the mangled
            // method suffix here and look it up.  If the name isn't in
            // functions_, Mono missed it — emit a diagnostic rather than
            // silently falling through to unrelated methods.
            std::string resolvedMember = memberExpr.member;
            if (!memberExpr.callTypeArgs.empty()) {
                resolvedMember = buildMangledClassName(memberExpr.member, memberExpr.callTypeArgs);
            }
            llvm::Function* methodFn = findClassInstanceMethod(className, resolvedMember);
            if (!methodFn && !memberExpr.callTypeArgs.empty()) {
                diag_.error(expr.location,
                    "codegen: method-generic '{}::{}' not found — Mono scan gap",
                    className, resolvedMember);
            }
            if (methodFn) {
                auto* objAlloca = namedValues_.count(memberExpr.object->as<IdentifierExpr>()->name)
                    ? namedValues_[memberExpr.object->as<IdentifierExpr>()->name]
                    : static_cast<llvm::Value*>(module_->getGlobalVariable(memberExpr.object->as<IdentifierExpr>()->name, true));
                // Dereference &mut reference parameters to get the actual struct pointer
                if (objAlloca) {
                    auto* storedTy = getValuePtrType(objAlloca);
                    if (storedTy && storedTy->isPointerTy() &&
                        methodFn->getFunctionType()->getNumParams() > 0 &&
                        methodFn->getFunctionType()->getParamType(0)->isPointerTy()) {
                        objAlloca = builder_->CreateLoad(storedTy, objAlloca, "ref.self");
                    }
                }
                // Primitive-target impl: if the function takes self by value
                // (`impl Hashable for i32 { fn hash() -> i64 { ... } }` emitted
                // as `i64 i32.hash(i32 self)`), load the value and pass it
                // instead of the alloca pointer.  Preserves the normal class
                // by-pointer ABI for everything else.
                bool selfByValueEarly = false;
                if (methodFn->getFunctionType()->getNumParams() > 0 &&
                    !methodFn->getFunctionType()->getParamType(0)->isPointerTy()) {
                    selfByValueEarly = true;
                }
                std::vector<llvm::Value*> argsV;
                const bool expectsSelf =
                    methodFn->getFunctionType()->getNumParams() > call->args.size();
                if (expectsSelf) {
                    if (!objAlloca) {
                        diag_.error(expr.location,
                            "codegen: instance method '{}.{}' has no receiver storage",
                            className, resolvedMember);
                        return nullptr;
                    }
                    if (selfByValueEarly) {
                        auto* p0 = methodFn->getFunctionType()->getParamType(0);
                        argsV.push_back(builder_->CreateLoad(p0, objAlloca, "prim.self"));
                    } else {
                        argsV.push_back(objAlloca);
                    }
                }
                std::string receiverTypeName = className;
                if (!isVecOfFnType(receiverTypeName)) {
                    auto inferredReceiver = inferredClassName(*memberExpr.object);
                    if (!inferredReceiver.empty()) receiverTypeName = inferredReceiver;
                }
                for (size_t argIdx = 0; argIdx < call->args.size(); ++argIdx) {
                    size_t paramIdx = argsV.size();
                    auto* savedHint = targetTypeHint_;
                    if (paramIdx < methodFn->getFunctionType()->getNumParams()) {
                        if (auto* pStTy = llvm::dyn_cast<llvm::StructType>(
                                methodFn->getFunctionType()->getParamType(paramIdx))) {
                            if (pStTy->hasName()) {
                                auto nm = pStTy->getName();
                                if (isOptionOrResultSlotName(nm))
                                    targetTypeHint_ = pStTy;
                            }
                        }
                    }
                    auto* v = emitExpr(*call->args[argIdx]);
                    targetTypeHint_ = savedHint;
                    if (v) {
                        // Raw-fn -> fat-ptr wrap. Method-generic callbacks are
                        // invoked through closureFatPtrVars_ inside the callee;
                        // Vec<fn> element writes also need the same heap fat
                        // handle so get() returns one uniform representation.
                        bool storesFnElement =
                            vecMethodStoresFnElement(receiverTypeName, argIdx);
                        if ((storesFnElement || !memberExpr.callTypeArgs.empty()) &&
                            paramIdx < methodFn->getFunctionType()->getNumParams() &&
                            methodFn->getFunctionType()->getParamType(paramIdx)->isPointerTy()) {
                            v = wrapRawFnValue(v, call->args[argIdx].get());
                        }
                        if (paramIdx < methodFn->getFunctionType()->getNumParams()) {
                            auto* paramTy = methodFn->getFunctionType()->getParamType(paramIdx);
                            // Interface boxing: if the method's param is
                            // an `__iface_X` struct and we're passing a
                            // concrete class value, build the proper
                            // {obj_ptr, vtable_ptr} pair instead of
                            // letting castToType zero-pad (NULL vtable
                            // → segfault on later dispatch).
                            if (v->getType()->isStructTy() && paramTy->isStructTy() &&
                                v->getType() != paramTy) {
                                if (auto* paramStTy = llvm::dyn_cast<llvm::StructType>(paramTy)) {
                                    if (paramStTy->hasName() &&
                                        paramStTy->getName().starts_with("__iface_")) {
                                        auto ifaceName = paramStTy->getName().substr(8).str();
                                        std::string concreteName;
                                        if (call->args[argIdx]->kind == ExprKind::StructInit)
                                            concreteName = call->args[argIdx]->as<StructInitExpr>()->structName;
                                        else if (call->args[argIdx]->kind == ExprKind::Identifier)
                                            concreteName = resolveClassName(*call->args[argIdx],
                                                call->args[argIdx]->as<IdentifierExpr>()->name);
                                        if (concreteName.empty()) {
                                            if (auto* argSt = llvm::dyn_cast<llvm::StructType>(v->getType())) {
                                                if (argSt->hasName()) concreteName = argSt->getName().str();
                                            }
                                        }
                                        // Already-boxed interface values must pass through.
                                        if (!concreteName.empty() &&
                                            concreteName.rfind("__iface_", 0) == 0) {
                                            concreteName.clear();
                                        }
                                        if (!concreteName.empty()) {
                                            auto* fn2 = builder_->GetInsertBlock()->getParent();
                                            auto* objStorage = createEntryBlockAlloca(fn2, v->getType(), "iface.arg.obj");
                                            builder_->CreateStore(v, objStorage);
                                            auto* ifaceAlloca = createEntryBlockAlloca(fn2, paramStTy, "iface.arg");
                                            builder_->CreateStore(objStorage,
                                                builder_->CreateStructGEP(paramStTy, ifaceAlloca, 0));
                                            std::string vtName = concreteName + "_vtable_" + ifaceName;
                                            auto* vtGlobal = module_->getGlobalVariable(vtName, true);
                                            if (vtGlobal) {
                                                builder_->CreateStore(vtGlobal,
                                                    builder_->CreateStructGEP(paramStTy, ifaceAlloca, 1));
                                            }
                                            v = builder_->CreateLoad(paramStTy, ifaceAlloca, "iface.arg.val");
                                        }
                                    }
                                }
                            }
                            v = castToType(v, paramTy);
                        }
                        argsV.push_back(v);
                    }
                }
                if (methodFn->getReturnType()->isVoidTy()) {
                    builder_->CreateCall(methodFn, argsV);
                    return llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0);
                }
                return builder_->CreateCall(methodFn, argsV, "method.result");
            }
        }
    }

    // Chain method dispatch: obj.field.method() — resolve field type from struct definition
    if (memberExpr.object->kind == ExprKind::MemberAccess && unit_) {
        auto& innerMa = *memberExpr.object->as<MemberAccessExpr>();
        std::string parentClass;
        if (innerMa.object && innerMa.object->kind == ExprKind::SelfExpr && !currentClassName_.empty())
            parentClass = currentClassName_;
        else if (innerMa.object && innerMa.object->kind == ExprKind::Identifier) {
            auto& idName = innerMa.object->as<IdentifierExpr>()->name;
            parentClass = resolveRefInner(*innerMa.object, idName);
            if (parentClass.empty())
                parentClass = resolveClassName(*innerMa.object, idName);
        }
        if (!parentClass.empty()) {
            std::string fieldTypeName;
            bool fieldIsRefLike = false;
            for (auto& d : unit_->declarations) {
                if (!d || d->name != parentClass) continue;
                std::vector<FieldDecl>* flds = nullptr;
                if (d->kind == DeclKind::Class) flds = &d->as<ClassDecl>()->fields;
                else if (d->kind == DeclKind::Struct) flds = &d->as<StructDecl>()->fields;
                if (flds) {
                    for (auto& f : *flds) {
                        if (f.name != innerMa.member || !f.type) continue;
                        if ((f.type->name == "Ref" || f.type->name == "Scope") &&
                            f.type->kind == TypeAnnotationKind::Generic) {
                            fieldIsRefLike = true;
                            auto& fSubs = static_cast<const GenericType&>(*f.type).typeArgs;
                            if (!fSubs.empty() && fSubs[0])
                                fieldTypeName = mangleTypeAnnotation(*fSubs[0]);
                        } else {
                            std::string mn = mangleTypeAnnotation(*f.type);
                            if (structTypes_.count(mn)) fieldTypeName = mn;
                            else if (structTypes_.count(f.type->name)) fieldTypeName = f.type->name;
                        }
                    }
                }
                break;
            }
            // Secondary fallback: parentClass didn't expose this field (possibly
            // mis-bound through an ADT variant mix-up such as enum Decl.Function
            // colliding with enum TypeAnnotation.Function — the match binder can
            // pick up the wrong payload type). When the outer call is a common
            // container method, search all other struct decls for a field of the
            // same name whose type mangles to an already-instantiated container,
            // so that findClassMethod resolves to a real (emitted) method body.
            if (fieldTypeName.empty() && unit_) {
                static const char* kContainerMethods[] = {
                    "get", "set", "push", "pop", "insert", "remove",
                    "contains", "clear", "put", "size", "len",
                    "keys", "values", "entries", "at", "front", "back"
                };
                bool isContainerMethod = false;
                for (auto* m : kContainerMethods) {
                    if (memberExpr.member == m) { isContainerMethod = true; break; }
                }
                if (isContainerMethod) {
                    // Find any candidate that resolves to a real emitted method
                    // via findClassMethod (it strips '<...>' to fall back to the
                    // base class method name, so all Vec<T>/Dict<K,V>
                    // instantiations share the underlying Vec/Dict body).
                    std::string candidate;
                    for (auto& d : unit_->declarations) {
                        if (!d) continue;
                        if (d->kind != DeclKind::Struct && d->kind != DeclKind::Class) continue;
                        std::vector<FieldDecl>* flds = (d->kind == DeclKind::Class)
                            ? &d->as<ClassDecl>()->fields
                            : &d->as<StructDecl>()->fields;
                        for (auto& f : *flds) {
                            if (f.name != innerMa.member || !f.type) continue;
                            std::string mn = mangleTypeAnnotation(*f.type);
                            std::string cand;
                            if (structTypes_.count(mn)) cand = mn;
                            else if (structTypes_.count(f.type->name)) cand = f.type->name;
                            if (cand.empty()) continue;
                            if (!findClassMethod(cand, memberExpr.member)) continue;
                            candidate = cand;
                            break;
                        }
                        if (!candidate.empty()) break;
                    }
                    if (!candidate.empty()) fieldTypeName = candidate;
                }
            }
            if (!fieldTypeName.empty()) {
                llvm::Function* methodFn = findClassMethod(fieldTypeName, memberExpr.member);
                if (methodFn) {
                    // Bug D1 fix: emit GEP into the actual field alloca instead of
                    // loading a struct copy.  This keeps mutations (self.counter = ...)
                    // visible to the caller after the method returns.
                    llvm::Value* selfArg = nullptr;

                    // Try to build a GEP to the field in the parent struct.
                    // Supported parent shapes:
                    //   SelfExpr  → load namedValues_["self"] to get selfPtr, GEP field
                    //   Identifier → namedValues_[name] is the struct alloca, GEP field
                    auto tryBuildFieldGEP = [&]() -> llvm::Value* {
                        auto fnIt2 = structFieldNames_.find(parentClass);
                        if (fnIt2 == structFieldNames_.end()) return nullptr;
                        auto& fieldNames = fnIt2->second;
                        size_t fieldIdx = SIZE_MAX;
                        for (size_t i = 0; i < fieldNames.size(); ++i) {
                            if (fieldNames[i] == innerMa.member) { fieldIdx = i; break; }
                        }
                        if (fieldIdx == SIZE_MAX) return nullptr;

                        auto stIt2 = structTypes_.find(parentClass);
                        if (stIt2 == structTypes_.end()) return nullptr;
                        auto* parentStructTy = stIt2->second;

                        llvm::Value* parentPtr = nullptr;
                        if (innerMa.object->kind == ExprKind::SelfExpr) {
                            auto selfIt = namedValues_.find("self");
                            if (selfIt == namedValues_.end()) return nullptr;
                            // namedValues_["self"] is an alloca of ptr-to-struct
                            parentPtr = builder_->CreateLoad(
                                getValuePtrType(selfIt->second), selfIt->second, "self.ptr");
                        } else if (innerMa.object->kind == ExprKind::Identifier) {
                            auto& idN = innerMa.object->as<IdentifierExpr>()->name;
                            auto nvIt = namedValues_.find(idN);
                            if (nvIt == namedValues_.end()) return nullptr;
                            auto* allocaTy = getValuePtrType(nvIt->second);
                            if (allocaTy == parentStructTy) {
                                // namedValues_[name] is the struct alloca directly
                                parentPtr = nvIt->second;
                            } else if (allocaTy->isPointerTy()) {
                                parentPtr = builder_->CreateLoad(allocaTy, nvIt->second, "ref.self");
                            } else {
                                return nullptr;
                            }
                        }
                        if (!parentPtr) return nullptr;
                        return builder_->CreateStructGEP(
                            parentStructTy, parentPtr, (unsigned)fieldIdx, "field.ptr");
                    };

                    selfArg = tryBuildFieldGEP();

                    if (!selfArg) {
                        // Fallback: evaluate the field expression (may produce a copy
                        // for true rvalue chains like call().field.method()).
                        auto* objVal = emitExpr(*memberExpr.object);
                        if (objVal) {
                            if (!objVal->getType()->isPointerTy()) {
                                auto* fn2 = builder_->GetInsertBlock()->getParent();
                                auto* tmp = createEntryBlockAlloca(fn2, objVal->getType(), "chain.self");
                                builder_->CreateStore(objVal, tmp);
                                selfArg = tmp;
                            } else {
                                selfArg = objVal;
                            }
                        }
                    }

                    if (selfArg) {
                        auto rcIt = structTypes_.find("__RefCounted");
                        if (fieldIsRefLike && rcIt != structTypes_.end() &&
                            getValuePtrType(selfArg) == rcIt->second) {
                            auto* dataPtr = builder_->CreateStructGEP(
                                rcIt->second, selfArg, 0, "ref.chain.data.ptr");
                            selfArg = builder_->CreateLoad(
                                llvm::PointerType::getUnqual(*context_),
                                dataPtr, "ref.chain.data");
                        }
                        std::vector<llvm::Value*> argsV = {selfArg};
                        for (size_t argIdx = 0; argIdx < call->args.size(); ++argIdx) {
                            size_t paramIdx = argIdx + 1;
                            auto* v = emitExpr(*call->args[argIdx]);
                            if (v) {
                                if (rcIt != structTypes_.end() &&
                                    v->getType() == rcIt->second &&
                                    paramIdx < methodFn->getFunctionType()->getNumParams()) {
                                    auto* fn2 = builder_->GetInsertBlock()->getParent();
                                    auto* argAlloca = createEntryBlockAlloca(
                                        fn2, rcIt->second, "ref.chain.arg.tmp");
                                    builder_->CreateStore(v, argAlloca);
                                    auto* argDataPtr = builder_->CreateStructGEP(
                                        rcIt->second, argAlloca, 0, "ref.chain.arg.data.ptr");
                                    auto* argData = builder_->CreateLoad(
                                        llvm::PointerType::getUnqual(*context_),
                                        argDataPtr, "ref.chain.arg.data");
                                    auto* expectedTy =
                                        methodFn->getFunctionType()->getParamType(paramIdx);
                                    if (expectedTy->isPointerTy()) {
                                        v = argData;
                                    } else {
                                        v = builder_->CreateLoad(
                                            expectedTy, argData, "ref.chain.arg.val");
                                    }
                                }
                                bool storesFnElement =
                                    vecMethodStoresFnElement(fieldTypeName, argIdx);
                                if (storesFnElement &&
                                    paramIdx < methodFn->getFunctionType()->getNumParams() &&
                                    methodFn->getFunctionType()->getParamType(paramIdx)->isPointerTy()) {
                                    v = wrapRawFnValue(v, call->args[argIdx].get());
                                }
                                if (paramIdx < methodFn->getFunctionType()->getNumParams())
                                    v = castToType(v, methodFn->getFunctionType()->getParamType(paramIdx));
                                argsV.push_back(v);
                            }
                        }
                        if (methodFn->getReturnType()->isVoidTy()) {
                            builder_->CreateCall(methodFn, argsV);
                            return llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0);
                        }
                        return builder_->CreateCall(methodFn, argsV, "chain.method.result");
                    }
                }
            }
        }
    }

    // ADT cast methods: .as_variant_name() → extract variant data from ADT enum
    if (memberExpr.member.size() > 3 && memberExpr.member.substr(0, 3) == "as_" && unit_) {
        std::string snakeName = memberExpr.member.substr(3);
        // Convert snake_case → CamelCase
        std::string camelName;
        bool capitalizeNext = true;
        for (char c : snakeName) {
            if (c == '_') { capitalizeNext = true; continue; }
            camelName += capitalizeNext ? (char)toupper(c) : c;
            capitalizeNext = false;
        }

        llvm::Value* adtVal = nullptr;
        std::string adtTypeName;

        auto getAdtVal = [&](llvm::Value* objVal) -> bool {
            if (!objVal) return false;
            auto* objTy = objVal->getType();
            // Direct ADT struct value
            if (auto* stTy = llvm::dyn_cast<llvm::StructType>(objTy)) {
                std::string tn = stTy->hasName() ? stTy->getName().str() : "";
                if (tn == "__RefCounted") {
                    // Ref<ADT>: load inner ADT from the data pointer
                    std::string innerType;
                    if (memberExpr.object) {
                        std::string legacy;
                        if (memberExpr.object->kind == ExprKind::Identifier)
                            legacy = memberExpr.object->as<IdentifierExpr>()->name;
                        innerType = resolveRefInner(*memberExpr.object, legacy);
                    }
                    // Fallback: search all ADT types for one that has this variant
                    if (innerType.empty()) {
                        for (auto& [evKey, evTag] : errorEnumValues_) {
                            auto dotPos = evKey.rfind('.');
                            if (dotPos != std::string::npos && evKey.substr(dotPos + 1) == camelName) {
                                std::string candidate = evKey.substr(0, dotPos);
                                auto candIt = structTypes_.find(candidate);
                                if (candIt != structTypes_.end()) {
                                    auto fnChk = structFieldNames_.find(candidate);
                                    if (fnChk != structFieldNames_.end() && fnChk->second.size() >= 2 &&
                                        fnChk->second[0] == "__tag" && fnChk->second[1] == "__data") {
                                        innerType = candidate;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    if (!innerType.empty()) {
                        auto innerStIt = structTypes_.find(innerType);
                        if (innerStIt != structTypes_.end()) {
                            auto fnIt4 = builder_->GetInsertBlock()->getParent();
                            auto* refAlloca = createEntryBlockAlloca(fnIt4, stTy, "as.ref.tmp");
                            builder_->CreateStore(objVal, refAlloca);
                            auto* dataPtrField = builder_->CreateStructGEP(stTy, refAlloca, 0, "ref.data.ptr");
                            auto* rawPtr = builder_->CreateLoad(llvm::PointerType::getUnqual(*context_), dataPtrField, "ref.raw.ptr");
                            adtVal = builder_->CreateLoad(innerStIt->second, rawPtr, "ref.adt");
                            adtTypeName = innerType;
                            return true;
                        }
                    }
                }
                auto fnIt2 = structFieldNames_.find(tn);
                if (fnIt2 != structFieldNames_.end() && fnIt2->second.size() >= 2 &&
                    fnIt2->second[0] == "__tag" && fnIt2->second[1] == "__data") {
                    adtVal = objVal;
                    adtTypeName = tn;
                    return true;
                }
            }
            // Pointer to ADT (Box<ADT>)
            if (objTy->isPointerTy()) {
                std::string innerType;
                if (memberExpr.object) {
                    std::string legacy;
                    if (memberExpr.object->kind == ExprKind::Identifier)
                        legacy = memberExpr.object->as<IdentifierExpr>()->name;
                    innerType = resolveRefInner(*memberExpr.object, legacy);
                }
                if (innerType.empty()) {
                    std::string camelLower = camelName;
                    for (auto& ch : camelLower) ch = (char)tolower(ch);
                    for (auto& [evKey, evTag] : errorEnumValues_) {
                        auto dotPos = evKey.rfind('.');
                        if (dotPos == std::string::npos) continue;
                        std::string variantName = evKey.substr(dotPos + 1);
                        std::string variantLower = variantName;
                        for (auto& ch : variantLower) ch = (char)tolower(ch);
                        if (variantName == camelName || variantLower == camelLower) {
                            std::string candidate = evKey.substr(0, dotPos);
                            auto candIt = structTypes_.find(candidate);
                            if (candIt != structTypes_.end()) {
                                auto fnChk = structFieldNames_.find(candidate);
                                if (fnChk != structFieldNames_.end() && fnChk->second.size() >= 2 &&
                                    fnChk->second[0] == "__tag" && fnChk->second[1] == "__data") {
                                    innerType = candidate;
                                    break;
                                }
                            }
                        }
                    }
                }
                if (innerType.empty() && unit_) {
                    for (auto& d : unit_->declarations) {
                        if (!d || d->kind != DeclKind::ErrorDef) continue;
                        auto* errDecl = d->as<ErrorDefDecl>();
                        for (size_t vi = 0; vi < errDecl->variantTypes.size(); ++vi) {
                            if (!errDecl->variantTypes[vi].empty() && errDecl->variantTypes[vi][0] &&
                                errDecl->variantTypes[vi][0]->name == camelName) {
                                std::string candidate = d->name;
                                if (structTypes_.count(candidate)) {
                                    auto fnChk = structFieldNames_.find(candidate);
                                    if (fnChk != structFieldNames_.end() && fnChk->second.size() >= 2 &&
                                        fnChk->second[0] == "__tag" && fnChk->second[1] == "__data") {
                                        innerType = candidate;
                                        break;
                                    }
                                }
                            }
                        }
                        if (!innerType.empty()) break;
                    }
                }
                if (!innerType.empty()) {
                    auto innerStIt = structTypes_.find(innerType);
                    if (innerStIt != structTypes_.end()) {
                        adtVal = builder_->CreateLoad(innerStIt->second, objVal, "box.adt.cast");
                        adtTypeName = innerType;
                        return true;
                    }
                }
            }
            return false;
        };

        llvm::Value* objVal = nullptr;
        if (memberExpr.object->kind == ExprKind::Identifier) {
            auto nvIt = namedValues_.find(memberExpr.object->as<IdentifierExpr>()->name);
            if (nvIt != namedValues_.end())
                objVal = builder_->CreateLoad(getValuePtrType(nvIt->second), nvIt->second, "as.obj");
        } else {
            objVal = emitExpr(*memberExpr.object);
        }

        // If the loaded value is an integer (from bad deref), try reloading from alloca with correct ADT type
        if (objVal && objVal->getType()->isIntegerTy() && memberExpr.object->kind == ExprKind::Identifier) {
            auto& idn2 = memberExpr.object->as<IdentifierExpr>()->name;
            auto nvIt = namedValues_.find(idn2);
            if (nvIt != namedValues_.end()) {
                std::string cn = resolveClassName(*memberExpr.object, idn2);
                if (!cn.empty()) {
                    auto stIt = structTypes_.find(cn);
                    if (stIt != structTypes_.end()) {
                        auto fnChk = structFieldNames_.find(cn);
                        if (fnChk != structFieldNames_.end() && fnChk->second.size() >= 2 &&
                            fnChk->second[0] == "__tag" && fnChk->second[1] == "__data") {
                            objVal = builder_->CreateLoad(stIt->second, nvIt->second, "as.reload.adt");
                        }
                    }
                }
            }
        }

        if (getAdtVal(objVal)) {
            std::string adtBaseName = adtTypeName;
            if (auto ltPos = adtBaseName.find('<'); ltPos != std::string::npos)
                adtBaseName = adtBaseName.substr(0, ltPos);
            auto evIt = errorEnumValues_.find(adtTypeName + "." + camelName);
            if (evIt == errorEnumValues_.end())
                evIt = errorEnumValues_.find(adtBaseName + "." + camelName);

            std::string resolvedVariant = camelName;
            if (evIt == errorEnumValues_.end()) {
                for (auto& d : unit_->declarations) {
                    if (!d || d->kind != DeclKind::ErrorDef || d->name != adtBaseName) continue;
                    auto* errDecl = d->as<ErrorDefDecl>();
                    for (size_t vi = 0; vi < errDecl->variants.size(); ++vi) {
                        if (!errDecl->variantTypes[vi].empty() && errDecl->variantTypes[vi][0]) {
                            std::string dataTypeName = errDecl->variantTypes[vi][0]->name;
                            if (dataTypeName == camelName) {
                                resolvedVariant = errDecl->variants[vi];
                                evIt = errorEnumValues_.find(adtTypeName + "." + resolvedVariant);
                                if (evIt == errorEnumValues_.end())
                                    evIt = errorEnumValues_.find(adtBaseName + "." + resolvedVariant);
                                break;
                            }
                        }
                        // Case-insensitive variant name match
                        std::string varLower = errDecl->variants[vi];
                        std::string camelLower = camelName;
                        for (auto& ch : varLower) ch = (char)tolower(ch);
                        for (auto& ch : camelLower) ch = (char)tolower(ch);
                        if (varLower == camelLower) {
                            resolvedVariant = errDecl->variants[vi];
                            evIt = errorEnumValues_.find(adtTypeName + "." + resolvedVariant);
                            if (evIt == errorEnumValues_.end())
                                evIt = errorEnumValues_.find(adtBaseName + "." + resolvedVariant);
                            break;
                        }
                    }
                    break;
                }
            }

            if (evIt != errorEnumValues_.end()) {
                for (auto& d : unit_->declarations) {
                    if (!d || d->kind != DeclKind::ErrorDef || d->name != adtBaseName) continue;
                    auto* errDecl = d->as<ErrorDefDecl>();
                    for (size_t vi = 0; vi < errDecl->variants.size(); ++vi) {
                        if (errDecl->variants[vi] != resolvedVariant) continue;
                        if (errDecl->variantTypes[vi].empty()) break;
                        auto* varTy = toLLVMType(*errDecl->variantTypes[vi][0]);
                        auto* fn3 = builder_->GetInsertBlock()->getParent();
                        auto* adtStTy = llvm::cast<llvm::StructType>(adtVal->getType());
                        auto* adtAlloca = createEntryBlockAlloca(fn3, adtStTy, "as.adt.tmp");
                        builder_->CreateStore(adtVal, adtAlloca);
                        auto* dataPtr = builder_->CreateStructGEP(adtStTy, adtAlloca, 1, "as.data");
                        auto* result = builder_->CreateLoad(varTy, dataPtr, "as." + resolvedVariant);
                        return result;
                    }
                    break;
                }
            }
        }
    }

    // [fat-std] Built-in Vec/Dict/Set/Queue/Stack methods removed — now in std/*.vyx

    // Method dispatch on nested member access (e.g., self.vec_field.push())
    // or builder pattern on temporary struct values (e.g., Dict.new().put(...))
    if (memberExpr.object->kind != ExprKind::Identifier) {
        // Try to get the address of a nested member (self.field) for in-place mutation
        llvm::Value* memberAddr = nullptr;
        if (memberExpr.object->kind == ExprKind::MemberAccess)
            memberAddr = getMemberAddress(*memberExpr.object);
        if (memberAddr) {
            auto* fieldTy = getValuePtrType(memberAddr);
            if (auto* stTy = llvm::dyn_cast<llvm::StructType>(fieldTy)) {
                std::string typeName = stTy->hasName() ? stTy->getName().str() : "";

                // Dict.keys() / Dict.values() → call the method but cast result to Vec type
                if ((memberExpr.member == "keys" || memberExpr.member == "values") &&
                    typeName.find("Dict<") != std::string::npos) {
                    auto* methodFn = findClassMethod(typeName, memberExpr.member);
                    if (methodFn) {
                        auto* rawResult = builder_->CreateCall(methodFn, {memberAddr}, "dict.kv.raw");
                        auto stIt = structTypes_.find("__Vec");
                        llvm::StructType* vecTy = stIt != structTypes_.end() ?
                            llvm::dyn_cast<llvm::StructType>(stIt->second) : nullptr;
                        if (vecTy && rawResult->getType() != vecTy) {
                            auto* fn2 = builder_->GetInsertBlock()->getParent();
                            auto* tmpAlloca = createEntryBlockAlloca(fn2, rawResult->getType(), "dict.kv.tmp");
                            builder_->CreateStore(rawResult, tmpAlloca);
                            return builder_->CreateLoad(vecTy, tmpAlloca, "dict.kv.vec");
                        }
                        return rawResult;
                    }
                }

                // Resolve mangled container type from struct field annotation
                std::string resolvedTypeName = typeName;
                
                if ((typeName.find("Vec") != std::string::npos || typeName.find("Dict") != std::string::npos ||
                     typeName == "__Vec" || typeName == "__Dict") &&
                    memberExpr.object->kind == ExprKind::MemberAccess && unit_) {
                    auto& parentMa = *memberExpr.object->as<MemberAccessExpr>();
                    std::string parentClass;
                    if (parentMa.object && parentMa.object->kind == ExprKind::Identifier) {
                        parentClass = resolveClassName(*parentMa.object,
                            parentMa.object->as<IdentifierExpr>()->name);
                        if (parentClass.empty()) {
                            parentClass = resolveRefInner(*parentMa.object,
                                parentMa.object->as<IdentifierExpr>()->name);
                        }
                    } else if (parentMa.object && parentMa.object->kind == ExprKind::SelfExpr && !currentClassName_.empty()) {
                        parentClass = currentClassName_;
                    }
                    if (!parentClass.empty()) {
                        for (auto& dd : unit_->declarations) {
                            if (!dd || dd->name != parentClass) continue;
                            std::vector<FieldDecl>* flds = nullptr;
                            if (dd->kind == DeclKind::Class) flds = &dd->as<ClassDecl>()->fields;
                            else if (dd->kind == DeclKind::Struct) flds = &dd->as<StructDecl>()->fields;
                            if (flds) {
                                for (auto& f : *flds) {
                                    if (f.name == parentMa.member && f.type) {
                                        std::string mangledFieldType = mangleTypeAnnotation(*f.type);
                                        // Mono is authoritative for field-type generic emission.
                                        // If mangledFieldType is absent from structTypes_ here,
                                        // the Mono scan missed it (field-chain gap).  We check
                                        // for an existing method in functions_ as a secondary signal
                                        // so that uninstantiated field types don't silently fall
                                        // through to wrong methods.
                                        if (structTypes_.count(mangledFieldType) ||
                                            functions_.count(mangledFieldType + "." + memberExpr.member)) {
                                            resolvedTypeName = mangledFieldType;
                                        }
                                    }
                                }
                            }
                            break;
                        }
                    }
                }
                // Resolve the method name: if callTypeArgs is set, build
                // the mangled form "method<U>" so findClassMethod returns
                // the Mono-emitted concrete body.
                std::string fieldMember = memberExpr.member;
                if (!memberExpr.callTypeArgs.empty())
                    fieldMember = buildMangledClassName(memberExpr.member, memberExpr.callTypeArgs);
                auto* methodFn = findClassMethod(resolvedTypeName, fieldMember);
                if (!methodFn && resolvedTypeName != typeName)
                    methodFn = findClassMethod(typeName, fieldMember);
                if (methodFn) {
                    std::vector<llvm::Value*> argsV = {memberAddr};
                    for (size_t argIdx = 0; argIdx < call->args.size(); ++argIdx) {
                        if (call->args[argIdx]) {
                            size_t paramIdx = argsV.size();
                            auto* savedHint = targetTypeHint_;
                            if (paramIdx < methodFn->getFunctionType()->getNumParams()) {
                                if (auto* pStTy = llvm::dyn_cast<llvm::StructType>(
                                        methodFn->getFunctionType()->getParamType(paramIdx))) {
                                    if (pStTy->hasName()) {
                                        auto nm = pStTy->getName();
                                        if (isOptionOrResultSlotName(nm))
                                            targetTypeHint_ = pStTy;
                                    }
                                }
                            }
                            auto* v = emitExpr(*call->args[argIdx]);
                            targetTypeHint_ = savedHint;
                            if (v) {
                                bool storesFnElement =
                                    vecMethodStoresFnElement(resolvedTypeName, argIdx);
                                if (storesFnElement &&
                                    paramIdx < methodFn->getFunctionType()->getNumParams() &&
                                    methodFn->getFunctionType()->getParamType(paramIdx)->isPointerTy()) {
                                    v = wrapRawFnValue(v, call->args[argIdx].get());
                                }
                                if (paramIdx < methodFn->getFunctionType()->getNumParams())
                                    v = castToType(v, methodFn->getFunctionType()->getParamType(paramIdx));
                                argsV.push_back(v);
                            }
                        }
                    }
                    if (methodFn->getReturnType()->isVoidTy()) {
                        builder_->CreateCall(methodFn, argsV);
                        return builder_->CreateLoad(fieldTy, memberAddr, "nested.self");
                    }
                    return builder_->CreateCall(methodFn, argsV, "nested.call");
                }
            }
        }
        auto* objVal = emitExpr(*memberExpr.object);
        if (objVal && objVal->getType()->isStructTy()) {
            auto* stTy = llvm::dyn_cast<llvm::StructType>(objVal->getType());
            if (stTy && stTy->hasName()) {
                std::string typeName = stTy->getName().str();
                auto* methodFn = findClassMethod(typeName, memberExpr.member);
                if (methodFn) {
                    auto* fn = builder_->GetInsertBlock()->getParent();
                    auto* tmpAlloca = createEntryBlockAlloca(fn, stTy, "builder.tmp");
                    builder_->CreateStore(objVal, tmpAlloca);
                    std::vector<llvm::Value*> argsV = {tmpAlloca};
                    for (auto& arg : call->args) {
                        if (arg) {
                            auto* v = emitExpr(*arg);
                            if (v) {
                                size_t paramIdx = argsV.size();
                                if (paramIdx < methodFn->getFunctionType()->getNumParams())
                                    v = castToType(v, methodFn->getFunctionType()->getParamType(paramIdx));
                                argsV.push_back(v);
                            }
                        }
                    }
                    auto* result = builder_->CreateCall(methodFn, argsV,
                        methodFn->getReturnType()->isVoidTy() ? "" : "builder.call");
                    if (methodFn->getReturnType()->isVoidTy()) {
                        return builder_->CreateLoad(stTy, tmpAlloca, "builder.self");
                    }
                    return result;
                }
            }
        }
        if (objVal && objVal->getType()->isPointerTy()) {
            // Chain on pointer return (builder pattern with self-returning methods)
            // Try all known class types
            for (auto& [name, fn] : functions_) {
                if (name.find("." + memberExpr.member) != std::string::npos &&
                    name.find(".") == name.find("." + memberExpr.member)) {
                    std::string className = name.substr(0, name.find("."));
                    if (structTypes_.count(className)) {
                        std::vector<llvm::Value*> argsV = {objVal};
                        for (auto& arg : call->args) {
                            if (arg) {
                                auto* v = emitExpr(*arg);
                                if (v) {
                                    size_t paramIdx = argsV.size();
                                    if (paramIdx < fn->getFunctionType()->getNumParams())
                                        v = castToType(v, fn->getFunctionType()->getParamType(paramIdx));
                                    argsV.push_back(v);
                                }
                            }
                        }
                        if (fn->getReturnType()->isVoidTy()) {
                            builder_->CreateCall(fn, argsV);
                            return objVal;
                        }
                        return builder_->CreateCall(fn, argsV, "chain.call");
                    }
                }
            }
        }
    }

    // Any type methods: asInt, asString, asFloat, asBool, isInt, isString, tag
    if (memberExpr.object->kind == ExprKind::Identifier) {
        auto nvIt = namedValues_.find(memberExpr.object->as<IdentifierExpr>()->name);
        if (nvIt != namedValues_.end() && getValuePtrType(nvIt->second) == getOrCreateAnyType()) {
            auto* anyTy = getOrCreateAnyType();
            auto* i8Ty = llvm::Type::getInt8Ty(*context_);
            auto* i32Ty = llvm::Type::getInt32Ty(*context_);
            auto* i64Ty = llvm::Type::getInt64Ty(*context_);
            auto* tagPtr = builder_->CreateStructGEP(anyTy, nvIt->second, 0, "any.tag.ptr");
            auto* valPtr = builder_->CreateStructGEP(anyTy, nvIt->second, 1, "any.val.ptr");
            auto* tag = builder_->CreateLoad(i8Ty, tagPtr, "any.tag");
            auto* val = builder_->CreateLoad(i64Ty, valPtr, "any.val");

            if (memberExpr.member == "asInt") {
                return builder_->CreateTrunc(val, i32Ty, "any.asInt");
            }
            if (memberExpr.member == "asI64") return val;
            if (memberExpr.member == "asFloat") {
                return builder_->CreateBitCast(val, llvm::Type::getDoubleTy(*context_), "any.asFloat");
            }
            if (memberExpr.member == "asBool") {
                return builder_->CreateTrunc(val, llvm::Type::getInt8Ty(*context_), "any.asBool");
            }
            if (memberExpr.member == "asString") {
                diag_.error(memberExpr.location,
                    "Any.asString is not supported at codegen; use typed accessors (asInt, asI64, …) or narrow the value first");
                return nullptr;
            }
            if (memberExpr.member == "asPtr") {
                return builder_->CreateIntToPtr(val, llvm::PointerType::getUnqual(*context_), "any.asPtr");
            }
            if (memberExpr.member == "tag") return builder_->CreateZExt(tag, i32Ty, "any.tag.i32");
            if (memberExpr.member == "isInt") {
                return createSafeICmp(llvm::CmpInst::ICMP_EQ, tag, llvm::ConstantInt::get(tag->getType(), 1), "any.isInt");
            }
            if (memberExpr.member == "isString") {
                return createSafeICmp(llvm::CmpInst::ICMP_EQ, tag, llvm::ConstantInt::get(tag->getType(), 4), "any.isString");
            }
            if (memberExpr.member == "isFloat") {
                return createSafeICmp(llvm::CmpInst::ICMP_EQ, tag, llvm::ConstantInt::get(tag->getType(), 3), "any.isFloat");
            }
            if (memberExpr.member == "isBool") {
                return createSafeICmp(llvm::CmpInst::ICMP_EQ, tag, llvm::ConstantInt::get(tag->getType(), 5), "any.isBool");
            }
            if (memberExpr.member == "isPtr") {
                return createSafeICmp(llvm::CmpInst::ICMP_EQ, tag, llvm::ConstantInt::get(tag->getType(), 6), "any.isPtr");
            }
            if (memberExpr.member == "isStruct") {
                return createSafeICmp(llvm::CmpInst::ICMP_EQ, tag, llvm::ConstantInt::get(tag->getType(), 7), "any.isStruct");
            }
            // .getTypeName() → return type name as string
            if (memberExpr.member == "getTypeName") {
                auto* fn2 = builder_->GetInsertBlock()->getParent();
                auto* defaultBB = llvm::BasicBlock::Create(*context_, "any.name.default", fn2);
                auto* mergeBB = llvm::BasicBlock::Create(*context_, "any.name.merge", fn2);
                auto* strTy = getOrCreateStringType();
                auto* resultAlloca = createEntryBlockAlloca(fn2, strTy, "any.typename");
                auto* sw = builder_->CreateSwitch(tag, defaultBB, 8);
                static const std::pair<int, const char*> tagNames[] = {
                    {0, "void"}, {1, "i32"}, {2, "i64"}, {3, "f64"},
                    {4, "string"}, {5, "bool"}, {6, "rawptr"}, {7, "struct"}
                };
                for (auto& [t, n] : tagNames) {
                    auto* bb = llvm::BasicBlock::Create(*context_, std::string("any.name.") + n, fn2);
                    sw->addCase(llvm::ConstantInt::get(i8Ty, t), bb);
                    builder_->SetInsertPoint(bb);
                    builder_->CreateStore(createStringValue(n), resultAlloca);
                    builder_->CreateBr(mergeBB);
                }
                builder_->SetInsertPoint(defaultBB);
                diag_.error(memberExpr.location, "Any.getTypeName: tag value has no registered type name");
                emitPanicCall(getOrCreateString("Any.getTypeName: bad tag"));
                builder_->SetInsertPoint(mergeBB);
                return builder_->CreateLoad(strTy, resultAlloca, "any.typename.val");
            }
        }
    }

    // Mutex methods: .lock() / .unlock()
    if (memberExpr.object->kind == ExprKind::Identifier) {
        auto ctIt = containerTypes_.find(memberExpr.object->as<IdentifierExpr>()->name);
        if (ctIt != containerTypes_.end() && ctIt->second == "Mutex") {
            auto nvIt = namedValues_.find(memberExpr.object->as<IdentifierExpr>()->name);
            if (nvIt != namedValues_.end()) {
                auto* ptrTy = llvm::PointerType::getUnqual(*context_);
                auto* i64Ty = llvm::Type::getInt64Ty(*context_);
                auto* mutexPtr = builder_->CreateLoad(ptrTy, nvIt->second, "mutex.ptr");
                if (memberExpr.member == "lock") {
                    auto* enterFn = module_->getFunction("EnterCriticalSection");
                    if (!enterFn) {
                        auto* fty = llvm::FunctionType::get(llvm::Type::getVoidTy(*context_), {ptrTy}, false);
                        enterFn = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, "EnterCriticalSection", *module_);
                    }
                    builder_->CreateCall(enterFn, {mutexPtr});
                    auto* valSlot = builder_->CreateGEP(llvm::Type::getInt8Ty(*context_), mutexPtr,
                        llvm::ConstantInt::get(i64Ty, 40), "mutex.val.ptr");
                    return builder_->CreateLoad(i64Ty, valSlot, "mutex.val");
                }
                if (memberExpr.member == "unlock") {
                    auto* leaveFn = module_->getFunction("LeaveCriticalSection");
                    if (!leaveFn) {
                        auto* fty = llvm::FunctionType::get(llvm::Type::getVoidTy(*context_), {ptrTy}, false);
                        leaveFn = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, "LeaveCriticalSection", *module_);
                    }
                    builder_->CreateCall(leaveFn, {mutexPtr});
                    return llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0);
                }
            }
        }
        // Channel methods: .send(val) / .recv()
        if (ctIt != containerTypes_.end() && ctIt->second == "Channel") {
            auto nvIt = namedValues_.find(memberExpr.object->as<IdentifierExpr>()->name);
            if (nvIt != namedValues_.end()) {
                auto* ptrTy = llvm::PointerType::getUnqual(*context_);
                auto* i64Ty = llvm::Type::getInt64Ty(*context_);
                auto* chanPtr = builder_->CreateLoad(ptrTy, nvIt->second, "chan.ptr");
                if (memberExpr.member == "send" && !call->args.empty()) {
                    auto* val = emitExpr(*call->args[0]);
                    if (!val) return llvm::ConstantInt::get(i64Ty, 0);
                    auto* enterFn = module_->getFunction("EnterCriticalSection");
                    if (!enterFn) {
                        auto* fty = llvm::FunctionType::get(llvm::Type::getVoidTy(*context_), {ptrTy}, false);
                        enterFn = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, "EnterCriticalSection", *module_);
                    }
                    builder_->CreateCall(enterFn, {chanPtr});
                    auto* valSlot = builder_->CreateGEP(llvm::Type::getInt8Ty(*context_), chanPtr,
                        llvm::ConstantInt::get(i64Ty, 40), "chan.val.ptr");
                    llvm::Value* stored = val;
                    if (stored->getType()->isIntegerTy() && stored->getType()->getIntegerBitWidth() < 64)
                        stored = builder_->CreateSExt(stored, i64Ty);
                    builder_->CreateStore(stored, valSlot);
                    auto* flagPtr = builder_->CreateGEP(llvm::Type::getInt8Ty(*context_), chanPtr,
                        llvm::ConstantInt::get(i64Ty, 48), "chan.flag.ptr");
                    builder_->CreateStore(llvm::ConstantInt::get(i64Ty, 1), flagPtr);
                    auto* leaveFn = module_->getFunction("LeaveCriticalSection");
                    if (!leaveFn) {
                        auto* fty = llvm::FunctionType::get(llvm::Type::getVoidTy(*context_), {ptrTy}, false);
                        leaveFn = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, "LeaveCriticalSection", *module_);
                    }
                    builder_->CreateCall(leaveFn, {chanPtr});
                    return llvm::ConstantInt::get(i64Ty, 0);
                }
                if (memberExpr.member == "recv") {
                    auto* fn = builder_->GetInsertBlock()->getParent();
                    auto* pollBB = llvm::BasicBlock::Create(*context_, "chan.poll", fn);
                    auto* recvBB = llvm::BasicBlock::Create(*context_, "chan.recv", fn);
                    builder_->CreateBr(pollBB);
                    builder_->SetInsertPoint(pollBB);
                    auto* flagPtr = builder_->CreateGEP(llvm::Type::getInt8Ty(*context_), chanPtr,
                        llvm::ConstantInt::get(i64Ty, 48), "chan.flag.ptr");
                    auto* flag = builder_->CreateLoad(i64Ty, flagPtr, "chan.flag");
                    auto* hasVal = createSafeICmp(llvm::CmpInst::ICMP_NE, flag,
                        llvm::ConstantInt::get(flag->getType(), 0), "chan.ready");
                    builder_->CreateCondBr(hasVal, recvBB, pollBB);
                    builder_->SetInsertPoint(recvBB);
                    auto* enterFn = module_->getFunction("EnterCriticalSection");
                    if (!enterFn) {
                        auto* fty = llvm::FunctionType::get(llvm::Type::getVoidTy(*context_), {ptrTy}, false);
                        enterFn = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, "EnterCriticalSection", *module_);
                    }
                    builder_->CreateCall(enterFn, {chanPtr});
                    auto* valSlot = builder_->CreateGEP(llvm::Type::getInt8Ty(*context_), chanPtr,
                        llvm::ConstantInt::get(i64Ty, 40), "chan.val.ptr");
                    auto* result = builder_->CreateLoad(i64Ty, valSlot, "chan.val");
                    builder_->CreateStore(llvm::ConstantInt::get(i64Ty, 0), flagPtr);
                    auto* leaveFn = module_->getFunction("LeaveCriticalSection");
                    if (!leaveFn) {
                        auto* fty = llvm::FunctionType::get(llvm::Type::getVoidTy(*context_), {ptrTy}, false);
                        leaveFn = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, "LeaveCriticalSection", *module_);
                    }
                    builder_->CreateCall(leaveFn, {chanPtr});
                    return result;
                }
            }
        }
        // Legacy Event builtin methods (subscribe/fire/unsubscribe/count)
        // were removed in R5 step 10 together with VyxTypeKind::Event.
        // The `containerTypes_[v] = "Event"` tag is never written anymore, so
        // this branch never fires; gate it off via `if (false)` to keep the
        // emitted IR reachable by the verifier (single-entry region) without
        // reformatting the surrounding structure.
        if (false && ctIt != containerTypes_.end() && ctIt->second == "Event") {
            auto nvIt = namedValues_.find(memberExpr.object->as<IdentifierExpr>()->name);
            if (nvIt != namedValues_.end()) {
                auto* ptrTy = llvm::PointerType::getUnqual(*context_);
                auto* i64Ty = llvm::Type::getInt64Ty(*context_);
                auto* stTy = getValuePtrType(nvIt->second);

                if (memberExpr.member == "subscribe" && !call->args.empty()) {
                    auto* handlerPtr = emitExpr(*call->args[0]);
                    if (!handlerPtr) return llvm::ConstantInt::get(i64Ty, 0);
                    auto* fn = builder_->GetInsertBlock()->getParent();
                    auto* dataField = builder_->CreateStructGEP(llvm::cast<llvm::StructType>(stTy), nvIt->second, 0);
                    auto* lenField = builder_->CreateStructGEP(llvm::cast<llvm::StructType>(stTy), nvIt->second, 1);
                    auto* capField = builder_->CreateStructGEP(llvm::cast<llvm::StructType>(stTy), nvIt->second, 2);
                    auto* len = builder_->CreateLoad(i64Ty, lenField, "evt.len");
                    auto* cap = builder_->CreateLoad(i64Ty, capField, "evt.cap");
                    auto* needGrow = builder_->CreateICmpEQ(len, cap, "evt.full");
                    auto* growBB = llvm::BasicBlock::Create(*context_, "evt.grow", fn);
                    auto* pushBB = llvm::BasicBlock::Create(*context_, "evt.push", fn);
                    builder_->CreateCondBr(needGrow, growBB, pushBB);
                    builder_->SetInsertPoint(growBB);
                    auto* newCap = builder_->CreateSelect(
                        createSafeICmp(llvm::CmpInst::ICMP_EQ, cap, llvm::ConstantInt::get(cap->getType(), 0)),
                        llvm::ConstantInt::get(i64Ty, 4),
                        builder_->CreateMul(cap, llvm::ConstantInt::get(i64Ty, 2)));
                    auto* elemSize = llvm::ConstantInt::get(i64Ty, 8);
                    auto* newSize = builder_->CreateMul(newCap, elemSize);
                    auto* reallocFn = module_->getFunction("realloc");
                    if (!reallocFn) {
                        auto* rty = llvm::FunctionType::get(ptrTy, {ptrTy, i64Ty}, false);
                        reallocFn = llvm::Function::Create(rty, llvm::Function::ExternalLinkage, "realloc", *module_);
                    }
                    auto* oldData = builder_->CreateLoad(ptrTy, dataField);
                    auto* newData = builder_->CreateCall(reallocFn, {oldData, newSize});
                    builder_->CreateStore(newData, dataField);
                    builder_->CreateStore(newCap, capField);
                    builder_->CreateBr(pushBB);
                    builder_->SetInsertPoint(pushBB);
                    auto* data = builder_->CreateLoad(ptrTy, dataField);
                    auto* curLen = builder_->CreateLoad(i64Ty, lenField);
                    auto* slot = builder_->CreateGEP(ptrTy, data, curLen, "evt.slot");
                    builder_->CreateStore(handlerPtr, slot);
                    builder_->CreateStore(builder_->CreateAdd(curLen, llvm::ConstantInt::get(i64Ty, 1)), lenField);
                    return llvm::ConstantInt::get(i64Ty, 0);
                }
                if (memberExpr.member == "fire") {
                    auto* fn = builder_->GetInsertBlock()->getParent();
                    auto* dataField = builder_->CreateStructGEP(llvm::cast<llvm::StructType>(stTy), nvIt->second, 0);
                    auto* lenField = builder_->CreateStructGEP(llvm::cast<llvm::StructType>(stTy), nvIt->second, 1);
                    auto* data = builder_->CreateLoad(ptrTy, dataField);
                    auto* len = builder_->CreateLoad(i64Ty, lenField, "evt.len");

                    std::vector<llvm::Value*> fireArgs;
                    std::vector<llvm::Type*> fireArgTypes;
                    for (auto& arg : call->args) {
                        if (arg) {
                            auto* v = emitExpr(*arg);
                            if (v) { fireArgs.push_back(v); fireArgTypes.push_back(v->getType()); }
                        }
                    }
                    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(*context_), fireArgTypes, false);

                    auto* iAlloca = createEntryBlockAlloca(fn, i64Ty, "evt.i");
                    builder_->CreateStore(llvm::ConstantInt::get(i64Ty, 0), iAlloca);
                    auto* loopBB = llvm::BasicBlock::Create(*context_, "evt.loop", fn);
                    auto* bodyBB = llvm::BasicBlock::Create(*context_, "evt.body", fn);
                    auto* exitBB = llvm::BasicBlock::Create(*context_, "evt.exit", fn);
                    builder_->CreateBr(loopBB);
                    builder_->SetInsertPoint(loopBB);
                    auto* idx = builder_->CreateLoad(i64Ty, iAlloca, "evt.idx");
                    builder_->CreateCondBr(builder_->CreateICmpSLT(idx, len), bodyBB, exitBB);
                    builder_->SetInsertPoint(bodyBB);
                    auto* handlerSlot = builder_->CreateGEP(ptrTy, data, idx, "evt.handler.ptr");
                    auto* handler = builder_->CreateLoad(ptrTy, handlerSlot, "evt.handler");
                    builder_->CreateCall(fnTy, handler, fireArgs);
                    builder_->CreateStore(builder_->CreateAdd(idx, llvm::ConstantInt::get(i64Ty, 1)), iAlloca);
                    builder_->CreateBr(loopBB);
                    builder_->SetInsertPoint(exitBB);
                    return llvm::ConstantInt::get(i64Ty, 0);
                }
                if (memberExpr.member == "count") {
                    auto* lenField = builder_->CreateStructGEP(llvm::cast<llvm::StructType>(stTy), nvIt->second, 1);
                    return builder_->CreateLoad(i64Ty, lenField, "evt.count");
                }
            }
        }
    }

    // Option<T>/Result<T> methods: delegate to std enum methods, fallback to
    // built-in inline IR for __Result. Accept both Identifier receivers
    // (`o.isSome()`) and arbitrary expression receivers (`o.map().unwrap_or(0)`,
    // `produce_opt().isSome()`) — for the latter we emit the expression to a
    // temporary alloca so the rest of the dispatch can treat it uniformly with
    // the named-variable case.
    {
        llvm::AllocaInst* tempAlloca = nullptr;
        llvm::Value* optAlloca = nullptr;
        if (memberExpr.object->kind == ExprKind::Identifier) {
            auto optIt = namedValues_.find(memberExpr.object->as<IdentifierExpr>()->name);
            if (optIt != namedValues_.end()) optAlloca = optIt->second;
        } else {
            // Speculatively peek at the receiver's inferred type — only emit
            // the expression and stage a temp if it's plausibly Option/Result.
            // Avoids running emitExpr (with side effects) for unrelated calls.
            // Falls back to a member-name heuristic when Sema didn't attach
            // inferredType to the chained-call receiver (e.g. `o.map().unwrap_or(0)`
            // where the .map() return inferredType isn't always populated).
            bool plausibly = false;
            if (memberExpr.object->inferredType) {
                const auto& infTy = *memberExpr.object->inferredType;
                auto k = infTy.kind;
                plausibly = (k == VyxTypeKind::ErrorType ||
                             isOptionLike(infTy) ||
                             isResultLike(infTy));
            }
            if (!plausibly) {
                // Method-name heuristic: only the well-known Option/Result
                // intrinsics route here. If the receiver isn't actually an
                // Option/Result struct, the isResultLike check below filters
                // it out and we leave the alloca alone (the broader dispatch
                // catches the call later).
                static const std::set<std::string> kOptResultMethods = {
                    "isSome", "isNone", "is_some", "is_none",
                    "isOk", "isErr", "is_ok", "is_err",
                    "unwrap", "unwrap_or", "unwrapOr",
                    "unwrap_err",
                    "map", "and_then", "andThen",
                    "or_else", "orElse", "map_err", "filter",
                };
                if (kOptResultMethods.count(memberExpr.member))
                    plausibly = true;
            }
            if (plausibly) {
                auto* recvVal = emitExpr(*memberExpr.object);
                if (recvVal) {
                    auto* fn = builder_->GetInsertBlock()->getParent();
                    if (recvVal->getType()->isPointerTy()) {
                        // Receiver already a pointer (e.g. returned by-ref);
                        // use directly as alloca.
                        optAlloca = recvVal;
                    } else if (auto* recvSt = llvm::dyn_cast<llvm::StructType>(recvVal->getType())) {
                        tempAlloca = createEntryBlockAlloca(fn, recvSt, "opt.chain.tmp");
                        builder_->CreateStore(recvVal, tempAlloca);
                        optAlloca = tempAlloca;
                    }
                }
            }
        }
        if (optAlloca) {
            auto* allocTy = getValuePtrType(optAlloca);
            auto* resTy = llvm::dyn_cast<llvm::StructType>(allocTy);
            bool isResultLike = resTy && resTy->hasName() &&
                isOptionOrResultSlotName(resTy->getName());
            if (isResultLike && resTy->getNumElements() >= 2) {
            // Below code expects `optIt->second`; substitute via local alias.
            struct OptItShim { llvm::Value* second; };
            OptItShim optItStorage{optAlloca};
            auto* optIt = &optItStorage;
            (void)optIt;
            // Delegate to std enum method if available (Option<T>.isSome, etc.)
            std::string typeName = resTy->getName().str();
            std::string resolvedMember = memberExpr.member;
            if (!memberExpr.callTypeArgs.empty()) {
                resolvedMember = buildMangledClassName(memberExpr.member, memberExpr.callTypeArgs);
            }
            auto* methodFn = findClassMethod(typeName, resolvedMember);
            // When receiver is a shared `__Result` / per-size `__Result_N`
            // LLVM struct, Mono may have registered the method under a
            // specific `Option<T>.member` / `Result<T,E>.member` mangled
            // name. Recover the concrete class name from the object's
            // inferredType and retry.  The per-size `__Result_N` variant
            // fires whenever the widest payload (E for Result, T for
            // Option) exceeds 8 bytes and gets allocated its own struct
            // (e.g. `__Result_32` for `Result<i32, string>` because
            // sizeof(string) = 32).
            if (!methodFn &&
                (typeName == "__Result" ||
                 typeName.rfind("__Result_", 0) == 0) &&
                memberExpr.object && memberExpr.object->inferredType) {
                auto& inf = memberExpr.object->inferredType;
                std::string specific;
                // Option<T> / Result<T,E> now arrive as Class-kind with the
                // mangled name already in `inf->name`; user nominal types and
                // user ADTs (ErrorType) use the same slot. Single path.
                if (inf->kind == VyxTypeKind::Class || inf->kind == VyxTypeKind::ErrorType) {
                    specific = inf->name;
                }
                if (!specific.empty()) {
                    methodFn = findClassMethod(specific, resolvedMember);
                }
            }
            if (methodFn) {
                std::vector<llvm::Value*> argsV = {optIt->second};
                for (auto& arg : call->args) {
                    auto* v = emitExpr(*arg);
                    if (v) {
                        size_t pi = argsV.size();
                        if (pi < methodFn->getFunctionType()->getNumParams())
                            v = castToType(v, methodFn->getFunctionType()->getParamType(pi));
                        argsV.push_back(v);
                    }
                }
                if (methodFn->getReturnType()->isVoidTy()) {
                    builder_->CreateCall(methodFn, argsV);
                    return llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0);
                }
                return builder_->CreateCall(methodFn, argsV, "opt.std.result");
            }

            // Fallback: built-in inline IR for __Result (legacy type without enum methods)
            auto* tagTy = resTy->getElementType(0);
            auto* dataTy = resTy->getElementType(1);
            auto* tagPtr = builder_->CreateStructGEP(resTy, optIt->second, 0, "opt.tag.ptr");
            auto* tag = builder_->CreateLoad(tagTy, tagPtr, "opt.tag");

            auto resolveDataType = [&](bool unwrapErr = false) -> llvm::Type* {
                VyxTypePtr payload;
                if (memberExpr.object && memberExpr.object->inferredType) {
                    if (unwrapErr) {
                        payload = resultErr(*memberExpr.object->inferredType);
                    } else if (auto inner = optionInner(*memberExpr.object->inferredType)) {
                        payload = inner;
                    } else {
                        payload = resultOk(*memberExpr.object->inferredType);
                    }
                }
                if (payload) {
                    auto* payloadTy = toLLVMType(*payload);
                    if (payloadTy && !payloadTy->isVoidTy()) return payloadTy;
                }
                if (!dataTy->isArrayTy()) return dataTy;
                uint64_t sz = module_->getDataLayout().getTypeAllocSize(dataTy);
                if (sz <= 1) return llvm::Type::getInt8Ty(*context_);
                if (sz <= 4) return llvm::Type::getInt32Ty(*context_);
                return llvm::Type::getInt64Ty(*context_);
            };
            auto loadPayloadValue = [&](llvm::Value* valPtr,
                                        llvm::Type* loadTy,
                                        llvm::StringRef tmpName) -> llvm::Value* {
                if (dataTy->isArrayTy() && loadTy && loadTy->isStructTy()) {
                    auto* innerStTy = llvm::cast<llvm::StructType>(loadTy);
                    auto* tmp = createEntryBlockAlloca(
                        builder_->GetInsertBlock()->getParent(), innerStTy,
                        tmpName.str());
                    auto* i8Ty = llvm::Type::getInt8Ty(*context_);
                    auto* i64Ty = llvm::Type::getInt64Ty(*context_);
                    uint64_t payloadSz = dataTy->getArrayNumElements();
                    uint64_t compactOffset = 0;
                    for (unsigned fi = 0; fi < innerStTy->getNumElements(); ++fi) {
                        auto* fieldTy = innerStTy->getElementType(fi);
                        uint64_t fieldSz = module_->getDataLayout().getTypeStoreSize(fieldTy);
                        if (compactOffset + fieldSz > payloadSz) {
                            diag_.error(memberExpr.location,
                                "unwrap(): payload struct is too large for destination slot");
                            hadHardTypeError_ = true;
                            return nullptr;
                        }
                        auto* bytePtr = builder_->CreateGEP(
                            i8Ty, valPtr,
                            llvm::ConstantInt::get(i64Ty, compactOffset),
                            "unwrap.field.byte.ptr");
                        auto* fieldVal = builder_->CreateLoad(fieldTy, bytePtr, "unwrap.field.val");
                        auto* fieldPtr = builder_->CreateStructGEP(
                            innerStTy, tmp, fi, "unwrap.field.dst.ptr");
                        builder_->CreateStore(fieldVal, fieldPtr);
                        compactOffset += fieldSz;
                    }
                    return builder_->CreateLoad(innerStTy, tmp, tmpName);
                }
                return builder_->CreateLoad(loadTy, valPtr, tmpName);
            };

            if (memberExpr.member == "isSome" || memberExpr.member == "isOk" ||
                memberExpr.member == "is_some" || memberExpr.member == "is_ok") {
                return builder_->CreateICmpEQ(tag, llvm::ConstantInt::get(tagTy, 0), "opt.isSome");
            }
            if (memberExpr.member == "isNone" || memberExpr.member == "isErr" ||
                memberExpr.member == "is_none" || memberExpr.member == "is_err") {
                return builder_->CreateICmpEQ(tag, llvm::ConstantInt::get(tagTy, 1), "opt.isNone");
            }
            if (memberExpr.member == "unwrap") {
                auto* fn = builder_->GetInsertBlock()->getParent();
                auto* isNone = builder_->CreateICmpEQ(tag, llvm::ConstantInt::get(tagTy, 1), "opt.isNone");
                auto* panicBB = llvm::BasicBlock::Create(*context_, "unwrap.panic", fn);
                auto* okBB = llvm::BasicBlock::Create(*context_, "unwrap.ok", fn);
                builder_->CreateCondBr(isNone, panicBB, okBB);
                builder_->SetInsertPoint(panicBB);
                emitPanicCall(getOrCreateString("PANIC: unwrap() called on None"));
                builder_->SetInsertPoint(okBB);
                auto* valPtr = builder_->CreateStructGEP(resTy, optIt->second, 1, "unwrap.val.ptr");
                auto* loadTy = resolveDataType();
                auto* unwrapped = loadPayloadValue(valPtr, loadTy, "unwrap.val");
                if (!unwrapped) return nullptr;
                return unwrapped;
            }
            if ((memberExpr.member == "unwrapOr" || memberExpr.member == "unwrap_or") && !call->args.empty()) {
                auto* defaultVal = emitExpr(*call->args[0]);
                if (!defaultVal) return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), 0);
                auto* fn = builder_->GetInsertBlock()->getParent();
                auto* loadTy = resolveDataType();
                auto* isSome = builder_->CreateICmpEQ(tag, llvm::ConstantInt::get(tagTy, 0), "opt.isSome");
                auto* someBB = llvm::BasicBlock::Create(*context_, "unwrapOr.some", fn);
                auto* noneBB = llvm::BasicBlock::Create(*context_, "unwrapOr.none", fn);
                auto* mergeBB = llvm::BasicBlock::Create(*context_, "unwrapOr.merge", fn);
                builder_->CreateCondBr(isSome, someBB, noneBB);
                builder_->SetInsertPoint(someBB);
                auto* valPtr = builder_->CreateStructGEP(resTy, optIt->second, 1, "unwrapOr.val.ptr");
                auto* someVal = loadPayloadValue(valPtr, loadTy, "unwrapOr.some.val");
                if (!someVal) return nullptr;
                auto* someCast = castToType(someVal, defaultVal->getType());
                builder_->CreateBr(mergeBB);
                auto* someBBEnd = builder_->GetInsertBlock();
                builder_->SetInsertPoint(noneBB);
                builder_->CreateBr(mergeBB);
                builder_->SetInsertPoint(mergeBB);
                auto* phi = builder_->CreatePHI(defaultVal->getType(), 2, "unwrapOr.result");
                phi->addIncoming(someCast, someBBEnd);
                phi->addIncoming(defaultVal, noneBB);
                return phi;
            }
            }
        }
    }

    // Built-in string methods (on identifiers or any expression returning __String)
    {
        llvm::Value* strAlloca = nullptr;
        auto* strTy = getOrCreateStringType();
        if (memberExpr.object->kind == ExprKind::Identifier) {
            auto strIt = namedValues_.find(memberExpr.object->as<IdentifierExpr>()->name);
            if (strIt != namedValues_.end() && getValuePtrType(strIt->second) == strTy)
                strAlloca = strIt->second;
        }
        if (!strAlloca) {
            auto* objVal = emitExpr(*memberExpr.object);
            if (objVal && objVal->getType() == strTy) {
                auto* fn = builder_->GetInsertBlock()->getParent();
                strAlloca = createEntryBlockAlloca(fn, strTy, "str.tmp");
                builder_->CreateStore(objVal, strAlloca);
            }
        }
        auto it = namedValues_.end();
        if (strAlloca) {
            namedValues_["__str_method_tmp"] = strAlloca;
            it = namedValues_.find("__str_method_tmp");
        }
        if (it != namedValues_.end() && getValuePtrType(it->second) == strTy) {
            if (memberExpr.member == "len") {
                auto* lenPtr = builder_->CreateStructGEP(strTy, it->second, 1, "str.len.ptr");
                return builder_->CreateLoad(llvm::Type::getInt64Ty(*context_), lenPtr, "str.len");
            }
            if (memberExpr.member == "c_str" || memberExpr.member == "ptr") {
                auto* ptrField = builder_->CreateStructGEP(strTy, it->second, 0, "str.ptr.field");
                return builder_->CreateLoad(llvm::PointerType::getUnqual(*context_), ptrField, "str.cstr");
            }
            if ((memberExpr.member == "charAt" || memberExpr.member == "char_at") && !call->args.empty()) {
                auto* idx = emitExpr(*call->args[0]);
                auto* ptrField = builder_->CreateStructGEP(strTy, it->second, 0);
                auto* rawPtr = builder_->CreateLoad(llvm::PointerType::getUnqual(*context_), ptrField);
                auto* charPtr = builder_->CreateGEP(llvm::Type::getInt8Ty(*context_), rawPtr, idx, "str.char.ptr");
                auto* ch = builder_->CreateLoad(llvm::Type::getInt8Ty(*context_), charPtr, "str.char");
                return builder_->CreateZExt(ch, llvm::Type::getInt32Ty(*context_));
            }
            if (memberExpr.member == "isEmpty") {
                auto* lenPtr = builder_->CreateStructGEP(strTy, it->second, 1);
                auto* len = builder_->CreateLoad(llvm::Type::getInt64Ty(*context_), lenPtr);
                auto* isZero = createSafeICmp(llvm::CmpInst::ICMP_EQ, len,
                    llvm::ConstantInt::get(len->getType(), 0));
                return builder_->CreateZExt(isZero, llvm::Type::getInt32Ty(*context_));
            }

            // [fat-std] Dispatch string methods to explicitly imported std free functions.
            {
                static const std::map<std::string, std::string> strMethodMap = {
                    {"substring", "str_substring"}, {"substr", "str_substr"},
                    {"contains", "str_contains"},
                    {"indexOf", "str_index_of"}, {"startsWith", "str_starts_with"},
                    {"endsWith", "str_ends_with"}, {"toUpper", "str_to_upper"},
                    {"toLower", "str_to_lower"}, {"trim", "str_trim"},
                    {"trimLeft", "str_trim_left"}, {"trimRight", "str_trim_right"},
                    {"trim_left", "str_trim_left"}, {"trim_right", "str_trim_right"},
                    {"replace", "str_replace"}, {"repeat", "str_repeat"},
                    {"starts_with", "str_starts_with"}, {"ends_with", "str_ends_with"},
                    {"index_of", "str_index_of"}, {"to_upper", "str_to_upper"},
                    {"to_lower", "str_to_lower"},
                    {"charAt", "str_char_at"}, {"char_at", "str_char_at"},
                    {"split", "str_split"}, {"is_empty", "str_is_empty"},
                    {"to_int", "str_to_int"}, {"to_float", "str_to_float"},
                };
                auto mIt = strMethodMap.find(memberExpr.member);
                if (mIt != strMethodMap.end()) {
                    auto fnIt = functions_.find(mIt->second);
                    if (fnIt != functions_.end()) {
                        auto* strVal = builder_->CreateLoad(strTy, it->second, "str.val");
                        std::vector<llvm::Value*> argsV = {strVal};
                        for (auto& arg : call->args) {
                            if (arg) {
                                auto* v = emitExpr(*arg);
                                if (v) {
                                    size_t paramIdx = argsV.size();
                                    if (paramIdx < fnIt->second->getFunctionType()->getNumParams())
                                        v = castToType(v, fnIt->second->getFunctionType()->getParamType(paramIdx));
                                    argsV.push_back(v);
                                }
                            }
                        }
                        if (fnIt->second->getReturnType()->isVoidTy()) {
                            builder_->CreateCall(fnIt->second, argsV);
                            return llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0);
                        }
                        return builder_->CreateCall(fnIt->second, argsV, "str.method.result");
                    }
                }
            }
        }
    }

    if (memberExpr.object && memberExpr.object->kind == ExprKind::Identifier) {
        auto refIt = namedValues_.find(memberExpr.object->as<IdentifierExpr>()->name);
        if (refIt != namedValues_.end()) {
            auto* allocTy = getValuePtrType(refIt->second);
            auto rcIt = structTypes_.find("__RefCounted");
            if (rcIt != structTypes_.end() && allocTy == rcIt->second) {
                auto* refTy = rcIt->second;
                auto* i64Ty = llvm::Type::getInt64Ty(*context_);
                auto* ptrTy = llvm::PointerType::getUnqual(*context_);

                if (memberExpr.member == "deref") {
                    auto* dataPtr = builder_->CreateStructGEP(refTy, refIt->second, 0);
                    auto* data = builder_->CreateLoad(ptrTy, dataPtr, "ref.data");
                    // Prefer the tracked inner type so `Ref<Payload>.deref()`
                    // returns the Payload struct, not a truncated i32.
                    // Without this, `r.deref().val` dropped through every
                    // member-access path and returned 0.
                    auto innerIt = refInnerTypeNames_.find(
                        memberExpr.object->as<IdentifierExpr>()->name);
                    if (innerIt != refInnerTypeNames_.end()) {
                        auto stIt = structTypes_.find(innerIt->second);
                        if (stIt != structTypes_.end()) {
                            return builder_->CreateLoad(stIt->second, data, "ref.deref");
                        }
                        const std::string& inm = innerIt->second;
                        if (inm == "i64" || inm == "u64")
                            return builder_->CreateLoad(i64Ty, data, "ref.deref");
                        if (inm == "i32" || inm == "u32")
                            return builder_->CreateLoad(llvm::Type::getInt32Ty(*context_), data, "ref.deref");
                        if (inm == "i8" || inm == "u8")
                            return builder_->CreateLoad(llvm::Type::getInt8Ty(*context_), data, "ref.deref");
                    }
                    auto* val = builder_->CreateLoad(i64Ty, data, "ref.deref");
                    return builder_->CreateTrunc(val, llvm::Type::getInt32Ty(*context_), "ref.val");
                }
                if (memberExpr.member == "retain") {
                    auto* rcPtr = builder_->CreateStructGEP(refTy, refIt->second, 1);
                    auto* rc = builder_->CreateLoad(i64Ty, rcPtr, "ref.rc");
                    auto* newRc = builder_->CreateAdd(rc, llvm::ConstantInt::get(i64Ty, 1));
                    builder_->CreateStore(newRc, rcPtr);
                    return newRc;
                }
                if (memberExpr.member == "release") {
                    auto* fn = builder_->GetInsertBlock()->getParent();
                    auto* rcPtr = builder_->CreateStructGEP(refTy, refIt->second, 1);
                    auto* rc = builder_->CreateLoad(i64Ty, rcPtr, "ref.rc");
                    auto* newRc = builder_->CreateSub(rc, llvm::ConstantInt::get(i64Ty, 1));
                    builder_->CreateStore(newRc, rcPtr);
                    auto* isZero = createSafeICmp(llvm::CmpInst::ICMP_EQ, newRc,
                        llvm::ConstantInt::get(newRc->getType(), 0));
                    auto* freeBB = llvm::BasicBlock::Create(*context_, "ref.free", fn);
                    auto* contBB = llvm::BasicBlock::Create(*context_, "ref.cont", fn);
                    builder_->CreateCondBr(isZero, freeBB, contBB);
                    builder_->SetInsertPoint(freeBB);
                    auto* dataPtr = builder_->CreateStructGEP(refTy, refIt->second, 0);
                    auto* data = builder_->CreateLoad(ptrTy, dataPtr);
                    auto* freeFn = module_->getFunction("free");
                    if (freeFn) builder_->CreateCall(freeFn, {data});
                    builder_->CreateBr(contBB);
                    builder_->SetInsertPoint(contBB);
                    return newRc;
                }
                if (memberExpr.member == "refCount") {
                    auto* rcPtr = builder_->CreateStructGEP(refTy, refIt->second, 1);
                    auto* rc = builder_->CreateLoad(i64Ty, rcPtr, "ref.rc");
                    return builder_->CreateTrunc(rc, llvm::Type::getInt32Ty(*context_));
                }

                // P4-D.2: First try to dispatch to a native Ref<T> method
                // (e.g. `Ref<i64>.count`, `Ref<i64>.clone`, `Ref<i64>.drop`,
                // `Ref<i64>.weak_count`).  Only fall through to auto-deref
                // when no such method exists — otherwise we'd misroute
                // `r.count()` (which needs the rc header) into the inner
                // `i64.count` (which doesn't exist, then crashes via a
                // fallback that treats `self = data` and reads past the
                // 8-byte data allocation).
                std::string innerTypeName = resolveRefInner(*memberExpr.object,
                    memberExpr.object->as<IdentifierExpr>()->name);
                {
                    llvm::Function* refMethod = nullptr;
                    if (!innerTypeName.empty()) {
                        auto it = functions_.find("Ref<" + innerTypeName + ">." + memberExpr.member);
                        if (it != functions_.end()) refMethod = it->second;
                    }
                    // Fallback: the tracked inner type may have been lost
                    // through a match-binding (`case Some(u) => u.count()`)
                    // or a cross-scope rebinding.  Accept any Ref<*>.method
                    // match — all share the `__RefCounted` self layout, so
                    // the method body is safe to invoke with the struct ptr.
                    if (!refMethod) {
                        std::string prefix = "Ref<";
                        std::string suffix = ">." + memberExpr.member;
                        for (auto& [fn_name, fn_ptr] : functions_) {
                            if (fn_name.size() > prefix.size() + suffix.size() &&
                                fn_name.compare(0, prefix.size(), prefix) == 0 &&
                                fn_name.size() >= suffix.size() &&
                                fn_name.compare(fn_name.size() - suffix.size(), suffix.size(), suffix) == 0) {
                                refMethod = fn_ptr;
                                break;
                            }
                        }
                    }
                    if (refMethod) {
                        auto* method = refMethod;
                        std::vector<llvm::Value*> argsV = {refIt->second};
                        for (size_t ai = 0; ai < call->args.size(); ++ai) {
                            auto* argVal = emitExpr(*call->args[ai]);
                            if (!argVal) continue;
                            size_t paramIdx = ai + 1;
                            if (paramIdx < method->getFunctionType()->getNumParams()) {
                                auto* expectedTy = method->getFunctionType()->getParamType(paramIdx);
                                if (argVal->getType() != expectedTy)
                                    argVal = castToType(argVal, expectedTy);
                            }
                            argsV.push_back(argVal);
                        }
                        return builder_->CreateCall(method, argsV, "ref.method.call");
                    }
                }

                // Auto-deref: Ref<T>.method() → load inner T, call T.method()
                // Fallback: search for .method() across all class types
                llvm::Function* foundMethod = nullptr;
                if (!innerTypeName.empty()) {
                    auto mIt = functions_.find(innerTypeName + "." + memberExpr.member);
                    if (mIt != functions_.end()) foundMethod = mIt->second;
                }
                if (!foundMethod) {
                    std::string suffix = "." + memberExpr.member;
                    for (auto& [fn_name, fn_ptr] : functions_) {
                        if (fn_name.size() > suffix.size() &&
                            fn_name.compare(fn_name.size() - suffix.size(), suffix.size(), suffix) == 0) {
                            auto dotPos = fn_name.rfind('.');
                            if (dotPos != std::string::npos) {
                                std::string candidateClass = fn_name.substr(0, dotPos);
                                if (structTypes_.count(candidateClass) &&
                                    candidateClass != "__RefCounted" &&
                                    !isInternalStructType(candidateClass)) {
                                    innerTypeName = candidateClass;
                                    foundMethod = fn_ptr;
                                    break;
                                }
                            }
                        }
                    }
                }
                if (foundMethod) {
                    {
                        auto* method = foundMethod;
                        auto* dataPtr = builder_->CreateStructGEP(refTy, refIt->second, 0);
                        auto* data = builder_->CreateLoad(ptrTy, dataPtr, "ref.auto.data");

                        // self ptr = data pointer (points to malloc'd T)
                        std::vector<llvm::Value*> argsV = {data};

                        for (size_t ai = 0; ai < call->args.size(); ++ai) {
                            auto* argVal = emitExpr(*call->args[ai]);
                            if (!argVal) continue;

                            // Auto-deref Ref<T> arguments to get inner T value
                            auto rcIt2 = structTypes_.find("__RefCounted");
                            if (rcIt2 != structTypes_.end() && argVal->getType() == rcIt2->second) {
                                auto* fn2 = builder_->GetInsertBlock()->getParent();
                                auto* argAlloca = createEntryBlockAlloca(fn2, rcIt2->second, "ref.arg.tmp");
                                builder_->CreateStore(argVal, argAlloca);
                                auto* argDataPtr = builder_->CreateStructGEP(rcIt2->second, argAlloca, 0);
                                auto* argData = builder_->CreateLoad(ptrTy, argDataPtr, "ref.arg.data");

                                size_t paramIdx = ai + 1;
                                if (paramIdx < method->getFunctionType()->getNumParams()) {
                                    auto* expectedTy = method->getFunctionType()->getParamType(paramIdx);
                                    if (expectedTy->isPointerTy()) {
                                        argsV.push_back(argData);
                                    } else {
                                        auto* loaded = builder_->CreateLoad(expectedTy, argData, "ref.arg.val");
                                        argsV.push_back(loaded);
                                    }
                                } else {
                                    argsV.push_back(argData);
                                }
                            } else {
                                size_t paramIdx = ai + 1;
                                if (paramIdx < method->getFunctionType()->getNumParams())
                                    argVal = castToType(argVal, method->getFunctionType()->getParamType(paramIdx));
                                argsV.push_back(argVal);
                            }
                        }

                        if (method->getReturnType()->isVoidTy()) {
                            builder_->CreateCall(method, argsV);
                            return llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0);
                        }
                        return builder_->CreateCall(method, argsV, "ref.deref.call");
                    }
                }
            }
        }
    }

    // has_field(fieldName) — compile-time reflection: check if field exists
    if (memberExpr.member == "has_field" && !call->args.empty() && memberExpr.object &&
        memberExpr.object->kind == ExprKind::Identifier) {
        auto objIt = namedValues_.find(memberExpr.object->as<IdentifierExpr>()->name);
        if (objIt != namedValues_.end()) {
            auto* allocaTy = getValuePtrType(objIt->second);
            if (auto* stTy = llvm::dyn_cast<llvm::StructType>(allocaTy)) {
                std::string fieldName;
                if (call->args[0]->kind == ExprKind::StringLiteral)
                    fieldName = call->args[0]->as<StringLiteralExpr>()->value;
                if (!fieldName.empty()) {
                    std::string typeName = stTy->hasName() ? stTy->getName().str() : "";
                    if (typeName.empty())
                        typeName = resolveClassName(*memberExpr.object,
                            memberExpr.object->as<IdentifierExpr>()->name);
                    auto fnIt = structFieldNames_.find(typeName);
                    if (fnIt != structFieldNames_.end()) {
                        for (auto& f : fnIt->second) {
                            if (f == fieldName)
                                return llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context_), 1);
                        }
                    }
                }
                return llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context_), 0);
            }
        }
    }

    // field_count() — compile-time reflection: get number of fields
    if (memberExpr.member == "field_count" && memberExpr.object &&
        memberExpr.object->kind == ExprKind::Identifier) {
        auto objIt = namedValues_.find(memberExpr.object->as<IdentifierExpr>()->name);
        if (objIt != namedValues_.end()) {
            auto* allocaTy = getValuePtrType(objIt->second);
            if (auto* stTy = llvm::dyn_cast<llvm::StructType>(allocaTy)) {
                std::string typeName = stTy->hasName() ? stTy->getName().str() : "";
                if (typeName.empty())
                    typeName = resolveClassName(*memberExpr.object,
                        memberExpr.object->as<IdentifierExpr>()->name);
                auto fnIt = structFieldNames_.find(typeName);
                int count = fnIt != structFieldNames_.end() ? (int)fnIt->second.size() : 0;
                return llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), count);
            }
        }
    }

    // field_name(index) — compile-time reflection: get field name by index
    if (memberExpr.member == "field_name" && !call->args.empty() && memberExpr.object &&
        memberExpr.object->kind == ExprKind::Identifier) {
        auto objIt = namedValues_.find(memberExpr.object->as<IdentifierExpr>()->name);
        if (objIt != namedValues_.end()) {
            auto* allocaTy = getValuePtrType(objIt->second);
            if (auto* stTy = llvm::dyn_cast<llvm::StructType>(allocaTy)) {
                int idx = 0;
                if (call->args[0]->kind == ExprKind::IntLiteral)
                    idx = static_cast<int>(call->args[0]->as<IntLiteralExpr>()->value);
                std::string typeName = stTy->hasName() ? stTy->getName().str() : "";
                if (typeName.empty())
                    typeName = resolveClassName(*memberExpr.object,
                        memberExpr.object->as<IdentifierExpr>()->name);
                auto fnIt = structFieldNames_.find(typeName);
                if (fnIt != structFieldNames_.end() && idx >= 0 && idx < (int)fnIt->second.size()) {
                    return createStringValue(fnIt->second[idx]);
                }
                diag_.error(memberExpr.location,
                    "field_name: index {} is out of range for type `{}` ({} fields)",
                    idx, typeName, fnIt != structFieldNames_.end() ? (int)fnIt->second.size() : 0);
                return nullptr;
            }
        }
    }

    // type_name() — compile-time reflection: get type name
    if (memberExpr.member == "type_name" && memberExpr.object &&
        memberExpr.object->kind == ExprKind::Identifier) {
        auto objIt = namedValues_.find(memberExpr.object->as<IdentifierExpr>()->name);
        if (objIt != namedValues_.end()) {
            auto* allocaTy = getValuePtrType(objIt->second);
            std::string typeName;
            if (auto* stTy = llvm::dyn_cast<llvm::StructType>(allocaTy)) {
                typeName = stTy->hasName() ? stTy->getName().str() : "struct";
            } else {
                typeName = resolveClassName(*memberExpr.object,
                    memberExpr.object->as<IdentifierExpr>()->name);
                if (typeName.empty()) {
                    diag_.error(memberExpr.location, "type_name(): could not resolve LLVM type name for this value");
                    return nullptr;
                }
            }
            return createStringValue(typeName);
        }
    }

    // get_field(fieldName) — compile-time reflection: access field by name string
    if (memberExpr.member == "get_field" && !call->args.empty() && memberExpr.object &&
        memberExpr.object->kind == ExprKind::Identifier) {
        auto objIt = namedValues_.find(memberExpr.object->as<IdentifierExpr>()->name);
        if (objIt != namedValues_.end()) {
            auto* allocaTy = getValuePtrType(objIt->second);
            if (auto* stTy = llvm::dyn_cast<llvm::StructType>(allocaTy)) {
                std::string fieldName;
                if (call->args[0]->kind == ExprKind::StringLiteral) {
                    fieldName = call->args[0]->as<StringLiteralExpr>()->value;
                } else if (call->args[0]->kind == ExprKind::Identifier) {
                    fieldName = call->args[0]->as<IdentifierExpr>()->name;
                }
                if (!fieldName.empty()) {
                    std::string typeName = stTy->hasName() ? stTy->getName().str() : "";
                    if (typeName.empty())
                        typeName = resolveClassName(*memberExpr.object,
                            memberExpr.object->as<IdentifierExpr>()->name);
                    auto fnIt = structFieldNames_.find(typeName);
                    if (fnIt != structFieldNames_.end()) {
                        auto& fields = fnIt->second;
                        for (size_t i = 0; i < fields.size(); ++i) {
                            if (fields[i] == fieldName) {
                                auto* gep = builder_->CreateStructGEP(stTy, objIt->second, i, fieldName);
                                auto* val = builder_->CreateLoad(stTy->getElementType(i), gep, fieldName + ".val");
                                if (stTy->getElementType(i)->isIntegerTy()) {
                                    auto* strTy = getOrCreateStringType();
                                    auto* buf = builder_->CreateCall(module_->getFunction("malloc"),
                                        {llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), 32)}, "itoa.buf");
                                    auto* snprintfFn = module_->getFunction("snprintf");
                                    if (!snprintfFn) {
                                        auto* ptrTy = llvm::PointerType::getUnqual(*context_);
                                        auto* i64Ty = llvm::Type::getInt64Ty(*context_);
                                        auto* sty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context_), {ptrTy, i64Ty, ptrTy}, true);
                                        snprintfFn = llvm::Function::Create(sty, llvm::Function::ExternalLinkage, "snprintf", *module_);
                                    }
                                    builder_->CreateCall(snprintfFn, {buf,
                                        llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), 32),
                                        getOrCreateString("%d"), val});
                                    auto* fn2 = builder_->GetInsertBlock()->getParent();
                                    auto* resAlloca = createEntryBlockAlloca(fn2, strTy, "field.str");
                                    builder_->CreateStore(buf, builder_->CreateStructGEP(strTy, resAlloca, 0));
                                    auto* strlenFn = module_->getFunction("strlen");
                                    if (!strlenFn) {
                                        auto* ptrTy = llvm::PointerType::getUnqual(*context_);
                                        auto* slty = llvm::FunctionType::get(llvm::Type::getInt64Ty(*context_), {ptrTy}, false);
                                        strlenFn = llvm::Function::Create(slty, llvm::Function::ExternalLinkage, "strlen", *module_);
                                    }
                                    auto* len = builder_->CreateCall(strlenFn, {buf}, "field.len");
                                    builder_->CreateStore(len, builder_->CreateStructGEP(strTy, resAlloca, 1));
                                    return builder_->CreateLoad(strTy, resAlloca, "field.str.val");
                                }
                                return val;
                            }
                        }
                    }
                }
            }
        }
    }

    if (memberExpr.member == "into_ref" && memberExpr.object) {
        diag_.error(memberExpr.location, "into_ref() is not supported in codegen; use Ref<T> / makeRef and explicit casts");
        return nullptr;
    }

    if (memberExpr.member == "toString" && memberExpr.object) {
        auto* val = emitExpr(*memberExpr.object);
        if (val) {
            if (val->getType()->isStructTy()) {
                auto* stTy = llvm::cast<llvm::StructType>(val->getType());
                if (stTy->hasName()) {
                    std::string debugName = stTy->getName().str() + ".toString";
                    auto fnIt = functions_.find(debugName);
                    if (fnIt != functions_.end()) {
                        auto* fnCur = builder_->GetInsertBlock()->getParent();
                        auto* tmp = createEntryBlockAlloca(fnCur, stTy, "tostr.struct.tmp");
                        builder_->CreateStore(val, tmp);
                        return builder_->CreateCall(fnIt->second, {tmp}, "tostr.struct");
                    }
                }
            }

            auto* fnCur = builder_->GetInsertBlock()->getParent();
            auto* i64Ty = llvm::Type::getInt64Ty(*context_);
            auto* ptrTy = llvm::PointerType::getUnqual(*context_);
            auto* strTy2 = getOrCreateStringType();

            auto* mallocFn = module_->getFunction("malloc");
            if (!mallocFn) {
                auto* mty = llvm::FunctionType::get(ptrTy, {i64Ty}, false);
                mallocFn = llvm::Function::Create(mty, llvm::Function::ExternalLinkage, "malloc", *module_);
            }
            auto* snprintfFn = module_->getFunction("snprintf");
            if (!snprintfFn) {
                auto* sty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context_), {ptrTy, i64Ty, ptrTy}, true);
                snprintfFn = llvm::Function::Create(sty, llvm::Function::ExternalLinkage, "snprintf", *module_);
            }
            auto* toCharsFn = getOrCreateI64ToCharsFunction();
            auto* buf = builder_->CreateCall(mallocFn, {llvm::ConstantInt::get(i64Ty, 32)}, "tostr.buf");
            llvm::Value* len = nullptr;
            if (val->getType()->isFloatingPointTy()) {
                auto* dval = val;
                if (val->getType()->isFloatTy()) dval = builder_->CreateFPExt(val, llvm::Type::getDoubleTy(*context_));
                auto* fmt = getOrCreateString("%g");
                auto* wrote = builder_->CreateCall(snprintfFn, {buf, llvm::ConstantInt::get(i64Ty, 32), fmt, dval}, "tostr.n");
                len = builder_->CreateSExt(wrote, i64Ty, "tostr.len");
            } else {
                auto* ival = val;
                if (!val->getType()->isIntegerTy(64))
                    ival = builder_->CreateIntCast(val, i64Ty, true);
                len = builder_->CreateCall(toCharsFn, {buf, ival}, "tostr.len");
            }
            auto* resAlloca = createEntryBlockAlloca(fnCur, strTy2, "tostr.res");
            builder_->CreateStore(buf, builder_->CreateStructGEP(strTy2, resAlloca, 0));
            builder_->CreateStore(len, builder_->CreateStructGEP(strTy2, resAlloca, 1));
            builder_->CreateStore(llvm::ConstantInt::get(i64Ty, 32), builder_->CreateStructGEP(strTy2, resAlloca, 2));
            builder_->CreateStore(llvm::ConstantInt::get(i64Ty, 1), builder_->CreateStructGEP(strTy2, resAlloca, 3));
            return builder_->CreateLoad(strTy2, resAlloca, "tostr.val");
        }
    }

    std::string objName;
    llvm::Value* objAlloca = nullptr;
    std::string synthIfaceName;  // filled when object is a call/expr returning __iface_X

    if (memberExpr.object->kind == ExprKind::Identifier) {
        objName = memberExpr.object->as<IdentifierExpr>()->name;
        auto it = namedValues_.find(objName);
        if (it != namedValues_.end()) objAlloca = it->second;
        else if (auto* gv = module_->getGlobalVariable(objName, true))
            objAlloca = gv;
    } else if (memberExpr.object->kind == ExprKind::Call ||
               memberExpr.object->kind == ExprKind::Index) {
        // `shapes.get(0).area()` / `lookup().area()`: the object is a
        // call/index expression whose return value may be an interface
        // struct `__iface_X { data_ptr, vtable_ptr }`. Evaluate, spill
        // to a temp alloca, then let the iface-dispatch path below
        // treat it like a regular interface-typed variable.
        auto* objVal = emitExpr(*memberExpr.object);
        if (objVal) {
            if (auto* stTy = llvm::dyn_cast<llvm::StructType>(objVal->getType())) {
                if (stTy->hasName() && stTy->getName().starts_with("__iface_")) {
                    auto* fn = builder_->GetInsertBlock()->getParent();
                    auto* tmp = createEntryBlockAlloca(fn, stTy, "iface.tmp");
                    builder_->CreateStore(objVal, tmp);
                    objAlloca = tmp;
                    synthIfaceName = stTy->getName().str().substr(8); // strip "__iface_"
                }
            }
        }
    }

    // Handle MemberAccess/SelfExpr objects that evaluate to __RefCounted
    // e.g. self.r.method() where r is Ref<T>
    if (!objAlloca && memberExpr.object->kind == ExprKind::MemberAccess) {
        auto* memberAddr = getMemberAddress(*memberExpr.object);
        if (memberAddr) {
            auto* fieldTy = getValuePtrType(memberAddr);
            auto rcIt3 = structTypes_.find("__RefCounted");
            if (rcIt3 != structTypes_.end() && fieldTy == rcIt3->second) {
                auto* refTy = rcIt3->second;
                auto* ptrTy4 = llvm::PointerType::getUnqual(*context_);

                llvm::Function* derefMethod2 = nullptr;
                std::string suffix = "." + memberExpr.member;
                for (auto& [fn_name, fn_ptr] : functions_) {
                    if (fn_name.size() > suffix.size() &&
                        fn_name.compare(fn_name.size() - suffix.size(), suffix.size(), suffix) == 0) {
                        auto dotPos = fn_name.rfind('.');
                        if (dotPos != std::string::npos) {
                            std::string candidateClass = fn_name.substr(0, dotPos);
                            if (structTypes_.count(candidateClass) &&
                                candidateClass != "__RefCounted" &&
                                !isInternalStructType(candidateClass)) {
                                derefMethod2 = fn_ptr;
                                break;
                            }
                        }
                    }
                }
                if (derefMethod2) {
                    auto* dataPtr = builder_->CreateStructGEP(refTy, memberAddr, 0);
                    auto* data = builder_->CreateLoad(ptrTy4, dataPtr, "ref.field.data");
                    std::vector<llvm::Value*> argsV = {data};

                    for (size_t ai = 0; ai < call->args.size(); ++ai) {
                        auto* argVal = emitExpr(*call->args[ai]);
                        if (!argVal) continue;
                        if (argVal->getType() == rcIt3->second) {
                            auto* fn4 = builder_->GetInsertBlock()->getParent();
                            auto* argAlloca = createEntryBlockAlloca(fn4, rcIt3->second, "ref.farg.tmp");
                            builder_->CreateStore(argVal, argAlloca);
                            auto* argDataPtr = builder_->CreateStructGEP(rcIt3->second, argAlloca, 0);
                            auto* argData = builder_->CreateLoad(ptrTy4, argDataPtr, "ref.farg.data");
                            size_t pi2 = ai + 1;
                            if (pi2 < derefMethod2->getFunctionType()->getNumParams()) {
                                auto* expectedTy = derefMethod2->getFunctionType()->getParamType(pi2);
                                if (expectedTy->isPointerTy()) argsV.push_back(argData);
                                else argsV.push_back(builder_->CreateLoad(expectedTy, argData, "ref.farg.val"));
                            } else {
                                argsV.push_back(argData);
                            }
                        } else {
                            size_t pi2 = ai + 1;
                            if (pi2 < derefMethod2->getFunctionType()->getNumParams())
                                argVal = castToType(argVal, derefMethod2->getFunctionType()->getParamType(pi2));
                            argsV.push_back(argVal);
                        }
                    }
                    if (derefMethod2->getReturnType()->isVoidTy()) {
                        builder_->CreateCall(derefMethod2, argsV);
                        return llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0);
                    }
                    return builder_->CreateCall(derefMethod2, argsV, "ref.field.call");
                }
            }
            objAlloca = memberAddr;
        }
    }

    // ADT variant construction: EnumName.Variant(args) using dot syntax
    if (!objAlloca && !objName.empty()) {
        auto stIt = structTypes_.find(objName);
        auto evIt = errorEnumValues_.find(objName + "." + memberExpr.member);
        if (stIt != structTypes_.end() && evIt != errorEnumValues_.end()) {
            auto* adtTy = stIt->second;
            auto* fn = builder_->GetInsertBlock()->getParent();
            auto* alloca = createEntryBlockAlloca(fn, adtTy, "adt.tmp");
            auto* tagPtr = builder_->CreateStructGEP(adtTy, alloca, 0, "adt.tag");
            builder_->CreateStore(
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), evIt->second), tagPtr);
            if (!call->args.empty()) {
                auto* dataPtr = builder_->CreateStructGEP(adtTy, alloca, 1, "adt.data");
                uint64_t offset = 0;
                for (size_t i = 0; i < call->args.size(); ++i) {
                    if (!call->args[i]) continue;
                    auto* argVal = emitExpr(*call->args[i]);
                    if (!argVal) continue;
                    auto* destPtr = builder_->CreateGEP(
                        llvm::Type::getInt8Ty(*context_), dataPtr,
                        llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), offset), "adt.field");
                    builder_->CreateStore(argVal, destPtr);
                    offset += module_->getDataLayout().getTypeAllocSize(argVal->getType());
                }
            }
            return builder_->CreateLoad(adtTy, alloca, "adt.val");
        }
    }

    // Static method call: ClassName.method() - no instance needed
    if (!objAlloca && !objName.empty()) {
        std::string resolvedObjName = objName;
        if (!currentClassName_.empty() && currentClassName_.find('<') != std::string::npos) {
            auto ltPos = currentClassName_.find('<');
            if (ltPos != std::string::npos && currentClassName_.substr(0, ltPos) == objName)
                resolvedObjName = currentClassName_;
        }
        // Turbofish receiver mangling: `Slice::<i64>.from_vec(...)` parses to
        // `MemberAccess(object=Identifier("Slice", callTypeArgs=[i64]), ...)`.
        // Mono produces a concrete `Slice<i64>` ClassDecl with `from_vec`
        // registered as `Slice<i64>.from_vec`, but the bare receiver name on
        // the AST is still the unmangled template head ("Slice"). Build the
        // mangled receiver from the turbofish args so the lookup hits the
        // monomorphised symbol — without this the link fails on
        // `undefined symbol: Slice.from_vec`.
        if (memberExpr.object && memberExpr.object->kind == ExprKind::Identifier) {
            auto* objIdent = memberExpr.object->as<IdentifierExpr>();
            if (!objIdent->callTypeArgs.empty()) {
                std::string mangledRecv =
                    buildMangledClassName(objName, objIdent->callTypeArgs);
                if (!mangledRecv.empty() && functions_.count(mangledRecv + "." + memberExpr.member)) {
                    resolvedObjName = mangledRecv;
                }
            }
        }
        std::string staticName = resolvedObjName + "." + memberExpr.member;
        auto fit = functions_.find(staticName);
        if (fit == functions_.end())
            fit = functions_.find(objName + "." + memberExpr.member);
        if (fit != functions_.end()) {
            auto* method = fit->second;
            std::vector<llvm::Value*> argsV;
            for (size_t i = 0; i < call->args.size(); ++i) {
                auto* argVal = emitExpr(*call->args[i]);
                if (argVal) {
                    if (i < method->getFunctionType()->getNumParams()) {
                        auto* paramTy = method->getFunctionType()->getParamType(i);
                        // Interface boxing for instance-method args: if
                        // the declared param is an `__iface_X` struct and
                        // the arg is a concrete class value, build a
                        // proper {obj_ptr, vtable_ptr} pair instead of
                        // letting castToType zero-pad (which leaves the
                        // vtable slot NULL and segfaults at dispatch).
                        if (argVal->getType()->isStructTy() && paramTy->isStructTy() &&
                            argVal->getType() != paramTy) {
                            if (auto* paramStTy = llvm::dyn_cast<llvm::StructType>(paramTy)) {
                                if (paramStTy->hasName() &&
                                    paramStTy->getName().starts_with("__iface_")) {
                                    auto ifaceName = paramStTy->getName().substr(8).str();
                                    std::string concreteName;
                                    if (call->args[i]->kind == ExprKind::StructInit)
                                        concreteName = call->args[i]->as<StructInitExpr>()->structName;
                                    else if (call->args[i]->kind == ExprKind::Identifier)
                                        concreteName = resolveClassName(*call->args[i],
                                            call->args[i]->as<IdentifierExpr>()->name);
                                    if (concreteName.empty()) {
                                        if (auto* argSt = llvm::dyn_cast<llvm::StructType>(argVal->getType())) {
                                            if (argSt->hasName()) concreteName = argSt->getName().str();
                                        }
                                    }
                                    // Already-boxed interface values must pass through.
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
                        }
                        argVal = castToType(argVal, paramTy);
                    }
                    argsV.push_back(argVal);
                }
            }
            if (method->getReturnType()->isVoidTy()) {
                builder_->CreateCall(method, argsV);
                return llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0);
            }
            return builder_->CreateCall(method, argsV, "static.call");
        }
    }

    if (!objAlloca) return nullptr;

    // Interface polymorphic dispatch via vtable
    // Path is taken when objName is known (named var) OR synthIfaceName was
    // set (by-value iface returned from a call/index — we already spilled
    // it to a tmp alloca above).
    if (!objName.empty() || !synthIfaceName.empty()) {
        std::string ifaceName = synthIfaceName;
        if (ifaceName.empty() && memberExpr.object && memberExpr.object->inferredType &&
            memberExpr.object->inferredType->kind == VyxTypeKind::Interface) {
            ifaceName = memberExpr.object->inferredType->name;
        }
        if (ifaceName.empty() && !objName.empty()) {
            auto ifaceVarIt = interfaceVarTypes_.find(objName);
            if (ifaceVarIt != interfaceVarTypes_.end()) ifaceName = ifaceVarIt->second;
        }
        if (!ifaceName.empty()) {
            auto ifaceTyIt = structTypes_.find("__iface_" + ifaceName);
            auto vtTyIt = structTypes_.find(ifaceName + "_vtable");

            if (ifaceTyIt != structTypes_.end() && vtTyIt != structTypes_.end()) {
                auto* ifaceTy = ifaceTyIt->second;
                auto* vtTy = vtTyIt->second;
                auto* ptrTy = llvm::PointerType::getUnqual(*context_);

                auto* objPtr = builder_->CreateLoad(ptrTy,
                    builder_->CreateStructGEP(ifaceTy, objAlloca, 0, "iface.obj.slot"), "iface.obj");
                auto* vtPtr = builder_->CreateLoad(ptrTy,
                    builder_->CreateStructGEP(ifaceTy, objAlloca, 1, "iface.vtbl.slot"), "iface.vtbl");

                auto& vtFields = structFieldNames_[ifaceName + "_vtable"];
                for (size_t vi = 0; vi < vtFields.size(); ++vi) {
                    if (vtFields[vi] != memberExpr.member) continue;

                    auto* fnPtr = builder_->CreateLoad(ptrTy,
                        builder_->CreateStructGEP(vtTy, vtPtr, vi, "vtbl.fn.slot"), "vtbl.fn");

                    std::vector<llvm::Value*> argsV = {objPtr};
                    for (auto& arg : call->args) {
                        auto* v = emitExpr(*arg);
                        if (v) argsV.push_back(v);
                    }

                    llvm::Type* retTy = llvm::Type::getInt32Ty(*context_);
                    std::vector<llvm::Type*> paramTypes;
                    std::string methodSuffix = "." + memberExpr.member;
                    for (auto& [fName, fVal] : functions_) {
                        if (fName.size() > methodSuffix.size() &&
                            fName.compare(fName.size() - methodSuffix.size(),
                                          methodSuffix.size(), methodSuffix) == 0) {
                            retTy = fVal->getReturnType();
                            for (unsigned pi = 0; pi < fVal->getFunctionType()->getNumParams(); ++pi)
                                paramTypes.push_back(fVal->getFunctionType()->getParamType(pi));
                            for (size_t ai = 0; ai < argsV.size() && ai < paramTypes.size(); ++ai)
                                argsV[ai] = castToType(argsV[ai], paramTypes[ai]);
                            break;
                        }
                    }
                    if (paramTypes.empty())
                        for (auto& a : argsV) paramTypes.push_back(a->getType());

                    auto* fnTy = llvm::FunctionType::get(retTy, paramTypes, false);
                    if (retTy->isVoidTy()) {
                        builder_->CreateCall(fnTy, fnPtr, argsV);
                        return nullptr;
                    }
                    return builder_->CreateCall(fnTy, fnPtr, argsV, "iface.call");
                }
            }
        }
    }

    auto* allocaTy = getValuePtrType(objAlloca);
    std::string typeName;
    if (memberExpr.object && memberExpr.object->kind == ExprKind::Identifier) {
        typeName = resolveClassName(*memberExpr.object,
            memberExpr.object->as<IdentifierExpr>()->name);
    }
    if (typeName.empty()) {
        if (auto* structTy = llvm::dyn_cast<llvm::StructType>(allocaTy)) {
            if (structTy->hasName()) typeName = structTy->getName().str();
        }
    }

    // Auto-deref for __RefCounted: if the object is Ref<T>, deref and call T.method
    if (typeName == "__RefCounted" &&
        memberExpr.member != "deref" && memberExpr.member != "retain" &&
        memberExpr.member != "release" && memberExpr.member != "refCount") {
        auto rcIt2 = structTypes_.find("__RefCounted");
        if (rcIt2 != structTypes_.end()) {
            auto* refTy = rcIt2->second;
            auto* ptrTy3 = llvm::PointerType::getUnqual(*context_);

            // Try to find method via Sema-inferred Ref inner type, fallback search
            std::string innerType;
            if (memberExpr.object) {
                std::string legacy;
                if (memberExpr.object->kind == ExprKind::Identifier)
                    legacy = memberExpr.object->as<IdentifierExpr>()->name;
                innerType = resolveRefInner(*memberExpr.object, legacy);
            }
            llvm::Function* derefMethod = nullptr;
            if (!innerType.empty()) {
                auto mIt = functions_.find(innerType + "." + memberExpr.member);
                if (mIt != functions_.end()) derefMethod = mIt->second;
            }
            if (!derefMethod) {
                std::string suffix = "." + memberExpr.member;
                for (auto& [fn_name, fn_ptr] : functions_) {
                    if (fn_name.size() > suffix.size() &&
                        fn_name.compare(fn_name.size() - suffix.size(), suffix.size(), suffix) == 0) {
                        auto dotPos = fn_name.rfind('.');
                        if (dotPos != std::string::npos) {
                            std::string candidateClass = fn_name.substr(0, dotPos);
                            if (structTypes_.count(candidateClass) &&
                                candidateClass != "__RefCounted" &&
                                !isInternalStructType(candidateClass)) {
                                innerType = candidateClass;
                                derefMethod = fn_ptr;
                                break;
                            }
                        }
                    }
                }
            }
            if (derefMethod) {
                auto* dataPtr = builder_->CreateStructGEP(refTy, objAlloca, 0);
                auto* data = builder_->CreateLoad(ptrTy3, dataPtr, "ref.auto.data");
                std::vector<llvm::Value*> argsV = {data};

                for (size_t ai = 0; ai < call->args.size(); ++ai) {
                    auto* argVal = emitExpr(*call->args[ai]);
                    if (!argVal) continue;
                    if (argVal->getType() == rcIt2->second) {
                        auto* fn3 = builder_->GetInsertBlock()->getParent();
                        auto* argAlloca = createEntryBlockAlloca(fn3, rcIt2->second, "ref.arg.tmp");
                        builder_->CreateStore(argVal, argAlloca);
                        auto* argDataPtr = builder_->CreateStructGEP(rcIt2->second, argAlloca, 0);
                        auto* argData = builder_->CreateLoad(ptrTy3, argDataPtr, "ref.arg.data");
                        size_t paramIdx = ai + 1;
                        if (paramIdx < derefMethod->getFunctionType()->getNumParams()) {
                            auto* expectedTy = derefMethod->getFunctionType()->getParamType(paramIdx);
                            if (expectedTy->isPointerTy()) argsV.push_back(argData);
                            else argsV.push_back(builder_->CreateLoad(expectedTy, argData, "ref.arg.val"));
                        } else {
                            argsV.push_back(argData);
                        }
                    } else {
                        size_t paramIdx = ai + 1;
                        if (paramIdx < derefMethod->getFunctionType()->getNumParams())
                            argVal = castToType(argVal, derefMethod->getFunctionType()->getParamType(paramIdx));
                        argsV.push_back(argVal);
                    }
                }
                if (derefMethod->getReturnType()->isVoidTy()) {
                    builder_->CreateCall(derefMethod, argsV);
                    return llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0);
                }
                return builder_->CreateCall(derefMethod, argsV, "ref.deref.call");
            }
        }
    }

    std::string mangledName = typeName + "." + memberExpr.member;
    auto fit = functions_.find(mangledName);

    if (fit == functions_.end() && memberExpr.object && memberExpr.object->kind == ExprKind::Identifier) {
        std::string altName = resolveClassName(*memberExpr.object,
            memberExpr.object->as<IdentifierExpr>()->name);
        if (!altName.empty() && altName != typeName) {
            mangledName = altName + "." + memberExpr.member;
            fit = functions_.find(mangledName);
        }
    }

    // Base-name fallback for generic types (e.g. Vec<i32>.method → Vec.method)
    if (fit == functions_.end()) {
        auto ltPos = typeName.find('<');
        if (ltPos != std::string::npos) {
            std::string baseFallback = typeName.substr(0, ltPos) + "." + memberExpr.member;
            fit = functions_.find(baseFallback);
        }
    }

    if (fit == functions_.end()) {
        // Try vtable dispatch: look for method in any vtable for this type
        for (auto& [vtName, vtType] : structTypes_) {
            if (vtName.find("_vtable") != std::string::npos) {
                auto& vtFields = structFieldNames_[vtName];
                for (size_t vi = 0; vi < vtFields.size(); ++vi) {
                    if (vtFields[vi] == memberExpr.member) {
                        std::string vtGlobalName = typeName + "_vtable_" + vtName.substr(0, vtName.find("_vtable"));
                        auto* vtGlobal = module_->getGlobalVariable(vtGlobalName, true);
                        if (vtGlobal) {
                            auto* fnPtr = builder_->CreateStructGEP(vtType, vtGlobal, vi, "vtbl.fn.ptr");
                            auto* fn = builder_->CreateLoad(llvm::PointerType::getUnqual(*context_), fnPtr, "vtbl.fn");
                            std::vector<llvm::Value*> argsV = {objAlloca};
                            for (auto& arg : call->args) {
                                auto* v = emitExpr(*arg);
                                if (v) argsV.push_back(v);
                            }
                            std::vector<llvm::Type*> paramTypes(argsV.size(), llvm::PointerType::getUnqual(*context_));
                            llvm::Type* retTy = llvm::Type::getInt32Ty(*context_);
                            std::string methodSuffix = "." + memberExpr.member;
                            for (auto& [fName, fVal] : functions_) {
                                if (fName.size() > methodSuffix.size() &&
                                    fName.compare(fName.size() - methodSuffix.size(),
                                                  methodSuffix.size(), methodSuffix) == 0) {
                                    retTy = fVal->getReturnType();
                                    paramTypes.clear();
                                    for (unsigned pi = 0; pi < fVal->getFunctionType()->getNumParams(); ++pi)
                                        paramTypes.push_back(fVal->getFunctionType()->getParamType(pi));
                                    for (size_t ai = 0; ai < argsV.size() && ai < paramTypes.size(); ++ai)
                                        argsV[ai] = castToType(argsV[ai], paramTypes[ai]);
                                    break;
                                }
                            }
                            auto* fnTy = llvm::FunctionType::get(retTy, paramTypes, false);
                            return builder_->CreateCall(fnTy, fn, argsV, "vtbl.call");
                        }
                    }
                }
            }
        }
        // UFCS fallback: prefer methods whose class prefix matches the object type
        auto ufcsIt = functions_.end();
        std::string baseTypeName = typeName;
        if (baseTypeName.starts_with("__")) baseTypeName = baseTypeName.substr(2);
        auto ltPos2 = baseTypeName.find('<');
        if (ltPos2 != std::string::npos) baseTypeName = baseTypeName.substr(0, ltPos2);

        for (auto& [fname, ffn] : functions_) {
            std::string methodPart;
            auto dotPos = fname.rfind('.');
            if (dotPos != std::string::npos) methodPart = fname.substr(dotPos + 1);
            else if (fname == memberExpr.member) methodPart = fname;
            if (methodPart != memberExpr.member) continue;

            if (dotPos != std::string::npos && !baseTypeName.empty()) {
                std::string classPrefix = fname.substr(0, dotPos);
                auto lt = classPrefix.find('<');
                std::string baseClass = (lt != std::string::npos) ? classPrefix.substr(0, lt) : classPrefix;
                if (baseClass == baseTypeName || baseClass == "__" + baseTypeName ||
                    baseTypeName == baseClass) {
                    ufcsIt = functions_.find(fname);
                    break;
                }
            }
        }
        // When the receiver type is known, do NOT fall through to loose bare-name
        // or suffix matching: those can accidentally bind a free function like
        // `sum(v: Vec<i64>)` instead of the proper `Vec<i64>.sum()` instance method.
        if (!typeName.empty() && ufcsIt == functions_.end()) {
            // Receiver type is known but class-prefixed UFCS scan found nothing —
            // return nullptr rather than risk a misbinding.
            return nullptr;
        }
        if (ufcsIt == functions_.end()) {
            // Type unknown: bare-name lookup (safe only when receiver type is absent)
            auto bareIt = functions_.find(memberExpr.member);
            if (bareIt != functions_.end()) {
                // Only accept if the function takes no self-like pointer param
                // (i.e., it really is a zero-arg or plain free function)
                auto* bfn = bareIt->second;
                auto* bfty = bfn->getFunctionType();
                bool firstParamIsStruct = bfty->getNumParams() > 0 &&
                    bfty->getParamType(0)->isStructTy();
                if (!firstParamIsStruct) {
                    ufcsIt = bareIt;
                }
            }
        }
        if (ufcsIt == functions_.end()) {
            for (auto& [fname, ffn] : functions_) {
                auto dotPos = fname.rfind('.');
                if (dotPos != std::string::npos && fname.substr(dotPos + 1) == memberExpr.member) {
                    ufcsIt = functions_.find(fname);
                    break;
                }
                auto colonPos = fname.rfind("::");
                if (colonPos != std::string::npos && fname.substr(colonPos + 2) == memberExpr.member) {
                    ufcsIt = functions_.find(fname);
                    break;
                }
            }
        }
        if (ufcsIt != functions_.end()) {
            auto* ufcsFn = ufcsIt->second;
            std::vector<llvm::Value*> argsV;
            if (ufcsFn->getFunctionType()->getNumParams() > 0 &&
                ufcsFn->getFunctionType()->getParamType(0)->isPointerTy()) {
                argsV.push_back(objAlloca);
            } else {
                auto* objVal = builder_->CreateLoad(allocaTy, objAlloca, "ufcs.obj");
                argsV.push_back(objVal);
            }
            for (auto& arg : call->args) {
                if (arg) argsV.push_back(emitExpr(*arg));
            }
            if (ufcsFn->getReturnType()->isVoidTy()) {
                builder_->CreateCall(ufcsFn, argsV);
                return nullptr;
            }
            return builder_->CreateCall(ufcsFn, argsV, "ufcs.call");
        }
        return nullptr;
    }

    auto* method = fit->second;

    std::vector<llvm::Value*> argsV;
    // Primitive-target impl method (`impl Hashable for i32`): the function
    // declares `self` by value (not by pointer), so pass the loaded value.
    // Detected by: first param is non-pointer, OR the typeName is a
    // known primitive (`i32`/`i64`/.../`string`). Non-primitive class
    // methods keep their self-pointer ABI unchanged.
    bool selfByValue = false;
    if (method->getFunctionType()->getNumParams() > 0) {
        auto* p0 = method->getFunctionType()->getParamType(0);
        if (!p0->isPointerTy()) selfByValue = true;
    }
    if (selfByValue && objAlloca) {
        auto* p0ty = method->getFunctionType()->getParamType(0);
        auto* selfVal = builder_->CreateLoad(p0ty, objAlloca, "prim.self");
        argsV.push_back(selfVal);
    } else {
        argsV.push_back(objAlloca); // self pointer (class / struct receiver)
    }

    for (size_t i = 0; i < call->args.size(); ++i) {
        auto* argVal = emitExpr(*call->args[i]);
        if (argVal) {
            if (i + 1 < method->getFunctionType()->getNumParams()) {
                argVal = castToType(argVal, method->getFunctionType()->getParamType(i + 1));
            }
            argsV.push_back(argVal);
        }
    }

    if (method->getReturnType()->isVoidTy()) {
        builder_->CreateCall(method, argsV);
        return nullptr;
    }
    return builder_->CreateCall(method, argsV, "mcall");
}

} // namespace vyx
