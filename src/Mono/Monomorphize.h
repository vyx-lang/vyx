#pragma once
#include "../Parser/AST.h"
#include "../Sema/Type.h"             // canonical TypeEnv + substituteType + mangleGeneric
#include "../Sema/TemplateResolver.h"
#include "../Sema/MonoScheduler.h"
#include "../Common/Determinism.h"
#include <vector>
#include <set>
#include <map>
#include <unordered_set>
#include <unordered_map>
#include <string>
#include <functional>

namespace vyx {

class DiagnosticsEngine;
class LangItemRegistry;

// `TypeEnv` lives in Sema/Type.h as the canonical generic substitution map.
// Free helper `toGenericSubstitution` defined in the .cpp anonymous namespace
// converts it to the older `GenericSubstitution` shape consumed by the
// existing `TemplateResolver::substituteType(const TypeAnnotation&, ...)`
// API. New code should prefer `::vyx::substituteType(VyxTypePtr, TypeEnv)`
// from Type.h directly.

// ============================================================
//  InstantiationRequest
// ============================================================

// A single instantiation request: which generic decl (template), with what
// concrete type environment. Resolved lazily by the Monomorphize driver.
struct InstantiationRequest {
    const Decl* templateDecl = nullptr;
    TypeEnv env;               // Maps template's genericParams → concrete VyxType
    std::string mangledName;   // Canonical identifier: templateDecl->name<args...>
    // For methods: classTemplate holds the enclosing class decl; env is
    // populated with BOTH class-level and method-level generic bindings.
    const Decl* classTemplate = nullptr;

    // Error-recovery fields (P2D error harvest).
    // `failed` is set when processOne encounters an unrecoverable error for
    // this instantiation (template not found, cycle, etc.). The worklist loop
    // does NOT abort; it continues to the next request so all errors surface
    // in a single build.
    bool failed = false;
    std::string errorMsg;      // Human-readable failure reason (for dedup key).

    // Number of distinct call sites that triggered this (mangled, error) pair.
    // Starts at 1 on first failure; incremented by dedup on subsequent sites.
    uint32_t callSiteCount = 0;
};

// ============================================================
//  Monomorphize
// ============================================================

// Monomorphization driver: walks the translation unit, collecting generic
// instantiation roots from call sites / field types / return types, then
// runs a fixed-point worklist until no new requests remain. Emits fully
// concretized Decls appended to the output unit. Handles cycle detection
// (same mangledName already seen → skip) and mutual recursion.
//
// Single-pass, deterministic. No lazy semantics — callers expect all
// monomorphs to exist by the time this returns.
class Monomorphize {
public:
    explicit Monomorphize(DiagnosticsEngine& diag) : diag_(diag) {}

    // Entry point. `unit` is the parsed + sema'd translation unit. On return,
    // `unit.declarations` contains all original decls PLUS concretized
    // instantiations for every reachable generic use. `roots` are the
    // user-requested entry points (typically main + exported functions).
    void run(TranslationUnit& unit, const std::vector<const Decl*>& roots);

    // PLAN_SEMA_ROOT_FIX — S5: share Sema's scheduler ledger so Mono's
    // accepted enqueue requests and ready items are reconciled in one place.
    void setScheduler(canon::MonoScheduler* scheduler) { scheduler_ = scheduler; }

    // Explicit request — caller supplies a template and the env to bind its
    // generic params to. Returns the mangled name of the resulting monomorph,
    // which will have been emitted by the time `run()` completes.
    std::string request(const Decl* templateDecl, TypeEnv env);

    // Lookup a decl by name in the current translation unit. Public so that
    // free helper functions in Monomorphize.cpp can use it.
    const Decl* findDecl(const std::string& name) const;

    // Enqueue a pre-built InstantiationRequest (deduplicated by mangledName).
    // Public so the static helper requestMethodInstantiation can call it
    // directly when it needs to set fields (e.g. classTemplate) that the
    // higher-level request() API doesn't expose.
    void enqueue(InstantiationRequest req);

    // Expose the current translation unit pointer so static helpers can
    // enumerate all declarations (e.g. for partial-spec candidate search).
    const TranslationUnit* getUnit() const { return unit_; }

    // Error-recovery: maximum number of DISTINCT failures before Mono stops
    // enqueuing new analysis (default 100). Failures already in the worklist
    // when the limit is hit are still drained.
    void setMaxErrors(uint32_t n) { maxErrors_ = n; }

    // After run() completes, returns the count of distinct failures recorded.
    uint32_t distinctFailureCount() const {
        return static_cast<uint32_t>(failedRequests_.size());
    }

