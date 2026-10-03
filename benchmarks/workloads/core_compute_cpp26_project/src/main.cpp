#include <cstdint>
#include <iostream>

static std::int64_t mix(std::int64_t x) {
    return (x * 1664525LL + 1013904223LL) & 2147483647LL;
}

static std::int64_t arith_control(std::int64_t n) {
    std::int64_t acc = 7;
    for (std::int64_t i = 0; i < n; ++i) {
        acc = mix(acc + i);
        if ((i % 7) == 0) {
            acc = acc ^ (i * 13);
        } else {
            acc = acc + (i % 97);
        }
    }
    return acc;
}

static std::int64_t matrix_small_int(std::int64_t n) {
    std::int64_t a00 = 1;
    std::int64_t a01 = 2;
    std::int64_t a10 = 3;
    std::int64_t a11 = 5;
    std::int64_t checksum = 0;
    for (std::int64_t i = 0; i < n; ++i) {
        const auto b00 = (a00 + a01 + i) % 1000003LL;
        const auto b01 = (a00 * 2 + a11 + i * 3) % 1000003LL;
        const auto b10 = (a10 + a11 * 2 + i * 5) % 1000003LL;
        const auto b11 = (a01 + a10 + a11 + i * 7) % 1000003LL;
        a00 = b00;
        a01 = b01;
        a10 = b10;
        a11 = b11;
        checksum = (checksum + a00 * 3 + a01 * 5 + a10 * 7 + a11 * 11) % 1000000007LL;
    }
    return checksum + a00 + a11;
}

static std::int64_t branchy_state_machine(std::int64_t n) {
    std::int64_t state = 0;
    std::int64_t accepted = 0;
    std::int64_t checksum = 11;
    for (std::int64_t i = 0; i < n; ++i) {
        const auto input = (i * 17 + state * 31 + 7) % 9;
        if (state == 0) {
            if (input < 3) { state = 1; } else if (input < 6) { state = 2; } else { state = 3; }
        } else if (state == 1) {
            if ((input % 2) == 0) { state = 4; } else { state = 2; }
        } else if (state == 2) {
            if (input == 5) { state = 0; } else { state = 3; }
        } else if (state == 3) {
            if (input > 6) { state = 4; } else { state = 1; }
        } else {
            accepted += 1;
            state = input % 4;
        }
        checksum = (checksum * 131 + state * 17 + input) % 1000000007LL;
    }
    return checksum + accepted + state;
}

int main() {
    if (arith_control(0) != 7 || matrix_small_int(0) != 6 || branchy_state_machine(0) != 11) {
        return 1;
    }

    const auto arith = arith_control(40000000);
    const auto matrix = matrix_small_int(25000000);
    const auto branchy = branchy_state_machine(35000000);
    if (arith != 798862194 || matrix != 354799773 || branchy != 663595331) {
        return 1;
    }
    std::cout << "runtime arith=" << arith << " matrix=" << matrix << " branchy=" << branchy << "\n";
    return 0;
}
