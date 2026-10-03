#pragma once
#include <string>
#include <vector>
#include <memory>
#include <map>

namespace vyx {

enum class VyxTypeKind {
    Void,
    Bool,
    Integer,
    Float,
    Char,
    RawPtr,
    Pointer,
    Reference,
    Array,
    Tuple,
    Function,
    Struct,
    Class,
    Interface,
    ErrorType,
    Range,          // Range {start, end, step}
    // R5 phase 4d: VyxTypeKind::RefPtr / ScopePtr / BoxPtr are gone. The
    // canonical forms are `Class{name="Ref<T>" / "Scope<T>" / "Box<T>",
    // paramTypes=[T]}`, matching the pattern Option / Result / Vec / Dict
    // / Stack / Queue / Set use.  std/ref.vyx carries
    // `@[lang_item("ref")] class Ref<T>` and `@[lang_item("box")] class
    // Box<T>`; Scope<T> is reached by direct name resolution (no stdlib
    // class declares `@[lang_item("scope")]` yet — same policy as
    // UnorderedMap / UnorderedSet).
    //
    // Backend aliasing: CodeGen's Class-kind branch in toLLVMType checks
    // the name prefix — `Ref<` / `Scope<` route to the shared
    // `__RefCounted { ptr, refcount }` struct, and `Box<` lowers to an
    // opaque pointer.  mangleVyxTypeForCodegen collapses these Class
    // names to their backend aliases (__RefCounted / rawptr) so
    // classVarTypes_ keys line up with findClassMethod / structTypes_.
    //
    // Use `isRefLike` / `isScopeLike` / `isBoxLike` / `isSmartPtrLike` +
    // `smartPtrInner` / `refInner` / `scopeInner` / `boxInner` helpers
    // for recognition and inner-type access.
    // R5 phase 4: Vec<T> is now a stdlib `@[lang_item("vec")] class Vec<T>`,
    // reaching the compiler as `Class{name="Vec<T>", paramTypes=[T]}`. The
    // former dedicated VyxTypeKind::DynArray has been retired — use the
    // `isVecLike` / `vecElement` helpers for recognition.
    // R5 phase 4b: VyxTypeKind::Dict / UnorderedMap are likewise gone —
    // Dict<K,V> is stdlib `@[lang_item("dict")] class Dict<K,V>`, and both
    // map-like containers arrive as `Class{name="Dict<..,..>" / "UnorderedMap
    // <..,..>", paramTypes=[K,V]}`. Use `isDictLike` / `isUnorderedMapLike`
    // / `isMapLike` + `mapKey` / `mapValue` helpers for recognition. (No
    // stdlib class declares `@[lang_item("unordered_map")]` yet — user code
    // spelling `UnorderedMap<K,V>` falls through to the same Class-form and
    // Sema's name resolution applies, same as any other generic class.)
    // R5 phase 4c: VyxTypeKind::Stack / Queue / Set / UnorderedSet are gone
    // too. Canonical forms are Class{name="Stack<T>" / "Queue<T>" / "Set<T>"
    // / "UnorderedSet<T>", paramTypes=[T]}. Set has a stdlib
    // `@[lang_item("set")] class Set<T>`; the other three are plain
    // user-generic classes reached via `unit_->declarations` by base name
    // (same policy as UnorderedMap). Use `isStackLike` / `isQueueLike` /
    // `isSetLike` / `isUnorderedSetLike` + `containerElement` helpers.
    // R5 phase 4 batch 4: VyxTypeKind::Delegate / Event are gone. The
    // canonical forms are `Class{name="Delegate<fn(...)->R>" /
    // "Event<fn(...)>", paramTypes=[fnType]}`. No stdlib class declares
    // `@[lang_item("delegate")]` / `@[lang_item("event")]`; source spelling
    // `Delegate<...>` / `Event<...>` falls through to the same Class-form
    // and name-based recognition path (same policy as UnorderedMap /
    // UnorderedSet / Scope). Use `isDelegateLike` / `isEventLike` /
    // `isCallableWrapperLike` + `callableSig` helpers for recognition and
    // underlying-signature access.
    Union,          // T | U discriminated union
    Generic,        // unresolved generic param
    Pack,           // variadic generic pack: <...Ts> or (...args); tupleTypes holds members
    Unknown,
};

struct VyxType {
    // ── Core identity ──
    VyxTypeKind kind = VyxTypeKind::Unknown;
    std::string name;

    // ── Numeric: Integer / Float ──
    int bitWidth = 0;       // 8, 16, 32, 64
    bool isSigned = true;
    bool isSizeType = false; // isize/usize

    // ── Indirection: Pointer / Reference ──
    std::shared_ptr<VyxType> pointeeType;
    bool isMutable = false;

    // ── Element: Array ──
    std::shared_ptr<VyxType> elementType;
    int arraySize = 0;      // fixed-size Array only

    // ── Compound: Tuple / Union ──
    std::vector<std::shared_ptr<VyxType>> tupleTypes;

    // ── Callable: Function + Class-form generics (paramTypes[0]=K for Dict,
    //    paramTypes[0]/[1]=K/V for Dict/UnorderedMap, paramTypes=[T…] for Vec/
    //    Option/Result/etc.) ──
    std::vector<std::shared_ptr<VyxType>> paramTypes;
    std::shared_ptr<VyxType> returnType;       // also: Delegate / Event

    // ── Composite: Struct / Class / Interface ──
    struct Field {
        std::string name;
        std::shared_ptr<VyxType> type;
        bool isPublic = false;
    };
    std::vector<Field> fields;

    struct MethodSig {
        std::string name;
        std::vector<std::shared_ptr<VyxType>> paramTypes;
        std::shared_ptr<VyxType> returnType;
        bool isPublic = true;
    };
    std::vector<MethodSig> methods;

    std::shared_ptr<VyxType> parentType;
    std::vector<std::shared_ptr<VyxType>> interfaceTypes;

    // ── P4-B move checker: heap-owning flag ──
    // True when this type transfers ownership on bind/assign rather than
    // copy-by-value. Set by Sema during class analysis when the class name
    // is registered as one of a curated list of heap-owning lang_items
    // (vec, dict, set, string, box, ref, scope, stack, queue, list, btree,
    // hashmap, hashset, deque) OR when the class exposes a `fn drop()`
    // method in its method table. Primitives, POD structs, and enums stay
    // false — they keep copy-by-default semantics. See SemaDecl.cpp's
    // analyzeClassDecl for the computation and SemaStmt / SemaExprOps for
    // the consumers (move-marking in var-decl / assignment / call-arg /
    // return).
    bool isHeapOwning = false;

    // ── ADT / Error ──
    std::vector<std::string> errorVariants;
    // Result<T,E> used to carry its ok/err via dedicated slots; after R5 it
    // is Class-kind with the two slots stored in paramTypes[0..1] like any
    // other user generic. The old okType/errType fields are gone.

