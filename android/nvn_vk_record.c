/* Vulkan command recording on a second thread (TOTK_VK_RECORD_THREAD=1, off by default).
 *
 * The replay thread translates NVN commands and used to make every vkCmd* call itself: ~4 ms of driver work per frame
 * in the open world (the "recording" phase of TOTK_VK_CPU_PHASES), on the thread the frame waits for. With the switch
 * on, the frequent calls of draws and passes (binds, dynamic state, draws, render passes, memory barriers, queries,
 * timestamps, attachment clears, descriptor template updates) are copied into a ring and made by a recording thread in
 * the same order; the replay thread goes on translating. Every other call that touches a command buffer, pool or the
 * queue (begin/end, submit, pool allocation/reset) first waits until the recording thread has emptied
 * the ring and is then made directly, so the order of all commands stays exactly as before.
 *
 * This works on the function pointers of the generated dispatch (nvk_vk_dispatch.h): nvk_record_install() keeps the
 * driver's pointers and puts wrappers in their place. Calls from threads other than the replay thread (presentation,
 * which has its own command buffer) go straight to the driver. */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "nvn_vk_internal.h"
/* Live switches live in nvn_vk_draw.c; tests that link this file without it read the environment. */
__attribute__((weak)) unsigned nvk_live_gen;
__attribute__((weak)) const char *nvk_live_getenv(const char *name) { return getenv(name); }

enum {
    R_BIND_PIPELINE, R_BIND_SETS, R_BIND_INDEX, R_BIND_VBUF, R_VERTEX_INPUT, R_DRAW, R_DRAW_INDEXED, R_DISPATCH,
    R_VIEWPORT, R_SCISSOR, R_TOPOLOGY, R_CULL, R_FRONT, R_BIAS_ENABLE, R_BIAS, R_BLEND_CONSTANTS, R_DEPTH_TEST,
    R_DEPTH_WRITE, R_DEPTH_OP, R_DEPTH_BOUNDS, R_RASTER_DISCARD, R_STENCIL_TEST, R_STENCIL_OP, R_STENCIL_COMPARE,
    R_STENCIL_WRITE, R_STENCIL_REF, R_RESTART, R_BEGIN_RENDERING, R_END_RENDERING, R_BARRIER, R_BEGIN_QUERY,
    R_END_QUERY, R_TIMESTAMP, R_CLEAR_ATTACHMENTS, R_SET_TEMPLATE, R_COPY_BUFFER, R_UPDATE_BUFFER, R_BUFFER_TO_IMAGE,
    R_IMAGE_TO_BUFFER, R_COPY_IMAGE, R_BLIT_IMAGE, R_CLEAR_COLOR_IMAGE, R_CLEAR_DS_IMAGE, R_SEGMENT, R_N
};

/* The driver's entry points (taken from the dispatch at install). */
static struct {
    PFN_vkCmdBindPipeline BindPipeline; PFN_vkCmdBindDescriptorSets BindDescriptorSets; PFN_vkCmdBindIndexBuffer BindIndexBuffer;
    PFN_vkCmdBindVertexBuffers BindVertexBuffers; PFN_vkCmdSetVertexInputEXT SetVertexInput; PFN_vkCmdDraw Draw;
    PFN_vkCmdDrawIndexed DrawIndexed; PFN_vkCmdDispatch Dispatch; PFN_vkCmdSetViewport SetViewport; PFN_vkCmdSetScissor SetScissor;
    PFN_vkCmdSetPrimitiveTopology SetPrimitiveTopology; PFN_vkCmdSetCullMode SetCullMode; PFN_vkCmdSetFrontFace SetFrontFace;
    PFN_vkCmdSetDepthBiasEnable SetDepthBiasEnable; PFN_vkCmdSetDepthBias SetDepthBias; PFN_vkCmdSetBlendConstants SetBlendConstants;
    PFN_vkCmdSetDepthTestEnable SetDepthTestEnable; PFN_vkCmdSetDepthWriteEnable SetDepthWriteEnable; PFN_vkCmdSetDepthCompareOp SetDepthCompareOp;
    PFN_vkCmdSetDepthBoundsTestEnable SetDepthBoundsTestEnable; PFN_vkCmdSetRasterizerDiscardEnable SetRasterizerDiscardEnable;
    PFN_vkCmdSetStencilTestEnable SetStencilTestEnable; PFN_vkCmdSetStencilOp SetStencilOp; PFN_vkCmdSetStencilCompareMask SetStencilCompareMask;
    PFN_vkCmdSetStencilWriteMask SetStencilWriteMask; PFN_vkCmdSetStencilReference SetStencilReference;
    PFN_vkCmdSetPrimitiveRestartEnable SetPrimitiveRestartEnable; PFN_vkCmdBeginRendering BeginRendering; PFN_vkCmdEndRendering EndRendering;
    PFN_vkCmdPipelineBarrier PipelineBarrier; PFN_vkCmdBeginQuery BeginQuery; PFN_vkCmdEndQuery EndQuery; PFN_vkCmdWriteTimestamp WriteTimestamp;
    PFN_vkCmdClearAttachments ClearAttachments; PFN_vkUpdateDescriptorSetWithTemplate UpdateDescriptorSetWithTemplate;
    /* made directly after the ring is empty */
    PFN_vkBeginCommandBuffer BeginCommandBuffer; PFN_vkEndCommandBuffer EndCommandBuffer; PFN_vkResetCommandPool ResetCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers; PFN_vkQueueSubmit QueueSubmit; PFN_vkQueueWaitIdle QueueWaitIdle;
    PFN_vkCmdCopyBuffer CopyBuffer; PFN_vkCmdUpdateBuffer UpdateBuffer; PFN_vkCmdCopyBufferToImage CopyBufferToImage;
    PFN_vkCmdCopyImageToBuffer CopyImageToBuffer; PFN_vkCmdCopyImage CopyImage; PFN_vkCmdBlitImage BlitImage;
    PFN_vkCmdClearColorImage ClearColorImage; PFN_vkCmdClearDepthStencilImage ClearDepthStencilImage;
} real;

