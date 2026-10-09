/**
 *  vyx_codegen.cpp — implementation of the bootstrap LLVM C ABI.
 *
 *  Current surface:
 *    * 121 VYX_API exports in include/vyx_codegen.h.
 *    * Module/type/function/global/builder helpers.
 *    * Integer, float, cast, aggregate, switch/PHI/select, and inline-asm
 *      builders.
 *    * IR verification, optimization, textual IR output, and object emission.
 *    * Bootstrap string/runtime helpers used by generated code.
 *
 *  Every entry-point in vyx_codegen.h now drives LLVM directly. Failure
 *  paths populate `lastError` via the Vyx-side `vyx_rt_get_last_error`
 *  channel; the previous "stub: <name>" placeholders are gone.
 *
 *  This translation unit is purposefully self-contained: it does NOT
 *  reuse any header out of src/CodeGen so the bootstrap module can be
 *  evolved independently of the host compiler.
 */

/* VYX_CODEGEN_BUILD is set by the CMake target_compile_definitions on
 * the SHARED target so the dllexport branch in vyx_codegen.h is taken
 * during *this* TU's compilation while consumers see dllimport.       */
#include "vyx_codegen.h"

#pragma warning(push, 0)
#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/IR/AutoUpgrade.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/DataLayout.h>
#include <cstdlib>
#include <llvm/IR/CallingConv.h>
#include <llvm/IR/Comdat.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/AbsoluteSymbols.h>
#include <llvm/ExecutionEngine/Orc/Mangling.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/DynamicLibrary.h>
#include <llvm/Support/Alignment.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/SubtargetFeature.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/Transforms/IPO/GlobalDCE.h>
#include <llvm/Transforms/IPO/Internalize.h>
#include <llvm/Transforms/IPO/StripDeadPrototypes.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Analysis/ModuleSummaryAnalysis.h>
#include <llvm/Analysis/ProfileSummaryInfo.h>
#include <llvm/IR/ModuleSummaryIndex.h>
#pragma warning(pop)

#include <cstring>
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>


namespace {

struct VyxEhFunction {
    llvm::AllocaInst* head = nullptr;
    llvm::BasicBlock* unwind = nullptr;
};

struct VyxEhAction {
    llvm::Function* destructor;
    llvm::Function* release;
    llvm::Function* thunk;
};

/* Per-module DLL state. Allocated by vyx_rt_module_new, freed by
 * vyx_rt_module_free. The handle returned to Vyx is `(void*)this`.
 *  - The IRBuilder is created lazily by vyx_rt_builder_new so that
 *    callers can construct types/functions before they need a builder.
 */
struct VyxRtModule {
    std::unique_ptr<llvm::LLVMContext> ctx;
    std::unique_ptr<llvm::Module>      mod;
    std::unique_ptr<llvm::IRBuilder<>> builder;
    std::string                        name;
    std::string                        triple;
    std::string                        targetCpu;
    std::string                        targetFeatures;
    std::string                        requestedTargetCpu;
    int32_t                            relocationModel = 0;
    std::string                        lastError;
    std::unique_ptr<llvm::DIBuilder>   debugBuilder;
    llvm::DIFile*                      debugFile = nullptr;
    llvm::DICompileUnit*               debugCU = nullptr;
    bool                               optimized = false;
    int32_t                            optimizedLevel = -1;
    bool                               debugEnabled = false;
    bool                               debugFinalized = false;
    std::unordered_map<llvm::Function*, VyxEhFunction> ehFunctions;
    std::unordered_set<llvm::Function*> ehDestructors;
    std::unordered_set<llvm::Function*> ehReleases;
    std::vector<VyxEhAction> ehActions;
};

#ifdef _WIN32
static int vyx_rt_jit_fltused = 0;

static void* vyx_rt_jit_operator_new(std::size_t n) {
    return ::operator new(n);
}

static void vyx_rt_jit_operator_delete_sized(void* p, std::size_t n) noexcept {
    ::operator delete(p, n);
}

static void* vyx_rt_jit_operator_array_new(std::size_t n) {
    return ::operator new[](n);
}

static void vyx_rt_jit_operator_array_delete_sized(void* p, std::size_t n) noexcept {
    ::operator delete[](p, n);
}

static int vyx_rt_jit_purecall() {
    std::abort();
    return 0;
}
#endif

static void vyx_rt_register_jit_process_symbols() {
    static bool registered = false;
    if (registered) return;
    registered = true;

    // Do not leave the allocator family to platform-dependent process symbol
    // lookup. On Windows an ORC module can otherwise resolve malloc and free
    // through different CRT import domains, which corrupts owned `str`
    // allocations such as Clone<str>. Bind the complete family together.
    llvm::sys::DynamicLibrary::AddSymbol(
        "malloc",
        reinterpret_cast<void*>(static_cast<void* (*)(std::size_t)>(&std::malloc)));
    llvm::sys::DynamicLibrary::AddSymbol(
        "calloc",
        reinterpret_cast<void*>(static_cast<void* (*)(std::size_t, std::size_t)>(&std::calloc)));
    llvm::sys::DynamicLibrary::AddSymbol(
        "realloc",
        reinterpret_cast<void*>(static_cast<void* (*)(void*, std::size_t)>(&std::realloc)));
    llvm::sys::DynamicLibrary::AddSymbol(
        "free",
        reinterpret_cast<void*>(static_cast<void (*)(void*)>(&std::free)));
    llvm::sys::DynamicLibrary::AddSymbol(
        "memcpy",
        reinterpret_cast<void*>(static_cast<void* (*)(void*, const void*, std::size_t)>(&std::memcpy)));
    llvm::sys::DynamicLibrary::AddSymbol(
        "memmove",
        reinterpret_cast<void*>(static_cast<void* (*)(void*, const void*, std::size_t)>(&std::memmove)));
    llvm::sys::DynamicLibrary::AddSymbol(
        "memset",
        reinterpret_cast<void*>(static_cast<void* (*)(void*, int, std::size_t)>(&std::memset)));
    llvm::sys::DynamicLibrary::AddSymbol(
        "memcmp",
        reinterpret_cast<void*>(static_cast<int (*)(const void*, const void*, std::size_t)>(&std::memcmp)));
    llvm::sys::DynamicLibrary::AddSymbol(
        "strlen",
        reinterpret_cast<void*>(static_cast<std::size_t (*)(const char*)>(&std::strlen)));
    llvm::sys::DynamicLibrary::AddSymbol(
        "snprintf",
        reinterpret_cast<void*>(static_cast<int (*)(char*, std::size_t, const char*, ...)>(&std::snprintf)));
    llvm::sys::DynamicLibrary::AddSymbol(
        "printf",
        reinterpret_cast<void*>(static_cast<int (*)(const char*, ...)>(&std::printf)));
    llvm::sys::DynamicLibrary::AddSymbol(
        "time",
        reinterpret_cast<void*>(static_cast<std::time_t (*)(std::time_t*)>(&std::time)));
    llvm::sys::DynamicLibrary::AddSymbol(
        "clock",
        reinterpret_cast<void*>(static_cast<std::clock_t (*)()>(&std::clock)));
    // Decimal append is a shared stdlib ABI used by normal MIR calls.  Register
    // the complete family explicitly: an ORC JIT on Windows cannot reliably
    // discover symbols exported by the bootstrap runtime through the process
    // image, while AOT resolves the same names from the static Vyx runtime.
    llvm::sys::DynamicLibrary::AddSymbol(
        "vyx_int_decimal_len_abi",
        reinterpret_cast<void*>(static_cast<int64_t (*)(int64_t)>(&vyx_int_decimal_len_abi)));
    llvm::sys::DynamicLibrary::AddSymbol(
        "vyx_uint_decimal_len_abi",
        reinterpret_cast<void*>(static_cast<int64_t (*)(uint64_t)>(&vyx_uint_decimal_len_abi)));
    llvm::sys::DynamicLibrary::AddSymbol(
        "vyx_int_to_string_into_abi",
        reinterpret_cast<void*>(static_cast<int64_t (*)(int64_t, char*)>(&vyx_int_to_string_into_abi)));
    llvm::sys::DynamicLibrary::AddSymbol(
        "vyx_uint_to_string_into_abi",
        reinterpret_cast<void*>(static_cast<int64_t (*)(uint64_t, char*)>(&vyx_uint_to_string_into_abi)));

#ifdef _WIN32
    llvm::sys::DynamicLibrary::AddSymbol(
        "??2@YAPEAX_K@Z",
        reinterpret_cast<void*>(&vyx_rt_jit_operator_new));
    llvm::sys::DynamicLibrary::AddSymbol(
        "??3@YAXPEAX_K@Z",
        reinterpret_cast<void*>(&vyx_rt_jit_operator_delete_sized));
    llvm::sys::DynamicLibrary::AddSymbol(
        "??_U@YAPEAX_K@Z",
        reinterpret_cast<void*>(&vyx_rt_jit_operator_array_new));
    llvm::sys::DynamicLibrary::AddSymbol(
        "??_V@YAXPEAX_K@Z",
        reinterpret_cast<void*>(&vyx_rt_jit_operator_array_delete_sized));
    llvm::sys::DynamicLibrary::AddSymbol(
        "_purecall",
        reinterpret_cast<void*>(&vyx_rt_jit_purecall));
    llvm::sys::DynamicLibrary::AddSymbol(
        "_fltused",
        reinterpret_cast<void*>(&vyx_rt_jit_fltused));
#endif
}

static std::string stringifyError(llvm::Error err);

static bool addJitAbsoluteSupportSymbols(llvm::orc::LLJIT& jit, std::string& errText) {
#ifdef _WIN32
    llvm::orc::MangleAndInterner mangle(jit.getExecutionSession(), jit.getDataLayout());
    llvm::orc::SymbolMap symbols;
    auto add = [&](const char* name, void* ptr) {
        symbols[mangle(name)] = llvm::orc::ExecutorSymbolDef::fromPtr(
            ptr, llvm::JITSymbolFlags::Exported);
    };
    add("malloc", reinterpret_cast<void*>(static_cast<void* (*)(std::size_t)>(&std::malloc)));
    add("calloc", reinterpret_cast<void*>(static_cast<void* (*)(std::size_t, std::size_t)>(&std::calloc)));
    add("realloc", reinterpret_cast<void*>(static_cast<void* (*)(void*, std::size_t)>(&std::realloc)));
    add("free", reinterpret_cast<void*>(static_cast<void (*)(void*)>(&std::free)));
    add("memcpy", reinterpret_cast<void*>(static_cast<void* (*)(void*, const void*, std::size_t)>(&std::memcpy)));
    add("memmove", reinterpret_cast<void*>(static_cast<void* (*)(void*, const void*, std::size_t)>(&std::memmove)));
    add("memset", reinterpret_cast<void*>(static_cast<void* (*)(void*, int, std::size_t)>(&std::memset)));
    add("memcmp", reinterpret_cast<void*>(static_cast<int (*)(const void*, const void*, std::size_t)>(&std::memcmp)));
    add("strlen", reinterpret_cast<void*>(static_cast<std::size_t (*)(const char*)>(&std::strlen)));
    add("vyx_int_decimal_len_abi", reinterpret_cast<void*>(static_cast<int64_t (*)(int64_t)>(&vyx_int_decimal_len_abi)));
    add("vyx_uint_decimal_len_abi", reinterpret_cast<void*>(static_cast<int64_t (*)(uint64_t)>(&vyx_uint_decimal_len_abi)));
    add("vyx_int_to_string_into_abi", reinterpret_cast<void*>(static_cast<int64_t (*)(int64_t, char*)>(&vyx_int_to_string_into_abi)));
    add("vyx_uint_to_string_into_abi", reinterpret_cast<void*>(static_cast<int64_t (*)(uint64_t, char*)>(&vyx_uint_to_string_into_abi)));
    add("??2@YAPEAX_K@Z", reinterpret_cast<void*>(&vyx_rt_jit_operator_new));
    add("??3@YAXPEAX_K@Z", reinterpret_cast<void*>(&vyx_rt_jit_operator_delete_sized));
    add("??_U@YAPEAX_K@Z", reinterpret_cast<void*>(&vyx_rt_jit_operator_array_new));
    add("??_V@YAXPEAX_K@Z", reinterpret_cast<void*>(&vyx_rt_jit_operator_array_delete_sized));
    add("_purecall", reinterpret_cast<void*>(&vyx_rt_jit_purecall));
    add("_fltused", reinterpret_cast<void*>(&vyx_rt_jit_fltused));
    if (auto err = jit.getMainJITDylib().define(llvm::orc::absoluteSymbols(std::move(symbols)))) {
        errText = stringifyError(std::move(err));
        return false;
    }
#else
    (void)jit;
    (void)errText;
#endif
    return true;
}

/* Cast helpers; all error-checking reduces to "is the handle null?". */
inline VyxRtModule* asMod(void* h)  { return static_cast<VyxRtModule*>(h); }

inline std::string asStr(const char* s, uint64_t len) {
    if (!s) return std::string{};
    if (len == 0) return std::string{s};
    return std::string{s, static_cast<size_t>(len)};
}

inline llvm::Type*       asTy (void* p) { return static_cast<llvm::Type*>(p); }
inline llvm::Value*      asVal(void* p) { return static_cast<llvm::Value*>(p); }
inline llvm::Function*   asFn (void* p) { return static_cast<llvm::Function*>(p); }
inline llvm::BasicBlock* asBB (void* p) { return static_cast<llvm::BasicBlock*>(p); }

// Ordinary loads/stores may retain an alloca's alignment only when the
// pointer is a direct, statically known struct-field path from that alloca.
// Do not infer alignment through packed structs, casts, array/byte GEPs,
// dynamic indices, or arbitrary pointer-producing instructions: those paths
// need an explicit ABI alignment from their caller instead.
static bool provenAllocaStructFieldAlignment(const llvm::DataLayout& dataLayout,
                                             llvm::Value* pointer,
                                             llvm::Align& alignment) {
    uint64_t byteOffset = 0;
    while (true) {
        if (auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(pointer)) {
            alignment = llvm::commonAlignment(alloca->getAlign(), byteOffset);
            return true;
        }

        auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(pointer);
        if (!gep || gep->getNumIndices() != 2) return false;

        auto* structTy = llvm::dyn_cast<llvm::StructType>(gep->getSourceElementType());
        if (!structTy || structTy->isPacked()) return false;

        auto index = gep->idx_begin();
        auto* zeroIndex = llvm::dyn_cast<llvm::ConstantInt>(index->get());
        ++index;
        auto* fieldIndex = llvm::dyn_cast<llvm::ConstantInt>(index->get());
        if (!zeroIndex || !zeroIndex->isZero() || !fieldIndex || fieldIndex->isNegative()) {
            return false;
        }

        const uint64_t field = fieldIndex->getLimitedValue(structTy->getNumElements());
        if (field >= structTy->getNumElements()) return false;

        const uint64_t fieldOffset = dataLayout.getStructLayout(structTy)->getElementOffset(
            static_cast<unsigned>(field));
        if (fieldOffset > std::numeric_limits<uint64_t>::max() - byteOffset) return false;
        byteOffset += fieldOffset;
        pointer = gep->getPointerOperand();
    }
}

enum class RtProfId : std::size_t {
    IntType,
    PtrType,
    VoidType,
    FnType,
    ArrayType,
    VectorType,
    StructTyNamed,
    StructTySetBody,
    GetStructFieldTy,
    StructFieldOffset,
    TypeAllocSize,
    ConstInt,
    ConstStr,
    ConstNull,
    ConstPtrNull,
    ConstReal,
    ParseRealBits,
    AddFunction,
    SetFunctionAttr,
    SetLinkage,
    GetParam,
    SetParamName,
    AddGlobal,
    SetInitializer,
    SetGlobalConstant,
    BuilderNew,
    AppendBlock,
    PositionAtEnd,
    PositionBefore,
    GetInsertBlock,
    ClearInsertion,
    BuildBin,
    BuildICmp,
    BuildBr,
    BuildCondBr,
    BuildRet,
    BuildRetVoid,
    BuildAlloca,
    BuildLifetimeStart,
    BuildLifetimeEnd,
    BuildLoad,
    BuildStore,
    BuildGep,
    BuildCall,
    BuildInvoke,
    BuildInlineAsm,
    BuildCast,
    FloatType,
    BuildFloat,
    BuildExtractValue,
    BuildInsertValue,
    BuildSwitch,
    AddCase,
    BuildPhi,
    AddIncoming,
    BuildSelect,
    ConstNamedStruct,
    SetAlignment,
    BuildUnreachable,
    AliasScopeDomain,
    AliasScope,
    SetAliasScopes,
    IsArrayType,
    ArrayElemType,
    Verify,
    Optimize,
    PrintToFile,
    EmitObject,
    EmitBitcode,
    Count
};

struct RtProfRec {
    const char* name;
    uint64_t calls;
    uint64_t ns;
};

static std::array<RtProfRec, static_cast<std::size_t>(RtProfId::Count)>& rtProfileRecords() {
    static std::array<RtProfRec, static_cast<std::size_t>(RtProfId::Count)> records{{
        {"int_type", 0, 0},
        {"ptr_ty", 0, 0},
        {"void_ty", 0, 0},
        {"fn_type", 0, 0},
        {"array_ty", 0, 0},
        {"struct_ty_named", 0, 0},
        {"struct_ty_set_body", 0, 0},
        {"get_struct_field_ty", 0, 0},
        {"struct_field_offset", 0, 0},
        {"type_alloc_size", 0, 0},
        {"const_int", 0, 0},
        {"const_str", 0, 0},
        {"const_null", 0, 0},
        {"const_ptr_null", 0, 0},
        {"const_real", 0, 0},
        {"parse_real_bits", 0, 0},
        {"add_function", 0, 0},
        {"set_function_attr", 0, 0},
        {"set_linkage", 0, 0},
        {"get_param", 0, 0},
        {"set_param_name", 0, 0},
        {"add_global", 0, 0},
        {"set_initializer", 0, 0},
        {"set_global_constant", 0, 0},
        {"builder_new", 0, 0},
        {"append_block", 0, 0},
        {"position_at_end", 0, 0},
        {"get_insert_block", 0, 0},
        {"clear_insertion", 0, 0},
        {"build_bin", 0, 0},
        {"build_icmp", 0, 0},
        {"build_br", 0, 0},
        {"build_cond_br", 0, 0},
        {"build_ret", 0, 0},
        {"build_ret_void", 0, 0},
        {"build_alloca", 0, 0},
        {"build_lifetime_start", 0, 0},
        {"build_lifetime_end", 0, 0},
        {"build_load", 0, 0},
        {"build_store", 0, 0},
        {"build_gep", 0, 0},
        {"build_call", 0, 0},
        {"build_invoke", 0, 0},
        {"build_inline_asm", 0, 0},
        {"build_cast", 0, 0},
        {"float_type", 0, 0},
        {"build_float", 0, 0},
        {"build_extract_value", 0, 0},
        {"build_insert_value", 0, 0},
        {"build_switch", 0, 0},
        {"add_case", 0, 0},
        {"build_phi", 0, 0},
        {"add_incoming", 0, 0},
        {"build_select", 0, 0},
        {"const_named_struct", 0, 0},
        {"set_alignment", 0, 0},
        {"build_unreachable", 0, 0},
        {"alias_scope_domain", 0, 0},
        {"alias_scope", 0, 0},
        {"set_alias_scopes", 0, 0},
        {"is_array_type", 0, 0},
        {"array_elem_type", 0, 0},
        {"verify", 0, 0},
        {"optimize", 0, 0},
        {"print_to_file", 0, 0},
        {"emit_object", 0, 0},
        {"emit_bitcode", 0, 0},
    }};
    return records;
}

static int rtProfileLevel() {
    static int level = -1;
    if (level >= 0) return level;
    const char* raw = std::getenv("VYX_RT_PROFILE");
    if (!raw || raw[0] == '\0') {
        level = 0;
    } else {
        char* end = nullptr;
        long parsed = std::strtol(raw, &end, 10);
        level = (end == raw) ? 1 : static_cast<int>(parsed);
        if (level < 0) level = 0;
    }
    if (level > 0) {
        std::atexit([] {
            auto& records = rtProfileRecords();
            std::fprintf(stderr, "[vyx-rt-prof] calls");
            if (rtProfileLevel() > 1) {
                std::fprintf(stderr, " ns");
            }
            std::fprintf(stderr, "\n");
            for (const auto& rec : records) {
                if (rec.calls == 0) continue;
                std::fprintf(stderr, "[vyx-rt-prof] %-24s %llu",
                             rec.name,
                             static_cast<unsigned long long>(rec.calls));
                if (rtProfileLevel() > 1) {
                    std::fprintf(stderr, " %llu",
                                 static_cast<unsigned long long>(rec.ns));
                }
                std::fprintf(stderr, "\n");
            }
        });
    }
    return level;
}

class RtProfScope {
public:
    explicit RtProfScope(RtProfId id)
        : id_(id),
          level_(rtProfileLevel()),
          start_(level_ > 1 ? std::chrono::steady_clock::now() : clock_time{}) {
        if (level_ > 0) {
            rtProfileRecords()[static_cast<std::size_t>(id_)].calls += 1;
            if (level_ >= 10) {
                std::fprintf(stderr, "[vyx-rt-trace] %s\n",
                             rtProfileRecords()[static_cast<std::size_t>(id_)].name);
                std::fflush(stderr);
            }
        }
    }

    ~RtProfScope() {
        if (level_ <= 1) return;
        const auto stop = std::chrono::steady_clock::now();
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start_).count();
        rtProfileRecords()[static_cast<std::size_t>(id_)].ns += static_cast<uint64_t>(ns);
    }

private:
    using clock_time = std::chrono::steady_clock::time_point;
    RtProfId id_;
    int level_;
    clock_time start_;
};

#define VYX_RT_PROF(ID) RtProfScope vyxRtProfScope_##__LINE__(RtProfId::ID)

static void finalizeDebugInfoForRt(VyxRtModule* m) {
    if (!m || !m->debugBuilder || m->debugFinalized) return;
    m->debugBuilder->finalize();
    m->debugFinalized = true;
}

static bool verifyModuleForRt(VyxRtModule* m, const char* phase) {
    if (!m || !m->mod) return false;
    finalizeDebugInfoForRt(m);
    std::string errs;
    llvm::raw_string_ostream os(errs);
    if (llvm::verifyModule(*m->mod, &os)) {
        os.flush();
        if (const char* dumpPath = std::getenv("VYX_RT_DUMP_BAD_IR")) {
            if (dumpPath[0] != '\0' && dumpPath[0] != '0') {
                std::error_code ec;
                llvm::raw_fd_ostream out(dumpPath, ec, llvm::sys::fs::OF_Text);
                if (!ec) {
                    m->mod->print(out, nullptr);
                }
            }
        }
        m->lastError = std::string("module verification failed at ") +
                       (phase ? phase : "unknown") + ":\n" + errs;
        return false;
    }
    return true;
}

static int32_t normalizedOptimizationLevelFromInt(int32_t level) {
    switch (level) {
        case 0:
        case 1:
        case 3:
        case 4:
        case 5:
            return level;
        default:
            return 2;
    }
}

