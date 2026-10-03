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
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#include <linux/futex.h>
#endif
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif
#endif

typedef int64_t (*vyx_th_fn)(void*);

enum {
    VYX_PARK_EMPTY = 0,
    VYX_PARK_PARKED = 1,
    VYX_PARK_NOTIFIED = 2
};

typedef struct VyxParker {
    int32_t state;
#if !defined(_WIN32) && !defined(__linux__)
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int32_t inited;
#endif
} VyxParker;

typedef struct VyxThread {
#if defined(_WIN32)
    HANDLE handle;
#else
    pthread_t thread;
#endif
    vyx_th_fn fn;
    void* arg;
    int64_t result;
    VyxParker parker;
    int32_t joined;
    int32_t detached;
} VyxThread;

typedef struct VyxOnce {
    int32_t state;
#if defined(_WIN32)
    SRWLOCK mu;
    CONDITION_VARIABLE cv;
#else
    pthread_mutex_t mu;
    pthread_cond_t cv;
#endif
} VyxOnce;

typedef struct VyxBarrier {
    int32_t n;
    int32_t count;
    int32_t gen;
#if defined(_WIN32)
    SRWLOCK mu;
    CONDITION_VARIABLE cv;
#else
    pthread_mutex_t mu;
    pthread_cond_t cv;
#endif
} VyxBarrier;

