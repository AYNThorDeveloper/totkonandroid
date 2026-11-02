/* Depth fetch choice (nvn_vk_draw.c fetch_depth_ok / fetch_choose, TOTK_VK_FETCH_DEPTH): a proven own-pixel sample of
 * the pass's depth attachment becomes attachment 8 with the sampled view's channel mapping, and is refused for another
 * level or layer, another format, a viewport not covering the target, or a draw that writes depth. Colour fetch is
 * unchanged. Also the per-pass cache of the pipeline key's target formats (key_formats): kept for later draws of the
 * same open pass on the same target serials, looked up again otherwise; TOTK_VK_KEY_CACHE=0 (second run) always
 * looks up. And the split diagnostics' classification (split_note). */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
static int test_setname(pthread_t t, const char *name) { (void)t; (void)name; return 0; }
#define pthread_setname_np test_setname
#include "../nvn_vk_draw.c"

VkDevice nvk_dev;
VkPhysicalDevice nvk_phys;
_Noreturn void nvk_die(const char *s) { fprintf(stderr, "%s\n", s); abort(); }

/* Textures by id: 1 = the depth target (serial 10), 2 = colour target 0 (serial 20). */
static tex_t tex_depth, tex_color; static view_t tex_view; static int tex_has_view;
int nvk_registered_texture(int id, tex_t *t, view_t *v, int *has_view) {
    if (id == 1) *t = tex_depth; else if (id == 2) *t = tex_color; else return 0;
    *v = tex_view; *has_view = tex_has_view;
    return 1;
}
static NvkImage *const img_depth = (NvkImage *)(uintptr_t)0x100, *const img_color = (NvkImage *)(uintptr_t)0x200;
NvkImage *nvk_image_existing(uint64_t serial) { return serial == 10 ? img_depth : serial == 20 ? img_color : NULL; }
uint16_t nvk_rt_slots(uint64_t serial, const NvkImage *img) { (void)img; return serial == 10 ? 0x100 : serial == 20 ? 0x1 : 0; }
static int depth_on = 1;
int nvk_fetch_depth_on(void) { return depth_on; }
static uint8_t got_att[8]; static uint16_t got_zsw; static int got_calls, got_z;
static NvkShader variant;
NvkShader *nvk_shader_fetch_variant(NvkShader *ps, const uint8_t att[8]) { (void)ps; memcpy(got_att, att, 8); got_calls++; got_z = 0; return &variant; }
NvkShader *nvk_shader_fetch_variant_z(NvkShader *ps, const uint8_t att[8], uint16_t zsw) { (void)ps; memcpy(got_att, att, 8); got_zsw = zsw; got_calls++; got_z = 1; return &variant; }
/* Pipeline key formats: an open pass number and image lookups that are counted. */
static uint64_t open_pass; static unsigned image_lookups;
uint64_t nvk_open_pass_serial(void) { return open_pass; }
static uint16_t written_slots;
uint16_t nvk_pass_written_slots(void) { return written_slots; }
NvkImage *nvk_image(const tex_t *t, VkCommandBuffer cb) { (void)cb; image_lookups++; return t->serial == 10 ? img_depth : img_color; }
VkFormat nvk_image_format(const NvkImage *image) { return image == img_depth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM; }
VkImageAspectFlags nvk_image_aspect(const NvkImage *image) { return image == img_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT; }

