/* Texel staging split over helper threads (nvn_vk_image.c stage_fill) writes exactly what the single-threaded
 * nvn_vk_texture_stage writes, for a mip chain, an array and a 3D texture, many times in a row (thread hand-over
 * between jobs), with helpers and without (TOTK_VK_UPLOAD_THREADS=0 is the second run of this binary). */
#include <assert.h>
#include <stdio.h>
#include "../nvn_vk_image.c"
VkDevice nvk_dev;
VkPhysicalDevice nvk_phys;
uint64_t nvk_pending_fence(void) { return 1; }
uint64_t nvk_completed_fence(void) { return 0; }
uint32_t nvk_memory_type(uint32_t bits, VkMemoryPropertyFlags flags) { (void)bits; (void)flags; return 0; }
_Noreturn void nvk_die(const char *message) { fprintf(stderr, "%s\n", message); abort(); }
void nvk_barrier(VkCommandBuffer cb) { (void)cb; }
void nvk_defer_image(VkImage i, VkDeviceMemory m, VkImageView *v, unsigned n, uint64_t f) { (void)i; (void)m; (void)v; (void)n; (void)f; }
void nvk_defer_buffer(VkBuffer b, VkDeviceMemory m, uint64_t f) { (void)b; (void)m; (void)f; }
uint64_t nvn_dirty_now(void) { return 1; }
static const uint8_t *dirty_lo, *dirty_hi, *dirty2_lo, *dirty2_hi;   /* the one written byte range of the partial re-upload check */
int nvn_dirty_since(const void *a, size_t l, uint64_t s) {
    (void)s; const uint8_t *p = a; return (dirty_lo && p < dirty_hi && p + l > dirty_lo) || (dirty2_lo && p < dirty2_hi && p + l > dirty2_lo);
}
int nvn_dirty_tracked(const void *a, size_t l) { (void)a; (void)l; return 1; }
void nvn_dirty_protect(const void *a, size_t l) { (void)a; (void)l; }
static void fprops(VkPhysicalDevice d, VkFormat f, VkFormatProperties *p) { (void)d; (void)f; memset(p, 0, sizeof *p); }
PFN_vkGetPhysicalDeviceFormatProperties vkGetPhysicalDeviceFormatProperties = fprops;
PFN_vkCreateImage vkCreateImage;
PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements;
PFN_vkAllocateMemory vkAllocateMemory;
PFN_vkBindImageMemory vkBindImageMemory;
PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier;
PFN_vkBindBufferMemory vkBindBufferMemory;
PFN_vkCmdClearColorImage vkCmdClearColorImage;
PFN_vkCmdCopyBufferToImage vkCmdCopyBufferToImage;
PFN_vkCreateBuffer vkCreateBuffer;
PFN_vkDestroyBuffer vkDestroyBuffer;
PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements;
PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties;
PFN_vkMapMemory vkMapMemory;
PFN_vkUnmapMemory vkUnmapMemory;

static unsigned jobs;
static void check(int target, int format, uint32_t w, uint32_t h, uint32_t depth, uint32_t levels, unsigned rounds) {
    tex_t t; memset(&t, 0, sizeof t);
    t.d.target = target; t.d.format = format; t.d.width = w; t.d.height = h; t.d.depth = depth; t.d.levels = levels; t.d.samples = 1;
    t.d.swizzle_r = NVN_TEXTURE_SWIZZLE_R; t.d.swizzle_g = NVN_TEXTURE_SWIZZLE_G; t.d.swizzle_b = NVN_TEXTURE_SWIZZLE_B; t.d.swizzle_a = NVN_TEXTURE_SWIZZLE_A;
    nvn_tex_layout(&t.d, &t.L);
    uint8_t *mem = malloc(t.L.size);
    for (uint64_t i = 0; i < t.L.size; ++i) mem[i] = (uint8_t)(i * 2654435761u >> 13);
    t.mem = mem;
    NvnVkTextureStage want;
    assert(nvn_vk_texture_stage(&t.d, mem, (size_t)t.L.size, &want) == NVN_VK_STAGE_OK);
    for (unsigned k = 0; k < rounds; ++k) {
        NvnVkTextureStage plan;
        assert(nvn_vk_texture_stage_plan(&t.d, mem, (size_t)t.L.size, &plan) == NVN_VK_STAGE_OK);
        assert(plan.data_size == want.data_size && plan.region_count == want.region_count && !plan.data);
        uint8_t *dst = calloc(1, plan.data_size);
        assert(!stage_fill(&t, &plan, dst));
        assert(!memcmp(dst, want.data, want.data_size));
        free(dst); nvn_vk_texture_stage_destroy(&plan);
        jobs++;
    }
    nvn_vk_texture_stage_destroy(&want);
    free(mem);
}

