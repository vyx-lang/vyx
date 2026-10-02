/// Monomorphize.cpp — standalone monomorphization pass for Vyx.
///
/// Design decisions (see header for rationale):
///
///  1. TypeEnv is defined in the header (not Type.h) to avoid modifying shared
///     headers. It wraps GenericSubstitution for TemplateResolver interop.
///
///  2. Deep cloning uses recursive helper functions (cloneTypeAnnotation,
///     cloneExpr, cloneStmt) that live entirely in this TU. No Decl::clone()
///     was added to AST.h. The cloners are conservative: any Expr/Stmt variant
///     not explicitly handled produces a null/stub result.
///
///  3. substituteAnnotation converts TypeAnnotation → VyxType (via
///     TemplateResolver::substituteType) then converts the resulting VyxType
///     back to a NamedType / PointerType / etc. AST node. Full round-trip
///     fidelity for DynArray/Dict/etc. produces a NamedType with toString()
///     content as a known TODO.
///
///  4. scanForRequests does a shallow walk: inspects param types, return type,
///     and for FunctionDecl it recursively visits the body looking for
///     CallExpr nodes whose callee IdentifierExpr carries callTypeArgs
///     (turbofish syntax).

#include "Monomorphize.h"
#include "../Common/Diagnostics.h"
#include "../Common/SourceLocation.h"
#include "../Sema/LangItemRegistry.h"
#include <algorithm>
#include <cctype>
#include <functional>
#include <iostream>
#include <set>
#include <map>
#include <vector>

