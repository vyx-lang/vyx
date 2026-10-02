/**
 *  vyx_codegen.h — C ABI for the Vyx self-host LLVM backend
 *  ---------------------------------------------------------
 *  This DLL is the bootstrap compiler's supported way to talk to LLVM.
 *  All exported entry points below take/return primitive C ABI types
 *  (int32_t / int64_t / uint64_t / void* / const char*+uint64_t pairs),
 *  so the Vyx side can declare them as ordinary `extern "C"` functions
 *  and call through `std/dll.vyx` + `std/ffi.vyx`.
 *
 *  Conventions
 *  -----------
 *   * Strings cross the boundary as `(const char* ptr, uint64_t len)`. A
 *     `len == 0` means "use strlen on ptr" except where noted.
 *   * Handles ("opaque pointers") are returned as `void*`. NULL on error.
 *   * Status codes returned as int32_t: 0 = success, non-zero = error
 *     (use `vyx_rt_get_last_error` for a message).
 *   * Bool crosses as int32_t (0/1).
 *   * The DLL owns one LLVMContext + Module + IRBuilder per "module
 *     handle"; the module handle is also the Vyx-side anchor for every
 *     subsequent call (`h` argument).
 *
 *  Entry-point count: 121.
 *
 *  The original bootstrap bridge started with a 55-entry bring-up surface.
 *  The current selfhost-yolo tree has grown that ABI to cover direct object
 *  emission, optimization, float operations, PHI/switch/select, inline asm,
 *  string helpers, bootstrap profiling hooks, and convenience fixed-arity
 *  wrappers used by the Vyx-side code generator.
 *  See docs/LLVM_FFI_SURVEY.md §4.A for the LLVM-C functions actually
 *  exercised inside the DLL implementation.
 */

#ifndef VYX_CODEGEN_H
#define VYX_CODEGEN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
#  ifdef VYX_CODEGEN_BUILD
#    define VYX_API __declspec(dllexport)
#  else
#    define VYX_API __declspec(dllimport)
#  endif
#else
#  define VYX_API __attribute__((visibility("default")))
#endif

/* ------------------------------------------------------------------- */
/*  Opaque handles                                                      */
/* ------------------------------------------------------------------- */
/* Every "thing" inside the DLL is a `void*` at the ABI; using typedefs
 * is purely documentary.                                                */
typedef struct vyx_rt_module_t       vyx_rt_module;
typedef struct vyx_rt_type_t         vyx_rt_type;
typedef struct vyx_rt_value_t        vyx_rt_value;
typedef struct vyx_rt_function_t     vyx_rt_function;
typedef struct vyx_rt_basic_block_t  vyx_rt_basic_block;

/* Linkage kinds (mirror LLVMLinkage enum's most useful subset).        */
#define VYX_RT_LINK_EXTERNAL  0
#define VYX_RT_LINK_INTERNAL  1
#define VYX_RT_LINK_PRIVATE   2
#define VYX_RT_LINK_LINKONCE_ODR 3
#define VYX_RT_LINK_WEAK_ODR 4

/* Integer comparison predicates — values must match LLVMIntPredicate
 * exactly (LLVM-C/Core.h): EQ=32, NE=33, UGT=34, UGE=35, ULT=36,
 * ULE=37, SGT=38, SGE=39, SLT=40, SLE=41.                             */
#define VYX_RT_ICMP_EQ   32
#define VYX_RT_ICMP_NE   33
#define VYX_RT_ICMP_UGT  34
#define VYX_RT_ICMP_UGE  35
#define VYX_RT_ICMP_ULT  36
#define VYX_RT_ICMP_ULE  37
#define VYX_RT_ICMP_SGT  38
#define VYX_RT_ICMP_SGE  39
#define VYX_RT_ICMP_SLT  40
#define VYX_RT_ICMP_SLE  41

/* ------------------------------------------------------------------- */
/*  1. Lifetime & module (8)                                            */
/* ------------------------------------------------------------------- */
VYX_API int32_t  vyx_rt_init(void);
VYX_API void     vyx_rt_shutdown(void);
VYX_API void*    vyx_rt_module_new(const char* name,    uint64_t name_len,
                                   const char* triple,  uint64_t triple_len);
