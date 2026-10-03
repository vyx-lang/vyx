#pragma once
#include "../Parser/AST.h"
#include "../Sema/Type.h"
#include "../Sema/LangItemRegistry.h"
#include "../Common/Diagnostics.h"
#include "../Common/Determinism.h"

#pragma warning(push, 0)
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/MC/MCSubtargetInfo.h>
#include <llvm/TargetParser/SubtargetFeature.h>
#pragma warning(pop)

#include <string>
#include <string_view>
#include <optional>
#include <set>
#include <map>
#include <memory>

namespace vyx {

class CodeGen {
public:
    CodeGen(DiagnosticsEngine& diag, const std::string& moduleName, const std::string& targetTriple = "");

    /// Wire the Sema-built lang-item registry into CodeGen so layout /
    /// match / `?` paths can identify "is this the stdlib Option / Result
    /// slot?" via the registered decl name rather than a hard-coded
    /// `"Option<"` / `"Result<"` string. Must be set before `generate()`
    /// for any unit that uses Option / Result — the helpers return false
    /// when the registry is null or the slot is unregistered.
    void setLangItems(const LangItemRegistry* reg) { langItems_ = reg; }

    /// Full codegen pipeline. If \p diag_ already has errors (e.g. Sema or parse failed), returns
    /// immediately: user-facing issues are diagnosed once upstream; emitExpr/emitStmt also short-circuit
    /// after the first codegen error to avoid duplicate/cascade IR diagnostics.
    void generate(const TranslationUnit& unit);
    void enableDebugInfo(const std::string& filename);
    void enableCoverage();

    void optimize(int level = 2);

    bool emitIR(const std::string& filename);
    bool emitObject(const std::string& filename);

    llvm::Module& getModule() { return *module_; }

    /// P3-B3: returns true iff this module declares (and references) any
    /// `extern "C"` symbol with the `LLVM*` prefix — i.e. the LLVM-C API.
    /// The driver uses this to auto-append `-lLLVM-C` (and the matching
    /// lib path) to the link command so a single-file `compiler.exe foo.vyx`
    /// invocation can self-host without a Vyx.toml `libs = LLVM-C` entry.
    bool referencesLlvmCRuntime() const;

    /// Diagnostics with no concrete source line (IR verify, codegen, backend I/O).
    static SourceLocation codegenInternalSourceLocation();

private:
    // Top-level generate phase helpers
    void generatePredeclareCRuntime();
    void generateFirstPassDeclareTypes();
    void generateDeriveMethods();
    void generatePredeclareSmartPtrRuntime();
    void generateRegisterErrorEnums();
    void generateEmitErrorDefMethods();
    void generateCreateInterfaceVtables();
    void generateEmitDefaultTraitMethods();
    void generatePreCreateClassStructs();
    void generateForwardDeclareFunctionsAndExtern();
    void generateEmitGlobalVars();
    void generateClassMethodsForwardDeclare();
    void generateClassMethodsEmitBodies();
    void generatePopulateInstanceVtables();
    void generateFreeFunctionBodies();
    void generateInstantiatePendingGenericClasses();
    void generateFinalizeModule();

    // Type mapping
    llvm::Type* toLLVMType(const VyxType& type);
    llvm::Type* toLLVMType(const TypeAnnotation& ann);
    llvm::StructType* optionResultStructFromVyxType(const VyxTypePtr& type);
    llvm::StructType* optionResultStructFromExpr(const Expr& expr);
    llvm::StructType* resolveOptionResultStructForExpr(const Expr& expr, bool allowReturnType);
    llvm::StructType* variantPayloadOptionResultHint(
        const Expr& expr, llvm::StructType* containerTy, const std::string& variant);
    std::string mangleTypeAnnotation(const TypeAnnotation& ann);
    // Nested form: recursive positions keep canonical spelling
    // (`Vec<Box<T>>`) so the key aligns with Mono's VyxType::mangle()
    // output, avoiding the `Vec<rawptr>` scan gap.
    std::string mangleTypeAnnotationNested(const TypeAnnotation& ann);

