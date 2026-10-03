#pragma once
//
// PLAN_SEMA_ROOT_FIX — S3 (Solver switchover, primitive-only).
//
// FactBase is the **sole authoritative store** for `(prim, trait)`
// satisfaction. The legacy `primitiveTraitSatisfied_` set has been
// reduced to a dead member kept around for one release as a fallback
// shim (plan §7 risk A). The on-demand re-registration patch
// (`ensurePrimitiveImplRegistered`) is gone.
//
// Lifecycle:
//
//   * Sema::analyze runs Phase A discovery once at the top of the
//     translation unit, calling `recordPrimImplFact` for every
//     `impl <Trait> for <Primitive>` block. The base is then `freeze()`d.
//
//   * Pass 1 (decl registration) and Pass 2 (method bodies) only ever
//     read via `TraitSolver::solve(...)`. There is no second writer.
//
//   * `verifyMethodConstraints` (SemaGeneric.cpp) routes every prim
//     trait-bound check through the solver. The fallback walks
//     (`primitiveMethodImpls_` for method-name lookup) are independent
//     and stay on a side-map until later phases migrate them.
//
// Design contract:
//
//   * FactBase writes are idempotent — Phase A discovery may visit the
//     same impl block multiple times (e.g. via pendingInstantiations_
//     that surface a duplicate impl); set semantics absorb the dupes.
//
//   * FactBase stores both the trait's short name (`Hashable`) and any
//     fully-qualified form (`std.hash.Hashable`) automatically. This
//     keeps callers out of the alias-handling business; the canonical-id
//     phase (S2) of the plan called for this folding.
//
//   * `freeze()` after Phase A is enforced: any subsequent
//     `registerPrimImpl` call would silently no-op only if the entry
//     was already present, and a brand-new fact would surface as a
//     hard ICE in `Sema::recordPrimImplFact` (asserted there). At S3
//     no production caller writes after freeze.
//
#include "Canonicalizer.h"

#include <cstdint>
#include <set>
#include <string>

namespace vyx::canon {

// Tri-state result of a solver query. Variant set fixed by §2 of the
// design doc: Sat / Unsat / NeedsBlanket. NeedsBlanket is reserved for
// S3 (nominal blanket impl resolution); S2 returns Sat or Unsat only.
enum class SolveResult : std::uint8_t {
    Sat,
    Unsat,
    NeedsBlanket,
};

// FactBase — discovery store for trait/impl/prim/blanket facts.
//
// At S2 it stores primitive `(PrimId, TraitId)` pairs only. Future
// phases extend the storage with nominal facts and blanket-impl
// indices; the public write/read shape stays compatible.
class FactBase {
public:
    bool isFrozen() const { return frozen_; }

    // Once frozen, S3 will enforce "no more writes". S2 leaves the flag
    // informational so the lazy `ensurePrimitiveImplRegistered` path can
    // continue to feed late discoveries without tripping an assert.
    void freeze() { frozen_ = true; }

    // Reset for a new translation unit.
    void clear() {
        frozen_ = false;
        primTraitFacts_.clear();
        postFreezeWrites_ = 0;
    }

    // Register a primitive impl fact. Both the trait id passed in and
    // its dotted-segment last component are inserted, so callers don't
    // have to second-guess short/long-name aliasing.
    //
    // Idempotent: redundant inserts collapse via set semantics. The
    // `postFreezeWrites_` counter is bumped every time a write attempt
    // arrives after `freeze()` introduces a NEW key — duplicates of
    // already-known facts are silently absorbed (they are by design
    // a contract-preserving no-op). A non-zero counter means somebody
    // discovered a fact too late and broke the "Phase A is the only
    // writer" invariant; `Sema::recordPrimImplFact` is wired to surface
    // it.
    void registerPrimImpl(const PrimId& prim, const TraitId& trait) {
        if (!prim.isValid() || !trait.isValid()) return;
        auto insertOne = [&](const std::string& key) {
            auto inserted = primTraitFacts_.insert(key).second;
            if (inserted && frozen_) ++postFreezeWrites_;
        };
        insertOne(makeKey(prim.name, trait.mangled));
        auto dot = trait.mangled.rfind('.');
        if (dot != std::string::npos) {
            insertOne(makeKey(prim.name, trait.mangled.substr(dot + 1)));
        }
    }

    std::size_t postFreezeWrites() const { return postFreezeWrites_; }

    bool primSatisfies(const PrimId& prim, const TraitId& trait) const {
        if (!prim.isValid() || !trait.isValid()) return false;
        return primTraitFacts_.count(makeKey(prim.name, trait.mangled)) > 0;
    }

    std::size_t primFactCount() const { return primTraitFacts_.size(); }

private:
    static std::string makeKey(const std::string& prim,
                               const std::string& trait) {
        return prim + "\x1f" + trait;
    }

    bool frozen_ = false;
    std::set<std::string> primTraitFacts_;
    std::size_t postFreezeWrites_ = 0;
};

// TraitSolver — read-only query service over a FactBase.
//
// S2: handles primitive `(PrimId, TraitId)` queries via the FactBase.
// Returns:
//   - Sat when the fact is present.
//   - Unsat for everything else (including non-primitive types).
// NeedsBlanket is reserved for S3 (nominal + blanket resolution).
class TraitSolver {
public:
    explicit TraitSolver(const FactBase& base) : base_(&base) {}

    SolveResult solve(const TraitId& trait,
                      const PrimId& prim,
                      std::uint64_t envHash = 0) const {
        (void)envHash;
        if (!trait.isValid() || !prim.isValid()) return SolveResult::Unsat;
        return base_->primSatisfies(prim, trait) ? SolveResult::Sat
                                                 : SolveResult::Unsat;
    }

    // Compatibility wrapper kept from S1 — accepts a TypeId (mangled
    // string). At S2 only primitive mangled forms produce Sat; any
    // nominal type yields Unsat (handled on legacy path).
    SolveResult solve(const TraitId& trait,
                      const TypeId& type,
                      std::uint64_t envHash = 0) const {
        (void)envHash;
        if (!trait.isValid() || !type.isValid()) return SolveResult::Unsat;
        return base_->primSatisfies(PrimId{type.mangled}, trait)
                   ? SolveResult::Sat
                   : SolveResult::Unsat;
    }

private:
    const FactBase* base_;
};

} // namespace vyx::canon
