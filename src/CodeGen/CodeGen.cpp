#include "CodeGenIncludes.h"

namespace vyx {

CodeGen::CodeGen(DiagnosticsEngine& diag, const std::string& moduleName, const std::string& targetTriple)
    : diag_(diag),
      context_(std::make_unique<llvm::LLVMContext>()),
      module_(std::make_unique<llvm::Module>(moduleName, *context_)),
      builder_(std::make_unique<llvm::IRBuilder<>>(*context_)) {

    std::string triple = targetTriple.empty()
        ? llvm::sys::getDefaultTargetTriple()
        : targetTriple;
    module_->setTargetTriple(llvm::Triple(triple));

    std::string error;
    auto* target = llvm::TargetRegistry::lookupTarget(llvm::Triple(triple), error);
    if (target) {
        std::unique_ptr<llvm::TargetMachine> tm(
            target->createTargetMachine(llvm::Triple(triple), "generic", "", {}, {}));
        if (tm) {
            module_->setDataLayout(tm->createDataLayout());
        }
    }
}

// Debug/coverage helpers moved to CodeGenDebug.cpp
// toLLVMType(const VyxType&) moved to CodeGenCore.cpp
// mangleTypeAnnotation moved to CodeGenCore.cpp
// toLLVMType(const TypeAnnotation&) moved to CodeGenTypeMap.cpp
// trackParamType moved to CodeGenCore.cpp
// generate moved to CodeGenGenerate.cpp
// emitDecl / emitFunctionDecl moved to CodeGenDecl.cpp
// Struct/generic-struct helpers moved to CodeGenStruct.cpp
// class helper methods moved to CodeGenClass.cpp
// extern block helpers moved to CodeGenExtern.cpp
// Utility helpers moved to CodeGenUtils.cpp
// getMemberAddress moved to CodeGenMember.cpp
// String/hash/Any/Result helpers moved to CodeGenUtils.cpp
// optimize/emitIR/emitObject moved to CodeGenBackend.cpp
// ABI/comptime helpers moved to CodeGenBackend.cpp

} // namespace vyx
