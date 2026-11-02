/* Real backend code: pending counter values for conditional rendering,
 * clears while rendering is disabled, finalised-texture redirects and the
 * same-target render pass check. No Vulkan calls are reached. */
#include <pthread.h>
static int test_setname(pthread_t thread, const char *name) { (void)thread; (void)name; return 0; }
#define pthread_setname_np test_setname
#include "../nvn_vk_backend.c"
/* Tiny target write-backs live in nvn_vk_image.c, not linked here. */
void *nvk_tiny_wb_record(VkCommandBuffer c, const tex_t *t, unsigned slot, size_t cap, unsigned slots) { (void)c; (void)t; (void)slot; (void)cap; (void)slots; return NULL; }
int nvk_tiny_wb_finish(void *s, uint64_t st) { (void)s; (void)st; return 0; }
#include <assert.h>
NvkState nvk_S;
static tex_t live_tex; static int have_live;
int nvn_tex_live_at(const void *mem, tex_t *out) { if (!have_live || mem != live_tex.mem) return 0; *out = live_tex; return 1; }
static uint64_t released;
void nvk_image_release(uint64_t serial) { released = serial; }
_Noreturn void runtime_fail(const char *m) { fprintf(stderr, "%s\n", m); abort(); }
/* Images are opaque here: every target is 1280x720, depth targets have depth and stencil. */
static NvkImage *fake_image = (NvkImage *)(uintptr_t)0x1000;
NvkImage *nvk_image(const tex_t *t, VkCommandBuffer cb) { (void)t; (void)cb; return fake_image; }
NvkImage *nvk_image_discard(const tex_t *t, VkCommandBuffer cb) { (void)t; (void)cb; return fake_image; }
VkImageAspectFlags nvk_image_aspect(const NvkImage *image) { (void)image; return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT; }
VkExtent2D nvk_image_level_extent(const NvkImage *image, uint32_t level) { (void)image; (void)level; return (VkExtent2D){1280, 720}; }
VkImageView nvk_image_attachment_view(NvkImage *image, const view_t *view, uint32_t *level, uint32_t *layers) { (void)image; (void)view; *level = 0; *layers = 1; return VK_NULL_HANDLE; }
void nvk_image_written(NvkImage *image) { (void)image; }
int nvk_image_gpu_owned(const NvkImage *image) { (void)image; return 0; }
/* B5 test: serials scene_lo..scene_hi are one shared scene image (60-69: one image each), serial 52 another; else none. */
static uint64_t scene_lo, scene_hi;
NvkImage *nvk_image_existing(uint64_t serial) {
    if (!scene_lo) return NULL;
    if (serial >= 60 && serial < 70) return (NvkImage *)(uintptr_t)(0x5000 + 0x10 * serial);   /* separate images */
    return serial == 52 ? (NvkImage *)(uintptr_t)0x4000 : serial >= scene_lo && serial <= scene_hi ? (NvkImage *)(uintptr_t)0x3000 : NULL;
}
int nvk_dump_frame_active(void) { return 0; }
VkImage nvk_image_handle(const NvkImage *image) { (void)image; return VK_NULL_HANDLE; }
void nvk_image_sweep(void) {}
void nvk_image_drop_range(uintptr_t lo, uintptr_t hi) { (void)lo; (void)hi; }
static VkAttachmentLoadOp began_color, began_depth, began_stencil; static float began_clear; static unsigned begins, ends, clears;
static VkAttachmentStoreOp stored_color, stored_depth;
static void begin_query(VkCommandBuffer c, VkQueryPool p, uint32_t i, VkQueryControlFlags f) { (void)c; (void)p; (void)i; (void)f; }
static void end_query(VkCommandBuffer c, VkQueryPool p, uint32_t i) { (void)c; (void)p; (void)i; }
static void begin_render(VkCommandBuffer c, const VkRenderingInfo *ri) {
    (void)c; begins++; began_color = ri->pColorAttachments[0].loadOp; began_clear = ri->pColorAttachments[0].clearValue.color.float32[1];
    began_depth = ri->pDepthAttachment ? ri->pDepthAttachment->loadOp : VK_ATTACHMENT_LOAD_OP_MAX_ENUM;
    began_stencil = ri->pStencilAttachment ? ri->pStencilAttachment->loadOp : VK_ATTACHMENT_LOAD_OP_MAX_ENUM;
    stored_color = ri->pColorAttachments[0].storeOp; stored_depth = ri->pDepthAttachment ? ri->pDepthAttachment->storeOp : VK_ATTACHMENT_STORE_OP_MAX_ENUM;
    assert(!ri->pStencilAttachment || !ri->pDepthAttachment || (ri->pStencilAttachment->storeOp == stored_depth &&
           (ri->pStencilAttachment->loadOp == ri->pDepthAttachment->loadOp || ri->pStencilAttachment->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR)));
}
static void end_render(VkCommandBuffer c) { (void)c; ends++; }
static VkPipelineStageFlags barrier_src, barrier_dst; static VkAccessFlags barrier_access, barrier_dst_access; static VkDependencyFlags barrier_flags;
static unsigned barrier_calls, barrier_memory;
static void barrier(VkCommandBuffer c, VkPipelineStageFlags a, VkPipelineStageFlags b, VkDependencyFlags d, uint32_t n1, const VkMemoryBarrier *m,
    uint32_t n2, const VkBufferMemoryBarrier *bm, uint32_t n3, const VkImageMemoryBarrier *im) {
    (void)c; (void)n2; (void)bm; (void)n3; (void)im; barrier_calls++; barrier_src = a; barrier_dst = b; barrier_flags = d; barrier_memory = n1;
    barrier_access = n1 ? m->srcAccessMask : 0; barrier_dst_access = n1 ? m->dstAccessMask : 0; }
