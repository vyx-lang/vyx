#include "mixed_helpers.h"

#ifndef MIXED_C_FLAG
#error "expected MIXED_C_FLAG to be defined via cflags"
#endif

int32_t mixed_c_helper(int32_t x) {
    return x + MIXED_C_FLAG;
}
