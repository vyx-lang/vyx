#pragma once
// Determinism.h — name-keyed ordered containers.
//
// The compiler MUST emit byte-identical output for the same input regardless
// of host toolchain, libstdc++/libc++ hash seed, or ASLR. Any container that
// is iterated in a way that affects codegen (e.g. emission order of
// functions_, structTypes_, stringConstants_, mangled-name lookup lists) MUST
// be ordered. We centralise this here so a future switch (btree, flat_map,
// etc.) is one edit.
//
// Rule of thumb:
//   * Key is a name (string / mangled symbol / module path) and iteration
//     order is ever observed  ->  NameMap / NameSet.
//   * Key is a pointer or opaque integer and iteration order is never
//     observed (pure address-keyed memoisation)  ->  std::unordered_map is
//     fine.
//
// Do not add operator[] / find semantics here; std::map already has them and
// is a drop-in replacement for unordered_map for all name-keyed uses in this
// codebase.

#include <map>
#include <set>
#include <string>

namespace vyx {

template <class K, class V>
using NameMap = std::map<K, V>;

template <class K>
using NameSet = std::set<K>;

} // namespace vyx