static int32_t vyx_i32_cas(int32_t* p, int32_t expected, int32_t desired) {
#if defined(_WIN32)
    return (int32_t)InterlockedCompareExchange((volatile LONG*)p, (LONG)desired, (LONG)expected);
#else
    int32_t old = expected;
    __atomic_compare_exchange_n(p, &old, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return old;
#endif
}

static int32_t vyx_i32_swap(int32_t* p, int32_t desired) {
#if defined(_WIN32)
    return (int32_t)InterlockedExchange((volatile LONG*)p, (LONG)desired);
#else
    return __atomic_exchange_n(p, desired, __ATOMIC_SEQ_CST);
#endif
}

static int32_t vyx_i32_load(const int32_t* p) {
#if defined(_WIN32)
    return (int32_t)InterlockedCompareExchange((volatile LONG*)p, 0, 0);
#else
    return __atomic_load_n(p, __ATOMIC_SEQ_CST);
#endif
}

static int vyx_popcount_uptr(uintptr_t v) {
    int n = 0;
    while (v != 0) {
        n += (int)(v & 1u);
        v >>= 1;
    }
    return n;
}

static void vyx_parker_init(VyxParker* p) {
    memset(p, 0, sizeof(*p));
#if !defined(_WIN32) && !defined(__linux__)
    pthread_mutex_init(&p->mu, NULL);
    pthread_cond_init(&p->cv, NULL);
    p->inited = 1;
#endif
}

static void vyx_parker_destroy(VyxParker* p) {
#if !defined(_WIN32) && !defined(__linux__)
    if (p->inited) {
        pthread_cond_destroy(&p->cv);
        pthread_mutex_destroy(&p->mu);
        p->inited = 0;
    }
#else
    (void)p;
#endif
}

static void vyx_parker_wait_parked(VyxParker* p) {
#if defined(_WIN32)
    int32_t cmp = VYX_PARK_PARKED;
    while (vyx_i32_load(&p->state) == VYX_PARK_PARKED) {
        WaitOnAddress(&p->state, &cmp, sizeof(cmp), INFINITE);
    }
#elif defined(__linux__)
    while (vyx_i32_load(&p->state) == VYX_PARK_PARKED) {
        syscall(SYS_futex, &p->state, FUTEX_WAIT_PRIVATE, VYX_PARK_PARKED, NULL, NULL, 0);
    }
#else
    pthread_mutex_lock(&p->mu);
    while (vyx_i32_load(&p->state) == VYX_PARK_PARKED) {
        pthread_cond_wait(&p->cv, &p->mu);
    }
    pthread_mutex_unlock(&p->mu);
#endif
}

static int vyx_parker_wait_parked_ns(VyxParker* p, uint64_t ns) {
#if defined(_WIN32)
    DWORD ms = (DWORD)((ns + 999999ull) / 1000000ull);
    if (ms == 0 && ns > 0) { ms = 1; }
    int32_t cmp = VYX_PARK_PARKED;
    if (vyx_i32_load(&p->state) != VYX_PARK_PARKED) { return 0; }
    WaitOnAddress(&p->state, &cmp, sizeof(cmp), ms);
    return vyx_i32_load(&p->state) == VYX_PARK_PARKED ? 1 : 0;
#elif defined(__linux__)
    struct timespec ts;
    ts.tv_sec = (time_t)(ns / 1000000000ull);
    ts.tv_nsec = (long)(ns % 1000000000ull);
    if (vyx_i32_load(&p->state) != VYX_PARK_PARKED) { return 0; }
    syscall(SYS_futex, &p->state, FUTEX_WAIT_PRIVATE, VYX_PARK_PARKED, &ts, NULL, 0);
    return vyx_i32_load(&p->state) == VYX_PARK_PARKED ? 1 : 0;
#else
    struct timespec abs;
    clock_gettime(CLOCK_REALTIME, &abs);
    abs.tv_sec += (time_t)(ns / 1000000000ull);
    abs.tv_nsec += (long)(ns % 1000000000ull);
    if (abs.tv_nsec >= 1000000000L) {
        abs.tv_sec += 1;
        abs.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&p->mu);
    int rc = 0;
    while (vyx_i32_load(&p->state) == VYX_PARK_PARKED) {
        rc = pthread_cond_timedwait(&p->cv, &p->mu, &abs);
        if (rc == ETIMEDOUT) { break; }
    }
    pthread_mutex_unlock(&p->mu);
    return vyx_i32_load(&p->state) == VYX_PARK_PARKED ? 1 : 0;
#endif
}

static void vyx_parker_wake(VyxParker* p) {
#if defined(_WIN32)
    WakeByAddressSingle(&p->state);
#elif defined(__linux__)
    syscall(SYS_futex, &p->state, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
#else
    pthread_mutex_lock(&p->mu);
    pthread_cond_signal(&p->cv);
    pthread_mutex_unlock(&p->mu);
#endif
}

static void vyx_parker_park(VyxParker* p) {
    if (vyx_i32_cas(&p->state, VYX_PARK_NOTIFIED, VYX_PARK_EMPTY) == VYX_PARK_NOTIFIED) {
        return;
    }
    if (vyx_i32_cas(&p->state, VYX_PARK_EMPTY, VYX_PARK_PARKED) != VYX_PARK_EMPTY) {
        vyx_i32_cas(&p->state, VYX_PARK_NOTIFIED, VYX_PARK_EMPTY);
        return;
    }
    vyx_parker_wait_parked(p);
    vyx_i32_cas(&p->state, VYX_PARK_NOTIFIED, VYX_PARK_EMPTY);
}

static int32_t vyx_parker_park_ns(VyxParker* p, uint64_t ns) {
    if (vyx_i32_cas(&p->state, VYX_PARK_NOTIFIED, VYX_PARK_EMPTY) == VYX_PARK_NOTIFIED) {
        return 0;
    }
    if (vyx_i32_cas(&p->state, VYX_PARK_EMPTY, VYX_PARK_PARKED) != VYX_PARK_EMPTY) {
        vyx_i32_cas(&p->state, VYX_PARK_NOTIFIED, VYX_PARK_EMPTY);
        return 0;
    }
    int timed_out = vyx_parker_wait_parked_ns(p, ns);
    if (timed_out) {
        if (vyx_i32_cas(&p->state, VYX_PARK_PARKED, VYX_PARK_EMPTY) == VYX_PARK_PARKED) {
            return 1;
        }
    }
    vyx_i32_cas(&p->state, VYX_PARK_NOTIFIED, VYX_PARK_EMPTY);
    return 0;
}

static void vyx_parker_unpark(VyxParker* p) {
    int32_t old = vyx_i32_swap(&p->state, VYX_PARK_NOTIFIED);
    if (old == VYX_PARK_PARKED) {
        vyx_parker_wake(p);
    }
}

static _Thread_local VyxParker* g_tls_parker = NULL;

#if defined(_WIN32)
static DWORD WINAPI vyx_th_win_start(LPVOID raw) {
#else
static void* vyx_th_posix_start(void* raw) {
#endif
    VyxThread* t = (VyxThread*)raw;
    g_tls_parker = &t->parker;
    if (t->fn != NULL) {
        t->result = t->fn(t->arg);
    }
    g_tls_parker = NULL;
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

#if defined(_WIN32)
static SRWLOCK* vyx_as_srw(void* m) { return (SRWLOCK*)m; }
static CONDITION_VARIABLE* vyx_as_cv(void* c) { return (CONDITION_VARIABLE*)c; }
static SRWLOCK* vyx_as_rw(void* r) { return (SRWLOCK*)r; }
#else
static pthread_mutex_t* vyx_as_mu(void* m) { return (pthread_mutex_t*)m; }
static pthread_cond_t* vyx_as_cv(void* c) { return (pthread_cond_t*)c; }
static pthread_rwlock_t* vyx_as_rw(void* r) { return (pthread_rwlock_t*)r; }
#endif

int32_t vyx_th_available_parallelism(void) {
#if defined(_WIN32)
    DWORD_PTR proc_mask = 0;
    DWORD_PTR sys_mask = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &proc_mask, &sys_mask) && proc_mask != 0) {
        int n = vyx_popcount_uptr((uintptr_t)proc_mask);
        if (n > 0) { return n; }
    }
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    if (info.dwNumberOfProcessors > 0) {
        return (int32_t)info.dwNumberOfProcessors;
    }
    return 1;
#elif defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        int n = CPU_COUNT(&set);
        if (n > 0) { return n; }
    }
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int32_t)n : 1;
#elif defined(__APPLE__)
    int n = 0;
    size_t len = sizeof(n);
    if (sysctlbyname("hw.logicalcpu", &n, &len, NULL, 0) == 0 && n > 0) {
        return n;
    }
    long sc = sysconf(_SC_NPROCESSORS_ONLN);
    return sc > 0 ? (int32_t)sc : 1;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int32_t)n : 1;
