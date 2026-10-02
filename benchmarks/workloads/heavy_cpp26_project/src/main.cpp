#include <bit>
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

static std::int64_t vyx_i64_bits(std::uint64_t bits) {
    return std::bit_cast<std::int64_t>(bits);
}

static std::int64_t vyx_vec_sort_value(std::int64_t i) {
    auto ui = static_cast<std::uint64_t>(i);
    auto lhs = ui * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
    auto rhs = ui * UINT64_C(2654435761);
    return vyx_i64_bits(lhs ^ rhs) % 1000000007LL;
}

static std::int64_t vyx_hashmap_value(std::int64_t i) {
    auto ui = static_cast<std::uint64_t>(i);
    return vyx_i64_bits(ui * UINT64_C(6364136223846793005) + UINT64_C(1)) % 1000000007LL;
}

static std::int64_t vyx_string_sort_value(std::int64_t i) {
    auto ui = static_cast<std::uint64_t>(i);
    return vyx_i64_bits(ui * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407)) % 999983LL;
}

static void append_i64(std::string& out, std::int64_t value) {
    char buffer[32];
    const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (error != std::errc{}) { std::abort(); }
    out.append(buffer, static_cast<std::size_t>(end - buffer));
}

// ── Vec 大规模排序 + 多趟扫描 ──────────────────────────────────────────────

static std::int64_t vec_sort_reduce(std::int64_t n) {
    std::vector<std::int64_t> v;
    v.reserve(static_cast<std::size_t>(n));
    for (std::int64_t i = 0; i < n; ++i) {
        v.push_back(vyx_vec_sort_value(i));
    }
    if (static_cast<std::int64_t>(v.size()) != n) { std::abort(); }

    std::sort(v.begin(), v.end());
    if (!std::is_sorted(v.begin(), v.end())) { std::abort(); }

    std::int64_t sum1 = 0;
    for (std::int64_t i = 0; i < n; ++i)
        sum1 = (sum1 + v[i] * ((i % 17) + 1)) % 1000000007LL;

    std::int64_t hits = 0;
    for (std::int64_t i = 0; i < 10000; ++i) {
        std::int64_t needle = (i * 99991LL) % 1000000007LL;
        if (std::binary_search(v.begin(), v.end(), needle)) ++hits;
    }

    std::int64_t diff_sum = 0;
    for (std::int64_t i = 1; i < n; ++i)
        diff_sum = (diff_sum + v[i] - v[i - 1]) % 1000000007LL;

    return (sum1 + hits * 1000 + diff_sum) % 1000000007LL;
}

// ── Vec 4路归并 ────────────────────────────────────────────────────────────

static std::int64_t vec_merge_sorted(std::int64_t n) {
    std::int64_t part = n / 4;
    std::vector<std::int64_t> a, b, c, d;
    a.reserve(part); b.reserve(part); c.reserve(part); d.reserve(part);
    for (std::int64_t i = 0; i < part; ++i) {
        a.push_back((i * 4 + 0) % 1000003);
        b.push_back((i * 4 + 1) % 1000003);
        c.push_back((i * 4 + 2) % 1000003);
        d.push_back((i * 4 + 3) % 1000003);
    }
    if (a.size() != static_cast<std::size_t>(part)
        || b.size() != static_cast<std::size_t>(part)
        || c.size() != static_cast<std::size_t>(part)
        || d.size() != static_cast<std::size_t>(part)) { std::abort(); }
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    std::sort(c.begin(), c.end());
    std::sort(d.begin(), d.end());

    std::vector<std::int64_t> ab(part * 2), cd(part * 2), result(n);
    std::merge(a.begin(), a.end(), b.begin(), b.end(), ab.begin());
    std::merge(c.begin(), c.end(), d.begin(), d.end(), cd.begin());
    std::merge(ab.begin(), ab.end(), cd.begin(), cd.end(), result.begin());
    if (static_cast<std::int64_t>(result.size()) != n
        || !std::is_sorted(result.begin(), result.end())) { std::abort(); }

    std::int64_t checksum = 0;
    std::int64_t ordered = 1;
    for (std::int64_t i = 1; i < n; ++i) {
        if (result[i] < result[i - 1]) ordered = 0;
        checksum = (checksum + result[i] * ((i % 13) + 1)) % 1000000007LL;
    }
    return checksum + ordered;
}

