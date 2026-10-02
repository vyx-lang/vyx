#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <ucontext.h>
#endif

static _Thread_local void* g_vyx_fiber_current = NULL;

void vyx_fiber_set_current(void* slot) {
    g_vyx_fiber_current = slot;
}

void* vyx_fiber_get_current(void) {
    return g_vyx_fiber_current;
}

int32_t vyx_fiber_invoke(int64_t work, void* arg) {
    typedef int32_t (*vyx_fiber_work)(void*);
    if (work == 0) {
        return 1;
    }
    return ((vyx_fiber_work)(uintptr_t)work)(arg);
}

#if defined(_WIN32)
VOID CALLBACK vyx_fiber_win_entry(PVOID slot) {
    char* s = (char*)slot;
    int64_t work = 0;
    int64_t done = 1;
    void* arg = NULL;
    void* host = NULL;
    g_vyx_fiber_current = slot;
    memcpy(&work, s + 8, 8);
    memcpy(&arg, s + 16, 8);
    memcpy(&host, s + 48, 8);
    vyx_fiber_invoke(work, arg);
    memcpy(s + 24, &done, 8);
    g_vyx_fiber_current = NULL;
    if (host != NULL) {
        SwitchToFiber(host);
    }
}

void* vyx_fiber_win_proc(void) {
    return (void*)&vyx_fiber_win_entry;
}

void* vyx_fiber_win_host(void) {
    void* host = ConvertThreadToFiber(NULL);
    if (host != NULL) {
        return host;
    }
    if (IsThreadAFiber()) {
        return GetCurrentFiber();
    }
    return NULL;
}

void* vyx_fiber_win_create(void* slot, int64_t stack) {
    LPFIBER_START_ROUTINE proc = (LPFIBER_START_ROUTINE)vyx_fiber_win_proc();
    SIZE_T size = stack > 0 ? (SIZE_T)stack : ((SIZE_T)1 << 20);
    void* handle = CreateFiber(size, proc, slot);
    if (handle == NULL) {
        return NULL;
    }
    return handle;
}
#elif defined(__ANDROID__)
void* vyx_fiber_posix_host(void) { return NULL; }
void* vyx_fiber_posix_create(int64_t work, void* arg, void* slot) {
    (void)work;
    (void)arg;
    (void)slot;
    return NULL;
}
void vyx_fiber_posix_enter(void* host, void* fiber) {
    (void)host;
    (void)fiber;
}
void vyx_fiber_posix_yield(void* fiber, void* host) {
    (void)fiber;
    (void)host;
}
void vyx_fiber_posix_destroy(void* fiber) { (void)fiber; }

void vyx_bootstrap_fiber_current_set(void* fiber) { (void)fiber; }
void* vyx_bootstrap_fiber_current_get(void) { return NULL; }
void vyx_bootstrap_scheduler_ctx_set(void* ctx) { (void)ctx; }
void* vyx_bootstrap_scheduler_ctx_get(void) { return NULL; }
void* vyx_bootstrap_fiber_start(void) { return NULL; }
void vyx_bootstrap_fiber_stop(void* main_fiber) { (void)main_fiber; }
void* vyx_bootstrap_fiber_create(void* main_fiber, void* work, void* arg) {
    (void)main_fiber;
    (void)work;
    (void)arg;
    return NULL;
}
void vyx_bootstrap_fiber_switch(void* fiber) { (void)fiber; }
void vyx_bootstrap_fiber_yield(void) {}
void vyx_bootstrap_fiber_delete(void* fiber) { (void)fiber; }
int32_t vyx_bootstrap_fiber_finished(void* fiber) {
    (void)fiber;
    return 1;
}
#else
typedef struct VyxPosixFiber {
    ucontext_t ctx;
    ucontext_t* host;
    void* stack;
    int64_t work;
    void* arg;
    void* slot;
} VyxPosixFiber;

static _Thread_local VyxPosixFiber* g_vyx_posix_fiber = NULL;

static void vyx_fiber_posix_entry(void) {
    VyxPosixFiber* fiber = g_vyx_posix_fiber;
    int64_t done = 1;
    if (fiber == NULL) {
        return;
    }
    g_vyx_fiber_current = fiber->slot;
    vyx_fiber_invoke(fiber->work, fiber->arg);
    if (fiber->slot != NULL) {
        memcpy((char*)fiber->slot + 24, &done, 8);
    }
    g_vyx_fiber_current = NULL;
    if (fiber->host != NULL) {
        swapcontext(&fiber->ctx, fiber->host);
    }
}

void* vyx_fiber_posix_host(void) {
    ucontext_t* host = (ucontext_t*)calloc(1, sizeof(ucontext_t));
    if (host == NULL) {
        return NULL;
    }
    if (getcontext(host) != 0) {
        free(host);
        return NULL;
    }
    return host;
}

void* vyx_fiber_posix_create(int64_t work, void* arg, void* slot) {
    VyxPosixFiber* fiber = (VyxPosixFiber*)calloc(1, sizeof(VyxPosixFiber));
    if (fiber == NULL) {
        return NULL;
    }
    if (getcontext(&fiber->ctx) != 0) {
        free(fiber);
        return NULL;
    }
    fiber->stack = malloc(1u << 20);
    if (fiber->stack == NULL) {
        free(fiber);
        return NULL;
    }
    fiber->ctx.uc_stack.ss_sp = fiber->stack;
    fiber->ctx.uc_stack.ss_size = 1u << 20;
    fiber->ctx.uc_link = NULL;
    fiber->work = work;
    fiber->arg = arg;
    fiber->slot = slot;
    makecontext(&fiber->ctx, vyx_fiber_posix_entry, 0);
    return fiber;
}

void vyx_fiber_posix_enter(void* host, void* fiber) {
    VyxPosixFiber* f = (VyxPosixFiber*)fiber;
    if (f == NULL || host == NULL) {
        return;
    }
    f->host = (ucontext_t*)host;
    g_vyx_posix_fiber = f;
    g_vyx_fiber_current = f->slot;
    swapcontext((ucontext_t*)host, &f->ctx);
}

void vyx_fiber_posix_yield(void* fiber, void* host) {
    VyxPosixFiber* f = (VyxPosixFiber*)fiber;
    if (f == NULL || host == NULL) {
        return;
    }
    swapcontext(&f->ctx, (ucontext_t*)host);
}

void vyx_fiber_posix_destroy(void* fiber) {
    VyxPosixFiber* f = (VyxPosixFiber*)fiber;
    if (f == NULL) {
        return;
    }
    free(f->stack);
    free(f);
}
#endif