VYX_API void     vyx_rt_module_free(void* h);
VYX_API int32_t  vyx_rt_set_target(void* h, const char* triple, uint64_t len);
/* LLVM relocation model: 0 = target default, 1 = static, 2 = PIC. */
/* Backend CPU/architecture, for example NVPTX `sm_120`. Empty restores the default. */
VYX_API int32_t  vyx_rt_set_target_cpu(void* h, const char* cpu, uint64_t len);
VYX_API int32_t  vyx_rt_set_relocation_model(void* h, int32_t model);
VYX_API int32_t  vyx_rt_set_data_layout(void* h, const char* dl, uint64_t len);
/* LLVM DataLayout address-space-0 index width, or 0 for an invalid handle. */
VYX_API int32_t  vyx_rt_pointer_index_bits(void* h);

/* ------------------------------------------------------------------- */
/*  2. Error reporting (1)                                              */
/* ------------------------------------------------------------------- */
/* Copies up to `cap` bytes of the last error into `buf` (without NUL
 * terminator) and returns the *required* length so callers can size
 * their buffer if cap was too small.                                  */
VYX_API uint64_t vyx_rt_get_last_error(void* h, char* buf, uint64_t cap);
VYX_API uint64_t vyx_rt_normalize_target_triple(const char* triple,
                                                 uint64_t triple_len,
                                                 char* buf,
                                                 uint64_t cap);
VYX_API uint64_t vyx_rt_default_target_triple(char* buf, uint64_t cap);
VYX_API uint64_t vyx_rt_validate_target_triple(const char* triple,
                                                uint64_t triple_len,
                                                char* buf,
                                                uint64_t cap);

/* ------------------------------------------------------------------- */
/*  3. Type construction (8)                                            */
/* ------------------------------------------------------------------- */
VYX_API void* vyx_rt_int_type(void* h, int32_t bits);            /* 1 / 8 / 16 / 32 / 64 */
VYX_API void* vyx_rt_ptr_ty(void* h);                            /* opaque ptr (LLVM 17+) */
VYX_API void* vyx_rt_void_ty(void* h);
VYX_API void* vyx_rt_fn_type(void* h,
                             void* ret_ty,
                             const void* const* params,
                             uint64_t n_params,
                             int32_t is_vararg);
VYX_API void* vyx_rt_array_ty(void* h, void* elem_ty, uint64_t count);
/* Fixed-width vector.  The ABI alignment of a vector is its size rounded up to
   a power of two (measured on this target: <4 x i64> -> 32, <8 x i64> -> 64,
   <16 x i64> -> 128), whereas integer types cap at the widest `iN:align`
   datalayout entry (i256 / i512 / [4 x i128] all report 16).  That makes a
   vector the only self-describing anchor for an over-aligned foreign record. */
VYX_API void* vyx_rt_vector_ty(void* h, void* elem_ty, uint64_t count);
VYX_API void* vyx_rt_struct_ty_named(void* h, const char* name, uint64_t name_len);
VYX_API int32_t vyx_rt_struct_ty_set_body(void* h, void* st,
                                          const void* const* fields,
                                          uint64_t n,
                                          int32_t packed);
VYX_API void* vyx_rt_get_struct_field_ty(void* h, void* st, uint64_t idx);
VYX_API int64_t vyx_rt_struct_field_offset(void* h, void* st, uint64_t idx);
VYX_API int64_t vyx_rt_type_alloc_size(void* h, void* ty);

/* ------------------------------------------------------------------- */
/*  4. Constants (5)                                                    */
/* ------------------------------------------------------------------- */
VYX_API void* vyx_rt_const_int(void* h, void* ty,
                               int64_t value,
                               int32_t sign_extend);
VYX_API void* vyx_rt_const_str(void* h,
                               const char* s, uint64_t len,
                               int32_t null_terminated);
VYX_API void* vyx_rt_const_vyx_string_ptr(void* h,
                                          const char* s, uint64_t len,
                                          const char* name, uint64_t name_len);
VYX_API void* vyx_rt_const_null(void* h, void* ty);
VYX_API void* vyx_rt_const_ptr_null(void* h, void* ptr_ty);
VYX_API void* vyx_rt_const_real(void* h, void* ty, double value);
VYX_API int64_t vyx_rt_parse_real_bits(void* h, const char* s, uint64_t len);

/* ------------------------------------------------------------------- */
/*  5. Functions / globals (7)                                          */
/* ------------------------------------------------------------------- */
VYX_API void* vyx_rt_add_function(void* h,
                                  const char* name, uint64_t name_len,
                                  void* fty);
