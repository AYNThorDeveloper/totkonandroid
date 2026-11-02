/* Recording thread (nvn_vk_record.c, TOTK_VK_RECORD_THREAD=1): every call reaches the driver in the order it was made,
 * whether it was queued (binds, dynamic state, draws, passes, memory barriers, queries, descriptor templates) or made
 * directly after the ring emptied (copies, image barriers, command buffer begin/end, submit); queued arguments are
 * copies (the caller may reuse its arrays at once); calls from another thread go straight to the driver; with the
 * switch off nothing is wrapped (second run). Thousands of draws also wrap the ring around. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
static int test_setname(pthread_t t, const char *name) { (void)t; (void)name; return 0; }
#define pthread_setname_np test_setname
#include "../nvn_vk_record.c"

VkDevice nvk_dev;
_Noreturn void nvk_die(const char *s) { fprintf(stderr, "%s\n", s); abort(); }

/* The "driver": logs (call id, first argument) in order; the recording thread and the test thread both call it, so
   a lock keeps the log consistent. */
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { int id; uint64_t arg; } calls[200000]; static unsigned ncalls;
static void note(int id, uint64_t arg) { pthread_mutex_lock(&log_lock); calls[ncalls].id = id; calls[ncalls++].arg = arg; pthread_mutex_unlock(&log_lock); }
static uint64_t last_sets[2], last_dyn, last_vp_w, last_rect_w, last_color0_view, last_template_word;
static void d_BindPipeline(VkCommandBuffer cb, VkPipelineBindPoint p, VkPipeline pipe) { (void)cb; (void)p; note(1, (uint64_t)(uintptr_t)pipe); }
static void d_BindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint p, VkPipelineLayout l, uint32_t f, uint32_t n, const VkDescriptorSet *s, uint32_t nd, const uint32_t *d) {
    (void)cb; (void)p; (void)l; (void)f; last_sets[0] = (uint64_t)(uintptr_t)s[0]; last_sets[1] = n > 1 ? (uint64_t)(uintptr_t)s[1] : 0; last_dyn = nd ? d[0] : 0; note(2, n); }
static void d_Draw(VkCommandBuffer cb, uint32_t n, uint32_t i, uint32_t f, uint32_t fi) { (void)cb; (void)i; (void)f; (void)fi; note(3, n); }
static void d_DrawIndexed(VkCommandBuffer cb, uint32_t n, uint32_t i, uint32_t f, int32_t v, uint32_t fi) { (void)cb; (void)i; (void)f; (void)v; (void)fi; note(4, n); }
static void d_SetViewport(VkCommandBuffer cb, uint32_t f, uint32_t n, const VkViewport *v) { (void)cb; (void)f; (void)n; last_vp_w = (uint64_t)v[0].width; note(5, n); }
static void d_SetScissor(VkCommandBuffer cb, uint32_t f, uint32_t n, const VkRect2D *r) { (void)cb; (void)f; (void)n; last_rect_w = r[0].extent.width; note(6, n); }
static void d_BeginRendering(VkCommandBuffer cb, const VkRenderingInfo *ri) {
    (void)cb; assert(ri->colorAttachmentCount == 2 && ri->pDepthAttachment && !ri->pStencilAttachment);
    last_color0_view = (uint64_t)(uintptr_t)ri->pColorAttachments[0].imageView; note(7, ri->renderArea.extent.width); }
static void d_EndRendering(VkCommandBuffer cb) { (void)cb; note(8, 0); }
static void d_PipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags s, VkPipelineStageFlags d, VkDependencyFlags f, uint32_t nm, const VkMemoryBarrier *mb,
    uint32_t nb, const VkBufferMemoryBarrier *bb, uint32_t ni, const VkImageMemoryBarrier *ib) {
    (void)cb; (void)s; (void)d; (void)f; (void)mb; (void)nb; (void)bb; (void)ib; note(ni ? 10 : 9, nm ? mb[0].srcAccessMask : 0); }
