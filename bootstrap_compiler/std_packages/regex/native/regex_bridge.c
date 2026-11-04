#define PCRE2_CODE_UNIT_WIDTH 8
#ifndef PCRE2_STATIC
#define PCRE2_STATIC
#endif
#include "pcre2.h"
#include <stdint.h>
#include <string.h>

/* Compiled code is immutable. Every call owns its match context and scratch. */
void *vyx_regex_compile(const void *pattern, int64_t length, int32_t flags,
                        int32_t *error, int64_t *offset) {
    *offset = -1;
    *error = 0;
    if (length < 0 || length > 65536 || (flags & ~15) != 0) { *error = -1000; return NULL; }
    uint32_t options = PCRE2_NEVER_BACKSLASH_C;
    if (flags & 1) options |= PCRE2_CASELESS;
    if (flags & 2) options |= PCRE2_MULTILINE;
    if (flags & 4) options |= PCRE2_DOTALL;
    if (flags & 8) options |= PCRE2_UTF | PCRE2_UCP;
    PCRE2_SIZE position = 0;
    pcre2_code *code = pcre2_compile(pattern, (PCRE2_SIZE)length, options, error, &position, NULL);
    if (!code) { *offset = (int64_t)position; return NULL; }
    uint32_t count = 0;
    pcre2_pattern_info(code, PCRE2_INFO_CAPTURECOUNT, &count);
    if (count > 256) { pcre2_code_free(code); *error = -1000; return NULL; }
    return code;
}
void vyx_regex_free(void *code) { pcre2_code_free(code); }
int32_t vyx_regex_groups(void *code) {
    uint32_t count = 0;
    if (!code || pcre2_pattern_info(code, PCRE2_INFO_CAPTURECOUNT, &count)) return -1;
    return (int32_t)count + 1;
}
int32_t vyx_regex_group_number(void *code, const char *name) {
    return code ? pcre2_substring_number_from_name(code, (PCRE2_SPTR)name) : -1;
}
int32_t vyx_regex_error(int32_t code, void *buffer, int64_t size) {
    if (code == -1000) {
        const char *message = "regex input, pattern, capture count or resource limit exceeds supported range";
        size_t n = strlen(message);
        if (size <= (int64_t)n) return -1;
        memcpy(buffer, message, n + 1);
        return (int32_t)n;
    }
    return pcre2_get_error_message(code, buffer, (PCRE2_SIZE)size);
}
int32_t vyx_regex_find(void *code, const void *text, int64_t size, int64_t start,
                       int32_t full, int64_t steps, int64_t depth, int64_t heap_kib,
                       int64_t *spans, int64_t capacity) {
    if (!code || size < 0 || size > 16777216 || start < 0 || start > size ||
        steps < 1 || steps > UINT32_MAX || depth < 1 || depth > UINT32_MAX ||
        heap_kib < 1 || heap_kib > 65536) return -1000;
    pcre2_match_data *data = pcre2_match_data_create_from_pattern(code, NULL);
    pcre2_match_context *context = pcre2_match_context_create(NULL);
    if (!data || !context) {
        pcre2_match_data_free(data); pcre2_match_context_free(context);
        return PCRE2_ERROR_NOMEMORY;
    }
    pcre2_set_match_limit(context, (uint32_t)steps);
    pcre2_set_depth_limit(context, (uint32_t)depth);
    pcre2_set_heap_limit(context, (uint32_t)heap_kib);
    uint32_t options = full ? PCRE2_ANCHORED | PCRE2_ENDANCHORED : 0;
    int result = pcre2_match(code, text, (PCRE2_SIZE)size, (PCRE2_SIZE)start, options, data, context);
    if (result >= 0) {
        uint32_t count = pcre2_get_ovector_count(data);
        if (capacity < 2 * (int64_t)count) result = -1000;
        else {
            PCRE2_SIZE *vector = pcre2_get_ovector_pointer(data);
            for (uint32_t i = 0; i < count * 2; ++i)
                spans[i] = vector[i] == PCRE2_UNSET ? -1 : (int64_t)vector[i];
            result = 1;
        }
    } else if (result == PCRE2_ERROR_NOMATCH) result = 0;
    pcre2_match_context_free(context);
    pcre2_match_data_free(data);
    return result;
}