VYX_API int32_t vyx_rt_set_function_attr(void* h, void* fn,
                                         const char* attr, uint64_t attr_len,
                                         const char* value, uint64_t value_len);
/* Apply a parameter ABI or semantic attribute to an LLVM Function or
 * CallBase. */
VYX_API int32_t vyx_rt_set_param_abi_attr(void* h, void* fn_or_call,
                                          uint64_t param_index,
                                          const char* attr, uint64_t attr_len,
                                          void* pointee_type,
                                          uint64_t alignment);
/* Set a canonical DCI calling convention on an LLVM Function or CallBase.
 * Accepted names are default/c/system/win64/x86_64_sysv/cdecl/stdcall/
 * fastcall/thiscall/vectorcall plus the cxx_* compatibility names. */
VYX_API int32_t vyx_rt_set_calling_convention(void* h, void* fn_or_call,
                                              const char* convention,
                                              uint64_t convention_len);
VYX_API void  vyx_rt_set_linkage(void* h, void* fn_or_global, int32_t linkage_kind);
VYX_API void* vyx_rt_get_param(void* h, void* fn, uint64_t idx);
VYX_API void  vyx_rt_set_param_name(void* h, void* param,
                                    const char* name, uint64_t len);
VYX_API void* vyx_rt_add_global(void* h, void* ty,
                                const char* name, uint64_t name_len);
VYX_API void  vyx_rt_set_initializer(void* h, void* gv, void* const_val);
VYX_API void  vyx_rt_set_global_constant(void* h, void* gv, int32_t is_const);

/* ------------------------------------------------------------------- */
/*  6. Builder & basic blocks (5)                                       */
/* ------------------------------------------------------------------- */
/* The DLL keeps a per-module IRBuilder behind the `h` handle once
 * `vyx_rt_builder_new` has been called; `vyx_rt_module_free` disposes
 * it together with the module.                                         */
VYX_API int32_t vyx_rt_builder_new(void* h);
VYX_API void*   vyx_rt_append_block(void* h, void* fn,
                                    const char* name, uint64_t name_len);
VYX_API void    vyx_rt_position_at_end(void* h, void* bb);
VYX_API void    vyx_rt_position_before(void* h, void* instruction);
VYX_API void*   vyx_rt_get_insert_block(void* h);
VYX_API void    vyx_rt_clear_insertion(void* h);

/* ------------------------------------------------------------------- */
/*  7. IR construction — arithmetic / compare / control flow (15)       */
/* ------------------------------------------------------------------- */
VYX_API void* vyx_rt_build_add (void* h, void* lhs, void* rhs, const char* name, uint64_t len);
/* Sequentially-consistent atomic add. Returns the value before the add. */
VYX_API void* vyx_rt_build_atomicrmw_add(void* h, void* ptr, void* value,
                                          const char* name, uint64_t len);
/* Sequentially-consistent atomic load. */
VYX_API void* vyx_rt_build_atomic_load(void* h, void* ty, void* ptr,
                                        const char* name, uint64_t len);
/* Sequentially-consistent compare/exchange. Returns the observed value. */
VYX_API void* vyx_rt_build_atomic_cmpxchg(void* h, void* ptr,
                                          void* expected, void* desired,
                                          const char* name, uint64_t len);
VYX_API void* vyx_rt_build_sub (void* h, void* lhs, void* rhs, const char* name, uint64_t len);
VYX_API void* vyx_rt_build_mul (void* h, void* lhs, void* rhs, const char* name, uint64_t len);
VYX_API void* vyx_rt_build_sdiv(void* h, void* lhs, void* rhs, const char* name, uint64_t len);
VYX_API void* vyx_rt_build_srem(void* h, void* lhs, void* rhs, const char* name, uint64_t len);
VYX_API void* vyx_rt_build_icmp(void* h, int32_t pred,
                                void* lhs, void* rhs,
                                const char* name, uint64_t len);
VYX_API void  vyx_rt_build_br      (void* h, void* dest_bb);
VYX_API void  vyx_rt_build_cond_br (void* h, void* cond, void* then_bb, void* else_bb);
VYX_API void  vyx_rt_build_ret     (void* h, void* val);
VYX_API void  vyx_rt_build_ret_void(void* h);
VYX_API void* vyx_rt_build_alloca(void* h, void* ty, const char* name, uint64_t len);
/* Lifetime intrinsics deliberately accept only a direct alloca handle. The
 * bridge derives the LLVM intrinsic form; callers do not provide a size. */
