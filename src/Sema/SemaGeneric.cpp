#include "Sema.h"
#include "SemaCloneHelpers.h"
#include "../Common/StringUtils.h"
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <set>
#include <map>

namespace vyx {

// ---------------------------------------------------------------------------
// substConstIdentsInStmt / substConstIdentsInExpr
//
// Walks a Stmt/Expr tree (post-clone) and replaces every IdentifierExpr whose
// name is a key in `cpBindings` with a freshly-synthesized IntLiteralExpr.
// Used by instantiateTemplateIfNeeded so `return N;` in a body of
// `fn f<const N: i64>()` lowers to `return <literal>;` before CodeGen sees it.
//
// Structural mutation: we replace the ExprPtr owned by the parent node.
// Only the expression kinds that commonly hold sub-expressions are covered —
// literals / self / null etc. are leaves.
// ---------------------------------------------------------------------------
static void substConstIdentsInExpr(ExprPtr& e,
                                   const std::map<std::string, int64_t>& cp);
static void substConstIdentsInStmt(StmtPtr& s,
                                   const std::map<std::string, int64_t>& cp);

// Forward-declared here so substConstIdents* can fold const-param names
// that appear inside type annotations (VarDecl types, Cast targets,
// StructInit typeAnnotation, Identifier/MemberAccess turbofish args, etc.).
// Definition lives further down in this file.
static TypePtr rewriteConstSizesInType(
        const TypeAnnotation* src,
        const std::map<std::string, int64_t>& constBindings);

// Wrapper that rewrites a TypePtr slot in-place using rewriteConstSizesInType.
static void substConstInTypeAnn(TypePtr& t,
                                const std::map<std::string, int64_t>& cp)
{
    if (!t || cp.empty()) return;
    auto rewritten = rewriteConstSizesInType(t.get(), cp);
    if (rewritten) t = std::move(rewritten);
}

static void substConstIdentsInExpr(ExprPtr& e,
                                   const std::map<std::string, int64_t>& cp)
{
    if (!e) return;
    switch (e->kind) {
        case ExprKind::Identifier: {
            auto* id = e->as<IdentifierExpr>();
            auto it = cp.find(id->name);
            if (it != cp.end()) {
                auto lit = std::make_unique<IntLiteralExpr>();
                lit->location = e->location;
                lit->value = it->second;
                e = std::move(lit);
                return;
            }
            // Rewrite const-param names inside each turbofish type arg
            // (e.g. `inner::<Array<T, N>>` needs `N` → bound literal so the
            // callee is resolved against the right instantiation).
            for (auto& ta : id->callTypeArgs) substConstInTypeAnn(ta, cp);
            substConstInTypeAnn(id->typeAnnotation, cp);
            // If the identifier carries turbofish const args (e.g.
            // `inner::<N>` inside a body where N is a caller's const param),
            // walk those too — each callArgExprs[i] may itself be an
            // IdentifierExpr referencing a const param we need to rewrite.
            // When a callArgExprs slot becomes an IntLit, also null out the
            // corresponding callTypeArgs slot so the Sema turbofish resolver
            // treats it as a pure const-arg position rather than trying to
            // resolve `N` as a type.
            for (size_t i = 0; i < id->callArgExprs.size(); ++i) {
                auto& ae = id->callArgExprs[i];
                substConstIdentsInExpr(ae, cp);
                if (ae && ae->kind == ExprKind::IntLiteral &&
                    i < id->callTypeArgs.size()) {
                    id->callTypeArgs[i].reset();
                }
            }
            return;
        }
        case ExprKind::BinaryOp: {
            auto* b = e->as<BinaryOpExpr>();
            substConstIdentsInExpr(b->lhs, cp);
            substConstIdentsInExpr(b->rhs, cp);
            return;
        }
        case ExprKind::UnaryOp: {
            auto* u = e->as<UnaryOpExpr>();
            substConstIdentsInExpr(u->operand, cp);
            return;
        }
        case ExprKind::Call: {
            auto* c = e->as<CallExpr>();
            substConstIdentsInExpr(c->callee, cp);
            for (auto& a : c->args) substConstIdentsInExpr(a, cp);
            return;
        }
        case ExprKind::MemberAccess: {
            auto* m = e->as<MemberAccessExpr>();
            substConstIdentsInExpr(m->object, cp);
            for (auto& ta : m->callTypeArgs) substConstInTypeAnn(ta, cp);
            return;
        }
        case ExprKind::Index: {
            auto* ix = e->as<IndexExpr>();
            substConstIdentsInExpr(ix->object, cp);
            substConstIdentsInExpr(ix->indexExpr, cp);
            return;
        }
        case ExprKind::Assignment: {
            auto* a = e->as<AssignmentExpr>();
            substConstIdentsInExpr(a->lhs, cp);
            substConstIdentsInExpr(a->rhs, cp);
            return;
        }
        case ExprKind::CompoundAssignment: {
            auto* a = e->as<CompoundAssignmentExpr>();
            substConstIdentsInExpr(a->target, cp);
            substConstIdentsInExpr(a->value, cp);
            return;
        }
        case ExprKind::Cast: {
            auto* c = e->as<CastExpr>();
            substConstIdentsInExpr(c->operand, cp);
            substConstInTypeAnn(c->targetType, cp);
            return;
        }
        case ExprKind::Ternary: {
            auto* t = e->as<TernaryExpr>();
            substConstIdentsInExpr(t->condition, cp);
            substConstIdentsInExpr(t->trueExpr, cp);
            substConstIdentsInExpr(t->falseExpr, cp);
            return;
        }
        case ExprKind::ArrayInit: {
            auto* a = e->as<ArrayInitExpr>();
            for (auto& el : a->elements) substConstIdentsInExpr(el, cp);
            // Repeat-count `[v; N]` — N may be a const-generic param that
            // must be folded into the matching integer literal before Sema
            // analyses the array literal's size.
            if (a->repeatCount) substConstIdentsInExpr(a->repeatCount, cp);
            return;
        }
        case ExprKind::TupleInit: {
            auto* t = e->as<TupleInitExpr>();
            for (auto& el : t->elements) substConstIdentsInExpr(el, cp);
            return;
        }
        case ExprKind::StringInterpolation: {
            auto* si = e->as<StringInterpExpr>();
            for (auto& part : si->parts)
                if (part.expr) substConstIdentsInExpr(part.expr, cp);
            return;
        }
        case ExprKind::StructInit: {
            auto* si = e->as<StructInitExpr>();
            for (auto& [_, fv] : si->fieldInits) substConstIdentsInExpr(fv, cp);
            if (si->spreadBase) substConstIdentsInExpr(si->spreadBase, cp);
            // `Array::<T, N> { ... }` — the Generic type annotation carries
            // `N` in typeArgs; rewrite it to the bound integer so Sema can
            // select the correct concrete instantiation.
            substConstInTypeAnn(si->typeAnnotation, cp);
            // The parser bakes the turbofish args into `structName` as a
            // mangled string like "Array<T,N>". The Sema-path method-body
            // clone (cloneExprWithSubst) rewrites typeAnnotation but leaves
            // structName untouched, so when const-generic params land in
            // that string we must rewrite matching tokens to their integer
            // values here. (Type params are separately rewritten at Sema
            // analysis time via the typeAnnotation path.)
            if (!si->structName.empty() &&
                si->structName.find('<') != std::string::npos) {
                std::string& s = si->structName;
                std::string out;
                out.reserve(s.size());
                size_t i = 0;
                bool changed = false;
                while (i < s.size()) {
                    char c = s[i];
                    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_') {
                        size_t j = i;
                        while (j < s.size() &&
                               ((s[j] >= 'A' && s[j] <= 'Z') ||
                                (s[j] >= 'a' && s[j] <= 'z') ||
                                (s[j] >= '0' && s[j] <= '9') ||
                                s[j] == '_')) ++j;
                        std::string token(s, i, j - i);
                        auto it2 = cp.find(token);
                        if (it2 != cp.end()) {
                            out += std::to_string(it2->second);
                            changed = true;
                        } else {
                            out += token;
                        }
                        i = j;
                    } else {
                        out.push_back(c);
                        ++i;
                    }
                }
                if (changed) si->structName = std::move(out);
            }
            return;
        }
        default:
            return;
    }
}

// Forward-declare (defined later in this file) so tryEvalConstBool can use it.
bool evalConstExprInt(const Expr* expr,
                      const std::map<std::string, int64_t>& env,
                      int64_t& out);

// Returns true if `s` unconditionally transfers control (Return/Break/Continue/
// Throw), or is a Block whose last statement does so, or an If whose both
// branches do so. Used to prune dead code after const-if folding so that
// `if (TRUE) { return … }` doesn't leave a still-analysed unreachable tail.
static bool stmtUnconditionallyExits(const Stmt* s) {
    if (!s) return false;
    switch (s->kind) {
        case StmtKind::Return:   return true;
        case StmtKind::Break:    return true;
        case StmtKind::Continue: return true;
        case StmtKind::Block: {
            auto* b = s->as<BlockStmt>();
            if (b->statements.empty()) return false;
            return stmtUnconditionallyExits(b->statements.back().get());
        }
        case StmtKind::If: {
            auto* ifs = s->as<IfStmt>();
            if (!ifs->elseBranch) return false;
            return stmtUnconditionallyExits(ifs->thenBranch.get()) &&
                   stmtUnconditionallyExits(ifs->elseBranch.get());
        }
        default: return false;
    }
}

// Evaluate a substituted expression tree to a bool, if it reduces to a pure
// const (IntLit / BoolLit / comparison or logical op of pure consts). Returns
// true iff the expression is fully known; `out` gets the truth value.
static bool tryEvalConstBool(const Expr* e, bool& out) {
    if (!e) return false;
    switch (e->kind) {
        case ExprKind::BoolLiteral:
            out = e->as<BoolLiteralExpr>()->value;
            return true;
        case ExprKind::IntLiteral:
            out = e->as<IntLiteralExpr>()->value != 0;
            return true;
        case ExprKind::UnaryOp: {
            auto* u = e->as<UnaryOpExpr>();
            if (u->op != UnaryOp::Not) return false;
            bool v; if (!tryEvalConstBool(u->operand.get(), v)) return false;
            out = !v; return true;
        }
        case ExprKind::BinaryOp: {
            auto* b = e->as<BinaryOpExpr>();
            std::map<std::string, int64_t> emptyEnv;
            int64_t lv = 0, rv = 0;
            bool lok = evalConstExprInt(b->lhs.get(), emptyEnv, lv);
            bool rok = evalConstExprInt(b->rhs.get(), emptyEnv, rv);
            if (lok && rok) {
                switch (b->op) {
                    case BinaryOp::Eq:  out = lv == rv; return true;
                    case BinaryOp::Neq: out = lv != rv; return true;
                    case BinaryOp::Lt:  out = lv <  rv; return true;
                    case BinaryOp::Lte: out = lv <= rv; return true;
                    case BinaryOp::Gt:  out = lv >  rv; return true;
                    case BinaryOp::Gte: out = lv >= rv; return true;
                    default: break;
                }
            }
            if (b->op == BinaryOp::And || b->op == BinaryOp::Or) {
                bool lb, rb;
                if (!tryEvalConstBool(b->lhs.get(), lb)) return false;
                if (!tryEvalConstBool(b->rhs.get(), rb)) return false;
                out = (b->op == BinaryOp::And) ? (lb && rb) : (lb || rb);
                return true;
            }
            return false;
        }
        default: return false;
    }
}

static void substConstIdentsInStmt(StmtPtr& s,
                                   const std::map<std::string, int64_t>& cp)
{
    if (!s) return;
    switch (s->kind) {
        case StmtKind::Block: {
            auto* b = s->as<BlockStmt>();
            for (size_t i = 0; i < b->statements.size(); ++i) {
                substConstIdentsInStmt(b->statements[i], cp);
                // After folding, if this statement unconditionally exits,
                // the rest of the block is dead. Prune it so Sema doesn't
                // walk unreachable recursive calls.
                if (stmtUnconditionallyExits(b->statements[i].get())) {
                    b->statements.resize(i + 1);
                    break;
                }
            }
            return;
        }
        case StmtKind::ExprStmt: {
            auto* es = s->as<ExprStmt>();
            substConstIdentsInExpr(es->expr, cp);
            return;
        }
        case StmtKind::VarDecl: {
            auto* vd = s->as<VarDeclStmt>();
            substConstIdentsInExpr(vd->initExpr, cp);
            // `var a: [N]T = ...` — rewrite N → literal in the annotation
            // so Sema picks the right fixed-array size and element type.
            substConstInTypeAnn(vd->varType, cp);
            return;
        }
        case StmtKind::Return: {
            auto* rs = s->as<ReturnStmt>();
            substConstIdentsInExpr(rs->expr, cp);
            return;
        }
        case StmtKind::If: {
            auto* ifs = s->as<IfStmt>();
            substConstIdentsInExpr(ifs->condition, cp);
            substConstIdentsInStmt(ifs->thenBranch, cp);
            substConstIdentsInStmt(ifs->elseBranch, cp);
            // Const-if folding: if the (now-substituted) condition reduces
            // to a pure constant, replace the whole If with just the taken
            // branch. This is `if constexpr` for const-generic-driven code,
            // and it prevents Sema from walking unreachable recursive
            // template instantiations (fac<N=0> → fac<N=-1> → ...).
            bool cv;
            if (tryEvalConstBool(ifs->condition.get(), cv)) {
                if (cv) {
                    s = std::move(ifs->thenBranch);
                } else if (ifs->elseBranch) {
                    s = std::move(ifs->elseBranch);
                } else {
                    auto empty = std::make_unique<BlockStmt>();
                    empty->location = ifs->location;
                    s = std::move(empty);
                }
            }
            return;
        }
        case StmtKind::While: {
            auto* ws = s->as<WhileStmt>();
            substConstIdentsInExpr(ws->condition, cp);
            substConstIdentsInStmt(ws->body, cp);
            return;
        }
        case StmtKind::For: {
            auto* fs = s->as<ForStmt>();
            substConstIdentsInStmt(fs->init, cp);
            substConstIdentsInExpr(fs->condition, cp);
            substConstIdentsInExpr(fs->step, cp);
            substConstIdentsInStmt(fs->body, cp);
            return;
        }
        default:
            return;
    }
}

// ---------------------------------------------------------------------------
// expandReflectForEachInStmt
//
// Post-clone pass for Sema-produced generic function instances. Walks the
// cloned body; whenever it finds `for f in T::fields` or `for m in T::methods`
// whose base refers to a known concrete type in `subst`, replaces the ForEach
// with N sequential blocks — one per field/method — each binding the loop
// variable to a string literal of the member name. Mirrors the same expansion
// that Monomorphize::cloneStmt performs in its own path.
// ---------------------------------------------------------------------------
static StmtPtr expandReflectForEachOnce(
        const ForEachStmt* fe,
        const VyxTypePtr& concrete);

static void lowerTypeReflectInExpr(
        ExprPtr& e,
        const std::map<std::string, VyxTypePtr>& typeSubst,
        DiagnosticsEngine* diag = nullptr,
        const TranslationUnit* unit = nullptr,
        const Sema* sema = nullptr);

static const char* vyxTypeKindToReflectString(VyxTypeKind k) {
    switch (k) {
        case VyxTypeKind::Struct:    return "struct";
        case VyxTypeKind::Class:     return "class";
        case VyxTypeKind::Interface: return "interface";
        case VyxTypeKind::Integer:   return "int";
        case VyxTypeKind::Float:     return "float";
        case VyxTypeKind::Bool:      return "bool";
        case VyxTypeKind::Function:  return "fn";
        default: return "unknown";
    }
}

// P5-traitconst: find a class decl by name in the translation unit so we can
// resolve `T::MAX` (where T is a generic param) to the class's `constMembers`
// binding after T has been substituted to that concrete class.
static const ClassDecl* findClassDeclByName(const TranslationUnit* unit,
                                             const std::string& name)
{
    if (!unit) return nullptr;
    for (const auto& d : unit->declarations) {
        if (!d) continue;
        if (d->kind != DeclKind::Class) continue;
        if (d->name == name) return static_cast<const ClassDecl*>(d.get());
    }
    return nullptr;
}

static void lowerTypeReflectInExpr(
        ExprPtr& e,
        const std::map<std::string, VyxTypePtr>& typeSubst,
        DiagnosticsEngine* diag,
        const TranslationUnit* unit,
        const Sema* sema)
{
    if (!e) return;
    switch (e->kind) {
        case ExprKind::TypeReflect: {
            auto* r = e->as<TypeReflectExpr>();
            auto it = typeSubst.find(r->typeParam);
            if (it == typeSubst.end() || !it->second) return;
            auto& concrete = it->second;
            std::string value;
            if (r->member == "name")      value = concrete->toString();
            else if (r->member == "kind") value = isStringType(*concrete) ? "string" : vyxTypeKindToReflectString(concrete->kind);
            else return;  // fields/methods handled by ForEach pass
            auto lit = std::make_unique<StringLiteralExpr>();
            lit->location = e->location;
            lit->value = value;
            e = std::move(lit);
            return;
        }
        case ExprKind::BinaryOp: {
            auto* b = e->as<BinaryOpExpr>();
            lowerTypeReflectInExpr(b->lhs, typeSubst, diag, unit, sema);
            lowerTypeReflectInExpr(b->rhs, typeSubst, diag, unit, sema);
            return;
        }
        case ExprKind::UnaryOp: {
            auto* u = e->as<UnaryOpExpr>();
            lowerTypeReflectInExpr(u->operand, typeSubst, diag, unit, sema);
            return;
        }
        case ExprKind::Call: {
            auto* c = e->as<CallExpr>();
            lowerTypeReflectInExpr(c->callee, typeSubst, diag, unit, sema);
            for (auto& a : c->args) lowerTypeReflectInExpr(a, typeSubst, diag, unit, sema);
            return;
        }
        case ExprKind::MemberAccess: {
            auto* m = e->as<MemberAccessExpr>();
            // P2D-009 late-catch: `T::bad_member` where T is a substituted type
            // parameter. The parser steers only name/kind/fields/methods into
            // TypeReflectExpr; everything else lands here as MemberAccess on
            // an Identifier that won't resolve at runtime (it's a type, not a
            // value). Emit P2D-009 now and replace with a placeholder string
            // literal so CodeGen doesn't later surface the misleading
            // "undefined variable 'T'" after substitution has already happened.
            if (m->object && m->object->kind == ExprKind::Identifier) {
                auto* objId = m->object->as<IdentifierExpr>();
                const auto& objName = objId->name;
                auto sit = typeSubst.find(objName);
                if (sit != typeSubst.end() && sit->second) {
                    // This instantiation may still be parameterized by an
                    // enclosing generic (for example `V` in
                    // `dict_default_value::<V>()`). There is no concrete
                    // method table to inspect yet, so preserve the member
                    // access for the later concrete instantiation instead of
                    // misclassifying a static trait method as reflection.
                    if (sit->second->kind == VyxTypeKind::Generic ||
                        sit->second->kind == VyxTypeKind::Unknown) {
                        return;
                    }
                    // P5-traitconst: try to resolve `T::MAX` by looking up the
                    // const member on the substituted class.
                    const std::string className = sit->second->toString();
                    if (const ClassDecl* cls = findClassDeclByName(unit, className)) {
                        for (const auto& cm : cls->constMembers) {
                            if (cm.name == m->member && cm.defaultValue) {
                                auto clone = cloneExpr(*cm.defaultValue);
                                if (clone) {
                                    clone->location = e->location;
                                    e = std::move(clone);
                                    return;
                                }
                            }
                        }
                    }
                    // Phase 8: static trait method dispatch.
                    // `T::method(...)` where T has a trait bound that declares
                    // `method` as a non-self (static) function. When the
                    // concrete type bound to T has an `impl Trait for
                    // <Concrete>` block (for primitives) or a declared
                    // method named `method`, rewrite the Identifier's name
                    // from "T" to the concrete type's string form so the
                    // downstream MemberAccess dispatches through the
                    // regular static-method mangling (<ConcreteName>.method).
                    //
                    // Leaves reflection keywords untouched: those were
                    // already lowered by the TypeReflectExpr arm above
                    // (parser routing guarantees name/kind/fields/methods
                    // never reach this path as MemberAccess).
                    if (sema && sema->hasStaticMethodForConcreteType(*sit->second, m->member)) {
                        objId->name = className;
                        // Drop any turbofish args that were attached to T
                        // (e.g. `T::<U>`) — they were meant for the
                        // type-parameter, not the concrete type; keeping
                        // them would produce `i32::<U>.method` which isn't
                        // valid.
                        objId->callTypeArgs.clear();
                        objId->callArgExprs.clear();
                        return;
                    }
                    if (diag) {
                        diag->errorCoded("P2D-009", e->location,
                            "unknown reflection member '{}' on type parameter '{}'; "
                            "valid members are 'name', 'kind', 'fields', 'methods'",
                            m->member, objName);
                    }
                    auto lit = std::make_unique<StringLiteralExpr>();
                    lit->location = e->location;
                    lit->value = "";
                    e = std::move(lit);
                    return;
                }
            }
            lowerTypeReflectInExpr(m->object, typeSubst, diag, unit, sema);
            return;
        }
        case ExprKind::Index: {
            auto* ix = e->as<IndexExpr>();
            lowerTypeReflectInExpr(ix->object, typeSubst, diag, unit, sema);
            lowerTypeReflectInExpr(ix->indexExpr, typeSubst, diag, unit, sema);
            return;
        }
        case ExprKind::Assignment: {
            auto* a = e->as<AssignmentExpr>();
            lowerTypeReflectInExpr(a->lhs, typeSubst, diag, unit, sema);
            lowerTypeReflectInExpr(a->rhs, typeSubst, diag, unit, sema);
            return;
        }
        case ExprKind::Cast: {
            auto* c = e->as<CastExpr>();
            lowerTypeReflectInExpr(c->operand, typeSubst, diag, unit, sema);
            return;
        }
        case ExprKind::Ternary: {
            auto* t = e->as<TernaryExpr>();
            lowerTypeReflectInExpr(t->condition, typeSubst, diag, unit, sema);
            lowerTypeReflectInExpr(t->trueExpr, typeSubst, diag, unit, sema);
            lowerTypeReflectInExpr(t->falseExpr, typeSubst, diag, unit, sema);
            return;
        }
        default:
            return;
    }
}

static void lowerTypeReflectInStmt(
        StmtPtr& s,
        const std::map<std::string, VyxTypePtr>& typeSubst,
        DiagnosticsEngine* diag = nullptr,
        const TranslationUnit* unit = nullptr,
        const Sema* sema = nullptr);

static void lowerTypeReflectInStmt(
        StmtPtr& s,
        const std::map<std::string, VyxTypePtr>& typeSubst,
        DiagnosticsEngine* diag,
        const TranslationUnit* unit,
        const Sema* sema)
{
    if (!s) return;
    switch (s->kind) {
        case StmtKind::Block: {
            auto* b = s->as<BlockStmt>();
            for (auto& st : b->statements) lowerTypeReflectInStmt(st, typeSubst, diag, unit, sema);
            return;
        }
        case StmtKind::ExprStmt:
            lowerTypeReflectInExpr(s->as<ExprStmt>()->expr, typeSubst, diag, unit, sema); return;
        case StmtKind::VarDecl:
            lowerTypeReflectInExpr(s->as<VarDeclStmt>()->initExpr, typeSubst, diag, unit, sema); return;
        case StmtKind::Return:
            lowerTypeReflectInExpr(s->as<ReturnStmt>()->expr, typeSubst, diag, unit, sema); return;
        case StmtKind::If: {
            auto* ifs = s->as<IfStmt>();
            lowerTypeReflectInExpr(ifs->condition, typeSubst, diag, unit, sema);
            lowerTypeReflectInStmt(ifs->thenBranch, typeSubst, diag, unit, sema);
            lowerTypeReflectInStmt(ifs->elseBranch, typeSubst, diag, unit, sema);
            return;
        }
        case StmtKind::While: {
            auto* ws = s->as<WhileStmt>();
            lowerTypeReflectInExpr(ws->condition, typeSubst, diag, unit, sema);
            lowerTypeReflectInStmt(ws->body, typeSubst, diag, unit, sema);
            return;
        }
        case StmtKind::For: {
            auto* fs = s->as<ForStmt>();
            lowerTypeReflectInStmt(fs->init, typeSubst, diag, unit, sema);
            lowerTypeReflectInExpr(fs->condition, typeSubst, diag, unit, sema);
            lowerTypeReflectInExpr(fs->step, typeSubst, diag, unit, sema);
            lowerTypeReflectInStmt(fs->body, typeSubst, diag, unit, sema);
            return;
        }
        case StmtKind::ForEach: {
            auto* fe = s->as<ForEachStmt>();
            // Don't lower `T::fields/methods` in collection — that's the
            // ForEach expansion pass's job. Do lower body.
            lowerTypeReflectInStmt(fe->body, typeSubst, diag, unit, sema);
            return;
        }
        default:
            return;
    }
}