static void d_CopyBuffer(VkCommandBuffer cb, VkBuffer s, VkBuffer d, uint32_t n, const VkBufferCopy *r) { (void)cb; (void)s; (void)d; (void)n; note(11, r[0].size); }
static VkResult d_EndCommandBuffer(VkCommandBuffer cb) { (void)cb; note(12, 0); return VK_SUCCESS; }
static VkResult d_BeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo *i) { (void)cb; (void)i; note(13, 0); return VK_SUCCESS; }
static void d_SetDepthTestEnable(VkCommandBuffer cb, VkBool32 v) { (void)cb; note(14, v); }
static void d_UpdateTemplate(VkDevice dv, VkDescriptorSet s, VkDescriptorUpdateTemplate t, const void *data) { (void)dv; (void)t; last_template_word = *(const uint64_t *)data; note(15, (uint64_t)(uintptr_t)s); }
static void d_BeginQuery(VkCommandBuffer cb, VkQueryPool p, uint32_t q, VkQueryControlFlags f) { (void)cb; (void)p; (void)f; note(16, q); }
static void d_EndQuery(VkCommandBuffer cb, VkQueryPool p, uint32_t q) { (void)cb; (void)p; note(17, q); }
static void d_SetVertexInput(VkCommandBuffer cb, uint32_t nb, const VkVertexInputBindingDescription2EXT *b, uint32_t na, const VkVertexInputAttributeDescription2EXT *a) {
    (void)cb; (void)na; (void)a; note(18, nb ? b[0].stride : 0); }

/* Queue locking models the real queue shared with presentation. The submit gate proves that enqueuing a
   segment does not block translation while the recorder/driver is busy. */
static pthread_mutex_t queue_test_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uintptr_t queue_owner;
void nvk_queue_lock(void) { pthread_mutex_lock(&queue_test_lock); atomic_store(&queue_owner, (uintptr_t)pthread_self()); }
void nvk_queue_unlock(void) { assert(atomic_load(&queue_owner) == (uintptr_t)pthread_self()); atomic_store(&queue_owner, 0); pthread_mutex_unlock(&queue_test_lock); }
static pthread_mutex_t submit_gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t submit_cv = PTHREAD_COND_INITIALIZER;
static int hold_submit, submit_entered;
static VkResult d_QueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *si, VkFence f) {
    (void)q;
    assert(atomic_load(&queue_owner) == (uintptr_t)pthread_self());
    assert(n == 1 && si->commandBufferCount == 1 && !si->waitSemaphoreCount && !f);
    if (si->signalSemaphoreCount) {
        const VkTimelineSemaphoreSubmitInfo *timeline = si->pNext;
        assert(si->signalSemaphoreCount == 1 && timeline && timeline->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO
            && timeline->signalSemaphoreValueCount == 1 && timeline->pSignalSemaphoreValues[0] == 42);
        note(20, 42); return VK_SUCCESS;
    }
    assert(!si->pNext);
    pthread_mutex_lock(&submit_gate); submit_entered = 1; pthread_cond_broadcast(&submit_cv);
    while (hold_submit) pthread_cond_wait(&submit_cv, &submit_gate);
    pthread_mutex_unlock(&submit_gate);
    note(19, (uint64_t)(uintptr_t)si->pCommandBuffers[0]); return VK_SUCCESS;
}

