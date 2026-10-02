#include "CodeGenIncludes.h"
#include "../Sema/TemplateResolver.h"
#include <map>

namespace vyx {

llvm::Type* CodeGen::toLLVMType(const TypeAnnotation& ann) {
    static const std::map<std::string, std::function<llvm::Type*(CodeGen*)>> builtinMap = {
        {"void",   [](CodeGen* cg) { return llvm::Type::getVoidTy(*cg->context_); }},
        {"bool",   [](CodeGen* cg) { return llvm::Type::getInt8Ty(*cg->context_); }},
        {"i8",     [](CodeGen* cg) { return llvm::Type::getInt8Ty(*cg->context_); }},
        {"i16",    [](CodeGen* cg) { return llvm::Type::getInt16Ty(*cg->context_); }},
        {"i32",    [](CodeGen* cg) { return llvm::Type::getInt32Ty(*cg->context_); }},
        {"i64",    [](CodeGen* cg) { return llvm::Type::getInt64Ty(*cg->context_); }},
        {"u8",     [](CodeGen* cg) { return llvm::Type::getInt8Ty(*cg->context_); }},
        {"u16",    [](CodeGen* cg) { return llvm::Type::getInt16Ty(*cg->context_); }},
        {"u32",    [](CodeGen* cg) { return llvm::Type::getInt32Ty(*cg->context_); }},
        {"u64",    [](CodeGen* cg) { return llvm::Type::getInt64Ty(*cg->context_); }},
        {"isize",  [](CodeGen* cg) { return llvm::Type::getInt64Ty(*cg->context_); }},
        {"usize",  [](CodeGen* cg) { return llvm::Type::getInt64Ty(*cg->context_); }},
        {"f32",    [](CodeGen* cg) { return llvm::Type::getFloatTy(*cg->context_); }},
        {"f64",    [](CodeGen* cg) { return llvm::Type::getDoubleTy(*cg->context_); }},
        {"char",   [](CodeGen* cg) { return llvm::Type::getInt32Ty(*cg->context_); }},
        {"str",    [](CodeGen* cg) { return static_cast<llvm::Type*>(cg->getOrCreateStringType()); }},
        {"string", [](CodeGen* cg) { return static_cast<llvm::Type*>(cg->getOrCreateStringType()); }},
        {"__String", [](CodeGen* cg) { return static_cast<llvm::Type*>(cg->getOrCreateStringType()); }},
        {"rawptr", [](CodeGen* cg) { return llvm::PointerType::getUnqual(*cg->context_); }},
        {"__RefCounted", [](CodeGen* cg) {
            auto it = cg->structTypes_.find("__RefCounted");
            if (it != cg->structTypes_.end()) return static_cast<llvm::Type*>(it->second);
            auto* ptrTy = llvm::PointerType::getUnqual(*cg->context_);
            auto* i64Ty = llvm::Type::getInt64Ty(*cg->context_);
            auto* ty = llvm::StructType::create(*cg->context_, {ptrTy, i64Ty}, "__RefCounted");
            cg->structTypes_["__RefCounted"] = ty;
            return static_cast<llvm::Type*>(ty);
        }},
        {"f32x4", [](CodeGen* cg) { return static_cast<llvm::Type*>(llvm::FixedVectorType::get(llvm::Type::getFloatTy(*cg->context_), 4)); }},
        {"f32x8", [](CodeGen* cg) { return static_cast<llvm::Type*>(llvm::FixedVectorType::get(llvm::Type::getFloatTy(*cg->context_), 8)); }},
        {"f64x2", [](CodeGen* cg) { return static_cast<llvm::Type*>(llvm::FixedVectorType::get(llvm::Type::getDoubleTy(*cg->context_), 2)); }},
        {"f64x4", [](CodeGen* cg) { return static_cast<llvm::Type*>(llvm::FixedVectorType::get(llvm::Type::getDoubleTy(*cg->context_), 4)); }},
        {"i32x4", [](CodeGen* cg) { return static_cast<llvm::Type*>(llvm::FixedVectorType::get(llvm::Type::getInt32Ty(*cg->context_), 4)); }},
        {"i32x8", [](CodeGen* cg) { return static_cast<llvm::Type*>(llvm::FixedVectorType::get(llvm::Type::getInt32Ty(*cg->context_), 8)); }},
        {"i64x2", [](CodeGen* cg) { return static_cast<llvm::Type*>(llvm::FixedVectorType::get(llvm::Type::getInt64Ty(*cg->context_), 2)); }},
        {"i64x4", [](CodeGen* cg) { return static_cast<llvm::Type*>(llvm::FixedVectorType::get(llvm::Type::getInt64Ty(*cg->context_), 4)); }},
    };

    if (ann.kind == TypeAnnotationKind::Named || ann.kind == TypeAnnotationKind::Generic) {
        if (ann.name == "_") return llvm::Type::getInt32Ty(*context_);
        if (ann.name == "Any") return getOrCreateAnyType();
        // Stringified function signature (`fn(...)->R`) bled into a
        // NamedType.name slot by some Mono substitution path. Callable
        // types lower to an opaque pointer; precise dispatch is handled
        // at call sites. Short-circuit before the regular class/struct
        // lookups or we'd misinterpret the name as a nominal type.
        if (ann.name.size() >= 4 && ann.name.starts_with("fn(")) {
            return llvm::PointerType::getUnqual(*context_);
        }
        // R5 stage 2: Option / Result names come from the lang-item
        // registry. No bootstrap fallback — a TU without stdlib cannot
        // use wide-payload shared-layout semantics.
        auto langItemMatches = [&](const char* slot) {
            if (!langItems_) return false;
            auto* l = langItems_->find(slot);
            return l && l->name == ann.name;
        };
        const bool annIsOption = langItemMatches("option");
        const bool annIsResult = langItemMatches("result");
        if (annIsResult || annIsOption) {
            auto& subs = getTypeSubTypes(ann);
            if (!subs.empty()) {
                std::string mangled = ann.name + "<";
                for (size_t si = 0; si < subs.size(); ++si) {
                    if (si > 0) mangled += ",";
                    if (subs[si]) mangled += mangleTypeAnnotation(*subs[si]);
                    else mangled += "i64";
                }
                mangled += ">";
                auto stItR = structTypes_.find(mangled);
                // A Mono-created declaration shell can reserve this mangled
                // name before its body is laid out. It is not a usable
                // Option/Result ABI value: continue below to install the
                // canonical shared/wide tagged-union layout instead.
                if (stItR != structTypes_.end() && !stItR->second->isOpaque())
                    return stItR->second;
                // `Result<T,E>` / `Option<T>` are BUILTIN tagged unions whose
                // LLVM layout is shared across every monomorph (discriminator
                // + payload slot); per-instantiation struct types are an
                // optimisation, not a requirement. Miss here is expected and
                // we silently fall through to the shared layout — no Mono
                // scan gap, no diagnostic.
                //
                // However: if the first (value) type argument maps to an LLVM
                // type larger than 8 bytes (e.g. Vec<T>=24 bytes, Dict=24
                // bytes), the shared __Result { i32, i64 } payload slot
                // truncates the stored value. In that case, generate a
                // per-size concrete struct with an [N x i8] payload that is
                // large enough to hold the inner value without truncation.
                // Size payload by the WIDEST variant: for Result<T, E> we
                // must fit both T and E, not just T. Picks the largest
                // type across all type args, then registers per-variant
                // inner types keyed by (struct name, variant label) so
                // match binding / `?` operator load the correct type per
                // variant rather than truncating E to sizeof(T).
                llvm::Type* widestInnerTy = nullptr;
                uint64_t widestSz = 0;
                std::vector<llvm::Type*> variantTys;
                variantTys.reserve(subs.size());
                for (auto& sub : subs) {
                    if (!sub) { variantTys.push_back(nullptr); continue; }
                    llvm::Type* innerTy = nullptr;
                    {
                        bool prev = softTypeLookup_;
                        softTypeLookup_ = true;
                        innerTy = toLLVMType(*sub);
                        softTypeLookup_ = prev;
                    }
                    variantTys.push_back(innerTy);
                    if (!innerTy || innerTy->isPointerTy()) continue;
                    uint64_t innerSz = module_->getDataLayout().getTypeAllocSize(innerTy);
                    if (innerSz > widestSz) {
                        widestSz = innerSz;
                        widestInnerTy = innerTy;
                    }
                }
                auto registerVariants = [&](const std::string& llvmName) {
                    auto& perVar = resultVariantInnerTypes_[llvmName];
                    if (annIsResult && variantTys.size() >= 2) {
                        if (variantTys[0]) perVar["Ok"]  = variantTys[0];
                        if (variantTys[1]) perVar["Err"] = variantTys[1];
                    } else if (annIsOption && !variantTys.empty()) {
                        if (variantTys[0]) perVar["Some"] = variantTys[0];
                    }
                };
                // Register the {__tag, __data} alias under the mangled
                // name so that Mono-synthesised method bodies keyed on the
                // parameterised receiver (e.g. `currentClassName_` =
                // "Result<i32,string>") can hit the pointer-deref branch
                // in CodeGenMatch.cpp and load the self pointer using the
                // correct per-instantiation struct type — not the shared
                // 16-byte `%__Result` fallback. Mirrors the alias that
                // generateRegisterErrorEnums installs for the bare
                // "Result" / "Option" names.
                if (!structFieldNames_.count(mangled)) {
                    structFieldNames_[mangled] = {"__tag", "__data"};
                }
                if (widestInnerTy && widestSz > 8) {
                    auto* wideTy = getOrCreateResultTypeForInner(widestInnerTy);
                    structTypes_[mangled] = wideTy;
                    if (wideTy->hasName()) registerVariants(wideTy->getName().str());
                    registerVariants(mangled);
                    return wideTy;
                }
                // Shared 8-byte __Result — still register per-variant hint
                // keyed by the mangled name so codegen can distinguish
                // Ok/Err types when the match fires.
                registerVariants("__Result");
                registerVariants(mangled);
                // Alias the mangled name to the shared __Result struct so
                // `Vec<Option<i32>>` / nested-generic field lookups find
                // it at `structTypes_[mangled]` rather than falling off
                // the scan-gap cliff. Both keys point at the same llvm
                // type — no extra allocation, just an additional entry.
                auto* sharedTy = getOrCreateResultType();
                structTypes_[mangled] = sharedTy;
                return sharedTy;
            }
            return getOrCreateResultType();
        }
        // R5 phase 4 batch 4: Delegate<fn(...)> / Event<fn(...)> are Class-kind
        // now. Both lower to an opaque pointer (the concrete dispatch layout
        // lives in std/event.vyx helper functions; the VyxType here is just a
        // typed handle). Name prefix is authoritative — match both bare base
        // names and the mangled `Delegate<fn(...)>` / `Event<fn(...)>` forms.
        if (ann.name == "Delegate" || ann.name == "Event" ||
            (ann.name.size() > 9 && ann.name.compare(0, 9, "Delegate<") == 0) ||
            (ann.name.size() > 6 && ann.name.compare(0, 6, "Event<") == 0)) {
            return llvm::PointerType::getUnqual(*context_);
        }

        // User-defined generic enum (e.g. `enum Either<L, R> { Left(L), Right(R) }`).
        // Mono's scan-time skip for ErrorDef means no concrete struct gets
        // registered for `Either<i32, string>`. Lower on-demand here: find
        // the template, compute the widest variant payload with type args
        // substituted, and either alias to shared `__Result` (small payload)
        // or synthesise a per-size struct (large payload). Mirrors the path
        // used for builtin Option/Result above.
        if (ann.kind == TypeAnnotationKind::Generic && !getTypeSubTypes(ann).empty() && unit_) {
            const Decl* errTemplate = nullptr;
            for (auto& d : unit_->declarations) {
                if (d && d->kind == DeclKind::ErrorDef && d->name == ann.name &&
                    d->genericParams.size() == getTypeSubTypes(ann).size()) {
                    errTemplate = d.get();
                    break;
                }
            }
            if (errTemplate) {
                auto& subs = getTypeSubTypes(ann);
                std::string mangled = ann.name + "<";
                for (size_t si = 0; si < subs.size(); ++si) {
                    if (si > 0) mangled += ",";
                    if (subs[si]) mangled += mangleTypeAnnotationNested(*subs[si]);
                    else mangled += "?";
                }
                mangled += ">";
                auto already = structTypes_.find(mangled);
                if (already != structTypes_.end()) return already->second;
                // Substitute class-level generics into each variant's payload
                // to get concrete LLVM widths. Pair genericParams[i] with
                // subs[i] then walk variantTypes[][].
                auto* errDecl = errTemplate->as<ErrorDefDecl>();
                TypeEnv env;
                for (size_t i = 0; i < errTemplate->genericParams.size() && i < subs.size(); ++i) {
                    if (!subs[i]) continue;
                    auto concrete = TemplateResolver::resolveTypeAnnotation(*subs[i]);
                    if (concrete) env.bind(errTemplate->genericParams[i], concrete);
                }
                uint64_t widestSz = 0;
                llvm::Type* widestTy = nullptr;
                // Per-variant payload type map so CodeGenMatch can pick
                // the correct load type when binding `case Variant(v) =>`
                // on a shared __Result_N struct that backs multiple
                // variants with different widths.
                std::vector<std::pair<std::string, llvm::Type*>> variantFirstTys;
                variantFirstTys.reserve(errDecl->variants.size());
                // Pass the template's generic param names to
                // resolveTypeAnnotation so `L`/`R` resolve to
                // `VyxTypeKind::Generic` (not `Class{name="L"}`) and
                // `substituteType` can map them through `env` to the
                // concrete types.  Without the param list, the fallback
                // classifies `L` as a user class named "L" which
                // substituteType has no hook for — the variant stays
                // generic and its LLVM width defaults to opaque pointer,
                // which causes per-size struct selection to mis-pick the
                // shared 16-byte layout even when the real payload is
                // 32 bytes (e.g. `Either<i32, string>`).
                for (size_t vi = 0; vi < errDecl->variantTypes.size(); ++vi) {
                    auto& vts = errDecl->variantTypes[vi];
                    uint64_t sumSz = 0;
                    llvm::Type* firstTy = nullptr;
                    for (auto& vt : vts) {
                        if (!vt) continue;
                        auto rawResolved = TemplateResolver::resolveTypeAnnotation(
                            *vt, &errTemplate->genericParams);
                        auto resolved = ::vyx::substituteType(rawResolved, env);
                        if (!resolved || resolved->kind == VyxTypeKind::Unknown)
                            resolved = rawResolved;
                        if (!resolved) continue;
                        bool prev = softTypeLookup_;
                        softTypeLookup_ = true;
                        auto* llvmTy = toLLVMType(*resolved);
                        softTypeLookup_ = prev;
                        if (!llvmTy) continue;
                        if (!firstTy) firstTy = llvmTy;
                        if (!llvmTy->isPointerTy())
                            sumSz += module_->getDataLayout().getTypeAllocSize(llvmTy);
                    }
                    if (sumSz > widestSz) { widestSz = sumSz; widestTy = firstTy; }
                    if (vi < errDecl->variants.size() && firstTy) {
                        variantFirstTys.emplace_back(errDecl->variants[vi], firstTy);
                    }
                }
                // Register variant tag indices (Mono skipped this for the
                // generic parent; without them `match (e) { case Either::Left(_) => ... }`
                // can't discriminate).
                for (size_t vi = 0; vi < errDecl->variants.size(); ++vi) {
                    errorEnumValues_[mangled + "." + errDecl->variants[vi]] = static_cast<int>(vi);
                    errorEnumValues_[ann.name + "." + errDecl->variants[vi]] = static_cast<int>(vi);
                    // Bare-label fallback so CodeGenMatch's arm-label lookup
                    // (`errorEnumValues_.find(arm.label)`) succeeds for
                    // un-qualified patterns like `case Left(v) => ...`.
                    // Mirrors the generateRegisterErrorEnums convention
                    // that the non-generic ADT path uses for concrete
                    // variants so both generic and non-generic enum
                    // receivers share the arm-matching logic in
                    // CodeGenMatch.cpp's isResultStruct branch.
                    if (!errorEnumValues_.count(errDecl->variants[vi]))
                        errorEnumValues_[errDecl->variants[vi]] = static_cast<int>(vi);
                }
                // Publish per-variant payload types under both the
                // mangled (`Either<i32,string>`) and LLVM-struct-name
                // (`__Result_N`) keys so the match-binding code can
                // locate the correct per-variant load type.  Mirrors
                // the `registerVariants` helper used for builtin
                // Option/Result above.
                auto publishVariants = [&](const std::string& key) {
                    auto& perVar = resultVariantInnerTypes_[key];
                    for (auto& [vn, vty] : variantFirstTys) {
                        if (vty) perVar[vn] = vty;
                    }
                };
                publishVariants(mangled);
                // Install the ADT `{__tag, __data}` alias under the
                // mangled name so `match (e) { case Variant(v) => ... }`
                // inside Mono-synthesised method bodies (when/if the
                // user generic enum ever hosts methods) hits the
                // pointer-deref branch in CodeGenMatch.cpp with the
                // per-instantiation struct instead of a shared
                // fallback.  The field-name vector is intentionally
                // the 2-slot tagged-union shape used by all of
                // CodeGen's enum dispatches.
                if (!structFieldNames_.count(mangled)) {
                    structFieldNames_[mangled] = {"__tag", "__data"};
                }
                if (widestTy && widestSz > 8) {
                    // When the widest variant has multi-field payload (e.g.
                    // `V(i32, string)` with sumSz=36), `widestTy` is the
                    // FIRST field's type alone, not the whole payload.
                    // `getOrCreateResultTypeForInner` sizes the byte-array
                    // payload slot by `getTypeAllocSize(widestTy)` — which
                    // would truncate `V(i32, string)` to 4 bytes.  When the
                    // summed variant size exceeds the first field's alloc
                    // size, route through a byte-array sentinel of the full
                    // width so the __Result_<N> slot covers every field.
                    // Single-field variants keep the old path unchanged.
                    uint64_t widestTyAllocSz = module_->getDataLayout().getTypeAllocSize(widestTy);
                    llvm::Type* widePayloadTy = widestTy;
                    if (widestSz > widestTyAllocSz) {
                        widePayloadTy = llvm::ArrayType::get(
                            llvm::Type::getInt8Ty(*context_), widestSz);
                    }
                    auto* wide = getOrCreateResultTypeForInner(widePayloadTy);
                    structTypes_[mangled] = wide;
                    if (wide->hasName()) publishVariants(wide->getName().str());
                    return wide;
                }
                auto* shared = getOrCreateResultType();
                structTypes_[mangled] = shared;
                publishVariants("__Result");
                return shared;
            }
        }
        auto stIt = structTypes_.find(ann.name + "_vtable");
        if (stIt != structTypes_.end()) {
            auto ifaceIt = structTypes_.find("__iface_" + ann.name);
            if (ifaceIt != structTypes_.end()) return ifaceIt->second;
            auto* ptrTy = llvm::PointerType::getUnqual(*context_);
            auto* ifaceTy = llvm::StructType::create(*context_, {ptrTy, ptrTy}, "__iface_" + ann.name);
            structTypes_["__iface_" + ann.name] = ifaceTy;
            return ifaceTy;
        }
        // Generic interface lookup: `Container<i64>` shares its base name's
        // vtable + interface struct. Strip the `<...>` suffix and try again.
        if (ann.name.find('<') != std::string::npos) {
            std::string base = ann.name.substr(0, ann.name.find('<'));
            auto stItB = structTypes_.find(base + "_vtable");
            if (stItB != structTypes_.end()) {
                auto ifaceIt = structTypes_.find("__iface_" + base);
                if (ifaceIt != structTypes_.end()) return ifaceIt->second;
                auto* ptrTy = llvm::PointerType::getUnqual(*context_);
                auto* ifaceTy = llvm::StructType::create(*context_, {ptrTy, ptrTy}, "__iface_" + base);
                structTypes_["__iface_" + base] = ifaceTy;
                return ifaceTy;
            }
        }
        if (ann.name == "Ref" || ann.name == "Scope") {
            auto rcIt = structTypes_.find("__RefCounted");
            if (rcIt != structTypes_.end()) return rcIt->second;
            auto* ptrTy2 = llvm::PointerType::getUnqual(*context_);
            auto* i64Ty2 = llvm::Type::getInt64Ty(*context_);
            auto* rcTy = llvm::StructType::create(*context_, {ptrTy2, i64Ty2}, "__RefCounted");
            structTypes_["__RefCounted"] = rcTy;
            return rcTy;
        }
        if ((ann.name.rfind("Ref<", 0) == 0 ||
             ann.name.rfind("Scope<", 0) == 0) &&
            !ann.name.empty() && ann.name.back() == '>') {
            auto rcIt = structTypes_.find("__RefCounted");
            if (rcIt != structTypes_.end()) return rcIt->second;
            auto* ptrTy2 = llvm::PointerType::getUnqual(*context_);
            auto* i64Ty2 = llvm::Type::getInt64Ty(*context_);
            auto* rcTy = llvm::StructType::create(*context_, {ptrTy2, i64Ty2}, "__RefCounted");
            structTypes_["__RefCounted"] = rcTy;
            return rcTy;
        }
        if (ann.name == "Box") {
            return llvm::PointerType::getUnqual(*context_);
        }
        // Smart-pointer containers (`Box<T>`, `Ref<T>`, and any user-defined
        // `class MyBox<T> : Deref<T>`) have a backend-special layout. `Ref<T>`
        // / `Scope<T>` keep the two-slot `__RefCounted` value above; the rest
        // lower to an opaque pointer regardless of T.  When the annotation arrives with T baked into the
        // base name (mangled form `Box<Inc>` produced by Mono / nested generic
        // type-arg rendering) neither the bare-name branches above nor the
        // generic-struct lookup below succeed because no per-instantiation
        // struct is registered.  Short-circuit to ptr here so `Vec<Box<Inc>>`
        // / `Dict<string, Box<dyn Trait>>` etc. don't trigger spurious
        // `generic struct type not found` diagnostics.  The Mono scanner still
        // drives `Box::<T>.new` etc. through its normal class instantiation
        // path for method dispatch.
        //
        // Eligibility: the owning class declares the `Deref<T>` trait
        // (std/ref.vyx) — `Box<T>` and `Ref<T>` in the stdlib, plus any user
        // class that opts in.  Phase 6 trait generalization (2026-04-23)
        // replaced the hardcoded `"Box<" | "Ref<" | "Scope<"` prefix set with
        // this trait check.
        if (classDeclaresDeref(ann.name)) {
            return llvm::PointerType::getUnqual(*context_);
        }
        // `GenericType { name="Box", typeArgs=[...] }` — the typeArgs carry T.
        if (ann.kind == TypeAnnotationKind::Generic &&
            (ann.name == "Box" || ann.name == "Ref" || ann.name == "Scope")) {
            // Ref/Scope already matched above when bare; only Box with args
            // reaches here.
            return llvm::PointerType::getUnqual(*context_);
        }

        {
            auto gpIt = genericTypeParams_.find(ann.name);
            if (gpIt != genericTypeParams_.end()) {
                return gpIt->second;
            }
        }

        {
        auto& annSubs = getTypeSubTypes(ann);
        // Guard: when `ann.name` is already a fully-mangled generic
        // (e.g. `Vec<i64>`), appending another `<...>` from `annSubs`
        // below would yield `Vec<i64><i64>`, which
        // `getOrCreateGenericStructType` later slices into the bogus
        // `i64><i64` typeParamStrs entry. Defer to the fall-through
        // branch at the bottom of this function (below) which handles
        // an already-mangled `ann.name` by a direct lookup.
        bool annNameAlreadyMangled =
            !ann.name.empty() && ann.name.back() == '>' &&
            ann.name.find('<') != std::string::npos;
        if (!annNameAlreadyMangled && !annSubs.empty() && !genericTypeParams_.empty()) {
            bool hasGenericSub = false;
            for (auto& sub : annSubs) {
                if (sub && genericTypeParams_.count(sub->name)) { hasGenericSub = true; break; }
            }
            if (hasGenericSub) {
                std::string mangled = ann.name + "<";
                for (size_t si = 0; si < annSubs.size(); ++si) {
                    if (si > 0) mangled += ",";
                    if (annSubs[si]) {
                        auto& sub = *annSubs[si];
                        auto gIt = genericTypeParams_.find(sub.name);
                        auto& subSubs = getTypeSubTypes(sub);
                        if (gIt != genericTypeParams_.end() && subSubs.empty()) {
                            auto gnIt = genericTypeParamNames_.find(sub.name);
                            mangled += gnIt != genericTypeParamNames_.end()
                                ? gnIt->second
                                : llvmTypeToString(gIt->second);
                        } else if (!subSubs.empty()) {
                            mangled += mangleTypeAnnotation(sub);
                        } else {
                            // Use canonical name `string` (matches Mono's
                            // VyxType::mangle()). The `__String` LLVM struct
                            // is aliased under the `string` key in
                            // structTypes_ via getOrCreateStringType().
                            mangled += sub.name;
                        }
                    }
                }
                mangled += ">";
                {
                    auto stItG = structTypes_.find(mangled);
                    if (stItG != structTypes_.end()) return stItG->second;
                    if (unreachableTemplateBodyWalk_)
                        return llvm::PointerType::getUnqual(*context_);
                    diag_.error(codegenInternalSourceLocation(),
                        "codegen: type `{}` is not lowered; Mono scan gap or unresolved generic", mangled);
                    if (softTypeLookup_) {
                        hadHardTypeError_ = true;
                        return llvm::PointerType::getUnqual(*context_);
                    }
                }
            }
        }

        if (!annNameAlreadyMangled && !annSubs.empty()) {
            std::string mangled = ann.name + "<";
            for (size_t si = 0; si < annSubs.size(); ++si) {
                if (si > 0) mangled += ",";
                if (annSubs[si]) {
                    // IMPORTANT: nested form — Mono registers `Vec<Box<Tree<T>>>`
                    // under the canonical name (Box NOT aliased to rawptr).
                    // Using top-level mangleTypeAnnotation here would produce
                    // `Vec<rawptr>` and miss the Mono-registered entry.
                    mangled += mangleTypeAnnotationNested(*annSubs[si]);
                }
            }
            mangled += ">";
            {
                auto stItA = structTypes_.find(mangled);
                if (stItA != structTypes_.end()) return stItA->second;
                // Not yet registered — try a lazy emit of the Mono-instantiated
                // decl (name == mangled, genericParams already cleared by Mono).
                // This handles declaration-order dependencies such as
                //   BitSet { data: Vec<i64> }  appearing before Vec<i64> in
                // unit_->declarations after Mono appends its instantiations.
                if (unit_) {
                    for (auto& d : unit_->declarations) {
                        if (!d) continue;
                        if ((d->kind == DeclKind::Class || d->kind == DeclKind::Struct) &&
                            d->name == mangled && d->genericParams.empty()) {
                            emitStructDecl(*d);
                            auto stItLazy = structTypes_.find(mangled);
                            if (stItLazy != structTypes_.end()) return stItLazy->second;
                        }
                    }
                }
                if (unreachableTemplateBodyWalk_)
                    return llvm::PointerType::getUnqual(*context_);
                diag_.error(codegenInternalSourceLocation(),
                    "codegen: type `{}` is not lowered; Mono scan gap or unresolved generic", mangled);
                if (softTypeLookup_) {
                    hadHardTypeError_ = true;
                    return llvm::PointerType::getUnqual(*context_);
                }
            }
        }
        }

        auto it = builtinMap.find(ann.name);
        if (it != builtinMap.end()) return it->second(this);

        auto sit = structTypes_.find(ann.name);
        if (sit != structTypes_.end()) {
            if (!genericTypeParams_.empty() && getTypeSubTypes(ann).empty() && unit_) {
                for (auto& d : unit_->declarations) {
                    if (d && (d->kind == DeclKind::Class || d->kind == DeclKind::Struct) &&
                        d->name == ann.name && !d->genericParams.empty()) {
                        std::string mangled = ann.name + "<";
                        for (size_t gi = 0; gi < d->genericParams.size(); ++gi) {
                            if (gi > 0) mangled += ",";
                            auto gpIt = genericTypeParams_.find(d->genericParams[gi]);
                            if (gpIt != genericTypeParams_.end()) {
                                auto gnIt = genericTypeParamNames_.find(d->genericParams[gi]);
                                mangled += gnIt != genericTypeParamNames_.end()
                                    ? gnIt->second
                                    : llvmTypeToString(gpIt->second);
                            } else {
                                mangled += "i64";
                            }
                        }
                        mangled += ">";
                        {
                            auto stItM = structTypes_.find(mangled);
                            if (stItM != structTypes_.end()) return stItM->second;
                            if (unreachableTemplateBodyWalk_)
                                return llvm::PointerType::getUnqual(*context_);
                            diag_.error(codegenInternalSourceLocation(),
                                "codegen: type `{}` is not lowered; Mono scan gap or unresolved generic", mangled);
                            if (softTypeLookup_) {
                                hadHardTypeError_ = true;
                                return llvm::PointerType::getUnqual(*context_);
                            }
                        }
                        break;
                    }
                }
            }
            return sit->second;
        }

        if (ann.name.find('<') != std::string::npos) {
            auto stItN = structTypes_.find(ann.name);
            if (stItN != structTypes_.end()) return stItN->second;
            // Same lazy-emit fallback as above for already-mangled names.
            if (unit_) {
                for (auto& d : unit_->declarations) {
                    if (!d) continue;
                    if ((d->kind == DeclKind::Class || d->kind == DeclKind::Struct) &&
                        d->name == ann.name && d->genericParams.empty()) {
                        emitStructDecl(*d);
                        auto stItLN = structTypes_.find(ann.name);
                        if (stItLN != structTypes_.end()) return stItLN->second;
                    }
                }
            }
            if (unreachableTemplateBodyWalk_)
                return llvm::PointerType::getUnqual(*context_);
            // Bare single-uppercase generic parameter with a trailing `<>`
            // (e.g. "U<>" produced by a re-mangle of an unbound method-level
            // generic param during Mono cloning): historically this was
            // silently collapsed to i64. Under the strict error model the
            // collapse is treated as a hard bug — the enclosing method
            // should have been either instantiated or skipped by now. Emit
            // a diagnostic; keep the i64 return only as a defensive
            // anti-crash so the remainder of the walk can surface more
            // gaps in the same pass.
            if (ann.name.size() == 3 && ann.name[1] == '<' && ann.name[2] == '>' &&
                std::isupper(static_cast<unsigned char>(ann.name[0]))) {
                diag_.error(codegenInternalSourceLocation(),
                    "codegen: bare uppercase generic `{}` reached toLLVMType (enclosing "
                    "method should have been instantiated or skipped)", ann.name);
                hadHardTypeError_ = true;
                return llvm::Type::getInt64Ty(*context_);
            }
            // Primitive with stray `<>` from a Mono substitution path that
            // wrapped the builtin as a zero-arg generic. Historically this
            // was silently collapsed back to the primitive's LLVM type.
            // Strict model: emit a diagnostic (this is an upstream bug — the
            // caller should have stripped the empty type-arg list before
            // getting here), but keep the recovery so downstream error
            // aggregation continues to work.
            if (ann.name.size() >= 3 && ann.name.back() == '>' &&
                ann.name[ann.name.size() - 2] == '<') {
                std::string bare = ann.name.substr(0, ann.name.size() - 2);
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
                        "codegen: primitive `{}` wrapped as zero-arg generic `{}` reached "
                        "toLLVMType (upstream Mono substitution bug)", bare, ann.name);
                    hadHardTypeError_ = true;
                    return recovered;
                }
            }
            // Stringified fn-type (`fn(A,B)->R`) that slipped through as a
            // NamedType. These show up when a generic arg's `VyxType::mangle()`
            // output was stuffed into a NamedType.name by an upstream pass.
            // Any callable type lowers to an opaque pointer; the precise
            // function signature is resolved at call sites via fat-ptr /
            // direct-call dispatch. This is a LEGITIMATE recovery (callable
            // types truly are opaque pointers at the LLVM level), so it
            // stays silent — no `hadHardTypeError_` set.
            if (ann.name.size() >= 4 && ann.name.starts_with("fn(")) {
                return llvm::PointerType::getUnqual(*context_);
            }
            // R5 phase 3: a NamedType whose name is itself a mangled
            // generic (`Option<i32>`) hits this path when upstream stages
            // collapsed a structured GenericType into a bare NamedType. If
            // the base name matches a generic ErrorDef (user-lang-item enum
            // like Option / Result / Maybe<T>), decompose the mangled name
            // into `base + typeArgs` and re-enter toLLVMType on the
            // synthesized GenericType so the on-demand registration path
            // above can run.
            if (auto lt = ann.name.find('<');
                lt != std::string::npos && ann.name.back() == '>' && unit_) {
                std::string base = ann.name.substr(0, lt);
                const Decl* errTemplate = nullptr;
                for (auto& d : unit_->declarations) {
                    if (d && d->kind == DeclKind::ErrorDef &&
                        d->name == base && !d->genericParams.empty()) {
                        errTemplate = d.get();
                        break;
                    }
                }
                if (errTemplate) {
                    std::string innerArgs = ann.name.substr(lt + 1,
                        ann.name.size() - lt - 2);
                    std::vector<std::string> argTokens;
                    {
                        int depth = 0;
                        std::string cur;
                        for (char c : innerArgs) {
                            if (c == '<') { ++depth; cur += c; }
                            else if (c == '>') { --depth; cur += c; }
                            else if (c == ',' && depth == 0) {
                                argTokens.push_back(cur); cur.clear();
                            } else cur += c;
                        }
                        if (!cur.empty()) argTokens.push_back(cur);
                    }
                    if (argTokens.size() == errTemplate->genericParams.size()) {
                        auto gt = std::make_unique<GenericType>();
                        gt->name = base;
                        gt->location = codegenInternalSourceLocation();
                        for (auto& tok : argTokens) {
                            auto nt = std::make_unique<NamedType>();
                            nt->name = tok;
                            nt->location = gt->location;
                            gt->typeArgs.push_back(std::move(nt));
                        }
                        auto* lowered = toLLVMType(*gt);
                        if (lowered) return lowered;
                    }
                }
            }
            diag_.error(codegenInternalSourceLocation(),
                "codegen: type `{}` is not lowered; Mono scan gap or unresolved generic", ann.name);
            hadHardTypeError_ = true;
        }

