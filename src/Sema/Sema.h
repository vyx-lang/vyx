#pragma once
#include "Type.h"
#include "SymbolTable.h"
#include "TemplateResolver.h"
#include "LangItemRegistry.h"
#include "Canonicalizer.h"
#include "TraitSolver.h"
#include "MonoScheduler.h"
#include "../Parser/AST.h"
#include "../Common/Diagnostics.h"
#include "../Common/Determinism.h"
#include <unordered_map>
#include <set>
#include <map>
#include <memory>

namespace vyx {

/// Stores information about an instantiated template function
struct TemplateInstance {
    DeclPtr originalDecl;           // Original generic function declaration
    DeclPtr instantiatedDecl;       // Instantiated function with concrete types
    std::vector<VyxTypePtr> argTypes; // Concrete types used for instantiation
    std::string mangledName;        // Unique mangled name for this instance
};

class Sema {
public:
    explicit Sema(DiagnosticsEngine& diag);

    void analyze(TranslationUnit& unit);

    // P2b Wave 3+: expose the associated-type template registry for Mono.
    // Called by the driver after analyze() completes, before Mono::run().
    const std::map<std::string,
        std::map<std::string, const TypeAnnotation*>>&
    getAssocTemplateMap() const { return implAssocByTemplate_; }

    // R5: expose the lang-item registry so Mono / CodeGen can look up
    // well-known stdlib declarations (Option / Result / String / …) by
    // slot name rather than hard-coded identifiers.  Populated during
    // Sema pre-pass (see scanLangItemAttributes).
    const LangItemRegistry& getLangItems() const { return langItems_; }
    LangItemRegistry& getLangItemsMut() { return langItems_; }

    // PLAN_SEMA_ROOT_FIX — S5: driver hands this shared scheduler to Mono
    // after Sema analysis so both phases write to the same request ledger.
    canon::MonoScheduler& getMonoSchedulerMut() { return monoScheduler_; }
    const canon::MonoScheduler& getMonoScheduler() const { return monoScheduler_; }

    // Phase 8: static trait method dispatch (T::method() where T is a generic
    // param with a trait bound). Query whether the substituted concrete type
    // has a method with this name, either via:
    //   * primitive-target impl (`impl Trait for i32 { fn m() { ... } }`) —
    //     looked up in primitiveMethodImpls_,
    //   * class method or class-target impl — looked up in the class's
    //     method table (concreteType.methods), OR
    //   * ClassDecl methods found via the translation unit (for classes
    //     whose trait-impl methods were installed after the type was first
    //     created in the symbol table).
    // When true, `lowerTypeReflectInExpr` rewrites `T.method(...)` to
    // `<ConcreteName>.method(...)` so CodeGen's regular static-method
    // dispatch resolves it (same mangling as `i32.hash` / `MyClass.foo`).
    bool hasStaticMethodForConcreteType(const VyxType& concreteType,
                                        const std::string& methodName) const;

private:
    // R5: walk every decl and harvest `@[lang_item("slot")]` attributes
    // into langItems_.  Runs before analyzeDecl so downstream Sema
    // passes, Mono, and CodeGen can consult the registry.  Silently
    // tolerates units that do not import the corresponding stdlib module
    // — downstream lookups return nullptr and the affected sugar (the
    // `?` operator, auto-wrap return, Option/Result match) simply won't
    // trigger, leaving the normal "undefined type" / "unknown
    // identifier" diagnostic for the user to follow.
    void scanLangItemAttributes(TranslationUnit& unit);

    void registerBuiltinTypes();
    void registerBuiltinFunctions();

    // Selectively import enum variants from specific standard-library modules
    // into the global lexical scope (prelude-style). Currently injects
    // `Option::Some` / `Option::None` as bare identifiers when the translation
    // unit carries a `use std.core;` directive. This keeps user code terse
    // (`return Some(x); return None;`) without hard-coding these symbols as
    // language built-ins — they remain ordinary members of the Option ADT.
    void applyPreludeImports(TranslationUnit& unit);

    // Declaration analysis
    void analyzeDecl(Decl& decl);
    void analyzeFunctionDecl(Decl& decl);
    void analyzeStructDecl(Decl& decl);
    void analyzeClassDecl(Decl& decl);