static VkResult d_AllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo *ci, VkCommandBuffer *out) {
    (void)device; assert(atomic_load(&ring_head) == atomic_load(&ring_tail));
    assert(ci->commandBufferCount == 1); *out = (VkCommandBuffer)(uintptr_t)0x30; note(21, 1); return VK_SUCCESS;
}
static VkResult d_ResetCommandPool(VkDevice device, VkCommandPool pool, VkCommandPoolResetFlags flags) {
    (void)device; (void)pool; (void)flags; assert(atomic_load(&ring_head) == atomic_load(&ring_tail));
    note(22, 0); return VK_SUCCESS;
}
/* Every dispatch pointer nvn_vk_record.c takes; the ones not exercised here stay NULL. */
PFN_vkCmdBindPipeline vkCmdBindPipeline = d_BindPipeline; PFN_vkCmdBindDescriptorSets vkCmdBindDescriptorSets = d_BindDescriptorSets;
PFN_vkCmdBindIndexBuffer vkCmdBindIndexBuffer; PFN_vkCmdBindVertexBuffers vkCmdBindVertexBuffers; PFN_vkCmdDraw vkCmdDraw = d_Draw;
PFN_vkCmdDrawIndexed vkCmdDrawIndexed = d_DrawIndexed; PFN_vkCmdDispatch vkCmdDispatch; PFN_vkCmdSetViewport vkCmdSetViewport = d_SetViewport;
PFN_vkCmdSetScissor vkCmdSetScissor = d_SetScissor; PFN_vkCmdSetPrimitiveTopology vkCmdSetPrimitiveTopology; PFN_vkCmdSetCullMode vkCmdSetCullMode;
PFN_vkCmdSetFrontFace vkCmdSetFrontFace; PFN_vkCmdSetDepthBiasEnable vkCmdSetDepthBiasEnable; PFN_vkCmdSetDepthBias vkCmdSetDepthBias;
PFN_vkCmdSetBlendConstants vkCmdSetBlendConstants; PFN_vkCmdSetDepthTestEnable vkCmdSetDepthTestEnable = d_SetDepthTestEnable;
PFN_vkCmdSetDepthWriteEnable vkCmdSetDepthWriteEnable; PFN_vkCmdSetDepthCompareOp vkCmdSetDepthCompareOp;
PFN_vkCmdSetDepthBoundsTestEnable vkCmdSetDepthBoundsTestEnable; PFN_vkCmdSetRasterizerDiscardEnable vkCmdSetRasterizerDiscardEnable;
PFN_vkCmdSetStencilTestEnable vkCmdSetStencilTestEnable; PFN_vkCmdSetStencilOp vkCmdSetStencilOp; PFN_vkCmdSetStencilCompareMask vkCmdSetStencilCompareMask;
PFN_vkCmdSetStencilWriteMask vkCmdSetStencilWriteMask; PFN_vkCmdSetStencilReference vkCmdSetStencilReference;
PFN_vkCmdSetPrimitiveRestartEnable vkCmdSetPrimitiveRestartEnable; PFN_vkCmdBeginRendering vkCmdBeginRendering = d_BeginRendering;
PFN_vkCmdEndRendering vkCmdEndRendering = d_EndRendering; PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier = d_PipelineBarrier;
PFN_vkCmdBeginQuery vkCmdBeginQuery = d_BeginQuery; PFN_vkCmdEndQuery vkCmdEndQuery = d_EndQuery; PFN_vkCmdWriteTimestamp vkCmdWriteTimestamp;
PFN_vkCmdClearAttachments vkCmdClearAttachments; PFN_vkUpdateDescriptorSetWithTemplate vkUpdateDescriptorSetWithTemplate = d_UpdateTemplate;
PFN_vkBeginCommandBuffer vkBeginCommandBuffer = d_BeginCommandBuffer; PFN_vkEndCommandBuffer vkEndCommandBuffer = d_EndCommandBuffer;
PFN_vkResetCommandPool vkResetCommandPool = d_ResetCommandPool; PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers = d_AllocateCommandBuffers; PFN_vkQueueSubmit vkQueueSubmit = d_QueueSubmit;
PFN_vkQueueWaitIdle vkQueueWaitIdle; PFN_vkCmdCopyBuffer vkCmdCopyBuffer = d_CopyBuffer; PFN_vkCmdUpdateBuffer vkCmdUpdateBuffer;
PFN_vkCmdCopyBufferToImage vkCmdCopyBufferToImage; PFN_vkCmdCopyImageToBuffer vkCmdCopyImageToBuffer; PFN_vkCmdCopyImage vkCmdCopyImage;
PFN_vkCmdBlitImage vkCmdBlitImage; PFN_vkCmdClearColorImage vkCmdClearColorImage; PFN_vkCmdClearDepthStencilImage vkCmdClearDepthStencilImage;

static VkCommandBuffer CB = (VkCommandBuffer)(uintptr_t)0x10;
static void *other_thread(void *unused) { (void)unused; vkCmdDraw(CB, 777, 1, 0, 0); return NULL; }

static void *concurrent_sync(void *unused) {
    (void)unused;
    for (unsigned i = 0; i < 20000; ++i) nvk_record_sync();
    return NULL;
}
static void short_wrap_regression(void) {
    /* A 24-byte packet can leave only 8 bytes at the end. The wrap marker owns
     * only its op/size, never the 16-byte Head's cb beyond the allocation. */
    ring = malloc(RING_BYTES); assert(ring);
    atomic_store(&ring_head, RING_BYTES - 8); atomic_store(&ring_tail, RING_BYTES - 8);
    uint8_t *p = push_begin(R_DRAW, NULL, sizeof(Head) + sizeof(PDraw));
    const uint32_t *marker = (const void *)(ring + RING_BYTES - 8);
    assert(marker[0] == R_N && marker[1] == 8);
    memset(p, 0, sizeof(PDraw)); push_done(p);
    assert(atomic_load(&ring_head) == RING_BYTES + ((sizeof(Head) + sizeof(PDraw) + 7) & ~7u));
    free(ring); ring = NULL;
    atomic_store(&ring_head, 0); atomic_store(&ring_tail, 0);
    packets = wraps = 0;
    puts("recording 8-byte wrap marker: pass");
}

