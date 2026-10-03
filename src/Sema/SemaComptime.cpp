#include "Sema.h"
#include "SemaCloneHelpers.h"
#include "../Common/StringUtils.h"
#include <algorithm>
#include <functional>
#include <map>

namespace vyx {
// ============================================================
//  Compile-time evaluation
// ============================================================

Sema::ComptimeValue Sema::comptimeEval(const Expr& expr,
    const std::map<std::string, ComptimeValue>& env) {
    ComptimeValue result;

    switch (expr.kind) {
        case ExprKind::IntLiteral:
            result.kind = ComptimeValue::Int;
            result.intVal = expr.as<IntLiteralExpr>()->value;
            return result;
        case ExprKind::FloatLiteral:
            result.kind = ComptimeValue::Float;
            result.floatVal = expr.as<FloatLiteralExpr>()->value;
            return result;
        case ExprKind::BoolLiteral:
            result.kind = ComptimeValue::Bool;
            result.boolVal = expr.as<BoolLiteralExpr>()->value;
            return result;
        case ExprKind::StringLiteral:
            result.kind = ComptimeValue::String;
            result.strVal = expr.as<StringLiteralExpr>()->value;
            return result;
        case ExprKind::Identifier: {
            auto it = env.find(expr.as<IdentifierExpr>()->name);
            if (it != env.end()) return it->second;
            return result;
        }
        case ExprKind::BinaryOp: {
            auto* bin = expr.as<BinaryOpExpr>();
            if (!bin->lhs || !bin->rhs) return result;
            auto l = comptimeEval(*bin->lhs, env);
            auto r = comptimeEval(*bin->rhs, env);
            if (l.kind == ComptimeValue::Int && r.kind == ComptimeValue::Int) {
                result.kind = ComptimeValue::Int;
                switch (bin->op) {
                    case BinaryOp::Add: result.intVal = l.intVal + r.intVal; break;
                    case BinaryOp::Sub: result.intVal = l.intVal - r.intVal; break;
                    case BinaryOp::Mul: result.intVal = l.intVal * r.intVal; break;
                    case BinaryOp::Div:
                        if (r.intVal == 0) { diag_.error(expr.location, "compile-time division by zero"); result.intVal = 0; }
                        else result.intVal = l.intVal / r.intVal;
                        break;
                    case BinaryOp::Mod:
                        if (r.intVal == 0) { diag_.error(expr.location, "compile-time modulo by zero"); result.intVal = 0; }
                        else result.intVal = l.intVal % r.intVal;
                        break;
                    case BinaryOp::Eq:  result.kind = ComptimeValue::Bool; result.boolVal = (l.intVal == r.intVal); break;
                    case BinaryOp::Neq: result.kind = ComptimeValue::Bool; result.boolVal = (l.intVal != r.intVal); break;
                    case BinaryOp::Lt:  result.kind = ComptimeValue::Bool; result.boolVal = (l.intVal < r.intVal); break;
                    case BinaryOp::Lte: result.kind = ComptimeValue::Bool; result.boolVal = (l.intVal <= r.intVal); break;
                    case BinaryOp::Gt:  result.kind = ComptimeValue::Bool; result.boolVal = (l.intVal > r.intVal); break;
                    case BinaryOp::Gte: result.kind = ComptimeValue::Bool; result.boolVal = (l.intVal >= r.intVal); break;
                    default: break;
                }
            }
            return result;
        }
        case ExprKind::UnaryOp: {
            auto* un = expr.as<UnaryOpExpr>();
            if (!un->operand) return result;
            auto v = comptimeEval(*un->operand, env);
            if (v.kind == ComptimeValue::Int && un->op == UnaryOp::Neg) {
                result.kind = ComptimeValue::Int;
                result.intVal = -v.intVal;
            } else if (v.kind == ComptimeValue::Bool && un->op == UnaryOp::Not) {
                result.kind = ComptimeValue::Bool;
                result.boolVal = !v.boolVal;
            }
            return result;
        }
        case ExprKind::Call: {
            auto* call = expr.as<CallExpr>();
            if (!call->callee || call->callee->kind != ExprKind::Identifier) return result;
            auto cfIt = comptimeFunctions_.find(call->callee->as<IdentifierExpr>()->name);
            if (cfIt == comptimeFunctions_.end()) return result;
            const Decl* fn = cfIt->second;
            auto* fnDecl = fn->as<FunctionDecl>();
            std::map<std::string, ComptimeValue> callEnv;
            for (size_t i = 0; i < fnDecl->params.size() && i < call->args.size(); ++i) {
                callEnv[fnDecl->params[i].name] = comptimeEval(*call->args[i], env);
            }
            if (fnDecl->body) return comptimeExecBody(*fnDecl->body, callEnv);
            return result;
        }
        default:
            return result;
    }
}

Sema::ComptimeValue Sema::comptimeExecBody(const Stmt& body,
    std::map<std::string, ComptimeValue>& env) {
    ComptimeValue result;

    if (body.kind == StmtKind::Block) {
        auto* block = body.as<BlockStmt>();
        for (auto& s : block->statements) {
            if (!s) continue;
            if (s->kind == StmtKind::Return) {
                auto* rs = s->as<ReturnStmt>();
                if (rs->expr) return comptimeEval(*rs->expr, env);
            }
            if (s->kind == StmtKind::VarDecl) {
                auto* vd = s->as<VarDeclStmt>();
                if (vd->initExpr) env[vd->varName] = comptimeEval(*vd->initExpr, env);
            }
            if (s->kind == StmtKind::If) {
                auto* ifS = s->as<IfStmt>();
                auto cond = ifS->condition ? comptimeEval(*ifS->condition, env) : ComptimeValue{};
                bool taken = (cond.kind == ComptimeValue::Bool && cond.boolVal) ||
                             (cond.kind == ComptimeValue::Int && cond.intVal != 0);
                if (taken && ifS->thenBranch) {
                    auto r = comptimeExecBody(*ifS->thenBranch, env);
                    if (r.kind != ComptimeValue::None) return r;
                } else if (!taken && ifS->elseBranch) {
                    auto r = comptimeExecBody(*ifS->elseBranch, env);
                    if (r.kind != ComptimeValue::None) return r;
                }
            }
            if (s->kind == StmtKind::ExprStmt) {
                auto* es = s->as<ExprStmt>();
                if (es->expr) result = comptimeEval(*es->expr, env);
            }
        }
    } else if (body.kind == StmtKind::Return) {
        auto* rs = body.as<ReturnStmt>();
        if (rs->expr) return comptimeEval(*rs->expr, env);
    }

    return result;
}

} // namespace vyx