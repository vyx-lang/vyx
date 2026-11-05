/* Test instrumentation around the actual engine, not a production wrapper. */
#define pcre2_compile_8 counted_compile_impl
#define pcre2_code_free_8 counted_free_impl
#include "../../../../bootstrap_compiler/std_packages/regex/vendor/pcre2/pcre2_compile.c"
#undef pcre2_compile_8
#undef pcre2_code_free_8

#define pcre2_match_data_create_from_pattern_8 counted_data_create_impl
#define pcre2_match_data_free_8 counted_data_free_impl
#include "../../../../bootstrap_compiler/std_packages/regex/vendor/pcre2/pcre2_match_data.c"
#undef pcre2_match_data_create_from_pattern_8
#undef pcre2_match_data_free_8
#define pcre2_match_context_create_8 counted_context_create_impl
#define pcre2_match_context_free_8 counted_context_free_impl
#include "../../../../bootstrap_compiler/std_packages/regex/vendor/pcre2/pcre2_context.c"
#undef pcre2_match_context_create_8
#undef pcre2_match_context_free_8

#define vyx_toml_abi_parse counted_toml_parse_impl
#define vyx_toml_abi_free counted_toml_free_impl
#include "../../../../bootstrap_compiler/std_packages/serde/native/toml_abi.c"
#undef vyx_toml_abi_parse
#undef vyx_toml_abi_free

static int64_t regex_live, toml_live, data_live, context_live;
static int fail_scratch;
int64_t std_engines_live_regex(void) { return regex_live; }
int64_t std_engines_live_toml(void) { return toml_live; }
int64_t std_engines_live_scratch(void) { return data_live + context_live; }
void std_engines_fail_scratch(int mode) { fail_scratch = mode; }

pcre2_code *pcre2_compile_8(PCRE2_SPTR pattern, PCRE2_SIZE length, uint32_t flags,
                         int *failure, PCRE2_SIZE *offset, pcre2_compile_context *context) {
    pcre2_code *p = counted_compile_impl(pattern, length, flags, failure, offset, context);
    if (p) ++regex_live;
    return p;
}
void pcre2_code_free_8(pcre2_code *p) { if (p) --regex_live; counted_free_impl(p); }
pcre2_match_data *pcre2_match_data_create_from_pattern_8(const pcre2_code *code, pcre2_general_context *context) {
    if (fail_scratch == 1) { fail_scratch = 0; return NULL; }
    pcre2_match_data *p = counted_data_create_impl(code, context);
    if (p) ++data_live;
    return p;
}
void pcre2_match_data_free_8(pcre2_match_data *p) { if (p) --data_live; counted_data_free_impl(p); }
pcre2_match_context *pcre2_match_context_create_8(pcre2_general_context *context) {
    if (fail_scratch == 2) { fail_scratch = 0; return NULL; }
    pcre2_match_context *p = counted_context_create_impl(context);
    if (p) ++context_live;
    return p;
}
void pcre2_match_context_free_8(pcre2_match_context *p) { if (p) --context_live; counted_context_free_impl(p); }
void vyx_toml_abi_parse(void *out, const char *text, int32_t length) {
    counted_toml_parse_impl(out, text, length); ++toml_live;
}
void vyx_toml_abi_free(void *storage) { --toml_live; counted_toml_free_impl(storage); }