#endif
}

void* vyx_th_spawn(void* fn, void* arg, uint64_t stack, const char* name) {
    VyxThread* t = (VyxThread*)malloc(sizeof(VyxThread));
    if (t == NULL) { return NULL; }
    memset(t, 0, sizeof(*t));
    t->fn = (vyx_th_fn)(uintptr_t)fn;
    t->arg = arg;
    vyx_parker_init(&t->parker);
#if defined(_WIN32)
    SIZE_T stack_size = (SIZE_T)stack;
    t->handle = CreateThread(NULL, stack_size, vyx_th_win_start, t, 0, NULL);
    if (t->handle == NULL) {
        vyx_parker_destroy(&t->parker);
        free(t);
        return NULL;
    }
    if (name != NULL && name[0] != 0) {
        wchar_t wname[64];
        size_t i = 0;
        while (i + 1 < 64 && name[i] != 0) {
            wname[i] = (wchar_t)(unsigned char)name[i];
            i += 1;
        }
        wname[i] = 0;
        SetThreadDescription(t->handle, wname);
    }
#else
    pthread_attr_t attr;
    pthread_attr_t* attrp = NULL;
    if (stack > 0) {
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, (size_t)stack);
        attrp = &attr;
    }
    int rc = pthread_create(&t->thread, attrp, vyx_th_posix_start, t);
    if (attrp != NULL) { pthread_attr_destroy(&attr); }
    if (rc != 0) {
        vyx_parker_destroy(&t->parker);
        free(t);
        return NULL;
    }
    if (name != NULL && name[0] != 0) {
#if defined(__APPLE__)
        pthread_setname_np(name);
#elif defined(__linux__)
        pthread_setname_np(t->thread, name);
#endif
    }
#endif
    return t;
}

