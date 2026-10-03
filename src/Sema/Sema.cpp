#include "Sema.h"
#include "SemaCloneHelpers.h"
#include "../Common/StringUtils.h"
#include <algorithm>
#include <functional>
#include <iostream>
#include <map>
#include <set>

namespace vyx {

// ────────────────────────────────────────────────────────────────────────────
// typeAnnotationToMangle (Sema-local copy)
//
// Converts a TypeAnnotation tree into the same canonical string that
// VyxType::mangle() / the parser's typeAnnotationToMangle helper produce.
// Needed by checkTypeRedefinition (partial-spec ODR key) and
// selectPartialSpec (specificity counting) without taking a dependency on
// Parser.h.
// ────────────────────────────────────────────────────────────────────────────
static std::string typeAnnotationToMangle(const TypeAnnotation* ann) {
    if (!ann) return "?";
    switch (ann->kind) {
        case TypeAnnotationKind::Named:
            return ann->name;
        case TypeAnnotationKind::Generic: {
            auto* gt = ann->as<GenericType>();
            std::string r = ann->name + "<";
            for (size_t i = 0; i < gt->typeArgs.size(); ++i) {
                if (i > 0) r += ",";
                r += typeAnnotationToMangle(gt->typeArgs[i].get());
            }
            r += ">";
            return r;
        }
        case TypeAnnotationKind::Pointer: {
            auto* pt = ann->as<PointerType>();
            return "*" + typeAnnotationToMangle(pt->innerType.get());
        }
        case TypeAnnotationKind::Reference: {
            auto* rt = ann->as<ReferenceType>();
            return (rt->isMutable ? "&mut " : "&") +
                   typeAnnotationToMangle(rt->innerType.get());
        }
        case TypeAnnotationKind::Array: {
            auto* at = ann->as<ArrayType>();
            return "[" + typeAnnotationToMangle(at->elementType.get()) + "]";
        }
        case TypeAnnotationKind::Tuple: {
            auto* tt = ann->as<TupleType>();
            std::string r = "(";
            for (size_t i = 0; i < tt->elements.size(); ++i) {
                if (i > 0) r += ",";
                r += typeAnnotationToMangle(tt->elements[i].get());
            }
            r += ")";
            return r;
        }
        case TypeAnnotationKind::Function: {
            auto* ft = ann->as<FunctionType>();
            std::string r = "fn(";
            for (size_t i = 0; i < ft->paramTypes.size(); ++i) {
                if (i > 0) r += ",";
                r += typeAnnotationToMangle(ft->paramTypes[i].get());
            }
            r += ")->" + typeAnnotationToMangle(ft->returnType.get());
            return r;
        }
        case TypeAnnotationKind::Union: {
            auto* ut = ann->as<UnionType>();
            std::string r;
            for (size_t i = 0; i < ut->members.size(); ++i) {
                if (i > 0) r += "|";
                r += typeAnnotationToMangle(ut->members[i].get());
            }
            return r;
        }
        default:
            break;
    }
    return ann->name.empty() ? "?" : ann->name;
}

SourceLocation Sema::semaInternalSourceLocation() {
    static const SourceLocation loc = [] {
        SourceLocation l;
        l.filename = SourceLocation::intern("<sema>");
        l.line = 0;
        l.column = 0;
        return l;
    }();
    return loc;
}

SourceLocation Sema::pickAssignWarnLoc(const SourceLocation& at) {
    return at.filename.empty() ? semaInternalSourceLocation() : at;
}

std::string Sema::qualifiedKey(const std::string& ns, const std::string& name) {
    return (ns.empty() ? std::string("<global>") : ns) + "::" + name;
}

bool Sema::isPrimitiveTypeName(const std::string& name) {
    // Must match mangleVyxTypeForCodegen's output exactly so that Sema's
    // dispatch key (`i32`) and CodeGen's mangled key (`i32.hash`) agree.
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

VyxTypePtr Sema::lookupPrimitiveType(const std::string& name) {
    if (!isPrimitiveTypeName(name)) return nullptr;
    // symbols_ has all primitives registered at Sema ctor time
    // (see Sema::Sema in this file — `registerType("i32", ...)` etc.).
    // `string` resolves via the symbol table too (R5: `string` is no
    // longer a keyword; it's the lang-item-bound class/primitive alias).
    return symbols_.lookupType(name);
}

// PLAN_SEMA_ROOT_FIX — S3: single discovery entry-point.
//
// At S3 the FactBase is the sole authoritative store. `recordPrimImplFact`
// is the only write into `factBase_` and its short/long-name expansion
// matches what `verifyMethodConstraints` historically did with
// `primitiveTraitSatisfied_` (the legacy set is no longer written).
//
// Why keep the field-shaped helper rather than inlining at the
// callsite: the entry pre-scan, `analyzeClassDecl`, and any future
// discovery channel (e.g. compile-time generated impls) all need
// identical alias-handling semantics; centralising avoids a third
// drift source between Sema and TraitSolver. The callsite still
// performs the `isImplBlock && isPrimitiveTypeName(...)` filter so this
// helper assumes its arguments are valid.
void Sema::recordPrimImplFact(const std::string& primName,
                              const std::string& traitName) {
    if (primName.empty() || traitName.empty()) return;
    factBase_.registerPrimImpl(canonicalizer_.canonicalizePrim(primName),
                               canonicalizer_.canonicalizeTrait(traitName));
}

// PLAN_SEMA_ROOT_FIX — S3: surface the prim-query telemetry.
//
// Two signals make it onto the line:
//   * `prim_queries` — how often `verifyMethodConstraints` consulted the
//     solver during this analyze() invocation. Zero with a populated
//     fact base just means the program contained no `where T: Trait`
//     where T bound to a primitive; that is fine.
//   * `post_freeze_writes` — how many genuinely new facts arrived AFTER
//     Phase A finished. The "Phase A is the only writer" invariant says
//     this must stay 0; a non-zero value is loud red-flag output that
//     indicates a re-introduced lazy / on-demand write path.
//
// Writes to stdout (not stderr) so PowerShell pipelines and CI log
// collectors that don't capture stderr-on-spawn can grep it. Pure
// diagnostic — never affects semantic outcomes.
void Sema::dumpSolverReconciliationStats() {
    if (solverPrimQueries_ == 0 && factBase_.postFreezeWrites() == 0) return;
    std::cout << "[S3 solver] prim_queries=" << solverPrimQueries_
              << " facts=" << factBase_.primFactCount()
              << " frozen=" << (factBase_.isFrozen() ? "yes" : "no")
              << " post_freeze_writes=" << factBase_.postFreezeWrites();
    if (factBase_.postFreezeWrites() > 0) {
        std::cout << " (REGRESSION — Phase A is no longer the only writer)";
    }
    std::cout << "\n";
}

// PLAN_SEMA_ROOT_FIX — S5: scheduler ledger summary after Sema.
//
// Format:
//   [S4 mono-sched] requests=N unique=M classes=Cc free_fns=Ff methods=Mm
//
//   * `requests` — Sema-side record* calls (with duplicates).
//   * `unique`   — distinct (kind, key) pairs seen.
//   * `classes` / `free_fns` / `methods` — per-kind counts.
void Sema::dumpMonoSchedulerStats() {
    if (monoScheduler_.totalRequests() == 0) return;
    std::cout << "[S4 mono-sched] requests=" << monoScheduler_.totalRequests()
              << " unique="   << monoScheduler_.uniqueRequests()
              << " sema="     << monoScheduler_.semaRequests()
              << " mono="     << monoScheduler_.monoRequests()
              << " classes="  << monoScheduler_.classRequests()
              << " free_fns=" << monoScheduler_.freeFnRequests()
              << " methods="  << monoScheduler_.methodRequests()
              << "\n";
}

const char* Sema::declKindName(DeclKind kind) {
    switch (kind) {
        case DeclKind::Function:    return "function";
        case DeclKind::Struct:      return "struct";
        case DeclKind::Class:       return "class";
        case DeclKind::Interface:   return "interface";
        case DeclKind::ErrorDef:    return "errordef";
        case DeclKind::TypeAlias:   return "type alias";
        case DeclKind::GlobalVar:   return "global variable";
        case DeclKind::Concept:     return "concept";
        case DeclKind::ExternBlock: return "extern block";
        case DeclKind::Import:      return "import";
        case DeclKind::Macro:       return "macro";
    }
    return "declaration";
}

std::string Sema::typeSignatureString(const std::vector<VyxTypePtr>& paramTypes) {
    std::string r = "(";
    for (size_t i = 0; i < paramTypes.size(); ++i) {
        if (i > 0) r += ", ";
        r += paramTypes[i] ? paramTypes[i]->toString() : "<?>";
    }
    r += ")";
    return r;
}

bool Sema::paramTypesEqual(const std::vector<VyxTypePtr>& a, const std::vector<VyxTypePtr>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (!a[i] || !b[i]) {
            if (a[i].get() != b[i].get()) return false;
            continue;
        }
        if (!a[i]->isEqual(*b[i])) return false;
    }
    return true;
}

// Phase 9 free-function overloading: build a stable suffix from a parameter
// type list. Underscores join the per-param mangles so "fn add(i32, i32)"
// becomes "add$$i32_i32" (ASCII, no shell metacharacters, idempotent).
// We use VyxType::mangle() so the encoding matches what Mono / CodeGen
// already produce for generics.
std::string Sema::mangleOverloadSuffix(const std::vector<VyxTypePtr>& paramTypes) {
    std::string r;
    for (size_t i = 0; i < paramTypes.size(); ++i) {
        if (i > 0) r += "_";
        r += paramTypes[i] ? paramTypes[i]->mangle() : std::string("?");
    }
    return r;
}

std::string Sema::mangleOverloadName(const std::string& name,
                                     const std::vector<VyxTypePtr>& paramTypes) {
    if (paramTypes.empty()) return name + "$$";
    return name + "$$" + mangleOverloadSuffix(paramTypes);
}

bool Sema::checkTypeRedefinition(const std::string& ns, const Decl& decl) {
    // Full template specialization coexistence rule (P2-C2):
    //   * A generic template `class Vec<T>` has decl.name = "Vec".
    //   * A full specialization `class Vec<i32>` has decl.name = "Vec<i32>"
    //     (parser sets name = mangled form, clears genericParams).
    // Because these produce DIFFERENT keys, the ODR registry naturally allows
    // them to coexist in the same translation unit. Two specializations with
    // the same mangled name (e.g. two `class Vec<i32>`) share the same key and
    // are correctly rejected as redefinitions by the logic below.
    //
    // P2b Partial specialization coexistence rule:
    //   * A partial spec `class Pair<T, i32>` keeps decl.name = "Pair" BUT
    //     differs from the primary and from other partials by its
    //     specializationPattern. We key it as "Pair#partial#<mangled-pattern>"
    //     so the ODR registry treats each distinct pattern as a unique entry
    //     while still catching verbatim duplicates.
    std::string odrName = decl.name;
    if (decl.isPartialSpecialization) {
        odrName = decl.partialSpecBaseName + "#partial#<";
        for (size_t i = 0; i < decl.specializationPattern.size(); ++i) {
            if (i > 0) odrName += ",";
            odrName += typeAnnotationToMangle(decl.specializationPattern[i].get());
        }
        odrName += ">";
    }
    std::string key = qualifiedKey(ns, odrName);
    auto it = typeDeclRegistry_.find(key);
    if (it == typeDeclRegistry_.end()) {
        TypeDeclRecord rec;
        rec.kind = decl.kind;
        rec.location = decl.location;
        rec.isImported = decl.isImported;
        rec.isFullSpecialization = decl.isFullSpecialization;
        rec.isPartialSpecialization = decl.isPartialSpecialization;
        typeDeclRegistry_.emplace(std::move(key), std::move(rec));
        return false;
    }
    const auto& prev = it->second;
    // Same exact declaration site (e.g., analyzed twice across Pass 1 + re-entry)
    // is not a true redefinition; silently skip.
    if (prev.location.filename == decl.location.filename &&
        prev.location.line == decl.location.line &&
        prev.location.column == decl.location.column) {
        return false;
    }
    std::string nsDisplay = ns.empty() ? std::string("<global>") : ns;
    // For duplicate full specializations, emit a specialization-specific message
    // so the user knows the mangled form (e.g. "Vec<i32>") is the conflict site.
    if (decl.isFullSpecialization) {
        diag_.error(decl.location,
            "redefinition of full template specialization '{}' in namespace '{}'",
            decl.name, nsDisplay);
        diag_.note(prev.location,
            "previous specialization '{}' is here",
            decl.name);
        return true;
    }
    // For duplicate partial specializations (same base name + same pattern),
    // emit a pattern-specific message.
    if (decl.isPartialSpecialization) {
        std::string patStr = decl.partialSpecBaseName + "<";
        for (size_t i = 0; i < decl.specializationPattern.size(); ++i) {
            if (i > 0) patStr += ",";
            patStr += typeAnnotationToMangle(decl.specializationPattern[i].get());
        }
        patStr += ">";
        diag_.error(decl.location,
            "redefinition of partial template specialization '{}' in namespace '{}'",
            patStr, nsDisplay);
        diag_.note(prev.location,
            "previous partial specialization '{}' is here", patStr);
        return true;
    }
    // Clang/GCC-style diagnostic pair: primary error at the new site, followed
    // by a secondary note pointing at the previous definition. Emitting the
    // earlier location as its own note lets the diagnostic printer show the
    // source context of both sites (and lets IDEs navigate to both).
    diag_.error(decl.location,
        "redefinition of {} '{}' in namespace '{}'",
        declKindName(decl.kind), decl.name, nsDisplay);
    diag_.note(prev.location,
        "previous definition of {} '{}' is here",
        declKindName(prev.kind), decl.name);
    return true;
}

std::string Sema::moduleOfDecl(const Decl& d) const {
    auto it = declModule_.find(&d);
    return it != declModule_.end() ? it->second : std::string{};
}

bool Sema::checkFunctionRedefinition(const std::string& ns,
                                     const Decl& decl,
                                     const std::vector<VyxTypePtr>& paramTypes,
                                     bool isGeneric,
                                     size_t genericArity) {
    // P2-C2 full-specialization coexistence: `fn foo<T>` (name="foo", generic)
    // and `fn foo<i32>` (name="foo<i32>", non-generic, isFullSpecialization=true)
    // use DIFFERENT keys here, so they coexist without conflict.  Two identical
    // specializations `fn foo<i32>` produce the same key "foo<i32>" and are
    // correctly rejected as a redefinition by the loop below.
    std::string key = qualifiedKey(ns, decl.name);
    auto& overloads = functionDeclRegistry_[key];
    for (auto& prev : overloads) {
        // Skip the same declaration site re-entered across Passes.
        if (prev.location.filename == decl.location.filename &&
            prev.location.line == decl.location.line &&
            prev.location.column == decl.location.column) {
            return false;
        }
        bool conflict = false;
        if (isGeneric || prev.isGeneric) {
            if (isGeneric == prev.isGeneric && genericArity == prev.genericArity) {
                conflict = true;
            }
        } else {
            conflict = paramTypesEqual(prev.paramTypes, paramTypes);
        }
        if (!conflict) continue;
        std::string nsDisplay = ns.empty() ? std::string("<global>") : ns;
        // Clang/GCC-style pair: primary error at new definition + note at the
        // prior definition so both locations get highlighted source context.
        if (isGeneric) {
            diag_.error(decl.location,
                "redefinition of generic function '{}' with {} type parameter(s) "
                "in namespace '{}'",
                decl.name, genericArity, nsDisplay);
            diag_.note(prev.location,
                "previous definition of generic function '{}' with {} type parameter(s) is here",
                decl.name, prev.genericArity);
        } else if (decl.isFullSpecialization) {
            // P2-C2: duplicate full template specializations get a specialization-
            // specific message so the user can identify the mangled form.
            diag_.error(decl.location,
                "redefinition of full template specialization '{}' in namespace '{}'",
                decl.name, nsDisplay);
            diag_.note(prev.location,
                "previous specialization '{}' is here",
                decl.name);
        } else {
            diag_.error(decl.location,
                "redefinition of function '{}{}' in namespace '{}'",
                decl.name, typeSignatureString(paramTypes), nsDisplay);
            diag_.note(prev.location,
                "previous definition of '{}{}' is here",
                decl.name, typeSignatureString(prev.paramTypes));
        }
        return true;
    }
    FunctionDeclRecord rec;
    rec.paramTypes = paramTypes;
    rec.location = decl.location;
    rec.isImported = decl.isImported;
    rec.isGeneric = isGeneric;
    rec.genericArity = genericArity;
    overloads.push_back(std::move(rec));
    return false;
}

Sema::Sema(DiagnosticsEngine& diag) : diag_(diag) {
    registerBuiltinTypes();
    registerBuiltinFunctions();
}

void Sema::registerBuiltinTypes() {
    symbols_.registerType("void",    types::makeVoid());
    symbols_.registerType("bool",    types::makeBool());
    symbols_.registerType("char",    types::makeChar());
    symbols_.registerType("str",     types::makeString());
    symbols_.registerType("string",  types::makeString());
    symbols_.registerType("rawptr",  types::makeRawPtr());
    symbols_.registerType("i8",      types::makeInt(8, true));
    symbols_.registerType("i16",     types::makeInt(16, true));
    symbols_.registerType("i32",     types::makeInt(32, true));
    symbols_.registerType("i64",     types::makeInt(64, true));
    symbols_.registerType("u8",      types::makeInt(8, false));
    symbols_.registerType("u16",     types::makeInt(16, false));
    symbols_.registerType("u32",     types::makeInt(32, false));
    symbols_.registerType("u64",     types::makeInt(64, false));
    symbols_.registerType("isize",   types::makeISize());
    symbols_.registerType("usize",   types::makeUSize());
    symbols_.registerType("f32",     types::makeFloat(32));
    symbols_.registerType("f64",     types::makeFloat(64));

    // Only primitive, first-class language types are built in. Collection wrappers
    // (`Vec`, `Dict`, `Set`, `Queue`, ...), Box/Ref, and the concept interfaces
    // (Iterable/Hashable/...) live in user-visible modules that must be
    // imported explicitly.
}

void Sema::registerBuiltinFunctions() {
    struct BuiltinFn {
        const char* name;
        std::vector<VyxTypePtr> params;
        VyxTypePtr ret;
    };

    auto S = types::makeString();
    auto V = types::makeVoid();
    auto I32 = types::makeInt(32);
    auto I64 = types::makeInt(64);
    auto R = types::makeRawPtr();
    auto U = types::makeUnknown();

    // Minimum set of non-collection intrinsics. I/O, threading, and FFI helpers
    // previously lived here; anything that is not a core language primitive has
    // been removed. If a program needs them, they must come from an imported
    // module or extern `C` declaration.
    BuiltinFn builtins[] = {
        {"print",         {S},       V},
        {"assert",        {I32},     V},
        {"assert_eq",     {I32,I32}, V},
        {"panic",         {S},       V},
        {"type_name",     {},        S},
        {"typeinfo",      {},        S},
        // typeof(expr) — compile-time type introspection. Takes an arbitrary
        // expression and returns a string containing the canonical name of
        // that expression's static type. CodeGen resolves it at emission
        // time from the call's argument inferredType.
        {"typeof",        {U},       S},
        {"from_cstr",     {R},       S},
        {"from_cstr_len", {R,I64},   S},
        {"from_cstr_view_len", {R,I64}, S},
        {"from_raw_string_parts", {R,I64,I64,I64}, S},
        {"to_rawptr",     {U},       R},
        {"dealloc",       {R},       V},
        {"format",        {S},       S},
        // Runtime/CLI helpers whose bodies CodeGen synthesises. Without
        // Sema entries std/args.vyx, std/io.vyx, std/reflect.vyx, etc.
        // fail with "undefined function".
        {"argCount",      {},        I32},
        {"getArg",        {I32},     S},
        // BUG-LV-20: `input` is now a real stdlib function in std/io.vyx
        // (one parameter `prompt: string`).  The historical 0-arg builtin
        // entry shadowed it and forced CodeGen to emit a call to a
        // never-implemented `vyx_io_input` symbol — link error.  Removing
        // the builtin lets the std.io declaration win.
        {"memcpy",        {R,R,I64}, R},
    };

    for (auto& b : builtins) {
        Symbol sym;
        sym.name = b.name;
        sym.isFunction = true;
        sym.paramTypes = std::move(b.params);
        sym.returnType = b.ret;
        sym.type = b.ret;
        symbols_.declare(b.name, std::move(sym));
    }

    // Only true language-level intrinsics are generic builtins. Collection
    // constructors such as `makeVec`/`makeDict`/... used to live here; they are
    // now expected to come from user-visible library modules if the program
    // needs them.
    struct GenericFn { const char* name; size_t arity; VyxTypePtr ret; bool deprecated; const char* deprecationMsg; };
    GenericFn generics[] = {
        {"sizeof",    1, I64, false, nullptr},
        {"alignof",   1, I64, false, nullptr},
        {"alloc",     1, R,   false, nullptr},
        {"transmute", 2, U,   false, nullptr},
    };
    for (auto& g : generics) {
        Symbol sym;
        sym.name = g.name;
        sym.isFunction = true;
        sym.isGeneric = true;
        sym.genericArity = g.arity;
        sym.returnType = g.ret;
        sym.type = g.ret;
        sym.isDeprecated = g.deprecated;
        if (g.deprecationMsg) sym.deprecationMsg = g.deprecationMsg;
        symbols_.declare(g.name, std::move(sym));
    }


    // `Some` / `None` / `Ok` / `Err` are exposed as language-level built-in
    // symbols so the common ergonomic cases (`return Some(x);`, `None`,
    // `Ok(v)`, `Err(e)`) parse even without `use std.core;`. They resolve
    // with Unknown payload/return types, and CodeGen synthesizes the
    // concrete ADT layout from the call-site target-type hint. Users who
    // bind a conflicting name (e.g. a helper function called `Some`) still
    // win because `applyPreludeImports`'s `lookup` guard will not overwrite
    // existing symbols; the real `std.core` Option/Result ADT (when pulled
    // in via `use std.collections;` or similar) then re-binds the same
    // names with the correct `declLocation` for diagnostics.
    struct VariantSym { const char* name; size_t arity; };
    VariantSym variantSyms[] = {
        {"Some", 1}, {"None", 0},
        {"Ok",   1}, {"Err",  1},
    };
    for (auto& v : variantSyms) {
        if (symbols_.lookup(v.name)) continue;
        Symbol sym;
        sym.name = v.name;
        sym.isImported = true;
        sym.isExported = true;
        if (v.arity > 0) {
            sym.isFunction = true;
            sym.paramCount = v.arity;
            sym.paramTypes.assign(v.arity, types::makeUnknown());
            sym.returnType = types::makeUnknown();
            sym.type = types::makeUnknown();
        } else {
            sym.type = types::makeUnknown();
        }
        symbols_.declare(v.name, std::move(sym));
    }
}

void Sema::scanLangItemAttributes(TranslationUnit& unit) {
    // Walk every top-level declaration and register any `@[lang_item("s")]`
    // attribute into the registry.  The attribute key is `lang_item` and
    // the value is the slot name (the parser stores it verbatim, minus the
    // surrounding quotes).  Only a curated set of slot names currently
    // means anything to the compiler; unknown slots are registered anyway
    // so third-party libraries can stake out names without diagnostics
    // (future sugar passes can reserve them later).
    langItems_.clear();

    // Also register the nested decls produced by import resolution —
    // each `use std.result;` pulls the standard-library file's decls
    // into `unit.declarations` transparently, so a single top-level
    // walk suffices.
    for (auto& decl : unit.declarations) {
        if (!decl) continue;
        for (auto& [attrName, attrValue] : decl->attributes) {
            if (attrName != "lang_item") continue;
            // Strip surrounding quotes the parser preserved so callers
            // can write `@[lang_item("result")]` and we look up by the
            // bare slot name.
            std::string slot = attrValue;
            if (slot.size() >= 2 && slot.front() == '"' && slot.back() == '"') {
                slot = slot.substr(1, slot.size() - 2);
            }
            if (slot.empty()) continue;
            langItems_.registerItem(slot, decl.get(), diag_, decl->location);
        }
    }
}

void Sema::applyPreludeImports(TranslationUnit& unit) {
    // A prelude entry says: "when the program `use`s module M, expose the
    // variants of enum E defined in M as bare identifiers". Keeping the table
    // small and data-driven means future preludes (e.g. std.result) can be
    // added without touching analyze().
    struct PreludeEntry {
        std::string modulePath;   // e.g. "std.core"
        std::string shortModule;  // e.g. "core" (the trailing segment)
        std::string enumName;     // e.g. "Option"
    };
    static const PreludeEntry kPreludes[] = {
        {"std.core", "core", "Option"},
        {"std.core", "core", "Result"},
    };

    auto hasUseOfModule = [&](const std::string& modulePath) {
        std::vector<std::string> segs;
        {
            std::string cur;
            for (char ch : modulePath) {
                if (ch == '.') { segs.push_back(cur); cur.clear(); }
                else cur.push_back(ch);
            }
            if (!cur.empty()) segs.push_back(cur);
        }
        for (auto& decl : unit.declarations) {
            if (!decl || decl->kind != DeclKind::Import) continue;
            auto* imp = decl->as<ImportDecl>();
            if (!imp->importNames.empty() && imp->importNames[0] == "__module")
                continue;
            if (imp->importPath.size() != segs.size()) continue;
            bool match = true;
            for (size_t i = 0; i < segs.size(); ++i) {
                if (imp->importPath[i] != segs[i]) { match = false; break; }
            }
            if (match) return true;
        }
        return false;
    };

    auto registerVariant = [&](const Decl* declPtr, const std::string& variantName,
                               size_t payloadArity) {
        if (symbols_.lookup(variantName)) return;
        Symbol sym;
        sym.name = variantName;
        sym.declLocation = declPtr->location;
        sym.isImported = true;
        sym.isExported = true;
        if (payloadArity > 0) {
            sym.isFunction = true;
            sym.paramCount = payloadArity;
            sym.paramTypes.assign(payloadArity, types::makeUnknown());
            sym.returnType = types::makeUnknown();
            sym.type = types::makeUnknown();
        } else {
            sym.type = types::makeUnknown();
        }
        symbols_.declare(variantName, std::move(sym));
    };

    // Pass A: expose std.core's Option/Result variants.
    //
    // The variants Some/None/Ok/Err are already provided as built-in symbols
    // by `registerBuiltinFunctions` so that `return Some(x);` works out of
    // the box without any `use` directive. This pass re-binds them to the
    // real Option/Result ADT declarations whenever std.core is present in
    // the TU (transitively via `use std.collections;` or directly via
    // `use std.core;`), so downstream Sema can attribute them to the actual
    // enum at the right `declLocation` for diagnostics.
    for (auto& p : kPreludes) {
        if (!hasUseOfModule(p.modulePath)) continue;
        for (auto& decl : unit.declarations) {
            if (!decl || decl->kind != DeclKind::ErrorDef) continue;
            if (decl->name != p.enumName) continue;
            auto modIt = declModule_.find(decl.get());
            if (modIt == declModule_.end() || modIt->second != p.shortModule) continue;
            auto* errDecl = decl->as<ErrorDefDecl>();
            for (size_t vi = 0; vi < errDecl->variants.size(); ++vi) {
                size_t payloadArity = (vi < errDecl->variantTypes.size())
                    ? errDecl->variantTypes[vi].size()
                    : 0;
                registerVariant(decl.get(), errDecl->variants[vi], payloadArity);
            }
            break;
        }
    }

    // Pass B: promote user-defined ADT variants into the global scope when
    // the TU contains a `use <module>.<EnumName>` (exact name match) or a
    // `use <module>.*` wildcard. std.core's Option/Result entries above are
    // already handled; non-core ADTs remain scoped unless explicitly listed.
    std::map<std::string, const Decl*> pendingVariantCtors;
    for (auto& decl : unit.declarations) {
        if (!decl || decl->kind != DeclKind::ErrorDef) continue;
        pendingVariantCtors.emplace(decl->name, decl.get());
    }

    for (auto& decl : unit.declarations) {
        if (!decl || decl->kind != DeclKind::Import) continue;
        auto* imp = decl->as<ImportDecl>();
        if (!imp->importNames.empty() && imp->importNames[0] == "__module")
            continue;
        bool wildcard = !imp->importNames.empty() && imp->importNames[0] == "*";
        for (auto& name : imp->importNames) {
            if (name == "__module" || name.starts_with("__")) continue;
            if (name == "*") continue;
            auto it = pendingVariantCtors.find(name);
            if (it == pendingVariantCtors.end()) continue;
            auto* errDecl = it->second->as<ErrorDefDecl>();
            for (size_t vi = 0; vi < errDecl->variants.size(); ++vi) {
                size_t payloadArity = (vi < errDecl->variantTypes.size())
                    ? errDecl->variantTypes[vi].size()
                    : 0;
                registerVariant(it->second, errDecl->variants[vi], payloadArity);
            }
        }
        if (wildcard) {
            for (auto& [enumName, declPtr] : pendingVariantCtors) {
                auto modIt = declModule_.find(declPtr);
                if (modIt == declModule_.end()) continue;
                std::string declMod = modIt->second;
                if (imp->importPath.empty()) continue;
                if (declMod != imp->importPath.back()) continue;
                auto* errDecl = declPtr->as<ErrorDefDecl>();
                for (size_t vi = 0; vi < errDecl->variants.size(); ++vi) {
                    size_t payloadArity = (vi < errDecl->variantTypes.size())
                        ? errDecl->variantTypes[vi].size()
                        : 0;
                    registerVariant(declPtr, errDecl->variants[vi], payloadArity);
                }
            }
        }
    }
}

void Sema::analyze(TranslationUnit& unit) {
    unit_ = &unit;

    // Reset ODR state for this translation unit so repeated analyze() calls
    // don't bleed duplicates across invocations.
    typeDeclRegistry_.clear();
    functionDeclRegistry_.clear();
    overloadGroups_.clear();
    declModule_.clear();

    // Build a `Decl* → enclosing module name` map by replaying the sequence of
    // `__module` / `__module_end` markers in unit.declarations. Used by ODR
    // diagnostics and name-resolution scope.
    //
    // Three mechanisms produce markers:
    //   1. File-wide `module Foo.Bar;` — emits a single `__module Foo.Bar`
    //      sentinel at the start of the decl stream; scope extends to the
    //      end of that source file's decls (or until a later `__module`).
    //   2. Block form `module Foo.Bar { ... }` — emits a `__module Foo.Bar`
    //      sentinel, then the inner decls, then a `__module_end` sentinel.
    //      The stack-based replay below pops on `__module_end` so the
    //      enclosing namespace is restored at the closing brace.
    //   3. `use std.X;` imports — ImportResolver prepends each imported
    //      file's decls (which carry their own `__module` sentinel) to
    //      unit.declarations. Imported decls MUST NOT contaminate user-side
    //      scope, so on the first non-imported decl we unconditionally pop
    //      back to the user's own module context.
    //
    // To make the `use X;` → user-scope transition robust under nesting,
    // we pre-scan for the user's top-level `__module` (non-imported) and
    // use that as the baseline. The scoping stack only grows on explicit
    // `{` blocks or file-wide sentinels encountered AFTER we've entered
    // user-side territory.
    {
        // Extract "last segment" of a dotted path, e.g. `std.collections` → `collections`.
        auto lastSegment = [](std::string_view s) -> std::string {
            auto dot = s.rfind('.');
            return std::string(dot == std::string_view::npos ? s : s.substr(dot + 1));
        };

        std::string userModule;
        for (auto& decl : unit.declarations) {
            if (!decl || decl->isImported) continue;
            if (decl->kind == DeclKind::Import) {
                auto* imp = decl->as<ImportDecl>();
                if (!imp->importNames.empty() &&
                    imp->importNames[0] == "__module" &&
                    !imp->importPath.empty()) {
                    userModule = lastSegment(imp->importPath.back());
                    break;
                }
            }
        }

        // Stack-based replay. Each `__module X` push adds a frame; each
        // `__module_end` pops one. Between-frame gaps use the top of stack.
        std::vector<std::string> modStack;
        modStack.push_back(userModule);
        bool sawUserDecl = false;
        for (auto& decl : unit.declarations) {
            if (!decl) continue;
            // The moment we cross from imported decls into user decls,
            // forget any import-side module that was last in effect — the
            // user's own code lives in `userModule` (or `<global>` if
            // the file has no `module` at all).
            if (!decl->isImported && !sawUserDecl) {
                modStack.clear();
                modStack.push_back(userModule);
                sawUserDecl = true;
            }
            if (decl->kind == DeclKind::Import) {
                auto* imp = decl->as<ImportDecl>();
                if (!imp->importNames.empty()) {
                    const auto& marker = imp->importNames[0];
                    if (marker == "__module" && !imp->importPath.empty()) {
                        modStack.push_back(lastSegment(imp->importPath.back()));
                    } else if (marker == "__module_end") {
                        if (modStack.size() > 1) modStack.pop_back();
                    }
                }
            }
            declModule_[decl.get()] = modStack.back();
        }
    }

    // Pass 0: detect module declaration, import aliases, and use-imports
    for (auto& decl : unit.declarations) {
        if (!decl || decl->kind != DeclKind::Import) continue;
        auto* imp = decl->as<ImportDecl>();
        if (!imp->importNames.empty() && imp->importNames[0] == "__module" && !imp->importPath.empty()) {
            if (!decl->isImported) {
                currentModule_ = imp->importPath[0];
            }
        }
        for (auto& name : imp->importNames) {
            if (name.starts_with("__fn_alias:")) {
                auto payload = name.substr(11);
                auto eq = payload.find('=');
                if (eq != std::string::npos) {
                    std::string alias = payload.substr(0, eq);
                    std::string original = payload.substr(eq + 1);
                    functionAliases_[alias] = original;
                }
            } else if (name.starts_with("__alias:")) {
                std::string alias = name.substr(8);
                std::string fullPath;
                for (size_t i = 0; i < imp->importPath.size(); ++i) {
                    if (i > 0) fullPath += ".";
                    fullPath += imp->importPath[i];
                }
                importAliases_[alias] = fullPath;
            } else if (name != "__module" && name != "*") {
                std::string modulePath;
                for (size_t i = 0; i < imp->importPath.size(); ++i) {
                    if (i > 0) modulePath += ".";
                    modulePath += imp->importPath[i];
                }
                importedSymbols_[name] = modulePath + "." + name;
            }
        }
    }

    // Pass 0.5: inject prelude-style variant symbols (e.g. `Some`/`None` from
    // std.core's `Option`) into the global scope when the TU requests them via
    // a matching `use` directive. Runs after module/alias tracking so every
    // `ErrorDefDecl` already has its enclosing-module attribution in
    // `declModule_`, and before type/function registration so the injected
    // symbols participate in later lookups.
    applyPreludeImports(unit);

    // R5 pre-pass: harvest `@[lang_item("slot")]` attributes into
    // langItems_.  Runs before the per-decl analysis so downstream Sema
    // paths that consult the registry always see a complete picture.
    scanLangItemAttributes(unit);

    // PLAN_SEMA_ROOT_FIX — S3: Phase A discovery.
    //
    // Walks every top-level declaration in source order and records
    // every `impl <Trait> for <Primitive>` block as a fact in
    // `factBase_`. Two things flow from this single pass:
    //
    //   1. The `primitiveMethodImpls_` side-map (used by CodeGen and
    //      by `verifyMethodConstraints`'s method-name lookup) is
    //      populated. The side-map is intentionally not migrated into
    //      FactBase yet — CodeGen still consults it directly, and
    //      moving it out is S5 / S7 territory.
    //
    //   2. `factBase_` learns every prim trait satisfaction. After the
    //      walk we `freeze()` the base. Any subsequent attempt to write
    //      via `recordPrimImplFact` would be a contract violation: the
    //      design forbids "on-demand re-registration", which is exactly
    //      the patch we are deleting (S2's `ensurePrimitiveImplRegistered`
    //      lambda is gone in this commit).
    //
    // Why here, not inside `analyzeClassDecl`: Pass 1's `resolveType`
    // can recursively instantiate generics whose where-clauses already
    // need primitive trait facts (`Vec<Dict<string, i64>>` triggers
    // `K: Hashable + Eq` on string before std/hash.vyx's impl block is
    // visited in declaration order). Discovery has to complete before
    // any consumer fires.
    for (auto& decl : unit.declarations) {
        if (!decl || decl->kind != DeclKind::Class) continue;
        auto* cd = decl->as<ClassDecl>();
        if (!cd || !cd->isImplBlock || !isPrimitiveTypeName(decl->name)) continue;
        for (auto& m : cd->methods) {
            primitiveMethodImpls_[decl->name][m.name] = &m;
        }
        for (auto& traitName : cd->interfaces) {
            recordPrimImplFact(decl->name, traitName);
        }
    }
    factBase_.freeze();

    // First pass: register all type declarations. Duplicate class/struct/
    // interface/errordef declarations in the same namespace are rejected here
    // with a C++-style redefinition diagnostic (ODR check).
    for (auto& decl : unit.declarations) {
        if (!decl) continue;
        const std::string& declNs = declModule_[decl.get()];
        if (decl->kind == DeclKind::TypeAlias) {
            auto* ta = decl->as<TypeAliasDecl>();
            if (ta->aliasType) {
                if (ta->isNewtype) {
                    if (checkTypeRedefinition(declNs, *decl)) continue;
                    auto type = std::make_shared<VyxType>();
                    type->name = decl->name;
                    type->kind = VyxTypeKind::Struct;
                    symbols_.registerType(decl->name, type);
                } else {
                    auto resolvedType = resolveType(*ta->aliasType);
                    symbols_.registerType(decl->name, resolvedType);
                }
            }
        }
        if (decl->kind == DeclKind::Struct || decl->kind == DeclKind::Class ||
            decl->kind == DeclKind::Interface || decl->kind == DeclKind::ErrorDef) {
            if (decl->kind == DeclKind::Class && decl->as<ClassDecl>()->isImplBlock)
                continue;
            // P2b: Partial specializations are not registered as independent
            // named types in the symbol table (their base name "Pair" is already
            // occupied by the primary template). They are found via
            // selectPartialSpec() at use-site and then instantiated through the
            // normal instantiateClassTemplate path. We do run the ODR check to
            // catch verbatim duplicate partial patterns.
            if (decl->isPartialSpecialization) {
                checkTypeRedefinition(declNs, *decl);
                continue;
            }
            if (checkTypeRedefinition(declNs, *decl)) continue;
            auto type = std::make_shared<VyxType>();
            type->name = decl->name;
            switch (decl->kind) {
                case DeclKind::Struct:    type->kind = VyxTypeKind::Struct; break;
                case DeclKind::Class:     type->kind = VyxTypeKind::Class; break;
                case DeclKind::Interface: type->kind = VyxTypeKind::Interface; break;
                case DeclKind::ErrorDef:  type->kind = VyxTypeKind::ErrorType; break;
                default: break;
            }

            // ── P4-B move checker: compute `isHeapOwning` ───────────────
            // A class/struct is heap-owning when either:
            //   (a) it carries a `@[lang_item(...)]` attribute whose slot
            //       is in a curated list of container / smart-ptr / string
            //       slots that logically own heap storage, OR
            //   (b) it declares a `fn drop()` method (explicit opt-in for
            //       user-defined RAII types).
            // Heap-owning types move-by-default; non-heap-owning (POD) stay
            // copy-by-default. Interfaces and error-defs never qualify.
            if (decl->kind == DeclKind::Class || decl->kind == DeclKind::Struct) {
                static const std::set<std::string> kHeapLangItems = {
                    "vec", "dict", "hashmap", "hashset",
                    "set", "stack", "queue", "deque", "list", "btree",
                    "string", "box", "ref", "scope",
                };
                for (auto& [attrName, attrValue] : decl->attributes) {
                    if (attrName != "lang_item") continue;
                    std::string slot = attrValue;
                    if (slot.size() >= 2 && slot.front() == '"' && slot.back() == '"')
                        slot = slot.substr(1, slot.size() - 2);
                    if (kHeapLangItems.count(slot)) {
                        type->isHeapOwning = true;
                        break;
                    }
                }
                if (!type->isHeapOwning) {
                    // Check for a user-declared `fn drop()` method. We consult
                    // the AST directly — the VyxType's method table isn't
                    // filled until analyzeClassDecl runs in a later pass.
                    if (decl->kind == DeclKind::Class) {
                        auto* cd = decl->as<ClassDecl>();
                        for (auto& m : cd->methods) {
                            if (m.name == "drop" && m.params.empty()) {
                                type->isHeapOwning = true;
                                break;
                            }
                        }
                    }
                }
            }
            // Phase 8: @[derive(...)] method synthesis happens later, in
            // synthesizeDeriveMethods() run between type registration and
            // per-decl analysis. That pass inserts real AST MethodDecls onto
            // cd->methods so they're analyzed through the normal pipeline —
            // no need to stub up MethodSig entries here. (The old stub-only
            // handler was removed because its signatures went through
            // neither Sema method-body analysis nor CodeGen emission,
            // leading to unresolved method calls at the call site.)
            if (decl->kind == DeclKind::Interface) {
                auto* ifaceDecl = decl->as<InterfaceDecl>();
                std::vector<std::string> assocNames;
                assocNames.reserve(ifaceDecl->associatedTypes.size());
                for (auto& at : ifaceDecl->associatedTypes) {
                    assocNames.push_back(at.name);
                    if (at.defaultType) {
                        auto resolvedDefault = resolveType(*at.defaultType);
                        symbols_.registerType(decl->name + "::" + at.name, resolvedDefault);
                        // P2-generics C5: per-trait default for assoc fallback.
                        implAssocByTrait_[decl->name + "::" + at.name] = resolvedDefault;
                    }
                }
                if (!assocNames.empty()) {
                    traitAssocTypes_[decl->name] = std::move(assocNames);
                }
                // Trait composition (2026-04-23): record direct supertraits.
                //   `trait Num : Add + Sub + Mul + Div + Zero + One {}`
                // Transitive closure + cycle detection happens later, on
                // demand, in expandTraitWithSupertraits().
                std::vector<std::string> supers;
                if (!ifaceDecl->parentName.empty()) {
                    supers.push_back(ifaceDecl->parentName);
                }
                for (auto& s : ifaceDecl->interfaces) {
                    supers.push_back(s);
                }
                if (!supers.empty()) {
                    traitSupertraits_[decl->name] = std::move(supers);
                }
            }
            // Name registration:
            //   - `decl->name` is the primary key. For block-form
            //     `module Foo.Bar { class Point { ... } }`, the parser
            //     has already renamed the class to `Foo.Bar.Point` AND
            //     rewritten every short-name reference inside that block
            //     into the qualified form, so no short-name alias is
            //     needed (and registering one would re-enable the
            //     collision we just solved for two `class Point` in
            //     disjoint namespaces).
            //   - For file-wide `module X;`, the qualified mirror under
            //     `currentModule_` is still registered so imported std
            //     lookups (`std.collections.Vec`) resolve.
            symbols_.registerType(decl->name, type);
            if (!currentModule_.empty() &&
                decl->name.find('.') == std::string::npos) {
                symbols_.registerType(currentModule_ + "." + decl->name, type);
                symbolVisibility_[currentModule_ + "." + decl->name] = decl->isExport;
            }
        }
        if (decl->kind == DeclKind::ExternBlock) {
            auto* eb = decl->as<ExternBlockDecl>();
            for (auto& child : eb->externDecls) {
                if (!child) continue;
                if (child->kind == DeclKind::Struct || child->kind == DeclKind::Class) {
                    if (checkTypeRedefinition(declNs, *child)) continue;
                    auto type = std::make_shared<VyxType>();
                    type->name = child->name;
                    type->kind = (child->kind == DeclKind::Class) ? VyxTypeKind::Class : VyxTypeKind::Struct;
                    if (child->kind == DeclKind::Struct) {
                        auto* sd = child->as<StructDecl>();
                        for (auto& field : sd->fields) {
                            if (field.type) {
                                VyxType::Field f;
                                f.name = field.name;
                                f.type = resolveType(*field.type);
                                f.isPublic = (field.visibility == Visibility::Public);
                                type->fields.push_back(std::move(f));
                            }
                        }
                    } else {
                        auto* cd = child->as<ClassDecl>();
                        for (auto& field : cd->fields) {
                            if (field.type) {
                                VyxType::Field f;
                                f.name = field.name;
                                f.type = resolveType(*field.type);
                                f.isPublic = (field.visibility == Visibility::Public);
                                type->fields.push_back(std::move(f));
                            }
                        }
                        for (auto& method : cd->methods) {
                            VyxType::MethodSig sig;
                            sig.name = method.name;
                            sig.isPublic = (method.visibility == Visibility::Public);
                            sig.returnType = method.returnType ? resolveType(*method.returnType) : types::makeVoid();
                            for (auto& param : method.params) {
                                sig.paramTypes.push_back(param.type ? resolveType(*param.type) : types::makeUnknown());
                            }
                            type->methods.push_back(std::move(sig));
                        }
                    }
                    symbols_.registerType(child->name, type);
                }
            }
        }
    }

    // P4-A.2: back-compat short-name alias for std-auto-qualified types.
    // The parser's `autoQualifyStdModule` pass renames every top-level
    // type in a `module std.X;` file to `std.X.Name`. Downstream code
    // (user TUs calling `StringBuilder::create()`, rewriteDottedModulePaths
    // stripping the `std.collections` prefix off a member-access chain, …)
    // still references these types by their bare short name. Register a
    // short-name entry pointing at the same VyxType — but ONLY if no user
    // decl has already claimed the bare name. This preserves the fix for
    // ODR (user's `struct StringBuilder` wins against std's; the std type
    // remains reachable via its qualified name) while keeping every
    // existing reference site working unchanged.
    //
    // Order matters: this runs AFTER the primary registration pass above,
    // so every user-declared bare-name type is already in the registry.
    // A user decl under the same short name means `lookupType` already
    // returns it directly at SemaResolve.cpp:888, and we skip the alias.
    for (auto& decl : unit.declarations) {
        if (!decl) continue;
        if (decl->kind != DeclKind::Class && decl->kind != DeclKind::Struct &&
            decl->kind != DeclKind::Interface && decl->kind != DeclKind::ErrorDef &&
            decl->kind != DeclKind::TypeAlias) {
            continue;
        }
        if (decl->kind == DeclKind::Class && decl->as<ClassDecl>()->isImplBlock) continue;
        if (decl->isPartialSpecialization) continue;
        const std::string& qualified = decl->name;
        auto lastDot = qualified.rfind('.');
        if (lastDot == std::string::npos) continue;  // bare name already
        std::string shortName = qualified.substr(lastDot + 1);
        if (shortName.empty()) continue;
        if (symbols_.lookupType(shortName)) continue;  // user claim wins
        auto qualType = symbols_.lookupType(qualified);
        if (!qualType) continue;
        symbols_.registerType(shortName, qualType);
    }

    size_t declCount = unit.declarations.size();

    // Pre-register all function signatures. Before inserting into the symbol
    // table, verify there is no prior declaration of the same (namespace, name,
    // parameter types) tuple; that would violate the one-definition rule.
    std::string trackModule;
    for (size_t i = 0; i < declCount; ++i) {
        if (!unit.declarations[i]) continue;
        auto& d = *unit.declarations[i];
        if (d.kind == DeclKind::Import) {
            auto* imp = d.as<ImportDecl>();
            if (!imp->importNames.empty() && imp->importNames[0] == "__module" && !imp->importPath.empty()) {
                trackModule = imp->importPath.back();
                auto dot = trackModule.rfind('.');
                if (dot != std::string::npos) trackModule = trackModule.substr(dot + 1);
            }
        }
        if (d.kind == DeclKind::Function && d.genericParams.empty()) {
            // Skip function decls whose @[platform("X")] doesn't match the
            // current target OS, so mutually-exclusive OS variants (e.g.
            // std/vio.vyx's Windows/Linux/macOS `vio_start` trio) don't
            // trip the redefinition check.
            bool platformMismatch = false;
            for (auto& [an, av] : d.attributes) {
                if (an != "platform") continue;
#ifdef _WIN32
                if (av != "windows") platformMismatch = true;
#elif __linux__
                if (av != "linux") platformMismatch = true;
#elif __APPLE__
                if (av != "macos") platformMismatch = true;
#endif
            }
            if (platformMismatch) continue;
            auto* fn = d.as<FunctionDecl>();
            std::vector<VyxTypePtr> paramTypes;
            paramTypes.reserve(fn->params.size());
            for (auto& param : fn->params) {
                paramTypes.push_back(param.type ? resolveType(*param.type) : types::makeUnknown());
            }
            const std::string& declNs2 = declModule_[&d];
            if (checkFunctionRedefinition(declNs2, d, paramTypes, /*isGeneric=*/false, /*arity=*/0)) {
                // Duplicate definition already reported; keep existing symbol.
                continue;
            }

            // Phase 9: free function overloading. Track every non-generic
            // overload by its unmangled (original) name in overloadGroups_.
            // When a *second* overload arrives with a different signature,
            // mangle BOTH the previous decl(s) and the new decl into unique
            // symbols (`name$$<paramSig>`) so CodeGen emits distinct LLVM
            // functions and the call site can dispatch on argument types.
            // Special-case names (`main`) and generic-only siblings stay
            // unmangled.
            const std::string unmangledName = d.name;
            const bool isMainFn = (unmangledName == "main");
            auto& group = overloadGroups_[unmangledName];
            // Skip duplicates that overloadGroups_ has already absorbed:
            //   * same Decl* (Pass 1 may reach the same FunctionDecl twice
            //     when the import resolver inlines the same file under
            //     multiple module paths, or when analyze() runs repeatedly).
            //   * same source location (defensive fallback).
            //   * same parameter signature (different file, same module name
            //     OR different module names — e.g. an stdlib file declared in
            //     the project root AND its `tests/projects/<x>/std/` mirror
            //     copy. The ODR check key is `<module>::<name>`, so cross-
            //     module duplicates leak through; collapsing here keeps
            //     overload resolution unambiguous and matches user intent
            //     ("there is one `str_substring(string, i64, i64)`" — the
            //     mirror copy is treated as a re-export).
            bool alreadyTracked = false;
            for (auto& e : group) {
                if (e.decl == &d) { alreadyTracked = true; break; }
                if (e.decl &&
                    e.decl->location.filename == d.location.filename &&
                    e.decl->location.line == d.location.line &&
                    e.decl->location.column == d.location.column) {
                    alreadyTracked = true;
                    break;
                }
                if (!e.isGeneric && paramTypesEqual(e.paramTypes, paramTypes)) {
                    alreadyTracked = true;
                    break;
                }
            }
            if (alreadyTracked) continue;

            // Detect whether the symbol table already binds this name to a
            // SOLE-overload entry (not yet mangled). If we're about to add a
            // distinct second overload, retroactively mangle the previous
            // entry's decl->name + symbol.
            bool retroMangle = false;
            if (!isMainFn && group.size() == 1 && !group[0].isGeneric) {
                if (!paramTypesEqual(group[0].paramTypes, paramTypes)) {
                    retroMangle = true;
                }
            }
            // Also retroactively mangle the previous if any prior decl was
            // sole-unmangled (group.size() == 1 case above) — but if group
            // already has > 1 entries, those are already mangled.
            if (retroMangle) {
                auto& first = group[0];
                std::string firstMangled = mangleOverloadName(unmangledName, first.paramTypes);
                if (first.decl) first.decl->name = firstMangled;
                first.mangledName = firstMangled;
                // Re-key the symbol entry. Symbol stored as `unmangledName`
                // gets dropped; new symbol declared under firstMangled.
                Symbol* prevSym = symbols_.lookup(unmangledName);
                if (prevSym) {
                    Symbol copy = *prevSym;
                    copy.name = firstMangled;
                    // We can't erase from the unordered_map directly via the
                    // public Scope API; but a stale binding under the
                    // unmangled name would shadow the overload group during
                    // analyzeCall. Mark the symbol's name field so later
                    // call-site code knows the unmangled lookup is stale —
                    // the analyzeCall override path will check overloadGroups_
                    // first regardless. We do declare the mangled symbol now.
                    if (!symbols_.lookup(firstMangled)) {
                        symbols_.declare(firstMangled, copy);
                    }
                }
            }

            // Decide whether THIS new overload should be mangled. It's mangled
            // when the group already has any entry (sole or multi).
            bool mangleThis = !isMainFn && (!group.empty());
            std::string declName = mangleThis
                ? mangleOverloadName(unmangledName, paramTypes)
                : unmangledName;

            // Mutate decl->name to the chosen form so CodeGen forward-decl /
            // emitFunctionDecl pick up the mangled symbol naturally.
            if (mangleThis) d.name = declName;

            if (!symbols_.lookup(declName)) {
                VyxTypePtr retType = fn->returnType ? resolveType(*fn->returnType) : types::makeVoid();
                Symbol fnSym;
                fnSym.name = declName;
                fnSym.isFunction = true;
                fnSym.returnType = retType;
                fnSym.type = retType;
                fnSym.declLocation = d.location;
                fnSym.isImported = d.isImported;
                fnSym.isExported = d.isExport;
                fnSym.paramCount = fn->params.size();
                fnSym.paramTypes = paramTypes;
                for (auto& [an, _av] : d.attributes) {
                    if (an == "borrow_args") {
                        fnSym.borrowsArgs = true;
                        break;
                    }
                }
                symbols_.declare(declName, fnSym);
                if (d.isImported && !trackModule.empty()) {
                    // Track only for the unmangled identity — overloads sharing
                    // a name from different modules collapse onto one source
                    // for diagnostic purposes (analyzeCall consults the
                    // overload group, not this map, when picking).
                    auto existIt = functionSourceModule_.find(unmangledName);
                    if (existIt != functionSourceModule_.end() && existIt->second != trackModule) {
                        // With overloading we no longer flag this as
                        // ambiguous: the call site disambiguates by arg types.
                    }
                    functionSourceModule_[unmangledName] = trackModule;
                }
            }

            // Record in overload group AFTER symbol setup.
            OverloadEntry entry;
            entry.decl = &d;
            entry.paramTypes = paramTypes;
            entry.mangledName = mangleThis ? declName : std::string{};
            entry.isGeneric = false;
            group.push_back(std::move(entry));
        }
        if (d.kind == DeclKind::GlobalVar && !symbols_.lookup(d.name)) {
            auto* gv = d.as<GlobalVarDecl>();
            VyxTypePtr varType = gv->varType ? resolveType(*gv->varType) : nullptr;
            if (!varType && gv->initBody && gv->initBody->kind == StmtKind::ExprStmt) {
                auto* es = gv->initBody->as<ExprStmt>();
                if (es->expr) varType = analyzeExpr(*es->expr);
            }
            if (!varType) varType = types::makeUnknown();
            Symbol sym;
            sym.name = d.name;
            sym.type = varType;
            sym.isConst = !gv->isMutableVar;
            sym.declLocation = d.location;
            symbols_.declare(d.name, std::move(sym));
        }
    }

    auto shouldSkipDecl = [](Decl* d) -> bool {
        for (auto& [attrName, attrValue] : d->attributes) {
            if (attrName == "platform") {
#ifdef _WIN32
                if (attrValue != "windows") return true;
#elif __linux__
                if (attrValue != "linux") return true;
#elif __APPLE__
                if (attrValue != "macos") return true;
#endif
            }
        }
        return false;
    };

    // Phase 8: synthesize `@[derive(Trait, ...)]` methods onto class/struct
    // declarations before the per-decl pass runs. The injected MethodDecls
    // carry real AST bodies (e.g. `return self.x == other.x && ...` for Eq)
    // so they go through normal Sema via analyzeClassDecl's method-signature
    // registration and analyzeAllMethodBodies' body-analysis pass, no
    // CodeGen-level hook needed.
    synthesizeDeriveMethods(unit);

    // Split the per-decl pass in two:
    //   1. Type declarations (classes, structs, interfaces, errordefs, etc.)
    //      — their method signatures land in VyxType::methods.
    //   2. Structural trait-default injection — fills in default-body methods
    //      on classes that match a trait's required-method set without an
    //      explicit `: Trait` bound.
    //   3. Function declarations + their bodies — call sites calling
    //      `bag.is_empty()` must see the injected method.
    for (size_t i = 0; i < declCount; ++i) {
        if (!unit.declarations[i]) continue;
        if (unit.declarations[i]->kind == DeclKind::Function) continue;
        if (shouldSkipDecl(unit.declarations[i].get())) continue;
        analyzeDecl(*unit.declarations[i]);
    }
    injectStructuralTraitDefaults(unit);
    for (size_t i = 0; i < declCount; ++i) {
        if (!unit.declarations[i]) continue;
        if (unit.declarations[i]->kind != DeclKind::Function) continue;
        if (shouldSkipDecl(unit.declarations[i].get())) continue;
        analyzeDecl(*unit.declarations[i]);
    }

    // Second dedicated pass: type-check every class / interface / ErrorDef
    // method body (see analyzeAllMethodBodies / analyzeMethodBodiesGeneric
    // in SemaDecl.cpp).
    //
    // Design notes (P1c):
    //   * Generic class-level params (e.g. `T` in `class Vec<T>`) are pushed
    //     into activeGenericParams_ by analyzeAllMethodBodies before entering
    //     analyzeMethodBodiesGeneric, so resolveNamedType sees them and
    //     returns Generic(T) rather than "undefined type T".
    //   * Instantiated monomorphs (`Vec<i32>` etc.) live in
    //     pendingInstantiations_ and are appended to unit.declarations AFTER
    //     this call, so they are never re-analysed here — their bodies still
    //     carry the un-substituted `var out: T = 0;` pattern which is
    //     intentional and handled by CodeGen / Mono.
    //   * Method-level generics (`fn map<U>`) are pushed/popped inside
    //     analyzeMethodBodiesGeneric per-method with no cross-method leakage.
    //   * `self` is bound to the raw class VyxType (base name, concrete
    //     fields) so field access (`self.data`, `self.len`) resolves normally
    //     even inside a generic class body.
    analyzeAllMethodBodies(unit);

    // Check for circular Ref<> references
    std::map<std::string, std::set<std::string>> refGraph;
    for (auto& decl : unit.declarations) {
        if (!decl) continue;
        if (decl->kind != DeclKind::Struct && decl->kind != DeclKind::Class) continue;
        auto& fields = (decl->kind == DeclKind::Struct)
            ? decl->as<StructDecl>()->fields
            : decl->as<ClassDecl>()->fields;
        for (auto& field : fields) {
            if (field.isWeak) continue;
            if (field.type && field.type->kind == TypeAnnotationKind::Generic &&
                (field.type->name == "Ref" || field.type->name == "Scope")) {
                auto* gt = field.type->as<GenericType>();
                if (!gt->typeArgs.empty()) {
                    refGraph[decl->name].insert(gt->typeArgs[0]->name);
                }
            }
        }
    }
    {
        std::set<std::string> reported;
        for (auto& [start, _] : refGraph) {
            std::set<std::string> visited, inStack;
            std::vector<std::string> path;
            std::function<void(const std::string&)> dfs = [&](const std::string& node) {
                if (inStack.count(node)) {
                    std::string cycle;
                    bool found = false;
                    for (auto& p : path) {
                        if (p == node) found = true;
                        if (found) { if (!cycle.empty()) cycle += " -> "; cycle += p; }
                    }
                    cycle += " -> " + node;
                    if (!reported.count(cycle)) {
                        reported.insert(cycle);
                        diag_.warning(semaInternalSourceLocation(),
                            "circular Ref<> chain: {}; use 'weak' to break the cycle", cycle);
                    }
                    return;
                }
                if (visited.count(node)) return;
                visited.insert(node);
                inStack.insert(node);
                path.push_back(node);
                auto it = refGraph.find(node);
                if (it != refGraph.end()) {
                    for (auto& next : it->second) dfs(next);
                }
                path.pop_back();
                inStack.erase(node);
            };
            dfs(start);
        }
    }

    for (auto& inst : pendingInstantiations_) {
        unit.declarations.push_back(std::move(inst));
    }
    pendingInstantiations_.clear();

    // PLAN_SEMA_ROOT_FIX — S3: surface the read-only solver telemetry
    // once per analyze() invocation.
    dumpSolverReconciliationStats();

    // PLAN_SEMA_ROOT_FIX — S4: surface the scheduler ledger so the
    // sidecar's coverage of `pendingInstantiations_` is visible to CI
    // ahead of the S5 switchover.
    dumpMonoSchedulerStats();
}

} // namespace vyx
