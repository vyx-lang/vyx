#include "TemplateResolver.h"
#include "../Common/Diagnostics.h"
#include <sstream>
#include <algorithm>
#include <functional>

namespace vyx {

VyxTypePtr TemplateResolver::substituteType(const TypeAnnotation& type, const GenericSubstitution& subst) {
    // P2-generics C5: Dependent type `T::Item`.
    // The substitution map is pre-populated by `bindDependentTypesForSubst`
    // with keys like "T::Item" -> ConcreteType. Look that up first so that
    // the return type `Option<T::Item>` in a generic function becomes
    // `Option<i64>` after the template is instantiated with T=CountUp.
    if (type.kind == TypeAnnotationKind::Dependent) {
        auto* dt = type.as<DependentType>();
        const std::string key = dt->baseName + "::" + dt->memberName;
        // Direct lookup in the substitution map (key = "T::Item").
        if (subst.isGenericParam(key)) {
            return subst.getConcreteType(key);
        }
        // If the base type itself was substituted (T -> CountUp), try to
        // resolve "CountUp::Item" from the map.
        if (subst.isGenericParam(dt->baseName)) {
            auto concrete = subst.getConcreteType(dt->baseName);
            if (concrete) {
                const std::string concreteKey = concrete->name + "::" + dt->memberName;
                if (subst.isGenericParam(concreteKey)) {
                    return subst.getConcreteType(concreteKey);
                }
            }
        }
        // Not yet resolved: keep as an opaque Generic named "T::Item" so
        // Sema's resolveType (Dependent branch) can resolve it later if a
        // binding is registered in implAssocByTarget_.
        auto t = std::make_shared<VyxType>();
        t->kind = VyxTypeKind::Generic;
        t->name = key;
        return t;
    }

    if (type.kind == TypeAnnotationKind::Named) {
        if (subst.isGenericParam(type.name)) {
            return subst.getConcreteType(type.name);
        }
        auto t = std::make_shared<VyxType>();
        t->kind = VyxTypeKind::Generic;
        t->name = type.name;
        return t;
    }

    if (type.kind == TypeAnnotationKind::Generic) {
        auto* gt = type.as<GenericType>();
        if (gt->typeArgs.empty()) {
            if (subst.isGenericParam(type.name)) {
                return subst.getConcreteType(type.name);
            }
            auto t = std::make_shared<VyxType>();
            t->kind = VyxTypeKind::Generic;
            t->name = type.name;
            return t;
        }

        // First resolve every sub-type via recursion (skipping const-null
        // slots which don't have a TypeAnnotation). `resolvedSubs` is used
        // by the builtin-container preservation branch below.
        std::vector<VyxTypePtr> resolvedSubs;
        resolvedSubs.reserve(gt->typeArgs.size());
        for (auto& sub : gt->typeArgs) {
            if (!sub) { resolvedSubs.push_back(nullptr); continue; }
            resolvedSubs.push_back(substituteType(*sub, subst));
        }

        // Preserve structural VyxTypeKind for known builtin container bases
        // so round-trip substituteType → convertTypeToAnnotation reproduces
        // a typed GenericType AST node (Option<i64>) rather than a bare
        // NamedType("Option<i64>") that Sema can't resolve.
        const std::string& base = type.name;
        // R5 stage 2: Option / Result no longer produced as built-in kinds —
        // stdlib supplies them as generic ErrorDefs, so substitute falls
        // through to the mangled-Class path below. The VyxType overload
        // of toLLVMType in CodeGenCore handles the on-demand ADT layout
        // registration for these user-lang-item enums. Other container
        // builtins still produce dedicated kinds (stage 3 territory).
        if (base == "Vec" && resolvedSubs.size() == 1 && resolvedSubs[0])
            return types::makeDynArray(resolvedSubs[0]);
        if (base == "Ref" && resolvedSubs.size() == 1 && resolvedSubs[0])
            return types::makeRefPtr(resolvedSubs[0]);
        if (base == "Scope" && resolvedSubs.size() == 1 && resolvedSubs[0])
            return types::makeScopePtr(resolvedSubs[0]);
        if (base == "Box" && resolvedSubs.size() == 1 && resolvedSubs[0])
            return types::makeBoxPtr(resolvedSubs[0]);
        if (base == "Stack" && resolvedSubs.size() == 1 && resolvedSubs[0])
            return types::makeStack(resolvedSubs[0]);
        if (base == "Queue" && resolvedSubs.size() == 1 && resolvedSubs[0])
            return types::makeQueue(resolvedSubs[0]);
        if (base == "Set" && resolvedSubs.size() == 1 && resolvedSubs[0])
            return types::makeSet(resolvedSubs[0]);
        if (base == "UnorderedSet" && resolvedSubs.size() == 1 && resolvedSubs[0])
            return types::makeUnorderedSet(resolvedSubs[0]);
        if (base == "Dict" && resolvedSubs.size() == 2 && resolvedSubs[0] && resolvedSubs[1])
            return types::makeDict(resolvedSubs[0], resolvedSubs[1]);
        if (base == "UnorderedMap" && resolvedSubs.size() == 2 && resolvedSubs[0] && resolvedSubs[1])
            return types::makeUnorderedMap(resolvedSubs[0], resolvedSubs[1]);
        if (base == "Delegate" && resolvedSubs.size() == 1 && resolvedSubs[0])
            return types::makeDelegate(resolvedSubs[0]);
        if (base == "Event" && resolvedSubs.size() == 1 && resolvedSubs[0])
            return types::makeEvent(resolvedSubs[0]);

        // Non-builtin / user generic class — fall through to the mangled-name
        // Class kind. typeArgs and argExprs are kept aligned by
        // rewriteConstSizesInType: a null typeArgs[i] means the slot is a
        // const-generic param whose concrete integer is in argExprs[i].
        std::string mangledName = type.name + "<";
        for (size_t i = 0; i < gt->typeArgs.size(); ++i) {
            if (i > 0) mangledName += ",";
            if (!gt->typeArgs[i]) {
                // Const-generic slot: emit the integer from argExprs[i].
                if (i < gt->argExprs.size() && gt->argExprs[i] &&
                    gt->argExprs[i]->kind == ExprKind::IntLiteral) {
                    mangledName += std::to_string(
                        gt->argExprs[i]->as<IntLiteralExpr>()->value);
                } else {
                    // Fallback: keep the slot name if available.
                    mangledName += "?";
                }
            } else {
                auto resolved = substituteType(*gt->typeArgs[i], subst);
                mangledName += resolved ? resolved->toString() : "?";
            }
        }
        mangledName += ">";

        auto t = std::make_shared<VyxType>();
        t->kind = VyxTypeKind::Class;
        t->name = mangledName;
        // Populate paramTypes with the concrete class-level type args so that
        // downstream consumers (Mono::requestMethodInstantiation in particular)
        // can recover the class generic bindings when resolving method-generic
        // calls on a value of this type. Without this, a local bound to
        // `Container::<i64>.new()` carries an empty paramTypes and method-
        // generic turbofish dispatch (c.push_many::<i32>(...)) falls back to
        // the empty request env → no Mono instantiation → "method-generic
        // not found" at CodeGen.
        for (auto& sub : resolvedSubs) {
            t->paramTypes.push_back(sub); // may be null for const-generic slots
        }
        return t;
    }

    if (type.kind == TypeAnnotationKind::Function) {
        auto* ft = type.as<FunctionType>();
        auto fnType = std::make_shared<VyxType>();
        fnType->kind = VyxTypeKind::Function;
        fnType->name = "fn";
        for (const auto& paramType : ft->paramTypes) {
            fnType->paramTypes.push_back(substituteType(*paramType, subst));
        }
        if (ft->returnType) {
            fnType->returnType = substituteType(*ft->returnType, subst);
        }
        return fnType;
    }

    if (type.kind == TypeAnnotationKind::Tuple) {
        auto* tt = type.as<TupleType>();
        if (!tt->elements.empty()) {
            std::vector<VyxTypePtr> elementTypes;
            for (const auto& elemType : tt->elements) {
                elementTypes.push_back(substituteType(*elemType, subst));
            }
            return types::makeTuple(elementTypes);
        }
    }
    
    return substituteComplexType(type, subst);
}

VyxTypePtr TemplateResolver::substituteComplexType(const TypeAnnotation& type, const GenericSubstitution& subst) {
    VyxTypePtr baseType;
    if (subst.isGenericParam(type.name)) {
        baseType = subst.getConcreteType(type.name);
    } else {
        auto t = std::make_shared<VyxType>();
        t->kind = VyxTypeKind::Generic;
        t->name = type.name;
        baseType = t;
    }
    
    VyxTypePtr result = baseType;
    
    if (type.kind == TypeAnnotationKind::Pointer) {
        auto* pt = type.as<PointerType>();
        if (pt->innerType) {
            result = types::makePointer(substituteType(*pt->innerType, subst));
        } else {
            result = types::makePointer(baseType);
        }
    }
    
    if (type.kind == TypeAnnotationKind::Reference) {
        auto* rt = type.as<ReferenceType>();
        if (rt->innerType) {
            result = types::makeReference(substituteType(*rt->innerType, subst), rt->isMutable);
        } else {
            result = types::makeReference(baseType, false);
        }
    }
    
    if (type.kind == TypeAnnotationKind::Array) {
        auto* at = type.as<ArrayType>();
        // Preserve the concrete array size when the annotation's size node
        // is already a plain integer literal. Callers who substitute a
        // const-generic array (e.g. `[N]T` after rewriteConstSizesInType
        // folded N → IntLit) depend on this so the produced VyxType has
        // the correct arraySize — otherwise CodeGen lowers to `[0 x T]`.
        int sz = 0;
        if (at->size && at->size->kind == ExprKind::IntLiteral) {
            sz = static_cast<int>(at->size->as<IntLiteralExpr>()->value);
        }
        if (at->elementType) {
            result = types::makeArray(substituteType(*at->elementType, subst), sz);
        } else {
            result = types::makeArray(baseType, sz);
        }
    }
    
    return result;
}

ParamDecl TemplateResolver::substituteParam(const ParamDecl& param, const GenericSubstitution& subst) {
    ParamDecl newParam;
    newParam.name = param.name;
    newParam.isMutRef = param.isMutRef;
    
    if (param.type) {
        VyxTypePtr substitutedType = substituteType(*param.type, subst);
        
        if (substitutedType->kind == VyxTypeKind::Pointer) {
            auto ptrAnn = std::make_unique<PointerType>();
            ptrAnn->location = SourceLocation{};
            if (substitutedType->pointeeType) {
                auto inner = std::make_unique<NamedType>();
                inner->name = substitutedType->pointeeType->name;
                ptrAnn->innerType = std::move(inner);
            }
            newParam.type = std::move(ptrAnn);
        } else if (substitutedType->kind == VyxTypeKind::Reference) {
            auto refAnn = std::make_unique<ReferenceType>();
            refAnn->location = SourceLocation{};
            refAnn->isMutable = substitutedType->isMutable;
            if (substitutedType->pointeeType) {
                auto inner = std::make_unique<NamedType>();
                inner->name = substitutedType->pointeeType->name;
                refAnn->innerType = std::move(inner);
            }
            newParam.type = std::move(refAnn);
        } else if (substitutedType->kind == VyxTypeKind::Array) {
            auto arrAnn = std::make_unique<ArrayType>();
            arrAnn->location = SourceLocation{};
            if (substitutedType->elementType) {
                auto inner = std::make_unique<NamedType>();
                inner->name = substitutedType->elementType->name;
                arrAnn->elementType = std::move(inner);
            }
            newParam.type = std::move(arrAnn);
        } else if (param.type->kind == TypeAnnotationKind::Generic) {
            // Preserve Generic structure (e.g. `Container<T>` → `Container<i64>`)
            // when substituting a class/struct/interface instantiation. The
            // base name + concrete type-args are needed downstream so CodeGen
            // can find per-instantiation vtables / structs (or fall back to
            // the base-name vtable for interfaces). Collapsing to NamedType
            // would lose the args and reduce dispatch to bare base lookup.
            auto* origGT = param.type->as<GenericType>();
            auto out = std::make_unique<GenericType>();
            out->location = SourceLocation{};
            out->name = origGT->name;
            for (auto& ta : origGT->typeArgs) {
                if (!ta) { out->typeArgs.push_back(nullptr); continue; }
                if (ta->kind == TypeAnnotationKind::Named &&
                    subst.isGenericParam(ta->name)) {
                    auto bound = subst.getConcreteType(ta->name);
                    auto na = std::make_unique<NamedType>();
                    na->name = bound ? bound->name : ta->name;
                    out->typeArgs.push_back(std::move(na));
                } else {
                    auto na = std::make_unique<NamedType>();
                    na->name = ta->name;
                    out->typeArgs.push_back(std::move(na));
                }
            }
            newParam.type = std::move(out);
        } else {
            auto ann = std::make_unique<NamedType>();
            ann->name = substitutedType->name;
            ann->location = SourceLocation{};
            newParam.type = std::move(ann);
        }
    }

    return newParam;
}

namespace {

// Map a bare name to a builtin VyxType (by kind), or nullptr if not builtin.
VyxTypePtr builtinFromName(const std::string& n) {
    if (n == "void")   return types::makeVoid();
    if (n == "bool")   return types::makeBool();
    if (n == "char")   return types::makeChar();
    if (n == "str")    return types::makeString();
    if (n == "string") return types::makeString();
    if (n == "rawptr") return types::makeRawPtr();
    if (n == "i8")     return types::makeInt(8,  true);
    if (n == "i16")    return types::makeInt(16, true);
    if (n == "i32")    return types::makeInt(32, true);
    if (n == "i64")    return types::makeInt(64, true);
    if (n == "u8")     return types::makeInt(8,  false);
    if (n == "u16")    return types::makeInt(16, false);
    if (n == "u32")    return types::makeInt(32, false);
    if (n == "u64")    return types::makeInt(64, false);
    if (n == "isize")  return types::makeISize();
    if (n == "usize")  return types::makeUSize();
    if (n == "f32")    return types::makeFloat(32);
    if (n == "f64")    return types::makeFloat(64);
    return nullptr;
}

} // namespace

VyxTypePtr TemplateResolver::resolveTypeAnnotation(const TypeAnnotation& ann,
                                                    const std::vector<std::string>* genericParams) {
    auto isGenericName = [&](const std::string& n) -> bool {
        if (!genericParams) return false;
        return std::find(genericParams->begin(), genericParams->end(), n) != genericParams->end();
    };

    switch (ann.kind) {
        case TypeAnnotationKind::Named: {
            if (isGenericName(ann.name)) return types::makeGeneric(ann.name);
            if (auto bt = builtinFromName(ann.name)) return bt;
            // Nominal user type. Parking as Class — Sema will reconcile
            // Struct/Class/Interface/ErrorType via its symbol table.
            return types::makeClass(ann.name);
        }
        case TypeAnnotationKind::Pointer: {
            auto* pt = ann.as<PointerType>();
            auto inner = pt->innerType ? resolveTypeAnnotation(*pt->innerType, genericParams)
                                       : types::makeUnknown();
            return types::makePointer(inner);
        }
        case TypeAnnotationKind::Reference: {
            auto* rt = ann.as<ReferenceType>();
            auto inner = rt->innerType ? resolveTypeAnnotation(*rt->innerType, genericParams)
                                       : types::makeUnknown();
            return types::makeReference(inner, rt->isMutable);
        }
        case TypeAnnotationKind::Array: {
            auto* at = ann.as<ArrayType>();
            auto inner = at->elementType ? resolveTypeAnnotation(*at->elementType, genericParams)
                                         : types::makeUnknown();
            return types::makeArray(inner, 0);
        }
        case TypeAnnotationKind::Tuple: {
            auto* tt = ann.as<TupleType>();
            std::vector<VyxTypePtr> elems;
            elems.reserve(tt->elements.size());
            for (auto& e : tt->elements) {
                elems.push_back(e ? resolveTypeAnnotation(*e, genericParams) : types::makeUnknown());
            }
            return types::makeTuple(elems);
        }
        case TypeAnnotationKind::Function: {
            auto* ft = ann.as<FunctionType>();
            auto fn = std::make_shared<VyxType>();
            fn->kind = VyxTypeKind::Function;
            fn->name = "fn";
            fn->paramTypes.reserve(ft->paramTypes.size());
            for (auto& p : ft->paramTypes) {
                fn->paramTypes.push_back(p ? resolveTypeAnnotation(*p, genericParams) : types::makeUnknown());
            }
            fn->returnType = ft->returnType ? resolveTypeAnnotation(*ft->returnType, genericParams)
                                            : types::makeVoid();
            return fn;
        }
        case TypeAnnotationKind::Generic: {
            auto* gt = ann.as<GenericType>();
            if (gt->typeArgs.empty() && isGenericName(ann.name))
                return types::makeGeneric(ann.name);
            std::vector<VyxTypePtr> args;
            args.reserve(gt->typeArgs.size());
            for (auto& a : gt->typeArgs) {
                args.push_back(a ? resolveTypeAnnotation(*a, genericParams) : types::makeUnknown());
            }
            // Map well-known container bases to their kinds; everything else
            // becomes a Class whose name is the mangled instantiation.
            const std::string& base = ann.name;
            if (base == "Vec" && args.size() == 1) return types::makeDynArray(args[0]);
            // R5 stage 2: Option/Result no longer produce builtin kinds; they
            // flow through the user-generic-enum path below and are recognized
            // by lang_item registry + @[lang_item("option"/"result")] on stdlib.
            if (base == "Ref" && args.size() == 1) return types::makeRefPtr(args[0]);
            if (base == "Scope" && args.size() == 1) return types::makeScopePtr(args[0]);
            if (base == "Box" && args.size() == 1) return types::makeBoxPtr(args[0]);
            if (base == "Stack" && args.size() == 1) return types::makeStack(args[0]);
            if (base == "Queue" && args.size() == 1) return types::makeQueue(args[0]);
            if (base == "Set" && args.size() == 1) return types::makeSet(args[0]);
            if (base == "UnorderedSet" && args.size() == 1) return types::makeUnorderedSet(args[0]);
            if (base == "Dict" && args.size() == 2) return types::makeDict(args[0], args[1]);
            if (base == "UnorderedMap" && args.size() == 2) return types::makeUnorderedMap(args[0], args[1]);
            if (base == "Delegate" && args.size() == 1) return types::makeDelegate(args[0]);
            if (base == "Event" && args.size() == 1) return types::makeEvent(args[0]);
            // Fallback: user-defined generic class. Mangled name via canonical mangleGeneric.
            // Populate paramTypes so Mono::requestMethodInstantiation can recover
            // the class-level type args for method-generic dispatch
            // (e.g. c.push_many::<i32>() where c: Container<i64>).
            auto userClass = types::makeClass(mangleGeneric(base, args));
            userClass->paramTypes = std::move(args);
            return userClass;
        }
        case TypeAnnotationKind::Union: {
            auto* ut = ann.as<UnionType>();
            std::vector<VyxTypePtr> mems;
            mems.reserve(ut->members.size());
            for (auto& m : ut->members) {
                mems.push_back(m ? resolveTypeAnnotation(*m, genericParams) : types::makeUnknown());
            }
            return types::makeUnion(mems);
        }
        case TypeAnnotationKind::Dependent: {
            // P2-generics C5: `T::Item` in structural type annotation form.
            // Return a Generic placeholder named "T::Item" — callers who
            // have a full substitution map will replace this via
            // `substituteType`; others see it as an opaque unbound param.
            auto* dt = ann.as<DependentType>();
            return types::makeGeneric(dt->baseName + "::" + dt->memberName);
        }
        case TypeAnnotationKind::PackIndex: {
            // P2 C6: variadic type-pack indexing should be erased by
            // Sema before any structural type resolution. Surfacing
            // here means an `Ts[i]` slipped past expansion (e.g.
            // referenced from a non-variadic context). Park as
            // Generic so callers see "unbound" semantics rather than
            // a silent Unknown — `isUnboundGeneric` will then defer
            // mono and Sema downstream will report a clearer error.
            auto* pi = ann.as<PackIndexType>();
            return types::makeGeneric(pi->packName + "[?]");
        }
    }
    return types::makeUnknown();
}

std::string TemplateResolver::mangleTemplateName(const std::string& baseName, const std::vector<VyxTypePtr>& argTypes) {
    // Forward to the canonical mangler in Type.h. This function is kept as a
    // thin shim for legacy call sites; new code should call `mangleGeneric`
    // directly. The old implementation used `type->name` which was unsafe —
    // it produced `Vec<i64>` as the bare name of a nested class type,
    // leading to double-mangle artefacts like `Outer<Vec<i64>>` getting
    // flattened by naive substring handling downstream. `mangleGeneric` uses
    // structural `type->mangle()` on each argument which is guaranteed
    // idempotent and pack-aware.
    return mangleGeneric(baseName, argTypes);
}

GenericSubstitution TemplateResolver::inferGenericTypes(
    const std::vector<std::string>& genericParams,
    const std::vector<ParamDecl>& declaredParams,
    [[maybe_unused]] const std::vector<ExprPtr>& callArgs,
    const std::vector<VyxTypePtr>& argTypes
) {
    GenericSubstitution subst;

    // Helper: try to infer generic param bindings by structurally matching
    // a declared-param TypeAnnotation against a concrete call-site VyxType.
    // Handles:
    //   Named("T")               → direct binding T → argType
    //   GenericType("Option",[T]) with argType Optional{pointeeType}
    //   GenericType("Vec",[T])   with argType DynArray{elementType}
    //   GenericType("Result",[T,E]) with argType Result{okType,errType}
    //   GenericType("Ref",[T])   with argType RefPtr{pointeeType}
    //   GenericType("Scope",[T]) with argType ScopePtr{pointeeType}
    //   GenericType("Box",[T])   with argType BoxPtr{pointeeType}
    // This enables inference from `fn first_or<T>(v: Option<T>, ...) -> T`
    // called without explicit turbofish when the caller passes an Option<i64>.
    std::function<void(const TypeAnnotation*, const VyxTypePtr&)> matchAndBind;
    matchAndBind = [&](const TypeAnnotation* ann, const VyxTypePtr& argType) {
        if (!ann || !argType) return;
        if (ann->kind == TypeAnnotationKind::Named) {
            if (std::find(genericParams.begin(), genericParams.end(), ann->name)
                    != genericParams.end()) {
                if (!subst.isGenericParam(ann->name))
                    subst.add(ann->name, argType);
            }
            return;
        }
        if (ann->kind == TypeAnnotationKind::Generic) {
            auto* gt = ann->as<GenericType>();
            // Single-arg container types: unwrap both the annotation and the
            // concrete argType to extract the inner T.
            if (gt->typeArgs.size() == 1) {
                VyxTypePtr inner;
                // After R5, Option<T> is Class-kind with paramTypes[0]=T; the
                // smart-pointer factories (Ref/Scope/Box) still carry T in
                // pointeeType until their own migration. Handle each per-shape.
                if (gt->name == "Option" && !argType->paramTypes.empty())
                    inner = argType->paramTypes[0];
                else if ((gt->name == "Ref" || gt->name == "Scope" ||
                          gt->name == "Box") && argType->pointeeType)
                    inner = argType->pointeeType;
                else if (gt->name == "Vec")
                    inner = vecElement(*argType);
                if (inner)
                    matchAndBind(gt->typeArgs[0].get(), inner);
            }
            // Two-arg container types: Result<T,E>
            // After R5 the Result factory produces Class-kind with paramTypes
            // populated; the old okType/errType-specific slots are gone.
            if (gt->typeArgs.size() == 2 && gt->name == "Result" &&
                argType->paramTypes.size() >= 2) {
                matchAndBind(gt->typeArgs[0].get(), argType->paramTypes[0]);
                matchAndBind(gt->typeArgs[1].get(), argType->paramTypes[1]);
            }
            // Dict<K,V>
            if (gt->typeArgs.size() == 2 && gt->name == "Dict" &&
                argType->paramTypes.size() >= 2) {
                matchAndBind(gt->typeArgs[0].get(), argType->paramTypes[0]);
                matchAndBind(gt->typeArgs[1].get(), argType->paramTypes[1]);
            }
        }
    };

    for (size_t paramIdx = 0; paramIdx < declaredParams.size() && paramIdx < argTypes.size(); ++paramIdx) {
        const auto& declParam = declaredParams[paramIdx];
        const auto& argType = argTypes[paramIdx];
        if (declParam.type)
            matchAndBind(declParam.type.get(), argType);
    }

    return subst;
}

} // namespace vyx