int32_t vyx_th_join(void* handle, int64_t* result_out) {
    VyxThread* t = (VyxThread*)handle;
    if (t == NULL) { return 1; }
    if (t->joined) { return 1; }
#if defined(_WIN32)
    if (WaitForSingleObject(t->handle, INFINITE) != WAIT_OBJECT_0) { return 1; }
    CloseHandle(t->handle);
    t->handle = NULL;
#else
    if (pthread_join(t->thread, NULL) != 0) { return 1; }
#endif
    t->joined = 1;
    if (result_out != NULL) { *result_out = t->result; }
    vyx_parker_destroy(&t->parker);
    free(t);
    return 0;
}

int32_t vyx_th_detach(void* handle) {
    VyxThread* t = (VyxThread*)handle;
    if (t == NULL) { return 1; }
    if (t->joined || t->detached) { return 1; }
#if defined(_WIN32)
    CloseHandle(t->handle);
    t->handle = NULL;
#else
    if (pthread_detach(t->thread) != 0) { return 1; }
#endif
    t->detached = 1;
    return 0;
}

int64_t vyx_th_current_id(void) {
#if defined(_WIN32)
    return (int64_t)GetCurrentThreadId();
#else
    return (int64_t)pthread_self();
#endif
}

void vyx_th_yield(void) {
#if defined(_WIN32)
    SwitchToThread();
#else
    sched_yield();
#endif
}

void vyx_th_sleep_ns(uint64_t ns) {
#if defined(_WIN32)
    DWORD ms = (DWORD)((ns + 999999ull) / 1000000ull);
    if (ms == 0 && ns > 0) { ms = 1; }
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec = (time_t)(ns / 1000000000ull);
    ts.tv_nsec = (long)(ns % 1000000000ull);
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
#endif
}

void* vyx_th_thread_parker(void* handle) {
    VyxThread* t = (VyxThread*)handle;
    if (t == NULL) { return NULL; }
    return &t->parker;
}

void* vyx_th_current_parker(void) {
    if (g_tls_parker == NULL) {
        VyxParker* p = (VyxParker*)malloc(sizeof(VyxParker));
        if (p == NULL) { return NULL; }
        vyx_parker_init(p);
        g_tls_parker = p;
    }
    return g_tls_parker;
}

void vyx_th_park(void) {
    vyx_parker_park((VyxParker*)vyx_th_current_parker());
}

int32_t vyx_th_park_ns(uint64_t ns) {
    return vyx_parker_park_ns((VyxParker*)vyx_th_current_parker(), ns);
}

void vyx_th_unpark(void* parker) {
    if (parker == NULL) { return; }
    vyx_parker_unpark((VyxParker*)parker);
}

void* vyx_mu_new(void) {
#if defined(_WIN32)
    SRWLOCK* m = (SRWLOCK*)malloc(sizeof(SRWLOCK));
    if (m == NULL) { return NULL; }
    InitializeSRWLock(m);
    return m;
#else
    pthread_mutex_t* m = (pthread_mutex_t*)malloc(sizeof(pthread_mutex_t));
    if (m == NULL) { return NULL; }
    if (pthread_mutex_init(m, NULL) != 0) {
        free(m);
        return NULL;
    }
    return m;
#endif
}

void vyx_mu_lock(void* m) {
    if (m == NULL) { return; }
#if defined(_WIN32)
    AcquireSRWLockExclusive(vyx_as_srw(m));
#else
    pthread_mutex_lock(vyx_as_mu(m));
#endif
}

int32_t vyx_mu_try_lock(void* m) {
    if (m == NULL) { return 0; }
#if defined(_WIN32)
    return TryAcquireSRWLockExclusive(vyx_as_srw(m)) ? 1 : 0;
#else
    return pthread_mutex_trylock(vyx_as_mu(m)) == 0 ? 1 : 0;
#endif
}

void vyx_mu_unlock(void* m) {
    if (m == NULL) { return; }
#if defined(_WIN32)
    ReleaseSRWLockExclusive(vyx_as_srw(m));
#else
    pthread_mutex_unlock(vyx_as_mu(m));
#endif
}