VYX_API void* vyx_rt_build_lifetime_start(void* h, void* alloca);
VYX_API void* vyx_rt_build_lifetime_end  (void* h, void* alloca);
VYX_API void* vyx_rt_build_load  (void* h, void* ty, void* ptr, const char* name, uint64_t len);
VYX_API void* vyx_rt_build_store (void* h, void* val, void* ptr);
VYX_API void* vyx_rt_build_store_instruction(void* h, void* val, void* ptr);
VYX_API void* vyx_rt_build_load_aligned(void* h, void* ty, void* ptr,
                                        int32_t alignment,
                                        const char* name, uint64_t len);
VYX_API void* vyx_rt_build_store_aligned(void* h, void* val, void* ptr,
                                         int32_t alignment);
VYX_API void* vyx_rt_build_gep   (void* h, void* ty, void* ptr,
                                  const void* const* idxs, uint64_t n,
                                  const char* name, uint64_t len);
VYX_API void* vyx_rt_build_gep1  (void* h, void* ty, void* ptr,
                                  void* i0,
                                  const char* name, uint64_t len);
VYX_API void* vyx_rt_build_gep2  (void* h, void* ty, void* ptr,
                                  void* i0, void* i1,
                                  const char* name, uint64_t len);
VYX_API void* vyx_rt_build_call  (void* h, void* fty, void* callee,
                                  const void* const* args, uint64_t n,
                                  const char* name, uint64_t len);
VYX_API void* vyx_rt_build_invoke(void* h, void* fty, void* callee,
                                  const void* const* args, uint64_t n,
                                  const char* name, uint64_t len);
VYX_API int32_t vyx_rt_track_eh_allocation(void* h, void* object, void* release);
VYX_API int32_t vyx_rt_track_eh_owner(void* h, void* object, void* destructor, void* release);
VYX_API void* vyx_rt_build_owned_constructor(
    void* h, void* fty, void* callee, const void* const* args, uint64_t n,
    const char* name, uint64_t len, void* destructor, void* release,
    int32_t needs_invoke);
VYX_API void* vyx_rt_build_call1(void* h, void* fty, void* callee,
                                 void* a0,
                                 const char* name, uint64_t len);
VYX_API void* vyx_rt_build_call2(void* h, void* fty, void* callee,
                                 void* a0, void* a1,
                                 const char* name, uint64_t len);
VYX_API void* vyx_rt_build_call3(void* h, void* fty, void* callee,
                                 void* a0, void* a1, void* a2,
                                 const char* name, uint64_t len);
VYX_API void* vyx_rt_build_call4(void* h, void* fty, void* callee,
                                 void* a0, void* a1, void* a2, void* a3,
                                 const char* name, uint64_t len);
VYX_API void* vyx_rt_build_format_call(void* h,
                                       void* snprintf_fty,
                                       void* snprintf_fn,
                                       void* alloc_fty,
                                       void* alloc_fn,
                                       void* fmt,
                                       const void* const* extra_args,
                                       uint64_t extra_count);
VYX_API void* vyx_rt_build_vyx_string_len(void* h, void* value, void* empty_value);
VYX_API void* vyx_rt_build_short_string_literal_equals(void* h,
                                                       void* fn,
                                                       void* other_val,
                                                       const char* lit,
                                                       int64_t lit_len,
                                                       int32_t want_eq);
VYX_API void* vyx_rt_build_fast_string_equals(void* h,
                                              void* fn,
                                              void* lhs,
                                              void* rhs,
                                              void* memcmp_fty,
                                              void* memcmp_fn,
                                              void* empty_value,
                                              int32_t want_eq);
VYX_API void* vyx_rt_build_inline_asm(void* h, void* fty,
                                      const char* asm_template, uint64_t asm_len,
                                      const char* constraints, uint64_t constraints_len,
                                      int32_t has_side_effects,
                                      const void* const* args, uint64_t n,
                                      const char* name, uint64_t len);