    // Total call-site hits across all deduplicated failures (i.e. the raw
    // number of times we saw a failing (mangled, errorMsg) key).
    uint32_t totalFailureSiteCount() const {
        uint32_t total = 0;
        for (auto& [key, req] : failedRequests_) total += req.callSiteCount;
        return total;
    }

    // Synthesise and store a FunctionDecl wrapper around a MethodDecl —
    // public so static helper requestMethodInstantiation can call it.
    const Decl* makeSyntheticMethodDecl(const std::string& className,
                                         const MethodDecl& method,
                                         bool forceStatic = false);

    // ── Strict error model (Front 3) ─────────────────────────────────────
    // Record a genuine Mono failure at a concrete MONO-XXX diagnostic code
    // from a static helper. The helpers don't have access to an
    // InstantiationRequest, so we synthesise a bare one keyed by `mangled`
    // for dedup purposes. Emits the diagnostic on first sight; subsequent
    // calls with the same (mangled, code) pair are silenced to avoid
    // flooding the stream.
    void reportFailureFromHelper(const char* code,
                                 SourceLocation loc,
                                 const std::string& mangled,
                                 const std::string& msg);
    // Mark a mangle as a legitimate deferral (unbound generic, will be
    // re-scanned when bindings are available). If the mangle is never
    // retried by the end of the worklist drain, a MONO-005 diagnostic is
    // emitted per entry.
    void markDeferred(const std::string& mangled, SourceLocation loc);

    // Build the mangled name for a method-level generic instantiation.
    // Form: "<classMono>.<methodName><<U1>,<U2>,...>"
    // E.g. classMono="Iterator<i64>", methodName="map", methodArgTypes=[u32]
    //    → "Iterator<i64>.map<u32>"
    // Public and static so requestMethodInstantiation can call it without a
    // Monomorphize instance (it only needs the canonical mangler).
    static std::string buildMangledMethodName(const std::string& classMono,
                                              const std::string& methodName,
                                              const std::vector<VyxTypePtr>& methodArgTypes);

private:
    DiagnosticsEngine& diag_;
    canon::MonoScheduler* scheduler_ = nullptr;

    // Worklist + seen-set. Ordered for determinism.
    std::vector<InstantiationRequest> worklist_;
    std::set<std::string> seen_;
    // Cycle guard: names currently on the stack during instantiation,
    // detected to produce clear diagnostics instead of infinite recursion.
    std::set<std::string> active_;

    // Front 1: O(1) worklist-dedup set. Every `enqueue` that is about to
    // push onto worklist_ first checks (and inserts into) this set in
    // log-amortized time; `processOne` erases the entry when it pops.
    // Replaces the prior linear scan of worklist_, which was O(N) per
    // enqueue and made the driver quadratic under self-hosting load
    // (hundreds of thousands of requests).
    //
    // Uses std::set (NOT std::unordered_set) to stay consistent with the
    // in-flight NameMap=std::map determinism policy.
    std::set<std::string> pendingMangles_;

    // Front 1: global instantiation budget. `kMaxTotalInstantiations` is a
    // hard upper bound on the *distinct* requests the driver will process
    // in a single run; the counter advances whenever `enqueue` succeeds
    // in adding a new mangle. When the ceiling is exceeded Mono emits
    // `MONO-004`, drains the worklist without further processing, and
    // returns cleanly (callers still see a failing build via the error
    // count, but the compiler never hangs).
    //
    // Budget rationale: std + industrial corpus peaks around 30k
    // instantiations on a clean build; 100k is ~3x headroom so a
    // legitimate self-hosting run never trips the cap, but a runaway
    // generic cascade (e.g. `Pair<Pair<T,T>, Pair<T,T>>` recursion) is
    // caught in <1s instead of OOM-ing the host.
    static constexpr uint32_t kMaxTotalInstantiations = 100000;
    uint32_t totalInstantiations_ = 0;
    bool budgetExhausted_ = false;

    // Error-recovery: dedup table keyed by (mangledName + "\x1f" + errorMsg).
    // Value carries the first-seen InstantiationRequest augmented with
    // callSiteCount so the post-run summary can report collapsed sites.
    std::map<std::string, InstantiationRequest> failedRequests_;
    // Maximum distinct failures before we stop enqueueing new analysis.
    uint32_t maxErrors_ = 100;

    // Strict error model (Front 3): mangles that were deferred by a silent
    // early-return in a request helper because some binding was not yet
    // available. After the worklist drains, any entry still in this set
    // that didn't also land in `seen_` surfaces as MONO-005.
    std::unordered_map<std::string, SourceLocation> deferredRequests_;

    // Pointer to the translation unit being processed (valid during run()).
    TranslationUnit* unit_ = nullptr;

    // Template resolver instance used for type substitution.
    // Declared mutable so const helper methods (cloneExpr, substituteAnnotation)
    // can call the non-const TemplateResolver::substituteType.
    mutable TemplateResolver resolver_;