static llvm::OptimizationLevel optimizationLevelFromInt(int32_t level) {
    switch (normalizedOptimizationLevelFromInt(level)) {
        case 0:  return llvm::OptimizationLevel::O0;
        case 1:  return llvm::OptimizationLevel::O1;
        case 3:  return llvm::OptimizationLevel::O3;
        case 4:  return llvm::OptimizationLevel::Os;
        case 5:  return llvm::OptimizationLevel::Oz;
        default: return llvm::OptimizationLevel::O2;
    }
}

static llvm::CodeGenOptLevel codeGenOptimizationLevelFromInt(int32_t level) {
    switch (normalizedOptimizationLevelFromInt(level)) {
        case 0:  return llvm::CodeGenOptLevel::None;
        case 1:  return llvm::CodeGenOptLevel::Less;
        case 3:  return llvm::CodeGenOptLevel::Aggressive;
        default: return llvm::CodeGenOptLevel::Default;
    }
}

static bool targetTripleMatchesHostObjectFormat(const llvm::Triple& triple) {
    const llvm::Triple host(llvm::sys::getDefaultTargetTriple());
    return triple.getArch() == host.getArch() &&
           triple.getSubArch() == host.getSubArch() &&
           triple.getObjectFormat() == host.getObjectFormat();
}

static std::unique_ptr<llvm::TargetMachine>
createModuleTargetMachine(VyxRtModule* m, int32_t opt_level = 2) {
    if (!m || !m->mod) return nullptr;
    std::string tripleText = m->mod->getTargetTriple().str();
    if (tripleText.empty()) tripleText = m->triple;
    if (tripleText.empty()) tripleText = llvm::sys::getDefaultTargetTriple();
    tripleText = llvm::Triple::normalize(tripleText);
    llvm::Triple triple(tripleText);
    m->triple = triple.getTriple();
    std::string err;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, err);
    if (!target) {
        m->lastError = err.empty() ? "lookupTarget failed" : err;
        return nullptr;
    }

    // Let each non-host backend select its own default CPU. Several LLVM
    // backends (notably NVPTX) do not define a processor named "generic".
    std::string requestedCpu = m->requestedTargetCpu;
    // LLVM 22 recognizes CUDA kernels through the ptx_kernel calling
    // convention. Upgrade legacy nvvm.annotations before either the
    // optimization pipeline or final emission inspects the module.
    if (triple.isNVPTX())
        llvm::UpgradeNVVMAnnotations(*m->mod);
    m->targetFeatures.clear();
    if (!requestedCpu.empty() && triple.isNVPTX()) {
        m->targetCpu = requestedCpu;
        requestedCpu.clear();
    }
    if (targetTripleMatchesHostObjectFormat(triple)) {
        m->targetCpu = llvm::sys::getHostCPUName().str();
        llvm::SubtargetFeatures features;
        auto hostFeatures = llvm::sys::getHostCPUFeatures();
        for (auto& f : hostFeatures) {
            features.AddFeature(f.first(), f.second);
        }
        m->targetFeatures = features.getString();
    }

    if (!requestedCpu.empty()) {
        m->lastError = "target CPU `" + m->requestedTargetCpu +
                       "` is only supported for NVPTX targets";
        return nullptr;
    }
    if (triple.isNVPTX() && !m->targetCpu.empty() &&
        !triple.getArchName().empty() && m->targetCpu.find("sm_") != 0) {
        m->targetCpu = "sm_" + m->targetCpu;
    }

    llvm::TargetOptions opt;
    std::optional<llvm::Reloc::Model> rm;
    if (m->relocationModel == 1) {
        rm = llvm::Reloc::Static;
    } else if (m->relocationModel == 2) {
        rm = llvm::Reloc::PIC_;
    }
    auto tm = std::unique_ptr<llvm::TargetMachine>(
        target->createTargetMachine(triple, m->targetCpu, m->targetFeatures,
                                    opt, rm, std::nullopt,
                                    codeGenOptimizationLevelFromInt(opt_level)));
    if (!tm) {
        m->lastError = "createTargetMachine failed";
        return nullptr;
    }
    m->mod->setTargetTriple(triple);
    m->mod->setDataLayout(tm->createDataLayout());
    return tm;
}

// LLVM cannot optimize Vyx's runtime allocations while these calls stay opaque
// external functions: capture tracking, alias analysis, dead-allocation
// elimination and heap-to-stack are all blind to them. Rather than hand-roll a
// residual escape analysis as a per-shape use whitelist, we declare the
// allocators' real semantics once (an honest description of the existing ABI
// contract, not a transform tied to any one caller) and then prove non-escape
// with LLVM's own capture tracking. An allocation and its deallocator share an
// alloc-family so the pairing is generic; the freed pointer is marked
// non-capturing so the paired free reads as a non-escaping use.
constexpr uint64_t kVyxStackPromotionMaxBytes = 256;

// Hard cap on the promotion fixpoint. Real inputs converge in one or two passes
// (Box<T> exposes at most its wrapper then its payload); the cap only bounds
// pathological modules so the optimizer can never spin or grow without limit.
constexpr int kVyxStackPromotionMaxIterations = 8;

namespace {
// Recognises Vyx's custom bump/arena allocators by name. We deliberately do
// NOT describe them to LLVM with allockind/allocsize/noalias: those advertise a
// standard, stateless allocator and invite LLVM's own allocation transforms
// (dead-alloc elimination, allocation merging, alias assumptions) to fire on a
// bump allocator whose thread-local cursor state those transforms do not model
// — which miscompiles under -O2. Keeping allocator knowledge local to this one
// stack-promotion proof is both safer and, per the pass's charter, not a
// per-shape use whitelist: it only says which functions are allocators.
struct VyxAllocKind {
    unsigned sizeArg;   // operand carrying the byte size
    int alignArg;       // operand carrying alignment, or -1 if unaligned
    const char* family; // pairs an allocator with its deallocator
};
} // namespace

static const VyxAllocKind* vyxAllocatorKind(const llvm::Function* callee) {
    static const VyxAllocKind kClass{0, -1, "vyx_class"};
    static const VyxAllocKind kAligned{0, 1, "vyx_aligned"};
    if (callee == nullptr) return nullptr;
    const llvm::StringRef name = callee->getName();
    if (name == "vyx_class_alloc_abi") return &kClass;
    if (name == "vyx_aligned_alloc_abi") return &kAligned;
    return nullptr;
}

static bool vyxIsFamilyFree(const llvm::Function* callee, llvm::StringRef family) {
    if (callee == nullptr) return false;
    const llvm::StringRef name = callee->getName();
    if (family == "vyx_class") return name == "vyx_class_free_abi";
    if (family == "vyx_aligned") return name == "vyx_aligned_free_abi";
    return false;
}

// Non-escaping allocation proof by a conservative use walk. Every use of the
// allocation (and pointers derived from it through casts/GEPs) must be one we
// can prove keeps it inside this function: a load or store *through* it, a null
// test, a memset clear, or a matching-family free. ANYTHING else — storing the
// pointer itself as a value, an insertvalue into an aggregate, a return, or a
// call we don't recognise — fails closed as an escape.
//
// This is deliberately more conservative than LLVM's PointerMayBeCaptured.
// Capture tracking under-approximates escapes that flow a pointer through an
// aggregate (e.g. insertvalue into a dyn-trait handle that a container then
// takes ownership of); trusting it stack-promoted an escaping polymorphic
// object and crashed under -O2. Failing closed on any unrecognised use is the
// safe direction for a heap-to-stack transform. PHI/select merges are treated
// as escapes too — the shapes we promote (Box payloads, Ref/Box class wrappers)
// never merge their allocation pointer, so nothing is lost by refusing them.
static bool collectStackPromotableAllocation(
    llvm::CallInst& call,
    std::vector<llvm::CallInst*>& frees,
    std::vector<llvm::CallInst*>& deps,
    llvm::Align& outAlign) {
    const llvm::Function* callee = call.getCalledFunction();
    const VyxAllocKind* info = vyxAllocatorKind(callee);
    if (info == nullptr) return false;
    if (!call.getType()->isPointerTy()) return false;
    if (info->sizeArg >= call.arg_size()) return false;

    // Fixed, small size: the stack slot must be a compile-time constant.
    const auto* sizeConst =
        llvm::dyn_cast<llvm::ConstantInt>(call.getArgOperand(info->sizeArg));
    if (sizeConst == nullptr || sizeConst->isZero()
        || sizeConst->getZExtValue() > kVyxStackPromotionMaxBytes) {
        return false;
    }

    // Alignment from the allocator's alignment operand when it has one.
    llvm::Align alignment(16);
    if (info->alignArg >= 0
        && static_cast<unsigned>(info->alignArg) < call.arg_size()) {
        const auto* alignConst = llvm::dyn_cast<llvm::ConstantInt>(
            call.getArgOperand(static_cast<unsigned>(info->alignArg)));
        if (alignConst == nullptr || alignConst->isZero()
            || !alignConst->getValue().isPowerOf2()
            || alignConst->getZExtValue() > 4096u) {
            return false;
        }
        alignment = llvm::Align(alignConst->getZExtValue());
    }
    outAlign = alignment;

    // Conservative non-escape proof + paired-free collection.
    const llvm::StringRef family = info->family;
    llvm::SmallPtrSet<const llvm::Value*, 16> seen;
    std::vector<const llvm::Value*> pending;
    seen.insert(&call);
    pending.push_back(&call);
    while (!pending.empty()) {
        const llvm::Value* value = pending.back();
        pending.pop_back();
        for (const llvm::User* user : value->users()) {
            const auto* inst = llvm::dyn_cast<llvm::Instruction>(user);
            if (inst == nullptr) return false;
            if (const auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(inst)) {
                if (gep->getPointerOperand() != value) return false;
                if (seen.insert(gep).second) pending.push_back(gep);
                continue;
            }
            if (const auto* bitcast = llvm::dyn_cast<llvm::BitCastInst>(inst)) {
                if (bitcast->getOperand(0) != value) return false;
                if (seen.insert(bitcast).second) pending.push_back(bitcast);
                continue;
            }
            if (const auto* addr = llvm::dyn_cast<llvm::AddrSpaceCastInst>(inst)) {
                if (addr->getOperand(0) != value) return false;
                if (seen.insert(addr).second) pending.push_back(addr);
                continue;
            }
            if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(inst)) {
                if (load->getPointerOperand() != value) return false;
                continue;
            }
            if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(inst)) {
                // Writing *through* the allocation is fine; storing the
                // allocation pointer itself as a value into other storage
                // escapes.
                if (store->getPointerOperand() != value) return false;
                continue;
            }
            if (const auto* cmp = llvm::dyn_cast<llvm::ICmpInst>(inst)) {
                const llvm::Value* other = cmp->getOperand(0) == value
                    ? cmp->getOperand(1)
                    : cmp->getOperand(0);
                if (!llvm::isa<llvm::ConstantPointerNull>(other)) return false;
                continue;
            }
            // A PHI/select may merge this allocation with null or with ANOTHER
            // allocation of the same family (the clone success/empty-handle
            // split, which -O2 collapses into a PHI). Any other incoming is an
            // outside pointer and fails closed. A same-family incoming is
            // recorded as a dependency so the pass promotes the whole merged
            // group together or not at all — otherwise a shared free could end
            // up freeing a stack slot.
            if (const auto* phi = llvm::dyn_cast<llvm::PHINode>(inst)) {
                for (const llvm::Value* in : phi->incoming_values()) {
                    if (in == value || llvm::isa<llvm::ConstantPointerNull>(in)
                        || seen.count(in)) {
                        continue;
                    }
                    auto* inCall = llvm::dyn_cast<llvm::CallInst>(in);
                    if (inCall != nullptr
                        && vyxAllocatorKind(inCall->getCalledFunction()) == info) {
                        deps.push_back(const_cast<llvm::CallInst*>(inCall));
                        continue;
                    }
                    return false;
                }
                if (seen.insert(phi).second) pending.push_back(phi);
                continue;
            }
            if (const auto* sel = llvm::dyn_cast<llvm::SelectInst>(inst)) {
                for (const llvm::Value* op :
                     {sel->getTrueValue(), sel->getFalseValue()}) {
                    if (op == value || llvm::isa<llvm::ConstantPointerNull>(op)
                        || seen.count(op)) {
                        continue;
                    }
                    auto* opCall = llvm::dyn_cast<llvm::CallInst>(op);
                    if (opCall != nullptr
                        && vyxAllocatorKind(opCall->getCalledFunction()) == info) {
                        deps.push_back(const_cast<llvm::CallInst*>(opCall));
                        continue;
                    }
                    return false;
                }
                if (seen.insert(sel).second) pending.push_back(sel);
                continue;
            }
            if (const auto* callUse = llvm::dyn_cast<llvm::CallInst>(inst)) {
                const llvm::Function* target = callUse->getCalledFunction();
                // A matching-family free of any pointer we've proven derives
                // from this allocation (including through the merges above).
                if (vyxIsFamilyFree(target, family) && callUse->arg_size() >= 1
                    && seen.count(
                           callUse->getArgOperand(0)->stripPointerCasts())) {
                    frees.push_back(const_cast<llvm::CallInst*>(callUse));
                    continue;
                }
                if (target != nullptr
                    && target->getName().starts_with("llvm.memset.")
                    && callUse->arg_size() >= 1
                    && callUse->getArgOperand(0) == value) {
                    continue;
                }
                return false;
            }
            return false;
        }
    }
    return true;
}

static void promoteVyxSmallAllocation(llvm::CallInst& allocation,
                                       llvm::Align alignment) {
    llvm::Function* function = allocation.getFunction();
    if (function == nullptr || function->empty()) return;
    llvm::IRBuilder<> builder(&*function->getEntryBlock().getFirstInsertionPt());
    llvm::AllocaInst* slot = builder.CreateAlloca(
        llvm::Type::getInt8Ty(allocation.getContext()),
        allocation.getArgOperand(0),
        "vyx.promoted.alloc");
    // The aligned Box ABI carries an explicit power-of-two alignment, preserved
    // here so the stack replacement stays ABI-correct. Paired frees are erased
    // by the caller — once each, after group consistency is settled.
    slot->setAlignment(alignment);
    allocation.replaceAllUsesWith(slot);
    allocation.eraseFromParent();
}

static bool promoteVyxNonEscapingSmallAllocations(llvm::Module& module) {
    struct Candidate {
        llvm::CallInst* allocation;
        std::vector<llvm::CallInst*> frees;
        std::vector<llvm::CallInst*> deps;
        llvm::Align alignment;
    };
    std::vector<Candidate> candidates;

    for (llvm::Function& function : module) {
        if (function.isDeclaration()) continue;
        for (llvm::BasicBlock& block : function) {
            for (llvm::Instruction& inst : block) {
                auto* call = llvm::dyn_cast<llvm::CallInst>(&inst);
                if (call == nullptr) continue;
                std::vector<llvm::CallInst*> frees;
                std::vector<llvm::CallInst*> deps;
                llvm::Align alignment(16);
                if (collectStackPromotableAllocation(*call, frees, deps,
                                                     alignment)) {
                    candidates.push_back(
                        {call, std::move(frees), std::move(deps), alignment});
                }
            }
        }
    }
    if (candidates.empty()) return false;

    // Group consistency. A candidate whose pointer is merged (PHI/select) with
    // another same-family allocation may only be promoted if that other
    // allocation is itself a candidate; otherwise the shared free would end up
    // freeing a stack slot. Drop incomplete groups to a fixed point, since one
    // drop can invalidate another candidate that depended on it.
    llvm::SmallPtrSet<llvm::CallInst*, 32> live;
    for (const Candidate& c : candidates) live.insert(c.allocation);
    bool changed = true;
    while (changed) {
        changed = false;
        for (const Candidate& c : candidates) {
            if (!live.count(c.allocation)) continue;
            for (llvm::CallInst* dep : c.deps) {
                if (!live.count(dep)) {
                    live.erase(c.allocation);
                    changed = true;
                    break;
                }
            }
        }
    }

    // Erase each surviving group's frees exactly once (a merged free is
    // collected by every allocation in the group), then replace the
    // allocations with stack slots.
    llvm::SmallPtrSet<llvm::CallInst*, 32> freesToErase;
    for (const Candidate& c : candidates) {
        if (!live.count(c.allocation)) continue;
        for (llvm::CallInst* f : c.frees) freesToErase.insert(f);
    }
    for (llvm::CallInst* f : freesToErase) f->eraseFromParent();

    bool promotedAny = false;
    for (Candidate& c : candidates) {
        if (!live.count(c.allocation)) continue;
        promoteVyxSmallAllocation(*c.allocation, c.alignment);
        promotedAny = true;
    }
    return promotedAny;
}

static void stripUnusedNvptxDeclarations(llvm::Module& mod) {
    // Device modules receive the shared host runtime declarations even when
    // the Vyx program never calls them. Remove only declarations with no
    // uses; defined kernels and their actual references stay untouched.
    llvm::LoopAnalysisManager LAM;
    llvm::FunctionAnalysisManager FAM;
    llvm::CGSCCAnalysisManager CGAM;
    llvm::ModuleAnalysisManager MAM;
    llvm::PassBuilder PB;
    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
    llvm::ModulePassManager cleanup;
    cleanup.addPass(llvm::GlobalDCEPass());
    cleanup.addPass(llvm::StripDeadPrototypesPass());
    cleanup.run(mod, MAM);
}

static bool optimizeModuleForRt(VyxRtModule* m, int32_t opt_level) {
    const char* tracePath = std::getenv("VYX_RT_TRACE_OPT");
    auto traceLine = [&](const char* label, uint64_t before, uint64_t after) {
        if (!tracePath || tracePath[0] == '\0' || tracePath[0] == '0') return;
        std::error_code ec;
        llvm::raw_fd_ostream out(tracePath, ec, llvm::sys::fs::OF_Append);
        if (!ec) out << label << " level=" << opt_level
                     << " before=" << before << " after=" << after << "\n";
    };
    if (!m || !m->mod) {
        traceLine("missing", 0, 0);
        return false;
    }
    if (llvm::Triple(m->mod->getTargetTriple()).isNVPTX()) {
        stripUnusedNvptxDeclarations(*m->mod);
    }
    const auto beforeInstructions = m->mod->getInstructionCount();
    if (!verifyModuleForRt(m, "before-optimize")) {
        traceLine("verify-failed", beforeInstructions, beforeInstructions);
        return false;
    }

    const int32_t normalizedLevel = normalizedOptimizationLevelFromInt(opt_level);
    auto optLevel = optimizationLevelFromInt(normalizedLevel);
    if (optLevel == llvm::OptimizationLevel::O0) {
        if (!verifyModuleForRt(m, "after-optimize-O0-noop")) {
            return false;
        }
        m->optimized = true;
        m->optimizedLevel = normalizedLevel;
        return true;
    }

    auto tm = createModuleTargetMachine(m, normalizedLevel);
    if (!tm) {
        return false;
    }

    llvm::LoopAnalysisManager LAM;
    llvm::FunctionAnalysisManager FAM;
    llvm::CGSCCAnalysisManager CGAM;
    llvm::ModuleAnalysisManager MAM;

    llvm::PassBuilder PB(tm.get());
    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

    auto MPM = PB.buildPerModuleDefaultPipeline(optLevel);
    MPM.run(*m->mod, MAM);
    traceLine("ran", beforeInstructions, m->mod->getInstructionCount());

    // Wrapper promotion can expose a second allocation layer after SROA. In
    // particular, Box<T> first sheds its temporary class wrapper and only then
    // presents the aligned payload allocation as a direct non-escaping pair.
    // Iterate the strict promotion proof and cleanup pipeline to a fixed point.
    // Real inputs converge in one or two passes; the hard iteration cap is a
    // belt-and-braces guard so a pathological module can never spin here.
    for (int pass = 0;
         pass < kVyxStackPromotionMaxIterations
             && promoteVyxNonEscapingSmallAllocations(*m->mod);
         ++pass) {
        auto cleanupMPM = PB.buildPerModuleDefaultPipeline(optLevel);
        cleanupMPM.run(*m->mod, MAM);
    }

    if (!verifyModuleForRt(m, "after-optimize")) {
        return false;
    }
    m->optimized = true;
    m->optimizedLevel = normalizedLevel;
    return true;
}

static bool prepareModuleForJit(VyxRtModule* m, bool preserveExternalDefinitions) {
    if (!verifyModuleForRt(m, "before-jit-cleanup")) {
        return false;
    }

    llvm::LoopAnalysisManager LAM;
    llvm::FunctionAnalysisManager FAM;
    llvm::CGSCCAnalysisManager CGAM;
    llvm::ModuleAnalysisManager MAM;

    llvm::PassBuilder PB;
    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

    llvm::ModulePassManager MPM;
    MPM.addPass(llvm::InternalizePass([preserveExternalDefinitions](const llvm::GlobalValue& gv) {
        if (gv.getName() == "main") return true;
        return preserveExternalDefinitions
            && !gv.isDeclaration()
            && gv.hasExternalLinkage();
    }));
    MPM.addPass(llvm::GlobalDCEPass());
    MPM.addPass(llvm::StripDeadPrototypesPass());
    MPM.run(*m->mod, MAM);

    return verifyModuleForRt(m, "after-jit-cleanup");
}

static llvm::Function* addJitEntryWrapper(VyxRtModule* m) {
    if (!m || !m->mod || !m->ctx) return nullptr;
    auto* mainFn = m->mod->getFunction("main");
    if (!mainFn || mainFn->isDeclaration()) {
        m->lastError = "JIT module has no defined main entry";
        return nullptr;
    }
    auto* mainTy = mainFn->getFunctionType();
    if (!mainTy || !mainTy->getReturnType()->isIntegerTy(32)
        || mainTy->getNumParams() != 2
        || !mainTy->getParamType(0)->isIntegerTy(32)
        || !mainTy->getParamType(1)->isPointerTy()) {
        m->lastError = "JIT main must use i32 main(i32, ptr) ABI";
        return nullptr;
    }

    auto* entryTy = llvm::FunctionType::get(llvm::Type::getInt32Ty(*m->ctx), false);
    auto* entryFn = llvm::Function::Create(entryTy,
                                            llvm::GlobalValue::ExternalLinkage,
                                            "__vyx_jit_entry",
                                            m->mod.get());
    auto* entryBlock = llvm::BasicBlock::Create(*m->ctx, "entry", entryFn);
    llvm::IRBuilder<> builder(entryBlock);
    auto* argvTy = llvm::cast<llvm::PointerType>(mainTy->getParamType(1));
    auto* result = builder.CreateCall(mainFn,
                                      {llvm::ConstantInt::get(mainTy->getParamType(0), 0),
                                       llvm::ConstantPointerNull::get(argvTy)});
    builder.CreateRet(result);
    return entryFn;
}

static std::string stringifyError(llvm::Error err) {
    if (!err) return {};
    std::string text;
    llvm::raw_string_ostream os(text);
    llvm::logAllUnhandledErrors(std::move(err), os, "");
    os.flush();
    return text.empty() ? "LLVM error" : text;
}

static std::vector<std::string> splitLinkArgs(const char* text, uint64_t len) {
    std::vector<std::string> out;
    if (!text || len == 0) return out;
    std::string cur;
    bool inQuote = false;
    for (uint64_t i = 0; i < len && text[i]; ++i) {
        const char c = text[i];
        if (c == '\\' && i + 1 < len && text[i + 1] == '"') {
            cur.push_back('"');
            ++i;
            continue;
        }
        if (c == '"') {
            inQuote = !inQuote;
            continue;
        }
        if (!inQuote && std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
            continue;
        }
        cur.push_back(c);
    }
    if (!cur.empty()) {
        out.push_back(cur);
    }
    return out;
}