static void clear_attachments(VkCommandBuffer c, uint32_t n, const VkClearAttachment *a, uint32_t nr, const VkClearRect *r) { (void)c; (void)n; (void)a; (void)nr; (void)r; clears++; }
PFN_vkCmdBeginQuery vkCmdBeginQuery = begin_query;
PFN_vkCmdEndQuery vkCmdEndQuery = end_query;
PFN_vkCmdBeginRendering vkCmdBeginRendering = begin_render;
PFN_vkCmdEndRendering vkCmdEndRendering = end_render;
PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier = barrier;
PFN_vkCmdClearAttachments vkCmdClearAttachments = clear_attachments;
static unsigned image_clears; static float image_clear_value;
static void clear_color_image(VkCommandBuffer c, VkImage i, VkImageLayout l, const VkClearColorValue *v, uint32_t n, const VkImageSubresourceRange *r) {
    (void)c; (void)i; (void)l; (void)n; (void)r; image_clears++; image_clear_value = v->float32[1]; }
PFN_vkCmdClearColorImage vkCmdClearColorImage = clear_color_image;
static unsigned updates;
static void update_buffer(VkCommandBuffer c, VkBuffer b, VkDeviceSize o, VkDeviceSize n, const void *d) { (void)c; (void)b; (void)o; (void)n; (void)d; updates++; }
PFN_vkCmdUpdateBuffer vkCmdUpdateBuffer = update_buffer;
int totk_vpool_backing(uintptr_t va, uint64_t len, uintptr_t *cpu) { (void)va; (void)len; (void)cpu; return 0; }
/* Segmented submission: draws issued so far, and the command buffers ended, submitted and begun. */
static uint64_t draws_now; static unsigned cb_ended, cb_submitted, cb_begun, cb_allocated;
uint64_t nvk_draws_issued(void) { return draws_now; }
static VkResult end_cb(VkCommandBuffer c) { (void)c; cb_ended++; return VK_SUCCESS; }
static VkResult begin_cb(VkCommandBuffer c, const VkCommandBufferBeginInfo *i) { (void)c; (void)i; cb_begun++; return VK_SUCCESS; }
static VkResult submit(VkQueue q, uint32_t n, const VkSubmitInfo *si, VkFence f) {
    (void)q; (void)f; assert(n == 1 && si->commandBufferCount == 1 && !si->signalSemaphoreCount && !si->pNext); cb_submitted++; return VK_SUCCESS; }
static VkResult alloc_cb(VkDevice d, const VkCommandBufferAllocateInfo *i, VkCommandBuffer *out) {
    (void)d; (void)i; *out = (VkCommandBuffer)(uintptr_t)(0x2000 + 0x10 * ++cb_allocated); return VK_SUCCESS; }
/* Backend routing of the optional handover; the actual threaded handover is tested in record_thread_test.c. */
int nvk_record_segments_on(void) { const char *e = getenv("TOTK_VK_RECORD_SEGMENTS"); return e && !strcmp(e, "1"); }
static unsigned async_handoffs;
int nvk_record_segment(VkQueue q, VkCommandBuffer old, VkCommandBuffer next) {
    assert(nvk_record_segments_on() && old && next && old != next);
    end_cb(old);
    VkSubmitInfo si = {.commandBufferCount = 1, .pCommandBuffers = &old}; submit(q, 1, &si, VK_NULL_HANDLE);
    VkCommandBufferBeginInfo bi = {.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT}; begin_cb(next, &bi);
    async_handoffs++; return 1;
}
PFN_vkEndCommandBuffer vkEndCommandBuffer = end_cb;
PFN_vkBeginCommandBuffer vkBeginCommandBuffer = begin_cb;
PFN_vkQueueSubmit vkQueueSubmit = submit;
PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers = alloc_cb;

static tex_t texture(uint64_t serial, uint8_t *mem, int format) {
    tex_t t; memset(&t, 0, sizeof t);
    t.serial = serial; t.mem = mem; t.d.format = format; t.d.width = 1280; t.d.height = 720; t.d.depth = 1; t.d.levels = 1; t.d.target = 1; t.L.size = 1280 * 720 * 4;
    return t;
}

