#pragma once

#include <cstdint>
#include <string>
#include <utility>

std::int64_t mix_round(std::int64_t seed, std::int64_t rounds);
std::int64_t math_batch(std::int64_t n);
std::int64_t gcd_batch(std::int64_t n);
std::int64_t poly_batch(std::int64_t n);
std::int64_t string_batch(std::int64_t n);
std::int64_t string_scan_batch(std::int64_t n);
std::string label_value(const std::string& prefix, std::int64_t value);

namespace bench {

template <class T>
struct Pair {
    T left;
    T right;
};

template <class T>
Pair<T> make_pair(T left, T right) {
    return Pair<T>{left, right};
}

std::int64_t pair_sum(Pair<std::int64_t> p);
std::int64_t generic_batch(std::int64_t n);
std::int64_t nested_pair_sum(Pair<Pair<std::int64_t>> p);
std::int64_t generic_nested_batch(std::int64_t n);

}
