#include <cstdint>
#include <iostream>
#include <memory>

struct Payload {
    std::int64_t left;
    std::int64_t right;
    std::int64_t stamp;
};

int main() {
    constexpr std::int64_t prime = 1000000007LL;
    std::int64_t checksum = 31;
    for (std::int64_t i = 0; i < 5000000; ++i) {
        auto owner = std::make_shared<Payload>(Payload{
            i, (i * 11 + 5) % prime, i * 17 + 9});
        if (owner.use_count() != 1) return 1;
        {
            auto first = owner;
            if (owner.use_count() != 2) return 2;
            {
                auto second = first;
                if (owner.use_count() != 3) return 3;
                const Payload value = *second;
                checksum = (checksum * 131 + value.left * 3 + value.right * 5
                    + value.stamp * 7 + owner.use_count()) % prime;
            }
            if (owner.use_count() != 2) return 4;
            const Payload value = *first;
            checksum = (checksum * 131 + value.left * 11 + value.right * 13
                + value.stamp * 17 + owner.use_count()) % prime;
        }
        if (owner.use_count() != 1) return 5;
    }
    if (checksum != 399739965) return 6;
    std::cout << "memory_shared total=" << checksum << "\n";
    return 0;
}