    // While `instantiate()` is rewriting a single template into a concrete
    // monomorph, this points at the active substitution env. `cloneTypeAnnotation`
    // consults it on every visit so that *every* TypeAnnotation reachable
    // through the body (var decls, casts, turbofish call type-args, lambda
    // params, match patterns, struct/array init annotations, …) gets
    // env-substituted — not just the explicitly visited param/return/field
    // slots. Null outside of an active instantiation, in which case
    // `cloneTypeAnnotation` falls back to the pure-structural deep clone.
    mutable const TypeEnv* currentEnv_ = nullptr;

    void processOne(const InstantiationRequest& req, TranslationUnit& unit);

    // Dedup helper for error-recovery: inserts into failedRequests_; returns
    // true when this is a new (first-seen) failure, false when deduplicated.
    bool recordFailure(const std::string& mangledName,
                       const std::string& errorMsg,
                       const InstantiationRequest& req);

    // Scans a decl body for generic references requiring instantiation
    // (call exprs with turbofish / type args, generic-typed fields / params /
    // returns).
    void scanForRequests(const Decl& decl, const TypeEnv& env);

    // Deep-clones a template decl and applies env substitution to every
    // TypeAnnotation it contains. Produces a fresh Decl with genericParams
    // empty and name set to mangledName.
    //
    // DESIGN CHOICE: Uses the existing Sema helpers (duplicateDecl-style
    // manual clone) by leveraging TemplateResolver::substituteParam /
    // substituteType. Rather than adding Decl::clone() (which would touch a
    // shared header), all cloning is done inside this compilation unit via
    // helper free functions (cloneTypeAnnotation, cloneStmt, cloneExpr).
    std::unique_ptr<Decl> instantiate(const Decl& templateDecl,
                                       const TypeEnv& env,
                                       const std::string& mangledName);

    // ── AST deep-clone helpers ──────────────────────────────────────────────
    // These are self-contained within Mono and do not modify any shared header.
    TypePtr cloneTypeAnnotation(const TypeAnnotation* src) const;
    ExprPtr cloneExpr(const Expr* src) const;
    StmtPtr cloneStmt(const Stmt* src) const;

    // ── Type-annotation substitution ───────────────────────────────────────
    // Rewrites a TypeAnnotation in-place (by replacement): resolves the type
    // via TemplateResolver::substituteType using the provided env, then
    // converts the resulting VyxType back to an AST TypeAnnotation.
    TypePtr substituteAnnotation(const TypeAnnotation* src, const TypeEnv& env) const;

    // Build a mangled name from a template decl and a type env, ordering args
    // by the template's genericParams vector for determinism.
    std::string buildMangledName(const Decl& templateDecl, const TypeEnv& env) const;

    // Lifetime storage for synthetic FunctionDecls created by makeSyntheticMethodDecl.
    // These are needed because InstantiationRequest::templateDecl is a raw pointer
    // and must point to a stable allocation that outlives the worklist.
    std::vector<std::unique_ptr<Decl>> syntheticDecls_;

public:
    // P2b Wave 3+: associated-type template registry, populated by Sema before
    // run() is called. Maps `templateBaseName → (assocName → raw TypeAnnotation*)`.
    // The raw pointer lifetime is tied to the TranslationUnit (owned by Sema).
    // Mono uses this to resolve `T::Item` when T is bound to a generic class
    // instantiation (e.g. `Iter<i32>`).
    void setAssocTemplateMap(
        std::map<std::string,
            std::map<std::string, const TypeAnnotation*>> map) {
        implAssocByTemplate_ = std::move(map);
    }

    // R5 step 3: hand Mono the stdlib lang-item registry so its VyxType →
    // TypeAnnotation reverse path (`vyxTypeToAnnotation`) can emit the
    // registered slot name for Option/Result rather than the hard-coded
    // strings "Option"/"Result". Non-owning pointer; caller (Compiler /
    // main / ProjectManager) ensures the registry outlives this run().
    // When null (tooling / tests without Sema), the reverse path falls back
    // to a `<unknown>` sentinel for Option/Result so mis-resolution is loud.
    void setLangItems(const LangItemRegistry* reg) { langItems_ = reg; }
    const LangItemRegistry* getLangItems() const { return langItems_; }

private:
    // implAssocByTemplate_["Iter"]["Item"] = &TypeAnnotation(T) from class body.
    std::map<std::string,
        std::map<std::string, const TypeAnnotation*>> implAssocByTemplate_;

    // R5 step 3: non-owning pointer to the Sema-populated lang-item registry.
    const LangItemRegistry* langItems_ = nullptr;
};

} // namespace vyx
