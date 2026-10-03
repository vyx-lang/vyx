#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static int32_t g_host_argc = 0;
static char** g_host_argv = NULL;
static int g_host_ready = 0;

#ifdef _WIN32
static int vyx_is_wspace(unsigned char c) {
    return c == ' ' || c == '\t';
}

static int vyx_count_args(const char* p) {
    int n = 0;
    int in_q = 0;
    int in_arg = 0;
    if (p == NULL) { return 0; }
    for (; *p != 0; p++) {
        unsigned char c = (unsigned char)*p;
        if (in_q) {
            if (c == '"') { in_q = 0; }
        } else if (c == '"') {
            in_q = 1;
            in_arg = 1;
        } else if (vyx_is_wspace(c)) {
            if (in_arg) {
                n++;
                in_arg = 0;
            }
        } else {
            in_arg = 1;
        }
    }
    if (in_arg) { n++; }
    return n;
}

static char* vyx_dup_range(const char* start, const char* end) {
    size_t n = (size_t)(end - start);
    char* out = (char*)malloc(n + 1);
    if (out == NULL) { return NULL; }
    if (n > 0) { memcpy(out, start, n); }
    out[n] = 0;
    return out;
}

static void vyx_host_args_init(void) {
    const char* line;
    int expected;
    char** argv;
    int count;
    int in_q;
    const char* p;
    const char* arg_start;
    if (g_host_ready) { return; }
    g_host_ready = 1;
    line = GetCommandLineA();
    if (line == NULL) { return; }
    expected = vyx_count_args(line);
    if (expected <= 0) { return; }
    argv = (char**)malloc((size_t)expected * sizeof(char*));
    if (argv == NULL) { return; }
    memset(argv, 0, (size_t)expected * sizeof(char*));
    count = 0;
    p = line;
    while (count < expected && *p != 0) {
        while (vyx_is_wspace((unsigned char)*p)) { p++; }
        if (*p == 0) { break; }
        in_q = 0;
        if (*p == '"') {
            in_q = 1;
            p++;
        }
        arg_start = p;
        while (*p != 0) {
            if (in_q) {
                if (*p == '"') { break; }
            } else if (vyx_is_wspace((unsigned char)*p)) {
                break;
            }
            p++;
        }
        argv[count] = vyx_dup_range(arg_start, p);
        if (argv[count] == NULL) { argv[count] = (char*)calloc(1, 1); }
        count++;
        if (in_q && *p == '"') { p++; }
    }
    g_host_argc = count;
    g_host_argv = argv;
}
#else
static void vyx_host_args_init(void) {
    g_host_ready = 1;
}
#endif

int32_t vyx_host_argc(void) {
    vyx_host_args_init();
    return g_host_argc;
}

char** vyx_host_argv(void) {
    vyx_host_args_init();
    return g_host_argv;
}