    // ── Phase D: prefer Sema-inferred types over variable-name side-maps ──
    //
    // These helpers centralise the lookup so we can gradually retire
    // containerElemTypes_, ptrElemTypes_, classVarTypes_, refInnerTypeNames_,
    // adtOrigTypeArgs_ etc. They first consult expr.inferredType (set by Sema);
    // if that is unavailable they fall back to the legacy side-maps so the
    // migration can proceed one call site at a time without regressions.

    /// Pointee / element type of `expr` as an LLVM type, or nullptr.
    /// Handles Pointer / Reference / Optional / RefPtr / BoxPtr / ScopePtr
    /// (→ pointeeType) and DynArray / Array / Stack / Queue / Set / UnorderedSet
    /// (→ elementType).
    llvm::Type* inferredPointeeLLVM(const Expr& expr);

    /// Inner type *name* for a Ref<T> / Box<T> / Scope<T> / Option<T> expression
    /// (e.g. "Num", "Token"). Returns empty string when not applicable.
    std::string inferredInnerTypeName(const Expr& expr);

    /// Container element type as LLVM type (Vec<T>, Stack<T>, Queue<T>, Set<T>).
    /// Returns nullptr if `expr` is not a container or its element type is unknown.
    llvm::Type* inferredContainerElemLLVM(const Expr& expr);

    /// Dict<K,V> key/value LLVM types (nullptr if not a Dict or unknown).
    llvm::Type* inferredDictKeyLLVM(const Expr& expr);
    llvm::Type* inferredDictValueLLVM(const Expr& expr);

    /// Class / struct / interface name carried by `expr`, or empty.
    /// For generic classes returns the mangled form (e.g. "Vec<i32>").
    std::string inferredClassName(const Expr& expr);

    /// Resolve Ref<T>/Box<T>/Scope<T> inner type name, preferring Sema's
    /// inferredType on `e`, falling back to refInnerTypeNames_[legacyKey].
    /// Pass the variable name for legacyKey so migration is gradual.
    std::string resolveRefInner(const Expr& e, const std::string& legacyKey);

    /// Resolve a class/container name for an expression, preferring Sema's
    /// inferredType, falling back to classVarTypes_[legacyKey].
    std::string resolveClassName(const Expr& e, const std::string& legacyKey);

    /// Snapshot / restore all variable-name keyed side-maps.
    /// Use around each function/method body emission to prevent leaking
    /// type tracking between functions (e.g. classVarTypes_["n"] from one
    /// method polluting 'n' in another). Only variable-scope side-maps are
    /// touched; class/struct/function registries remain intact.
    struct SideMapSnapshot {
        std::map<std::string, std::string> containerTypes;
        std::map<std::string, llvm::Type*>  containerElemTypes;
        std::map<std::string, llvm::Type*>  containerValTypes;
        std::map<std::string, llvm::Type*>  ptrElemTypes;
        std::map<std::string, std::string>  classVarTypes;
        std::map<std::string, std::string>  interfaceVarTypes;
        std::map<std::string, std::string>  refInnerTypeNames;
        std::set<std::string>               unsignedVars;
        std::set<std::string>               volatileVars;
        std::map<std::string, llvm::Type*>  closureFatPtrVars;
    };
    SideMapSnapshot snapshotVarScopeSideMaps();
    void clearVarScopeSideMaps();
    void restoreVarScopeSideMaps(SideMapSnapshot&& snap);

    // Declaration codegen
    void emitDecl(Decl& decl);
    void emitFunctionDecl(Decl& decl);
    void emitStructDecl(const Decl& decl);
    void emitExternBlock(const Decl& decl);

