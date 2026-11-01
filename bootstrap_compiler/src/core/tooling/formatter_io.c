/* Filesystem publication for the source formatter. Formatting itself is Vyx. */
#ifndef _WIN32
/* Project native sources compile as strict C11. Request the POSIX filesystem
   declarations explicitly; Darwin's extension set also supplies flock. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#endif
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <wchar.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

/* 1 IO, 2 changed since read, 3 link/nonregular input, 4 read-only input. */
#ifdef _WIN32
static wchar_t *fmt_wide(const char *path) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (!n) return NULL;
    wchar_t *out = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (out && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, out, n)) {
        free(out); return NULL;
    }
    return out;
}

static int fmt_same(HANDLE file, const char *expected, int64_t length) {
    LARGE_INTEGER size, zero;
    zero.QuadPart = 0;
    if (!GetFileSizeEx(file, &size) || size.QuadPart != length ||
        !SetFilePointerEx(file, zero, NULL, FILE_BEGIN)) return 0;
    char buf[65536];
    for (int64_t at = 0; at < length;) {
        DWORD want = (DWORD)((length - at) > (int64_t)sizeof(buf) ? (int64_t)sizeof(buf) : length - at), got = 0;
        if (!ReadFile(file, buf, want, &got, NULL) || got != want ||
            memcmp(buf, expected + at, want)) return 0;
        at += want;
    }
    return 1;
}