/* ---------- Ring (one producer: the replay thread; one consumer: the recording thread) ---------- */
#define RING_BYTES (8u << 20)
typedef struct { uint32_t op, size; VkCommandBuffer cb; } Head;   /* size: whole packet, 8-byte aligned */
static uint8_t *ring;
/* Keep the independently written producer/consumer positions on separate cache lines. */
_Alignas(128) static _Atomic uint64_t ring_head;
_Alignas(128) static _Atomic uint64_t ring_tail;   /* bytes written / consumed, ever */
_Alignas(128) static _Atomic int worker_sleeping;
static pthread_mutex_t wake_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake_cv = PTHREAD_COND_INITIALIZER;
static _Atomic uintptr_t front;   /* the replay thread */
static int active;
static uint64_t packets, wraps, queued_segments;
static _Atomic uint64_t syncs, sync_waits, sync_wait_ns;
static _Atomic uint64_t segment_submit_ns;

static int on_front(void) { return active && atomic_load_explicit(&front, memory_order_relaxed) == (uintptr_t)pthread_self(); }

/* Replay-thread-only shadow: eliminate only immediately consecutive identical
 * global memory barriers on the same command buffer. Any other packet or a
 * direct-operation drain invalidates it, including timestamps and queries.
 * No access/stage mask is weakened; resource-specific barriers always pass. */
static struct {
    int valid;
    VkCommandBuffer cb;
    VkPipelineStageFlags src, dst;
    VkDependencyFlags flags;
    uint32_t n;
    VkMemoryBarrier mb[4];
} last_gpu_barrier;
static uint64_t gpu_barriers_elided;
static int barrier_dedup_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_VK_BARRIER_DEDUP"); on = !(e && !strcmp(e, "0")); }
    return on;
}
/* Space for a packet of n bytes (header included) at the head; the caller fills it and calls push_done. */
static uint8_t *push_begin(uint32_t op, VkCommandBuffer cb, size_t n) {
    if (op != R_BARRIER) last_gpu_barrier.valid = 0;
    n = (n + 7) & ~(size_t)7;
    uint64_t head = atomic_load_explicit(&ring_head, memory_order_relaxed);
    size_t at = (size_t)(head % RING_BYTES);
    if (at + n > RING_BYTES) {   /* no room before the end: a filler to the end, then start over */
        size_t rest = RING_BYTES - at;
        while (head + rest - atomic_load_explicit(&ring_tail, memory_order_acquire) > RING_BYTES) sched_yield();
        Head *f = (Head *)(ring + at); f->op = R_N; f->size = (uint32_t)rest;   /* the filler may be only 8 bytes; its cb is never read */
        head += rest; at = 0; wraps++;
        atomic_store_explicit(&ring_head, head, memory_order_release);
    }
    while (head + n - atomic_load_explicit(&ring_tail, memory_order_acquire) > RING_BYTES) sched_yield();   /* full */
    Head *h = (Head *)(ring + at); h->op = op; h->size = (uint32_t)n; h->cb = cb;
    return ring + at + sizeof(Head);
}
static void push_done(const uint8_t *payload) {
    const Head *h = (const Head *)(payload - sizeof(Head));
    /* Both publication/sleep checks use the same SC order: producer and sleeper cannot both miss the other's
       store, which would leave the final packet asleep while its caller waits for completion. */
    atomic_store_explicit(&ring_head, atomic_load_explicit(&ring_head, memory_order_relaxed) + h->size, memory_order_seq_cst);
    packets++;
    if (atomic_load_explicit(&worker_sleeping, memory_order_seq_cst)) {
        pthread_mutex_lock(&wake_lock); pthread_cond_signal(&wake_cv); pthread_mutex_unlock(&wake_lock);
    }
}
/* The replay thread waits until everything queued has been recorded. */
void nvk_record_sync(void) {
    if (on_front()) last_gpu_barrier.valid = 0;   /* caller may issue a direct GPU command after the drain */
    if (!active) return;
    syncs++;
    /* Non-producer callers (drain/resize) take a snapshot while the producer can continue enqueueing.
       The consumer may already have passed that snapshot: equality would then wait forever. */
    uint64_t head = atomic_load_explicit(&ring_head, memory_order_acquire);
    if (atomic_load_explicit(&ring_tail, memory_order_acquire) >= head) return;
    sync_waits++;
    uint64_t begin = nvk_now_ns();
    for (unsigned spin = 0; atomic_load_explicit(&ring_tail, memory_order_acquire) < head; ++spin) if (spin > 64) sched_yield();
    sync_wait_ns += nvk_now_ns() - begin;
}

/* android_compat.c pins the thread when TOTK_AFFINITY_RECORD is set; the host tests link without it. */
__attribute__((weak)) void android_thread_affinity(const char *env_name) { (void)env_name; }
static void execute(const Head *h);
static void *worker(void *unused) {
    (void)unused;
    pthread_setname_np(pthread_self(), "totk-vk-record");
    android_thread_affinity("TOTK_AFFINITY_RECORD");
    const char *batch_env = getenv("TOTK_VK_RECORD_BATCH");
    unsigned batch_limit = batch_env && !strcmp(batch_env, "0") ? 1u : 32u;
    for (;;) {
        uint64_t tail = atomic_load_explicit(&ring_tail, memory_order_relaxed);
        unsigned spin = 0;
        /* A short busy wait (packets of a frame come microseconds apart), a few yields, then sleep. Spinning with up to
           3800 sched_yield calls kept a big core busy between frames: 23 Oct, Thor profile, more than half of this
           thread's time was sched_yield and the kernel. TOTK_VK_RECORD_SPIN=old: as before. */
        static int old_spin = -1; static unsigned g;
        if (old_spin < 0 || g != nvk_live_gen) { g = nvk_live_gen; const char *e = nvk_live_getenv("TOTK_VK_RECORD_SPIN"); old_spin = e && !strcmp(e, "old"); }
        while (atomic_load_explicit(&ring_head, memory_order_acquire) == tail) {
            if (old_spin) { if (++spin < 4000) { if (spin > 200) sched_yield(); continue; } }
            else if (++spin < 2048 + 32) { if (spin > 2048) sched_yield(); else __asm__ volatile("yield"); continue; }
            pthread_mutex_lock(&wake_lock);
            atomic_store_explicit(&worker_sleeping, 1, memory_order_seq_cst);
            while (atomic_load_explicit(&ring_head, memory_order_seq_cst) == tail) pthread_cond_wait(&wake_cv, &wake_lock);
            atomic_store_explicit(&worker_sleeping, 0, memory_order_relaxed);
            pthread_mutex_unlock(&wake_lock);
            spin = 0;
        }
        /* Consume only published packets, in the original order. Retire up to 32 at
         * once to reduce cache-line handovers. Publish immediately when the snapshot
         * is drained: a lone final packet never waits for a batch to fill. Keeping
         * tail conservative prevents the producer from overwriting any live payload. */
        uint64_t ready = atomic_load_explicit(&ring_head, memory_order_acquire);
        unsigned consumed = 0;
        do {
            const Head *h = (const Head *)(ring + tail % RING_BYTES);
            uint32_t size = h->size;
            if (h->op != R_N) execute(h);
            tail += size;
        } while (tail < ready && ++consumed < batch_limit);
        atomic_store_explicit(&ring_tail, tail, memory_order_release);
    }
    return NULL;
}