    // Diagnose method-level generic parameter names that shadow the enclosing
    // class/struct/errordef/interface's generic parameters. Silent shadowing
    // causes subtle type pollution because the outer/inner substitution maps
    // are merged into a single lookup map (later writes win). Force the user
    // to pick distinct identifiers (e.g. rename `fn test<T>` to `fn test<U>`).
    void checkMethodGenericShadowing(const Decl& outerDecl, const MethodDecl& method);

    // Statement analysis
    void analyzeStmt(Stmt& stmt);
    void analyzeBlock(Stmt& block);
    void analyzeVarDecl(Stmt& stmt);
    void analyzeIfStmt(Stmt& stmt);
    void analyzeWhileStmt(Stmt& stmt);
    void analyzeForStmt(Stmt& stmt);
    void analyzeReturnStmt(Stmt& stmt);

    // Expression analysis.
    // analyzeExpr() is the public entry point: it runs analyzeExprImpl() and
    // persists the resulting type onto expr.inferredType so Codegen can read
    // it without resorting to variable-name side-maps (Phase D refactor).
    VyxTypePtr analyzeExpr(Expr& expr);
    VyxTypePtr analyzeExprImpl(Expr& expr);
    VyxTypePtr analyzeBinaryOp(Expr& expr);
    VyxTypePtr analyzeUnaryOp(Expr& expr);
    VyxTypePtr analyzeCall(Expr& expr);
    VyxTypePtr analyzeMemberAccess(Expr& expr);
    VyxTypePtr analyzeIndex(Expr& expr);

    // Type resolution
    VyxTypePtr resolveType(const TypeAnnotation& annotation);
    VyxTypePtr resolveNamedType(const std::string& name, SourceLocation loc = {});
    // Infer generic type parameters for a bare struct-init expression (e.g.
    // `Cache { value: 42 }` → Cache<i64, i64>) by matching field names against
    // the class template's declared field type annotations. Returns nullptr if
    // structName does not correspond to a generic template.
    VyxTypePtr inferGenericStructInit(
        const std::string& structName,
        const std::vector<std::pair<std::string, VyxTypePtr>>& fieldTypes,
        SourceLocation loc);

    // Type checking
    // The ABI-level `string` type is distinct from the stdlib-owned String
    // class. Only the class registered as `@[lang_item("string")]` may flow
    // into that ABI representation.
    bool isRegisteredStringLangItem(const VyxType& type) const;
    bool isAssignable(const VyxType& target, const VyxType& source, SourceLocation at = {});
    VyxTypePtr commonType(const VyxType& a, const VyxType& b);
    bool checkNumericOp(const VyxType& type, SourceLocation loc);

    // Dedicated pass: analyse every class / interface / errordef method
    // body. Runs after the per-decl signature pass so every nested type,
    // struct field, and method signature is already registered before any
    // body sees them. Without this ordering, methods that construct or
    // return types declared later in the unit fail with spurious
    // "no field X" / "undefined type" errors.
    void analyzeAllMethodBodies(TranslationUnit& unit);

    // Structural trait-default injection: for each class not explicitly
    // listing a trait in `parentName`/`interfaces`, detect traits where the
    // class provides every required (bodyless) method. In that case clone
    // the trait's default-body methods onto the class's method table so
    // `let x = Bag.of(0).is_empty();` dispatches to the trait's default.
    // Purely additive: never removes signatures or overrides methods the
    // class declared itself. Runs after analyzeDecl's per-class pass.
    void injectStructuralTraitDefaults(TranslationUnit& unit);

    // Phase 8: parse `@[derive(Trait1, Trait2, ...)]` attributes on class /
    // struct declarations and synthesize the corresponding impl methods
    // based on the declaration's fields. Runs BEFORE analyzeDecl so the
    // injected methods participate in normal Sema (signatures land in
    // VyxType::methods and bodies are analyzed in Pass 2).
    //
    // Supported traits (field types must themselves satisfy the trait):
    //   Eq       → fn operator_eq(other: Self) -> bool
    //   Ord      → fn compare(other: Self) -> i32
    //   Hashable → fn hash() -> i64
    //   Clone    → fn clone() -> Self
    //   Copy     → marker (trait bound only, no method)
    //   Debug    → fn debug() -> string (ClassName{f1=v1,f2=v2} format)
    //   Display  → fn display() -> string (field-joined format)
    //
    // If a derived trait is already manually implemented on the class (the
    // user wrote the method body themselves), synthesis is skipped for that
    // trait. If a field's type cannot satisfy the derived trait, a clear
    // diagnostic is emitted pointing at the offending field.
    void synthesizeDeriveMethods(TranslationUnit& unit);

