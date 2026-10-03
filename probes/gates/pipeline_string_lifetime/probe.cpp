#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 3) { return 2; }
    const HMODULE runtime = LoadLibraryA(argv[1]);
    if (!runtime) { std::fprintf(stderr, "LoadLibrary error=%lu\n", GetLastError()); return 3; }
    const auto length = reinterpret_cast<int64_t (*)(const char*)>(GetProcAddress(runtime, "vyx_string_len"));
    const auto copy = reinterpret_cast<char* (*)(const char*)>(GetProcAddress(runtime, "from_cstr"));
    const auto copy_n = reinterpret_cast<char* (*)(const char*, int64_t)>(GetProcAddress(runtime, "from_cstr_len"));
    const auto view_n = reinterpret_cast<char* (*)(const char*, int64_t)>(GetProcAddress(runtime, "from_cstr_view_len"));
    const auto note = reinterpret_cast<void (*)(const char*, int64_t)>(GetProcAddress(runtime, "vyx_note_string_len"));
    const auto forget = reinterpret_cast<void (*)(const char*)>(GetProcAddress(runtime, "vyx_forget_string_len"));
    const auto content_stamp = reinterpret_cast<char* (*)(const char*)>(GetProcAddress(runtime, "vyx_bootstrap_file_content_stamp"));
    const auto file_stamp = reinterpret_cast<char* (*)(const char*)>(GetProcAddress(runtime, "vyx_bootstrap_file_stamp"));
    if (!length || !copy || !copy_n || !view_n || !note || !forget || !content_stamp || !file_stamp) {
        std::fprintf(stderr, "runtime is missing required string APIs\n"); return 4;
    }

    char stack_buffer[128] = "tiny";
    if (length(stack_buffer) != 4) { return 9; }
    std::strcpy(stack_buffer, "the same stack address now holds more bytes");
    if (length(stack_buffer) != static_cast<int64_t>(std::strlen(stack_buffer))) {
        std::fprintf(stderr, "FAIL unknown stack pointer reused stale length\n"); return 10;
    }
    // Force the last-pointer cache away; the direct cache must not retain an
    // unregistered external address either.
    (void)length("another external input");
    std::strcpy(stack_buffer, "short");
    if (length(stack_buffer) != 5 || std::strcmp(copy(stack_buffer), "short") != 0) {
        std::fprintf(stderr, "FAIL unknown direct-cache pointer reused stale length\n"); return 11;
    }

    bool reused = false;
    for (int attempt = 0; attempt < 1024 && !reused; ++attempt) {
        char* first = static_cast<char*>(std::malloc(128));
        if (!first) { return 12; }
        std::strcpy(first, "first lifetime");
        if (length(first) != 14) { std::free(first); return 13; }
        const auto address = reinterpret_cast<std::uintptr_t>(first);
        std::free(first);
        char* next = static_cast<char*>(std::malloc(128));
        if (!next) { return 14; }
        std::strcpy(next, "a new heap allocation owns the reused address");
        if (reinterpret_cast<std::uintptr_t>(next) == address) {
            reused = true;
            const auto size = static_cast<int64_t>(std::strlen(next));
            if (length(next) != size || std::strcmp(copy(next), next) != 0) {
                std::free(next);
                std::fprintf(stderr, "FAIL new heap lifetime inherited old length\n"); return 15;
            }
        }
        std::free(next);
    }
    if (!reused) { std::fprintf(stderr, "heap address reuse not observed\n"); return 16; }

    char registered[5] = {'a', '\0', 'b', 'c', '\0'};
    note(registered, 4);
    if (length(registered) != 4) { return 20; }
    const char* c_copy = copy(registered);
    if (length(c_copy) != 1 || std::strcmp(c_copy, "a") != 0) {
        std::fprintf(stderr, "FAIL C-string boundary honored registered byte-view length\n"); return 21;
    }
    const char* byte_copy = copy_n(registered, 4);
    if (length(byte_copy) != 4 || std::memcmp(byte_copy, registered, 4) != 0 || byte_copy[4] != 0) {
        std::fprintf(stderr, "FAIL explicit-length copy lost embedded NUL\n"); return 22;
    }
    const char* view = view_n(registered, 4);
    if (view != registered || length(view) != 4) { return 23; }
    forget(registered);
    if (length(registered) != 1) { return 24; }
    std::strcpy(registered, "xy");
    if (length(registered) != 2) { return 25; }
    if (length(copy(nullptr)) != 0) { return 26; }
    std::string large((1u << 20) + 17u, 'z');
    const char* large_copy = copy(large.c_str());
    if (length(large_copy) != static_cast<int64_t>(large.size())
        || std::memcmp(large_copy, large.data(), large.size()) != 0) {
        std::fprintf(stderr, "FAIL valid C string was silently capped at 1 MiB\n"); return 27;
    }

    const std::filesystem::path root(argv[2]);
    std::vector<std::string> paths;
    std::vector<std::string> expected_content;
    std::vector<std::string> expected_stat;
    for (int i = 0; i < 32; ++i) {
        const auto path = root / ("stamp_" + std::to_string(i) + ".txt");
        std::ofstream(path, std::ios::binary) << std::string(static_cast<size_t>(i * 31), 'x');
        paths.push_back(path.string());
        expected_content.emplace_back(content_stamp(paths.back().c_str()));
        expected_stat.emplace_back(file_stamp(paths.back().c_str()));
        if (expected_content.back().back() != ';' || expected_stat.back().back() != ';') { return 30; }
    }
    for (int pass = 0; pass < 64; ++pass) {
        for (int i = 31; i >= 0; --i) {
            const std::string actual_content(content_stamp(paths[static_cast<size_t>(i)].c_str()));
            const std::string actual_stat(file_stamp(paths[static_cast<size_t>(i)].c_str()));
            if (actual_content != expected_content[static_cast<size_t>(i)]
                || actual_stat != expected_stat[static_cast<size_t>(i)]) {
                std::fprintf(stderr, "FAIL repeated file stamp changed for unchanged file\n"); return 31;
            }
        }
    }
    std::puts("pipeline_string_cache: OK stack/heap/registered/forget/embedded-NUL/stamps");
    FreeLibrary(runtime);
    return 0;
}