PFN_vkCreateQueryPool vkCreateQueryPool; PFN_vkResetQueryPool vkResetQueryPool;   /* not reached: no query pools here */
/* Deferred destruction (collect_garbage): destroyed images counted, releases recorded in order. */
static unsigned images_destroyed, released_n; static uintptr_t released_ctx[256];
static void destroy_image(VkDevice d, VkImage i, const VkAllocationCallbacks *a) { (void)d; (void)i; (void)a; images_destroyed++; }
static void destroy_view(VkDevice d, VkImageView v, const VkAllocationCallbacks *a) { (void)d; (void)v; (void)a; }
static void destroy_buffer(VkDevice d, VkBuffer b, const VkAllocationCallbacks *a) { (void)d; (void)b; (void)a; }
static void free_memory(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks *a) { (void)d; (void)m; (void)a; }
PFN_vkDestroyImage vkDestroyImage = destroy_image; PFN_vkDestroyImageView vkDestroyImageView = destroy_view;
PFN_vkDestroyBuffer vkDestroyBuffer = destroy_buffer; PFN_vkFreeMemory vkFreeMemory = free_memory;
static void note_release(void *ctx) { assert(images_destroyed > released_n); released_ctx[released_n++] = (uintptr_t)ctx; }
PFN_vkCmdWriteTimestamp vkCmdWriteTimestamp;   /* GPU-timed counter reports are off (TOTK_VK_COUNTER_GPU_TIME) */
int main(void) {
    /* A timestamp report still on the GPU writes counter 0; samples passed is unknown. */
    uint64_t counters[4] = {1, 0, 0, 0}, v = 99;
    ring[ring_cur].fence = 1;
    mtl_sub_counter((uintptr_t)&counters[0], NVN_COUNTER_TYPE_TIMESTAMP);
    assert(nvk_counter_lookup((uintptr_t)&counters[0], &v) == 1 && v == 0);
    assert(nvk_counter_lookup((uintptr_t)&counters[2], &v) == -1);
    mtl_sub_counter((uintptr_t)&counters[2], NVN_COUNTER_TYPE_SAMPLES_PASSED);
    assert(nvk_counter_lookup((uintptr_t)&counters[2], &v) == 0);
    fence_done = 1;   /* completed: the value is in memory */
    assert(nvk_counter_lookup((uintptr_t)&counters[0], &v) == -1);
    post_n = 0; counter_release(qcounter); qcounter = NULL;

    /* Uniform log: logical contents overlay pool memory with updates the GPU has not run, in order. */
    {
        uint8_t mem[16] = {0}, first[4] = {1, 2, 3, 4}, second[2] = {9, 9}, out[16];
        uint64_t base = (uintptr_t)mem;
        assert(!nvk_ubo_unflushed(base, 16));
        ubo_append(base + 4, first, 4, 0); ubo_unflushed++;
        ubo_append(base + 6, second, 2, 0); ubo_unflushed++;
        ubo_unflushed_lo = base + 4; ubo_unflushed_hi = base + 8;
        assert(nvk_ubo_unflushed(base, 16) && nvk_ubo_unflushed(base + 7, 1) && !nvk_ubo_unflushed(base + 8, 8));
        nvk_ubo_read(base, 16, out);
        assert(out[3] == 0 && out[4] == 1 && out[5] == 2 && out[6] == 9 && out[7] == 9 && out[8] == 0 && mem[4] == 0);
        for (size_t i = ubo_head; i < ubo_n; ++i) { ubo_log[i].flushed = 1; ubo_log[i].fence = 0; }
        ubo_unflushed = 0; fence_done = 1; ubo_prune();
        assert(ubo_n == 0 && !nvk_ubo_unflushed(base, 16));
        /* Page index (TOTK_VK_UBO_INDEX, second run off): entries on other pages leave a read untouched without a walk;
           an entry spanning pages, a hash collision (UBO_PAGES pages further) and pruning still give the right bytes. */
        static uint8_t big[(UBO_PAGES + 4) * 4096] __attribute__((aligned(4096)));
        uint64_t b0 = (uintptr_t)big;
        uint8_t span[8] = {7, 7, 7, 7, 7, 7, 7, 7}, col[4] = {5, 5, 5, 5}, o8[8];
        ubo_append(b0 + 4096 - 4, span, 8, 0); ubo_unflushed++;                      /* pages 0 and 1 */
        ubo_unflushed_lo = b0 + 4092; ubo_unflushed_hi = b0 + 4100;
        uint64_t sk = ubo_index_skips;
        nvk_ubo_read(b0 + 3 * 4096, 8, o8); assert(!o8[0] && !o8[7]);               /* page 3: nothing there */
        assert(ubo_index_on() ? ubo_index_skips == sk + 1 : ubo_index_skips == sk);
        nvk_ubo_read(b0 + 4096, 8, o8); assert(o8[0] == 7 && o8[3] == 7 && o8[4] == 0);   /* tail of the spanning entry */
        assert(nvk_ubo_unflushed(b0 + 4096, 4) && !nvk_ubo_unflushed(b0 + 4100, 4));
        ubo_append(b0 + (uint64_t)UBO_PAGES * 4096 + 4096 + 8, col, 4, 1);          /* same hash slot as page 1 */
        nvk_ubo_read(b0 + 4096, 16, out); assert(out[0] == 7 && out[8] == 0);       /* collision: walked, still right */
        nvk_ubo_read(b0 + (uint64_t)UBO_PAGES * 4096 + 4096, 16, out); assert(out[8] == 5 && out[0] == 0);
        for (size_t i = ubo_head; i < ubo_n; ++i) { ubo_log[i].flushed = 1; ubo_log[i].fence = 0; }
        ubo_unflushed = 0; ubo_prune();
        assert(ubo_n == 0);
        for (unsigned i = 0; i < UBO_PAGES; ++i) assert(!ubo_page_all[i]);         /* pruning cleared the index */
        /* Storage cannot be recycled while an entry is unflushed or its GPU fence is pending.
           Both modes must preserve overlays even across partial pruning and log compaction. */
        fence_done = 100; fence_issued = 100;
        uint8_t value[64]; memset(value, 0x3a, sizeof value);
        ubo_append(b0, value, sizeof value, 1);
        uint8_t *pending_copy = ubo_log[ubo_head].data;
        ubo_prune(); assert(ubo_n - ubo_head == 1);
        ubo_append(b0 + 64, value, sizeof value, 0);
        assert(ubo_log[ubo_n - 1].data != pending_copy);
        fence_done = 101; ubo_prune();
        assert(ubo_n - ubo_head == 1 && !ubo_log[ubo_head].flushed);
        memset(value, 0x6b, sizeof value);
        ubo_append(b0 + 128, value, sizeof value, 1);
        nvk_ubo_read(b0 + 64, 16, out); assert(out[0] == 0x3a && out[15] == 0x3a);
        nvk_ubo_read(b0 + 128, 16, out); assert(out[0] == 0x6b && out[15] == 0x6b);
        for (size_t i = ubo_head; i < ubo_n; ++i) ubo_log[i].flushed = 1;
        ubo_prune(); assert(!ubo_n);
        if (ubo_pool_on()) {
            uint64_t hits = ubo_pool_hits;
            uint8_t *small = ubo_data_alloc(33); assert(small); memset(small, 0xcd, 33);
            ubo_data_free(small, 33);
            uint8_t *again = ubo_data_alloc(64); assert(again == small && ubo_pool_hits > hits);
            ubo_data_free(again, 64);
            /* Every class boundary, including the first unpooled size. */
            for (uint64_t n = 63; n <= 65537; n = n < 65535 ? (n + 1) * 2 - 1 : n + 1) {
                uint8_t *p = ubo_data_alloc(n); assert(p); memset(p, 0xa5, n); ubo_data_free(p, n);
            }
            uint8_t *blocks[40];
            for (unsigned i = 0; i < 40; ++i) { blocks[i] = ubo_data_alloc(65536); assert(blocks[i]); memset(blocks[i], i, 65536); }
            for (unsigned i = 0; i < 40; ++i) ubo_data_free(blocks[i], 65536);
            assert(ubo_pool_bytes <= UBO_POOL_LIMIT);
            /* Drain cached blocks so the allocator's own accounting and release can be checked by ASan. */
            for (unsigned c = 0; c < UBO_POOL_CLASSES; ++c) while (ubo_free[c]) {
                UboFree *p = ubo_free[c]; ubo_free[c] = p->next; free(p); ubo_pool_bytes -= (size_t)64 << c;
            }
            assert(!ubo_pool_bytes);
            printf("uniform storage pool: pass (completion and flush lifetime, class boundaries, reuse, 2 MiB cap)\n");
        }
        fence_issued = 0; fence_done = 1;
        printf("uniform log: pass (overlay order, unflushed ranges, prune after completion, page index %s)\n", ubo_index_on() ? "on" : "off");
    }

    /* Disabled clears return before touching render targets or Vulkan. */
    nvk_S.render_disabled = 1;
    p_clear_color cc = {0}; cc.mask = 0xf; p_clear_ds cd = {0}; cd.depth_mask = 1;
    clear_color(&cc); clear_depth_stencil(&cd);
    assert(clears_skipped == 2);
    nvk_S.render_disabled = 0;

    /* A registration follows its storage once its serial is finalised. */
    static uint8_t storage[16], other[16];
    tex_t old = texture(10, storage, 0x25), got; view_t view; int has_view;
    mtl_reg_tex(7, &old, NULL);
    assert(nvk_registered_texture(7, &got, &view, &has_view) && got.serial == 10);
    live_tex = texture(11, storage, 0x25); have_live = 1;
    assert(nvk_registered_texture(7, &got, &view, &has_view) && got.serial == 10);   /* still alive: no redirect */
    mtl_tex_release(&old);
    assert(released == 10);
    assert(nvk_registered_texture(7, &got, &view, &has_view) && got.serial == 11 && tex_redirects == 1);
    assert(nvk_registered_texture(7, &got, &view, &has_view) && got.serial == 11 && tex_redirects == 1);
    /* Different shape at the same storage: not the same texture, keep the old one. */
    tex_t old2 = texture(20, other, 0x25);
    mtl_reg_tex(8, &old2, NULL); mtl_tex_release(&old2);
    live_tex = texture(21, other, 0x3f);
    assert(nvk_registered_texture(8, &got, &view, &has_view) && got.serial == 20 && tex_dead_unresolved == 1);
    /* Many finalised serials: the set grows and still answers. */
    for (uint64_t s = 1000; s < 11000; ++s) { tex_t t = texture(s, NULL, 0x25); mtl_tex_release(&t); }
    pthread_mutex_lock(&reg_lock);
    assert(dead_has_locked(10) && dead_has_locked(10999) && !dead_has_locked(11000) && dead_n == 10002);
    pthread_mutex_unlock(&reg_lock);

    /* Same targets as the open pass continue it; another serial does not. */
    tex_t c0 = texture(30, NULL, 0x25), d0 = texture(31, NULL, 0x46), c1 = texture(32, NULL, 0x25);
    p_set_rt rt; memset(&rt, 0, sizeof rt); rt.n = 1; rt.color[0] = &c0; rt.depth = &d0;
    cur_rt = rt; have_rt = 1; rendering = 1; pass_color[0] = 30; pass_depth = 31;
    assert(rt_same_as_pass(&rt));
    assert(nvk_rendering_attached(30) && nvk_rendering_attached(31) && !nvk_rendering_attached(32));
    p_set_rt moved = rt; moved.color[0] = &c1;
    assert(!rt_same_as_pass(&moved));
    p_set_rt viewed = rt; viewed.has_cview[0] = 1;
    assert(!rt_same_as_pass(&viewed));
    p_set_rt none; memset(&none, 0, sizeof none);
    pass_color[0] = 0; pass_depth = 0; cur_rt = none;
    assert(!rt_same_as_pass(&none));   /* no attachments: the viewport sizes the pass */
    rendering = 0;
    assert(!nvk_rendering_attached(30));

    /* Full-area clears before a pass become its load ops; a partial one does not. */
    cur_rt = rt; have_rt = 1; have_scissor = 0;
    cc.index = 0; cc.rgba[1] = 0.5f; cd.depth = 1.0f; cd.stencil_mask = 0xff;
    clear_color(&cc); clear_depth_stencil(&cd);
    assert(pend.any && begins == 0 && clears_folded == 2 && nvk_rendering_attached(30) == 2 && nvk_rendering_attached(31) == 2);
    p_set_rt again = rt; set_render_targets(&again);   /* same targets keep the folded clears */
    assert(pend.any && begins == 0);
    end_rendering(NVK_END_COMMIT);   /* nothing drew: the clears still happen */
    assert(begins == 1 && ends == 1 && !pend.any);
    assert(began_color == VK_ATTACHMENT_LOAD_OP_CLEAR && began_clear == 0.5f && began_depth == VK_ATTACHMENT_LOAD_OP_CLEAR && began_stencil == VK_ATTACHMENT_LOAD_OP_CLEAR);
    scissor = (p_scissor){0, 0, 640, 720}; have_scissor = 1;
    clear_color(&cc);
    assert(!pend.any && begins == 2 && clears == 1 && began_color == VK_ATTACHMENT_LOAD_OP_LOAD);
    end_rendering(NVK_END_COMMIT);
    have_scissor = 0;
    clear_color(&cc); set_render_targets(&moved);   /* other targets: flushed first */
    assert(begins == 3 && ends == 3 && !pend.any && began_color == VK_ATTACHMENT_LOAD_OP_CLEAR);

    /* B5 lazy scene clear (TOTK_VK_KEEP_SCENE=2). Serials 50 and 51 are one shared 1280x720 image, 52 another. */
    {
        end_rendering(NVK_END_COMMIT);
        scene_lo = 50; scene_hi = 51;
        tex_t s50 = texture(50, NULL, 0x25), s51 = texture(51, NULL, 0x25), s52 = texture(52, NULL, 0x25);
        p_set_rt srt; memset(&srt, 0, sizeof srt); srt.n = 1; srt.color[0] = &s50;
        p_set_rt via51 = srt; via51.color[0] = &s51;
        p_set_rt other = srt; other.color[0] = &s52;
        unsigned b0 = begins;
        /* A store through the second serial before any pass: the clear runs first, the later pass loads (26 Oct: the
           clear ran as that pass's load op and wiped the stored tiles). */
        cur_rt = srt; have_rt = 1; have_scissor = 0;
        clear_color(&cc);
        assert(scene_pending == 1 && !pend.any && begins == b0);
        nvk_scene_defer_write((NvkImage *)(uintptr_t)0x3000, 51);
        assert(!scene_pending && image_clears == 1 && image_clear_value == 0.5f);
        begin_rendering(); assert(began_color == VK_ATTACHMENT_LOAD_OP_LOAD); end_rendering(NVK_END_COMMIT);
        /* Menu frame: the scene is cleared, another 1280x720 image is cleared and drawn, the scene only read; the next
           clear supersedes it: the scene is never cleared (28 Oct: the other clear ran it, backdrop black again). */
        unsigned b1 = begins; uint64_t dropped = scene_dropped;
        clear_color(&cc);
        cur_rt = other; clear_color(&cc);
        assert(scene_pending == 2 && image_clears == 1 && begins == b1);
        begin_rendering(); assert(began_color == VK_ATTACHMENT_LOAD_OP_CLEAR && scene_pending == 1); end_rendering(NVK_END_COMMIT);
        cur_rt = srt; clear_color(&cc);
        assert(scene_pending == 1 && scene_dropped == dropped + 1 && image_clears == 1);
        /* Menu frame as TOTK draws it (28 Oct, fix 4 still black): the blur samples the scene, later a pass draws into it.
           The read drops the clear, so that pass loads and the next frame's blur still sees the world. */
        uint64_t reads = scene_read;
        clear_color(&cc); assert(scene_pending == 1);
        nvk_scene_defer_read((NvkImage *)(uintptr_t)0x3000, 51);
        assert(!scene_pending && scene_read == reads + 1 && image_clears == 1);
        begin_rendering(); assert(began_color == VK_ATTACHMENT_LOAD_OP_LOAD); end_rendering(NVK_END_COMMIT);
        cur_rt = srt; clear_color(&cc);
        /* World frame: the first pass, attaching the image through the other serial, clears it as its load op. */
        cur_rt = via51;
        begin_rendering(); assert(began_color == VK_ATTACHMENT_LOAD_OP_CLEAR && began_clear == 0.5f && !scene_pending);
        end_rendering(NVK_END_COMMIT);
        /* More pending images than slots: the oldest runs, none is lost. */
        static tex_t many[SCENE_SLOTS + 1]; scene_lo = 60; scene_hi = 60 + SCENE_SLOTS;
        for (int i = 0; i <= SCENE_SLOTS; ++i) many[i] = texture(60 + (uint64_t)i, NULL, 0x25);
        unsigned c0 = image_clears;
        for (int i = 0; i < SCENE_SLOTS; ++i) { cur_rt = srt; cur_rt.color[0] = &many[i]; clear_color(&cc); }
        assert(scene_pending == SCENE_SLOTS && image_clears == c0);
        cur_rt = srt; cur_rt.color[0] = &many[SCENE_SLOTS]; clear_color(&cc);
        assert(scene_pending == SCENE_SLOTS && image_clears == c0 + 1 && scene_evicted == 1);
        memset(scene_defer, 0, sizeof scene_defer); scene_pending = 0; scene_lo = scene_hi = 0; cur_rt = rt;
        printf("lazy scene clear: pass (store through another serial runs it first, a read first drops it, menu frames supersede it while another image clears, a pass through any serial loads it as clear, full slots run the oldest)\n");
    }

    /* Segmented submission: at a target change, once enough draws were recorded, the command buffer goes to the
       queue unsignalled and recording continues in a fresh one with a new state epoch. */
    VkCommandBuffer first = (VkCommandBuffer)(uintptr_t)0x1000;
    end_rendering(NVK_END_COMMIT); cb = first; ring_cur = 0; ring[0].nseg = 0; seg_mark = 0; draws_now = 20;
    uint64_t epoch = nvk_cmd_epoch();
    set_render_targets(&rt);                       /* 20 draws: below the first segment's 30, no segment */
    assert(cb == first && !cb_submitted);
    draws_now = 450; begin_rendering();
    p_set_rt same = rt; set_render_targets(&same); /* same targets keep the pass open: no segment */
    assert(cb == first && !cb_submitted && rendering);
    set_render_targets(&moved);                    /* the pass ends: segment */
    assert(!rendering && cb_ended == 1 && cb_submitted == 1 && cb_begun == 1 && cb != first && nvk_cmd_epoch() == epoch + 1);
    VkCommandBuffer second = cb;
    draws_now = 550; set_render_targets(&rt);      /* only 100 draws since the segment: later ones wait for 200 */
    assert(cb == second && cb_submitted == 1);
    draws_now = 700; set_render_targets(&moved);
    assert(cb_submitted == 2 && cb != second && ring[0].nseg == 2 && segments_total == 2);
    ring[0].nseg = 0; cb_allocated = 0;            /* a new submission reuses the slot's segment buffers */
    draws_now = 2000; set_render_targets(&rt);
    assert(cb == second && cb_allocated == 0);
    /* Draw checkpoints reuse closed-pass boundaries; they must never split a live or pending-clear pass. */
    {
        unsigned submitted = cb_submitted; uint64_t old_epoch = nvk_cmd_epoch(), old_fence = fence_issued;
        draws_now += 500;
        begin_rendering(); nvk_segment_checkpoint(); assert(cb_submitted == submitted && rendering);
        end_rendering(NVK_END_COMMIT);
        pend.any = 1; nvk_segment_checkpoint(); assert(cb_submitted == submitted); pend.any = 0;
        nvk_segment_checkpoint();
        const char *e = getenv("TOTK_VK_SEGMENT_CHECKPOINTS"); int on = e && !strcmp(e, "1");
        assert(cb_submitted == submitted + on && nvk_cmd_epoch() == old_epoch + on && fence_issued == old_fence);
        assert(checkpoint_cuts == (unsigned)on);
        puts("safe draw checkpoints: pass (closed-pass only, pending clears preserved, command epoch refreshed, no timeline signal)");
    }
    /* Sampled attachments: kept in the pass until something wrote them, the draw writes them, or a later draw does. */
    cur_rt = rt; have_rt = 1; memset(&pend, 0, sizeof pend); begin_rendering();
    assert(nvk_rt_slots(30, NULL) == 1 && nvk_rt_slots(31, NULL) == 0x100 && !nvk_rt_slots(32, NULL));
    assert(!nvk_feedback_split_needed(0x100, 0x1) && fb_kept == 1);      /* samples depth, writes colour 0 only */
    nvk_pass_after_draw(0x1, 0x100);
    assert(!nvk_feedback_split_needed(0x100, 0x1));                     /* the next one too: no split (was one each) */
    assert(nvk_feedback_split_needed(0x1, 0) && fb_written == 1);       /* colour 0 was written in the pass */
    assert(nvk_feedback_split_needed(0x100, 0x100) && fb_self == 1);    /* the draw writes the depth it samples */
    nvk_pass_before_draw(0x100);                                         /* depth sampled earlier, now written: split */
    assert(!rendering && fb_war == 1);
    begin_rendering(); nvk_pass_before_draw(0x100); assert(rendering);  /* fresh pass: nothing sampled yet */
    end_rendering(NVK_END_COMMIT);
    clear_depth_stencil(&cd);                                            /* folded clear: the load op writes depth */
    begin_rendering(); assert(nvk_feedback_split_needed(0x100, 0));
    end_rendering(NVK_END_COMMIT);
    /* Stores in the pass: a buffer store (TOTK's tile-class buffer) keeps sampling in the pass, an image store splits it. */
    begin_rendering(); nvk_pass_storage_write(0);
    uint64_t kept_before = fb_kept, written_before = fb_written;
    assert(!nvk_feedback_split_needed(0x100, 0) && fb_kept == kept_before + 1);
    nvk_pass_storage_write(1);
    assert(nvk_feedback_split_needed(0x100, 0) && fb_written == written_before + 1);
    end_rendering(NVK_END_COMMIT);
    begin_rendering(); assert(!nvk_feedback_split_needed(0x100, 0));   /* a new pass forgets both */
    end_rendering(NVK_END_COMMIT);
    /* Pass-end barriers: none when the pass wrote nothing, else scoped to what it wrote. */
    begin_rendering(); unsigned calls = barrier_calls; end_rendering(NVK_END_COMMIT);
    assert(barrier_calls == calls && pass_barriers_skipped >= 1);
    begin_rendering(); nvk_pass_after_draw(0x1, 0); end_rendering(NVK_END_COMMIT);
    assert(barrier_src == VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT && barrier_access == VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    begin_rendering(); nvk_pass_after_draw(0x100, 0); nvk_pass_storage_write(0); end_rendering(NVK_END_COMMIT);
    assert((barrier_src & VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT) && (barrier_src & VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT) &&
           !(barrier_src & VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT) && (barrier_access & VK_ACCESS_SHADER_WRITE_BIT));
    printf("pass-end barriers: pass (skipped without writes, colour / depth / storage scoped)\n");
    /* Discards before a pass begins: that pass does not load the attachment; target changes forget them. */
    set_render_targets(&moved); discard_op(0); discard_op(8); begin_rendering();
    assert(began_color == VK_ATTACHMENT_LOAD_OP_DONT_CARE && began_depth == VK_ATTACHMENT_LOAD_OP_DONT_CARE && discards_load == 2);
    discard_op(0); assert(discards_open == 1);                           /* the pass already loaded it */
    end_rendering(NVK_END_COMMIT); begin_rendering();
    assert(began_color == VK_ATTACHMENT_LOAD_OP_LOAD);                   /* consumed by the pass it applied to */
    end_rendering(NVK_END_COMMIT);
    discard_op(0); set_render_targets(&rt); set_render_targets(&moved); begin_rendering();
    assert(began_color == VK_ATTACHMENT_LOAD_OP_LOAD && discards_load == 2);
    end_rendering(NVK_END_COMMIT);
    /* Attachment use prediction: a frame shape seen UP_STREAK frames in a row elides stores (and loads) the passes
       never needed; a draw breaking the prediction ends the pass first and continues it with normal ops. */
    {
        android_vk_load_op_none = 1;
        /* Per frame: pass A writes colour, only tests depth; pass B (same targets, second in the frame) touches nothing. */
        for (uint64_t frame = 100; frame < 100 + UP_STREAK + 1; ++frame) {
            atomic_store(&current_frame, frame);
            set_render_targets(&rt); set_render_targets(&moved);
            nvk_pass_access(0x1, 0x100); begin_rendering(); nvk_pass_after_draw(0x1, 0);
            if (frame < 100 + UP_STREAK) assert(stored_color == VK_ATTACHMENT_STORE_OP_STORE && stored_depth == VK_ATTACHMENT_STORE_OP_STORE);
            else assert(stored_color == VK_ATTACHMENT_STORE_OP_STORE && stored_depth == VK_ATTACHMENT_STORE_OP_NONE &&
                        began_depth == VK_ATTACHMENT_LOAD_OP_LOAD);           /* depth: read, never written */
            end_rendering(NVK_END_FEEDBACK);
            begin_rendering();
            if (frame == 100 + UP_STREAK) assert(stored_color == VK_ATTACHMENT_STORE_OP_NONE && began_color == VK_ATTACHMENT_LOAD_OP_NONE &&
                                                 stored_depth == VK_ATTACHMENT_STORE_OP_NONE && began_depth == VK_ATTACHMENT_LOAD_OP_NONE);
            end_rendering(NVK_END_FEEDBACK);
        }
        /* Next frame pass A writes depth this time: the pass ends before that draw (nothing stored) and continues. */
        atomic_store(&current_frame, 100 + UP_STREAK + 1);
        set_render_targets(&rt); set_render_targets(&moved);
        nvk_pass_access(0x1, 0x100); begin_rendering(); nvk_pass_after_draw(0x1, 0);
        assert(stored_depth == VK_ATTACHMENT_STORE_OP_NONE);
        unsigned b0 = begins;
        nvk_pass_access(0x101, 0x100);
        assert(!rendering && up_mispredicted == 1 && pass_ends[NVK_END_PREDICT] == 1);
        begin_rendering(); nvk_pass_after_draw(0x101, 0);
        assert(begins == b0 + 1 && stored_depth == VK_ATTACHMENT_STORE_OP_STORE && stored_color == VK_ATTACHMENT_STORE_OP_STORE);
        nvk_pass_access(0x101, 0x100); assert(rendering);                        /* the continued pass elides nothing */
        end_rendering(NVK_END_FEEDBACK);
        /* Pass B uses colour now (a clear in the pass): ends first too. */
        begin_rendering(); assert(began_color == VK_ATTACHMENT_LOAD_OP_NONE);
        p_clear_color one = {0}; one.mask = 0xf; one.index = 0;
        clear_color(&one); assert(up_mispredicted == 2 && rendering && stored_color == VK_ATTACHMENT_STORE_OP_STORE);
        end_rendering(NVK_END_FEEDBACK);
        /* The frame after: depth was written in pass A, so it is stored again; pass B's colour too. */
        atomic_store(&current_frame, 100 + UP_STREAK + 2);
        set_render_targets(&rt); set_render_targets(&moved);
        nvk_pass_access(0x1, 0x100); begin_rendering(); assert(stored_depth == VK_ATTACHMENT_STORE_OP_STORE); end_rendering(NVK_END_FEEDBACK);
        begin_rendering(); assert(stored_color == VK_ATTACHMENT_STORE_OP_STORE); end_rendering(NVK_END_FEEDBACK);
        /* Without load op none only stores are elided; TOTK_VK_ATTACH_PREDICT=0 would leave up_tab NULL. */
        android_vk_load_op_none = 0;
        printf("attachment use prediction: pass (stores and loads elided after %d steady frames, a breaking draw or clear ends the pass first, observation stops the prediction)\n", UP_STREAK);
    }
    /* Framebuffer fetch barriers inside the pass. Depth read as an input attachment (TOTK_VK_FETCH_DEPTH): a by-region
       memory dependency from the depth writes of the pass, and before a later draw writes depth an execution dependency
       from the reads (no split). Colour keeps its old behaviour (memory dependency only, no write-after-read barrier). */
    {
        set_render_targets(&rt); begin_rendering(); assert(rendering);
        unsigned b0 = barrier_calls, e0 = ends;
        nvk_pass_local_read_barrier(0x100);                                    /* nothing written yet: no barrier */
        assert(barrier_calls == b0);
        nvk_pass_before_draw(0x100); assert(barrier_calls == b0 + 1 && barrier_src == VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT &&
            barrier_dst == (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT) &&
            barrier_flags == VK_DEPENDENCY_BY_REGION_BIT && !barrier_memory);   /* the read above, before this depth write */
        nvk_pass_after_draw(0x100, 0);
        nvk_pass_before_draw(0x100); assert(barrier_calls == b0 + 1);         /* no read since: nothing to order */
        nvk_pass_after_draw(0x100, 0);
        nvk_pass_local_read_barrier(0x100);
        assert(barrier_calls == b0 + 2 && barrier_src == (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT) &&
               barrier_access == VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT && barrier_dst == VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT &&
               barrier_dst_access == VK_ACCESS_INPUT_ATTACHMENT_READ_BIT && barrier_flags == VK_DEPENDENCY_BY_REGION_BIT);
        nvk_pass_before_draw(0x1); assert(barrier_calls == b0 + 2);           /* a colour write does not touch depth */
        nvk_pass_after_draw(0x1, 0);
        nvk_pass_local_read_barrier(0x1);                                      /* colour: as before */
        assert(barrier_calls == b0 + 3 && barrier_src == VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT && barrier_access == VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        nvk_pass_before_draw(0x1); assert(barrier_calls == b0 + 3);
        assert(rendering && ends == e0);                                       /* none of it split the pass */
        end_rendering(NVK_END_FEEDBACK);
        unsigned b1 = barrier_calls;
        begin_rendering(); assert(pass_fetch_read == 0);                      /* a new pass starts without pending reads */
        nvk_pass_before_draw(0x100); assert(barrier_calls == b1);
        end_rendering(NVK_END_FEEDBACK);
        printf("framebuffer fetch barriers: pass (depth: by-region memory dependency after depth writes, execution dependency before a later depth write, no split; colour unchanged)\n");
    }
    cb = NULL;
    /* Deferred destruction: at most 64 objects per submission (TOTK_VK_FREE_PER_SUBMIT), oldest first, the image
       destroyed before its memory release runs; nothing whose submission is still running. */
    {
        uint64_t saved_issued = fence_issued, saved_done = fence_done;
        fence_issued = 50; fence_done = 50;
        for (uintptr_t k = 1; k <= 100; ++k) nvk_defer_image_release((VkImage)(uintptr_t)(0x9000 + k), VK_NULL_HANDLE, NULL, 0, 50, note_release, (void *)k);
        nvk_defer_image_release((VkImage)(uintptr_t)0x9999, VK_NULL_HANDLE, NULL, 0, 51, note_release, (void *)999);   /* still running */
        collect_garbage(); assert(images_destroyed == 64 && released_n == 64 && released_ctx[0] == 1 && released_ctx[63] == 64);
        collect_garbage(); assert(images_destroyed == 100 && released_n == 100 && released_ctx[99] == 100);
        fence_done = 51; collect_garbage(); assert(released_n == 101 && released_ctx[100] == 999);
        fence_issued = saved_issued; fence_done = saved_done;
        printf("deferred destruction: pass (64 per submission, oldest first, release after destroy, running submissions kept)\n");
    }
    printf("discards: pass (not loaded when discarded before the pass, kept once it began, forgotten at a target change)\n");
    printf("feedback tracking: pass (read-only sampling and buffer stores keep the pass; written, self-written, image-stored, write-after-read and clear load ops split)\n");
    assert(async_handoffs == (nvk_record_segments_on() ? segments_total : 0));
    printf("segmented submission: pass (first after 30 draws, then 200, at a target change, never in an open pass, fresh state epoch, buffers reused)\n");
    /* Buffer ranges the GPU writes stay "live" until their submission completed. */
    fence_issued = 10; fence_done = 10;
    nvk_note_gpu_write(0x10000, 0x100);            /* written by the pending submission 11 */
    assert(nvk_gpu_written_live(0x10080, 0x10) && !nvk_gpu_written_live(0x10100, 0x10) && !nvk_gpu_written_live(0xff00, 0x100));
    fence_done = 11;
    assert(!nvk_gpu_written_live(0x10080, 0x10));
    printf("GPU-written ranges: pass (live until their submission completed, neighbours untouched)\n");
    printf("conditional clears / texture redirects / clear load ops: pass (pending counter values, %llu clears skipped, %llu redirect, same-target pass, %llu clears folded)\n",
        (unsigned long long)clears_skipped, (unsigned long long)tex_redirects, (unsigned long long)clears_folded);
    free(dead_set); free(tex_slots); free(post_ops);
}
