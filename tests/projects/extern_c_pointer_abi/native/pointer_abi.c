#include <stdint.h>

int64_t vyx_sum_i64(int64_t* values, int32_t count) {
    int64_t total = 0;
    for (int32_t i = 0; i < count; ++i) {
        total += values[i];
    }
    return total;
}

int64_t* vyx_identity_i64(int64_t* value) {
    return value;
}

int64_t vyx_add_mut_i64(int64_t* value, int64_t delta) {
    *value += delta;
    return *value;
}

int64_t vyx_read_ref_i64(const int64_t* value) {
    return *value;
}
