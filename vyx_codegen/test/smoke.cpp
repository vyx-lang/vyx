/* vyx_codegen smoke — exercises the W2C P1+ surface (≥ 16 build_*
 *   helpers and the type/const/builder fixings) by emitting a small
 *   module to ./out.ll, then string-grepping the IR for the LLVM
 *   keywords each builder is supposed to materialise.
 *
 *   Module emitted (logically):
 *
 *     define i32 @main() { entry: ret i32 42 }
 *     define i32 @add(i32, i32) { entry: %r = add i32 %0, %1; ret i32 %r }
 *     define i32 @abs(i32) {
 *       entry: %neg = sub i32 0, %0
 *              %cmp = icmp ne i32 %0, 0
 *              br i1 %cmp, label %then, label %else
 *       then:  ret i32 %neg
 *       else:  ret i32 %0
 *     }
 *     define void @stash(ptr) {
 *       entry: %s = alloca i32
 *              store i32 7, ptr %s
 *              %v = load i32, ptr %s
 *              store i32 %v, ptr %0
 *              ret void
 *     }
 *     define i32 @driver() {
 *       entry: %r = call i32 @add(i32 1, i32 2)
 *              ret i32 %r
 *     }
 */
#include "vyx_codegen.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#define CHECK(cond, msg) \
    do { if (!(cond)) { std::fprintf(stderr, "smoke FAIL: %s\n", msg); return 1; } } while (0)

static std::string readAll(const char* path) {
    std::ifstream f(path);
    std::stringstream s; s << f.rdbuf(); return s.str();
}

static bool instructionLineContains(const std::string& ir,
                                    const char* instruction,
                                    const char* required) {
    const auto start = ir.find(instruction);
    if (start == std::string::npos) return false;
    const auto end = ir.find('\n', start);
    const auto found = ir.find(required, start);
    return found != std::string::npos && (end == std::string::npos || found < end);
}

static bool nextInstructionLineContains(const std::string& ir,
                                        const char* anchor,
                                        const char* instruction,
                                        const char* required) {
    const auto anchorStart = ir.find(anchor);
    if (anchorStart == std::string::npos) return false;
    const auto lineStart = ir.find('\n', anchorStart);
    if (lineStart == std::string::npos) return false;
    const auto nextStart = lineStart + 1;
    const auto nextEnd = ir.find('\n', nextStart);
    const auto instructionStart = ir.find(instruction, nextStart);
    if (instructionStart == std::string::npos ||
        (nextEnd != std::string::npos && instructionStart >= nextEnd)) {
        return false;
    }
    const auto requiredStart = ir.find(required, nextStart);
    return requiredStart != std::string::npos &&
           (nextEnd == std::string::npos || requiredStart < nextEnd);
}

static bool symbolAttributeGroupContains(const std::string& ir,
                                         const char* symbol,
                                         const char* attribute) {
    const std::string symbolNeedle = std::string("@") + symbol + "(";
    const auto symbolPos = ir.find(symbolNeedle);
    if (symbolPos == std::string::npos) return false;

    const auto lineEnd = ir.find('\n', symbolPos);
    const auto attrRef = ir.find('#', symbolPos);
    if (attrRef == std::string::npos ||
        (lineEnd != std::string::npos && attrRef > lineEnd)) {
        return false;
    }

    auto attrRefEnd = attrRef + 1;
    while (attrRefEnd < ir.size() &&
           ir[attrRefEnd] >= '0' && ir[attrRefEnd] <= '9') {
        ++attrRefEnd;
    }
    if (attrRefEnd == attrRef + 1) return false;

    const std::string groupNeedle =
        "attributes " + ir.substr(attrRef, attrRefEnd - attrRef) + " =";
    const auto groupPos = ir.find(groupNeedle);
    if (groupPos == std::string::npos) return false;
    const auto groupEnd = ir.find('\n', groupPos);
    return ir.find(attribute, groupPos) < groupEnd;
}

static bool symbolDeclarationContains(const std::string& ir,
                                      const char* symbol,
                                      const char* fragment) {
    const std::string needle = std::string("@") + symbol + "(";
    const auto symbolPos = ir.find(needle);
    if (symbolPos == std::string::npos) return false;
    const auto lineEnd = ir.find('\n', symbolPos);
    const auto fragmentPos = ir.find(fragment, symbolPos);
    return fragmentPos != std::string::npos &&
           (lineEnd == std::string::npos || fragmentPos < lineEnd);
}

static int build_runtime_memory_attr_decls(void* h, void* i32_ty) {
    void* i64_ty = vyx_rt_int_type(h, 64);
    void* ptr_ty = vyx_rt_ptr_ty(h);
    CHECK(i64_ty && ptr_ty, "string runtime declaration types");

    const void* ptr[] = { ptr_ty };
    const void* ptr_ptr[] = { ptr_ty, ptr_ty };
    const void* ptr_ptr_i64[] = { ptr_ty, ptr_ty, i64_ty };
    const void* ptr_i64_ptr_i64[] = { ptr_ty, i64_ty, ptr_ty, i64_ty };
    const void* ptr_i32[] = { ptr_ty, i32_ty };
    struct RuntimeDecl {
        const char* name;
        void* returnType;
        const void* const* params;
        uint64_t paramCount;
    };
    const RuntimeDecl declarations[] = {
        {"str_length",             i64_ty, ptr,                 1},
        {"ptr_read_u8",            i32_ty, ptr,                 1},
        {"ptr_read_i32",           i32_ty, ptr,                 1},
        {"ptr_read_i64",           i64_ty, ptr,                 1},
        {"vyx_string_len",         i64_ty, ptr,                 1},
        {"vyx_string_equals",      i32_ty, ptr_ptr,             2},
        {"vyx_string_equals_len",  i32_ty, ptr_ptr_i64,         3},
        {"vyx_string_compare",     i32_ty, ptr_ptr,             2},
        {"vyx_string_contains",    i32_ty, ptr_i64_ptr_i64,     4},
        {"vyx_string_starts_with", i32_ty, ptr_i64_ptr_i64,     4},
        {"vyx_string_ends_with",   i32_ty, ptr_i64_ptr_i64,     4},
        {"vyx_string_char_at",     i32_ty, ptr_i32,             2},
    };

    for (const auto& declaration : declarations) {
        void* type = vyx_rt_fn_type(h,
                                    declaration.returnType,
                                    declaration.params,
                                    declaration.paramCount,
                                    0);
        CHECK(type, "string runtime function type");
        CHECK(vyx_rt_add_function(h,
                                  declaration.name,
                                  std::strlen(declaration.name),
                                  type),
              "string runtime function declaration");
    }
    return 0;
}

