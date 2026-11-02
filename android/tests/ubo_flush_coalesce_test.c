/* Actual GPU-flush path: fewer updates, identical bytes and immutable CPU overlays. */
#include <pthread.h>
static int test_setname(pthread_t t, const char *s) { (void)t; (void)s; return 0; }
#define pthread_setname_np test_setname
#include "../nvn_vk_backend.c"
#include <assert.h>
static uint8_t mem[16384] __attribute__((aligned(4096))), expected[sizeof mem];
static unsigned emitted, barriers;
_Noreturn void runtime_fail(const char *s) { fprintf(stderr, "%s\n", s); abort(); }
int totk_vpool_backing(uintptr_t va, uint64_t n, uintptr_t *cpu) { (void)va; (void)n; (void)cpu; return 0; }
static void update(VkCommandBuffer c, VkBuffer b, VkDeviceSize off, VkDeviceSize n, const void *data) {
    (void)c; assert(b == (VkBuffer)(uintptr_t)2 && off + n <= sizeof mem); memcpy(mem + off, data, n); emitted++;
}
static void barrier(VkCommandBuffer c, VkPipelineStageFlags s, VkPipelineStageFlags d, VkDependencyFlags f,
    uint32_t nm, const VkMemoryBarrier *mb, uint32_t nb, const VkBufferMemoryBarrier *bb, uint32_t ni, const VkImageMemoryBarrier *ib) {
    (void)c;(void)s;(void)d;(void)f;(void)mb;(void)bb;(void)ib; assert(nm == 1 && !nb && !ni); barriers++;
}
PFN_vkCmdUpdateBuffer vkCmdUpdateBuffer = update;
PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier = barrier;
static uint64_t base;
static void append(size_t off, size_t n, uint8_t v) {
    uint8_t bytes[256]; assert(n <= sizeof bytes && off + n <= sizeof mem); memset(bytes,v,n);
    ubo_append(base+off,bytes,n,0); ubo_unflushed++;
    if (base+off < ubo_unflushed_lo) ubo_unflushed_lo=base+off;
    if (base+off+n > ubo_unflushed_hi) ubo_unflushed_hi=base+off+n;
    memcpy(expected+off,bytes,n);
    uint8_t out[256]; nvk_ubo_read(base+off,n,out); assert(!memcmp(out,expected+off,n));
}
static void finish(unsigned want) {
    unsigned before=emitted, b=barriers; ubo_flush();
    assert(emitted-before == want && barriers == b+1 && !ubo_unflushed);
    assert(!memcmp(mem,expected,sizeof mem));
    uint8_t out[sizeof mem]; nvk_ubo_read(base,sizeof mem,out); assert(!memcmp(out,expected,sizeof out));
    for (size_t i=ubo_head;i<ubo_n;++i) assert(ubo_log[i].flushed && ubo_log[i].fence == nvk_pending_fence());
    fence_done=fence_issued+1; ubo_prune(); assert(!ubo_n); fence_issued=fence_done;
}
int main(void) {
    base=(uintptr_t)mem;
    pools=calloc(1,sizeof *pools); pool_cap=pool_count=1;
    pools[0]=(Pool){base,base+sizeof mem,mem,(VkDeviceMemory)(uintptr_t)1,(VkBuffer)(uintptr_t)2};
    int on=ubo_coalesce_on();
    append(64,64,1); append(64,64,2); append(80,16,3); append(64,64,4);
    finish(on?2:4);                                  /* earlier complete values discarded, partial stays ordered */
    append(64,64,5); append(64,16,6); finish(2);       /* a partial later update cannot remove an earlier tail */
    append(64,16,7); append(64,64,8); finish(on?1:2); /* a larger later update covers the whole smaller update */
    append(64,64,9); ubo_flush();                     /* prior flush: GPU consumers may have observed this value */
    unsigned before=emitted; append(64,64,10); finish(1); assert(emitted == before+1);
    /* Addresses sharing the planning bucket must preserve one another's contents. */
    size_t other=0;
    for(size_t off=128;off+64<=sizeof mem;off+=4) if(ubo_cover_slot(base+off)==ubo_cover_slot(base+64)){other=off;break;}
    assert(other); append(64,64,11); append(other,64,12); finish(2);
    /* Mixed overlapping updates: final GPU bytes must match sequential replay byte for byte. */
    uint32_t rng=123456;
    for(unsigned i=0;i<2000;++i) { rng=rng*1664525u+1013904223u; append(((rng>>8)%32)*16,16*((rng%8)+1),(uint8_t)i); }
    unsigned n=emitted; ubo_flush(); assert(!memcmp(mem,expected,sizeof mem));
    assert(emitted-n <= 2000); if(!on) assert(emitted-n==2000);
    uint64_t skipped=ubo_gpu_elided; assert(on ? skipped>=3 : !skipped);
    puts("GPU uniform flush: pass (overwrites removed, partial overlap, larger coverage, hash collision, prior flush boundary, CPU overlays, randomized byte equality)");
    printf("GPU updates eliminated in this synthetic test: %llu\n",(unsigned long long)skipped);
}