/* ------------------------------------------------------------------- */
/*  8. IR construction — casts (6)                                      */
/* ------------------------------------------------------------------- */
VYX_API void* vyx_rt_build_sext      (void* h, void* val, void* dst_ty, const char* name, uint64_t len);
VYX_API void* vyx_rt_build_zext      (void* h, void* val, void* dst_ty, const char* name, uint64_t len);
VYX_API void* vyx_rt_build_trunc     (void* h, void* val, void* dst_ty, const char* name, uint64_t len);
VYX_API void* vyx_rt_build_bitcast   (void* h, void* val, void* dst_ty, const char* name, uint64_t len);
VYX_API void* vyx_rt_build_ptr_to_int(void* h, void* val, void* dst_ty, const char* name, uint64_t len);
VYX_API void* vyx_rt_build_int_to_ptr(void* h, void* val, void* dst_ty, const char* name, uint64_t len);
VYX_API void* vyx_rt_build_select    (void* h, void* cond, void* true_val, void* false_val,
                                      const char* name, uint64_t len);

/* ------------------------------------------------------------------- */
/*  8c. Debug info (5)                                                  */
/* ------------------------------------------------------------------- */
VYX_API int32_t vyx_rt_enable_debug(void* h, const char* filename, uint64_t len);
VYX_API int32_t vyx_rt_debug_set_function(void* h, void* fn_handle,
                                           const char* name, uint64_t len,
                                           int32_t line);
VYX_API int32_t vyx_rt_debug_declare_local(void* h, void* fn_handle,
                                           void* storage, void* llvm_ty,
                                           const char* name, uint64_t name_len,
                                           const char* type_name, uint64_t type_name_len,
                                           const char* field_names, uint64_t field_names_len,
                                           int32_t line, int32_t arg_index);
VYX_API void    vyx_rt_debug_location(void* h, int32_t line, int32_t col);
VYX_API void    vyx_rt_debug_finalize(void* h);

/* ------------------------------------------------------------------- */
/*  9. Verify / output (4)                                              */
/* ------------------------------------------------------------------- */
VYX_API int32_t vyx_rt_verify(void* h);                                  /* 0 ok */
VYX_API int32_t vyx_rt_optimize(void* h, int32_t opt_level);
VYX_API int32_t vyx_rt_print_to_file(void* h, const char* path, uint64_t len);
VYX_API int32_t vyx_rt_emit_object  (void* h, const char* path, uint64_t len, int32_t opt_level);
/* 0 = object, 1 = assembly. NVPTX uses assembly to emit textual PTX. */
VYX_API int32_t vyx_rt_emit_codegen (void* h, const char* path, uint64_t len, int32_t opt_level, int32_t file_type);
VYX_API int32_t vyx_rt_emit_bitcode (void* h, const char* path, uint64_t len, int32_t opt_level);
VYX_API int32_t vyx_rt_jit_run_main (void* h, int32_t opt_level);
VYX_API int32_t vyx_rt_jit_run_main_with_args(void* h, int32_t opt_level,
                                              const char* link_args,
                                              uint64_t link_args_len);
VYX_API int64_t vyx_string_len(const char* s);
VYX_API int32_t vyx_string_contains(const char* hay, uint64_t hay_len,
                                    const char* needle, uint64_t needle_len);
VYX_API int64_t vyx_string_index_of(const char* hay, uint64_t hay_len,
                                    const char* needle, uint64_t needle_len);
VYX_API int32_t vyx_string_equals(const char* a, const char* b);
VYX_API int32_t vyx_string_compare(const char* a, const char* b);
VYX_API int32_t vyx_string_equals_len(const char* a, const char* b, int64_t b_len);
VYX_API char*   int_to_string(int64_t v);
VYX_API char*   uint_to_string(uint64_t v);
VYX_API char*   int_to_string_len_abi(int64_t v, int64_t* out_len);
VYX_API int64_t vyx_int_decimal_len_abi(int64_t v);
VYX_API int64_t vyx_int_to_string_into_abi(int64_t v, char* out);
VYX_API char*   uint_to_string_len_abi(uint64_t v, int64_t* out_len);
VYX_API int64_t vyx_uint_decimal_len_abi(uint64_t v);
VYX_API int64_t vyx_uint_to_string_into_abi(uint64_t v, char* out);
VYX_API void*   vyx_string_builder_create_abi(void);
VYX_API void    vyx_string_builder_append_i64_abi(void* builder, int64_t v);
VYX_API void    vyx_string_builder_append_u64_abi(void* builder, uint64_t v);
VYX_API void*   vyx_class_alloc_abi(int64_t size);
VYX_API char*   from_cstr_view_len(const char* s, int64_t len);
VYX_API void    vyx_note_string_len(const char* s, int64_t len);
VYX_API void    vyx_free_runtime_string(char* s);
/* Allocates checkpoint-managed raw bytes from the same thread-local arena as
 * bootstrap runtime strings. The returned storage carries no string header. */
