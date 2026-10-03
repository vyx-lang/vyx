#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct Point {
    int32_t x;
    int32_t y;
};

struct Packed {
    char a;
    int32_t b;
} __attribute__((packed));

struct Aligned {
    int32_t x;
} __attribute__((aligned(16)));

struct Quad {
    int32_t a;
    int32_t b;
    int32_t c;
    int32_t d;
};

struct F2 {
    float a;
    float b;
};

int32_t point_sum(struct Point p) {
    return p.x + p.y;
}

int32_t named_sum(int32_t a, int32_t b) {
    return a + b;
}

int32_t packed_size(void) {
    return (int32_t)sizeof(struct Packed);
}

int32_t packed_off_b(void) {
    return (int32_t)offsetof(struct Packed, b);
}

int32_t aligned_size(void) {
    return (int32_t)sizeof(struct Aligned);
}

int32_t aligned_align(void) {
    return (int32_t)_Alignof(struct Aligned);
}

void fill_packed(struct Packed* p) {
    p->a = 1;
    p->b = 2;
}

int32_t read_packed_b(const struct Packed* p) {
    return p->b;
}

int32_t call_cb(int32_t (*cb)(int32_t), int32_t x) {
    return cb(x);
}

int32_t sum_ints(int32_t n, ...) {
    va_list ap;
    va_start(ap, n);
    int32_t s = 0;
    int32_t i = 0;
    while (i < n) {
        s += va_arg(ap, int32_t);
        i += 1;
    }
    va_end(ap);
    return s;
}

int32_t c_strlen_i32(const char* s) {
    if (s == NULL) {
        return -1;
    }
    return (int32_t)strlen(s);
}

int32_t vyx_add(int32_t a, int32_t b);
int32_t vyx_times(int32_t a, int32_t b);

int32_t call_exported(int32_t a, int32_t b) {
    return vyx_add(a, b);
}

int32_t call_export_name(int32_t a, int32_t b) {
    return vyx_times(a, b);
}

struct Point make_point(int32_t x, int32_t y) {
    struct Point p;
    p.x = x;
    p.y = y;
    return p;
}

int32_t quad_sum(struct Quad q) {
    return q.a + q.b + q.c + q.d;
}

int32_t packed_val(struct Packed p) {
    return (int32_t)p.a + p.b;
}

float f2_sum(struct F2 s) {
    return s.a + s.b;
}