static void expandReflectForEachInStmt(
        StmtPtr& s,
        const std::map<std::string, VyxTypePtr>& typeSubst)
{
    if (!s) return;
    switch (s->kind) {
        case StmtKind::Block: {
            auto* b = s->as<BlockStmt>();
            for (auto& st : b->statements) expandReflectForEachInStmt(st, typeSubst);
            return;
        }
        case StmtKind::If: {
            auto* ifs = s->as<IfStmt>();
            expandReflectForEachInStmt(ifs->thenBranch, typeSubst);
            expandReflectForEachInStmt(ifs->elseBranch, typeSubst);
            return;
        }
        case StmtKind::While: {
            auto* ws = s->as<WhileStmt>();
            expandReflectForEachInStmt(ws->body, typeSubst);
            return;
        }
        case StmtKind::For: {
            auto* fs = s->as<ForStmt>();
            expandReflectForEachInStmt(fs->body, typeSubst);
            return;
        }
        case StmtKind::ForEach: {
            auto* fe = s->as<ForEachStmt>();
            if (fe->collection && fe->collection->kind == ExprKind::TypeReflect) {
                auto* re = fe->collection->as<TypeReflectExpr>();
                if (re->member == "fields" || re->member == "methods") {
                    auto it = typeSubst.find(re->typeParam);
                    if (it != typeSubst.end() && it->second) {
                        auto replacement = expandReflectForEachOnce(fe, it->second);
                        if (replacement) {
                            s = std::move(replacement);
                            return;
                        }
                    }
                }
            }
            // Recurse into a non-expanded foreach body.
            expandReflectForEachInStmt(fe->body, typeSubst);
            return;
        }
        default:
            return;
    }
}

static StmtPtr expandReflectForEachOnce(
        const ForEachStmt* fe,
        const VyxTypePtr& concrete)
{
    auto* re = fe->collection->as<TypeReflectExpr>();
    bool isFields = (re->member == "fields");

    std::vector<std::string> names;
    if (isFields) {
        for (auto& f : concrete->fields) names.push_back(f.name);
    } else {
        for (auto& m : concrete->methods) names.push_back(m.name);
    }

    auto blk = std::make_unique<BlockStmt>();
    blk->location = fe->location;
    for (auto& nm : names) {
        auto nameStr = std::make_unique<StringLiteralExpr>();
        nameStr->location = fe->location;
        nameStr->value = nm;

        auto varDecl = std::make_unique<VarDeclStmt>();
        varDecl->location = fe->location;
        varDecl->isConst = true;
        varDecl->varName = fe->varName;
        varDecl->initExpr = std::move(nameStr);

        auto bodyClone = cloneStatement(*fe->body);

        auto innerBlk = std::make_unique<BlockStmt>();
        innerBlk->location = fe->location;
        innerBlk->statements.push_back(std::move(varDecl));
        if (bodyClone) innerBlk->statements.push_back(std::move(bodyClone));
        blk->statements.push_back(std::move(innerBlk));
    }
    return blk;
}

// ---------------------------------------------------------------------------
// rewriteConstSizesInType
//
// Deep-clone a TypeAnnotation tree, replacing every `ArrayType::size` that is
// an IdentifierExpr naming a key in `constBindings` with an IntLiteralExpr
// carrying the bound value. This is necessary in `instantiateClassTemplate`
// because `TemplateResolver::substituteComplexType` passes `0` as the array
// size for any `[N]T` annotation (it has no access to const-parameter values),
// which causes `convertTypeToAnnotation` → `resolveType` to produce a
// zero-element array type.
//
// All other node kinds are plain-cloned (structurally recursive, no side
// effects). The clone is necessary — modifying the template's own AST nodes
// in-place would corrupt subsequent instantiations.
// ---------------------------------------------------------------------------
static TypePtr rewriteConstSizesInType(
        const TypeAnnotation* src,
        const std::map<std::string, int64_t>& constBindings)
{
    if (!src) return nullptr;

    switch (src->kind) {
        case TypeAnnotationKind::Array: {
            auto* at = src->as<ArrayType>();
            auto out = std::make_unique<ArrayType>();
            out->location = src->location;
            out->name     = src->name;
            // Element type: recurse.
            if (at->elementType)
                out->elementType = rewriteConstSizesInType(at->elementType.get(), constBindings);
            // Size expression: if it is an identifier that names a const param,
            // replace with the literal value. If it is already a literal, copy
            // it verbatim. Unknown expr kinds (shouldn't appear in template
            // field types) leave the size as null — resolveType then produces
            // a size-0 array, which downstream CodeGen flags clearly.
            if (at->size) {
                if (at->size->kind == ExprKind::Identifier) {
                    const auto& nm = at->size->as<IdentifierExpr>()->name;
                    auto it = constBindings.find(nm);
                    if (it != constBindings.end()) {
                        // Rewrite to the bound integer literal.
                        auto lit = std::make_unique<IntLiteralExpr>();
                        lit->location = at->size->location;
                        lit->value    = it->second;
                        out->size     = std::move(lit);
                    } else {
                        // Identifier not a const param — keep as a placeholder ident.
                        auto ident = std::make_unique<IdentifierExpr>();
                        ident->location = at->size->location;
                        ident->name     = nm;
                        out->size       = std::move(ident);
                    }
                } else if (at->size->kind == ExprKind::IntLiteral) {
                    // Already concrete — clone the literal directly.
                    auto lit = std::make_unique<IntLiteralExpr>();
                    lit->location = at->size->location;
                    lit->value    = at->size->as<IntLiteralExpr>()->value;
                    out->size     = std::move(lit);
                }
                // Other expr kinds: leave size null.
            }
            return out;
        }
        case TypeAnnotationKind::Named: {
            auto out = std::make_unique<NamedType>();
            out->location = src->location;
            out->name     = src->name;
            return out;
        }
        case TypeAnnotationKind::Pointer: {
            auto* pt = src->as<PointerType>();
            auto out = std::make_unique<PointerType>();
            out->location = src->location;
            out->name     = src->name;
            if (pt->innerType)
                out->innerType = rewriteConstSizesInType(pt->innerType.get(), constBindings);
            return out;
        }
        case TypeAnnotationKind::Reference: {
            auto* rt = src->as<ReferenceType>();
            auto out = std::make_unique<ReferenceType>();
            out->location  = src->location;
            out->name      = src->name;
            out->isMutable = rt->isMutable;
            if (rt->innerType)
                out->innerType = rewriteConstSizesInType(rt->innerType.get(), constBindings);
            return out;
        }
        case TypeAnnotationKind::Generic: {
            auto* gt = src->as<GenericType>();
            auto out = std::make_unique<GenericType>();
            out->location = src->location;
            out->name     = src->name;
            // typeArgs and argExprs must stay aligned (same length).
            // For each slot:
            //   - If typeArgs[i] is a NamedType whose name is a const-generic
            //     param (present in constBindings), emit null into typeArgs and
            //     an IntLiteralExpr with the bound value into argExprs.  This
            //     converts e.g. `Buffer<T, N>` (return-type annotation inside a
            //     class body) to `Buffer<T, [argExpr=16]>` so that
            //     TemplateResolver::substituteType followed by resolveType can
            //     produce the correct mangled name `Buffer<i32,16>`.
            //   - Otherwise clone typeArgs[i] recursively and propagate
            //     the matching argExprs[i] (or null) unchanged.
            for (size_t ai = 0; ai < gt->typeArgs.size(); ++ai) {
                auto& a = gt->typeArgs[ai];
                // Check if this type-arg slot is a const-param NamedType.
                if (a && a->kind == TypeAnnotationKind::Named) {
                    auto it = constBindings.find(a->name);
                    if (it != constBindings.end()) {
                        // Const param: null in typeArgs, IntLiteral in argExprs.
                        out->typeArgs.push_back(nullptr);
                        auto lit = std::make_unique<IntLiteralExpr>();
                        lit->location = a->location;
                        lit->value    = it->second;
                        out->argExprs.push_back(std::move(lit));
                        continue;
                    }
                }
                out->typeArgs.push_back(rewriteConstSizesInType(a.get(), constBindings));
                // Propagate any existing argExpr for this slot.
                if (ai < gt->argExprs.size() && gt->argExprs[ai]) {
                    if (gt->argExprs[ai]->kind == ExprKind::IntLiteral) {
                        auto lit = std::make_unique<IntLiteralExpr>();
                        lit->location = gt->argExprs[ai]->location;
                        lit->value    = gt->argExprs[ai]->as<IntLiteralExpr>()->value;
                        out->argExprs.push_back(std::move(lit));
                    } else {
                        out->argExprs.push_back(nullptr);
                    }
                } else {
                    out->argExprs.push_back(nullptr);
                }
            }
            // If the original argExprs has more entries than typeArgs (can
            // happen if the caller passed const values without matching
            // typeArgs slots), append them as plain IntLiteral clones.
            for (size_t ei = gt->typeArgs.size(); ei < gt->argExprs.size(); ++ei) {
                auto& e = gt->argExprs[ei];
                if (e && e->kind == ExprKind::IntLiteral) {
                    auto lit = std::make_unique<IntLiteralExpr>();
                    lit->location = e->location;
                    lit->value    = e->as<IntLiteralExpr>()->value;
                    out->argExprs.push_back(std::move(lit));
                } else {
                    out->argExprs.push_back(nullptr);
                }
            }
            return out;
        }
        case TypeAnnotationKind::Tuple: {
            auto* tt = src->as<TupleType>();
            auto out = std::make_unique<TupleType>();
            out->location = src->location;
            out->name     = src->name;
            for (auto& e : tt->elements)
                out->elements.push_back(rewriteConstSizesInType(e.get(), constBindings));
            return out;
        }
        case TypeAnnotationKind::Function: {
            auto* ft = src->as<FunctionType>();
            auto out = std::make_unique<FunctionType>();
            out->location = src->location;
            out->name     = src->name;
            for (auto& p : ft->paramTypes)
                out->paramTypes.push_back(rewriteConstSizesInType(p.get(), constBindings));
            if (ft->returnType)
                out->returnType = rewriteConstSizesInType(ft->returnType.get(), constBindings);
            return out;
        }
        case TypeAnnotationKind::Union: {
            auto* ut = src->as<UnionType>();
            auto out = std::make_unique<UnionType>();
            out->location = src->location;
            out->name     = src->name;
            for (auto& m : ut->members)
                out->members.push_back(rewriteConstSizesInType(m.get(), constBindings));
            return out;
        }
        default: {
            // Fallback: return a NamedType with the same name.
            auto out = std::make_unique<NamedType>();
            out->location = src->location;
            out->name     = src->name;
            return out;
        }
    }
}

// ── Pack substitution context (P2 C6) ──
//
// Placed here (before instantiateClassTemplate) so both the class-template
// instantiation path and the variadic-function expansion path can reference
// the struct and thread-local. The full prose commentary lives alongside the
// original definition site in the "Pack-substitution helpers" section below;
// that block now only contains the thread-local _reference_ and the
// cloneTypeWithPackSubst forward-declaration.
struct PackTypeSubstCtx {
    const std::string* typePackName = nullptr;     // `Ts`
    const std::string* valuePackName = nullptr;    // `args`
    const std::vector<VyxTypePtr>* packTypes = nullptr;
    Sema* sema = nullptr;
    // Sugar binding: `<T>(args: ...T) -> T` promotes T to variadic but T
    // also appears in non-pack positions (return type, body type
    // annotations). All pack elements share type T, so bind T → first
    // concrete type here. NamedType("T") substitutes to that concrete
    // type during instance clone.
    const std::string* elementTypeName = nullptr;  // "T"
    VyxTypePtr elementConcreteType = nullptr;      // i64 / string / ...
};
static thread_local PackTypeSubstCtx g_packTypeSubstCtx;

// Forward declaration: full definition in the "Pack-substitution helpers"
// section below. Needed here for instantiateClassTemplate.
static TypePtr cloneTypeWithPackSubst(const TypeAnnotation& type, DiagnosticsEngine& diag);

// Forward declaration for Gap 2: variadic class method bodies need the
// pack-aware statement cloner so `Ts[i]` inside a method body resolves to
// the concrete type. Full definition is in the "Pack-substitution helpers"
// section below (after cloneExprWithPackSubst).
static StmtPtr cloneStmtWithPackSubst(
    const Stmt& stmt, const std::string& packName, size_t packLen,
    DiagnosticsEngine& diag);


// ============================================================
//  P2-C1 Concepts: where-clause verification
// ============================================================
//
// Single entry point used by every template-instantiation path
// (fn / struct / class). Validates that the concrete bindings of
// the template's generic parameters satisfy each declared trait
// constraint, and emits a P2C-prefixed diagnostic with the active
// instantiation chain attached as `note`s.

// ============================================================
//  Phase 8: static trait method dispatch (T::method())
// ============================================================
//
// Query whether a concrete type (post-generic-substitution) has a method
// named `methodName`. Used by `lowerTypeReflectInExpr` to decide whether
// `T::method(...)` in the template body can be rewritten as
// `<ConcreteName>::method(...)` at instantiation time.
//
// Lookup order (mirrors the real dispatch paths CodeGen tries):
//   1. Primitive-target impl block (primitiveMethodImpls_) — hit for
//      `impl Default for i32 { fn default_value() -> i32 { ... } }`.
//      Matches the `i32.default_value` mangling CodeGen emits for the impl.
//   2. Method on the VyxType itself (classes). Covers both declared
//      methods on the class and trait-impl methods that were folded into
//      the class's method table during analyzeClassDecl.
//
// NOTE: does not consider UFCS / free-function lookups; those never
// dispatch as `T::foo` from inside a generic body.
bool Sema::hasStaticMethodForConcreteType(const VyxType& concreteType,
                                          const std::string& methodName) const
{
    // Path 1: primitive-target impls.
    //
    // Build the canonical name string the same way analyzeMemberAccess's
    // primitive-method fallback does (mirrors mangleVyxTypeForCodegen).
    std::string primName;
    switch (concreteType.kind) {
        case VyxTypeKind::Integer:
            if (concreteType.isSizeType)
                primName = concreteType.isSigned ? "isize" : "usize";
            else
                primName = (concreteType.isSigned ? "i" : "u") +
                           std::to_string(concreteType.bitWidth);
            break;
        case VyxTypeKind::Float:
            primName = "f" + std::to_string(concreteType.bitWidth);
            break;
        case VyxTypeKind::Bool:   primName = "bool";   break;
        case VyxTypeKind::Char:   primName = "char";   break;
        case VyxTypeKind::RawPtr: primName = "rawptr"; break;
        case VyxTypeKind::Class:
            // R5: Class-kind "string" wrapper uses the "string" primitive key.
            if (concreteType.name == "string") primName = "string";
            break;
        default:
            break;
    }
    if (!primName.empty()) {
        auto primIt = primitiveMethodImpls_.find(primName);
        if (primIt != primitiveMethodImpls_.end() &&
            primIt->second.count(methodName)) {
            return true;
        }
    }

    // Path 2: concrete type's method table (classes / structs).
    if (concreteType.kind == VyxTypeKind::Class ||
        concreteType.kind == VyxTypeKind::Struct) {
        for (const auto& m : concreteType.methods) {
            if (m.name == methodName) return true;
        }
    }
    return false;
}

void Sema::emitInstantiationChainNotes(SourceLocation siteLoc) {
    // Delegate to the shared free function in Common/Diagnostics.cpp so
    // Monomorphize (and any other pass) can emit identical chain notes
    // without depending on this class.  The member function is kept for
    // backward-compatibility with existing call sites inside Sema.
    ::vyx::emitInstantiationChainNotes(diag_, siteLoc, instantiationStack_);
}

// Trait composition (2026-04-23): walk a trait's direct supertraits
// transitively, returning the trait itself followed by every supertrait
// reachable through the `traitSupertraits_` graph (built during Pass 1
// from `trait Num : Add + Sub + ... {}`). The result is deduped in
// discovery order and cycles (`A : B` and `B : A`) are reported once
// with a P2D-013 diagnostic before the walk bails.
std::vector<std::string> Sema::expandTraitWithSupertraits(
    const std::string& traitName,
    SourceLocation diagLoc)
{
    std::vector<std::string> result;
    std::set<std::string> seen;
    // Reserve a "currently on the DFS stack" set so we can detect cycles
    // distinct from plain re-visits (a diamond supertrait graph like
    // `trait A : B + C {}`, `trait B : D {}`, `trait C : D {}` must NOT
    // trip the cycle detector even though D is reached twice).
    std::set<std::string> onStack;
    std::function<void(const std::string&)> dfs = [&](const std::string& t) {
        if (onStack.count(t)) {
            // Cycle found — dedup on (cycle root name) so the same circular
            // definition reported from multiple call sites collapses.
            std::string dedupKey = "P2D-013\x1f" + t;
            if (reportedConstraintFailures_.insert(dedupKey).second) {
                diag_.errorCoded("P2D-013", diagLoc,
                    "circular supertrait reference involving '{}'", t);
                diag_.noteCompact(diagLoc,
                    "trait '{}' (in)directly lists itself as a supertrait; "
                    "composition chains must form a DAG",
                    t);
            }
            return;
        }
        if (!seen.insert(t).second) return;
        onStack.insert(t);
        auto it = traitSupertraits_.find(t);
        if (it != traitSupertraits_.end()) {
            for (auto& s : it->second) {
                dfs(s);
            }
        }
        onStack.erase(t);
        result.push_back(t);
    };
    dfs(traitName);
    return result;
}

