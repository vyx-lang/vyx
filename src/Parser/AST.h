#pragma once
#include "../Common/SourceLocation.h"
#include "../Lexer/Token.h"
#include <memory>
#include <string>
#include <vector>
#include <optional>
#include <variant>
#include <map>

namespace vyx {

// ============================================================
//  Forward Declarations & Smart Pointers
// ============================================================

struct Expr;
struct Stmt;
struct Decl;
struct TypeAnnotation;

using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;
using DeclPtr = std::unique_ptr<Decl>;
using TypePtr = std::unique_ptr<TypeAnnotation>;

// Forward-declared Sema type system; Expr nodes carry their inferred Sema type
// after Sema has analyzed them. Codegen must prefer expr.inferredType over
// string-keyed side-maps. See docs/BOOTSTRAP_PROGRESS.md Phase D.
struct VyxType;
using VyxTypePtr = std::shared_ptr<VyxType>;

// ============================================================
//  Enums
// ============================================================

enum class TypeAnnotationKind {
    Named,          // i32, string, Player
    Pointer,        // *T
    Reference,      // &T, &mut T
    Array,          // [T; N]
    Tuple,          // (T, U)
    Function,       // fn(T, U) -> R
    Generic,        // Result<T, E>, Ref<i32>
    Union,          // T | U
    PackIndex,      // Ts[N] in type position (variadic type-pack indexing, P2 C6)
    Dependent,      // T::Item / T::Output (P2 C5 dependent associated types)
};

enum class ExprKind {
    IntLiteral,
    FloatLiteral,
    StringLiteral,
    BoolLiteral,
    CharLiteral,
    NullLiteral,
    Identifier,
    BinaryOp,
    UnaryOp,
    Call,
    MemberAccess,
    Index,
    Assignment,
    CompoundAssignment,
    Cast,
    StructInit,
    ArrayInit,
    TupleInit,
    StringInterpolation,
    SelfExpr,
    FailExpr,
    TryExpr,
    Ternary,
    Closure,
    AwaitExpr,
    InlineAsm,
    // P5-pack: pack fold expression `args...+`
    PackFold,
    // P3-Q: compile-time type reflection — T::kind / T::name / T::fields / T::methods
    TypeReflect,
    // F: sizeof...(pack) — compile-time pack-length query
    SizeofPack,
};

enum class BinaryOp {
    Add, Sub, Mul, Div, Mod,
    Eq, Neq, Lt, Lte, Gt, Gte,
    And, Or,
    BitAnd, BitOr, BitXor, Shl, Shr,
    MatchOp, // =~
    RangeOp, // ..
    Pipe,    // |>
    NullCoalesce, // ??
};

enum class UnaryOp {
    Neg, Not, BitNot,
    Ref, MutRef,    // &x, &mut x
    Deref,          // *x
    PreInc, PreDec, // ++x, --x
    PostInc, PostDec,
};

enum class CompoundOp {
    AddEq, SubEq, MulEq, DivEq, ModEq,
    ShlEq, ShrEq, BandEq, BorEq, BxorEq,
};

enum class StmtKind {
    VarDecl,
    ExprStmt,
    Return,
    If,
    While,
    For,
    ForEach,
    Block,
    Break,
    Continue,
    Match,
    Assignment,
    Defer,
    StaticAssert,
    Unsafe,
};

enum class DeclKind {
    Function,
    Struct,
    Class,
    Interface,
    ErrorDef,
    Import,
    ExternBlock,
    TypeAlias,
    GlobalVar,
    Concept,
    Macro,
};

enum class Visibility { Public, Private, Internal, Protected };

// ============================================================
//  TypeAnnotation Hierarchy
// ============================================================

struct TypeAnnotation {
    TypeAnnotationKind kind;
    SourceLocation location;
    std::string name; // Named, Generic, Function("fn")

    virtual ~TypeAnnotation() = default;

    template<typename T> T* as() { return static_cast<T*>(this); }
    template<typename T> const T* as() const { return static_cast<const T*>(this); }
};

// ============================================================
//  Base Classes (Expr, Stmt, Decl)
// ============================================================

struct Expr {
    ExprKind kind;
    SourceLocation location;

    // Sema-inferred type (null before Sema or if Sema couldn't determine).
    // Codegen reads this INSTEAD OF looking up types from variable-name side-maps.
    VyxTypePtr inferredType;

    virtual ~Expr() = default;

    template<typename T> T* as() { return static_cast<T*>(this); }
    template<typename T> const T* as() const { return static_cast<const T*>(this); }
};

struct Stmt {
    StmtKind kind;
    SourceLocation location;

    virtual ~Stmt() = default;

    template<typename T> T* as() { return static_cast<T*>(this); }
    template<typename T> const T* as() const { return static_cast<const T*>(this); }
};

struct Decl {
    DeclKind kind;
    SourceLocation location;
    std::string name;
    bool isExport = false;
    bool isImported = false;
    Visibility visibility = Visibility::Public;

