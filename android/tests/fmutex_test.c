/* nvn_vk_fmutex.h: mutual exclusion and no lost wake-up. Eight threads bump a plain counter under the lock (some hold it long
 * enough to push the others into the wait path); the total must be exact. Run under ThreadSanitizer as well. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include "../nvn_vk_fmutex.h"

static fmutex mu = FMUTEX_INIT;
static uint64_t counter;
static uint64_t slow_hold;
enum { THREADS = 8, ROUNDS = 150000 };

static void *worker(void *arg) {
    unsigned id = (unsigned)(uintptr_t)arg;
    for (unsigned i = 0; i < ROUNDS; ++i) {
        fm_lock(&mu);
        counter++;
        if (id == 0 && i % 5000 == 0) { struct timespec ts = {0, 200000}; nanosleep(&ts, NULL); slow_hold++; }   /* held across a sleep: others must wait and be woken */
        fm_unlock(&mu);
    }
    return NULL;
}

int main(void) {
    pthread_t t[THREADS];
    for (unsigned i = 0; i < THREADS; ++i) assert(!pthread_create(&t[i], NULL, worker, (void *)(uintptr_t)i));
    for (unsigned i = 0; i < THREADS; ++i) pthread_join(t[i], NULL);
    assert(counter == (uint64_t)THREADS * ROUNDS);
    assert(atomic_load(&mu.v) == 0);
    printf("fmutex: pass (%llu increments exact, %llu long holds, lock free at the end)\n", (unsigned long long)counter, (unsigned long long)slow_hold);
    return 0;
}
