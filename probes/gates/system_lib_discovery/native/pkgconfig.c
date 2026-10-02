#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    const char *dir = getenv("VYX_TEST_PKGCONFIG_LIBDIR");
    if (!dir) { return 1; }
    puts(dir);
    return 0;
}
