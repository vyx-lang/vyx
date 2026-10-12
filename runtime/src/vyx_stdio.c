#include <stdint.h>
#include <stdio.h>

#ifndef VYX_STDIO_ABI
#define VYX_STDIO_ABI
#endif

/* The compiler supplies an exact slice, including embedded NUL bytes.
 * Keep stdout ownership inside the platform C runtime rather than lowering
 * FILE handles or Windows CRT internals into generated LLVM IR. */
#ifdef __cplusplus
extern "C"
#endif
VYX_STDIO_ABI void vyx_print_bytes_abi(const char* value, uint64_t len, int32_t newline) {
    if (len > 0) {
        fwrite(value, 1, (size_t)len, stdout);
    }
    if (newline != 0) {
        fputc('\n', stdout);
        fflush(stdout);
    }
}
