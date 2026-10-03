#include <cstdint>
#include <stdexcept>
#include <string>

namespace abi {
int32_t checked_divide(int32_t a, int32_t b) {
    if (b == 0) {
        throw std::runtime_error("div0");
    }
    return a / b;
}

int32_t catch_runtime(int32_t a, int32_t b) {
    try {
        return checked_divide(a, b);
    } catch (const std::runtime_error& e) {
        (void)e;
        return -1;
    }
}

int32_t catch_all(int32_t a, int32_t b) {
    try {
        return checked_divide(a, b);
    } catch (...) {
        return -2;
    }
}

int32_t throw_int() {
    throw 42;
}

int32_t catch_int() {
    try {
        return throw_int();
    } catch (int x) {
        return x;
    }
}
}