    // Statement codegen
    void emitStmt(const Stmt& stmt);
    void emitBlock(const Stmt& block);
    void emitVarDecl(const Stmt& stmt);
    void emitIfStmt(const Stmt& stmt);
    void emitWhileStmt(const Stmt& stmt);
    void emitForStmt(const Stmt& stmt);
    void emitReturnStmt(const Stmt& stmt);
    void emitAssignment(const Stmt& stmt);

    // Walk every open block scope (`blockAutoDropStack_` / `adtBlockStack_`)
    // top-to-bottom and emit `.drop()` for each owning local / ADT payload.
    // Shared by `emitReturnStmt` (early `return`) and `emitFailExpr`
    // (BUG-LV-07: `fail` was the fourth drop-injection point that previously
    // skipped scope cleanup, leaving Ref<T> rc cells leaked / Vec buffers
    // un-freed on the unwind path). Caller is responsible for any preceding
    // `emitDeferredStmts(0)` and the actual `CreateRet`.
    void emitScopeExitDrops();

    // Expression codegen
    llvm::Value* emitExpr(const Expr& expr);
    llvm::Value* emitBinaryOp(const Expr& expr);
    llvm::Value* emitUnaryOp(const Expr& expr);
    llvm::Value* emitCall(const Expr& expr);
    llvm::Value* emitMemberAccess(const Expr& expr);
    llvm::Value* emitStringInterpolation(const Expr& expr);
    llvm::Value* emitCompoundAssignment(const Expr& expr);
    llvm::Value* emitIndex(const Expr& expr);

    // Built-in call dispatch (split from emitCall)
    llvm::Value* emitBuiltinContainer(const Expr& expr);
    llvm::Value* emitBuiltinMemory(const Expr& expr);
    llvm::Value* emitBuiltinIO(const Expr& expr);
    llvm::Value* emitUserCall(const Expr& expr);

    // Match
    void emitMatchStmt(const Stmt& stmt);

    // Class methods
    void emitClassDecl(const Decl& decl, bool declareOnly = false);
    void instantiateGenericClass(const Decl& genericDecl,
        const std::vector<llvm::Type*>& concreteTypes,
        const std::vector<std::string>& typeNames);
    llvm::Value* emitMethodCall(const Expr& expr);

    // Closures / Error handling
    llvm::Value* emitClosure(const Expr& expr);
    llvm::Value* emitFailExpr(const Expr& expr);
    llvm::Value* emitTryExpr(const Expr& expr);
    // Fat closure pointer struct: { ptr fn_ptr, ptr env_ptr }
    llvm::StructType* getOrCreateClosureFatPtrType();
    // Wrap a raw llvm::Function* into a heap { trampoline, null } fat-pointer.
    // The trampoline discards the leading env_ptr slot and forwards remaining
    // args to rawFn. Used anywhere a bare fn must enter the uniform fn-value
    // ABI, including fn-typed params, locals, and Vec<fn> storage.
    llvm::Value* wrapRawFnAsFatPtr(llvm::Function* rawFn);

    // Closure tracking
    uint32_t closureCounter_ = 0;
    // Variables that hold a heap-boxed fat-closure pointer { fn_ptr, env_ptr }
    // (as opposed to a bare function pointer with no captures). The mapped
    // value is the closure's declared return type (captured when the fat
    // pointer is created) — used at call-sites to avoid defaulting the
    // call's return type to i64 and truncating struct returns.
    std::map<std::string, llvm::Type*> closureFatPtrVars_;

    // Recursion guard for codegen
    int emitDepth_ = 0;
    static constexpr int MaxEmitDepth = 100;

    // Current class being emitted (for self.field resolution)
    std::string currentClassName_;
    std::map<std::string, llvm::Type*> genericTypeParams_;
    std::map<std::string, std::string> genericTypeParamNames_;
    std::set<std::string> instantiatedClasses_;

