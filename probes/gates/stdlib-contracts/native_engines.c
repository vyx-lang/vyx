/* Validate the pinned engines and the ABI-only TOML projections under ASan. */
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define PCRE2_CODE_UNIT_WIDTH 8
#include "pcre2.h"
#include "../../../bootstrap_compiler/std_packages/serde/native/toml_abi.c"

int main(void) {
    assert(vyx_toml_abi_result_size() == sizeof(toml_result_t));
    assert(vyx_toml_abi_result_align() == _Alignof(toml_result_t));
    for (int iteration = 0; iteration < 1000; ++iteration) {
        const char *text = "[a]\ns='long owned child value'\nx=[1,2,3]\n";
        void *result = malloc(vyx_toml_abi_result_size()); assert(result);
        vyx_toml_abi_parse(result, text, (int32_t)strlen(text));
        assert(vyx_toml_abi_ok(result));
        const void *root = vyx_toml_abi_root(result);
        assert(vyx_toml_abi_type(root) == TOML_TABLE && vyx_toml_abi_table_size(root) == 1);
        assert(vyx_toml_abi_key_length(root, 0) == 1 && *vyx_toml_abi_key(root, 0) == 'a');
        const void *table = vyx_toml_abi_table_at(root, 0);
        const void *str = vyx_toml_abi_table_at(table, 0);
        assert(vyx_toml_abi_type(str) == TOML_STRING && vyx_toml_abi_string_length(str) == 22);
        assert(!memcmp(vyx_toml_abi_string(str), "long owned child value", 22));
        const void *array = vyx_toml_abi_table_at(table, 1);
        assert(vyx_toml_abi_array_size(array) == 3);
        assert(vyx_toml_abi_integer(vyx_toml_abi_array_at(array, 2)) == 3);
        vyx_toml_abi_free(result); free(result);
    }
    toml_result_t failure;
    vyx_toml_abi_parse(&failure, "x=1\nx=2", 7);
    assert(!vyx_toml_abi_ok(&failure) && strstr(vyx_toml_abi_error(&failure), "line 2"));
    vyx_toml_abi_free(&failure);
    int code; PCRE2_SIZE offset;
    pcre2_code *pattern = pcre2_compile((PCRE2_SPTR)"(a)(b)?()", 9, PCRE2_UTF | PCRE2_UCP, &code, &offset, NULL);
    assert(pattern);
    for (int n = 0; n < 1000; ++n) {
        pcre2_match_data *data = pcre2_match_data_create_from_pattern(pattern, NULL); assert(data);
        assert(pcre2_match(pattern, (PCRE2_SPTR)"a", 1, 0, PCRE2_ANCHORED | PCRE2_ENDANCHORED, data, NULL) > 0);
        PCRE2_SIZE *spans = pcre2_get_ovector_pointer(data);
        assert(spans[0] == 0 && spans[1] == 1 && spans[4] == PCRE2_UNSET && spans[6] == 1 && spans[7] == 1);
        pcre2_match_data_free(data);
    }
    pcre2_code_free(pattern);
    assert(!pcre2_compile((PCRE2_SPTR)"[", 1, 0, &code, &offset, NULL) && code > 0);
    puts("native engines ABI and memory checks ok");
    return 0;
}