static bool pathLooksExplicit(const std::string& name) {
    return name.find('/') != std::string::npos
        || name.find('\\') != std::string::npos
        || name.find(':') != std::string::npos;
}

static bool hasDynamicLibrarySuffix(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto endsWith = [&](const char* suffix) {
        const std::size_t n = std::strlen(suffix);
        return lower.size() >= n && lower.compare(lower.size() - n, n, suffix) == 0;
    };
    return endsWith(".dll") || endsWith(".so") || endsWith(".dylib");
}

static bool hasObjectFileSuffix(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto endsWith = [&](const char* suffix) {
        const std::size_t n = std::strlen(suffix);
        return lower.size() >= n && lower.compare(lower.size() - n, n, suffix) == 0;
    };
    return endsWith(".obj") || endsWith(".o");
}

static std::vector<std::string> objectFilesForLinkArgs(const char* link_args,
                                                       uint64_t link_args_len) {
    std::vector<std::string> objects;
    if (!link_args || link_args_len == 0) return objects;
    const std::vector<std::string> args = splitLinkArgs(link_args, link_args_len);
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--link-obj" && i + 1 < args.size()) {
            objects.push_back(args[++i]);
        } else if (hasObjectFileSuffix(a)) {
            objects.push_back(a);
        }
    }
    return objects;
}

static std::vector<std::string> dynamicLibraryNamesForLinkName(const std::string& name) {
    std::vector<std::string> names;
    if (name.empty()) return names;
    if (hasDynamicLibrarySuffix(name) || pathLooksExplicit(name)) {
        names.push_back(name);
        return names;
    }
#ifdef _WIN32
    names.push_back(name + ".dll");
    names.push_back("lib" + name + ".dll");
#elif defined(__APPLE__)
    names.push_back("lib" + name + ".dylib");
    names.push_back(name + ".dylib");
    names.push_back("lib" + name + ".so");
#else
    names.push_back("lib" + name + ".so");
    names.push_back(name + ".so");
#endif
    names.push_back(name);
    return names;
}

static bool tryLoadDynamicLibraryPath(const std::string& path,
                                      std::string& err) {
    err.clear();
    return !llvm::sys::DynamicLibrary::LoadLibraryPermanently(path.c_str(), &err);
}

static bool loadJitLinkLibraries(VyxRtModule* m,
                                 const char* link_args,
                                 uint64_t link_args_len) {
    if (!m || !link_args || link_args_len == 0) return true;
    const std::vector<std::string> args = splitLinkArgs(link_args, link_args_len);
    std::vector<std::filesystem::path> searchPaths;
    searchPaths.emplace_back(".");
    std::vector<std::string> libs;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if ((a == "-L" || a == "--lib-path") && i + 1 < args.size()) {
            searchPaths.emplace_back(args[++i]);
        } else if (a.rfind("-L", 0) == 0 && a.size() > 2) {
            searchPaths.emplace_back(a.substr(2));
        } else if ((a == "-l" || a == "--link") && i + 1 < args.size()) {
            libs.push_back(args[++i]);
        } else if (a.rfind("-l", 0) == 0 && a.size() > 2) {
            libs.push_back(a.substr(2));
        }
    }

    for (const std::string& lib : libs) {
        std::vector<std::string> candidates;
        for (const auto& name : dynamicLibraryNamesForLinkName(lib)) {
            if (pathLooksExplicit(name)) {
                candidates.push_back(name);
            } else {
                for (const auto& dir : searchPaths) {
                    candidates.push_back((dir / name).string());
                }
                candidates.push_back(name);
            }
        }
        std::string lastErr;
        bool loaded = false;
        for (const auto& candidate : candidates) {
            std::error_code ec;
            const bool explicitPath = pathLooksExplicit(candidate);
            if (explicitPath && !std::filesystem::exists(candidate, ec)) {
                continue;
            }
            if (tryLoadDynamicLibraryPath(candidate, lastErr)) {
                loaded = true;
                lastErr.clear();
                break;
            }
        }
        if (!loaded) {
            if (lastErr.empty()) {
                lastErr = "no dynamic library candidate was found";
            }
            m->lastError = "JIT failed to load library '" + lib + "': " + lastErr;
            return false;
        }
    }
    return true;
}

} /* anonymous namespace */

/* =================================================================== */
/*   1. Lifetime & module                                              */
/* =================================================================== */

extern "C" VYX_API int32_t vyx_rt_init(void) {
    /* Register every backend compiled into this LLVM SDK. Targets.def keeps
     * this in sync with the SDK instead of maintaining an architecture list. */
    llvm::InitializeAllTargetInfos();
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmParsers();
    llvm::InitializeAllAsmPrinters();
    return 0;
}

extern "C" VYX_API void vyx_rt_shutdown(void) {
    /* No global state owned here; per-module shutdown happens in free. */
}

extern "C" VYX_API void* vyx_rt_module_new(const char* name,    uint64_t name_len,
                                           const char* triple,  uint64_t triple_len) {
    auto m = new VyxRtModule{};
    m->ctx = std::make_unique<llvm::LLVMContext>();
    m->ctx->setDiscardValueNames(true);
    m->name = asStr(name, name_len);
    if (m->name.empty()) m->name = "vyx_module";
    m->mod = std::make_unique<llvm::Module>(m->name, *m->ctx);
    m->triple = asStr(triple, triple_len);
    if (m->triple.empty()) m->triple = llvm::sys::getDefaultTargetTriple();
    m->mod->setTargetTriple(llvm::Triple(m->triple));
    (void)createModuleTargetMachine(m);
    return static_cast<void*>(m);
}

extern "C" VYX_API void vyx_rt_module_free(void* h) {
    if (!h) return;
    /* Builder must die before module/context per LLVM ownership rules. */
    auto* m = asMod(h);
    finalizeDebugInfoForRt(m);
    m->debugBuilder.reset();
    m->builder.reset();
    m->mod.reset();
    m->ctx.reset();
    delete m;
}

extern "C" VYX_API int32_t vyx_rt_set_target(void* h, const char* triple, uint64_t len) {
    if (!h) return -1;
    auto* m = asMod(h);
    m->triple = llvm::Triple::normalize(asStr(triple, len));
    m->mod->setTargetTriple(llvm::Triple(m->triple));
    return 0;
}

extern "C" VYX_API int32_t vyx_rt_set_target_cpu(void* h, const char* cpu, uint64_t len) {
    if (!h) return -1;
    auto* m = asMod(h);
    m->requestedTargetCpu = asStr(cpu, len);
    m->targetCpu = m->requestedTargetCpu;
    return 0;
}

extern "C" VYX_API int32_t vyx_rt_set_relocation_model(void* h, int32_t model) {
    if (!h || model < 0 || model > 2) return -1;
    asMod(h)->relocationModel = model;
    return 0;
}

extern "C" VYX_API int32_t vyx_rt_set_data_layout(void* h, const char* dl, uint64_t len) {
    if (!h) return -1;
    asMod(h)->mod->setDataLayout(asStr(dl, len));
    return 0;
}

extern "C" VYX_API int32_t vyx_rt_pointer_index_bits(void* h) {
    if (!h) return 0;
    return static_cast<int32_t>(
        asMod(h)->mod->getDataLayout().getIndexSizeInBits(/*AddressSpace=*/0));
}

/* =================================================================== */
/*   2. Error reporting                                                */
/* =================================================================== */

extern "C" VYX_API uint64_t vyx_rt_get_last_error(void* h, char* buf, uint64_t cap) {
    if (!h) return 0;
    auto& s = asMod(h)->lastError;
    auto required = static_cast<uint64_t>(s.size());
    if (buf && cap) {
        auto copy_n = required < cap ? required : cap;
        std::memcpy(buf, s.data(), static_cast<size_t>(copy_n));
        if (copy_n < cap) buf[copy_n] = '\0';
    }
    return required;
}

namespace {

llvm::json::Object dciNativeTypeFacts(llvm::Type* type, const llvm::DataLayout& layout,
                                     unsigned depth = 0) {
    llvm::json::Object result;
    std::string spelling;
    llvm::raw_string_ostream stream(spelling);
    type->print(stream);
    stream.flush();
    result["llvm_type"] = spelling;
    result["kind"] = "unsupported";
    result["bits"] = int64_t(0);
    result["size"] = int64_t(0);
    result["alignment"] = int64_t(0);
    if (type->isVoidTy()) {
        result["kind"] = "void";
        return result;
    }
    if (!type->isSized() || layout.getTypeAllocSize(type).isScalable()) return result;
    result["size"] = int64_t(layout.getTypeAllocSize(type).getFixedValue());
    result["alignment"] = int64_t(layout.getABITypeAlign(type).value());
    if (type->isIntegerTy()) {
        result["kind"] = "integer";
        result["bits"] = int64_t(type->getIntegerBitWidth());
    } else if (type->isFloatTy() || type->isDoubleTy()) {
        result["kind"] = "float";
        result["bits"] = int64_t(type->isFloatTy() ? 32 : 64);
    } else if (auto* pointer = llvm::dyn_cast<llvm::PointerType>(type)) {
        result["kind"] = "pointer";
        result["bits"] = int64_t(layout.getPointerSizeInBits(pointer->getAddressSpace()));
        result["address_space"] = int64_t(pointer->getAddressSpace());
    } else if (auto* structure = llvm::dyn_cast<llvm::StructType>(type)) {
        if (depth >= 32 || structure->isOpaque()) return result;
        result["kind"] = "struct";
        result["packed"] = structure->isPacked();
        llvm::json::Array fields;
        const auto* positions = layout.getStructLayout(structure);
        for (unsigned i = 0; i < structure->getNumElements(); ++i) {
            auto field = dciNativeTypeFacts(structure->getElementType(i), layout, depth + 1);
            field["offset"] = int64_t(positions->getElementOffset(i));
            fields.push_back(std::move(field));
        }
        result["fields"] = std::move(fields);
    }
    return result;
}

// Absence of a nounwind attribute is not evidence of propagation. Prove simple
// defined call graphs, but reject unknown external, indirect and recursive edges.
bool dciNativeNoUnwind(const llvm::Function& function,
                      llvm::SmallPtrSetImpl<const llvm::Function*>& visiting,
                      unsigned depth = 0) {
    // Native lowering can put nounwind on a definition/call before its
    // external callees have been proved. Inspect defined bodies regardless of
    // those attributes. Only LLVM intrinsic semantics establish a declaration
    // as no-unwind here; an arbitrary foreign declaration is not a proof.
    if (function.isDeclaration()) return function.isIntrinsic() && function.doesNotThrow();
    if (depth >= 128 || !visiting.insert(&function).second) return false;
    bool proven = true;
    for (const auto& block : function) {
        for (const auto& instruction : block) {
            if (const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction)) {
                const auto* callee = call->getCalledFunction();
                if (!callee || !dciNativeNoUnwind(*callee, visiting, depth + 1)) proven = false;
            } else if (instruction.mayThrow() || llvm::isa<llvm::ResumeInst>(instruction)) {
                proven = false;
            }
        }
    }
    visiting.erase(&function);
    return proven;
}

llvm::json::Array dciNativeAttributes(llvm::AttributeSet attributes) {
    llvm::json::Array result;
    for (const auto& attribute : attributes) result.push_back(attribute.getAsString());
    return result;
}

} // namespace

extern "C" VYX_API char* vyx_rt_dci_export_function_json(void* h, const char* name,
                                                          uint64_t len) {
    if (!h || !name) return nullptr;
    auto* context = asMod(h);
    auto* function = context->mod->getFunction(asStr(name, len));
    if (!function || function->isDeclaration() || function->hasLocalLinkage()) {
        context->lastError = "DCI export requires a defined externally addressable native function";
        return nullptr;
    }
    if (function->getCallingConv() != llvm::CallingConv::C) {
        context->lastError = "DCI export does not support this native calling convention";
        return nullptr;
    }
    const auto& layout = context->mod->getDataLayout();
    const llvm::Triple triple(context->mod->getTargetTriple());
    if (layout.isDefault() || triple.getArch() == llvm::Triple::UnknownArch) {
        context->lastError = "DCI export requires a resolved native target and data layout";
        return nullptr;
    }
    llvm::json::Object facts;
    facts["link_name"] = function->getName().str();
    facts["defined"] = true;
    facts["external"] = true;
    facts["calling_convention"] = "cdecl";
    facts["variadic"] = function->isVarArg();
    auto returned = dciNativeTypeFacts(function->getReturnType(), layout);
    returned["attributes"] = dciNativeAttributes(function->getAttributes().getRetAttrs());
    facts["return"] = std::move(returned);
    llvm::json::Array parameters;
    unsigned index = 0;
    for (const auto& parameter : function->args()) {
        auto info = dciNativeTypeFacts(parameter.getType(), layout);
        info["attributes"] = dciNativeAttributes(function->getAttributes().getParamAttrs(index));
        if (auto* type = function->getParamByValType(index)) info["byval_type"] = dciNativeTypeFacts(type, layout);
        if (auto* type = function->getParamStructRetType(index)) info["sret_type"] = dciNativeTypeFacts(type, layout);
        parameters.push_back(std::move(info));
        ++index;
    }
    facts["parameters"] = std::move(parameters);
    llvm::SmallPtrSet<const llvm::Function*, 16> visiting;
    facts["unwind"] = dciNativeNoUnwind(*function, visiting) ? "no_unwind" : "may_unwind";
    std::string abi;
    if (triple.isWindowsMSVCEnvironment()) abi = "msvc";
    else if (triple.isAndroid()) abi = "android";
    else if (triple.isOSDarwin()) abi = "darwin";
    else if (triple.isOSLinux() && triple.isGNUEnvironment()) abi = "gnu";
    else {
        context->lastError = "DCI export does not support this target ABI";
        return nullptr;
    }
    facts["target"] = llvm::json::Object{
        {"triple", triple.str()}, {"architecture", triple.getArchName().str()},
        {"pointer_width", int64_t(layout.getPointerSizeInBits())},
        {"endianness", layout.isLittleEndian() ? "little" : "big"},
        {"abi", abi}, {"abi_family", triple.isWindowsMSVCEnvironment() ? "msvc" : "itanium"}};
    std::string text;
    llvm::raw_string_ostream output(text);
    output << llvm::json::Value(std::move(facts));
    output.flush();
    auto* result = static_cast<char*>(std::malloc(text.size() + 1));
    if (!result) {
        context->lastError = "out of memory collecting DCI native ABI facts";
        return nullptr;
    }
    std::memcpy(result, text.c_str(), text.size() + 1);
    return result;
}

extern "C" VYX_API void vyx_rt_dci_export_free_json(void* text) {
    std::free(text);
}

extern "C" VYX_API int32_t vyx_rt_dci_export_write_file(void* h, const char* path,
                                                        uint64_t path_len,
                                                        const void* bytes, uint64_t length) {
    if (!h || !path || !bytes || length > 64 * 1024 * 1024) return 1;
    auto* context = asMod(h);
    const std::string destination = asStr(path, path_len);
    int descriptor = -1;
    llvm::SmallString<256> temporary;
    auto error = llvm::sys::fs::createUniqueFile(destination + ".tmp-%%%%%%", descriptor, temporary);
    if (error) {
        context->lastError = "cannot stage DCI contract: " + error.message();
        return 1;
    }
    {
        llvm::raw_fd_ostream output(descriptor, true);
        output.write(static_cast<const char*>(bytes), static_cast<size_t>(length));
        output.close();
        if (output.has_error()) {
            error = output.error();
            output.clear_error();
        }
    }
    if (!error) error = llvm::sys::fs::rename(temporary, destination);
    if (error) {
        llvm::sys::fs::remove(temporary);
        context->lastError = "cannot publish DCI contract: " + error.message();
        return 1;
    }
    return 0;
}

extern "C" VYX_API uint64_t vyx_rt_normalize_target_triple(const char* triple,
                                                             uint64_t triple_len,
                                                             char* buf,
                                                             uint64_t cap) {
    const std::string normalized = llvm::Triple::normalize(asStr(triple, triple_len));
    const auto required = static_cast<uint64_t>(normalized.size());
    if (buf && cap) {
        const auto copy_n = required < cap ? required : cap;
        std::memcpy(buf, normalized.data(), static_cast<size_t>(copy_n));
        if (copy_n < cap) buf[copy_n] = '\0';
    }
    return required;
}

extern "C" VYX_API uint64_t vyx_rt_default_target_triple(char* buf, uint64_t cap) {
    const std::string normalized = llvm::Triple::normalize(llvm::sys::getDefaultTargetTriple());
    const auto required = static_cast<uint64_t>(normalized.size());
    if (buf && cap) {
        const auto copy_n = required < cap ? required : cap;
        std::memcpy(buf, normalized.data(), static_cast<size_t>(copy_n));
        if (copy_n < cap) buf[copy_n] = '\0';
    }
    return required;
}

extern "C" VYX_API uint64_t vyx_rt_validate_target_triple(const char* triple,
                                                            uint64_t triple_len,
                                                            char* buf,
                                                            uint64_t cap) {
    vyx_rt_init();
    const std::string normalized = llvm::Triple::normalize(asStr(triple, triple_len));
    std::string error;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(llvm::Triple(normalized), error);
    if (target) return 0;
    if (error.empty()) error = "LLVM target lookup failed";
    const auto required = static_cast<uint64_t>(error.size());
    if (buf && cap) {
        const auto copy_n = required < cap ? required : cap;
        std::memcpy(buf, error.data(), static_cast<size_t>(copy_n));
        if (copy_n < cap) buf[copy_n] = '\0';
    }
    return required;
}

/* =================================================================== */
/*   3. Type construction                                              */
/* =================================================================== */

extern "C" VYX_API void* vyx_rt_int_type(void* h, int32_t bits) {
    VYX_RT_PROF(IntType);
    if (!h) return nullptr;
    auto& ctx = *asMod(h)->ctx;
    switch (bits) {
        case 1:  return llvm::Type::getInt1Ty(ctx);
        case 8:  return llvm::Type::getInt8Ty(ctx);
        case 16: return llvm::Type::getInt16Ty(ctx);
        case 32: return llvm::Type::getInt32Ty(ctx);
        case 64: return llvm::Type::getInt64Ty(ctx);
        default: return llvm::Type::getIntNTy(ctx, static_cast<unsigned>(bits));
    }
}

extern "C" VYX_API void* vyx_rt_ptr_ty(void* h) {
    VYX_RT_PROF(PtrType);
    if (!h) return nullptr;
    return llvm::PointerType::get(*asMod(h)->ctx, /*AddressSpace=*/0);
}
extern "C" VYX_API void* vyx_rt_void_ty(void* h) {
    VYX_RT_PROF(VoidType);
    if (!h) return nullptr;
    return llvm::Type::getVoidTy(*asMod(h)->ctx);
}

extern "C" VYX_API void* vyx_rt_fn_type(void* h,
                                        void* ret_ty,
                                        const void* const* params,
                                        uint64_t n_params,
                                        int32_t is_vararg) {
    VYX_RT_PROF(FnType);
    if (!h || !ret_ty) return nullptr;
    std::vector<llvm::Type*> ps;
    ps.reserve(static_cast<size_t>(n_params));
    for (uint64_t i = 0; i < n_params; ++i) {
        ps.push_back(static_cast<llvm::Type*>(const_cast<void*>(params ? params[i] : nullptr)));
    }
    return llvm::FunctionType::get(asTy(ret_ty), ps, is_vararg != 0);
}

extern "C" VYX_API void* vyx_rt_array_ty(void* h, void* elem_ty, uint64_t count) {
    VYX_RT_PROF(ArrayType);
    if (!h || !elem_ty) return nullptr;
    return llvm::ArrayType::get(asTy(elem_ty), count);
}
extern "C" VYX_API void* vyx_rt_vector_ty(void* h, void* elem_ty, uint64_t count) {
    VYX_RT_PROF(VectorType);
    if (!h || !elem_ty || count == 0) return nullptr;
    auto* elem = asTy(elem_ty);
    if (!elem->isIntegerTy()) return nullptr;
    return llvm::FixedVectorType::get(elem, count);
}
extern "C" VYX_API void* vyx_rt_struct_ty_named(void* h, const char* name, uint64_t name_len) {
    VYX_RT_PROF(StructTyNamed);
    if (!h) return nullptr;
    auto ident = asStr(name, name_len);
    if (auto* existing = llvm::StructType::getTypeByName(*asMod(h)->ctx, ident)) {
        return existing;
    }
    return llvm::StructType::create(*asMod(h)->ctx, ident);
}
extern "C" VYX_API int32_t vyx_rt_struct_ty_set_body(void* h, void* st,
                                                    const void* const* fields, uint64_t n,
                                                    int32_t packed) {
    VYX_RT_PROF(StructTySetBody);
    if (!h || !st) return -1;
    auto* sty = llvm::dyn_cast<llvm::StructType>(asTy(st));
    if (!sty) return -1;
    std::vector<llvm::Type*> fs;
    fs.reserve(static_cast<size_t>(n));
    for (uint64_t i = 0; i < n; ++i) {
        fs.push_back(static_cast<llvm::Type*>(const_cast<void*>(fields ? fields[i] : nullptr)));
    }
    sty->setBody(fs, packed != 0);
    return 0;
}
extern "C" VYX_API void* vyx_rt_get_struct_field_ty(void* h, void* st, uint64_t idx) {
    VYX_RT_PROF(GetStructFieldTy);
    if (!h || !st) return nullptr;
    auto* sty = llvm::dyn_cast<llvm::StructType>(asTy(st));
    if (!sty || idx >= sty->getNumElements()) return nullptr;
    return sty->getElementType(static_cast<unsigned>(idx));
}

extern "C" VYX_API int64_t vyx_rt_struct_field_offset(void* h, void* st, uint64_t idx) {
    VYX_RT_PROF(StructFieldOffset);
    if (!h || !st) return -1;
    auto* sty = llvm::dyn_cast<llvm::StructType>(asTy(st));
    if (!sty || idx >= sty->getNumElements()) return -1;
    auto* m = asMod(h);
    const auto* layout = m->mod->getDataLayout().getStructLayout(sty);
    if (!layout) return -1;
    return static_cast<int64_t>(layout->getElementOffset(static_cast<unsigned>(idx)));
}

extern "C" VYX_API int64_t vyx_rt_type_alloc_size(void* h, void* ty) {
    VYX_RT_PROF(TypeAllocSize);
    if (!h || !ty) return 0;
    auto* m = asMod(h);
    return static_cast<int64_t>(m->mod->getDataLayout().getTypeAllocSize(asTy(ty)).getFixedValue());
}

/* =================================================================== */
/*   4. Constants                                                      */
/* =================================================================== */

extern "C" VYX_API void* vyx_rt_const_int(void* h, void* ty,
                                          int64_t value, int32_t sign_extend) {
    VYX_RT_PROF(ConstInt);
    if (!h || !ty) return nullptr;
    auto* ity = llvm::dyn_cast<llvm::IntegerType>(asTy(ty));
    if (!ity) return nullptr;
    return llvm::ConstantInt::get(ity,
                                  static_cast<uint64_t>(value),
                                  sign_extend != 0);
}

