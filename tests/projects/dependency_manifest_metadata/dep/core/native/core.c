#include <stdint.h>

int32_t dependency_core_support_adjust(int32_t value);

int32_t dependency_core_bias(void) {
    return dependency_core_support_adjust(5);
}
