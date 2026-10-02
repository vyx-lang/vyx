#include <cstdint>
#include <iostream>
#include <memory>

struct Payload {
    std::int64_t left;
    std::int64_t right;
    std::int64_t stamp;
    std::int64_t guard;
};

int main() {
    constexpr std::int64_t prime = 1000000007LL;
    std::int64_t checksum = 23;
    for (std::int64_t i = 0; i < 5000000; ++i) {
        auto owner = std::make_unique<Payload>(Payload{
            i, (i * 7 + 3) % prime, i * 13 + 5, i % 113});
        Payload value = *owner;
        value.left = (value.left + value.stamp + 19) % prime;
        value.right = (value.right * 3 + value.left) % prime;
        *owner = value;
        const Payload observed = *owner;
        if (observed.guard != i % 113
            || observed.left != (i + i * 13 + 5 + 19) % prime) return 1;
        checksum = (checksum * 131 + observed.left * 3 + observed.right * 5
            + observed.stamp * 7 + observed.guard) % prime;
    }
    if (checksum != 838378122) return 1;
    std::cout << "memory_unique total=" << checksum << "\n";
    return 0;
}
