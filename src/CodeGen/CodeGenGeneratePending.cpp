#include "CodeGenIncludes.h"
#include <set>

namespace vyx {

void CodeGen::generateInstantiatePendingGenericClasses() {
    fprintf(stderr, "[CodeGen] Phase: generic class instantiation (%zu pending)\n", pendingGenericClasses_.size()); fflush(stderr);
    // Fixed-point loop: instantiating one pending class body may, through
    // `toLLVMType`/`getOrCreateGenericStructType`, push_back more entries into
    // `pendingGenericClasses_` while we iterate. Snapshotting per wave via
    // `std::move` and processing value-copies keeps references/iterators
    // stable and ensures newly-generated pending entries are also compiled.
    std::set<std::string> compiledPending;
    while (!pendingGenericClasses_.empty()) {
        std::vector<PendingGenericClass> batch = std::move(pendingGenericClasses_);
        pendingGenericClasses_.clear();
        for (size_t pi = 0; pi < batch.size(); ++pi) {
            auto pending = batch[pi];
            if (compiledPending.count(pending.mangledName)) continue;
            compiledPending.insert(pending.mangledName);

            bool pendingParamsUnresolved = false;
            for (auto& [gp, concreteName] : pending.paramToType) {
                if (concreteName.size() == 1 && std::isupper(concreteName[0])) {
                    diag_.error(pending.templateDecl ? pending.templateDecl->location : SourceLocation{},
                        "pending generic instantiation `{}` still has unresolved type parameter `{}` -> `{}`",
                        pending.mangledName, gp, concreteName);
                    pendingParamsUnresolved = true;
                }
            }
            if (pendingParamsUnresolved) continue;

            auto savedClassName = currentClassName_;
            auto savedGenericParams = genericTypeParams_;
            auto savedGenericParamNames = genericTypeParamNames_;
            currentClassName_ = pending.mangledName;
            genericInstantiating_.insert(pending.mangledName);

            for (auto& [gp, concreteName] : pending.paramToType) {
                TypeAnnotation ann;
                ann.kind = TypeAnnotationKind::Named;
                ann.name = concreteName;
                genericTypeParams_[gp] = toLLVMType(ann);
                genericTypeParamNames_[gp] = concreteName;
            }

            const std::vector<MethodDecl>* methodsPtr = nullptr;
            if (pending.templateDecl->kind == DeclKind::Class)
                methodsPtr = &pending.templateDecl->as<const ClassDecl>()->methods;
            else if (pending.templateDecl->kind == DeclKind::ErrorDef)
                methodsPtr = &pending.templateDecl->as<const ErrorDefDecl>()->methods;
            if (!methodsPtr) {
                currentClassName_ = savedClassName;
                genericTypeParams_ = savedGenericParams;
                genericTypeParamNames_ = savedGenericParamNames;
                genericInstantiating_.erase(pending.mangledName);
                continue;
            }
            auto& pendingMethods = *methodsPtr;
            for (size_t mi = 0; mi < pendingMethods.size(); ++mi) {
                auto& method = pendingMethods[mi];
                if (!method.genericParams.empty()) continue;
                std::string methodName = pending.mangledName + "." + method.name;
                auto* fn = functions_.count(methodName) ? functions_[methodName] : nullptr;
                if (!fn || !method.body || !fn->empty()) continue;

                auto* entry = llvm::BasicBlock::Create(*context_, "entry", fn);
                builder_->SetInsertPoint(entry);
                auto savedValues = namedValues_;
                namedValues_.clear();
                if (!method.isStatic) {
                    auto* selfAlloca = createEntryBlockAlloca(fn,
                        llvm::PointerType::getUnqual(*context_), "self");
                    builder_->CreateStore(fn->arg_begin(), selfAlloca);
                    namedValues_["self"] = selfAlloca;
                    classVarTypes_["self"] = pending.mangledName;
                }
                size_t idx = method.isStatic ? 0 : 1;
                for (auto& arg : llvm::make_range(fn->arg_begin() + (method.isStatic ? 0 : 1), fn->arg_end())) {
                    size_t paramIdx = method.isStatic ? idx : idx - 1;
                    if (paramIdx < method.params.size()) {
                        auto* alloca = createEntryBlockAlloca(fn, arg.getType(), method.params[paramIdx].name);
                        builder_->CreateStore(&arg, alloca);
                        namedValues_[method.params[paramIdx].name] = alloca;
                        trackParamType(method.params[paramIdx]);
                    }
                    ++idx;
                }
                emitStmt(*method.body);
                if (builder_->GetInsertBlock() && !builder_->GetInsertBlock()->getTerminator()) {
                    if (fn->getReturnType()->isVoidTy()) builder_->CreateRetVoid();
                    else builder_->CreateRet(llvm::Constant::getNullValue(fn->getReturnType()));
                }
                bool verifyFailed = llvm::verifyFunction(*fn, &llvm::errs());
                if (verifyFailed) {
                    diag_.error(CodeGen::codegenInternalSourceLocation(), "verification failed for function '{}'", fn->getName().str());
                } else {
                    // Deferred-linkage companion: the body has been verified,
                    // so demote the External seed from `CodeGenStruct` to
                    // InternalLinkage. The inline hint is already attached at
                    // creation, so we only adjust linkage here; if body gen
                    // had failed, External remains and `generateFinalizeModule`
                    // can surface a diagnostic later.
                    fn->setLinkage(llvm::GlobalValue::InternalLinkage);
                }
                namedValues_ = std::move(savedValues);
            }
            currentClassName_ = savedClassName;
            genericTypeParams_ = savedGenericParams;
            genericTypeParamNames_ = savedGenericParamNames;
            genericInstantiating_.erase(pending.mangledName);
        }
    }
}

} // namespace vyx
