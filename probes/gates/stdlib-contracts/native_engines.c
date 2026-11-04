/* Native ownership/failure regression, compiled with ASan+UBSan on POSIX. */
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static int fail_after = -1;
static void *checked_malloc(size_t size) {
    if (fail_after == 0) return NULL;
    if (fail_after > 0) --fail_after;
    return malloc(size);
}
#define malloc checked_malloc
#include "../../../bootstrap_compiler/std_packages/serde/native/toml_bridge.c"
#undef malloc

void *vyx_regex_compile(const void *, int64_t, int32_t, int32_t *, int64_t *);
void vyx_regex_free(void *);
int32_t vyx_regex_find(void *, const void *, int64_t, int64_t, int32_t, int64_t, int64_t, int64_t, int64_t *, int64_t);

int main(void) {
    char error[256]; int64_t line, length;
    for (int iteration = 0; iteration < 1000; ++iteration) {
        const char *text = "[a]\ns='long owned child value'\nx=[1,2,3]\n";
        void *doc = vyx_toml_parse(text, (int64_t)strlen(text), error, sizeof(error), &line);
        assert(doc);
        int32_t status;
        void *child = vyx_toml_get(doc, "a", 1, &status); assert(status == 1 && child);
        void *copy = vyx_toml_clone(child); assert(copy);
        vyx_toml_free(doc); vyx_toml_free(child);
        void *str = vyx_toml_get(copy, "s", 1, &status);
        assert(status == 1 && !memcmp(vyx_toml_string(str, &length), "long owned child value", 22) && length == 22);
        fail_after = 0;
        assert(!vyx_toml_get(copy, "s", 1, &status) && status == -1);
        assert(!vyx_toml_clone(copy));
        fail_after = -1;
        vyx_toml_free(copy); vyx_toml_free(str);
    }
    for (int n = 0; n < 2; ++n) {
        fail_after = n;
        assert(!vyx_toml_parse("x=1", 3, error, sizeof(error), &line));
    }
    fail_after = -1;
    void *saturated = vyx_toml_parse("x=1", 3, error, sizeof(error), &line);
    assert(saturated);
    atomic_store(&((value *)saturated)->owner->refs, UINT_MAX);
    assert(!vyx_toml_clone(saturated));
    atomic_store(&((value *)saturated)->owner->refs, 1);
    vyx_toml_free(saturated);
    assert(!vyx_toml_parse("x=1\nx=2", 7, error, sizeof(error), &line) && line == 2);
    const unsigned char invalid[] = {'x','=', '\'',0xc0,0x80,'\''};
    assert(!vyx_toml_parse(invalid, sizeof(invalid), error, sizeof(error), &line));
    assert(!vyx_toml_parse("x='a\0b'", 7, error, sizeof(error), &line));
    int32_t code; int64_t offset, spans[8];
    void *pattern = vyx_regex_compile("(a)(b)?()", 9, 8, &code, &offset); assert(pattern);
    for (int n = 0; n < 1000; ++n) {
        assert(vyx_regex_find(pattern,"a",1,0,1,1000,100,1024,spans,8) == 1);
        assert(spans[0] == 0 && spans[1] == 1 && spans[4] == -1 && spans[5] == -1 && spans[6] == 1 && spans[7] == 1);
        assert(vyx_regex_find(pattern,"a",1,0,0,1000,100,1024,spans,2) < 0);
        assert(vyx_regex_find(pattern,"a",1,-1,0,1000,100,1024,spans,8) < 0);
    }
    vyx_regex_free(pattern);
    assert(!vyx_regex_compile("[",1,8,&code,&offset) && code > 0 && offset >= 0);
    puts("native engines ownership and failures ok");
    return 0;
}