        if (unit_) {
            for (auto& d : unit_->declarations) {
                if (!d) continue;
                if ((d->kind == DeclKind::Class || d->kind == DeclKind::Struct) &&
                    d->name == ann.name && d->genericParams.empty()) {
                    emitStructDecl(*d);
                    auto sit2 = structTypes_.find(ann.name);
                    if (sit2 != structTypes_.end()) return sit2->second;
                }
                if (d->kind == DeclKind::ErrorDef && d->name == ann.name) {
                    return llvm::Type::getInt32Ty(*context_);
                }
            }
        }

        auto git = genericTypeParams_.find(ann.name);
        if (git != genericTypeParams_.end()) {
            return git->second;
        }

        // Single-uppercase bare generic (e.g. "T", "U") reaching this point
        // is expected in two scenarios today:
        //   (1) soft/template-body walk: Mono skips the template, CodeGen
        //       walks its body for layout only. T has no concrete binding
        //       and must collapse to something lowerable — i64 works
        //       because unused layouts are never instantiated.
        //   (2) residual Sema leak: even instantiated monomorphs sometimes
        //       carry a bare `T` in a deep signature slot (Option<T>-
        //       chained generics). These are Sema pipeline bugs with a
        //       deliberate i64 recovery dating back to R2.
        // We keep the i64 collapse in both cases but do NOT flag it as a
        // hard type error — a loud diagnostic here has been shown to
        // over-regress the test suite without helping the user (the
        // fix has to land in Sema, not at the CodeGen seam). Left as a
        // TODO(T-leak-audit) for the eventual Sema cleanup.
        if (ann.name.size() == 1 && std::isupper(ann.name[0])) {
            return llvm::Type::getInt64Ty(*context_);
        }