    // Loop stack for break/continue
    struct LoopInfo {
        llvm::BasicBlock* breakBB = nullptr;
        llvm::BasicBlock* continueBB = nullptr;
        size_t deferStackDepth = 0;
        // P4-D follow-up (limit #5): snapshot of `blockAutoDropStack_.size()`
        // at loop entry. `break` / `continue` walk the stack from the top
        // down to this index (exclusive) emitting drops for every scope
        // they bypass on their way out of / around the loop body.
        size_t blockStackDepthAtEntry = 0;
    };
    std::vector<LoopInfo> loopStack_;

    // P4-D follow-up: stack of "currently-open" block scopes' auto-drop lists.
    // Each emitBlock pushes its `bs.autoDropLocals` here on entry and pops on
    // exit. Used by `emitReturnStmt` (limit #5: walk every enclosing scope to
    // drop heap-owning locals before the function-body terminator) and by
    // `emitAssignment` (limit #1: skip injecting an old-lvalue drop when the
    // current lvalue is on this list — the natural block-end drop will take
    // care of it, and double-dropping a Vec/Ref<T> is a use-after-free).
    std::vector<const std::vector<std::string>*> blockAutoDropStack_;

    // P4-D follow-up (limit #2): companion stack to blockAutoDropStack_,
    // each entry is a vector of `BlockStmt::AutoDropAdt` entries collected
    // by Sema for this block. emitReturnStmt / emitBlock / break / continue
    // walk this stack to emit tag-conditional payload drops alongside the
    // regular full-local drops.
    std::vector<const std::vector<BlockStmt::AutoDropAdt>*> adtBlockStack_;

    // P4-D follow-up (limit #1 fix): emit `.drop(&local)` for `local` if it
    // resolves to a heap-owning struct alloca with a registered drop method.
    // Mirrors the logic embedded in emitBlock's natural-exit drop loop, but
    // hoisted so emitAssignment / emitReturnStmt can reuse it. Returns true
    // if a drop call was actually emitted.
    bool emitDropForLocal(const std::string& localName);

    // P4-D follow-up (limit #2 fix): emit a tag-conditional payload drop
    // for an ADT-typed local (Option<H>/Result<H,_>/Result<_,H>) — when
    // the runtime tag matches `variantTag`, the payload field is treated
    // as a `H` and dispatched to `H.drop`. No-op if the local's struct
    // type is missing or `H.drop` isn't registered.
    bool emitAdtPayloadDrop(const std::string& localName,
                            const std::string& innerHeapTypeName,
                            int variantTag);

    // Defer stack: each entry is one block scope's deferred statements
    std::vector<std::vector<const Stmt*>> deferStack_;
    void emitDeferredStmts(size_t fromDepth);

    // Helpers
    llvm::AllocaInst* createEntryBlockAlloca(llvm::Function* fn, llvm::Type* type, const std::string& name);
    llvm::Value* getOrCreateString(const std::string& str);
    llvm::Value* castToType(llvm::Value* val, llvm::Type* targetType);
    llvm::Function* getOrCreatePrintf();
    llvm::Function* getOrCreateI64ToCharsFunction();
    llvm::Function* getOrCreatePanicFn();
    void emitPanicCall(llvm::Value* msgStr);
    llvm::Value* emitPrintCall(const std::vector<llvm::Value*>& args);
    llvm::Value* getVariableAddress(const Expr& expr);
    llvm::Value* getMemberAddress(const Expr& expr, int depth = 0);
    llvm::Value* numericCast(llvm::Value* val, llvm::Type* targetType);
    llvm::Value* createSafeICmp(llvm::CmpInst::Predicate pred, llvm::Value* lhs, llvm::Value* rhs, const llvm::Twine& name = "");
    llvm::Value* coerceToBool(llvm::Value* val, const llvm::Twine& name = "");
    static bool isInternalStructType(const std::string& name);
    std::string llvmTypeToString(llvm::Type* ty);
    llvm::StructType* getOrCreateResultType();
    /// Returns a per-inner-type Option struct with a correctly-sized payload
    /// when sizeof(innerTy) > 8.  Falls back to getOrCreateResultType() for
    /// small (≤ 8 byte) payloads so the common case is unchanged.
    llvm::StructType* getOrCreateResultTypeForInner(llvm::Type* innerTy);
    llvm::StructType* getOrCreateStringType();
    llvm::StructType* getOrCreateAnyType();
    llvm::Value* createStringValue(const std::string& str);
    llvm::Value* extractStringPtr(llvm::Value* strVal);
    llvm::Value* emitHashValue(llvm::Value* key);
    static bool isSimpleExpr(const Expr& expr);