void Sema::verifyMethodConstraints(
    const std::string& contextName,
    const std::map<std::string, std::vector<std::string>>& constraints,
    const std::map<std::string, VyxTypePtr>& bindings,
    SourceLocation siteLoc)
{
    auto primitiveNameOf = [](const VyxTypePtr& type) -> std::string {
        if (!type) return {};
        switch (type->kind) {
            case VyxTypeKind::Integer:
                if (type->isSizeType)
                    return type->isSigned ? "isize" : "usize";
                return (type->isSigned ? "i" : "u") +
                       std::to_string(type->bitWidth);
            case VyxTypeKind::Float:
                return "f" + std::to_string(type->bitWidth);
            case VyxTypeKind::Bool:   return "bool";
            case VyxTypeKind::Char:   return "char";
            case VyxTypeKind::RawPtr: return "rawptr";
            case VyxTypeKind::Class:
                return type->name == "string" ? "string" : std::string{};
            default:
                return {};
        }
    };
    // PLAN_SEMA_ROOT_FIX — S3: primitive trait satisfaction is answered
    // by the canonical TraitSolver. Every primitive query routes through
    // this lambda so the answer is consistent across the two callsites
    // below (post-Pass-1 type-resolution path and post-method-walk
    // fallback path).
    //
    // `factBase_` is frozen by the time we get here (Phase A discovery
    // ran in Sema::analyze before the first generic was instantiated),
    // so the solver query is a pure read with no observable side-effect
    // on the rest of Sema. The S2 `recordPrimImplFact` /
    // `ensurePrimitiveImplRegistered` patch and the legacy
    // `primitiveTraitSatisfied_` table are intentionally NOT consulted
    // — the solver is the single source of truth.
    auto solverPrimSatisfies = [&](const std::string& primName,
                                   const std::string& traitName) -> bool {
        if (primName.empty() || traitName.empty()) return false;
        ++solverPrimQueries_;
        auto traitId = canonicalizer_.canonicalizeTrait(traitName);
        auto primId  = canonicalizer_.canonicalizePrim(primName);
        return traitSolver_.solve(traitId, primId) ==
               canon::SolveResult::Sat;
    };

    for (auto& [paramName, directTraits] : constraints) {
        auto bindIt = bindings.find(paramName);
        if (bindIt == bindings.end() || !bindIt->second) continue;
        auto& concreteType = bindIt->second;
        // Trait composition: expand every direct bound to its full
        // transitive supertrait closure so `T: Num` (with
        // `trait Num : Add + Sub + ... {}`) implicitly checks Add, Sub, etc.
        // Dedup across multiple direct bounds (`where T: Num + Add` must
        // not double-check Add's methods).
        std::vector<std::string> traits;
        std::set<std::string> traitSeen;
        for (auto& direct : directTraits) {
            for (auto& expanded : expandTraitWithSupertraits(direct, siteLoc)) {
                if (traitSeen.insert(expanded).second) {
                    traits.push_back(expanded);
                }
            }
        }
        for (auto& traitName : traits) {
            // PLAN_SEMA_ROOT_FIX — canonicalizer dial-tone.
            //
            // Always route the trait name through the canonicalizer so the
            // pipeline is exercised on every iteration, even for nominal
            // types where S2's reconciliation probe does not fire. S2 has
            // since taken over the prim path via `reconcilePrimTraitFact`
            // below; S3 will replace the legacy string lookups outright.
            (void)canonicalizer_.canonicalizeTrait(traitName);
            // Front 1 — P2D-021: concrete-type cycle guard for
            // `class Foo<T> where Foo<T>: Eq<Foo<T>>` style recursion.
            // Key the guard on the *concrete* (bound) type mangle + trait
            // so that unrelated (Foo<i32>, Eq) and (Foo<f64>, Eq) pairs
            // don't collide. If we are already verifying exactly this
            // pair higher in the call stack, emit once and bail — otherwise
            // verifyMethodConstraints will recurse (via re-entrant type
            // resolution / nested instantiation) until the C++ stack blows.
            std::string cycleKey = concreteType->toString()
                                 + "\x1f" + traitName;
            if (!activeConstraintResolution_.insert(cycleKey).second) {
                std::string dedupKey = "P2D-021\x1f" + cycleKey;
                if (reportedConstraintFailures_.insert(dedupKey).second) {
                    diag_.errorCoded("P2D-021", siteLoc,
                        "cyclic where-clause constraint: verifying '{}: {}' "
                        "would require verifying itself",
                        concreteType->toString(), traitName);
                    diag_.helpCompact(siteLoc,
                        "remove the self-referential bound or rewrite it "
                        "to reference a different type");
                    emitInstantiationChainNotes(siteLoc);
                }
                continue; // do NOT recurse on this (type, trait) pair
            }
            // RAII-style cleanup for the guard key on every exit path of
            // the current trait iteration. C++ doesn't give us a native
            // 'scope guard' here without a helper type, so use a small
            // lambda + unique_ptr<void, Deleter> would be overkill —
            // just erase at the natural end of each iteration below.
            struct ConstraintGuardErase {
                std::set<std::string>& set;
                std::string key;
                ~ConstraintGuardErase() { set.erase(key); }
            } _cg{activeConstraintResolution_, cycleKey};

            auto ifaceType = symbols_.lookupType(traitName);
            if (!ifaceType || ifaceType->kind != VyxTypeKind::Interface) {
                // P2D-001: unknown trait — concept name not found in symbol table.
                // Dedup: same (traitName, contextName) pair across multiple chains
                // collapses to a single diagnostic.
                std::string dedupKey001 = traitName + "\x1f" + contextName + "\x1f<unknown-trait>";
                if (reportedConstraintFailures_.insert(dedupKey001).second) {
                    diag_.errorCoded("P2D-001", siteLoc,
                        "unknown trait '{}' in where-clause of '{}'",
                        traitName, contextName);
                    diag_.noteCompact(siteLoc,
                        "no interface or concept named '{}' is visible at this point",
                        traitName);
                    diag_.helpCompact(siteLoc,
                        "declare 'interface {} {{ ... }}' or 'concept {} = ...' before use",
                        traitName, traitName);
                    emitInstantiationChainNotes(siteLoc);
                }
                continue;
            }
            if (concreteType->kind != VyxTypeKind::Struct &&
                concreteType->kind != VyxTypeKind::Class) {
                // Primitive-target trait impls (2026-04-23): if the caller
                // bound T to a primitive (i32/string/bool/...) and the user
                // wrote `impl <traitName> for <primitive> { ... }`, consider
                // the bound satisfied.  primitiveTraitSatisfied_ is populated
                // in analyzeClassDecl when the impl block is registered.
                //
                // We derive the primitive's canonical name from its
                // VyxTypeKind via the same mangler CodeGen uses; that keeps
                // the key space consistent between primitiveMethodImpls_,
                // primitiveTraitSatisfied_, and CodeGen's `i32.hash` mangle.
                std::string primName = primitiveNameOf(concreteType);
                if (solverPrimSatisfies(primName, traitName)) {
                    continue; // satisfied via `impl traitName for primName`
                }
                // Otherwise fall through to the old tolerance (skip) — leaving
                // a primitive unbound to a trait has historically been lenient
                // (cf. std/hash.vyx note).  Future work: harden this once
                // every primitive has proper impls.
                continue;
            }
            for (auto& reqMethod : ifaceType->methods) {
                bool found = false;
                bool sigMismatch = false;
                // `Self` appearing in a trait method's signature unifies with
                // the implementing type. When comparing `trait { fn clone(self) -> Self; }`
                // against `class Node { fn clone(self) -> Node; }`, a bare
                // Self slot on the trait side matches whatever the class
                // provides as long as the impl type is concreteType itself.
                // Strip the generic-arg suffix (`<...>`) from a mangled template
                // name so `Option<T>` and `Option<i32>` both yield `"Option"`.
                // Bare names (no `<`) pass through unchanged.
                auto baseTemplateName = [](const std::string& n) -> std::string {
                    auto lt = n.find('<');
                    return (lt == std::string::npos) ? n : n.substr(0, lt);
                };
                std::function<bool(const VyxTypePtr&, const VyxTypePtr&)> matchRec;
                matchRec = [&](const VyxTypePtr& reqT,
                               const VyxTypePtr& gotT) -> bool {
                    if (!reqT || !gotT) return true; // tolerate missing
                    // Both Unknown: typical for the implicit `self` param in
                    // both trait and impl (no explicit type annotation).
                    // Treat as matching — the concrete receiver type is
                    // enforced by the method dispatch, not this shape check.
                    if (reqT->kind == VyxTypeKind::Unknown &&
                        gotT->kind == VyxTypeKind::Unknown) return true;
                    // Either side Unknown: tolerate (Sema may not have
                    // fully resolved one side yet; Mono will re-check).
                    if (reqT->kind == VyxTypeKind::Unknown ||
                        gotT->kind == VyxTypeKind::Unknown) return true;
                    if (reqT->kind == VyxTypeKind::Generic && reqT->name == "Self") {
                        // Self resolves to the implementing type.
                        if (gotT->kind == VyxTypeKind::Class ||
                            gotT->kind == VyxTypeKind::Struct) {
                            return gotT->name == concreteType->name;
                        }
                        return false;
                    }
                    // `Self::Item` on the trait side: rebind to the concrete
                    // class's associated-type binding and compare. This lets
                    // `fn first(self) -> Self::Item` in a trait match
                    // `fn first(self) -> i32` in a class that declared
                    // `type Item = i32;`.
                    if (reqT->kind == VyxTypeKind::Generic &&
                        reqT->name.rfind("Self::", 0) == 0) {
                        const std::string assoc = reqT->name.substr(6); // after "Self::"
                        const std::string tgtKey = concreteType->name + "::" + assoc;
                        auto it = implAssocByTarget_.find(tgtKey);
                        if (it != implAssocByTarget_.end() && it->second) {
                            return matchRec(it->second, gotT);
                        }
                        // No concrete binding on the class: tolerate to avoid
                        // false negatives; the Mono/resolver path will surface
                        // a proper error if the binding is truly missing.
                        return true;
                    }
                    // A bare Generic-kind slot on the trait side (e.g. the
                    // trait's own type parameter `T` in `fn next(self) ->
                    // Option<T>`) unifies with whatever the impl provides
                    // at that structural position — this verification pass
                    // is name-level only and does not track concrete bindings
                    // of trait parameters, so treat trait-side generics as
                    // wildcards. The caller's where-clause plus Mono's
                    // instantiation-time typecheck enforce the real binding.
                    if (reqT->kind == VyxTypeKind::Generic) return true;
                    // Symmetric: if the impl side declares a Generic-kind
                    // slot (rare — typical for methods with their own
                    // generic params whose signature leaks to this check),
                    // tolerate as well.
                    if (gotT->kind == VyxTypeKind::Generic) return true;

                    // Cross-kind equivalence for user generic enums:
                    // `resolveType`'s generic-ErrorDef branch produces
                    // `ErrorType{name="Option<T>", ...}` while
                    // `makeOptional`/TemplateResolver produce
                    // `Class{name="Option<...>", ...}`. For the purposes of
                    // trait-method compat, the two kinds are the same
                    // nominal type when their base name + paramTypes match
                    // structurally.
                    auto isClassLike = [](VyxTypeKind k) {
                        return k == VyxTypeKind::Class ||
                               k == VyxTypeKind::ErrorType ||
                               k == VyxTypeKind::Struct;
                    };
                    if (isClassLike(reqT->kind) && isClassLike(gotT->kind)) {
                        // Outer base name (e.g. `Option` in `Option<T>`) must
                        // agree. paramTypes are compared element-wise through
                        // this same recursive matcher so nested generic slots
                        // (including trait-side T) unify correctly.
                        if (baseTemplateName(reqT->name) !=
                            baseTemplateName(gotT->name)) {
                            // Fall back to full name equality (catches non-
                            // generic class/struct names with no `<`).
                            if (reqT->name != gotT->name) return false;
                        }
                        if (reqT->paramTypes.size() != gotT->paramTypes.size()) {
                            // No param info on either side: rely on base-name
                            // agreement. Otherwise a real arity mismatch.
                            if (reqT->paramTypes.empty() ||
                                gotT->paramTypes.empty()) return true;
                            return false;
                        }
                        for (size_t i = 0; i < reqT->paramTypes.size(); ++i) {
                            if (!matchRec(reqT->paramTypes[i], gotT->paramTypes[i]))
                                return false;
                        }
                        return true;
                    }
                    return reqT->isEqual(*gotT);
                };
                auto typesMatchWithSelf = [&](const VyxTypePtr& reqT,
                                              const VyxTypePtr& gotT) -> bool {
                    return matchRec(reqT, gotT);
                };
                // Primitive-target impl satisfaction (2026-04-23): if the
                // concrete type is a class-kind primitive like `string`
                // (R5: String is Class{name="string"}) and the user wrote
                // `impl <traitName> for string { ... }`, accept it even
                // though the method doesn't live on concreteType->methods
                // (we keep the impl methods in primitiveMethodImpls_
                // side-map to avoid mutating the shared VyxType).
                if (!found && isPrimitiveTypeName(concreteType->name)) {
                    if (solverPrimSatisfies(concreteType->name, traitName)) {
                        auto implIt = primitiveMethodImpls_.find(concreteType->name);
                        if (implIt != primitiveMethodImpls_.end() &&
                            implIt->second.count(reqMethod.name)) {
                            found = true;
                        }
                    }
                }
                for (auto& m : concreteType->methods) {
                    if (found) break;
                    if (m.name != reqMethod.name) continue;
                    if (m.paramTypes.size() != reqMethod.paramTypes.size()) {
                        sigMismatch = true;
                        continue;
                    }
                    bool sigMatch = true;
                    for (size_t pi = 0; pi < m.paramTypes.size(); ++pi) {
                        if (!typesMatchWithSelf(reqMethod.paramTypes[pi], m.paramTypes[pi])) {
                            sigMatch = false;
                            break;
                        }
                    }
                    // Also check the return type — trait methods can declare
                    // `-> Self` and the impl returns its own concrete type.
                    if (sigMatch && reqMethod.returnType && m.returnType &&
                        !typesMatchWithSelf(reqMethod.returnType, m.returnType)) {
                        sigMatch = false;
                    }
                    if (sigMatch) { found = true; break; }
                    sigMismatch = true;
                }
                if (!found) {
                    // Dedup key: (traitName, concreteTypeName, requiredMethodName).
                    // This collapses repeated failures of the SAME predicate across
                    // multiple instantiation chains (e.g. 5 call sites of log<Foo>
                    // all fail `Foo: Printable / print` → 1 diagnostic, not 5).
                    std::string dedupKey = traitName + "\x1f"
                                        + concreteType->toString() + "\x1f"
                                        + reqMethod.name;
                    bool isNew = reportedConstraintFailures_.insert(dedupKey).second;

                    if (isNew) {
                    if (sigMismatch) {
                        // P2D-003: class has the method but with an incompatible signature.
                        // Build human-readable required / provided signature strings.
                        std::string sigReq = reqMethod.name + "(";
                        for (size_t pi = 0; pi < reqMethod.paramTypes.size(); ++pi) {
                            if (pi) sigReq += ", ";
                            sigReq += (reqMethod.paramTypes[pi] ? reqMethod.paramTypes[pi]->toString() : "?");
                        }
                        sigReq += ")";
                        std::string sigGot = "?";
                        for (auto& m : concreteType->methods) {
                            if (m.name != reqMethod.name) continue;
                            sigGot = m.name + "(";
                            for (size_t pi = 0; pi < m.paramTypes.size(); ++pi) {
                                if (pi) sigGot += ", ";
                                sigGot += (m.paramTypes[pi] ? m.paramTypes[pi]->toString() : "?");
                            }
                            sigGot += ")";
                            break;
                        }
                        diag_.errorCoded("P2D-003", siteLoc,
                            "type '{}' implements '{}' with incompatible signature — trait requires '{}', type provides '{}'",
                            concreteType->toString(), reqMethod.name, sigReq, sigGot);
                        diag_.noteCompact(siteLoc,
                            "required signature for '{}' is declared on interface '{}'",
                            reqMethod.name, traitName);
                        diag_.noteCompact(siteLoc,
                            "conflicting implementation is provided by '{}'",
                            concreteType->toString());
                        diag_.helpCompact(siteLoc,
                            "adjust the signature of '{}::{}' to match '{}', or update the trait definition",
                            concreteType->toString(), reqMethod.name, sigReq);
                    } else {
                        // P2D-002: class doesn't implement the required method at all.
                        diag_.errorCoded("P2D-002", siteLoc,
                            "type '{}' does not implement required method '{}' of trait '{}'",
                            concreteType->toString(), reqMethod.name, traitName);
                        diag_.noteCompact(siteLoc,
                            "required method '{}' is declared on interface '{}'",
                            reqMethod.name, traitName);
                        diag_.helpCompact(siteLoc,
                            "implement '{}' for '{}', or remove the 'where {}: {}' constraint",
                            traitName, concreteType->toString(), paramName, traitName);
                    }
                    emitInstantiationChainNotes(siteLoc);
                    } // end isNew
                }
            }
        }
    }
}

// ── P2D-007: reflection-constraint verification ───────────────────────────
// Helper: map a concrete VyxTypeKind to the canonical reflection "kind" string
// that the user writes in `where T::kind == "struct"`.
//
// IMPORTANT: Must match vyxTypeKindToReflectString() (used for body T::kind
// expression lowering) exactly — so that `where T::kind == "int"` and
// `if (T::kind == "int")` are consistent for the same T.
static std::string reflectKindString(const VyxType& t) {
    // R5 phase 3: String is Class-kind with name="string"; check before the
    // generic Class case so `where T::kind == "string"` keeps working.
    if (isStringType(t)) return "string";
    switch (t.kind) {
        case VyxTypeKind::Struct:    return "struct";
        case VyxTypeKind::Class:     return "class";
        case VyxTypeKind::Interface: return "interface";
        case VyxTypeKind::ErrorType: return "error";
        case VyxTypeKind::Integer:   return "int";
        case VyxTypeKind::Float:     return "float";
        case VyxTypeKind::Bool:      return "bool";
        case VyxTypeKind::Function:  return "fn";
        default:                     return "primitive";
    }
}

void Sema::verifyConstraints(
    const Decl& templateDecl,
    const std::map<std::string, VyxTypePtr>& bindings,
    SourceLocation siteLoc)
{
    verifyMethodConstraints(templateDecl.name, templateDecl.genericConstraints, bindings, siteLoc);
    verifyPackConstraints(templateDecl, bindings, siteLoc);

    // P3-Q: verify reflection constraints (`T::kind == "struct"` etc.)
    for (auto& rc : templateDecl.reflectConstraints) {
        auto it = bindings.find(rc.typeParam);
        if (it == bindings.end() || !it->second) continue;  // unbound — skip
        auto& concreteType = it->second;

        std::string actual;
        if (rc.member == "kind") {
            actual = reflectKindString(*concreteType);
        } else if (rc.member == "name") {
            actual = concreteType->toString();
        } else {
            // "fields" / "methods" constraints not supported in where-clause yet.
            continue;
        }

        bool matches = (actual == rc.expected);
        bool pass    = rc.negate ? !matches : matches;
        if (!pass) {
            std::string dedupKey = "P2D-007\x1f" + rc.typeParam + "\x1f" + rc.member
                                 + "\x1f" + rc.expected + "\x1f" + concreteType->toString();
            if (reportedConstraintFailures_.insert(dedupKey).second) {
                diag_.errorCoded("P2D-007", siteLoc,
                    "reflection constraint '{}::{} {}{} {}' not satisfied ({}={}, {}='{}')",
                    rc.typeParam, rc.member,
                    rc.negate ? "!=" : "==",
                    "",
                    rc.expected,
                    rc.typeParam, concreteType->toString(),
                    rc.member, actual);
                diag_.noteCompact(siteLoc,
                    "type '{}' has {}='{}', but the where-clause requires {}='{}'",
                    concreteType->toString(), rc.member, actual, rc.member, rc.expected);
                diag_.helpCompact(siteLoc,
                    "only types whose '{}' is '{}' may be used with '{}'",
                    rc.member, rc.expected, templateDecl.name);
                emitInstantiationChainNotes(siteLoc);
            }
        }
    }

    // ── P2D-008: typeof(v) == "typename" constraints ──────────────────────
    // For each TypeofConstraint, resolve the value-parameter `paramName` to
    // its concrete type by looking up the backing type-parameter in `bindings`.
    // Strategy: if the decl is a FunctionDecl, scan its param list for a param
    // whose name matches `tc.paramName`, then get the type-annotation name (the
    // type-parameter identifier, e.g. "T") and look that up in `bindings`.
    // If `paramName` itself is directly a key in `bindings` (for struct/class
    // type params used in typeof), fall back to that.
    if (!templateDecl.typeofConstraints.empty()) {
        for (auto& tc : templateDecl.typeofConstraints) {
            VyxTypePtr concreteType;

            // Try to find value param → type-param chain.
            if (templateDecl.kind == DeclKind::Function) {
                auto* fn = templateDecl.as<FunctionDecl>();
                for (auto& p : fn->params) {
                    if (p.name == tc.paramName && p.type) {
                        // The type annotation name is the type-parameter identifier.
                        auto it = bindings.find(p.type->name);
                        if (it != bindings.end() && it->second) {
                            concreteType = it->second;
                        }
                        break;
                    }
                }
            }
            // Fallback: paramName might directly be a type-parameter name.
            if (!concreteType) {
                auto it = bindings.find(tc.paramName);
                if (it != bindings.end() && it->second)
                    concreteType = it->second;
            }

            if (!concreteType) continue; // unbound — skip

            std::string actual = concreteType->toString();
            bool matches = (actual == tc.expected);
            bool pass    = tc.negate ? !matches : matches;
            if (!pass) {
                std::string dedupKey = "P2D-008\x1f" + tc.paramName
                                     + "\x1f" + tc.expected + "\x1f" + actual;
                if (reportedConstraintFailures_.insert(dedupKey).second) {
                    diag_.errorCoded("P2D-008", siteLoc,
                        "typeof constraint 'typeof({}) {}\"{}\"' not satisfied: "
                        "argument has type '{}', expected '{}'",
                        tc.paramName,
                        tc.negate ? "!=" : "==",
                        tc.expected,
                        actual, tc.expected);
                    diag_.noteCompact(siteLoc,
                        "parameter '{}' was bound to type '{}', "
                        "but the where-clause requires typeof({}) {}\"{}\"",
                        tc.paramName, actual,
                        tc.paramName,
                        tc.negate ? "!=" : "==",
                        tc.expected);
                    diag_.helpCompact(siteLoc,
                        "only arguments of type '{}' satisfy 'typeof({}) {}\"{}\"' "
                        "on '{}'",
                        tc.expected,
                        tc.paramName,
                        tc.negate ? "!=" : "==",
                        tc.expected,
                        templateDecl.name);
                    emitInstantiationChainNotes(siteLoc);
                }
            }
        }
    }
}

// ── P5-pack: verify all<Ts>/any<Ts>: Trait pack constraints ──────────────
//
// For each PackConstraint on `decl`:
//  - Look up `packName` in `bindings`.  The bound type must be a Pack-kind
//    VyxType whose `tupleTypes` vector contains the concrete member types.
//  - `All`: every member must satisfy `traitName`.  First failure → P2D-005.
//  - `Any`: at least one member must satisfy `traitName`.  Total failure → P2D-006.
//
// "Satisfies a trait" reuses the inline check already present in
// verifyMethodConstraints: look up the interface in the symbol table,
// then verify that the concrete type has all required methods.
void Sema::verifyPackConstraints(
    const Decl& decl,
    const std::map<std::string, VyxTypePtr>& bindings,
    SourceLocation siteLoc)
{
    // P5-pack Feature 3: Ts.len refinements.
    // When the decl is a FunctionDecl, scan its refinements for "Ts.len op N"
    // entries and verify the pack cardinality against them.
    if (decl.kind == DeclKind::Function) {
        auto* fn = decl.as<FunctionDecl>();
        for (auto& ref : fn->refinements) {
            // Only handle "X.len" form.
            auto dotPos = ref.paramName.rfind(".len");
            if (dotPos == std::string::npos) continue;
            std::string packName = ref.paramName.substr(0, dotPos);
            auto packIt = bindings.find(packName);
            if (packIt == bindings.end() || !packIt->second) continue;
            auto& packType = packIt->second;
            if (!packType->isPack()) continue;
            int64_t packLen = static_cast<int64_t>(packType->tupleTypes.size());
            bool ok = false;
            switch (ref.op) {
                case BinaryOp::Gt:  ok = (packLen >  ref.value); break;
                case BinaryOp::Gte: ok = (packLen >= ref.value); break;
                case BinaryOp::Lt:  ok = (packLen <  ref.value); break;
                case BinaryOp::Lte: ok = (packLen <= ref.value); break;
                case BinaryOp::Eq:  ok = (packLen == ref.value); break;
                case BinaryOp::Neq: ok = (packLen != ref.value); break;
                default: ok = true; break;
            }
            if (!ok) {
                diag_.errorCoded("P2D-004", siteLoc,
                    "pack '{}' has {} member(s), but the where-clause requires '{}.len {} {}'",
                    packName, packLen, packName,
                    (ref.op == BinaryOp::Gt ? ">" : ref.op == BinaryOp::Gte ? ">=" :
                     ref.op == BinaryOp::Lt ? "<" : ref.op == BinaryOp::Lte ? "<=" :
                     ref.op == BinaryOp::Eq ? "==" : "!="),
                    ref.value);
                diag_.noteCompact(siteLoc,
                    "variadic pack '{}' was instantiated with {} argument(s)",
                    packName, packLen);
                emitInstantiationChainNotes(siteLoc);
            }
        }
    }

    for (auto& pc : decl.packConstraints) {
        // Resolve the pack binding.
        auto packIt = bindings.find(pc.packName);
        if (packIt == bindings.end() || !packIt->second) {
            // Unknown pack at this instantiation site — skip silently; Sema may
            // not have a binding yet (e.g. in a forward-declaration context).
            continue;
        }
        auto& packType = packIt->second;
        if (!packType->isPack()) {
            diag_.errorCoded("P2D-005", siteLoc,
                "'{}' is not a variadic pack parameter in where-clause constraint 'all<{}>: {}'",
                pc.packName, pc.packName, pc.traitName);
            emitInstantiationChainNotes(siteLoc);
            continue;
        }

        // Resolve the trait.
        auto ifaceType = symbols_.lookupType(pc.traitName);
        if (!ifaceType || ifaceType->kind != VyxTypeKind::Interface) {
            diag_.errorCoded("P2D-001", siteLoc,
                "unknown trait '{}' in where-clause of '{}'",
                pc.traitName, decl.name);
            diag_.noteCompact(siteLoc,
                "no interface or concept named '{}' is visible at this point",
                pc.traitName);
            diag_.helpCompact(siteLoc,
                "declare 'interface {} {{ ... }}' or 'concept {} = ...' before use",
                pc.traitName, pc.traitName);
            emitInstantiationChainNotes(siteLoc);
            continue;
        }

        // Helper lambda: does `memberType` satisfy the trait?
        auto satisfiesTrait = [&](const VyxTypePtr& memberType) -> bool {
            if (!memberType) return false;
            if (memberType->kind != VyxTypeKind::Struct &&
                memberType->kind != VyxTypeKind::Class) {
                return false;
            }
            // Check every required method is present on the concrete type.
            for (auto& reqMethod : ifaceType->methods) {
                bool found = false;
                for (auto& m : memberType->methods) {
                    if (m.name == reqMethod.name) { found = true; break; }
                }
                if (!found) return false;
            }
            return true;
        };

        const auto& members = packType->tupleTypes;
        if (pc.kind == Decl::PackConstraint::All) {
            for (size_t i = 0; i < members.size(); ++i) {
                if (!satisfiesTrait(members[i])) {
                    std::string memberName = members[i] ? members[i]->toString() : "?";
                    diag_.errorCoded("P2D-005", siteLoc,
                        "pack member at index {} ('{}') does not satisfy '{}' as required by `all<{}>: {}`",
                        i, memberName, pc.traitName, pc.packName, pc.traitName);
                    diag_.noteCompact(siteLoc,
                        "type '{}' is missing one or more methods required by interface '{}'",
                        memberName, pc.traitName);
                    diag_.helpCompact(siteLoc,
                        "implement 'interface {}' on '{}' or remove it from the argument list",
                        pc.traitName, memberName);
                    emitInstantiationChainNotes(siteLoc);
                }
            }
        } else { // Any
            bool anyPasses = false;
            for (auto& member : members) {
                if (satisfiesTrait(member)) { anyPasses = true; break; }
            }
            if (!anyPasses && !members.empty()) {
                diag_.errorCoded("P2D-006", siteLoc,
                    "no pack member satisfies '{}' as required by `any<{}>: {}`",
                    pc.traitName, pc.packName, pc.traitName);
                diag_.noteCompact(siteLoc,
                    "none of the {} pack members implement interface '{}'",
                    members.size(), pc.traitName);
                diag_.helpCompact(siteLoc,
                    "at least one argument must implement '{}' when using `any<{}>: {}`",
                    pc.traitName, pc.packName, pc.traitName);
                emitInstantiationChainNotes(siteLoc);
            }
        }
    }
}

