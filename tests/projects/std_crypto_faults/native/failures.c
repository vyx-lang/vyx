#include <stdint.h>
#include <stddef.h>
#include <string.h>

static int mode;
static int frees;
static size_t digest_size;
void crypto_probe_mode(int value) { mode = value; frees = 0; }
int crypto_probe_frees(void) { return frees; }
unsigned long ERR_get_error(void) { return 0x1234UL; }
void *EVP_MD_CTX_new(void) { return mode == 0 ? NULL : &digest_size; }
void EVP_MD_CTX_free(void *ctx) { (void)ctx; ++frees; }
const void *EVP_sha256(void) { return (const void *)(uintptr_t)32; }
const void *EVP_sha512(void) { return (const void *)(uintptr_t)64; }
const void *EVP_sha1(void) { return (const void *)(uintptr_t)20; }
const void *EVP_md5(void) { return (const void *)(uintptr_t)16; }
int EVP_DigestInit_ex(void *ctx, const void *algorithm, void *impl) {
    (void)ctx; (void)impl;
    digest_size = (size_t)(uintptr_t)algorithm;
    return mode != 1;
}
int EVP_DigestUpdate(void *ctx, const void *data, size_t length) {
    (void)ctx; (void)data; (void)length;
    return mode != 2;
}
int EVP_DigestFinal_ex(void *ctx, unsigned char *output, unsigned int *length) {
    (void)ctx;
    if (mode == 3) return 0;
    memset(output, 0xA5, digest_size);
    *length = (unsigned int)digest_size - (mode == 4 ? 1 : 0);
    return 1;
}
unsigned char *HMAC(const void *algorithm, const void *key, int key_len,
                    const unsigned char *data, size_t length,
                    unsigned char *output, unsigned int *output_len) {
    (void)algorithm; (void)key; (void)key_len; (void)data; (void)length;
    if (mode == 5) return NULL;
    memset(output, 0x5A, 32); *output_len = 32;
    return output;
}
int RAND_bytes(unsigned char *output, int count) {
    if (mode == 6) return 0;
    memset(output, 0x3C, (size_t)count);
    return 1;
}
