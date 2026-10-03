#include <ctime>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#elif defined(__APPLE__)
#include <sys/resource.h>
#endif

constexpr std::uint64_t kMod = 1'000'000'007ULL;
constexpr std::uint64_t kMask31 = 0x7fffffffULL;
#ifndef VYX_BENCH_PROFILE
#define VYX_BENCH_PROFILE "matched-o2"
#endif

struct Rng {
    std::uint64_t state;
    explicit Rng(std::uint64_t seed) : state((seed & kMask31) == 0 ? 1 : seed & kMask31) {}
    std::uint64_t next() {
        state = (state * 1'103'515'245ULL + 12'345ULL) & kMask31;
        return state;
    }
};

struct Config {
    std::uint64_t workload = 1'000'000'000ULL;
    std::size_t live_set = 1'000'000;
    std::size_t string_bytes = 1ULL << 20;
    std::uint64_t seed = 0x5eed1234ULL;
    std::string selected_case = "all";
    std::string profile = VYX_BENCH_PROFILE;
    std::string hash_policy = "deterministic-splitmix64";
    std::uint64_t clock_rate = CLOCKS_PER_SEC;
    bool reserve = true;
};

std::uint64_t parse_u64(std::string_view text, std::uint64_t fallback) {
    try {
        std::size_t used = 0;
        const auto value = std::stoull(std::string(text), &used, 0);
        return used == text.size() ? value : fallback;
    } catch (...) { return fallback; }
}

Config parse_config(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--help" || arg == "-h") {
            std::cout << "--case=all|vec|dict|string --workload=N --live-set=N --string-bytes=N "
                         "--seed=N --profile=matched-o2|native-release "
                         "--hash-policy=native|deterministic-splitmix64 --clock-rate=N [--no-reserve]\n";
            std::exit(0);
        }
        auto value = [&](std::string_view prefix, std::uint64_t& target) {
            if (arg.starts_with(prefix)) { target = parse_u64(arg.substr(prefix.size()), target); return true; }
            return false;
        };
        if (arg.starts_with("--case=")) cfg.selected_case = std::string(arg.substr(7));
        else if (arg.starts_with("--profile=")) cfg.profile = std::string(arg.substr(10));
        else if (arg.starts_with("--hash-policy=")) cfg.hash_policy = std::string(arg.substr(14));
        else if (value("--clock-rate=", cfg.clock_rate)) {}
        else if (value("--workload=", cfg.workload)) {}
        else if (arg.starts_with("--live-set=")) cfg.live_set = static_cast<std::size_t>(parse_u64(arg.substr(11), cfg.live_set));
        else if (arg.starts_with("--string-bytes=")) cfg.string_bytes = static_cast<std::size_t>(parse_u64(arg.substr(15), cfg.string_bytes));
        else if (value("--seed=", cfg.seed)) {}
        else if (arg == "--no-reserve") cfg.reserve = false;
    }
    cfg.live_set = std::max<std::size_t>(1, cfg.live_set);
    cfg.string_bytes = std::max<std::size_t>(1, cfg.string_bytes);
    return cfg;
}

constexpr std::uint64_t arithmetic_shift_right(std::uint64_t bits, unsigned shift) noexcept {
    const auto shifted = bits >> shift;
    return (bits & (std::uint64_t{1} << 63)) == 0 ? shifted : shifted | (~std::uint64_t{0} << (64 - shift));
}

constexpr std::uint64_t vyx_splitmix64(std::int64_t value) noexcept {
    auto h = static_cast<std::uint64_t>(value);
    h ^= arithmetic_shift_right(h, 30); h *= 0xbf58476d1ce4e5b9ULL;
    h ^= arithmetic_shift_right(h, 27); h *= 0x94d049bb133111ebULL;
    h ^= arithmetic_shift_right(h, 31); return h;
}

struct VyxSplitMix64 {
    std::size_t operator()(std::int64_t value) const noexcept { return static_cast<std::size_t>(vyx_splitmix64(value)); }
};