/* --------- @main : returns 42 (legacy MVP path) ---------------------- */
static int build_main(void* h, void* i32_ty) {
    void* fty = vyx_rt_fn_type(h, i32_ty, nullptr, 0, 0);
    CHECK(fty, "fn_type(main)");
    void* fn  = vyx_rt_add_function(h, "main", 4, fty);
    CHECK(fn,  "add_function(main)");
    void* bb  = vyx_rt_append_block(h, fn, "entry", 5);
    CHECK(bb,  "append_block(main.entry)");
    vyx_rt_position_at_end(h, bb);
    void* k42 = vyx_rt_const_int(h, i32_ty, 42, 1);
    CHECK(k42, "const_int(42)");
    vyx_rt_build_ret(h, k42);
    return 0;
}

/* --------- @add(i32,i32) -> i32 : exercises build_add + get_param ---- */
static void* build_add_fn(void* h, void* i32_ty) {
    const void* params[2] = { i32_ty, i32_ty };
    void* fty = vyx_rt_fn_type(h, i32_ty, params, 2, 0);
    if (!fty) return nullptr;
    void* fn  = vyx_rt_add_function(h, "vyx_add", 7, fty);
    if (!fn)  return nullptr;
    if (vyx_rt_set_calling_convention(h, fn, "win64", 5) != 0) return nullptr;
    void* a   = vyx_rt_get_param(h, fn, 0);
    void* b   = vyx_rt_get_param(h, fn, 1);
    if (!a || !b) return nullptr;
    vyx_rt_set_param_name(h, a, "a", 1);
    vyx_rt_set_param_name(h, b, "b", 1);
    void* bb  = vyx_rt_append_block(h, fn, "entry", 5);
    if (!bb)  return nullptr;
    vyx_rt_position_at_end(h, bb);
    void* r = vyx_rt_build_add(h, a, b, "r", 1);
    if (!r) return nullptr;
    vyx_rt_build_ret(h, r);
    return fn;
}

/* --------- @abs(i32) -> i32 : exercises sub + icmp + cond_br + br ---- */
static int build_abs_fn(void* h, void* i32_ty) {
    const void* params[1] = { i32_ty };
    void* fty = vyx_rt_fn_type(h, i32_ty, params, 1, 0);
    CHECK(fty, "fn_type(abs)");
    void* fn  = vyx_rt_add_function(h, "vyx_abs", 7, fty);
    CHECK(fn,  "add_function(abs)");
    void* x   = vyx_rt_get_param(h, fn, 0);
    CHECK(x,   "get_param(abs.0)");
    vyx_rt_set_param_name(h, x, "x", 1);

    void* entry = vyx_rt_append_block(h, fn, "entry", 5);
    void* thenB = vyx_rt_append_block(h, fn, "then",  4);
    void* elseB = vyx_rt_append_block(h, fn, "else",  4);
    void* join  = vyx_rt_append_block(h, fn, "join",  4);
    CHECK(entry && thenB && elseB && join, "abs blocks");

    void* zero = vyx_rt_const_int(h, i32_ty, 0, 1);
    CHECK(zero, "abs zero");

    /* entry: branch on (x != 0). then: keep x. else: -x. join: phi-ish
     * via two stores+load (we don't expose CreatePHI yet, so route via
     * an alloca to also exercise alloca/load/store on the abs path).  */
    void* slot;
    vyx_rt_position_at_end(h, entry);
    slot = vyx_rt_build_alloca(h, i32_ty, "slot", 4);
    CHECK(slot, "abs alloca");
    void* cmp = vyx_rt_build_icmp(h, VYX_RT_ICMP_NE, x, zero, "cmp", 3);
    CHECK(cmp, "abs icmp");
    vyx_rt_build_cond_br(h, cmp, thenB, elseB);

    vyx_rt_position_at_end(h, thenB);
    vyx_rt_build_store(h, x, slot);
    vyx_rt_build_br(h, join);

    vyx_rt_position_at_end(h, elseB);
    void* neg = vyx_rt_build_sub(h, zero, x, "neg", 3);
    CHECK(neg, "abs sub");
    vyx_rt_build_store(h, neg, slot);
    vyx_rt_build_br(h, join);

    vyx_rt_position_at_end(h, join);
    void* v = vyx_rt_build_load(h, i32_ty, slot, "v", 1);
    CHECK(v, "abs load");
    vyx_rt_build_ret(h, v);
    return 0;
}