/* ---------- Packets ---------- */
typedef struct { VkPipelineBindPoint point; VkPipeline pipe; } PBindPipeline;
typedef struct { VkPipelineBindPoint point; VkPipelineLayout layout; uint32_t first, nsets, ndyn; } PBindSets;   /* + sets, dyn */
typedef struct { VkBuffer buf; VkDeviceSize off; VkIndexType type; } PBindIndex;
typedef struct { uint32_t first, n; } PBindVbuf;   /* + buffers, offsets */
typedef struct { uint32_t nb, na; } PVertexInput;  /* + bindings, attributes */
typedef struct { uint32_t a, b, c, d; int32_t e; uint32_t f; } PDraw;   /* draw: a b c d; indexed: a b c e f */
typedef struct { uint32_t first, n; } PRects;      /* + viewports or scissors */
typedef struct { uint32_t v; } PU32;
typedef struct { float a, b, c; } PBias;
typedef struct { float c[4]; } PBlend;
typedef struct { VkStencilFaceFlags face; VkStencilOp fail, pass, zfail; VkCompareOp cmp; } PStencilOp;
typedef struct { VkStencilFaceFlags face; uint32_t v; } PStencilVal;
typedef struct { VkRenderingInfo ri; VkRenderingAttachmentInfo color[8], depth, stencil; int has_depth, has_stencil; } PRendering;
typedef struct { VkPipelineStageFlags src, dst; VkDependencyFlags flags; uint32_t n; VkMemoryBarrier mb[4]; } PBarrier;
typedef struct { VkQueryPool pool; uint32_t q; VkQueryControlFlags flags; } PQuery;
typedef struct { VkPipelineStageFlagBits stage; VkQueryPool pool; uint32_t q; } PTimestamp;
typedef struct { uint32_t na, nr; } PClear;        /* + attachments, rects */
typedef struct { VkDescriptorSet set; VkDescriptorUpdateTemplate t; uint32_t bytes; } PTemplate;   /* + data */
typedef struct { VkBuffer src, dst; uint32_t n; } PCopyBuffer;                                    /* + regions */
typedef struct { VkBuffer dst; VkDeviceSize off, n; } PUpdateBuffer;                              /* + data */
typedef struct { VkBuffer buf; VkImage img; VkImageLayout layout; uint32_t n; } PBufImage;         /* + regions */
typedef struct { VkImage src, dst; VkImageLayout sl, dl; uint32_t n; VkFilter filter; } PImageImage;   /* + regions */
typedef struct { VkImage img; VkImageLayout layout; VkClearColorValue c; VkClearDepthStencilValue ds; uint32_t n; } PClearImage;   /* + ranges */

/* A segment handover is ordered in the same ring as its draws. The replay can prepare the next segment while
   this thread ends/submits the previous one and begins the next. Callers allocate next's command buffer before
   enqueuing this packet; allocation/reset wrappers drain the ring before touching a command pool.
   The final submission remains synchronous and owns all timeline signals and completion bookkeeping.
   Opt-in: TOTK_VK_RECORD_THREAD=1 + TOTK_VK_RECORD_SEGMENTS=1. */
typedef struct { VkQueue queue; VkCommandBuffer next; } PSegment;
int nvk_record_segments_on(void) {
    if (!on_front()) return 0;
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_VK_RECORD_SEGMENTS"); on = e && !strcmp(e, "1"); }
    return on;
}
int nvk_record_segment(VkQueue queue, VkCommandBuffer old, VkCommandBuffer next) {
    if (!nvk_record_segments_on()) return 0;
    if (!old || !next || old == next) nvk_die("invalid asynchronous segment handover");
    PSegment *a = (void *)push_begin(R_SEGMENT, old, sizeof(Head) + sizeof *a);
    a->queue = queue; a->next = next; push_done((const uint8_t *)a); queued_segments++;
    return 1;
}
void nvk_record_overlap_stats(uint64_t v[3]) {
    v[0] = queued_segments; v[1] = sync_wait_ns; v[2] = atomic_load_explicit(&segment_submit_ns, memory_order_relaxed);
}