    std::vector<std::string> genericParams;
    std::map<std::string, std::vector<std::string>> genericConstraints;
    std::map<std::string, std::string> typeEqualityConstraints;
    std::map<std::string, std::string> typeInequalityConstraints;
    // P5-pack: `all<Ts>: Trait` / `any<Ts>: Trait` where-clause constraints.
    // These are additive alongside the per-param `genericConstraints` map.
    // `PackConstraint::kind == All` requires every pack member to satisfy
    // `traitName`; `Any` requires at least one.  Verified by
    // Sema::verifyPackConstraints during template instantiation.
    struct PackConstraint {
        std::string packName;    // "Ts"
        std::string traitName;   // "Printable"
        enum Kind { All, Any } kind = All;
    };
    std::vector<PackConstraint> packConstraints;
    // Non-type template parameters: paramName -> declared value type
    // annotation (e.g. `usize` for `const N: usize`). When a name is in this
    // map, it is a *value* parameter rather than a type parameter; the
    // corresponding entry still appears in `genericParams` so legacy iterators
    // see a stable name list. Sema requires call-site arguments at these
    // positions to be constant expressions of the declared type; Mono mangles
    // their evaluated integer values into the instantiated decl name.
    std::map<std::string, TypePtr> genericConstParams;
    bool isVariadicGeneric = false;

    // ── P2D const-integer predicates (const-where) ────────────────────────
    // Parsed from where-clause entries that look like const expressions:
    //   `where N > 0 + N <= 1024 + N % 8 == 0`
    // Each entry carries two const expression trees and a comparison operator.
    // `verifyConstPredicates` evaluates them at instantiation time using the
    // `constBindings` map, and emits P2D-004 if any predicate is false.
    struct ConstPredicate {
        ExprPtr lhs;   // const expression tree (IntLit | IdentifierExpr | BinaryOpExpr)
        ExprPtr rhs;   // const expression tree
        BinaryOp op;   // Lt / Lte / Gt / Gte / Eq / Neq
    };
    std::vector<ConstPredicate> constPredicates;

    // ── P3-Q reflection constraints (T::kind == "struct") ─────────────────
    // Parsed from where-clause entries that look like:
    //   `where T::kind == "struct"`  /  `T::kind != "class"`
    // Verified by Sema::verifyConstraints at instantiation time.
    // On mismatch, emits P2D-007 "reflection constraint not satisfied".
    struct ReflectConstraint {
        std::string typeParam;  // "T"
        std::string member;     // "kind"
        std::string expected;   // "struct"
        bool negate = false;    // true for !=
    };
    std::vector<ReflectConstraint> reflectConstraints;

    // ── P2D-008: typeof(v) == "typename" runtime-receiver-type constraints ──
    // Parsed from where-clause entries that look like:
    //   `where typeof(v) == "i32"`  /  `typeof(v) != "string"`
    // `paramName` is the VALUE parameter whose concrete type will be checked
    // at instantiation time (the argument to typeof()).
    // Verified by Sema::verifyConstraints at instantiation time via
    // VyxType::toString() on the parameter's bound concrete type.
    // On mismatch, emits P2D-008 "typeof constraint not satisfied".
    struct TypeofConstraint {
        std::string paramName;  // "v"
        std::string expected;   // "i32"
        bool negate = false;    // true for !=
    };
    std::vector<TypeofConstraint> typeofConstraints;

    // Full template specialization marker.
    //
    // Set by the parser when the user writes `fn foo<i32>(...)` or
    // `class Vec<i32> { ... }`: the angle-bracket arguments are *concrete*
    // types rather than template parameter names. The parser then collapses
    // such a decl into:
    //   - `name`             = mangled form `Foo<i32>`.
    //   - `genericParams`    = empty (Sema sees it as a fully-concrete decl).
    //   - `isFullSpecialization` = true.
    //   - `fullSpecBaseName` = `"Foo"` (the bare template name).
    //
    // Sema's `resolveType("Foo<i32>")` / Mono's `processOne` consult
    // `name == mangled` first: when a matching full-specialization Decl
    // exists, the regular template-instantiation / clone path is skipped and
    // the user-supplied body is used verbatim.
    bool isFullSpecialization = false;
    std::string fullSpecBaseName;