std::uint64_t peak_rss_bytes() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    return GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))
        ? static_cast<std::uint64_t>(counters.PeakWorkingSetSize) : 0;
#elif defined(__APPLE__)
    rusage usage{}; return getrusage(RUSAGE_SELF, &usage) == 0 ? static_cast<std::uint64_t>(usage.ru_maxrss) : 0;
#else
    std::ifstream status("/proc/self/status"); std::string key, unit; std::uint64_t value = 0;
    while (status >> key >> value >> unit) if (key == "VmHWM:") return value * 1024ULL;
    return 0;
#endif
}

std::uint64_t ticks_to_ns(std::uint64_t ticks, std::uint64_t rate) {
    return ticks == 0 || rate == 0 ? 0 : static_cast<std::uint64_t>(static_cast<long double>(ticks) * 1e9L / rate);
}

void emit_result(std::string_view bench, const Config& cfg, std::uint64_t setup, std::uint64_t op,
                 std::uint64_t checksum, std::size_t final_size) {
    const auto total = setup + op;
    std::cout << "language=cpp26 bench=" << bench << " workload=" << cfg.workload
        << " live_set=" << cfg.live_set << " string_bytes=" << cfg.string_bytes << " seed=" << cfg.seed
        << " reserve=" << (cfg.reserve ? 1 : 0) << " profile=" << cfg.profile
        << " api_mode=safe hash_policy=" << cfg.hash_policy << " timing_mode=c_clock_cpu"
        << " clock_rate=" << cfg.clock_rate << " setup_ticks=" << setup << " op_ticks=" << op
        << " total_ticks=" << total << " setup_ns=" << ticks_to_ns(setup, cfg.clock_rate)
        << " op_ns=" << ticks_to_ns(op, cfg.clock_rate) << " total_ns=" << ticks_to_ns(total, cfg.clock_rate)
        << " op_ops_per_sec=" << std::fixed << std::setprecision(3)
        << (op == 0 ? 0.0 : static_cast<double>(cfg.workload * cfg.clock_rate / static_cast<long double>(op)))
        << " final_size=" << final_size << " checksum=" << checksum << " peak_rss_bytes=" << peak_rss_bytes() << '\n';
}

void bench_vec(const Config& cfg) {
    Rng rng(cfg.seed); auto setup_begin = std::clock(); std::vector<std::int64_t> values;
    if (cfg.reserve) values.reserve(cfg.live_set);
    const auto initial = std::min<std::size_t>(cfg.live_set, std::max<std::uint64_t>(1, cfg.workload));
    for (std::size_t i = 0; i < initial; ++i) values.push_back(static_cast<std::int64_t>(rng.next()));
    auto setup = std::clock() - setup_begin; std::uint64_t checksum = 0; auto op_begin = std::clock();
    for (std::uint64_t op = 0; op < cfg.workload; ++op) {
        const auto action = rng.next() % 100, index_draw = rng.next(), value = rng.next();
        if (action < 35 && values.size() < cfg.live_set) values.push_back(static_cast<std::int64_t>(value));
        else if (action < 55 && values.size() > 1) { checksum = (checksum + static_cast<std::uint64_t>(values.back())) % kMod; values.pop_back(); }
        else if (action < 80 && !values.empty()) { const auto i = index_draw % values.size(); checksum = (checksum + static_cast<std::uint64_t>(values.at(i)) + i) % kMod; }
        else if (!values.empty()) { const auto i = index_draw % values.size(); values.at(i) = static_cast<std::int64_t>(value); checksum = (checksum ^ value) % kMod; }
    }
    auto op_ticks = std::clock() - op_begin;
    if (!values.empty()) checksum = (checksum + static_cast<std::uint64_t>(values.front()) + static_cast<std::uint64_t>(values.back())) % kMod;
    emit_result("vec", cfg, setup, op_ticks, checksum, values.size());
}

