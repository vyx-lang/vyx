#include <array>
#include <cstdint>
#include <cstdio>
#include <string_view>

namespace {

constexpr std::uint64_t C0 = 6364136223846793005ULL;
constexpr std::uint64_t C1 = 1442695040888963407ULL;
constexpr std::uint64_t C2 = 7046029254386353131ULL;
constexpr std::uint64_t C3 = 3202034522624059733ULL;
constexpr std::uint64_t C4 = 3935559000370003845ULL;
constexpr std::uint64_t C5 = 2691343689449507681ULL;
constexpr std::uint64_t C6 = 4768777513237032717ULL;
constexpr std::uint64_t C7 = 2405875930906139467ULL;
constexpr std::uint64_t ACCEPT_DIGEST = 6317825462749680818ULL;

constexpr std::uint64_t rotate_left(std::uint64_t value, unsigned amount) noexcept {
    return (value << amount) | (value >> (64U - amount));
}

constexpr int hex_nibble(char ch) noexcept {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

__declspec(noinline) bool parse_hex(
    std::string_view input,
    std::array<std::uint8_t, 32>& output) noexcept {
    if (input.size() != output.size() * 2U) {
        return false;
    }
    for (std::size_t i = 0; i < output.size(); ++i) {
        const int high = hex_nibble(input[i * 2U]);
        const int low = hex_nibble(input[i * 2U + 1U]);
        if (high < 0 || low < 0) {
            return false;
        }
        output[i] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return true;
}

__declspec(noinline) std::uint64_t arx_mix(
    const std::uint8_t* data,
    std::size_t count,
    std::uint64_t seed) noexcept {
    std::uint64_t state = seed ^ C2;
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint64_t keyed =
            static_cast<std::uint64_t>(data[i]) +
            static_cast<std::uint64_t>((i + 1U) * 257U);
        state ^= keyed * C0;
        state = rotate_left(state, static_cast<unsigned>((i * 7U + 13U) % 63U + 1U));
        state = state * C3 + C1;
    }
    return state ^ rotate_left(state, 17U);
}

__declspec(noinline) std::uint64_t state_walk(
    const std::uint8_t* data,
    std::size_t count,
    std::uint64_t seed) noexcept {
    std::uint64_t state = seed ^ C4;
    std::uint64_t score = C5;
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint64_t byte = data[i];
        switch ((state ^ byte ^ i) & 3U) {
        case 0:
            state = rotate_left(state + byte + C1, 5U);
            break;
        case 1:
            state = rotate_left(state ^ byte * C2, 11U) + C3;
            break;
        case 2:
            state = state * C0 ^ rotate_left(byte + C4, 17U);
            break;
        default:
            state = rotate_left(state + (byte << ((i & 7U) + 1U)), 23U) ^ C6;
            break;
        }
        score = rotate_left(score ^ state ^ byte * (i + 1U), 9U);
        score = score * C7 + C2;
    }
    return score ^ rotate_left(state, 29U);
}

std::uint64_t read_u64_le(const std::uint8_t* data) noexcept {
    std::uint64_t word = 0;
    for (unsigned i = 0; i < 8U; ++i) {
        word |= static_cast<std::uint64_t>(data[i]) << (i * 8U);
    }
    return word;
}

__declspec(noinline) std::uint64_t verify_token(
    const std::array<std::uint8_t, 32>& bytes) noexcept {
    const std::uint64_t left = arx_mix(bytes.data(), bytes.size(), C1);
    const std::uint64_t right = state_walk(bytes.data(), bytes.size(), left ^ C6);
    std::uint64_t fold = C5;
    for (unsigned block = 0; block < 4U; ++block) {
        const std::uint64_t word = read_u64_le(bytes.data() + block * 8U);
        fold ^= rotate_left(word + left, block * 11U + 3U);
        fold = fold * C0 + C3;
    }
    return rotate_left(fold ^ right, 27U) + (left ^ C4);
}

} // namespace

int main(int argc, char** argv) {
    std::array<std::uint8_t, 32> bytes{};
    if (argc != 2 || !parse_hex(argv[1], bytes)) {
        std::puts("INVALID");
        return 2;
    }

    const std::uint64_t digest = verify_token(bytes);
    if (digest == ACCEPT_DIGEST) {
        std::printf("ACCEPT %llu\n", static_cast<unsigned long long>(digest));
        return 0;
    }
    std::printf("REJECT %llu\n", static_cast<unsigned long long>(digest));
    return 1;
}