    // Partial template specialization marker (P2b).
    //
    // Set by the parser when a declaration head like `class Pair<T, T>`,
    // `class Pair<T, i32>`, or `class Pair<Vec<T>, U>` is detected:
    // some argument positions are concrete types (or nested generics), while
    // others are still fresh generic parameters.
    //
    //   - `isPartialSpecialization` = true.
    //   - `partialSpecBaseName`     = the unmangled template name (e.g. "Pair").
    //   - `genericParams`           = the *partial*-spec's own fresh parameters
    //       (e.g. ["T"] for `Pair<T,T>`, ["T"] for `Pair<T,i32>`,
    //       ["T","U"] for `Pair<Vec<T>,U>`).
    //   - `specializationPattern`   = the literal type list from the angle
    //       brackets, length = number of primary-template params.
    //       Positions that are a fresh generic param carry a NamedType whose
    //       name is that param; concrete positions carry the full type AST.
    //
    // During Sema / Mono: when resolving `Pair::<i32, i32>`, every partial
    // specialization with matching `partialSpecBaseName` is tried via
    // pattern-matching against the use-site arguments. The most-specific
    // match wins (higher concrete-token count). Ambiguity (two equally-
    // specific matches) emits a coded error. If no spec matches, the
    // primary template is used.
    bool isPartialSpecialization = false;
    std::string partialSpecBaseName;
    std::vector<TypePtr> specializationPattern; // length == primary arity

    std::string externABI;
    std::vector<std::pair<std::string, std::string>> attributes;
    std::string docComment;

    virtual ~Decl() = default;

    template<typename T> T* as() { return static_cast<T*>(this); }
    template<typename T> const T* as() const { return static_cast<const T*>(this); }
};

// ============================================================
//  TypeAnnotation Derived Types (after Expr/Stmt/Decl bases)
// ============================================================

struct NamedType : TypeAnnotation {
    NamedType() { kind = TypeAnnotationKind::Named; }
};

struct PointerType : TypeAnnotation {
    TypePtr innerType;
    PointerType() { kind = TypeAnnotationKind::Pointer; }
};

struct ReferenceType : TypeAnnotation {
    TypePtr innerType;
    bool isMutable = false;
    ReferenceType() { kind = TypeAnnotationKind::Reference; }
};

struct ArrayType : TypeAnnotation {
    TypePtr elementType;
    ExprPtr size;
    ArrayType() { kind = TypeAnnotationKind::Array; }
};

struct TupleType : TypeAnnotation {
    std::vector<TypePtr> elements;
    TupleType() { kind = TypeAnnotationKind::Tuple; }
};

struct FunctionType : TypeAnnotation {
    std::vector<TypePtr> paramTypes;
    TypePtr returnType;
    FunctionType() { kind = TypeAnnotationKind::Function; name = "fn"; }
};

struct GenericType : TypeAnnotation {
    std::vector<TypePtr> typeArgs;
    // Parallel to `typeArgs`. Slot `i` holds an expression argument when the
    // call site passes a non-type (constant) value to a `const N: T` template
    // parameter, e.g. `Array<i32, 8>` -> typeArgs[1] = nullptr,
    // argExprs[1] = IntLiteral(8). When non-empty, this vector has the same
    // length as `typeArgs`. Type-only call sites leave it empty for backward
    // compatibility with the legacy iteration pattern.
    std::vector<ExprPtr> argExprs;
    GenericType() { kind = TypeAnnotationKind::Generic; }
};

struct UnionType : TypeAnnotation {
    std::vector<TypePtr> members;
    UnionType() { kind = TypeAnnotationKind::Union; }
};

// Variadic type-pack indexing (P2-generics C6).
//
// Spelled `Ts[N]` in source code (e.g. return type `-> Ts[0]`, var type
// `let x: Ts[1]`). `packName` is the bare identifier that names the
// type pack (`Ts` here); `indexExpr` is the integer-literal index
// (other expressions are rejected by Sema). Sema rewrites this node
// during variadic instantiation: it looks up the i-th type bound to
// the pack and substitutes the result back into the surrounding
// annotation. After Sema, well-formed programs no longer carry
// `PackIndexType` nodes; Mono and CodeGen treat any residual one as a
// pass-through (Mono clones it, CodeGen emits a diagnostic if it sees
// one in a fully concrete decl).
struct PackIndexType : TypeAnnotation {
    std::string packName;
    ExprPtr indexExpr;
    PackIndexType() { kind = TypeAnnotationKind::PackIndex; }
};

// Dependent associated type reference (P2-generics C5).
struct DependentType : TypeAnnotation {
    std::string baseName;
    std::string memberName;
    DependentType() { kind = TypeAnnotationKind::Dependent; }
};

// ============================================================
//  Shared Data Types
// ============================================================

struct ParamDecl {
    std::string name;
    TypePtr type;
    bool isMutRef = false;
    ExprPtr defaultValue;
};

struct CaptureItem {
    std::string name;
    bool byRef = false;
    bool move = false;
    ExprPtr moveExpr;
};

struct ClosureParam {
    std::string name;
    TypePtr type;
};

struct InterpPart {
    bool isExpr = false;
    std::string text;
    ExprPtr expr;
};

struct FieldDecl {
    Visibility visibility = Visibility::Private;
    bool isWeak = false;
    std::string name;
    TypePtr type;
    ExprPtr defaultValue;
    SourceLocation location;
    std::vector<std::string> extraNames; // x, y, z: f32
    bool isStatic = false; // static field: not in per-instance layout, emitted as module-scope global
};

struct NestedPattern {
    std::string name;
    bool isTuple = false;
    std::vector<NestedPattern> children;
    // Literal tuple-pattern element (e.g. `case (0, x) =>` — slot 0 is a
    // literal, slot 1 is a binding). Set `isLiteral=true` and stash the
    // literal expression here. Non-literal slots leave this nullptr and use
    // `name` as the binding. Only IntLiteral currently; extend as needed.
    bool isLiteral = false;
    ExprPtr literalExpr;