    // Generalised form used by class, interface (default bodies), and error
    // def (ADT methods). `selfType` is what `self` binds to; pass nullptr
    // for static-only decls. `methods` is the owning decl's method list.
    void analyzeMethodBodiesGeneric(Decl& decl,
                                    std::vector<MethodDecl>& methods,
                                    VyxTypePtr selfType);

    // Template instantiation
    void instantiateTemplateIfNeeded(const Expr& callExpr, const Decl* calleeDecl,
                                       const std::vector<VyxTypePtr>& argTypes,
                                       const std::vector<VyxTypePtr>* mangleArgsOverride = nullptr);
    // `constBindings` maps each non-type (`const N: usize`) parameter name to
    // its evaluated integer value at the call site. When non-empty the cloned
    // body has its `[T; N]`-style array-size expressions rewritten to the
    // concrete literal before Sema analyses the instance.
    void instantiateClassTemplate(const Decl& templateDecl,
        const std::string& mangledName,
        const std::vector<VyxTypePtr>& concreteTypes,
        SourceLocation useSite = {},
        const std::map<std::string, int64_t>& constBindings = {});
    DeclPtr duplicateDecl(const Decl* decl);
    DeclPtr createTemplateInstance(const Decl& genericDecl, const GenericSubstitution& subst);

    // ── P2-C1 Concepts: where T: Foo + Bar verification ──────────────────
    // One frame per active template instantiation. Pushed before the
    // satisfaction check, popped after. Forms the "instantiation chain"
    // that surfaces in P2C diagnostics.
    //
    // The underlying struct is now defined in Common/Diagnostics.h as
    // `vyx::InstantiationFrame` so Monomorphize.cpp (and any future pass)
    // can use the same type without depending on the full Sema header.
    // This alias preserves all existing Sema code that refers to the type
    // as `Sema::InstantiationFrame`.
    using InstantiationFrame = vyx::InstantiationFrame;
    std::vector<InstantiationFrame> instantiationStack_;

    // Front 1: maximum depth of the Sema-side instantiation stack. Once
    // exceeded, `instantiateClassTemplate` emits P2D-020 and returns
    // early — this prevents an infinite Sema-level recursion (e.g. a
    // class that references itself through an ever-growing generic arg)
    // from blowing the C++ call stack. 64 is ~8x the deepest frame any
    // legitimate test hits; self-hosting peaks at ~20.
    static constexpr size_t kMaxSemaInstantiationStack = 64;

    // Front 1: re-entry guard for where-clause verification. Keyed on
    // `typeMangled + "\x1f" + traitMangled`, inserted on entry and
    // erased on exit of verifyMethodConstraints' inner trait loop.
    // Re-entry with the same key indicates a recursive predicate such
    // as `class Foo<T> where Foo<T>: Eq<Foo<T>>` whose verification
    // would otherwise loop; we emit P2D-021 and break the cycle.
    std::set<std::string> activeConstraintResolution_;

    // P2D-001 de-duplication: same `(traitName, concreteTypeName)` pair
    // would otherwise be reported once per nested instantiation level
    // (outer/mid/inner all forward `T` -> MyTag through `where T: Add`).
    // Track which combinations have already been emitted so the user
    // sees the failure exactly once, with the deepest (= most informative)
    // chain attached.
    std::set<std::string> reportedConstraintFailures_;
    // Verify `genericConstraints` of `templateDecl` against `bindings` (the
    // generic-param → concrete-type substitution). Emits P2C-001 (missing
    // method), P2C-002 (signature mismatch) or P2C-003 (unknown trait) with
    // the current instantiation chain attached as `note`s + a `help` line.
    // `siteLoc` is the user-visible source location for the primary error.
    void verifyConstraints(
        const Decl& templateDecl,
        const std::map<std::string, VyxTypePtr>& bindings,
        SourceLocation siteLoc);
    // Same, but consumes a method's own genericConstraints map (for methods
    // whose where-clause talks about their own generic params).
    void verifyMethodConstraints(
        const std::string& contextName,
        const std::map<std::string, std::vector<std::string>>& constraints,
        const std::map<std::string, VyxTypePtr>& bindings,
        SourceLocation siteLoc);

