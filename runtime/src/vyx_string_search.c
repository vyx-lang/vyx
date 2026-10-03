#include <stdint.h>
#include <string.h>

int64_t vyx_string_index_of(const char* hay, uint64_t hay_len,
                            const char* needle, uint64_t needle_len) {
    if (!hay || !needle) {
        return -1;
    }
    if (needle_len == 0) {
        return 0;
    }
    if (needle_len > hay_len) {
        return -1;
    }
    {
        const size_t nlen = (size_t)needle_len;
        const size_t hlen = (size_t)hay_len;
        const unsigned char first = (unsigned char)needle[0];
        const char* cur = hay;
        const char* end = hay + (hlen - nlen + 1);
        while (cur < end) {
            const void* hit = memchr(cur, first, (size_t)(end - cur));
            if (!hit) {
                return -1;
            }
            cur = (const char*)hit;
            if (nlen == 1 || memcmp(cur + 1, needle + 1, nlen - 1) == 0) {
                return (int64_t)(cur - hay);
            }
            ++cur;
        }
    }
    return -1;
}
