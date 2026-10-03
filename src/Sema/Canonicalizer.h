#pragma once
//
// PLAN_SEMA_ROOT_FIX — S1 (Canonical name) scaffolding.
//
// Introduces three opaque id wrappers (PrimId / TypeId / TraitId) and a
// Canonicalizer class. At S1 every method is identity: the wrapper holds
// the original mangled string verbatim and downstream consumers must
// already treat it as an opaque value usable for equality / hashing only.
//
// Behavioural goal at S1:
//   - Pure scaffolding. No existing string lookup is rerouted, so
//     -HostOnly gate counts and emitted IR remain bit-identical.
//   - Existing call points may dial-tone the canonicalizer (`(void)
//     canonicalizeTrait(name)`) to demonstrate the wiring without
//     branching on the result.
//
// S2/S3 plan (not implemented here):
//   - Replace the identity body with an interning table indexed by
//     fully-qualified name; emit a stable `uint32_t` token alongside the
//     mangled string for cache keys.
//   - Plug into the FactBase discovery so that
//     `solver.solve(canonicalizeTrait(traitName), ...)` can take over from
//     `primitiveTraitSatisfied_` lookups.
//
#include <cstdint>
#include <string>

namespace vyx::canon {

// PrimId — canonical identifier for a primitive type (i32, u8, string,
// bool, char, f32/f64, isize/usize, rawptr).
//
// At S1 the inner string is the same form produced by
// `mangleVyxTypeForCodegen` so the existing Sema lookup tables
// (`primitiveTraitSatisfied_`, `primitiveMethodImpls_`) can interoperate
// without translation. Callers MUST treat the value as opaque except for
// equality / hashing.
struct PrimId {
    std::string name;
    bool isValid() const { return !name.empty(); }
    friend bool operator==(const PrimId& a, const PrimId& b) {
        return a.name == b.name;
    }
};

// TypeId — canonical identifier for any type (primitive or nominal).
// Holds the type's mangled string. A future phase will swap the inner
// representation for an interned token, but the public type stays stable.
struct TypeId {
    std::string mangled;
    bool isValid() const { return !mangled.empty(); }
    friend bool operator==(const TypeId& a, const TypeId& b) {
        return a.mangled == b.mangled;
    }
};

// TraitId — canonical identifier for a trait/interface/concept. At S1
// the inner string is whatever the caller passed in (short or fully
// qualified); the S2 implementation will fold both forms into a single
// canonical token.
struct TraitId {
    std::string mangled;
    bool isValid() const { return !mangled.empty(); }
    friend bool operator==(const TraitId& a, const TraitId& b) {
        return a.mangled == b.mangled;
    }
};

// Canonicalizer — name-resolution scaffold.
//
// Entry points are deliberately small and mirror the three id types.
// At S1 every call is identity. The class stores no state yet; later
// phases will add an interning map and an alias-table reset hook.
class Canonicalizer {
public:
    PrimId canonicalizePrim(const std::string& name) const {
        return PrimId{name};
    }
    TypeId canonicalizeType(const std::string& mangled) const {
        return TypeId{mangled};
    }
    TraitId canonicalizeTrait(const std::string& name) const {
        return TraitId{name};
    }

    // Hash hook reserved for the S2 interning cache. At S1 it is a
    // stable string hash so cache keys can already be assembled even
    // though no consumer reads them yet.
    static std::uint64_t hashTrait(const TraitId& id) {
        std::uint64_t h = 1469598103934665603ull;
        for (char c : id.mangled) {
            h ^= static_cast<std::uint8_t>(c);
            h *= 1099511628211ull;
        }
        return h;
    }

    // Reserved no-op. Future phases will flush the alias map between
    // translation units.
    void reset() {}
};

} // namespace vyx::canon