extern "C" VYX_API void* vyx_rt_const_str(void* h, const char* s, uint64_t len,
                                          int32_t null_terminated) {
    VYX_RT_PROF(ConstStr);
    if (!h || !s) return nullptr;
    auto& ctx = *asMod(h)->ctx;
    if (len == 0) len = std::strlen(s);
    return llvm::ConstantDataArray::getString(
        ctx,
        llvm::StringRef(s, static_cast<size_t>(len)),
        null_terminated != 0);
}

extern "C" VYX_API void* vyx_rt_const_vyx_string_ptr(void* h,
                                                     const char* s,
                                                     uint64_t len,
                                                     const char* name,
                                                     uint64_t name_len) {
    VYX_RT_PROF(ConstStr);
    if (!h || !s) return nullptr;
    auto* m = asMod(h);
    auto& ctx = *m->ctx;
    constexpr uint64_t kMagic = 0x5659585354524c45ull;

    std::string bytes;
    bytes.resize(sizeof(uint64_t) * 2u + static_cast<std::size_t>(len) + 1u);
    std::memcpy(bytes.data(), &kMagic, sizeof(uint64_t));
    std::memcpy(bytes.data() + sizeof(uint64_t), &len, sizeof(uint64_t));
    if (len > 0) {
        std::memcpy(bytes.data() + sizeof(uint64_t) * 2u, s, static_cast<std::size_t>(len));
    }
    bytes[sizeof(uint64_t) * 2u + static_cast<std::size_t>(len)] = '\0';

    auto* arrTy = llvm::ArrayType::get(llvm::Type::getInt8Ty(ctx), bytes.size());
    auto* init = llvm::ConstantDataArray::get(ctx, llvm::ArrayRef<uint8_t>(
        reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
    auto globalName = asStr(name, name_len);
    if (globalName.empty()) globalName = ".vyxstr";
    auto* gv = new llvm::GlobalVariable(*m->mod,
                                        arrTy,
                                        true,
                                        llvm::GlobalValue::PrivateLinkage,
                                        init,
                                        globalName);
    gv->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    gv->setAlignment(llvm::Align(8));

    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* zero = llvm::ConstantInt::get(i32Ty, 0);
    auto* dataOffset = llvm::ConstantInt::get(i32Ty, static_cast<uint64_t>(sizeof(uint64_t) * 2u));
    llvm::Constant* idxs[] = {zero, dataOffset};
    return llvm::ConstantExpr::getInBoundsGetElementPtr(
        arrTy, gv, llvm::ArrayRef<llvm::Constant*>(idxs, 2));
}

extern "C" VYX_API void* vyx_rt_const_null(void* h, void* ty) {
    VYX_RT_PROF(ConstNull);
    if (!h || !ty) return nullptr;
    return llvm::Constant::getNullValue(asTy(ty));
}
extern "C" VYX_API void* vyx_rt_const_ptr_null(void* h, void* ptr_ty) {
    VYX_RT_PROF(ConstPtrNull);
    if (!h || !ptr_ty) return nullptr;
    auto* pty = llvm::dyn_cast<llvm::PointerType>(asTy(ptr_ty));
    if (!pty) return nullptr;
    return llvm::ConstantPointerNull::get(pty);
}

extern "C" VYX_API void* vyx_rt_const_real(void* h, void* ty, double value) {
    VYX_RT_PROF(ConstReal);
    /* P1 ABI: floating-point constant. Mirrors const_int's pattern —
     * null-checked handle/type, dyn-cast to the target category, build
     * via the C++ Constants API (LLVMConstReal would also work but the
     * rest of this TU stays on the C++ surface).                       */
    if (!h || !ty) return nullptr;
    auto* fty = asTy(ty);
    if (!fty->isFloatingPointTy()) {
        asMod(h)->lastError = "const_real: ty is not a floating-point type";
        return nullptr;
    }
    return llvm::ConstantFP::get(fty, value);
}

extern "C" VYX_API int64_t vyx_rt_parse_real_bits(void* h, const char* s, uint64_t len) {
    VYX_RT_PROF(ParseRealBits);
    (void)h;
    if (!s) return 0;
    std::string text(s, static_cast<size_t>(len));
    char* end = nullptr;
    double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str()) return 0;
    int64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "double bit width mismatch");
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

/* =================================================================== */
/*   5. Functions / globals                                            */
/* =================================================================== */

static bool vyx_rt_is_readonly_runtime_function(llvm::StringRef name) {
    return name == "strlen" ||
           name == "memcmp" ||
           name == "strcmp" ||
           name == "strstr";
}

static bool vyx_rt_is_bootstrap_inline_helper(llvm::StringRef name) {
    return name == "entry_at" ||
           name == "empty_name" ||
           name == "entry_write" ||
           name == "entry_key_matches_ptr" ||
           name == "entry_lookup_value" ||
           name == "entry_lookup_extra" ||
           name == "entry_lookup_value_rev" ||
           name == "entry_lookup_extra_rev" ||
           name == "entry_slot_name_matches" ||
           name == "entry_has_later_name" ||
           name == "entry_name_occurrence_count" ||
           name == "entry_lookup_value_nth" ||
           name == "entry_lookup_extra_nth" ||
           name == "cap_at" ||
           name == "local_at" ||
           name == "loop_at" ||
           name == "write_rawptr" ||
           name == "read_rawptr" ||
           name == "write_i64" ||
           name == "read_i64" ||
           name == "slot_at" ||
           name == "slot_read_i32" ||
           name == "slot_write_i32" ||
           name == "slot_read_i64" ||
           name == "slot_write_i64" ||
           name == "slot_read_rawptr" ||
           name == "slot_write_rawptr" ||
           name == "ast_read_rawptr" ||
           name == "ast_write_rawptr" ||
           name == "ast_read_i32" ||
           name == "ast_write_i32" ||
           name == "ast_read_i64" ||
           name == "ast_write_i64" ||
           name == "string_contains" ||
           name == "driver_string_contains" ||
           name == "generic_call_base" ||
           name == "generic_call_args" ||
           name == "generic_call_base_raw" ||
           name == "generic_call_args_raw" ||
           name == "codegen_generic_param_at" ||
           name == "codegen_generic_arg_at" ||
           name == "codegen_string_method_kind" ||
           name == "codegen_mangle_part" ||
           name == "codegen_env_get" ||
           name == "codegen_env_get_ptr" ||
           name == "codegen_env_set" ||
           name == "codegen_env_set_ptr" ||
           name == "codegen_env_set_text" ||
           name == "codegen_env_set_text_ptr" ||
           name == "codegen_starts_with" ||
           name == "codegen_lit_is_float" ||
           name == "Parser_kind" ||
           name == "Parser_check" ||
           name == "Parser_lex" ||
           name == "Parser_accept" ||
           name == "type_ref_is_float" ||
           name == "token_kind" ||
           name == "token_line" ||
           name == "token_col" ||
           name == "token_char_value" ||
           name.starts_with("ty_") ||
           name.starts_with("ex_") ||
           name.starts_with("st_") ||
           name.starts_with("de_") ||
           name.starts_with("tk_") ||
           name == "lex_token_at" ||
           name.starts_with("expr_get_") ||
           name.starts_with("expr_set_") ||
           name.starts_with("stmt_get_") ||
           name.starts_with("stmt_set_") ||
           name.starts_with("decl_get_") ||
           name.starts_with("decl_set_") ||
           name == "CodeGen_value_as_string_ptr" ||
           name == "CodeGen_int_type_for_bits" ||
           name == "CodeGen_type_int_bits" ||
           name == "CodeGen_type_is_pointer_like" ||
           name == "CodeGen_emit_string_len_value" ||
           name == "CodeGen_emit_string_concat2" ||
           name == "CodeGen_emit_str_ptr" ||
           name == "CodeGen_expr_int_bits_current_env" ||
           name.starts_with("tref_get_") ||
           name.starts_with("tref_set_");
}

static void vyx_rt_apply_function_attrs(llvm::Function* fn, llvm::StringRef name) {
    if (!fn) return;
    // Unwind behavior belongs to the source/runtime ABI contract. This entry
    // point also declares unknown C and DCI functions, so it cannot default
    // every function to nounwind.
    if (name == "exit" || name == "abort") {
        fn->addFnAttr(llvm::Attribute::NoReturn);
    }
    if (vyx_rt_is_bootstrap_inline_helper(name)) {
        fn->addFnAttr(llvm::Attribute::AlwaysInline);
    }
    // The vyx_string_* helpers read caller buffers and may update runtime
    // length caches and profiling state, so they intentionally receive no
    // memory-effects attribute here.
    if (vyx_rt_is_readonly_runtime_function(name)) {
        fn->setOnlyReadsMemory();
    }
}

extern "C" VYX_API void* vyx_rt_add_function(void* h,
                                             const char* name, uint64_t name_len,
                                             void* fty) {
    VYX_RT_PROF(AddFunction);
    if (!h || !fty) return nullptr;
    auto* m = asMod(h);
    const auto fnName = asStr(name, name_len);
    auto* functionType = llvm::cast<llvm::FunctionType>(asTy(fty));
    if (auto* existing = m->mod->getFunction(fnName)) {
        if (existing->getFunctionType() != functionType) {
            m->lastError = "add_function: conflicting declaration for `" + fnName + "`";
            return nullptr;
        }
        if (!existing->empty()) {
            m->lastError = "add_function: duplicate definition for `" + fnName + "`";
            return nullptr;
        }
        vyx_rt_apply_function_attrs(existing, fnName);
        return existing;
    }
    auto* fn = llvm::Function::Create(
        functionType,
        llvm::Function::ExternalLinkage,
        fnName,
        *m->mod);
    vyx_rt_apply_function_attrs(fn, fnName);
    return fn;
}

extern "C" VYX_API int32_t vyx_rt_set_function_attr(void* h, void* fn_handle,
                                                    const char* attr, uint64_t attr_len,
                                                    const char* value, uint64_t value_len) {
    VYX_RT_PROF(SetFunctionAttr);
    if (!h || !fn_handle || !attr) return 1;
    auto* m = asMod(h);
    auto* fn = llvm::dyn_cast<llvm::Function>(asVal(fn_handle));
    if (!fn) {
        m->lastError = "set_function_attr: handle is not a function";
        return 2;
    }

    auto name = asStr(attr, attr_len);
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (name == "inlinehint" || name == "inline_hint") {
        fn->removeFnAttr(llvm::Attribute::NoInline);
        fn->removeFnAttr(llvm::Attribute::AlwaysInline);
        fn->addFnAttr(llvm::Attribute::InlineHint);
        return 0;
    }
    if (name == "inline" || name == "alwaysinline") {
        fn->removeFnAttr(llvm::Attribute::NoInline);
        fn->removeFnAttr(llvm::Attribute::InlineHint);
        fn->addFnAttr(llvm::Attribute::AlwaysInline);
        return 0;
    }
    if (name == "noinline") {
        fn->removeFnAttr(llvm::Attribute::AlwaysInline);
        fn->removeFnAttr(llvm::Attribute::InlineHint);
        fn->addFnAttr(llvm::Attribute::NoInline);
        return 0;
    }
    if (name == "cold") {
        fn->removeFnAttr(llvm::Attribute::Hot);
        fn->addFnAttr(llvm::Attribute::Cold);
        return 0;
    }
    if (name == "hot") {
        fn->removeFnAttr(llvm::Attribute::Cold);
        fn->addFnAttr(llvm::Attribute::Hot);
        return 0;
    }
    if (name == "pure" || name == "readnone") {
        fn->setDoesNotAccessMemory();
        return 0;
    }
    if (name == "readonly") {
        fn->setOnlyReadsMemory();
        return 0;
    }
    if (name == "writeonly") {
        fn->setOnlyWritesMemory();
        return 0;
    }
    if (name == "noreturn") {
        fn->removeFnAttr(llvm::Attribute::WillReturn);
        fn->addFnAttr(llvm::Attribute::NoReturn);
        return 0;
    }
    if (name == "nounwind") {
        fn->addFnAttr(llvm::Attribute::NoUnwind);
        return 0;
    }
    if (name == "uwtable") {
        fn->setUWTableKind(llvm::UWTableKind::Default);
        return 0;
    }
    if (name == "personality") {
        auto pname = asStr(value, value_len);
        if (pname.empty()) {
            m->lastError = "set_function_attr: personality requires a symbol name";
            return 3;
        }
        auto* i32_ty = llvm::Type::getInt32Ty(*m->ctx);
        auto* pty = llvm::FunctionType::get(i32_ty, true);
        fn->setPersonalityFn(llvm::cast<llvm::Constant>(
            m->mod->getOrInsertFunction(pname, pty).getCallee()));
        fn->setUWTableKind(llvm::UWTableKind::Default);
        return 0;
    }
    if (name == "willreturn") {
        if (!fn->hasFnAttribute(llvm::Attribute::NoReturn)) {
            fn->addFnAttr(llvm::Attribute::WillReturn);
        }
        return 0;
    }
    if (name == "tailcall") {
        fn->addFnAttr("tailcall");
        return 0;
    }
    if (name == "nvvm.kernel" || name == "nvvm_kernel") {
        // NVPTX device-kernel entry.  This LLVM's NVPTXAsmPrinter only
        // honours the `!nvvm.annotations` named metadata (the fn attribute
        // alone still lowers the function to a `.func`), so attach both:
        // the attribute for tooling and the metadata tuple llc consumes.
        fn->addFnAttr("nvvm.kernel");
        auto& ctx = fn->getContext();
        auto* md_tuple = llvm::MDNode::get(ctx, {
            llvm::ValueAsMetadata::get(fn),
            llvm::MDString::get(ctx, "kernel"),
            llvm::ConstantAsMetadata::get(
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 1)),
        });
        m->mod->getOrInsertNamedMetadata("nvvm.annotations")
            ->addOperand(md_tuple);
        return 0;
    }
    if (name == "align") {
        const auto text = asStr(value, value_len);
        char* end = nullptr;
        const auto parsed = std::strtoull(text.c_str(), &end, 10);
        if (text.empty() || end == text.c_str() || *end != '\0' ||
            parsed == 0 || (parsed & (parsed - 1)) != 0) {
            m->lastError = "set_function_attr: align requires a positive power-of-two value";
            return 3;
        }
        fn->setAlignment(llvm::Align(parsed));
        return 0;
    }

    m->lastError = "set_function_attr: unsupported attribute '" + name + "'";
    return 4;
}

extern "C" VYX_API int32_t vyx_rt_set_param_abi_attr(void* h, void* fn_handle,
                                                       uint64_t param_index,
                                                       const char* attr, uint64_t attr_len,
                                                       void* pointee_type,
                                                       uint64_t alignment) {
    VYX_RT_PROF(SetFunctionAttr);
    if (!h || !fn_handle || !attr) return 1;
    auto* m = asMod(h);
    auto* value = asVal(fn_handle);
    auto* fn = llvm::dyn_cast<llvm::Function>(value);
    auto* call = llvm::dyn_cast<llvm::CallBase>(value);
    const auto arg_count = fn ? fn->arg_size() : (call ? call->arg_size() : 0);
    if ((!fn && !call) || param_index >= arg_count ||
        param_index > std::numeric_limits<unsigned>::max()) {
        m->lastError = "set_param_abi_attr: invalid function/call or parameter index";
        return 2;
    }
    // LLVM's Function/CallBase parameter indices are unsigned. The range
    // check above makes this narrowing conversion safe.
    const auto llvm_param_index = static_cast<unsigned>(param_index);
    auto name = asStr(attr, attr_len);
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (alignment != 0 &&
        ((alignment & (alignment - 1)) != 0 ||
         alignment > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()))) {
        m->lastError =
            "set_param_abi_attr: alignment must be a representable power of two";
        return 5;
    }
    auto* type = pointee_type ? asTy(pointee_type) : nullptr;
    auto add_param_attr = [&](llvm::Attribute attribute) {
        if (fn) fn->addParamAttr(llvm_param_index, attribute);
        else call->addParamAttr(llvm_param_index, attribute);
    };
    auto* param_type = fn
        ? fn->getFunctionType()->getParamType(llvm_param_index)
        : call->getArgOperand(llvm_param_index)->getType();
    auto require_pointer_param = [&]() -> bool {
        if (param_type && param_type->isPointerTy()) {
            return true;
        }
        m->lastError = "set_param_abi_attr: " + name
            + " requires a pointer parameter";
        return false;
    };
    if (name == "sret") {
        if (!type) {
            m->lastError = "set_param_abi_attr: sret requires a pointee type";
            return 3;
        }
        add_param_attr(llvm::Attribute::getWithStructRetType(value->getContext(), type));
    } else if (name == "byval") {
        if (!type) {
            m->lastError = "set_param_abi_attr: byval requires a pointee type";
            return 3;
        }
        add_param_attr(llvm::Attribute::getWithByValType(value->getContext(), type));
    } else if (name == "inalloca") {
        if (!type) {
            m->lastError = "set_param_abi_attr: inalloca requires a pointee type";
            return 3;
        }
        add_param_attr(llvm::Attribute::getWithInAllocaType(value->getContext(), type));
    } else if (name == "noalias") {
        if (!require_pointer_param()) { return 3; }
        add_param_attr(llvm::Attribute::get(value->getContext(), llvm::Attribute::NoAlias));
    } else if (name == "nonnull") {
        if (!require_pointer_param()) { return 3; }
        add_param_attr(llvm::Attribute::get(value->getContext(), llvm::Attribute::NonNull));
    } else if (name == "readonly") {
        if (!require_pointer_param()) { return 3; }
        add_param_attr(llvm::Attribute::get(value->getContext(), llvm::Attribute::ReadOnly));
    } else if (name == "writeonly") {
        if (!require_pointer_param()) { return 3; }
        add_param_attr(llvm::Attribute::get(value->getContext(), llvm::Attribute::WriteOnly));
    } else if (name == "captures(none)") {
        if (!require_pointer_param()) { return 3; }
        add_param_attr(llvm::Attribute::getWithCaptureInfo(
            value->getContext(), llvm::CaptureInfo::none()));
    } else if (name == "align") {
        if (alignment == 0) {
            m->lastError = "set_param_abi_attr: align requires a non-zero alignment";
            return 5;
        }
    } else {
        m->lastError = "set_param_abi_attr: unsupported attribute '" + name + "'";
        return 4;
    }
    if (alignment != 0) {
        add_param_attr(llvm::Attribute::getWithAlignment(value->getContext(), llvm::Align(alignment)));
    }
    return 0;
}

extern "C" VYX_API int32_t vyx_rt_set_return_attr(void* h, void* fn_handle,
                                                  const char* attr, uint64_t attr_len) {
    if (!h || !fn_handle || !attr) return 1;
    auto* m = asMod(h);
    auto* value = asVal(fn_handle);
    auto* fn = llvm::dyn_cast<llvm::Function>(value);
    auto* call = llvm::dyn_cast<llvm::CallBase>(value);
    if (!fn && !call) {
        m->lastError = "set_return_attr: handle is not a function or call";
        return 2;
    }
    auto name = asStr(attr, attr_len);
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<unsigned char>(std::tolower(c)); });
    auto add_ret_attr = [&](llvm::Attribute attribute) {
        if (fn) fn->addRetAttr(attribute);
        else call->addRetAttr(attribute);
    };
    if (name == "noalias") {
        add_ret_attr(llvm::Attribute::get(value->getContext(), llvm::Attribute::NoAlias));
    } else if (name == "nonnull") {
        add_ret_attr(llvm::Attribute::get(value->getContext(), llvm::Attribute::NonNull));
    } else if (name == "noundef") {
        add_ret_attr(llvm::Attribute::get(value->getContext(), llvm::Attribute::NoUndef));
    } else {
        m->lastError = "set_return_attr: unsupported attribute '" + name + "'";
        return 4;
    }
    return 0;
}

static bool vyx_rt_parse_calling_convention(std::string name,
                                            llvm::CallingConv::ID& result) {
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (name == "default" || name == "c" || name == "system" ||
        name == "cdecl" || name == "cxx_free_function" ||
        name == "cxx_static_method" || name == "cxx_method" ||
        name == "cxx_virtual_method" || name == "cxx_constructor" ||
        name == "cxx_destructor") {
        result = llvm::CallingConv::C;
        return true;
    }
    if (name == "win64") {
        result = llvm::CallingConv::Win64;
        return true;
    }
    if (name == "x86_64_sysv") {
        result = llvm::CallingConv::X86_64_SysV;
        return true;
    }
    if (name == "stdcall") {
        result = llvm::CallingConv::X86_StdCall;
        return true;
    }
    if (name == "fastcall") {
        result = llvm::CallingConv::X86_FastCall;
        return true;
    }
    if (name == "thiscall") {
        result = llvm::CallingConv::X86_ThisCall;
        return true;
    }
    if (name == "vectorcall") {
        result = llvm::CallingConv::X86_VectorCall;
        return true;
    }
    if (name == "rust") {
        // rustc's extern "Rust" integer/pointer surface matches the C ABI on
        // the supported Win64/SysV targets.  Split fat pointers are described
        // by the Adapter contract, not by a distinct LLVM CC id.
        result = llvm::CallingConv::C;
        return true;
    }
    return false;
}

extern "C" VYX_API int32_t vyx_rt_set_calling_convention(void* h,
                                                          void* fn_or_call,
                                                          const char* convention,
                                                          uint64_t convention_len) {
    VYX_RT_PROF(SetFunctionAttr);
    if (!h) return 1;
    auto* m = asMod(h);
    if (!fn_or_call || !convention) {
        m->lastError = "set_calling_convention: invalid argument";
        return 1;
    }

    const auto name = asStr(convention, convention_len);
    llvm::CallingConv::ID callingConvention = llvm::CallingConv::C;
    if (!vyx_rt_parse_calling_convention(name, callingConvention)) {
        m->lastError = "set_calling_convention: unsupported convention '" + name + "'";
        return 3;
    }

    auto* value = asVal(fn_or_call);
    if (auto* fn = llvm::dyn_cast<llvm::Function>(value)) {
        fn->setCallingConv(callingConvention);
        return 0;
    }
    if (auto* call = llvm::dyn_cast<llvm::CallBase>(value)) {
        call->setCallingConv(callingConvention);
        return 0;
    }

    m->lastError = "set_calling_convention: handle is not a function or call";
    return 2;
}

extern "C" VYX_API void vyx_rt_set_linkage(void* h, void* fn_or_global, int32_t linkage_kind) {
    VYX_RT_PROF(SetLinkage);
    if (!h || !fn_or_global) return;
    auto* gv = llvm::dyn_cast<llvm::GlobalValue>(asVal(fn_or_global));
    if (!gv) return;
    switch (linkage_kind) {
        case VYX_RT_LINK_INTERNAL: gv->setLinkage(llvm::GlobalValue::InternalLinkage); break;
        case VYX_RT_LINK_PRIVATE:  gv->setLinkage(llvm::GlobalValue::PrivateLinkage);  break;
        case VYX_RT_LINK_LINKONCE_ODR: gv->setLinkage(llvm::GlobalValue::LinkOnceODRLinkage); break;
        case VYX_RT_LINK_WEAK_ODR: gv->setLinkage(llvm::GlobalValue::WeakODRLinkage); break;
        case VYX_RT_LINK_EXTERNAL:
        default:                   gv->setLinkage(llvm::GlobalValue::ExternalLinkage); break;
    }
    // COFF requires an actual COMDAT group to coalesce ODR definitions.
    // Weak linkage alone becomes a weak external alias backed by a strong
    // .text definition, which lld rejects when two consumers instantiate
    // the same generic. Preserve the canonical name for cross-CGU calls.
    if (auto* object = llvm::dyn_cast<llvm::GlobalObject>(gv)) {
        const llvm::Triple triple(object->getParent()->getTargetTriple());
        if ((gv->hasLinkOnceODRLinkage() || gv->hasWeakODRLinkage()) &&
            (triple.isOSBinFormatCOFF() || triple.isOSBinFormatELF() ||
             triple.isOSBinFormatWasm())) {
            auto* comdat = object->getParent()->getOrInsertComdat(gv->getName());
            comdat->setSelectionKind(llvm::Comdat::Any);
            object->setComdat(comdat);
        } else {
            object->setComdat(nullptr);
        }
    }
}

