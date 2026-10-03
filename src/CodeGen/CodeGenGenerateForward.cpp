#include "CodeGenIncludes.h"

namespace vyx {

void CodeGen::generateForwardDeclareFunctionsAndExtern() {
    fprintf(stderr, "[CodeGen] Phase: forward-declare functions + extern\n"); fflush(stderr);
    for (auto& decl : unit_->declarations) {
        if (!decl) continue;
        bool skipPlatform = false;
        for (auto& [an, av] : decl->attributes) {
            if (an == "platform") {
#ifdef _WIN32
                if (av != "windows") skipPlatform = true;
#elif __linux__
                if (av != "linux") skipPlatform = true;
#elif __APPLE__
                if (av != "macos") skipPlatform = true;
#endif
            }
        }
        if (skipPlatform) continue;
        if (decl->kind == DeclKind::Function) {
            auto* fnDecl = decl->as<FunctionDecl>();
            if (!decl->genericParams.empty()) continue;
            if (fnDecl->isComptime) comptimeDecls_[decl->name] = decl.get();
            if (functions_.count(decl->name)) continue;
            
            llvm::Type* retTy = fnDecl->returnType
                ? toLLVMType(*fnDecl->returnType)
                : llvm::Type::getVoidTy(*context_);

            std::vector<llvm::Type*> paramTypes;
            bool hasRuntimeVarargs = false;
            for (auto& p : fnDecl->params) {
                if (p.name.size() > 3 && p.name.substr(0, 3) == "..." &&
                    p.type && p.type->name == "Any") {
                    hasRuntimeVarargs = true;
                    paramTypes.push_back(llvm::PointerType::getUnqual(*context_));
                    paramTypes.push_back(llvm::Type::getInt64Ty(*context_));
                } else {
                    paramTypes.push_back(p.type ? toLLVMType(*p.type) : llvm::Type::getInt32Ty(*context_));
                }
            }

            auto* fnType = llvm::FunctionType::get(retTy, paramTypes, false);
            auto linkage = llvm::Function::ExternalLinkage;
            auto* fn = llvm::Function::Create(fnType, linkage, decl->name, *module_);

            size_t paramIdx = 0;
            size_t llvmArgIdx = 0;
            for (auto it = fn->arg_begin(); it != fn->arg_end() && paramIdx < fnDecl->params.size(); ++it, ++llvmArgIdx) {
                auto& p = fnDecl->params[paramIdx];
                if (hasRuntimeVarargs && p.name.size() > 3 && p.name.substr(0, 3) == "...") {
                    it->setName(p.name.substr(3) + ".ptr");
                    ++it;
                    ++llvmArgIdx;
                    if (it != fn->arg_end())
                        it->setName(p.name.substr(3) + ".count");
                } else {
                    it->setName(p.name);
                }
                ++paramIdx;
            }

            functions_[decl->name] = fn;
        }
        if (decl->kind == DeclKind::ExternBlock) {
            emitExternBlock(*decl);
        }
    }
}

} // namespace vyx
