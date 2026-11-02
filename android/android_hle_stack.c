/* Stable host-call thunks for imports reached from a guest stack. */
#include "android_thread_slots.h"
#include <pthread.h>
#include <stdint.h>
#include <string.h>

#define ANDROID_HLE_THUNKS 4096
#define ANDROID_HLE_THUNK_BYTES 16

extern char android_hle_thunks[];
extern _Noreturn void runtime_fail(const char *message);
void *android_hle_targets[ANDROID_HLE_THUNKS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned used;

void *runtime_hle_wrap_named(void *function, const char *name) {
    (void)name;
    if (!function) return NULL;
    pthread_mutex_lock(&lock);
    unsigned i = 0;
    while (i < used && android_hle_targets[i] != function) ++i;
    if (i == used) {
        if (used == ANDROID_HLE_THUNKS) runtime_fail("Android host-call thunk table full");
        __atomic_store_n(&android_hle_targets[used++], function, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&lock);
    return android_hle_thunks + (size_t)i * ANDROID_HLE_THUNK_BYTES;
}

void *runtime_hle_wrap(void *function) { return runtime_hle_wrap_named(function, NULL); }
