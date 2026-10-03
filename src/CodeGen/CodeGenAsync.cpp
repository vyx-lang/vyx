#include "CodeGenIncludes.h"

namespace vyx {

void CodeGen::emitAsyncFunctionDecl(Decl& decl) {
    // Async functions use the same lowering pipeline as sync functions.
    // The async behavior lives in AwaitExpr / yield lowering and the std.vio
    // runtime types; no separate coroutine object model is synthesized here.
    emitFunctionDecl(decl);
}

llvm::Value* CodeGen::emitAwaitExpr(const Expr& expr) {
    auto* ae = expr.as<const AwaitExpr>();
    if (!ae) return nullptr;

    auto emitVioYield = [&]() -> llvm::Value* {
        auto* yieldFn = module_->getFunction("vio_yield");
        if (!yieldFn) {
            auto* yieldTy = llvm::FunctionType::get(llvm::Type::getVoidTy(*context_), {}, false);
            yieldFn = llvm::cast<llvm::Function>(
                module_->getOrInsertFunction("vio_yield", yieldTy).getCallee());
        }
        builder_->CreateCall(yieldFn, {});
        return nullptr;
    };

    if (!ae->inner) {
        return emitVioYield();
    }

    auto* val = emitExpr(*ae->inner);
    auto* innerTy = ae->inner->inferredType.get();
    if (innerTy && isAsyncLike(*innerTy)) {
        auto* methodFn = findClassMethod(innerTy->name, "await_value");
        if (!methodFn) {
            auto lt = innerTy->name.find('<');
            if (lt != std::string::npos) {
                methodFn = findClassMethod(innerTy->name.substr(0, lt), "await_value");
            }
        }
        if (!methodFn) {
            diag_.error(expr.location,
                "async value '{}' does not expose an await_value() method",
                innerTy->name);
            return nullptr;
        }
        if (!val) {
            diag_.error(expr.location,
                "await operand '{}' did not produce a value",
                innerTy->name);
            return nullptr;
        }
        auto* selfTy = methodFn->getFunctionType()->getNumParams() > 0
            ? methodFn->getFunctionType()->getParamType(0)
            : nullptr;
        std::vector<llvm::Value*> args;
        if (selfTy) {
            args.push_back(castToType(val, selfTy));
        } else {
            args.push_back(val);
        }
        return builder_->CreateCall(methodFn, args, "await.value");
    }

    emitVioYield();
    return val;
}

} // namespace vyx