// ── P2D-004: const-integer where-clause predicate verification ────────────
//
// Small const-evaluator: walks an Expr tree that contains only IntLiteralExpr,
// IdentifierExpr (resolved via `env`), and BinaryOpExpr (arithmetic subset).
// Returns true on success (stores result in `out`), false if the expression
// cannot be evaluated (unbound identifier, division/modulo by zero, or an
// unsupported node kind).
//
// Supported operations: Add, Sub, Mul, Div, Mod.
// Unsupported / non-integer operations are treated as evaluation failure.
// Externally visible so SemaExprOps.cpp can fold turbofish const-args.
bool evalConstExprInt(
        const Expr* expr,
        const std::map<std::string, int64_t>& env,
        int64_t& out)
{
    if (!expr) return false;

    switch (expr->kind) {
        case ExprKind::IntLiteral:
            out = expr->as<IntLiteralExpr>()->value;
            return true;

        case ExprKind::Identifier: {
            const auto& name = expr->as<IdentifierExpr>()->name;
            auto it = env.find(name);
            if (it == env.end()) return false; // unbound
            out = it->second;
            return true;
        }

        case ExprKind::BinaryOp: {
            const auto* bin = expr->as<BinaryOpExpr>();
            int64_t lVal = 0, rVal = 0;
            if (!evalConstExprInt(bin->lhs.get(), env, lVal)) return false;
            if (!evalConstExprInt(bin->rhs.get(), env, rVal)) return false;
            switch (bin->op) {
                case BinaryOp::Add: out = lVal + rVal; return true;
                case BinaryOp::Sub: out = lVal - rVal; return true;
                case BinaryOp::Mul: out = lVal * rVal; return true;
                case BinaryOp::Div:
                    if (rVal == 0) return false; // division by zero
                    out = lVal / rVal;
                    return true;
                case BinaryOp::Mod:
                    if (rVal == 0) return false; // modulo by zero
                    out = lVal % rVal;
                    return true;
                default:
                    return false; // non-arithmetic op in const predicate
            }
        }

        default:
            return false; // unrecognised node kind
    }
}

// Render a const expression tree back to a source-like string for diagnostics.
static std::string constExprToString(const Expr* expr)
{
    if (!expr) return "?";
    switch (expr->kind) {
        case ExprKind::IntLiteral:
            return std::to_string(expr->as<IntLiteralExpr>()->value);
        case ExprKind::Identifier:
            return expr->as<IdentifierExpr>()->name;
        case ExprKind::BinaryOp: {
            const auto* bin = expr->as<BinaryOpExpr>();
            const char* opStr = "?";
            switch (bin->op) {
                case BinaryOp::Add: opStr = "+"; break;
                case BinaryOp::Sub: opStr = "-"; break;
                case BinaryOp::Mul: opStr = "*"; break;
                case BinaryOp::Div: opStr = "/"; break;
                case BinaryOp::Mod: opStr = "%"; break;
                case BinaryOp::Eq:  opStr = "=="; break;
                case BinaryOp::Neq: opStr = "!="; break;
                case BinaryOp::Lt:  opStr = "<"; break;
                case BinaryOp::Lte: opStr = "<="; break;
                case BinaryOp::Gt:  opStr = ">"; break;
                case BinaryOp::Gte: opStr = ">="; break;
                default: break;
            }
            return constExprToString(bin->lhs.get()) + " " + opStr
                 + " " + constExprToString(bin->rhs.get());
        }
        default: return "?";
    }
}

void Sema::verifyConstPredicates(
        const Decl& templateDecl,
        const std::map<std::string, int64_t>& constBindings,
        SourceLocation siteLoc)
{
    if (templateDecl.constPredicates.empty()) return;

    for (const auto& pred : templateDecl.constPredicates) {
        int64_t lVal = 0, rVal = 0;

        bool lOk = evalConstExprInt(pred.lhs.get(), constBindings, lVal);
        bool rOk = evalConstExprInt(pred.rhs.get(), constBindings, rVal);

        if (!lOk || !rOk) {
            // Unbound identifiers or division-by-zero: skip with a warning.
            // This can happen if the predicate references a type param (not a
            // const param); it's not an error here — Sema hasn't yet bound it.
            diag_.warning(siteLoc,
                "where-clause const predicate `{}` could not be fully evaluated "
                "(unbound identifier or arithmetic error); skipping check",
                constExprToString(pred.lhs.get()) + " " +
                [&]() -> std::string {
                    switch (pred.op) {
                        case BinaryOp::Lt:  return "<";
                        case BinaryOp::Lte: return "<=";
                        case BinaryOp::Gt:  return ">";
                        case BinaryOp::Gte: return ">=";
                        case BinaryOp::Eq:  return "==";
                        case BinaryOp::Neq: return "!=";
                        default: return "?";
                    }
                }() + " " + constExprToString(pred.rhs.get()));
            continue;
        }

        bool satisfied = false;
        switch (pred.op) {
            case BinaryOp::Lt:  satisfied = lVal <  rVal; break;
            case BinaryOp::Lte: satisfied = lVal <= rVal; break;
            case BinaryOp::Gt:  satisfied = lVal >  rVal; break;
            case BinaryOp::Gte: satisfied = lVal >= rVal; break;
            case BinaryOp::Eq:  satisfied = lVal == rVal; break;
            case BinaryOp::Neq: satisfied = lVal != rVal; break;
            default:
                diag_.warning(siteLoc,
                    "unexpected comparison operator in const predicate; skipping check");
                continue;
        }

        if (!satisfied) {
            // Build a human-readable predicate string with evaluated values.
            const char* opStr = "?";
            switch (pred.op) {
                case BinaryOp::Lt:  opStr = "<";  break;
                case BinaryOp::Lte: opStr = "<="; break;
                case BinaryOp::Gt:  opStr = ">";  break;
                case BinaryOp::Gte: opStr = ">="; break;
                case BinaryOp::Eq:  opStr = "=="; break;
                case BinaryOp::Neq: opStr = "!="; break;
                default: break;
            }

            std::string lhsStr = constExprToString(pred.lhs.get());
            std::string rhsStr = constExprToString(pred.rhs.get());
            std::string predStr = lhsStr + " " + opStr + " " + rhsStr;

            // Build a binding summary for the right-hand note
            // (shows the evaluated values, e.g. "N = 0, M = 5")
            std::string bindingSummary;
            for (auto& [k, v] : constBindings) {
                if (!bindingSummary.empty()) bindingSummary += ", ";
                bindingSummary += k + " = " + std::to_string(v);
            }
            if (bindingSummary.empty()) {
                // Rare: the predicate uses only literals.
                bindingSummary = std::to_string(lVal) + " vs " + std::to_string(rVal);
            }

            diag_.errorCoded("P2D-004", siteLoc,
                "const predicate `{}` not satisfied ({})",
                predStr, bindingSummary);

            // Attach the instantiation chain as compact notes.
            emitInstantiationChainNotes(siteLoc);

            diag_.helpCompact(siteLoc,
                "ensure the template arguments satisfy the where-clause constraint `{}`",
                predStr);
        }
    }
}

// P2-generics C5: for each where-clause constraint `T : Trait` in
// `constraints`, look up every associated type declared on `Trait` in
// `traitAssocTypes_`. For each one, find the concrete binding from the
// class that `T` was bound to (via `implAssocByTarget_`), and insert a
// `T::AssocName -> ConcreteType` entry into `substitutions`. The entries
// are then picked up by `TemplateResolver::substituteType` when it sees a
// `Dependent` annotation inside the cloned template body.
//
// Example: `T = CountUp`, `Trait = Iterable`, assoc type `Item` bound to
// `i64` in CountUp's body → inserts `"T::Item" -> i64` so that the return
// type `Option<T::Item>` in `peek<T: Iterable>` becomes `Option<i64>`.
void Sema::bindDependentTypesForSubst(
    const std::map<std::string, std::vector<std::string>>& constraints,
    std::map<std::string, VyxTypePtr>& substitutions)
{
    for (auto& [paramName, traits] : constraints) {
        auto bindIt = substitutions.find(paramName);
        if (bindIt == substitutions.end() || !bindIt->second) continue;
        auto& concreteType = bindIt->second;

        for (auto& traitName : traits) {
            // Find all associated type names declared on this trait.
            auto traitIt = traitAssocTypes_.find(traitName);
            if (traitIt == traitAssocTypes_.end()) continue;

            for (auto& assocName : traitIt->second) {
                // Key in substitutions: "T::Item" (param name + "::" + assoc).
                const std::string substKey = paramName + "::" + assocName;
                if (substitutions.count(substKey)) continue; // already bound

                // Look up the concrete binding from the class bound to T.
                const std::string targetKey = concreteType->name + "::" + assocName;
                auto assocIt = implAssocByTarget_.find(targetKey);
                if (assocIt != implAssocByTarget_.end() && assocIt->second) {
                    substitutions[substKey] = assocIt->second;
                    continue;
                }

                // P2b Wave 3+: concreteType may be a generic class instantiation
                // like "Iter<i32>". The raw assoc binding lives in
                // implAssocByTemplate_["Iter"]["Item"] = annotation for T.
                // Substitute using the args encoded in concreteType->name.
                {
                    auto lt = concreteType->name.find('<');
                    if (lt != std::string::npos) {
                        const std::string baseName = concreteType->name.substr(0, lt);
                        auto tmplIt = implAssocByTemplate_.find(baseName);
                        if (tmplIt != implAssocByTemplate_.end()) {
                            auto memberIt = tmplIt->second.find(assocName);
                            if (memberIt != tmplIt->second.end() && memberIt->second) {
                                // Find the template decl to get param names.
                                if (unit_) {
                                    for (auto& d : unit_->declarations) {
                                        if (!d || d->name != baseName) continue;
                                        if (d->kind != DeclKind::Class &&
                                            d->kind != DeclKind::Struct) continue;
                                        if (d->genericParams.empty()) continue;
                                        // Parse args from mangled name.
                                        std::string argsStr = concreteType->name.substr(lt + 1);
                                        if (!argsStr.empty() && argsStr.back() == '>')
                                            argsStr.pop_back();
                                        std::vector<std::string> argNames;
                                        {
                                            int depth = 0; std::string cur;
                                            for (char c : argsStr) {
                                                if (c == '<') { ++depth; cur += c; }
                                                else if (c == '>') { --depth; cur += c; }
                                                else if (c == ',' && depth == 0) {
                                                    argNames.push_back(cur); cur.clear();
                                                } else { cur += c; }
                                            }
                                            if (!cur.empty()) argNames.push_back(cur);
                                        }
                                        GenericSubstitution subst;
                                        for (size_t i = 0; i < d->genericParams.size() &&
                                                           i < argNames.size(); ++i) {
                                            auto argType = symbols_.lookupType(argNames[i]);
                                            if (!argType)
                                                argType = resolveNamedType(argNames[i], {});
                                            if (argType) subst.add(d->genericParams[i], argType);
                                        }
                                        auto resolved = templateResolver_.substituteType(
                                            *memberIt->second, subst);
                                        if (resolved && resolved->kind != VyxTypeKind::Generic) {
                                            substitutions[substKey] = resolved;
                                        }
                                        break;
                                    }
                                }
                                if (substitutions.count(substKey)) continue;
                            }
                        }
                    }
                }

                // Fallback to the trait default.
                const std::string traitKey = traitName + "::" + assocName;
                auto defIt = implAssocByTrait_.find(traitKey);
                if (defIt != implAssocByTrait_.end() && defIt->second) {
                    substitutions[substKey] = defIt->second;
                }
            }
        }
    }
}

void Sema::instantiateTemplateIfNeeded(const Expr& callExpr, const Decl* calleeDecl,
                                         const std::vector<VyxTypePtr>& argTypes,
                                         const std::vector<VyxTypePtr>* mangleArgsOverride) {
    if (!calleeDecl || calleeDecl->kind != DeclKind::Function) return;
    if (calleeDecl->genericParams.empty()) return;

    auto* fnDecl = calleeDecl->as<FunctionDecl>();
    // mangleArgsOverride: when caller has explicit turbofish args, the mangle
    // key MUST come from the generic-param bindings (T → i64) directly.
    // Otherwise a parameter shaped like `Container<T>` would resolve to its
    // base name only (`<Container>`) and produce a wrong instantiation key.
    // Inference and the rest of the pipeline still consume `argTypes`
    // (param-substituted form) unchanged.
    const std::vector<VyxTypePtr>& mangleArgs =
        mangleArgsOverride ? *mangleArgsOverride : argTypes;
    std::string mangledName = TemplateResolver::mangleTemplateName(calleeDecl->name, mangleArgs);
    // Append const-arg values to the mangled name so distinct const
    // instantiations share distinct monomorphs (same suffix rule as the
    // SemaExprOps turbofish-call path uses).
    {
        auto* callE_for_mangle = callExpr.as<CallExpr>();
        if (callE_for_mangle && callE_for_mangle->callee &&
            callE_for_mangle->callee->kind == ExprKind::Identifier &&
            !calleeDecl->genericConstParams.empty()) {
            auto* id = callE_for_mangle->callee->as<IdentifierExpr>();
            std::map<std::string, int64_t> emptyEnv;
            for (auto& ae : id->callArgExprs) {
                int64_t cv = 0;
                if (ae && evalConstExprInt(ae.get(), emptyEnv, cv)) {
                    mangledName += "#" + std::to_string(cv);
                }
            }
        }
    }

    if (instantiatedReturnTypes_.find(mangledName) != instantiatedReturnTypes_.end()) {
        return;
    }

    // Full-template-specialization fast path: if the user supplied a
    // hand-written `fn foo<i32>(...)` whose collapsed name (`foo<i32>`)
    // matches the mangled key we'd normally synthesize, skip the
    // template-clone / re-analysis pipeline entirely. The user's body has
    // already been registered + analysed as a regular non-generic function
    // by the Pass-1 / analyzeDecl flow, so its return type is queryable
    // from the symbol table directly. We mirror it into
    // `instantiatedReturnTypes_` so subsequent call sites bypass this
    // method on the fast `instantiatedReturnTypes_.find(...)` check above.
    if (unit_) {
        for (auto& d : unit_->declarations) {
            if (!d || !d->isFullSpecialization) continue;
            if (d->kind != DeclKind::Function) continue;
            if (d->name != mangledName) continue;
            VyxTypePtr retType = types::makeVoid();
            if (auto* sym = symbols_.lookup(mangledName)) {
                if (sym->returnType) retType = sym->returnType;
            } else if (auto* fnSpec = d->as<FunctionDecl>(); fnSpec && fnSpec->returnType) {
                retType = resolveType(*fnSpec->returnType);
            }
            instantiatedReturnTypes_[mangledName] = retType;
            return;
        }
    }

    // Variadic generic expansion
    if (calleeDecl->isVariadicGeneric) {
        // P5-pack: verify all<Ts>/any<Ts> and Ts.len constraints before expansion.
        // Build a synthetic bindings map with the pack type so verifyPackConstraints
        // can enumerate members.  The variadic pack param name is in genericParams[0].
        if (!calleeDecl->genericParams.empty() &&
            (!calleeDecl->packConstraints.empty() ||
             !calleeDecl->as<FunctionDecl>()->refinements.empty())) {
            std::string packParamName = calleeDecl->genericParams.front();
            auto packType = std::make_shared<VyxType>();
            packType->kind = VyxTypeKind::Pack;
            for (auto& at : argTypes)
                packType->tupleTypes.push_back(at);
            std::map<std::string, VyxTypePtr> synthBindings;
            synthBindings[packParamName] = packType;
            // Also bind individual members `packParam_0`, `packParam_1` for
            // refinement index checks.
            for (size_t i = 0; i < argTypes.size(); ++i)
                synthBindings[packParamName + "_" + std::to_string(i)] = argTypes[i];
            verifyPackConstraints(*calleeDecl, synthBindings, callExpr.location);
        }

        auto instantiatedDecl = createVariadicInstance(*calleeDecl, argTypes);
        instantiatedDecl->name = mangledName;

        // Variadic functions that ALSO have const-generic params need the
        // same N→literal substitution as the non-variadic path. Extract
        // const bindings from the call's callArgExprs and apply.
        if (!calleeDecl->genericConstParams.empty()) {
            std::map<std::string, int64_t> cpBindingsV;
            auto* callE_v = callExpr.as<CallExpr>();
            if (callE_v && callE_v->callee &&
                callE_v->callee->kind == ExprKind::Identifier) {
                auto* id = callE_v->callee->as<IdentifierExpr>();
                std::map<std::string, int64_t> emptyEnvV;
                for (size_t i = 0; i < calleeDecl->genericParams.size() &&
                                   i < id->callArgExprs.size(); ++i) {
                    const auto& pn = calleeDecl->genericParams[i];
                    if (!calleeDecl->genericConstParams.count(pn)) continue;
                    if (!id->callArgExprs[i]) continue;
                    int64_t cvV = 0;
                    if (evalConstExprInt(id->callArgExprs[i].get(), emptyEnvV, cvV)) {
                        cpBindingsV[pn] = cvV;
                    }
                }
            }
            if (!cpBindingsV.empty()) {
                auto* fn = instantiatedDecl->as<FunctionDecl>();
                if (fn && fn->body) substConstIdentsInStmt(fn->body, cpBindingsV);
            }
        }

        // Insert a placeholder before analysis so that recursive calls to the
        // same mangled name hit the cache and return early instead of looping.
        instantiatedReturnTypes_[mangledName] = types::makeVoid();

        analyzeFunctionDecl(*instantiatedDecl);

        auto* instFnSym = symbols_.lookup(mangledName);
        VyxTypePtr returnType = (instFnSym && instFnSym->returnType) ? instFnSym->returnType : types::makeVoid();
        instantiatedReturnTypes_[mangledName] = returnType;

        // PLAN_SEMA_ROOT_FIX — S4: sidecar ledger. Read-only at S4;
        // mirrors every push into `pendingInstantiations_` so the
        // scheduler observes the same instantiation set Sema produces.
        monoScheduler_.recordFreeFnRequest(mangledName);
        pendingInstantiations_.push_back(std::move(instantiatedDecl));
        return;
    }

    auto* callE = callExpr.as<CallExpr>();
    GenericSubstitution subst = TemplateResolver::inferGenericTypes(
        calleeDecl->genericParams,
        fnDecl->params,
        callE->args,
        argTypes
    );
    // When explicit turbofish is provided (mangleArgsOverride non-null +
    // sized to genericParams), seed subst with those bindings. inferGeneric-
    // Types can't infer through user-defined interfaces (`fn f<T>(c: Container<T>)`
    // called with a Bag impl): the matchAndBind helper only walks builtin
    // containers (Option/Vec/Ref/Result/...). Without this seed, T remains
    // unbound, the cloned body still references `T` (uppercase), and
    // convertTypeToAnnotation later collapses the param type to `rawptr`.
    if (mangleArgsOverride &&
        mangleArgsOverride->size() == calleeDecl->genericParams.size()) {
        for (size_t i = 0; i < calleeDecl->genericParams.size(); ++i) {
            const auto& gp = calleeDecl->genericParams[i];
            if ((*mangleArgsOverride)[i] && !subst.isGenericParam(gp))
                subst.add(gp, (*mangleArgsOverride)[i]);
        }
    }

    // P2-generics C5: inject `T::Item -> ConcreteAssoc` entries for every
    // where-clause constraint `T : Trait` on this template, so that
    // `substituteType` can replace `DependentType{baseName="T", memberName="Item"}`
    // with the concrete associated type when cloning the body.
    bindDependentTypesForSubst(calleeDecl->genericConstraints, subst.substitutions);

    // Front 1 — P2D-020: same stack-depth cap on the function-template
    // path. A recursive generic function (e.g. `fn f<T>() { f::<Box<T>>() }`)
    // would otherwise keep pushing frames until the C++ stack blew.
    if (instantiationStack_.size() >= kMaxSemaInstantiationStack) {
        std::string dedupKey = "P2D-020\x1f" + mangledName;
        if (reportedConstraintFailures_.insert(dedupKey).second) {
            diag_.errorCoded("P2D-020", callExpr.location,
                "instantiation stack overflow while instantiating '{}' "
                "(depth {} >= {}); this usually indicates a recursive generic "
                "without a base case",
                mangledName, instantiationStack_.size(),
                kMaxSemaInstantiationStack);
            emitInstantiationChainNotes(callExpr.location);
        }
        return;
    }

    // P2D-001/002/003 industrial diagnostic chain: keep the InstantiationGuard
    // alive across BOTH the where-clause check AND the recursive
    // `analyzeFunctionDecl` of the cloned body. Earlier the guard scope
    // ended right after `verifyConstraints`, which meant any constraint
    // violation discovered while analysing an *inner* generic call site
    // (mid -> inner, etc.) saw an empty stack and emitted a 1-frame chain
    // instead of the full nesting. Holding the frame for the entire
    // recursive analysis surfaces the complete N-level chain.
    InstantiationFrame frame;
    std::string targs;
    for (auto& at : argTypes) {
        if (!targs.empty()) targs += ", ";
        targs += at ? at->toString() : "?";
    }
    frame.calleeName = calleeDecl->name + "<" + targs + ">";
    frame.site = callExpr.location;
    for (auto& [tp, ct] : subst.substitutions)
        frame.bindings.emplace_back(tp, ct ? ct->toString() : "?");
    InstantiationGuard guard(*this, std::move(frame));

    verifyConstraints(*calleeDecl, subst.substitutions, callExpr.location);

    // Extract the const-param bindings from the turbofish call-arg expressions.
    // Used both for P2D-004 const-predicate verification (if any) and for
    // substituting `return N;` identifiers in the cloned body (always).
    std::map<std::string, int64_t> cpBindings;
    if (callE && !calleeDecl->genericConstParams.empty()) {
        auto* idCallee = (callE->callee && callE->callee->kind == ExprKind::Identifier)
            ? callE->callee->as<IdentifierExpr>() : nullptr;
        if (idCallee && !idCallee->callArgExprs.empty()) {
            std::map<std::string, int64_t> emptyEnvCp;
            for (size_t i = 0; i < calleeDecl->genericParams.size() &&
                               i < idCallee->callArgExprs.size(); ++i) {
                const auto& pname = calleeDecl->genericParams[i];
                if (!calleeDecl->genericConstParams.count(pname)) continue;
                if (!idCallee->callArgExprs[i]) continue;
                int64_t cv = 0;
                if (evalConstExprInt(idCallee->callArgExprs[i].get(), emptyEnvCp, cv)) {
                    cpBindings[pname] = cv;
                }
            }
        }
    }
    if (!calleeDecl->constPredicates.empty()) {
        verifyConstPredicates(*calleeDecl, cpBindings, callExpr.location);
    }

    auto instantiatedDecl = createTemplateInstance(*calleeDecl, subst);
    instantiatedDecl->name = mangledName;

    // After createTemplateInstance substituted T→i32, walk the instance's
    // body and also any nested VarDecl type annotations to fold const-
    // generic identifiers inside Array sizes. We iterate the ORIGINAL
    // template body to recover the Identifier("N") shape (the cloned body's
    // varType has already lost the size expression via substituteType's
    // round-trip), building a fresh annotation per VarDecl via the same
    // two-step rewrite used for the signature below.
    if (!cpBindings.empty() && instantiatedDecl->kind == DeclKind::Function) {
        auto* instFn = instantiatedDecl->as<FunctionDecl>();
        const auto* srcFn = calleeDecl->as<FunctionDecl>();
        if (instFn->body && srcFn->body) {
            // Collect every VarDecl in source body in tree order.
            std::vector<const VarDeclStmt*> srcVarDecls;
            std::function<void(const Stmt*)> collectSrc = [&](const Stmt* s) {
                if (!s) return;
                switch (s->kind) {
                    case StmtKind::VarDecl:
                        srcVarDecls.push_back(s->as<VarDeclStmt>());
                        return;
                    case StmtKind::Block: {
                        auto* b = s->as<BlockStmt>();
                        for (auto& st : b->statements) collectSrc(st.get());
                        return;
                    }
                    case StmtKind::If: {
                        auto* i = s->as<IfStmt>();
                        collectSrc(i->thenBranch.get());
                        collectSrc(i->elseBranch.get());
                        return;
                    }
                    case StmtKind::While:
                        collectSrc(s->as<WhileStmt>()->body.get());
                        return;
                    case StmtKind::For: {
                        auto* f = s->as<ForStmt>();
                        collectSrc(f->init.get());
                        collectSrc(f->body.get());
                        return;
                    }
                    case StmtKind::Unsafe:
                        collectSrc(s->as<UnsafeStmt>()->body.get());
                        return;
                    default: return;
                }
            };
            collectSrc(srcFn->body.get());
            // Walk the instance body in the same order and rebuild each
            // VarDecl's varType from the original annotation via the two-
            // step rewrite (const-size fold + type-param subst).
            size_t srcIdx = 0;
            std::function<void(Stmt*)> rebuild = [&](Stmt* s) {
                if (!s) return;
                switch (s->kind) {
                    case StmtKind::VarDecl: {
                        auto* vd = s->as<VarDeclStmt>();
                        if (srcIdx < srcVarDecls.size()) {
                            const auto* origVd = srcVarDecls[srcIdx++];
                            if (origVd->varType && vd->varType) {
                                auto rewritten = rewriteConstSizesInType(
                                    origVd->varType.get(), cpBindings);
                                const TypeAnnotation& effSrc = rewritten
                                    ? *rewritten : *origVd->varType;
                                auto substituted = templateResolver_.substituteType(effSrc, subst);
                                auto reifed = convertTypeToAnnotation(substituted);
                                if (reifed) vd->varType = std::move(reifed);
                            }
                        }
                        return;
                    }
                    case StmtKind::Block: {
                        auto* b = s->as<BlockStmt>();
                        for (auto& st : b->statements) rebuild(st.get());
                        return;
                    }
                    case StmtKind::If: {
                        auto* i = s->as<IfStmt>();
                        rebuild(i->thenBranch.get());
                        rebuild(i->elseBranch.get());
                        return;
                    }
                    case StmtKind::While:
                        rebuild(s->as<WhileStmt>()->body.get());
                        return;
                    case StmtKind::For: {
                        auto* f = s->as<ForStmt>();
                        rebuild(f->init.get());
                        rebuild(f->body.get());
                        return;
                    }
                    case StmtKind::Unsafe:
                        rebuild(s->as<UnsafeStmt>()->body.get());
                        return;
                    default: return;
                }
            };
            rebuild(instFn->body.get());
        }
    }

    // Substitute const-generic identifiers (N) with their concrete integer
    // literals in the cloned body BEFORE Sema re-analyses it. Without this,
    // `return N;` reaches CodeGen with `IdentifierExpr(N)` unresolved and
    // errors with "undefined variable 'N'".
    //
    // Also fold const-param names inside the instance's signature (param
    // annotations, return type). `createTemplateInstance` already ran the
    // type-param substitution (T → i32) via TemplateResolver which loses
    // the Array size expression when it isn't a plain IntLiteral. Rebuild
    // each signature annotation from the *original* template source: first
    // fold `N` → bound integer on the raw annotation, then re-run the
    // type-param substitution through TemplateResolver. This two-step order
    // guarantees that `[N]T` becomes `[bound]T` with a concrete integer
    // size, then `T` is substituted to its concrete type, producing the
    // correct fixed-array layout for CodeGen.
    if (!cpBindings.empty() && instantiatedDecl->kind == DeclKind::Function) {
        auto* instFn = instantiatedDecl->as<FunctionDecl>();
        if (instFn->body) substConstIdentsInStmt(instFn->body, cpBindings);
        auto reify = [this](const VyxTypePtr& t) -> TypePtr {
            return convertTypeToAnnotation(t);
        };
        auto rebuildSig = [&](TypePtr& slot, const TypeAnnotation* origAnn) {
            if (!origAnn) return;
            auto rewritten = rewriteConstSizesInType(origAnn, cpBindings);
            const TypeAnnotation& effSrc = rewritten ? *rewritten : *origAnn;
            auto substituted = templateResolver_.substituteType(effSrc, subst);
            auto reifed = reify(substituted);
            if (reifed) slot = std::move(reifed);
        };
        if (instFn->returnType && fnDecl->returnType) {
            rebuildSig(instFn->returnType, fnDecl->returnType.get());
        }
        for (size_t i = 0; i < instFn->params.size() && i < fnDecl->params.size(); ++i) {
            if (fnDecl->params[i].type) {
                rebuildSig(instFn->params[i].type, fnDecl->params[i].type.get());
            }
        }
    }

    // Expand `for f in T::fields` / `T::methods` foreach loops against the
    // concrete type bindings. Mirrors Monomorphize::cloneStmt's ForEach path
    // for Sema-instantiated function bodies (Mono only re-walks bodies it
    // instantiates itself). For zero-param generic functions like
    // `fn count_fields<T>()`, inferGenericTypes returns an empty subst (no
    // declared params to infer from); rebuild the T→Point binding from the
    // call's callTypeArgs directly so expansion can fire.
    if (instantiatedDecl->kind == DeclKind::Function) {
        auto* instFn = instantiatedDecl->as<FunctionDecl>();
        if (instFn->body) {
            std::map<std::string, VyxTypePtr> richSubst;
            for (auto& [p, ct] : subst.substitutions) {
                if (!ct) { richSubst[p] = ct; continue; }
                auto looked = symbols_.lookupType(ct->name);
                richSubst[p] = looked ? looked : ct;
            }
            // Rebuild from turbofish callTypeArgs if subst didn't cover T.
            if (callE && callE->callee && callE->callee->kind == ExprKind::Identifier) {
                auto* id = callE->callee->as<IdentifierExpr>();
                for (size_t i = 0; i < calleeDecl->genericParams.size() &&
                                   i < id->callTypeArgs.size(); ++i) {
                    const auto& pname = calleeDecl->genericParams[i];
                    if (richSubst.count(pname) && richSubst[pname]) continue;
                    if (!id->callTypeArgs[i]) continue;
                    auto resolved = resolveType(*id->callTypeArgs[i]);
                    if (resolved) {
                        auto looked = symbols_.lookupType(resolved->name);
                        richSubst[pname] = looked ? looked : resolved;
                    }
                }
            }
            expandReflectForEachInStmt(instFn->body, richSubst);
            // Lower bare `T::name` / `T::kind` in expression position to
            // string literals (T::fields/methods stay as ForEach collections
            // handled above). Pass &diag_ so P2D-009 (invalid reflection
            // member) can fire at the real instantiation site.
            lowerTypeReflectInStmt(instFn->body, richSubst, &diag_, unit_, this);
        }
    }

    std::vector<std::pair<std::string, VyxTypePtr>> savedTypes;
    for (const auto& [param, concreteType] : subst.substitutions) {
        savedTypes.emplace_back(param, symbols_.lookupType(param));
        symbols_.registerType(param, concreteType);
    }

    // Push const generic params so that identifier expressions like `return N;`
    // inside the instantiated body don't trigger "undefined symbol" during Sema.
    for (const auto& [cpName, cpType] : calleeDecl->genericConstParams) {
        activeConstGenericParams_.insert(cpName);
    }

    // Insert a placeholder before analysis so that recursive calls to the
    // same mangled name hit the cache and return early instead of looping.
    // Use the declared (substituted) return type as the placeholder so that
    // a recursive call inside the body (e.g. `return iter::<T,U>(n-1,acc+1)`)
    // gets the correct result type instead of void, which would cause a
    // spurious "cannot return 'void' from function returning 'U'" error.
    {
        VyxTypePtr placeholder = types::makeVoid();
        if (instantiatedDecl->kind == DeclKind::Function) {
            auto* instFn = instantiatedDecl->as<FunctionDecl>();
            if (instFn->returnType) {
                auto rt = resolveType(*instFn->returnType);
                if (rt && rt->kind != VyxTypeKind::Unknown)
                    placeholder = rt;
            }
        }
        instantiatedReturnTypes_[mangledName] = placeholder;
    }

    analyzeFunctionDecl(*instantiatedDecl);

    // Pop const generic params.
    for (const auto& [cpName, cpType] : calleeDecl->genericConstParams) {
        activeConstGenericParams_.erase(cpName);
    }

    auto* instFnSym = symbols_.lookup(mangledName);
    VyxTypePtr returnType = (instFnSym && instFnSym->returnType) ? instFnSym->returnType : types::makeVoid();
    instantiatedReturnTypes_[mangledName] = returnType;

    for (const auto& [param, oldType] : savedTypes) {
        if (oldType) {
            symbols_.registerType(param, oldType);
        } else {
            symbols_.unregisterType(param);
        }
    }

    // PLAN_SEMA_ROOT_FIX — S4: sidecar ledger (free-fn instantiation,
    // turbofish path).
    monoScheduler_.recordFreeFnRequest(mangledName);
    pendingInstantiations_.push_back(std::move(instantiatedDecl));
}

