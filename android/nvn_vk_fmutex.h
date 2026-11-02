/* A small mutex for the replay thread's hot locks (the image registry `lock` in nvn_vk_image.c and `reg_lock` in the backend).
 * bionic's pthread_mutex_lock/unlock go through outlined atomic helpers (__aarch64_cas2_acq / swp2_rel) plus type checks; the
 * 25 Oct Thor profile put those four symbols at ~14% of the translation thread (several lock pairs per bound texture per draw).
 * This is the three-state futex mutex (0 free, 1 locked, 2 locked with waiters) with inline atomics: the uncontended path is one
 * compare-and-swap in, one atomic subtract out. TOTK_FMUTEX=0 at build time (-DTOTK_FMUTEX=0) falls back to pthread. Hosts
 * without futex (the Mac test builds) use a mutex/condition shim for the wait, so the same algorithm runs in the host tests. */
#ifndef NVN_VK_FMUTEX_H
#define NVN_VK_FMUTEX_H
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#ifndef TOTK_FMUTEX
#define TOTK_FMUTEX 1
#endif
#if TOTK_FMUTEX
#ifdef __linux__
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
static inline void fm_wait(_Atomic uint32_t *a, uint32_t v) { syscall(SYS_futex, (uint32_t *)a, FUTEX_WAIT_PRIVATE, v, (void *)0, (void *)0, 0); }
static inline void fm_wake(_Atomic uint32_t *a) { syscall(SYS_futex, (uint32_t *)a, FUTEX_WAKE_PRIVATE, 1, (void *)0, (void *)0, 0); }
#else
static pthread_mutex_t fm_shim_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fm_shim_cv = PTHREAD_COND_INITIALIZER;
static inline void fm_wait(_Atomic uint32_t *a, uint32_t v) {
    pthread_mutex_lock(&fm_shim_mu);
    if (atomic_load_explicit(a, memory_order_relaxed) == v) pthread_cond_wait(&fm_shim_cv, &fm_shim_mu);
    pthread_mutex_unlock(&fm_shim_mu);
}
static inline void fm_wake(_Atomic uint32_t *a) { (void)a; pthread_mutex_lock(&fm_shim_mu); pthread_cond_broadcast(&fm_shim_cv); pthread_mutex_unlock(&fm_shim_mu); }
#endif
typedef struct { _Atomic uint32_t v; } fmutex;
#define FMUTEX_INIT {0}
static inline void fm_relax(void) {
#if defined(__aarch64__)
    __asm__ volatile("yield");
#elif defined(__x86_64__)
    __asm__ volatile("pause");
#endif
}
static inline void fm_lock_slow(fmutex *m) {
    for (int i = 0; i < 64; ++i) {   /* the holders keep it for a few hundred nanoseconds */
        uint32_t c = 0;
        if (atomic_load_explicit(&m->v, memory_order_relaxed) == 0 && atomic_compare_exchange_weak_explicit(&m->v, &c, 1, memory_order_acquire, memory_order_relaxed)) return;
        fm_relax();
    }
    uint32_t c = atomic_exchange_explicit(&m->v, 2, memory_order_acquire);
    while (c != 0) { fm_wait(&m->v, 2); c = atomic_exchange_explicit(&m->v, 2, memory_order_acquire); }
}
static inline void fm_lock(fmutex *m) {
    uint32_t c = 0;
    if (__builtin_expect(atomic_compare_exchange_strong_explicit(&m->v, &c, 1, memory_order_acquire, memory_order_relaxed), 1)) return;
    fm_lock_slow(m);
}
static inline void fm_unlock(fmutex *m) {
    if (__builtin_expect(atomic_fetch_sub_explicit(&m->v, 1, memory_order_release) == 1, 1)) return;
    atomic_store_explicit(&m->v, 0, memory_order_release);
    fm_wake(&m->v);
}
#else
typedef pthread_mutex_t fmutex;
#define FMUTEX_INIT PTHREAD_MUTEX_INITIALIZER
static inline void fm_lock(fmutex *m) { pthread_mutex_lock(m); }
static inline void fm_unlock(fmutex *m) { pthread_mutex_unlock(m); }
#endif
#endif