    bool isEqual(const VyxType& other) const {
        if (kind != other.kind) return false;
        switch (kind) {
            case VyxTypeKind::Integer:
                return bitWidth == other.bitWidth && isSigned == other.isSigned && isSizeType == other.isSizeType;
            case VyxTypeKind::Float:
                return bitWidth == other.bitWidth;
            case VyxTypeKind::Struct:
            case VyxTypeKind::Class:
            case VyxTypeKind::Interface:
            case VyxTypeKind::ErrorType:
            case VyxTypeKind::Generic:
                return name == other.name;
            case VyxTypeKind::Pointer:
            case VyxTypeKind::Reference:
                return pointeeType && other.pointeeType && pointeeType->isEqual(*other.pointeeType)
                    && isMutable == other.isMutable;
            case VyxTypeKind::Array:
                return elementType && other.elementType && elementType->isEqual(*other.elementType)
                    && arraySize == other.arraySize;
            case VyxTypeKind::Tuple:
            case VyxTypeKind::Union: {
                if (tupleTypes.size() != other.tupleTypes.size()) return false;
                for (size_t i = 0; i < tupleTypes.size(); ++i) {
                    if (!tupleTypes[i] || !other.tupleTypes[i] || !tupleTypes[i]->isEqual(*other.tupleTypes[i]))
                        return false;
                }
                return true;
            }
            case VyxTypeKind::Function: {
                if (paramTypes.size() != other.paramTypes.size()) return false;
                for (size_t i = 0; i < paramTypes.size(); ++i) {
                    if (!paramTypes[i] || !other.paramTypes[i] || !paramTypes[i]->isEqual(*other.paramTypes[i]))
                        return false;
                }
                if (returnType && other.returnType) return returnType->isEqual(*other.returnType);
                return !returnType && !other.returnType;
            }
            case VyxTypeKind::Range:
                return true;
            case VyxTypeKind::Unknown:
                return false;
            case VyxTypeKind::Pack: {
                if (tupleTypes.size() != other.tupleTypes.size()) return false;
                for (size_t i = 0; i < tupleTypes.size(); ++i) {
                    if (!tupleTypes[i] || !other.tupleTypes[i] ||
                        !tupleTypes[i]->isEqual(*other.tupleTypes[i])) return false;
                }
                return true;
            }
            default:
                return true;
        }
    }

    std::string toString() const {
        switch (kind) {
            case VyxTypeKind::Void:    return "void";
            case VyxTypeKind::Bool:    return "bool";
            case VyxTypeKind::Char:    return "char";
            case VyxTypeKind::RawPtr:  return "rawptr";
            case VyxTypeKind::Unknown: return "<unknown>";
            case VyxTypeKind::Integer: {
                if (isSizeType) return isSigned ? "isize" : "usize";
                return (isSigned ? "i" : "u") + std::to_string(bitWidth);
            }
            case VyxTypeKind::Float:
                return "f" + std::to_string(bitWidth);
            case VyxTypeKind::Pointer:
                return "*" + (pointeeType ? pointeeType->toString() : "?");
            case VyxTypeKind::Reference:
                return (isMutable ? "&mut " : "&") + (pointeeType ? pointeeType->toString() : "?");
            case VyxTypeKind::Array:
                return "[" + (elementType ? elementType->toString() : "?") + "; " + std::to_string(arraySize) + "]";
            case VyxTypeKind::Struct:
            case VyxTypeKind::Class:
            case VyxTypeKind::Interface:
            case VyxTypeKind::ErrorType:
            case VyxTypeKind::Generic:
                return name;
            case VyxTypeKind::Range:
                return "Range";
            case VyxTypeKind::Function: {
                std::string r = "fn(";
                for (size_t i = 0; i < paramTypes.size(); ++i) {
                    if (i > 0) r += ", ";
                    r += paramTypes[i]->toString();
                }
                r += ") -> " + (returnType ? returnType->toString() : "void");
                return r;
            }
            case VyxTypeKind::Union: {
                std::string r;
                for (size_t i = 0; i < tupleTypes.size(); ++i) {
                    if (i > 0) r += " | ";
                    r += tupleTypes[i]->toString();
                }
                return r;
            }
            case VyxTypeKind::Tuple: {
                std::string r = "(";
                for (size_t i = 0; i < tupleTypes.size(); ++i) {
                    if (i > 0) r += ", ";
                    r += tupleTypes[i]->toString();
                }
                return r + ")";
            }
            case VyxTypeKind::Pack: {
                std::string r = "...(";
                for (size_t i = 0; i < tupleTypes.size(); ++i) {
                    if (i > 0) r += ", ";
                    r += tupleTypes[i] ? tupleTypes[i]->toString() : "?";
                }
                return r + ")";
            }
        }
        return "<unknown>";
    }

    bool isNumeric() const { return kind == VyxTypeKind::Integer || kind == VyxTypeKind::Float; }
    bool isIntegral() const { return kind == VyxTypeKind::Integer; }
    bool isFloatingPoint() const { return kind == VyxTypeKind::Float; }
    bool isBoolType() const { return kind == VyxTypeKind::Bool; }
    bool isPack() const { return kind == VyxTypeKind::Pack; }
    bool isGeneric() const { return kind == VyxTypeKind::Generic; }