static void execute(const Head *h) {
    const uint8_t *p = (const uint8_t *)(h + 1);
    VkCommandBuffer cb = h->cb;
    switch (h->op) {
    case R_SEGMENT: {
        const PSegment *a = (const void *)p;
        VKCHECK(real.EndCommandBuffer(cb), "end queued NVN segment");
        VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
        uint64_t begin = nvk_now_ns();
        nvk_queue_lock(); VkResult result = real.QueueSubmit(a->queue, 1, &si, VK_NULL_HANDLE); nvk_queue_unlock();
        atomic_fetch_add_explicit(&segment_submit_ns, nvk_now_ns() - begin, memory_order_relaxed);
        VKCHECK(result, "submit queued NVN segment");
        VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        VKCHECK(real.BeginCommandBuffer(a->next, &bi), "begin queued NVN segment");
        break;
    }
    case R_BIND_PIPELINE: { const PBindPipeline *a = (const void *)p; real.BindPipeline(cb, a->point, a->pipe); break; }
    case R_BIND_SETS: { const PBindSets *a = (const void *)p; const VkDescriptorSet *s = (const void *)(a + 1);
        real.BindDescriptorSets(cb, a->point, a->layout, a->first, a->nsets, s, a->ndyn, (const uint32_t *)(s + a->nsets)); break; }
    case R_BIND_INDEX: { const PBindIndex *a = (const void *)p; real.BindIndexBuffer(cb, a->buf, a->off, a->type); break; }
    case R_BIND_VBUF: { const PBindVbuf *a = (const void *)p; const VkBuffer *b = (const void *)(a + 1);
        real.BindVertexBuffers(cb, a->first, a->n, b, (const VkDeviceSize *)(b + a->n)); break; }
    case R_VERTEX_INPUT: { const PVertexInput *a = (const void *)p; const VkVertexInputBindingDescription2EXT *b = (const void *)(a + 1);
        real.SetVertexInput(cb, a->nb, b, a->na, (const VkVertexInputAttributeDescription2EXT *)(b + a->nb)); break; }
    case R_DRAW: { const PDraw *a = (const void *)p; real.Draw(cb, a->a, a->b, a->c, a->d); break; }
    case R_DRAW_INDEXED: { const PDraw *a = (const void *)p; real.DrawIndexed(cb, a->a, a->b, a->c, a->e, a->f); break; }
    case R_DISPATCH: { const PDraw *a = (const void *)p; real.Dispatch(cb, a->a, a->b, a->c); break; }
    case R_VIEWPORT: { const PRects *a = (const void *)p; real.SetViewport(cb, a->first, a->n, (const VkViewport *)(a + 1)); break; }
    case R_SCISSOR: { const PRects *a = (const void *)p; real.SetScissor(cb, a->first, a->n, (const VkRect2D *)(a + 1)); break; }
    case R_TOPOLOGY: real.SetPrimitiveTopology(cb, (VkPrimitiveTopology)((const PU32 *)p)->v); break;
    case R_CULL: real.SetCullMode(cb, ((const PU32 *)p)->v); break;
    case R_FRONT: real.SetFrontFace(cb, (VkFrontFace)((const PU32 *)p)->v); break;
    case R_BIAS_ENABLE: real.SetDepthBiasEnable(cb, ((const PU32 *)p)->v); break;
    case R_BIAS: { const PBias *a = (const void *)p; real.SetDepthBias(cb, a->a, a->b, a->c); break; }
    case R_BLEND_CONSTANTS: real.SetBlendConstants(cb, ((const PBlend *)p)->c); break;
    case R_DEPTH_TEST: real.SetDepthTestEnable(cb, ((const PU32 *)p)->v); break;
    case R_DEPTH_WRITE: real.SetDepthWriteEnable(cb, ((const PU32 *)p)->v); break;
    case R_DEPTH_OP: real.SetDepthCompareOp(cb, (VkCompareOp)((const PU32 *)p)->v); break;
    case R_DEPTH_BOUNDS: real.SetDepthBoundsTestEnable(cb, ((const PU32 *)p)->v); break;
    case R_RASTER_DISCARD: real.SetRasterizerDiscardEnable(cb, ((const PU32 *)p)->v); break;
    case R_STENCIL_TEST: real.SetStencilTestEnable(cb, ((const PU32 *)p)->v); break;
    case R_STENCIL_OP: { const PStencilOp *a = (const void *)p; real.SetStencilOp(cb, a->face, a->fail, a->pass, a->zfail, a->cmp); break; }
    case R_STENCIL_COMPARE: { const PStencilVal *a = (const void *)p; real.SetStencilCompareMask(cb, a->face, a->v); break; }
    case R_STENCIL_WRITE: { const PStencilVal *a = (const void *)p; real.SetStencilWriteMask(cb, a->face, a->v); break; }
    case R_STENCIL_REF: { const PStencilVal *a = (const void *)p; real.SetStencilReference(cb, a->face, a->v); break; }
    case R_RESTART: real.SetPrimitiveRestartEnable(cb, ((const PU32 *)p)->v); break;
    case R_BEGIN_RENDERING: { PRendering r = *(const PRendering *)p;   /* pointers rebuilt to this copy */
        r.ri.pColorAttachments = r.ri.colorAttachmentCount ? r.color : NULL;
        r.ri.pDepthAttachment = r.has_depth ? &r.depth : NULL; r.ri.pStencilAttachment = r.has_stencil ? &r.stencil : NULL;
        real.BeginRendering(cb, &r.ri); break; }
    case R_END_RENDERING: real.EndRendering(cb); break;
    case R_BARRIER: { const PBarrier *a = (const void *)p; real.PipelineBarrier(cb, a->src, a->dst, a->flags, a->n, a->n ? a->mb : NULL, 0, NULL, 0, NULL); break; }
    case R_BEGIN_QUERY: { const PQuery *a = (const void *)p; real.BeginQuery(cb, a->pool, a->q, a->flags); break; }
    case R_END_QUERY: { const PQuery *a = (const void *)p; real.EndQuery(cb, a->pool, a->q); break; }
    case R_TIMESTAMP: { const PTimestamp *a = (const void *)p; real.WriteTimestamp(cb, a->stage, a->pool, a->q); break; }
    case R_CLEAR_ATTACHMENTS: { const PClear *a = (const void *)p; const VkClearAttachment *c = (const void *)(a + 1);
        real.ClearAttachments(cb, a->na, c, a->nr, (const VkClearRect *)(c + a->na)); break; }
    case R_SET_TEMPLATE: { const PTemplate *a = (const void *)p; real.UpdateDescriptorSetWithTemplate(nvk_dev, a->set, a->t, a + 1); break; }
    case R_COPY_BUFFER: { const PCopyBuffer *a = (const void *)p; real.CopyBuffer(cb, a->src, a->dst, a->n, (const VkBufferCopy *)(a + 1)); break; }
    case R_UPDATE_BUFFER: { const PUpdateBuffer *a = (const void *)p; real.UpdateBuffer(cb, a->dst, a->off, a->n, a + 1); break; }
    case R_BUFFER_TO_IMAGE: { const PBufImage *a = (const void *)p; real.CopyBufferToImage(cb, a->buf, a->img, a->layout, a->n, (const VkBufferImageCopy *)(a + 1)); break; }
    case R_IMAGE_TO_BUFFER: { const PBufImage *a = (const void *)p; real.CopyImageToBuffer(cb, a->img, a->layout, a->buf, a->n, (const VkBufferImageCopy *)(a + 1)); break; }
    case R_COPY_IMAGE: { const PImageImage *a = (const void *)p; real.CopyImage(cb, a->src, a->sl, a->dst, a->dl, a->n, (const VkImageCopy *)(a + 1)); break; }
    case R_BLIT_IMAGE: { const PImageImage *a = (const void *)p; real.BlitImage(cb, a->src, a->sl, a->dst, a->dl, a->n, (const VkImageBlit *)(a + 1), a->filter); break; }
    case R_CLEAR_COLOR_IMAGE: { const PClearImage *a = (const void *)p; real.ClearColorImage(cb, a->img, a->layout, &a->c, a->n, (const VkImageSubresourceRange *)(a + 1)); break; }
    case R_CLEAR_DS_IMAGE: { const PClearImage *a = (const void *)p; real.ClearDepthStencilImage(cb, a->img, a->layout, &a->ds, a->n, (const VkImageSubresourceRange *)(a + 1)); break; }
    }
}