/* Re-upload after a write into one level of one layer: only the regions over that storage are staged, packed from
 * offset 0, and each holds exactly the bytes a full staging puts at its old offset. */
static void check_partial(uint32_t layers, uint32_t levels, uint32_t level, uint32_t layer) {
    tex_t t; memset(&t, 0, sizeof t);
    t.d.target = NVN_TEXTURE_TARGET_2D_ARRAY; t.d.format = NVN_FORMAT_RGBA8; t.d.width = 256; t.d.height = 128; t.d.depth = layers;
    t.d.levels = levels; t.d.samples = 1;
    t.d.swizzle_r = NVN_TEXTURE_SWIZZLE_R; t.d.swizzle_g = NVN_TEXTURE_SWIZZLE_G; t.d.swizzle_b = NVN_TEXTURE_SWIZZLE_B; t.d.swizzle_a = NVN_TEXTURE_SWIZZLE_A;
    nvn_tex_layout(&t.d, &t.L);
    uint8_t *mem = malloc(t.L.size);
    for (uint64_t i = 0; i < t.L.size; ++i) mem[i] = (uint8_t)(i * 2654435761u >> 11);
    t.mem = mem;
    NvnVkTextureStage want, plan;
    assert(nvn_vk_texture_stage(&t.d, mem, (size_t)t.L.size, &want) == NVN_VK_STAGE_OK);
    assert(nvn_vk_texture_stage_plan(&t.d, mem, (size_t)t.L.size, &plan) == NVN_VK_STAGE_OK);
    uint64_t a = layer * t.L.layer_size + nvn_tex_level_offset(&t.d, level);
    dirty_lo = mem + a + 16; dirty_hi = dirty_lo + 8;   /* a few bytes inside that level */
    /* Test the original subresource selector separately; strip rectangles are checked below. */
    int strip_result = restage_strips(&plan, &t, 1);
    if (strip_result) {
        /* This small fixture has a single band in the selected level. */
        assert(plan.region_count == 1 && plan.regions[0].imageOffset.y == 0);
    } else restage_dirty(&plan, &t, 1);
    dirty_lo = dirty_hi = NULL;
    assert(plan.region_count == 1 && plan.regions[0].bufferOffset == 0);
    assert(plan.regions[0].imageSubresource.mipLevel == level && plan.regions[0].imageSubresource.baseArrayLayer == layer);
    uint32_t k = layer * levels + level;   /* the plan orders regions layer-major */
    for (uint32_t i = 0; i < want.region_count; ++i)
        if (want.regions[i].imageSubresource.mipLevel == level && want.regions[i].imageSubresource.baseArrayLayer == layer) k = i;
    VkDeviceSize end = k + 1 < want.region_count ? want.regions[k + 1].bufferOffset : want.data_size;
    assert(plan.data_size == end - want.regions[k].bufferOffset);
    uint8_t *dst = calloc(1, plan.data_size);
    assert(!stage_fill(&t, &plan, dst));
    assert(!memcmp(dst, (uint8_t *)want.data + want.regions[k].bufferOffset, plan.data_size));
    free(dst); nvn_vk_texture_stage_destroy(&plan); nvn_vk_texture_stage_destroy(&want); free(mem);
}