template <typename Hasher> void bench_dict(const Config& cfg) {
    Rng rng(cfg.seed); const auto key_space = static_cast<std::int64_t>(std::max<std::size_t>(1, cfg.live_set));
    auto setup_begin = std::clock(); std::unordered_map<std::int64_t, std::int64_t, Hasher> values;
    if (cfg.reserve) values.reserve(cfg.live_set);
    const auto initial = std::min<std::size_t>(cfg.live_set, std::max<std::uint64_t>(1, cfg.workload));
    for (std::size_t i = 0; i < initial; ++i) {
        const auto key = static_cast<std::int64_t>(rng.next() % key_space);
        const auto value = static_cast<std::int64_t>(rng.next());
        values[key] = value;
    }
    auto setup = std::clock() - setup_begin; std::uint64_t checksum = 0; auto op_begin = std::clock();
    for (std::uint64_t op = 0; op < cfg.workload; ++op) {
        const auto action = rng.next() % 100, key = rng.next() % key_space, value = rng.next();
        if (action < 45) values[static_cast<std::int64_t>(key)] = static_cast<std::int64_t>(value);
        else if (action < 75) { auto it = values.find(static_cast<std::int64_t>(key)); if (it != values.end()) checksum = (checksum + static_cast<std::uint64_t>(it->second)) % kMod; }
        else if (action < 92) checksum = (checksum + values.erase(static_cast<std::int64_t>(key))) % kMod;
        else checksum = (checksum ^ static_cast<std::uint64_t>(values.contains(static_cast<std::int64_t>(key)))) % kMod;
    }
    emit_result("dict", cfg, setup, std::clock() - op_begin, checksum, values.size());
}

void bench_string(const Config& cfg) {
    Rng rng(cfg.seed); auto setup_begin = std::clock(); std::string value;
    if (cfg.reserve) value.reserve(cfg.string_bytes);
    const auto initial = std::min<std::size_t>(cfg.string_bytes / 2 + (cfg.string_bytes == 1 ? 1 : 0), std::max<std::uint64_t>(1, cfg.workload));
    for (std::size_t i = 0; i < initial; ++i) value.push_back(static_cast<char>(32 + rng.next() % 95));
    auto setup = std::clock() - setup_begin; std::uint64_t checksum = 0; auto op_begin = std::clock();
    for (std::uint64_t op = 0; op < cfg.workload; ++op) {
        const auto action = rng.next() % 100, index = rng.next(), byte = rng.next();
        if (action < 60) { if (value.size() < cfg.string_bytes) value.push_back(static_cast<char>(32 + byte % 95)); else value.clear(); }
        else if (action < 85 && !value.empty()) { const auto i = index % value.size(); checksum = (checksum + static_cast<unsigned char>(value.at(i)) + i) % kMod; }
        else value.clear();
    }
    if (!value.empty()) checksum = (checksum + static_cast<unsigned char>(value.front()) + static_cast<unsigned char>(value.back())) % kMod;
    emit_result("string", cfg, setup, std::clock() - op_begin, checksum, value.size());
}

int main(int argc, char** argv) {
    const auto cfg = parse_config(argc, argv);
    if (cfg.hash_policy != "native" && cfg.hash_policy != "deterministic-splitmix64") return 2;
    if (cfg.selected_case == "all" || cfg.selected_case == "vec") bench_vec(cfg);
    if (cfg.selected_case == "all" || cfg.selected_case == "dict") {
        if (cfg.hash_policy == "native") bench_dict<std::hash<std::int64_t>>(cfg);
        else bench_dict<VyxSplitMix64>(cfg);
    }
    if (cfg.selected_case == "all" || cfg.selected_case == "string") bench_string(cfg);
    return cfg.selected_case == "all" || cfg.selected_case == "vec" || cfg.selected_case == "dict" || cfg.selected_case == "string" ? 0 : 2;
}