/* ---------- Wrappers ---------- */
#define DIRECT_IF_NOT_FRONT(call) do { if (!on_front()) { call; return; } } while (0)
static void w_BindPipeline(VkCommandBuffer cb, VkPipelineBindPoint point, VkPipeline pipe) {
    DIRECT_IF_NOT_FRONT(real.BindPipeline(cb, point, pipe));
    PBindPipeline *a = (void *)push_begin(R_BIND_PIPELINE, cb, sizeof(Head) + sizeof *a); a->point = point; a->pipe = pipe; push_done((void *)a);
}
static void w_BindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint point, VkPipelineLayout layout, uint32_t first, uint32_t n,
                                 const VkDescriptorSet *sets, uint32_t ndyn, const uint32_t *dyn) {
    DIRECT_IF_NOT_FRONT(real.BindDescriptorSets(cb, point, layout, first, n, sets, ndyn, dyn));
    PBindSets *a = (void *)push_begin(R_BIND_SETS, cb, sizeof(Head) + sizeof *a + n * sizeof *sets + ndyn * sizeof *dyn);
    a->point = point; a->layout = layout; a->first = first; a->nsets = n; a->ndyn = ndyn;
    VkDescriptorSet *s = (void *)(a + 1); memcpy(s, sets, n * sizeof *sets); if (ndyn) memcpy(s + n, dyn, ndyn * sizeof *dyn);
    push_done((void *)a);
}
static void w_BindIndexBuffer(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, VkIndexType type) {
    DIRECT_IF_NOT_FRONT(real.BindIndexBuffer(cb, buf, off, type));
    PBindIndex *a = (void *)push_begin(R_BIND_INDEX, cb, sizeof(Head) + sizeof *a); a->buf = buf; a->off = off; a->type = type; push_done((void *)a);
}
static void w_BindVertexBuffers(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *bufs, const VkDeviceSize *offs) {
    DIRECT_IF_NOT_FRONT(real.BindVertexBuffers(cb, first, n, bufs, offs));
    PBindVbuf *a = (void *)push_begin(R_BIND_VBUF, cb, sizeof(Head) + sizeof *a + n * (sizeof *bufs + sizeof *offs));
    a->first = first; a->n = n;
    VkBuffer *b = (void *)(a + 1); memcpy(b, bufs, n * sizeof *bufs); memcpy(b + n, offs, n * sizeof *offs);
    push_done((void *)a);
}
static void w_SetVertexInput(VkCommandBuffer cb, uint32_t nb, const VkVertexInputBindingDescription2EXT *b, uint32_t na,
                             const VkVertexInputAttributeDescription2EXT *at) {
    DIRECT_IF_NOT_FRONT(real.SetVertexInput(cb, nb, b, na, at));
    PVertexInput *a = (void *)push_begin(R_VERTEX_INPUT, cb, sizeof(Head) + sizeof *a + nb * sizeof *b + na * sizeof *at);
    a->nb = nb; a->na = na;
    VkVertexInputBindingDescription2EXT *bb = (void *)(a + 1);
    for (uint32_t i = 0; i < nb; ++i) { bb[i] = b[i]; bb[i].pNext = NULL; }
    VkVertexInputAttributeDescription2EXT *aa = (void *)(bb + nb);
    for (uint32_t i = 0; i < na; ++i) { aa[i] = at[i]; aa[i].pNext = NULL; }
    push_done((void *)a);
}
static void w_Draw(VkCommandBuffer cb, uint32_t n, uint32_t inst, uint32_t first, uint32_t first_inst) {
    DIRECT_IF_NOT_FRONT(real.Draw(cb, n, inst, first, first_inst));
    PDraw *a = (void *)push_begin(R_DRAW, cb, sizeof(Head) + sizeof *a); a->a = n; a->b = inst; a->c = first; a->d = first_inst; push_done((void *)a);
}
static void w_DrawIndexed(VkCommandBuffer cb, uint32_t n, uint32_t inst, uint32_t first, int32_t vofs, uint32_t first_inst) {
    DIRECT_IF_NOT_FRONT(real.DrawIndexed(cb, n, inst, first, vofs, first_inst));
    PDraw *a = (void *)push_begin(R_DRAW_INDEXED, cb, sizeof(Head) + sizeof *a); a->a = n; a->b = inst; a->c = first; a->e = vofs; a->f = first_inst; push_done((void *)a);
}
static void w_Dispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
    DIRECT_IF_NOT_FRONT(real.Dispatch(cb, x, y, z));
    PDraw *a = (void *)push_begin(R_DISPATCH, cb, sizeof(Head) + sizeof *a); a->a = x; a->b = y; a->c = z; push_done((void *)a);
}
static void w_SetViewport(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport *v) {
    DIRECT_IF_NOT_FRONT(real.SetViewport(cb, first, n, v));
    PRects *a = (void *)push_begin(R_VIEWPORT, cb, sizeof(Head) + sizeof *a + n * sizeof *v); a->first = first; a->n = n; memcpy(a + 1, v, n * sizeof *v); push_done((void *)a);
}
static void w_SetScissor(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkRect2D *r) {
    DIRECT_IF_NOT_FRONT(real.SetScissor(cb, first, n, r));
    PRects *a = (void *)push_begin(R_SCISSOR, cb, sizeof(Head) + sizeof *a + n * sizeof *r); a->first = first; a->n = n; memcpy(a + 1, r, n * sizeof *r); push_done((void *)a);
}
#define U32_WRAPPER(name, op, type) \
    static void w_##name(VkCommandBuffer cb, type v) { DIRECT_IF_NOT_FRONT(real.name(cb, v)); \
        PU32 *a = (void *)push_begin(op, cb, sizeof(Head) + sizeof *a); a->v = (uint32_t)v; push_done((void *)a); }