/* Reconstruct each updated subresource from the old GPU bytes plus the proposed copies.
   Dirty granularity deliberately matches the Thor's coarse 128 KiB tracker. Every uploaded byte must agree with
   a fresh full conversion; every changed source byte must be represented, including at bands and BC image edges. */
static unsigned strip_cases;
static uint64_t strip_full_bytes, strip_actual_bytes;
static void check_strips(int format, uint32_t width, uint32_t height, uint32_t layers, uint32_t levels, unsigned pattern) {
    tex_t t = {0};
    t.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D_ARRAY, .format = format, .width = width, .height = height,
                         .depth = layers, .levels = levels, .samples = 1};
    nvn_tex_layout(&t.d, &t.L);
    uint8_t *mem = malloc(t.L.size); assert(mem); t.mem = mem;
    for (uint64_t i = 0; i < t.L.size; ++i) mem[i] = (uint8_t)((i * 97u + i / 37u) & 255u);
    NvnVkTextureStage old, want, plan;
    assert(nvn_vk_texture_stage(&t.d, mem, t.L.size, &old) == NVN_VK_STAGE_OK);
    uint64_t start;
    if (pattern == 0) start = t.L.size / 2;
    else if (pattern == 1) start = 0;
    else if (pattern == 2) start = t.L.size > 131072 ? t.L.size - 131072 : 0;
    else start = t.L.size / 3;
    start = start / 131072 * 131072;
    uint64_t len = pattern == 3 ? 262144 : 131072;
    if (len > t.L.size - start) len = t.L.size - start;
    for (uint64_t i = start; i < start + len; ++i) mem[i] ^= 0x5b;
    dirty_lo = mem + start; dirty_hi = dirty_lo + len;
    if (pattern == 4) {
        uint64_t last = t.L.size > 131072 ? (t.L.size - 1) / 131072 * 131072 : 0;
        dirty2_lo = mem + last; dirty2_hi = mem + t.L.size;
        for (uint64_t i = last; i < t.L.size; ++i) mem[i] ^= 0x3d;
    }
    assert(nvn_vk_texture_stage(&t.d, mem, t.L.size, &want) == NVN_VK_STAGE_OK);
    assert(nvn_vk_texture_stage_plan(&t.d, mem, t.L.size, &plan) == NVN_VK_STAGE_OK);
    size_t full_size = plan.data_size;
    restage_dirty(&plan, &t, 1);
    dirty_lo = dirty_hi = dirty2_lo = dirty2_hi = NULL;
    uint8_t *dst = malloc(plan.data_size ? plan.data_size : 1); assert(dst);
    assert(!stage_fill(&t, &plan, dst));
    int bw, bh; uint32_t bpb = nvn_fmt_bpb(format, &bw, &bh);
    for (uint32_t i = 0; i < plan.region_count; ++i) {
        const VkBufferImageCopy *r = &plan.regions[i];
        uint32_t j = 0;
        while (j < old.region_count && (old.regions[j].imageSubresource.mipLevel != r->imageSubresource.mipLevel
            || old.regions[j].imageSubresource.baseArrayLayer != r->imageSubresource.baseArrayLayer)) j++;
        assert(j < old.region_count && r->imageOffset.x == 0 && r->imageOffset.y >= 0 && r->imageOffset.z == 0);
        assert(r->bufferOffset % 4 == 0 && r->bufferOffset % bpb == 0);
        const VkBufferImageCopy *whole = &old.regions[j];
        size_t row = (whole->imageExtent.width + bw - 1) / bw * bpb;
        size_t rows = (r->imageExtent.height + bh - 1) / bh;
        assert((uint32_t)r->imageOffset.y + r->imageExtent.height <= whole->imageExtent.height);
        size_t offset = whole->bufferOffset + (size_t)r->imageOffset.y / bh * row;
        assert(offset + rows * row <= old.data_size && r->bufferOffset + rows * row <= plan.data_size);
        assert(!memcmp(dst + r->bufferOffset, (uint8_t *)want.data + offset, rows * row));
        memcpy((uint8_t *)old.data + offset, dst + r->bufferOffset, rows * row);
    }
    for (uint32_t j = 0; j < old.region_count; ++j) {
        const VkBufferImageCopy *r = &old.regions[j];
        size_t bytes = (size_t)((r->imageExtent.width + bw - 1) / bw) * bpb * ((r->imageExtent.height + bh - 1) / bh);
        assert(!memcmp((uint8_t *)old.data + r->bufferOffset, (uint8_t *)want.data + r->bufferOffset, bytes));
    }
    strip_full_bytes += full_size; strip_actual_bytes += plan.data_size; strip_cases++;
    NvnVkTextureStage clean;
    assert(nvn_vk_texture_stage_plan(&t.d, mem, t.L.size, &clean) == NVN_VK_STAGE_OK);
    restage_dirty(&clean, &t, 1);
    assert(!clean.region_count && !clean.data_size); /* no dirty sources: no copies */
    nvn_vk_texture_stage_destroy(&clean);
    free(dst); free(mem); nvn_vk_texture_stage_destroy(&old); nvn_vk_texture_stage_destroy(&want); nvn_vk_texture_stage_destroy(&plan);
}

