#include "CodeGenIncludes.h"
#include <map>

namespace vyx {

void CodeGen::emitExternBlock(const Decl& decl) {
    auto& eb = *decl.as<const ExternBlockDecl>();
    bool isCpp = (decl.externABI == "C++");

    for (auto& fnDecl : eb.externDecls) {
        if (!fnDecl || fnDecl->kind != DeclKind::Function) continue;
        auto* fn = fnDecl->as<FunctionDecl>();

        llvm::Type* retTy = fn->returnType
            ? toLLVMType(*fn->returnType)
            : llvm::Type::getVoidTy(*context_);

        std::vector<llvm::Type*> paramTypes;
        bool isVarArg = false;

        for (auto& p : fn->params) {
            if (p.name.starts_with("...")) {
                isVarArg = true;
                continue;
            }
            paramTypes.push_back(p.type ? toLLVMType(*p.type) : llvm::Type::getInt32Ty(*context_));
        }

        auto* fnType = llvm::FunctionType::get(retTy, paramTypes, isVarArg);

        std::string linkName = fnDecl->name;

        if (!isCpp && usesMsvcAbi()) {
            static const std::map<std::string, std::string> posixToWin = {
                {"stat", "_stat64"},
                {"fstat", "_fstat64"},
                {"access", "_access"},
            };
            auto pit = posixToWin.find(linkName);
            if (pit != posixToWin.end()) linkName = pit->second;
        }

        if (isCpp) {
            bool hasExplicitName = false;
            for (auto& [attrName, attrValue] : fnDecl->attributes) {
                if (attrName == "link_name" && !attrValue.empty()) {
                    linkName = attrValue;
                    hasExplicitName = true;
                }
            }
            if (!hasExplicitName) {
                bool useMsvc = usesMsvcAbi();
                if (useMsvc) {
                    linkName = "?" + fnDecl->name;
                    if (!eb.namespaceName.empty()) linkName += "@" + eb.namespaceName;
                    linkName += "@@YA";
                    linkName += fn->returnType ? msvcMangleType(fn->returnType->name) : "X";
                    if (fn->params.empty()) { linkName += "X"; }
                    else { for (auto& p : fn->params) linkName += p.type ? msvcMangleType(p.type->name) : "H"; }
                    linkName += "@Z";
                } else {
                    linkName = "_Z";
                    if (!eb.namespaceName.empty()) {
                        linkName += "N" + std::to_string(eb.namespaceName.size()) + eb.namespaceName;
                    }
                    linkName += std::to_string(fnDecl->name.size()) + fnDecl->name;
                    if (!eb.namespaceName.empty()) linkName += "E";
                    if (fn->params.empty()) { linkName += "v"; }
                    else { for (auto& p : fn->params) linkName += p.type ? itaniumMangleType(p.type->name) : "i"; }
                }
            }
        }

        auto linkage = llvm::Function::ExternalLinkage;
        auto* func = module_->getFunction(linkName);
        if (func) {
            functions_[fnDecl->name] = func;
            continue;
        }
        func = llvm::Function::Create(fnType, linkage, linkName, *module_);

        if (decl.isExport) {
            func->setDLLStorageClass(llvm::GlobalValue::DLLExportStorageClass);
            functions_[fnDecl->name] = func;
            if (fn->body) {
                auto* entry = llvm::BasicBlock::Create(*context_, "entry", func);
                builder_->SetInsertPoint(entry);
                for (unsigned i = 0; i < func->arg_size(); ++i) {
                    auto* arg = func->getArg(i);
                    auto* alloca = createEntryBlockAlloca(func, arg->getType(), fn->params[i].name);
                    builder_->CreateStore(arg, alloca);
                    namedValues_[fn->params[i].name] = alloca;
                    trackParamType(fn->params[i]);
                }
                emitStmt(*fn->body);
                if (!builder_->GetInsertBlock()->getTerminator()) {
                    if (retTy->isVoidTy())
                        builder_->CreateRetVoid();
                    else
                        builder_->CreateRet(llvm::Constant::getNullValue(retTy));
                }
                for (auto& p : fn->params) namedValues_.erase(p.name);
            }
            continue;
        }

        bool isStaticLink = false;
        for (auto& [an, av] : decl.attributes) {
            if (an == "static_link" || an == "static") isStaticLink = true;
        }
        if (isCpp && !isStaticLink) {
            func->setDLLStorageClass(llvm::GlobalValue::DLLImportStorageClass);
        }

        functions_[fnDecl->name] = func;
    }

    bool externIsStaticLink = false;
    for (auto& [an, av] : decl.attributes) {
        if (an == "static_link" || an == "static") externIsStaticLink = true;
    }

    for (auto& child : eb.externDecls) {
        if (!child || child->kind != DeclKind::GlobalVar) continue;
        auto* gvChild = child->as<GlobalVarDecl>();
        llvm::Type* varTy = gvChild->varType
            ? toLLVMType(*gvChild->varType)
            : llvm::Type::getInt32Ty(*context_);
        auto* gv = module_->getGlobalVariable(child->name, true);
        if (!gv) {
            gv = new llvm::GlobalVariable(*module_, varTy, false,
                llvm::GlobalValue::ExternalLinkage, nullptr, child->name);
            gv->setExternallyInitialized(true);
        }
    }

    for (auto& child : eb.externDecls) {
        if (!child) continue;
        if (child->kind == DeclKind::Class || child->kind == DeclKind::Struct) {
            emitStructDecl(*child);

            if (isCpp) {
                std::string className = child->name;
                std::string ns = eb.namespaceName;
                if (child->kind != DeclKind::Class) continue;
                auto& childMethods = child->as<ClassDecl>()->methods;

                for (auto& method : childMethods) {
                    std::string vyxName = className + "." + method.name;

                    llvm::Type* retTy = method.returnType
                        ? toLLVMType(*method.returnType)
                        : llvm::Type::getVoidTy(*context_);

                    std::vector<llvm::Type*> paramTypes;
                    if (!method.isStatic) {
                        paramTypes.push_back(llvm::PointerType::getUnqual(*context_));
                    }
                    for (auto& p : method.params) {
                        paramTypes.push_back(p.type ? toLLVMType(*p.type) : llvm::Type::getInt32Ty(*context_));
                    }

                    auto* fnType = llvm::FunctionType::get(retTy, paramTypes, false);

                    if (method.name == className) continue;

                    bool useMsvc = usesMsvcAbi();

                    std::string mangledName;
                    if (useMsvc) {
                        mangledName = "?" + method.name + "@" + className;
                        if (!ns.empty()) mangledName += "@" + ns;
                        mangledName += "@@QEAA";
                        mangledName += method.returnType ? msvcMangleType(method.returnType->name) : "X";
                        if (method.params.empty()) { mangledName += "XZ"; }
                        else {
                            for (auto& p : method.params) mangledName += p.type ? msvcMangleType(p.type->name) : "H";
                            mangledName += "@Z";
                        }
                    } else {
                        mangledName = "_ZN";
                        if (!ns.empty()) mangledName += std::to_string(ns.size()) + ns;
                        mangledName += std::to_string(className.size()) + className;
                        mangledName += std::to_string(method.name.size()) + method.name + "E";
                        if (method.params.empty()) mangledName += "v";
                        else { for (auto& p : method.params) mangledName += p.type ? itaniumMangleType(p.type->name) : "i"; }
                    }

                    if (method.body) {
                        auto* fn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, mangledName, *module_);
                        if (!method.isStatic) {
                            fn->arg_begin()->setName("self");
                        }
                        size_t idx = method.isStatic ? 0 : 1;
                        for (auto& arg : llvm::make_range(
                                fn->arg_begin() + (method.isStatic ? 0 : 1), fn->arg_end())) {
                            size_t paramIdx = method.isStatic ? idx : idx - 1;
                            if (paramIdx < method.params.size())
                                arg.setName(method.params[paramIdx].name);
                            ++idx;
                        }

                        for (auto& [attrName, attrValue] : method.attributes) {
                            if (attrName == "noinline") fn->addFnAttr(llvm::Attribute::NoInline);
                            else if (attrName == "inline") fn->addFnAttr(llvm::Attribute::AlwaysInline);
                        }

                        functions_[vyxName] = fn;

                        auto* entry = llvm::BasicBlock::Create(*context_, "entry", fn);
                        builder_->SetInsertPoint(entry);

                        auto savedValues = namedValues_;
                        auto savedClassName = currentClassName_;
                        auto savedSideMaps = snapshotVarScopeSideMaps();
                        namedValues_.clear();
                        clearVarScopeSideMaps();
                        currentClassName_ = className;

                        if (!method.isStatic) {
                            auto* selfPtrTy = llvm::PointerType::getUnqual(*context_);
                            auto* selfAlloca = createEntryBlockAlloca(fn, selfPtrTy, "self");
                            builder_->CreateStore(fn->arg_begin(), selfAlloca);
                            namedValues_["self"] = selfAlloca;
                        }

                        idx = method.isStatic ? 0 : 1;
                        for (auto& arg : llvm::make_range(
                                fn->arg_begin() + (method.isStatic ? 0 : 1), fn->arg_end())) {
                            size_t paramIdx = method.isStatic ? idx : idx - 1;
                            std::string paramName = std::string(arg.getName());
                            if (paramIdx < method.params.size()) paramName = method.params[paramIdx].name;
                            auto* alloca = createEntryBlockAlloca(fn, arg.getType(), paramName);
                            builder_->CreateStore(&arg, alloca);
                            namedValues_[paramName] = alloca;
                            if (paramIdx < method.params.size()) trackParamType(method.params[paramIdx]);
                            ++idx;
                        }

                        emitBlock(*method.body);

                        auto* lastBB = builder_->GetInsertBlock();
                        if (lastBB && !lastBB->getTerminator()) {
                            if (fn->getReturnType()->isVoidTy()) builder_->CreateRetVoid();
                            else builder_->CreateRet(llvm::Constant::getNullValue(fn->getReturnType()));
                        }

                        if (llvm::verifyFunction(*fn, &llvm::errs())) {
                            diag_.error(CodeGen::codegenInternalSourceLocation(), "verification failed for function '{}'", fn->getName().str());
                        }
                        currentClassName_ = savedClassName;
                        namedValues_ = std::move(savedValues);
                        restoreVarScopeSideMaps(std::move(savedSideMaps));
                    } else {
                        auto* func = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, mangledName, *module_);
                        if (!externIsStaticLink)
                            func->setDLLStorageClass(llvm::GlobalValue::DLLImportStorageClass);
                        if (!method.isStatic) func->arg_begin()->setName("this");
                        functions_[vyxName] = func;
                    }
                }

                bool allMethodsHaveBody = true;
                for (auto& m : childMethods) {
                    if (m.name == className) continue;
                    if (!m.body) { allMethodsHaveBody = false; break; }
                }

                auto* ptrTy = llvm::PointerType::getUnqual(*context_);

                if (!allMethodsHaveBody) {
                    bool useMsvc2 = usesMsvcAbi();

                    int ctorCount = 0;
                    for (auto& method : childMethods) {
                        if (method.name != className) continue;

                        std::string ctorMangled;
                        if (useMsvc2) {
                            ctorMangled = "??0" + className;
                            if (!ns.empty()) ctorMangled += "@" + ns;
                            ctorMangled += "@@QEAA@";
                            if (method.params.empty()) { ctorMangled += "XZ"; }
                            else {
                                for (auto& p : method.params) ctorMangled += p.type ? msvcMangleType(p.type->name) : "H";
                                ctorMangled += "@Z";
                            }
                        } else {
                            ctorMangled = "_ZN";
                            if (!ns.empty()) ctorMangled += std::to_string(ns.size()) + ns;
                            ctorMangled += std::to_string(className.size()) + className + "C1E";
                            if (method.params.empty()) ctorMangled += "v";
                            else { for (auto& p : method.params) ctorMangled += p.type ? itaniumMangleType(p.type->name) : "i"; }
                        }

                        std::vector<llvm::Type*> ctorParams = {ptrTy};
                        for (auto& p : method.params)
                            ctorParams.push_back(p.type ? toLLVMType(*p.type) : llvm::Type::getInt32Ty(*context_));
                        auto* ctorTy = llvm::FunctionType::get(ptrTy, ctorParams, false);
                        auto* ctorFn = llvm::Function::Create(ctorTy, llvm::Function::ExternalLinkage, ctorMangled, *module_);
                        if (!externIsStaticLink)
                            ctorFn->setDLLStorageClass(llvm::GlobalValue::DLLImportStorageClass);

                        std::string key = className + ".__ctor_" + std::to_string(method.params.size());
                        functions_[key] = ctorFn;
                        if (ctorCount == 0) functions_[className + ".__ctor"] = ctorFn;
                        ctorCount++;
                    }

                    if (ctorCount == 0) {
                        std::string ctorMangled;
                        if (useMsvc2) {
                            ctorMangled = "??0" + className;
                            if (!ns.empty()) ctorMangled += "@" + ns;
                            ctorMangled += "@@QEAA@XZ";
                        } else {
                            ctorMangled = "_ZN";
                            if (!ns.empty()) ctorMangled += std::to_string(ns.size()) + ns;
                            ctorMangled += std::to_string(className.size()) + className + "C1Ev";
                        }
                        auto* ctorTy = llvm::FunctionType::get(ptrTy, {ptrTy}, false);
                        auto* ctorFn = llvm::Function::Create(ctorTy, llvm::Function::ExternalLinkage, ctorMangled, *module_);
                        if (!externIsStaticLink)
                            ctorFn->setDLLStorageClass(llvm::GlobalValue::DLLImportStorageClass);
                        functions_[className + ".__ctor"] = ctorFn;
                        functions_[className + ".__ctor_0"] = ctorFn;
                    }

                    {
                        std::string dtorMangled;
                        if (useMsvc2) {
                            dtorMangled = "??1" + className;
                            if (!ns.empty()) dtorMangled += "@" + ns;
                            dtorMangled += "@@QEAA@XZ";
                        } else {
                            dtorMangled = "_ZN";
                            if (!ns.empty()) dtorMangled += std::to_string(ns.size()) + ns;
                            dtorMangled += std::to_string(className.size()) + className + "D1Ev";
                        }
                        auto* dtorTy = llvm::FunctionType::get(llvm::Type::getVoidTy(*context_), {ptrTy}, false);
                        auto* dtorFn = llvm::Function::Create(dtorTy, llvm::Function::ExternalLinkage, dtorMangled, *module_);
                        if (!externIsStaticLink)
                            dtorFn->setDLLStorageClass(llvm::GlobalValue::DLLImportStorageClass);
                        functions_[className + ".__dtor"] = dtorFn;

                        if (functions_.find(className + ".drop") == functions_.end()) {
                            auto* dropTy = llvm::FunctionType::get(llvm::Type::getVoidTy(*context_), {ptrTy}, false);
                            auto* dropFn = llvm::Function::Create(dropTy, llvm::Function::InternalLinkage,
                                className + ".drop", *module_);
                            dropFn->arg_begin()->setName("self");
                            auto* dropEntry = llvm::BasicBlock::Create(*context_, "entry", dropFn);

                            auto* savedBB = builder_->GetInsertBlock();
                            auto savedPt = builder_->GetInsertPoint();
                            builder_->SetInsertPoint(dropEntry);
                            builder_->CreateCall(dtorFn, {&*dropFn->arg_begin()});
                            std::string delName = useMsvc2 ? "??3@YAXPEAX@Z" : "_ZdlPv";
                            auto* delFn = module_->getFunction(delName);
                            if (delFn) builder_->CreateCall(delFn, {&*dropFn->arg_begin()});
                            builder_->CreateRetVoid();
                            if (savedBB) builder_->SetInsertPoint(savedBB, savedPt);
                            functions_[className + ".drop"] = dropFn;
                        }
                    }

                    if (!module_->getFunction("??2@YAPEAX_K@Z") && !module_->getFunction("_Znwm")) {
                        auto* i64Ty = llvm::Type::getInt64Ty(*context_);
                        auto* newTy = llvm::FunctionType::get(ptrTy, {i64Ty}, false);
                        std::string newMangled = useMsvc2 ? "??2@YAPEAX_K@Z" : "_Znwm";
                        llvm::Function::Create(newTy, llvm::Function::ExternalLinkage, newMangled, *module_);

                        auto* delTy = llvm::FunctionType::get(llvm::Type::getVoidTy(*context_), {ptrTy}, false);
                        std::string delMangled = useMsvc2 ? "??3@YAXPEAX@Z" : "_ZdlPv";
                        llvm::Function::Create(delTy, llvm::Function::ExternalLinkage, delMangled, *module_);
                    }
                }

                auto stIt = structTypes_.find(className);
                if (stIt != structTypes_.end()) {
                    auto* stTy = stIt->second;
                    auto& dl = module_->getDataLayout();
                    uint64_t classSize = dl.getTypeAllocSize(stTy);
                    if (classSize == 0) classSize = 8;
                    structFieldNames_[className + ".__size"] = {std::to_string(classSize)};
                }

                if (functions_.find(className + ".drop") == functions_.end()) {
                    auto* dropTy = llvm::FunctionType::get(llvm::Type::getVoidTy(*context_), {ptrTy}, false);
                    auto* dropFn = llvm::Function::Create(dropTy, llvm::Function::InternalLinkage,
                        className + ".drop", *module_);
                    dropFn->arg_begin()->setName("self");
                    auto* dropEntry = llvm::BasicBlock::Create(*context_, "entry", dropFn);

                    auto* savedBB = builder_->GetInsertBlock();
                    auto savedPt = builder_->GetInsertPoint();
                    builder_->SetInsertPoint(dropEntry);
                    builder_->CreateRetVoid();
                    if (savedBB) builder_->SetInsertPoint(savedBB, savedPt);
                    functions_[className + ".drop"] = dropFn;
                }
            }
        }
    }
}

} // namespace vyx