U32_WRAPPER(SetPrimitiveTopology, R_TOPOLOGY, VkPrimitiveTopology)
U32_WRAPPER(SetCullMode, R_CULL, VkCullModeFlags)
U32_WRAPPER(SetFrontFace, R_FRONT, VkFrontFace)
U32_WRAPPER(SetDepthBiasEnable, R_BIAS_ENABLE, VkBool32)
U32_WRAPPER(SetDepthTestEnable, R_DEPTH_TEST, VkBool32)
U32_WRAPPER(SetDepthWriteEnable, R_DEPTH_WRITE, VkBool32)
U32_WRAPPER(SetDepthCompareOp, R_DEPTH_OP, VkCompareOp)
U32_WRAPPER(SetDepthBoundsTestEnable, R_DEPTH_BOUNDS, VkBool32)
U32_WRAPPER(SetRasterizerDiscardEnable, R_RASTER_DISCARD, VkBool32)
U32_WRAPPER(SetStencilTestEnable, R_STENCIL_TEST, VkBool32)
U32_WRAPPER(SetPrimitiveRestartEnable, R_RESTART, VkBool32)
static void w_SetDepthBias(VkCommandBuffer cb, float c, float clamp, float slope) {
    DIRECT_IF_NOT_FRONT(real.SetDepthBias(cb, c, clamp, slope));
    PBias *a = (void *)push_begin(R_BIAS, cb, sizeof(Head) + sizeof *a); a->a = c; a->b = clamp; a->c = slope; push_done((void *)a);
}
static void w_SetBlendConstants(VkCommandBuffer cb, const float c[4]) {
    DIRECT_IF_NOT_FRONT(real.SetBlendConstants(cb, c));
    PBlend *a = (void *)push_begin(R_BLEND_CONSTANTS, cb, sizeof(Head) + sizeof *a); memcpy(a->c, c, sizeof a->c); push_done((void *)a);
}
static void w_SetStencilOp(VkCommandBuffer cb, VkStencilFaceFlags face, VkStencilOp fail, VkStencilOp pass, VkStencilOp zfail, VkCompareOp cmp) {
    DIRECT_IF_NOT_FRONT(real.SetStencilOp(cb, face, fail, pass, zfail, cmp));
    PStencilOp *a = (void *)push_begin(R_STENCIL_OP, cb, sizeof(Head) + sizeof *a); a->face = face; a->fail = fail; a->pass = pass; a->zfail = zfail; a->cmp = cmp; push_done((void *)a);
}
#define STENCIL_WRAPPER(name, op) \
    static void w_##name(VkCommandBuffer cb, VkStencilFaceFlags face, uint32_t v) { DIRECT_IF_NOT_FRONT(real.name(cb, face, v)); \
        PStencilVal *a = (void *)push_begin(op, cb, sizeof(Head) + sizeof *a); a->face = face; a->v = v; push_done((void *)a); }
STENCIL_WRAPPER(SetStencilCompareMask, R_STENCIL_COMPARE)
STENCIL_WRAPPER(SetStencilWriteMask, R_STENCIL_WRITE)
STENCIL_WRAPPER(SetStencilReference, R_STENCIL_REF)
static void w_BeginRendering(VkCommandBuffer cb, const VkRenderingInfo *ri) {
    if (!on_front() || ri->pNext || ri->colorAttachmentCount > 8) { if (on_front()) nvk_record_sync(); real.BeginRendering(cb, ri); return; }
    PRendering *a = (void *)push_begin(R_BEGIN_RENDERING, cb, sizeof(Head) + sizeof *a);
    a->ri = *ri;
    for (uint32_t i = 0; i < ri->colorAttachmentCount; ++i) a->color[i] = ri->pColorAttachments[i];
    a->has_depth = ri->pDepthAttachment != NULL; if (a->has_depth) a->depth = *ri->pDepthAttachment;
    a->has_stencil = ri->pStencilAttachment != NULL; if (a->has_stencil) a->stencil = *ri->pStencilAttachment;
    push_done((void *)a);
}
static void w_EndRendering(VkCommandBuffer cb) {
    DIRECT_IF_NOT_FRONT(real.EndRendering(cb));
    void *a = push_begin(R_END_RENDERING, cb, sizeof(Head)); push_done(a);
}
static void w_PipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags flags,
                              uint32_t nm, const VkMemoryBarrier *mb, uint32_t nb, const VkBufferMemoryBarrier *bb, uint32_t ni, const VkImageMemoryBarrier *ib) {
    int simple = nb == 0 && ni == 0 && nm <= 4;
    for (uint32_t i = 0; simple && i < nm; ++i) simple = mb[i].pNext == NULL;
    if (!on_front() || !simple) { if (on_front()) nvk_record_sync(); real.PipelineBarrier(cb, src, dst, flags, nm, mb, nb, bb, ni, ib); return; }
    if (barrier_dedup_on()) {
        int same = last_gpu_barrier.valid && last_gpu_barrier.cb == cb && last_gpu_barrier.src == src
            && last_gpu_barrier.dst == dst && last_gpu_barrier.flags == flags && last_gpu_barrier.n == nm;
        for (uint32_t i = 0; same && i < nm; ++i)
            same = last_gpu_barrier.mb[i].sType == mb[i].sType && last_gpu_barrier.mb[i].srcAccessMask == mb[i].srcAccessMask
                && last_gpu_barrier.mb[i].dstAccessMask == mb[i].dstAccessMask;
        if (same) { gpu_barriers_elided++; return; }
        last_gpu_barrier.valid = 1; last_gpu_barrier.cb = cb; last_gpu_barrier.src = src;
        last_gpu_barrier.dst = dst; last_gpu_barrier.flags = flags; last_gpu_barrier.n = nm;
        if (nm) memcpy(last_gpu_barrier.mb, mb, nm * sizeof *mb);
    }
    PBarrier *a = (void *)push_begin(R_BARRIER, cb, sizeof(Head) + sizeof *a);
    a->src = src; a->dst = dst; a->flags = flags; a->n = nm; if (nm) memcpy(a->mb, mb, nm * sizeof *mb);
    push_done((void *)a);
}
static void w_BeginQuery(VkCommandBuffer cb, VkQueryPool pool, uint32_t q, VkQueryControlFlags flags) {
    DIRECT_IF_NOT_FRONT(real.BeginQuery(cb, pool, q, flags));
    PQuery *a = (void *)push_begin(R_BEGIN_QUERY, cb, sizeof(Head) + sizeof *a); a->pool = pool; a->q = q; a->flags = flags; push_done((void *)a);
}
static void w_EndQuery(VkCommandBuffer cb, VkQueryPool pool, uint32_t q) {
    DIRECT_IF_NOT_FRONT(real.EndQuery(cb, pool, q));
    PQuery *a = (void *)push_begin(R_END_QUERY, cb, sizeof(Head) + sizeof *a); a->pool = pool; a->q = q; a->flags = 0; push_done((void *)a);
}
static void w_WriteTimestamp(VkCommandBuffer cb, VkPipelineStageFlagBits stage, VkQueryPool pool, uint32_t q) {
    DIRECT_IF_NOT_FRONT(real.WriteTimestamp(cb, stage, pool, q));
    PTimestamp *a = (void *)push_begin(R_TIMESTAMP, cb, sizeof(Head) + sizeof *a); a->stage = stage; a->pool = pool; a->q = q; push_done((void *)a);
}
static void w_ClearAttachments(VkCommandBuffer cb, uint32_t na, const VkClearAttachment *at, uint32_t nr, const VkClearRect *r) {
    DIRECT_IF_NOT_FRONT(real.ClearAttachments(cb, na, at, nr, r));
    PClear *a = (void *)push_begin(R_CLEAR_ATTACHMENTS, cb, sizeof(Head) + sizeof *a + na * sizeof *at + nr * sizeof *r);
    a->na = na; a->nr = nr;
    VkClearAttachment *c = (void *)(a + 1); memcpy(c, at, na * sizeof *at); memcpy(c + na, r, nr * sizeof *r);
    push_done((void *)a);
}
/* Descriptor writes through a template (nvn_vk_draw.c write_descriptors). The caller says how many bytes its block
   has (nvk_record_template_bytes, set just before the call); without that, the largest block (DC_WRITES slots). */
