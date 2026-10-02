#include "work.hpp"

#include <iostream>

int main() {
    if (math_batch(0) != 0 || string_batch(0) != 5 || bench::generic_batch(0) != 0
        || gcd_batch(0) != 0 || poly_batch(0) != 19 || string_scan_batch(0) != 0
        || bench::generic_nested_batch(0) != 0) {
        return 2;
    }

    auto a = math_batch(16000);
    auto b = string_batch(6000);
    auto c = bench::generic_batch(40000);
    auto d = gcd_batch(12000);
    auto e = poly_batch(18000);
    auto f = string_scan_batch(5000);
    auto g = bench::generic_nested_batch(24000);
    if (a != 16268912790390LL || b != 1103 || c != 3199920000LL
        || d != 506485 || e != 145647344 || f != 6753813
        || g != 671519924) {
        return 1;
    }
    auto extra = (d + e + f + g) % 1000000007LL;
    auto label = label_value("score", a + b + c + extra);
    std::cout << "runtime math=" << a
              << " string=" << b
              << " generic=" << c
              << " gcd=" << d
              << " poly=" << e
              << " scan=" << f
              << " nested=" << g
              << " label=" << label
              << '\n';
    return label.empty() ? 1 : 0;
}
