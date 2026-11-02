/* Real backend counter/replay code, with only Vulkan calls replaced. */
#include <pthread.h>
static int test_setname(pthread_t thread, const char *name) { (void)thread; (void)name; return 0; }
#define pthread_setname_np test_setname
#include "../nvn_vk_backend.c"
/* Tiny target write-backs live in nvn_vk_image.c, not linked here. */
void *nvk_tiny_wb_record(VkCommandBuffer c, const tex_t *t, unsigned slot, size_t cap, unsigned slots) { (void)c; (void)t; (void)slot; (void)cap; (void)slots; return NULL; }
int nvk_tiny_wb_finish(void *s, uint64_t st) { (void)s; (void)st; return 0; }
#include <assert.h>
static uint64_t values[QUERIES];
static unsigned live, peak;
static VkResult make_pool(VkDevice d, const VkQueryPoolCreateInfo *ci, const VkAllocationCallbacks *a, VkQueryPool *p) {
    (void)d; (void)a; assert(ci->queryCount == QUERIES); *p = (VkQueryPool)(uintptr_t)1; return VK_SUCCESS;
}
static void reset_query(VkDevice d, VkQueryPool p, uint32_t first, uint32_t n) {
    (void)d; (void)p; for (uint32_t i=first;i<first+n;i++) values[i]=0;
}
static void features(VkPhysicalDevice d, VkPhysicalDeviceFeatures *f) { (void)d; memset(f,0,sizeof *f); f->occlusionQueryPrecise=VK_TRUE; }
static void begin_query(VkCommandBuffer c, VkQueryPool p, uint32_t i, VkQueryControlFlags f) {
    (void)c; (void)p; (void)f; assert(!values[i]); values[i]=7; live++; if (live>peak) peak=live;
}
static void end_query(VkCommandBuffer c, VkQueryPool p, uint32_t i) { (void)c; (void)p; (void)i; }
static unsigned batch_calls, single_calls;
static VkResult get_query(VkDevice d, VkQueryPool p, uint32_t first, uint32_t n, size_t sz, void *out, VkDeviceSize stride, VkQueryResultFlags f) {
    (void)d; (void)p; (void)sz;
    if (f & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) {   /* TOTK_VK_QUERY_BATCH: a range; unused slots in it are not available */
        assert(stride == 2 * sizeof(uint64_t)); batch_calls++;
        int all = 1;
        for (uint32_t i = 0; i < n; ++i) { ((uint64_t *)out)[2 * i] = values[first + i]; ((uint64_t *)out)[2 * i + 1] = values[first + i] != 0; all &= values[first + i] != 0; }
        return all ? VK_SUCCESS : VK_NOT_READY;
    }
    *(uint64_t *)out=values[first]; assert(live); live--; single_calls++; return VK_SUCCESS;
}
PFN_vkCreateQueryPool vkCreateQueryPool=make_pool;
PFN_vkResetQueryPool vkResetQueryPool=reset_query;
PFN_vkGetPhysicalDeviceFeatures vkGetPhysicalDeviceFeatures=features;
PFN_vkCmdBeginQuery vkCmdBeginQuery=begin_query;
PFN_vkCmdEndQuery vkCmdEndQuery=end_query;
PFN_vkGetQueryPoolResults vkGetQueryPoolResults=get_query;
void dm_guest_write_prepare(void *p,size_t n) { (void)p; (void)n; }
_Noreturn void runtime_fail(const char *m) { fprintf(stderr,"%s\n",m); abort(); }
static void complete(void) {   /* as completion_thread does */
    Pending job = {0, post_ops, post_n, -1, 0};
    query_batch_read(&job);
    for(int i=0;i<post_n;i++) run_post(&post_ops[i]);
    qb_valid = 0; post_n=0;
}
PFN_vkCmdWriteTimestamp vkCmdWriteTimestamp;   /* GPU-timed counter reports are off (TOTK_VK_COUNTER_GPU_TIME) */
int main(void) {
    query_pool_init(); counter_reset(NVN_COUNTER_TYPE_SAMPLES_PASSED);
    uint64_t report[2]={0}, earlier[2]={0};
    /* Beyond both old limits, without a guest ResetCounter. */
    for(unsigned submit=0;submit<40;submit++) {
        for(unsigned q=0;q<1000;q++) { query_begin(VK_NULL_HANDLE); query_end(VK_NULL_HANDLE); }
        mtl_sub_counter((uintptr_t)report,NVN_COUNTER_TYPE_SAMPLES_PASSED); complete();
        assert(report[0]==(uint64_t)(submit+1)*7000); assert(qfree_n==QUERIES);
    }
    /* Reports before/after a reset must keep independent totals while in flight. */
    mtl_sub_counter((uintptr_t)earlier,NVN_COUNTER_TYPE_SAMPLES_PASSED);
    counter_reset(NVN_COUNTER_TYPE_SAMPLES_PASSED);
    query_begin(VK_NULL_HANDLE); query_end(VK_NULL_HANDLE);
    mtl_sub_counter((uintptr_t)report,NVN_COUNTER_TYPE_SAMPLES_PASSED); complete();
    assert(earlier[0]==280000 && report[0]==7 && qfree_n==QUERIES);
    /* Consecutive reports in one submission capture their point-in-time total. */
    query_begin(VK_NULL_HANDLE); query_end(VK_NULL_HANDLE);
    mtl_sub_counter((uintptr_t)earlier,NVN_COUNTER_TYPE_SAMPLES_PASSED);
    query_begin(VK_NULL_HANDLE); query_end(VK_NULL_HANDLE);
    mtl_sub_counter((uintptr_t)report,NVN_COUNTER_TYPE_SAMPLES_PASSED); complete();
    assert(earlier[0]==14 && report[0]==21);
    counter_release(qcounter); free(post_ops);
    if (query_batch_on()) assert(batch_calls == 41 && single_calls == 1);   /* one call per submission with 2+ queries; the lone query alone */
    else assert(!batch_calls && single_calls == 40003);
    printf("occlusion counters: pass (40000 queries, peak %u live; cumulative reports, resets in flight%s)\n",peak, query_batch_on() ? "; read in one call per submission" : "");
}