/* --------- @stash(ptr) : exercises ret_void and ptr_ty -------------- */
static int build_stash_fn(void* h, void* i32_ty) {
    void* ptr_ty = vyx_rt_ptr_ty(h);
    void* void_ty = vyx_rt_void_ty(h);
    CHECK(ptr_ty,  "ptr_ty");
    CHECK(void_ty, "void_ty");
    const void* params[1] = { ptr_ty };
    void* fty = vyx_rt_fn_type(h, void_ty, params, 1, 0);
    CHECK(fty, "fn_type(stash)");
    void* fn = vyx_rt_add_function(h, "vyx_stash", 9, fty);
    CHECK(fn, "add_function(stash)");
    void* p = vyx_rt_get_param(h, fn, 0);
    vyx_rt_set_param_name(h, p, "p", 1);
    void* bb = vyx_rt_append_block(h, fn, "entry", 5);
    vyx_rt_position_at_end(h, bb);
    void* slot = vyx_rt_build_alloca(h, i32_ty, "s", 1);
    CHECK(slot, "stash alloca");
    void* k7 = vyx_rt_const_int(h, i32_ty, 7, 1);
    vyx_rt_build_store(h, k7, slot);
    void* v = vyx_rt_build_load(h, i32_ty, slot, "v", 1);
    CHECK(v, "stash load");
    vyx_rt_build_store(h, v, p);
    vyx_rt_build_ret_void(h);
    return 0;
}

/* --------- @driver() : exercises build_call (with named result) ----- */
static int build_driver_fn(void* h, void* i32_ty, void* add_fn) {
    void* fty_void = vyx_rt_fn_type(h, i32_ty, nullptr, 0, 0);
    CHECK(fty_void, "fn_type(driver)");
    void* fn  = vyx_rt_add_function(h, "vyx_driver", 10, fty_void);
    CHECK(fn,  "add_function(driver)");
    void* bb  = vyx_rt_append_block(h, fn, "entry", 5);
    vyx_rt_position_at_end(h, bb);

    /* Re-derive the (i32,i32)->i32 FunctionType for the call site. */
    const void* params[2] = { i32_ty, i32_ty };
    void* call_fty = vyx_rt_fn_type(h, i32_ty, params, 2, 0);
    CHECK(call_fty, "call_fty");

    void* one = vyx_rt_const_int(h, i32_ty, 1, 1);
    void* two = vyx_rt_const_int(h, i32_ty, 2, 1);
    const void* args[2] = { one, two };
    void* r = vyx_rt_build_call(h, call_fty, add_fn, args, 2, "r", 1);
    CHECK(r, "build_call(add)");
    CHECK(vyx_rt_set_calling_convention(h, r, "WIN64", 5) == 0,
          "set_calling_convention(call)");
    vyx_rt_build_ret(h, r);
    return 0;
}

static int build_calling_convention_decls(void* h, void* i32_ty) {
    struct ConventionCase {
        const char* symbol;
        const char* convention;
    };
    static const ConventionCase cases[] = {
        {"cc_default", "default"},
        {"cc_c", "c"},
        {"cc_system", "system"},
        {"cc_cdecl", "cdecl"},
        {"cc_win64", "win64"},
        {"cc_sysv", "x86_64_sysv"},
        {"cc_stdcall", "stdcall"},
        {"cc_fastcall", "fastcall"},
        {"cc_thiscall", "thiscall"},
        {"cc_vectorcall", "vectorcall"},
        {"cc_cxx_free", "cxx_free_function"},
        {"cc_cxx_static", "cxx_static_method"},
        {"cc_cxx_method", "cxx_method"},
        {"cc_cxx_virtual", "cxx_virtual_method"},
        {"cc_cxx_ctor", "cxx_constructor"},
        {"cc_cxx_dtor", "cxx_destructor"},
    };

    void* fty = vyx_rt_fn_type(h, i32_ty, nullptr, 0, 0);
    CHECK(fty, "fn_type(cc declarations)");
    for (const auto& item : cases) {
        void* fn = vyx_rt_add_function(h, item.symbol, std::strlen(item.symbol), fty);
        CHECK(fn, "add_function(cc declaration)");
        CHECK(vyx_rt_set_calling_convention(
                  h, fn, item.convention, std::strlen(item.convention)) == 0,
              "set_calling_convention(function)");
    }
    void* explicitNounwind = vyx_rt_add_function(
        h, "explicit_nounwind", 17, fty);
    CHECK(explicitNounwind, "add_function(explicit nounwind declaration)");
    CHECK(vyx_rt_set_function_attr(h, explicitNounwind,
                                   "nounwind", 8, nullptr, 0) == 0,
          "set_function_attr(explicit nounwind)");
    void* explicitWriteonly = vyx_rt_add_function(
        h, "explicit_writeonly", 18, fty);
    CHECK(explicitWriteonly, "add_function(explicit writeonly declaration)");
    CHECK(vyx_rt_set_function_attr(h, explicitWriteonly,
                                   "writeonly", 9, nullptr, 0) == 0,
          "set_function_attr(explicit writeonly)");
    return 0;
}

