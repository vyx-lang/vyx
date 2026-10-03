#include "work.hpp"

namespace bench {

std::int64_t pair_sum(Pair<std::int64_t> p) {
    return p.left + p.right;
}

std::int64_t generic_batch(std::int64_t n) {
    std::int64_t total = 0;
    for (std::int64_t i = 0; i < n; ++i) {
        auto p = make_pair<std::int64_t>(i, i * 3);
        total += pair_sum(p);
    }
    return total;
}

std::int64_t nested_pair_sum(Pair<Pair<std::int64_t>> p) {
    return p.left.left + p.left.right + p.right.left + p.right.right;
}

std::int64_t generic_nested_batch(std::int64_t n) {
    std::int64_t total = 0;
    for (std::int64_t i = 0; i < n; ++i) {
        auto left = make_pair<std::int64_t>(i, i * 2 + 1);
        auto right = make_pair<std::int64_t>(i * 3 + 2, i * 5 + 3);
        auto nested = make_pair<Pair<std::int64_t>>(left, right);
        total = (total + nested_pair_sum(nested) * ((i % 7) + 1)) % 1000000007LL;
    }
    return total;
}

}