VYX_API void*   vyx_rt_string_arena_alloc(int64_t bytes);
VYX_API char*   vyx_alloc_string_len(int64_t len);
VYX_API char*   vyx_dup_bytes_raw(const char* s, int64_t len);
/* Stage-0 bootstrap ABI. Current MIR lowering emits in-module append
 * control flow so JIT allocations never cross this DLL boundary. */
VYX_API char*   vyx_string_append_assign_abi(const char* base,
                                             int64_t base_len,
                                             int64_t base_cap,
                                             int64_t base_owned,
                                             const char* suffix,
                                             int64_t suffix_len,
                                             int64_t* out_len,
                                             int64_t* out_cap,
                                             int64_t* out_owned);
/* Additive, length-aware clone ABI for generated code. It always returns an
 * independently owned runtime allocation, including for an empty input. The
 * older vyx_string_clone ABI remains bootstrap-compatible and must not be
 * repurposed. */
VYX_API char*   vyx_string_clone_len_abi(const char* s, int64_t len);
VYX_API void    vyx_dict_i64_i64_put(void* dict, int64_t key, int64_t val);
VYX_API int32_t vyx_dict_i64_i64_try_get(void* dict, int64_t key, int64_t* out);
VYX_API void    vyx_dict_str_i64_put(void* dict, const char* key, int64_t len,
                                     int64_t cap, int64_t owned, int64_t val);
VYX_API void    vyx_dict_str_i64_put_fast(void* dict, const char* key, int64_t len,
                                          int64_t cap, int64_t owned, int64_t val);
VYX_API int32_t vyx_dict_str_i64_try_get(void* dict, const char* key, int64_t len,
                                         int64_t* out);
VYX_API int32_t vyx_dict_str_i64_try_get_fast(void* dict, const char* key, int64_t len,
                                              int64_t* out);
VYX_API char*   vyx_string_substring_len(const char* s, int64_t len, int32_t start, int32_t end);
VYX_API char*   vyx_string_to_upper_len_abi(const char* s, int64_t len);
VYX_API char*   vyx_string_to_lower_len_abi(const char* s, int64_t len);
VYX_API char*   vyx_string_concat3(const char* a, const char* b, const char* c);
VYX_API char*   vyx_string_concat4(const char* a, const char* b, const char* c, const char* d);
VYX_API int64_t vyx_bootstrap_now_ms(void);
VYX_API int32_t vyx_bootstrap_profile_level(void);
VYX_API int32_t vyx_bootstrap_platform(void);
VYX_API const char* vyx_bootstrap_llvm_version(void);
VYX_API const char* vyx_bootstrap_llvm_host_triple(void);
VYX_API int64_t vyx_bootstrap_file_size(const char* path);
VYX_API void    vyx_rt_set_args(int32_t argc, char** argv);
VYX_API const char* vyx_bootstrap_abs_path(const char* path);
/* Parses a TOML string-array and returns project-relative paths as a
 * normalized absolute UTF-8 newline list. The runtime string arena owns
 * the returned pointer; callers must not free it. */
VYX_API const char* vyx_bootstrap_toml_array_abs_paths(const char* project_dir,
                                                       int64_t project_dir_len,
                                                       const char* array_text,
                                                       int64_t array_text_len);
VYX_API int64_t vyx_bootstrap_process_spawn(const char* command,
                                            const char* cwd,
                                            const char* stdout_path,
                                            const char* stderr_path,
                                            const char* env_name,
                                            const char* env_value);
VYX_API int32_t vyx_bootstrap_process_is_running(int64_t id);
VYX_API int32_t vyx_bootstrap_process_exit_code(int64_t id);
VYX_API int32_t vyx_bootstrap_process_wait(int64_t id);
VYX_API void    vyx_bootstrap_process_close(int64_t id);
VYX_API int32_t vyx_bootstrap_copy_file(const char* src, const char* dst);
VYX_API int32_t vyx_bootstrap_copy_tree(const char* src, const char* dst);
VYX_API void    vyx_bootstrap_sleep_ms(int32_t ms);
VYX_API int32_t vyx_bootstrap_mkdir_p(const char* path);
VYX_API int32_t vyx_bootstrap_build_lock_try_acquire(const char* lock_dir, const char* owner);
VYX_API int32_t vyx_bootstrap_build_lock_release(const char* lock_dir);
VYX_API int32_t vyx_bootstrap_list_vyx_deps(const char* root, const char* out_path);