    // Async / await lowering support
    void emitAsyncFunctionDecl(Decl& decl);
    llvm::Value* emitAwaitExpr(const Expr& expr);

    std::set<std::string> genericInstantiating_;

    std::map<std::string, const Decl*> comptimeDecls_;
    struct ComptimeVal {
        enum { Int, Float, Bool, Str, None } kind = None;
        int64_t intVal = 0;
        double floatVal = 0.0;
        bool boolVal = false;
        std::string strVal;
    };
    int comptimeDepth_ = 0;
    static constexpr int MaxComptimeDepth = 200;
    ComptimeVal comptimeEvalExpr(const Expr& expr, std::map<std::string, ComptimeVal>& env);
    ComptimeVal comptimeExecBody(const Stmt& body, std::map<std::string, ComptimeVal>& env);
    llvm::Value* tryComptimeCall(const CallExpr& call);

    struct PendingGenericClass {
        const Decl* templateDecl;
        std::string mangledName;
        std::map<std::string, std::string> paramToType;
    };
    std::vector<PendingGenericClass> pendingGenericClasses_;

    struct GenericArgs {
        std::string baseName;
        std::vector<std::string> typeArgs;
    };
    static std::optional<GenericArgs> parseGenericArgs(std::string_view mangledName);
    void setupGenericTypeParams(const Decl& decl);

    void emitDebugLocation(const SourceLocation& loc);

    // Stdlib lang-item registry.  Non-owning; set by Compiler / main.cpp
    // via setLangItems() after Sema finishes.  Nullable to keep the raw
    // constructor usable from tooling that doesn't run Sema (e.g. ad-hoc
    // IR builders); in that case the slot helpers below simply report the
    // name as non-matching.  Consult `isOptionSlotName` /
    // `isResultSlotName` rather than poking this directly.
    const LangItemRegistry* langItems_ = nullptr;

    /// True iff `name` refers to the option lang-item's mangled form
    /// (e.g. "Option<i32>" when stdlib registered @[lang_item("option")]
    /// enum Option<T>, or a user-renamed variant like "Maybe<T>").
    /// Returns false when the registry is null or the "option" slot is
    /// unregistered — there is no hard-coded "Option" fallback.
    bool isOptionSlotName(llvm::StringRef name) const;
    /// Same for the result lang-item slot; additionally accepts the
    /// internal `__Result` / `__Result_N` layout-alias prefix which is
    /// CodeGen-private and never user-visible.
    bool isResultSlotName(llvm::StringRef name) const;
    /// Union helper: either option or result slot.
    bool isOptionOrResultSlotName(llvm::StringRef name) const {
        return isOptionSlotName(name) || isResultSlotName(name);
    }

    /// True iff the struct type name `structName` refers to a class that
    /// declares `Deref<...>` in its interface list (or any interface whose
    /// bare name is `"Deref"`).  Used by `castToType` to decide when a
    /// struct-to-pointer coercion must heap-allocate the struct rather
    /// than stack-spill it — required for any smart-pointer-shaped class
    /// whose `deref()` method would otherwise read through a dangling
    /// stack pointer after the callee returns.  Also used by `toLLVMType`
    /// to short-circuit `Box<T>` / `Ref<T>` / user-defined smart pointers
    /// to an opaque pointer regardless of T (Phase 6 generalisation).
    /// `Box<T>` / `Ref<T>` are the canonical stdlib cases; user-defined
    /// `class MyBox<T> : Deref<T>` opts in through the same trait.
    /// Lookup order: (1) current TU `unit_->declarations`, (2) lang-item
    /// registry slots `"box"` / `"ref"`, (3) canonical-name fallback for
    /// `Box` / `Ref` / `Scope` when neither source knows about the class
    /// (handles TUs that use these via Sema's implicit stdlib resolution
    /// without a direct `use std.ref;`).
    bool classDeclaresDeref(llvm::StringRef structName) const;

