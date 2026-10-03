#include "CodeGenIncludes.h"
#include <set>

namespace vyx {

// Names that arrive here as `decl->name` for primitive-target impl blocks
// (`impl Hashable for i32 { ... }` — see SemaDecl.cpp primitive branch).
// Primitives never need a vtable (dispatch is always static
// `<prim>.<method>`), and trying to populate one can spuriously fail
// when a user module declares a same-named trait whose method set
// differs from the std trait that the impl targets.  Keep in sync with
// CodeGenClass.cpp's codegenIsPrimitiveImplTarget.
static bool vtablePrimitiveTarget(const std::string& name) {
    static const std::set<std::string> kPrim = {
        "i8", "i16", "i32", "i64",
        "u8", "u16", "u32", "u64",
        "f32", "f64",
        "bool", "char",
        "rawptr", "isize", "usize",
        "str",
        "string",
    };
    return kPrim.count(name) > 0;
}

void CodeGen::generatePopulateInstanceVtables() {
    fprintf(stderr, "[CodeGen] Phase: vtable populate\n"); fflush(stderr);
    for (auto& decl : unit_->declarations) {
        if (!decl || decl->kind != DeclKind::Class) continue;

        auto* clsDecl = decl->as<ClassDecl>();
        // Skip primitive-target impl blocks (`impl Hashable for i32`).
        // Primitives dispatch through static `i32.hash` etc., never through
        // a vtable.  Populating one here also risks picking up a same-named
        // user-declared trait with a different method set (see test
        // _generics_industrial/where_eq_hash.vyx's local `trait Eq { fn eq }`
        // colliding with hash.vyx's `trait Eq { fn operator_eq }`).
        if (clsDecl->isImplBlock && vtablePrimitiveTarget(decl->name)) continue;
        // Skip generic class templates: their methods don't exist as concrete LLVM
        // functions until instantiation (e.g. `Vec<string>.push`). Vtable populate
        // for each concrete instance happens through `getOrCreateGenericStructType`.
        if (!decl->genericParams.empty()) continue;
        // Skip Mono-emitted instantiations (mangled `Foo<i64>` names): vtable
        // populate for these happens through the existing
        // `instantiateGenericClass` path on first reference, *not* via this
        // declaration walk. Running the walk anyway would (a) duplicate
        // vtable globals, and (b) fail noisily for std types like
        // `Vec<i64>`/`Dict<i64,bool>` which only implement parts of
        // `Iterable` (`length()` not `len()`) — a pre-existing slot-name
        // mismatch that the legacy non-Mono path silently tolerated.
        if (decl->name.find('<') != std::string::npos) continue;
        std::vector<std::string> allIfaces = clsDecl->interfaces;
        // Strip `<...>` from parentName when checking vtable existence so a
        // class declared as `: Container<i64>` matches the bare-base
        // `Container_vtable`. The arg-bearing name is preserved in allIfaces
        // for the per-iface stripping loop below.
        if (!clsDecl->parentName.empty()) {
            const std::string& pn = clsDecl->parentName;
            auto plt = pn.find('<');
            const std::string parentBase = (plt == std::string::npos) ? pn : pn.substr(0, plt);
            if (structTypes_.count(parentBase + "_vtable")) {
                allIfaces.insert(allIfaces.begin(), pn);
            }
        }

        for (auto& ifaceNameRaw : allIfaces) {
            // Strip `<...>` for vtable / interface lookups: a class declaring
            // `: Container<i64>` shares Container's base vtable layout (the
            // method signatures are the same shape — vtable slots are opaque
            // ptrs anyway). Default-method bodies and the call-site
            // `Bag_vtable_Container` reference also use the bare base name,
            // so emitting the global under the bare name keeps both ends
            // aligned. Per-T per-class vtables would be needed only if the
            // interface methods' VTable signatures differed by T (none in
            // the current ABI).
            auto lt = ifaceNameRaw.find('<');
            std::string ifaceName = (lt == std::string::npos)
                ? ifaceNameRaw
                : ifaceNameRaw.substr(0, lt);

            std::string vtableTyName = ifaceName + "_vtable";
            auto vtIt = structTypes_.find(vtableTyName);
            if (vtIt == structTypes_.end()) continue;

            auto* vtableTy = vtIt->second;
            auto& vtFieldNames = structFieldNames_[vtableTyName];

            std::vector<llvm::Constant*> entries;
            bool vtableOk = true;
            for (auto& methodName : vtFieldNames) {
                std::string mangledName = decl->name + "." + methodName;
                auto fnIt = functions_.find(mangledName);
                if (fnIt != functions_.end()) {
                    entries.push_back(fnIt->second);
                    continue;
                }
                std::string defaultName = ifaceName + "." + methodName;
                auto defIt = functions_.find(defaultName);
                if (defIt != functions_.end()) {
                    entries.push_back(defIt->second);
                    continue;
                }
                vtableOk = false;
                diag_.error(decl->location,
                    "vtable for class `{}` implementing `{}`: no implementation for interface method `{}` (expected `{}` or `{}`)",
                    decl->name, ifaceName, methodName, mangledName, defaultName);
            }
            if (!vtableOk) continue;

            std::string vtableGlobalName = decl->name + "_vtable_" + ifaceName;
            // Idempotent: re-running for two `<T1>`/`<T2>` instantiations
            // would otherwise emit duplicate globals under the same name.
            if (module_->getGlobalVariable(vtableGlobalName, true)) continue;

            auto* vtableConst = llvm::ConstantStruct::get(vtableTy, entries);
            new llvm::GlobalVariable(*module_, vtableTy, true, llvm::GlobalValue::InternalLinkage,
                vtableConst, vtableGlobalName);
        }
    }
}

} // namespace vyx