/* --------- @lifetime : direct alloca lifetime markers -------------- */
static int build_lifetime_fn(void* h, void* i32_ty) {
    void* void_ty = vyx_rt_void_ty(h);
    CHECK(void_ty, "void_ty(lifetime)");
    void* fty = vyx_rt_fn_type(h, void_ty, nullptr, 0, 0);
    CHECK(fty, "fn_type(lifetime)");
    void* fn = vyx_rt_add_function(h, "vyx_lifetime", 12, fty);
    CHECK(fn, "add_function(lifetime)");
    void* bb = vyx_rt_append_block(h, fn, "entry", 5);
    CHECK(bb, "append_block(lifetime.entry)");
    vyx_rt_position_at_end(h, bb);

    void* slot = vyx_rt_build_alloca(h, i32_ty, "lifetime.slot", 13);
    CHECK(slot, "lifetime alloca");
    CHECK(vyx_rt_build_lifetime_start(h, slot), "build_lifetime_start");
    void* value = vyx_rt_const_int(h, i32_ty, 17, 1);
    CHECK(value, "lifetime value");
    vyx_rt_build_store(h, value, slot);
    CHECK(vyx_rt_build_load(h, i32_ty, slot, "lifetime.value", 14),
          "lifetime load");
    CHECK(vyx_rt_build_lifetime_end(h, slot), "build_lifetime_end");
    vyx_rt_build_ret_void(h);
    return 0;
}