/* Driver banks (driver_bank): a fake arena so fills can be counted and their bytes checked. */
static uint64_t pend_fence = 1; static uint8_t arena_mem[1 << 22]; static unsigned buffers;
uint64_t nvk_pending_fence(void) { return pend_fence; }
int nvk_counter_lookup(uint64_t addr, uint64_t *v) { (void)addr; (void)v; return 0; }
static uint64_t rt_ep = 1, tex_gen = 1;
uint64_t nvk_rt_epoch(void) { return rt_ep; }
uint64_t nvk_tex_info_gen(void) { return tex_gen; }
uint64_t nvk_completed_fence(void) { return pend_fence - 1; }
uint32_t nvk_memory_type(uint32_t bits, VkMemoryPropertyFlags f) { (void)bits; (void)f; return 0; }
void nvk_defer_buffer(VkBuffer b, VkDeviceMemory m, uint64_t f) { (void)b; (void)m; (void)f; }
static VkResult cbuf(VkDevice d, const VkBufferCreateInfo *ci, const VkAllocationCallbacks *a, VkBuffer *out) { (void)d; (void)ci; (void)a; *out = (VkBuffer)(uintptr_t)(0x700 + ++buffers); return VK_SUCCESS; }
static void breq(VkDevice d, VkBuffer b, VkMemoryRequirements *r) { (void)d; (void)b; r->size = 1 << 22; r->memoryTypeBits = 1; r->alignment = 256; }
static VkResult amem(VkDevice d, const VkMemoryAllocateInfo *i, const VkAllocationCallbacks *a, VkDeviceMemory *m) { (void)d; (void)i; (void)a; *m = (VkDeviceMemory)(uintptr_t)1; return VK_SUCCESS; }
static VkResult bmem(VkDevice d, VkBuffer b, VkDeviceMemory m, VkDeviceSize o) { (void)d; (void)b; (void)m; (void)o; return VK_SUCCESS; }
static VkResult mapm(VkDevice d, VkDeviceMemory m, VkDeviceSize o, VkDeviceSize n, VkMemoryMapFlags f, void **p) { (void)d; (void)m; (void)o; (void)n; (void)f; *p = arena_mem; return VK_SUCCESS; }
static void unmapm(VkDevice d, VkDeviceMemory m) { (void)d; (void)m; }
static void props(VkPhysicalDevice d, VkPhysicalDeviceProperties *p) { (void)d; memset(p, 0, sizeof *p); p->limits.minUniformBufferOffsetAlignment = 64; p->limits.minStorageBufferOffsetAlignment = 64; }
PFN_vkCreateBuffer vkCreateBuffer = cbuf; PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements = breq;
PFN_vkAllocateMemory vkAllocateMemory = amem; PFN_vkBindBufferMemory vkBindBufferMemory = bmem;
PFN_vkMapMemory vkMapMemory = mapm; PFN_vkUnmapMemory vkUnmapMemory = unmapm; PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties = props;
/* Misaligned storage copies (buffer_info): one pool buffer at 0x10000, copies counted. */
static uint64_t write_epoch = 1; static unsigned copies, ends;
uint64_t nvk_buffer_write_epoch(void) { return write_epoch; }
void nvk_buffer_write_bump(void) { write_epoch++; }
int nvk_resolve(uint64_t addr, uint64_t len, VkBuffer *b, uint64_t *off) { (void)len; *b = (VkBuffer)(uintptr_t)0x99; *off = addr - 0x10000; return addr >= 0x10000; }
static unsigned pool_queries;
uint64_t nvk_pool_remaining(uint64_t addr) { pool_queries++; return 0x100000 - (addr - 0x10000); }
static uint64_t pending_lo, pending_hi, test_ubo_epoch = 1;
int nvk_ubo_unflushed(uint64_t addr, uint64_t size) { return pending_lo && addr < pending_hi && addr + size > pending_lo; }
uint64_t nvk_ubo_epoch(void) { return test_ubo_epoch; }
/* Each epoch bump stands for a new overlapping in-pass update (nvn_vk_draw.c keys snapshots by it). */
uint64_t nvk_ubo_unflushed_seq(uint64_t addr, uint64_t size) { return nvk_ubo_unflushed(addr, size) ? test_ubo_epoch : 0; }
void nvk_ubo_read(uint64_t addr, uint64_t size, uint8_t *out) { (void)addr; memset(out, (uint8_t)test_ubo_epoch, size); }
VkCommandBuffer nvk_cmd(void) { return (VkCommandBuffer)(uintptr_t)0x55; }
void nvk_rendering_end(int cause) { (void)cause; ends++; }
void nvk_barrier(VkCommandBuffer c) { (void)c; }
static void copybuf(VkCommandBuffer c, VkBuffer s, VkBuffer d, uint32_t n, const VkBufferCopy *r) { (void)c; (void)s; (void)d; (void)n; (void)r; copies++; }
static void pbarrier(VkCommandBuffer c, VkPipelineStageFlags a, VkPipelineStageFlags b, VkDependencyFlags d, uint32_t n1, const VkMemoryBarrier *m,
    uint32_t n2, const VkBufferMemoryBarrier *bm, uint32_t n3, const VkImageMemoryBarrier *im) { (void)c; (void)a; (void)b; (void)d; (void)n1; (void)m; (void)n2; (void)bm; (void)n3; (void)im; }