int main(void) {
    for (unsigned pattern = 0; pattern < 5; ++pattern) {
        check_strips(NVN_FORMAT_RGBA8, 2048, 2048, 1, 1, pattern);  /* large single-level uploads */
        check_strips(NVN_FORMAT_RGBA8, 513, 1025, 3, 5, pattern);  /* odd dimensions, layers, mips */
        check_strips(NVN_FORMAT_RGB8, 513, 1025, 2, 4, pattern);   /* 3-byte texels and copy alignment */
        check_strips(NVN_FORMAT_RGBA_DXT5, 1025, 2049, 2, 4, pattern); /* compressed edges */
        check_strips(NVN_FORMAT_RGBA_ASTC_8x8, 2049, 2049, 1, 3, pattern);
    }
    printf("strip uploads: %u reconstructions identical to full staging; %.2f MiB -> %.2f MiB (%s)\n",
        strip_cases, strip_full_bytes / 1048576.0, strip_actual_bytes / 1048576.0, strips_on() ? "strips on" : "strips off");
    assert(strip_actual_bytes < strip_full_bytes);
    check_partial(8, 6, 0, 3);   /* the large top level of a middle layer */
    check_partial(8, 6, 4, 7);   /* a small level of the last layer */
    check_partial(1, 9, 2, 0);
    check(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 1024, 1024, 1, 11, 40);
    check(NVN_TEXTURE_TARGET_2D_ARRAY, NVN_FORMAT_RGBA_DXT5, 512, 256, 6, 4, 20);
    check(NVN_TEXTURE_TARGET_3D, NVN_FORMAT_RG16F, 128, 96, 40, 3, 20);
    check(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_R8, 300, 200, 1, 1, 10);   /* below the split size: replay thread only */
    /* Many regions (layers x levels): over half the task table stays on the replay thread (the first Thor run of
     * 22 Oct hung here: the sizing loop never ended), just under it is split with the largest task size. */
    check(NVN_TEXTURE_TARGET_2D_ARRAY, NVN_FORMAT_RGBA8, 64, 64, 600, 1, 2);
    check(NVN_TEXTURE_TARGET_2D_ARRAY, NVN_FORMAT_RGBA8, 128, 128, 200, 2, 2);
    check(NVN_TEXTURE_TARGET_2D_ARRAY, NVN_FORMAT_RGBA8, 256, 256, 42, 6, 4);
    assert(!atomic_load(&upload_split_refused));
    printf("partial re-upload: pass (one changed level x layer staged alone, bytes identical to full staging)\n");
    printf("texture staging over %d helper threads: %u jobs identical to single-threaded staging (%llu split, %llu row ranges)\n",
        fill_threads, jobs, (unsigned long long)atomic_load(&upload_split), (unsigned long long)atomic_load(&upload_split_tasks));
    return 0;
}