    DiagnosticsEngine& diag_;
    const TranslationUnit* unit_ = nullptr;
    std::unique_ptr<llvm::LLVMContext> context_;
    std::unique_ptr<llvm::Module> module_;
    std::unique_ptr<llvm::IRBuilder<>> builder_;
    std::unique_ptr<llvm::DIBuilder> debugBuilder_;
    llvm::DICompileUnit* debugCU_ = nullptr;
    llvm::DIFile* debugFile_ = nullptr;
    bool emitDebug_ = false;
    bool emitCoverage_ = false;
    // When set, toLLVMType returns an opaque pointer for unresolved generic
    // names so the walk can proceed. In the new strict model this still
    // emits a diagnostic and sets `hadHardTypeError_`; the i8* return is
    // kept only so the remainder of the walk surfaces every gap in a single
    // pass rather than bailing at the first miss.
    bool softTypeLookup_ = false;
    // Distinct carve-out for the ONE legitimate "this type may not be
    // lowerable and that is fine" case: walking the fields of an
    // uninstantiated generic template body (see CodeGenStruct.cpp).
    // Inner references like `Dict<T, bool>` exist only after Mono
    // substitutes T; the template-body walk itself produces no object
    // code. When this flag is set, toLLVMType silently falls back to i8*
    // without emitting a diagnostic. Mutually exclusive with
    // `softTypeLookup_`: both branches return i8*, but only softTypeLookup_
    // trips `hadHardTypeError_`.
    bool unreachableTemplateBodyWalk_ = false;
    // Sticky flag set whenever a soft-mode lookup had to substitute a
    // default (pointer/i64/i32) for a type it couldn't properly lower.
    // At module-emission time (emitObject) this forces a single aggregate
    // error if the diagnostic stream is otherwise clean — belts-and-braces
    // defence against silent garbage IR.
    bool hadHardTypeError_ = false;
    std::map<std::string, llvm::GlobalVariable*> coverageCounters_;
    void emitCoverageIncrement(const std::string& funcName, int region);

    // Named values (local variable addresses — AllocaInst or GlobalVariable)
    std::map<std::string, llvm::Value*> namedValues_;
    std::set<std::string> volatileVars_;
    std::set<std::string> unsignedVars_;
    llvm::Type* getValuePtrType(llvm::Value* v);
    bool isUnsignedExpr(const Expr& expr);

    // Target type hint for Some()/None constructors in typed variable declarations
    llvm::StructType* targetTypeHint_ = nullptr;

    // Target element type hint for fixed-size array initializers: `let a: [i64;N] = [lit, lit, lit]`
    llvm::Type* targetArrayElemHint_ = nullptr;

    // Functions
    std::map<std::string, llvm::Function*> functions_;

    // Struct types
    std::map<std::string, llvm::StructType*> structTypes_;
    std::map<std::string, std::vector<std::string>> structFieldNames_;
    // Parallel to structFieldNames_: stores the resolved type name for each field
    // For pointer/reference fields, stores the inner (pointee) type name
    std::map<std::string, std::vector<std::string>> structFieldTypeNames_;

    // String constants cache
    std::map<std::string, llvm::GlobalVariable*> stringConstants_;

    // Container type tracking: variable name -> "Vec"/"Dict"/"Set"/"Stack"/"Queue"
    std::map<std::string, std::string> containerTypes_;

    // Container element type tracking: variable name -> LLVM element type
    std::map<std::string, llvm::Type*> containerElemTypes_;
    // Dict value type tracking: variable name -> LLVM value type
    std::map<std::string, llvm::Type*> containerValTypes_;

