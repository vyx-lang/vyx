#include <chrono>
#include <ctime>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr int kRounds = 3;
constexpr std::size_t kRepeats = 1'997'288;

double elapsed_ms(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
}

void print_result(const char* label, double ms, std::size_t bytes) {
    const double mib_per_second = (static_cast<double>(bytes) * 1000.0 / 1024.0 / 1024.0) / ms;
    std::cout << label << ": " << ms << " ms, " << mib_per_second << " MiB/s\n";
}

double cpu_ms(std::clock_t start) {
    return static_cast<double>(std::clock() - start) * 1000.0 / CLOCKS_PER_SEC;
}

}

int main() {
    const std::string pattern = "VYX-DCI-BINARY-IO-0123456789ABCDEF";
    std::string payload;
    payload.reserve(pattern.size() * kRepeats);
    for (std::size_t i = 0; i < kRepeats; ++i) payload += pattern;
    const auto bytes = payload.size();
    const char* path = "target/cpp_iostream_bench.bin";

    for (int round = 0; round < kRounds; ++round) {
        const auto write_start = std::chrono::steady_clock::now();
        const auto write_cpu_start = std::clock();
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
            if (!output) return 2;
        }
        print_result("cpp iostream write", elapsed_ms(write_start), bytes);
        print_result("cpp iostream write cpu", cpu_ms(write_cpu_start), bytes);

        std::vector<char> restored(bytes);
        const auto read_start = std::chrono::steady_clock::now();
        const auto read_cpu_start = std::clock();
        {
            std::ifstream input(path, std::ios::binary);
            input.read(restored.data(), static_cast<std::streamsize>(restored.size()));
            if (input.gcount() != static_cast<std::streamsize>(restored.size())) return 3;
        }
        print_result("cpp iostream read", elapsed_ms(read_start), bytes);
        print_result("cpp iostream read cpu", cpu_ms(read_cpu_start), bytes);
        if (std::string(restored.data(), restored.size()) != payload) return 4;
    }
    std::remove(path);
    std::cout << "cpp_iostream_bench OK\n";
    return 0;
}
