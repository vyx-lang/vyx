#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <vector>

static std::int64_t vec_generic(std::int64_t n) {
    std::vector<std::int64_t> v;
    v.reserve(static_cast<std::size_t>(n));
    for (std::int64_t i = 0; i < n; ++i) {
        v.push_back((i * 17 + 3) % 100000);
    }
    std::int64_t sum = 0;
    std::int64_t i = 0;
    for (const auto x : v) {
        sum = (sum + x * ((i % 11) + 1)) % 1000000007LL;
        ++i;
    }
    if (i != n) { std::abort(); }
    return sum + static_cast<std::int64_t>(v.size());
}

static std::int64_t vec_sort_search(std::int64_t n, std::int64_t probes) {
    std::vector<std::int64_t> v;
    v.reserve(static_cast<std::size_t>(n));
    for (std::int64_t i = 0; i < n; ++i) {
        v.push_back((i * 48271 + 17) % 100003);
    }
    if (static_cast<std::int64_t>(v.size()) != n) { std::abort(); }

    std::sort(v.begin(), v.end());
    if (!std::is_sorted(v.begin(), v.end())) { std::abort(); }

    std::int64_t hits = 0;
    std::int64_t checksum = 0;
    for (std::int64_t i = 0; i < probes; ++i) {
        const auto needle = (i * 97) % 100003;
        const auto it = std::lower_bound(v.begin(), v.end(), needle);
        if (it != v.end() && *it == needle) {
            hits += 1;
            checksum = (checksum + *it) % 1000000007LL;
        }
    }

    return static_cast<std::int64_t>(v.size()) + v.front() + v.back() + hits + checksum;
}

int main() {
    const auto vec = vec_generic(1000000);
    const auto search = vec_sort_search(160000, 40000);
    if (vec != 997797807 || search != 991791866) { return 1; }
    std::cout << "runtime vec=" << vec << " search=" << search << "\n";
    return 0;
}