PFN_vkCmdCopyBuffer vkCmdCopyBuffer = copybuf; PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier = pbarrier;
static void bank_matches(VkDescriptorBufferInfo bi, int bank, int stage) {
    uint8_t want[0xA00]; nvk_driver_bank_fill(want, bank, stage, dispatch_grid);
    assert(!memcmp(arena_mem + bi.offset, want, sizeof want));
}

static tex_t target(uint64_t serial, int format) {
    tex_t t; memset(&t, 0, sizeof t);
    t.serial = serial; t.d.format = format; t.d.width = 1280; t.d.height = 720; t.d.depth = 1; t.d.levels = 1; t.d.target = 1;
    t.d.swizzle_r = NVN_TEXTURE_SWIZZLE_R; t.d.swizzle_g = NVN_TEXTURE_SWIZZLE_G; t.d.swizzle_b = NVN_TEXTURE_SWIZZLE_B; t.d.swizzle_a = NVN_TEXTURE_SWIZZLE_A;
    return t;
}
static p_set_rt rt;
static void state(int depth_write) {
    nvk_S.has_vp = 1; nvk_S.vp[0][0] = 0; nvk_S.vp[0][1] = 0; nvk_S.vp[0][2] = 1280; nvk_S.vp[0][3] = 720;
    nvk_S.ds.dt = 1; nvk_S.ds.dw = (uint8_t)depth_write;
}
static int ok(uint16_t *z) { return fetch_depth_ok(&tex_depth, tex_has_view ? &tex_view : NULL, &rt, z); }