static size_t template_hint;
void nvk_record_template_bytes(size_t bytes) { template_hint = bytes; }
static void w_UpdateDescriptorSetWithTemplate(VkDevice dev, VkDescriptorSet set, VkDescriptorUpdateTemplate t, const void *data) {
    size_t bytes = template_hint ? template_hint : 32 * 24; template_hint = 0;
    if (!on_front()) { real.UpdateDescriptorSetWithTemplate(dev, set, t, data); return; }
    PTemplate *a = (void *)push_begin(R_SET_TEMPLATE, VK_NULL_HANDLE, sizeof(Head) + sizeof *a + bytes);
    a->set = set; a->t = t; a->bytes = (uint32_t)bytes; memcpy(a + 1, data, bytes);
    push_done((void *)a);
}
/* Transfers: queued too (their arrays are plain data); updates up to 64 KiB, the vkCmdUpdateBuffer limit. */
static void w_CopyBuffer(VkCommandBuffer cb, VkBuffer s, VkBuffer d, uint32_t n, const VkBufferCopy *r) {
    DIRECT_IF_NOT_FRONT(real.CopyBuffer(cb, s, d, n, r));
    PCopyBuffer *a = (void *)push_begin(R_COPY_BUFFER, cb, sizeof(Head) + sizeof *a + n * sizeof *r); a->src = s; a->dst = d; a->n = n; memcpy(a + 1, r, n * sizeof *r); push_done((void *)a);
}
static void w_UpdateBuffer(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkDeviceSize n, const void *data) {
    if (!on_front() || n > 65536) { if (on_front()) nvk_record_sync(); real.UpdateBuffer(cb, b, o, n, data); return; }
    PUpdateBuffer *a = (void *)push_begin(R_UPDATE_BUFFER, cb, sizeof(Head) + sizeof *a + (size_t)n); a->dst = b; a->off = o; a->n = n; memcpy(a + 1, data, (size_t)n); push_done((void *)a);
}
static void w_CopyBufferToImage(VkCommandBuffer cb, VkBuffer b, VkImage i, VkImageLayout l, uint32_t n, const VkBufferImageCopy *r) {
    DIRECT_IF_NOT_FRONT(real.CopyBufferToImage(cb, b, i, l, n, r));
    PBufImage *a = (void *)push_begin(R_BUFFER_TO_IMAGE, cb, sizeof(Head) + sizeof *a + n * sizeof *r); a->buf = b; a->img = i; a->layout = l; a->n = n; memcpy(a + 1, r, n * sizeof *r); push_done((void *)a);
}
static void w_CopyImageToBuffer(VkCommandBuffer cb, VkImage i, VkImageLayout l, VkBuffer b, uint32_t n, const VkBufferImageCopy *r) {
    DIRECT_IF_NOT_FRONT(real.CopyImageToBuffer(cb, i, l, b, n, r));
    PBufImage *a = (void *)push_begin(R_IMAGE_TO_BUFFER, cb, sizeof(Head) + sizeof *a + n * sizeof *r); a->buf = b; a->img = i; a->layout = l; a->n = n; memcpy(a + 1, r, n * sizeof *r); push_done((void *)a);
}
static void w_CopyImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageCopy *r) {
    DIRECT_IF_NOT_FRONT(real.CopyImage(cb, s, sl, d, dl, n, r));
    PImageImage *a = (void *)push_begin(R_COPY_IMAGE, cb, sizeof(Head) + sizeof *a + n * sizeof *r); a->src = s; a->sl = sl; a->dst = d; a->dl = dl; a->n = n; memcpy(a + 1, r, n * sizeof *r); push_done((void *)a);
}
static void w_BlitImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageBlit *r, VkFilter f) {
    DIRECT_IF_NOT_FRONT(real.BlitImage(cb, s, sl, d, dl, n, r, f));
    PImageImage *a = (void *)push_begin(R_BLIT_IMAGE, cb, sizeof(Head) + sizeof *a + n * sizeof *r); a->src = s; a->sl = sl; a->dst = d; a->dl = dl; a->n = n; a->filter = f; memcpy(a + 1, r, n * sizeof *r); push_done((void *)a);
}
static void w_ClearColorImage(VkCommandBuffer cb, VkImage i, VkImageLayout l, const VkClearColorValue *c, uint32_t n, const VkImageSubresourceRange *r) {
    DIRECT_IF_NOT_FRONT(real.ClearColorImage(cb, i, l, c, n, r));
    PClearImage *a = (void *)push_begin(R_CLEAR_COLOR_IMAGE, cb, sizeof(Head) + sizeof *a + n * sizeof *r); a->img = i; a->layout = l; a->c = *c; a->n = n; memcpy(a + 1, r, n * sizeof *r); push_done((void *)a);
}
static void w_ClearDepthStencilImage(VkCommandBuffer cb, VkImage i, VkImageLayout l, const VkClearDepthStencilValue *c, uint32_t n, const VkImageSubresourceRange *r) {
    DIRECT_IF_NOT_FRONT(real.ClearDepthStencilImage(cb, i, l, c, n, r));
    PClearImage *a = (void *)push_begin(R_CLEAR_DS_IMAGE, cb, sizeof(Head) + sizeof *a + n * sizeof *r); a->img = i; a->layout = l; a->ds = *c; a->n = n; memcpy(a + 1, r, n * sizeof *r); push_done((void *)a);
}