extern "C" VYX_API void* vyx_rt_get_param(void* h, void* fn, uint64_t idx) {
    VYX_RT_PROF(GetParam);
    if (!h || !fn) return nullptr;
    auto* f = asFn(fn);
    if (idx >= f->arg_size()) return nullptr;
    return f->getArg(static_cast<unsigned>(idx));
}
extern "C" VYX_API void vyx_rt_set_param_name(void* h, void* param,
                                              const char* name, uint64_t len) {
    VYX_RT_PROF(SetParamName);
    if (!h || !param) return;
    asVal(param)->setName(asStr(name, len));
}
extern "C" VYX_API void* vyx_rt_add_global(void* h, void* ty,
                                           const char* name, uint64_t name_len) {
    VYX_RT_PROF(AddGlobal);
    if (!h || !ty) return nullptr;
    auto* m = asMod(h);
    return new llvm::GlobalVariable(
        *m->mod,
        asTy(ty),
        /*isConstant=*/false,
        llvm::GlobalValue::ExternalLinkage,
        /*Initializer=*/nullptr,
        asStr(name, name_len));
}
extern "C" VYX_API void vyx_rt_set_initializer(void* h, void* gv, void* const_val) {
    VYX_RT_PROF(SetInitializer);
    if (!h || !gv || !const_val) return;
    auto* g = llvm::dyn_cast<llvm::GlobalVariable>(asVal(gv));
    auto* c = llvm::dyn_cast<llvm::Constant>(asVal(const_val));
    if (!g || !c) return;
    g->setInitializer(c);
}
extern "C" VYX_API void vyx_rt_set_global_constant(void* h, void* gv, int32_t is_const) {
    VYX_RT_PROF(SetGlobalConstant);
    if (!h || !gv) return;
    auto* g = llvm::dyn_cast<llvm::GlobalVariable>(asVal(gv));
    if (!g) return;
    g->setConstant(is_const != 0);
}

/* =================================================================== */
/*   6. Builder & basic blocks                                         */
/* =================================================================== */

extern "C" VYX_API int32_t vyx_rt_builder_new(void* h) {
    VYX_RT_PROF(BuilderNew);
    if (!h) return -1;
    auto* m = asMod(h);
    if (!m->builder) m->builder = std::make_unique<llvm::IRBuilder<>>(*m->ctx);
    return 0;
}

extern "C" VYX_API void* vyx_rt_append_block(void* h, void* fn,
                                             const char* name, uint64_t name_len) {
    VYX_RT_PROF(AppendBlock);
    if (!h || !fn) return nullptr;
    auto* m = asMod(h);
    if (rtProfileLevel() >= 10) {
        auto* f = asFn(fn);
        std::fprintf(stderr, "[vyx-rt-trace-detail] append_block fn=%s bb=%s\n",
                     f ? f->getName().str().c_str() : "<null>",
                     asStr(name, name_len).c_str());
        std::fflush(stderr);
    }
    return llvm::BasicBlock::Create(*m->ctx, asStr(name, name_len), asFn(fn));
}

extern "C" VYX_API void vyx_rt_position_at_end(void* h, void* bb) {
    VYX_RT_PROF(PositionAtEnd);
    if (!h || !bb) return;
    auto* m = asMod(h);
    if (!m->builder) m->builder = std::make_unique<llvm::IRBuilder<>>(*m->ctx);
    auto* block = asBB(bb);
    if (rtProfileLevel() >= 10) {
        auto* parent = block ? block->getParent() : nullptr;
        std::fprintf(stderr, "[vyx-rt-trace-detail] position_at_end fn=%s bb=%s\n",
                     parent ? parent->getName().str().c_str() : "<null>",
                     block ? block->getName().str().c_str() : "<null>");
        std::fflush(stderr);
    }
    m->builder->SetInsertPoint(block);
}

extern "C" VYX_API void vyx_rt_position_before(void* h, void* instruction) {
    VYX_RT_PROF(PositionBefore);
    if (!h || !instruction) return;
    auto* m = asMod(h);
    auto* point = llvm::dyn_cast<llvm::Instruction>(asVal(instruction));
    if (!m || !point || !point->getParent()) return;
    if (!m->builder) m->builder = std::make_unique<llvm::IRBuilder<>>(*m->ctx);
    m->builder->SetInsertPoint(point);
}

extern "C" VYX_API void* vyx_rt_get_insert_block(void* h) {
    VYX_RT_PROF(GetInsertBlock);
    if (!h) return nullptr;
    auto* m = asMod(h);
    if (!m->builder) return nullptr;
    return m->builder->GetInsertBlock();
}
extern "C" VYX_API void vyx_rt_clear_insertion(void* h) {
    VYX_RT_PROF(ClearInsertion);
    if (!h) return;
    auto* m = asMod(h);
    if (m->builder) m->builder->ClearInsertionPoint();
}

/* =================================================================== */
/*   7. IR construction — arithmetic / compare / control flow          */
/* =================================================================== */

/* All build_* helpers route through the per-module IRBuilder. The
 * builder is created lazily on `vyx_rt_position_at_end`; if it is still
 * absent here, the call is treated as a no-op error path so the Vyx
 * caller can pick the failure up via `vyx_rt_get_last_error`.        */