    // ── P2D-004: const-integer where-clause predicate verification ────────
    // Evaluate each ConstPredicate against `constBindings`; emit P2D-004 on
    // violation with instantiation chain attached.
    void verifyConstPredicates(
        const Decl& templateDecl,
        const std::map<std::string, int64_t>& constBindings,
        SourceLocation siteLoc);

    // ── P2D-005/006: pack `all<Ts>/any<Ts>: Trait` constraint verification ─
    // For each PackConstraint, look up the pack binding in `bindings`,
    // iterate its members and check each against the trait.
    // `all` → every member must satisfy; first failure emits P2D-005.
    // `any` → at least one must satisfy; total failure emits P2D-006.
    void verifyPackConstraints(
        const Decl& decl,
        const std::map<std::string, VyxTypePtr>& bindings,
        SourceLocation siteLoc);
    // Emit "instantiation chain:" notes from the current stack; siteLoc is
    // the location of the triggering use site (for the first note line).
    void emitInstantiationChainNotes(SourceLocation siteLoc);
    // RAII helper.
    struct InstantiationGuard {
        Sema& sema;
        bool popOnExit;
        InstantiationGuard(Sema& s, InstantiationFrame f) : sema(s), popOnExit(true) {
            sema.instantiationStack_.push_back(std::move(f));
        }
        ~InstantiationGuard() { if (popOnExit) sema.instantiationStack_.pop_back(); }
        InstantiationGuard(const InstantiationGuard&) = delete;
        InstantiationGuard& operator=(const InstantiationGuard&) = delete;
    };
public:
    // P2-C6: file-static helpers in SemaGeneric.cpp need to materialise
    // expanded type-pack indices back into TypeAnnotations. Exposing this
    // as a public helper avoids forward-declaring an anonymous-namespace
    // friend (which doesn't link the way we want across TUs). The function
    // is a pure conversion utility and has no side effects on Sema state,
    // so widening visibility is safe.
    TypePtr convertTypeToAnnotation(const VyxTypePtr& vyxType);

    // P2-C6 Gap 1: expose the instantiation stack so that static helpers
    // (e.g. cloneTypeWithPackSubst) can attach chain notes to OOB diagnostics
    // without needing to be methods of Sema. Read-only; callers must not hold
    // the reference across any Sema call that may push/pop the stack.
    const std::vector<InstantiationFrame>& getInstantiationStack() const {
        return instantiationStack_;
    }
private:
    TypePtr cloneTypeAnnotation(const TypeAnnotation& src);
    // Variadic generic expansion
    DeclPtr createVariadicInstance(const Decl& genericDecl, const std::vector<VyxTypePtr>& argTypes);
    StmtPtr expandVariadicBody(const Stmt& body, const std::string& variadicParamName, const std::vector<VyxTypePtr>& argTypes);

    // Compile-time evaluation
    struct ComptimeValue {
        enum { Int, Float, Bool, String, None } kind = None;
        int64_t intVal = 0;
        double floatVal = 0.0;
        bool boolVal = false;
        std::string strVal;
    };
    ComptimeValue comptimeEval(const Expr& expr, const std::map<std::string, ComptimeValue>& env);
    ComptimeValue comptimeExecBody(const Stmt& body, std::map<std::string, ComptimeValue>& env);

    std::map<std::string, const Decl*> comptimeFunctions_;

    DiagnosticsEngine& diag_;
    SymbolTable symbols_;
    TranslationUnit* unit_ = nullptr;
    // R5: lang-item registry — populated by scanLangItemAttributes during
    // the Sema pre-pass and queried by Mono / CodeGen via getLangItems().
    LangItemRegistry langItems_;

