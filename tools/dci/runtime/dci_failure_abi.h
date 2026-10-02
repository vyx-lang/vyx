#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DciFailure {
    const char* type_identity;
    uint64_t type_identity_len;
    const char* message;
    uint64_t message_len;
    const uint8_t* payload;
    uint64_t payload_size;
    uint64_t payload_align;
    uint32_t producer_tag;
} DciFailure;

enum {
    DCI_FAILURE_TAG_MSVC_CXX = 1,
    DCI_FAILURE_TAG_ITANIUM_CXX = 2,
    DCI_FAILURE_TAG_RUST_PANIC = 3,
    DCI_FAILURE_TAG_RUST_RESULT = 4,
    DCI_FAILURE_TAG_ZIG_ERROR = 5
};

void dci_failure_clear(DciFailure* failure);
void dci_failure_destroy(DciFailure* failure);
int dci_failure_copy(const DciFailure* src, DciFailure* dst);
uint64_t dci_failure_sizeof(void);

#ifdef __cplusplus
}
#endif
