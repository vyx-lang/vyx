#include "CodeGenIncludes.h"

namespace vyx {

llvm::Type* CodeGen::toLLVMType(const VyxType& type) {
    switch (type.kind) {
        case VyxTypeKind::Void:
            return llvm::Type::getVoidTy(*context_);
        case VyxTypeKind::Bool:
            return llvm::Type::getInt8Ty(*context_);
        case VyxTypeKind::Char:
            return llvm::Type::getInt32Ty(*context_);
        case VyxTypeKind::Integer:
            return llvm::Type::getIntNTy(*context_, type.bitWidth);
        case VyxTypeKind::Float:
            return type.bitWidth == 32
                ? llvm::Type::getFloatTy(*context_)
                : llvm::Type::getDoubleTy(*context_);
        case VyxTypeKind::RawPtr:
            return llvm::PointerType::getUnqual(*context_);
        case VyxTypeKind::Pointer:
        case VyxTypeKind::Reference:
            return llvm::PointerType::getUnqual(*context_);
        case VyxTypeKind::Function:
            // Uniform fn-value ABI: function-typed values lower to an opaque
            // handle pointer. Capturing closures and wrapped bare functions
            // both materialize this as a heap {fn_ptr, env_ptr} fat pointer.
            return llvm::PointerType::getUnqual(*context_);
        case VyxTypeKind::Array:
            if (type.elementType) {
                return llvm::ArrayType::get(toLLVMType(*type.elementType), type.arraySize);
            }
            diag_.error(codegenInternalSourceLocation(),
                "internal: array type `{}` has no element type in LLVM lowering", type.toString());
            return llvm::PointerType::getUnqual(*context_);
        case VyxTypeKind::Union: {
            auto* tagTy = llvm::Type::getInt8Ty(*context_);
            auto* valTy = llvm::Type::getInt64Ty(*context_);
            return llvm::StructType::get(*context_, {tagTy, valTy});
        }
        case VyxTypeKind::Struct:
        case VyxTypeKind::Class:
        case VyxTypeKind::ErrorType:
        case VyxTypeKind::Interface: {
            // R5 phase 4d: smart-pointer class names route to the dedicated
            // LLVM layouts, NOT to the generic structTypes_ registry. Ref<T>
            // and Scope<T> share the `__RefCounted {ptr, refcount}` struct;
            // Box<T> lowers to an opaque pointer.  This MUST run before the
            // structTypes_ lookup below — otherwise an accidentally-registered
            // `Ref<i32>` class-body struct would shadow the runtime layout.
            if (type.kind == VyxTypeKind::Class) {
                if (isRefLike(type) || isScopeLike(type)) {
                    auto rcIt = structTypes_.find("__RefCounted");
                    if (rcIt != structTypes_.end()) return rcIt->second;
                    auto* ty = llvm::StructType::create(*context_,
                        {llvm::PointerType::getUnqual(*context_),
                         llvm::Type::getInt64Ty(*context_)},
                        "__RefCounted");
                    structTypes_["__RefCounted"] = ty;
                    return ty;
                }
                if (isBoxLike(type)) {
                    return llvm::PointerType::getUnqual(*context_);
                }
                // Note: `string` / `__String` no longer need a special case
                // here. `generateRegisterErrorEnums` now eagerly installs
                // the runtime `__String` layout under both keys during
                // CodeGen bootstrap, so the `structTypes_.find(type.name)`
                // lookup below succeeds for `Class{name="string"}` without
                // any hardcoded name match at this site.
            }
            // Interface types (e.g. `dyn Job`, `dyn Shape`) lower to their
            // `__iface_X` fat-pointer struct ({data_ptr, vtable_ptr}). Without
            // this hop the bare name `Job` misses in structTypes_ (only
            // `__iface_Job` / `Job_vtable` are registered) and we fall through
            // to the opaque-pointer fallback, losing 8 bytes per value —
            // enough to break `Vec<Box<dyn Job>>.get(i).deref().run()` where
            // the chain-call deref path uses toLLVMType to size the load.
            //
            // Sema sometimes hands us a Class-kind `VyxType{name="Renderer"}`
            // for `dyn Renderer` (the parser strips `dyn` and the name flows
            // through as a bare class reference). Route BOTH Interface-kind
            // and Class-kind to the iface struct when a matching
            // `__iface_X` / `X_vtable` pair exists — the combination is
            // unambiguous because the iface registry is only populated by
            // generateCreateInterfaceVtables from actual InterfaceDecl nodes.
            if (type.kind == VyxTypeKind::Interface ||
                (type.kind == VyxTypeKind::Class &&
                 structTypes_.count(type.name + "_vtable"))) {
                auto ifIt = structTypes_.find("__iface_" + type.name);
                if (ifIt != structTypes_.end()) return ifIt->second;
            }
            auto it = structTypes_.find(type.name);
            if (it != structTypes_.end()) return it->second;
            if (type.name.find('<') != std::string::npos) {
                auto stIt2 = structTypes_.find(type.name);
                if (stIt2 != structTypes_.end()) return stIt2->second;
                // R5 stage 2: delegate to the TypeAnnotation overload's
                // on-demand user-generic-enum registration path. When
                // TemplateResolver produces `Class{name="Option<i32>"}`
                // (no longer the built-in Optional kind), this VyxType
                // overload would otherwise miss the lowering. Synthesise
                // a GenericType annotation from the mangled name and
                // forward — the annotation path knows how to find the
                // stdlib-registered lang-item enum, register the ADT
                // struct, and alias it to the shared __Result layout.
                {
                    auto lt = type.name.find('<');
                    std::string base = type.name.substr(0, lt);
                    if (unit_) {
                        for (auto& d : unit_->declarations) {
                            if (d && d->kind == DeclKind::ErrorDef &&
                                d->name == base && !d->genericParams.empty()) {
                                // Build a GenericType annotation mirroring
                                // the mangled shape. Recover each arg by
                                // treating the existing paramTypes (set by
                                // TemplateResolver's user-class fallback
                                // path). If paramTypes is empty, bail so
                                // the scan-gap error below still fires.
                                if (type.paramTypes.size() == d->genericParams.size()) {
                                    auto gt = std::make_unique<GenericType>();
                                    gt->name = base;
                                    gt->location = codegenInternalSourceLocation();
                                    for (auto& pt : type.paramTypes) {
                                        if (pt) {
                                            auto tn = std::make_unique<NamedType>();
                                            tn->name = pt->mangle();
                                            tn->location = gt->location;
                                            gt->typeArgs.push_back(std::move(tn));
                                        } else {
                                            gt->typeArgs.push_back(nullptr);
                                        }
                                    }
                                    auto* lowered = toLLVMType(*gt);
                                    if (lowered) return lowered;
                                }
                                break;
                            }
                        }
                    }
                }
                // Recovery: a stray `Prim<>` mangled shell (primitive name
                // with empty typeArgs — produced by a Mono substitution
                // path that wrapped a primitive as a zero-arg Generic).
                // Strict model: flag the upstream bug loudly but keep the
                // recovery so later walks can surface more gaps.
                if (type.name.size() >= 3 && type.name.back() == '>' &&
                    type.name[type.name.size() - 2] == '<') {
                    std::string bare = type.name.substr(0, type.name.size() - 2);
                    auto collapsePrim = [&]() -> llvm::Type* {
                        if (bare == "bool")   return llvm::Type::getInt8Ty(*context_);
                        if (bare == "char")   return llvm::Type::getInt32Ty(*context_);
                        if (bare == "i8" || bare == "u8")   return llvm::Type::getInt8Ty(*context_);
                        if (bare == "i16" || bare == "u16") return llvm::Type::getInt16Ty(*context_);
                        if (bare == "i32" || bare == "u32") return llvm::Type::getInt32Ty(*context_);
                        if (bare == "i64" || bare == "u64") return llvm::Type::getInt64Ty(*context_);
                        if (bare == "f32")    return llvm::Type::getFloatTy(*context_);
                        if (bare == "f64")    return llvm::Type::getDoubleTy(*context_);
                        if (bare == "rawptr") return llvm::PointerType::getUnqual(*context_);
                        return nullptr;
                    };
                    if (auto* recovered = collapsePrim()) {
                        diag_.error(codegenInternalSourceLocation(),
                            "codegen: primitive `{}` wrapped as zero-arg generic `{}` "
                            "reached toLLVMType (upstream Mono substitution bug)",
                            bare, type.name);
                        hadHardTypeError_ = true;
                        return recovered;
                    }
                }
                // Stringified function type (`fn(A,B)->R`) that bled into a
                // nominal VyxType name slot. Legitimate recovery: callable
                // types are truly opaque at LLVM level. Stay silent.
                if (type.name.size() >= 4 && type.name.starts_with("fn(")) {
                    return llvm::PointerType::getUnqual(*context_);
                }
                if (unreachableTemplateBodyWalk_)
                    return llvm::PointerType::getUnqual(*context_);
                diag_.error(codegenInternalSourceLocation(),
                    "codegen: type `{}` is not lowered; Mono scan gap or unresolved generic", type.name);
                hadHardTypeError_ = true;
            }
            if (type.kind == VyxTypeKind::ErrorType)
                return llvm::Type::getInt32Ty(*context_);
            return llvm::PointerType::getUnqual(*context_);
        }
        case VyxTypeKind::Generic:
            return llvm::Type::getInt64Ty(*context_);
        default:
            diag_.error(codegenInternalSourceLocation(), "cannot map type '{}' to LLVM type", type.toString());
            hadHardTypeError_ = true;
            return llvm::Type::getInt32Ty(*context_);
    }
}

std::string CodeGen::mangleTypeAnnotation(const TypeAnnotation& ann) {
    // Backend-name aliases (`Ref`/`Scope` → `__RefCounted`, `Box` → `rawptr`)
    // apply at the TOP level only so bare `Ref<T>` / `Box<T>` parameter types
    // resolve to their LLVM runtime struct / rawptr directly. NESTED positions
    // inside a generic (like `Vec<Box<T>>` or `Dict<K, Ref<V>>`) must preserve
    // the canonical spelling, because Mono's VyxType::mangle() registers the
    // container under the canonical name (`Vec<Box<T>>`, NOT `Vec<rawptr>`).
    // Without this split, a call site lowers to `Vec<rawptr>` and CodeGen
    // misses the Mono-registered entry entirely ("scan gap" error).
    if (ann.name == "Ref" || ann.name == "Scope") return "__RefCounted";
    if (ann.name == "Box") return "rawptr";
    auto& subTypes = getTypeSubTypes(ann);
    if (subTypes.empty()) return ann.name;
    // Guard: if ann.name is already a fully-mangled generic string (e.g.
    // `Vec<i64>`), an upstream pass must have stuffed the mangled form into
    // `name` while also keeping `subTypes` in parallel. Concatenation in that
    // case would yield `Vec<i64><i64>` whose inner slice becomes the bogus
    // `i64><i64` hitting `getOrCreateGenericStructType`.
    if (ann.name.size() >= 2 &&
        ann.name.find('<') != std::string::npos &&
        ann.name.back() == '>') {
        return ann.name;
    }
    std::string result = ann.name + "<";
    for (size_t i = 0; i < subTypes.size(); ++i) {
        if (i > 0) result += ",";
        if (subTypes[i]) result += mangleTypeAnnotationNested(*subTypes[i]);
    }
    result += ">";
    return result;
}

// Nested variant: does NOT apply top-level backend translation. Used for
// every recursive step inside a generic mangle so `Vec<Box<T>>` / `Dict<K,
// Ref<V>>` keep canonical form — aligning with Mono's VyxType::mangle()
// which also emits `Box<T>` / `Ref<V>` in nested positions.
std::string CodeGen::mangleTypeAnnotationNested(const TypeAnnotation& ann) {
    // Function types: emit the canonical `fn(p0,p1,...)->R` form that
    // VyxType::mangle() uses for VyxTypeKind::Function. Without this,
    // the generic fall-through below would wrap `fn(i32)->i32` as
    // `fn<i32>` (which no Mono pass registers) and miss the container
    // lookup for `Vec<fn(i32)->i32>`.
    if (ann.kind == TypeAnnotationKind::Function) {
        auto& ft = static_cast<const FunctionType&>(ann);
        std::string r = "fn(";
        for (size_t i = 0; i < ft.paramTypes.size(); ++i) {
            if (i > 0) r += ",";
            if (ft.paramTypes[i]) r += mangleTypeAnnotationNested(*ft.paramTypes[i]);
            else r += "?";
        }
        r += ")->";
        r += ft.returnType ? mangleTypeAnnotationNested(*ft.returnType) : "void";
        return r;
    }
    auto& subTypes = getTypeSubTypes(ann);
    if (subTypes.empty()) return ann.name;
    if (ann.name.size() >= 2 &&
        ann.name.find('<') != std::string::npos &&
        ann.name.back() == '>') {
        return ann.name;
    }
    std::string result = ann.name + "<";
    for (size_t i = 0; i < subTypes.size(); ++i) {
        if (i > 0) result += ",";
        if (subTypes[i]) result += mangleTypeAnnotationNested(*subTypes[i]);
    }
    result += ">";
    return result;
}

void CodeGen::trackParamType(const ParamDecl& p) {
    if (!p.type) return;

    // Callable-type parameters (`fn(T,...) -> R`) must not walk the generic
    // class instantiation path: `FunctionType::name` is literally "fn" and
    // `getTypeSubTypes` returns the parameter-type list, which would
    // otherwise be mangled into a bogus `fn<T,...>` name and handed to
    // `getOrCreateGenericStructType`.
    if (p.type->kind == TypeAnnotationKind::Function) return;

    std::string typeName = p.type->name;
    auto& ptSubs = getTypeSubTypes(*p.type);
    if (!ptSubs.empty()) {
        typeName = buildMangledClassName(typeName, ptSubs);
    }

    if (p.type->kind == TypeAnnotationKind::Reference && p.type->as<ReferenceType>()->innerType) {
        typeName = p.type->as<ReferenceType>()->innerType->name;
        auto& innerSubs = getTypeSubTypes(*p.type->as<ReferenceType>()->innerType);
        if (!innerSubs.empty()) {
            typeName = buildMangledClassName(typeName, innerSubs);
        }
    }

    if ((p.type->name == "Ref" || p.type->name == "Scope" ||
         p.type->name == "Box") &&
        p.type->kind == TypeAnnotationKind::Generic) {
        auto& refSubs = static_cast<const GenericType&>(*p.type).typeArgs;
        if (!refSubs.empty() && refSubs[0]) {
            refInnerTypeNames_[p.name] = mangleTypeAnnotationNested(*refSubs[0]);
        }
    }

    if (p.type->name == "Vec" && p.type->kind == TypeAnnotationKind::Generic) {
        auto& vecSubs = static_cast<const GenericType&>(*p.type).typeArgs;
        if (!vecSubs.empty() && vecSubs[0]) {
            containerElemTypes_[p.name] = toLLVMType(*vecSubs[0]);
        }
    } else if (p.type->name == "Dict" && p.type->kind == TypeAnnotationKind::Generic) {
        auto& dictSubs = static_cast<const GenericType&>(*p.type).typeArgs;
        if (dictSubs.size() >= 1 && dictSubs[0])
            containerElemTypes_[p.name] = toLLVMType(*dictSubs[0]);
        if (dictSubs.size() >= 2 && dictSubs[1])
            containerValTypes_[p.name] = toLLVMType(*dictSubs[1]);
    }

    if (typeName.find('<') != std::string::npos) {
        // TODO(P1c-C): Mono scan gap — if the type is not yet in structTypes_
        // here, Mono failed to emit it before CodeGen; no on-demand creation.
        classVarTypes_[p.name] = typeName;
        auto ltPos = typeName.find('<');
        std::string baseName = typeName.substr(0, ltPos);
        containerTypes_[p.name] = baseName;
    } else if (typeName == "Vec" || typeName == "Dict" || typeName == "Set" ||
               typeName == "Stack" || typeName == "Queue") {
        containerTypes_[p.name] = typeName;
    }

    if (auto* stTy = llvm::dyn_cast_or_null<llvm::StructType>(
            structTypes_.count(typeName) ? structTypes_[typeName] : nullptr)) {
        if (stTy->hasName())
            classVarTypes_[p.name] = stTy->getName().str();
    }
}

} // namespace vyx