    // PLAN_SEMA_ROOT_FIX — S3 (Solver switchover, primitive-only).
    //
    // Three pieces, FactBase is the sole authoritative store for
    // `(prim, trait)` satisfaction:
    //   * `canonicalizer_`  — name → opaque id wrapper (still identity at
    //                         S3; later phases interpose interning).
    //   * `factBase_`       — Phase A discovery store. Populated once
    //                         in the entry pre-scan, frozen before Pass 1,
    //                         read-only thereafter.
    //   * `traitSolver_`    — read-only query service backed by FactBase.
    //                         Every primitive trait-bound check in
    //                         `verifyMethodConstraints` consults it.
    //                         Nominal types still fall through to the
    //                         interface method walk.
    //
    // Declaration order matters: factBase_ must precede traitSolver_ because
    // the latter binds a const-reference to it during construction.
    canon::Canonicalizer canonicalizer_;
    canon::FactBase      factBase_;
    canon::TraitSolver   traitSolver_{factBase_};

    // PLAN_SEMA_ROOT_FIX — S5 (Scheduler-backed Mono telemetry).
    //
    // Sema records every push into `pendingInstantiations_`; Mono records
    // every accepted worklist enqueue and ready item through the same
    // instance once the driver wires it in.
    canon::MonoScheduler monoScheduler_;

    // S3 telemetry. Counts every primitive trait-bound query routed
    // through the solver; a zero count combined with a non-empty fact
    // base means discovery succeeded but no generic instantiation
    // exercised the prim path (i.e. the program contains no `where T:
    // Trait` with T bound to a primitive). A non-zero count combined
    // with an empty fact base is the canary for a discovery regression.
    mutable std::size_t solverPrimQueries_ = 0;

    // Phase A discovery entry-point. The single sanctioned channel for
    // recording `impl <Trait> for <Primitive>` facts into `factBase_`.
    // Idempotent and short/long-name aware (legacy callsites used the
    // last dotted segment as a fallback key, so the FactBase mirrors
    // the same expansion internally).
    void recordPrimImplFact(const std::string& primName,
                            const std::string& traitName);

    // Emit a single-line solver telemetry summary at the end of
    // `analyze()`. Diagnostic-only: never affects semantic results.
    void dumpSolverReconciliationStats();

    // PLAN_SEMA_ROOT_FIX — S4: emit a single-line scheduler ledger
    // summary at the end of `analyze()`. Diagnostic-only.
    void dumpMonoSchedulerStats();
    VyxTypePtr currentReturnType_;
    // Raw return-type annotation of the enclosing function. Used when the
    // resolved `currentReturnType_` is an enum type (Result/Option are ADTs
    // registered as `ErrorDef`-kind) and we still need the generic payload
    // types for `@[auto_wrap]` injection.
    const TypeAnnotation* currentReturnAnnotation_ = nullptr;
    bool inUnsafe_ = false;
    bool inImportedScope_ = false;
    // When true, the enclosing function/method is annotated with @[auto_wrap]:
    // `return <e>` inside a function returning Result<T,E> (or Option<T>)
    // is implicitly rewritten to `return Ok(<e>)` (or `return Some(<e>)`)
    // when <e>'s type matches the Ok payload (or Some payload).
    bool currentFnAutoWrap_ = false;
    std::set<std::string> externClassNames_;
    std::set<std::string> iteratingCollections_;
    TemplateResolver templateResolver_;
    
    // Cache of template instantiations: function name + arg types -> instance
    std::map<std::string, std::unique_ptr<TemplateInstance>> templateInstances_;

    // Instantiated declarations waiting to be appended to the translation unit
    std::vector<DeclPtr> pendingInstantiations_;

    // Cached return types for instantiated templates: mangled name -> return type
    std::map<std::string, VyxTypePtr> instantiatedReturnTypes_;

    // Tracks which class template specializations have been emitted as concrete Decls
    std::set<std::string> instantiatedClassTemplates_;

    // Active generic type parameters (pushed/popped when entering/leaving generic scopes)
    std::set<std::string> activeGenericParams_;

    // Active const generic parameters (pushed/popped around analyzeFunctionDecl of
    // instantiated generic functions so that `return N;` resolves without "undefined symbol").
    std::set<std::string> activeConstGenericParams_;