    NestedPattern() = default;
    NestedPattern(NestedPattern&&) noexcept = default;
    NestedPattern& operator=(NestedPattern&&) noexcept = default;
    // Copy: literalExpr is intentionally NOT deep-cloned (unique_ptr). The
    // AST cloners in SemaCloneHelpers handle expression duplication when
    // tuple patterns need to be cloned. Regular vector operations just
    // need this copy to satisfy std::vector's interface without moving.
    NestedPattern(const NestedPattern& o)
        : name(o.name), isTuple(o.isTuple),
          children(o.children),
          isLiteral(o.isLiteral),
          literalExpr(nullptr) {}
    NestedPattern& operator=(const NestedPattern& o) {
        if (this == &o) return *this;
        name = o.name;
        isTuple = o.isTuple;
        children = o.children;
        isLiteral = o.isLiteral;
        literalExpr.reset();
        return *this;
    }
};

struct MatchArm {
    SourceLocation location;
    std::string label;
    std::string bindingName;
    TypePtr typePattern;
    ExprPtr valuePattern;
    ExprPtr guardExpr;
    std::vector<std::string> tupleBindings;
    std::vector<NestedPattern> nestedPatterns;
    bool isDefault = false;
    StmtPtr body;
};

struct MethodDecl {
    Visibility visibility = Visibility::Public;
    std::string name;
    std::string docComment;
    std::vector<ParamDecl> params;
    TypePtr returnType;
    StmtPtr body;
    bool isOverride = false;
    bool isAsync = false;
    bool isStatic = false;
    SourceLocation location;
    std::vector<std::pair<std::string, std::string>> attributes;
    std::vector<std::string> genericParams;
    std::map<std::string, std::vector<std::string>> genericConstraints;
    std::map<std::string, std::string> typeEqualityConstraints;
    std::map<std::string, std::string> typeInequalityConstraints;
    std::map<std::string, TypePtr> genericConstParams;
    // P5-pack: all<Ts>/any<Ts> pack constraints on method-level generics.
    std::vector<Decl::PackConstraint> packConstraints;
};

struct Refinement {
    std::string paramName;
    BinaryOp op = BinaryOp::Neq;
    int64_t value = 0;
};

struct AssociatedType {
    std::string name;
    TypePtr defaultType;
    // Optional qualifying trait name for disambiguated bindings:
    //   `type A::Item = i32;` inside a class implementing both A and B.
    // Empty for unqualified `type Item = X;` bindings.
    std::string qualifyingTrait;
};

// Trait/class-body `const NAME: Type [= expr];` member declaration.
// In an interface (trait) body this is a *requirement*: each implementing
// class must provide a value. In a class body this is a *definition*:
// the class binds `NAME` to the given constant expression, which is
// reachable via `T::NAME` when T is a generic param bound to the trait.
struct ConstMember {
    std::string name;
    TypePtr type;
    ExprPtr defaultValue;   // null when this is a requirement without default
    SourceLocation location;
};

// ============================================================
//  Expr Derived Types
// ============================================================

struct IntLiteralExpr : Expr {
    int64_t value = 0;
    IntLiteralExpr() { kind = ExprKind::IntLiteral; }
};

struct FloatLiteralExpr : Expr {
    double value = 0.0;
    FloatLiteralExpr() { kind = ExprKind::FloatLiteral; }
};

struct StringLiteralExpr : Expr {
    std::string value;
    StringLiteralExpr() { kind = ExprKind::StringLiteral; }
};

struct BoolLiteralExpr : Expr {
    bool value = false;
    BoolLiteralExpr() { kind = ExprKind::BoolLiteral; }
};

struct CharLiteralExpr : Expr {
    std::string value;
    CharLiteralExpr() { kind = ExprKind::CharLiteral; }
};

struct NullLiteralExpr : Expr {
    NullLiteralExpr() { kind = ExprKind::NullLiteral; }
};

struct IdentifierExpr : Expr {
    std::string name;
    std::vector<TypePtr> callTypeArgs;
    // Parallel to callTypeArgs: slot i holds a constant-value expression when
    // the corresponding turbofish argument is a non-type generic parameter
    // (e.g. `Array::<i32, 16>` → callTypeArgs[1]=nullptr, callArgExprs[1]=IntLit(16)).
    // Empty for calls with only type arguments.  Same semantics as GenericType::argExprs.
    std::vector<ExprPtr> callArgExprs;
    TypePtr typeAnnotation;
    IdentifierExpr() { kind = ExprKind::Identifier; }
};

struct BinaryOpExpr : Expr {
    BinaryOp op = BinaryOp::Add;
    ExprPtr lhs;
    ExprPtr rhs;
    BinaryOpExpr() { kind = ExprKind::BinaryOp; }
};

struct UnaryOpExpr : Expr {
    UnaryOp op = UnaryOp::Neg;
    ExprPtr operand;
    UnaryOpExpr() { kind = ExprKind::UnaryOp; }
};

struct CallExpr : Expr {
    ExprPtr callee;
    std::vector<ExprPtr> args;
    std::vector<std::string> argNames;
    CallExpr() { kind = ExprKind::Call; }
};

struct MemberAccessExpr : Expr {
    ExprPtr object;
    std::string member;
    std::vector<TypePtr> callTypeArgs;
    MemberAccessExpr() { kind = ExprKind::MemberAccess; }
};

struct IndexExpr : Expr {
    ExprPtr object;
    ExprPtr indexExpr;
    IndexExpr() { kind = ExprKind::Index; }
};

struct AssignmentExpr : Expr {
    ExprPtr lhs;
    ExprPtr rhs;
    AssignmentExpr() { kind = ExprKind::Assignment; }
};

struct CompoundAssignmentExpr : Expr {
    CompoundOp op = CompoundOp::AddEq;
    ExprPtr target;
    ExprPtr value;
    CompoundAssignmentExpr() { kind = ExprKind::CompoundAssignment; }
};

struct CastExpr : Expr {
    ExprPtr operand;
    TypePtr targetType;
    CastExpr() { kind = ExprKind::Cast; }
};

struct StructInitExpr : Expr {
    std::string structName;
    std::vector<std::pair<std::string, ExprPtr>> fieldInits;
    ExprPtr spreadBase;
    TypePtr typeAnnotation;
    StructInitExpr() { kind = ExprKind::StructInit; }
};

struct ArrayInitExpr : Expr {
    std::vector<ExprPtr> elements;
    std::vector<bool> elementIsSpread;
    ExprPtr repeatCount;
    ArrayInitExpr() { kind = ExprKind::ArrayInit; }
};

struct TupleInitExpr : Expr {
    std::vector<ExprPtr> elements;
    TupleInitExpr() { kind = ExprKind::TupleInit; }
};

struct StringInterpExpr : Expr {
    std::vector<InterpPart> parts;
    StringInterpExpr() { kind = ExprKind::StringInterpolation; }
};

struct SelfExpr : Expr {
    SelfExpr() { kind = ExprKind::SelfExpr; }
};

struct FailExpr : Expr {
    std::string typeName;
    std::string variant;
    ExprPtr message;
    // BUG-LV-12: distinguishes `fail E.V(payload)` (true, payload field of
    // the variant) from `fail E.V.with("debug-msg")` (false, debug log only).
    // Both share `message` for backward parser compatibility, but codegen
    // routes them through different IR paths so payload-bearing variants
    // construct the inner ADT instance with its data laid out properly.
    bool isPayload = false;
    FailExpr() { kind = ExprKind::FailExpr; }
};

struct TryExpr : Expr {
    ExprPtr inner;
    TryExpr() { kind = ExprKind::TryExpr; }
};

struct TernaryExpr : Expr {
    ExprPtr condition;
    ExprPtr trueExpr;
    ExprPtr falseExpr;
    TernaryExpr() { kind = ExprKind::Ternary; }
};

struct ClosureExpr : Expr {
    std::vector<CaptureItem> captures;
    std::vector<ClosureParam> params;
    TypePtr returnType;
    StmtPtr body;
    ExprPtr singleExpr;
    ClosureExpr() { kind = ExprKind::Closure; }
};

struct AwaitExpr : Expr {
    ExprPtr inner;
    AwaitExpr() { kind = ExprKind::AwaitExpr; }
};

struct InlineAsmExpr : Expr {
    std::string asmTemplate;
    std::string constraints;
    std::vector<ExprPtr> operands;
    bool hasSideEffects = true;
    InlineAsmExpr() { kind = ExprKind::InlineAsm; }
};

// P5-pack: pack fold expression — `args...+`, `flags...&&`, etc.
//
// Spelled `packIdent ... binOp` in source code. Sema verifies `packIdent`
// names a variadic parameter. Mono expands to a left-associative binary
// chain: args_0 op args_1 op args_2 … (at least one element required).
// For zero-length packs Sema/Mono emits error P2D-004.
struct PackFoldExpr : Expr {
    std::string packName;   // e.g. "args" or "flags"
    BinaryOp op = BinaryOp::Add;
    PackFoldExpr() { kind = ExprKind::PackFold; }
};

// P3-Q: compile-time type reflection expression.
//
// Produced by the parser when it encounters `T::kind`, `T::name`,
// `T::fields`, or `T::methods` where the LHS is a bare identifier
// that starts with an uppercase letter (type-parameter heuristic).
//
// Sema:
//   kind/name  → inferred type = string (compile-time constant)
//   fields/methods → inferred type = string (ReflectPack; Mono expands)
//
// Mono: replace with a concrete StringLiteralExpr (kind/name) or expand
//   a ForEachStmt whose collection is this node into N sequential copies
//   of the loop body (fields/methods).
struct TypeReflectExpr : Expr {
    std::string typeParam;  // e.g. "T"
    std::string member;     // "kind" | "name" | "fields" | "methods"
    TypeReflectExpr() { kind = ExprKind::TypeReflect; }
};

// ============================================================
//  Stmt Derived Types
// ============================================================

struct VarDeclStmt : Stmt {
    bool isConst = true;
    bool isComptime = false;
    std::string varName;
    TypePtr varType;
    ExprPtr initExpr;
    StmtPtr elseBranch;
    // Tuple destructuring: `let (a, b, c) = expr;`
    // When non-empty, `varName` is unused and each entry names one binding.
    // Populated by the parser; consumed by Sema and CodeGen.
    std::vector<std::string> tupleBindings;
    VarDeclStmt() { kind = StmtKind::VarDecl; }
};

struct ExprStmt : Stmt {
    ExprPtr expr;
    DeclPtr localDecl;
    ExprStmt() { kind = StmtKind::ExprStmt; }
};

struct ReturnStmt : Stmt {
    ExprPtr expr;
    ReturnStmt() { kind = StmtKind::Return; }
};

struct IfStmt : Stmt {
    ExprPtr condition;
    StmtPtr thenBranch;
    std::vector<std::pair<ExprPtr, StmtPtr>> elifBranches;
    StmtPtr elseBranch;
    IfStmt() { kind = StmtKind::If; }
};

struct WhileStmt : Stmt {
    ExprPtr condition;
    StmtPtr body;
    WhileStmt() { kind = StmtKind::While; }
};

struct ForStmt : Stmt {
    StmtPtr init;
    ExprPtr condition;
    ExprPtr step;
    StmtPtr body;
    ForStmt() { kind = StmtKind::For; }
};

struct ForEachStmt : Stmt {
    std::string varName;
    std::vector<std::string> destructure;
    ExprPtr collection;
    StmtPtr body;
    ForEachStmt() { kind = StmtKind::ForEach; }
};

struct BlockStmt : Stmt {
    std::vector<StmtPtr> statements;
    // P4-C scope-aware auto-drop: names declared in THIS scope whose type
    // is heap-owning AND have not been moved out / escaped. Populated by
    // Sema's analyzeBlock just before popScope; consumed by CodeGen's
    // emitBlock / emitFunctionDecl at block-end emission.
    std::vector<std::string> autoDropLocals;
    // P4-D follow-up (limit #2 fix): locals whose type is `Option<H>` /
    // `Result<H,E>` / `Result<T,H>` for some heap-owning H. Populated by
    // Sema's analyzeBlock alongside autoDropLocals; consumed by CodeGen
    // to emit a tag-conditional payload drop *in addition to* (or
    // *instead of*, when the ADT itself is not heap-owning) the regular
    // drop. The triple is {localName, innerHeapTypeName, variantTag}
    // where variantTag is the i32 discriminant the payload is associated
    // with (0 for Some/Ok, 1 for Err with heap-owning err type).
    struct AutoDropAdt {
        std::string localName;
        std::string innerHeapTypeName;
        int variantTag = 0;
    };
    std::vector<AutoDropAdt> autoDropAdtPayloads;
    BlockStmt() { kind = StmtKind::Block; }
};

struct BreakStmt : Stmt {
    BreakStmt() { kind = StmtKind::Break; }
};

struct ContinueStmt : Stmt {
    ContinueStmt() { kind = StmtKind::Continue; }
};

struct MatchStmt : Stmt {
    ExprPtr expr;
    std::vector<MatchArm> arms;
    MatchStmt() { kind = StmtKind::Match; }
};

struct AssignStmt : Stmt {
    ExprPtr target;
    ExprPtr value;
    // P4-D follow-up (limit #1 fix): Sema sets this to true when `target`
    // is an identifier bound to a heap-owning value that is still live
    // (not moved, not escaped, and we're rebinding it in-place). CodeGen
    // emits `Type.drop(&target)` BEFORE storing the new value to avoid
    // leaking the heap data the old binding owned.
    bool dropOldLvalue = false;
    AssignStmt() { kind = StmtKind::Assignment; }
};

struct DeferStmt : Stmt {
    StmtPtr body;
    DeferStmt() { kind = StmtKind::Defer; }
};

struct StaticAssertStmt : Stmt {
    ExprPtr expr;
    std::string message;
    StaticAssertStmt() { kind = StmtKind::StaticAssert; }
};

struct UnsafeStmt : Stmt {
    StmtPtr body;
    UnsafeStmt() { kind = StmtKind::Unsafe; }
};

// ============================================================
//  Decl Derived Types
// ============================================================

struct FunctionDecl : Decl {
    std::vector<ParamDecl> params;
    TypePtr returnType;
    StmtPtr body;
    bool isAsync = false;
    bool isComptime = false;
    bool isBench = false;
    std::vector<Refinement> refinements;
    // Non-empty when this FunctionDecl is a Mono-synthesised enum-method
    // instantiation (e.g. `Option<T>.map<U>` cloned for codegen). Carries the
    // enum's name ("Option", "Result", or a user ErrorDef) so emitFunctionDecl
    // can set up the tagged-union dispatch context (currentClassName_,
    // structTypes_/structFieldNames_/errorEnumValues_ aliases) needed for the
    // body's `match (self) { case Some(v) => ... }` to unwrap the payload as T.
    std::string syntheticEnumReceiver;
    // Non-empty when this FunctionDecl is a Mono-synthesised class-method
    // instantiation (e.g. `Box<i32>.dup<i64>` cloned for codegen). Carries the
    // fully-mangled receiver class name (e.g. "Box<i32>") so emitFunctionDecl
    // can set currentClassName_ and allow self.field accesses to resolve the
    // correct field offset inside the method-generic body.
    std::string syntheticClassReceiver;
    FunctionDecl() { kind = DeclKind::Function; }
};

struct StructDecl : Decl {
    std::string parentName;
    std::vector<FieldDecl> fields;
    std::vector<MethodDecl> methods;
    StructDecl() { kind = DeclKind::Struct; }
};

struct ClassDecl : Decl {
    std::string parentName;
    std::vector<std::string> interfaces;
    std::vector<FieldDecl> fields;
    std::vector<MethodDecl> methods;
    bool isImplBlock = false;
    // Associated-type bindings: 'impl Trait for Target { type Item = X; ... }'.
    std::vector<AssociatedType> associatedTypes;
    // Trait-bound const members (P5-traitconst). Populated by:
    //   class U8Like { const MAX: i64 = 255; ... }
    // Sema looks these up when resolving `T::MAX` where T is a generic
    // param bound to a trait that declares `MAX`.
    std::vector<ConstMember> constMembers;
    ClassDecl() { kind = DeclKind::Class; }
};

struct InterfaceDecl : Decl {
    std::vector<MethodDecl> methods;
    std::vector<AssociatedType> associatedTypes;
    // Trait-body const requirements (P5-traitconst). Populated by:
    //   trait BoundedInt { const MAX: i64; ... }
    // Each entry demands implementing classes to define the same name
    // with a matching type; Sema checks conformance during impl analysis.
    std::vector<ConstMember> constMembers;
    // Trait composition (2026-04-23): supertraits declared via
    //   `trait Num : Add + Sub + Mul + Div + Zero + One {}`
    // The first supertrait lives in `parentName`, the rest in `interfaces`
    // — same slot layout as ClassDecl so Sema can treat `class X : Trait`
    // and `trait X : Super` uniformly when walking bound requirements.
    // Transitive method requirements are collected in verifyMethodConstraints.
    std::string parentName;
    std::vector<std::string> interfaces;
    InterfaceDecl() { kind = DeclKind::Interface; }
};

struct ErrorDefDecl : Decl {
    std::vector<std::string> variants;
    std::vector<std::vector<TypePtr>> variantTypes;
    std::vector<MethodDecl> methods;
    ErrorDefDecl() { kind = DeclKind::ErrorDef; }
};

struct ImportDecl : Decl {
    std::vector<std::string> importPath;
    std::vector<std::string> importNames;
    ImportDecl() { kind = DeclKind::Import; }
};

struct ExternBlockDecl : Decl {
    std::string namespaceName;
    std::vector<DeclPtr> externDecls;
    ExternBlockDecl() { kind = DeclKind::ExternBlock; }
};

struct TypeAliasDecl : Decl {
    TypePtr aliasType;
    bool isNewtype = false;
    TypeAliasDecl() { kind = DeclKind::TypeAlias; }
};

struct GlobalVarDecl : Decl {
    TypePtr varType;
    StmtPtr initBody; // ExprStmt wrapping init expression
    bool isMutableVar = false;
    GlobalVarDecl() { kind = DeclKind::GlobalVar; }
};

struct ConceptDecl : Decl {
    std::vector<std::string> conceptRequires;
    ConceptDecl() { kind = DeclKind::Concept; }
};

struct MacroDecl : Decl {
    std::vector<std::string> macroParams;
    std::string macroBody;
    MacroDecl() { kind = DeclKind::Macro; }
};

// ============================================================
//  Translation Unit
// ============================================================

struct TranslationUnit {
    std::string filename;
    std::vector<DeclPtr> declarations;
};

// F: sizeof...(pack) — compile-time pack-length query.
//
// Spelled `sizeof...(packIdent)` in source. Sema resolves this to a
// compile-time integer constant equal to the number of elements in the
// variadic pack bound to `packName`. Mono substitutes a plain
// IntLiteralExpr(packLen) during monomorphisation.
struct SizeofPackExpr : Expr {
    std::string packName;   // e.g. "args"
    SizeofPackExpr() { kind = ExprKind::SizeofPack; }
};

// ============================================================
//  Helper Constructors
// ============================================================

namespace ast {

inline std::unique_ptr<IntLiteralExpr> makeIntLiteral(SourceLocation loc, int64_t value) {
    auto e = std::make_unique<IntLiteralExpr>();
    e->location = loc;
    e->value = value;
    return e;
}

inline std::unique_ptr<FloatLiteralExpr> makeFloatLiteral(SourceLocation loc, double value) {
    auto e = std::make_unique<FloatLiteralExpr>();
    e->location = loc;
    e->value = value;
    return e;
}

inline std::unique_ptr<StringLiteralExpr> makeStringLiteral(SourceLocation loc, std::string value) {
    auto e = std::make_unique<StringLiteralExpr>();
    e->location = loc;
    e->value = std::move(value);
    return e;
}

inline std::unique_ptr<BoolLiteralExpr> makeBoolLiteral(SourceLocation loc, bool value) {
    auto e = std::make_unique<BoolLiteralExpr>();
    e->location = loc;
    e->value = value;
    return e;
}

inline std::unique_ptr<IdentifierExpr> makeIdentifier(SourceLocation loc, std::string name) {
    auto e = std::make_unique<IdentifierExpr>();
    e->location = loc;
    e->name = std::move(name);
    return e;
}

inline std::unique_ptr<BinaryOpExpr> makeBinaryOp(SourceLocation loc, BinaryOp op, ExprPtr lhs, ExprPtr rhs) {
    auto e = std::make_unique<BinaryOpExpr>();
    e->location = loc;
    e->op = op;
    e->lhs = std::move(lhs);
    e->rhs = std::move(rhs);
    return e;
}

inline std::unique_ptr<UnaryOpExpr> makeUnaryOp(SourceLocation loc, UnaryOp op, ExprPtr operand) {
    auto e = std::make_unique<UnaryOpExpr>();
    e->location = loc;
    e->op = op;
    e->operand = std::move(operand);
    return e;
}

inline std::unique_ptr<CallExpr> makeCall(SourceLocation loc, ExprPtr callee, std::vector<ExprPtr> args) {
    auto e = std::make_unique<CallExpr>();
    e->location = loc;
    e->callee = std::move(callee);
    e->args = std::move(args);
    return e;
}

inline std::unique_ptr<MemberAccessExpr> makeMemberAccess(SourceLocation loc, ExprPtr obj, std::string member) {
    auto e = std::make_unique<MemberAccessExpr>();
    e->location = loc;
    e->object = std::move(obj);
    e->member = std::move(member);
    return e;
}

inline std::unique_ptr<BlockStmt> makeBlock(SourceLocation loc, std::vector<StmtPtr> stmts) {
    auto s = std::make_unique<BlockStmt>();
    s->location = loc;
    s->statements = std::move(stmts);
    return s;
}

inline std::unique_ptr<ReturnStmt> makeReturn(SourceLocation loc, ExprPtr expr) {
    auto s = std::make_unique<ReturnStmt>();
    s->location = loc;
    s->expr = std::move(expr);
    return s;
}

inline std::unique_ptr<ExprStmt> makeExprStmt(SourceLocation loc, ExprPtr expr) {
    auto s = std::make_unique<ExprStmt>();
    s->location = loc;
    s->expr = std::move(expr);
    return s;
}

inline std::unique_ptr<VarDeclStmt> makeVarDecl(SourceLocation loc, bool isConst, std::string name, TypePtr type, ExprPtr init) {
    auto s = std::make_unique<VarDeclStmt>();
    s->location = loc;
    s->isConst = isConst;
    s->varName = std::move(name);
    s->varType = std::move(type);
    s->initExpr = std::move(init);
    return s;
}

inline std::unique_ptr<NamedType> makeNamedType(SourceLocation loc, std::string name) {
    auto t = std::make_unique<NamedType>();
    t->location = loc;
    t->name = std::move(name);
    return t;
}

inline std::unique_ptr<GenericType> makeGenericType(SourceLocation loc, std::string name, std::vector<TypePtr> args) {
    auto t = std::make_unique<GenericType>();
    t->location = loc;
    t->name = std::move(name);
    t->typeArgs = std::move(args);
    return t;
}

inline std::unique_ptr<TypeReflectExpr> makeTypeReflect(SourceLocation loc,
                                                          std::string typeParam,
                                                          std::string member) {
    auto e = std::make_unique<TypeReflectExpr>();
    e->location = loc;
    e->typeParam = std::move(typeParam);
    e->member = std::move(member);
    return e;
}

} // namespace ast
} // namespace vyx
