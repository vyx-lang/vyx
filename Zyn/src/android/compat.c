#include <stddef.h>

// LLVM can lower equality-only memcmp calls to bcmp. Android API 29 does not
// export bcmp, so provide it from the Zyn shared library used by Android apps.
// Keep this function unoptimized to prevent the loop becoming another bcmp.
__attribute__((visibility("default"), optnone))
int bcmp(const void *lhs, const void *rhs, size_t count) {
    const unsigned char *left = (const unsigned char *)lhs;
    const unsigned char *right = (const unsigned char *)rhs;
    for (size_t i = 0; i < count; ++i) {
        if (left[i] != right[i]) {
            return 1;
        }
    }
    return 0;
}
