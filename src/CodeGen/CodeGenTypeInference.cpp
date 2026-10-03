#include "CodeGenIncludes.h"

namespace vyx {

// ============================================================
//  Phase D: inferredType-based helpers
// ============================================================

llvm::Type* CodeGen::inferredPointeeLLVM(const Expr& expr) {
    if (!expr.inferredType) return nullptr;
    const VyxType& t = *expr.inferredType;
    // R5 phase 4: Vec<T> arrives as Class-kind; helper normalizes.
    if (isVecLike(t)) {
        if (auto elem = vecElement(t)) return toLLVMType(*elem);
        return nullptr;
    }
    // R5 phase 4c: Stack / Queue / Set / UnorderedSet are Class-kind too.
    if (isElementContainerLike(t)) {
        if (auto elem = containerElement(t)) return toLLVMType(*elem);
        return nullptr;
    }
    // R5 phase 4d: smart-pointer (Ref / Scope / Box) are Class-kind.
    if (isSmartPtrLike(t)) {
        if (auto inner = smartPtrInner(t)) return toLLVMType(*inner);
        return nullptr;
    }
    switch (t.kind) {
        case VyxTypeKind::Pointer:
        case VyxTypeKind::Reference:
            if (t.pointeeType) return toLLVMType(*t.pointeeType);
            return nullptr;
        case VyxTypeKind::Array:
            if (t.elementType) return toLLVMType(*t.elementType);
            return nullptr;
        default:
            return nullptr;
    }
}

// Produce a mangled type name matching mangleTypeAnnotation's format.
// This is critical: classVarTypes_ stores mangled names (e.g. "Vec<i32>",
// "Dict<string,Foo>" without spaces, "Ref"→"__RefCounted", "Box"→"rawptr"),
// and findClassMethod / structTypes_ are keyed by those mangled names.
//
// R5 final: Option<T> / Result<T,E> arrive here as Class-kind with the
// mangled name already baked in (e.g. `Option<i32>`, `Result<i32,string>`).
// The Class branch below returns `t.name` verbatim, so no dedicated
// Option/Result arms are required.
static std::string mangleVyxTypeForCodegen(const VyxType& t, const LangItemRegistry* langItems) {
    // R5 phase 4d: smart-pointer Class-kind types collapse to their backend
    // aliases — `Ref<T>` / `Scope<T>` share `__RefCounted`, `Box<T>` collapses
    // to `rawptr`. Must run before the Class branch returns `t.name` verbatim,
    // otherwise classVarTypes_ would get keyed on `Ref<i32>` and miss lookups.
    if (isRefLike(t) || isScopeLike(t)) return "__RefCounted";
    if (isBoxLike(t)) return "rawptr";
    switch (t.kind) {
        case VyxTypeKind::Void:    return "void";
        case VyxTypeKind::Bool:    return "bool";
        case VyxTypeKind::Char:    return "char";
        case VyxTypeKind::RawPtr:  return "rawptr";
        case VyxTypeKind::Integer: {
            if (t.isSizeType) return t.isSigned ? "isize" : "usize";
            return (t.isSigned ? "i" : "u") + std::to_string(t.bitWidth);
        }
        case VyxTypeKind::Float:
            return "f" + std::to_string(t.bitWidth);
        case VyxTypeKind::Class:
        case VyxTypeKind::Struct:
        case VyxTypeKind::Interface:
            return t.name;
        // R5 phase 4: Vec<T>, Dict<K,V>, UnorderedMap<K,V>, Stack<T>, Queue<T>,
        // Set<T>, UnorderedSet<T>, Ref<T>, Scope<T>, Box<T> are all Class-kind
        // with mangled names. The smart-pointer aliasing (Ref/Scope →
        // __RefCounted, Box → rawptr) runs above the switch; ordinary
        // containers fall through to the Class branch which returns `t.name`
        // verbatim matching the canonical mangle.
        case VyxTypeKind::ErrorType:
            // Sema currently doesn't attach class-level T args to ErrorType
            // receivers (Option<i64> arrives as ErrorType{name="Option"}). Use
            // the bare name so CodeGen lookups line up with what Mono enqueues
            // for enum method-generic instantiations (`Option.map<U>`).
            // Per-T disambiguation is intentionally lost until Sema starts
            // attaching type args to ErrorType receivers.
            return t.name;
        default:
            return {};
    }
}

std::string CodeGen::inferredInnerTypeName(const Expr& expr) {
    if (!expr.inferredType) return {};
    const VyxType& t = *expr.inferredType;
    // R5 phase 4d: smart-pointer Class-kind.
    if (isSmartPtrLike(t)) {
        if (auto inner = smartPtrInner(t))
            return mangleVyxTypeForCodegen(*inner, langItems_);
        return {};
    }
    switch (t.kind) {
        case VyxTypeKind::Pointer:
        case VyxTypeKind::Reference:
            if (t.pointeeType) return mangleVyxTypeForCodegen(*t.pointeeType, langItems_);
            return {};
        default:
            return {};
    }
}

llvm::Type* CodeGen::inferredContainerElemLLVM(const Expr& expr) {
    if (!expr.inferredType) return nullptr;
    const VyxType& t = *expr.inferredType;
    // R5 phase 4: Vec<T> arrives as Class-kind.
    if (isVecLike(t)) {
        if (auto elem = vecElement(t)) return toLLVMType(*elem);
        return nullptr;
    }
    // R5 phase 4c: Stack / Queue / Set / UnorderedSet are Class-kind too.
    if (isElementContainerLike(t)) {
        if (auto elem = containerElement(t)) return toLLVMType(*elem);
        return nullptr;
    }
    switch (t.kind) {
        case VyxTypeKind::Array:
            if (t.elementType) return toLLVMType(*t.elementType);
            return nullptr;
        default:
            return nullptr;
    }
}

llvm::Type* CodeGen::inferredDictKeyLLVM(const Expr& expr) {
    if (!expr.inferredType) return nullptr;
    const VyxType& t = *expr.inferredType;
    if (isMapLike(t) && t.paramTypes.size() >= 1 && t.paramTypes[0]) {
        return toLLVMType(*t.paramTypes[0]);
    }
    return nullptr;
}

llvm::Type* CodeGen::inferredDictValueLLVM(const Expr& expr) {
    if (!expr.inferredType) return nullptr;
    const VyxType& t = *expr.inferredType;
    if (isMapLike(t) && t.paramTypes.size() >= 2 && t.paramTypes[1]) {
        return toLLVMType(*t.paramTypes[1]);
    }
    return nullptr;
}

std::string CodeGen::resolveRefInner(const Expr& e, const std::string& legacyKey) {
    // Same rationale as resolveClassName: legacy side-map stores mangled
    // names that the rest of codegen depends on.
    if (!legacyKey.empty()) {
        auto it = refInnerTypeNames_.find(legacyKey);
        if (it != refInnerTypeNames_.end()) return it->second;
    }
    return inferredInnerTypeName(e);
}

CodeGen::SideMapSnapshot CodeGen::snapshotVarScopeSideMaps() {
    SideMapSnapshot s;
    s.containerTypes     = containerTypes_;
    s.containerElemTypes = containerElemTypes_;
    s.containerValTypes  = containerValTypes_;
    s.ptrElemTypes       = ptrElemTypes_;
    s.classVarTypes      = classVarTypes_;
    s.interfaceVarTypes  = interfaceVarTypes_;
    s.refInnerTypeNames  = refInnerTypeNames_;
    s.unsignedVars       = unsignedVars_;
    s.volatileVars       = volatileVars_;
    s.closureFatPtrVars  = closureFatPtrVars_;
    return s;
}

void CodeGen::clearVarScopeSideMaps() {
    containerTypes_.clear();
    containerElemTypes_.clear();
    containerValTypes_.clear();
    ptrElemTypes_.clear();
    classVarTypes_.clear();
    interfaceVarTypes_.clear();
    refInnerTypeNames_.clear();
    unsignedVars_.clear();
    volatileVars_.clear();
    closureFatPtrVars_.clear();
}

void CodeGen::restoreVarScopeSideMaps(SideMapSnapshot&& s) {
    containerTypes_     = std::move(s.containerTypes);
    containerElemTypes_ = std::move(s.containerElemTypes);
    containerValTypes_  = std::move(s.containerValTypes);
    ptrElemTypes_       = std::move(s.ptrElemTypes);
    classVarTypes_      = std::move(s.classVarTypes);
    interfaceVarTypes_  = std::move(s.interfaceVarTypes);
    refInnerTypeNames_  = std::move(s.refInnerTypeNames);
    unsignedVars_       = std::move(s.unsignedVars);
    volatileVars_       = std::move(s.volatileVars);
    closureFatPtrVars_  = std::move(s.closureFatPtrVars);
}

std::string CodeGen::resolveClassName(const Expr& e, const std::string& legacyKey) {
    // Phase D: side-map first.
    //
    // classVarTypes_ stores the precise mangled instantiation key expected by
    // findClassMethod / structTypes_ (e.g. "Vec<i32>", "__RefCounted"), and it
    // only records *variable-shaped* bindings – never bare class names.
    //
    // For `Foo.static_method()`, `memberExpr.object` is the identifier "Foo"
    // whose Sema inferredType is Class{name="Foo"}. If we consulted inferred
    // there, we'd mis-classify the call as a class-instance dispatch. Hence:
    //   (1) legacy map first – authoritative for instance variables,
    //   (2) inferred as fallback – for temporaries/chained expressions whose
    //       variable-name key is unavailable, AND only when the identifier is
    //       not also a registered class type (i.e. not a bare class name).
    if (!legacyKey.empty()) {
        auto it = classVarTypes_.find(legacyKey);
        if (it != classVarTypes_.end()) {
            // Special case: `Option<T>` and `Result<T,E>` share the LLVM
            // struct `__Result` (tagged-union layout), so legacy classification
            // collapses every Option/Result variable to "__Result". For
            // method-level generic dispatch (`o.map::<U>()`) Mono enqueues
            // under the canonical Vyx name "Option.map<U>" — `__Result` would
            // never match. Prefer the inferredType's bare canonical name
            // ("Option" / "Result") when available; that matches what Mono's
            // synthetic-FunctionDecl path registered.
            // Also match __Result_N (wide-payload variants for sizeof(T)>8,
            // e.g. __Result_32 for Option<string>).  Mono always registers
            // enum method-generics under the canonical base name "Option" /
            // "Result", so any __Result* class name must fall back the same way.
            auto& cv = it->second;
            bool isResultVariant = (cv == "__Result") ||
                (cv.size() > 9 && cv.compare(0, 9, "__Result_") == 0);
            if (isResultVariant && e.inferredType &&
                (isOptionLike(*e.inferredType) ||
                 e.inferredType->kind == VyxTypeKind::ErrorType)) {
                std::string inferred = inferredClassName(e);
                if (!inferred.empty()) return inferred;
            }
            return it->second;
        }
    }
    // Skip inferred fallback when the identifier is itself a class type name –
    // that indicates `ClassName.method()` static dispatch, not an instance.
    if (e.kind == ExprKind::Identifier) {
        auto& nm = static_cast<const IdentifierExpr&>(e).name;
        if (structTypes_.count(nm)) return {};
    }
    return inferredClassName(e);
}

std::string CodeGen::inferredClassName(const Expr& expr) {
    if (!expr.inferredType) return {};
    // Primitive-target impl support (2026-04-23): inside a monomorphized
    // generic function `h<T>(x: T)` with T=i32, Mono carries the original
    // Generic{name="T"} inferredType through to the instance.  Resolve T
    // against the active genericTypeParamNames_ map so method dispatch on
    // `x.hash()` can find `i32.hash` (etc.) via resolveClassName → "i32".
    if (expr.inferredType->kind == VyxTypeKind::Generic) {
        auto it = genericTypeParamNames_.find(expr.inferredType->name);
        if (it != genericTypeParamNames_.end()) return it->second;
    }
    return mangleVyxTypeForCodegen(*expr.inferredType, langItems_);
}

} // namespace vyx
