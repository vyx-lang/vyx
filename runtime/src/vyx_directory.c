#ifndef _WIN32
#include <dirent.h>
#include <errno.h>
#include <stdint.h>

/* Keep both readdir's symbol selection and struct dirent in the same native
 * translation unit. Darwin's inode ABI variants are selected by its headers;
 * a generated caller must never guess d_name's offset or select readdir itself.
 * A returned name remains borrowed until the next read or close of this DIR. */
void* vyx_posix_directory_open(const char* path) {
    return opendir(path);
}

const char* vyx_posix_directory_next(void* directory, int32_t* error) {
    errno = 0;
    struct dirent* entry = readdir((DIR*)directory);
    *error = entry ? 0 : errno;
    return entry ? entry->d_name : 0;
}

int32_t vyx_posix_directory_close(void* directory) {
    return closedir((DIR*)directory);
}
#endif
