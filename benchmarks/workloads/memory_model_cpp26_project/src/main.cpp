#include <cstdint>
#include <iostream>
#include <memory>

struct ValuePayload {
    std::int64_t left;
    std::int64_t right;
    std::int64_t stamp;
    std::int64_t guard;
};

struct OwnerPayload {
    std::int64_t left;
    std::int64_t right;
    std::int64_t stamp;
    std::int64_t guard;
};

struct SharedPayload {
    std::int64_t left;
    std::int64_t right;
    std::int64_t stamp;
};

static std::int64_t value_semantics(std::int64_t n) {
    constexpr std::int64_t prime = 1000000007LL;
    std::int64_t checksum = 17;
    std::int64_t state = 29;
    for (std::int64_t i = 0; i < n; ++i) {
        const ValuePayload source{i, (i * 3 + 1) % prime, state, i % 97};
        ValuePayload copied = source;
        copied.left = (copied.left + copied.stamp + 11) % prime;
        copied.right = (copied.right * 5 + copied.left + 7) % prime;
        if (source.left != i || source.guard != i % 97) return -1;
        state = (state * 33 + copied.right + source.stamp) % prime;
        checksum = (checksum * 131 + source.left * 3 + source.right * 5
            + copied.left * 7 + copied.right * 11 + copied.guard) % prime;
    }
    return checksum;
}

static std::int64_t unique_heap(std::int64_t n) {
    constexpr std::int64_t prime = 1000000007LL;
    std::int64_t checksum = 23;
    for (std::int64_t i = 0; i < n; ++i) {
        auto owner = std::make_unique<OwnerPayload>(OwnerPayload{
            i, (i * 7 + 3) % prime, i * 13 + 5, i % 113});
        OwnerPayload value = *owner;
        value.left = (value.left + value.stamp + 19) % prime;
        value.right = (value.right * 3 + value.left) % prime;
        *owner = value;
        const OwnerPayload observed = *owner;
        if (observed.guard != i % 113
            || observed.left != (i + i * 13 + 5 + 19) % prime) return -1;
        checksum = (checksum * 131 + observed.left * 3 + observed.right * 5
            + observed.stamp * 7 + observed.guard) % prime;
    }
    return checksum;
}

static std::int64_t shared_refcount(std::int64_t n) {
    constexpr std::int64_t prime = 1000000007LL;
    std::int64_t checksum = 31;
    for (std::int64_t i = 0; i < n; ++i) {
        auto owner = std::make_shared<SharedPayload>(SharedPayload{
            i, (i * 11 + 5) % prime, i * 17 + 9});
        if (owner.use_count() != 1) return -1;
        {
            auto first = owner;
            if (owner.use_count() != 2) return -1;
            {
                auto second = first;
                if (owner.use_count() != 3) return -1;
                const SharedPayload value = *second;
                checksum = (checksum * 131 + value.left * 3 + value.right * 5
                    + value.stamp * 7 + owner.use_count()) % prime;
            }
            if (owner.use_count() != 2) return -1;
            const SharedPayload value = *first;
            checksum = (checksum * 131 + value.left * 11 + value.right * 13
                + value.stamp * 17 + owner.use_count()) % prime;
        }
        if (owner.use_count() != 1) return -1;
    }
    return checksum;
}

int main() {
    const auto value = value_semantics(100000000);
    const auto unique = unique_heap(5000000);
    const auto shared = shared_refcount(5000000);
    if (value != 682325596 || unique != 838378122 || shared != 399739965) return 1;
    std::cout << "runtime value=" << value << " unique=" << unique
              << " shared=" << shared << "\n";
    return 0;
}
