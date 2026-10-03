#include <cstdint>
#include <iostream>

struct Payload {
    std::int64_t left;
    std::int64_t right;
    std::int64_t stamp;
    std::int64_t guard;
};

int main() {
    constexpr std::int64_t prime = 1000000007LL;
    std::int64_t checksum = 17;
    std::int64_t state = 29;
    for (std::int64_t i = 0; i < 100000000; ++i) {
        const Payload source{i, (i * 3 + 1) % prime, state, i % 97};
        Payload copied = source;
        copied.left = (copied.left + copied.stamp + 11) % prime;
        copied.right = (copied.right * 5 + copied.left + 7) % prime;
        if (source.left != i || source.guard != i % 97) return 1;
        state = (state * 33 + copied.right + source.stamp) % prime;
        checksum = (checksum * 131 + source.left * 3 + source.right * 5
            + copied.left * 7 + copied.right * 11 + copied.guard) % prime;
    }
    if (checksum != 682325596) return 1;
    std::cout << "memory_value total=" << checksum << "\n";
    return 0;
}
