#include "tomlc17.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* Immutable trees with independently owned value handles. A child may outlive
   the document. No global allocator/options mutation, including during parse. */
typedef struct { atomic_uint refs; toml_result_t result; } tree;
typedef struct { tree *owner; const toml_datum_t *node; } value;
static value *wrap(tree *owner, const toml_datum_t *node) {
    value *v = malloc(sizeof(*v));
    if (!v) return NULL;
    v->owner = owner; v->node = node;
    unsigned refs = atomic_load_explicit(&owner->refs, memory_order_relaxed);
    do {
        if (refs == UINT_MAX) { free(v); return NULL; }
    } while (!atomic_compare_exchange_weak_explicit(&owner->refs, &refs, refs + 1,
                 memory_order_relaxed, memory_order_relaxed));
    return v;
}
static int utf8_valid(const unsigned char *s, size_t n) {
    for (size_t i = 0; i < n;) {
        unsigned c = s[i++], need, cp, min;
        if (c < 128) { if (c == 0) return 0; continue; }
        if (c >= 0xc2 && c <= 0xdf) { need = 1; cp = c & 31; min = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { need = 2; cp = c & 15; min = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { need = 3; cp = c & 7; min = 0x10000; }
        else return 0;
        if (need > n - i) return 0;
        while (need--) { unsigned b = s[i++]; if ((b & 0xc0) != 0x80) return 0; cp = (cp << 6) | (b & 63); }
        if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return 0;
    }
    return 1;
}
void *vyx_toml_parse(const void *text, int64_t size, char *error, int64_t cap, int64_t *line) {
    *line = 0;
    const char *reason = NULL;
    if (size < 0 || size > 16777216) reason = "TOML document exceeds 16 MiB limit";
    else if (!utf8_valid(text, (size_t)size)) reason = "TOML input must be valid UTF-8 without literal NUL bytes";
    if (reason) { snprintf(error, (size_t)cap, "%s", reason); return NULL; }
    tree *owner = malloc(sizeof(*owner));
    if (!owner) { snprintf(error, (size_t)cap, "TOML allocation failed"); return NULL; }
    atomic_init(&owner->refs, 0);
    owner->result = toml_parse(text, (int)size);
    if (!owner->result.ok) {
        snprintf(error, (size_t)cap, "%s", owner->result.errmsg);
        int number = 0;
        if (sscanf(error, "(line %d)", &number) == 1) *line = number;
        toml_free(owner->result); free(owner); return NULL;
    }
    value *v = wrap(owner, &owner->result.toptab);
    if (!v) { toml_free(owner->result); free(owner); snprintf(error, (size_t)cap, "TOML allocation failed"); }
    return v;
}
void vyx_toml_free(void *raw) {
    value *v = raw;
    if (!v) return;
    if (atomic_fetch_sub_explicit(&v->owner->refs, 1, memory_order_acq_rel) == 1) {
        toml_free(v->owner->result); free(v->owner);
    }
    free(v);
}
void *vyx_toml_clone(void *raw) { value *v = raw; return v ? wrap(v->owner, v->node) : NULL; }
int32_t vyx_toml_type(void *raw) { value *v = raw; return v ? v->node->type : TOML_UNKNOWN; }
int64_t vyx_toml_line(void *raw) { value *v = raw; return v ? v->node->lineno : 0; }
int64_t vyx_toml_column(void *raw) { value *v = raw; return v ? v->node->colno : 0; }
int64_t vyx_toml_size(void *raw) {
    value *v = raw; if (!v) return -1;
    if (v->node->type == TOML_ARRAY) return v->node->u.arr.size;
    if (v->node->type == TOML_TABLE) return v->node->u.tab.size;
    return -1;
}
void *vyx_toml_get(void *raw, const void *key, int64_t length, int32_t *status) {
    value *v = raw; *status = 0;
    if (!v || v->node->type != TOML_TABLE) { *status = -2; return NULL; }
    for (int i = 0; i < v->node->u.tab.size; ++i) {
        if (v->node->u.tab.len[i] == length && !memcmp(v->node->u.tab.key[i], key, (size_t)length)) {
            value *child = wrap(v->owner, &v->node->u.tab.value[i]);
            *status = child ? 1 : -1; return child;
        }
    }
    return NULL;
}
void *vyx_toml_at(void *raw, int64_t index) {
    value *v = raw;
    if (!v || v->node->type != TOML_ARRAY || index < 0 || index >= v->node->u.arr.size) return NULL;
    return wrap(v->owner, &v->node->u.arr.elem[index]);
}
const char *vyx_toml_key(void *raw, int64_t index, int64_t *length) {
    value *v = raw;
    if (!v || v->node->type != TOML_TABLE || index < 0 || index >= v->node->u.tab.size) return NULL;
    *length = v->node->u.tab.len[index]; return v->node->u.tab.key[index];
}
const char *vyx_toml_string(void *raw, int64_t *length) {
    value *v = raw;
    if (!v || v->node->type != TOML_STRING) return NULL;
    *length = v->node->u.str.len; return v->node->u.str.ptr;
}
int64_t vyx_toml_integer(void *raw) { return ((value *)raw)->node->u.int64; }
double vyx_toml_float(void *raw) { return ((value *)raw)->node->u.fp64; }
int32_t vyx_toml_bool(void *raw) { return ((value *)raw)->node->u.boolean; }
void vyx_toml_datetime(void *raw, int32_t *fields) {
    const toml_datum_t *n = ((value *)raw)->node;
    fields[0] = n->u.ts.year; fields[1] = n->u.ts.month; fields[2] = n->u.ts.day;
    fields[3] = n->u.ts.hour; fields[4] = n->u.ts.minute; fields[5] = n->u.ts.second;
    fields[6] = n->u.ts.usec; fields[7] = n->u.ts.tz;
}