/*  Current export count: 120+ VYX_API declarations. Keep this header,
 *  bootstrap_compiler/src/vyx_rt.vyx, bootstrap_compiler/src/codegen.vyx,
 *  and docs/LLVM_FFI_SURVEY.md in sync whenever the ABI changes.          */

/* ------------------------------------------------------------------- */
/*  10. Bitwise / unsigned / shift (7)                                  */
/* ------------------------------------------------------------------- */
VYX_API void* vyx_rt_build_and (void* h, void* lhs, void* rhs, const char* name, uint64_t len);
VYX_API void* vyx_rt_build_or  (void* h, void* lhs, void* rhs, const char* name, uint64_t len);
VYX_API void* vyx_rt_build_xor (void* h, void* lhs, void* rhs, const char* name, uint64_t len);
VYX_API void* vyx_rt_build_shl (void* h, void* lhs, void* rhs, const char* name, uint64_t len);
VYX_API void* vyx_rt_build_lshr(void* h, void* lhs, void* rhs, const char* name, uint64_t len);
VYX_API void* vyx_rt_build_udiv(void* h, void* lhs, void* rhs, const char* name, uint64_t len);
VYX_API void* vyx_rt_build_urem(void* h, void* lhs, void* rhs, const char* name, uint64_t len);

/* ------------------------------------------------------------------- */
/*  11. Floating-point (8)                                              */
/* ------------------------------------------------------------------- */
VYX_API void* vyx_rt_float_type(void* h, int32_t bits);  /* 32 or 64 */
VYX_API void* vyx_rt_build_fadd(void* h, void* lhs, void* rhs, const char* name, uint64_t len);
VYX_API void* vyx_rt_build_fsub(void* h, void* lhs, void* rhs, const char* name, uint64_t len);
VYX_API void* vyx_rt_build_fmul(void* h, void* lhs, void* rhs, const char* name, uint64_t len);
VYX_API void* vyx_rt_build_fdiv(void* h, void* lhs, void* rhs, const char* name, uint64_t len);
VYX_API void* vyx_rt_build_frem(void* h, void* lhs, void* rhs, const char* name, uint64_t len);
VYX_API void* vyx_rt_build_fcmp(void* h, int32_t pred,
                                void* lhs, void* rhs,
                                const char* name, uint64_t len);
VYX_API void* vyx_rt_build_fp_to_si(void* h, void* val, void* dst_ty, const char* name, uint64_t len);
VYX_API void* vyx_rt_build_si_to_fp(void* h, void* val, void* dst_ty, const char* name, uint64_t len);
VYX_API void* vyx_rt_build_fp_ext(void* h, void* val, void* dst_ty, const char* name, uint64_t len);
VYX_API void* vyx_rt_build_fp_trunc(void* h, void* val, void* dst_ty, const char* name, uint64_t len);

/* Float comparison predicates — values must match LLVMRealPredicate
 * exactly (LLVM-C/Core.h): FALSE=0, OEQ=1, OGT=2, OGE=3, OLT=4,
 * OLE=5, ONE=6, ORD=7, UNO=8, UEQ=9, UGT=10, UGE=11, ULT=12,
 * ULE=13, UNE=14, TRUE=15.                                             */
#define VYX_RT_FCMP_FALSE  0
#define VYX_RT_FCMP_OEQ    1
#define VYX_RT_FCMP_OGT    2
#define VYX_RT_FCMP_OGE    3
#define VYX_RT_FCMP_OLT    4
#define VYX_RT_FCMP_OLE    5
#define VYX_RT_FCMP_ONE    6
#define VYX_RT_FCMP_ORD    7
#define VYX_RT_FCMP_UNO    8
#define VYX_RT_FCMP_UEQ    9
#define VYX_RT_FCMP_UGT   10
#define VYX_RT_FCMP_UGE   11
#define VYX_RT_FCMP_ULT   12
#define VYX_RT_FCMP_ULE   13
#define VYX_RT_FCMP_UNE   14
#define VYX_RT_FCMP_TRUE  15