static int build_param_attr_callsite(void* h, void* i32_ty) {
    void* ptr_ty = vyx_rt_ptr_ty(h);
    void* void_ty = vyx_rt_void_ty(h);
    const void* sink_params[1] = { ptr_ty };
    void* sink_type = vyx_rt_fn_type(h, void_ty, sink_params, 1, 0);
    CHECK(sink_type, "fn_type(abi sink)");
    void* sink = vyx_rt_add_function(h, "abi_sink", 8, sink_type);
    CHECK(sink, "add_function(abi sink)");
    CHECK(vyx_rt_set_param_abi_attr(h, sink, 0, "byval", 5, i32_ty, 4) == 0,
          "set_param_abi_attr(function)");

    void* caller_type = vyx_rt_fn_type(h, void_ty, nullptr, 0, 0);
    void* caller = vyx_rt_add_function(h, "abi_callsite", 12, caller_type);
    CHECK(caller, "add_function(abi callsite)");
    void* entry = vyx_rt_append_block(h, caller, "entry", 5);
    CHECK(entry, "append_block(abi callsite)");
    vyx_rt_position_at_end(h, entry);
    void* slot = vyx_rt_build_alloca(h, i32_ty, "abi.value", 9);
    CHECK(slot, "alloca(abi value)");
    vyx_rt_build_store(h, vyx_rt_const_int(h, i32_ty, 9, 1), slot);
    const void* args[1] = { slot };
    void* call = vyx_rt_build_call(h, sink_type, sink, args, 1, "", 0);
    CHECK(call, "build_call(abi sink)");
    CHECK(vyx_rt_set_param_abi_attr(h, call, 0, "byval", 5, i32_ty, 4) == 0,
          "set_param_abi_attr(callsite)");

    void* semantic_read_sink = vyx_rt_add_function(
        h, "semantic_read_sink", 18, sink_type);
    CHECK(semantic_read_sink, "add_function(semantic read sink)");
    CHECK(vyx_rt_set_param_abi_attr(h, semantic_read_sink, 0,
                                    "readonly", 8, nullptr, 0) == 0,
          "set_param_abi_attr(readonly function)");
    CHECK(vyx_rt_set_param_abi_attr(h, semantic_read_sink, 0,
                                    "captures(none)", 14, nullptr, 0) == 0,
          "set_param_abi_attr(captures none function)");
    void* semantic_read_call = vyx_rt_build_call(
        h, sink_type, semantic_read_sink, args, 1, "", 0);
    CHECK(semantic_read_call, "build_call(semantic read sink)");
    CHECK(vyx_rt_set_param_abi_attr(h, semantic_read_call, 0,
                                    "readonly", 8, nullptr, 0) == 0,
          "set_param_abi_attr(readonly callsite)");
    CHECK(vyx_rt_set_param_abi_attr(h, semantic_read_call, 0,
                                    "captures(none)", 14, nullptr, 0) == 0,
          "set_param_abi_attr(captures none callsite)");

    void* semantic_write_sink = vyx_rt_add_function(
        h, "semantic_write_sink", 19, sink_type);
    CHECK(semantic_write_sink, "add_function(semantic write sink)");
    CHECK(vyx_rt_set_param_abi_attr(h, semantic_write_sink, 0,
                                    "writeonly", 9, nullptr, 0) == 0,
          "set_param_abi_attr(writeonly function)");
    CHECK(vyx_rt_set_param_abi_attr(h, semantic_write_sink, 0,
                                    "captures(none)", 14, nullptr, 0) == 0,
          "set_param_abi_attr(write captures none function)");
    void* semantic_write_call = vyx_rt_build_call(
        h, sink_type, semantic_write_sink, args, 1, "", 0);
    CHECK(semantic_write_call, "build_call(semantic write sink)");
    CHECK(vyx_rt_set_param_abi_attr(h, semantic_write_call, 0,
                                    "writeonly", 9, nullptr, 0) == 0,
          "set_param_abi_attr(writeonly callsite)");
    CHECK(vyx_rt_set_param_abi_attr(h, semantic_write_call, 0,
                                    "captures(none)", 14, nullptr, 0) == 0,
          "set_param_abi_attr(write captures none callsite)");

    const void* scalar_params[1] = { i32_ty };
    void* scalar_type = vyx_rt_fn_type(h, void_ty, scalar_params, 1, 0);
    CHECK(scalar_type, "fn_type(scalar semantic sink)");
    void* scalar_sink = vyx_rt_add_function(h, "scalar_semantic_sink", 20, scalar_type);
    CHECK(scalar_sink, "add_function(scalar semantic sink)");
    CHECK(vyx_rt_set_param_abi_attr(h, scalar_sink, 0,
                                    "noalias", 7, nullptr, 0) != 0,
          "noalias must reject a scalar parameter");
    CHECK(vyx_rt_set_param_abi_attr(h, scalar_sink, 0,
                                    "nonnull", 7, nullptr, 0) != 0,
          "nonnull must reject a scalar parameter");
    CHECK(vyx_rt_set_param_abi_attr(h, scalar_sink, 0,
                                    "readonly", 8, nullptr, 0) != 0,
          "readonly must reject a scalar parameter");
    CHECK(vyx_rt_set_param_abi_attr(h, scalar_sink, 0,
                                    "writeonly", 9, nullptr, 0) != 0,
          "writeonly must reject a scalar parameter");
    CHECK(vyx_rt_set_param_abi_attr(h, scalar_sink, 0,
                                    "captures(none)", 14, nullptr, 0) != 0,
          "captures(none) must reject a scalar parameter");

    void* aligned_sink = vyx_rt_add_function(h, "abi_aligned_sink", 16, sink_type);
    CHECK(aligned_sink, "add_function(aligned ABI sink)");
    CHECK(vyx_rt_set_param_abi_attr(h, aligned_sink, 0, "align", 5, nullptr, 16) == 0,
          "set_param_abi_attr(aligned function)");
    void* aligned_call = vyx_rt_build_call(h, sink_type, aligned_sink, args, 1, "", 0);
    CHECK(aligned_call, "build_call(aligned ABI sink)");
    CHECK(vyx_rt_set_param_abi_attr(h, aligned_call, 0, "align", 5, nullptr, 16) == 0,
          "set_param_abi_attr(aligned callsite)");
    CHECK(vyx_rt_set_param_abi_attr(h, aligned_call, 0, "align", 5, nullptr, 3) != 0,
          "set_param_abi_attr must reject non-power-of-two alignment");
    CHECK(vyx_rt_set_param_abi_attr(h, aligned_call, 0, "align", 5, nullptr, 1ULL << 31) != 0,
          "set_param_abi_attr must reject unrepresentable alignment");

    void* i64_ty = vyx_rt_int_type(h, 64);
    CHECK(i64_ty, "int_type(64)");
    void* coerce_slot = vyx_rt_build_alloca(h, i64_ty, "abi.coerce.slot", 15);
    CHECK(coerce_slot, "alloca(aligned coerce slot)");
    vyx_rt_set_alignment(h, coerce_slot, 4);
    CHECK(vyx_rt_build_store_aligned(h,
                                     vyx_rt_const_int(h, i64_ty, 42, 1),
                                     coerce_slot,
                                     4),
          "build_store_aligned");
    CHECK(vyx_rt_build_load_aligned(h,
                                    i64_ty,
                                    coerce_slot,
                                    4,
                                    "abi.coerce.value",
                                    16),
          "build_load_aligned");

    // Normal bridge accesses must inherit an alloca's actual alignment too.
    // This is the path used by ordinary aggregate locals and DCI indirect
    // materialization after their slot alignment is overridden.
    void* inherited_slot = vyx_rt_build_alloca(h, i64_ty, "abi.inherited.slot", 18);
    CHECK(inherited_slot, "alloca(inherited alignment slot)");
    vyx_rt_set_alignment(h, inherited_slot, 4);
    vyx_rt_build_store(h, vyx_rt_const_int(h, i64_ty, 13, 1), inherited_slot);
    CHECK(vyx_rt_build_load(h,
                            i64_ty,
                            inherited_slot,
                            "abi.inherited.value",
                            19),
          "build_load(inherited alignment slot)");

    // A nested constant struct-field GEP chain has a statically known byte
    // offset from its alloca. The ordinary access must keep that proven
    // alignment, while a raw byte GEP from the same alloca must not inherit it.
    void* i8_ty = vyx_rt_int_type(h, 8);
    void* inner_ty = vyx_rt_struct_ty_named(h, "abi.aligned.inner", 17);
    void* outer_ty = vyx_rt_struct_ty_named(h, "abi.aligned.outer", 17);
    CHECK(i8_ty && inner_ty && outer_ty, "struct field alignment types");
    const void* inner_fields[2] = { i8_ty, i64_ty };
    const void* outer_fields[2] = { i8_ty, inner_ty };
    CHECK(vyx_rt_struct_ty_set_body(h, inner_ty, inner_fields, 2, 0) == 0,
          "set inner struct body");
    CHECK(vyx_rt_struct_ty_set_body(h, outer_ty, outer_fields, 2, 0) == 0,
          "set outer struct body");
    void* struct_slot = vyx_rt_build_alloca(h, outer_ty, "abi.struct.slot", 15);
    CHECK(struct_slot, "alloca(struct field alignment slot)");
    vyx_rt_set_alignment(h, struct_slot, 16);
    void* gep_zero = vyx_rt_const_int(h, i32_ty, 0, 0);
    void* gep_one = vyx_rt_const_int(h, i32_ty, 1, 0);
    void* gep_eight = vyx_rt_const_int(h, i32_ty, 8, 0);
    CHECK(gep_zero && gep_one && gep_eight, "struct field alignment indices");
    void* inner_field = vyx_rt_build_gep2(h, outer_ty, struct_slot, gep_zero, gep_one,
                                           "abi.inner.field", 15);
    CHECK(inner_field, "outer struct field GEP");
    void* aligned_field = vyx_rt_build_gep2(h, inner_ty, inner_field, gep_zero, gep_one,
                                             "abi.aligned.field", 17);
    CHECK(aligned_field, "nested struct field GEP");
    vyx_rt_build_store(h, vyx_rt_const_int(h, i64_ty, 29, 1), aligned_field);
    CHECK(vyx_rt_build_load(h, i64_ty, aligned_field, "abi.aligned.value", 17),
          "build_load(nested struct field)");

    void* raw_byte_field = vyx_rt_build_gep1(h, i8_ty, struct_slot, gep_eight,
                                              "abi.raw.byte", 12);
    CHECK(raw_byte_field, "raw byte GEP");
    vyx_rt_build_store(h, vyx_rt_const_int(h, i32_ty, 31, 1), raw_byte_field);
    CHECK(vyx_rt_build_load(h, i32_ty, raw_byte_field, "abi.raw.value", 13),
          "build_load(raw byte GEP)");

    CHECK(vyx_rt_build_store_aligned(h,
                                     vyx_rt_const_int(h, i64_ty, 7, 1),
                                     coerce_slot,
                                     3) == nullptr,
          "build_store_aligned must reject non-power-of-two alignment");
    vyx_rt_set_alignment(h, slot, 3);
    char alignment_error[256] = {0};
    vyx_rt_get_last_error(h, alignment_error, sizeof(alignment_error));
    CHECK(std::strstr(alignment_error, "set_alignment") != nullptr,
          "set_alignment must reject invalid alignment without asserting");
    vyx_rt_build_ret_void(h);
    return 0;
}