/* Made directly once everything queued is recorded. */
#define SYNC_WRAPPER(ret, name, params, args) \
    static ret w_##name params { if (on_front()) nvk_record_sync(); return real.name args; }
SYNC_WRAPPER(VkResult, BeginCommandBuffer, (VkCommandBuffer cb, const VkCommandBufferBeginInfo *i), (cb, i))
SYNC_WRAPPER(VkResult, EndCommandBuffer, (VkCommandBuffer cb), (cb))
SYNC_WRAPPER(VkResult, ResetCommandPool, (VkDevice d, VkCommandPool p, VkCommandPoolResetFlags f), (d, p, f))
SYNC_WRAPPER(VkResult, AllocateCommandBuffers, (VkDevice d, const VkCommandBufferAllocateInfo *i, VkCommandBuffer *o), (d, i, o))
SYNC_WRAPPER(VkResult, QueueSubmit, (VkQueue q, uint32_t n, const VkSubmitInfo *s, VkFence f), (q, n, s, f))
SYNC_WRAPPER(VkResult, QueueWaitIdle, (VkQueue q), (q))

static int record_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_VK_RECORD_THREAD"); on = e && !strcmp(e, "1"); }
    return on;
}
/* After the device's entry points are loaded, before the first recording. */
void nvk_record_install(void) {
    if (!record_on() || active) return;
    ring = malloc(RING_BYTES);
    if (!ring) { VKLOG("recording thread: no memory for the ring, off"); return; }
#define TAKE(field, name) do { real.field = name; name = w_##field; } while (0)
    TAKE(BindPipeline, vkCmdBindPipeline); TAKE(BindDescriptorSets, vkCmdBindDescriptorSets); TAKE(BindIndexBuffer, vkCmdBindIndexBuffer);
    TAKE(BindVertexBuffers, vkCmdBindVertexBuffers); TAKE(Draw, vkCmdDraw); TAKE(DrawIndexed, vkCmdDrawIndexed); TAKE(Dispatch, vkCmdDispatch);
    TAKE(SetViewport, vkCmdSetViewport); TAKE(SetScissor, vkCmdSetScissor); TAKE(SetPrimitiveTopology, vkCmdSetPrimitiveTopology);
    TAKE(SetCullMode, vkCmdSetCullMode); TAKE(SetFrontFace, vkCmdSetFrontFace); TAKE(SetDepthBiasEnable, vkCmdSetDepthBiasEnable);
    TAKE(SetDepthBias, vkCmdSetDepthBias); TAKE(SetBlendConstants, vkCmdSetBlendConstants); TAKE(SetDepthTestEnable, vkCmdSetDepthTestEnable);
    TAKE(SetDepthWriteEnable, vkCmdSetDepthWriteEnable); TAKE(SetDepthCompareOp, vkCmdSetDepthCompareOp);
    TAKE(SetDepthBoundsTestEnable, vkCmdSetDepthBoundsTestEnable); TAKE(SetRasterizerDiscardEnable, vkCmdSetRasterizerDiscardEnable);
    TAKE(SetStencilTestEnable, vkCmdSetStencilTestEnable); TAKE(SetStencilOp, vkCmdSetStencilOp); TAKE(SetStencilCompareMask, vkCmdSetStencilCompareMask);
    TAKE(SetStencilWriteMask, vkCmdSetStencilWriteMask); TAKE(SetStencilReference, vkCmdSetStencilReference);
    TAKE(SetPrimitiveRestartEnable, vkCmdSetPrimitiveRestartEnable); TAKE(BeginRendering, vkCmdBeginRendering); TAKE(EndRendering, vkCmdEndRendering);
    TAKE(PipelineBarrier, vkCmdPipelineBarrier); TAKE(BeginQuery, vkCmdBeginQuery); TAKE(EndQuery, vkCmdEndQuery); TAKE(WriteTimestamp, vkCmdWriteTimestamp);
    TAKE(ClearAttachments, vkCmdClearAttachments); TAKE(UpdateDescriptorSetWithTemplate, vkUpdateDescriptorSetWithTemplate);
    TAKE(BeginCommandBuffer, vkBeginCommandBuffer); TAKE(EndCommandBuffer, vkEndCommandBuffer); TAKE(ResetCommandPool, vkResetCommandPool);
    TAKE(AllocateCommandBuffers, vkAllocateCommandBuffers); TAKE(QueueSubmit, vkQueueSubmit); TAKE(QueueWaitIdle, vkQueueWaitIdle);
    TAKE(CopyBuffer, vkCmdCopyBuffer); TAKE(UpdateBuffer, vkCmdUpdateBuffer); TAKE(CopyBufferToImage, vkCmdCopyBufferToImage);
    TAKE(CopyImageToBuffer, vkCmdCopyImageToBuffer); TAKE(CopyImage, vkCmdCopyImage); TAKE(BlitImage, vkCmdBlitImage);
    TAKE(ClearColorImage, vkCmdClearColorImage); TAKE(ClearDepthStencilImage, vkCmdClearDepthStencilImage);
#undef TAKE
    pthread_t t;
    if (pthread_create(&t, NULL, worker, NULL)) { VKLOG("recording thread: not started, off"); return; }   /* wrappers stay inactive: direct */
    pthread_detach(t);
    active = 1;
    VKLOG("recording thread: on (TOTK_VK_RECORD_THREAD=1)");
}
/* vkCmdSetVertexInputEXT is fetched where used (nvn_vk_draw.c vertex_input): the pointer to call. */
PFN_vkCmdSetVertexInputEXT nvk_record_vertex_input(PFN_vkCmdSetVertexInputEXT driver) {
    if (!active) return driver;
    real.SetVertexInput = driver;
    return w_SetVertexInput;
}
/* The thread that begins submissions (mtl_sub_begin) is the one whose calls are queued. Called with the ring empty. */
void nvk_record_front(void) {
    if (!active) return;
    uintptr_t self = (uintptr_t)pthread_self();
    if (atomic_load_explicit(&front, memory_order_relaxed) != self) { nvk_record_sync(); atomic_store_explicit(&front, self, memory_order_relaxed); }
}
void nvk_record_stats(uint64_t v[4]) { v[0] = packets; v[1] = syncs; v[2] = sync_waits; v[3] = wraps; }
