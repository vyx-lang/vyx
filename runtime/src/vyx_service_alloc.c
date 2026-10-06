#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

/* The allocator and matching free remain in the calling executable's CRT.
 * Windows SDKs can link the backend DLL to a different CRT instance. Never
 * allocate in that DLL and free frontend storage in the executable (or vice
 * versa). Scope cleanup invokes this same release callback.
 */
extern void vyx_rt_service_track(void*, void (*)(void*));
extern void vyx_rt_service_untrack(void*);
extern void* vyx_rt_service_begin(void);
extern void vyx_rt_service_end(void*);
extern int32_t vyx_rt_service_active(void);
extern void vyx_runtime_memory_service_set(void (*)(void*, void (*)(void*)), void (*)(void*));
extern void* vyx_runtime_memory_service_begin(void);
extern void vyx_runtime_memory_service_end(void*);
struct ServiceScope { void* backend; void* runtime; };

void* vyx_frontend_service_begin(void) {
    struct ServiceScope* scope = malloc(sizeof(*scope));
    if (!scope) abort();
    scope->backend = vyx_rt_service_begin();
    vyx_runtime_memory_service_set(vyx_rt_service_track, vyx_rt_service_untrack);
    scope->runtime = vyx_runtime_memory_service_begin();
    return scope;
}
void vyx_frontend_service_end(void* mark) {
    struct ServiceScope* scope = mark;
    vyx_runtime_memory_service_end(scope->runtime);
    vyx_rt_service_end(scope->backend);
    if (!vyx_rt_service_active()) vyx_runtime_memory_service_set(NULL, NULL);
    free(scope);
}

void* vyx_frontend_alloc(uint64_t size) {
    if (size == 0 || size > SIZE_MAX) return NULL;
    void* p = malloc((size_t)size);
    vyx_rt_service_track(p, free);
    return p;
}
void vyx_frontend_free(void* p) {
    if (!p) return;
    vyx_rt_service_untrack(p);
    free(p);
}