void vyx_mu_destroy(void* m) {
    if (m == NULL) { return; }
#if defined(_WIN32)
    free(m);
#else
    pthread_mutex_destroy(vyx_as_mu(m));
    free(m);
#endif
}

void* vyx_rw_new(void) {
#if defined(_WIN32)
    SRWLOCK* r = (SRWLOCK*)malloc(sizeof(SRWLOCK));
    if (r == NULL) { return NULL; }
    InitializeSRWLock(r);
    return r;
#else
    pthread_rwlock_t* r = (pthread_rwlock_t*)malloc(sizeof(pthread_rwlock_t));
    if (r == NULL) { return NULL; }
    if (pthread_rwlock_init(r, NULL) != 0) {
        free(r);
        return NULL;
    }
    return r;
#endif
}

void vyx_rw_read(void* r) {
    if (r == NULL) { return; }
#if defined(_WIN32)
    AcquireSRWLockShared(vyx_as_rw(r));
#else
    pthread_rwlock_rdlock(vyx_as_rw(r));
#endif
}

void vyx_rw_write(void* r) {
    if (r == NULL) { return; }
#if defined(_WIN32)
    AcquireSRWLockExclusive(vyx_as_rw(r));
#else
    pthread_rwlock_wrlock(vyx_as_rw(r));
#endif
}

int32_t vyx_rw_try_read(void* r) {
    if (r == NULL) { return 0; }
#if defined(_WIN32)
    return TryAcquireSRWLockShared(vyx_as_rw(r)) ? 1 : 0;
#else
    return pthread_rwlock_tryrdlock(vyx_as_rw(r)) == 0 ? 1 : 0;
#endif
}

int32_t vyx_rw_try_write(void* r) {
    if (r == NULL) { return 0; }
#if defined(_WIN32)
    return TryAcquireSRWLockExclusive(vyx_as_rw(r)) ? 1 : 0;
#else
    return pthread_rwlock_trywrlock(vyx_as_rw(r)) == 0 ? 1 : 0;
#endif
}

void vyx_rw_unlock_read(void* r) {
    if (r == NULL) { return; }
#if defined(_WIN32)
    ReleaseSRWLockShared(vyx_as_rw(r));
#else
    pthread_rwlock_unlock(vyx_as_rw(r));
#endif
}

void vyx_rw_unlock_write(void* r) {
    if (r == NULL) { return; }
#if defined(_WIN32)
    ReleaseSRWLockExclusive(vyx_as_rw(r));
#else
    pthread_rwlock_unlock(vyx_as_rw(r));
#endif
}

void vyx_rw_destroy(void* r) {
    if (r == NULL) { return; }
#if defined(_WIN32)
    free(r);
#else
    pthread_rwlock_destroy(vyx_as_rw(r));
    free(r);
#endif
}

void* vyx_cv_new(void) {
#if defined(_WIN32)
    CONDITION_VARIABLE* c = (CONDITION_VARIABLE*)malloc(sizeof(CONDITION_VARIABLE));
    if (c == NULL) { return NULL; }
    InitializeConditionVariable(c);
    return c;
#else
    pthread_cond_t* c = (pthread_cond_t*)malloc(sizeof(pthread_cond_t));
    if (c == NULL) { return NULL; }
    if (pthread_cond_init(c, NULL) != 0) {
        free(c);
        return NULL;
    }
    return c;
#endif
}

void vyx_cv_wait(void* cv, void* mu) {
    if (cv == NULL || mu == NULL) { return; }
#if defined(_WIN32)
    SleepConditionVariableSRW(vyx_as_cv(cv), vyx_as_srw(mu), INFINITE, 0);
#else
    pthread_cond_wait(vyx_as_cv(cv), vyx_as_mu(mu));
#endif
}