DeclPtr Sema::duplicateDecl(const Decl* decl) {
    if (decl->kind != DeclKind::Function) {
        auto copy = std::make_unique<FunctionDecl>();
        copy->location = decl->location;
        copy->name = decl->name;
        copy->isExport = decl->isExport;
        copy->visibility = decl->visibility;
        return copy;
    }

    auto* fnDecl = decl->as<FunctionDecl>();
    auto copy = std::make_unique<FunctionDecl>();
    copy->location = decl->location;
    copy->name = decl->name;
    
    for (const auto& param : fnDecl->params) {
        ParamDecl newParam;
        newParam.name = param.name;
        if (param.type) {
            newParam.type = cloneTypeAnnotation(*param.type);
        }
        newParam.isMutRef = param.isMutRef;
        copy->params.push_back(std::move(newParam));
    }
    
    if (fnDecl->returnType) {
        copy->returnType = cloneTypeAnnotation(*fnDecl->returnType);
    }
    
    copy->body = nullptr;
    copy->isExport = decl->isExport;
    copy->isAsync = fnDecl->isAsync;
    copy->visibility = decl->visibility;
    copy->isVariadicGeneric = decl->isVariadicGeneric;
    
    return copy;
}

DeclPtr Sema::createTemplateInstance(const Decl& genericDecl, const GenericSubstitution& subst) {
    auto* fnDecl = genericDecl.as<FunctionDecl>();
    auto instance = std::make_unique<FunctionDecl>();
    
    instance->location = genericDecl.location;
    instance->name = genericDecl.name;
    
    for (const auto& param : fnDecl->params) {
        ParamDecl newParam;
        newParam.name = param.name;
        if (param.type) {
            newParam.type = cloneTypeAnnotation(*param.type);
        }
        newParam.isMutRef = param.isMutRef;
        instance->params.push_back(std::move(newParam));
    }
    
    if (fnDecl->returnType) {
        instance->returnType = cloneTypeAnnotation(*fnDecl->returnType);
    }
    
    // Build a reifier lambda once so it can be shared across all WithSubst calls.
    auto reify = [this](const VyxTypePtr& t) -> TypePtr {
        return convertTypeToAnnotation(t);
    };

    if (fnDecl->body) {
        // Use the substituting clone so every TypeAnnotation inside the
        // function body (var-decl annotations, cast targets, turbofish
        // call-type-args, closure param/return types, match arm type
        // patterns, struct-init typeAnnotations, …) is rewritten from
        // generic parameter names (e.g. T) to the concrete bound types
        // (e.g. i32) before CodeGen ever sees the body.
        instance->body = cloneStatementWithSubst(*fnDecl->body,
                                                  templateResolver_, subst, reify);
    }

    instance->isExport = genericDecl.isExport;
    instance->isAsync = fnDecl->isAsync;
    instance->isComptime = fnDecl->isComptime;
    instance->isBench = fnDecl->isBench;
    instance->attributes = fnDecl->attributes;
    instance->visibility = genericDecl.visibility;
    instance->isVariadicGeneric = false;
    instance->genericParams.clear();

    for (auto& param : instance->params) {
        if (param.type) {
            auto substitutedType = templateResolver_.substituteType(*param.type, subst);
            param.type = convertTypeToAnnotation(substitutedType);
        }
    }

    if (instance->returnType) {
        auto substitutedType = templateResolver_.substituteType(*instance->returnType, subst);
        instance->returnType = convertTypeToAnnotation(substitutedType);
    }

    return instance;
}

void Sema::instantiateClassTemplate(const Decl& templateDecl,
    const std::string& mangledName,
    const std::vector<VyxTypePtr>& concreteTypes,
    SourceLocation useSite,
    const std::map<std::string, int64_t>& constBindings) {

    // Front 1 — P2D-020: cap the Sema-side instantiation stack. The
    // `InstantiationGuard` pushed below never checked the existing depth,
    // so a pathological template like
    //   class Rec<T> { field: Rec<Rec<T>>; }
    // that survives earlier cycle filters would recurse until the C++
    // stack blew. Dedup on the mangled-name key so repeat call sites
    // don't flood the diagnostic channel.
    if (instantiationStack_.size() >= kMaxSemaInstantiationStack) {
        std::string dedupKey = "P2D-020\x1f" + mangledName;
        if (reportedConstraintFailures_.insert(dedupKey).second) {
            SourceLocation siteLoc = (useSite.line == 0 && useSite.column == 0)
                ? templateDecl.location : useSite;
            diag_.errorCoded("P2D-020", siteLoc,
                "instantiation stack overflow while instantiating '{}' "
                "(depth {} >= {}); this usually indicates a recursive generic "
                "without a base case",
                mangledName, instantiationStack_.size(),
                kMaxSemaInstantiationStack);
            emitInstantiationChainNotes(siteLoc);
        }
        return;
    }

    // Build the type-substitution map. For non-type (const) parameters the
    // slot in `concreteTypes` is null — only the type params get a real
    // VyxType binding; const params are handled via `constBindings`.
    // `concreteTypes[i]` aligns with `templateDecl.genericParams[i]` one-to-one:
    // callers from SemaResolve pass nulls at const-param positions.
    GenericSubstitution subst;
    for (size_t i = 0; i < templateDecl.genericParams.size() && i < concreteTypes.size(); ++i) {
        const auto& pname = templateDecl.genericParams[i];
        bool isConstParam = templateDecl.genericConstParams.count(pname) > 0;
        if (!isConstParam && concreteTypes[i]) {
            subst.add(pname, concreteTypes[i]);
        }
    }

    // P2 C6: For variadic generic class templates (class Pair<...Ts>), set up
    // the pack-type substitution context so that PackIndexType annotations in
    // field types and method signatures (e.g. `a: Ts[0]; b: Ts[1]`) are
    // rewritten to the concrete types during field/method cloning below.
    // `concreteTypes` holds all packed concrete types for the variadic class
    // (caller in SemaResolve builds one entry per type argument).
    std::string variadicTypePackName;
    if (templateDecl.isVariadicGeneric && !templateDecl.genericParams.empty()) {
        variadicTypePackName = templateDecl.genericParams.front();
    }
    PackTypeSubstCtx prevPackCtx = g_packTypeSubstCtx;
    if (!variadicTypePackName.empty()) {
        g_packTypeSubstCtx.typePackName  = &variadicTypePackName;
        g_packTypeSubstCtx.valuePackName = nullptr; // no value-pack in class context
        g_packTypeSubstCtx.packTypes     = &concreteTypes;
        g_packTypeSubstCtx.sema          = this;
    }

    SourceLocation siteLoc = (useSite.line == 0 && useSite.column == 0)
        ? templateDecl.location : useSite;

    if (!templateDecl.genericConstraints.empty() ||
        !templateDecl.constPredicates.empty()    ||
        !templateDecl.reflectConstraints.empty() ||
        !templateDecl.typeofConstraints.empty()  ||
        !templateDecl.packConstraints.empty()) {
        InstantiationFrame frame;
        std::string targs;
        for (auto& at : concreteTypes) {
            if (!targs.empty()) targs += ", ";
            targs += at ? at->toString() : "?";
        }
        frame.calleeName = templateDecl.name + "<" + targs + ">";
        frame.site = siteLoc;
        for (auto& [tp, ct] : subst.substitutions)
            frame.bindings.emplace_back(tp, ct ? ct->toString() : "?");
        InstantiationGuard guard(*this, std::move(frame));
        if (!templateDecl.genericConstraints.empty() ||
            !templateDecl.reflectConstraints.empty() ||
            !templateDecl.typeofConstraints.empty()  ||
            !templateDecl.packConstraints.empty())
            verifyConstraints(templateDecl, subst.substitutions, siteLoc);
        // P2D-004: verify const-integer where-clause predicates.
        if (!templateDecl.constPredicates.empty())
            verifyConstPredicates(templateDecl, constBindings, siteLoc);
    }

    // Reifier shared across all WithSubst calls below.
    auto reify = [this](const VyxTypePtr& t) -> TypePtr {
        return convertTypeToAnnotation(t);
    };

    if (templateDecl.kind == DeclKind::Struct) {
        auto* sd = templateDecl.as<StructDecl>();
        auto instance = std::make_unique<StructDecl>();
        instance->location = templateDecl.location;
        instance->name = mangledName;
        // Concrete instantiation: all type-params are bound. Clear genericParams so
        // CodeGen treats this as a regular (non-template) struct/class — same as Mono.
        instance->genericParams = {};
        instance->parentName = sd->parentName;
        instance->visibility = templateDecl.visibility;
        instance->attributes = templateDecl.attributes;
        instance->docComment = templateDecl.docComment;

        for (auto& field : sd->fields) {
            FieldDecl f;
            f.name = field.name;
            f.visibility = field.visibility;
            f.isWeak = field.isWeak;
            f.location = field.location;
            f.extraNames = field.extraNames;
            if (field.type) {
                if (!variadicTypePackName.empty()) {
                    // P2 C6: variadic struct — route through pack-substituting
                    // cloner so `Ts[i]` annotations become concrete types.
                    f.type = cloneTypeWithPackSubst(*field.type, diag_);
                } else {
                    // Rewrite `[N]T` array-size identifiers to literals before
                    // TemplateResolver substitution so the VyxType arraySize is set.
                    TypePtr effectiveType;
                    if (!constBindings.empty())
                        effectiveType = rewriteConstSizesInType(field.type.get(), constBindings);
                    const TypeAnnotation& src = effectiveType ? *effectiveType : *field.type;
                    auto substituted = templateResolver_.substituteType(src, subst);
                    f.type = convertTypeToAnnotation(substituted);
                }
            }
            if (field.defaultValue)
                f.defaultValue = cloneExprWithSubst(*field.defaultValue,
                                                     templateResolver_, subst, reify);
            instance->fields.push_back(std::move(f));
        }

        g_packTypeSubstCtx = prevPackCtx;
        // PLAN_SEMA_ROOT_FIX — S4: sidecar ledger (struct instantiation).
        monoScheduler_.recordClassRequest(mangledName);
        pendingInstantiations_.push_back(std::move(instance));
        return;
    }

    auto* cd = templateDecl.as<ClassDecl>();
    auto instance = std::make_unique<ClassDecl>();
    instance->location = templateDecl.location;
    instance->name = mangledName;
    // Concrete instantiation: all type-params are bound. Clear genericParams so
    // CodeGen treats this as a regular (non-template) class — same as Mono.
    instance->genericParams = {};
    instance->parentName = cd->parentName;
    instance->interfaces = cd->interfaces;
    instance->visibility = templateDecl.visibility;
    instance->attributes = templateDecl.attributes;
    instance->docComment = templateDecl.docComment;

    for (auto& field : cd->fields) {
        FieldDecl f;
        f.name = field.name;
        f.visibility = field.visibility;
        f.isWeak = field.isWeak;
        f.location = field.location;
        f.extraNames = field.extraNames;
        if (field.type) {
            if (!variadicTypePackName.empty()) {
                // P2 C6: variadic class — route through pack-substituting
                // cloner so `Ts[i]` annotations become concrete types.
                f.type = cloneTypeWithPackSubst(*field.type, diag_);
            } else {
                // Rewrite `[N]T` array-size identifiers to literals before
                // TemplateResolver substitution so the VyxType arraySize is set.
                TypePtr effectiveType;
                if (!constBindings.empty())
                    effectiveType = rewriteConstSizesInType(field.type.get(), constBindings);
                const TypeAnnotation& src = effectiveType ? *effectiveType : *field.type;
                auto substituted = templateResolver_.substituteType(src, subst);
                f.type = convertTypeToAnnotation(substituted);
            }
        }
        if (field.defaultValue)
            f.defaultValue = cloneExprWithSubst(*field.defaultValue,
                                                 templateResolver_, subst, reify);
        instance->fields.push_back(std::move(f));
    }

    for (auto& method : cd->methods) {
        // R-phase-11 candidate 3++: "signature-kept / body-skipped" pattern,
        // extended to ALL method kinds (static + override + ordinary).
        //
        // Every method is LAZY BY DEFAULT at the SemaGeneric instance-clone
        // level: the signature is always cloned into the instance's method
        // list so Sema's member-lookup (`obj.method` / `Type::<T>.method`)
        // resolves correctly on generic-method-return receivers like
        // `b.map::<U>().get()`, but body substitution + Sema re-analysis is
        // deferred.
        //
        // Why this is safe for every kind:
        //   CodeGen never reads these instance-cloned bodies. At CodeGen
        //   time Mono's own deep-clone pipeline (Monomorphize::instantiate,
        //   DeclKind::Class branch) re-reads the ORIGINAL class template via
        //   `findDecl(name)` and substitutes bodies from there — so leaving a
        //   null `m.body` here does not starve the emitter; the method still
        //   gets a body once the Mono instance lands in
        //   `TranslationUnit::declarations`.
        //
        // Opt-OUT: `@[eager]` attribute forces body cloning + Sema
        // re-analysis at this site (useful when an error in a specific
        // instantiation should fire at class-clone time).
        //
        //   - `isOverride` used to be eager out of concern that the
        //     per-class vtable global needed a concrete function pointer at
        //     the class-clone point. That concern was moot:
        //     `generatePopulateInstanceVtables` (CodeGenGenerateInstanceVtables.cpp)
        //     explicitly SKIPS declarations whose name contains `<` (i.e.
        //     Mono-emitted instantiations like `Foo<i32>`), so vtable globals
        //     for generic instantiations never go through the
        //     "signature must be populated here" gate in the first place.
        //
        //   - `isStatic` used to be eager out of concern that the static
        //     dispatch path did not synth FunctionDecls at call sites yet.
        //     But static methods on generic classes are not emitted through
        //     the Sema instance clone either — they are reached via Mono's
        //     `requestTemplateWithArgs(Class, ...)` path, which seeds the
        //     whole class (fields + methods) from the original template.
        //     So dropping `!method.isStatic` too is a clone-time no-op for
        //     codegen correctness.
        //
        // The explicit `@[lazy]` attribute is still accepted for back-compat
        // but is redundant under the new default.
        bool hasExplicitLazy = false;
        bool hasExplicitEager = false;
        for (auto& [an, _av] : method.attributes) {
            if (an == "lazy")  hasExplicitLazy = true;
            if (an == "eager") hasExplicitEager = true;
        }
        bool isLazyMethod_ = hasExplicitLazy || !hasExplicitEager;

        bool constraintSatisfied = true;
        for (auto& [tparam, requiredType] : method.typeEqualityConstraints) {
            auto substIt = subst.substitutions.find(tparam);
            if (substIt != subst.substitutions.end()) {
                if (substIt->second->toString() != requiredType &&
                    substIt->second->name != requiredType) {
                    constraintSatisfied = false;
                    break;
                }
            }
        }
        for (auto& [tparam, excludedType] : method.typeInequalityConstraints) {
            auto substIt = subst.substitutions.find(tparam);
            if (substIt != subst.substitutions.end()) {
                if (substIt->second->toString() == excludedType ||
                    substIt->second->name == excludedType) {
                    constraintSatisfied = false;
                    break;
                }
            }
        }
        if (!constraintSatisfied) continue;

        MethodDecl m;
        m.name = method.name;
        m.visibility = method.visibility;
        m.isOverride = method.isOverride;
        m.isAsync = method.isAsync;
        m.isStatic = method.isStatic;
        m.location = method.location;
        m.attributes = method.attributes;
        m.docComment = method.docComment;

        if (method.returnType) {
            // Apply const-param substitution (N→16) before type-param
            // substitution (T→i32) so that e.g. `Buffer<T, N>` becomes
            // `Buffer<i32,16>` rather than the intermediate `Buffer<i32,N>`
            // which confuses convertTypeToAnnotation's unresolved-param check.
            TypePtr effectiveRetType;
            if (!constBindings.empty())
                effectiveRetType = rewriteConstSizesInType(
                    method.returnType.get(), constBindings);
            const TypeAnnotation& retSrc =
                effectiveRetType ? *effectiveRetType : *method.returnType;
            auto substituted = templateResolver_.substituteType(retSrc, subst);
            m.returnType = convertTypeToAnnotation(substituted);
        }

        for (auto& param : method.params) {
            ParamDecl p;
            p.name = param.name;
            p.isMutRef = param.isMutRef;
            if (param.type) {
                TypePtr effectiveParamType;
                if (!constBindings.empty())
                    effectiveParamType = rewriteConstSizesInType(
                        param.type.get(), constBindings);
                const TypeAnnotation& paramSrc =
                    effectiveParamType ? *effectiveParamType : *param.type;
                auto substituted = templateResolver_.substituteType(paramSrc, subst);
                p.type = convertTypeToAnnotation(substituted);
            }
            if (param.defaultValue)
                p.defaultValue = cloneExprWithSubst(*param.defaultValue,
                                                     templateResolver_, subst, reify);
            m.params.push_back(std::move(p));
        }

        if (method.body && !isLazyMethod_) {
            if (!variadicTypePackName.empty()) {
                // Gap 2: variadic class template — route method body through
                // the pack-aware statement cloner so `Ts[i]` PackIndexType
                // annotations inside the body (e.g. `var x: Ts[0]`) are
                // resolved to the concrete packed type.
                // g_packTypeSubstCtx was already set up above with
                // typePackName / packTypes / sema, so cloneStmtWithPackSubst
                // will call cloneTypeWithPackSubst (which reads that context)
                // for every TypeAnnotation node it encounters.
                m.body = cloneStmtWithPackSubst(*method.body,
                                                variadicTypePackName,
                                                concreteTypes.size(),
                                                diag_);
            } else {
                // Non-variadic class template: substitute generic parameter
                // names (e.g. `T`) with their concrete bound types in all
                // TypeAnnotation positions inside the body.
                //   - VarDecl annotations        (var x: T → var x: i32)
                //   - Cast target types          (val as T → val as i32)
                //   - Turbofish call type-args   (foo::<T>() → foo::<i32>())
                //   - Struct-init type annotation (Foo::<T>{} → Foo::<i32>{})
                //   - Closure param/return types
                //   - Match arm type patterns    (is T => … → is i32 => …)
                m.body = cloneStatementWithSubst(*method.body,
                                                  templateResolver_, subst, reify);
            }
            // Const-param identifier substitution for class template bodies.
            // Mirrors the function-template path in instantiateTemplateIfNeeded —
            // `return N;` inside a class method must lower to `return 16;`
            // before Sema re-analyses the instance.
            if (!constBindings.empty() && m.body) {
                substConstIdentsInStmt(m.body, constBindings);
            }
        }
        // If isLazyMethod_, m.body is intentionally left null. Mono will
        // instantiate the body on-demand via requestLazyMethodInstantiation
        // when it sees an actual call site.
        m.typeEqualityConstraints = method.typeEqualityConstraints;
        instance->methods.push_back(std::move(m));
    }

    // Specificity: remove less-specific duplicates
    {
        std::map<std::string, size_t> bestConstraintCount;
        for (auto& m : instance->methods) {
            size_t cc = m.typeEqualityConstraints.size();
            auto it = bestConstraintCount.find(m.name);
            if (it == bestConstraintCount.end() || cc > it->second) {
                bestConstraintCount[m.name] = cc;
            }
        }
        std::vector<MethodDecl> deduped;
        for (auto& m : instance->methods) {
            size_t best = bestConstraintCount[m.name];
            if (best == 0 || m.typeEqualityConstraints.size() == best) {
                deduped.push_back(std::move(m));
            }
        }
        instance->methods = std::move(deduped);
    }

    g_packTypeSubstCtx = prevPackCtx;
    // PLAN_SEMA_ROOT_FIX — S4: sidecar ledger (class instantiation).
    monoScheduler_.recordClassRequest(mangledName);
    pendingInstantiations_.push_back(std::move(instance));
}