static int build_string_header_len(void* h) {
    void* i32_ty = vyx_rt_int_type(h, 32);
    void* i64_ty = vyx_rt_int_type(h, 64);
    void* void_ty = vyx_rt_void_ty(h);
    void* header_ty = vyx_rt_array_ty(h, i64_ty, 3);
    CHECK(i32_ty && i64_ty && void_ty && header_ty, "string header test types");

    void* fty = vyx_rt_fn_type(h, void_ty, nullptr, 0, 0);
    void* fn = vyx_rt_add_function(h, "string_header_len", 17, fty);
    CHECK(fn, "add_function(string_header_len)");
    void* entry = vyx_rt_append_block(h, fn, "entry", 5);
    CHECK(entry, "append_block(string_header_len.entry)");
    vyx_rt_position_at_end(h, entry);

    void* storage = vyx_rt_build_alloca(h, header_ty, "string.storage", 14);
    void* zero = vyx_rt_const_int(h, i32_ty, 0, 0);
    void* one = vyx_rt_const_int(h, i32_ty, 1, 0);
    CHECK(storage && zero && one, "string header test values");
    void* data = vyx_rt_build_gep2(h, header_ty, storage, zero, one,
                                   "string.data", 11);
    CHECK(data, "string header data pointer");
    CHECK(vyx_rt_build_vyx_string_len(h, data, nullptr),
          "build_vyx_string_len");
    vyx_rt_build_ret_void(h);
    return 0;
}

static int check_pointer_index_widths(void* host) {
    const int32_t hostBits = vyx_rt_pointer_index_bits(host);
    CHECK(hostBits == 32 || hostBits == 64,
          "host DataLayout pointer index width must be 32 or 64 bits");

    struct TargetCase {
        const char* triple;
        int32_t expectedBits;
    };
    static const TargetCase cases[] = {
        {"wasm32-unknown-unknown", 32},
        {"armv7-unknown-linux-gnueabihf", 32},
    };
    for (const auto& target : cases) {
        void* module = vyx_rt_module_new(target.triple,
                                         std::strlen(target.triple),
                                         target.triple,
                                         std::strlen(target.triple));
        CHECK(module, "cross-target module_new");
        const int32_t bits = vyx_rt_pointer_index_bits(module);
        if (bits != target.expectedBits) {
            std::fprintf(stderr,
                         "smoke FAIL: DataLayout index width for %s: expected %d, got %d\n",
                         target.triple,
                         target.expectedBits,
                         bits);
            vyx_rt_module_free(module);
            return 1;
        }
        vyx_rt_module_free(module);
    }

    constexpr const char* indexNarrowLayout = "e-p:64:64:64:32";
    void* layoutModule = vyx_rt_module_new("index-layout", 12, nullptr, 0);
    CHECK(layoutModule, "index-width layout module_new");
    if (vyx_rt_set_data_layout(layoutModule,
                               indexNarrowLayout,
                               std::strlen(indexNarrowLayout)) != 0) {
        std::fprintf(stderr, "smoke FAIL: set index-width test DataLayout\n");
        vyx_rt_module_free(layoutModule);
        return 1;
    }
    const int32_t narrowBits = vyx_rt_pointer_index_bits(layoutModule);
    if (narrowBits != 32) {
        std::fprintf(stderr,
                     "smoke FAIL: custom DataLayout index width: expected 32, got %d\n",
                     narrowBits);
        vyx_rt_module_free(layoutModule);
        return 1;
    }
    vyx_rt_module_free(layoutModule);
    return 0;
}

