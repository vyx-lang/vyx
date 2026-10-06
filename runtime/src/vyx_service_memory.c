#include <stdint.h>
#include <stdlib.h>

/* Optional service ownership. Ordinary programs never install these callbacks.
 * All allocations and releases stay in the executable's allocator. */
typedef void (*VyxTrack)(void*, void (*)(void*));
typedef void (*VyxUntrack)(void*);
static _Thread_local VyxTrack service_track;
static _Thread_local VyxUntrack service_untrack;
void vyx_runtime_memory_service_set(VyxTrack track, VyxUntrack untrack) {
    service_track = track;
    service_untrack = untrack;
}
void* vyx_runtime_malloc(uint64_t size) {
    if (size == 0 || size > SIZE_MAX) return NULL;
    void* p = malloc((size_t)size);
    if (p && service_track) service_track(p, free);
    return p;
}
void* vyx_runtime_calloc(uint64_t count, uint64_t size) {
    if (count == 0 || size == 0 || count > SIZE_MAX / size) return NULL;
    void* p = calloc((size_t)count, (size_t)size);
    if (p && service_track) service_track(p, free);
    return p;
}
void vyx_runtime_free(void* p) {
    if (!p) return;
    if (service_untrack) service_untrack(p);
    free(p);
}
void* vyx_runtime_realloc(void* old, uint64_t size) {
    if (size > SIZE_MAX) return NULL;
    if (size == 0) { vyx_runtime_free(old); return NULL; }
    void* next = realloc(old, (size_t)size);
    if (!next) return NULL;
    if (service_untrack) service_untrack(old);
    if (service_track) service_track(next, free);
    return next;
}
void vyx_runtime_memory_keep(void* p) {
    if (p && service_untrack) service_untrack(p);
}
