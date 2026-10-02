#include "CodeGenIncludes.h"
#include <map>

namespace vyx {

// IR Regression Guard — D4 / PLAN_IR_REGRESSION_GUARD.md §3.3
// 强制贯穿全优化级别的 verifier：codegen 完成紧接着跑 1 次（已有），优化结束后跑
// 第 2 次（D4 新增）；O0 路径不再静默短路，依然跑这 2 次 verify。
static bool VyxRunModuleVerify(llvm::Module& M, DiagnosticsEngine& diag, const char* phase) {
    std::string verifyErrors;
    llvm::raw_string_ostream verifyStream(verifyErrors);
    if (llvm::verifyModule(M, &verifyStream)) {
        verifyStream.flush();
        // 同时打印到 errs() 满足 D4 "不能静默" 要求；diag.error 同步入库以
        // 保证 main 的 hasErrors() 短路链路。
        llvm::errs() << "[ir-guard] verifyModule failed at " << phase << ":\n"
                     << verifyErrors.substr(0, 2000) << "\n";
        diag.error(CodeGen::codegenInternalSourceLocation(),
                   "module verification failed at {}:\n{}", phase,
                   verifyErrors.substr(0, 2000));
        return false;
    }
    return true;
}

void CodeGen::optimize(int level) {
    if (!VyxRunModuleVerify(*module_, diag_, "before-optimize")) {
        return;
    }

    llvm::OptimizationLevel optLevel;
    switch (level) {
        case 0:  optLevel = llvm::OptimizationLevel::O0; break;
        case 1:  optLevel = llvm::OptimizationLevel::O1; break;
        case 3:  optLevel = llvm::OptimizationLevel::O3; break;
        case 4:  optLevel = llvm::OptimizationLevel::Os; break;
        case 5:  optLevel = llvm::OptimizationLevel::Oz; break;
        default: optLevel = llvm::OptimizationLevel::O2; break;
    }

    if (optLevel == llvm::OptimizationLevel::O0) {
        // D4: O0 不跑 pass，但仍贯穿"二次 verify"（前后各一次）；
        // pre-D4 的静默 return 在此被替换为显式 verify。
        VyxRunModuleVerify(*module_, diag_, "after-optimize-O0-noop");
        return;
    }

    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86Target();
    LLVMInitializeX86TargetMC();
    LLVMInitializeX86AsmParser();
    LLVMInitializeX86AsmPrinter();
    LLVMInitializeAArch64TargetInfo();
    LLVMInitializeAArch64Target();
    LLVMInitializeAArch64TargetMC();
    LLVMInitializeAArch64AsmParser();
    LLVMInitializeAArch64AsmPrinter();

    auto targetTriple = module_->getTargetTriple();

    std::string error;
    auto target = llvm::TargetRegistry::lookupTarget(targetTriple, error);
    std::unique_ptr<llvm::TargetMachine> TM;
    if (target) {
        auto cpu = llvm::sys::getHostCPUName();
        llvm::SubtargetFeatures features;
        auto hostFeatures = llvm::sys::getHostCPUFeatures();
        for (auto& f : hostFeatures)
            features.AddFeature(f.first(), f.second);
        llvm::TargetOptions opt;
        auto rm = std::optional<llvm::Reloc::Model>();
        TM.reset(target->createTargetMachine(targetTriple, cpu, features.getString(), opt, rm));
        module_->setDataLayout(TM->createDataLayout());
    }

    llvm::LoopAnalysisManager LAM;
    llvm::FunctionAnalysisManager FAM;
    llvm::CGSCCAnalysisManager CGAM;
    llvm::ModuleAnalysisManager MAM;

    llvm::PassBuilder PB(TM.get());
    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

    auto MPM = PB.buildPerModuleDefaultPipeline(optLevel);
    MPM.run(*module_, MAM);

    // D4: 优化结束后的第二次 verify。任何 pass 把 IR 弄畸形都会在这里被抓。
    VyxRunModuleVerify(*module_, diag_, "after-optimize");
}

bool CodeGen::referencesLlvmCRuntime() const {
    // P3-B3: scan the in-memory module for any *declaration* (no body) whose
    // mangled name starts with "LLVM". `extern "C"` keeps these names
    // unmangled, so this matches LLVMContextCreate / LLVMModuleCreate* / etc.
    // Definitions are skipped — we only care about unresolved-at-link
    // references that need LLVM-C.lib to satisfy. Globals are also checked
    // for completeness (e.g. LLVMVerifierFailureAction-style enum tables).
    for (auto& F : module_->functions()) {
        if (!F.isDeclaration()) continue;
        auto name = F.getName();
        if (name.starts_with("LLVM") && !name.starts_with("llvm.")) return true;
    }
    for (auto& G : module_->globals()) {
        if (!G.isDeclaration()) continue;
        auto name = G.getName();
        if (name.starts_with("LLVM")) return true;
    }
    return false;
}

bool CodeGen::emitIR(const std::string& filename) {
    // D4: 写盘前再 verify 一次，对齐 emitObject 的强约束。
    if (!VyxRunModuleVerify(*module_, diag_, "before-emit-ir")) {
        return false;
    }
    std::error_code ec;
    llvm::raw_fd_ostream out(filename, ec, llvm::sys::fs::OF_None);
    if (ec) {
        diag_.error(CodeGen::codegenInternalSourceLocation(), "failed to open output file '{}': {}", filename, ec.message());
        return false;
    }
    module_->print(out, nullptr);
    return true;
}

bool CodeGen::emitObject(const std::string& filename) {
    // Strict error model: if any soft-mode type lookup fell back to an
    // opaque pointer/i64/i32 placeholder, the module's IR is poisoned even
    // if the diagnostic stream happens to be empty. Force a single
    // aggregate error so the driver's `diag.hasErrors()` check skips
    // linking — we never want to hand the OS an object file that was
    // compiled around an unresolved type.
    if (hadHardTypeError_ && !diag_.hasErrors()) {
        diag_.error(CodeGen::codegenInternalSourceLocation(),
            "codegen: one or more types could not be lowered (soft-mode fallback "
            "was exercised); see prior diagnostics");
    }
    std::string verifyErrors;
    llvm::raw_string_ostream verifyStream(verifyErrors);
    if (llvm::verifyModule(*module_, &verifyStream)) {
        diag_.error(CodeGen::codegenInternalSourceLocation(), "module verification failed before emitObject:\n{}",
                    verifyErrors.substr(0, 2000));
        return false;
    }

    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86Target();
    LLVMInitializeX86TargetMC();
    LLVMInitializeX86AsmParser();
    LLVMInitializeX86AsmPrinter();

    LLVMInitializeAArch64TargetInfo();
    LLVMInitializeAArch64Target();
    LLVMInitializeAArch64TargetMC();
    LLVMInitializeAArch64AsmParser();
    LLVMInitializeAArch64AsmPrinter();

    auto targetTriple = module_->getTargetTriple();

    std::string error;
    auto target = llvm::TargetRegistry::lookupTarget(targetTriple, error);
    if (!target) {
        diag_.error(CodeGen::codegenInternalSourceLocation(), "target lookup failed: {}", error);
        return false;
    }

    auto cpu = llvm::sys::getHostCPUName();
    llvm::SubtargetFeatures featSet;
    auto hostFeat = llvm::sys::getHostCPUFeatures();
    for (auto& f : hostFeat)
        featSet.AddFeature(f.first(), f.second);
    llvm::TargetOptions opt;
    auto rm = std::optional<llvm::Reloc::Model>();
    std::unique_ptr<llvm::TargetMachine> targetMachine(
        target->createTargetMachine(targetTriple, cpu, featSet.getString(), opt, rm));

    module_->setDataLayout(targetMachine->createDataLayout());

    std::error_code ec;
    llvm::raw_fd_ostream dest(filename, ec, llvm::sys::fs::OF_None);
    if (ec) {
        diag_.error(CodeGen::codegenInternalSourceLocation(), "failed to open output file '{}': {}", filename, ec.message());
        return false;
    }

    llvm::legacy::PassManager pm;
    if (targetMachine->addPassesToEmitFile(pm, dest, nullptr, llvm::CodeGenFileType::ObjectFile)) {
        diag_.error(CodeGen::codegenInternalSourceLocation(), "target machine cannot emit object file");
        return false;
    }

    pm.run(*module_);
    dest.flush();
    return true;
}

std::string CodeGen::msvcMangleType(const std::string& t) {
    if (t == "i32") return "H"; if (t == "i64") return "_J";
    if (t == "i16") return "F"; if (t == "i8") return "C";
    if (t == "u32") return "I"; if (t == "u64") return "_K";
    if (t == "u16") return "G"; if (t == "u8") return "E";
    if (t == "f32") return "M"; if (t == "f64") return "N";
    if (t == "bool") return "_N"; if (t == "void") return "X";
    if (t == "char") return "D";
    if (t == "rawptr") return "PEAX"; if (t == "str" || t == "string") return "PEAD";
    return "H";
}

std::string CodeGen::itaniumMangleType(const std::string& t) {
    if (t == "i32") return "i"; if (t == "i64") return "l";
    if (t == "i16") return "s"; if (t == "i8") return "c";
    if (t == "u32") return "j"; if (t == "u64") return "m";
    if (t == "u16") return "t"; if (t == "u8") return "h";
    if (t == "f32") return "f"; if (t == "f64") return "d";
    if (t == "bool") return "b"; if (t == "void") return "v";
    if (t == "char") return "c";
    if (t == "rawptr") return "Pv"; if (t == "str" || t == "string") return "Pc";
    return "i";
}

bool CodeGen::usesMsvcAbi() const {
    auto& triple = module_->getTargetTriple();
    return triple.getTriple().empty() ||
        (triple.isOSWindows() && triple.getEnvironment() != llvm::Triple::GNU &&
         triple.getEnvironment() != llvm::Triple::Cygnus);
}

// ============================================================
//  Compile-time evaluation for comptime fn
// ============================================================

CodeGen::ComptimeVal CodeGen::comptimeEvalExpr(const Expr& expr,
    std::map<std::string, ComptimeVal>& env) {
    ComptimeVal result;
    if (++comptimeDepth_ > MaxComptimeDepth) { --comptimeDepth_; return result; }
    struct DepthGuard { int& d; ~DepthGuard() { --d; } } guard{comptimeDepth_};
    switch (expr.kind) {
        case ExprKind::IntLiteral:
            result.kind = ComptimeVal::Int;
            result.intVal = expr.as<IntLiteralExpr>()->value;
            return result;
        case ExprKind::FloatLiteral:
            result.kind = ComptimeVal::Float;
            result.floatVal = expr.as<FloatLiteralExpr>()->value;
            return result;
        case ExprKind::BoolLiteral:
            result.kind = ComptimeVal::Bool;
            result.boolVal = expr.as<BoolLiteralExpr>()->value;
            return result;
        case ExprKind::StringLiteral:
            result.kind = ComptimeVal::Str;
            result.strVal = std::string(expr.as<StringLiteralExpr>()->value);
            return result;
        case ExprKind::Identifier: {
            auto it = env.find(expr.as<IdentifierExpr>()->name);
            if (it != env.end()) return it->second;
            return result;
        }
        case ExprKind::BinaryOp: {
            auto* bin = expr.as<BinaryOpExpr>();
            if (!bin->lhs || !bin->rhs) return result;
            auto l = comptimeEvalExpr(*bin->lhs, env);
            auto r = comptimeEvalExpr(*bin->rhs, env);
            if (l.kind == ComptimeVal::Int && r.kind == ComptimeVal::Int) {
                result.kind = ComptimeVal::Int;
                switch (bin->op) {
                    case BinaryOp::Add: result.intVal = l.intVal + r.intVal; break;
                    case BinaryOp::Sub: result.intVal = l.intVal - r.intVal; break;
                    case BinaryOp::Mul: result.intVal = l.intVal * r.intVal; break;
                    case BinaryOp::Div: result.intVal = r.intVal ? l.intVal / r.intVal : 0; break;
                    case BinaryOp::Mod: result.intVal = r.intVal ? l.intVal % r.intVal : 0; break;
                    case BinaryOp::Eq: result.kind = ComptimeVal::Bool; result.boolVal = l.intVal == r.intVal; break;
                    case BinaryOp::Neq: result.kind = ComptimeVal::Bool; result.boolVal = l.intVal != r.intVal; break;
                    case BinaryOp::Lt: result.kind = ComptimeVal::Bool; result.boolVal = l.intVal < r.intVal; break;
                    case BinaryOp::Gt: result.kind = ComptimeVal::Bool; result.boolVal = l.intVal > r.intVal; break;
                    case BinaryOp::Lte: result.kind = ComptimeVal::Bool; result.boolVal = l.intVal <= r.intVal; break;
                    case BinaryOp::Gte: result.kind = ComptimeVal::Bool; result.boolVal = l.intVal >= r.intVal; break;
                    default: result.kind = ComptimeVal::None; break;
                }
            }
            if (l.kind == ComptimeVal::Bool && r.kind == ComptimeVal::Bool) {
                result.kind = ComptimeVal::Bool;
                if (bin->op == BinaryOp::And) result.boolVal = l.boolVal && r.boolVal;
                else if (bin->op == BinaryOp::Or) result.boolVal = l.boolVal || r.boolVal;
            }
            return result;
        }
        case ExprKind::UnaryOp: {
            auto* un = expr.as<UnaryOpExpr>();
            if (!un->operand) return result;
            auto v = comptimeEvalExpr(*un->operand, env);
            if (v.kind == ComptimeVal::Int && un->op == UnaryOp::Neg) {
                result.kind = ComptimeVal::Int;
                result.intVal = -v.intVal;
            }
            if (v.kind == ComptimeVal::Bool && un->op == UnaryOp::Not) {
                result.kind = ComptimeVal::Bool;
                result.boolVal = !v.boolVal;
            }
            return result;
        }
        case ExprKind::Call: {
            auto* call = expr.as<CallExpr>();
            if (!call->callee || call->callee->kind != ExprKind::Identifier) return result;
            auto& name = call->callee->as<IdentifierExpr>()->name;
            auto cfIt = comptimeDecls_.find(name);
            if (cfIt == comptimeDecls_.end()) return result;
            auto* fnDecl = cfIt->second->as<FunctionDecl>();
            std::map<std::string, ComptimeVal> callEnv;
            for (size_t i = 0; i < fnDecl->params.size() && i < call->args.size(); ++i) {
                callEnv[fnDecl->params[i].name] = comptimeEvalExpr(*call->args[i], env);
            }
            if (fnDecl->body) return comptimeExecBody(*fnDecl->body, callEnv);
            return result;
        }
        default:
            return result;
    }
}

CodeGen::ComptimeVal CodeGen::comptimeExecBody(const Stmt& body,
    std::map<std::string, ComptimeVal>& env) {
    ComptimeVal result;
    if (body.kind != StmtKind::Block) return result;
    auto* block = body.as<BlockStmt>();
    for (auto& s : block->statements) {
        if (!s) continue;
        if (s->kind == StmtKind::Return) {
            auto* rs = s->as<ReturnStmt>();
            if (rs->expr) return comptimeEvalExpr(*rs->expr, env);
        }
        if (s->kind == StmtKind::VarDecl) {
            auto* vd = s->as<VarDeclStmt>();
            if (vd->initExpr) env[vd->varName] = comptimeEvalExpr(*vd->initExpr, env);
        }
        if (s->kind == StmtKind::If) {
            auto* ifS = s->as<IfStmt>();
            auto cond = ifS->condition ? comptimeEvalExpr(*ifS->condition, env) : ComptimeVal{};
            bool taken = (cond.kind == ComptimeVal::Bool && cond.boolVal) ||
                         (cond.kind == ComptimeVal::Int && cond.intVal != 0);
            if (taken && ifS->thenBranch) {
                auto r = comptimeExecBody(*ifS->thenBranch, env);
                if (r.kind != ComptimeVal::None) return r;
            } else if (!taken && ifS->elseBranch) {
                auto r = comptimeExecBody(*ifS->elseBranch, env);
                if (r.kind != ComptimeVal::None) return r;
            }
        }
        if (s->kind == StmtKind::Block) {
            auto r = comptimeExecBody(*s, env);
            if (r.kind != ComptimeVal::None) return r;
        }
    }
    return result;
}

llvm::Value* CodeGen::tryComptimeCall(const CallExpr& call) {
    if (!call.callee || call.callee->kind != ExprKind::Identifier) return nullptr;
    auto& name = call.callee->as<IdentifierExpr>()->name;
    auto cfIt = comptimeDecls_.find(name);
    if (cfIt == comptimeDecls_.end() || !cfIt->second) return nullptr;

    auto* fnDecl = cfIt->second->as<FunctionDecl>();
    if (!fnDecl || !fnDecl->body) return nullptr;

    std::map<std::string, ComptimeVal> env;
    bool allConst = true;
    for (size_t i = 0; i < fnDecl->params.size() && i < call.args.size(); ++i) {
        if (!call.args[i]) { allConst = false; break; }
        auto v = comptimeEvalExpr(*call.args[i], env);
        if (v.kind == ComptimeVal::None) { allConst = false; break; }
        env[fnDecl->params[i].name] = v;
    }
    if (!allConst) return nullptr;

    auto result = comptimeExecBody(*fnDecl->body, env);
    if (result.kind == ComptimeVal::Int)
        return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), result.intVal);
    if (result.kind == ComptimeVal::Bool)
        return llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context_), result.boolVal ? 1 : 0);
    if (result.kind == ComptimeVal::Float)
        return llvm::ConstantFP::get(llvm::Type::getDoubleTy(*context_), result.floatVal);
    if (result.kind == ComptimeVal::Str)
        return getOrCreateString(result.strVal);
    return nullptr;
}

} // namespace vyx