    // Enclosing class type when analysing method bodies. `self` resolves to
    // this type inside member methods. Empty when at module/function scope.
    // For generic classes, the type is a `Class`-kind VyxType whose .name
    // is the class base name (generic params are still in scope via
    // activeGenericParams_). Stack semantics: save/restore around each method.
    VyxTypePtr currentClassType_;

    // Module namespace support
    std::string currentModule_;
    std::map<std::string, std::string> importAliases_;
    // Imported symbols: unqualified name → qualified name
    std::map<std::string, std::string> importedSymbols_;
    // Function → source module short name (e.g., "args_count" → "env")
    std::map<std::string, std::string> functionSourceModule_;
    // Functions with name collisions across modules (must use qualified name)
    std::set<std::string> ambiguousFunctions_;
    // Function aliases: alias → original name (e.g., "my_count" → "args_count")
    std::map<std::string, std::string> functionAliases_;
    // Module-level visibility: qualified name → is public
    std::map<std::string, bool> symbolVisibility_;

    // ── ODR diagnostics (one-definition-rule) ──
    // Track every user-declared type so duplicate class/struct/interface/errordef
    // within the same namespace produces a C++-style redefinition diagnostic.
    struct TypeDeclRecord {
        DeclKind kind;
        SourceLocation location;
        bool isImported = false;
        // P2-C2: set when this record corresponds to a full template
        // specialization (parser set isFullSpecialization = true). Used by
        // checkTypeRedefinition to emit a specialization-specific diagnostic
        // when the same mangled name (e.g. "Vec<i32>") is defined twice.
        bool isFullSpecialization = false;
        // P2b: set when this record is a partial template specialization.
        bool isPartialSpecialization = false;
    };
    // key = "<module>::<name>" (module empty → "<global>")
    std::map<std::string, TypeDeclRecord> typeDeclRegistry_;

    // ── P2b Partial template specialization support ──────────────────────
    // selectPartialSpec: given a base name (e.g. "Pair") and a concrete
    // argument list (length == primary arity), enumerate all partial-spec
    // decls registered for that base name, pattern-match each against the
    // concrete args, rank by specificity, and return:
    //   - the winning Decl* + a substitution map  (unambiguous single winner)
    //   - nullptr                                  (no match → use primary)
    // Emits error[P2E-001] if two equally-specific specs match (ambiguous).
    struct PartialSpecMatch {
        const Decl* decl = nullptr;
        std::map<std::string, VyxTypePtr> bindings;
    };
    PartialSpecMatch selectPartialSpec(
        const std::string& baseName,
        const std::vector<VyxTypePtr>& concreteArgs,
        SourceLocation useSite);

    // P2-generics C5: associated-type bindings recorded from `impl Trait for Target`
    // blocks. Three complementary maps:
    //   * implAssocByTarget_  : key = "TargetName::AssocName"  -> concrete VyxType
    //     Used when the target is fully concrete and we can look up `T::Item`
    //     after binding T to that target type. The TargetName is the impl
    //     decl's mangled `name` (e.g. "U8Like" or "Iter<i32>").
    //   * implAssocByTrait_   : key = "TraitName::AssocName"   -> default VyxType
    //     Mirrors the trait's `type Item = Default;` defaults (when the
    //     interface declares a default), used as a fallback.
    //   * implAssocByTemplate_: key = "TemplateName" -> ("AssocName" -> TypeAnnotation*)
    //     Used for generic class templates like `class Iter<T> : Iterable`.
    //     The value annotation is NOT resolved — it carries the original
    //     `type Item = T;` annotation (where T is a class generic param).
    //     At mono time, look up the base name (stripped of `<args>`), find
    //     the annotation, then substitute using the instantiation's TypeEnv.
    //     Populated by SemaDecl when a generic class binds an assoc type.
    // Populated during Pass 1 (Sema.cpp) when interface and impl decls are
    // registered, consumed by resolveType (Dependent branch) and the env-based
    // substituteType when a `Generic` placeholder named "T::Item" surfaces.
    std::map<std::string, VyxTypePtr> implAssocByTarget_;
    std::map<std::string, VyxTypePtr> implAssocByTrait_;
    // implAssocByTemplate_["Iter"]["Item"] = TypeAnnotation* pointing at the
    // raw annotation inside the class's AssociatedType node.
    // We store a raw pointer (lifetime == TranslationUnit lifetime) to avoid
    // deep-cloning the annotation at registration time.  Do NOT store unique_ptr.
    std::map<std::string,
        std::map<std::string, const TypeAnnotation*>> implAssocByTemplate_;

