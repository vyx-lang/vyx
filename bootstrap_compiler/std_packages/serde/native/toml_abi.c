/* ABI projection only: tomlc17 returns structures containing anonymous unions
   by value. Allocation, ownership, UTF-8 validation and lookup live in Vyx.
   Sizes/alignment come from this target's actual C compiler, never constants. */
#include "tomlc17.h"
#include <stddef.h>

size_t vyx_toml_abi_result_size(void) { return sizeof(toml_result_t); }
size_t vyx_toml_abi_result_align(void) { return _Alignof(toml_result_t); }
void vyx_toml_abi_parse(void *out, const char *text, int32_t length) { *(toml_result_t *)out = toml_parse(text, length); }
void vyx_toml_abi_free(void *result) { toml_free(*(toml_result_t *)result); }
int32_t vyx_toml_abi_ok(const void *result) { return ((const toml_result_t *)result)->ok; }
const char *vyx_toml_abi_error(const void *result) { return ((const toml_result_t *)result)->errmsg; }
const void *vyx_toml_abi_root(const void *result) { return &((const toml_result_t *)result)->toptab; }

int32_t vyx_toml_abi_type(const toml_datum_t *node) { return node->type; }
int64_t vyx_toml_abi_line(const toml_datum_t *node) { return node->lineno; }
int64_t vyx_toml_abi_column(const toml_datum_t *node) { return node->colno; }
int64_t vyx_toml_abi_array_size(const toml_datum_t *node) { return node->u.arr.size; }
int64_t vyx_toml_abi_table_size(const toml_datum_t *node) { return node->u.tab.size; }
const void *vyx_toml_abi_array_at(const toml_datum_t *node, int64_t i) { return &node->u.arr.elem[i]; }
const void *vyx_toml_abi_table_at(const toml_datum_t *node, int64_t i) { return &node->u.tab.value[i]; }
const char *vyx_toml_abi_key(const toml_datum_t *node, int64_t i) { return node->u.tab.key[i]; }
int64_t vyx_toml_abi_key_length(const toml_datum_t *node, int64_t i) { return node->u.tab.len[i]; }
const char *vyx_toml_abi_string(const toml_datum_t *node) { return node->u.str.ptr; }
int64_t vyx_toml_abi_string_length(const toml_datum_t *node) { return node->u.str.len; }
int64_t vyx_toml_abi_integer(const toml_datum_t *node) { return node->u.int64; }
double vyx_toml_abi_float(const toml_datum_t *node) { return node->u.fp64; }
int32_t vyx_toml_abi_bool(const toml_datum_t *node) { return node->u.boolean; }
void vyx_toml_abi_datetime(const toml_datum_t *node, int32_t *fields) {
    fields[0] = node->u.ts.year; fields[1] = node->u.ts.month; fields[2] = node->u.ts.day;
    fields[3] = node->u.ts.hour; fields[4] = node->u.ts.minute; fields[5] = node->u.ts.second;
    fields[6] = node->u.ts.usec; fields[7] = node->u.ts.tz;
}