// ── HashMap i64 key ────────────────────────────────────────────────────────

static std::int64_t hashmap_int_stress(std::int64_t n) {
    std::unordered_map<std::int64_t, std::int64_t> m;
    m.reserve(static_cast<std::size_t>(n * 2));

    for (std::int64_t i = 0; i < n; ++i) {
        std::int64_t key = (i * 2654435761LL) ^ (i >> 16);
        std::int64_t val = vyx_hashmap_value(i);
        m.emplace(key, val);
    }
    if (static_cast<std::int64_t>(m.size()) != n) { std::abort(); }

    std::int64_t hits = 0, checksum = 0;
    for (std::int64_t i = 0; i < n * 2; ++i) {
        std::int64_t key = (i * 2654435761LL) ^ (i >> 16);
        auto it = m.find(key);
        if (it != m.end()) {
            ++hits;
            checksum = (checksum + it->second) % 1000000007LL;
        }
    }
    if (hits != n) { std::abort(); }

    for (std::int64_t i = 0; i < n; ++i) {
        std::int64_t key = (i * 2654435761LL) ^ (i >> 16);
        auto it = m.find(key);
        if (it != m.end())
            it->second = (it->second + i) % 1000000007LL;
    }

    std::int64_t final_sum = 0;
    for (std::int64_t i = 0; i < n; ++i) {
        std::int64_t key = (i * 2654435761LL) ^ (i >> 16);
        auto it = m.find(key);
        if (it != m.end())
            final_sum = (final_sum + it->second * ((i % 7) + 1)) % 1000000007LL;
    }

    return (hits * 1000 + checksum + final_sum) % 1000000007LL;
}

// ── HashMap string key ─────────────────────────────────────────────────────

static std::int64_t hashmap_string_stress(std::int64_t n) {
    std::unordered_map<std::string, std::int64_t> m;
    m.reserve(static_cast<std::size_t>(n * 2));

    for (std::int64_t i = 0; i < n; ++i) {
        std::string key;
        key.reserve(32);
        key += "key_";
        append_i64(key, i);
        key += '_';
        append_i64(key, (i * 48271) % 99991);
        m.emplace(std::move(key), i * 3 + 7);
    }
    if (static_cast<std::int64_t>(m.size()) != n) { std::abort(); }

    std::int64_t checksum = 0;
    for (std::int64_t i = 0; i < n; ++i) {
        std::string key;
        key.reserve(32);
        key += "key_";
        append_i64(key, i);
        key += '_';
        append_i64(key, (i * 48271) % 99991);
        auto it = m.find(key);
        if (it != m.end())
            checksum = (checksum + it->second * ((i % 11) + 1)) % 1000000007LL;
    }
    return checksum;
}

// ── 矩阵乘法 256×256 ───────────────────────────────────────────────────────

static std::int64_t matrix_mul(std::int64_t n) {
    std::int64_t size = n * n;
    std::vector<std::int64_t> a(size), b(size), c(size, 0);
    for (std::int64_t i = 0; i < size; ++i) {
        a[i] = (i * 48271 + 17) % 1009;
        b[i] = (i * 69621 + 31) % 1013;
    }
    for (std::int64_t row = 0; row < n; ++row)
        for (std::int64_t col = 0; col < n; ++col) {
            std::int64_t acc = 0;
            for (std::int64_t k = 0; k < n; ++k)
                acc = (acc + a[row * n + k] * b[k * n + col]) % 1000000007LL;
            c[row * n + col] = acc;
        }
    std::int64_t checksum = 0;
    for (std::int64_t i = 0; i < n; ++i) {
        checksum = (checksum + c[i * n + i]) % 1000000007LL;
        checksum = (checksum + c[i * n + (n - 1 - i)]) % 1000000007LL;
    }
    return checksum;
}

// ── 矩阵快速幂 4×4 ────────────────────────────────────────────────────────

static void mat4_mul(const std::int64_t* a, const std::int64_t* b,
                     std::int64_t* out, std::int64_t p) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            std::int64_t acc = 0;
            for (int k = 0; k < 4; ++k)
                acc = (acc + a[r * 4 + k] * b[k * 4 + c]) % p;
            out[r * 4 + c] = acc;
        }
}

