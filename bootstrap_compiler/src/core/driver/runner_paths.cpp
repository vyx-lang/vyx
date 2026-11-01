// Filesystem identities for test/benchmark discovery and output validation.
// Display paths remain independent of these keys (notably for fmt symlinks).
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/stat.h>
#endif

static char* runner_copy_key(const std::string& key) {
    char* result = static_cast<char*>(std::malloc(key.size() + 1));
    if (result) std::memcpy(result, key.c_str(), key.size() + 1);
    return result;
}

extern "C" char* vyx_runner_path_key(const char* path) {
    if (!path || !path[0]) return nullptr;
    try {
        std::error_code error;
        auto absolute = std::filesystem::absolute(std::filesystem::u8path(path), error);
        if (error) return nullptr;
        // Resolves existing symlinked parents even when the output is new.
        auto canonical = std::filesystem::weakly_canonical(absolute, error);
        if (error || !canonical.is_absolute()) return nullptr;
#ifdef _WIN32
        HANDLE file = CreateFileW(canonical.c_str(), 0,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            BY_HANDLE_FILE_INFORMATION info;
            const bool valid = GetFileInformationByHandle(file, &info) != 0;
            CloseHandle(file);
            if (!valid) return nullptr;
            return runner_copy_key("file:" + std::to_string(info.dwVolumeSerialNumber)
                + ":" + std::to_string(info.nFileIndexHigh) + ":" + std::to_string(info.nFileIndexLow));
        }
        const DWORD failure = GetLastError();
        if (failure != ERROR_FILE_NOT_FOUND && failure != ERROR_PATH_NOT_FOUND) return nullptr;
        // Windows report destinations are case-insensitive, including UTF-8 names.
        const auto wide = canonical.native();
        const int size = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
            wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr, 0);
        if (!size) return nullptr;
        std::wstring folded(static_cast<std::size_t>(size), L'\0');
        if (!LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, wide.data(),
            static_cast<int>(wide.size()), folded.data(), size, nullptr, nullptr, 0)) return nullptr;
        canonical = std::filesystem::path(folded);
#else
        struct stat info;
        if (::stat(canonical.c_str(), &info) == 0) {
            return runner_copy_key("file:" + std::to_string(info.st_dev) + ":" + std::to_string(info.st_ino));
        }
        if (errno != ENOENT && errno != ENOTDIR) return nullptr;
#endif
        const auto bytes = canonical.generic_u8string();
        return runner_copy_key("path:" + std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    } catch (...) {
        return nullptr;
    }
}

extern "C" void vyx_runner_path_key_free(char* key) { std::free(key); }