namespace vyx {

// ── Mono-side instantiation chain (Option-2, .cpp-only side-channel) ──
// Thread-local so concurrent Sema+Mono on separate TUs don't race.
// `processOne` pushes a frame before entering the instantiation and pops
// it on exit; errors emitted inside that window see the full chain via
// `emitInstantiationChainNotes(diag_, loc, g_monoInstantiationStack)`.
static thread_local std::vector<InstantiationFrame> g_monoInstantiationStack;

// ── R5 step 3: Mono-side lang-item side-channel ──
// `vyxTypeToAnnotation` is a static free function (not a Mono member) that
// must emit the *registered* stdlib Option/Result slot name rather than
// hard-coded "Option"/"Result" strings — otherwise a user who registered
// `@[lang_item("option")] enum Maybe<T>` would see the reverse path
// produce an AST NamedType("Option") that subsequent Sema resolution
// cannot find. We thread the registry via a thread-local pointer that
// `Monomorphize::run()` sets on entry and clears on exit. Null means
// "no registry available" (tooling paths); in that case Option/Result
// slot resolution emits the `<unknown>` sentinel, which fails loudly.
static thread_local const LangItemRegistry* g_monoLangItems = nullptr;

// RAII guard so `run()` cannot leak a dangling pointer via an exception.
namespace {
struct MonoLangItemsGuard {
    const LangItemRegistry* prev;
    explicit MonoLangItemsGuard(const LangItemRegistry* reg) : prev(g_monoLangItems) {
        g_monoLangItems = reg;
    }
    ~MonoLangItemsGuard() { g_monoLangItems = prev; }
    MonoLangItemsGuard(const MonoLangItemsGuard&) = delete;
    MonoLangItemsGuard& operator=(const MonoLangItemsGuard&) = delete;
};

// Look up a lang-item slot's canonical decl name via the thread-local
// registry pointer. Returns the empty string when the registry is absent
// or the slot is unregistered; callers must treat empty as a hard miss.
std::string langItemSlotName(const char* slot) {
    if (!g_monoLangItems) return {};
    if (const Decl* d = g_monoLangItems->find(slot)) return d->name;
    return {};
}
} // namespace

struct MonoInstantiationGuard {
    MonoInstantiationGuard(InstantiationFrame f) {
        g_monoInstantiationStack.push_back(std::move(f));
    }
    ~MonoInstantiationGuard() {
        if (!g_monoInstantiationStack.empty())
            g_monoInstantiationStack.pop_back();
    }
    MonoInstantiationGuard(const MonoInstantiationGuard&) = delete;
    MonoInstantiationGuard& operator=(const MonoInstantiationGuard&) = delete;
};

namespace {
// Bridge: the canonical TypeEnv (Sema/Type.h) → the older GenericSubstitution
// shape that TemplateResolver::substituteType(const TypeAnnotation&, …) still
// expects. Once that legacy entry point is rewritten to take a TypeEnv
// directly, this can disappear.
GenericSubstitution toGenericSubstitution(const TypeEnv& env) {
    GenericSubstitution subst;
    for (auto& [name, type] : env.bindings) subst.add(name, type);
    return subst;
}

canon::WorkItemKind schedulerKindForRequest(const InstantiationRequest& req) {
    if (req.classTemplate)
        return canon::WorkItemKind::InstantiateMethod;
    if (req.templateDecl &&
        (req.templateDecl->kind == DeclKind::Class ||
         req.templateDecl->kind == DeclKind::Struct)) {
        return canon::WorkItemKind::InstantiateClass;
    }
    return canon::WorkItemKind::InstantiateFreeFn;
}

std::pair<std::string, std::string> splitMethodMangle(const std::string& mangledName) {
    auto dot = mangledName.rfind('.');
    if (dot == std::string::npos)
        return {"", mangledName};
    return {mangledName.substr(0, dot), mangledName.substr(dot + 1)};
}

void recordSchedulerRequest(canon::MonoScheduler* scheduler,
                            const InstantiationRequest& req) {
    if (!scheduler || req.mangledName.empty()) return;
    auto kind = schedulerKindForRequest(req);
    switch (kind) {
        case canon::WorkItemKind::InstantiateClass:
            scheduler->recordMonoClassRequest(req.mangledName);
            break;
        case canon::WorkItemKind::InstantiateFreeFn:
            scheduler->recordMonoFreeFnRequest(req.mangledName);
            break;
        case canon::WorkItemKind::InstantiateMethod: {
            auto [cls, method] = splitMethodMangle(req.mangledName);
            scheduler->recordMonoMethodRequest(cls, method);
            break;
        }
    }
}

void markSchedulerReady(canon::MonoScheduler* scheduler,
                        const InstantiationRequest& req) {
    if (!scheduler || req.mangledName.empty()) return;
    auto kind = schedulerKindForRequest(req);
    if (kind == canon::WorkItemKind::InstantiateMethod) {
        auto [cls, method] = splitMethodMangle(req.mangledName);
        scheduler->markReady(cls + "::" + method, kind);
        return;
    }
    scheduler->markReady(req.mangledName, kind);
}
} // namespace

// ============================================================
//  VyxType → TypeAnnotation conversion helper
// ============================================================

/// Convert a VyxTypePtr back to an AST TypeAnnotation.
///
/// Round-trip fidelity contract:
///   - Container kinds (DynArray/Dict/Result/Option/Ref/Scope/Box/Stack/Queue/
///     Set/UnorderedSet/UnorderedMap/Delegate/Event) produce a structured
///     `GenericType` AST node carrying their type arguments — never a flat
///     `NamedType` whose name is `toString()`. This matches what the parser
///     would have produced for `Vec<T>`, `Dict<K,V>`, etc., so subsequent
///     Sema/CodeGen passes (which dispatch on `TypeAnnotationKind::Generic`)
///     see the same shape they would for hand-written source.
///   - Primitive kinds (Void/Bool/Char/String/Integer/Float/RawPtr)
///     and nominal kinds (Struct/Class/Interface/Generic/ErrorType) emit a
///     `NamedType`.
///   - `fn(P...)->R` emits `FunctionType`, `(T,U,...)` emits `TupleType`,
///     `T|U` emits `UnionType`, `*T` / `&T` / `[T;N]` emit their dedicated
///     nodes.
///   - `Pack` (variadic generic pack) emits a `GenericType` named "pack"
///     whose `typeArgs` are the expanded members. AST has no dedicated
///     PackType node, so this synthesised generic is the canonical
///     representation downstream consumers already understand.
// Forward declaration: evaluator for non-type generic call-site arguments.
// Defined later in the file alongside `requestTemplateWithArgs`. Needed
// here so `substituteAnnotation` can rewrite `[T; N]` size expressions
// that reference a `const N: usize` parameter into the bound integer.
static bool evalConstGenericArg(const Expr* e, const TypeEnv& env, int64_t& out);

static TypePtr vyxTypeToAnnotation(const VyxTypePtr& t) {
    if (!t) {
        auto n = std::make_unique<NamedType>();
        n->name = "<unknown>";
        return n;
    }

    auto makeNamed = [](std::string name) -> TypePtr {
        auto n = std::make_unique<NamedType>();
        n->name = std::move(name);
        return n;
    };

    auto makeGeneric1 = [](const std::string& base, TypePtr arg) -> TypePtr {
        auto g = std::make_unique<GenericType>();
        g->name = base;
        g->typeArgs.push_back(std::move(arg));
        return g;
    };

    auto makeGeneric2 = [](const std::string& base, TypePtr a, TypePtr b) -> TypePtr {
        auto g = std::make_unique<GenericType>();
        g->name = base;
        g->typeArgs.push_back(std::move(a));
        g->typeArgs.push_back(std::move(b));
        return g;
    };

    switch (t->kind) {
        // ── Primitives → NamedType ──
        case VyxTypeKind::Void:    return makeNamed("void");
        case VyxTypeKind::Bool:    return makeNamed("bool");
        case VyxTypeKind::Char:    return makeNamed("char");
        case VyxTypeKind::RawPtr:  return makeNamed("rawptr");
        case VyxTypeKind::Integer: {
            if (t->isSizeType) return makeNamed(t->isSigned ? "isize" : "usize");
            return makeNamed((t->isSigned ? "i" : "u") + std::to_string(t->bitWidth));
        }
        case VyxTypeKind::Float:
            return makeNamed("f" + std::to_string(t->bitWidth));

        // ── Nominal kinds carry their resolved name verbatim ──
        case VyxTypeKind::Struct:
        case VyxTypeKind::Class:
        case VyxTypeKind::Interface:
        case VyxTypeKind::ErrorType:
        case VyxTypeKind::Generic:
            return makeNamed(t->name);
        case VyxTypeKind::Range:
            return makeNamed("Range");

        // ── Indirection: dedicated AST nodes ──
        case VyxTypeKind::Pointer: {
            auto p = std::make_unique<PointerType>();
            p->innerType = vyxTypeToAnnotation(t->pointeeType);
            return p;
        }
        case VyxTypeKind::Reference: {
            auto r = std::make_unique<ReferenceType>();
            r->isMutable = t->isMutable;
            r->innerType = vyxTypeToAnnotation(t->pointeeType);
            return r;
        }
        case VyxTypeKind::Array: {
            auto a = std::make_unique<ArrayType>();
            a->elementType = vyxTypeToAnnotation(t->elementType);
            // Project the static array length back into the AST as an
            // `IntLiteralExpr`, so downstream Sema/CodeGen passes that
            // re-resolve this annotation rebuild the same fixed-width
            // LLVM ArrayType (otherwise the size collapses to 0 and the
            // backend emits a zero-element array).
            if (t->arraySize > 0) {
                auto lit = std::make_unique<IntLiteralExpr>();
                lit->value = t->arraySize;
                lit->location = SourceLocation{};
                a->size = std::move(lit);
            }
            return a;
        }
        case VyxTypeKind::Tuple: {
            auto tup = std::make_unique<TupleType>();
            for (auto& elem : t->tupleTypes)
                tup->elements.push_back(vyxTypeToAnnotation(elem));
            return tup;
        }
        case VyxTypeKind::Function: {
            auto fn = std::make_unique<FunctionType>();
            for (auto& p : t->paramTypes)
                fn->paramTypes.push_back(vyxTypeToAnnotation(p));
            if (t->returnType)
                fn->returnType = vyxTypeToAnnotation(t->returnType);
            return fn;
        }
        case VyxTypeKind::Union: {
            auto u = std::make_unique<UnionType>();
            for (auto& m : t->tupleTypes)
                u->members.push_back(vyxTypeToAnnotation(m));
            return u;
        }

        // ── Container kinds ──
        // Option<T> is gone from VyxTypeKind — it arrives as Class-kind with
        // paramTypes[0]=T and falls through to the generic Class path below.
        // R5 phase 4d: Ref<T> / Scope<T> / Box<T> are Class-kind too; they
        // fall through to the Class branch below (same fall-through as
        // every other R5 container pivot).
        // R5 phase 4: Vec<T> is gone from VyxTypeKind — arrives as Class with
        // mangled name "Vec<T>" and falls through to the generic Class path
        // below (same treatment as Option / Result / String).
        // R5 phase 4b: Dict<K,V> / UnorderedMap<K,V> are Class-kind now; they
        // fall through to the generic Class-path below.
        // R5 phase 4c: Stack<T> / Queue<T> / Set<T> / UnorderedSet<T> are
        // Class-kind too; same fall-through — the Class branch decomposes
        // the mangled name back into a structured GenericType annotation.
        // R5 phase 4 batch 4: Delegate<fn(...)> / Event<fn(...)> are Class-kind
        // now too; same fall-through — name prefix "Delegate<" / "Event<" is
        // recognised by CodeGen for backend routing.

        // ── Variadic pack: AST has no PackType; synthesise GenericType("pack") ──
        case VyxTypeKind::Pack: {
            auto g = std::make_unique<GenericType>();
            g->name = "pack";
            for (auto& m : t->tupleTypes)
                g->typeArgs.push_back(vyxTypeToAnnotation(m));
            return g;
        }

        case VyxTypeKind::Unknown:
            return makeNamed("<unknown>");
    }

    return makeNamed(t->name.empty() ? "<unknown>" : t->name);
}

// ============================================================
//  AST deep-clone helpers
// ============================================================

TypePtr Monomorphize::cloneTypeAnnotation(const TypeAnnotation* src) const {
    if (!src) return nullptr;

    // If we are mid-instantiation, every TypeAnnotation in the cloned subtree
    // must be env-substituted. `substituteAnnotation` already handles the
    // resolve→substitute→reify round-trip and falls back to a pure clone if
    // resolution fails — exactly the shape we want here.
    if (currentEnv_) {
        return substituteAnnotation(src, *currentEnv_);
    }

    switch (src->kind) {
        case TypeAnnotationKind::Named: {
            auto n = std::make_unique<NamedType>();
            n->location = src->location;
            n->name = src->name;
            return n;
        }
        case TypeAnnotationKind::Generic: {
            auto* gt = src->as<GenericType>();
            auto g = std::make_unique<GenericType>();
            g->location = src->location;
            g->name = src->name;
            for (auto& arg : gt->typeArgs)
                g->typeArgs.push_back(cloneTypeAnnotation(arg.get()));
            for (auto& e : gt->argExprs)
                g->argExprs.push_back(cloneExpr(e.get()));
            return g;
        }
        case TypeAnnotationKind::Pointer: {
            auto* pt = src->as<PointerType>();
            auto p = std::make_unique<PointerType>();
            p->location = src->location;
            p->innerType = cloneTypeAnnotation(pt->innerType.get());
            return p;
        }
        case TypeAnnotationKind::Reference: {
            auto* rt = src->as<ReferenceType>();
            auto r = std::make_unique<ReferenceType>();
            r->location = src->location;
            r->isMutable = rt->isMutable;
            r->innerType = cloneTypeAnnotation(rt->innerType.get());
            return r;
        }
        case TypeAnnotationKind::Array: {
            auto* at = src->as<ArrayType>();
            auto a = std::make_unique<ArrayType>();
            a->location = src->location;
            a->elementType = cloneTypeAnnotation(at->elementType.get());
            a->size = cloneExpr(at->size.get());
            return a;
        }
        case TypeAnnotationKind::Tuple: {
            auto* tt = src->as<TupleType>();
            auto t = std::make_unique<TupleType>();
            t->location = src->location;
            for (auto& e : tt->elements)
                t->elements.push_back(cloneTypeAnnotation(e.get()));
            return t;
        }
        case TypeAnnotationKind::Function: {
            auto* ft = src->as<FunctionType>();
            auto f = std::make_unique<FunctionType>();
            f->location = src->location;
            for (auto& p : ft->paramTypes)
                f->paramTypes.push_back(cloneTypeAnnotation(p.get()));
            f->returnType = cloneTypeAnnotation(ft->returnType.get());
            return f;
        }
        case TypeAnnotationKind::Union: {
            auto* ut = src->as<UnionType>();
            auto u = std::make_unique<UnionType>();
            u->location = src->location;
            for (auto& m : ut->members)
                u->members.push_back(cloneTypeAnnotation(m.get()));
            return u;
        }
        case TypeAnnotationKind::PackIndex: {
            // Variadic type-pack indexing should have been resolved by
            // Sema during variadic expansion (createVariadicInstance).
            // If one survives into Mono, we keep it as a placeholder
            // so downstream passes can emit a precise diagnostic
            // pointing at the original `Ts[i]` annotation.
            auto* pi = src->as<PackIndexType>();
            auto cloned = std::make_unique<PackIndexType>();
            cloned->location = src->location;
            cloned->name = src->name;
            cloned->packName = pi->packName;
            return cloned;
        }
    }
    auto n = std::make_unique<NamedType>();
    n->location = src->location;
    n->name = src->name;
    return n;
}

ExprPtr Monomorphize::cloneExpr(const Expr* src) const {
    if (!src) return nullptr;
    auto cloneInferred = [this](const VyxTypePtr& inferred) -> VyxTypePtr {
        if (!inferred || !currentEnv_) return inferred;
        return ::vyx::substituteType(inferred, *currentEnv_);
    };

    switch (src->kind) {
        case ExprKind::IntLiteral: {
            auto* e = src->as<IntLiteralExpr>();
            auto n = std::make_unique<IntLiteralExpr>();
            n->location = e->location;
            n->value = e->value;
            n->inferredType = e->inferredType;
            return n;
        }
        case ExprKind::FloatLiteral: {
            auto* e = src->as<FloatLiteralExpr>();
            auto n = std::make_unique<FloatLiteralExpr>();
            n->location = e->location;
            n->value = e->value;
            n->inferredType = e->inferredType;
            return n;
        }
        case ExprKind::StringLiteral: {
            auto* e = src->as<StringLiteralExpr>();
            auto n = std::make_unique<StringLiteralExpr>();
            n->location = e->location;
            n->value = e->value;
            n->inferredType = e->inferredType;
            return n;
        }
        case ExprKind::BoolLiteral: {
            auto* e = src->as<BoolLiteralExpr>();
            auto n = std::make_unique<BoolLiteralExpr>();
            n->location = e->location;
            n->value = e->value;
            n->inferredType = e->inferredType;
            return n;
        }
        case ExprKind::CharLiteral: {
            auto* e = src->as<CharLiteralExpr>();
            auto n = std::make_unique<CharLiteralExpr>();
            n->location = e->location;
            n->value = e->value;
            n->inferredType = e->inferredType;
            return n;
        }
        case ExprKind::NullLiteral: {
            auto n = std::make_unique<NullLiteralExpr>();
            n->location = src->location;
            n->inferredType = src->inferredType;
            return n;
        }
        case ExprKind::Identifier: {
            auto* e = src->as<IdentifierExpr>();
            // If this identifier names a const generic parameter, substitute it
            // with the concrete integer literal from the active TypeEnv so that
            // e.g. `return N;` inside `fn make_buf<const N: i64>()` emits the
            // correct literal value at each instantiation site.
            if (currentEnv_ && currentEnv_->hasConst(e->name)) {
                auto lit = std::make_unique<IntLiteralExpr>();
                lit->location = e->location;
                lit->value = currentEnv_->lookupConst(e->name);
                lit->inferredType = e->inferredType;
                return lit;
            }
            auto n = std::make_unique<IdentifierExpr>();
            n->location = e->location;
            n->name = e->name;
            n->inferredType = cloneInferred(e->inferredType);
            n->typeAnnotation = cloneTypeAnnotation(e->typeAnnotation.get());
            for (auto& ta : e->callTypeArgs)
                n->callTypeArgs.push_back(cloneTypeAnnotation(ta.get()));
            // Const-arg expressions (non-type turbofish args like `Array::<i32, 16>`).
            for (auto& ae : e->callArgExprs)
                n->callArgExprs.push_back(cloneExpr(ae.get()));
            return n;
        }
        case ExprKind::BinaryOp: {
            auto* e = src->as<BinaryOpExpr>();
            auto n = std::make_unique<BinaryOpExpr>();
            n->location = e->location;
            n->op = e->op;
            n->lhs = cloneExpr(e->lhs.get());
            n->rhs = cloneExpr(e->rhs.get());
            n->inferredType = e->inferredType;
            return n;
        }
        case ExprKind::UnaryOp: {
            auto* e = src->as<UnaryOpExpr>();
            auto n = std::make_unique<UnaryOpExpr>();
            n->location = e->location;
            n->op = e->op;
            n->operand = cloneExpr(e->operand.get());
            n->inferredType = e->inferredType;
            return n;
        }
        case ExprKind::Call: {
            auto* e = src->as<CallExpr>();
            auto n = std::make_unique<CallExpr>();
            n->location = e->location;
            n->callee = cloneExpr(e->callee.get());
            n->argNames = e->argNames;
            n->inferredType = e->inferredType;
            for (auto& arg : e->args)
                n->args.push_back(cloneExpr(arg.get()));
            return n;
        }
        case ExprKind::MemberAccess: {
            auto* e = src->as<MemberAccessExpr>();
            auto n = std::make_unique<MemberAccessExpr>();
            n->location = e->location;
            n->object = cloneExpr(e->object.get());
            n->member = e->member;
            n->inferredType = e->inferredType;
            for (auto& ta : e->callTypeArgs)
                n->callTypeArgs.push_back(cloneTypeAnnotation(ta.get()));
            return n;
        }
        case ExprKind::Index: {
            auto* e = src->as<IndexExpr>();
            auto n = std::make_unique<IndexExpr>();
            n->location = e->location;
            n->object = cloneExpr(e->object.get());
            n->indexExpr = cloneExpr(e->indexExpr.get());
            n->inferredType = e->inferredType;
            return n;
        }
        case ExprKind::Assignment: {
            auto* e = src->as<AssignmentExpr>();
            auto n = std::make_unique<AssignmentExpr>();
            n->location = e->location;
            n->lhs = cloneExpr(e->lhs.get());
            n->rhs = cloneExpr(e->rhs.get());
            n->inferredType = e->inferredType;
            return n;
        }
        case ExprKind::CompoundAssignment: {
            auto* e = src->as<CompoundAssignmentExpr>();
            auto n = std::make_unique<CompoundAssignmentExpr>();
            n->location = e->location;
            n->op = e->op;
            n->target = cloneExpr(e->target.get());
            n->value = cloneExpr(e->value.get());
            n->inferredType = e->inferredType;
            return n;
        }
        case ExprKind::Cast: {
            auto* e = src->as<CastExpr>();
            auto n = std::make_unique<CastExpr>();
            n->location = e->location;
            n->operand = cloneExpr(e->operand.get());
            n->targetType = cloneTypeAnnotation(e->targetType.get());
            n->inferredType = e->inferredType;
            return n;
        }
        case ExprKind::StructInit: {
            auto* e = src->as<StructInitExpr>();
            auto n = std::make_unique<StructInitExpr>();
            n->location = e->location;
            n->structName = e->structName;
            // Substitute generic params embedded in structName (e.g. the
            // `T` in `Tree::<T> { ... }` appearing inside `Tree<T>.leaf()`).
            // Parser bakes the mangled form into `structName`, so at Mono
            // substitution time we need to rewrite each identifier token
            // against the active env. Single-identifier substring replace
            // is safe here because structName comes from the mangler and
            // tokens are `[A-Za-z_][A-Za-z0-9_]*` separated by `<,>`.
            // Substitute class-level generic params embedded in structName
            // (e.g. the `T` in `Tree::<T> { ... }` used inside `Tree<T>.leaf()`).
            // Parser bakes the mangled form into structName; at Mono time we
            // rewrite each generic-param identifier against the active env so
            // the downstream CodeGen lookup matches the Mono-registered
            // concrete name (`Tree<i32>` rather than `Tree<T>`).
            //
            // Scope tightly: only env keys that are *single uppercase letters
            // optionally followed by digits* (T, U, K, V, T1, …) are considered
            // generic params. This avoids accidentally rewriting already-
            // mangled inner tokens like built-in class names.
            if (currentEnv_ && !n->structName.empty() &&
                n->structName.find('<') != std::string::npos) {
                std::string& s = n->structName;
                std::string out;
                out.reserve(s.size() + 16);
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
                        // Recognise a generic-param token by checking if the
                        // active env has a binding for it (type or const).
                        // `isGenericParamName` was a conservative syntactic
                        // guard for names like `T`/`T1`; we tighten it to the
                        // env's own set so multi-letter names like `Cap` or
                        // `Dim` also resolve.
                        if (currentEnv_->has(token)) {
                            auto sub = currentEnv_->lookup(token);
                            auto m = sub ? sub->mangle() : std::string();
                            if (!m.empty()) {
                                out += m;
                                changed = true;
                            } else {
                                out += token;
                            }
                        } else if (currentEnv_->hasConst(token)) {
                            // Const-generic slot: emit the bound integer.
                            // (e.g. `Array<i32,N>` with N=8 → `Array<i32,8>`.)
                            out += std::to_string(currentEnv_->lookupConst(token));
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
                if (changed) n->structName = std::move(out);
            }
            n->inferredType = e->inferredType;
            n->typeAnnotation = cloneTypeAnnotation(e->typeAnnotation.get());
            n->spreadBase = cloneExpr(e->spreadBase.get());
            for (auto& [fname, fexpr] : e->fieldInits)
                n->fieldInits.emplace_back(fname, cloneExpr(fexpr.get()));
            return n;
        }
        case ExprKind::ArrayInit: {
            auto* e = src->as<ArrayInitExpr>();
            auto n = std::make_unique<ArrayInitExpr>();
            n->location = e->location;
            n->inferredType = e->inferredType;
            n->elementIsSpread = e->elementIsSpread;
            n->repeatCount = cloneExpr(e->repeatCount.get());
            for (auto& elem : e->elements)
                n->elements.push_back(cloneExpr(elem.get()));
            return n;
        }
        case ExprKind::TupleInit: {
            auto* e = src->as<TupleInitExpr>();
            auto n = std::make_unique<TupleInitExpr>();
            n->location = e->location;
            n->inferredType = e->inferredType;
            for (auto& elem : e->elements)
                n->elements.push_back(cloneExpr(elem.get()));
            return n;
        }
        case ExprKind::StringInterpolation: {
            auto* e = src->as<StringInterpExpr>();
            auto n = std::make_unique<StringInterpExpr>();
            n->location = e->location;
            n->inferredType = e->inferredType;
            for (auto& part : e->parts) {
                InterpPart p;
                p.isExpr = part.isExpr;
                p.text = part.text;
                if (part.expr)
                    p.expr = cloneExpr(part.expr.get());
                n->parts.push_back(std::move(p));
            }
            return n;
        }
        case ExprKind::SelfExpr: {
            auto n = std::make_unique<SelfExpr>();
            n->location = src->location;
            n->inferredType = src->inferredType;
            return n;
        }
        case ExprKind::FailExpr: {
            auto* e = src->as<FailExpr>();
            auto n = std::make_unique<FailExpr>();
            n->location = e->location;
            n->typeName = e->typeName;
            n->variant = e->variant;
            n->message = cloneExpr(e->message.get());
            n->inferredType = e->inferredType;
            return n;
        }
        case ExprKind::TryExpr: {
            auto* e = src->as<TryExpr>();
            auto n = std::make_unique<TryExpr>();
            n->location = e->location;
            n->inner = cloneExpr(e->inner.get());
            n->inferredType = e->inferredType;
            return n;
        }
        case ExprKind::Ternary: {
            auto* e = src->as<TernaryExpr>();
            auto n = std::make_unique<TernaryExpr>();
            n->location = e->location;
            n->condition = cloneExpr(e->condition.get());
            n->trueExpr = cloneExpr(e->trueExpr.get());
            n->falseExpr = cloneExpr(e->falseExpr.get());
            n->inferredType = e->inferredType;
            return n;
        }
        case ExprKind::Closure: {
            auto* e = src->as<ClosureExpr>();
            auto n = std::make_unique<ClosureExpr>();
            n->location = e->location;
            n->inferredType = e->inferredType;
            // CaptureItem contains an ExprPtr moveExpr — deep-copy each
            for (auto& cap : e->captures) {
                CaptureItem c;
                c.name = cap.name;
                c.byRef = cap.byRef;
                c.move = cap.move;
                c.moveExpr = cloneExpr(cap.moveExpr.get());
                n->captures.push_back(std::move(c));
            }
            for (auto& p : e->params) {
                ClosureParam cp;
                cp.name = p.name;
                cp.type = cloneTypeAnnotation(p.type.get());
                n->params.push_back(std::move(cp));
            }
            n->returnType = cloneTypeAnnotation(e->returnType.get());
            n->body = cloneStmt(e->body.get());
            n->singleExpr = cloneExpr(e->singleExpr.get());
            return n;
        }
        case ExprKind::AwaitExpr: {
            auto* e = src->as<AwaitExpr>();
            auto n = std::make_unique<AwaitExpr>();
            n->location = e->location;
            n->inner = cloneExpr(e->inner.get());
            n->inferredType = e->inferredType;
            return n;
        }
        case ExprKind::InlineAsm: {
            auto* e = src->as<InlineAsmExpr>();
            auto n = std::make_unique<InlineAsmExpr>();
            n->location = e->location;
            n->asmTemplate = e->asmTemplate;
            n->constraints = e->constraints;
            n->hasSideEffects = e->hasSideEffects;
            n->inferredType = e->inferredType;
            for (auto& op : e->operands)
                n->operands.push_back(cloneExpr(op.get()));
            return n;
        }
        // P3-Q: compile-time type reflection — T::kind / T::name
        // When currentEnv_ is active, look up the concrete type bound to typeParam
        // and replace with a StringLiteralExpr carrying the result.
        // T::fields / T::methods are NOT lowered here — they are expanded by
        // cloneStmt when it processes the enclosing ForEachStmt.  If they appear
        // outside a for-in loop (illegal usage), return an empty string literal.
        case ExprKind::TypeReflect: {
            auto* e = src->as<TypeReflectExpr>();
            auto lit = std::make_unique<StringLiteralExpr>();
            lit->location = e->location;
            lit->inferredType = src->inferredType;

            if (currentEnv_) {
                VyxTypePtr concreteType = currentEnv_->lookup(e->typeParam);
                if (concreteType) {
                    if (e->member == "kind") {
                        // Must match SemaGeneric::reflectKindString() and
                        // vyxTypeKindToReflectString() for where-clause / body consistency.
                        switch (concreteType->kind) {
                            case VyxTypeKind::Struct:    lit->value = "struct";    break;
                            case VyxTypeKind::Class:     lit->value = "class";     break;
                            case VyxTypeKind::Interface: lit->value = "interface"; break;
                            case VyxTypeKind::ErrorType: lit->value = "error";     break;
                            case VyxTypeKind::Integer:   lit->value = "int";       break;
                            case VyxTypeKind::Float:     lit->value = "float";     break;
                            case VyxTypeKind::Bool:      lit->value = "bool";      break;
                            case VyxTypeKind::Function:  lit->value = "fn";        break;
                            default:                     lit->value = "primitive"; break;
                        }
                    } else if (e->member == "name") {
                        lit->value = concreteType->toString();
                    } else {
                        // fields/methods outside for-in: leave as empty string.
                        lit->value = "";
                    }
                } else {
                    // Unbound generic param — leave as empty string; Sema should
                    // have caught this, but be defensive.
                    lit->value = "";
                }
            } else {
                // No active env (not inside an instantiation): plain clone.
                auto cloned = std::make_unique<TypeReflectExpr>();
                cloned->location  = e->location;
                cloned->typeParam = e->typeParam;
                cloned->member    = e->member;
                cloned->inferredType = e->inferredType;
                return cloned;
            }
            return lit;
        }

        // P5-pack: pack fold expansion — `args...+` becomes `arg0 op arg1 op arg2 ...`
        //
        // At mono time `currentEnv_` is active, so we can look up the concrete pack
        // type to find how many members it has, then synthesise one IdentifierExpr
        // per numbered expansion param (`args_0`, `args_1`, …, matching the names
        // assigned by Sema::createVariadicInstance) and chain them left-to-right with
        // the fold operator.
        //
        // Name convention for variadic params: Sema's createVariadicInstance names
        // expanded positional params as `<packValueName>_<i>` where the pack value
        // name is just the original param name (e.g. `args_0`, `args_1`). We use
        // the same convention here.
        //
        // If the pack is unknown / empty, fall through to a plain clone so Sema's
        // earlier error (P2D-004) stands alone without an ICE.
        // F: sizeof...(pack) — resolve to the concrete pack length at Mono time
        // (mirrors the PackFold pack-length lookup).
        case ExprKind::SizeofPack: {
            auto* e = src->as<SizeofPackExpr>();
            size_t packLen = 0;
            if (currentEnv_) {
                auto packType = currentEnv_->lookup(e->packName);
                if (packType && packType->isPack()) {
                    packLen = packType->tupleTypes.size();
                } else if (packType) {
                    size_t i = 0;
                    while (currentEnv_->lookup(e->packName + "_" + std::to_string(i))) ++i;
                    packLen = i;
                }
            }
            auto lit = std::make_unique<IntLiteralExpr>();
            lit->location = e->location;
            lit->value = static_cast<int64_t>(packLen);
            return lit;
        }

        case ExprKind::PackFold: {
            auto* e = src->as<PackFoldExpr>();

            // Attempt to locate the concrete pack type from the current env.
            VyxTypePtr packType;
            if (currentEnv_) {
                packType = currentEnv_->lookup(e->packName);
            }

            size_t packLen = 0;
            if (packType && packType->isPack()) {
                packLen = packType->tupleTypes.size();
            } else if (packType) {
                // Pack may be stored as individual bindings `packName_0`, `packName_1`...
                // Count them from the env by scanning numbered suffixes.
                size_t i = 0;
                while (currentEnv_->lookup(e->packName + "_" + std::to_string(i))) ++i;
                packLen = i;
            }

            if (packLen == 0) {
                // Empty or unknown pack — emit a plain clone (errors were already
                // reported by Sema).
                auto n = std::make_unique<PackFoldExpr>();
                n->location    = e->location;
                n->packName    = e->packName;
                n->op          = e->op;
                n->inferredType = e->inferredType;
                return n;
            }

            // Build the expansion left-to-right:
            //   packLen == 1  →  pack_0
            //   packLen == 2  →  pack_0 op pack_1
            //   packLen == 3  →  (pack_0 op pack_1) op pack_2
            auto makeArg = [&](size_t i) -> ExprPtr {
                auto id = std::make_unique<IdentifierExpr>();
                id->location = e->location;
                id->name = e->packName + "_" + std::to_string(i);
                return id;
            };

            ExprPtr chain = makeArg(0);
            for (size_t i = 1; i < packLen; ++i) {
                auto bin = std::make_unique<BinaryOpExpr>();
                bin->location = e->location;
                bin->op  = e->op;
                bin->lhs = std::move(chain);
                bin->rhs = makeArg(i);
                chain = std::move(bin);
            }
            return chain;
        }
    }
    // Unreachable fallback
    auto stub = std::make_unique<IntLiteralExpr>();
    stub->location = src->location;
    return stub;
}

StmtPtr Monomorphize::cloneStmt(const Stmt* src) const {
    if (!src) return nullptr;

    switch (src->kind) {
        case StmtKind::VarDecl: {
            auto* s = src->as<VarDeclStmt>();
            auto n = std::make_unique<VarDeclStmt>();
            n->location = s->location;
            n->isConst = s->isConst;
            n->isComptime = s->isComptime;
            n->varName = s->varName;
            n->varType = cloneTypeAnnotation(s->varType.get());
            n->initExpr = cloneExpr(s->initExpr.get());
            n->elseBranch = cloneStmt(s->elseBranch.get());
            n->tupleBindings = s->tupleBindings;
            return n;
        }
        case StmtKind::ExprStmt: {
            auto* s = src->as<ExprStmt>();
            auto n = std::make_unique<ExprStmt>();
            n->location = s->location;
            n->expr = cloneExpr(s->expr.get());
            return n;
        }
        case StmtKind::Return: {
            auto* s = src->as<ReturnStmt>();
            auto n = std::make_unique<ReturnStmt>();
            n->location = s->location;
            n->expr = cloneExpr(s->expr.get());
            return n;
        }
        case StmtKind::If: {
            auto* s = src->as<IfStmt>();
            auto n = std::make_unique<IfStmt>();
            n->location = s->location;
            n->condition = cloneExpr(s->condition.get());
            n->thenBranch = cloneStmt(s->thenBranch.get());
            for (auto& [cond, branch] : s->elifBranches)
                n->elifBranches.emplace_back(cloneExpr(cond.get()),
                                              cloneStmt(branch.get()));
            n->elseBranch = cloneStmt(s->elseBranch.get());
            return n;
        }
        case StmtKind::While: {
            auto* s = src->as<WhileStmt>();
            auto n = std::make_unique<WhileStmt>();
            n->location = s->location;
            n->condition = cloneExpr(s->condition.get());
            n->body = cloneStmt(s->body.get());
            return n;
        }
        case StmtKind::For: {
            auto* s = src->as<ForStmt>();
            auto n = std::make_unique<ForStmt>();
            n->location = s->location;
            n->init = cloneStmt(s->init.get());
            n->condition = cloneExpr(s->condition.get());
            n->step = cloneExpr(s->step.get());
            n->body = cloneStmt(s->body.get());
            return n;
        }
        case StmtKind::ForEach: {
            auto* s = src->as<ForEachStmt>();

            // P3-Q: `for f in T::fields` / `for m in T::methods` expansion.
            // When the collection is a TypeReflectExpr and currentEnv_ is active,
            // we know the concrete type — expand to N copies of the body, one per
            // field (or method).  Each copy runs with the loop variable bound to
            // a synthetic VarDeclStmt carrying the field/method name as a string
            // literal so the body can reference `f` if it wants.
            if (s->collection && s->collection->kind == ExprKind::TypeReflect && currentEnv_) {
                auto* rexpr = s->collection->as<TypeReflectExpr>();
                bool isFields  = (rexpr->member == "fields");
                bool isMethods = (rexpr->member == "methods");
                if (isFields || isMethods) {
                    VyxTypePtr concreteType = currentEnv_->lookup(rexpr->typeParam);
                    if (concreteType) {
                        // Collect the names we want to iterate over.
                        std::vector<std::string> names;
                        if (isFields) {
                            for (auto& f : concreteType->fields)
                                names.push_back(f.name);
                        } else {
                            for (auto& m : concreteType->methods)
                                names.push_back(m.name);
                        }

                        // Build a Block containing N copies of the body.
                        auto blk = std::make_unique<BlockStmt>();
                        blk->location = src->location;
                        for (size_t i = 0; i < names.size(); ++i) {
                            // `let f = "<fieldname>";`  — makes `f` available in body.
                            auto nameStr = std::make_unique<StringLiteralExpr>();
                            nameStr->location = src->location;
                            nameStr->value = names[i];

                            auto varDecl = std::make_unique<VarDeclStmt>();
                            varDecl->location = src->location;
                            varDecl->isConst  = true;
                            varDecl->varName  = s->varName;
                            varDecl->initExpr = std::move(nameStr);

                            auto bodyClone = cloneStmt(s->body.get());

                            auto innerBlk = std::make_unique<BlockStmt>();
                            innerBlk->location = src->location;
                            innerBlk->statements.push_back(std::move(varDecl));
                            if (bodyClone) innerBlk->statements.push_back(std::move(bodyClone));

                            blk->statements.push_back(std::move(innerBlk));
                        }
                        return blk;
                    }
                }
            }

            // Regular foreach — structural clone.
            auto n = std::make_unique<ForEachStmt>();
            n->location = s->location;
            n->varName = s->varName;
            n->destructure = s->destructure;
            n->collection = cloneExpr(s->collection.get());
            n->body = cloneStmt(s->body.get());
            return n;
        }
        case StmtKind::Block: {
            auto* s = src->as<BlockStmt>();
            auto n = std::make_unique<BlockStmt>();
            n->location = s->location;
            for (auto& stmt : s->statements)
                n->statements.push_back(cloneStmt(stmt.get()));
            return n;
        }
        case StmtKind::Break: {
            auto n = std::make_unique<BreakStmt>();
            n->location = src->location;
            return n;
        }
        case StmtKind::Continue: {
            auto n = std::make_unique<ContinueStmt>();
            n->location = src->location;
            return n;
        }
        case StmtKind::Match: {
            auto* s = src->as<MatchStmt>();
            auto n = std::make_unique<MatchStmt>();
            n->location = s->location;
            n->expr = cloneExpr(s->expr.get());
            for (auto& arm : s->arms) {
                MatchArm a;
                a.location = arm.location;
                a.label = arm.label;
                a.bindingName = arm.bindingName;
                a.typePattern = cloneTypeAnnotation(arm.typePattern.get());
                a.valuePattern = cloneExpr(arm.valuePattern.get());
                a.guardExpr = cloneExpr(arm.guardExpr.get());
                a.tupleBindings = arm.tupleBindings;
                a.nestedPatterns = arm.nestedPatterns;
                a.isDefault = arm.isDefault;
                a.body = cloneStmt(arm.body.get());
                n->arms.push_back(std::move(a));
            }
            return n;
        }
        case StmtKind::Assignment: {
            auto* s = src->as<AssignStmt>();
            auto n = std::make_unique<AssignStmt>();
            n->location = s->location;
            n->target = cloneExpr(s->target.get());
            n->value = cloneExpr(s->value.get());
            return n;
        }
        case StmtKind::Defer: {
            auto* s = src->as<DeferStmt>();
            auto n = std::make_unique<DeferStmt>();
            n->location = s->location;
            n->body = cloneStmt(s->body.get());
            return n;
        }
        case StmtKind::StaticAssert: {
            auto* s = src->as<StaticAssertStmt>();
            auto n = std::make_unique<StaticAssertStmt>();
            n->location = s->location;
            n->expr = cloneExpr(s->expr.get());
            n->message = s->message;
            return n;
        }
        case StmtKind::Unsafe: {
            auto* s = src->as<UnsafeStmt>();
            auto n = std::make_unique<UnsafeStmt>();
            n->location = s->location;
            n->body = cloneStmt(s->body.get());
            return n;
        }
    }
    return nullptr;
}

// ============================================================
//  TypeAnnotation substitution
// ============================================================

TypePtr Monomorphize::substituteAnnotation(const TypeAnnotation* src,
                                            const TypeEnv& env) const {
    if (!src) return nullptr;

    // ── Unified GenericType path (H2) ──
    // Industry-strength generics must not name-discriminate: every
    // `Foo<T1, T2, ...>` annotation — whether `Foo` is a built-in container
    // (Vec, Option, Ref, Scope, Box, Stack, Queue, Set, UnorderedSet, Dict,
    // UnorderedMap, Result, Delegate, Event) or a user-defined generic class
    // (Slice, PriorityQueue, HashSet, ...) — substitutes its type arguments
    // structurally while preserving the `GenericType` shell.
    //
    // Pre-H2 the built-ins took the VyxType round-trip below
    // (TemplateResolver::resolveTypeAnnotation → substituteType →
    // vyxTypeToAnnotation) which re-built equivalent GenericType nodes by
    // dispatching on VyxTypeKind. That produced a name-based whitelist that
    // user classes had to bypass with a fast-path or be re-emitted as
    // `Class("PriorityQueue<T>")` (an opaque NamedType the downstream
    // CodeGen rejected as "unknown type annotation 'PriorityQueue<T>'").
    //
    // The ErrorDef intrinsics Option / Result are likewise no longer
    // identified by string match here: they are first identified by their
    // VyxTypeKind (Optional / Result) only when the downstream Sema /
    // CodeGen passes call resolveTypeAnnotation on the resulting GenericType
    // — at which point name → kind mapping already exists and this pass
    // does not need to second-guess it.
    if (src->kind == TypeAnnotationKind::Generic) {
        auto* gt = src->as<GenericType>();
        auto out = std::make_unique<GenericType>();
        out->location = src->location;
        out->name = src->name;
        out->typeArgs.reserve(gt->typeArgs.size());
        for (auto& a : gt->typeArgs)
            out->typeArgs.push_back(substituteAnnotation(a.get(), env));
        // Carry argExprs through so a nested `Array<T, N>` reference inside a
        // template body keeps its const-arg expression after instantiation —
        // when the outer instantiation later substitutes `T → i32` the same
        // Generic node still has the original constant slot for downstream
        // requestTemplateWithArgs to evaluate.
        for (auto& e : gt->argExprs)
            out->argExprs.push_back(cloneExpr(e.get()));
        return out;
    }

    // Array types: substitute element type, and rewrite a non-literal size
    // expression that references a `const N: usize`-style template parameter
    // into the bound integer literal. Without this rewrite the size collapses
    // to 0 in the cloned annotation and `SemaResolve` would produce a
    // zero-length array type.
    if (src->kind == TypeAnnotationKind::Array) {
        auto* at = src->as<ArrayType>();
        auto out = std::make_unique<ArrayType>();
        out->location = src->location;
        out->elementType = substituteAnnotation(at->elementType.get(), env);
        if (at->size) {
            int64_t cv = 0;
            if (evalConstGenericArg(at->size.get(), env, cv)) {
                auto lit = std::make_unique<IntLiteralExpr>();
                lit->value = cv;
                lit->location = at->size->location;
                out->size = std::move(lit);
            } else {
                out->size = cloneExpr(at->size.get());
            }
        }
        return out;
    }

    // Bare `T` NamedType: substitute directly via env so we get the
    // concrete type back as a NamedType (e.g. `T` → `i64`). Without
    // this short-circuit the roundtrip below works too, but only when
    // resolveTypeAnnotation sees `T` listed in `genericParams`.
    if (src->kind == TypeAnnotationKind::Named) {
        auto repl = env.lookup(src->name);
        if (repl) return vyxTypeToAnnotation(repl);
        // Const-generic param (e.g. N in Buffer<T, N>): the integer is in
        // env.constBindings, not env.bindings.  Emit it as a NamedType string
        // so GenericType("Buffer",[NamedType("i32"),NamedType("16")]) is produced
        // and toLLVMType can find "Buffer<i32,16>" in structTypes_.
        if (env.hasConst(src->name)) {
            auto n = std::make_unique<NamedType>();
            n->name = std::to_string(env.lookupConst(src->name));
            n->location = src->location;
            return n;
        }
    }

    // P2b Wave 3+: Dependent type `T::Item` where T has been bound to a
    // generic class instantiation like `Iter<i32>`.
    //
    // Resolution order:
    //  a. Direct lookup in env ("T::Item" already pre-seeded by bindDependentTypesForSubst).
    //  b. If T is bound to a monomorphised name ("Iter<i32>"), strip the args,
    //     look up implAssocByTemplate_["Iter"]["Item"] → raw annotation,
    //     build substitution from the args, resolve.
    if (src->kind == TypeAnnotationKind::Dependent) {
        auto* dt = src->as<DependentType>();
        const std::string depKey = dt->baseName + "::" + dt->memberName;
        // (a) Direct env lookup (pre-seeded by Sema's bindDependentTypesForSubst).
        auto directRepl = env.lookup(depKey);
        if (directRepl) return vyxTypeToAnnotation(directRepl);
        // (b) Derive from T → Iter<i32> binding via template registry.
        auto baseRepl = env.lookup(dt->baseName);
        if (baseRepl && !baseRepl->name.empty()) {
            auto lt = baseRepl->name.find('<');
            if (lt != std::string::npos) {
                const std::string baseName = baseRepl->name.substr(0, lt);
                auto tmplIt = implAssocByTemplate_.find(baseName);
                if (tmplIt != implAssocByTemplate_.end()) {
                    auto memberIt = tmplIt->second.find(dt->memberName);
                    if (memberIt != tmplIt->second.end() && memberIt->second) {
                        // Parse args from the monomorphised name.
                        std::string argsStr = baseRepl->name.substr(lt + 1);
                        if (!argsStr.empty() && argsStr.back() == '>') argsStr.pop_back();
                        std::vector<std::string> argNames;
                        { int depth = 0; std::string cur;
                          for (char c : argsStr) {
                            if (c == '<') { ++depth; cur += c; }
                            else if (c == '>') { --depth; cur += c; }
                            else if (c == ',' && depth == 0) { argNames.push_back(cur); cur.clear(); }
                            else cur += c;
                          }
                          if (!cur.empty()) argNames.push_back(cur); }
                        // Find the template decl to get param names.
                        if (unit_) {
                            for (auto& d : unit_->declarations) {
                                if (!d || d->name != baseName) continue;
                                if (d->kind != DeclKind::Class &&
                                    d->kind != DeclKind::Struct) continue;
                                if (d->genericParams.empty()) continue;
                                GenericSubstitution subst;
                                for (size_t i = 0; i < d->genericParams.size() &&
                                                   i < argNames.size(); ++i) {
                                    // Build a synthetic VyxType from the arg name string.
                                    auto argType = std::make_shared<VyxType>();
                                    argType->kind = VyxTypeKind::Class;
                                    argType->name = argNames[i];
                                    subst.add(d->genericParams[i], argType);
                                }
                                auto resolved = resolver_.substituteType(*memberIt->second, subst);
                                if (resolved && resolved->kind != VyxTypeKind::Generic) {
                                    return vyxTypeToAnnotation(resolved);
                                }
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    // Two-step: AST → VyxType (pure structural conversion via the canonical
    // resolver), then VyxType → VyxType through env substitution. The
    // resulting VyxType is then projected back to a TypeAnnotation for the
    // cloned decl. The legacy `TemplateResolver::substituteType` route via
    // `toGenericSubstitution(env)` is kept for fallback in case
    // `resolveTypeAnnotation` returns Unknown (defensive — should not happen
    // for any valid AST shape).
    //
    // Hand `resolveTypeAnnotation` the env's bound names as the active
    // genericParams set. Without this, a bare `T` NamedType is parked as
    // `Class("T")` and slips past `substituteType` (which only rewrites
    // `Generic` nodes) — surfacing later as `undefined type T` errors in
    // method bodies after T4-bodies enabled `analyzeAllMethodBodies`.
    std::vector<std::string> generics;
    generics.reserve(env.bindings.size());
    for (auto& [name, _] : env.bindings) generics.push_back(name);

    VyxTypePtr resolved = TemplateResolver::resolveTypeAnnotation(*src, &generics);
    VyxTypePtr substituted = ::vyx::substituteType(resolved, env);
    if (!substituted || substituted->kind == VyxTypeKind::Unknown) {
        auto subst = toGenericSubstitution(env);
        substituted = resolver_.substituteType(*src, subst);
    }
    if (substituted) {
        return vyxTypeToAnnotation(substituted);
    }
    // Avoid recursion through `cloneTypeAnnotation` here: at this point we
    // already know `currentEnv_` may be set, but substitution failed, so
    // fall back to a pure structural copy by routing through the same
    // function with a temporarily-cleared env.
    auto* save = currentEnv_;
    currentEnv_ = nullptr;
    auto out = cloneTypeAnnotation(src);
    currentEnv_ = save;
    return out;
}

// ============================================================
//  Lookup helpers
// ============================================================

const Decl* Monomorphize::findDecl(const std::string& name) const {
    if (!unit_) return nullptr;
    const Decl* importedMatch = nullptr;
    for (auto& d : unit_->declarations) {
        if (!d || d->name != name) continue;
        if (!d->isImported)
            return d.get();
        if (!importedMatch)
            importedMatch = d.get();
    }
    return importedMatch;
}

// ============================================================
//  Method-level generic helpers
// ============================================================

// static
std::string Monomorphize::buildMangledMethodName(
        const std::string& classMono,
        const std::string& methodName,
        const std::vector<VyxTypePtr>& methodArgTypes) {
    // Form: "ClassMono.method<U1,U2,...>"
    // The method suffix is built with the same canonical mangler used for
    // class-level generics so the names are consistent (e.g. same separator,
    // same nested angle-bracket serialisation).
    //
    // We only use TemplateResolver::mangleTemplateName for the method part
    // (not the class part) since classMono is already fully mangled.
    std::string methodMangled = TemplateResolver::mangleTemplateName(methodName, methodArgTypes);
    return classMono + "." + methodMangled;
}

const Decl* Monomorphize::makeSyntheticMethodDecl(const std::string& /*className*/,
                                                     const MethodDecl& method,
                                                     bool forceStatic) {
    auto fd = std::make_unique<FunctionDecl>();
    fd->kind      = DeclKind::Function;
    fd->location  = method.location;
    // Use the plain method name; the InstantiationRequest carries the full
    // "ClassMono.method<U>" mangled name that will be used as the output name.
    fd->name      = method.name;
    fd->isExport  = false;
    fd->isImported = false;
    fd->visibility = method.visibility;
    fd->isAsync   = method.isAsync;
    fd->isComptime = false;
    fd->isBench   = false;
    fd->attributes = method.attributes;
    fd->docComment = method.docComment;

    // The method's own generic parameters become the template parameters of
    // this synthetic FunctionDecl. Class-level bindings are supplied via the
    // TypeEnv of the InstantiationRequest rather than appearing in genericParams.
    fd->genericParams = method.genericParams;
    // genericConstParams maps param name -> TypeAnnotation (unique_ptr); must
    // deep-clone since unique_ptr is not copyable.
    {
        auto* savedEnv = currentEnv_;
        currentEnv_ = nullptr;
        for (auto& [k, v] : method.genericConstParams)
            fd->genericConstParams[k] = cloneTypeAnnotation(v.get());
        currentEnv_ = savedEnv;
    }
    fd->genericConstraints  = method.genericConstraints;
    fd->typeEqualityConstraints = method.typeEqualityConstraints;
    fd->typeInequalityConstraints = method.typeInequalityConstraints;

    // Prepend an implicit `self` pointer parameter so the emitted LLVM
    // function matches the ABI expected by CodeGen class-method dispatch
    // (first arg = pointer to receiver struct).
    //
    // IMPORTANT: the parser retains an explicitly-written `self` in
    // `method.params[0]` (e.g. `fn push_many<U>(self, others, conv)`).
    // In that case we MUST NOT prepend a second synthetic self — the
    // real param already occupies slot 0 and the type clone below will
    // copy it through (its annotation is null, which is exactly the
    // shape the emitted body expects because emitFunctionDecl treats
    // an unannotated `self` as a pointer to currentClassName_).
    const bool emitAsStatic = forceStatic || method.isStatic;
    bool hasExplicitSelf = !method.params.empty() &&
                           method.params.front().name == "self";
    if (!emitAsStatic && !hasExplicitSelf) {
        ParamDecl selfParam;
        selfParam.name = "self";
        selfParam.isMutRef = true;
        auto ptrTy = std::make_unique<PointerType>();
        ptrTy->innerType = std::make_unique<NamedType>();
        ptrTy->innerType->name = "__self_opaque";
        selfParam.type = std::move(ptrTy);
        fd->params.push_back(std::move(selfParam));
    }

    // Copy the remaining declared parameters verbatim; substituteAnnotation
    // inside `instantiate()` will later resolve any type references that use
    // class-level OR method-level generic names when instantiate() is called
    // with the merged TypeEnv.
    for (auto& p : method.params) {
        ParamDecl pd;
        pd.name = p.name;
        pd.isMutRef = p.isMutRef;
        // Deep-clone the type annotation without substitution (the env is
        // applied by instantiate() → substituteAnnotation).
        if (p.type) {
            // Use the pure structural cloner (no active env yet).
            auto* savedEnv = currentEnv_;
            currentEnv_ = nullptr;
            pd.type = cloneTypeAnnotation(p.type.get());
            currentEnv_ = savedEnv;
        } else if (p.name == "self") {
            // Explicit `self` param has no type annotation (the parser leaves
            // it null so Sema can fill it from currentClassType_). For the
            // Mono synthetic decl we must stamp a concrete annotation so the
            // CodeGen forward-declare pass produces a `ptr` parameter instead
            // of defaulting to i32 (see CodeGenGenerateForward.cpp — null
            // `p.type` currently falls through to `getInt32Ty`).
            auto ptrTy = std::make_unique<PointerType>();
            ptrTy->innerType = std::make_unique<NamedType>();
            ptrTy->innerType->name = "__self_opaque";
            pd.type = std::move(ptrTy);
            pd.isMutRef = true;
        }
        pd.defaultValue = nullptr; // not needed for mono
        fd->params.push_back(std::move(pd));
    }

    // Clone the return type without substitution (same reasoning as params).
    {
        auto* savedEnv = currentEnv_;
        currentEnv_ = nullptr;
        fd->returnType = cloneTypeAnnotation(method.returnType.get());
        currentEnv_ = savedEnv;
    }

    // Clone body without substitution — instantiate() will substitute.
    {
        auto* savedEnv = currentEnv_;
        currentEnv_ = nullptr;
        fd->body = cloneStmt(method.body.get());
        currentEnv_ = savedEnv;
    }

    const Decl* raw = fd.get();
    syntheticDecls_.push_back(std::move(fd));
    return raw;
}

std::string Monomorphize::buildMangledName(const Decl& templateDecl,
                                            const TypeEnv& env) const {
    // Mixed type+const path: when the decl declares any non-type parameter
    // (`const N: usize`), we must mangle the constant value at that slot
    // rather than emitting an Unknown VyxType. Otherwise two distinct
    // instantiations differing only in N collapse to the same key and
    // monomorphisation deduplicates them incorrectly.
    if (!templateDecl.genericConstParams.empty()) {
        std::vector<GenericArgSlot> slots;
        slots.reserve(templateDecl.genericParams.size());
        for (auto& param : templateDecl.genericParams) {
            GenericArgSlot s;
            auto cit = templateDecl.genericConstParams.find(param);
            if (cit != templateDecl.genericConstParams.end()) {
                s.isConst = true;
                s.constValue = env.lookupConst(param);
            } else {
                s.isConst = false;
                s.type = env.lookup(param);
                if (!s.type) {
                    auto unk = std::make_shared<VyxType>();
                    unk->kind = VyxTypeKind::Unknown;
                    unk->name = param;
                    s.type = unk;
                }
            }
            slots.push_back(std::move(s));
        }
        return mangleGenericMixed(templateDecl.name, slots);
    }

    std::vector<VyxTypePtr> argTypes;
    argTypes.reserve(templateDecl.genericParams.size());
    for (auto& param : templateDecl.genericParams) {
        auto t = env.lookup(param);
        if (t) {
            argTypes.push_back(t);
        } else {
            auto unk = std::make_shared<VyxType>();
            unk->kind = VyxTypeKind::Unknown;
            unk->name = param;
            argTypes.push_back(unk);
        }
    }
    return TemplateResolver::mangleTemplateName(templateDecl.name, argTypes);
}

// ============================================================
//  Public API
// ============================================================

std::string Monomorphize::request(const Decl* templateDecl, TypeEnv env) {
    if (!templateDecl) return "";

    std::string mangled = buildMangledName(*templateDecl, env);

    InstantiationRequest req;
    req.templateDecl = templateDecl;
    req.env = std::move(env);
    req.mangledName = mangled;

    enqueue(std::move(req));
    return mangled;
}

void Monomorphize::run(TranslationUnit& unit,
                        const std::vector<const Decl*>& roots) {
    unit_ = &unit;

    // R5 step 3: publish the caller-supplied lang-item registry to the
    // thread-local pointer that `vyxTypeToAnnotation` consults. Restored
    // on exit via RAII so nested / re-entrant runs (tooling, tests) do
    // not leak a dangling pointer from a previous driver.
    MonoLangItemsGuard langItemsGuard(langItems_);

    // Seed: scan each root decl for generic call sites
    for (auto* root : roots) {
        if (root)
            scanForRequests(*root, TypeEnv{});
    }

    // Seed: also scan all top-level GlobalVar declarations. These are not
    // included in the `roots` vector (which carries only Function decls), but
    // they may carry generic type annotations (`var g: Foo<i32>`) or generic
    // initialiser expressions that must be discovered before CodeGen emits the
    // global variable's type. Covers CodeGenDecl.cpp TODO(P1c-C) scan gap.
    for (auto& decl : unit.declarations) {
        if (decl && decl->kind == DeclKind::GlobalVar)
            scanForRequests(*decl, TypeEnv{});
    }

    // Seed: scan non-generic top-level Class/Struct declarations. Template
    // classes drive cascade via requestTemplateWithArgs, but a plain class
    // like `class Tree { children: Vec<Tree>; fn sum() { for c in self.children {...} } }`
    // references Vec<Tree> / Iterator<Tree> only inside its body — never
    // from a free function. Without this seed, Mono misses those and
    // CodeGen later errors with "generic struct type not found".
    // Imported (std/user) non-generic classes are scanned too so that e.g.
    // `HashSet<i32>` pulls in every generic type referenced by monomorph
    // bodies — idempotent when nothing new is discovered.
    for (auto& decl : unit.declarations) {
        if (!decl) continue;
        if ((decl->kind == DeclKind::Class || decl->kind == DeclKind::Struct) &&
            decl->genericParams.empty()) {
            scanForRequests(*decl, TypeEnv{});
        }
    }


    // Fixed-point worklist loop (LIFO for determinism with dependency ordering).
    // Error recovery: we do NOT abort on first failure. processOne records
    // failures into failedRequests_ (deduplicated) and continues. The only
    // early-stop is when we hit maxErrors_ distinct failures — at that point
    // we drain the current item but stop enqueuing new analysis.
    while (!worklist_.empty()) {
        InstantiationRequest req = std::move(worklist_.back());
        worklist_.pop_back();
        // Front 1: mirror the pop in the O(1) dedup set so re-enqueues of
        // the same mangle (post-processing) remain possible.
        pendingMangles_.erase(req.mangledName);

        if (seen_.count(req.mangledName))
            continue;

        // Hard cap: if we already collected maxErrors_ distinct failures,
        // skip further analysis to avoid an exponential error cascade. The
        // failures already recorded are still reported in the summary below.
        if (maxErrors_ > 0 && failedRequests_.size() >= maxErrors_)
            continue;

        // Front 1 budget: once the ceiling has been hit (MONO-004 already
        // emitted by enqueue), drain remaining items without processing
        // so the driver exits cleanly instead of spinning on whatever
        // recursive generic triggered the exhaustion.
        if (budgetExhausted_)
            continue;

        processOne(req, unit);
    }

    // Post-run summary: if any distinct failures were recorded, emit the
    // summary line even when individual errors were already printed above.
    // This gives the user a single "N failures, M sites" count at the end.
    if (!failedRequests_.empty()) {
        uint32_t distinct = static_cast<uint32_t>(failedRequests_.size());
        uint32_t sites    = totalFailureSiteCount();
        uint32_t collapsed = (sites > distinct) ? (sites - distinct) : 0;
        if (collapsed > 0) {
            diag_.noteCompact({},
                "template analysis completed with {} distinct failure(s) ({} call site(s) collapsed via dedup)",
                distinct, collapsed);
        } else {
            diag_.noteCompact({},
                "template analysis completed with {} distinct failure(s)",
                distinct);
        }
    }

    // Strict error model note: the `deferredRequests_` set tracks silent
    // early-returns from helper scan functions. Originally the plan was
    // to emit MONO-005 for any entry in that set that seen_/unit_->decls
    // did not subsume, but experiment shows too many false positives —
    // a legitimately-unused stdlib generic (Mutex<T>, any_equal<T>, etc.)
    // gets scanned at template-body time with no bindings and never
    // materialises because nobody calls it concretely. Distinguishing
    // "deferred but the real caller dropped it" from "deferred and no
    // caller exists" requires cross-referencing call sites with scan
    // entry points, which is not something the current worklist
    // preserves.
    //
    // We keep the tracking machinery wired up (the dedup is cheap and
    // harmless) so future refinements can hook in, but do not emit the
    // MONO-005 diagnostic here. The genuinely-bad cases (null receiver,
    // missing template, malformed typeArgs, no matching method) are
    // instead reported inline via MONO-006..011 in the helpers.
    (void)deferredRequests_;

    if (scheduler_ && scheduler_->totalRequests() > 0) {
        scheduler_->freeze();
        std::cout << "[S5 mono-sched] requests=" << scheduler_->totalRequests()
                  << " unique=" << scheduler_->uniqueRequests()
                  << " sema=" << scheduler_->semaRequests()
                  << " mono=" << scheduler_->monoRequests()
                  << " ready=" << scheduler_->readyItems()
                  << " classes=" << scheduler_->classRequests()
                  << " free_fns=" << scheduler_->freeFnRequests()
                  << " methods=" << scheduler_->methodRequests()
                  << " ready_classes=" << scheduler_->readyClassItems()
                  << " ready_free_fns=" << scheduler_->readyFreeFnItems()
                  << " ready_methods=" << scheduler_->readyMethodItems()
                  << "\n";
    }

    unit_ = nullptr;
}

// ============================================================
//  Worklist management
// ============================================================

void Monomorphize::enqueue(InstantiationRequest req) {
    if (seen_.count(req.mangledName))
        return;
    // Front 1: O(1) pending-set dedup (std::set for determinism). This
    // replaces the prior linear scan of worklist_ which made the driver
    // quadratic under self-hosting load.
    if (pendingMangles_.count(req.mangledName))
        return;

    // Budget short-circuit: once the hard ceiling is hit, stop accepting
    // new requests. We intentionally do NOT emit here (the emission happens
    // once at the moment we detect exhaustion in `run()`), and we do NOT
    // push onto the worklist, so the drain completes in bounded time.
    if (budgetExhausted_)
        return;

    // Defense-in-depth: cap the bracket-nesting depth of a mangled
    // instantiation name at 32.  Well-formed code never approaches this —
    // even industrial stress tests top out around depth 6.  A runaway
    // pattern like `Box<Box<Box<...<T>...>>>` that escapes the per-arg
    // unbound-generic filter (`isUnboundGeneric`) would otherwise fill
    // the worklist with ever-growing mangles; this cap stops it and emits
    // a clear diagnostic so the cause is easy to spot.
    //
    // NOTE: The underlying bug is already caught by the recursive
    // `isUnboundGeneric` check (which inspects `paramTypes` / `pointeeType`).
    // This second line of defence ensures any future regression that
    // re-introduces the cascade produces a clean error rather than a hang.
    {
        int maxDepth = 0;
        int curDepth = 0;
        for (char c : req.mangledName) {
            if (c == '<') {
                ++curDepth;
                if (curDepth > maxDepth) maxDepth = curDepth;
            } else if (c == '>') {
                --curDepth;
            }
        }
        constexpr int kMaxMonoNestingDepth = 32;
        if (maxDepth > kMaxMonoNestingDepth) {
            std::string msg = "template instantiation nesting depth exceeded"
                              " (depth > " +
                              std::to_string(kMaxMonoNestingDepth) + ")";
            if (recordFailure(req.mangledName, msg, req)) {
                SourceLocation loc = req.templateDecl
                    ? req.templateDecl->location : SourceLocation{};
                diag_.errorCoded("MONO-003", loc,
                    "template instantiation nesting depth exceeded for '{}'"
                    " (depth {} > {})",
                    req.mangledName, maxDepth, kMaxMonoNestingDepth);
                emitInstantiationChainNotes(diag_, loc,
                                            g_monoInstantiationStack);
            }
            // Mark as seen so the same name doesn't re-enter via another
            // call path and re-emit the error.
            seen_.insert(req.mangledName);
            return;
        }
    }

    // Bump the global instantiation counter on every successful add. When
    // we cross the ceiling, flip the exhausted flag so subsequent enqueue
    // calls short-circuit; the current request still goes onto the
    // worklist so the user-visible error can point at a concrete mangle.
    ++totalInstantiations_;
    if (totalInstantiations_ > kMaxTotalInstantiations && !budgetExhausted_) {
        budgetExhausted_ = true;
        std::string msg = "monomorphization budget exhausted (" +
                          std::to_string(totalInstantiations_) +
                          " instantiations)";
        if (recordFailure(req.mangledName, msg, req)) {
            SourceLocation loc = req.templateDecl
                ? req.templateDecl->location : SourceLocation{};
            diag_.errorCoded("MONO-004", loc,
                "monomorphization budget exhausted ({} instantiations); "
                "this usually indicates a recursive generic without a base case",
                totalInstantiations_);
            emitInstantiationChainNotes(diag_, loc,
                                        g_monoInstantiationStack);
        }
        // Do not enqueue the triggering request; the worklist drain below
        // will continue processing items already queued but will stop
        // adding new ones.
        return;
    }

    pendingMangles_.insert(req.mangledName);
    recordSchedulerRequest(scheduler_, req);
    worklist_.push_back(std::move(req));
}

// ============================================================
//  processOne
// ============================================================

// ── Error-recovery dedup helper ──────────────────────────────────────────
// Called whenever a failure is detected in processOne. Records the failure
// in failedRequests_ using key = mangledName + '\x1f' + errorMsg.
//   • First occurrence: insert with callSiteCount = 1, emit the diagnostic.
//   • Repeated occurrence: bump callSiteCount, suppress duplicate emission.
// Returns true when the error is NEW (should be emitted), false when deduped.
bool Monomorphize::recordFailure(const std::string& mangledName,
                                  const std::string& errorMsg,
                                  const InstantiationRequest& req)
{
    std::string key = mangledName + "\x1f" + errorMsg;
    auto it = failedRequests_.find(key);
    if (it != failedRequests_.end()) {
        it->second.callSiteCount++;
        return false; // already reported
    }
    InstantiationRequest recorded = req;
    recorded.failed       = true;
    recorded.errorMsg     = errorMsg;
    recorded.callSiteCount = 1;
    failedRequests_.emplace(key, std::move(recorded));
    return true; // new failure — caller should emit
}

// ── Strict error model helpers (Front 3) ────────────────────────────────
void Monomorphize::reportFailureFromHelper(const char* code,
                                           SourceLocation loc,
                                           const std::string& mangled,
                                           const std::string& msg) {
    // Build a synthetic InstantiationRequest keyed on the mangle so
    // recordFailure's dedup map absorbs duplicate call sites.
    InstantiationRequest synth;
    synth.mangledName = mangled;
    if (recordFailure(mangled, std::string(code) + ": " + msg, synth)) {
        diag_.errorCoded(code, loc, "{}", msg);
    }
}

void Monomorphize::markDeferred(const std::string& mangled, SourceLocation loc) {
    // Insert-if-absent. If the same mangle is later satisfied by another
    // scan path, seen_ will contain it and the post-run walk suppresses
    // the diagnostic. We keep the FIRST recorded location because it
    // tends to be the user's visible call site.
    if (mangled.empty()) return;
    deferredRequests_.emplace(mangled, loc);
}

// Forward decl — body lower in the file, after helpers it depends on.
static void monoVerifyConstPredicates(
    const Decl& templateDecl,
    const TypeEnv& env,
    DiagnosticsEngine& diag,
    const std::vector<InstantiationFrame>& stack);

// Forward decl — P3-Q reflection constraint verification (mono side).
static void monoVerifyReflectConstraints(
    const Decl& templateDecl,
    const TypeEnv& env,
    DiagnosticsEngine& diag,
    const std::vector<InstantiationFrame>& stack);

// Forward decl — P2D-008 typeof(v) constraint verification (mono side).
static void monoVerifyTypeofConstraints(
    const Decl& templateDecl,
    const TypeEnv& env,
    DiagnosticsEngine& diag,
    const std::vector<InstantiationFrame>& stack);

void Monomorphize::processOne(const InstantiationRequest& req,
                               TranslationUnit& unit) {
    if (seen_.count(req.mangledName))
        return;

    // Cycle detection: active_ already contains all ancestors of this
    // instantiation; g_monoInstantiationStack provides the readable chain.
    // Error recovery: record the cycle failure, then CONTINUE the worklist
    // (do not return early) so other unrelated instantiations are processed.
    if (active_.count(req.mangledName)) {
        if (req.templateDecl) {
            std::string msg = "monomorphization cycle detected for '" + req.mangledName + "'";
            if (recordFailure(req.mangledName, msg, req)) {
                diag_.errorCoded("MONO-001", req.templateDecl->location,
                    "monomorphization cycle detected for '{}'", req.mangledName);
                emitInstantiationChainNotes(diag_, req.templateDecl->location,
                                            g_monoInstantiationStack);
            }
        }
        // Mark as seen so we don't reprocess; but do NOT add to active_ again.
        seen_.insert(req.mangledName);
        return;
    }

    seen_.insert(req.mangledName);
    active_.insert(req.mangledName);

    // Push a frame for this instantiation so any nested error (inside
    // instantiate / scanForRequests / deeper processOne) sees the chain.
    {
        InstantiationFrame frame;
        frame.calleeName = req.mangledName;
        frame.site       = req.templateDecl ? req.templateDecl->location
                                            : SourceLocation{};
        for (auto& [param, type] : req.env.bindings)
            frame.bindings.emplace_back(param, type ? type->toString() : "?");
        MonoInstantiationGuard _guard(std::move(frame));

        // Full-template-specialization fast path: if the user already wrote a
        // hand-tuned `class Vec<i32> { ... }` / `fn foo<i32>(...)` the parser
        // collapsed it into a non-generic decl whose `name` is identical to the
        // mangled key we'd generate for an equivalent template instantiation.
        const Decl* userSpec = findDecl(req.mangledName);
        if (userSpec && userSpec->isFullSpecialization &&
            userSpec->genericParams.empty()) {
            scanForRequests(*userSpec, req.env);
            markSchedulerReady(scheduler_, req);
            active_.erase(req.mangledName);
            return;
        }

        // P2b: Partial-template-specialization fast path.
        // Sema already instantiated the concrete class/struct body and added it
        // to unit_.declarations under the full mangled name ("Pair<i32,i32>").
        // When req.templateDecl is a partial spec, skip Mono re-cloning — just
        // scan the Sema-generated concrete decl for nested instantiation requests.
        if (req.templateDecl && req.templateDecl->isPartialSpecialization) {
            const Decl* partialInst = findDecl(req.mangledName);
            if (partialInst) {
                scanForRequests(*partialInst, req.env);
                markSchedulerReady(scheduler_, req);
                active_.erase(req.mangledName);
                return;
            }
            // If Sema instance not found yet (shouldn't happen in normal flow),
            // fall through to standard instantiation from the partial-spec body.
        }

        if (req.templateDecl) {
            // P2D-004: verify const-integer where-clause predicates before
            // instantiation. Emits error[P2D-004] with the chain attached.
            monoVerifyConstPredicates(*req.templateDecl, req.env, diag_,
                                      g_monoInstantiationStack);
            // P2D-007: verify reflection constraints before instantiation.
            monoVerifyReflectConstraints(*req.templateDecl, req.env, diag_,
                                         g_monoInstantiationStack);
            // P2D-008: verify typeof(v) constraints before instantiation.
            monoVerifyTypeofConstraints(*req.templateDecl, req.env, diag_,
                                        g_monoInstantiationStack);

            auto concreteDecl = instantiate(*req.templateDecl, req.env, req.mangledName);
            if (concreteDecl) {
                scanForRequests(*concreteDecl, req.env);
                markSchedulerReady(scheduler_, req);
                unit.declarations.push_back(std::move(concreteDecl));
            } else {
                // instantiate() returned nullptr (unsupported decl kind). The
                // error was already emitted; record it for the dedup summary.
                std::string msg = "instantiation of '" + req.mangledName + "' failed (unsupported template kind)";
                recordFailure(req.mangledName, msg, req);
            }
        } else {
            // No template decl: the request was produced by scanForRequests
            // against an enqueued reference that couldn't be resolved.
            std::string msg = "template '" + req.mangledName + "' not found";
            if (recordFailure(req.mangledName, msg, req)) {
                SourceLocation loc{};
                diag_.errorCoded("MONO-002", loc,
                    "template '{}' not found during monomorphization",
                    req.mangledName);
            }
        }
    } // guard pops here

    active_.erase(req.mangledName);
}

// ============================================================
//  scanForRequests — static helpers then member dispatch
// ============================================================

// Forward declarations for mutual recursion. `activeGenerics` lists the
// generic-parameter names in lexical scope at the call site (class +
// method + function genericParams, in any combination that applies). It
// is forwarded to `TemplateResolver::resolveTypeAnnotation` so names
// that refer to in-scope template parameters resolve to `Generic`
// VyxType nodes (rather than being misclassified as user `Class` types
// just because they happen to be a single capital letter).
static void scanStmtForRequests(const Stmt* stmt, const TypeEnv& env,
                                  const std::vector<std::string>& activeGenerics,
                                  Monomorphize& mono);
static void scanExprForRequests(const Expr* expr, const TypeEnv& env,
                                  const std::vector<std::string>& activeGenerics,
                                  Monomorphize& mono);
static void scanTypeForRequests(const TypeAnnotation* ann, const TypeEnv& env,
                                  const std::vector<std::string>& activeGenerics,
                                  Monomorphize& mono);
static void scanMangledTypeNameForRequests(const std::string& mangledName,
                                            const TypeEnv& env,
                                            const std::vector<std::string>& activeGenerics,
                                            Monomorphize& mono);

// Returns true when `t` cannot drive a real monomorph request because it
// still references a template parameter that the current substitution
// `env` does not bind. Replaces the old single-capital-letter heuristic
// with a precise check: we trust `TemplateResolver::resolveTypeAnnotation`
// (configured with the lexically-active generic-param names) to mark
// template references as `Generic` kind, then ask `env` whether each
// such name is bound to a concrete type.
//
// Recursive: a compound type whose *outer* kind is Class / BoxPtr / etc.
// (e.g. `Box<T>`) must still be rejected when an inner paramType / pointee
// is an unbound Generic.  Without this recursive check,
// `requestTemplateWithArgs` would bind the template's formal parameter to
// a type still containing `T`, and the resulting mangled name
// (e.g. `Box<Box<T>>`) would enter the worklist.  Processing that request
// then scans the newly instantiated body, re-references `Box<T>` (still
// with `T` unbound under the new env `{T → Box<T>}`), and feeds
// `Box<Box<Box<T>>>`, `Box<Box<Box<Box<T>>>>`, … ad infinitum.  See the
// `fn deref2<T>(b: Box<Box<T>>) -> T` regression: deref2's own
// template-level scan walks the parameter `Box<Box<T>>`, recurses into
// `Box<T>`, and used to enqueue `Box<Box<T>>` as a "concrete" request
// even though `T` was unbound — each subsequent Box instantiation
// amplified the nesting by one level.
static bool isUnboundGeneric(const VyxTypePtr& t, const TypeEnv& env) {
    if (!t) return true;
    if (t->kind == VyxTypeKind::Unknown) return true;
    if (t->kind == VyxTypeKind::Generic) {
        // Bound by the active env? Then `substituteType` would have
        // replaced it; if we still see Generic here it is genuinely
        // unbound at this call site.
        return !env.has(t->name);
    }
    if (t->pointeeType && isUnboundGeneric(t->pointeeType, env))
        return true;
    for (auto& p : t->paramTypes) {
        if (p && isUnboundGeneric(p, env))
            return true;
    }
    return false;
}

// Resolve a list of TypeAnnotation type-arguments through the active
// substitution env, then enqueue an instantiation request for the named
// template. Shared by static-method calls (`Type::<Args>.method(...)`),
// turbofish identifier calls (`fn::<T>(...)`), and field-type recursion
// (`field: Dict<K,V>` discovered in a substituted class body).
// Evaluate a generic call-site argument expression to an integer. Supports
// integer literals, simple unary negation, and references to other const
// generic parameters bound in the active env. Returns true on success.
static bool evalConstGenericArg(const Expr* e, const TypeEnv& env, int64_t& out) {
    if (!e) return false;
    switch (e->kind) {
        case ExprKind::IntLiteral:
            out = e->as<IntLiteralExpr>()->value;
            return true;
        case ExprKind::BoolLiteral:
            out = e->as<BoolLiteralExpr>()->value ? 1 : 0;
            return true;
        case ExprKind::CharLiteral: {
            const auto& s = e->as<CharLiteralExpr>()->value;
            if (s.empty()) return false;
            out = static_cast<int64_t>(static_cast<unsigned char>(s[0]));
            return true;
        }
        case ExprKind::Identifier: {
            const auto& nm = e->as<IdentifierExpr>()->name;
            if (env.hasConst(nm)) { out = env.lookupConst(nm); return true; }
            return false;
        }
        case ExprKind::UnaryOp: {
            auto* u = e->as<UnaryOpExpr>();
            int64_t v = 0;
            if (!evalConstGenericArg(u->operand.get(), env, v)) return false;
            switch (u->op) {
                case UnaryOp::Neg: out = -v; return true;
                case UnaryOp::Not: out = !v; return true;
                case UnaryOp::BitNot: out = ~v; return true;
                default: return false;
            }
        }
        case ExprKind::BinaryOp: {
            auto* b = e->as<BinaryOpExpr>();
            int64_t l = 0, r = 0;
            if (!evalConstGenericArg(b->lhs.get(), env, l)) return false;
            if (!evalConstGenericArg(b->rhs.get(), env, r)) return false;
            switch (b->op) {
                case BinaryOp::Add: out = l + r; return true;
                case BinaryOp::Sub: out = l - r; return true;
                case BinaryOp::Mul: out = l * r; return true;
                case BinaryOp::Div: if (r == 0) return false; out = l / r; return true;
                case BinaryOp::Mod: if (r == 0) return false; out = l % r; return true;
                case BinaryOp::BitAnd: out = l & r; return true;
                case BinaryOp::BitOr:  out = l | r; return true;
                case BinaryOp::BitXor: out = l ^ r; return true;
                case BinaryOp::Shl: out = l << r; return true;
                case BinaryOp::Shr: out = l >> r; return true;
                default: return false;
            }
        }
        default: return false;
    }
}

// ── P2D-004: const-predicate evaluator for Mono ───────────────────────────
// Evaluates a const expression tree against the TypeEnv's constBindings.
// Handles IntLiteral, Identifier (looks up env.constBindings), and
// BinaryOp (Add, Sub, Mul, Div, Mod). Returns true on success.
static bool monoEvalConstPredExpr(const Expr* expr, const TypeEnv& env, int64_t& out)
{
    if (!expr) return false;
    switch (expr->kind) {
        case ExprKind::IntLiteral:
            out = expr->as<IntLiteralExpr>()->value;
            return true;
        case ExprKind::Identifier: {
            const auto& nm = expr->as<IdentifierExpr>()->name;
            if (env.hasConst(nm)) { out = env.lookupConst(nm); return true; }
            return false;
        }
        case ExprKind::BinaryOp: {
            const auto* bin = expr->as<BinaryOpExpr>();
            int64_t lVal = 0, rVal = 0;
            if (!monoEvalConstPredExpr(bin->lhs.get(), env, lVal)) return false;
            if (!monoEvalConstPredExpr(bin->rhs.get(), env, rVal)) return false;
            switch (bin->op) {
                case BinaryOp::Add: out = lVal + rVal; return true;
                case BinaryOp::Sub: out = lVal - rVal; return true;
                case BinaryOp::Mul: out = lVal * rVal; return true;
                case BinaryOp::Div: if (rVal == 0) return false; out = lVal / rVal; return true;
                case BinaryOp::Mod: if (rVal == 0) return false; out = lVal % rVal; return true;
                default: return false;
            }
        }
        default: return false;
    }
}

// Render a const predicate expr to string (for diagnostics).
static std::string monoConstExprStr(const Expr* e)
{
    if (!e) return "?";
    switch (e->kind) {
        case ExprKind::IntLiteral:  return std::to_string(e->as<IntLiteralExpr>()->value);
        case ExprKind::Identifier:  return e->as<IdentifierExpr>()->name;
        case ExprKind::BinaryOp: {
            const auto* b = e->as<BinaryOpExpr>();
            const char* op = "?";
            switch (b->op) {
                case BinaryOp::Add: op="+"; break; case BinaryOp::Sub: op="-"; break;
                case BinaryOp::Mul: op="*"; break; case BinaryOp::Div: op="/"; break;
                case BinaryOp::Mod: op="%"; break; case BinaryOp::Eq:  op="==";break;
                case BinaryOp::Neq: op="!=";break; case BinaryOp::Lt:  op="<"; break;
                case BinaryOp::Lte: op="<=";break; case BinaryOp::Gt:  op=">";  break;
                case BinaryOp::Gte: op=">=";break; default: break;
            }
            return monoConstExprStr(b->lhs.get()) + " " + op + " " + monoConstExprStr(b->rhs.get());
        }
        default: return "?";
    }
}

// Verify all ConstPredicates in `templateDecl` against `env.constBindings`.
// Emits error[P2D-004] for each failing predicate.
static void monoVerifyConstPredicates(
        const Decl& templateDecl,
        const TypeEnv& env,
        DiagnosticsEngine& diag,
        const std::vector<InstantiationFrame>& stack)
{
    if (templateDecl.constPredicates.empty()) return;

    for (const auto& pred : templateDecl.constPredicates) {
        int64_t lVal = 0, rVal = 0;
        bool lOk = monoEvalConstPredExpr(pred.lhs.get(), env, lVal);
        bool rOk = monoEvalConstPredExpr(pred.rhs.get(), env, rVal);

        if (!lOk || !rOk) continue; // unbound — skip (not yet substituted)

        bool sat = false;
        switch (pred.op) {
            case BinaryOp::Lt:  sat = lVal <  rVal; break;
            case BinaryOp::Lte: sat = lVal <= rVal; break;
            case BinaryOp::Gt:  sat = lVal >  rVal; break;
            case BinaryOp::Gte: sat = lVal >= rVal; break;
            case BinaryOp::Eq:  sat = lVal == rVal; break;
            case BinaryOp::Neq: sat = lVal != rVal; break;
            default: continue;
        }

        if (!sat) {
            const char* opStr = "?";
            switch (pred.op) {
                case BinaryOp::Lt:  opStr = "<";  break; case BinaryOp::Lte: opStr = "<="; break;
                case BinaryOp::Gt:  opStr = ">";  break; case BinaryOp::Gte: opStr = ">="; break;
                case BinaryOp::Eq:  opStr = "=="; break; case BinaryOp::Neq: opStr = "!="; break;
                default: break;
            }
            std::string predStr = monoConstExprStr(pred.lhs.get())
                                + " " + opStr + " "
                                + monoConstExprStr(pred.rhs.get());
            std::string bindSummary;
            for (auto& [k, v] : env.constBindings) {
                if (!bindSummary.empty()) bindSummary += ", ";
                bindSummary += k + " = " + std::to_string(v);
            }
            if (bindSummary.empty())
                bindSummary = std::to_string(lVal) + " vs " + std::to_string(rVal);

            SourceLocation loc = templateDecl.location;
            diag.errorCoded("P2D-004", loc,
                "const predicate `{}` not satisfied ({})", predStr, bindSummary);
            emitInstantiationChainNotes(diag, loc, stack);
            diag.helpCompact(loc,
                "ensure the template arguments satisfy the where-clause constraint `{}`",
                predStr);
        }
    }
}

// P2D-007: verify reflection constraints at Mono time.
// This mirrors the Sema-side check in SemaGeneric.cpp::verifyConstraints so that
// constraints on function templates that Sema visited only at the generic level
// (not yet instantiated) are also caught here when concrete bindings are known.
static void monoVerifyReflectConstraints(
        const Decl& templateDecl,
        const TypeEnv& env,
        DiagnosticsEngine& diag,
        const std::vector<InstantiationFrame>& stack)
{
    if (templateDecl.reflectConstraints.empty()) return;

    for (const auto& rc : templateDecl.reflectConstraints) {
        VyxTypePtr concreteType = env.lookup(rc.typeParam);
        if (!concreteType) continue; // unbound — skip

        std::string actual;
        if (rc.member == "kind") {
            switch (concreteType->kind) {
                case VyxTypeKind::Struct:    actual = "struct";    break;
                case VyxTypeKind::Class:     actual = "class";     break;
                case VyxTypeKind::Interface: actual = "interface"; break;
                case VyxTypeKind::ErrorType: actual = "error";     break;
                default:                     actual = "primitive"; break;
            }
        } else if (rc.member == "name") {
            actual = concreteType->toString();
        } else {
            continue; // fields/methods in where-clause not supported
        }

        bool matches = (actual == rc.expected);
        bool pass    = rc.negate ? !matches : matches;
        if (!pass) {
            SourceLocation loc = templateDecl.location;
            diag.errorCoded("P2D-007", loc,
                "reflection constraint '{}::{} {} \"{}\"' not satisfied ({} = '{}', {} = '{}')",
                rc.typeParam, rc.member, rc.negate ? "!=" : "==", rc.expected,
                rc.typeParam, concreteType->toString(),
                rc.member, actual);
            diag.noteCompact(loc,
                "type '{}' has {}='{}', but the where-clause requires {}='{}'",
                concreteType->toString(), rc.member, actual, rc.member, rc.expected);
            diag.helpCompact(loc,
                "only types whose '{}' is '{}' may be used with '{}'",
                rc.member, rc.expected, templateDecl.name);
            emitInstantiationChainNotes(diag, loc, stack);
        }
    }
}

// P2D-008: verify typeof(v) == "typename" constraints at Mono time.
// Mirrors the Sema-side check in SemaGeneric.cpp::verifyConstraints.
static void monoVerifyTypeofConstraints(
        const Decl& templateDecl,
        const TypeEnv& env,
        DiagnosticsEngine& diag,
        const std::vector<InstantiationFrame>& stack)
{
    if (templateDecl.typeofConstraints.empty()) return;

    for (const auto& tc : templateDecl.typeofConstraints) {
        VyxTypePtr concreteType;

        // Try to find value param -> type-param chain (FunctionDecl only).
        if (templateDecl.kind == DeclKind::Function) {
            auto* fn = templateDecl.as<FunctionDecl>();
            for (auto& p : fn->params) {
                if (p.name == tc.paramName && p.type) {
                    concreteType = env.lookup(p.type->name);
                    break;
                }
            }
        }
        // Fallback: paramName might directly be a type-parameter name.
        if (!concreteType)
            concreteType = env.lookup(tc.paramName);

        if (!concreteType) continue; // unbound — skip

        std::string actual = concreteType->toString();
        bool matches = (actual == tc.expected);
        bool pass    = tc.negate ? !matches : matches;
        if (!pass) {
            SourceLocation loc = templateDecl.location;
            diag.errorCoded("P2D-008", loc,
                "typeof constraint 'typeof({}) {}\"{}\"' not satisfied: "
                "argument has type '{}', expected '{}'",
                tc.paramName,
                tc.negate ? "!=" : "==",
                tc.expected,
                actual, tc.expected);
            diag.noteCompact(loc,
                "parameter '{}' was bound to type '{}', "
                "but the where-clause requires typeof({}) {}\"{}\"",
                tc.paramName, actual,
                tc.paramName,
                tc.negate ? "!=" : "==",
                tc.expected);
            diag.helpCompact(loc,
                "only arguments of type '{}' satisfy 'typeof({}) {}\"{}\"' on '{}'",
                tc.expected,
                tc.paramName,
                tc.negate ? "!=" : "==",
                tc.expected,
                templateDecl.name);
            emitInstantiationChainNotes(diag, loc, stack);
        }
    }
}

static void requestTemplateWithArgs(const std::string& name,
                                      const std::vector<TypePtr>& callTypeArgs,
                                      const std::vector<ExprPtr>* callArgExprs,
                                      const TypeEnv& env,
                                      const std::vector<std::string>& activeGenerics,
                                      Monomorphize& mono) {
    const Decl* td = mono.findDecl(name);
    if (!td || td->genericParams.empty()) return;
    if (callTypeArgs.empty()) return;

    // Function / Class / Struct are the canonical monomorphisable kinds.
    // User-defined generic ErrorDef (e.g. `enum BinTree<T> { Leaf, Node(T,
    // Box<BinTree<T>>) }`) are also monomorphisable — Mono produces a fully
    // concretised ErrorDefDecl whose variant payload types have T → concrete
    // applied, so CodeGen's generateRegisterErrorEnums sees the mangled
    // entry and can register `structTypes_["BinTree<i32>"]` +
    // `errorEnumValues_["BinTree<i32>.Node"]`. The lang_item Option/Result
    // ErrorDefs are intentionally skipped: they share the runtime
    // `__Result` 16-byte layout via getOrCreateResultType() + enumAliases,
    // and a parallel mangled struct would conflict with that aliasing.
    if (td->kind != DeclKind::Function &&
        td->kind != DeclKind::Class &&
        td->kind != DeclKind::Struct &&
        td->kind != DeclKind::ErrorDef) {
        return;
    }
    if (td->kind == DeclKind::ErrorDef) {
        for (auto& [an, _] : td->attributes) {
            if (an == "lang_item") return;  // Option/Result et al. — keep alias path.
        }
    }

    // P2b: Partial template specialization resolution in Mono.
    // Before binding args against the primary template, check if any partial
    // specialization matches. If so, use that decl as the template (it has
    // its own body that overrides the primary).  Sema resolved the concrete
    // type args already, so we resolve them here through `env` and then do a
    // simple name-equality match against the pattern (adequate for the Mono
    // pass which only needs to enqueue the right decl body for code-gen).
    //
    // Pattern-match: for each partial spec candidate, resolve the call-site
    // type args and compare against the pattern using the same heuristic as
    // Sema's selectPartialSpec.  The specificity ranking follows the same
    // concrete-token count rule.
    if (!callTypeArgs.empty() &&
        (td->kind == DeclKind::Class || td->kind == DeclKind::Struct)) {
        // Resolve the call-site type args to VyxType
        std::vector<VyxTypePtr> resolvedArgs;
        resolvedArgs.reserve(callTypeArgs.size());
        for (auto& ta : callTypeArgs) {
            if (!ta) { resolvedArgs.push_back(nullptr); continue; }
            VyxTypePtr resolved = TemplateResolver::resolveTypeAnnotation(*ta, &activeGenerics);
            resolvedArgs.push_back(::vyx::substituteType(resolved, env));
        }

        // Enumerate partial spec candidates
        const TranslationUnit* unit = mono.getUnit();
        if (unit) {
            struct PsCandidate {
                const Decl* decl;
                int score;
                std::map<std::string, VyxTypePtr> bindings;
            };
            std::vector<PsCandidate> psCandidates;

            auto matchSlot = [&](const TypeAnnotation* pat,
                                  const VyxTypePtr& concrete,
                                  const std::set<std::string>& fresh,
                                  std::map<std::string, VyxTypePtr>& binds) -> bool {
                if (!pat || !concrete) return false;
                if (pat->kind == TypeAnnotationKind::Named) {
                    if (fresh.count(pat->name)) {
                        auto it = binds.find(pat->name);
                        if (it == binds.end()) { binds[pat->name] = concrete; return true; }
                        return it->second->toString() == concrete->toString();
                    }
                    return concrete->toString() == pat->name ||
                           concrete->name == pat->name;
                }
                if (pat->kind == TypeAnnotationKind::Generic) {
                    auto* gt = pat->as<GenericType>();
                    auto lt = concrete->name.find('<');
                    std::string conBase = (lt != std::string::npos)
                        ? concrete->name.substr(0, lt) : concrete->name;
                    if (conBase != gt->name) return false;
                    if (lt == std::string::npos) return gt->typeArgs.empty();
                    std::string inner = concrete->name.substr(lt + 1, concrete->name.size() - lt - 2);
                    std::vector<std::string> subs;
                    { int d = 0; std::string cur;
                      for (char c : inner) {
                        if (c == '<') { ++d; cur += c; }
                        else if (c == '>') { --d; cur += c; }
                        else if (c == ',' && d == 0) { subs.push_back(cur); cur.clear(); }
                        else cur += c;
                      }
                      subs.push_back(cur); }
                    if (subs.size() != gt->typeArgs.size()) return false;
                    for (size_t si = 0; si < gt->typeArgs.size(); ++si) {
                        auto* subPat = gt->typeArgs[si].get();
                        if (!subPat) return false;
                        if (subPat->kind == TypeAnnotationKind::Named && fresh.count(subPat->name)) {
                            auto synth = std::make_shared<VyxType>();
                            synth->name = subs[si];
                            auto it = binds.find(subPat->name);
                            if (it == binds.end()) binds[subPat->name] = synth;
                            else if (it->second->toString() != subs[si]) return false;
                        } else {
                            if (subPat->name != subs[si]) return false;
                        }
                    }
                    return true;
                }
                return false;
            };

            for (auto& d : unit->declarations) {
                if (!d || !d->isPartialSpecialization) continue;
                if (d->partialSpecBaseName != name) continue;
                if (d->specializationPattern.size() != resolvedArgs.size()) continue;
                std::set<std::string> fresh(
                    d->genericParams.begin(), d->genericParams.end());
                std::map<std::string, VyxTypePtr> binds;
                bool ok = true;
                for (size_t i = 0; i < resolvedArgs.size(); ++i) {
                    if (!matchSlot(d->specializationPattern[i].get(),
                                   resolvedArgs[i], fresh, binds)) { ok = false; break; }
                }
                if (!ok) continue;
                bool allBound = true;
                for (auto& fp : d->genericParams)
                    if (!binds.count(fp)) { allBound = false; break; }
                if (!allBound) continue;
                // Compute specificity
                std::function<int(const TypeAnnotation*, const std::set<std::string>&)>
                    score = [&](const TypeAnnotation* p, const std::set<std::string>& f) -> int {
                    if (!p) return 0;
                    if (p->kind == TypeAnnotationKind::Named) return f.count(p->name) ? 0 : 1;
                    if (p->kind == TypeAnnotationKind::Generic) {
                        auto* g = p->as<GenericType>(); int s = 1;
                        for (auto& a : g->typeArgs) s += score(a.get(), f);
                        return s;
                    }
                    return 1;
                };
                int s = 0;
                for (auto& ps : d->specializationPattern) s += score(ps.get(), fresh);
                psCandidates.push_back({d.get(), s, std::move(binds)});
            }
            if (!psCandidates.empty()) {
                int bestS = 0;
                for (auto& pc : psCandidates) if (pc.score > bestS) bestS = pc.score;
                std::vector<const PsCandidate*> best;
                for (auto& pc : psCandidates) if (pc.score == bestS) best.push_back(&pc);
                if (best.size() == 1) {
                    // Use partial spec decl as templateDecl
                    td = best[0]->decl;
                    // The partial spec has its own genericParams (fresh params).
                    // We need to build a TypeEnv mapping fresh params → concrete types.
                    // Fall through with td = partial spec; the generic params size
                    // check below will use td->genericParams.size().
                    // Set a sentinel so we skip the arity check against primary.
                    // (We re-derive the env below.)
                    const Decl* psDecl = best[0]->decl;
                    const auto& psBindings = best[0]->bindings;
                    // Build mangled name from the ORIGINAL resolved args (use-site arity)
                    std::string psMangled = name + "<";
                    for (size_t i = 0; i < resolvedArgs.size(); ++i) {
                        if (i > 0) psMangled += ",";
                        psMangled += resolvedArgs[i] ? resolvedArgs[i]->toString() : "?";
                    }
                    psMangled += ">";
                    // Build env for the partial spec's own params (the fresh params)
                    TypeEnv psEnv;
                    for (auto& fp : psDecl->genericParams) {
                        auto it = psBindings.find(fp);
                        if (it != psBindings.end() && it->second)
                            psEnv.bind(fp, it->second);
                    }
                    // Enqueue the partial spec directly (bypass the rest of this function)
                    InstantiationRequest req;
                    req.templateDecl = psDecl;
                    req.mangledName = psMangled;
                    req.env = std::move(psEnv);
                    mono.enqueue(std::move(req));
                    return; // done
                }
                // Multiple equally-specific: ambiguity was already reported by Sema.
                // Fall through to primary template as a safe fallback.
            }
        }
    }

    if (td->genericParams.size() != callTypeArgs.size()) return;

    // Walk the type-arg AST through the active substitution env so we get
    // canonical, fully-concrete VyxTypes (e.g. `T` -> `i64` when scanning
    // inside a partially-substituted body, `Vec<T>` -> `Vec<i64>`, etc.).
    // `activeGenerics` is the lexical scope of generic-parameter names —
    // not the env's bound subset — so names that refer to in-scope
    // template parameters resolve to `Generic` even when env has not
    // bound them yet (e.g. method-level generics inside a class body).
    TypeEnv callEnv;
    // Synthesize a "best-effort" mangle stub for deferred-request tracking.
    // If a param can't be bound here (unbound generic, missing arg), we
    // still want some key to record. Not a true mangle — just a
    // stable-ish identifier for the post-run MONO-005 audit.
    std::string deferStub = name + "<?>";
    for (size_t i = 0; i < td->genericParams.size(); ++i) {
        const std::string& pname = td->genericParams[i];
        bool isConstParam =
            td->genericConstParams.find(pname) != td->genericConstParams.end();
        if (isConstParam) {
            // Non-type parameter slot: the parser parks the value expression
            // in `argExprs[i]` (with `typeArgs[i] == nullptr`). Evaluate it
            // through the active env so nested const-generic references
            // ("Array<T, N>" inside a class with `const N: usize`) resolve.
            int64_t cv = 0;
            const Expr* ce = nullptr;
            if (callArgExprs && i < callArgExprs->size())
                ce = (*callArgExprs)[i].get();
            if (!ce) {
                // Fallback: legacy callers without argExprs (unlikely once
                // parser is updated) — defer. Record for MONO-005 audit.
                mono.markDeferred(deferStub, td->location);
                return;
            }
            if (!evalConstGenericArg(ce, env, cv)) {
                mono.markDeferred(deferStub, ce->location);
                return;
            }
            callEnv.bindConst(pname, cv);
            continue;
        }
        if (!callTypeArgs[i]) {
            mono.markDeferred(deferStub, td->location);
            return;
        }
        VyxTypePtr resolved = TemplateResolver::resolveTypeAnnotation(
            *callTypeArgs[i], &activeGenerics);
        VyxTypePtr concrete = ::vyx::substituteType(resolved, env);
        if (isUnboundGeneric(concrete, env)) {
            // Legit deferral: will be re-scanned by an outer instantiation
            // that binds this parameter. Tracked for the post-drain audit.
            mono.markDeferred(deferStub, td->location);
            return;
        }
        callEnv.bind(pname, concrete);
    }
    // Inherit any in-scope const bindings so nested instantiations
    // (e.g. method-level `Array<T, N>` inside `Container<const N: usize>`)
    // can still mangle their N value when it isn't passed explicitly at the
    // call site.
    for (auto& [k, v] : env.constBindings) {
        if (!callEnv.hasConst(k)) callEnv.bindConst(k, v);
    }
    mono.request(td, std::move(callEnv));
}

// Parse a mangled type-name string like `"Foo<i32,Bar<string>>"` into a
// flat list of top-level type-argument name strings (e.g. {"i32",
// "Bar<string>"}). Handles arbitrarily nested angle brackets by tracking
// depth. Used by `scanMangledTypeNameForRequests` below, which is called
// from `scanExprForRequests` for `ExprKind::StructInit` nodes whose parser-
// produced `structName` is already the mangled form (e.g. `"Pair<i32,string>"`).
static std::vector<std::string> splitMangledTypeArgs(const std::string& mangled,
                                                      std::string& baseName) {
    auto lt = mangled.find('<');
    if (lt == std::string::npos) {
        baseName = mangled;
        return {};
    }
    baseName = mangled.substr(0, lt);
    // Strip outermost `<` ... `>`
    auto inner = mangled.substr(lt + 1, mangled.size() - lt - 2);
    std::vector<std::string> args;
    int depth = 0;
    std::string cur;
    for (char c : inner) {
        if (c == '<') { ++depth; cur += c; }
        else if (c == '>') { --depth; cur += c; }
        else if (c == ',' && depth == 0) {
            if (!cur.empty()) args.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) args.push_back(cur);
    return args;
}

// Build a minimal `TypeAnnotation` tree from a bare string token produced
// by `splitMangledTypeArgs`. Simple primitive / nominal names become
// `NamedType`; anything containing `<` becomes a one-level `GenericType`
// (recursing via the same splitter so nested generics are handled).
static TypePtr buildTypeAnnotationFromString(const std::string& s) {
    std::string base;
    auto args = splitMangledTypeArgs(s, base);
    if (args.empty()) {
        auto n = std::make_unique<NamedType>();
        n->name = s;
        return n;
    }
    auto g = std::make_unique<GenericType>();
    g->name = base;
    for (auto& a : args)
        g->typeArgs.push_back(buildTypeAnnotationFromString(a));
    return g;
}

// Enqueue an instantiation request derived from a mangled type-name string
// (e.g. `"Pair<i32,string>"`). This is needed for `ExprKind::StructInit`
// nodes where the parser bakes the type args into `structName` instead of
// preserving them as a structured `TypeAnnotation` on the node.
//
// Contract:
//  - If `mangledName` contains no `<`, nothing is enqueued (no generic args).
//  - The base name is looked up in the translation unit; if no generic decl
//    is found, returns immediately.
//  - Each top-level type-arg token is converted to a minimal `TypeAnnotation`
//    (via `buildTypeAnnotationFromString`) and forwarded to
//    `requestTemplateWithArgs`, which resolves each arg through `env` and
//    `activeGenerics` before building the final `TypeEnv` for the request.
//  - Recurses into the synthesised type-arg annotations so nested generics
//    like `Box<Vec<i32>>` are also enqueued.
static void scanMangledTypeNameForRequests(const std::string& mangledName,
                                            const TypeEnv& env,
                                            const std::vector<std::string>& activeGenerics,
                                            Monomorphize& mono) {
    std::string base;
    auto argStrs = splitMangledTypeArgs(mangledName, base);
    if (argStrs.empty()) return; // not a generic name
    std::vector<TypePtr> typeArgs;
    typeArgs.reserve(argStrs.size());
    for (auto& s : argStrs)
        typeArgs.push_back(buildTypeAnnotationFromString(s));
    // Recurse into each arg first (handles e.g. `Dict<K, Vec<i32>>`).
    for (auto& ta : typeArgs)
        scanTypeForRequests(ta.get(), env, activeGenerics, mono);
    // Forward the structured type-arg list to the canonical enqueue path.
    requestTemplateWithArgs(base, typeArgs, nullptr, env, activeGenerics, mono);
}

// Walk a TypeAnnotation looking for nested `GenericType` (Dict<K,V>,
// Vec<T>, user-defined generic classes, ...). Each one resolved here is
// enqueued as an instantiation request — this is what bridges the
// "second-order" case where a generic class's substituted body still
// references *other* generic templates that no user call site mentions
// directly (e.g. `class HashSet<T> { inner: Dict<T, bool>; }`).
static void scanTypeForRequests(const TypeAnnotation* ann, const TypeEnv& env,
                                  const std::vector<std::string>& activeGenerics,
                                  Monomorphize& mono) {
    if (!ann) return;
    switch (ann->kind) {
        case TypeAnnotationKind::Generic: {
            auto* gt = ann->as<GenericType>();
            // Recurse into nested type arguments first so deeply-nested
            // forms like `Dict<i64, Vec<i64>>` enqueue both Dict<...> and
            // Vec<i64>.
            for (auto& a : gt->typeArgs)
                scanTypeForRequests(a.get(), env, activeGenerics, mono);
            requestTemplateWithArgs(ann->name, gt->typeArgs, &gt->argExprs,
                                    env, activeGenerics, mono);
            break;
        }
        case TypeAnnotationKind::Pointer:
            scanTypeForRequests(ann->as<PointerType>()->innerType.get(), env, activeGenerics, mono);
            break;
        case TypeAnnotationKind::Reference:
            scanTypeForRequests(ann->as<ReferenceType>()->innerType.get(), env, activeGenerics, mono);
            break;
        case TypeAnnotationKind::Array:
            scanTypeForRequests(ann->as<ArrayType>()->elementType.get(), env, activeGenerics, mono);
            break;
        case TypeAnnotationKind::Tuple:
            for (auto& e : ann->as<TupleType>()->elements)
                scanTypeForRequests(e.get(), env, activeGenerics, mono);
            break;
        case TypeAnnotationKind::Function: {
            auto* ft = ann->as<FunctionType>();
            for (auto& p : ft->paramTypes)
                scanTypeForRequests(p.get(), env, activeGenerics, mono);
            scanTypeForRequests(ft->returnType.get(), env, activeGenerics, mono);
            break;
        }
        case TypeAnnotationKind::Union:
            for (auto& m : ann->as<UnionType>()->members)
                scanTypeForRequests(m.get(), env, activeGenerics, mono);
            break;
        case TypeAnnotationKind::Named:
            // NamedType annotations whose name already contains '<' are
            // mangled class names produced by the TemplateResolver round-trip
            // for user-defined generic classes (e.g. "Iterator<Vec<i32>>")
            // when the Fix-2 convertTypeToAnnotation decomposition path was
            // not reached (e.g. in older Sema-produced symbol-table entries or
            // var-decl annotations that weren't substituted via Mono paths).
            // Forward these to scanMangledTypeNameForRequests so Mono enqueues
            // the template instantiation instead of silently ignoring it.
            if (ann->name.find('<') != std::string::npos)
                scanMangledTypeNameForRequests(ann->name, env, activeGenerics, mono);
            break;
        default:
            break;
    }
}

// ============================================================
//  requestMethodInstantiation — instance/static method turbofish
// ============================================================
//
// Called when we see `recv.method::<U1,U2>(args)`.
//
// Algorithm:
//   1. classMono   = recvType->name (e.g. "Iterator<i64>") — already fully mangled
//                  by Sema and attached to `ma->object->inferredType`.
//   2. baseClassName = strip the "<...>" suffix → "Iterator"
//   3. Look up the ClassDecl template named baseClassName in the TU.
//   4. Find the MethodDecl whose `.name == ma->member` AND whose
//      `.genericParams.size() == ma->callTypeArgs.size()`.
//   5. Resolve ma->callTypeArgs through the active substitution env to get
//      concrete VyxTypes (U1, U2, ...).
//   6. Build a merged TypeEnv:
//        class-level bindings: parse the "<...>" args from classMono and zip
//          them with classTemplate->genericParams.
//        method-level bindings: zip method.genericParams with resolved callTypeArgs.
//   7. Request a standalone FunctionDecl instantiation named
//      "classMono.method<U1,U2>" via Monomorphize::request().
//
// The produced FunctionDecl is registered in the TU by processOne(). CodeGen's
// emitClassDecl (CodeGenClass.cpp) iterates `cls.methods` and registers each
// method under "className.methodName". However, because our synthetic decl is a
// top-level FunctionDecl (not embedded in a ClassDecl), it goes through
// `emitFunctionDecl` / `CodeGenGenerateForward`. CodeGen then inserts it into
// `functions_["Iterator<i64>.map<u32>"]`. When the call site calls
// findClassMethod("Iterator<i64>", "map<u32>") it will find it.
//
// Edge cases punted (see task deliverable §5):
//   - Variadic method generics (method.isVariadicGeneric = true): skipped
//     with a TODO; they require pack expansion before env building.
//   - Concept-constrained method generics: constraint checking is Sema's job;
//     Mono trusts Sema already validated the call.
//   - Static methods with method-level generics (Class.staticFn::<U>()):
//     handled by the same path since we detect `!ma->callTypeArgs.empty()`
//     regardless of `method.isStatic`; we just skip the self-param prepend.
static void requestMethodInstantiation(
        const MemberAccessExpr* ma,
        const TypeEnv& env,
        const std::vector<std::string>& activeGenerics,
        Monomorphize& mono) {

    if (!ma || ma->callTypeArgs.empty()) return;
    if (!ma->object || !ma->object->inferredType) {
        // Strict model: Sema failed to attach an inferredType to the receiver.
        // Previously silent — that masked the real problem (Sema gap) and
        // produced mysterious codegen garbage downstream. Emit MONO-006.
        SourceLocation loc = ma->location;
        std::string mangle = std::string("<method-instantiation ") + ma->member + ">";
        mono.reportFailureFromHelper("MONO-006", loc, mangle,
            "method instantiation '" + ma->member +
            "' has no inferred receiver type; Sema did not complete analysis "
            "of the receiver expression");
        return;
    }

    // `self.method::<U>(...)` inside a generic class method body carries
    // the TEMPLATE shape on its SelfExpr (e.g. `Transformer<T>`) because
    // Sema analysed the body once with unsubstituted T. Substitute through
    // the active env so the receiver settles on the concrete instance
    // (e.g. `Transformer<i32>`) before we derive classMono below; without
    // this step the synthesised FunctionDecl name comes out as
    // `Transformer<T>.method<U>` and CodeGen's `findClassMethod` can't
    // resolve the call at emission time.
    VyxTypePtr selfSubstituted;
    if (ma->object->kind == ExprKind::SelfExpr && ma->object->inferredType) {
        // Sema may have recorded SelfExpr's inferredType either as the
        // template shape (`Transformer<T>`) or — more commonly for class
        // methods analysed once under unsubstituted params — as the bare
        // base name (`Transformer`) with empty paramTypes. Both paths need
        // a recovery through the active env:
        //   - For `Transformer<T>` shape: substituteType rewrites T -> i32.
        //   - For bare `Transformer`: we look up the class template,
        //     reconstruct paramTypes from env-bound generic params, and
        //     synthesise the concrete `Transformer<i32>` VyxType so the
        //     downstream classMono derivation lands on the instance name
        //     (otherwise the synthesised FunctionDecl is mis-mangled and
        //     CodeGen's findClassMethod can't resolve `self.method::<U>()`).
        auto& orig = ma->object->inferredType;
        selfSubstituted = ::vyx::substituteType(orig, env);
        if (selfSubstituted && selfSubstituted->name.find('<') == std::string::npos) {
            if (const Decl* clsTpl = mono.findDecl(selfSubstituted->name)) {
                if (clsTpl->kind == DeclKind::Class && !clsTpl->genericParams.empty()) {
                    std::vector<VyxTypePtr> boundArgs;
                    boundArgs.reserve(clsTpl->genericParams.size());
                    bool allBound = true;
                    for (auto& pn : clsTpl->genericParams) {
                        auto b = env.lookup(pn);
                        if (!b || isUnboundGeneric(b, env)) { allBound = false; break; }
                        boundArgs.push_back(b);
                    }
                    if (allBound) {
                        auto rebuilt = std::make_shared<VyxType>(*selfSubstituted);
                        std::string rebuiltName = selfSubstituted->name + "<";
                        for (size_t i = 0; i < boundArgs.size(); ++i) {
                            if (i > 0) rebuiltName += ",";
                            rebuiltName += boundArgs[i]->mangle();
                        }
                        rebuiltName += ">";
                        rebuilt->name = std::move(rebuiltName);
                        rebuilt->paramTypes = std::move(boundArgs);
                        selfSubstituted = rebuilt;
                    }
                }
            }
        }
    }
    const VyxTypePtr& recvType = selfSubstituted
        ? selfSubstituted
        : ma->object->inferredType;
    // Method-level generics work on classes/structs (`obj.map::<U>()`) and on
    // enum receivers (`Option<T>.map::<U>()` / `Result<T,E>.map::<U>()`). For
    // enum receivers the methods live in an ErrorDefDecl rather than a
    // ClassDecl; the synthetic FunctionDecl carries an extra
    // `syntheticEnumReceiver` tag (set in makeSyntheticMethodDecl below) so
    // CodeGen's emitFunctionDecl can wire up the tagged-union dispatch
    // context required for the body's `match (self)`.
    bool isEnumReceiver = (isOptionLike(*recvType) ||
                           isResultLike(*recvType) ||
                           recvType->kind == VyxTypeKind::ErrorType);
    if (recvType->kind != VyxTypeKind::Class &&
        recvType->kind != VyxTypeKind::Struct &&
        !isEnumReceiver) {
        // Generic primitives (i32, bool, …) or unknown-typed receivers may
        // legitimately reach here via `impl Trait for i32` when the call
        // goes through the primitive-method path: Sema resolves the
        // dispatch, no class-level Mono instantiation is needed.  Generic
        // params still in scope likewise don't need a monomorph at this
        // site. For any OTHER kind (Array/Pointer/Reference/…) emit
        // MONO-007: Mono was handed a method-call receiver whose kind it
        // has no path to instantiate — almost always a Sema gap.
        if (recvType->kind != VyxTypeKind::Integer &&
            recvType->kind != VyxTypeKind::Float &&
            recvType->kind != VyxTypeKind::Bool &&
            recvType->kind != VyxTypeKind::Char &&
            recvType->kind != VyxTypeKind::RawPtr &&
            recvType->kind != VyxTypeKind::Generic &&
            recvType->kind != VyxTypeKind::Unknown) {
            SourceLocation loc = ma->location;
            std::string mangle = recvType->name + "." + ma->member;
            mono.reportFailureFromHelper("MONO-007", loc, mangle,
                "method instantiation receiver '" + recvType->toString() +
                "' is not a class/struct/enum; Mono has no instantiation path");
        }
        return;
    }

    // Step 1-2: derive base template name. Every template-kind receiver
    // (Class/Struct/user ErrorDef) carries a fully mangled name like
    // `Iterator<i64>` / `Option<i32>` / `Result<i32,string>`; the R5
    // refactor collapsed the former per-kind reconstruction into this
    // single `recvType->name` read.
    std::string classMono;
    if (isOptionLike(*recvType) || isResultLike(*recvType)) {
        // Option<T> / Result<T,E> are now canonical Class-kind with the
        // already-mangled name (e.g. "Option<i32>", "Result<i32,string>").
        classMono = recvType->name;
    } else {
        classMono = recvType->name;
    }
    auto lt = classMono.find('<');
    std::string baseClassName = (lt == std::string::npos)
        ? classMono
        : classMono.substr(0, lt);
    if (isEnumReceiver && baseClassName.empty())
        baseClassName = recvType->name; // last-resort

    // Step 3: find the class/enum template.
    const Decl* classTemplateDecl = mono.findDecl(baseClassName);
    if (!classTemplateDecl ||
        (classTemplateDecl->kind != DeclKind::Class &&
         classTemplateDecl->kind != DeclKind::Struct &&
         classTemplateDecl->kind != DeclKind::ErrorDef)) {
        // Strict model: the receiver names a type that has no backing
        // class/struct/errordef template Mono can resolve. Previously
        // silent; now surfaced as MONO-008.
        SourceLocation loc = ma->location;
        std::string mangle = baseClassName + "." + ma->member;
        mono.reportFailureFromHelper("MONO-008", loc, mangle,
            "method instantiation: class/struct/enum template '" +
            baseClassName + "' not found in translation unit");
        return;
    }

    // Recovery: when the method body of a parameterised enum instantiation
    // calls `self.method::<U>(...)`, the inner `self` still carries the
    // bare ErrorType name from Sema's class-body analysis (currentClassType_
    // was set to the *template* shape because Sema analyzed the generic
    // body once with unsubstituted T). After instantiation we want the call
    // site to see the *parameterised* receiver (e.g. `Option<i32>`) so the
    // per-instantiation method body is selected — not the bare-name
    // synthetic which CodeGen's `findClassMethod(currentClassName_, ...)`
    // can no longer resolve (currentClassName_ is the parameterised form).
    // When `classMono` lacks a `<...>` suffix and the template has generic
    // params that are bound in the active env, reconstruct the mangled
    // name from those bindings. Keeps the generic-free case (bare non-
    // generic ErrorDef) untouched — templateDecl has no genericParams so
    // the loop is skipped.
    if (classTemplateDecl->kind == DeclKind::ErrorDef && lt == std::string::npos &&
        !classTemplateDecl->genericParams.empty() &&
        recvType->paramTypes.empty()) {
        std::vector<VyxTypePtr> boundArgs;
        boundArgs.reserve(classTemplateDecl->genericParams.size());
        bool allBound = true;
        for (auto& pn : classTemplateDecl->genericParams) {
            auto bound = env.lookup(pn);
            if (!bound || isUnboundGeneric(bound, env)) { allBound = false; break; }
            boundArgs.push_back(bound);
        }
        if (allBound) {
            std::string rebuilt = baseClassName + "<";
            for (size_t i = 0; i < boundArgs.size(); ++i) {
                if (i > 0) rebuilt += ",";
                rebuilt += boundArgs[i] ? boundArgs[i]->mangle() : "?";
            }
            rebuilt += ">";
            classMono = rebuilt;
            lt = classMono.find('<');
        }
    }

    // Step 4: find the matching MethodDecl in either ClassDecl or ErrorDefDecl.
    const std::vector<MethodDecl>* methods = nullptr;
    if (classTemplateDecl->kind == DeclKind::Class)
        methods = &classTemplateDecl->as<ClassDecl>()->methods;
    else if (classTemplateDecl->kind == DeclKind::ErrorDef)
        methods = &classTemplateDecl->as<ErrorDefDecl>()->methods;
    else {
        // Struct: DTO-only (see feedback_struct_is_dto.md). Method-level
        // generics on a struct receiver are a Sema bug — the parser should
        // have rejected the `struct.method::<U>()` syntax. Surface as
        // MONO-009 so the user (or upstream Sema pass) notices.
        SourceLocation loc = ma->location;
        std::string mangle = baseClassName + "." + ma->member;
        mono.reportFailureFromHelper("MONO-009", loc, mangle,
            "method-level generic call on struct '" + baseClassName +
            "' — structs are DTO-only and carry no methods");
        return;
    }

    const MethodDecl* targetMethod = nullptr;
    bool targetFromImplBlock = false;
    for (auto& m : *methods) {
        if (m.name == ma->member &&
            m.genericParams.size() == ma->callTypeArgs.size()) {
            targetMethod = &m;
            break;
        }
    }
    if (!targetMethod && classTemplateDecl->kind == DeclKind::Class) {
        // Methods declared in `impl Trait for Type` are registered onto the
        // receiver's Sema method table, so Sema accepts calls like
        // `VecF.fmap::<A,B>(...)`. The AST owner, however, is a separate
        // ClassDecl marked isImplBlock, not the base `class VecF {}`. Mirror
        // Sema's lookup here so method-level Mono can synthesize the concrete
        // standalone function instead of reporting a false MONO-010.
        if (const auto* unit = mono.getUnit()) {
            for (auto& d : unit->declarations) {
                if (!d || d->kind != DeclKind::Class) continue;
                auto* impl = d->as<const ClassDecl>();
                if (!impl->isImplBlock || d->name != baseClassName) continue;
                for (auto& m : impl->methods) {
                    if (m.name == ma->member &&
                        m.genericParams.size() == ma->callTypeArgs.size()) {
                        targetMethod = &m;
                        targetFromImplBlock = true;
                        break;
                    }
                }
                if (targetMethod) break;
            }
        }
    }
    if (!targetMethod) {
        // Strict model: no method of the right name+generic-arity on the
        // receiver type. Sema should have caught this, but when it didn't
        // the old code silently dropped the request — leaving the call
        // un-instantiated and CodeGen later unable to resolve the call.
        // Surface as MONO-010.
        SourceLocation loc = ma->location;
        std::string mangle = baseClassName + "." + ma->member + "<" +
            std::to_string(ma->callTypeArgs.size()) + ">";
        mono.reportFailureFromHelper("MONO-010", loc, mangle,
            "method instantiation: no method named '" + ma->member +
            "' with " + std::to_string(ma->callTypeArgs.size()) +
            " type-arg(s) on '" + baseClassName + "'");
        return;
    }

    // Punt variadic method generics — they need pack expansion first.
    // MethodDecl has no isVariadicGeneric field (that lives on Decl); detect
    // variadic methods by checking whether the genericParams vector is larger
    // than the callTypeArgs.  This can happen when a method exposes a variadic
    // pack `fn zip<...Us>(...)` — skip those for now.
    // NOTE: if MethodDecl ever gains isVariadicGeneric, replace this heuristic.
    // For now the arity check above (genericParams.size() == callTypeArgs.size())
    // already prevents a match when the method has more params than provided.
    // Nothing extra needed here; fall through.

    // Step 5: resolve callTypeArgs through the active env.
    std::vector<VyxTypePtr> resolvedMethodArgs;
    resolvedMethodArgs.reserve(ma->callTypeArgs.size());
    for (auto& ta : ma->callTypeArgs) {
        if (!ta) {
            // Malformed AST: a turbofish slot with a null TypeAnnotation.
            // Almost certainly a parser or Sema bug; surface it.
            SourceLocation loc = ma->location;
            std::string mangle = baseClassName + "." + ma->member + "<null>";
            mono.reportFailureFromHelper("MONO-011", loc, mangle,
                "method instantiation '" + baseClassName + "." + ma->member +
                "' carries a null type-arg slot (malformed AST)");
            return;
        }
        VyxTypePtr resolved = TemplateResolver::resolveTypeAnnotation(*ta, &activeGenerics);
        VyxTypePtr concrete = ::vyx::substituteType(resolved, env);
        if (isUnboundGeneric(concrete, env)) {
            // Legit deferral: still-unbound generic at this site means an
            // outer instantiation will re-drive the scan with a concrete
            // binding. Track for the post-drain MONO-005 audit.
            std::string deferStub = baseClassName + "." + ma->member + "<?>";
            mono.markDeferred(deferStub, ma->location);
            return;
        }
        resolvedMethodArgs.push_back(concrete);
    }

    // Step 6: build the merged TypeEnv.
    // 6a) Class-level bindings: parse the type args from classMono.
    //     E.g. classMono = "Iterator<i64>" → class has genericParam "T" → T = i64.
    TypeEnv mergedEnv;
    if (lt != std::string::npos) {
        // Parse the inner "i64" from "Iterator<i64>".  We use the canonical
        // GenericType parser that Sema uses: re-resolve the classMono name
        // through TemplateResolver to get a GenericType, then zip its
        // resolved type-args with classTemplate->genericParams.
        // Simpler: use the VyxType already attached to the receiver — its
        // `typeParams` field carries the class-level arg types when Sema
        // filled them in.  Fall back to best-effort name-based parse only
        // when typeParams is empty (older Sema output).
        const std::vector<std::string>& classGenericParams =
            classTemplateDecl->genericParams;
        if (!classGenericParams.empty()) {
            // Primary path: recvType itself carries paramTypes. After R5
            // Option<T> / Result<T,E> are Class-kind with paramTypes populated
            // by makeOptional/makeResult, so the uniform Class path works for
            // user classes and the stdlib-registered Option/Result alike.
            std::vector<VyxTypePtr> classTyParams = recvType->paramTypes;
            if (classTyParams.size() == classGenericParams.size()) {
                for (size_t i = 0; i < classGenericParams.size(); ++i)
                    mergedEnv.bind(classGenericParams[i], classTyParams[i]);
            } else {
                // Fallback A: parse the `<...>` suffix of classMono directly.
                // When Sema resolves `v.iter()` to a Class-kind whose `.name`
                // is already the fully-mangled "Iterator<i32>" but leaves
                // `.paramTypes` empty (the common stdlib-return path seen
                // with method-chain receivers like
                // `v.iter().map::<U>(...)`), the class-level T binding never
                // lands in mergedEnv. The cloned body's `var val: T` /
                // `sizeof::<T>()` then fall through unsubstituted and lower
                // to the i64 default — silently corrupting reads at each T
                // element for any T whose size != 8 bytes (i32/i8/i16/f32/…).
                // This was the root cause of the `static_method_chain`
                // segfault (Vec<i32>.iter().map::<i32>(fn).collect()).
                //
                // Parse `Iterator<i32>` → inner="i32" → argNames=["i32"] with
                // angle-bracket depth tracking so nested generics like
                // `Iterator<Vec<i32>>` don't split on the inner comma.
                std::vector<std::string> parsedArgNames;
                {
                    std::string inner = classMono.substr(lt + 1);
                    if (!inner.empty() && inner.back() == '>') inner.pop_back();
                    int depth = 0;
                    std::string cur;
                    for (char c : inner) {
                        if (c == '<') { ++depth; cur += c; }
                        else if (c == '>') { --depth; cur += c; }
                        else if (c == ',' && depth == 0) {
                            parsedArgNames.push_back(cur);
                            cur.clear();
                        } else cur += c;
                    }
                    if (!cur.empty()) parsedArgNames.push_back(cur);
                }
                bool parsedOk = parsedArgNames.size() == classGenericParams.size();
                if (parsedOk) {
                    for (size_t i = 0; i < classGenericParams.size(); ++i) {
                        // Resolve each arg name through TemplateResolver so
                        // primitives (i32, string, ...) become canonical
                        // VyxType kinds, and nested generics (`Vec<i32>`)
                        // are lowered to a Class-kind VyxType whose name
                        // keeps the mangled form — substituteAnnotation in
                        // instantiate() then rewrites `var val: T` to the
                        // concrete type when cloning the method body.
                        TypeAnnotation ann;
                        ann.kind = TypeAnnotationKind::Named;
                        ann.name = parsedArgNames[i];
                        VyxTypePtr resolved = TemplateResolver::resolveTypeAnnotation(
                            ann, &activeGenerics);
                        if (!resolved) { parsedOk = false; break; }
                        mergedEnv.bind(classGenericParams[i], resolved);
                    }
                }
                if (!parsedOk) {
                    // Fallback B: look up the already-emitted monomorph in
                    // the TU (it was processed in a prior wave). Its env
                    // was already computed by whatever request created it;
                    // we approximate by re-using `env` which should already
                    // contain the class bindings if we are currently inside
                    // a substituted class body.
                    mergedEnv = TypeEnv{};  // drop any partial binds above
                    for (auto& [k, v] : env.bindings) mergedEnv.bind(k, v);
                    for (auto& [k, v] : env.constBindings) mergedEnv.bindConst(k, v);
                }
            }
        }
    }
    // 6b) Method-level bindings.
    for (size_t i = 0; i < targetMethod->genericParams.size(); ++i)
        mergedEnv.bind(targetMethod->genericParams[i], resolvedMethodArgs[i]);

    // Step 7: build the mangled name and enqueue.
    std::string mangledMethodName = Monomorphize::buildMangledMethodName(
        classMono, ma->member, resolvedMethodArgs);

    // Create a synthetic FunctionDecl that represents the method template.
    // makeSyntheticMethodDecl stores the decl in syntheticDecls_ (stable ptr).
    // Pass the merged env as the request env so instantiate() can substitute
    // both class-level and method-level references.
    bool forceStaticSynthetic = false;
    if (!targetMethod->isStatic &&
        ma->object && ma->object->kind == ExprKind::Identifier) {
        auto* objId = ma->object->as<const IdentifierExpr>();
        const bool isTypeRefReceiver =
            objId->name == baseClassName ||
            objId->name == classMono ||
            !objId->callTypeArgs.empty() ||
            !objId->callArgExprs.empty();
        const bool hasExplicitSelf =
            !targetMethod->params.empty() &&
            targetMethod->params.front().name == "self";
        if (isTypeRefReceiver && !hasExplicitSelf && targetFromImplBlock) {
            // Static-style associated calls through an impl block (for
            // example `VecF.fmap::<A,B>(...)`) have no receiver value at the
            // call site. Sema models their signature from the declared params
            // only, so the synthetic function must not grow an implicit self
            // pointer even though the source method omitted `static`.
            forceStaticSynthetic = true;
        }
    }
    const Decl* syntheticDecl = mono.makeSyntheticMethodDecl(
        baseClassName, *targetMethod, forceStaticSynthetic);

    // For ErrorDef receivers (Option/Result/user enums), tag the synthetic
    // decl with the enum's base name so emitFunctionDecl can stage the
    // tagged-union dispatch context (currentClassName_, structFieldNames_
    // alias) that the body's `match (self) { case Some(v) => ... }` requires
    // to deref the `*__self_opaque` self pointer back to a __Result struct
    // before the existing isResultStruct match path takes over.
    // Also propagate the fully-parameterised classMono (e.g.
    // "Result<i32,string>") as syntheticClassReceiver so CodeGen can
    // prefer the per-instantiation LLVM struct (e.g. `%__Result_32`
    // when the widest variant payload exceeds 8 bytes) instead of
    // falling back to the shared `%__Result` alias. Without the
    // parameterised name, `match (self)` would deref using the 16-byte
    // shared struct and mis-read the payload for wide-payload
    // instantiations like `Result<i32, string>`.
    if (classTemplateDecl->kind == DeclKind::ErrorDef) {
        auto* syntheticFd = const_cast<FunctionDecl*>(syntheticDecl->as<FunctionDecl>());
        syntheticFd->syntheticEnumReceiver = baseClassName;
        if (classMono != baseClassName) {
            syntheticFd->syntheticClassReceiver = classMono;
        }
    }
    // For Class receivers (e.g. Box<i32>.dup<i64>), tag the synthetic decl
    // with the fully-mangled receiver class name so emitFunctionDecl can set
    // currentClassName_ and resolve self.field accesses in the method body.
    if (classTemplateDecl->kind == DeclKind::Class) {
        const_cast<FunctionDecl*>(syntheticDecl->as<FunctionDecl>())
            ->syntheticClassReceiver = classMono;
    }

    InstantiationRequest req;
    req.templateDecl  = syntheticDecl;
    req.env           = std::move(mergedEnv);
    req.mangledName   = mangledMethodName;
    req.classTemplate = classTemplateDecl;

    mono.enqueue(std::move(req));
}

// ============================================================
//  isLazyMethod — @[lazy] opt-in check (Phase 8a, still opt-in)
// ============================================================
//
// Returns true when a MethodDecl carries the `@[lazy]` attribute, in
// which case it is dropped from the cloned ClassDecl body by both Sema
// and Mono and instead emitted on-demand as a standalone synthetic
// FunctionDecl the first time a call site is discovered (see
// `requestLazyMethodInstantiation` below).
//
// The `@[eager]` attribute is also recognised as an explicit-no-op
// counterpart for source compatibility with code written under the
// (attempted-and-rolled-back) lazy-by-default policy.  Since methods
// are already eager by default, `@[eager]` currently has no effect on
// behaviour — it exists so future work can flip the default without
// needing to touch every class.
//
// ── Architectural blocker for lazy-by-default ─────────────────────
// Making inherent instance methods lazy by default was attempted but
// reverted because the existing Sema→Mono pipeline does not robustly
// emit method-call requests for every reachable call site.  Specifically,
// when the receiver of `recv.method(...)` is the result of a
// method-generic call (e.g. `let bs = b.map::<string>(...);
// bs.get();`), Sema's member-lookup for `bs.get()` does not always
// converge to the concrete `MyBox<string>` class at analysis time —
// it emits a "no member 'get' found" warning and defers to CodeGen.
// CodeGen then probes `findClassMethod("MyBox<string>", "get")`, which
// previously succeeded because Mono eagerly emitted every method of
// every instantiated class.  With lazy-by-default, `get` would have
// to be enqueued on-demand by the Mono scanner; but the scanner's
// `findLazyMethodForCall` relies on `ma->object->inferredType` being
// a `VyxTypeKind::Class` at scan time, which is not the case when Sema
// fell back to the unresolved-member fallback path.
//
// A proper lazy-by-default implementation would need either:
//   (a) Sema's member resolution to fall back to the class template
//       when the concrete monomorph hasn't been emitted yet, or
//   (b) a deferred-resolution pass between Sema and Mono that chases
//       chained generic method returns to concrete types and patches
//       inferredType slots, or
//   (c) keeping method signatures (but skipping body re-analysis) in
//       Sema's class clone so member lookup succeeds even under lazy.
// All three are larger refactors than the Phase 8a opt-in mechanism.
// Until one lands, users opt into laziness with `@[lazy]` per method.
static bool isLazyMethod(const MethodDecl& method) {
    for (auto& [an, _av] : method.attributes) {
        if (an == "lazy") return true;
        // `@[eager]` is currently a no-op (eager is the default); the
        // attribute is parsed+accepted so source annotated under a
        // future lazy-by-default policy continues to compile.
    }
    return false;
}

// ============================================================
//  requestLazyMethodInstantiation — non-turbofish `obj.method(...)`
// ============================================================
//
// Called when we see `recv.method(args)` and the target `MethodDecl` is
// classified as lazy (see `isLazyMethod`).  Lazy methods are dropped from
// the cloned ClassDecl body in `Monomorphize::instantiate()` (so CodeGen
// never emits them unconditionally), and this helper re-introduces them
// on-demand as a standalone synthetic FunctionDecl named
// "<classMono>.<methodName>".
//
// This is the no-method-generic-params counterpart to
// `requestMethodInstantiation`: the algorithm is the same except
//   (1) there are no method-level `callTypeArgs` / `resolvedMethodArgs`, so
//       only the class-level bindings seed the merged TypeEnv;
//   (2) the output mangled name is just `<classMono>.<methodName>` with no
//       trailing `<U1,U2,...>` suffix — matching the key CodeGen's
//       `findClassMethod(className, methodName)` already probes.
//
// On unsubstituted receivers (type still carries a bare generic param name)
// we bail silently; the call will be re-scanned once the outer instantiation
// substitutes the env.  This mirrors the existing guards in
// `requestMethodInstantiation`.
static void requestLazyMethodInstantiation(
        const MemberAccessExpr* ma,
        const MethodDecl& targetMethod,
        const Decl* classTemplateDecl,
        const std::string& classMono,
        const TypeEnv& env,
        const std::vector<std::string>& activeGenerics,
        Monomorphize& mono) {

    if (!classTemplateDecl) return;
    (void)activeGenerics; // currently only used in a fallback branch below

    // Mirror the SelfExpr recovery in requestMethodInstantiation: for a
    // `self.method()` call whose inferredType is still the template shape
    // (`Transformer<T>`), substitute through the active env so classMono /
    // recvType settle on the concrete instance (`Transformer<i32>`).
    std::string effectiveClassMono = classMono;
    VyxTypePtr selfSubstituted;
    if (ma->object && ma->object->kind == ExprKind::SelfExpr && ma->object->inferredType) {
        selfSubstituted = ::vyx::substituteType(ma->object->inferredType, env);
        if (selfSubstituted && !selfSubstituted->name.empty()) {
            effectiveClassMono = selfSubstituted->name;
        }
    }

    auto lt = effectiveClassMono.find('<');
    std::string baseClassName = (lt == std::string::npos)
        ? effectiveClassMono
        : effectiveClassMono.substr(0, lt);

    // Build class-level TypeEnv from the receiver's fully-mangled name and
    // its paramTypes, mirroring `requestMethodInstantiation` step 6a.
    TypeEnv mergedEnv;
    const VyxTypePtr& recvType = selfSubstituted
        ? selfSubstituted
        : ma->object->inferredType;
    const std::vector<std::string>& classGenericParams =
        classTemplateDecl->genericParams;
    if (!classGenericParams.empty() && lt != std::string::npos) {
        std::vector<VyxTypePtr> classTyParams = recvType ? recvType->paramTypes
                                                         : std::vector<VyxTypePtr>{};
        if (classTyParams.size() == classGenericParams.size()) {
            for (size_t i = 0; i < classGenericParams.size(); ++i)
                mergedEnv.bind(classGenericParams[i], classTyParams[i]);
        } else {
            // Fallback: parse `<...>` suffix of effectiveClassMono with angle-
            // bracket depth tracking so nested generics don't split on inner
            // commas.
            std::vector<std::string> parsedArgNames;
            {
                std::string inner = effectiveClassMono.substr(lt + 1);
                if (!inner.empty() && inner.back() == '>') inner.pop_back();
                int depth = 0;
                std::string cur;
                for (char c : inner) {
                    if (c == '<') { ++depth; cur += c; }
                    else if (c == '>') { --depth; cur += c; }
                    else if (c == ',' && depth == 0) {
                        parsedArgNames.push_back(cur);
                        cur.clear();
                    } else cur += c;
                }
                if (!cur.empty()) parsedArgNames.push_back(cur);
            }
            if (parsedArgNames.size() == classGenericParams.size()) {
                for (size_t i = 0; i < classGenericParams.size(); ++i) {
                    TypeAnnotation ann;
                    ann.kind = TypeAnnotationKind::Named;
                    ann.name = parsedArgNames[i];
                    VyxTypePtr resolved = TemplateResolver::resolveTypeAnnotation(
                        ann, &activeGenerics);
                    if (!resolved) return;
                    mergedEnv.bind(classGenericParams[i], resolved);
                }
            } else {
                // Last-resort: reuse the outer env (it will contain class
                // bindings when scanning inside an already-substituted body).
                for (auto& [k, v] : env.bindings) mergedEnv.bind(k, v);
                for (auto& [k, v] : env.constBindings) mergedEnv.bindConst(k, v);
            }
        }
    } else {
        // Non-generic class (no type params): nothing to bind.
        for (auto& [k, v] : env.bindings) mergedEnv.bind(k, v);
        for (auto& [k, v] : env.constBindings) mergedEnv.bindConst(k, v);
    }

    // Mangled name: "<classMono>.<methodName>" — no method-generic suffix.
    std::string mangledMethodName = effectiveClassMono + "." + ma->member;

    const Decl* syntheticDecl = mono.makeSyntheticMethodDecl(baseClassName, targetMethod);
    auto* syntheticFd = const_cast<FunctionDecl*>(syntheticDecl->as<FunctionDecl>());
    syntheticFd->syntheticClassReceiver = effectiveClassMono;
    // Mirror the requestMethodInstantiation tag for ErrorDef receivers
    // (Option/Result/user enums): without this, emitFunctionDecl can't
    // wire up the tagged-union dispatch context for `match (self)`
    // bodies, and Option<i32>.filter's `case Some(v) => ...` reads
    // garbage from the wrong receiver layout (BUG-LV-11 segfault).
    if (classTemplateDecl->kind == DeclKind::ErrorDef) {
        syntheticFd->syntheticEnumReceiver = baseClassName;
    }

    InstantiationRequest req;
    req.templateDecl  = syntheticDecl;
    req.env           = std::move(mergedEnv);
    req.mangledName   = mangledMethodName;
    req.classTemplate = classTemplateDecl;

    mono.enqueue(std::move(req));
}

// Probe: resolve a `recv.method(...)` call site to its class/enum template
// and the corresponding MethodDecl. Returns non-null when the method needs
// on-demand synthesis at this call site:
//
//   * Class receivers — only `@[lazy]` opt-in methods (the class-clone in
//     instantiate() emits all non-lazy methods alongside the class).
//   * lang-item ErrorDef receivers (Option<T> / Result<T,E>) — EVERY non-
//     generic method, because `requestTemplateWithArgs` early-returns for
//     `@[lang_item]` ErrorDefs (they alias the runtime `__Result` layout
//     and are never cloned). Without this branch, BUG-LV-11 fires:
//     `m1.filter(is_even)` has no turbofish (so requestMethodInstantiation
//     skips it), filter has no `@[lazy]` (so the old lazy probe rejected
//     it), and the class-clone path is disabled — so Mono never schedules
//     `Option<i32>.filter` and CodeGen reports `no member 'filter'`.
//
//     The extension covers ALL non-generic methods of Option/Result
//     uniformly — no per-method allowlist, no `if (name == "filter")`
//     special case (the task's "禁仅 Ref 特例 / 禁宏跳过" constraint).
static const MethodDecl* findLazyMethodForCall(
        const MemberAccessExpr* ma,
        const Monomorphize& mono,
        std::string& outClassMono,
        const Decl*& outClassTemplate) {
    outClassTemplate = nullptr;
    if (!ma || !ma->object || !ma->object->inferredType) return nullptr;
    auto& recvType = ma->object->inferredType;
    // Class-kind: regular user classes (and post-R5 some std generics).
    // ErrorType-kind: Option<T> / Result<T,E> (lang-item enums) and
    //   user-defined `enum BinTree<T> { ... }` style ADTs. Both flavours
    //   may carry methods that need on-demand synthesis at call sites
    //   that lack turbofish / @[lazy] markers.
    if (recvType->kind != VyxTypeKind::Class &&
        recvType->kind != VyxTypeKind::ErrorType) return nullptr;

    outClassMono = recvType->name;
    auto lt = outClassMono.find('<');
    std::string baseClassName = (lt == std::string::npos)
        ? outClassMono
        : outClassMono.substr(0, lt);

    const Decl* classTemplateDecl = mono.findDecl(baseClassName);
    if (!classTemplateDecl) return nullptr;

    auto isLangItemErrorDef = [](const Decl* d) -> bool {
        if (!d || d->kind != DeclKind::ErrorDef) return false;
        for (auto& [an, _] : d->attributes) {
            if (an == "lang_item") return true;
        }
        return false;
    };

    // Structural test (NOT a method allowlist — task constraint
    // forbids "仅 Ref 特例" / per-name special cases): the method
    // takes at least one Function-typed parameter (closure / fn-ptr).
    // These are the methods that the lang-item-ErrorDef code path
    // genuinely needs Mono to synthesise — `map(f)`, `and_then(f)`,
    // `or_else(f)`, `filter(pred)`, `map_err(f)`, ... — because their
    // bodies invoke the fn-typed argument and CodeGen has no inline
    // builtin equivalent.
    //
    // Methods without fn parameters (`unwrap`, `unwrap_or`,
    // `isSome`, `isNone`, the plain `clone`, ...) are handled inline
    // by CodeGenMethodCall.cpp's tagged-union dispatch — synthesising
    // them too would emit duplicate IR and (under the current
    // panic-after-match emission) trip the LLVM verifier with
    // "Terminator found in the middle of a basic block".
    auto methodTakesFnParam = [](const MethodDecl& m) -> bool {
        for (auto& p : m.params) {
            if (!p.type) continue;
            if (p.type->kind == TypeAnnotationKind::Function) return true;
        }
        return false;
    };

    const std::vector<MethodDecl>* methods = nullptr;
    bool isLangItemErr = false;
    if (classTemplateDecl->kind == DeclKind::Class) {
        methods = &classTemplateDecl->as<ClassDecl>()->methods;
    } else if (isLangItemErrorDef(classTemplateDecl)) {
        methods = &classTemplateDecl->as<ErrorDefDecl>()->methods;
        isLangItemErr = true;
    } else {
        // Non-lang-item ErrorDef / Struct / Interface: methods (if any) are
        // emitted by their respective instantiation paths — nothing to do
        // here.
        return nullptr;
    }

    for (auto& m : *methods) {
        if (m.name != ma->member) continue;
        // Method-level generics (turbofish) go through requestMethodInstantiation.
        if (!m.genericParams.empty()) continue;
        // Class: only opt-in @[lazy] methods need on-demand synthesis;
        //        non-lazy methods are emitted alongside the class instance.
        // Lang-item ErrorDef (Option/Result): the class-instantiation
        //        clone path is short-circuited in requestTemplateWithArgs,
        //        so non-generic higher-order methods (those with a fn
        //        parameter) need on-demand synthesis. Plain query/unwrap
        //        methods stay on the inline-CodeGen path.
        if (isLazyMethod(m) || (isLangItemErr && methodTakesFnParam(m))) {
            outClassTemplate = classTemplateDecl;
            return &m;
        }
        return nullptr; // found but no synth path applies
    }
    return nullptr;
}

static void scanExprForRequests(const Expr* expr, const TypeEnv& env,
                                  const std::vector<std::string>& activeGenerics,
                                  Monomorphize& mono) {
    if (!expr) return;

    switch (expr->kind) {
        case ExprKind::Call: {
            auto* ce = expr->as<CallExpr>();
            scanExprForRequests(ce->callee.get(), env, activeGenerics, mono);
            for (auto& arg : ce->args)
                scanExprForRequests(arg.get(), env, activeGenerics, mono);

            // Turbofish: callee is Identifier with callTypeArgs
            if (ce->callee && ce->callee->kind == ExprKind::Identifier) {
                auto* ident = ce->callee->as<IdentifierExpr>();
                if (!ident->callTypeArgs.empty()) {
                    requestTemplateWithArgs(ident->name, ident->callTypeArgs,
                                            nullptr, env, activeGenerics, mono);
                    // Builtin container factory functions are not generic decls
                    // themselves, but their type-argument(s) denote a concrete
                    // container class that Mono must instantiate. Map the
                    // well-known factory names to their corresponding class
                    // templates so `makeRef::<Foo>(...)` seeds `Ref<Foo>`,
                    // `makeScope::<Foo>(...)` seeds `Scope<Foo>`, etc.
                    // Covers CodeGenVarDecl.cpp TODO(P1c-C) scan gap for
                    // builtin container constructors.
                    static const std::map<std::string, std::string>
                        kBuiltinToClass = {
                            {"makeRef",     "Ref"},
                            {"makeScope",   "Scope"},
                            {"makeChannel", "Channel"},
                            {"makeMutex",   "Mutex"},
                        };
                    auto bIt = kBuiltinToClass.find(ident->name);
                    if (bIt != kBuiltinToClass.end()) {
                        requestTemplateWithArgs(bIt->second, ident->callTypeArgs,
                                                nullptr, env, activeGenerics, mono);
                    }
                }
            }

            // Static method call: `Type::<Args>.method(...)` parses to
            //   Call(callee = MemberAccess(
            //          object = Identifier(name="Type", callTypeArgs=[Args...]),
            //          member = "method"))
            // Without this branch the enclosing class template (Slice, Dict,
            // HashSet, ...) would never be enqueued via a free-function call
            // site, so its `static fn new`/`from_vec`/etc. methods stay
            // un-mangled and lld later complains about undefined symbols.
            //
            // We also want `obj.method::<U>(...)` (instance-method turbofish)
            // to seed monomorphisation of the *enclosing class*, which we
            // approximate by routing the `callTypeArgs` on the MemberAccess
            // through the same template-request helper as static calls.
            if (ce->callee && ce->callee->kind == ExprKind::MemberAccess) {
                auto* ma = ce->callee->as<MemberAccessExpr>();
                if (ma->object && ma->object->kind == ExprKind::Identifier) {
                    auto* objIdent = ma->object->as<IdentifierExpr>();
                    if (!objIdent->callTypeArgs.empty()) {
                        // Recurse into each type-arg first so that nested
                        // generic instantiations like Vec::<Vec<i32>>.new()
                        // also enqueue the inner Vec<i32> before the outer
                        // Vec<Vec<i32>> (mirrors the scanTypeForRequests
                        // Generic branch which recurses before requesting).
                        for (auto& ta : objIdent->callTypeArgs)
                            scanTypeForRequests(ta.get(), env, activeGenerics, mono);
                        // Pass callArgExprs so that const-param values (e.g. the `0`
                        // in `NonEmpty::<i32, 0>.new()`) reach requestTemplateWithArgs
                        // and monoVerifyConstPredicates can fire P2D-004 if needed.
                        requestTemplateWithArgs(objIdent->name,
                                                objIdent->callTypeArgs,
                                                &objIdent->callArgExprs,
                                                env, activeGenerics, mono);
                    }
                }
                // `obj.method::<U>(...)` — the type args belong to the
                // *method*, not the enclosing class. Delegate to the
                // dedicated method-instantiation helper which builds the
                // merged env (class bindings + method bindings) and
                // enqueues a standalone FunctionDecl named
                // "ClassMono.method<U>" so CodeGen's findClassMethod can
                // locate the concrete body at emission time.
                if (!ma->callTypeArgs.empty()) {
                    requestMethodInstantiation(ma, env, activeGenerics, mono);
                } else {
                    // `obj.method(...)` with no method-generic args — normally
                    // the method is emitted as part of the enclosing class
                    // instantiation, but methods tagged `@[lazy]` are skipped
                    // from the cloned class body (see instantiate() Class
                    // case). Detect a lazy target here and enqueue a
                    // standalone synthetic FunctionDecl so the call site
                    // links correctly.
                    std::string classMono;
                    const Decl* classTemplate = nullptr;
                    if (auto* lazyM = findLazyMethodForCall(
                            ma, mono, classMono, classTemplate)) {
                        requestLazyMethodInstantiation(
                            ma, *lazyM, classTemplate, classMono,
                            env, activeGenerics, mono);
                    }
                }
            }
            break;
        }
        case ExprKind::BinaryOp: {
            auto* e = expr->as<BinaryOpExpr>();
            scanExprForRequests(e->lhs.get(), env, activeGenerics, mono);
            scanExprForRequests(e->rhs.get(), env, activeGenerics, mono);
            break;
        }
        case ExprKind::UnaryOp: {
            auto* e = expr->as<UnaryOpExpr>();
            scanExprForRequests(e->operand.get(), env, activeGenerics, mono);
            break;
        }
        case ExprKind::MemberAccess: {
            auto* e = expr->as<MemberAccessExpr>();
            scanExprForRequests(e->object.get(), env, activeGenerics, mono);
            break;
        }
        case ExprKind::Index: {
            auto* e = expr->as<IndexExpr>();
            scanExprForRequests(e->object.get(), env, activeGenerics, mono);
            scanExprForRequests(e->indexExpr.get(), env, activeGenerics, mono);
            break;
        }
        case ExprKind::Assignment: {
            auto* e = expr->as<AssignmentExpr>();
            scanExprForRequests(e->lhs.get(), env, activeGenerics, mono);
            scanExprForRequests(e->rhs.get(), env, activeGenerics, mono);
            break;
        }
        case ExprKind::CompoundAssignment: {
            auto* e = expr->as<CompoundAssignmentExpr>();
            scanExprForRequests(e->target.get(), env, activeGenerics, mono);
            scanExprForRequests(e->value.get(), env, activeGenerics, mono);
            break;
        }
        case ExprKind::Ternary: {
            auto* e = expr->as<TernaryExpr>();
            scanExprForRequests(e->condition.get(), env, activeGenerics, mono);
            scanExprForRequests(e->trueExpr.get(), env, activeGenerics, mono);
            scanExprForRequests(e->falseExpr.get(), env, activeGenerics, mono);
            break;
        }
        case ExprKind::Closure: {
            auto* e = expr->as<ClosureExpr>();
            scanStmtForRequests(e->body.get(), env, activeGenerics, mono);
            scanExprForRequests(e->singleExpr.get(), env, activeGenerics, mono);
            break;
        }
        case ExprKind::FailExpr: {
            auto* e = expr->as<FailExpr>();
            scanExprForRequests(e->message.get(), env, activeGenerics, mono);
            break;
        }
        case ExprKind::TryExpr: {
            auto* e = expr->as<TryExpr>();
            scanExprForRequests(e->inner.get(), env, activeGenerics, mono);
            break;
        }
        case ExprKind::AwaitExpr: {
            auto* e = expr->as<AwaitExpr>();
            scanExprForRequests(e->inner.get(), env, activeGenerics, mono);
            break;
        }
        case ExprKind::StructInit: {
            auto* e = expr->as<StructInitExpr>();
            // Scan field init expressions.
            for (auto& [fname, fexpr] : e->fieldInits)
                scanExprForRequests(fexpr.get(), env, activeGenerics, mono);
            scanExprForRequests(e->spreadBase.get(), env, activeGenerics, mono);
            // Scan the struct type itself: when the source was `Foo<T> { ... }`
            // the parser folds the type args into `structName` as a mangled
            // string (e.g. `"Pair<i32,string>"`). The `typeAnnotation` field on
            // `StructInitExpr` is not populated by the parser, so we recover the
            // base name and type args by parsing the mangled string.
            // Covers CodeGenExpr.cpp TODO(P1c-C) scan gap.
            if (e->structName.find('<') != std::string::npos)
                scanMangledTypeNameForRequests(e->structName, env, activeGenerics, mono);
            break;
        }
        case ExprKind::ArrayInit: {
            auto* e = expr->as<ArrayInitExpr>();
            for (auto& elem : e->elements)
                scanExprForRequests(elem.get(), env, activeGenerics, mono);
            scanExprForRequests(e->repeatCount.get(), env, activeGenerics, mono);
            break;
        }
        case ExprKind::TupleInit: {
            auto* e = expr->as<TupleInitExpr>();
            for (auto& elem : e->elements)
                scanExprForRequests(elem.get(), env, activeGenerics, mono);
            break;
        }
        case ExprKind::StringInterpolation: {
            auto* e = expr->as<StringInterpExpr>();
            for (auto& part : e->parts)
                if (part.isExpr)
                    scanExprForRequests(part.expr.get(), env, activeGenerics, mono);
            break;
        }
        case ExprKind::Cast: {
            auto* e = expr->as<CastExpr>();
            scanExprForRequests(e->operand.get(), env, activeGenerics, mono);
            break;
        }
        default:
            break;
    }
}

static void scanStmtForRequests(const Stmt* stmt, const TypeEnv& env,
                                  const std::vector<std::string>& activeGenerics,
                                  Monomorphize& mono) {
    if (!stmt) return;

    switch (stmt->kind) {
        case StmtKind::Block: {
            auto* s = stmt->as<BlockStmt>();
            for (auto& child : s->statements)
                scanStmtForRequests(child.get(), env, activeGenerics, mono);
            break;
        }
        case StmtKind::ExprStmt: {
            auto* s = stmt->as<ExprStmt>();
            scanExprForRequests(s->expr.get(), env, activeGenerics, mono);
            break;
        }
        case StmtKind::Return: {
            auto* s = stmt->as<ReturnStmt>();
            scanExprForRequests(s->expr.get(), env, activeGenerics, mono);
            break;
        }
        case StmtKind::VarDecl: {
            auto* s = stmt->as<VarDeclStmt>();
            // Scan the explicit type annotation first: `var x: Foo<T> = ...`
            // carries `varType` as a proper GenericType that already encodes the
            // base name and type args in structured form. This covers the Mono
            // scan gap for ctor-detection, method-call return type, and builtin
            // container cases in CodeGenVarDecl.cpp TODO(P1c-C) sites where
            // the structTypes_ lookup might otherwise find no entry.
            scanTypeForRequests(s->varType.get(), env, activeGenerics, mono);
            scanExprForRequests(s->initExpr.get(), env, activeGenerics, mono);
            break;
        }
        case StmtKind::If: {
            auto* s = stmt->as<IfStmt>();
            scanExprForRequests(s->condition.get(), env, activeGenerics, mono);
            scanStmtForRequests(s->thenBranch.get(), env, activeGenerics, mono);
            for (auto& [cond, branch] : s->elifBranches) {
                scanExprForRequests(cond.get(), env, activeGenerics, mono);
                scanStmtForRequests(branch.get(), env, activeGenerics, mono);
            }
            scanStmtForRequests(s->elseBranch.get(), env, activeGenerics, mono);
            break;
        }
        case StmtKind::While: {
            auto* s = stmt->as<WhileStmt>();
            scanExprForRequests(s->condition.get(), env, activeGenerics, mono);
            scanStmtForRequests(s->body.get(), env, activeGenerics, mono);
            break;
        }
        case StmtKind::For: {
            auto* s = stmt->as<ForStmt>();
            scanStmtForRequests(s->init.get(), env, activeGenerics, mono);
            scanExprForRequests(s->condition.get(), env, activeGenerics, mono);
            scanExprForRequests(s->step.get(), env, activeGenerics, mono);
            scanStmtForRequests(s->body.get(), env, activeGenerics, mono);
            break;
        }
        case StmtKind::ForEach: {
            auto* s = stmt->as<ForEachStmt>();
            scanExprForRequests(s->collection.get(), env, activeGenerics, mono);
            scanStmtForRequests(s->body.get(), env, activeGenerics, mono);
            break;
        }
        case StmtKind::Match: {
            auto* s = stmt->as<MatchStmt>();
            scanExprForRequests(s->expr.get(), env, activeGenerics, mono);
            for (auto& arm : s->arms) {
                scanExprForRequests(arm.valuePattern.get(), env, activeGenerics, mono);
                scanExprForRequests(arm.guardExpr.get(), env, activeGenerics, mono);
                scanStmtForRequests(arm.body.get(), env, activeGenerics, mono);
            }
            break;
        }
        case StmtKind::Assignment: {
            auto* s = stmt->as<AssignStmt>();
            scanExprForRequests(s->target.get(), env, activeGenerics, mono);
            scanExprForRequests(s->value.get(), env, activeGenerics, mono);
            break;
        }
        case StmtKind::Defer: {
            auto* s = stmt->as<DeferStmt>();
            scanStmtForRequests(s->body.get(), env, activeGenerics, mono);
            break;
        }
        case StmtKind::Unsafe: {
            auto* s = stmt->as<UnsafeStmt>();
            scanStmtForRequests(s->body.get(), env, activeGenerics, mono);
            break;
        }
        default:
            break;
    }
}

void Monomorphize::scanForRequests(const Decl& decl, const TypeEnv& env) {
    switch (decl.kind) {
        case DeclKind::Function: {
            auto* fd = decl.as<FunctionDecl>();
            // Active generic-parameter scope: this function's own params,
            // plus anything already bound by `env` (so partially-substituted
            // bodies still see ancestor generics).
            std::vector<std::string> activeGenerics = fd->genericParams;
            for (auto& [n, _] : env.bindings) activeGenerics.push_back(n);
            // Walk param/return types so a free function with no body but
            // signature like `fn foo(d: Dict<i64, bool>)` still seeds the
            // Dict instantiation chain. (Concrete usage is rare for
            // free fns, but the symmetry with class fields is worth
            // preserving.)
            for (auto& p : fd->params)
                scanTypeForRequests(p.type.get(), env, activeGenerics, *this);
            scanTypeForRequests(fd->returnType.get(), env, activeGenerics, *this);
            scanStmtForRequests(fd->body.get(), env, activeGenerics, *this);
            break;
        }
        case DeclKind::Class: {
            auto* cd = decl.as<ClassDecl>();
            // Class-level scope for field/method-signature scans.
            std::vector<std::string> classScope = cd->genericParams;
            for (auto& [n, _] : env.bindings) classScope.push_back(n);
            // Field types: when this class is itself an instantiation
            // (e.g. `class HashSet<i64>` whose `inner: Dict<T, bool>` has
            // already been substituted to `Dict<i64, bool>`), the
            // resulting field annotation references *another* generic
            // template that no user call site mentions directly. Without
            // this scan, codegen would emit `class Dict<i64, bool>` only
            // if a user happens to write `Dict::<i64,bool>` somewhere —
            // which is exactly what fails for `hashset_smoke.vyx` where
            // the only Dict user is HashSet itself.
            for (auto& f : cd->fields)
                scanTypeForRequests(f.type.get(), env, classScope, *this);
            for (auto& method : cd->methods) {
                // Per-method scope: union of class generics + method
                // generics. Method-level params are explicitly visible
                // here so that turbofish call sites mentioning them resolve
                // to in-scope `Generic` (and are skipped as unbound)
                // rather than being misclassified as user `Class` types.
                std::vector<std::string> methodScope = classScope;
                for (auto& gp : method.genericParams) methodScope.push_back(gp);
                for (auto& p : method.params)
                    scanTypeForRequests(p.type.get(), env, methodScope, *this);
                scanTypeForRequests(method.returnType.get(), env, methodScope, *this);
                scanStmtForRequests(method.body.get(), env, methodScope, *this);
            }
            break;
        }
        case DeclKind::Struct: {
            auto* sd = decl.as<StructDecl>();
            std::vector<std::string> activeGenerics = sd->genericParams;
            for (auto& [n, _] : env.bindings) activeGenerics.push_back(n);
            for (auto& f : sd->fields)
                scanTypeForRequests(f.type.get(), env, activeGenerics, *this);
            break;
        }
        case DeclKind::GlobalVar: {
            // Global variable declarations carry an explicit type annotation
            // (`varType`) that may reference a generic class. Scan it so the
            // instantiation request is enqueued before CodeGen runs.
            // Covers CodeGenDecl.cpp TODO(P1c-C) scan gap.
            auto* gv = decl.as<GlobalVarDecl>();
            std::vector<std::string> activeGenerics;
            for (auto& [n, _] : env.bindings) activeGenerics.push_back(n);
            scanTypeForRequests(gv->varType.get(), env, activeGenerics, *this);
            // Also walk the initialiser body in case it contains further
            // generic call sites (e.g. `makeRef::<Foo>(...)` at module level).
            scanStmtForRequests(gv->initBody.get(), env, activeGenerics, *this);
            break;
        }
        default:
            break;
    }
}

// ============================================================
//  instantiate — deep clone + type substitution
// ============================================================

std::unique_ptr<Decl> Monomorphize::instantiate(const Decl& templateDecl,
                                                   const TypeEnv& env,
                                                   const std::string& mangledName) {
    // Activate the env for the entire body of this instantiation so that the
    // deep-clone path picks up T → concrete substitutions automatically.
    // RAII-style restore via a small scope guard so early returns / exceptions
    // don't leak the pointer into the next instantiation.
    struct EnvGuard {
        const TypeEnv*& slot;
        const TypeEnv* prev;
        EnvGuard(const TypeEnv*& s, const TypeEnv* next) : slot(s), prev(s) { slot = next; }
        ~EnvGuard() { slot = prev; }
    } guard(currentEnv_, &env);

    switch (templateDecl.kind) {
        case DeclKind::Function: {
            auto* src = templateDecl.as<FunctionDecl>();
            auto clone = std::make_unique<FunctionDecl>();

            clone->kind = DeclKind::Function;
            clone->location = src->location;
            clone->name = mangledName;
            clone->isExport = src->isExport;
            clone->isImported = src->isImported;
            clone->visibility = src->visibility;
            clone->genericParams.clear();
            clone->isVariadicGeneric = false;
            clone->externABI = src->externABI;
            clone->attributes = src->attributes;
            clone->docComment = src->docComment;
            clone->isAsync = src->isAsync;
            clone->isComptime = src->isComptime;
            clone->isBench = src->isBench;
            // Propagate receiver tags so emitFunctionDecl can restore context.
            clone->syntheticEnumReceiver = src->syntheticEnumReceiver;
            clone->syntheticClassReceiver = src->syntheticClassReceiver;

            for (auto& param : src->params) {
                ParamDecl p;
                p.name = param.name;
                p.isMutRef = param.isMutRef;
                p.type = substituteAnnotation(param.type.get(), env);
                p.defaultValue = cloneExpr(param.defaultValue.get());
                clone->params.push_back(std::move(p));
            }

            clone->returnType = substituteAnnotation(src->returnType.get(), env);
            clone->body = cloneStmt(src->body.get());

            return clone;
        }

        case DeclKind::Class: {
            auto* src = templateDecl.as<ClassDecl>();
            auto clone = std::make_unique<ClassDecl>();

            clone->kind = DeclKind::Class;
            clone->location = src->location;
            clone->name = mangledName;
            clone->isExport = src->isExport;
            clone->isImported = src->isImported;
            clone->visibility = src->visibility;
            clone->genericParams.clear();
            clone->isVariadicGeneric = false;
            clone->attributes = src->attributes;
            clone->docComment = src->docComment;
            clone->parentName = src->parentName;
            clone->interfaces = src->interfaces;
            clone->isImplBlock = src->isImplBlock;

            for (auto& field : src->fields) {
                FieldDecl f;
                f.visibility = field.visibility;
                f.isWeak = field.isWeak;
                f.name = field.name;
                f.type = substituteAnnotation(field.type.get(), env);
                f.defaultValue = cloneExpr(field.defaultValue.get());
                f.location = field.location;
                f.extraNames = field.extraNames;
                clone->fields.push_back(std::move(f));
            }

            for (auto& method : src->methods) {
                // `@[lazy]` opt-in: methods tagged lazy are skipped from the
                // cloned class body and instead emitted on-demand as a
                // standalone synthetic FunctionDecl named
                // "<classMono>.<methodName>" the first time a call site is
                // discovered (see `requestLazyMethodInstantiation`). This
                // matches Rust's behaviour for methods with trait bounds
                // that are only required if the method is actually invoked:
                // e.g. `Vec<T>.clone()` needs `T: Clone`, but a `Vec<Box<dyn
                // Job>>` that is never cloned must still compile. Eagerly
                // instantiating every method of every class would otherwise
                // trip the LLVM verifier when a body like `e.clone()` sees
                // a T without an `impl Clone`.
                //
                // Making lazy the default was attempted and reverted — see
                // the `isLazyMethod` helper above for the architectural
                // blocker writeup.  Users opt into laziness per-method with
                // `@[lazy]`.
                if (isLazyMethod(method)) continue;

                MethodDecl m;
                m.visibility = method.visibility;
                m.name = method.name;
                m.docComment = method.docComment;
                m.isOverride = method.isOverride;
                m.isAsync = method.isAsync;
                m.isStatic = method.isStatic;
                m.location = method.location;
                m.attributes = method.attributes;
                // H4-method-mono: keep method-level generic params intact when
                // the method introduces NEW type parameters not bound by the
                // class env (e.g. `fn map<U>` on `Iterator<T>`). Codegen
                // continues to skip these (CodeGenStruct/GeneratePending guard
                // on `method.genericParams` non-empty), and the dedicated
                // method-monomorph generation chain (planned H5 follow-up)
                // walks each call site with its inferred U bindings and
                // appends concrete `method<U>`-mangled standalone clones.
                // Clearing here would emit a generic body into codegen with
                // `U` still unbound and trip the LLVM verifier.
                m.genericParams = method.genericParams;
                m.typeEqualityConstraints = method.typeEqualityConstraints;
                m.typeInequalityConstraints = method.typeInequalityConstraints;

                for (auto& param : method.params) {
                    ParamDecl p;
                    p.name = param.name;
                    p.isMutRef = param.isMutRef;
                    p.type = substituteAnnotation(param.type.get(), env);
                    p.defaultValue = cloneExpr(param.defaultValue.get());
                    m.params.push_back(std::move(p));
                }
                m.returnType = substituteAnnotation(method.returnType.get(), env);
                m.body = cloneStmt(method.body.get());

                clone->methods.push_back(std::move(m));
            }

            return clone;
        }

        case DeclKind::Struct: {
            auto* src = templateDecl.as<StructDecl>();
            auto clone = std::make_unique<StructDecl>();

            clone->kind = DeclKind::Struct;
            clone->location = src->location;
            clone->name = mangledName;
            clone->isExport = src->isExport;
            clone->isImported = src->isImported;
            clone->visibility = src->visibility;
            clone->genericParams.clear();
            clone->isVariadicGeneric = false;
            clone->attributes = src->attributes;
            clone->docComment = src->docComment;
            clone->parentName = src->parentName;

            for (auto& field : src->fields) {
                FieldDecl f;
                f.visibility = field.visibility;
                f.isWeak = field.isWeak;
                f.name = field.name;
                f.type = substituteAnnotation(field.type.get(), env);
                f.defaultValue = cloneExpr(field.defaultValue.get());
                f.location = field.location;
                f.extraNames = field.extraNames;
                clone->fields.push_back(std::move(f));
            }

            return clone;
        }

        case DeclKind::ErrorDef: {
            // Generic enum / ADT instantiation. We deep-clone the variant
            // list, walk every payload TypeAnnotation through the env so a
            // recursive `Box<Self<T>>` payload (`enum BinTree<T> { Leaf,
            // Node(T, Box<BinTree<T>>) }`) becomes `Box<BinTree<i32>>` on
            // the cloned variant — both occurrences of the type parameter
            // (the bare `T` payload and the nested `Self<T>`-shaped child)
            // pass through `substituteAnnotation` uniformly. This is the
            // generic enum-payload path: any T that appears anywhere inside
            // a variant TypeAnnotation gets substituted, regardless of how
            // deeply it is nested or whether it sits behind `Box`/`Ref`.
            auto* src = templateDecl.as<ErrorDefDecl>();
            auto clone = std::make_unique<ErrorDefDecl>();

            clone->kind = DeclKind::ErrorDef;
            clone->location = src->location;
            clone->name = mangledName;
            clone->isExport = src->isExport;
            clone->isImported = src->isImported;
            clone->visibility = src->visibility;
            clone->genericParams.clear();
            clone->isVariadicGeneric = false;
            clone->attributes = src->attributes;
            clone->docComment = src->docComment;

            clone->variants = src->variants;
            clone->variantTypes.reserve(src->variantTypes.size());
            for (auto& payload : src->variantTypes) {
                std::vector<TypePtr> mappedPayload;
                mappedPayload.reserve(payload.size());
                for (auto& t : payload) {
                    mappedPayload.push_back(substituteAnnotation(t.get(), env));
                }
                clone->variantTypes.push_back(std::move(mappedPayload));
            }

            for (auto& method : src->methods) {
                if (isLazyMethod(method)) continue;

                MethodDecl m;
                m.visibility = method.visibility;
                m.name = method.name;
                m.docComment = method.docComment;
                m.isOverride = method.isOverride;
                m.isAsync = method.isAsync;
                m.isStatic = method.isStatic;
                m.location = method.location;
                m.attributes = method.attributes;
                m.genericParams = method.genericParams;
                m.typeEqualityConstraints = method.typeEqualityConstraints;
                m.typeInequalityConstraints = method.typeInequalityConstraints;

                for (auto& param : method.params) {
                    ParamDecl p;
                    p.name = param.name;
                    p.isMutRef = param.isMutRef;
                    p.type = substituteAnnotation(param.type.get(), env);
                    p.defaultValue = cloneExpr(param.defaultValue.get());
                    m.params.push_back(std::move(p));
                }
                m.returnType = substituteAnnotation(method.returnType.get(), env);
                m.body = cloneStmt(method.body.get());

                clone->methods.push_back(std::move(m));
            }

            return clone;
        }

        default: {
            diag_.error(templateDecl.location,
                "monomorphization of '{}' is not supported for this declaration kind",
                templateDecl.name);
            emitInstantiationChainNotes(diag_, templateDecl.location,
                                        g_monoInstantiationStack);
            return nullptr;
        }
    }
}

} // namespace vyx
