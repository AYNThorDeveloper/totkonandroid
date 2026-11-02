/* Darwin libc/pthread extensions used by the shared NVN sources, forced in
 * with -include for those files only. */
#pragma once
#define _GNU_SOURCE 1
#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include <sys/syscall.h>
#include <unistd.h>

#define CLOCK_UPTIME_RAW CLOCK_MONOTONIC
#define CLOCK_MONOTONIC_RAW_APPROX CLOCK_MONOTONIC
static inline uint64_t clock_gettime_nsec_np(clockid_t clock) {
    struct timespec ts;
    if (clock_gettime(clock, &ts)) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
/* Darwin names only the calling thread. */
static inline int android_setname_self(const char *name) { return pthread_setname_np(pthread_self(), name); }
#define ANDROID_SETNAME_PICK(_1, _2, which, ...) which
#define pthread_setname_np(...) ANDROID_SETNAME_PICK(__VA_ARGS__, (pthread_setname_np)(__VA_ARGS__), android_setname_self(__VA_ARGS__))
static inline int android_threadid_np(uint64_t *id) { if (!id) return -1; *id = (uint64_t)syscall(SYS_gettid); return 0; }
#ifndef pthread_threadid_np
#define pthread_threadid_np(thread, id) android_threadid_np((id))
#endif