    // Trait-name -> set of associated-type names declared on that trait.
    // Built from InterfaceDecl.associatedTypes during Pass 1; consumed by
    // template instantiation to enumerate the assoc types that need binding
    // when a generic param T : Trait gets a concrete substitution.
    std::map<std::string, std::vector<std::string>> traitAssocTypes_;

    // Trait composition (2026-04-23): traitName -> direct supertrait list.
    //   `trait Num : Add + Sub + Mul + Div + Zero + One {}`
    // yields traitSupertraits_["Num"] = { "Add","Sub","Mul","Mul","Div","Zero","One" }
    // (first entry is parentName, rest are interfaces). verifyMethodConstraints
    // walks this map transitively so a where-clause `T: Num` implies
    // `T: Add`, `T: Sub`, ... without the caller listing each supertrait.
    std::map<std::string, std::vector<std::string>> traitSupertraits_;
    // Expand a trait name into the set of all transitive supertraits
    // (including the trait itself). Detects cycles and emits a diagnostic
    // once per cycle root, then breaks to avoid infinite recursion. The
    // returned vector is deduped, in discovery order.
    std::vector<std::string> expandTraitWithSupertraits(
        const std::string& traitName,
        SourceLocation diagLoc);

    // ── Primitive-type trait impls (2026-04-23) ─────────────────────────────
    // `impl Hashable for i32 { fn hash() -> i64 { ... } }` registers its
    // methods here.  Keyed by (primitiveTypeName, methodName) so that
    //   * analyzeMemberAccess can return the method's return type when the
    //     receiver is a primitive value, and
    //   * verifyMethodConstraints can treat the primitive as nominally
    //     satisfying the trait when a where-clause binds T to it.
    //
    // Why a side-map (not type->methods): the primitive VyxType instance
    // (`types::makeInt(32, true)`) is shared across every i32 use-site and
    // registered with the symbol table — mutating its methods list at Pass 1
    // would leak the impl's methods to unrelated VyxType comparisons
    // (isEqual / printing / structural checks).  Keeping them side-car
    // preserves `i32`'s "no-methods" shape everywhere the compiler already
    // assumes it.
    //
    // Keys use the same string form that CodeGen's mangleVyxTypeForCodegen
    // produces ("i32", "i64", "string", "bool", "char", "f32", "f64",
    // "u8".."u64", "rawptr", "isize", "usize") so function mangling
    // (ClassName + "." + MethodName) lines up between Sema analysis and
    // CodeGen emission.
    std::map<std::string,
        std::map<std::string, const MethodDecl*>> primitiveMethodImpls_;
    // (primitiveTypeName, traitName) → satisfied.  Populated whenever an
    // `impl <Trait> for <Primitive>` block is registered, enabling
    // verifyMethodConstraints to accept `where T: Trait` bindings to that
    // primitive without scanning methods.
    std::set<std::string> primitiveTraitSatisfied_;
    static std::string primTraitKey(const std::string& prim, const std::string& trait) {
        return prim + "\x1f" + trait;
    }
    // True iff `name` is one of the primitive type names produced by
    // mangleVyxTypeForCodegen / stored in the symbol table by Sema bootstrap.
    static bool isPrimitiveTypeName(const std::string& name);
    // Convert a primitive type name to its VyxType instance.  Returns nullptr
    // for non-primitive names.  Used when analyzing impl-block method bodies
    // that target a primitive so `self` gets a meaningful type.
    VyxTypePtr lookupPrimitiveType(const std::string& name);

    // Helper used by the instantiation entry-points (functions, classes,
    // structs) to populate `subst` with `T::Item -> ConcreteAssoc` entries
    // for every active where-clause `T : Trait` constraint, so subsequent
    // substitution of dependent types (`T::Item`) finds a binding.
    void bindDependentTypesForSubst(
        const std::map<std::string, std::vector<std::string>>& constraints,
        std::map<std::string, VyxTypePtr>& substitutions);