        // `Prim<>` recovery: an empty-typeArgs Generic whose base is a
        // primitive came through some Mono substitution path that kept
        // the GenericType shape even after the type args were stripped.
        // Strict model: flag the upstream bug, keep the collapse as an
        // anti-crash.
        if (ann.name.size() >= 3 && ann.name.back() == '>' &&
            ann.name[ann.name.size() - 2] == '<') {
            std::string bare = ann.name.substr(0, ann.name.size() - 2);
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
                diag_.error(ann.location,
                    "codegen: primitive `{}` wrapped as zero-arg generic `{}` reached "
                    "toLLVMType (upstream Mono substitution bug)", bare, ann.name);
                hadHardTypeError_ = true;
                return recovered;
            }
        }

        if (ann.name != "unknown" && ann.name != "Unknown" && !ann.name.empty()) {
            diag_.error(ann.location, "unknown type annotation '{}'", ann.name);
            hadHardTypeError_ = true;
        }
        return llvm::Type::getInt32Ty(*context_);
    }

    if (ann.kind == TypeAnnotationKind::Tuple) {
        std::vector<llvm::Type*> elemTypes;
        auto& tt = static_cast<const TupleType&>(ann);
        for (size_t ei = 0; ei < tt.elements.size(); ++ei) {
            if (!tt.elements[ei]) {
                diag_.error(ann.location, "tuple type: missing element type at position {}", ei);
                hadHardTypeError_ = true;
                elemTypes.push_back(llvm::Type::getInt32Ty(*context_));
                continue;
            }
            elemTypes.push_back(toLLVMType(*tt.elements[ei]));
        }
        if (elemTypes.empty()) {
            diag_.error(ann.location, "tuple type has no elements");
            hadHardTypeError_ = true;
            return llvm::Type::getInt32Ty(*context_);
        }
        return llvm::StructType::get(*context_, elemTypes);
    }

    if (ann.kind == TypeAnnotationKind::Function) {
        return llvm::PointerType::getUnqual(*context_);
    }

    if (ann.kind == TypeAnnotationKind::Pointer || ann.kind == TypeAnnotationKind::Reference) {
        return llvm::PointerType::getUnqual(*context_);
    }

    if (ann.kind == TypeAnnotationKind::Array) {
        auto& arrTy = static_cast<const ArrayType&>(ann);
        if (!arrTy.elementType) {
            diag_.error(ann.location, "array type has no element type in codegen");
            hadHardTypeError_ = true;
            return llvm::Type::getInt32Ty(*context_);
        }
        auto elemTy = toLLVMType(*arrTy.elementType);
        int size = 0;
        if (arrTy.size && arrTy.size->kind == ExprKind::IntLiteral) {
            size = static_cast<int>(arrTy.size->as<const IntLiteralExpr>()->value);
        }
        return llvm::ArrayType::get(elemTy, size);
    }

    if (ann.kind == TypeAnnotationKind::Union) {
        auto* tagTy = llvm::Type::getInt8Ty(*context_);
        auto* valTy = llvm::Type::getInt64Ty(*context_);
        return llvm::StructType::get(*context_, {tagTy, valTy});
    }

    // BUG-LV-14: trait default-method bodies cloned onto an implementing
    // class can reach CodeGen with Dependent type annotations like
    // `Self::Item` / `T::Item` still unresolved. Resolve them here by
    // looking up the enclosing class's `type <Member> = X;` binding via
    // the TU. `Self` redirects through `currentClassName_`. Failure
    // falls back to i32 with a diagnostic so the build still completes.
    if (ann.kind == TypeAnnotationKind::Dependent) {
        const auto& dt = static_cast<const DependentType&>(ann);
        std::string baseName = dt.baseName;
        if (baseName == "Self") baseName = currentClassName_;
        if (unit_ && !baseName.empty()) {
            for (const auto& d : unit_->declarations) {
                if (!d || d->name != baseName) continue;
                if (d->kind != DeclKind::Class && d->kind != DeclKind::Struct) continue;
                const auto* cd = d->as<ClassDecl>();
                if (!cd) continue;
                for (const auto& at : cd->associatedTypes) {
                    if (at.name == dt.memberName && at.defaultType) {
                        return toLLVMType(*at.defaultType);
                    }
                }
                break;
            }
        }
        diag_.error(ann.location,
            "internal: unresolved associated type '{}::{}' reached codegen "
            "(Self bound to '{}')", dt.baseName, dt.memberName,
            currentClassName_.empty() ? "<none>" : currentClassName_);
        hadHardTypeError_ = true;
        return llvm::Type::getInt32Ty(*context_);
    }

    diag_.error(ann.location, "internal: unsupported type annotation kind in toLLVMType ({})",
        static_cast<int>(ann.kind));
    hadHardTypeError_ = true;
    return llvm::Type::getInt32Ty(*context_);
}

} // namespace vyx