int main() {
    CHECK(vyx_rt_init() == 0, "init");
    void* h = vyx_rt_module_new("smoke", 5, nullptr, 0);
    CHECK(h, "module_new");
    if (check_pointer_index_widths(h)) return 1;
    void* i32_ty = vyx_rt_int_type(h, 32);
    CHECK(i32_ty, "int_type(32)");
    CHECK(vyx_rt_builder_new(h) == 0, "builder_new");

    if (build_main(h, i32_ty)) return 1;
    void* add_fn = build_add_fn(h, i32_ty);
    CHECK(add_fn, "build_add_fn");
    if (build_abs_fn(h, i32_ty))   return 1;
    if (build_stash_fn(h, i32_ty)) return 1;
    if (build_lifetime_fn(h, i32_ty)) return 1;
    if (build_driver_fn(h, i32_ty, add_fn)) return 1;
    if (build_calling_convention_decls(h, i32_ty)) return 1;
    if (build_param_attr_callsite(h, i32_ty)) return 1;
    if (build_string_header_len(h)) return 1;
    if (build_runtime_memory_attr_decls(h, i32_ty)) return 1;

    /* P1 ABI: const_real smoke — float type + double-precision constant.
     * Stays detached from any BB so it doesn't perturb the IR keyword
     * checks below; just proves the entry point is wired and rejects
     * non-float types via the lastError channel.                      */
    {
        void* f64_ty = vyx_rt_float_type(h, 64);
        CHECK(f64_ty, "float_type(64)");
        void* k_pi = vyx_rt_const_real(h, f64_ty, 3.14159);
        CHECK(k_pi, "const_real(3.14159)");

        void* bad = vyx_rt_const_real(h, i32_ty, 1.0);
        CHECK(bad == nullptr, "const_real(i32) must reject");
        char err[256] = {0};
        vyx_rt_get_last_error(h, err, sizeof(err));
        if (std::strstr(err, "const_real") == nullptr) {
            std::fprintf(stderr,
                "smoke FAIL: lastError not populated for bad const_real (got '%s')\n", err);
            return 1;
        }
    }

    /* Module-level verifier MUST pass after we wired every block. */
    if (vyx_rt_verify(h) != 0) {
        char err[512] = {0};
        vyx_rt_get_last_error(h, err, sizeof(err));
        std::fprintf(stderr, "smoke FAIL: verify -> %s\n", err);
        return 1;
    }

    CHECK(vyx_rt_print_to_file(h, "out.ll", 6) == 0, "print_to_file");

    auto ir = readAll("out.ll");
    CHECK(!ir.empty(), "out.ll non-empty");

    /* Every keyword below should appear at least once given the above
     * module shape; missing one indicates a regressed builder.        */
    static const char* kKeywords[] = {
        "ret i32 42",
        "@main",
        "@vyx_add",
        "@vyx_abs",
        "@vyx_stash",
        "@vyx_lifetime",
        "@vyx_driver",
        "define win64cc i32 @vyx_add",
        "call win64cc i32 @vyx_add",
        "declare win64cc i32 @cc_win64",
        "declare x86_64_sysvcc i32 @cc_sysv",
        "declare x86_stdcallcc i32 @cc_stdcall",
        "declare x86_fastcallcc i32 @cc_fastcall",
        "declare x86_thiscallcc i32 @cc_thiscall",
        "declare x86_vectorcallcc i32 @cc_vectorcall",
        "declare void @abi_sink(ptr byval(i32) align 4)",
        "call void @abi_sink(ptr byval(i32) align 4",
        "declare void @abi_aligned_sink(ptr align 16)",
        "call void @abi_aligned_sink(ptr align 16",
        "alloca i64, align 4",
        "store i64 42",
        "load i64",
        "getelementptr i8, ptr",
        "add i32",
        "sub i32",
        "icmp ne",
        "br i1",
        "br label",
        "alloca i32",
        "load i32",
        "store i32",
        "ret void",
        "@llvm.lifetime.start",
        "@llvm.lifetime.end",
    };
    for (auto* kw : kKeywords) {
        if (ir.find(kw) == std::string::npos) {
            std::fprintf(stderr, "smoke FAIL: missing keyword '%s' in out.ll\n", kw);
            return 1;
        }
    }
    CHECK(instructionLineContains(ir, "store i64 42", "align 4"),
          "aligned store must retain align 4 in IR");
    CHECK(instructionLineContains(ir, "load i64", "align 4"),
          "aligned load must retain align 4 in IR");
    CHECK(instructionLineContains(ir, "store i64 13", "align 4"),
          "ordinary store must inherit direct alloca alignment");
    CHECK(nextInstructionLineContains(ir, "store i64 13", "load i64", "align 4"),
          "ordinary load must inherit direct alloca alignment");
    CHECK(instructionLineContains(ir, "store i64 29", "align 16"),
          "ordinary store must inherit nested struct-field alignment");
    CHECK(nextInstructionLineContains(ir, "store i64 29", "load i64", "align 16"),
          "ordinary load must inherit nested struct-field alignment");
    CHECK(instructionLineContains(ir, "store i32 31", "align 4"),
          "ordinary byte-GEP store must not inherit alloca alignment");
    CHECK(nextInstructionLineContains(ir, "store i32 31", "load i32", "align 4"),
          "ordinary byte-GEP load must not inherit alloca alignment");
    const auto stringHeaderStart = ir.find("define void @string_header_len()");
    CHECK(stringHeaderStart != std::string::npos &&
              instructionLineContains(ir.substr(stringHeaderStart),
                                      "getelementptr i8, ptr", "i64 -8"),
          "string header lookup must use a byte GEP with offset -8");
    CHECK(ir.find("ptrtoint") == std::string::npos,
          "string header lookup must preserve pointer provenance");
    CHECK(ir.find("inttoptr") == std::string::npos,
          "string header lookup must not round-trip through integers");

    void* non_alloca = vyx_rt_const_int(h, i32_ty, 1, 0);
    CHECK(non_alloca, "lifetime rejection value");
    CHECK(vyx_rt_build_lifetime_start(h, non_alloca) == nullptr,
          "build_lifetime_start must reject non-alloca");
    char lifetime_err[256] = {0};
    vyx_rt_get_last_error(h, lifetime_err, sizeof(lifetime_err));
    CHECK(std::strstr(lifetime_err, "build_lifetime_start") != nullptr,
          "build_lifetime_start rejection must populate lastError");
    CHECK(vyx_rt_build_lifetime_end(h, non_alloca) == nullptr,
          "build_lifetime_end must reject non-alloca");
    std::memset(lifetime_err, 0, sizeof(lifetime_err));
    vyx_rt_get_last_error(h, lifetime_err, sizeof(lifetime_err));
    CHECK(std::strstr(lifetime_err, "build_lifetime_end") != nullptr,
          "build_lifetime_end rejection must populate lastError");

    static const char* kMemoryReadingRuntimeSymbols[] = {
        "str_length",
        "ptr_read_u8",
        "ptr_read_i32",
        "ptr_read_i64",
        "vyx_string_len",
        "vyx_string_equals",
        "vyx_string_equals_len",
        "vyx_string_compare",
        "vyx_string_contains",
        "vyx_string_starts_with",
        "vyx_string_ends_with",
        "vyx_string_char_at",
    };
    for (auto* symbol : kMemoryReadingRuntimeSymbols) {
        CHECK(ir.find(std::string("@") + symbol + "(") != std::string::npos,
              "vyx_string runtime declaration must be emitted");
        CHECK(!symbolAttributeGroupContains(ir, symbol, "memory(none)"),
              "memory-reading runtime declaration must not be memory(none)");
        CHECK(!symbolAttributeGroupContains(ir, symbol, "readnone"),
              "memory-reading runtime declaration must not be readnone");
        CHECK(!symbolAttributeGroupContains(ir, symbol, "memory(read)"),
              "vyx_string runtime declaration must include cache/profile writes");
        CHECK(!symbolAttributeGroupContains(ir, symbol, "readonly"),
              "vyx_string runtime declaration must not be readonly");
    }

    static const char* kProfiledReadSymbols[] = {
        "str_length",
        "ptr_read_u8",
        "ptr_read_i32",
        "ptr_read_i64",
    };
    for (auto* symbol : kProfiledReadSymbols) {
        CHECK(ir.find(std::string("@") + symbol + "(") != std::string::npos,
              "profiled runtime read declaration must be emitted");
        CHECK(!symbolAttributeGroupContains(ir, symbol, "memory(read)"),
              "profiled runtime read declaration must include profile writes");
        CHECK(!symbolAttributeGroupContains(ir, symbol, "readonly"),
              "profiled runtime read declaration must not be readonly");
    }

    CHECK(ir.find("@cc_default(") != std::string::npos,
          "default external declaration must be emitted");
    CHECK(!symbolAttributeGroupContains(ir, "cc_default", "nounwind"),
          "unknown external declaration must not default to nounwind");
    CHECK(symbolAttributeGroupContains(ir, "explicit_nounwind", "nounwind"),
          "explicit nounwind declaration must retain its ABI attribute");
    CHECK(symbolAttributeGroupContains(ir, "explicit_writeonly", "memory(write)") ||
              symbolAttributeGroupContains(ir, "explicit_writeonly", "writeonly"),
          "explicit writeonly declaration must retain its memory attribute");

    CHECK(symbolDeclarationContains(ir, "semantic_read_sink", "readonly"),
          "readonly parameter attribute must reach function declaration");
    CHECK(symbolDeclarationContains(ir, "semantic_read_sink", "captures(none)"),
          "captures(none) parameter attribute must reach function declaration");
    CHECK(symbolDeclarationContains(ir, "semantic_write_sink", "writeonly"),
          "writeonly parameter attribute must reach function declaration");
    CHECK(symbolDeclarationContains(ir, "semantic_write_sink", "captures(none)"),
          "write captures(none) attribute must reach function declaration");
    CHECK(instructionLineContains(ir, "call void @semantic_read_sink", "readonly") &&
              instructionLineContains(ir, "call void @semantic_read_sink", "captures(none)"),
          "semantic read callsite must retain parameter attributes");
    CHECK(instructionLineContains(ir, "call void @semantic_write_sink", "writeonly") &&
              instructionLineContains(ir, "call void @semantic_write_sink", "captures(none)"),
          "semantic write callsite must retain parameter attributes");

    /* Negative path: build_call with a non-FunctionType should set the
     * lastError channel without crashing, proving error reporting is
     * still wired through after the stub purge.                      */
    void* not_a_fn_ty = i32_ty;
    void* bogus = vyx_rt_build_call(h, not_a_fn_ty, add_fn, nullptr, 0, "x", 1);
    if (bogus != nullptr) {
        std::fprintf(stderr, "smoke FAIL: build_call w/ non-fn ty should return null\n");
        return 1;
    }
    char err[256] = {0};
    vyx_rt_get_last_error(h, err, sizeof(err));
    if (std::strstr(err, "build_call") == nullptr) {
        std::fprintf(stderr, "smoke FAIL: lastError not populated for bad build_call (got '%s')\n", err);
        return 1;
    }

    CHECK(vyx_rt_set_calling_convention(h, add_fn, "not_a_cc", 8) != 0,
          "unknown calling convention must reject");
    std::memset(err, 0, sizeof(err));
    vyx_rt_get_last_error(h, err, sizeof(err));
    if (std::strstr(err, "set_calling_convention") == nullptr) {
        std::fprintf(stderr,
            "smoke FAIL: lastError not populated for bad calling convention (got '%s')\n",
            err);
        return 1;
    }

    vyx_rt_module_free(h);
    vyx_rt_shutdown();
    std::fprintf(stdout,
        "vyx_codegen smoke OK — out.ll covers add/sub/icmp/br/cond_br/"
        "alloca/load/store/call/ret_void; verify pass; error channel live.\n");
    return 0;
}