// ============================================================
//  Variadic generic expansion
// ============================================================

// ----------------------------------------------------------------
// Pack-substitution helpers
//
// These functions clone an expression / statement / type tree while
// rewriting three pack-specific patterns:
//
//   <packName>[<integer-literal>]   →  IdentifierExpr "<packName>_<i>"   (P1a, value position)
//   <packName>.len                  →  IntLiteralExpr  <packLen>          (P1a)
//   PackIndexType{packName, i}      →  TypeAnnotation of argTypes[i]     (P2 C6, type position)
//
// All other sub-expressions / sub-types are cloned verbatim (via
// cloneExpr / cloneStatement / cloneTypeAnnotation from SemaCloneHelpers).
// ----------------------------------------------------------------

// ── Pack substitution context (P2 C6) ──
//
// `PackTypeSubstCtx`, `g_packTypeSubstCtx`, and the
// `cloneTypeWithPackSubst` forward-declaration have been moved to the
// top of this file (before `instantiateClassTemplate`) so the class-template
// instantiation path can also reference them. See the block right after
// `rewriteConstSizesInType` for the definitions. The thread-local context
// is set up by both `createVariadicInstance` (function path) and
// `instantiateClassTemplate` (class/struct path), and torn down before
// their respective returns.
//
// `cloneTypeWithPackSubst` needs:
//   - the *type* pack name (`Ts` in `fn foo<...Ts>(...args: Ts)`)
//   - the *value* pack name (`args`) so `args[i]` and `Ts[i]` resolve the
//     same slot (only set in the function path)
//   - the concrete type list bound to the pack
//   - a `Sema*` to project a VyxType back to a TypeAnnotation

static ExprPtr cloneExprWithPackSubst(
    const Expr& expr,
    const std::string& packName,
    size_t packLen,
    DiagnosticsEngine& diag)
{
    // args.len  →  integer literal
    if (expr.kind == ExprKind::MemberAccess) {
        auto* ma = expr.as<MemberAccessExpr>();
        if (ma->member == "len" &&
            ma->object && ma->object->kind == ExprKind::Identifier &&
            ma->object->as<IdentifierExpr>()->name == packName) {
            auto lit = std::make_unique<IntLiteralExpr>();
            lit->location = expr.location;
            lit->value = static_cast<int64_t>(packLen);
            return lit;
        }
        // Otherwise clone children
        auto cloned = std::make_unique<MemberAccessExpr>();
        cloned->location = expr.location;
        cloned->member = ma->member;
        if (ma->object)
            cloned->object = cloneExprWithPackSubst(*ma->object, packName, packLen, diag);
        for (const auto& ta : ma->callTypeArgs)
            if (ta) cloned->callTypeArgs.push_back(cloneTypeWithPackSubst(*ta, diag));
        return cloned;
    }

    // args[<literal>]  →  identifier args_<i>       (constexpr pack indexing)
    // args[<runtime>]  →  __pack_args[<runtime>]    (on-demand materialization)
    //
    // The constexpr form inlines the corresponding pack element directly, so
    // unrolled monomorphs see no `args` name at all (each args_i is a separate
    // function parameter). The runtime form routes through a synthesized
    // `[T; N]` stack array named `__pack_<packName>`, which `expandVariadicBody`
    // injects at the top of the body iff this rewrite actually fires.
    if (expr.kind == ExprKind::Index) {
        auto* idx = expr.as<IndexExpr>();
        if (idx->object && idx->object->kind == ExprKind::Identifier &&
            idx->object->as<IdentifierExpr>()->name == packName) {
            if (idx->indexExpr && idx->indexExpr->kind == ExprKind::IntLiteral) {
                int64_t idxVal = idx->indexExpr->as<IntLiteralExpr>()->value;
                if (idxVal < 0 || static_cast<size_t>(idxVal) >= packLen) {
                    diag.error(expr.location,
                        "pack index {} out of range (pack '{}' has {} element{})",
                        idxVal, packName, packLen, packLen == 1 ? "" : "s");
                    auto null_ = std::make_unique<NullLiteralExpr>();
                    null_->location = expr.location;
                    return null_;
                }
                auto ident = std::make_unique<IdentifierExpr>();
                ident->location = expr.location;
                ident->name = packName + "_" + std::to_string(static_cast<size_t>(idxVal));
                return ident;
            }
            // Runtime index: redirect to the on-demand materialized array.
            auto cloned = std::make_unique<IndexExpr>();
            cloned->location = expr.location;
            auto newObj = std::make_unique<IdentifierExpr>();
            newObj->location = idx->object->location;
            newObj->name = "__pack_" + packName;
            cloned->object = std::move(newObj);
            cloned->indexExpr = cloneExprWithPackSubst(*idx->indexExpr, packName, packLen, diag);
            return cloned;
        }
        // Not a pack index – clone children normally
        auto cloned = std::make_unique<IndexExpr>();
        cloned->location = expr.location;
        if (idx->object)
            cloned->object = cloneExprWithPackSubst(*idx->object, packName, packLen, diag);
        if (idx->indexExpr)
            cloned->indexExpr = cloneExprWithPackSubst(*idx->indexExpr, packName, packLen, diag);
        return cloned;
    }

    // Recurse into all other expression kinds, cloning children with substitution
    switch (expr.kind) {
        case ExprKind::BinaryOp: {
            auto* src = expr.as<BinaryOpExpr>();
            auto cloned = std::make_unique<BinaryOpExpr>();
            cloned->location = expr.location;
            cloned->op = src->op;
            if (src->lhs) cloned->lhs = cloneExprWithPackSubst(*src->lhs, packName, packLen, diag);
            if (src->rhs) cloned->rhs = cloneExprWithPackSubst(*src->rhs, packName, packLen, diag);
            return cloned;
        }
        case ExprKind::UnaryOp: {
            auto* src = expr.as<UnaryOpExpr>();
            auto cloned = std::make_unique<UnaryOpExpr>();
            cloned->location = expr.location;
            cloned->op = src->op;
            if (src->operand) cloned->operand = cloneExprWithPackSubst(*src->operand, packName, packLen, diag);
            return cloned;
        }
        case ExprKind::Call: {
            auto* src = expr.as<CallExpr>();
            auto cloned = std::make_unique<CallExpr>();
            cloned->location = expr.location;
            if (src->callee) cloned->callee = cloneExprWithPackSubst(*src->callee, packName, packLen, diag);
            for (const auto& arg : src->args)
                if (arg) cloned->args.push_back(cloneExprWithPackSubst(*arg, packName, packLen, diag));
            cloned->argNames = src->argNames;
            return cloned;
        }
        case ExprKind::Assignment: {
            auto* src = expr.as<AssignmentExpr>();
            auto cloned = std::make_unique<AssignmentExpr>();
            cloned->location = expr.location;
            if (src->lhs) cloned->lhs = cloneExprWithPackSubst(*src->lhs, packName, packLen, diag);
            if (src->rhs) cloned->rhs = cloneExprWithPackSubst(*src->rhs, packName, packLen, diag);
            return cloned;
        }
        case ExprKind::CompoundAssignment: {
            auto* src = expr.as<CompoundAssignmentExpr>();
            auto cloned = std::make_unique<CompoundAssignmentExpr>();
            cloned->location = expr.location;
            cloned->op = src->op;
            if (src->target) cloned->target = cloneExprWithPackSubst(*src->target, packName, packLen, diag);
            if (src->value)  cloned->value  = cloneExprWithPackSubst(*src->value,  packName, packLen, diag);
            return cloned;
        }
        case ExprKind::Cast: {
            auto* src = expr.as<CastExpr>();
            auto cloned = std::make_unique<CastExpr>();
            cloned->location = expr.location;
            if (src->operand) cloned->operand = cloneExprWithPackSubst(*src->operand, packName, packLen, diag);
            if (src->targetType) cloned->targetType = cloneTypeWithPackSubst(*src->targetType, diag);
            return cloned;
        }
        case ExprKind::StructInit: {
            auto* src = expr.as<StructInitExpr>();
            auto cloned = std::make_unique<StructInitExpr>();
            cloned->location = expr.location;
            cloned->structName = src->structName;
            for (const auto& [name, fExpr] : src->fieldInits)
                if (fExpr) cloned->fieldInits.emplace_back(name, cloneExprWithPackSubst(*fExpr, packName, packLen, diag));
            if (src->spreadBase) cloned->spreadBase = cloneExprWithPackSubst(*src->spreadBase, packName, packLen, diag);
            if (src->typeAnnotation) cloned->typeAnnotation = cloneTypeWithPackSubst(*src->typeAnnotation, diag);
            return cloned;
        }
        case ExprKind::ArrayInit: {
            auto* src = expr.as<ArrayInitExpr>();
            auto cloned = std::make_unique<ArrayInitExpr>();
            cloned->location = expr.location;
            for (const auto& elem : src->elements)
                if (elem) cloned->elements.push_back(cloneExprWithPackSubst(*elem, packName, packLen, diag));
            cloned->elementIsSpread = src->elementIsSpread;
            return cloned;
        }
        case ExprKind::TupleInit: {
            auto* src = expr.as<TupleInitExpr>();
            auto cloned = std::make_unique<TupleInitExpr>();
            cloned->location = expr.location;
            for (const auto& elem : src->elements)
                if (elem) cloned->elements.push_back(cloneExprWithPackSubst(*elem, packName, packLen, diag));
            return cloned;
        }
        case ExprKind::StringInterpolation: {
            auto* src = expr.as<StringInterpExpr>();
            auto cloned = std::make_unique<StringInterpExpr>();
            cloned->location = expr.location;
            for (const auto& part : src->parts) {
                InterpPart cp;
                cp.isExpr = part.isExpr;
                cp.text   = part.text;
                if (part.expr) cp.expr = cloneExprWithPackSubst(*part.expr, packName, packLen, diag);
                cloned->parts.push_back(std::move(cp));
            }
            return cloned;
        }
        case ExprKind::TryExpr: {
            auto* src = expr.as<TryExpr>();
            auto cloned = std::make_unique<TryExpr>();
            cloned->location = expr.location;
            if (src->inner) cloned->inner = cloneExprWithPackSubst(*src->inner, packName, packLen, diag);
            return cloned;
        }
        case ExprKind::AwaitExpr: {
            auto* src = expr.as<AwaitExpr>();
            auto cloned = std::make_unique<AwaitExpr>();
            cloned->location = expr.location;
            if (src->inner) cloned->inner = cloneExprWithPackSubst(*src->inner, packName, packLen, diag);
            return cloned;
        }
        case ExprKind::Ternary: {
            auto* src = expr.as<TernaryExpr>();
            auto cloned = std::make_unique<TernaryExpr>();
            cloned->location = expr.location;
            if (src->condition) cloned->condition = cloneExprWithPackSubst(*src->condition, packName, packLen, diag);
            if (src->trueExpr)  cloned->trueExpr  = cloneExprWithPackSubst(*src->trueExpr,  packName, packLen, diag);
            if (src->falseExpr) cloned->falseExpr = cloneExprWithPackSubst(*src->falseExpr, packName, packLen, diag);
            return cloned;
        }
        case ExprKind::FailExpr: {
            auto* src = expr.as<FailExpr>();
            auto cloned = std::make_unique<FailExpr>();
            cloned->location = expr.location;
            cloned->typeName = src->typeName;
            cloned->variant  = src->variant;
            cloned->isPayload = src->isPayload;
            if (src->message) cloned->message = cloneExprWithPackSubst(*src->message, packName, packLen, diag);
            return cloned;
        }
        case ExprKind::Closure: {
            auto* src = expr.as<ClosureExpr>();
            auto cloned = std::make_unique<ClosureExpr>();
            cloned->location = expr.location;
            for (const auto& cap : src->captures) {
                CaptureItem cc;
                cc.name  = cap.name;
                cc.byRef = cap.byRef;
                cc.move  = cap.move;
                if (cap.moveExpr) cc.moveExpr = cloneExprWithPackSubst(*cap.moveExpr, packName, packLen, diag);
                cloned->captures.push_back(std::move(cc));
            }
            for (const auto& p : src->params) {
                ClosureParam cp;
                cp.name = p.name;
                if (p.type) cp.type = cloneTypeWithPackSubst(*p.type, diag);
                cloned->params.push_back(std::move(cp));
            }
            if (src->returnType) cloned->returnType = cloneTypeWithPackSubst(*src->returnType, diag);
            // Closure body: use plain cloneStatement (pack not in scope inside closure)
            if (src->body)       cloned->body       = cloneStatement(*src->body);
            if (src->singleExpr) cloned->singleExpr = cloneExprWithPackSubst(*src->singleExpr, packName, packLen, diag);
            return cloned;
        }
        // P5-pack: pack fold expansion inside Sema's variadic body cloner.
        // `args...+` with packLen members expands to `args_0 op args_1 op ... op args_{N-1}`.
        case ExprKind::PackFold: {
            auto* src = expr.as<PackFoldExpr>();
            // Wrong-pack passthrough — Mono may handle it later.
            if (src->packName != packName) {
                auto cloned = std::make_unique<PackFoldExpr>();
                cloned->location = expr.location;
                cloned->packName = src->packName;
                cloned->op       = src->op;
                return cloned;
            }
            // Empty pack: emit the operator's identity element, or diagnose
            // for operators without an identity. CodeGen must never see a
            // PackFoldExpr, so we resolve here.
            if (packLen == 0) {
                switch (src->op) {
                    case BinaryOp::Add:
                    case BinaryOp::Sub:
                    case BinaryOp::BitOr:
                    case BinaryOp::BitXor: {
                        auto z = std::make_unique<IntLiteralExpr>();
                        z->location = expr.location;
                        z->value = 0;
                        return z;
                    }
                    case BinaryOp::Mul: {
                        auto o = std::make_unique<IntLiteralExpr>();
                        o->location = expr.location;
                        o->value = 1;
                        return o;
                    }
                    case BinaryOp::And: {
                        auto b = std::make_unique<BoolLiteralExpr>();
                        b->location = expr.location;
                        b->value = true;  // all() of nothing = true
                        return b;
                    }
                    case BinaryOp::Or: {
                        auto b = std::make_unique<BoolLiteralExpr>();
                        b->location = expr.location;
                        b->value = false; // any() of nothing = false
                        return b;
                    }
                    case BinaryOp::BitAnd: {
                        auto m = std::make_unique<IntLiteralExpr>();
                        m->location = expr.location;
                        m->value = -1;    // all-bits-set identity
                        return m;
                    }
                    default: {
                        diag.error(expr.location,
                            "pack fold over empty pack '{}' has no identity for this operator; "
                            "guard with `where {}.len > 0` or supply an explicit seed",
                            src->packName, src->packName);
                        auto z = std::make_unique<IntLiteralExpr>();
                        z->location = expr.location;
                        z->value = 0;
                        return z;
                    }
                }
            }
            // Build left-associative chain: args_0 op args_1 op ... op args_{N-1}
            auto makeArg = [&](size_t i) -> ExprPtr {
                auto id = std::make_unique<IdentifierExpr>();
                id->location = expr.location;
                id->name = src->packName + "_" + std::to_string(i);
                return id;
            };
            ExprPtr chain = makeArg(0);
            for (size_t i = 1; i < packLen; ++i) {
                auto bin = std::make_unique<BinaryOpExpr>();
                bin->location = expr.location;
                bin->op  = src->op;
                bin->lhs = std::move(chain);
                bin->rhs = makeArg(i);
                chain = std::move(bin);
            }
            return chain;
        }

        // F: sizeof...(pack) — resolve to the concrete pack length.
        case ExprKind::SizeofPack: {
            auto* src = expr.as<SizeofPackExpr>();
            // If this is the pack we're currently expanding, resolve to the count.
            // Otherwise clone verbatim (wrong pack; Mono will handle it).
            if (src->packName == packName) {
                auto lit = std::make_unique<IntLiteralExpr>();
                lit->location = expr.location;
                lit->value = static_cast<int64_t>(packLen);
                return lit;
            }
            auto cloned = std::make_unique<SizeofPackExpr>();
            cloned->location = expr.location;
            cloned->packName = src->packName;
            return cloned;
        }

        default:
            // Leaf nodes (literals, self, null, inline-asm, …): plain clone
            return cloneExpr(expr);
    }
}

// Forward declaration for mutual recursion
static StmtPtr cloneStmtWithPackSubst(
    const Stmt& stmt,
    const std::string& packName,
    size_t packLen,
    DiagnosticsEngine& diag);

static StmtPtr cloneStmtWithPackSubst(
    const Stmt& stmt,
    const std::string& packName,
    size_t packLen,
    DiagnosticsEngine& diag)
{
    // Helper to shorten repeated calls
    auto subExpr = [&](const Expr& e) -> ExprPtr {
        return cloneExprWithPackSubst(e, packName, packLen, diag);
    };
    auto subStmt = [&](const Stmt& s) -> StmtPtr {
        return cloneStmtWithPackSubst(s, packName, packLen, diag);
    };

    switch (stmt.kind) {
        case StmtKind::VarDecl: {
            auto* src = stmt.as<VarDeclStmt>();
            auto cloned = std::make_unique<VarDeclStmt>();
            cloned->location   = stmt.location;
            cloned->isConst    = src->isConst;
            cloned->isComptime = src->isComptime;
            cloned->varName    = src->varName;
            if (src->varType)    cloned->varType    = cloneTypeWithPackSubst(*src->varType, diag);
            if (src->initExpr)   cloned->initExpr   = subExpr(*src->initExpr);
            if (src->elseBranch) cloned->elseBranch = subStmt(*src->elseBranch);
            return cloned;
        }
        case StmtKind::ExprStmt: {
            auto* src = stmt.as<ExprStmt>();
            auto cloned = std::make_unique<ExprStmt>();
            cloned->location = stmt.location;
            if (src->expr) cloned->expr = subExpr(*src->expr);
            return cloned;
        }
        case StmtKind::Return: {
            auto* src = stmt.as<ReturnStmt>();
            auto cloned = std::make_unique<ReturnStmt>();
            cloned->location = stmt.location;
            if (src->expr) cloned->expr = subExpr(*src->expr);
            return cloned;
        }
        case StmtKind::If: {
            auto* src = stmt.as<IfStmt>();
            auto cloned = std::make_unique<IfStmt>();
            cloned->location = stmt.location;
            if (src->condition)  cloned->condition  = subExpr(*src->condition);
            if (src->thenBranch) cloned->thenBranch = subStmt(*src->thenBranch);
            for (const auto& [cond, branch] : src->elifBranches)
                if (cond && branch) cloned->elifBranches.emplace_back(subExpr(*cond), subStmt(*branch));
            if (src->elseBranch) cloned->elseBranch = subStmt(*src->elseBranch);
            return cloned;
        }
        case StmtKind::While: {
            auto* src = stmt.as<WhileStmt>();
            auto cloned = std::make_unique<WhileStmt>();
            cloned->location = stmt.location;
            if (src->condition) cloned->condition = subExpr(*src->condition);
            if (src->body)      cloned->body      = subStmt(*src->body);
            return cloned;
        }
        case StmtKind::For: {
            auto* src = stmt.as<ForStmt>();
            auto cloned = std::make_unique<ForStmt>();
            cloned->location = stmt.location;
            if (src->init)      cloned->init      = subStmt(*src->init);
            if (src->condition) cloned->condition = subExpr(*src->condition);
            if (src->step)      cloned->step      = subExpr(*src->step);
            if (src->body)      cloned->body      = subStmt(*src->body);
            return cloned;
        }
        case StmtKind::ForEach: {
            auto* src = stmt.as<ForEachStmt>();
            auto cloned = std::make_unique<ForEachStmt>();
            cloned->location    = stmt.location;
            cloned->varName     = src->varName;
            cloned->destructure = src->destructure;
            if (src->collection) cloned->collection = subExpr(*src->collection);
            if (src->body)       cloned->body       = subStmt(*src->body);
            return cloned;
        }
        case StmtKind::Block: {
            auto* src = stmt.as<BlockStmt>();
            auto cloned = std::make_unique<BlockStmt>();
            cloned->location = stmt.location;
            for (const auto& s : src->statements)
                if (s) cloned->statements.push_back(subStmt(*s));
            return cloned;
        }
        case StmtKind::Assignment: {
            auto* src = stmt.as<AssignStmt>();
            auto cloned = std::make_unique<AssignStmt>();
            cloned->location = stmt.location;
            if (src->target) cloned->target = subExpr(*src->target);
            if (src->value)  cloned->value  = subExpr(*src->value);
            return cloned;
        }
        case StmtKind::Defer: {
            auto* src = stmt.as<DeferStmt>();
            auto cloned = std::make_unique<DeferStmt>();
            cloned->location = stmt.location;
            if (src->body) cloned->body = subStmt(*src->body);
            return cloned;
        }
        case StmtKind::StaticAssert: {
            auto* src = stmt.as<StaticAssertStmt>();
            auto cloned = std::make_unique<StaticAssertStmt>();
            cloned->location = stmt.location;
            if (src->expr) cloned->expr = subExpr(*src->expr);
            cloned->message = src->message;
            return cloned;
        }
        case StmtKind::Unsafe: {
            auto* src = stmt.as<UnsafeStmt>();
            auto cloned = std::make_unique<UnsafeStmt>();
            cloned->location = stmt.location;
            if (src->body) cloned->body = subStmt(*src->body);
            return cloned;
        }
        case StmtKind::Match: {
            auto* src = stmt.as<MatchStmt>();
            auto cloned = std::make_unique<MatchStmt>();
            cloned->location = stmt.location;
            if (src->expr) cloned->expr = subExpr(*src->expr);
            for (const auto& arm : src->arms) {
                MatchArm ca;
                ca.location       = arm.location;
                ca.label          = arm.label;
                ca.bindingName    = arm.bindingName;
                ca.isDefault      = arm.isDefault;
                ca.tupleBindings  = arm.tupleBindings;
                ca.nestedPatterns = arm.nestedPatterns;
                if (arm.typePattern)  ca.typePattern  = cloneTypeWithPackSubst(*arm.typePattern, diag);
                if (arm.valuePattern) ca.valuePattern = subExpr(*arm.valuePattern);
                if (arm.guardExpr)    ca.guardExpr    = subExpr(*arm.guardExpr);
                if (arm.body)         ca.body         = subStmt(*arm.body);
                cloned->arms.push_back(std::move(ca));
            }
            return cloned;
        }
        // Leaf statements
        default:
            return cloneStatement(stmt);
    }
}

