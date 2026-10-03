#include <concepts>
#include <cstdint>
#include <iostream>

template <class T>
struct Pair {
    T first;
    T second;
};

template <class T>
static Pair<T> make_pair_value(T a, T b) {
    return Pair<T>{a, b};
}

template <std::totally_ordered T>
static T min_value(T a, T b) {
    return a < b ? a : b;
}

template <std::totally_ordered T>
static T max_value(T a, T b) {
    return a > b ? a : b;
}

static std::int64_t fold_i64(std::int64_t n) {
    std::int64_t low = 1000000;
    std::int64_t high = -1000000;
    std::int64_t checksum = 0;
    for (std::int64_t i = 0; i < n; ++i) {
        const auto x = (i * 48271 + 13) % 200003 - 100001;
        low = min_value<std::int64_t>(low, x);
        high = max_value<std::int64_t>(high, x);
        checksum = (checksum + (high - low) + x) % 1000000007LL;
    }
    return checksum + low + high;
}

static std::int32_t fold_i32(std::int32_t n) {
    std::int32_t low = 1000000;
    std::int32_t high = -1000000;
    std::int32_t checksum = 0;
    for (std::int32_t i = 0; i < n; ++i) {
        const auto x = (i * 7919 + 23) % 100003 - 50001;
        low = min_value<std::int32_t>(low, x);
        high = max_value<std::int32_t>(high, x);
        checksum = (checksum + (high - low) + x) % 1000000007;
    }
    return checksum + low + high;
}

static std::int64_t generic_minmax_pipeline() {
    return fold_i64(16000) + static_cast<std::int64_t>(fold_i32(9000));
}

static std::int64_t pair_sum(Pair<std::int64_t> p) {
    return p.first + p.second;
}

static std::int64_t generic_batch(std::int64_t n) {
    std::int64_t acc = 0;
    for (std::int64_t i = 0; i < n; ++i) {
        const auto p = make_pair_value<std::int64_t>(i, i * 2 + 1);
        acc = (acc + pair_sum(p)) % 1000000007LL;
    }
    return acc;
}

static std::int64_t nested_pair_sum(Pair<Pair<std::int64_t>> p) {
    return p.first.first + p.first.second + p.second.first + p.second.second;
}

static std::int64_t generic_nested_batch(std::int64_t n) {
    std::int64_t acc = 0;
    for (std::int64_t i = 0; i < n; ++i) {
        const auto left = make_pair_value<std::int64_t>(i, i + 1);
        const auto right = make_pair_value<std::int64_t>(i * 2, i * 2 + 1);
        const auto outer = make_pair_value<Pair<std::int64_t>>(left, right);
        acc = (acc + nested_pair_sum(outer)) % 1000000007LL;
    }
    return acc;
}

int main() {
    const auto minmax = generic_minmax_pipeline();
    const auto batch = generic_batch(60000000);
    const auto nested = generic_nested_batch(40000000);
    if (minmax != 1097579249LL || batch != 932200007LL || nested != 926400007LL) { return 1; }
    std::cout << "runtime minmax=" << minmax << " batch=" << batch << " nested=" << nested << "\n";
    return 0;
}