#define BUILD_BIN(NAME, METHOD)                                                                         \
    extern "C" VYX_API void* vyx_rt_build_##NAME(void* h, void* lhs, void* rhs,                         \
                                                 const char* name, uint64_t len) {                     \
        VYX_RT_PROF(BuildBin);                                                                          \
        if (!h || !lhs || !rhs) return nullptr;                                                         \
        auto* m = asMod(h);                                                                             \
        if (!m->builder) { m->lastError = "build_" #NAME ": no builder"; return nullptr; }              \
        return m->builder->METHOD(asVal(lhs), asVal(rhs), asStr(name, len));                            \
    }

BUILD_BIN(add,  CreateAdd)
BUILD_BIN(sub,  CreateSub)
BUILD_BIN(mul,  CreateMul)
BUILD_BIN(sdiv, CreateSDiv)
BUILD_BIN(srem, CreateSRem)

#undef BUILD_BIN

extern "C" VYX_API void* vyx_rt_build_atomicrmw_add(void* h,
                                                      void* ptr,
                                                      void* value,
                                                      const char* name,
                                                      uint64_t len) {
    VYX_RT_PROF(BuildBin);
    if (!h || !ptr || !value) return nullptr;
    auto* m = asMod(h);
    if (!m->builder) {
        m->lastError = "build_atomicrmw_add: no builder";
        return nullptr;
    }
    auto* out = m->builder->CreateAtomicRMW(llvm::AtomicRMWInst::Add,
                                             asVal(ptr),
                                             asVal(value),
                                             llvm::MaybeAlign(),
                                             llvm::AtomicOrdering::SequentiallyConsistent,
                                             llvm::SyncScope::System);
    if (out) out->setName(asStr(name, len));
    return out;
}

extern "C" VYX_API void* vyx_rt_build_atomic_load(void* h,
                                                     void* ty,
                                                     void* ptr,
                                                     const char* name,
                                                     uint64_t len) {
    VYX_RT_PROF(BuildLoad);
    if (!h || !ty || !ptr) return nullptr;
    auto* m = asMod(h);
    if (!m->builder) {
        m->lastError = "build_atomic_load: no builder";
        return nullptr;
    }
    auto* out = m->builder->CreateLoad(asTy(ty), asVal(ptr), asStr(name, len));
    out->setAtomic(llvm::AtomicOrdering::SequentiallyConsistent);
    out->setSyncScopeID(llvm::SyncScope::System);
    return out;
}

extern "C" VYX_API void* vyx_rt_build_atomic_cmpxchg(void* h,
                                                       void* ptr,
                                                       void* expected,
                                                       void* desired,
                                                       const char* name,
                                                       uint64_t len) {
    VYX_RT_PROF(BuildBin);
    if (!h || !ptr || !expected || !desired) return nullptr;
    auto* m = asMod(h);
    if (!m->builder) {
        m->lastError = "build_atomic_cmpxchg: no builder";
        return nullptr;
    }
    auto* pair = m->builder->CreateAtomicCmpXchg(
        asVal(ptr),
        asVal(expected),
        asVal(desired),
        llvm::MaybeAlign(),
        llvm::AtomicOrdering::SequentiallyConsistent,
        llvm::AtomicOrdering::SequentiallyConsistent,
        llvm::SyncScope::System);
    if (!pair) return nullptr;
    return m->builder->CreateExtractValue(pair, 0, asStr(name, len));
}

extern "C" VYX_API void* vyx_rt_build_icmp(void* h, int32_t pred,
                                           void* lhs, void* rhs,
                                           const char* name, uint64_t len) {
    VYX_RT_PROF(BuildICmp);
    if (!h || !lhs || !rhs) return nullptr;
    auto* m = asMod(h);
    if (!m->builder) { m->lastError = "build_icmp: no builder"; return nullptr; }
    llvm::Value* l = asVal(lhs);
    llvm::Value* r = asVal(rhs);
    llvm::Type* lt = l->getType();
    llvm::Type* rt = r->getType();
    if (lt != rt) {
        if (lt->isIntegerTy() && rt->isIntegerTy()) {
            auto* li = llvm::cast<llvm::IntegerType>(lt);
            auto* ri = llvm::cast<llvm::IntegerType>(rt);
            unsigned bits = std::max(li->getBitWidth(), ri->getBitWidth());
            auto* dst = llvm::IntegerType::get(*m->ctx, bits);
            if (li->getBitWidth() != bits) {
                l = m->builder->CreateSExtOrTrunc(l, dst);
            }
            if (ri->getBitWidth() != bits) {
                r = m->builder->CreateSExtOrTrunc(r, dst);
            }
        } else if (lt->isPointerTy() && rt->isIntegerTy()) {
            auto* intptrTy = llvm::Type::getInt64Ty(*m->ctx);
            if (r->getType() != intptrTy) {
                r = m->builder->CreateSExtOrTrunc(r, intptrTy);
            }
            r = m->builder->CreateIntToPtr(r, lt);
        } else if (rt->isPointerTy() && lt->isIntegerTy()) {
            auto* intptrTy = llvm::Type::getInt64Ty(*m->ctx);
            if (l->getType() != intptrTy) {
                l = m->builder->CreateSExtOrTrunc(l, intptrTy);
            }
            l = m->builder->CreateIntToPtr(l, rt);
        } else {
            m->lastError = "build_icmp: incompatible operand types";
            return nullptr;
        }
    }
    /* `pred` is intentionally a raw LLVMIntPredicate value (32..41) so
     * the Vyx side can use the VYX_RT_ICMP_* macros from the header.  */
    return m->builder->CreateICmp(static_cast<llvm::CmpInst::Predicate>(pred),
                                  l, r, asStr(name, len));
}

extern "C" VYX_API void vyx_rt_build_br(void* h, void* dest_bb) {
    VYX_RT_PROF(BuildBr);
    if (!h || !dest_bb) return;
    auto* m = asMod(h);
    if (!m->builder) { m->lastError = "build_br: no builder"; return; }
    m->builder->CreateBr(asBB(dest_bb));
}

extern "C" VYX_API void vyx_rt_build_cond_br(void* h, void* cond,
                                             void* then_bb, void* else_bb) {
    VYX_RT_PROF(BuildCondBr);
    if (!h || !cond || !then_bb || !else_bb) return;
    auto* m = asMod(h);
    if (!m->builder) { m->lastError = "build_cond_br: no builder"; return; }
    m->builder->CreateCondBr(asVal(cond), asBB(then_bb), asBB(else_bb));
}

extern "C" VYX_API void vyx_rt_build_ret(void* h, void* val) {
    VYX_RT_PROF(BuildRet);
    if (!h) return;
    auto* m = asMod(h);
    if (!m->builder) return;
    if (val) m->builder->CreateRet(asVal(val));
    else     m->builder->CreateRetVoid();
}

extern "C" VYX_API void vyx_rt_build_ret_void(void* h) {
    VYX_RT_PROF(BuildRetVoid);
    if (!h) return;
    auto* m = asMod(h);
    if (m->builder) m->builder->CreateRetVoid();
}

extern "C" VYX_API void* vyx_rt_build_alloca(void* h, void* ty,
                                             const char* name, uint64_t len) {
    VYX_RT_PROF(BuildAlloca);
    if (!h || !ty) return nullptr;
    auto* m = asMod(h);
    if (!m->builder) { m->lastError = "build_alloca: no builder"; return nullptr; }
    auto* currentBlock = m->builder->GetInsertBlock();
    if (!currentBlock) { m->lastError = "build_alloca: no insert block"; return nullptr; }
    auto* fn = currentBlock->getParent();
    if (!fn) { m->lastError = "build_alloca: no parent function"; return nullptr; }

    llvm::IRBuilder<> entryBuilder(&fn->getEntryBlock(), fn->getEntryBlock().begin());
    auto* alloca = entryBuilder.CreateAlloca(asTy(ty), /*ArraySize=*/nullptr, asStr(name, len));
    alloca->setAlignment(m->mod->getDataLayout().getPrefTypeAlign(asTy(ty)));
    return alloca;
}

extern "C" VYX_API void* vyx_rt_build_lifetime_start(void* h, void* alloca_handle) {
    VYX_RT_PROF(BuildLifetimeStart);
    if (!h) return nullptr;
    auto* m = asMod(h);
    if (!m->builder) {
        m->lastError = "build_lifetime_start: no builder";
        return nullptr;
    }
    if (!alloca_handle) {
        m->lastError = "build_lifetime_start: alloca is null";
        return nullptr;
    }
    auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(asVal(alloca_handle));
    if (!alloca) {
        m->lastError = "build_lifetime_start: value is not a direct alloca";
        return nullptr;
    }
    return m->builder->CreateLifetimeStart(alloca);
}

extern "C" VYX_API void* vyx_rt_build_lifetime_end(void* h, void* alloca_handle) {
    VYX_RT_PROF(BuildLifetimeEnd);
    if (!h) return nullptr;
    auto* m = asMod(h);
    if (!m->builder) {
        m->lastError = "build_lifetime_end: no builder";
        return nullptr;
    }
    if (!alloca_handle) {
        m->lastError = "build_lifetime_end: alloca is null";
        return nullptr;
    }
    auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(asVal(alloca_handle));
    if (!alloca) {
        m->lastError = "build_lifetime_end: value is not a direct alloca";
        return nullptr;
    }
    return m->builder->CreateLifetimeEnd(alloca);
}

extern "C" VYX_API void* vyx_rt_build_load(void* h, void* ty, void* ptr,
                                           const char* name, uint64_t len) {
    VYX_RT_PROF(BuildLoad);
    if (!h || !ty || !ptr) return nullptr;
    auto* m = asMod(h);
    if (!m->builder) { m->lastError = "build_load: no builder"; return nullptr; }
    auto* pointer = asVal(ptr);
    llvm::Align alignment(1);
    if (provenAllocaStructFieldAlignment(m->mod->getDataLayout(), pointer, alignment)) {
        return m->builder->CreateAlignedLoad(asTy(ty),
                                             pointer,
                                             alignment,
                                             asStr(name, len));
    }
    return m->builder->CreateLoad(asTy(ty), pointer, asStr(name, len));
}

extern "C" VYX_API void* vyx_rt_build_store(void* h, void* val, void* ptr) {
    VYX_RT_PROF(BuildStore);
    if (!h || !val || !ptr) return nullptr;
    auto* m = asMod(h);
    if (!m->builder) { m->lastError = "build_store: no builder"; return nullptr; }
    return vyx_rt_build_store_instruction(h, val, ptr);
}

extern "C" VYX_API void* vyx_rt_build_store_instruction(void* h, void* val, void* ptr) {
    VYX_RT_PROF(BuildStore);
    if (!h || !val || !ptr) return nullptr;
    auto* m = asMod(h);
    if (!m->builder) { m->lastError = "build_store: no builder"; return nullptr; }
    auto* pointer = asVal(ptr);
    llvm::Align alignment(1);
    if (provenAllocaStructFieldAlignment(m->mod->getDataLayout(), pointer, alignment)) {
        return m->builder->CreateAlignedStore(asVal(val), pointer, alignment);
    }
    return m->builder->CreateStore(asVal(val), pointer);
}

extern "C" VYX_API void* vyx_rt_build_load_volatile(void* h, void* ty, void* ptr,
                                                    const char* name, uint64_t len) {
    VYX_RT_PROF(BuildLoad);
    if (!h || !ty || !ptr) return nullptr;
    auto* m = asMod(h);
    if (!m->builder) { m->lastError = "build_load_volatile: no builder"; return nullptr; }
    auto* load = m->builder->CreateLoad(asTy(ty), asVal(ptr), asStr(name, len));
    if (load) load->setVolatile(true);
    return load;
}

extern "C" VYX_API void vyx_rt_build_store_volatile(void* h, void* val, void* ptr) {
    VYX_RT_PROF(BuildStore);
    if (!h || !val || !ptr) return;
    auto* m = asMod(h);
    if (!m->builder) { m->lastError = "build_store_volatile: no builder"; return; }
    auto* store = m->builder->CreateStore(asVal(val), asVal(ptr));
    if (store) store->setVolatile(true);
}

extern "C" VYX_API void* vyx_rt_build_load_aligned(void* h, void* ty, void* ptr,
                                                    int32_t alignment,
                                                    const char* name, uint64_t len) {
    VYX_RT_PROF(BuildLoad);
    if (!h || !ty || !ptr) return nullptr;
    auto* m = asMod(h);
    if (!m->builder) { m->lastError = "build_load_aligned: no builder"; return nullptr; }
    if (alignment <= 0 || (alignment & (alignment - 1)) != 0) {
        m->lastError = "build_load_aligned: alignment must be a positive power of two";
        return nullptr;
    }
    return m->builder->CreateAlignedLoad(
        asTy(ty), asVal(ptr), llvm::Align(static_cast<unsigned>(alignment)), asStr(name, len));
}

extern "C" VYX_API void* vyx_rt_build_store_aligned(void* h, void* val, void* ptr,
                                                     int32_t alignment) {
    VYX_RT_PROF(BuildStore);
    if (!h || !val || !ptr) return nullptr;
    auto* m = asMod(h);
    if (!m->builder) { m->lastError = "build_store_aligned: no builder"; return nullptr; }
    if (alignment <= 0 || (alignment & (alignment - 1)) != 0) {
        m->lastError = "build_store_aligned: alignment must be a positive power of two";
        return nullptr;
    }
    return m->builder->CreateAlignedStore(
        asVal(val), asVal(ptr), llvm::Align(static_cast<unsigned>(alignment)));
}

extern "C" VYX_API void* vyx_rt_build_gep(void* h, void* ty, void* ptr,
                                          const void* const* idxs, uint64_t n,
                                          const char* name, uint64_t len) {
    VYX_RT_PROF(BuildGep);
    if (!h || !ty || !ptr) return nullptr;
    auto* m = asMod(h);
    if (!m->builder) { m->lastError = "build_gep: no builder"; return nullptr; }
    std::vector<llvm::Value*> ix;
    ix.reserve(static_cast<size_t>(n));
    for (uint64_t i = 0; i < n; ++i) {
        ix.push_back(static_cast<llvm::Value*>(const_cast<void*>(idxs ? idxs[i] : nullptr)));
    }
    return m->builder->CreateGEP(asTy(ty), asVal(ptr), ix, asStr(name, len));
}

extern "C" VYX_API void* vyx_rt_build_gep1(void* h, void* ty, void* ptr,
                                           void* i0,
                                           const char* name, uint64_t len) {
    VYX_RT_PROF(BuildGep);
    if (!h || !ty || !ptr) return nullptr;
    auto* m = asMod(h);
    if (!m->builder) { m->lastError = "build_gep1: no builder"; return nullptr; }
    llvm::Value* ix[] = {asVal(i0)};
    return m->builder->CreateGEP(asTy(ty), asVal(ptr), llvm::ArrayRef<llvm::Value*>(ix, 1), asStr(name, len));
}

extern "C" VYX_API void* vyx_rt_build_gep2(void* h, void* ty, void* ptr,
                                           void* i0, void* i1,
                                           const char* name, uint64_t len) {
    VYX_RT_PROF(BuildGep);
    if (!h || !ty || !ptr) return nullptr;
    auto* m = asMod(h);
    if (!m->builder) { m->lastError = "build_gep2: no builder"; return nullptr; }
    llvm::Value* ix[] = {asVal(i0), asVal(i1)};
    return m->builder->CreateGEP(asTy(ty), asVal(ptr), llvm::ArrayRef<llvm::Value*>(ix, 2), asStr(name, len));
}

static bool vyx_rt_module_uses_msvc_eh(VyxRtModule* m) {
    std::string triple;
    if (m->mod) {
        triple = m->mod->getTargetTriple().str();
    }
    if (triple.empty()) {
        triple = m->triple;
    }
    return triple.find("windows") != std::string::npos
        && triple.find("msvc") != std::string::npos;
}

static llvm::BasicBlock* vyx_rt_ensure_cleanup_unwind_block(VyxRtModule*, llvm::Function*);

static llvm::FunctionType* vyx_rt_checked_call_type(VyxRtModule* m,
                                                    void* fty,
                                                    llvm::ArrayRef<llvm::Value*> argv,
                                                    const char* name,
                                                    uint64_t len,
                                                    const char* op) {
    auto* fnTy = llvm::dyn_cast<llvm::FunctionType>(asTy(fty));
    if (!fnTy) {
        m->lastError = std::string(op) + ": fty is not a FunctionType";
        return nullptr;
    }
    const auto fixedParams = fnTy->getNumParams();
    if ((!fnTy->isVarArg() && argv.size() != fixedParams) ||
        (fnTy->isVarArg() && argv.size() < fixedParams)) {
        m->lastError = std::string(op) + ": argument count mismatch for '" +
            asStr(name, len) + "' (expected " + std::to_string(fixedParams) +
            (fnTy->isVarArg() ? "+, got " : ", got ") +
            std::to_string(argv.size()) + ")";
        return nullptr;
    }
    for (unsigned i = 0; i < fixedParams; ++i) {
        auto* arg = argv[i];
        if (!arg) {
            m->lastError = std::string(op) + ": null argument " + std::to_string(i) +
                " for '" + asStr(name, len) + "'";
            return nullptr;
        }
        auto* want = fnTy->getParamType(i);
        auto* got = arg->getType();
        if (got != want) {
            std::string gotText;
            std::string wantText;
            std::string valueText;
            llvm::raw_string_ostream gotOs(gotText);
            llvm::raw_string_ostream wantOs(wantText);
            llvm::raw_string_ostream valueOs(valueText);
            got->print(gotOs);
            want->print(wantOs);
            arg->print(valueOs);
            gotOs.flush();
            wantOs.flush();
            valueOs.flush();
            m->lastError = std::string(op) + ": argument type mismatch for '" +
                asStr(name, len) + "' at " + std::to_string(i) +
                " (expected " + wantText + ", got " + gotText +
                "; value " + valueText + ")";
            return nullptr;
        }
    }
    return fnTy;
}

#include "vyx_eh_cleanup.inc"

static void* vyx_rt_build_invoke_array(void* h, void* fty, void* callee,
                                      llvm::ArrayRef<llvm::Value*> argv,
                                      const char* name, uint64_t len);

static void* vyx_rt_build_call_array(void* h,
                                     void* fty,
                                     void* callee,
                                     llvm::ArrayRef<llvm::Value*> argv,
                                     const char* name,
                                     uint64_t len) {
    if (!h || !fty || !callee) return nullptr;
    auto* m = asMod(h);
    if (!m->builder) { m->lastError = "build_call: no builder"; return nullptr; }
    auto* fnTy = vyx_rt_checked_call_type(m, fty, argv, name, len, "build_call");
    if (!fnTy) return nullptr;
    if (!vyx_eh_prepare_call(m, callee, argv)) return nullptr;
    auto* callerBlock = m->builder->GetInsertBlock();
    const bool indirectShared = callerBlock && callerBlock->getParent()->hasPersonalityFn()
        && !llvm::isa<llvm::Function>(asVal(callee));
    if (vyx_eh_destructor_needs_invoke(m, callee) || indirectShared) {
        return vyx_rt_build_invoke_array(h, fty, callee, argv, name, len);
    }
    const bool voidRet = fnTy->getReturnType()->isVoidTy();
    auto* call = m->builder->CreateCall(
        fnTy, asVal(callee), argv,
        voidRet ? llvm::Twine() : llvm::Twine(asStr(name, len)));
    if (auto* calleeFunction = llvm::dyn_cast<llvm::Function>(asVal(callee))) {
        call->setCallingConv(calleeFunction->getCallingConv());
    }
    return call;
}

static void* vyx_rt_build_invoke_array(void* h,
                                       void* fty,
                                       void* callee,
                                       llvm::ArrayRef<llvm::Value*> argv,
                                       const char* name,
                                       uint64_t len) {
    if (!h || !fty || !callee) return nullptr;
    auto* m = asMod(h);
    if (!m->builder) { m->lastError = "build_invoke: no builder"; return nullptr; }
    auto* curr = m->builder->GetInsertBlock();
    if (!curr) {
        m->lastError = "build_invoke: no insert block";
        return nullptr;
    }
    auto* parent = curr->getParent();
    if (!parent) {
        m->lastError = "build_invoke: insert block has no parent function";
        return nullptr;
    }
    if (!parent->hasPersonalityFn()) {
        m->lastError = "build_invoke: caller is missing a personality function";
        return nullptr;
    }
    auto* fnTy = vyx_rt_checked_call_type(m, fty, argv, name, len, "build_invoke");
    if (!fnTy) return nullptr;
    auto* unwind = vyx_rt_ensure_cleanup_unwind_block(m, parent);
    auto* cont = llvm::BasicBlock::Create(*m->ctx, "invoke.cont", parent, curr->getNextNode());
    const bool voidRet = fnTy->getReturnType()->isVoidTy();
    auto* invoke = m->builder->CreateInvoke(
        fnTy, asVal(callee), cont, unwind, argv,
        voidRet ? llvm::Twine() : llvm::Twine(asStr(name, len)));
    if (auto* calleeFunction = llvm::dyn_cast<llvm::Function>(asVal(callee))) {
        invoke->setCallingConv(calleeFunction->getCallingConv());
    }
    m->builder->SetInsertPoint(cont);
    return invoke;
}

extern "C" VYX_API void* vyx_rt_build_call(void* h, void* fty, void* callee,
                                           const void* const* args, uint64_t n,
                                           const char* name, uint64_t len) {
    VYX_RT_PROF(BuildCall);
    std::vector<llvm::Value*> argv;
    argv.reserve(static_cast<size_t>(n));
    for (uint64_t i = 0; i < n; ++i) {
        argv.push_back(static_cast<llvm::Value*>(const_cast<void*>(args ? args[i] : nullptr)));
    }
    return vyx_rt_build_call_array(h, fty, callee, argv, name, len);
}

extern "C" VYX_API void* vyx_rt_build_invoke(void* h, void* fty, void* callee,
                                             const void* const* args, uint64_t n,
                                             const char* name, uint64_t len) {
    VYX_RT_PROF(BuildInvoke);
    std::vector<llvm::Value*> argv;
    argv.reserve(static_cast<size_t>(n));
    for (uint64_t i = 0; i < n; ++i) {
        argv.push_back(static_cast<llvm::Value*>(const_cast<void*>(args ? args[i] : nullptr)));
    }
    return vyx_rt_build_invoke_array(h, fty, callee, argv, name, len);
}

extern "C" VYX_API int32_t vyx_rt_track_eh_allocation(void* h, void* object,
                                                      void* release) {
    if (!h || !object) return -1;
    auto* m = asMod(h);
    auto* bb = m->builder ? m->builder->GetInsertBlock() : nullptr;
    if (!bb || !bb->getParent()->hasPersonalityFn()) return 0;
    auto* freeFn = release ? llvm::dyn_cast<llvm::Function>(asVal(release)) : m->mod->getFunction("free");
    if (!freeFn || !asVal(object)->getType()->isPointerTy()) {
        m->lastError = "shared unwind allocation requires a pointer and release function";
        return -1;
    }
    auto* action = vyx_eh_action(m, nullptr, freeFn);
    if (!action) return -1;
    m->ehReleases.insert(freeFn);
    auto* node = vyx_eh_new_node(m, bb->getParent());
    vyx_eh_push(m, node, asVal(object), action);
    return 0;
}

extern "C" VYX_API int32_t vyx_rt_track_eh_owner(
    void* h, void* object, void* destructor, void* release) {
    if (!h || !object || !destructor) return -1;
    auto* m = asMod(h);
    auto* bb = m->builder ? m->builder->GetInsertBlock() : nullptr;
    if (!bb || !bb->getParent()->hasPersonalityFn()) return 0;
    auto* dtor = llvm::dyn_cast<llvm::Function>(asVal(destructor));
    auto* freeFn = release ? llvm::dyn_cast<llvm::Function>(asVal(release)) : nullptr;
    if (!dtor || !asVal(object)->getType()->isPointerTy()) {
        m->lastError = "shared unwind owner requires a resolved complete object and MIR Drop";
        return -1;
    }
    auto* action = vyx_eh_action(m, dtor, freeFn);
    auto* freeAction = freeFn ? vyx_eh_action(m, nullptr, freeFn) : nullptr;
    if (!action || (freeFn && !freeAction)) return -1;
    m->ehDestructors.insert(dtor);
    if (freeFn) m->ehReleases.insert(freeFn);
    auto* node = vyx_eh_prepare_constructor(m, asVal(object));
    auto& b = *m->builder;
    b.CreateStore(action, vyx_eh_field(m, b, node, 3));
    if (freeAction) b.CreateStore(freeAction, vyx_eh_field(m, b, node, 4));
    return 0;
}

extern "C" VYX_API void* vyx_rt_build_owned_constructor(
    void* h, void* fty, void* callee, const void* const* args, uint64_t n,
    const char* name, uint64_t len, void* destructor, void* release,
    int32_t needs_invoke) {
    if (!h) return nullptr;
    auto* m = asMod(h);
    if (!fty || !callee || !args || n == 0 || !destructor) {
        m->lastError = "shared unwind constructor is missing its resolved owning MIR Drop";
        return nullptr;
    }
    auto* bb = m->builder ? m->builder->GetInsertBlock() : nullptr;
    if (!bb || !bb->getParent()->hasPersonalityFn()) {
        m->lastError = "owned constructor requires a shared unwind caller";
        return nullptr;
    }
    std::vector<llvm::Value*> argv;
    for (uint64_t i = 0; i < n; ++i) argv.push_back(asVal(const_cast<void*>(args[i])));
    if (!vyx_rt_checked_call_type(m, fty, argv, name, len, "owned_constructor")) return nullptr;
    auto* dtor = llvm::dyn_cast<llvm::Function>(asVal(destructor));
    auto* freeFn = release ? llvm::dyn_cast<llvm::Function>(asVal(release)) : nullptr;
    if (!dtor || !argv[0]->getType()->isPointerTy()) {
        m->lastError = "owned constructor requires a complete object receiver and destructor";
        return nullptr;
    }
    auto* inlineAction = vyx_eh_action(m, dtor, nullptr);
    auto* ownedAction = freeFn ? vyx_eh_action(m, dtor, freeFn) : inlineAction;
    if (!inlineAction || !ownedAction) return nullptr;
    m->ehDestructors.insert(dtor);
    auto* node = vyx_eh_prepare_constructor(m, argv[0]);
    auto* out = needs_invoke
        ? vyx_rt_build_invoke_array(h, fty, callee, argv, name, len)
        : vyx_rt_build_call_array(h, fty, callee, argv, name, len);
    if (!out) return nullptr;
    auto& b = *m->builder;
    auto* freeAction = b.CreateLoad(vyx_eh_ptr(m), vyx_eh_field(m, b, node, 4));
    auto* action = b.CreateSelect(b.CreateICmpNE(freeAction, vyx_eh_null(m)),
                                  ownedAction, inlineAction);
    b.CreateStore(action, vyx_eh_field(m, b, node, 3));
    return out;
}

extern "C" VYX_API void* vyx_rt_build_call1(void* h, void* fty, void* callee,
                                            void* a0,
                                            const char* name, uint64_t len) {
    VYX_RT_PROF(BuildCall);
    llvm::Value* argv[] = {asVal(a0)};
    return vyx_rt_build_call_array(h, fty, callee, llvm::ArrayRef<llvm::Value*>(argv, 1), name, len);
}

extern "C" VYX_API void* vyx_rt_build_call2(void* h, void* fty, void* callee,
                                            void* a0, void* a1,
                                            const char* name, uint64_t len) {
    VYX_RT_PROF(BuildCall);
    llvm::Value* argv[] = {asVal(a0), asVal(a1)};
    return vyx_rt_build_call_array(h, fty, callee, llvm::ArrayRef<llvm::Value*>(argv, 2), name, len);
}

extern "C" VYX_API void* vyx_rt_build_call3(void* h, void* fty, void* callee,
                                            void* a0, void* a1, void* a2,
                                            const char* name, uint64_t len) {
    VYX_RT_PROF(BuildCall);
    llvm::Value* argv[] = {asVal(a0), asVal(a1), asVal(a2)};
    return vyx_rt_build_call_array(h, fty, callee, llvm::ArrayRef<llvm::Value*>(argv, 3), name, len);
}

extern "C" VYX_API void* vyx_rt_build_call4(void* h, void* fty, void* callee,
                                            void* a0, void* a1, void* a2, void* a3,
                                            const char* name, uint64_t len) {
    VYX_RT_PROF(BuildCall);
    llvm::Value* argv[] = {asVal(a0), asVal(a1), asVal(a2), asVal(a3)};
    return vyx_rt_build_call_array(h, fty, callee, llvm::ArrayRef<llvm::Value*>(argv, 4), name, len);
}

extern "C" VYX_API void* vyx_rt_build_format_call(void* h,
                                                  void* snprintf_fty,
                                                  void* snprintf_fn,
                                                  void* alloc_fty,
                                                  void* alloc_fn,
                                                  void* fmt,
                                                  const void* const* extra_args,
                                                  uint64_t extra_count) {
    VYX_RT_PROF(BuildCall);
    if (!h || !snprintf_fty || !snprintf_fn || !alloc_fty || !alloc_fn || !fmt) {
        return nullptr;
    }
    auto* m = asMod(h);
    if (!m->builder) {
        m->lastError = "build_format_call: no builder";
        return nullptr;
    }
    auto* snprintfTy = llvm::dyn_cast<llvm::FunctionType>(asTy(snprintf_fty));
    auto* allocTy = llvm::dyn_cast<llvm::FunctionType>(asTy(alloc_fty));
    if (!snprintfTy || !allocTy) {
        m->lastError = "build_format_call: fty is not a FunctionType";
        return nullptr;
    }

    auto* i64Ty = llvm::Type::getInt64Ty(*m->ctx);
    auto* ptrTy = llvm::PointerType::get(*m->ctx, 0);
    std::vector<llvm::Value*> formatArgs;
    formatArgs.reserve(static_cast<std::size_t>(3 + extra_count));
    formatArgs.push_back(llvm::ConstantPointerNull::get(ptrTy));
    formatArgs.push_back(llvm::ConstantInt::get(i64Ty, 0));
    formatArgs.push_back(asVal(fmt));
    for (uint64_t i = 0; i < extra_count; ++i) {
        formatArgs.push_back(static_cast<llvm::Value*>(const_cast<void*>(extra_args ? extra_args[i] : nullptr)));
    }

    auto* needed = m->builder->CreateCall(snprintfTy, asVal(snprintf_fn), formatArgs, "fmt.needed");
    auto* needed64 = m->builder->CreateSExt(needed, i64Ty, "fmt.needed64");
    auto* allocSize = m->builder->CreateAdd(needed64,
                                            llvm::ConstantInt::get(i64Ty, 1),
                                            "fmt.size");
    auto* out = m->builder->CreateCall(allocTy, asVal(alloc_fn), {needed64}, "fmt.buf");

    std::vector<llvm::Value*> writeArgs;
    writeArgs.reserve(static_cast<std::size_t>(3 + extra_count));
    writeArgs.push_back(out);
    writeArgs.push_back(allocSize);
    writeArgs.push_back(asVal(fmt));
    for (uint64_t i = 0; i < extra_count; ++i) {
        writeArgs.push_back(static_cast<llvm::Value*>(const_cast<void*>(extra_args ? extra_args[i] : nullptr)));
    }
    m->builder->CreateCall(snprintfTy, asVal(snprintf_fn), writeArgs);
    return out;
}

extern "C" VYX_API void* vyx_rt_build_inline_asm(void* h, void* fty,
                                                 const char* asm_template, uint64_t asm_len,
                                                 const char* constraints, uint64_t constraints_len,
                                                 int32_t has_side_effects,
                                                 const void* const* args, uint64_t n,
                                                 const char* name, uint64_t len) {
    VYX_RT_PROF(BuildInlineAsm);
    if (!h || !fty) return nullptr;
    auto* m = asMod(h);
    if (!m->builder) { m->lastError = "build_inline_asm: no builder"; return nullptr; }
    auto* fnTy = llvm::dyn_cast<llvm::FunctionType>(asTy(fty));
    if (!fnTy) { m->lastError = "build_inline_asm: fty is not a FunctionType"; return nullptr; }
    std::vector<llvm::Value*> argv;
    argv.reserve(static_cast<size_t>(n));
    for (uint64_t i = 0; i < n; ++i) {
        argv.push_back(static_cast<llvm::Value*>(const_cast<void*>(args ? args[i] : nullptr)));
    }
    auto* ia = llvm::InlineAsm::get(fnTy,
                                    asStr(asm_template, asm_len),
                                    asStr(constraints, constraints_len),
                                    has_side_effects != 0);
    const bool voidRet = fnTy->getReturnType()->isVoidTy();
    return m->builder->CreateCall(fnTy, ia, argv,
                                  voidRet ? llvm::Twine() : llvm::Twine(asStr(name, len)));
}

/* =================================================================== */
/*   8. IR construction — casts                                        */
/* =================================================================== */

#define BUILD_CAST(NAME, METHOD)                                                                       \
    extern "C" VYX_API void* vyx_rt_build_##NAME(void* h, void* val, void* dst_ty,                     \
                                                 const char* name, uint64_t len) {                    \
        VYX_RT_PROF(BuildCast);                                                                         \
        if (!h || !val || !dst_ty) return nullptr;                                                     \
        auto* m = asMod(h);                                                                            \
        if (!m->builder) { m->lastError = "build_" #NAME ": no builder"; return nullptr; }             \
        return m->builder->METHOD(asVal(val), asTy(dst_ty), asStr(name, len));                         \
    }

BUILD_CAST(sext,       CreateSExt)
BUILD_CAST(zext,       CreateZExt)
BUILD_CAST(trunc,      CreateTrunc)
BUILD_CAST(bitcast,    CreateBitCast)
BUILD_CAST(ptr_to_int, CreatePtrToInt)
BUILD_CAST(int_to_ptr, CreateIntToPtr)

#undef BUILD_CAST

extern "C" VYX_API void* vyx_rt_build_select(void* h,
                                             void* cond,
                                             void* true_val,
                                             void* false_val,
                                             const char* name,
                                             uint64_t len) {
    VYX_RT_PROF(BuildSelect);
    if (!h || !cond || !true_val || !false_val) return nullptr;
    auto* m = asMod(h);
    if (!m->builder) {
        m->lastError = "build_select: no builder";
        return nullptr;
    }
    return m->builder->CreateSelect(asVal(cond),
                                    asVal(true_val),
                                    asVal(false_val),
                                    asStr(name, len));
}

static llvm::Value* vyx_rt_build_vyx_string_len_impl(VyxRtModule* m,
                                                     llvm::Value* value,
                                                     llvm::Value* emptyValue) {
    auto* i8Ty = llvm::Type::getInt8Ty(*m->ctx);
    auto* i64Ty = llvm::Type::getInt64Ty(*m->ctx);
    if (!value) {
        return llvm::ConstantInt::get(i64Ty, 0);
    }
    auto* ptrTy = llvm::PointerType::get(*m->ctx, 0);
    auto* nullPtr = llvm::ConstantPointerNull::get(ptrTy);
    auto* rawValue = value;
    auto* safeValue = rawValue;
    if (emptyValue) {
        auto* isNull = m->builder->CreateICmpEQ(rawValue, nullPtr);
        safeValue = m->builder->CreateSelect(isNull, emptyValue, rawValue);
    }
    auto* lenAddr = m->builder->CreateGEP(
        i8Ty,
        safeValue,
        llvm::ConstantInt::getSigned(i64Ty, -8),
        "vyx.str.len.addr");
    return m->builder->CreateLoad(i64Ty, lenAddr, "vyx.str.len");
}

extern "C" VYX_API void* vyx_rt_build_vyx_string_len(void* h, void* value, void* empty_value) {
    VYX_RT_PROF(BuildLoad);
    if (!h) return nullptr;
    auto* m = asMod(h);
    if (!m->builder) {
        m->lastError = "build_vyx_string_len: no builder";
        return nullptr;
    }
    return vyx_rt_build_vyx_string_len_impl(m,
                                            value ? asVal(value) : nullptr,
                                            empty_value ? asVal(empty_value) : nullptr);
}

extern "C" VYX_API void* vyx_rt_build_short_string_literal_equals(void* h,
                                                                  void* fn,
                                                                  void* other_val,
                                                                  const char* lit,
                                                                  int64_t lit_len,
                                                                  int32_t want_eq) {
    VYX_RT_PROF(BuildICmp);
    if (!h || !fn || !other_val || !lit || lit_len <= 0 || lit_len > 16) {
        return nullptr;
    }
    auto* m = asMod(h);
    if (!m->builder) {
        m->lastError = "build_short_string_literal_equals: no builder";
        return nullptr;
    }
    auto* curFn = asFn(fn);
    auto* i1Ty = llvm::Type::getInt1Ty(*m->ctx);
    auto* i8Ty = llvm::Type::getInt8Ty(*m->ctx);
    auto* i32Ty = llvm::Type::getInt32Ty(*m->ctx);
    auto* ptrTy = llvm::PointerType::get(*m->ctx, 0);
    auto* other = asVal(other_val);
    auto* falseConst = llvm::ConstantInt::get(i1Ty, 0);

    auto* nullPtr = llvm::ConstantPointerNull::get(ptrTy);
    auto* nonnull = m->builder->CreateICmpNE(other, nullPtr);
    auto* checkBB = llvm::BasicBlock::Create(*m->ctx, "strlit.eq.check", curFn);
    auto* falseBB = llvm::BasicBlock::Create(*m->ctx, "strlit.eq.null", curFn);
    auto* mergeBB = llvm::BasicBlock::Create(*m->ctx, "strlit.eq.end", curFn);
    if (auto* insertBB = m->builder->GetInsertBlock(); insertBB && !insertBB->getTerminator()) {
        m->builder->CreateCondBr(nonnull, checkBB, falseBB);
    }

    m->builder->SetInsertPoint(checkBB);
    llvm::Value* ok = llvm::ConstantInt::get(i1Ty, 1);
    for (int64_t i = 0; i < lit_len; ++i) {
        auto* idx = llvm::ConstantInt::get(i32Ty, static_cast<uint64_t>(i));
        auto* chPtr = m->builder->CreateGEP(i8Ty, other, idx);
        auto* ch = m->builder->CreateLoad(i8Ty, chPtr);
        auto* expected = llvm::ConstantInt::get(i8Ty, static_cast<unsigned char>(lit[i]));
        auto* same = m->builder->CreateICmpEQ(ch, expected);
        ok = m->builder->CreateAnd(ok, same);
    }
    auto* termIdx = llvm::ConstantInt::get(i32Ty, static_cast<uint64_t>(lit_len));
    auto* termPtr = m->builder->CreateGEP(i8Ty, other, termIdx);
    auto* termCh = m->builder->CreateLoad(i8Ty, termPtr);
    auto* termOk = m->builder->CreateICmpEQ(termCh, llvm::ConstantInt::get(i8Ty, 0));
    ok = m->builder->CreateAnd(ok, termOk);
    auto* checkEnd = m->builder->GetInsertBlock();
    m->builder->CreateBr(mergeBB);

    m->builder->SetInsertPoint(falseBB);
    auto* falseEnd = m->builder->GetInsertBlock();
    m->builder->CreateBr(mergeBB);

    m->builder->SetInsertPoint(mergeBB);
    auto* phi = m->builder->CreatePHI(i1Ty, 2);
    phi->addIncoming(ok, checkEnd);
    phi->addIncoming(falseConst, falseEnd);
    if (want_eq != 0) {
        return phi;
    }
    return m->builder->CreateICmpEQ(phi, falseConst);
}

extern "C" VYX_API void* vyx_rt_build_fast_string_equals(void* h,
                                                         void* fn,
                                                         void* lhs,
                                                         void* rhs,
                                                         void* memcmp_fty,
                                                         void* memcmp_fn,
                                                         void* empty_value,
                                                         int32_t want_eq) {
    VYX_RT_PROF(BuildICmp);
    if (!h || !fn || !lhs || !rhs || !memcmp_fty || !memcmp_fn) {
        return nullptr;
    }
    auto* m = asMod(h);
    if (!m->builder) {
        m->lastError = "build_fast_string_equals: no builder";
        return nullptr;
    }
    auto* memcmpTy = llvm::dyn_cast<llvm::FunctionType>(asTy(memcmp_fty));
    if (!memcmpTy) {
        m->lastError = "build_fast_string_equals: memcmp fty is not a FunctionType";
        return nullptr;
    }
    auto* curFn = asFn(fn);
    auto* i1Ty = llvm::Type::getInt1Ty(*m->ctx);
    auto* i8Ty = llvm::Type::getInt8Ty(*m->ctx);
    auto* i32Ty = llvm::Type::getInt32Ty(*m->ctx);
    auto* ptrTy = llvm::PointerType::get(*m->ctx, 0);
    auto* lhsVal = asVal(lhs);
    auto* rhsVal = asVal(rhs);
    auto* falseConst = llvm::ConstantInt::get(i1Ty, 0);

    auto* trueBB = llvm::BasicBlock::Create(*m->ctx, "str.eq.ptr", curFn);
    auto* nonnullBB = llvm::BasicBlock::Create(*m->ctx, "str.eq.nonnull", curFn);
    auto* prefixBB = llvm::BasicBlock::Create(*m->ctx, "str.eq.prefix", curFn);
    auto* runtimeBB = llvm::BasicBlock::Create(*m->ctx, "str.eq.runtime", curFn);
    auto* memcmpBB = llvm::BasicBlock::Create(*m->ctx, "str.eq.memcmp", curFn);
    auto* falseBB = llvm::BasicBlock::Create(*m->ctx, "str.eq.false", curFn);
    auto* mergeBB = llvm::BasicBlock::Create(*m->ctx, "str.eq.end", curFn);

    auto* ptrSame = m->builder->CreateICmpEQ(lhsVal, rhsVal);
    if (auto* insertBB = m->builder->GetInsertBlock(); insertBB && !insertBB->getTerminator()) {
        m->builder->CreateCondBr(ptrSame, trueBB, nonnullBB);
    }

    m->builder->SetInsertPoint(trueBB);
    auto* trueConst = llvm::ConstantInt::get(i1Ty, 1);
    auto* trueEnd = m->builder->GetInsertBlock();
    m->builder->CreateBr(mergeBB);

    m->builder->SetInsertPoint(nonnullBB);
    auto* nullPtr = llvm::ConstantPointerNull::get(ptrTy);
    auto* lhsNonnull = m->builder->CreateICmpNE(lhsVal, nullPtr);
    auto* rhsNonnull = m->builder->CreateICmpNE(rhsVal, nullPtr);
    auto* bothNonnull = m->builder->CreateAnd(lhsNonnull, rhsNonnull);
    m->builder->CreateCondBr(bothNonnull, prefixBB, falseBB);

    m->builder->SetInsertPoint(prefixBB);
    auto* zeroIdx = llvm::ConstantInt::get(i32Ty, 0);
    auto* lhsChPtr = m->builder->CreateGEP(i8Ty, lhsVal, zeroIdx);
    auto* rhsChPtr = m->builder->CreateGEP(i8Ty, rhsVal, zeroIdx);
    auto* lhsCh = m->builder->CreateLoad(i8Ty, lhsChPtr);
    auto* rhsCh = m->builder->CreateLoad(i8Ty, rhsChPtr);
    auto* firstSame = m->builder->CreateICmpEQ(lhsCh, rhsCh);
    m->builder->CreateCondBr(firstSame, runtimeBB, falseBB);

    m->builder->SetInsertPoint(runtimeBB);
    auto* emptyVal = empty_value ? asVal(empty_value) : nullptr;
    auto* lhsLen = vyx_rt_build_vyx_string_len_impl(m, lhsVal, emptyVal);
    auto* rhsLen = vyx_rt_build_vyx_string_len_impl(m, rhsVal, emptyVal);
    auto* lenSame = m->builder->CreateICmpEQ(lhsLen, rhsLen);
    m->builder->CreateCondBr(lenSame, memcmpBB, falseBB);

    m->builder->SetInsertPoint(memcmpBB);
    auto* cmpNative = m->builder->CreateCall(memcmpTy, asVal(memcmp_fn), {lhsVal, rhsVal, lhsLen});
    auto* runtimeEq = m->builder->CreateICmpEQ(cmpNative, llvm::ConstantInt::get(i32Ty, 0));
    auto* runtimeEnd = m->builder->GetInsertBlock();
    m->builder->CreateBr(mergeBB);

    m->builder->SetInsertPoint(falseBB);
    auto* falseEnd = m->builder->GetInsertBlock();
    m->builder->CreateBr(mergeBB);

    m->builder->SetInsertPoint(mergeBB);
    auto* phi = m->builder->CreatePHI(i1Ty, 3);
    phi->addIncoming(trueConst, trueEnd);
    phi->addIncoming(runtimeEq, runtimeEnd);
    phi->addIncoming(falseConst, falseEnd);
    if (want_eq != 0) {
        return phi;
    }
    return m->builder->CreateICmpEQ(phi, falseConst);
}

/* =================================================================== */
/*  10. Bitwise / unsigned / shift (7)                                  */
/* =================================================================== */

#define BUILD_BIN(NAME, METHOD)                                                                         \
    extern "C" VYX_API void* vyx_rt_build_##NAME(void* h, void* lhs, void* rhs,                         \
                                                 const char* name, uint64_t len) {                     \
        VYX_RT_PROF(BuildBin);                                                                          \
        if (!h || !lhs || !rhs) return nullptr;                                                         \
        auto* m = asMod(h);                                                                             \
        if (!m->builder) { m->lastError = "build_" #NAME ": no builder"; return nullptr; }              \
        return m->builder->METHOD(asVal(lhs), asVal(rhs), asStr(name, len));                            \
    }

BUILD_BIN(and,  CreateAnd)
BUILD_BIN(or,   CreateOr)
BUILD_BIN(xor,  CreateXor)
BUILD_BIN(shl,  CreateShl)
BUILD_BIN(lshr, CreateLShr)
BUILD_BIN(udiv, CreateUDiv)
BUILD_BIN(urem, CreateURem)

#undef BUILD_BIN

/* =================================================================== */
/*  11. Floating-point (8)                                              */
/* =================================================================== */

extern "C" VYX_API void* vyx_rt_float_type(void* h, int32_t bits) {
    VYX_RT_PROF(FloatType);
    if (!h) return nullptr;
    auto* m = asMod(h);
    if (bits == 32) return llvm::Type::getFloatTy(*m->ctx);
    if (bits == 64) return llvm::Type::getDoubleTy(*m->ctx);
    m->lastError = "float_type: bits must be 32 or 64";
    return nullptr;
}

static llvm::Value* alignFloatOperand(VyxRtModule* m, llvm::Value* value, llvm::Type* targetTy) {
    if (!m || !m->builder || !value || !targetTy) return value;
    auto* sourceTy = value->getType();
    if (sourceTy == targetTy) return value;
    if (!sourceTy->isFloatingPointTy() || !targetTy->isFloatingPointTy()) return value;
    if (sourceTy->isFloatTy() && targetTy->isDoubleTy()) {
        return m->builder->CreateFPExt(value, targetTy);
    }
    if (sourceTy->isDoubleTy() && targetTy->isFloatTy()) {
        return m->builder->CreateFPTrunc(value, targetTy);
    }
    return value;
}

static bool alignFloatPair(VyxRtModule* m, llvm::Value*& lhs, llvm::Value*& rhs) {
    if (!m || !m->builder || !lhs || !rhs) return false;
    auto* lhsTy = lhs->getType();
    auto* rhsTy = rhs->getType();
    if (lhsTy == rhsTy) return true;
    if (!lhsTy->isFloatingPointTy() || !rhsTy->isFloatingPointTy()) return false;
    if (lhsTy->isDoubleTy() || rhsTy->isDoubleTy()) {
        auto* targetTy = llvm::Type::getDoubleTy(*m->ctx);
        lhs = alignFloatOperand(m, lhs, targetTy);
        rhs = alignFloatOperand(m, rhs, targetTy);
        return lhs->getType() == rhs->getType();
    }
    if (lhsTy->isFloatTy() || rhsTy->isFloatTy()) {
        auto* targetTy = llvm::Type::getFloatTy(*m->ctx);
        lhs = alignFloatOperand(m, lhs, targetTy);
        rhs = alignFloatOperand(m, rhs, targetTy);
        return lhs->getType() == rhs->getType();
    }
    return false;
}

extern "C" VYX_API void* vyx_rt_build_fadd(void* h, void* lhs, void* rhs,
                                            const char* name, uint64_t len) {
    VYX_RT_PROF(BuildFloat);
    if (!h || !lhs || !rhs) return nullptr;
    auto* m = asMod(h);
    if (!m->builder) { m->lastError = "build_fadd: no builder"; return nullptr; }
    auto* lv = asVal(lhs);
    auto* rv = asVal(rhs);
    if (!alignFloatPair(m, lv, rv)) { m->lastError = "build_fadd: operands must be floating point values"; return nullptr; }
    return m->builder->CreateFAdd(lv, rv, asStr(name, len));
}

extern "C" VYX_API void* vyx_rt_build_fsub(void* h, void* lhs, void* rhs,
                                            const char* name, uint64_t len) {
    VYX_RT_PROF(BuildFloat);
    if (!h || !lhs || !rhs) return nullptr;
    auto* m = asMod(h);
    if (!m->builder) { m->lastError = "build_fsub: no builder"; return nullptr; }
    auto* lv = asVal(lhs);
    auto* rv = asVal(rhs);
    if (!alignFloatPair(m, lv, rv)) { m->lastError = "build_fsub: operands must be floating point values"; return nullptr; }
    return m->builder->CreateFSub(lv, rv, asStr(name, len));
}

extern "C" VYX_API void* vyx_rt_build_fmul(void* h, void* lhs, void* rhs,
                                            const char* name, uint64_t len) {
    VYX_RT_PROF(BuildFloat);
    if (!h || !lhs || !rhs) return nullptr;
    auto* m = asMod(h);
    if (!m->builder) { m->lastError = "build_fmul: no builder"; return nullptr; }
    auto* lv = asVal(lhs);
    auto* rv = asVal(rhs);
    if (!alignFloatPair(m, lv, rv)) { m->lastError = "build_fmul: operands must be floating point values"; return nullptr; }
    return m->builder->CreateFMul(lv, rv, asStr(name, len));
}

extern "C" VYX_API void* vyx_rt_build_fdiv(void* h, void* lhs, void* rhs,
                                            const char* name, uint64_t len) {
    VYX_RT_PROF(BuildFloat);
    if (!h || !lhs || !rhs) return nullptr;
    auto* m = asMod(h);
    if (!m->builder) { m->lastError = "build_fdiv: no builder"; return nullptr; }
    auto* lv = asVal(lhs);
    auto* rv = asVal(rhs);
    if (!alignFloatPair(m, lv, rv)) { m->lastError = "build_fdiv: operands must be floating point values"; return nullptr; }
    return m->builder->CreateFDiv(lv, rv, asStr(name, len));
}

extern "C" VYX_API void* vyx_rt_build_frem(void* h, void* lhs, void* rhs,
                                            const char* name, uint64_t len) {
    VYX_RT_PROF(BuildFloat);
    if (!h || !lhs || !rhs) return nullptr;
    auto* m = asMod(h);
    if (!m->builder) { m->lastError = "build_frem: no builder"; return nullptr; }
    auto* lv = asVal(lhs);
    auto* rv = asVal(rhs);
    if (!alignFloatPair(m, lv, rv)) { m->lastError = "build_frem: operands must be floating point values"; return nullptr; }
    return m->builder->CreateFRem(lv, rv, asStr(name, len));
}

extern "C" VYX_API void* vyx_rt_build_fcmp(void* h, int32_t pred,
                                            void* lhs, void* rhs,
                                            const char* name, uint64_t len) {
    VYX_RT_PROF(BuildFloat);
    if (!h || !lhs || !rhs) return nullptr;
    auto* m = asMod(h);
    if (!m->builder) { m->lastError = "build_fcmp: no builder"; return nullptr; }
    auto* lv = asVal(lhs);
    auto* rv = asVal(rhs);
    if (!alignFloatPair(m, lv, rv)) { m->lastError = "build_fcmp: operands must be floating point values"; return nullptr; }
    return m->builder->CreateFCmp(static_cast<llvm::CmpInst::Predicate>(pred),
                                  lv, rv, asStr(name, len));
}

extern "C" VYX_API void* vyx_rt_build_fp_to_si(void* h, void* val, void* dst_ty,
                                                const char* name, uint64_t len) {
    VYX_RT_PROF(BuildCast);
    if (!h || !val || !dst_ty) return nullptr;
    auto* m = asMod(h);
    if (!m->builder) { m->lastError = "build_fp_to_si: no builder"; return nullptr; }
    return m->builder->CreateFPToSI(asVal(val), asTy(dst_ty), asStr(name, len));
}

extern "C" VYX_API void* vyx_rt_build_si_to_fp(void* h, void* val, void* dst_ty,
                                                const char* name, uint64_t len) {
    VYX_RT_PROF(BuildCast);
    if (!h || !val || !dst_ty) return nullptr;
    auto* m = asMod(h);
    if (!m->builder) { m->lastError = "build_si_to_fp: no builder"; return nullptr; }
    return m->builder->CreateSIToFP(asVal(val), asTy(dst_ty), asStr(name, len));
}

extern "C" VYX_API void* vyx_rt_build_fp_ext(void* h, void* val, void* dst_ty,
                                              const char* name, uint64_t len) {
    VYX_RT_PROF(BuildCast);
    if (!h || !val || !dst_ty) return nullptr;
    auto* m = asMod(h);
    if (!m->builder) { m->lastError = "build_fp_ext: no builder"; return nullptr; }
    return m->builder->CreateFPExt(asVal(val), asTy(dst_ty), asStr(name, len));
}

extern "C" VYX_API void* vyx_rt_build_fp_trunc(void* h, void* val, void* dst_ty,
                                               const char* name, uint64_t len) {
    VYX_RT_PROF(BuildCast);
    if (!h || !val || !dst_ty) return nullptr;
    auto* m = asMod(h);
    if (!m->builder) { m->lastError = "build_fp_trunc: no builder"; return nullptr; }
    return m->builder->CreateFPTrunc(asVal(val), asTy(dst_ty), asStr(name, len));
}

/* =================================================================== */
/*  12. Aggregate value ops (2)                                         */
/* =================================================================== */

extern "C" VYX_API int32_t vyx_rt_value_is_struct(void* val) {
    if (!val) return 0;
    return asVal(val)->getType()->isStructTy() ? 1 : 0;
}

extern "C" VYX_API void* vyx_rt_build_extract_value(void* h, void* agg_val,
                                                    int32_t idx,
                                                    const char* name, uint64_t len) {
    VYX_RT_PROF(BuildExtractValue);
    if (!h || !agg_val) return nullptr;
    auto* m = asMod(h);
    if (!m->builder) { m->lastError = "build_extract_value: no builder"; return nullptr; }
    llvm::Value* v = asVal(agg_val);
    llvm::Type* ty = v->getType();
    // Method `self`, DCI `const T&` / `T*`, and foreign C handles are one
    // machine pointer. MIR still types them as two-word pointer handles.
    // extractvalue on a raw pointer aborts LLVM; treat the pointer as the
    // address word and a null metadata word.
    if (!ty->isAggregateType()) {
        if (ty->isPointerTy() && idx == 0) {
            return agg_val;
        }
        if (ty->isPointerTy() && idx == 1 && m->ctx) {
            return llvm::ConstantPointerNull::get(
                llvm::PointerType::getUnqual(*m->ctx));
        }
        m->lastError = "extract_value: value is not an aggregate";
        return nullptr;
    }
    if (auto* st = llvm::dyn_cast<llvm::StructType>(ty)) {
        if (idx < 0 || static_cast<unsigned>(idx) >= st->getNumElements()) {
            m->lastError = "extract_value: index out of range";
            return nullptr;
        }
    }
    return m->builder->CreateExtractValue(v,
                                          static_cast<unsigned>(idx),
                                          asStr(name, len));
}

extern "C" VYX_API void* vyx_rt_build_insert_value(void* h, void* agg_val,
                                                    void* elt, int32_t idx,
                                                    const char* name, uint64_t len) {
    VYX_RT_PROF(BuildInsertValue);
    if (!h || !agg_val || !elt) return nullptr;
    auto* m = asMod(h);
    if (!m->builder) { m->lastError = "build_insert_value: no builder"; return nullptr; }
    return m->builder->CreateInsertValue(asVal(agg_val), asVal(elt),
                                         static_cast<unsigned>(idx),
                                         asStr(name, len));
}

/* =================================================================== */
/*  13. Switch / PHI (4)                                                */
/* =================================================================== */

extern "C" VYX_API void* vyx_rt_build_switch(void* h, void* val, void* else_bb) {
    VYX_RT_PROF(BuildSwitch);
    if (!h || !val || !else_bb) return nullptr;
    auto* m = asMod(h);
    if (!m->builder) { m->lastError = "build_switch: no builder"; return nullptr; }
    return m->builder->CreateSwitch(asVal(val), asBB(else_bb));
}

extern "C" VYX_API void vyx_rt_add_case(void* h, void* switch_val,
                                         void* on_val, void* dest_bb) {
    VYX_RT_PROF(AddCase);
    if (!h || !switch_val || !on_val || !dest_bb) return;
    auto* sw = llvm::dyn_cast<llvm::SwitchInst>(asVal(switch_val));
    if (!sw) return;
    sw->addCase(llvm::cast<llvm::ConstantInt>(asVal(on_val)), asBB(dest_bb));
}

extern "C" VYX_API void* vyx_rt_build_phi(void* h, void* ty,
                                           const char* name, uint64_t len) {
    VYX_RT_PROF(BuildPhi);
    if (!h || !ty) return nullptr;
    auto* m = asMod(h);
    if (!m->builder) { m->lastError = "build_phi: no builder"; return nullptr; }
    return m->builder->CreatePHI(asTy(ty), 0, asStr(name, len));
}

extern "C" VYX_API void vyx_rt_add_incoming(void* h, void* phi,
                                             void* val, void* bb) {
    VYX_RT_PROF(AddIncoming);
    if (!h || !phi || !val || !bb) return;
    auto* pn = llvm::dyn_cast<llvm::PHINode>(asVal(phi));
    if (!pn) return;
    pn->addIncoming(asVal(val), asBB(bb));
}

/* =================================================================== */
/*  14. Named struct constant (1)                                       */
/* =================================================================== */

extern "C" VYX_API void* vyx_rt_const_named_struct(void* h, void* struct_ty,
                                                    const void* const* vals,
                                                    uint64_t n) {
    VYX_RT_PROF(ConstNamedStruct);
    if (!h || !struct_ty) return nullptr;
    auto* m = asMod(h);
    auto* stTy = llvm::dyn_cast<llvm::StructType>(asTy(struct_ty));
    if (!stTy) { m->lastError = "const_named_struct: not a StructType"; return nullptr; }
    std::vector<llvm::Constant*> cv;
    cv.reserve(static_cast<size_t>(n));
    for (uint64_t i = 0; i < n; ++i) {
        cv.push_back(llvm::cast<llvm::Constant>(asVal(const_cast<void*>(vals ? vals[i] : nullptr))));
    }
    return llvm::ConstantStruct::get(stTy, cv);
}

/* =================================================================== */
/*  15. Misc (2)                                                        */
/* =================================================================== */

extern "C" VYX_API void vyx_rt_set_alignment(void* h, void* val, int32_t bytes) {
    VYX_RT_PROF(SetAlignment);
    if (!h || !val) return;
    auto* m = asMod(h);
    if (bytes <= 0 || (bytes & (bytes - 1)) != 0) {
        m->lastError = "set_alignment: alignment must be a positive power of two";
        return;
    }
    auto* v = asVal(val);
    if (auto* ai = llvm::dyn_cast<llvm::AllocaInst>(v)) {
        ai->setAlignment(llvm::Align(static_cast<unsigned>(bytes)));
    } else if (auto* li = llvm::dyn_cast<llvm::LoadInst>(v)) {
        li->setAlignment(llvm::Align(static_cast<unsigned>(bytes)));
    } else if (auto* si = llvm::dyn_cast<llvm::StoreInst>(v)) {
        si->setAlignment(llvm::Align(static_cast<unsigned>(bytes)));
    } else {
        m->lastError = "set_alignment: value is not an alloca, load, or store";
    }
}

extern "C" VYX_API int32_t vyx_rt_set_alloca_inalloca(void* h, void* alloca_handle) {
    if (!h) return 1;
    auto* m = asMod(h);
    if (!alloca_handle) {
        m->lastError = "set_alloca_inalloca: alloca is null";
        return 2;
    }
    auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(asVal(alloca_handle));
    if (!alloca) {
        m->lastError = "set_alloca_inalloca: value is not a direct alloca";
        return 3;
    }
    alloca->setUsedWithInAlloca(true);
    return 0;
}

extern "C" VYX_API void* vyx_rt_build_unreachable(void* h) {
    VYX_RT_PROF(BuildUnreachable);
    if (!h) return nullptr;
    auto* m = asMod(h);
    if (!m->builder) { m->lastError = "build_unreachable: no builder"; return nullptr; }
    return m->builder->CreateUnreachable();
}

// Alias-scope metadata for typed-pointer audit hoisting.  The inline
// checked-address audit loads region metadata (a runtime-owned heap block)
// and the handle slot (an alloca).  Those accesses never overlap the payload
// object the pointer addresses, but opaque inttopot/ptrtoint round-trips hide
// that fact from BasicAA, so LICM must re-load every audit input on each loop
// iteration.  Declaring two disjoint alias domains — one for pointer-audit
// bookkeeping, one for user payload memory — lets LLVM prove the loop body's
// payload store cannot clobber the audit loads and hoist the whole check out
// of hot loops.
extern "C" VYX_API void* vyx_rt_alias_scope_domain(void* h) {
    VYX_RT_PROF(AliasScopeDomain);
    if (!h) return nullptr;
    auto* m = asMod(h);
    if (!m->mod) { m->lastError = "alias_scope_domain: no module"; return nullptr; }
    llvm::MDBuilder mdb(m->mod->getContext());
    // Returns a distinct node whose first operand is the name string —
    // exactly what the module verifier requires for an alias-scope domain.
    return mdb.createAnonymousAliasScopeDomain("vyx.ptr.audit");
}

// scope: kind 0 = pointer-audit bookkeeping, 1 = user payload memory.
extern "C" VYX_API void* vyx_rt_alias_scope(void* h, void* domain, int32_t kind) {
    VYX_RT_PROF(AliasScope);
    if (!h || !domain) return nullptr;
    auto* m = asMod(h);
    if (!m->mod) { m->lastError = "alias_scope: no module"; return nullptr; }
    auto* domainNode = static_cast<llvm::MDNode*>(domain);
    llvm::MDBuilder mdb(m->mod->getContext());
    const char* name = (kind == 0) ? "vyx.ptr.audit.meta" : "vyx.ptr.payload";
    return mdb.createAnonymousAliasScope(domainNode, name);
}

// Attach alias metadata to a load/store.  scope_kind selects which domain the
// access belongs to; other_kind is the disjoint domain declared noalias.
//   instr, scope_kind=0 -> alias.scope=audit,   noalias=payload
//   instr, scope_kind=1 -> alias.scope=payload, noalias=audit
extern "C" VYX_API void vyx_rt_set_alias_scopes(void* h, void* instr,
                                                void* domain, int32_t scope_kind) {
    VYX_RT_PROF(SetAliasScopes);
    if (!h || !instr || !domain) return;
    auto* m = asMod(h);
    if (!m->mod) { m->lastError = "set_alias_scopes: no module"; return; }
    auto* i = static_cast<llvm::Instruction*>(instr);
    auto* domainNode = static_cast<llvm::MDNode*>(domain);
    llvm::MDBuilder mdb(m->mod->getContext());
    const char* self_name  = (scope_kind == 0) ? "vyx.ptr.audit.meta" : "vyx.ptr.payload";
    const char* other_name = (scope_kind == 0) ? "vyx.ptr.payload"     : "vyx.ptr.audit.meta";
    auto* self_scope  = mdb.createAnonymousAliasScope(domainNode, self_name);
    auto* other_scope = mdb.createAnonymousAliasScope(domainNode, other_name);
    auto* self_list  = llvm::MDNode::get(m->mod->getContext(), {self_scope});
    auto* other_list = llvm::MDNode::get(m->mod->getContext(), {other_scope});
    i->setMetadata("alias.scope", self_list);
    i->setMetadata("noalias", other_list);
}

// Combined helper: build a plain store and attach payload-domain alias scopes
// in one step (vyx_rt_build_store returns void, so the Vyx side cannot grab
// the StoreInst to call set_alias_scopes afterwards).
extern "C" VYX_API void vyx_rt_set_alias_scopes_on_store(void* h, void* val,
                                                         void* ptr, void* domain,
                                                         int32_t scope_kind) {
    VYX_RT_PROF(SetAliasScopes);
    if (!h || !val || !ptr || !domain) return;
    auto* m = asMod(h);
    if (!m->builder || !m->mod) { m->lastError = "set_alias_scopes_on_store: no builder"; return; }
    auto* store = m->builder->CreateStore(asVal(val), asVal(ptr));
    if (store) vyx_rt_set_alias_scopes(h, store, domain, scope_kind);
}

/* =================================================================== */
/*   16. Type introspection                                            */
/* =================================================================== */

extern "C" VYX_API int32_t vyx_rt_is_array_type(void* ty) {
    VYX_RT_PROF(IsArrayType);
    if (!ty) return 0;
    struct Slot {
        void* ty;
        int32_t value;
    };
    static thread_local std::array<Slot, 4096> cache{};
    auto bits = reinterpret_cast<std::uintptr_t>(ty);
    bits ^= bits >> 12;
    bits ^= bits >> 24;
    const auto idx = static_cast<std::size_t>(bits) & (cache.size() - 1u);
    if (cache[idx].ty == ty) {
        return cache[idx].value;
    }
    const int32_t value = llvm::ArrayType::classof(asTy(ty)) ? 1 : 0;
    cache[idx] = Slot{ty, value};
    return value;
}

extern "C" VYX_API void* vyx_rt_array_elem_type(void* arr_ty) {
    VYX_RT_PROF(ArrayElemType);
    if (!arr_ty) return nullptr;
    auto* at = llvm::dyn_cast<llvm::ArrayType>(asTy(arr_ty));
    if (!at) return nullptr;
    return at->getElementType();
}

/* =================================================================== */
/*   8c. Debug info                                                     */
/* =================================================================== */

extern "C" VYX_API int32_t vyx_rt_enable_debug(void* h,
                                                 const char* filename,
                                                 uint64_t len) {
    if (!h) return -1;
    auto* m = asMod(h);
    if (!m || !m->mod || !m->ctx) return -1;
    if (m->debugEnabled) return 0;

    auto file = asStr(filename, len);
    if (file.empty()) file = "<unknown>";
    std::filesystem::path sourcePath(file);
    auto sourceName = sourcePath.filename().string();
    auto sourceDir = sourcePath.parent_path().string();
    if (sourceName.empty()) sourceName = file;
    if (sourceDir.empty()) sourceDir = ".";

    m->debugBuilder = std::make_unique<llvm::DIBuilder>(*m->mod);
    m->debugFile = m->debugBuilder->createFile(sourceName, sourceDir);
    m->debugCU = m->debugBuilder->createCompileUnit(
        llvm::dwarf::DW_LANG_C, m->debugFile, "Vyx Compiler bl-2026-03-24",
        false, "", 0);
    m->mod->addModuleFlag(llvm::Module::Warning, "Debug Info Version",
                          llvm::DEBUG_METADATA_VERSION);
#ifdef _WIN32
    m->mod->addModuleFlag(llvm::Module::Warning, "CodeView", 1);
#endif
    m->debugEnabled = true;
    m->debugFinalized = false;
    return 0;
}

static uint64_t debugTypeSizeBitsForRt(VyxRtModule* m, llvm::Type* ty) {
    if (!m || !m->mod || !ty || !ty->isSized()) return 0;
    const auto size = m->mod->getDataLayout().getTypeSizeInBits(ty);
    if (size.isScalable()) return 0;
    return size.getFixedValue();
}

static uint32_t debugTypeAlignBitsForRt(VyxRtModule* m, llvm::Type* ty) {
    if (!m || !m->mod || !ty || !ty->isSized()) return 0;
    return static_cast<uint32_t>(m->mod->getDataLayout().getABITypeAlign(ty).value() * 8u);
}

static std::vector<std::string> debugFieldNamesForRt(const char* names, uint64_t len) {
    std::vector<std::string> fields;
    if (!names || len == 0) return fields;
    std::string current;
    for (uint64_t i = 0; i < len; ++i) {
        const char ch = names[i];
        if (ch == '\0' || ch == ',') {
            if (!current.empty()) fields.push_back(current);
            current.clear();
            if (ch == '\0') break;
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) fields.push_back(current);
    return fields;
}

static llvm::DIType* debugTypeForRt(VyxRtModule* m,
                                    llvm::Type* ty,
                                    const std::string& sourceName,
                                    const std::vector<std::string>& fieldNames = {}) {
    if (!m || !m->debugBuilder || !ty || ty->isVoidTy()) return nullptr;
    auto name = sourceName;
    if (name.empty()) {
        if (ty->isIntegerTy()) {
            name = "i" + std::to_string(ty->getIntegerBitWidth());
        } else if (ty->isFloatTy()) {
            name = "f32";
        } else if (ty->isDoubleTy()) {
            name = "f64";
        } else if (auto* structTy = llvm::dyn_cast<llvm::StructType>(ty);
                   structTy && structTy->hasName()) {
            name = structTy->getName().str();
        } else {
            name = "value";
        }
    }

    if (auto* intTy = llvm::dyn_cast<llvm::IntegerType>(ty)) {
        unsigned encoding = llvm::dwarf::DW_ATE_signed;
        if (name == "bool") encoding = llvm::dwarf::DW_ATE_boolean;
        else if (!name.empty() && name.front() == 'u') encoding = llvm::dwarf::DW_ATE_unsigned;
        return m->debugBuilder->createBasicType(name, intTy->getBitWidth(), encoding);
    }
    if (ty->isFloatingPointTy()) {
        return m->debugBuilder->createBasicType(
            name, debugTypeSizeBitsForRt(m, ty), llvm::dwarf::DW_ATE_float);
    }
    if (ty->isPointerTy()) {
        const auto bits = m->mod->getDataLayout().getPointerSizeInBits(
            ty->getPointerAddressSpace());
        auto* pointee = m->debugBuilder->createUnspecifiedType(name + ".pointee");
        return m->debugBuilder->createPointerType(pointee, bits, bits, std::nullopt, name);
    }
    if (auto* arrayTy = llvm::dyn_cast<llvm::ArrayType>(ty)) {
        auto* elementTy = debugTypeForRt(m, arrayTy->getElementType(), "", {});
        auto* subrange = m->debugBuilder->getOrCreateSubrange(
            0, static_cast<int64_t>(arrayTy->getNumElements()));
        return m->debugBuilder->createArrayType(
            debugTypeSizeBitsForRt(m, ty), debugTypeAlignBitsForRt(m, ty), elementTy,
            m->debugBuilder->getOrCreateArray({subrange}));
    }
    if (auto* structTy = llvm::dyn_cast<llvm::StructType>(ty)) {
        std::vector<llvm::Metadata*> members;
        if (structTy->isSized() && !fieldNames.empty()) {
            const auto* layout = m->mod->getDataLayout().getStructLayout(structTy);
            const auto count = std::min<uint64_t>(fieldNames.size(), structTy->getNumElements());
            members.reserve(static_cast<size_t>(count));
            for (uint64_t index = 0; index < count; ++index) {
                auto* elementTy = structTy->getElementType(static_cast<unsigned>(index));
                auto* memberType = debugTypeForRt(m, elementTy, "", {});
                if (!memberType || !layout) continue;
                const auto offset = layout->getElementOffsetInBits(static_cast<unsigned>(index));
                members.push_back(m->debugBuilder->createMemberType(
                    m->debugFile, fieldNames[static_cast<size_t>(index)], m->debugFile, 0,
                    debugTypeSizeBitsForRt(m, elementTy), debugTypeAlignBitsForRt(m, elementTy),
                    offset, llvm::DINode::FlagZero, memberType));
            }
        }
        return m->debugBuilder->createStructType(
            m->debugFile, name, m->debugFile, 0, debugTypeSizeBitsForRt(m, ty),
            debugTypeAlignBitsForRt(m, ty), llvm::DINode::FlagZero, nullptr,
            m->debugBuilder->getOrCreateArray(members));
    }
    return m->debugBuilder->createUnspecifiedType(name);
}

extern "C" VYX_API int32_t vyx_rt_debug_set_function(void* h,
                                                      void* fn_handle,
                                                      const char* name,
                                                      uint64_t len,
                                                      int32_t line) {
    if (!h || !fn_handle) return -1;
    auto* m = asMod(h);
    auto* fn = asFn(fn_handle);
    if (!m || !m->debugBuilder || !m->debugFile || !fn) return 0;

    auto fnName = asStr(name, len);
    if (fnName.empty()) fnName = fn->getName().str();
    if (fn->getSubprogram()) return 0;
    std::vector<llvm::Metadata*> signature;
    signature.reserve(fn->arg_size() + 1);
    signature.push_back(debugTypeForRt(m, fn->getReturnType(), "", {}));
    for (auto& arg : fn->args()) {
        signature.push_back(debugTypeForRt(m, arg.getType(), "", {}));
    }
    if (fn->isVarArg()) {
        signature.push_back(m->debugBuilder->createUnspecifiedParameter());
    }
    auto* fnDI = m->debugBuilder->createFunction(
        m->debugFile, fnName, fn->getName(), m->debugFile, line,
        m->debugBuilder->createSubroutineType(
            m->debugBuilder->getOrCreateTypeArray(signature)),
        line, llvm::DINode::FlagZero, llvm::DISubprogram::SPFlagDefinition);
    fn->setSubprogram(fnDI);
    return 0;
}

extern "C" VYX_API int32_t vyx_rt_debug_declare_local(
    void* h,
    void* fn_handle,
    void* storage,
    void* llvm_ty,
    const char* name,
    uint64_t name_len,
    const char* type_name,
    uint64_t type_name_len,
    const char* field_names,
    uint64_t field_names_len,
    int32_t line,
    int32_t arg_index) {
    if (!h || !fn_handle || !storage || !llvm_ty) return -1;
    auto* m = asMod(h);
    auto* fn = asFn(fn_handle);
    auto* ty = asTy(llvm_ty);
    if (!m || !m->debugBuilder || !m->debugFile || !m->builder || !fn || !ty) return 0;
    auto* scope = fn->getSubprogram();
    auto* block = m->builder->GetInsertBlock();
    auto insertPoint = m->builder->GetInsertPoint();
    if (!scope || !block || insertPoint == block->end()) return 0;

    auto localName = asStr(name, name_len);
    if (localName.empty()) localName = "local";
    auto sourceTypeName = asStr(type_name, type_name_len);
    auto fields = debugFieldNamesForRt(field_names, field_names_len);
    auto* diType = debugTypeForRt(m, ty, sourceTypeName, fields);
    if (!diType) return 0;
    if (line <= 0) line = 1;

    llvm::DILocalVariable* variable = nullptr;
    if (arg_index > 0) {
        variable = m->debugBuilder->createParameterVariable(
            scope, localName, static_cast<unsigned>(arg_index), m->debugFile,
            static_cast<unsigned>(line), diType, true);
    } else {
        variable = m->debugBuilder->createAutoVariable(
            scope, localName, m->debugFile, static_cast<unsigned>(line), diType,
            true, llvm::DINode::FlagZero, debugTypeAlignBitsForRt(m, ty));
    }
    auto* location = llvm::DILocation::get(
        *m->ctx, static_cast<unsigned>(line), 1, scope);
    auto* storageValue = asVal(storage);
    auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(storageValue);
    // A static alloca is recorded by its frame index before instruction
    // selection. CodeView then uses the variable's lexical scope, which
    // includes the initializing source line. A dbg.assign is instead lowered
    // after the argument spills, leaving that line outside its range. Keep
    // the expression empty: DW_OP_deref makes CodeView describe a reference.
    if (alloca) {
        auto anchor = std::next(llvm::BasicBlock::iterator(alloca));
        m->debugBuilder->insertDeclare(
            storageValue, variable, m->debugBuilder->createExpression(),
            location, anchor);
        return 0;
    }
    m->debugBuilder->insertDeclare(
        storageValue, variable, m->debugBuilder->createExpression(), location,
        insertPoint);
    return 0;
}

extern "C" VYX_API void vyx_rt_debug_location(void* h,
                                               int32_t line,
                                               int32_t col) {
    if (!h || line <= 0) return;
    auto* m = asMod(h);
    if (!m || !m->debugBuilder || !m->debugCU || !m->builder || !m->ctx) return;

    llvm::DIScope* scope = nullptr;
    if (auto* bb = m->builder->GetInsertBlock()) {
        if (auto* fn = bb->getParent()) {
            if (fn->getSubprogram()) {
                scope = fn->getSubprogram();
            }
        }
    }
    if (!scope) return;
    m->builder->SetCurrentDebugLocation(
        llvm::DILocation::get(*m->ctx, line, col, scope));
}

extern "C" VYX_API void vyx_rt_debug_finalize(void* h) {
    finalizeDebugInfoForRt(asMod(h));
}

/* =================================================================== */
/*   9. Verify / output                                                */
/* =================================================================== */

extern "C" VYX_API int32_t vyx_rt_verify(void* h) {
    VYX_RT_PROF(Verify);
    if (!h) return -1;
    auto* m = asMod(h);
    return verifyModuleForRt(m, "verify") ? 0 : -1;
}

extern "C" VYX_API int32_t vyx_rt_optimize(void* h, int32_t opt_level) {
    VYX_RT_PROF(Optimize);
    if (!h) return -1;
    auto* m = asMod(h);
    return optimizeModuleForRt(m, opt_level) ? 0 : -1;
}

extern "C" VYX_API int32_t vyx_rt_print_to_file(void* h,
                                                const char* path, uint64_t len) {
    VYX_RT_PROF(PrintToFile);
    if (!h || !path || len == 0) return -1;
    auto* m = asMod(h);
    finalizeDebugInfoForRt(m);
    std::error_code ec;
    llvm::raw_fd_ostream out(asStr(path, len), ec, llvm::sys::fs::OF_None);
    if (ec) {
        m->lastError = ec.message();
        return -1;
    }
    m->mod->print(out, nullptr);
    return 0;
}

static int32_t emitModuleToFile(VyxRtModule* m, const char* path, uint64_t len,
                                int32_t opt_level, llvm::CodeGenFileType fileType) {
    if (!m || !path || len == 0) return -1;
    const int32_t normalizedLevel = normalizedOptimizationLevelFromInt(opt_level);
    if (normalizedLevel != 0 &&
        (!m->optimized || m->optimizedLevel != normalizedLevel)) {
        if (!optimizeModuleForRt(m, normalizedLevel)) {
            return -1;
        }
    }
    const char* phase = fileType == llvm::CodeGenFileType::AssemblyFile
                            ? "before-emit-assembly"
                            : "before-emit-object";
    if (!verifyModuleForRt(m, phase)) {
        return -1;
    }
    if (const char* dumpPath = std::getenv("VYX_RT_DUMP_OPT_IR")) {
        if (dumpPath[0] != '\0' && dumpPath[0] != '0') {
            std::error_code dumpEc;
            llvm::raw_fd_ostream dumpOut(dumpPath, dumpEc, llvm::sys::fs::OF_Text);
            if (!dumpEc) m->mod->print(dumpOut, nullptr);
        }
    }

    auto tm = createModuleTargetMachine(m, normalizedLevel);
    if (!tm) return -1;

    std::error_code ec;
    llvm::raw_fd_ostream out(asStr(path, len), ec, llvm::sys::fs::OF_None);
    if (ec) { m->lastError = ec.message(); return -1; }

    llvm::legacy::PassManager pm;
    if (tm->addPassesToEmitFile(pm, out, /*DwoOut=*/nullptr, fileType)) {
        m->lastError = fileType == llvm::CodeGenFileType::AssemblyFile
                           ? "TargetMachine cannot emit assembly"
                           : "TargetMachine cannot emit object file";
        return -1;
    }
    pm.run(*m->mod);
    out.flush();
    return 0;
}

extern "C" VYX_API int32_t vyx_rt_emit_object(void* h, const char* path, uint64_t len,
                                              int32_t opt_level) {
    VYX_RT_PROF(EmitObject);
    return emitModuleToFile(asMod(h), path, len, opt_level,
                            llvm::CodeGenFileType::ObjectFile);
}

extern "C" VYX_API int32_t vyx_rt_emit_codegen(void* h, const char* path, uint64_t len,
                                              int32_t opt_level, int32_t file_type) {
    VYX_RT_PROF(EmitObject);
    llvm::CodeGenFileType fileType = llvm::CodeGenFileType::ObjectFile;
    if (file_type == 1) {
        fileType = llvm::CodeGenFileType::AssemblyFile;
    } else if (file_type != 0) {
        if (h) asMod(h)->lastError = "unsupported code generation file type";
        return -1;
    }
    return emitModuleToFile(asMod(h), path, len, opt_level, fileType);
}


/* R6 ThinLTO pre-link: write bitcode with a module summary so the thin link
 * can plan cross-CGU imports.  The pre-link pipeline is the same per-module
 * one the native path uses; the canonical thin prelink split is a later
 * refinement (refactor doc §11, v1 note).  lld-link detects bitcode inputs
 * by content, so the file keeps its .obj name. */
extern "C" VYX_API int32_t vyx_rt_emit_bitcode(void* h, const char* path, uint64_t len,
                                               int32_t opt_level) {
    VYX_RT_PROF(EmitBitcode);
    if (!h || !path || len == 0) return -1;
    auto* m = asMod(h);
    const int32_t normalizedLevel = normalizedOptimizationLevelFromInt(opt_level);
    if (normalizedLevel != 0 &&
        (!m->optimized || m->optimizedLevel != normalizedLevel)) {
        if (!optimizeModuleForRt(m, normalizedLevel)) {
            return -1;
        }
    }
    if (!verifyModuleForRt(m, "before-emit-bitcode")) {
        return -1;
    }

    std::error_code ec;
    llvm::raw_fd_ostream out(asStr(path, len), ec, llvm::sys::fs::OF_None);
    if (ec) { m->lastError = ec.message(); return -1; }

    // buildModuleSummaryIndex asserts a non-null PSI and dereferences it in
    // release builds, so hand it a real (profile-less) summary object.
    llvm::ProfileSummaryInfo psi(*m->mod);
    llvm::ModuleSummaryIndex index =
        llvm::buildModuleSummaryIndex(*m->mod, /*GetBFICallback=*/nullptr,
                                      /*PSI=*/&psi);
    llvm::WriteBitcodeToFile(*m->mod, out,
                             /*ShouldPreserveUseListOrder=*/false,
                             &index,
                             /*GenerateHash=*/true);
    out.flush();
    return 0;
}

extern "C" VYX_API int32_t vyx_rt_jit_run_main_with_args(void* h,
                                                          int32_t opt_level,
                                                          const char* link_args,
                                                          uint64_t link_args_len) {
    if (!h) return -1;
    auto* m = asMod(h);
    if (!m || !m->mod || !m->ctx) {
        if (m) m->lastError = "JIT module is empty";
        return -1;
    }

    const int32_t normalizedLevel = normalizedOptimizationLevelFromInt(opt_level);
    if (normalizedLevel != 0 &&
        (!m->optimized || m->optimizedLevel != normalizedLevel)) {
        if (!optimizeModuleForRt(m, normalizedLevel)) {
            return -1;
        }
    }
    if (!verifyModuleForRt(m, "before-jit")) {
        return -1;
    }
    const auto objectPaths = objectFilesForLinkArgs(link_args, link_args_len);
    if (!prepareModuleForJit(m, !objectPaths.empty())) {
        return -1;
    }
    if (!addJitEntryWrapper(m) || !verifyModuleForRt(m, "after-jit-entry-wrapper")) {
        return -1;
    }
    if (!createModuleTargetMachine(m, normalizedLevel)) {
        return -1;
    }
    if (!loadJitLinkLibraries(m, link_args, link_args_len)) {
        return -1;
    }

    vyx_rt_register_jit_process_symbols();

    finalizeDebugInfoForRt(m);
    m->debugBuilder.reset();
    m->builder.reset();

    // The IR was built for the module's declared ABI. In particular, Windows
    // aggregate returns use a target-specific lowering. Construct the JIT
    // machine from that triple rather than independently detecting a host
    // ABI that may differ from the module.
    llvm::orc::JITTargetMachineBuilder jitTarget(
        llvm::Triple(m->mod->getTargetTriple()));
    jitTarget.setCodeGenOptLevel(
        codeGenOptimizationLevelFromInt(normalizedLevel));
    auto jitExpected = llvm::orc::LLJITBuilder()
                           .setJITTargetMachineBuilder(std::move(jitTarget))
                           .create();
    if (!jitExpected) {
        m->lastError = stringifyError(jitExpected.takeError());
        return -1;
    }
    auto jit = std::move(*jitExpected);

    auto generatorExpected =
        llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
            jit->getDataLayout().getGlobalPrefix());
    if (!generatorExpected) {
        m->lastError = stringifyError(generatorExpected.takeError());
        return -1;
    }
    jit->getMainJITDylib().addGenerator(std::move(*generatorExpected));

    std::string supportSymbolsError;
    if (!addJitAbsoluteSupportSymbols(*jit, supportSymbolsError)) {
        m->lastError = "JIT failed to define runtime support symbols: " + supportSymbolsError;
        return -1;
    }

    llvm::orc::ThreadSafeModule tsm(std::move(m->mod), std::move(m->ctx));
    if (auto err = jit->addIRModule(std::move(tsm))) {
        m->lastError = stringifyError(std::move(err));
        return -1;
    }

    for (const auto& objectPath : objectPaths) {
        auto bufferOrErr = llvm::MemoryBuffer::getFile(objectPath);
        if (!bufferOrErr) {
            m->lastError = "JIT failed to read object file '" + objectPath + "': "
                + bufferOrErr.getError().message();
            return -1;
        }
        if (auto err = jit->addObjectFile(std::move(*bufferOrErr))) {
            m->lastError = "JIT failed to add object file '" + objectPath + "': "
                + stringifyError(std::move(err));
            return -1;
        }
    }

    auto symExpected = jit->lookup("__vyx_jit_entry");
    if (!symExpected) {
        m->lastError = stringifyError(symExpected.takeError());
        return -1;
    }

    using EntryFn = int32_t();
    auto* entryFn = symExpected->toPtr<EntryFn>();
    if (!entryFn) {
        m->lastError = "JIT lookup returned null entry wrapper";
        return -1;
    }

    return entryFn();
}

extern "C" VYX_API int32_t vyx_rt_jit_run_main(void* h, int32_t opt_level) {
    return vyx_rt_jit_run_main_with_args(h, opt_level, nullptr, 0);
}
