#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

static std::int64_t string_ops(std::int64_t n) {
    std::string s;
    s.reserve(static_cast<std::size_t>(n) * 8 + 4);
    s.append("seed");
    std::int64_t hits = 0;

    for (std::int64_t i = 0; i < n; ++i) {
        s.push_back('x');
        s.append(std::to_string(i));
        if (s.contains("x12")) { hits += 1; }
        if (s.starts_with("seed")) { hits += 2; }
    }

    const auto head = s.substr(0, 4);
    return static_cast<std::int64_t>(s.size()) + hits + static_cast<std::int64_t>(head.size());
}

static std::int64_t ascii_token_scan(std::int64_t n) {
    std::int64_t digits = 0;
    std::int64_t alpha = 0;
    std::int64_t separators = 0;
    std::int64_t checksum = 0;
    std::string s;
    s.reserve(32);

    for (std::int64_t i = 0; i < n; ++i) {
        s.clear();
        s.append("key-");
        s.append(std::to_string(i));
        s.append("-value-");
        s.append(std::to_string((i * 37) % 1009));

        for (char c : s) {
            if (c >= '0' && c <= '9') {
                digits += 1;
                checksum = (checksum + (c - '0') * 3) % 1000000007LL;
            } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
                alpha += 1;
                checksum = (checksum + static_cast<unsigned char>(c)) % 1000000007LL;
            } else {
                separators += 1;
                checksum = (checksum + 17) % 1000000007LL;
            }
        }
    }

    return checksum + digits + alpha + separators;
}

static void ascii_upper_inplace(std::string& value) noexcept {
    for (char& c : value) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - ('a' - 'A'));
        }
    }
}

static void ascii_lower_inplace(std::string& value) noexcept {
    for (char& c : value) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + ('a' - 'A'));
        }
    }
}

static std::int64_t string_transform(std::int64_t n) {
    std::int64_t hits = 0;
    std::int64_t total = 0;
    std::string raw;
    raw.reserve(32);

    for (std::int64_t i = 0; i < n; ++i) {
        raw.clear();
        raw.append("AbC-");
        raw.append(std::to_string(i));
        raw.append("-zZ");

        auto upper = raw;
        ascii_upper_inplace(upper);
        auto lower = upper;
        ascii_lower_inplace(lower);

        if (upper.contains("ABC")) { hits += 1; }
        if (lower.ends_with("zz")) { hits += 2; }

        const std::string_view mid(raw.data() + 4, raw.size() - 7);
        total += static_cast<std::int64_t>(mid.size());
    }

    return hits + total;
}

int main() {
    if (string_ops(1) != 12 || ascii_token_scan(1) != 934 || string_transform(1) != 4) {
        return 1;
    }

    const auto ops = string_ops(80000);
    const auto scan = ascii_token_scan(5000000);
    const auto transform = string_transform(3000000);
    if (ops != 708886 || scan != 344751141 || transform != 28888890) {
        return 1;
    }

    std::cout << "runtime string_ops=" << ops
              << " scan=" << scan
              << " transform=" << transform << '\n';
    return 0;
}
