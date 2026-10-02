#include <cstdint>
#include <iostream>

struct Stepper {
    virtual ~Stepper() = default;
    virtual std::int64_t step(std::int64_t x) const = 0;
};

struct AddStepper final : Stepper {
    std::int64_t delta;
    explicit AddStepper(std::int64_t value) : delta(value) {}
    std::int64_t step(std::int64_t x) const override { return x + delta; }
};

struct MixStepper final : Stepper {
    std::int64_t factor;
    explicit MixStepper(std::int64_t value) : factor(value) {}
    std::int64_t step(std::int64_t x) const override { return x * factor + 17; }
};

template <class T>
static std::int64_t generic_step(const T& stepper, std::int64_t x) {
    return stepper.step(x);
}

[[gnu::noinline]]
static std::int64_t dynamic_step(const Stepper& stepper, std::int64_t x) {
    return stepper.step(x);
}

static std::int64_t generic_static(std::int64_t n) {
    const AddStepper stepper{7};
    constexpr std::int64_t prime = 1000000007LL;
    std::int64_t checksum = 41;
    for (std::int64_t i = 0; i < n; ++i) {
        const auto value = generic_step(stepper, i);
        if (value != i + 7) return -1;
        checksum = (checksum * 131 + value * 3 + i) % prime;
    }
    return checksum;
}

static std::int64_t concrete_trait(std::int64_t n) {
    const MixStepper stepper{3};
    constexpr std::int64_t prime = 1000000007LL;
    std::int64_t checksum = 43;
    for (std::int64_t i = 0; i < n; ++i) {
        const auto value = stepper.step(i);
        if (value != i * 3 + 17) return -1;
        checksum = (checksum * 131 + value * 5 + i * 2) % prime;
    }
    return checksum;
}

static std::int64_t dynamic_trait(std::int64_t n) {
    const AddStepper add{7};
    const MixStepper mix{3};
    constexpr std::int64_t prime = 1000000007LL;
    std::int64_t checksum = 47;
    for (std::int64_t i = 0; i < n; ++i) {
        const bool use_add = ((i + checksum) % 3) == 0;
        const Stepper& selected = use_add ? static_cast<const Stepper&>(add)
                                          : static_cast<const Stepper&>(mix);
        const auto value = dynamic_step(selected, i);
        if (use_add) {
            if (value != i + 7) return -1;
        } else {
            if (value != i * 3 + 17) return -1;
        }
        checksum = (checksum * 131 + value * 7 + i * 3) % prime;
    }
    return checksum;
}

int main() {
    const auto generic = generic_static(30000000);
    const auto concrete = concrete_trait(30000000);
    const auto dynamic = dynamic_trait(40000000);
    if (generic != 198010502 || concrete != 635720229 || dynamic != 902501151) return 1;
    std::cout << "runtime generic=" << generic << " concrete=" << concrete
              << " dynamic=" << dynamic << "\n";
    return 0;
}
