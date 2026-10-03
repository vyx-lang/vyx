#include <stdint.h>

int32_t dependency_support_adjust(int32_t value);

int32_t dependency_native_scale_add(int32_t value, int32_t bias) {
    return dependency_support_adjust(value * 7 + bias);
}
