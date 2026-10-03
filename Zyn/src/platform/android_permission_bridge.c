#include <SDL3/SDL_system.h>
#include <stdatomic.h>

enum { ZYN_PERMISSION_SLOTS = 16 };

struct zyn_permission_slot {
    atomic_int id;
    atomic_int state; /* 1 pending, 2 denied, 3 granted */
};

static struct zyn_permission_slot zyn_permission_slots[ZYN_PERMISSION_SLOTS];
#if defined(__ANDROID__)
static atomic_int zyn_permission_next_id = 1;

static void zyn_permission_reply(void *userdata, const char *permission, bool granted) {
    (void)permission;
    struct zyn_permission_slot *slot = userdata;
    atomic_store_explicit(&slot->state, granted ? 3 : 2, memory_order_release);
}
#endif

int zyn_android_permission_request(const char *permission) {
#if !defined(__ANDROID__)
    (void)permission;
    return -1;
#else
    if (!permission || !*permission) return -1;
    for (int i = 0; i < ZYN_PERMISSION_SLOTS; ++i) {
        struct zyn_permission_slot *slot = &zyn_permission_slots[i];
        int available = 0;
        if (!atomic_compare_exchange_strong(&slot->id, &available, -1)) continue;
        int id = atomic_fetch_add(&zyn_permission_next_id, 1);
        atomic_store_explicit(&slot->state, 1, memory_order_relaxed);
        atomic_store_explicit(&slot->id, id, memory_order_release);
        if (SDL_RequestAndroidPermission(permission, zyn_permission_reply, slot)) return id;
        atomic_store_explicit(&slot->state, 0, memory_order_relaxed);
        atomic_store_explicit(&slot->id, 0, memory_order_release);
        return -1;
    }
    return -1;
#endif
}

int zyn_android_permission_status(int id) {
    if (id <= 0) return -2;
    for (int i = 0; i < ZYN_PERMISSION_SLOTS; ++i) {
        struct zyn_permission_slot *slot = &zyn_permission_slots[i];
        if (atomic_load_explicit(&slot->id, memory_order_acquire) != id) continue;
        int state = atomic_load_explicit(&slot->state, memory_order_acquire);
        if (state == 3) return 1;
        if (state == 2) return 0;
        return -1;
    }
    return -2;
}

bool zyn_android_permission_release(int id) {
    if (id <= 0) return false;
    for (int i = 0; i < ZYN_PERMISSION_SLOTS; ++i) {
        struct zyn_permission_slot *slot = &zyn_permission_slots[i];
        if (atomic_load_explicit(&slot->id, memory_order_acquire) != id) continue;
        if (atomic_load_explicit(&slot->state, memory_order_acquire) == 1) return false;
        atomic_store_explicit(&slot->id, 0, memory_order_release);
        return true;
    }
    return false;
}