    // Typed pointer element type tracking: variable name -> pointee LLVM type
    // e.g. `var p: *i32 = &x;` → ptrElemTypes_["p"] = i32Ty
    std::map<std::string, llvm::Type*> ptrElemTypes_;

    // Class variable tracking for auto-drop: variable name -> class type name
    // Stores mangled names for generic classes (e.g. "Vec<i32>" not "Vec")
    std::map<std::string, std::string> classVarTypes_;

    // Mangle type annotation with generic param substitution from current context
    std::string resolveAndMangleTypeAnnotation(const TypeAnnotation& ann);
    // Build mangled class name from base + type args (e.g. "Vec" + [i32] → "Vec<i32>")
    std::string buildMangledClassName(const std::string& baseName,
                                      const std::vector<TypePtr>& typeArgs);
    // Same as above but also considers `callArgExprs` so that const-generic
    // slots (typeArgs[i]==nullptr, callArgExprs[i]==IntLit) mangle to their
    // literal value (e.g. `Array::<i32,4>` → "Array<i32,4>"). Falls back to
    // the type-only path when callArgExprs is empty.
    std::string buildMangledClassName(const std::string& baseName,
                                      const std::vector<TypePtr>& typeArgs,
                                      const std::vector<ExprPtr>& callArgExprs);
    void trackParamType(const ParamDecl& p);

    // Find a class method by exact mangled name.
    llvm::Function* findClassMethod(const std::string& className,
                                    const std::string& methodName);
    // Instance dispatch can inherit non-static methods from class parents.
    llvm::Function* findClassInstanceMethod(const std::string& className,
                                            const std::string& methodName);

    // Primitive-target impl support (2026-04-23): `impl Hashable for i32`
    // produces functions mangled `i32.hash` (etc.).  This helper gives the
    // LLVM value type CodeGen uses for a primitive `self` parameter, so
    // impl method bodies bind `self` by value rather than by pointer.
    // Returns nullptr for non-primitive names.
    llvm::Type* primitiveImplSelfLLVMType(const std::string& name);

    // Interface variable tracking: variable name -> interface name
    std::map<std::string, std::string> interfaceVarTypes_;

    // Ref<T> inner type tracking: variable name -> inner type name (e.g. "a" -> "Num")
    std::map<std::string, std::string> refInnerTypeNames_;

    // Original (pre-mangled) generic type args for ADT types
    // e.g. "Option<rawptr>" -> ["Box<Expr>"]
    std::map<std::string, std::vector<std::string>> adtOrigTypeArgs_;

    // Error enum values: "ErrorType.Variant" -> integer code
    std::map<std::string, int> errorEnumValues_;

    // For wide Option/Result structs (__Result_N): maps the struct name to the
    // concrete inner (payload) LLVM type so CodeGenMatch can do a typed load.
    std::map<std::string, llvm::Type*> resultInnerTypes_;

    // For Result<T, E> with T ≠ E: maps (LLVM struct name OR mangled Vyx
    // name, variant label) to the per-variant payload type so `case Ok(v)`
    // binds v as T and `case Err(e)` binds e as E regardless of whether
    // the underlying result struct shares the 8-byte slot or was widened
    // to max(sizeof T, sizeof E).
    std::map<std::string,
        std::map<std::string, llvm::Type*>> resultVariantInnerTypes_;

    // Bitfield layout: "StructName.fieldName" -> {bit_offset, bit_width}
    std::map<std::string, std::pair<int, int>> bitfieldLayouts_;

    // Temporary: last collected element type for iterator chain
    llvm::Type* lastCollectedElemType_ = nullptr;

    // C++ ABI name mangling helpers
    static std::string msvcMangleType(const std::string& t);
    static std::string itaniumMangleType(const std::string& t);
    bool usesMsvcAbi() const;
};

} // namespace vyx