// ----------------------------------------------------------------
// Type-pack indexing in type position (P2 C6).
//
// Rewrites `Ts[i]` (PackIndexType) to the concrete i-th type bound to
// the active pack, recursing into composite annotations (Pointer,
// Reference, Array, Tuple, Function, Generic, Union). Outside a
// variadic-expansion context, behaves exactly like cloneTypeAnnotation
// (so PackIndexType remains a transparent "I am still a placeholder"
// node when no expansion is active — useful for diagnostic clarity
// before Sema has run).
// ----------------------------------------------------------------
static TypePtr cloneTypeWithPackSubst(
    const TypeAnnotation& type,
    DiagnosticsEngine& diag)
{
    auto recurse = [&](const TypeAnnotation& t) -> TypePtr {
        return cloneTypeWithPackSubst(t, diag);
    };

    if (type.kind == TypeAnnotationKind::PackIndex) {
        auto* pi = type.as<PackIndexType>();
        const auto& ctx = g_packTypeSubstCtx;

        // Outside an active expansion: keep the placeholder intact so
        // downstream code can still inspect / diagnose it.
        if (!ctx.packTypes || !ctx.sema ||
            (!ctx.typePackName && !ctx.valuePackName)) {
            auto cloned = std::make_unique<PackIndexType>();
            cloned->location = type.location;
            cloned->name = type.name;
            cloned->packName = pi->packName;
            if (pi->indexExpr) cloned->indexExpr = cloneExpr(*pi->indexExpr);
            return cloned;
        }

        // Accept either the type-pack name (`Ts`) or the value-pack
        // name (`args`) since both refer to the same underlying slot
        // — users naturally write the former, but defensive code that
        // pivots on the value pack name is also valid.
        bool packMatches =
            (ctx.typePackName  && pi->packName == *ctx.typePackName) ||
            (ctx.valuePackName && pi->packName == *ctx.valuePackName);
        if (!packMatches) {
            std::string activeNames;
            if (ctx.typePackName)  activeNames += "'" + *ctx.typePackName + "'";
            if (ctx.valuePackName) {
                if (!activeNames.empty()) activeNames += " / ";
                activeNames += "'" + *ctx.valuePackName + "'";
            }
            diag.error(type.location,
                "type-pack index references unknown pack '{}' (active pack: {})",
                pi->packName, activeNames);
            return ast::makeNamedType(type.location, "error");
        }

        // Index must be a non-negative integer literal. We deliberately
        // don't try to constant-fold here — anything fancier is a
        // future feature; today we just reject so the user sees a
        // clear diagnostic instead of a confusing downstream type
        // error.
        if (!pi->indexExpr || pi->indexExpr->kind != ExprKind::IntLiteral) {
            diag.error(type.location,
                "type-pack index for '{}' must be an integer literal",
                pi->packName);
            return ast::makeNamedType(type.location, "error");
        }
        int64_t idxVal = pi->indexExpr->as<IntLiteralExpr>()->value;
        size_t packLen = ctx.packTypes->size();
        if (idxVal < 0 || static_cast<size_t>(idxVal) >= packLen) {
            // Diagnostic format mirrors the value-position equivalent
            // emitted from cloneExprWithPackSubst (P1a) so users see a
            // consistent message regardless of whether the offending
            // `[i]` was on a value or a type.
            diag.error(type.location,
                "pack index {} out of range (pack '{}' has {} element{})",
                idxVal, pi->packName, packLen, packLen == 1 ? "" : "s");
            // Gap 1: attach instantiation chain so the user can trace which
            // template call triggered this OOB. ctx.sema is always set here
            // because we are inside the active-expansion branch (ctx.packTypes
            // and ctx.sema were both checked non-null above).
            ::vyx::emitInstantiationChainNotes(diag, type.location,
                ctx.sema->getInstantiationStack());
            return ast::makeNamedType(type.location, "error");
        }

        const auto& concrete = (*ctx.packTypes)[static_cast<size_t>(idxVal)];
        if (!concrete) return ast::makeNamedType(type.location, "error");
        auto out = ctx.sema->convertTypeToAnnotation(concrete);
        if (out) out->location = type.location;
        return out;
    }

    // Composite annotations: clone shell, recurse on children.
    switch (type.kind) {
        case TypeAnnotationKind::Pointer: {
            auto* src = type.as<PointerType>();
            auto cloned = std::make_unique<PointerType>();
            cloned->location = type.location;
            cloned->name = type.name;
            if (src->innerType) cloned->innerType = recurse(*src->innerType);
            return cloned;
        }
        case TypeAnnotationKind::Reference: {
            auto* src = type.as<ReferenceType>();
            auto cloned = std::make_unique<ReferenceType>();
            cloned->location = type.location;
            cloned->name = type.name;
            cloned->isMutable = src->isMutable;
            if (src->innerType) cloned->innerType = recurse(*src->innerType);
            return cloned;
        }
        case TypeAnnotationKind::Array: {
            auto* src = type.as<ArrayType>();
            auto cloned = std::make_unique<ArrayType>();
            cloned->location = type.location;
            cloned->name = type.name;
            if (src->elementType) cloned->elementType = recurse(*src->elementType);
            if (src->size) cloned->size = cloneExpr(*src->size);
            return cloned;
        }
        case TypeAnnotationKind::Tuple: {
            auto* src = type.as<TupleType>();
            auto cloned = std::make_unique<TupleType>();
            cloned->location = type.location;
            cloned->name = type.name;
            for (const auto& e : src->elements)
                if (e) cloned->elements.push_back(recurse(*e));
            return cloned;
        }
        case TypeAnnotationKind::Function: {
            auto* src = type.as<FunctionType>();
            auto cloned = std::make_unique<FunctionType>();
            cloned->location = type.location;
            cloned->name = type.name;
            for (const auto& pt : src->paramTypes)
                if (pt) cloned->paramTypes.push_back(recurse(*pt));
            if (src->returnType) cloned->returnType = recurse(*src->returnType);
            return cloned;
        }
        case TypeAnnotationKind::Generic: {
            auto* src = type.as<GenericType>();
            auto cloned = std::make_unique<GenericType>();
            cloned->location = type.location;
            cloned->name = type.name;
            // Keep slot alignment with argExprs: const slots have typeArgs[i]==null
            // and argExprs[i]==IntLiteral; dropping nulls misaligns the vectors.
            cloned->typeArgs.reserve(src->typeArgs.size());
            for (const auto& arg : src->typeArgs)
                cloned->typeArgs.push_back(arg ? recurse(*arg) : nullptr);
            cloned->argExprs.reserve(src->argExprs.size());
            for (const auto& ae : src->argExprs)
                cloned->argExprs.push_back(ae ? cloneExpr(*ae) : nullptr);
            return cloned;
        }
        case TypeAnnotationKind::Union: {
            auto* src = type.as<UnionType>();
            auto cloned = std::make_unique<UnionType>();
            cloned->location = type.location;
            cloned->name = type.name;
            for (const auto& m : src->members)
                if (m) cloned->members.push_back(recurse(*m));
            return cloned;
        }
        case TypeAnnotationKind::Named: {
            // Sugar-form substitution: when `<T>(args: ...T) -> T` (the
            // promoted-to-variadic single-generic-param case), NamedType("T")
            // in return type / body type annotations is really the pack
            // element type — substitute it to the concrete argType[0] via
            // the context's elementTypeName/elementConcreteType binding.
            const auto& ctx = g_packTypeSubstCtx;
            if (ctx.elementTypeName && ctx.elementConcreteType &&
                ctx.sema && type.name == *ctx.elementTypeName) {
                auto out = ctx.sema->convertTypeToAnnotation(ctx.elementConcreteType);
                if (out) out->location = type.location;
                if (out) return out;
            }
            return cloneTypeAnnotation(type);
        }
        default:
            return cloneTypeAnnotation(type);
    }
}

DeclPtr Sema::createVariadicInstance(const Decl& genericDecl, const std::vector<VyxTypePtr>& argTypes) {
    auto* fnDecl = genericDecl.as<FunctionDecl>();
    auto instance = std::make_unique<FunctionDecl>();
    instance->location = genericDecl.location;
    instance->name = genericDecl.name;
    instance->isExport = genericDecl.isExport;
    instance->isAsync = fnDecl->isAsync;
    instance->isComptime = fnDecl->isComptime;
    instance->isBench = fnDecl->isBench;
    instance->attributes = fnDecl->attributes;
    instance->visibility = genericDecl.visibility;
    instance->isVariadicGeneric = false;
    instance->genericParams.clear();

    std::string variadicParamName;
    size_t variadicParamIdx = 0;
    bool variadicParamFound = false;
    for (size_t pi = 0; pi < fnDecl->params.size(); ++pi) {
        auto& param = fnDecl->params[pi];
        if (param.name.size() > 3 && param.name.substr(0, 3) == "...") {
            variadicParamName = param.name.substr(3);
            variadicParamIdx = pi;
            variadicParamFound = true;
            break;
        }
    }
    // Template params split into [prefix non-variadic][variadic pack].
    // `argTypes` that the caller built via the turbofish sugar-form path
    // matches the FULL call-arg arity (N+prefixCount), where entries
    // 0..prefixCount-1 describe prefix params and prefixCount.. describe
    // the pack elements. Sum_all-style templates (pack only, no prefix)
    // hit this with variadicParamIdx=0 and prefixCount=0, keeping the
    // original behaviour (all of argTypes treated as pack).
    size_t prefixCount = variadicParamFound ? variadicParamIdx : 0;
    // Guard against malformed calls where argTypes is shorter than the
    // non-variadic prefix (shouldn't happen given Sema wires argTypes to
    // match call-arg count, but stay defensive to avoid UB).
    if (prefixCount > argTypes.size()) prefixCount = argTypes.size();

    // Set up the type-pack substitution context for the entire
    // instance materialisation (params, return type, body). Anything
    // in the cloned subtree that is `Ts[i]` (PackIndexType) gets
    // rewritten to `argTypes[i]` via cloneTypeWithPackSubst.
    //
    // The type-pack name is the variadic generic parameter — when the
    // user writes `fn foo<...Ts>(...args: Ts) -> Ts[0]`, `Ts` lands
    // as the sole entry of `genericParams` (the `...` only flips
    // `isVariadicGeneric`). The value-pack name is the `...args`
    // parameter we extracted above.
    std::string typePackName;
    if (!fnDecl->genericParams.empty()) {
        typePackName = fnDecl->genericParams.front();
    }

    // Split argTypes into prefix (non-variadic template params) and pack
    // (remaining entries that feed the variadic). The pack substitution
    // machinery should only see the pack portion — the prefix args map
    // to regular params with unchanged names/types.
    std::vector<VyxTypePtr> packTypes(
        argTypes.begin() + static_cast<std::ptrdiff_t>(prefixCount),
        argTypes.end());

    PackTypeSubstCtx prevCtx = g_packTypeSubstCtx;
    g_packTypeSubstCtx.typePackName  = typePackName.empty() ? nullptr : &typePackName;
    g_packTypeSubstCtx.valuePackName = &variadicParamName;
    g_packTypeSubstCtx.packTypes     = &packTypes;
    g_packTypeSubstCtx.sema          = this;

    // Sugar-form binding: `<T>(args: ...T) -> T` — T is BOTH the pack
    // element type and a free type variable in non-pack positions (return
    // type, body). Bind T → packTypes[0] so NamedType("T") substitutes.
    // Only fires when all pack entries share a kind+name (homogeneous pack,
    // which is what the sugar form guarantees); heterogeneous packs skip
    // this because there's no single concrete type to substitute.
    bool homogeneous = !packTypes.empty();
    for (size_t i = 1; i < packTypes.size() && homogeneous; ++i) {
        if (!packTypes[i] || !packTypes[0] ||
            !packTypes[i]->isEqual(*packTypes[0])) homogeneous = false;
    }
    if (homogeneous && !typePackName.empty()) {
        g_packTypeSubstCtx.elementTypeName    = &typePackName;
        g_packTypeSubstCtx.elementConcreteType = packTypes[0];
    }

    // Copy prefix non-variadic params verbatim (e.g. `label: string` in
    // `fn print_many<T>(label: string, args: ...T)`). Their types go
    // through cloneTypeWithPackSubst so that `Ts[i]` or `T` uses in
    // prefix param types still substitute correctly.
    for (size_t pi = 0; pi < prefixCount && pi < fnDecl->params.size(); ++pi) {
        const auto& src = fnDecl->params[pi];
        ParamDecl newParam;
        newParam.name = src.name;
        if (src.type) {
            newParam.type = cloneTypeWithPackSubst(*src.type, diag_);
        }
        newParam.isMutRef = src.isMutRef;
        instance->params.push_back(std::move(newParam));
    }

    for (size_t i = 0; i < packTypes.size(); ++i) {
        ParamDecl newParam;
        newParam.name = variadicParamName + "_" + std::to_string(i);
        newParam.type = convertTypeToAnnotation(packTypes[i]);
        instance->params.push_back(std::move(newParam));
    }

    if (fnDecl->returnType) {
        // P2 C6: return-type can be `Ts[i]` — must go through the pack
        // substituting cloner, not the structural cloneTypeAnnotation.
        instance->returnType = cloneTypeWithPackSubst(*fnDecl->returnType, diag_);
    }

    if (fnDecl->body) {
        instance->body = expandVariadicBody(*fnDecl->body, variadicParamName, packTypes);
    }

    g_packTypeSubstCtx = prevCtx;
    return instance;
}

// Recursive scan: does any expression under `expr` reference the pack with a
// runtime (non-IntLiteral) index? Used by expandVariadicBody to decide whether
// to inject the on-demand `__pack_<name>` stack-array materialization.
static bool hasRuntimePackIndexExpr(const Expr& expr, const std::string& packName);
static bool hasRuntimePackIndexStmt(const Stmt& stmt, const std::string& packName);

static bool hasRuntimePackIndexExpr(const Expr& expr, const std::string& packName) {
    if (expr.kind == ExprKind::Index) {
        auto& idx = static_cast<const IndexExpr&>(expr);
        if (idx.object && idx.object->kind == ExprKind::Identifier &&
            idx.object->as<IdentifierExpr>()->name == packName &&
            idx.indexExpr && idx.indexExpr->kind != ExprKind::IntLiteral) {
            return true;
        }
        if (idx.object && hasRuntimePackIndexExpr(*idx.object, packName)) return true;
        if (idx.indexExpr && hasRuntimePackIndexExpr(*idx.indexExpr, packName)) return true;
        return false;
    }
    // Uniform recursion over every expression kind via a small visitor.
    switch (expr.kind) {
        case ExprKind::BinaryOp: {
            auto& e = static_cast<const BinaryOpExpr&>(expr);
            return (e.lhs && hasRuntimePackIndexExpr(*e.lhs, packName)) ||
                   (e.rhs && hasRuntimePackIndexExpr(*e.rhs, packName));
        }
        case ExprKind::UnaryOp: {
            auto& e = static_cast<const UnaryOpExpr&>(expr);
            return e.operand && hasRuntimePackIndexExpr(*e.operand, packName);
        }
        case ExprKind::Call: {
            auto& e = static_cast<const CallExpr&>(expr);
            if (e.callee && hasRuntimePackIndexExpr(*e.callee, packName)) return true;
            for (auto& a : e.args) if (a && hasRuntimePackIndexExpr(*a, packName)) return true;
            return false;
        }
        case ExprKind::MemberAccess: {
            auto& e = static_cast<const MemberAccessExpr&>(expr);
            return e.object && hasRuntimePackIndexExpr(*e.object, packName);
        }
        case ExprKind::Assignment: {
            auto& e = static_cast<const AssignmentExpr&>(expr);
            return (e.lhs && hasRuntimePackIndexExpr(*e.lhs, packName)) ||
                   (e.rhs && hasRuntimePackIndexExpr(*e.rhs, packName));
        }
        case ExprKind::StringInterpolation: {
            // String interpolation hides nested pack index expressions inside
            // `${...}` holes. The body rewrite descends into these (see
            // cloneExprWithPackSubst's StringInterpolation case), so the
            // runtime-index detector must mirror that recursion; otherwise
            // `__pack_<name>` never gets materialised and the rewritten
            // `__pack_args[<runtime>]` resolves to an undefined symbol.
            auto& e = static_cast<const StringInterpExpr&>(expr);
            for (auto& part : e.parts) {
                if (part.expr && hasRuntimePackIndexExpr(*part.expr, packName))
                    return true;
            }
            return false;
        }
        case ExprKind::TryExpr: {
            auto& e = static_cast<const TryExpr&>(expr);
            return e.inner && hasRuntimePackIndexExpr(*e.inner, packName);
        }
        case ExprKind::AwaitExpr: {
            auto& e = static_cast<const AwaitExpr&>(expr);
            return e.inner && hasRuntimePackIndexExpr(*e.inner, packName);
        }
        default:
            return false;
    }
}

static bool hasRuntimePackIndexStmt(const Stmt& stmt, const std::string& packName) {
    switch (stmt.kind) {
        case StmtKind::Block: {
            auto& s = static_cast<const BlockStmt&>(stmt);
            for (auto& sub : s.statements)
                if (sub && hasRuntimePackIndexStmt(*sub, packName)) return true;
            return false;
        }
        case StmtKind::ExprStmt: {
            auto& s = static_cast<const ExprStmt&>(stmt);
            return s.expr && hasRuntimePackIndexExpr(*s.expr, packName);
        }
        case StmtKind::VarDecl: {
            auto& s = static_cast<const VarDeclStmt&>(stmt);
            return s.initExpr && hasRuntimePackIndexExpr(*s.initExpr, packName);
        }
        case StmtKind::Assignment: {
            auto& s = static_cast<const AssignStmt&>(stmt);
            if (s.target && hasRuntimePackIndexExpr(*s.target, packName)) return true;
            if (s.value && hasRuntimePackIndexExpr(*s.value, packName)) return true;
            return false;
        }
        case StmtKind::Return: {
            auto& s = static_cast<const ReturnStmt&>(stmt);
            return s.expr && hasRuntimePackIndexExpr(*s.expr, packName);
        }
        case StmtKind::If: {
            auto& s = static_cast<const IfStmt&>(stmt);
            if (s.condition && hasRuntimePackIndexExpr(*s.condition, packName)) return true;
            if (s.thenBranch && hasRuntimePackIndexStmt(*s.thenBranch, packName)) return true;
            for (auto& [cond, body] : s.elifBranches) {
                if (cond && hasRuntimePackIndexExpr(*cond, packName)) return true;
                if (body && hasRuntimePackIndexStmt(*body, packName)) return true;
            }
            if (s.elseBranch && hasRuntimePackIndexStmt(*s.elseBranch, packName)) return true;
            return false;
        }
        case StmtKind::While: {
            auto& s = static_cast<const WhileStmt&>(stmt);
            if (s.condition && hasRuntimePackIndexExpr(*s.condition, packName)) return true;
            if (s.body && hasRuntimePackIndexStmt(*s.body, packName)) return true;
            return false;
        }
        case StmtKind::For: {
            auto& s = static_cast<const ForStmt&>(stmt);
            if (s.init && hasRuntimePackIndexStmt(*s.init, packName)) return true;
            if (s.condition && hasRuntimePackIndexExpr(*s.condition, packName)) return true;
            if (s.step && hasRuntimePackIndexExpr(*s.step, packName)) return true;
            if (s.body && hasRuntimePackIndexStmt(*s.body, packName)) return true;
            return false;
        }
        case StmtKind::ForEach: {
            auto& s = static_cast<const ForEachStmt&>(stmt);
            if (s.collection && hasRuntimePackIndexExpr(*s.collection, packName)) return true;
            if (s.body && hasRuntimePackIndexStmt(*s.body, packName)) return true;
            return false;
        }
        default:
            return false;
    }
}

StmtPtr Sema::expandVariadicBody(const Stmt& body, const std::string& variadicParamName, const std::vector<VyxTypePtr>& argTypes) {
    // We must handle the top-level block specially so that
    // `for (var d in args)` expansion can splice multiple statements
    // in place of a single ForEach node.
    if (body.kind == StmtKind::Block) {
        auto* srcBlock = body.as<BlockStmt>();
        auto block = std::make_unique<BlockStmt>();
        block->location = body.location;

        // On-demand pack materialization: if anywhere in the body the user
        // writes `args[<runtime>]` (index is not a constexpr IntLiteral), we
        // inject a prologue `let __pack_args = [args_0, args_1, ..., args_N-1];`
        // so the rewritten `__pack_args[<runtime>]` resolves to a real local.
        // Pure constexpr packs (args[0], args[K], args...+ folds,
        // `for var d in args` unrolls) bypass this — no runtime array cost.
        if (hasRuntimePackIndexStmt(body, variadicParamName)) {
            auto arrInit = std::make_unique<ArrayInitExpr>();
            arrInit->location = body.location;
            for (size_t i = 0; i < argTypes.size(); ++i) {
                arrInit->elements.push_back(ast::makeIdentifier(body.location,
                    variadicParamName + "_" + std::to_string(i)));
                arrInit->elementIsSpread.push_back(false);
            }
            block->statements.push_back(ast::makeVarDecl(body.location,
                /*isConst=*/true,
                "__pack_" + variadicParamName,
                /*type=*/nullptr,
                std::move(arrInit)));
        }

        for (auto& s : srcBlock->statements) {
            if (!s) continue;

            // Expansion statement: `for (var d in <packName>) { ... }`
            // Each iteration gets its own scope block with a `let d = args_i;`
            // binding, followed by the (pack-substituted) iteration body.
            //
            // Early-exit semantics (return/break/continue):
            //   - `return` inside the expansion exits the enclosing function.
            //   - `break` / `continue` are NOT intercepted; they refer to any
            //     enclosing loop that contains the expansion statement, NOT to
            //     the pack iteration itself (which is unrolled, not a runtime
            //     loop).  If the user wants to skip an iteration they should
            //     use an `if` guard inside the body.
            if (s->kind == StmtKind::ForEach) {
                auto* fe = s->as<ForEachStmt>();
                if (fe->collection &&
                    fe->collection->kind == ExprKind::Identifier &&
                    fe->collection->as<IdentifierExpr>()->name == variadicParamName) {

                    for (size_t i = 0; i < argTypes.size(); ++i) {
                        auto unrolled = std::make_unique<BlockStmt>();
                        unrolled->location = s->location;

                        // let d = args_i;
                        auto initExpr = ast::makeIdentifier(s->location,
                            variadicParamName + "_" + std::to_string(i));
                        auto varDecl = ast::makeVarDecl(s->location, true,
                            fe->varName, nullptr, std::move(initExpr));
                        unrolled->statements.push_back(std::move(varDecl));

                        if (fe->body) {
                            // Apply pack substitution to the body of each iteration
                            unrolled->statements.push_back(
                                cloneStmtWithPackSubst(*fe->body, variadicParamName,
                                                       argTypes.size(), diag_));
                        }

                        block->statements.push_back(std::move(unrolled));
                    }
                    continue;
                }

                // `for i in <lo>..<hi>` where both endpoints resolve to
                // constexpr — unroll the loop and substitute the iterator
                // with each concrete K. This makes `args[i]` (post-sub) see
                // a constexpr index and route through the pack-K → args_K
                // path, so heterogeneous packs get proper per-element
                // concrete types (no need for a homogeneous stack array).
                if (fe->collection && fe->collection->kind == ExprKind::BinaryOp &&
                    fe->collection->as<BinaryOpExpr>()->op == BinaryOp::RangeOp) {
                    auto* bin = fe->collection->as<BinaryOpExpr>();
                    auto tryEvalConstexpr = [&](const Expr* e, int64_t& out) -> bool {
                        if (!e) return false;
                        if (e->kind == ExprKind::IntLiteral) {
                            out = e->as<IntLiteralExpr>()->value;
                            return true;
                        }
                        // `args.len`
                        if (e->kind == ExprKind::MemberAccess) {
                            auto* ma = e->as<MemberAccessExpr>();
                            if (ma->member == "len" && ma->object &&
                                ma->object->kind == ExprKind::Identifier &&
                                ma->object->as<IdentifierExpr>()->name == variadicParamName) {
                                out = static_cast<int64_t>(argTypes.size());
                                return true;
                            }
                        }
                        // `sizeof...(args)`
                        if (e->kind == ExprKind::SizeofPack) {
                            auto* sp = e->as<SizeofPackExpr>();
                            if (sp->packName == variadicParamName) {
                                out = static_cast<int64_t>(argTypes.size());
                                return true;
                            }
                        }
                        return false;
                    };

                    int64_t lo = 0, hi = 0;
                    bool ok = bin->lhs && bin->rhs &&
                              tryEvalConstexpr(bin->lhs.get(), lo) &&
                              tryEvalConstexpr(bin->rhs.get(), hi);
                    if (ok && lo >= 0 && hi >= lo && fe->body) {
                        for (int64_t K = lo; K < hi; ++K) {
                            // Fresh structural copy of body (empty pack name
                            // so cloneStmtWithPackSubst doesn't pack-subst yet).
                            auto iterCopy = cloneStmtWithPackSubst(*fe->body,
                                /*packName=*/"", /*packLen=*/0, diag_);
                            // i → IntLiteral(K) — after this, args[i] becomes
                            // args[IntLiteral(K)] which the pack-subst pass
                            // below rewrites to args_K.
                            std::map<std::string, int64_t> constBind;
                            constBind[fe->varName] = K;
                            substConstIdentsInStmt(iterCopy, constBind);
                            auto unrolled = std::make_unique<BlockStmt>();
                            unrolled->location = s->location;
                            unrolled->statements.push_back(
                                cloneStmtWithPackSubst(*iterCopy, variadicParamName,
                                                       argTypes.size(), diag_));
                            block->statements.push_back(std::move(unrolled));
                        }
                        continue;
                    }
                }
            }

            // For all other statements, clone with pack substitution applied
            block->statements.push_back(
                cloneStmtWithPackSubst(*s, variadicParamName, argTypes.size(), diag_));
        }

        return block;
    }

    // Non-block top-level statement (rare, but handle gracefully)
    return cloneStmtWithPackSubst(body, variadicParamName, argTypes.size(), diag_);
}