    // ── Canonical mangling ──
    // Single source of truth for symbol-name encoding across Sema/Mono/CodeGen.
    // Structural, idempotent, stable. Never produce nested `Outer<Inner<...>>`
    // collapsed into a broken substring — structural descent guarantees shape.
    // Packs expand inline: Pack<i32, string> → "i32,string" (no wrapper).
    std::string mangle() const {
        switch (kind) {
            case VyxTypeKind::Void:    return "void";
            case VyxTypeKind::Bool:    return "bool";
            case VyxTypeKind::Char:    return "char";
            case VyxTypeKind::RawPtr:  return "rawptr";
            case VyxTypeKind::Unknown: return "?";
            case VyxTypeKind::Integer:
                if (isSizeType) return isSigned ? "isize" : "usize";
                return (isSigned ? "i" : "u") + std::to_string(bitWidth);
            case VyxTypeKind::Float:
                return "f" + std::to_string(bitWidth);
            case VyxTypeKind::Pointer:
                return "*" + (pointeeType ? pointeeType->mangle() : "?");
            case VyxTypeKind::Reference:
                return (isMutable ? "&mut " : "&") + (pointeeType ? pointeeType->mangle() : "?");
            case VyxTypeKind::Array:
                return "[" + (elementType ? elementType->mangle() : "?") + ";" + std::to_string(arraySize) + "]";
            case VyxTypeKind::Generic:
                return name;
            case VyxTypeKind::Struct:
            case VyxTypeKind::Class:
            case VyxTypeKind::Interface:
            case VyxTypeKind::ErrorType:
                return name;
            case VyxTypeKind::Range:
                return "Range";
            case VyxTypeKind::Function: {
                std::string r = "fn(";
                for (size_t i = 0; i < paramTypes.size(); ++i) {
                    if (i > 0) r += ",";
                    r += paramTypes[i] ? paramTypes[i]->mangle() : "?";
                }
                r += ")->" + (returnType ? returnType->mangle() : "void");
                return r;
            }
            case VyxTypeKind::Tuple: {
                std::string r = "(";
                for (size_t i = 0; i < tupleTypes.size(); ++i) {
                    if (i > 0) r += ",";
                    r += tupleTypes[i] ? tupleTypes[i]->mangle() : "?";
                }
                return r + ")";
            }
            case VyxTypeKind::Union: {
                std::string r;
                for (size_t i = 0; i < tupleTypes.size(); ++i) {
                    if (i > 0) r += "|";
                    r += tupleTypes[i] ? tupleTypes[i]->mangle() : "?";
                }
                return r;
            }
            case VyxTypeKind::Pack: {
                // Packs mangle as their members joined by commas, no wrapper.
                // Callers splice into their own context (generic args, tuple).
                std::string r;
                for (size_t i = 0; i < tupleTypes.size(); ++i) {
                    if (i > 0) r += ",";
                    r += tupleTypes[i] ? tupleTypes[i]->mangle() : "?";
                }
                return r;
            }
        }
        return "?";
    }
};

using VyxTypePtr = std::shared_ptr<VyxType>;

// ─────────── R5 lang-item helpers for Option / Result ────────────────────
// After the R5 refactor, Option<T> and Result<T,E> live in stdlib as user
// generic enums and arrive here as `Class{name="Option<T>" or "Result<T,E>",
// paramTypes=[...]}`. These predicates/accessors replace the former
// `kind == Optional` / `kind == Result` checks (those enum values are gone).
inline bool isOptionLike(const VyxType& t) {
    // After R5 Option<T> is Class-kind via makeOptional; generic resolution
    // via resolveType's ErrorDef branch produces ErrorType-kind with
    // paramTypes populated. Accept both so downstream code (Mono
    // method-generic dispatch, resultOk, etc.) sees a uniform view.
    if (t.kind != VyxTypeKind::Class && t.kind != VyxTypeKind::ErrorType) return false;
    return t.name.size() > 7 && t.name.compare(0, 7, "Option<") == 0;
}
inline bool isOptionLike(const VyxType* t) { return t && isOptionLike(*t); }

inline bool isResultLike(const VyxType& t) {
    if (t.kind != VyxTypeKind::Class && t.kind != VyxTypeKind::ErrorType) return false;
    return t.name.size() > 7 && t.name.compare(0, 7, "Result<") == 0;
}
inline bool isResultLike(const VyxType* t) { return t && isResultLike(*t); }

inline VyxTypePtr optionInner(const VyxType& t) {
    if (!isOptionLike(t)) return nullptr;
    if (t.paramTypes.empty()) return nullptr;
    return t.paramTypes[0];
}

inline VyxTypePtr resultOk(const VyxType& t) {
    if (!isResultLike(t)) return nullptr;
    if (t.paramTypes.empty()) return nullptr;
    return t.paramTypes[0];
}

inline VyxTypePtr resultErr(const VyxType& t) {
    if (!isResultLike(t)) return nullptr;
    if (t.paramTypes.size() < 2) return nullptr;
    return t.paramTypes[1];
}

inline bool isAsyncLike(const VyxType& t) {
    if (t.kind != VyxTypeKind::Class && t.kind != VyxTypeKind::ErrorType) return false;
    return (t.name.size() > 8 && t.name.compare(0, 8, "Promise<") == 0) ||
           (t.name.size() > 5 && t.name.compare(0, 5, "Task<") == 0);
}
inline bool isAsyncLike(const VyxType* t) { return t && isAsyncLike(*t); }

inline VyxTypePtr asyncInner(const VyxType& t) {
    if (!isAsyncLike(t)) return nullptr;
    if (t.paramTypes.empty()) return nullptr;
    return t.paramTypes[0];
}
inline VyxTypePtr asyncInner(const VyxType* t) {
    return t ? asyncInner(*t) : nullptr;
}

// R5 phase 3: String is a stdlib `@[lang_item("string")] class String`.
// Canonical VyxType form is `Class{name="string"}` (lowercase — matches
// Mono's `t->mangle()` output and user-written `Vec<string>` source).
inline bool isStringType(const VyxType& t) {
    return t.kind == VyxTypeKind::Class && t.name == "string";
}
inline bool isStringType(const VyxType* t) { return t && isStringType(*t); }

// R5 phase 4: Vec<T> is a stdlib `@[lang_item("vec")] class Vec<T>`.
// Canonical form is `Class{name="Vec<T>", paramTypes=[T]}` (same shape
// as Option / Result / every other user generic). The dedicated
// VyxTypeKind::DynArray is gone; these helpers centralize recognition.
inline bool isVecLike(const VyxType& t) {
    if (t.kind != VyxTypeKind::Class) return false;
    return t.name.size() > 4 && t.name.compare(0, 4, "Vec<") == 0;
}
inline bool isVecLike(const VyxType* t) { return t && isVecLike(*t); }

inline VyxTypePtr vecElement(const VyxType& t) {
    if (!isVecLike(t)) return nullptr;
    if (t.paramTypes.empty()) return nullptr;
    return t.paramTypes[0];
}
inline VyxTypePtr vecElement(const VyxType* t) {
    return t ? vecElement(*t) : nullptr;
}

// R5 phase 4b: Dict<K,V> / UnorderedMap<K,V> are stdlib-backed Class-kind
// types now (Dict via `@[lang_item("dict")] class Dict<K,V>` in
// std/dict.vyx; UnorderedMap has no stdlib class but reaches here as
// Class{name="UnorderedMap<..,..>"} when spelled literally). These helpers
// centralize the `name.starts_with(...)` recognition so call sites don't
// each re-implement the decode.
inline bool isDictLike(const VyxType& t) {
    if (t.kind != VyxTypeKind::Class) return false;
    return t.name.size() > 5 && t.name.compare(0, 5, "Dict<") == 0;
}
inline bool isDictLike(const VyxType* t) { return t && isDictLike(*t); }

inline bool isUnorderedMapLike(const VyxType& t) {
    if (t.kind != VyxTypeKind::Class) return false;
    return t.name.size() > 13 && t.name.compare(0, 13, "UnorderedMap<") == 0;
}
inline bool isUnorderedMapLike(const VyxType* t) { return t && isUnorderedMapLike(*t); }

// Unified map-like predicate: matches both Dict and UnorderedMap in either
// legacy or Class-form. Callers that want to distinguish should use the
// individual predicates.
inline bool isMapLike(const VyxType& t) {
    return isDictLike(t) || isUnorderedMapLike(t);
}
inline bool isMapLike(const VyxType* t) { return t && isMapLike(*t); }

inline VyxTypePtr mapKey(const VyxType& t) {
    if (!isMapLike(t)) return nullptr;
    if (t.paramTypes.size() < 1) return nullptr;
    return t.paramTypes[0];
}
inline VyxTypePtr mapKey(const VyxType* t) { return t ? mapKey(*t) : nullptr; }

inline VyxTypePtr mapValue(const VyxType& t) {
    if (!isMapLike(t)) return nullptr;
    if (t.paramTypes.size() < 2) return nullptr;
    return t.paramTypes[1];
}
inline VyxTypePtr mapValue(const VyxType* t) { return t ? mapValue(*t) : nullptr; }

// R5 phase 4c: Stack / Queue / Set / UnorderedSet — element containers sharing
// a single-T structure. Canonical form is `Class{name="X<T>", paramTypes=[T]}`
// just like Vec / Dict / Option / Result. The former dedicated
// VyxTypeKind::Stack / Queue / Set / UnorderedSet values are gone; these
// helpers centralize name-based recognition so call sites don't each
// re-implement the decode.
//
// `Set` vs `UnorderedSet`: the Class-kind name prefix distinguishes them
// (`Set<` starts `Set<...>` but NOT `UnorderedSet<...>`, we explicitly reject
// an `UnorderedSet<` prefix so the ordered/unordered semantics stay distinct).
inline bool isStackLike(const VyxType& t) {
    if (t.kind != VyxTypeKind::Class) return false;
    return t.name.size() > 6 && t.name.compare(0, 6, "Stack<") == 0;
}
inline bool isStackLike(const VyxType* t) { return t && isStackLike(*t); }

inline bool isQueueLike(const VyxType& t) {
    if (t.kind != VyxTypeKind::Class) return false;
    return t.name.size() > 6 && t.name.compare(0, 6, "Queue<") == 0;
}
inline bool isQueueLike(const VyxType* t) { return t && isQueueLike(*t); }

inline bool isUnorderedSetLike(const VyxType& t) {
    if (t.kind != VyxTypeKind::Class) return false;
    return t.name.size() > 13 && t.name.compare(0, 13, "UnorderedSet<") == 0;
}
inline bool isUnorderedSetLike(const VyxType* t) { return t && isUnorderedSetLike(*t); }

inline bool isSetLike(const VyxType& t) {
    if (t.kind != VyxTypeKind::Class) return false;
    // Must match "Set<..." but NOT "UnorderedSet<..." — the latter is a
    // distinct unordered container with different semantics.
    if (isUnorderedSetLike(t)) return false;
    return t.name.size() > 4 && t.name.compare(0, 4, "Set<") == 0;
}
inline bool isSetLike(const VyxType* t) { return t && isSetLike(*t); }

// Unified helper: true if the type is any of the four element-containers
// (Stack / Queue / Set / UnorderedSet) in canonical Class-kind form.
inline bool isElementContainerLike(const VyxType& t) {
    return isStackLike(t) || isQueueLike(t) || isSetLike(t) || isUnorderedSetLike(t);
}
inline bool isElementContainerLike(const VyxType* t) { return t && isElementContainerLike(*t); }

// Accessor: element T of any of the four containers. Reads `paramTypes[0]`
// from the Class-kind form.
inline VyxTypePtr containerElement(const VyxType& t) {
    if (!isElementContainerLike(t)) return nullptr;
    if (t.paramTypes.empty()) return nullptr;
    return t.paramTypes[0];
}
inline VyxTypePtr containerElement(const VyxType* t) { return t ? containerElement(*t) : nullptr; }

// Per-container element accessors — thin wrappers around `containerElement`
// that additionally assert the container kind, useful where the caller already
// narrowed the type via the corresponding predicate.
inline VyxTypePtr stackElement(const VyxType& t) {
    if (!isStackLike(t)) return nullptr;
    return containerElement(t);
}
inline VyxTypePtr queueElement(const VyxType& t) {
    if (!isQueueLike(t)) return nullptr;
    return containerElement(t);
}
inline VyxTypePtr setElement(const VyxType& t) {
    if (!isSetLike(t)) return nullptr;
    return containerElement(t);
}
inline VyxTypePtr unorderedSetElement(const VyxType& t) {
    if (!isUnorderedSetLike(t)) return nullptr;
    return containerElement(t);
}

// ─── R5 phase 4d: smart-pointer (Ref<T> / Scope<T> / Box<T>) helpers ──────
// Canonical form is `Class{name="Ref<T>" / "Scope<T>" / "Box<T>",
// paramTypes=[T]}`, matching the Option / Result / Vec pattern. The former
// dedicated VyxTypeKind::RefPtr / ScopePtr / BoxPtr values are retired; the
// name prefix is authoritative for recognition.
//
// CRITICAL backend note: `Ref<T>` and `Scope<T>` share the LLVM layout
// `__RefCounted { ptr, refcount }`, while `Box<T>` lowers to an opaque
// pointer. CodeGen's Class-kind branch special-cases these name prefixes
// to route to the correct LLVM layout; the name prefix must stay exactly
// "Ref<" / "Scope<" / "Box<" — downstream code keys on it.
inline bool isRefLike(const VyxType& t) {
    if (t.kind != VyxTypeKind::Class) return false;
    return t.name.size() > 4 && t.name.compare(0, 4, "Ref<") == 0;
}
inline bool isRefLike(const VyxType* t) { return t && isRefLike(*t); }

inline bool isScopeLike(const VyxType& t) {
    if (t.kind != VyxTypeKind::Class) return false;
    return t.name.size() > 6 && t.name.compare(0, 6, "Scope<") == 0;
}
inline bool isScopeLike(const VyxType* t) { return t && isScopeLike(*t); }

inline bool isBoxLike(const VyxType& t) {
    if (t.kind != VyxTypeKind::Class) return false;
    return t.name.size() > 4 && t.name.compare(0, 4, "Box<") == 0;
}
inline bool isBoxLike(const VyxType* t) { return t && isBoxLike(*t); }

// Unified predicate: true for any of the three smart-pointer shapes.
inline bool isSmartPtrLike(const VyxType& t) {
    return isRefLike(t) || isScopeLike(t) || isBoxLike(t);
}
inline bool isSmartPtrLike(const VyxType* t) { return t && isSmartPtrLike(*t); }

// Accessor: the inner T of a smart-pointer type. Reads paramTypes[0] from
// the canonical Class-kind form.
inline VyxTypePtr smartPtrInner(const VyxType& t) {
    if (!isSmartPtrLike(t)) return nullptr;
    if (t.paramTypes.empty()) return nullptr;
    return t.paramTypes[0];
}
inline VyxTypePtr smartPtrInner(const VyxType* t) { return t ? smartPtrInner(*t) : nullptr; }

// Per-pointer inner accessors — thin wrappers asserting the pointer kind.
inline VyxTypePtr refInner(const VyxType& t) {
    if (!isRefLike(t)) return nullptr;
    return smartPtrInner(t);
}
inline VyxTypePtr scopeInner(const VyxType& t) {
    if (!isScopeLike(t)) return nullptr;
    return smartPtrInner(t);
}
inline VyxTypePtr boxInner(const VyxType& t) {
    if (!isBoxLike(t)) return nullptr;
    return smartPtrInner(t);
}

// ─── R5 phase 4 batch 4: Delegate<fn(...)> / Event<fn(...)> helpers ───────
// Callable wrappers pivot to Class-kind Class{name="Delegate<fn(...)->R>" /
// "Event<fn(...)>", paramTypes=[fnType]}. No stdlib class declares
// `@[lang_item("delegate")]` or `@[lang_item("event")]` (std/event.vyx
// defines helper functions but no lang_item class) — literal `Delegate<>`
// / `Event<>` source resolves to this Class form and the compiler-
// synthesized backend layout (same policy as UnorderedMap / UnorderedSet /
// Scope). The name prefix ("Delegate<" / "Event<") is authoritative for
// recognition so CodeGen / Mono can route by string tag.
inline bool isDelegateLike(const VyxType& t) {
    if (t.kind != VyxTypeKind::Class) return false;
    return t.name.size() > 9 && t.name.compare(0, 9, "Delegate<") == 0;
}
inline bool isDelegateLike(const VyxType* t) { return t && isDelegateLike(*t); }

inline bool isEventLike(const VyxType& t) {
    if (t.kind != VyxTypeKind::Class) return false;
    return t.name.size() > 6 && t.name.compare(0, 6, "Event<") == 0;
}
inline bool isEventLike(const VyxType* t) { return t && isEventLike(*t); }

inline bool isCallableWrapperLike(const VyxType& t) {
    return isDelegateLike(t) || isEventLike(t);
}
inline bool isCallableWrapperLike(const VyxType* t) { return t && isCallableWrapperLike(*t); }

// ─── P4-B move checker: heap-owning classification ───────────────────────
// Returns true if `t` is a class/struct that transfers ownership on
// bind/assign (move-by-default) rather than shallow-copying. Recognises:
//   1. the explicit `isHeapOwning` flag set by Sema's class-analysis pass
//      (via lang_item attribute or presence of `fn drop()`);
//   2. name-prefix matching for canonical stdlib containers / smart-ptrs,
//      so generic monomorphs (`Vec<i32>`, `Dict<string,i32>`, `Box<T>`)
//      automatically qualify even when the monomorph was synthesized
//      without copying the flag from its template.
// Primitives, unions, tuples, arrays, enums, and POD structs all return
// false — they keep C-style copy-by-value semantics.
inline bool isHeapOwningType(const VyxType& t) {
    if (t.isHeapOwning) return true;
    if (t.kind != VyxTypeKind::Class && t.kind != VyxTypeKind::Struct) return false;
    // Name-prefix fallback for canonical container / smart-ptr / string
    // shapes. Every entry here corresponds to either a stdlib lang_item
    // slot or a compiler-synthesized Class{name="X<...>"} form the Sema
    // pipeline produces.
    if (isStringType(t)) return true;
    if (isVecLike(t)) return true;
    if (isDictLike(t) || isUnorderedMapLike(t)) return true;
    if (isSetLike(t) || isUnorderedSetLike(t)) return true;
    if (isStackLike(t) || isQueueLike(t)) return true;
    if (isSmartPtrLike(t)) return true; // Ref<T>, Scope<T>, Box<T>
    return false;
}
inline bool isHeapOwningType(const VyxType* t) { return t && isHeapOwningType(*t); }

// Heap-owning values that cannot safely behave as implicit shared copies.
// `string`, `Ref<T>`, and `Scope<T>` keep their existing copy/borrow-style
// front-end treatment; containers, Box<T>, and user types that opt into
// Drop must transfer ownership even when the source binding is `let`.
inline bool isMoveOnlyHeapOwningType(const VyxType& t) {
    if (!isHeapOwningType(t)) return false;
    if (isStringType(t)) return false;
    if (isRefLike(t) || isScopeLike(t)) return false;
    return true;
}
inline bool isMoveOnlyHeapOwningType(const VyxType* t) {
    return t && isMoveOnlyHeapOwningType(*t);
}

// Accessor: the underlying Function-kind signature stored in paramTypes[0].
inline VyxTypePtr callableSig(const VyxType& t) {
    if (!isCallableWrapperLike(t)) return nullptr;
    if (t.paramTypes.empty()) return nullptr;
    return t.paramTypes[0];
}
inline VyxTypePtr callableSig(const VyxType* t) { return t ? callableSig(*t) : nullptr; }

namespace types {

inline VyxTypePtr makeVoid()   { auto t = std::make_shared<VyxType>(); t->kind = VyxTypeKind::Void; t->name = "void"; return t; }
inline VyxTypePtr makeBool()   { auto t = std::make_shared<VyxType>(); t->kind = VyxTypeKind::Bool; t->name = "bool"; return t; }
inline VyxTypePtr makeChar()   { auto t = std::make_shared<VyxType>(); t->kind = VyxTypeKind::Char; t->name = "char"; return t; }
// R5 phase 3: String is now a Class-kind type (lowercase canonical "string")
// matching the stdlib `@[lang_item("string")] class String` decl. The LLVM-
// side `__String` fat-pointer struct is aliased under `structTypes_["string"]`
// in CodeGen so the concrete layout stays backend-internal.
inline VyxTypePtr makeString() { auto t = std::make_shared<VyxType>(); t->kind = VyxTypeKind::Class; t->name = "string"; return t; }
inline VyxTypePtr makeRawPtr() { auto t = std::make_shared<VyxType>(); t->kind = VyxTypeKind::RawPtr; t->name = "rawptr"; return t; }

inline VyxTypePtr makeInt(int bits, bool isSigned = true) {
    auto t = std::make_shared<VyxType>();
    t->kind = VyxTypeKind::Integer;
    t->bitWidth = bits;
    t->isSigned = isSigned;
    t->name = (isSigned ? "i" : "u") + std::to_string(bits);
    return t;
}

inline VyxTypePtr makeISize() {
    auto t = makeInt(64, true);
    t->isSizeType = true;
    t->name = "isize";
    return t;
}

inline VyxTypePtr makeUSize() {
    auto t = makeInt(64, false);
    t->isSizeType = true;
    t->name = "usize";
    return t;
}

inline VyxTypePtr makeFloat(int bits) {
    auto t = std::make_shared<VyxType>();
    t->kind = VyxTypeKind::Float;
    t->bitWidth = bits;
    t->name = "f" + std::to_string(bits);
    return t;
}

inline VyxTypePtr makePointer(VyxTypePtr pointee) {
    auto t = std::make_shared<VyxType>();
    t->kind = VyxTypeKind::Pointer;
    t->pointeeType = std::move(pointee);
    return t;
}

inline VyxTypePtr makeReference(VyxTypePtr pointee, bool mutable_) {
    auto t = std::make_shared<VyxType>();
    t->kind = VyxTypeKind::Reference;
    t->pointeeType = std::move(pointee);
    t->isMutable = mutable_;
    return t;
}

inline VyxTypePtr makeArray(VyxTypePtr elem, int size) {
    auto t = std::make_shared<VyxType>();
    t->kind = VyxTypeKind::Array;
    t->elementType = std::move(elem);
    t->arraySize = size;
    return t;
}

inline VyxTypePtr makeTuple(std::vector<VyxTypePtr> elements) {
    auto t = std::make_shared<VyxType>();
    t->kind = VyxTypeKind::Tuple;
    t->tupleTypes = std::move(elements);
    return t;
}

// R5 phase 4c: UnorderedSet<T> pivots to Class-kind
// Class{name="UnorderedSet<T>", paramTypes=[T]}, matching the pattern
// Option / Result / Vec / Dict already use. No stdlib class declares
// `@[lang_item("unordered_set")]`; literal `UnorderedSet<T>` source falls
// through to the same Class-form and name-based recognition path (same
// policy as UnorderedMap). Legacy `kind == UnorderedSet` consumers continue
// to work until step 8c because `isUnorderedSetLike` / `containerElement`
// accept both forms.
inline VyxTypePtr makeUnorderedSet(VyxTypePtr elem) {
    auto t = std::make_shared<VyxType>();
    t->kind = VyxTypeKind::Class;
    std::string em = elem ? elem->mangle() : "?";
    t->name = "UnorderedSet<" + em + ">";
    if (elem) t->paramTypes.push_back(std::move(elem));
    return t;
}

// R5 phase 4b: UnorderedMap<K,V> pivots to Class-kind Class{name="UnorderedMap<K,V>"},
// matching the Option / Result / Vec pattern. There is no stdlib class with a
// `@[lang_item("unordered_map")]` annotation yet — UnorderedMap only arises
// when source spells the type literally, and no test covers that path today.
// Legacy `kind == UnorderedMap` consumers continue to work until step 7c
// because `isUnorderedMapLike` / `mapKey` / `mapValue` accept both forms.
inline VyxTypePtr makeUnorderedMap(VyxTypePtr key, VyxTypePtr val) {
    auto t = std::make_shared<VyxType>();
    t->kind = VyxTypeKind::Class;
    std::string km = key ? key->mangle() : "?";
    std::string vm = val ? val->mangle() : "?";
    t->name = "UnorderedMap<" + km + "," + vm + ">";
    if (key) t->paramTypes.push_back(std::move(key));
    if (val) t->paramTypes.push_back(std::move(val));
    return t;
}

// R5 phase 4b: Dict<K,V> is a stdlib `@[lang_item("dict")] class Dict<K,V>`.
// Factory now produces the mangled Class form directly
// (`Class{name="Dict<string,i32>", paramTypes=[string,i32]}`), matching the
// pattern Option / Result / Vec already use. Legacy `kind == Dict` consumers
// continue to work until step 7c because `isDictLike` / `mapKey` / `mapValue`
// accept both forms.
inline VyxTypePtr makeDict(VyxTypePtr keyType, VyxTypePtr valType) {
    auto t = std::make_shared<VyxType>();
    t->kind = VyxTypeKind::Class;
    std::string km = keyType ? keyType->mangle() : "?";
    std::string vm = valType ? valType->mangle() : "?";
    t->name = "Dict<" + km + "," + vm + ">";
    if (keyType) t->paramTypes.push_back(std::move(keyType));
    if (valType) t->paramTypes.push_back(std::move(valType));
    return t;
}

// R5 phase 4c: Stack<T> / Queue<T> pivot to Class-kind Class{name="Stack<T>"
// / "Queue<T>", paramTypes=[T]}. Stdlib carries stack + queue classes in
// std/stack.vyx but without a `@[lang_item(...)]` annotation yet — the
// compiler reaches the class body via the existing user-generic-class path
// (`unit_->declarations` scan by name), same policy as UnorderedMap /
// UnorderedSet. Legacy `kind == Stack` / `Queue` consumers continue to work
// until step 8c because `isStackLike` / `isQueueLike` / `containerElement`
// accept both forms.
inline VyxTypePtr makeStack(VyxTypePtr elem) {
    auto t = std::make_shared<VyxType>();
    t->kind = VyxTypeKind::Class;
    std::string em = elem ? elem->mangle() : "?";
    t->name = "Stack<" + em + ">";
    if (elem) t->paramTypes.push_back(std::move(elem));
    return t;
}

inline VyxTypePtr makeQueue(VyxTypePtr elem) {
    auto t = std::make_shared<VyxType>();
    t->kind = VyxTypeKind::Class;
    std::string em = elem ? elem->mangle() : "?";
    t->name = "Queue<" + em + ">";
    if (elem) t->paramTypes.push_back(std::move(elem));
    return t;
}

// R5 phase 4c: Set<T> pivots to Class-kind Class{name="Set<T>",
// paramTypes=[T]}, matching Option / Result / Vec / Dict. std/set.vyx
// carries `@[lang_item("set")] class Set<T>` so the stdlib class body is
// reachable through the lang-item registry path. Legacy `kind == Set`
// consumers continue to work until step 8c because `isSetLike` /
// `containerElement` accept both forms.
inline VyxTypePtr makeSet(VyxTypePtr elem) {
    auto t = std::make_shared<VyxType>();
    t->kind = VyxTypeKind::Class;
    std::string em = elem ? elem->mangle() : "?";
    t->name = "Set<" + em + ">";
    if (elem) t->paramTypes.push_back(std::move(elem));
    return t;
}

// R5 phase 4: Vec<T> is a stdlib `@[lang_item("vec")] class Vec<T>`.
// Factory now produces the mangled Class form directly
// (`Class{name="Vec<i32>", paramTypes=[i32]}`), matching the pattern
// Option / Result / String already use. Legacy `kind == DynArray` consumers
// continue to work until step 6c because `isVecLike` / `vecElement` accept
// both forms.
inline VyxTypePtr makeDynArray(VyxTypePtr elem) {
    auto t = std::make_shared<VyxType>();
    t->kind = VyxTypeKind::Class;
    std::string innerMangle = elem ? elem->mangle() : "?";
    t->name = "Vec<" + innerMangle + ">";
    if (elem) t->paramTypes.push_back(std::move(elem));
    return t;
}

inline VyxTypePtr makeRange() {
    auto t = std::make_shared<VyxType>();
    t->kind = VyxTypeKind::Range;
    t->name = "Range";
    return t;
}

// R5 final: Option<T> is a Class-kind user generic enum registered via
// `@[lang_item("option")]`. Factory produces the mangled Class form directly
// (`Class{name="Option<i32>", paramTypes=[i32]}`).
inline VyxTypePtr makeOptional(VyxTypePtr inner) {
    auto t = std::make_shared<VyxType>();
    t->kind = VyxTypeKind::Class;
    std::string innerMangle = inner ? inner->mangle() : "?";
    t->name = "Option<" + innerMangle + ">";
    if (inner) t->paramTypes.push_back(std::move(inner));
    return t;
}

// R5 phase 4d: Ref<T> is a stdlib `@[lang_item("ref")] class Ref<T>`.
// Factory produces Class{name="Ref<T>", paramTypes=[T]} directly,
// matching the pattern Option / Result / Vec / Dict / Stack / Queue / Set
// use. Backend aliasing: Ref<T> and Scope<T> share `__RefCounted
// { ptr, refcount }` — CodeGen's toLLVMType Class branch and
// mangleVyxTypeForCodegen both special-case the `Ref<` / `Scope<` name
// prefix to route to that shared layout.  The canonical Vyx name stays
// `Ref<T>` so Mono's container registry and classVarTypes_ lookups find
// the right entry.
inline VyxTypePtr makeRefPtr(VyxTypePtr inner) {
    auto t = std::make_shared<VyxType>();
    t->kind = VyxTypeKind::Class;
    std::string innerMangle = inner ? inner->mangle() : "?";
    t->name = "Ref<" + innerMangle + ">";
    if (inner) t->paramTypes.push_back(std::move(inner));
    return t;
}

// R5 phase 4d: Scope<T> pivots to Class-kind Class{name="Scope<T>",
// paramTypes=[T]}. No stdlib class declares `@[lang_item("scope")]` yet —
// user source spelling `Scope<T>` falls through to the same Class-form
// and literal-name resolution path (same policy as UnorderedMap).
// Shares the `__RefCounted` LLVM layout with Ref<T> (see toLLVMType).
inline VyxTypePtr makeScopePtr(VyxTypePtr inner) {
    auto t = std::make_shared<VyxType>();
    t->kind = VyxTypeKind::Class;
    std::string innerMangle = inner ? inner->mangle() : "?";
    t->name = "Scope<" + innerMangle + ">";
    if (inner) t->paramTypes.push_back(std::move(inner));
    return t;
}

// R5 final: Result<T,E> is a Class-kind user generic enum registered via
// `@[lang_item("result")]`. Factory produces the mangled Class form directly
// (`Class{name="Result<i32,string>", paramTypes=[i32, string]}`).
inline VyxTypePtr makeResult(VyxTypePtr okType, VyxTypePtr errType) {
    auto t = std::make_shared<VyxType>();
    t->kind = VyxTypeKind::Class;
    std::string okMangle = okType ? okType->mangle() : "?";
    std::string errMangle = errType ? errType->mangle() : "?";
    t->name = "Result<" + okMangle + "," + errMangle + ">";
    if (okType) t->paramTypes.push_back(std::move(okType));
    if (errType) t->paramTypes.push_back(std::move(errType));
    return t;
}

// R5 phase 4d: Box<T> pivots to Class-kind Class{name="Box<T>",
// paramTypes=[T]}. std/ref.vyx carries `@[lang_item("box")] class Box<T>`
// so the stdlib class body is reached through the lang-item registry.
// Unlike Ref<T> / Scope<T>, Box<T> lowers to an opaque pointer — CodeGen's
// toLLVMType Class branch special-cases the `Box<` name prefix.
inline VyxTypePtr makeBoxPtr(VyxTypePtr inner) {
    auto t = std::make_shared<VyxType>();
    t->kind = VyxTypeKind::Class;
    std::string innerMangle = inner ? inner->mangle() : "?";
    t->name = "Box<" + innerMangle + ">";
    if (inner) t->paramTypes.push_back(std::move(inner));
    return t;
}


// R5 phase 4 batch 4: Delegate<fn(...)->R> pivots to Class-kind
// Class{name="Delegate<fn(...)->R>", paramTypes=[fnType]}. No stdlib class
// declares `@[lang_item("delegate")]` — literal `Delegate<...>` source
// falls through to the same Class-form and name-based recognition path
// (same policy as UnorderedMap / UnorderedSet / Scope). The canonical
// mangled name includes the full function signature so distinct
// Delegate<fn(i32)->bool> / Delegate<fn(i32,string)->i64> instantiations
// remain distinguishable. CodeGen's Class-kind branch special-cases the
// `Delegate<` / `Event<` name prefix to lower to an opaque pointer /
// backend-provided struct layout.
inline VyxTypePtr makeDelegate(VyxTypePtr fnType) {
    auto t = std::make_shared<VyxType>();
    t->kind = VyxTypeKind::Class;
    std::string sigMangle = fnType ? fnType->mangle() : "?";
    t->name = "Delegate<" + sigMangle + ">";
    if (fnType) t->paramTypes.push_back(std::move(fnType));
    return t;
}

// R5 phase 4 batch 4: Event<fn(...)> pivots to Class-kind
// Class{name="Event<fn(...)>", paramTypes=[fnType]}. std/event.vyx defines
// helper free-functions (event_new/add/remove/fire) but no lang_item class;
// source spelling `Event<...>` resolves to this Class form and reaches
// CodeGen's name-prefix routing for the shared multicast backend layout.
inline VyxTypePtr makeEvent(VyxTypePtr fnType) {
    auto t = std::make_shared<VyxType>();
    t->kind = VyxTypeKind::Class;
    std::string sigMangle = fnType ? fnType->mangle() : "?";
    t->name = "Event<" + sigMangle + ">";
    if (fnType) t->paramTypes.push_back(std::move(fnType));
    return t;
}

inline VyxTypePtr makeUnion(std::vector<VyxTypePtr> members) {
    auto t = std::make_shared<VyxType>();
    t->kind = VyxTypeKind::Union;
    t->name = "Union";
    t->tupleTypes = std::move(members);
    return t;
}

inline VyxTypePtr makeUnknown() {
    auto t = std::make_shared<VyxType>();
    t->kind = VyxTypeKind::Unknown;
    return t;
}

inline VyxTypePtr makeGeneric(const std::string& name) {
    auto t = std::make_shared<VyxType>();
    t->kind = VyxTypeKind::Generic;
    t->name = name;
    return t;
}

inline VyxTypePtr makePack(std::vector<VyxTypePtr> members) {
    auto t = std::make_shared<VyxType>();
    t->kind = VyxTypeKind::Pack;
    t->name = "pack";
    t->tupleTypes = std::move(members);
    return t;
}

inline VyxTypePtr makeClass(const std::string& mangled) {
    auto t = std::make_shared<VyxType>();
    t->kind = VyxTypeKind::Class;
    t->name = mangled;
    return t;
}

} // namespace types

// ─────────────────────────── TypeEnv ───────────────────────────────────────
// Named substitution from generic parameter identifier → concrete VyxType.
// Used uniformly by Sema, Mono, CodeGen during instantiation. Packs are
// stored as a Pack-kind VyxType whose tupleTypes are the expanded members.
struct TypeEnv {
    std::map<std::string, VyxTypePtr> bindings;
    // Non-type generic-parameter bindings: paramName -> evaluated integer
    // value. Populated by Mono when a call site supplies a constant for a
    // `const N: usize`-style parameter. Used by `substituteType` to fill in
    // `Array.arraySize` when the element type is `[T; N]` and `N` is the
    // bound generic parameter. Unused for type-only instantiations.
    std::map<std::string, int64_t> constBindings;

