#include "work.hpp"

std::int64_t mix_round(std::int64_t seed, std::int64_t rounds) {
    constexpr std::int64_t mod = 2147483647;
    std::int64_t x = seed;
    for (std::int64_t i = 0; i < rounds; ++i) {
        x = (x * 1103515245 + 12345) % mod;
        x = (x ^ (x / 7)) % mod;
        x = (x + i * 31) % mod;
    }
    return x;
}

std::int64_t math_batch(std::int64_t n) {
    std::int64_t total = 0;
    for (std::int64_t i = 0; i < n; ++i) {
        total += mix_round(i + 11, 24);
    }
    return total;
}

static std::int64_t gcd(std::int64_t a, std::int64_t b) {
    while (b != 0) {
        const auto r = a % b;
        a = b;
        b = r;
    }
    return a;
}

std::int64_t gcd_batch(std::int64_t n) {
    std::int64_t total = 0;
    for (std::int64_t i = 1; i <= n; ++i) {
        const auto a = (i * 48271 + 17) % 1000003 + 1;
        const auto b = (i * 69621 + 31) % 1000033 + 1;
        total = (total + gcd(a, b) * ((i % 13) + 1)) % 1000000007LL;
    }
    return total;
}

std::int64_t poly_batch(std::int64_t n) {
    std::int64_t acc = 19;
    for (std::int64_t i = 0; i < n; ++i) {
        const auto x = (i * 48271 + 12345) % 100003LL;
        const auto y = (x * x + 3 * x + 7) % 1000003LL;
        acc = (acc * 131 + y * ((i % 17) + 1)) % 1000000007LL;
    }
    return acc;
}