int32_t vyx_cv_wait_ns(void* cv, void* mu, uint64_t ns) {
    if (cv == NULL || mu == NULL) { return 1; }
#if defined(_WIN32)
    DWORD ms = (DWORD)((ns + 999999ull) / 1000000ull);
    if (ms == 0 && ns > 0) { ms = 1; }
    if (SleepConditionVariableSRW(vyx_as_cv(cv), vyx_as_srw(mu), ms, 0)) { return 0; }
    return GetLastError() == ERROR_TIMEOUT ? 1 : 1;
#else
    struct timespec abs;
    clock_gettime(CLOCK_REALTIME, &abs);
    abs.tv_sec += (time_t)(ns / 1000000000ull);
    abs.tv_nsec += (long)(ns % 1000000000ull);
    if (abs.tv_nsec >= 1000000000L) {
        abs.tv_sec += 1;
        abs.tv_nsec -= 1000000000L;
    }
    int rc = pthread_cond_timedwait(vyx_as_cv(cv), vyx_as_mu(mu), &abs);
    return rc == ETIMEDOUT ? 1 : 0;
#endif
}

void vyx_cv_signal(void* cv) {
    if (cv == NULL) { return; }
#if defined(_WIN32)
    WakeConditionVariable(vyx_as_cv(cv));
#else
    pthread_cond_signal(vyx_as_cv(cv));
#endif
}

void vyx_cv_broadcast(void* cv) {
    if (cv == NULL) { return; }
#if defined(_WIN32)
    WakeAllConditionVariable(vyx_as_cv(cv));
#else
    pthread_cond_broadcast(vyx_as_cv(cv));
#endif
}

void vyx_cv_destroy(void* cv) {
    if (cv == NULL) { return; }
#if defined(_WIN32)
    free(cv);
#else
    pthread_cond_destroy(vyx_as_cv(cv));
    free(cv);
#endif
}

void* vyx_once_new(void) {
    VyxOnce* o = (VyxOnce*)malloc(sizeof(VyxOnce));
    if (o == NULL) { return NULL; }
    memset(o, 0, sizeof(*o));
#if defined(_WIN32)
    InitializeSRWLock(&o->mu);
    InitializeConditionVariable(&o->cv);
#else
    if (pthread_mutex_init(&o->mu, NULL) != 0) {
        free(o);
        return NULL;
    }
    if (pthread_cond_init(&o->cv, NULL) != 0) {
        pthread_mutex_destroy(&o->mu);
        free(o);
        return NULL;
    }
#endif
    return o;
}

int32_t vyx_once_call(void* once, void* fn, void* arg) {
    VyxOnce* o = (VyxOnce*)once;
    typedef void (*vyx_once_fn)(void*);
    if (o == NULL || fn == NULL) { return 1; }
    if (vyx_i32_load(&o->state) == 2) { return 0; }
#if defined(_WIN32)
    AcquireSRWLockExclusive(&o->mu);
    while (o->state == 1) {
        SleepConditionVariableSRW(&o->cv, &o->mu, INFINITE, 0);
    }
    if (o->state == 0) {
        o->state = 1;
        ReleaseSRWLockExclusive(&o->mu);
        ((vyx_once_fn)(uintptr_t)fn)(arg);
        AcquireSRWLockExclusive(&o->mu);
        o->state = 2;
        WakeAllConditionVariable(&o->cv);
    }
    ReleaseSRWLockExclusive(&o->mu);
#else
    pthread_mutex_lock(&o->mu);
    while (o->state == 1) {
        pthread_cond_wait(&o->cv, &o->mu);
    }
    if (o->state == 0) {
        o->state = 1;
        pthread_mutex_unlock(&o->mu);
        ((vyx_once_fn)(uintptr_t)fn)(arg);
        pthread_mutex_lock(&o->mu);
        o->state = 2;
        pthread_cond_broadcast(&o->cv);
    }
    pthread_mutex_unlock(&o->mu);
#endif
    return 0;
}

void vyx_once_destroy(void* once) {
    VyxOnce* o = (VyxOnce*)once;
    if (o == NULL) { return; }
#if defined(_WIN32)
    free(o);
#else
    pthread_cond_destroy(&o->cv);
    pthread_mutex_destroy(&o->mu);
    free(o);
#endif
}