    // Track every non-generic function declaration so duplicate (name + parameter
    // type sequence) pairs in the same namespace trigger an ODR redefinition
    // error. Generic functions are keyed by name + arity only.
    struct FunctionDeclRecord {
        std::vector<VyxTypePtr> paramTypes;
        SourceLocation location;
        bool isImported = false;
        bool isGeneric = false;
        size_t genericArity = 0;
    };
    // key = "<module>::<name>" → list of overloads recorded so far
    std::map<std::string, std::vector<FunctionDeclRecord>> functionDeclRegistry_;

    // ── Free function overloading (Phase 9) ───────────────────────────────
    // Multiple `fn name(...)` decls with different parameter type signatures
    // coexist as overloads; the call site selects the best match. Each overload
    // gets a unique mangled symbol (`name(t0,t1,...)`); the unmangled name maps
    // to its overload set so analyzeCall can enumerate candidates and pick.
    //
    //   * For a 1-element overload set we keep the symbol and decl named after
    //     the unmangled form (no observable behaviour change).
    //   * The moment a second non-generic overload appears, BOTH decls get
    //     their `name` mutated to the mangled form (`name$$i32_i32`,
    //     `name$$f64_f64`) and re-registered in `symbols_` under the new key.
    //   * `overloadGroups_[unmangled]` is populated for every overload set
    //     (size 1 or more) so analyzeCall has a fast path. Generic overloads
    //     are NOT mangled (they go through Mono's existing instantiation path)
    //     but they ARE recorded so analyzeCall can decide between non-generic
    //     and generic candidates per the design rule (non-generic wins).
    struct OverloadEntry {
        Decl* decl = nullptr;             // the FunctionDecl
        std::vector<VyxTypePtr> paramTypes; // resolved at registration
        std::string mangledName;          // == decl->name; empty for the
                                          // sole-overload case where decl
                                          // keeps its unmangled name
        bool isGeneric = false;
    };
    std::map<std::string, std::vector<OverloadEntry>> overloadGroups_;
    // Mangle a parameter-type list into a stable suffix appended after `$$`.
    // Example: ["i32","i32"] -> "i32_i32"; ["Vec<i64>"] -> "Vec<i64>".
    static std::string mangleOverloadSuffix(const std::vector<VyxTypePtr>& paramTypes);
    // Combined name + suffix. Example: ("add", [i32,i32]) -> "add$$i32_i32".
    static std::string mangleOverloadName(const std::string& name,
                                          const std::vector<VyxTypePtr>& paramTypes);

    // Module attribution for every top-level decl of the current translation unit.
    // Populated at the start of analyze() by scanning __module markers (which
    // the import resolver inserts before each imported file's declarations).
    // Consumed by ODR checks across Pass 1, Pass 2, and analyzeFunctionDecl.
    std::unordered_map<const Decl*, std::string> declModule_;
    std::string moduleOfDecl(const Decl& d) const;

    // Format <ns>::<name>, using "<global>" when ns is empty.
    static std::string qualifiedKey(const std::string& ns, const std::string& name);
    static const char* declKindName(DeclKind kind);
    static std::string typeSignatureString(const std::vector<VyxTypePtr>& paramTypes);
    static bool paramTypesEqual(const std::vector<VyxTypePtr>& a, const std::vector<VyxTypePtr>& b);
    // Returns true and emits a diagnostic if a redefinition conflict exists.
    bool checkTypeRedefinition(const std::string& ns, const Decl& decl);
    // Returns true and emits a diagnostic if a redefinition conflict exists.
    // Caller supplies already-resolved parameter types.
    bool checkFunctionRedefinition(const std::string& ns,
                                   const Decl& decl,
                                   const std::vector<VyxTypePtr>& paramTypes,
                                   bool isGeneric,
                                   size_t genericArity);

    /// Synthetic location for diagnostics without a source span (shared across Sema TUs).
    static SourceLocation semaInternalSourceLocation();
    static SourceLocation pickAssignWarnLoc(const SourceLocation& at);
};

} // namespace vyx
