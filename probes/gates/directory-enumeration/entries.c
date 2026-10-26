#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void* vyx_posix_directory_open(const char*);
const char* vyx_posix_directory_next(void*, int32_t*);
int32_t vyx_posix_directory_close(void*);

int main(int argc, char** argv) {
    if (argc < 3 || argc > 32) return 2;
    for (int pass = 0; pass < 2; ++pass) {
        void* directory = vyx_posix_directory_open(argv[1]);
        if (!directory) { perror("open directory"); return 1; }
        int found[32] = {0};
        int32_t error = 123;
        const char* name;
        int count = 0;
        while ((name = vyx_posix_directory_next(directory, &error))) {
            if (error || !*name || ++count > 64) {
                fputs("invalid or repeated directory entries\n", stderr); return 1;
            }
            if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
            int matched = 0;
            for (int i = 2; i < argc; ++i) {
                if (!strcmp(name, argv[i])) { ++found[i]; matched = 1; break; }
            }
            if (!matched) { fprintf(stderr, "unexpected name: %s\n", name); return 1; }
        }
        if (error || vyx_posix_directory_close(directory)) {
            fprintf(stderr, "directory completion failed: %d\n", error); return 1;
        }
        for (int i = 2; i < argc; ++i) {
            if (found[i] != 1) { fprintf(stderr, "missing/duplicate name: %s\n", argv[i]); return 1; }
        }
    }
    puts("native directory names PASS");
    return 0;
}