void* vyx_bar_new(int32_t n) {
    if (n <= 0) { return NULL; }
    VyxBarrier* b = (VyxBarrier*)malloc(sizeof(VyxBarrier));
    if (b == NULL) { return NULL; }
    memset(b, 0, sizeof(*b));
    b->n = n;
#if defined(_WIN32)
    InitializeSRWLock(&b->mu);
    InitializeConditionVariable(&b->cv);
#else
    if (pthread_mutex_init(&b->mu, NULL) != 0) {
        free(b);
        return NULL;
    }
    if (pthread_cond_init(&b->cv, NULL) != 0) {
        pthread_mutex_destroy(&b->mu);
        free(b);
        return NULL;
    }
#endif
    return b;
}

int32_t vyx_bar_wait(void* bar) {
    VyxBarrier* b = (VyxBarrier*)bar;
    if (b == NULL) { return 0; }
    int32_t leader = 0;
#if defined(_WIN32)
    AcquireSRWLockExclusive(&b->mu);
    int32_t gen = b->gen;
    b->count += 1;
    if (b->count >= b->n) {
        b->count = 0;
        b->gen += 1;
        leader = 1;
        WakeAllConditionVariable(&b->cv);
    } else {
        while (gen == b->gen) {
            SleepConditionVariableSRW(&b->cv, &b->mu, INFINITE, 0);
        }
    }
    ReleaseSRWLockExclusive(&b->mu);
#else
    pthread_mutex_lock(&b->mu);
    int32_t gen = b->gen;
    b->count += 1;
    if (b->count >= b->n) {
        b->count = 0;
        b->gen += 1;
        leader = 1;
        pthread_cond_broadcast(&b->cv);
    } else {
        while (gen == b->gen) {
            pthread_cond_wait(&b->cv, &b->mu);
        }
    }
    pthread_mutex_unlock(&b->mu);
#endif
    return leader;
}

void vyx_bar_destroy(void* bar) {
    VyxBarrier* b = (VyxBarrier*)bar;
    if (b == NULL) { return; }
#if defined(_WIN32)
    free(b);
#else
    pthread_cond_destroy(&b->cv);
    pthread_mutex_destroy(&b->mu);
    free(b);
#endif
}

void* vyx_a64_new(int64_t init) {
#if defined(_WIN32)
    int64_t* p = (int64_t*)_aligned_malloc(sizeof(int64_t), 8);
#else
    int64_t* p = NULL;
    if (posix_memalign((void**)&p, 8, sizeof(int64_t)) != 0) { p = NULL; }
#endif
    if (p == NULL) { return NULL; }
    *p = init;
    return p;
}

int64_t vyx_a64_load(void* a) {
    int64_t* p = (int64_t*)a;
    if (p == NULL) { return 0; }
#if defined(_WIN32)
    return InterlockedCompareExchange64(p, 0, 0);
#else
    return __atomic_load_n(p, __ATOMIC_SEQ_CST);
#endif
}

void vyx_a64_store(void* a, int64_t v) {
    int64_t* p = (int64_t*)a;
    if (p == NULL) { return; }
#if defined(_WIN32)
    InterlockedExchange64(p, v);
#else
    __atomic_store_n(p, v, __ATOMIC_SEQ_CST);
#endif
}

int64_t vyx_a64_fetch_add(void* a, int64_t delta) {
    int64_t* p = (int64_t*)a;
    if (p == NULL) { return 0; }
#if defined(_WIN32)
    return InterlockedExchangeAdd64(p, delta);
#else
    return __atomic_fetch_add(p, delta, __ATOMIC_SEQ_CST);
#endif
}

int64_t vyx_a64_cas(void* a, int64_t expected, int64_t desired) {
    int64_t* p = (int64_t*)a;
    if (p == NULL) { return 0; }
#if defined(_WIN32)
    return InterlockedCompareExchange64(p, desired, expected);
#else
    int64_t old = expected;
    __atomic_compare_exchange_n(p, &old, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return old;
#endif
}

void vyx_a64_destroy(void* a) {
    if (a == NULL) { return; }
#if defined(_WIN32)
    _aligned_free(a);
#else
    free(a);
#endif
}