int main(void) {
    short_wrap_regression();
    nvk_record_install();
    int on = active;
    nvk_record_front();
    struct { int id; uint64_t arg; } want[64]; unsigned nw = 0;
#define WANT(i, a) (want[nw].id = (i), want[nw++].arg = (a))
    vkBeginCommandBuffer(CB, NULL); WANT(13, 0);
    VkRenderingAttachmentInfo color[2] = {{.imageView = (VkImageView)(uintptr_t)0x501}, {.imageView = (VkImageView)(uintptr_t)0x502}}, depth = {0};
    VkRenderingInfo ri = {.renderArea = {{0, 0}, {1280, 720}}, .colorAttachmentCount = 2, .pColorAttachments = color, .pDepthAttachment = &depth};
    vkCmdBeginRendering(CB, &ri); WANT(7, 1280);
    color[0].imageView = (VkImageView)(uintptr_t)0xbad;                      /* reused at once by the caller */
    vkCmdBindPipeline(CB, VK_PIPELINE_BIND_POINT_GRAPHICS, (VkPipeline)(uintptr_t)0x77); WANT(1, 0x77);
    uint64_t blob[96] = {0x1234}; vkUpdateDescriptorSetWithTemplate(nvk_dev, (VkDescriptorSet)(uintptr_t)0x900, (VkDescriptorUpdateTemplate)(uintptr_t)1, blob); WANT(15, 0x900);
    blob[0] = 0xdead;
    VkDescriptorSet sets[2] = {(VkDescriptorSet)(uintptr_t)0x900, (VkDescriptorSet)(uintptr_t)0x901}; uint32_t dyn[1] = {256};
    vkCmdBindDescriptorSets(CB, VK_PIPELINE_BIND_POINT_GRAPHICS, VK_NULL_HANDLE, 0, 2, sets, 1, dyn); WANT(2, 2);
    sets[0] = VK_NULL_HANDLE; dyn[0] = 0;
    VkViewport vp = {0, 0, 640, 360, 0, 1}; vkCmdSetViewport(CB, 0, 1, &vp); WANT(5, 1); vp.width = 1;
    VkRect2D rc = {{0, 0}, {320, 180}}; vkCmdSetScissor(CB, 0, 1, &rc); WANT(6, 1); rc.extent.width = 1;
    vkCmdSetDepthTestEnable(CB, 1); WANT(14, 1);
    VkVertexInputBindingDescription2EXT vb = {.stride = 24}; PFN_vkCmdSetVertexInputEXT vi = nvk_record_vertex_input(d_SetVertexInput);
    vi(CB, 1, &vb, 0, NULL); WANT(18, 24); vb.stride = 0;
    vkCmdBeginQuery(CB, VK_NULL_HANDLE, 5, 0); WANT(16, 5);
    vkCmdDrawIndexed(CB, 36, 1, 0, 0, 0); WANT(4, 36);
    vkCmdEndQuery(CB, VK_NULL_HANDLE, 5); WANT(17, 5);
    VkMemoryBarrier mb = {.srcAccessMask = 0x80}; vkCmdPipelineBarrier(CB, 1, 1, VK_DEPENDENCY_BY_REGION_BIT, 1, &mb, 0, NULL, 0, NULL); WANT(9, 0x80);
    vkCmdEndRendering(CB); WANT(8, 0);
    VkImageMemoryBarrier ib = {0}; vkCmdPipelineBarrier(CB, 1, 1, 0, 0, NULL, 0, NULL, 1, &ib); WANT(10, 0);   /* image barrier: direct, after the queue */
    VkBufferCopy r = {0, 0, 64}; vkCmdCopyBuffer(CB, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &r); WANT(11, 64);  /* copy: direct, after the queue */
    vkCmdDraw(CB, 3, 1, 0, 0); WANT(3, 3);
    pthread_t t; pthread_create(&t, NULL, other_thread, NULL); pthread_join(t, NULL);                      /* another thread: straight through */
    vkEndCommandBuffer(CB); WANT(12, 0);
    /* The other thread's draw may come before queued calls; everything else is in order. */
    unsigned k = 0, other = 0;
    for (unsigned i = 0; i < ncalls; ++i) {
        if (calls[i].id == 3 && calls[i].arg == 777) { other++; continue; }
        assert(k < nw && calls[i].id == want[k].id && calls[i].arg == want[k].arg);
        ++k;
    }
    assert(k == nw && other == 1);
    assert(last_color0_view == 0x501 && last_sets[0] == 0x900 && last_sets[1] == 0x901 && last_dyn == 256 && last_vp_w == 640 && last_rect_w == 320
           && last_template_word == 0x1234);                                                             /* copies, not the caller's arrays */
    /* Many draws: the ring wraps around (a template update is ~800 bytes) and stays in order. */
    unsigned before = ncalls;
    for (unsigned i = 0; i < 20000; ++i) {
        blob[0] = i; vkUpdateDescriptorSetWithTemplate(nvk_dev, (VkDescriptorSet)(uintptr_t)(0x10000 + i), (VkDescriptorUpdateTemplate)(uintptr_t)1, blob);
        vkCmdDraw(CB, i, 1, 0, 0);
    }
    vkEndCommandBuffer(CB);
    for (unsigned i = 0; i < 20000; ++i) {
        assert(calls[before + 2 * i].id == 15 && calls[before + 2 * i].arg == 0x10000 + i);
        assert(calls[before + 2 * i + 1].id == 3 && calls[before + 2 * i + 1].arg == i);
    }
    if (on) assert(wraps >= 1);
    if (on) {
        pthread_t waiter; assert(!pthread_create(&waiter, NULL, concurrent_sync, NULL));
        for (unsigned i = 0; i < 30000; ++i) vkCmdDraw(CB, i, 1, 0, 0);
        nvk_record_sync(); assert(!pthread_join(waiter, NULL));
        puts("concurrent recorder sync: pass (producer keeps enqueueing, consumer may pass the waiter's snapshot)");
    }
    if (on) {
        /* Identical neighbouring global barriers are redundant. A changed mask,
         * another command buffer, any packet, and a direct-operation drain are boundaries. */
        VkMemoryBarrier same = {.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT};
        unsigned start=ncalls;
        vkCmdPipelineBarrier(CB,1,1,0,1,&same,0,NULL,0,NULL);
        vkCmdPipelineBarrier(CB,1,1,0,1,&same,0,NULL,0,NULL);
        nvk_record_sync(); assert(ncalls==start+(barrier_dedup_on()?1:2));
        start=ncalls;
        vkCmdPipelineBarrier(CB,1,1,0,1,&same,0,NULL,0,NULL);
        vkCmdDraw(CB,12,1,0,0);
        vkCmdPipelineBarrier(CB,1,1,0,1,&same,0,NULL,0,NULL);
        nvk_record_sync(); assert(ncalls==start+3);
        start=ncalls;
        vkCmdPipelineBarrier(CB,1,1,0,1,&same,0,NULL,0,NULL);
        vkCmdPipelineBarrier((VkCommandBuffer)(uintptr_t)0x88,1,1,0,1,&same,0,NULL,0,NULL);
        same.dstAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier((VkCommandBuffer)(uintptr_t)0x88,1,1,0,1,&same,0,NULL,0,NULL);
        nvk_record_sync(); assert(ncalls==start+3);
        start=ncalls;
        vkCmdPipelineBarrier(CB,1,1,0,1,&same,0,NULL,0,NULL); nvk_record_sync();
        vkCmdPipelineBarrier(CB,1,1,0,1,&same,0,NULL,0,NULL); nvk_record_sync();
        assert(ncalls==start+2);
        printf("GPU duplicate barriers: pass (exact duplicate only, masks, command-buffer identity, intervening work and direct-call boundaries; %llu eliminated)\n",
            (unsigned long long)gpu_barriers_elided);
    }
    if (nvk_record_segments_on()) {
        unsigned start = ncalls;
        VkCommandBuffer next = (VkCommandBuffer)(uintptr_t)0x20;
        pthread_mutex_lock(&submit_gate); hold_submit = 1; submit_entered = 0; pthread_mutex_unlock(&submit_gate);
        uint64_t waits_before = atomic_load(&sync_waits);
        assert(nvk_record_segment(VK_NULL_HANDLE, CB, next));
        pthread_mutex_lock(&submit_gate);
        struct timespec deadline; clock_gettime(CLOCK_REALTIME, &deadline); deadline.tv_sec += 5;
        while (!submit_entered) assert(!pthread_cond_timedwait(&submit_cv, &submit_gate, &deadline));
        /* The driver's submit is now blocked; the producer can still enqueue commands for the next buffer. */
        vkCmdDraw(next, 12345, 1, 0, 0);
        assert(atomic_load(&sync_waits) == waits_before);
        pthread_mutex_lock(&log_lock); assert(ncalls == start + 1 && calls[start].id == 12); pthread_mutex_unlock(&log_lock);
        hold_submit = 0; pthread_cond_broadcast(&submit_cv); pthread_mutex_unlock(&submit_gate);
        nvk_record_sync();
        assert(ncalls == start + 4 && calls[start].id == 12 && calls[start + 1].id == 19
            && calls[start + 1].arg == (uint64_t)(uintptr_t)CB && calls[start + 2].id == 13
            && calls[start + 3].id == 3 && calls[start + 3].arg == 12345);
        /* A busy presentation queue cannot block enqueueing. Drain only after releasing that queue lock. */
        nvk_queue_lock(); assert(nvk_record_segment(VK_NULL_HANDLE, next, CB)); nvk_queue_unlock(); nvk_record_sync();
        /* Exercise wakeup from a fully idle recorder; repeated publication cannot strand a final packet asleep. */
        for (unsigned i = 0; i < 32; ++i) {
            uint64_t begin = nvk_now_ns();
            while (!atomic_load(&worker_sleeping)) { assert(nvk_now_ns() - begin < 5000000000ull); sched_yield(); }
            assert(nvk_record_segment(VK_NULL_HANDLE, CB, next)); nvk_record_sync();
        }
        uint64_t stats[3]; nvk_record_overlap_stats(stats);
        assert(stats[0] == 34 && stats[2] > 0);
        /* The last end still drains the recorder before the ONLY timeline signal is submitted by the front. */
        vkCmdDraw(next, 4242, 1, 0, 0);
        vkEndCommandBuffer(next);
        unsigned final_start = ncalls;
        VkSemaphore semaphore = (VkSemaphore)(uintptr_t)0x70; uint64_t value = 42;
        VkTimelineSemaphoreSubmitInfo timeline = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
            .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &value};
        VkSubmitInfo final = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &timeline,
            .commandBufferCount = 1, .pCommandBuffers = &next, .signalSemaphoreCount = 1, .pSignalSemaphores = &semaphore};
        nvk_queue_lock(); assert(vkQueueSubmit(VK_NULL_HANDLE, 1, &final, VK_NULL_HANDLE) == VK_SUCCESS); nvk_queue_unlock();
        assert(ncalls == final_start + 1 && calls[final_start].id == 20 && calls[final_start].arg == 42);
        /* Pool host operations must run after queued recording, even when a new buffer is needed mid-submission. */
        VkCommandBuffer allocated; VkCommandBufferAllocateInfo ci = {.commandBufferCount = 1};
        vkCmdDraw(next, 555, 1, 0, 0);
        assert(vkAllocateCommandBuffers(nvk_dev, &ci, &allocated) == VK_SUCCESS && allocated);
        vkCmdDraw(allocated, 666, 1, 0, 0);
        assert(vkResetCommandPool(nvk_dev, VK_NULL_HANDLE, 0) == VK_SUCCESS);
        puts("segment finalization: pass (last end drains, only final submit signals timeline, pool allocation/reset drain recording)");
        puts("asynchronous segments: pass (producer advances during blocked submit, end/submit/begin order, no fence signals, queue exclusion and idle wakeups)");
    } else {
        assert(!nvk_record_segment(VK_NULL_HANDLE, CB, (VkCommandBuffer)(uintptr_t)0x20));
        uint64_t stats[3]; nvk_record_overlap_stats(stats); assert(!stats[0]);
    }
    printf("recording thread: %s\n", on ? "pass (queued and direct calls in order, arguments copied, other threads direct, ring wraps)" : "off (TOTK_VK_RECORD_THREAD unset): calls go straight to the driver, in order");
    return 0;
}