// ============================================================
//  P2b Partial template specialization — pattern matching
// ============================================================
//
// Pattern-match algorithm (pseudocode):
//
//   matchPattern(pattern[], concreteArgs[], freshParams) -> bindings | fail
//     bindings = {}
//     for i in 0..len(pattern):
//       p = pattern[i], a = concreteArgs[i]
//       if p is NamedType and p.name in freshParams:
//         if p.name in bindings and bindings[p.name] != a: return fail   // inconsistent
//         bindings[p.name] = a
//       elif p is Generic(name, subPattern[]):
//         if a is not Generic(name, subArgs[]) with same arity: return fail
//         recursively match subPattern[] against subArgs[] using same bindings
//       else:  // p is a concrete type (NamedType builtin, etc.)
//         if typeAnnotationToMangle(p) != a.toString(): return fail
//     return bindings (success)
//
//   specificityScore(pattern[], freshParamNames) -> int
//     count "concrete tokens" = number of nodes in pattern tree that are NOT
//     a fresh param identifier.  Higher = more specific.
//
//   selectPartialSpec(baseName, concreteArgs, useSite):
//     candidates = all decls with isPartialSpecialization &&
//                  partialSpecBaseName == baseName &&
//                  |specializationPattern| == |concreteArgs|
//     matches = [(decl, bindings, score) for (decl, score) where matchPattern succeeds]
//     if no matches: return nullptr (use primary)
//     best = max(matches, key=score)
//     if multiple with same score: emit ambiguity error, return nullptr
//     return best

// Forward-declared helper: count concrete tokens in a pattern slot
// (i.e., not a fresh param identifier).
//
// Scoring rules (P2b extended):
//   Named(fresh)       → 0   (free parameter, contributes nothing)
//   Named(concrete)    → 1   (concrete leaf)
//   Generic            → 1 + sum(args)   (constructor + recursive args)
//   Pointer            → 1 + inner
//   Reference          → 1 + inner
//   Array              → 1 + inner + (1 if size is a concrete literal, 0 if free const param)
//   Tuple              → 1 + sum(elements)
//   Function           → 1 + sum(params) + return
//   Union              → 1 + sum(members)
static int partialSpecScore(const TypeAnnotation* pattern,
                            const std::set<std::string>& freshParams) {
    if (!pattern) return 0;
    if (pattern->kind == TypeAnnotationKind::Named) {
        // A fresh param placeholder counts 0; a concrete Named type counts 1.
        if (freshParams.count(pattern->name)) return 0;
        return 1;
    }
    if (pattern->kind == TypeAnnotationKind::Generic) {
        auto* gt = pattern->as<GenericType>();
        int score = 1; // the constructor itself is concrete
        for (auto& arg : gt->typeArgs)
            score += partialSpecScore(arg.get(), freshParams);
        return score;
    }
    if (pattern->kind == TypeAnnotationKind::Pointer) {
        auto* pt = pattern->as<PointerType>();
        return 1 + partialSpecScore(pt->innerType.get(), freshParams);
    }
    if (pattern->kind == TypeAnnotationKind::Reference) {
        auto* rt = pattern->as<ReferenceType>();
        return 1 + partialSpecScore(rt->innerType.get(), freshParams);
    }
    if (pattern->kind == TypeAnnotationKind::Array) {
        auto* at = pattern->as<ArrayType>();
        // Size: +1 if it is a concrete integer literal, +0 if it is a free
        // const-param identifier (named in freshParams).
        int sizeScore = 0;
        if (at->size) {
            if (at->size->kind == ExprKind::IntLiteral) {
                sizeScore = 1; // concrete literal → more specific
            } else if (at->size->kind == ExprKind::Identifier) {
                const auto& nm = at->size->as<IdentifierExpr>()->name;
                sizeScore = freshParams.count(nm) ? 0 : 1;
            }
        }
        return 1 + partialSpecScore(at->elementType.get(), freshParams) + sizeScore;
    }
    if (pattern->kind == TypeAnnotationKind::Tuple) {
        auto* tt = pattern->as<TupleType>();
        int s = 1;
        for (auto& e : tt->elements) s += partialSpecScore(e.get(), freshParams);
        return s;
    }
    if (pattern->kind == TypeAnnotationKind::Function) {
        auto* ft = pattern->as<FunctionType>();
        int s = 1;
        for (auto& p : ft->paramTypes) s += partialSpecScore(p.get(), freshParams);
        s += partialSpecScore(ft->returnType.get(), freshParams);
        return s;
    }
    if (pattern->kind == TypeAnnotationKind::Union) {
        auto* ut = pattern->as<UnionType>();
        int s = 1;
        for (auto& m : ut->members) s += partialSpecScore(m.get(), freshParams);
        return s;
    }
    return 1;
}

// Helper: build a synthetic VyxTypePtr from a mangled sub-string that
// was extracted from a concrete Generic type's argument list.  Used inside
// the Generic-pattern branch of matchPatternSlot when a sub-pattern is
// itself a structured type (Pointer, Reference, Array, Tuple, Function)
// and we need a VyxTypePtr to recurse into.
//
// The function only needs to produce enough structural fidelity for the
// recursive matchPatternSlot calls — specifically:
//   • Pointer  "*X"
//   • Reference  "&X" / "&mut X"
//   • Array  "[X;N]"
//   • Tuple  "(X,Y,...)"
//   • Function  "fn(X,Y)->R"
//   • Everything else → opaque Generic with the full string as name
// (That last case handles nested concrete generics like "Vec<i32>" correctly
// because the Named-pattern branch checks concrete->name == pattern->name.)
static VyxTypePtr synthVyxTypeFromMangle(const std::string& s);

// Split a comma-separated top-level argument list (no outer brackets).
static std::vector<std::string> splitTopLevel(const std::string& s, char sep = ',') {
    std::vector<std::string> parts;
    int depth = 0;
    std::string cur;
    for (char c : s) {
        if (c == '<' || c == '(' || c == '[') { ++depth; cur += c; }
        else if (c == '>' || c == ')' || c == ']') { --depth; cur += c; }
        else if (c == sep && depth == 0) { parts.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty() || !parts.empty()) parts.push_back(cur);
    return parts;
}

static VyxTypePtr synthVyxTypeFromMangle(const std::string& s) {
    if (s.empty()) {
        auto t = std::make_shared<VyxType>(); t->kind = VyxTypeKind::Unknown; return t;
    }
    // Pointer: "*X"
    if (s[0] == '*') {
        auto inner = synthVyxTypeFromMangle(s.substr(1));
        auto t = std::make_shared<VyxType>();
        t->kind = VyxTypeKind::Pointer;
        t->name = s;
        t->pointeeType = inner;
        return t;
    }
    // Reference: "&mut X" or "&X"
    if (s[0] == '&') {
        bool isMut = (s.size() >= 5 && s.substr(0, 5) == "&mut ");
        std::string inner_s = isMut ? s.substr(5) : s.substr(1);
        auto inner = synthVyxTypeFromMangle(inner_s);
        auto t = std::make_shared<VyxType>();
        t->kind = VyxTypeKind::Reference;
        t->name = s;
        t->isMutable = isMut;
        t->pointeeType = inner;
        return t;
    }
    // Array: "[X;N]"
    if (s[0] == '[' && s.back() == ']') {
        std::string body = s.substr(1, s.size() - 2);
        // Find the semicolon at top level
        auto semi_pos = std::string::npos;
        {
            int d = 0;
            for (size_t i = 0; i < body.size(); ++i) {
                char c = body[i];
                if (c == '<' || c == '(' || c == '[') ++d;
                else if (c == '>' || c == ')' || c == ']') --d;
                else if (c == ';' && d == 0) { semi_pos = i; break; }
            }
        }
        auto t = std::make_shared<VyxType>();
        t->kind = VyxTypeKind::Array;
        t->name = s;
        if (semi_pos != std::string::npos) {
            t->elementType = synthVyxTypeFromMangle(body.substr(0, semi_pos));
            // Parse the size integer
            std::string sz = body.substr(semi_pos + 1);
            char* end = nullptr;
            long v = std::strtol(sz.c_str(), &end, 10);
            if (end != sz.c_str()) t->arraySize = static_cast<int>(v);
        } else {
            t->elementType = synthVyxTypeFromMangle(body);
        }
        return t;
    }
    // Tuple: "(X,Y,...)"
    if (s[0] == '(' && s.back() == ')') {
        std::string body = s.substr(1, s.size() - 2);
        auto t = std::make_shared<VyxType>();
        t->kind = VyxTypeKind::Tuple;
        t->name = s;
        if (!body.empty()) {
            for (auto& elem : splitTopLevel(body))
                t->tupleTypes.push_back(synthVyxTypeFromMangle(elem));
        }
        return t;
    }
    // Function: "fn(X,Y)->R"
    if (s.size() >= 3 && s.substr(0, 3) == "fn(") {
        auto rp = s.find(")->");
        auto t = std::make_shared<VyxType>();
        t->kind = VyxTypeKind::Function;
        t->name = "fn";
        if (rp != std::string::npos) {
            std::string params_s = s.substr(3, rp - 3);
            std::string ret_s = s.substr(rp + 3);
            if (!params_s.empty())
                for (auto& p : splitTopLevel(params_s))
                    t->paramTypes.push_back(synthVyxTypeFromMangle(p));
            t->returnType = synthVyxTypeFromMangle(ret_s);
        }
        return t;
    }
    // Fallback: opaque generic (covers "Vec<i32>", "i32", etc.)
    auto t = std::make_shared<VyxType>();
    t->kind = VyxTypeKind::Generic;
    t->name = s;
    return t;
}

// Recursive pattern-match.  Returns false if no match; on success fills
// `bindings` with fresh-param → VyxType mappings.
//
// Supported pattern shapes (P2b extended):
//   Named(fresh)       — bind or consistency-check
//   Named(concrete)    — exact name match
//   Generic            — same base name, same arity, pairwise-recursive args
//                        (deep nesting now recurses fully)
//   Pointer            — concrete must be Pointer; inner types unify
//   Reference          — concrete must be Reference; mutability must match; inner unify
//   Array              — concrete must be Array; element types unify;
//                        size: if pattern size is a free const-param → bind;
//                              if concrete integer literal → equal-check
//   Tuple              — concrete must be Tuple; arity equal; pairwise unify
//   Function           — concrete must be Function; param arity equal;
//                        pairwise param unify + return unify
static bool matchPatternSlot(
    const TypeAnnotation* pattern,
    const VyxTypePtr& concrete,
    const std::set<std::string>& freshParams,
    std::map<std::string, VyxTypePtr>& bindings)
{
    if (!pattern || !concrete) return false;

    // ── Named ────────────────────────────────────────────────────────────────
    if (pattern->kind == TypeAnnotationKind::Named) {
        if (freshParams.count(pattern->name)) {
            // Fresh generic param: bind or check consistency
            auto it = bindings.find(pattern->name);
            if (it == bindings.end()) {
                bindings[pattern->name] = concrete;
                return true;
            }
            // Consistency check: the previously bound type must equal this one
            return it->second->toString() == concrete->toString();
        }
        // Concrete named type: must match exactly
        return (concrete->toString() == pattern->name ||
                concrete->name == pattern->name);
    }

    // ── Generic ──────────────────────────────────────────────────────────────
    if (pattern->kind == TypeAnnotationKind::Generic) {
        auto* gt = pattern->as<GenericType>();
        // Concrete side must also be a generic/class/struct with the same base name
        // and same arity.
        if (concrete->kind != VyxTypeKind::Class &&
            concrete->kind != VyxTypeKind::Struct &&
            concrete->kind != VyxTypeKind::Generic &&
            !isVecLike(*concrete)) {
            return false;
        }
        // Extract the base name from the concrete mangled name
        auto lt = concrete->name.find('<');
        std::string concreteBase = (lt != std::string::npos)
            ? concrete->name.substr(0, lt) : concrete->name;
        if (concreteBase != gt->name) return false;

        // Concrete has no type args; pattern does → no match unless pattern arity 0
        if (lt == std::string::npos) {
            return gt->typeArgs.empty();
        }
        // Extract the inner string between < and last >
        std::string inner = concrete->name.substr(lt + 1, concrete->name.size() - lt - 2);
        // Split by top-level commas
        std::vector<std::string> concreteArgStrings = splitTopLevel(inner);
        if (concreteArgStrings.size() != gt->typeArgs.size()) return false;

        // Match each sub-pattern recursively.  Build a synthetic VyxTypePtr for
        // each concrete sub-string so the recursive call can inspect structure
        // (handles deep nesting like `Pair<Vec<*i32>, U>`, `Triple<fn(T)->i32, U, V>`).
        for (size_t i = 0; i < gt->typeArgs.size(); ++i) {
            const TypeAnnotation* subPat = gt->typeArgs[i].get();
            if (!subPat) return false;
            VyxTypePtr synthConcrete = synthVyxTypeFromMangle(concreteArgStrings[i]);
            if (!matchPatternSlot(subPat, synthConcrete, freshParams, bindings))
                return false;
        }
        return true;
    }

    // ── Pointer ──────────────────────────────────────────────────────────────
    if (pattern->kind == TypeAnnotationKind::Pointer) {
        if (concrete->kind != VyxTypeKind::Pointer) return false;
        auto* pt = pattern->as<PointerType>();
        if (!pt->innerType || !concrete->pointeeType) return false;
        return matchPatternSlot(pt->innerType.get(), concrete->pointeeType,
                                freshParams, bindings);
    }

    // ── Reference ────────────────────────────────────────────────────────────
    if (pattern->kind == TypeAnnotationKind::Reference) {
        if (concrete->kind != VyxTypeKind::Reference) return false;
        auto* rt = pattern->as<ReferenceType>();
        // Mutability must match exactly (immutable pattern won't match mut ref
        // and vice-versa).
        if (rt->isMutable != concrete->isMutable) return false;
        if (!rt->innerType || !concrete->pointeeType) return false;
        return matchPatternSlot(rt->innerType.get(), concrete->pointeeType,
                                freshParams, bindings);
    }

    // ── Array ────────────────────────────────────────────────────────────────
    if (pattern->kind == TypeAnnotationKind::Array) {
        if (concrete->kind != VyxTypeKind::Array) return false;
        auto* at = pattern->as<ArrayType>();
        if (!at->elementType || !concrete->elementType) return false;
        // Element type: recurse.
        if (!matchPatternSlot(at->elementType.get(), concrete->elementType,
                              freshParams, bindings))
            return false;
        // Size: if pattern size is a free const-param identifier → bind it.
        //        if pattern size is a concrete integer literal → must equal.
        //        if pattern has no size → skip check (matches any size).
        if (at->size) {
            if (at->size->kind == ExprKind::Identifier) {
                const auto& nm = at->size->as<IdentifierExpr>()->name;
                if (freshParams.count(nm)) {
                    // Bind the const param to the concrete array size.
                    // Represent the bound size as a VyxType with kind Integer
                    // whose name is the decimal string (Mono consults constBindings
                    // separately; here we just record it as an opaque Generic so
                    // allBound check succeeds).
                    auto it = bindings.find(nm);
                    auto sizeType = std::make_shared<VyxType>();
                    sizeType->kind = VyxTypeKind::Generic;
                    sizeType->name = std::to_string(concrete->arraySize);
                    if (it == bindings.end()) {
                        bindings[nm] = sizeType;
                    } else {
                        if (it->second->name != sizeType->name) return false;
                    }
                } else {
                    // Non-fresh identifier: compare name against concrete size
                    // (unusual case; fall through to literal branch treatment).
                    const std::string& ns = at->size->as<IdentifierExpr>()->name;
                    char* end = nullptr;
                    long long patSize = std::strtoll(ns.c_str(), &end, 10);
                    if (end == ns.c_str()) return false;  // unparseable → mismatch
                    if (patSize != static_cast<int64_t>(concrete->arraySize)) return false;
                }
            } else if (at->size->kind == ExprKind::IntLiteral) {
                int64_t patSize = at->size->as<IntLiteralExpr>()->value;
                if (patSize != static_cast<int64_t>(concrete->arraySize)) return false;
            }
            // Other expr kinds: skip check (shouldn't arise in template patterns).
        }
        return true;
    }

    // ── Tuple ────────────────────────────────────────────────────────────────
    if (pattern->kind == TypeAnnotationKind::Tuple) {
        if (concrete->kind != VyxTypeKind::Tuple) return false;
        auto* tt = pattern->as<TupleType>();
        if (tt->elements.size() != concrete->tupleTypes.size()) return false;
        for (size_t i = 0; i < tt->elements.size(); ++i) {
            if (!tt->elements[i] || !concrete->tupleTypes[i]) return false;
            if (!matchPatternSlot(tt->elements[i].get(), concrete->tupleTypes[i],
                                  freshParams, bindings))
                return false;
        }
        return true;
    }

    // ── Function ─────────────────────────────────────────────────────────────
    if (pattern->kind == TypeAnnotationKind::Function) {
        if (concrete->kind != VyxTypeKind::Function) return false;
        auto* ft = pattern->as<FunctionType>();
        // Param arity must match; differing arities are punted (no match).
        if (ft->paramTypes.size() != concrete->paramTypes.size()) return false;
        for (size_t i = 0; i < ft->paramTypes.size(); ++i) {
            if (!ft->paramTypes[i] || !concrete->paramTypes[i]) return false;
            if (!matchPatternSlot(ft->paramTypes[i].get(), concrete->paramTypes[i],
                                  freshParams, bindings))
                return false;
        }
        // Return type unification.
        if (ft->returnType) {
            if (!concrete->returnType) return false;
            if (!matchPatternSlot(ft->returnType.get(), concrete->returnType,
                                  freshParams, bindings))
                return false;
        }
        // If pattern has no return type annotation, any concrete return type matches.
        return true;
    }

    // Any other pattern kind is not yet handled → no match.
    return false;
}

Sema::PartialSpecMatch Sema::selectPartialSpec(
    const std::string& baseName,
    const std::vector<VyxTypePtr>& concreteArgs,
    SourceLocation useSite)
{
    if (!unit_) return {};

    struct Candidate {
        const Decl* decl;
        std::map<std::string, VyxTypePtr> bindings;
        int score;
    };
    std::vector<Candidate> matches;

    for (auto& d : unit_->declarations) {
        if (!d || !d->isPartialSpecialization) continue;
        if (d->partialSpecBaseName != baseName) continue;
        if (d->specializationPattern.size() != concreteArgs.size()) continue;

        // Build set of fresh params for this spec
        std::set<std::string> freshSet(
            d->genericParams.begin(), d->genericParams.end());

        // Attempt to match each pattern slot against the concrete arg
        std::map<std::string, VyxTypePtr> bindings;
        bool ok = true;
        for (size_t i = 0; i < concreteArgs.size(); ++i) {
            if (!matchPatternSlot(d->specializationPattern[i].get(),
                                   concreteArgs[i], freshSet, bindings)) {
                ok = false;
                break;
            }
        }
        if (!ok) continue;

        // Verify all fresh params got bound (pattern may have stale params)
        bool allBound = true;
        for (auto& fp : d->genericParams) {
            if (!bindings.count(fp)) { allBound = false; break; }
        }
        if (!allBound) continue;

        // Compute specificity score
        int score = 0;
        for (auto& patSlot : d->specializationPattern)
            score += partialSpecScore(patSlot.get(), freshSet);

        matches.push_back({d.get(), std::move(bindings), score});
    }

    if (matches.empty()) return {};

    // Find max score
    int bestScore = matches[0].score;
    for (auto& m : matches)
        if (m.score > bestScore) bestScore = m.score;

    // Collect all at best score
    std::vector<size_t> bestIdx;
    for (size_t i = 0; i < matches.size(); ++i)
        if (matches[i].score == bestScore) bestIdx.push_back(i);

    if (bestIdx.size() == 1) {
        // Unique winner
        auto& winner = matches[bestIdx[0]];
        return {winner.decl, std::move(winner.bindings)};
    }

    // Ambiguity: emit P2E-001 error
    std::string candidateList;
    for (size_t idx : bestIdx) {
        if (!candidateList.empty()) candidateList += ", ";
        const Decl* d = matches[idx].decl;
        candidateList += d->partialSpecBaseName + "<";
        for (size_t i = 0; i < d->specializationPattern.size(); ++i) {
            if (i > 0) candidateList += ",";
            candidateList += d->specializationPattern[i]
                ? d->specializationPattern[i]->name : "?";
        }
        candidateList += ">";
    }
    // Build use-site args string
    std::string argsStr;
    for (size_t i = 0; i < concreteArgs.size(); ++i) {
        if (i > 0) argsStr += ",";
        argsStr += concreteArgs[i] ? concreteArgs[i]->toString() : "?";
    }
    diag_.errorCoded("P2E-001", useSite,
        "ambiguous partial template specialization '{}' for '{}::<{}>': "
        "candidates: {}",
        baseName, baseName, argsStr, candidateList);
    diag_.helpCompact(useSite,
        "add a more-specific partial specialization or a full specialization "
        "for '{}::<{}>' to resolve the ambiguity",
        baseName, argsStr);
    return {};
}

} // namespace vyx