int main(void) {
    tex_depth = target(10, NVN_FORMAT_DEPTH32F); tex_color = target(20, NVN_FORMAT_RGBA8);
    memset(&rt, 0, sizeof rt); rt.n = 1; rt.color[0] = &tex_color; rt.depth = &tex_depth;
    state(0);
    uint16_t z = 0;
    /* Channel mapping: identity gives (D, 0, 0, 1); a swizzle is carried into the shuffle. */
    assert(ok(&z) == FD_OK && z == 0x7440);
    tex_has_view = 1; memset(&tex_view, 0, sizeof tex_view); tex_view.set_swizzle = 1;
    for (int i = 0; i < 4; ++i) tex_view.swz[i] = NVN_TEXTURE_SWIZZLE_R;
    assert(ok(&z) == FD_OK && z == 0x0000);                                       /* RRRR */
    tex_view.swz[1] = NVN_TEXTURE_SWIZZLE_ZERO; tex_view.swz[2] = NVN_TEXTURE_SWIZZLE_ONE; tex_view.swz[3] = NVN_TEXTURE_SWIZZLE_G;
    assert(ok(&z) == FD_OK && z == 0x4740);                                       /* R, 0, 1, G (= 0) */
    /* Refusals. */
    memset(&tex_view, 0, sizeof tex_view); tex_view.set_levels = 1; tex_view.base_level = 1;
    assert(ok(&z) == FD_TARGET);
    memset(&tex_view, 0, sizeof tex_view); tex_view.set_format = 1; tex_view.format = NVN_FORMAT_DEPTH16;
    assert(ok(&z) == FD_FORMAT);
    tex_has_view = 0;
    nvk_S.vp[0][2] = 640; assert(ok(&z) == FD_VIEWPORT); state(0);
    state(1); assert(ok(&z) == FD_WRITES);
    nvk_S.ds.dt = 0; assert(ok(&z) == FD_OK);                                     /* depth write without the test writes nothing */
    state(0);
    /* fetch_choose: the depth slot becomes attachment 8, a colour slot its colour attachment, as before. */
    NvkShader vs; memset(&vs, 0, sizeof vs); vs.fetch_locs = 1u << 5;
    NvkShader ps; memset(&ps, 0, sizeof ps); ps.stage = 1; ps.nfetch = 2;
    ps.fetch[0].id = 100; ps.fetch[0].slot = 0; ps.fetch[0].loc = 5;
    ps.fetch[1].id = 101; ps.fetch[1].slot = 1; ps.fetch[1].loc = 5;
    nvk_S.tex[1][0] = 1; nvk_S.tex[1][1] = 2;                                     /* slot 0: depth, slot 1: colour */
    assert(fetch_choose(&ps, &vs, &rt) == &variant && got_z && got_att[0] == 8 && got_att[1] == 0 && got_zsw == 0x7440);
    depth_on = 0;
    assert(fetch_choose(&ps, &vs, &rt) == &variant && !got_z && got_att[0] == 0xff && got_att[1] == 0);   /* switch off: colour only */
    depth_on = 1; state(1);
    assert(fetch_choose(&ps, &vs, &rt) == &variant && !got_z && got_att[0] == 0xff && fd_count[FD_WRITES] == 1);
    state(0); ps.nfetch = 1;
    int calls = got_calls;
    nvk_S.vp[0][2] = 640; assert(!fetch_choose(&ps, &vs, &rt) && got_calls == calls); state(0);   /* nothing fetched: no variant */
    /* Split classification. */
    NvkShader decal; memset(&decal, 0, sizeof decal); snprintf(decal.variant, sizeof decal.variant, "5733c5bf03d78005bb7dd05115e17d12-6c026feb388a"); decal.nfetch = 2;
    written_slots = 0x101; split_note(&decal, 1, 0x100); split_note(&decal, 1, 0x100); split_note(&decal, 1, 0x1); split_note(&decal, 1, 0x101);
    written_slots = 0; split_note(&decal, 1, 0x1);
    assert(spl_total[SPL_DEPTH] == 2 && spl_total[SPL_COLOUR] == 1 && spl_total[SPL_BOTH] == 1 && spl_total[SPL_OTHER] == 1);
    int found = 0;
    for (int i = 0; i < 128; ++i) if (!strcmp(spl_tab[i].name, "5733c5bf")) { found = 1; assert(spl_tab[i].proof && !spl_tab[i].fetch && spl_tab[i].n[SPL_DEPTH] == 2); }
    assert(found);
    /* Pipeline key target formats. */
    int cache = key_cache_on();
    PipeKey k; memset(&k, 0, sizeof k);
    open_pass = 0; image_lookups = 0;
    key_formats(&k, &rt, NULL); assert(image_lookups == 2 && k.color[0] == VK_FORMAT_R8G8B8A8_UNORM && k.depth == VK_FORMAT_D32_SFLOAT && !k.stencil);
    open_pass = 7; key_formats(&k, &rt, NULL); assert(image_lookups == 4);       /* first draw of pass 7 */
    memset(&k, 0, sizeof k);
    key_formats(&k, &rt, NULL); assert(image_lookups == (cache ? 4u : 6u) && k.color[0] == VK_FORMAT_R8G8B8A8_UNORM && k.depth == VK_FORMAT_D32_SFLOAT);
    tex_t other = target(21, NVN_FORMAT_RGBA8); rt.color[0] = &other;             /* same pass, another target serial */
    unsigned before = image_lookups; key_formats(&k, &rt, NULL); assert(image_lookups == before + 2);
    rt.color[0] = &tex_color;
    open_pass = 8; before = image_lookups; key_formats(&k, &rt, NULL); assert(image_lookups == before + 2);   /* new pass */
    open_pass = 0; before = image_lookups; key_formats(&k, &rt, NULL); assert(image_lookups == before + 2);   /* no pass open */
    /* Texture registry lookup reused within one draw (TOTK_VK_TEX_LOOKUP_ONCE; off in the TOTK_VK_KEY_CACHE=0 run too). */
    {
        int once = tc_on();
        tex_t got; view_t gv; int ghv = 0; view_t vw; memset(&vw, 0, sizeof vw); vw.set_format = 1; vw.format = NVN_FORMAT_RGBA8;
        tc_token++; tc_put(1, 3, 0x123, &tex_color, &vw, 1);
        assert(tc_get(1, 3, 0x123, &got, &gv, &ghv) == once);
        if (once) assert(got.serial == 20 && ghv == 1 && gv.set_format && gv.format == NVN_FORMAT_RGBA8);
        assert(!tc_get(1, 3, 0x124, &got, &gv, &ghv));                                  /* another handle */
        assert(!tc_get(0, 3, 0x123, &got, &gv, &ghv) && !tc_get(1, 4, 0x123, &got, &gv, &ghv));   /* another stage / binding */
        tc_token++; assert(!tc_get(1, 3, 0x123, &got, &gv, &ghv));                       /* the next draw */
        assert(!tc_get(9, 3, 0x123, &got, &gv, &ghv) && !tc_get(1, 64, 0x123, &got, &gv, &ghv));  /* out of range */
        printf("texture lookup reuse: pass (%s)\n", once ? "same draw, stage, binding and handle only" : "TOTK_VK_TEX_LOOKUP_ONCE=0: never reused");
    }
    /* Driver banks: refilled only when the stage's bindings (or the grid, compute) changed or a submission began. */
    {
        int fast = driver_fast_on();
        state_reset();
        uint64_t fills0 = driver_fills;
        VkDescriptorBufferInfo a = driver_bank(2, 1); bank_matches(a, 2, 1);
        VkDescriptorBufferInfo b = driver_bank(2, 1); bank_matches(b, 2, 1);
        assert(driver_fills == fills0 + (fast ? 1 : 2) && a.offset == b.offset);   /* unchanged: the same copy */
        p_bind_handles th; memset(&th, 0, sizeof th); th.stage = 1; th.first = 5; th.n = 1; th.h[0] = 0x1234;
        nvk_state_op(OP_BIND_TEX, &th);
        VkDescriptorBufferInfo c = driver_bank(2, 1); bank_matches(c, 2, 1); assert(c.offset != a.offset);   /* new texture handle */
        th.stage = 0; nvk_state_op(OP_BIND_TEX, &th);                                                         /* another stage */
        uint64_t f1 = driver_fills; VkDescriptorBufferInfo d = driver_bank(2, 1); assert(d.offset == c.offset && driver_fills == f1 + !fast);
        p_bind_buf sb; memset(&sb, 0, sizeof sb); sb.stage = 1; sb.index = 2; sb.addr = 0x5000; sb.size = 64;
        nvk_state_op(OP_BIND_SSBO, &sb);
        VkDescriptorBufferInfo e = driver_bank(0, 1); bank_matches(e, 0, 1);
        dispatch_grid[0] = 4; VkDescriptorBufferInfo g1 = driver_bank(0, 5); bank_matches(g1, 0, 5);
        dispatch_grid[0] = 8; VkDescriptorBufferInfo g2 = driver_bank(0, 5); bank_matches(g2, 0, 5); assert(g2.offset != g1.offset);   /* grid */
        pend_fence++; f1 = driver_fills; driver_bank(2, 1); assert(driver_fills == f1 + 1);                  /* next submission */
        printf("driver banks: pass (%s)\n", fast ? "reused while the stage's bindings and the grid stay the same, refilled on any change or submission" : "TOTK_VK_DRIVER_BANK_FAST=0: filled every time");
    }
    /* Fast negative uniform-snapshot checks must preserve pool clamping and immutable snapshot lifetime. */
    {
        VkDescriptorBufferInfo bi;
        unsigned before = pool_queries;
        assert(!ubo_snapshot(0x11000, 256, &bi));
        assert(pool_queries == before + (ubo_early_on() ? 0 : 1));
        pending_lo = 0x11080; pending_hi = pending_lo + 16;
        assert(ubo_snapshot(0x11000, 256, &bi) && bi.range == 256);
        uint64_t saved = bi.offset;
        assert(arena_mem[saved] == 1 && arena_mem[saved + 255] == 1);
        assert(ubo_snapshot(0x11000, 256, &bi) && bi.offset == saved);
        test_ubo_epoch++;
        assert(ubo_snapshot(0x11000, 256, &bi) && bi.offset != saved);
        assert(arena_mem[bi.offset] == 2 && arena_mem[saved] == 1);
        /* A pending write outside the 64-KiB maximum is irrelevant for an unknown or oversized uniform range. */
        pending_lo = 0x12000 + 65536 + 16; pending_hi = pending_lo + 4;
        assert(!ubo_snapshot(0x12000, 0, &bi) && !ubo_snapshot(0x12000, 1u << 20, &bi));
        pending_lo = 0x12000 + 65535; pending_hi = pending_lo + 1;
        assert(ubo_snapshot(0x12000, 0, &bi) && bi.range == 65536);
        /* Candidate intersects, actual pool-clamped range does not. */
        pending_lo = 0x110000 + 16; pending_hi = pending_lo + 4;
        assert(!ubo_snapshot(0x110000 - 32, 256, &bi));
        pending_lo = pending_hi = 0;
        printf("uniform snapshot early check: pass (%s, unknown sizes, 64-KiB cap, pool boundary, epoch and immutable copies)\n", ubo_early_on() ? "pool lookup skipped when no pending write" : "original path");
    }
    /* Shader variant signature epochs: change exactly with what the stage's signature depends on. */
    {
        uint64_t a1[4], b1[4], a0[4], b0[4];
        if (nvk_sig_epochs(1, a1)) {
            p_bind_handles th; memset(&th, 0, sizeof th); th.stage = 0; th.first = 1; th.n = 1; th.h[0] = 7;
            nvk_sig_epochs(0, a0); nvk_state_op(OP_BIND_TEX, &th);
            nvk_sig_epochs(1, b1); nvk_sig_epochs(0, b0);
            assert(!memcmp(a1, b1, sizeof a1) && memcmp(a0, b0, sizeof a0));                 /* a vertex texture: stage 0 only */
            p_bind_vattr va; memset(&va, 0, sizeof va); va.n = 1; va.st[0] = 3;
            nvk_sig_epochs(0, a0); nvk_state_op(OP_BIND_VATTR, &va); nvk_sig_epochs(0, b0); assert(a0[1] != b0[1]);   /* attributes */
            nvk_sig_epochs(1, a1); rt_ep++; nvk_sig_epochs(1, b1); assert(a1[2] != b1[2]);    /* targets: fragment */
            nvk_sig_epochs(0, a0); assert(a0[2] == 0);                                         /* not vertex */
            tex_gen++; nvk_sig_epochs(0, b0); assert(b0[3] != a0[3]);                          /* a registration */
            printf("signature epochs: pass (bindings per stage, attributes, targets, registrations)\n");
        } else printf("signature epochs: off (TOTK_VK_SIG_CACHE=0)\n");
    }
    /* Misaligned read-only storage ranges: bound from the earlier copy until a GPU buffer write or a new submission. */
    {
        int reuse = !getenv("TOTK_VK_SBUF_COPY_REUSE");
        VkDescriptorBufferInfo a, b;
        copies = 0;
        assert(buffer_info(0x10010, 256, &a, 64, UINT32_MAX, 0) && copies == 1 && a.buffer != (VkBuffer)(uintptr_t)0x99);
        assert(buffer_info(0x10010, 256, &b, 64, UINT32_MAX, 0) && copies == (reuse ? 1u : 2u));
        if (reuse) assert(b.buffer == a.buffer && b.offset == a.offset);
        nvk_buffer_write_bump(); assert(buffer_info(0x10010, 256, &b, 64, UINT32_MAX, 0) && copies == (reuse ? 2u : 3u));   /* written since */
        unsigned c0 = copies; pend_fence++; assert(buffer_info(0x10010, 256, &b, 64, UINT32_MAX, 0) && copies == c0 + 1);   /* next submission */
        c0 = copies; assert(buffer_info(0x10010, 128, &b, 64, UINT32_MAX, 0) && copies == c0 + 1);                        /* other range */
        c0 = copies; buffer_info(0x10020, 256, &b, 64, UINT32_MAX, 1); buffer_info(0x10020, 256, &b, 64, UINT32_MAX, 1);
        assert(copies == c0 + 2); ncopy_back = 0;                                                                          /* written ranges: always */
        printf("misaligned storage copies: %s\n", reuse ? "pass (reused within a submission until a buffer write; written ranges always copied)" : "off (TOTK_VK_SBUF_COPY_REUSE=0)");
    }
    printf("depth fetch choice: pass (channel mapping carried, other level/format/viewport and depth-writing draws refused, colour as before, off by switch)\n");
    printf("split diagnostics: pass (depth / colour / both / neither per shader)\n");
    printf("pipeline key formats: pass (%s)\n", cache ? "kept within an open pass on the same targets, looked up for a new pass, other targets or no pass" : "TOTK_VK_KEY_CACHE=0: looked up every draw");
    return 0;
}
