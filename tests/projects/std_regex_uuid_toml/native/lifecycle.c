/* Exercise the actual bridge sources. Wrappers count live native resources;
   PCRE2/tomlc17 still come from the real linked standard packages. */
#define vyx_regex_compile regex_compile_impl
#define vyx_regex_free regex_free_impl
#include "../../../../bootstrap_compiler/std_packages/regex/native/regex_bridge.c"
#undef vyx_regex_compile
#undef vyx_regex_free
#define vyx_toml_parse toml_parse_impl
#define vyx_toml_get toml_get_impl
#define vyx_toml_at toml_at_impl
#define vyx_toml_clone toml_clone_impl
#define vyx_toml_free toml_free_impl
#include "../../../../bootstrap_compiler/std_packages/serde/native/toml_bridge.c"
#undef vyx_toml_parse
#undef vyx_toml_get
#undef vyx_toml_at
#undef vyx_toml_clone
#undef vyx_toml_free

static int64_t regex_live, toml_live;
int64_t std_engines_live_regex(void) { return regex_live; }
int64_t std_engines_live_toml(void) { return toml_live; }
void *vyx_regex_compile(const void *pattern, int64_t length, int32_t flags, int32_t *failure, int64_t *offset) {
    void *p = regex_compile_impl(pattern, length, flags, failure, offset);
    if (p) ++regex_live;
    return p;
}
void vyx_regex_free(void *p) { if (p) --regex_live; regex_free_impl(p); }
void *vyx_toml_parse(const void *text, int64_t size, char *failure, int64_t cap, int64_t *line) {
    void *p = toml_parse_impl(text, size, failure, cap, line);
    if (p) ++toml_live;
    return p;
}
void *vyx_toml_get(void *raw, const void *key, int64_t len, int32_t *status) {
    void *p = toml_get_impl(raw, key, len, status);
    if (p) ++toml_live;
    return p;
}
void *vyx_toml_at(void *raw, int64_t index) {
    void *p = toml_at_impl(raw, index); if (p) ++toml_live; return p;
}
void *vyx_toml_clone(void *raw) {
    void *p = toml_clone_impl(raw); if (p) ++toml_live; return p;
}
void vyx_toml_free(void *raw) { if (raw) --toml_live; toml_free_impl(raw); }
