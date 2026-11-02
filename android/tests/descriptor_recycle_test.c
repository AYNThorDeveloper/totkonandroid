/* Real backend allocator against a fake driver. No set may be issued twice
 * in a submission or recycled before its owning slot's timeline completes. */
#include <assert.h>
#include <pthread.h>
#include <sys/wait.h>
static int test_setname(pthread_t t, const char *s) { (void)t; (void)s; return 0; }
#define pthread_setname_np test_setname
#include "../nvn_vk_backend.c"
/* Tiny target write-backs live in nvn_vk_image.c, not linked here. */
void *nvk_tiny_wb_record(VkCommandBuffer c, const tex_t *t, unsigned slot, size_t cap, unsigned slots) { (void)c; (void)t; (void)slot; (void)cap; (void)slots; return NULL; }
int nvk_tiny_wb_finish(void *s, uint64_t st) { (void)s; (void)st; return 0; }
_Noreturn void runtime_fail(const char *m) { fprintf(stderr, "expected rejection: %s\n", m); _Exit(86); }
static unsigned fake_pool_count, driver_allocs, driver_resets, capacity = 100000;
static unsigned pool_used[128];
static VkDescriptorSetLayout layout_of[40000];
static VkResult create_pool(VkDevice d, const VkDescriptorPoolCreateInfo *i,
        const VkAllocationCallbacks *a, VkDescriptorPool *out) {
    (void)d; (void)a; assert(i->maxSets == 8192 && fake_pool_count < 127);
    *out = (VkDescriptorPool)(uintptr_t)++fake_pool_count; return VK_SUCCESS;
}
static VkResult alloc(VkDevice d, const VkDescriptorSetAllocateInfo *i, VkDescriptorSet *out) {
    (void)d; unsigned pool = (unsigned)(uintptr_t)i->descriptorPool;
    assert(i->descriptorSetCount == 1 && pool && pool <= fake_pool_count);
    if (pool_used[pool] == capacity) return VK_ERROR_OUT_OF_POOL_MEMORY;
    pool_used[pool]++; assert(driver_allocs + 1 < 40000);
    *out = (VkDescriptorSet)(uintptr_t)++driver_allocs;
    layout_of[driver_allocs] = i->pSetLayouts[0]; return VK_SUCCESS;
}
static VkResult reset(VkDevice d, VkDescriptorPool p, VkDescriptorPoolResetFlags flags) {
    (void)d; (void)flags; pool_used[(uintptr_t)p] = 0; driver_resets++; return VK_SUCCESS;
}
PFN_vkCreateDescriptorPool vkCreateDescriptorPool = create_pool;
PFN_vkAllocateDescriptorSets vkAllocateDescriptorSets = alloc;
PFN_vkResetDescriptorPool vkResetDescriptorPool = reset;
static _Atomic int waiter_started, waiter_finished;
static void *slot_waiter(void *unused) {
    (void)unused; atomic_store(&waiter_started, 1);
    assert(mtl_wait(ring[0].fence, UINT64_MAX));
    descriptor_slot_begin(0); atomic_store(&waiter_finished, 1); return NULL;
}
int main(void) {
    int on = desc_recycle_on();
    VkDescriptorSetLayout a = (VkDescriptorSetLayout)(uintptr_t)0x10;
    VkDescriptorSetLayout b = (VkDescriptorSetLayout)(uintptr_t)0x20;
    cb = (VkCommandBuffer)(uintptr_t)1;
    descriptor_slot_begin(0);
    VkDescriptorSet a1 = nvk_alloc_set(a), b1 = nvk_alloc_set(b), a2 = nvk_alloc_set(a);
    assert(a1 != a2 && a1 != b1 && a2 != b1);
    assert(layout_of[(uintptr_t)a1] == a && layout_of[(uintptr_t)b1] == b);
    ring[0].fence = 10;
    pid_t pid = fork(); assert(pid >= 0);
    if (!pid) { descriptor_slot_begin(0); _Exit(0); }
    int status; assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 86);
    pthread_t thread; assert(!pthread_create(&thread, NULL, slot_waiter, NULL));
    while (!atomic_load(&waiter_started)) sched_yield();
    struct timespec pause = {0, 20000000}; nanosleep(&pause, NULL);
    assert(!atomic_load(&waiter_finished));
    pthread_mutex_lock(&done_lock); fence_done = 10; pthread_cond_broadcast(&done_cv); pthread_mutex_unlock(&done_lock);
    pthread_join(thread, NULL); assert(atomic_load(&waiter_finished));
    unsigned allocated = driver_allocs;
    VkDescriptorSet next_b = nvk_alloc_set(b), next_a = nvk_alloc_set(a), next_a2 = nvk_alloc_set(a);
    if (on) assert(next_a == a1 && next_a2 == a2 && next_b == b1 && driver_allocs == allocated && !driver_resets);
    else assert(next_a != a1 && next_b != b1 && driver_allocs == allocated + 3 && driver_resets == 1);
    assert(nvk_alloc_set(a) != next_a && nvk_alloc_set(a) != next_a2); /* no same-submission recycling */
    ring_cur = 1; descriptor_slot_begin(1); assert(nvk_alloc_set(a) != a1); /* another slot owns its own sets */
    if (on) {
        ring_cur = 2; capacity = 2; descriptor_slot_begin(2);
        for (unsigned i = 0; i < 3; ++i) nvk_alloc_set(a);
        assert(ring[2].cur == 1 && ring[2].ndpool == 2);
        unsigned resets = driver_resets; descriptor_slot_begin(2);
        assert(driver_resets == resets + 2 && !desc_slots[2].total && !ring[2].cur);
        capacity = 100000;
        ring_cur = 3; descriptor_slot_begin(3);
        for (unsigned i = 0; i < DS_LAYOUTS + 1; ++i) nvk_alloc_set((VkDescriptorSetLayout)(uintptr_t)(0x1000 + i));
        assert(desc_slots[3].untracked && desc_slots[3].layouts == DS_LAYOUTS);
        descriptor_slot_begin(3); assert(!desc_slots[3].total && !desc_slots[3].layouts);
        ring_cur = 4; descriptor_slot_begin(4);
        for (unsigned i = 0; i <= DS_SETS; ++i) nvk_alloc_set(a);
        assert(desc_slots[4].total == DS_SETS && desc_slots[4].untracked);
        descriptor_slot_begin(4); assert(!desc_slots[4].total);
    }
    /* Live toggles must not make a set issued earlier in this submission free. */
    ring_cur = 5; descriptor_slot_begin(5);
    setenv("TOTK_VK_DESC_RECYCLE", "1", 1); nvk_live_gen++;
    VkDescriptorSet live_a = nvk_alloc_set(a);
    setenv("TOTK_VK_DESC_RECYCLE", "0", 1); nvk_live_gen++;
    VkDescriptorSet live_b = nvk_alloc_set(a); assert(live_a != live_b);
    setenv("TOTK_VK_DESC_RECYCLE", "1", 1); nvk_live_gen++;
    assert(nvk_alloc_set(a) != live_a); /* no reissue after off/on */
    descriptor_slot_begin(5); /* untracked allocation forces reset */
    assert(nvk_alloc_set(a) != live_a);
    setenv("TOTK_VK_DESC_RECYCLE", on ? "1" : "0", 1); nvk_live_gen++;
    for (unsigned i = 0; i < RING; ++i) {
        desc_slots[i].untracked = 1; descriptor_slot_begin(i);
        free(desc_slots[i].stock); desc_slots[i].stock = NULL;
    }
    printf("descriptor allocation recycling %s: pass (real fence wait, early reuse rejected, layouts, one issue per submission, independent slots, bounded stock and pool-overflow reset)\n", on ? "on" : "off");
}