static std::int64_t matrix_pow(std::int64_t k) {
    const std::int64_t p = 1000000007LL;
    std::int64_t base[16] = {1,1,0,0, 1,0,1,0, 0,1,0,1, 0,0,1,1};
    std::int64_t result[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    std::int64_t tmp[16];
    while (k > 0) {
        if (k & 1) { mat4_mul(result, base, tmp, p); std::copy(tmp, tmp+16, result); }
        mat4_mul(base, base, tmp, p); std::copy(tmp, tmp+16, base);
        k >>= 1;
    }
    std::int64_t checksum = 0;
    for (int i = 0; i < 16; ++i)
        checksum = (checksum + result[i] * ((i % 5) + 1)) % p;
    return checksum;
}

// ── 字符串大规模构建 ───────────────────────────────────────────────────────

static std::int64_t string_build_large(std::int64_t n) {
    std::string s;
    s.reserve(static_cast<std::size_t>(n * 16));
    for (std::int64_t i = 0; i < n; ++i) {
        s += "entry-";
        append_i64(s, i);
        s += ':';
        append_i64(s, (i * 48271 + 17) % 999983);
        s += ';';
    }
    return static_cast<std::int64_t>(s.size());
}

// ── 字符串大规模扫描 ───────────────────────────────────────────────────────

static std::int64_t string_scan_large(std::int64_t n) {
    std::string s;
    s.reserve(static_cast<std::size_t>(n * 16));
    for (std::int64_t i = 0; i < n; ++i) {
        s += "entry-";
        append_i64(s, i);
        s += ':';
        append_i64(s, (i * 48271 + 17) % 999983);
        s += ';';
    }
    std::int64_t freq[256] = {};
    for (unsigned char c : s) ++freq[c];
    std::int64_t checksum = 0;
    for (int j = 0; j < 256; ++j)
        checksum = (checksum + freq[j] * ((j % 17) + 1)) % 1000000007LL;
    return checksum;
}

// ── 字符串 Vec 排序 ────────────────────────────────────────────────────────

static std::int64_t string_vec_sort(std::int64_t n) {
    std::vector<std::string> v;
    v.reserve(static_cast<std::size_t>(n));
    for (std::int64_t i = 0; i < n; ++i) {
        std::int64_t val = vyx_string_sort_value(i);
        std::string value;
        value.reserve(32);
        value += "str_";
        append_i64(value, val);
        value += '_';
        append_i64(value, i);
        v.push_back(std::move(value));
    }
    std::sort(v.begin(), v.end());
    if (static_cast<std::int64_t>(v.size()) != n
        || !std::is_sorted(v.begin(), v.end())) { std::abort(); }
    std::int64_t checksum = 0;
    std::int64_t ordered = 1;
    for (std::int64_t i = 1; i < n; ++i) {
        if (v[i] < v[i - 1]) ordered = 0;
        checksum = (checksum + static_cast<std::int64_t>(v[i].size()) * ((i % 13) + 1)) % 1000000007LL;
    }
    return checksum + ordered;
}

// ── main ───────────────────────────────────────────────────────────────────

int main() {
    const auto r_vec_sort  = vec_sort_reduce(500000);
    const auto r_vec_merge = vec_merge_sorted(800000);
    const auto r_hmap_int  = hashmap_int_stress(200000);
    const auto r_hmap_str  = hashmap_string_stress(80000);
    const auto r_mat_mul   = matrix_mul(256);
    const auto r_mat_pow   = matrix_pow(1000000);
    const auto r_str_build = string_build_large(100000);
    const auto r_str_scan  = string_scan_large(50000);
    const auto r_str_sort  = string_vec_sort(50000);

    if (r_vec_sort != 777956373 || r_vec_merge != 991584335
        || r_hmap_int != 55567056 || r_hmap_str != 602159565
        || r_mat_mul != 456654910 || r_mat_pow != 364827586
        || r_str_build != 1877777 || r_str_scan != 8359923
        || r_str_sort != 5657112) { return 1; }

    std::cout << "runtime"
              << " vec_sort="  << r_vec_sort
              << " vec_merge=" << r_vec_merge
              << " hmap_int="  << r_hmap_int
              << " hmap_str="  << r_hmap_str
              << " mat_mul="   << r_mat_mul
              << " mat_pow="   << r_mat_pow
              << " str_build=" << r_str_build
              << " str_scan="  << r_str_scan
              << " str_sort="  << r_str_sort
              << "\n";
    return 0;
}
