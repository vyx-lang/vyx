#pragma once

#include "../Parser/AST.h"
#include "../Common/Diagnostics.h"
#include <string>
#include <map>
#include <vector>

namespace vyx {

/// Registry of "language items" contributed by the standard library.
///
/// A *lang item* is a declaration that the compiler special-cases for
/// syntax-sugar lowering (e.g. `?` operator, `${}` interpolation, auto-wrap
/// `return` inside a `Result`-returning function) yet whose body, layout,
/// and methods live entirely in `.vyx` source.  The standard library
/// opts a class / errordef / interface into a slot by annotating it with
/// `@[lang_item("slot_name")]`.  Sema's first pass scans every unit and
/// populates this registry so downstream Sema / Mono / CodeGen paths can
/// resolve well-known slots without hard-coding the name `"Result"`,
/// `"Option"`, `"String"`, etc. throughout the code base.
///
/// The registry deliberately stores a *pointer* to the host Decl (owned
/// by the TranslationUnit).  Callers must not hold these pointers across
/// any mutation of the unit that could relocate its declaration vector.
/// In practice all lookups are done after the Sema pre-pass has run and
/// before Mono rewrites the unit, which mirrors how other Sema registries
/// (associated-type maps, partial-spec tables, …) scope their lifetimes.
///
/// Slot name convention (kebab-cased):
///   - "option"   → `@[lang_item("option")] enum Option<T> { Some(T), None }`
///   - "result"   → `@[lang_item("result")] enum Result<T, E> { Ok(T), Err(E) }`
///   - "string"   → `@[lang_item("string")] class String { ... }`
///   - "box"      → `@[lang_item("box")]    class Box<T>  { ... }`
///   - "ref"      → `@[lang_item("ref")]    class Ref<T>  { ... }`
///   - "scope"    → `@[lang_item("scope")]  class Scope<T>{ ... }`
///   - "vec"      → `@[lang_item("vec")]    class Vec<T>  { ... }`
///   - "dict"     → `@[lang_item("dict")]   class Dict<K,V>{ ... }`
///
/// Future slots (`try_ok`, `try_err`, `iterator_next`, …) may be added as
/// the compiler grows new sugar that requires stdlib cooperation.
///
/// The registry is intentionally ignorant of the slot semantics — it just
/// maps a name string to a Decl pointer.  Sema/Mono/CodeGen interpret the
/// slot according to their lowering needs.
class LangItemRegistry {
public:
    /// Record the decl under the given slot name.  If a decl was already
    /// registered under `slot`, emits a duplicate-registration diagnostic
    /// via `diag` and keeps the *first* registration (deterministic winner).
    /// Ignored silently when either argument is empty / null.
    void registerItem(const std::string& slot,
                      const Decl* decl,
                      DiagnosticsEngine& diag,
                      SourceLocation loc);

    /// Look up the decl registered for `slot`.  Returns nullptr on miss.
    /// Callers should always tolerate a nullptr result and fall back to
    /// their legacy hard-coded behaviour so that compiling a unit that
    /// hasn't imported the relevant stdlib module still works.
    const Decl* find(const std::string& slot) const;

    /// Convenience: the slot name if `decl` is registered, empty otherwise.
    /// Linear in the number of slots; used sparingly (e.g. debug logging).
    std::string slotOf(const Decl* decl) const;

    /// Clear every registration.  Called at the start of a fresh Sema run.
    void clear();

    /// Iterate over all registered slots in insertion order.  Useful for
    /// diagnostic dumps ("std.core missing lang_item(\"result\")").
    const std::vector<std::pair<std::string, const Decl*>>& entries() const {
        return entries_;
    }

private:
    // Primary name→decl table.  Flat unordered_map is fine — the total
    // number of slots is under 32 in practice.
    std::map<std::string, const Decl*> byName_;
    // Insertion-ordered mirror for deterministic iteration in diagnostics.
    std::vector<std::pair<std::string, const Decl*>> entries_;
};

} // namespace vyx