/* ------------------------------------------------------------------- */
/*  12. Aggregate value ops (2)                                         */
/* ------------------------------------------------------------------- */
VYX_API void* vyx_rt_build_extract_value(void* h, void* agg_val, int32_t idx,
                                         const char* name, uint64_t len);
VYX_API void* vyx_rt_build_insert_value(void* h, void* agg_val, void* elt,
                                        int32_t idx, const char* name, uint64_t len);
/* 1 if `val` is an LLVM struct, else 0. Pointer-handle vs raw-pointer ABI. */
VYX_API int32_t vyx_rt_value_is_struct(void* val);

/* ------------------------------------------------------------------- */
/*  13. Switch / PHI (4)                                                */
/* ------------------------------------------------------------------- */
VYX_API void* vyx_rt_build_switch(void* h, void* val, void* else_bb);
VYX_API void  vyx_rt_add_case(void* h, void* switch_val, void* on_val, void* dest_bb);
VYX_API void* vyx_rt_build_phi(void* h, void* ty, const char* name, uint64_t len);
VYX_API void  vyx_rt_add_incoming(void* h, void* phi, void* val, void* bb);

/* ------------------------------------------------------------------- */
/*  14. Named struct constant (1)                                       */
/* ------------------------------------------------------------------- */
VYX_API void* vyx_rt_const_named_struct(void* h, void* struct_ty,
                                        const void* const* vals, uint64_t n);

/* ------------------------------------------------------------------- */
/*  15. Misc (2)                                                        */
/* ------------------------------------------------------------------- */
VYX_API void  vyx_rt_set_alignment(void* h, void* val, int32_t bytes);
/* Marks a direct alloca as the inalloca frame LLVM requires when a
 * parameter carries the inalloca attribute. The Consumer uses this both
 * for a single inalloca slot and for the packed multi-arg frame alloca. */
VYX_API int32_t vyx_rt_set_alloca_inalloca(void* h, void* alloca);
VYX_API void* vyx_rt_build_unreachable(void* h);
/* Alias-scope metadata: disjoint audit/payload domains so LICM can hoist
   typed-pointer audit chains out of loops across opaque ptr round-trips. */
VYX_API void* vyx_rt_alias_scope_domain(void* h);
VYX_API void* vyx_rt_alias_scope(void* h, void* domain, int32_t kind);
VYX_API void  vyx_rt_set_alias_scopes(void* h, void* instr,
                                      void* domain, int32_t scope_kind);
VYX_API void  vyx_rt_set_alias_scopes_on_store(void* h, void* val, void* ptr,
                                               void* domain, int32_t scope_kind);

/* ------------------------------------------------------------------- */
/*  16. Type introspection (2)                                          */
/* ------------------------------------------------------------------- */
VYX_API int32_t vyx_rt_is_array_type(void* ty);        /* 1 if ArrayType */
VYX_API void*   vyx_rt_array_elem_type(void* arr_ty);  /* element Type*  */

/* ------------------------------------------------------------------- */
/*  17. Native DAP engine (Linux ptrace + DWARF)                        */
/* ------------------------------------------------------------------- */
VYX_API int32_t vyx_dap_engine_available(void);
VYX_API int32_t vyx_dap_launch(const char* exe, const char* cwd);
VYX_API int32_t vyx_dap_set_breakpoint(const char* file, int32_t line);
VYX_API int32_t vyx_dap_clear_breakpoints(void);
VYX_API int32_t vyx_dap_continue(void);
VYX_API int32_t vyx_dap_step_over(void);
VYX_API int32_t vyx_dap_step_in(void);
VYX_API int32_t vyx_dap_step_out(void);
VYX_API int32_t vyx_dap_pause(void);
VYX_API int32_t vyx_dap_wait_stop(char* reason, int32_t cap);
VYX_API int32_t vyx_dap_stack_json(char* buf, int32_t cap);
VYX_API int32_t vyx_dap_vars_json(int32_t frame, char* buf, int32_t cap);
VYX_API int32_t vyx_dap_evaluate(const char* expr, char* buf, int32_t cap);
VYX_API void    vyx_dap_disconnect(void);

#ifdef __cplusplus
}  /* extern "C" */
#endif
#endif /* VYX_CODEGEN_H */
