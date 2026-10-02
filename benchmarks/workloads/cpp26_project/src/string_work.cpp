#include "work.hpp"

std::int64_t string_batch(std::int64_t n) {
    std::string s;
    s.reserve(4097);
    s.append("bench");
    for (std::int64_t i = 0; i < n; ++i) {
        s.push_back('-');
        s.append(std::to_string(i));
        if (s.size() > 4096) {
            s.resize(128);
        }
    }
    return static_cast<std::int64_t>(s.size());
}

std::string label_value(const std::string& prefix, std::int64_t value) {
    return prefix + ":" + std::to_string(value);
}

std::int64_t string_scan_batch(std::int64_t n) {
    std::int64_t total = 0;
    std::int64_t digits = 0;
    std::int64_t alpha = 0;
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
                total = (total + (c - '0') * 3) % 1000000007LL;
            } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
                alpha += 1;
                total = (total + static_cast<unsigned char>(c)) % 1000000007LL;
            } else {
                total = (total + 17) % 1000000007LL;
            }
        }
    }
    return (total + digits * 31 + alpha * 17) % 1000000007LL;
}
