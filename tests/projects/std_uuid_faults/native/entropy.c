/* Only this executable overrides OS entropy. No test hook exists in std.uuid. */
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <stdio.h>
static int mode, calls, interrupted;
void uuid_test_mode(int value) { mode = value; calls = interrupted = 0; }
#if defined(_WIN32)
int rand_s(unsigned int *out) {
    ++calls;
    if (mode == 0 || (mode == 1 && calls == 3)) { *out = 0; return 5; }
    *out = 0x03020100u;
    return 0;
}
#else
/* Signatures match libc; unrelated fopen use is an error in this fixture. */
FILE *fopen(const char *path, const char *flags) { (void)path; (void)flags; return mode == 0 ? NULL : (FILE *)&calls; }
size_t fread(void *p, size_t size, size_t n, FILE *f) {
    (void)size; (void)f;
    ++calls;
    if (mode == 1 || mode == 4) return 0;
    if (mode == 3 && calls == 1) { interrupted = 1; errno = EINTR; return 0; }
    size_t count = n > 3 ? 3 : n;
    for (size_t i = 0; i < count; ++i) ((unsigned char *)p)[i] = 0x11;
    return count;
}
int ferror(FILE *f) { (void)f; if (mode == 4) { errno = EIO; return 1; } return interrupted; }
void clearerr(FILE *f) { (void)f; interrupted = 0; }
int fclose(FILE *f) { (void)f; return mode == 5 ? -1 : 0; }
#endif