    void bind(const std::string& name, VyxTypePtr type) {
        bindings[name] = std::move(type);
    }
    void bindConst(const std::string& name, int64_t value) {
        constBindings[name] = value;
    }
    bool has(const std::string& name) const {
        return bindings.find(name) != bindings.end();
    }
    bool hasConst(const std::string& name) const {
        return constBindings.find(name) != constBindings.end();
    }
    VyxTypePtr lookup(const std::string& name) const {
        auto it = bindings.find(name);
        return it != bindings.end() ? it->second : nullptr;
    }
    int64_t lookupConst(const std::string& name) const {
        auto it = constBindings.find(name);
        return it != constBindings.end() ? it->second : 0;
    }
    // Merge another env into this one; incoming bindings win on conflict.
    void merge(const TypeEnv& other) {
        for (auto& [k, v] : other.bindings) bindings[k] = v;
        for (auto& [k, v] : other.constBindings) constBindings[k] = v;
    }
};

// Forward declaration: `substituteType` Class/ErrorType branch re-mangles
// after recursing on paramTypes; `mangleGeneric` is defined further below
// alongside the other mangling helpers.
inline std::string mangleGeneric(const std::string& baseName,
                                 const std::vector<VyxTypePtr>& args);

// Apply a TypeEnv to a VyxType, producing a fresh type with all Generic(T)
// nodes replaced by env.lookup(T). Structural recursion covers every kind
// including Pointer(*T), DynArray<T>, Dict<K,V>, Function<...>, Pack.
// A Pack binding expanded in a position that is not a pack-context (e.g.
// single slot) is an error — caller must unpack first.
inline VyxTypePtr substituteType(const VyxTypePtr& t, const TypeEnv& env) {
    if (!t) return t;
    switch (t->kind) {
        case VyxTypeKind::Generic: {
            auto repl = env.lookup(t->name);
            return repl ? repl : t;
        }
        case VyxTypeKind::Pointer: {
            auto inner = substituteType(t->pointeeType, env);
            auto r = types::makePointer(inner);
            r->isMutable = t->isMutable;
            return r;
        }
        case VyxTypeKind::Reference: {
            auto inner = substituteType(t->pointeeType, env);
            return types::makeReference(inner, t->isMutable);
        }
        case VyxTypeKind::Array: {
            auto inner = substituteType(t->elementType, env);
            return types::makeArray(inner, t->arraySize);
        }
        case VyxTypeKind::Tuple: {
            std::vector<VyxTypePtr> elems;
            elems.reserve(t->tupleTypes.size());
            for (auto& e : t->tupleTypes) {
                auto s = substituteType(e, env);
                // If substitution produces a Pack, splice its members into
                // the tuple (C++26 pack expansion into tuple context).
                if (s && s->kind == VyxTypeKind::Pack) {
                    for (auto& m : s->tupleTypes) elems.push_back(m);
                } else {
                    elems.push_back(s);
                }
            }
            return types::makeTuple(elems);
        }
        case VyxTypeKind::Union: {
            std::vector<VyxTypePtr> mems;
            mems.reserve(t->tupleTypes.size());
            for (auto& m : t->tupleTypes) mems.push_back(substituteType(m, env));
            return types::makeUnion(mems);
        }
        case VyxTypeKind::Function: {
            auto fn = std::make_shared<VyxType>();
            fn->kind = VyxTypeKind::Function;
            fn->name = "fn";
            fn->paramTypes.reserve(t->paramTypes.size());
            for (auto& p : t->paramTypes) {
                auto sp = substituteType(p, env);
                if (sp && sp->kind == VyxTypeKind::Pack) {
                    for (auto& m : sp->tupleTypes) fn->paramTypes.push_back(m);
                } else {
                    fn->paramTypes.push_back(sp);
                }
            }
            fn->returnType = substituteType(t->returnType, env);
            return fn;
        }
        case VyxTypeKind::Pack: {
            // Gap 3 policy (b): nested pack types are explicitly rejected.
            // We do NOT auto-flatten `<<i32, bool>, string>` → `<i32, bool, string>`
            // because silent reinterpretation would hide user mistakes (e.g. a
            // generic param that resolved to a Pack unexpectedly). Instead we
            // produce a sentinel `Generic("nested-pack-error")` member so that
            // downstream type checking sees a concrete mismatch and the call site
            // surfaces an intelligible type error.
            //
            // Diagnostic emission: substituteType has no DiagnosticsEngine param.
            // Callers that build pack-substituted types should check for members
            // with kind==Generic and name=="nested-pack-error" and emit:
            //   diag.error(loc, "nested pack types are not supported");
            // TODO(Gap3): add a DiagnosticsEngine* overload that emits the error
            // at the pack member's source location when the sentinel is detected.
            std::vector<VyxTypePtr> mems;
            mems.reserve(t->tupleTypes.size());
            for (auto& m : t->tupleTypes) {
                auto sm = substituteType(m, env);
                if (sm && sm->kind == VyxTypeKind::Pack) {
                    // Reject nested pack: insert an error sentinel that downstream
                    // phases will report as a type mismatch.
                    auto sentinel = types::makeGeneric("nested-pack-error");
                    mems.push_back(std::move(sentinel));
                } else {
                    mems.push_back(sm);
                }
            }
            return types::makePack(mems);
        }
        case VyxTypeKind::Class:
        case VyxTypeKind::Struct:
        case VyxTypeKind::Interface:
        case VyxTypeKind::ErrorType: {
            // Nominal types are normally opaque here — their generic args live
            // in an already-monomorphized .name. However, after R5 stage 2
            // TemplateResolver emits unresolved user-generic-enum instantiations
            // as `Class{name="Option<U>", paramTypes=[Generic(U)]}`. When Mono
            // feeds such a type through `substituteType` with U→i32, we must
            // substitute the paramTypes and re-mangle the name so the caller
            // sees `Option<i32>` — otherwise the unsubstituted "Option<U>"
            // string leaks all the way to CodeGen's structTypes_ lookup and
            // fires a spurious scan-gap error. Only rewrite when at least one
            // paramType still references the env (bound Generic slot); concrete
            // monomorphs are already done and return verbatim.
            if (t->paramTypes.empty()) return t;
            bool anyBound = false;
            for (auto& p : t->paramTypes) {
                if (p && p->kind == VyxTypeKind::Generic && env.has(p->name)) {
                    anyBound = true;
                    break;
                }
            }
            if (!anyBound) return t;
            auto lt = t->name.find('<');
            if (lt == std::string::npos) return t;
            std::string base = t->name.substr(0, lt);
            std::vector<VyxTypePtr> newParams;
            newParams.reserve(t->paramTypes.size());
            for (auto& p : t->paramTypes) newParams.push_back(substituteType(p, env));
            auto r = std::make_shared<VyxType>();
            r->kind = t->kind;
            r->name = mangleGeneric(base, newParams);
            r->paramTypes = std::move(newParams);
            return r;
        }
        default:
            return t;
    }
}

// Convenience: mangle a base name with a vector of type arguments, using
// the canonical `Base<arg0,arg1,...>` form. Packs auto-splice. This is the
// ONE place generic instantiation names are built.
inline std::string mangleGeneric(const std::string& baseName,
                                 const std::vector<VyxTypePtr>& args) {
    std::string r = baseName + "<";
    bool first = true;
    for (auto& a : args) {
        if (!a) {
            if (!first) r += ",";
            r += "?";
            first = false;
            continue;
        }
        if (a->kind == VyxTypeKind::Pack) {
            for (auto& m : a->tupleTypes) {
                if (!first) r += ",";
                r += m ? m->mangle() : "?";
                first = false;
            }
        } else {
            if (!first) r += ",";
            r += a->mangle();
            first = false;
        }
    }
    r += ">";
    return r;
}

// Mixed type+const variant: each slot is either a VyxType (type argument) or
// a literal int64 (non-type / `const N: usize` argument). When the slot is a
// `const` value the mangler appends the integer literally so two
// instantiations differing only in N (`Array<i32,8>` vs `Array<i32,16>`)
// land in different mangled buckets — the existing seen_/worklist tables
// then keep them separate without further plumbing.
struct GenericArgSlot {
    bool isConst = false;
    VyxTypePtr type;
    int64_t constValue = 0;
};

inline std::string mangleGenericMixed(const std::string& baseName,
                                      const std::vector<GenericArgSlot>& args) {
    std::string r = baseName + "<";
    bool first = true;
    for (auto& a : args) {
        if (!first) r += ",";
        first = false;
        if (a.isConst) {
            r += std::to_string(a.constValue);
        } else if (!a.type) {
            r += "?";
        } else if (a.type->kind == VyxTypeKind::Pack) {
            bool firstPack = true;
            for (auto& m : a.type->tupleTypes) {
                if (!firstPack) r += ",";
                firstPack = false;
                r += m ? m->mangle() : "?";
            }
        } else {
            r += a.type->mangle();
        }
    }
    r += ">";
    return r;
}

} // namespace vyx
