/* Real backend: the page index in front of gpu_written_after (TOTK_VK_GW_INDEX) must answer exactly like the ring scan. A long
 * random history of GPU writes (short ones, long ones as a storage buffer noted with the rest of its pool, more than the ring
 * and the range list hold) is queried with the `done` values a replay can use (never below the completed fence). */
#include <pthread.h>
static int test_setname(pthread_t thread, const char *name) { (void)thread; (void)name; return 0; }
#define pthread_setname_np test_setname
#include "../nvn_vk_backend.c"
void *nvk_tiny_wb_record(VkCommandBuffer c, const tex_t *t, unsigned slot, size_t cap, unsigned slots) { (void)c; (void)t; (void)slot; (void)cap; (void)slots; return NULL; }
int nvk_tiny_wb_finish(void *s, uint64_t st) { (void)s; (void)st; return 0; }
void dm_guest_write_prepare(void *p, size_t n) { (void)p; (void)n; }
_Noreturn void runtime_fail(const char *m) { fprintf(stderr, "%s\n", m); abort(); }
int totk_vpool_backing(uintptr_t va, uint64_t len, uintptr_t *cpu) { (void)va; (void)len; (void)cpu; return 0; }
#include <assert.h>

static uint64_t rng = 0x1234567;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

int main(void) {
    const uint64_t base = 0x7000000000ull, span = 96ull << 20;
    uint64_t asked = 0, none = 0, hit = 0;
    for (int round = 0; round < 6; ++round) {
        uint64_t fa = 0, fb = 0;
        (void)fa; (void)fb;
        for (int step = 0; step < 60000; ++step) {
            unsigned r = (unsigned)(rnd() % 100);
            if (r < 30) {   /* a write */
                uint64_t a = base + (rnd() % span), n;
                unsigned k = (unsigned)(rnd() % 100);
                if (k < 70) n = 16 + rnd() % 4096;
                else if (k < 90) n = 4096 + rnd() % (60 * 4096);
                else if (k < 98) n = (rnd() % 8192 + 1) * 4096;           /* long: a pool tail */
                else n = base + span - a;                                    /* rest of the pool */
                if (n == 0) n = 1;
                nvk_note_gpu_write(a, n);
            } else if (r < 34) {
                fence_issued++;
            } else if (r < 38 && fence_done < fence_issued) {
                pthread_mutex_lock(&done_lock); fence_done += 1 + rnd() % (fence_issued - fence_done); pthread_mutex_unlock(&done_lock);
            } else {        /* a query */
                uint64_t a = base + (rnd() % span), n = 1 + (rnd() % 100 < 90 ? rnd() % 256 : rnd() % 65536);
                uint64_t lo = fence_done, hi = fence_issued + 1;
                uint64_t done = lo + rnd() % (hi - lo + 1);
                int fast = gpu_written_after(a, n, done), slow = gpu_written_scan(a, n, done);
                if (fast != slow) { fprintf(stderr, "mismatch: round %d step %d addr %#llx n %llu done %llu fast %d scan %d\n", round, step, (unsigned long long)a, (unsigned long long)n, (unsigned long long)done, fast, slow); return 1; }
                asked++; none += !slow; hit += slow;
            }
        }
        /* a fresh history for the next round with a different shape of the range list */
        gw_head = 0; gwb_n = 0; gwb_lost = 0; memset(gwp_fence, 0, sizeof gwp_fence);
    }
    uint64_t fast, scans, steps; nvk_gw_stats(&fast, &scans, &steps);
    printf("gw index: %llu queries (%llu none, %llu overlapping), %llu answered by the index, %llu scans, %llu ring steps\n",
        (unsigned long long)asked, (unsigned long long)none, (unsigned long long)hit, (unsigned long long)fast, (unsigned long long)scans, (unsigned long long)steps);
    assert(none > 0 && hit > 0 && (gw_index_on() ? fast > 0 : fast == 0));
    puts("gw index test ok");
    return 0;
}