int32_t vyx_fmt_write_file(const char *path, const char *expected, int64_t old_len,
                          const char *replacement, int64_t new_len) {
    if (old_len < 0 || new_len < 0) return 1;
    wchar_t *name = fmt_wide(path);
    if (!name) return 1;
    /* Deny writes during compare/publication; allow ReplaceFile to publish. */
    HANDLE original = CreateFileW(name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (original == INVALID_HANDLE_VALUE) { free(name); return 1; }
    BY_HANDLE_FILE_INFORMATION info;
    OVERLAPPED lock = {0};
    int result = 1;
    if (!LockFileEx(original, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
        0, MAXDWORD, MAXDWORD, &lock)) goto close_original;
    if (!GetFileInformationByHandle(original, &info)) goto close_original;
    if ((info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
        info.nNumberOfLinks != 1) { result = 3; goto close_original; }
    if (info.dwFileAttributes & FILE_ATTRIBUTE_READONLY) { result = 4; goto close_original; }
    if (!fmt_same(original, expected, old_len)) { result = 2; goto close_original; }
    size_t chars = wcslen(name) + 96;
    wchar_t *temp = (wchar_t *)malloc(chars * sizeof(wchar_t));
    if (!temp) goto close_original;
    HANDLE output = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        swprintf(temp, chars, L"%ls.vyx-fmt-%lu-%llu-%u.tmp", name,
            (unsigned long)GetCurrentProcessId(), (unsigned long long)GetTickCount64(), attempt);
        output = CreateFileW(temp, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (output != INVALID_HANDLE_VALUE || GetLastError() != ERROR_FILE_EXISTS) break;
    }
    if (output == INVALID_HANDLE_VALUE) { free(temp); goto close_original; }
    int written = 1;
    for (int64_t at = 0; at < new_len;) {
        DWORD want = (DWORD)((new_len - at) > 65536 ? 65536 : new_len - at), got = 0;
        if (!WriteFile(output, replacement + at, want, &got, NULL) || got != want) {
            written = 0; break;
        }
        at += want;
    }
    if (!FlushFileBuffers(output)) written = 0;
    if (!CloseHandle(output)) written = 0;
    if (written) {
        HANDLE current = CreateFileW(name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
            NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        BY_HANDLE_FILE_INFORMATION now;
        int unchanged = current != INVALID_HANDLE_VALUE && GetFileInformationByHandle(current, &now) &&
            now.dwVolumeSerialNumber == info.dwVolumeSerialNumber &&
            now.nFileIndexHigh == info.nFileIndexHigh && now.nFileIndexLow == info.nFileIndexLow &&
            fmt_same(original, expected, old_len);
        if (current != INVALID_HANDLE_VALUE) CloseHandle(current);
        if (!unchanged) result = 2;
        else if (ReplaceFileW(name, temp, NULL, 0, NULL, NULL)) result = 0;
    }
    if (result != 0) DeleteFileW(temp);
    free(temp);
close_original:
    CloseHandle(original); free(name); return result;
}
#else
static int fmt_same(int fd, const char *expected, int64_t length) {
    struct stat info;
    if (fstat(fd, &info) || info.st_size != length || lseek(fd, 0, SEEK_SET) < 0) return 0;
    char buf[65536];
    for (int64_t at = 0; at < length;) {
        size_t want = (size_t)((length - at) > (int64_t)sizeof(buf) ? (int64_t)sizeof(buf) : length - at);
        ssize_t got = read(fd, buf, want);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0 || memcmp(buf, expected + at, (size_t)got)) return 0;
        at += got;
    }
    return 1;
}

int32_t vyx_fmt_write_file(const char *path, const char *expected, int64_t old_len,
                          const char *replacement, int64_t new_len) {
    if (old_len < 0 || new_len < 0) return 1;
    struct stat before, now;
    if (lstat(path, &before)) return 1;
    if (!S_ISREG(before.st_mode) || before.st_nlink != 1) return 3;
    if (!(before.st_mode & 0222)) return 4;
    int original = open(path, O_RDONLY | O_NOFOLLOW);
    if (original < 0) return 1;
    int result = 1;
    if (flock(original, LOCK_EX | LOCK_NB)) goto close_original;
    if (fstat(original, &now) || now.st_ino != before.st_ino || now.st_dev != before.st_dev ||
        !fmt_same(original, expected, old_len)) { result = 2; goto close_original; }
    size_t capacity = strlen(path) + 32;
    char *temp = (char *)malloc(capacity);
    if (!temp) goto close_original;
    snprintf(temp, capacity, "%s.vyx-fmt-XXXXXX", path);
    int output = mkstemp(temp);
    if (output < 0) { free(temp); goto close_original; }
    struct stat created;
    int written = fstat(output, &created) == 0;
    if (written && (created.st_uid != before.st_uid || created.st_gid != before.st_gid)) {
        written = fchown(output, before.st_uid, before.st_gid) == 0;
    }
    if (written) written = fchmod(output, before.st_mode & 07777) == 0;
    for (int64_t at = 0; written && at < new_len;) {
        size_t want = (size_t)((new_len - at) > 65536 ? 65536 : new_len - at);
        ssize_t got = write(output, replacement + at, want);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) { written = 0; break; }
        at += got;
    }
    if (fsync(output)) written = 0;
    if (close(output)) written = 0;
    if (written) {
        if (lstat(path, &now) || now.st_ino != before.st_ino || now.st_dev != before.st_dev ||
            now.st_nlink != 1 || !fmt_same(original, expected, old_len)) result = 2;
        else if (rename(temp, path) == 0) result = 0;
    }
    if (result) unlink(temp);
    free(temp);
close_original:
    close(original); return result;
}
#endif

/* A private, atomically reserved inventory file for each fmt invocation. */
char *vyx_fmt_temp_file(void) {
#ifdef _WIN32
    wchar_t dir[MAX_PATH + 1], name[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, dir);
    if (!n || n > MAX_PATH || !GetTempFileNameW(dir, L"vyf", 0, name)) return NULL;
    int bytes = WideCharToMultiByte(CP_UTF8, 0, name, -1, NULL, 0, NULL, NULL);
    char *path = bytes ? (char *)malloc((size_t)bytes) : NULL;
    if (!path || !WideCharToMultiByte(CP_UTF8, 0, name, -1, path, bytes, NULL, NULL)) {
        DeleteFileW(name); free(path); return NULL;
    }
    return path;
#else
    const char *dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = "/tmp";
    size_t size = strlen(dir) + 32;
    char *path = (char *)malloc(size);
    if (!path) return NULL;
    snprintf(path, size, "%s/vyx-fmt-XXXXXX", dir);
    int file = mkstemp(path);
    if (file < 0) { free(path); return NULL; }
    close(file); return path;
#endif
}

void vyx_fmt_temp_dispose(char *path) {
    if (!path) return;
#ifdef _WIN32
    wchar_t *name = fmt_wide(path);
    if (name) { DeleteFileW(name); free(name); }
#else
    unlink(path);
#endif
    free(path);
}

int32_t vyx_fmt_write_stream(int32_t error, const char *text, int64_t length) {
    if (length < 0) return 1;
    FILE *stream = error ? stderr : stdout;
#ifdef _WIN32
    int old_mode = _setmode(_fileno(stream), _O_BINARY);
#endif
    int result = fwrite(text, 1, (size_t)length, stream) == (size_t)length && fflush(stream) == 0 ? 0 : 1;
#ifdef _WIN32
    if (old_mode != -1) _setmode(_fileno(stream), old_mode);
#endif
    return result;
}
