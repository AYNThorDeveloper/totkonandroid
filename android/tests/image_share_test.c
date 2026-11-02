/* Real image table: serials at the same storage, layout and format share one
 * image; it is kept after its last holder, swept later and dropped with its pool. */
#include <assert.h>
#include <stdio.h>
#include "../nvn_vk_image.c"
VkDevice nvk_dev;
VkPhysicalDevice nvk_phys;
static uint64_t pending = 1, tracked = 1; static unsigned created, destroyed;
uint64_t nvk_pending_fence(void) { return pending; }
uint64_t nvk_completed_fence(void) { return pending - 1; }
uint32_t nvk_memory_type(uint32_t bits, VkMemoryPropertyFlags flags) { (void)bits; (void)flags; return 0; }
_Noreturn void nvk_die(const char *message) { fprintf(stderr, "%s\n", message); abort(); }
void nvk_barrier(VkCommandBuffer cb) { (void)cb; }
void nvk_defer_image(VkImage i, VkDeviceMemory m, VkImageView *v, unsigned n, uint64_t f) { (void)i; (void)m; (void)v; (void)n; (void)f; destroyed++; }
/* Suballocated images: the release runs when the test says the GPU is done (release_pending). */
static void (*pending_release[64])(void *); static void *pending_ctx[64]; static unsigned npending;
void nvk_defer_image_release(VkImage i, VkDeviceMemory m, VkImageView *v, unsigned n, uint64_t f, void (*rel)(void *), void *ctx) {
    (void)i; (void)m; (void)v; (void)n; (void)f; destroyed++;
    if (rel) { assert(npending < 64); pending_release[npending] = rel; pending_ctx[npending++] = ctx; }
}
static void release_pending(void) { for (unsigned k = 0; k < npending; ++k) pending_release[k](pending_ctx[k]); npending = 0; }
void nvk_defer_buffer(VkBuffer b, VkDeviceMemory m, uint64_t f) { (void)b; (void)m; (void)f; }
uint64_t nvn_dirty_now(void) { return 1; }
int nvn_dirty_since(const void *a, size_t l, uint64_t s) { (void)a; (void)l; (void)s; return 0; }
int nvn_dirty_tracked(const void *a, size_t l) { (void)a; (void)l; return (int)tracked; }
void nvn_dirty_protect(const void *a, size_t l) { (void)a; (void)l; }
static void fprops(VkPhysicalDevice d, VkFormat f, VkFormatProperties *p) { (void)d; (void)f; memset(p, 0, sizeof *p); p->optimalTilingFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT; }
static uint32_t last_nflist; static VkFormat last_fl[4]; static VkImageCreateFlags last_flags;
static VkResult cimage(VkDevice d, const VkImageCreateInfo *ci, const VkAllocationCallbacks *a, VkImage *out) {
    (void)d; (void)a; *out = (VkImage)(uintptr_t)++created; last_nflist = 0; last_flags = ci->flags;
    for (const VkBaseInStructure *n = ci->pNext; n; n = n->pNext) if (n->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO) {
        const VkImageFormatListCreateInfo *l = (const VkImageFormatListCreateInfo *)n;
        last_nflist = l->viewFormatCount; for (uint32_t i = 0; i < l->viewFormatCount && i < 4; ++i) last_fl[i] = l->pViewFormats[i];
    }
    return VK_SUCCESS;
}
static VkDeviceSize req_size = 1 << 20; static unsigned allocations; static VkDeviceSize last_bind_off; static VkDeviceMemory last_bind_mem;
static void ireq(VkDevice d, VkImage i, VkMemoryRequirements *r) { (void)d; (void)i; r->size = req_size; r->memoryTypeBits = 1; r->alignment = 256; }
static void ireq2(VkDevice d, const VkImageMemoryRequirementsInfo2 *in, VkMemoryRequirements2 *r) {
    ireq(d, in->image, &r->memoryRequirements);
    for (VkBaseOutStructure *n = r->pNext; n; n = n->pNext) if (n->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS)
        ((VkMemoryDedicatedRequirements *)n)->requiresDedicatedAllocation = ((VkMemoryDedicatedRequirements *)n)->prefersDedicatedAllocation = VK_FALSE;
}
static unsigned frees;
static VkResult alloc(VkDevice d, const VkMemoryAllocateInfo *i, const VkAllocationCallbacks *a, VkDeviceMemory *out) { (void)d; (void)i; (void)a; *out = (VkDeviceMemory)(uintptr_t)(0x100 + ++allocations); return VK_SUCCESS; }
static void freemem(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks *a) { (void)d; (void)m; (void)a; frees++; }
PFN_vkFreeMemory vkFreeMemory = freemem;
static VkResult bind(VkDevice d, VkImage i, VkDeviceMemory m, VkDeviceSize o) { (void)d; (void)i; last_bind_mem = m; last_bind_off = o; return VK_SUCCESS; }
static void barrier(VkCommandBuffer c, VkPipelineStageFlags s, VkPipelineStageFlags t, VkDependencyFlags f, uint32_t a, const VkMemoryBarrier *b,
    uint32_t bc, const VkBufferMemoryBarrier *bb, uint32_t ic, const VkImageMemoryBarrier *ib) { (void)c; (void)s; (void)t; (void)f; (void)a; (void)b; (void)bc; (void)bb; (void)ic; (void)ib; }
PFN_vkGetPhysicalDeviceFormatProperties vkGetPhysicalDeviceFormatProperties = fprops;
PFN_vkCreateImage vkCreateImage = cimage;
PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements = ireq;
PFN_vkGetImageMemoryRequirements2 vkGetImageMemoryRequirements2 = ireq2;
PFN_vkAllocateMemory vkAllocateMemory = alloc;
PFN_vkBindImageMemory vkBindImageMemory = bind;
PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier = barrier;
/* Upload path: not reached (every image here is a fully cleared target). */
PFN_vkBindBufferMemory vkBindBufferMemory;
PFN_vkCmdClearColorImage vkCmdClearColorImage;
PFN_vkCmdCopyBufferToImage vkCmdCopyBufferToImage;
PFN_vkCreateBuffer vkCreateBuffer;
PFN_vkDestroyBuffer vkDestroyBuffer;
PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements;
PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties;
PFN_vkMapMemory vkMapMemory;
PFN_vkUnmapMemory vkUnmapMemory;
static tex_t tex(uint64_t serial, void *mem, int format) {
    tex_t t; memset(&t, 0, sizeof t);
    t.serial = serial; t.mem = mem; t.L.size = 1280 * 720 * 4; t.d.format = format; t.d.width = 1280; t.d.height = 720; t.d.depth = 1;
    t.d.levels = 1; t.d.target = 1; t.d.samples = 1;
    return t;
}
int main(void) {
    static uint8_t pool[2][16];
    tex_t a = tex(1, pool[0], NVN_FORMAT_RGBA8), b = tex(2, pool[0], NVN_FORMAT_RGBA8), c = tex(3, pool[0], NVN_FORMAT_R32F);
    NvkImage *ia = nvk_image_discard(&a, NULL);
    assert(nvk_image_discard(&b, NULL) == ia && created == 1 && share_live == 1);   /* same storage: one image */
    assert(nvk_image_discard(&c, NULL) != ia && created == 2);                       /* other format: its own */
    assert(nvk_image_existing(2) == ia && nvk_image_existing(3) != ia);
    nvk_image_written(ia);
    nvk_image_release(1); nvk_image_release(2);
    assert(!destroyed && idle_images == 1 && !nvk_image_existing(1));                /* kept for the next frame's object */
    tex_t d = tex(4, pool[0], NVN_FORMAT_RGBA8);
    assert(nvk_image_discard(&d, NULL) == ia && share_revived == 1 && idle_images == 0 && nvk_image_gpu_owned(ia));
    nvk_image_release(4);
    pending += SHARE_IDLE_SUBMITS; nvk_image_sweep(); assert(!destroyed);
    pending += 1; nvk_image_sweep(); assert(destroyed == 1 && idle_images == 0);      /* unused too long */
    tex_t e = tex(5, pool[1], NVN_FORMAT_RGBA8);
    nvk_image_discard(&e, NULL); nvk_image_release(5);
    nvk_image_drop_range((uintptr_t)pool[1], (uintptr_t)pool[1] + 16);
    assert(destroyed == 2 && idle_bytes == 0);                                        /* pool finalised */
    tex_t f = tex(6, pool[1], NVN_FORMAT_RGBA8), g = tex(7, pool[1], NVN_FORMAT_RGBA8);
    NvkImage *fi = nvk_image_discard(&f, NULL); nvk_image_release(6);
    tracked = 0;                                                                      /* CPU copy, storage untracked */
    assert(nvk_image_discard(&g, NULL) != fi && share_dropped_untracked == 1 && destroyed == 3);
    nvk_image_release(7); nvk_image_release(3);
    share_enabled = 0;
    tex_t h = tex(8, pool[0], NVN_FORMAT_RGBA8), i = tex(9, pool[0], NVN_FORMAT_RGBA8);
    assert(nvk_image_discard(&h, NULL) != nvk_image_discard(&i, NULL));              /* TOTK_VK_TEX_SHARE=0 */
    puts("image storage sharing: pass (shared, kept, revived, swept, dropped with the pool, off switch)");
    /* Format lists (TOTK_VK_FORMAT_LIST=1 in the second run): own format plus its sRGB partner; a view outside the
       list is counted and gets the image's own format. Without the switch: no list, any view, still counted. */
    tex_t fl = tex(20, pool[0], NVN_FORMAT_RGBA8), fr = tex(21, pool[0], NVN_FORMAT_R32F);
    NvkImage *fli = nvk_image_discard(&fl, NULL);
    assert(last_flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT);
    if (format_list_on()) {
        assert(last_nflist == 2 && last_fl[0] == VK_FORMAT_R8G8B8A8_UNORM && last_fl[1] == VK_FORMAT_R8G8B8A8_SRGB);
        assert(view_format(fli, VK_FORMAT_R8G8B8A8_SRGB) == VK_FORMAT_R8G8B8A8_SRGB && flist_outside == 0);
        assert(view_format(fli, VK_FORMAT_R32_UINT) == VK_FORMAT_R8G8B8A8_UNORM && flist_outside == 1);
        NvkImage *fri = nvk_image_discard(&fr, NULL);
        assert(last_nflist == 1 && last_fl[0] == VK_FORMAT_R32_SFLOAT && view_format(fri, VK_FORMAT_R32_SFLOAT) == VK_FORMAT_R32_SFLOAT);
        assert(view_format(fri, VK_FORMAT_R32_UINT) == VK_FORMAT_R32_SFLOAT && flist_outside == 2);
    } else {
        assert(last_nflist == 0);
        assert(view_format(fli, VK_FORMAT_R8G8B8A8_SRGB) == VK_FORMAT_R8G8B8A8_SRGB && flist_outside == 0);
        assert(view_format(fli, VK_FORMAT_R32_UINT) == VK_FORMAT_R32_UINT && flist_outside == 1);   /* logged, still allowed */
    }
    assert(srgb_partner(VK_FORMAT_ASTC_6x6_UNORM_BLOCK) == VK_FORMAT_ASTC_6x6_SRGB_BLOCK && srgb_partner(VK_FORMAT_ASTC_6x6_SRGB_BLOCK) == VK_FORMAT_ASTC_6x6_UNORM_BLOCK);
    assert(srgb_partner(VK_FORMAT_BC7_SRGB_BLOCK) == VK_FORMAT_BC7_UNORM_BLOCK && srgb_partner(VK_FORMAT_D32_SFLOAT) == VK_FORMAT_UNDEFINED);
    /* Texture memory in shared blocks (TOTK_VK_TEX_SUBALLOC; the second run sets TOTK_VK_FORMAT_LIST=0 TOTK_VK_TEX_SUBALLOC=0). */
    {
        static uint8_t sp[8][16];
        int sub = tex_suballoc_on();
        release_pending();   /* ranges of images dropped earlier in this test come back first */
        unsigned al0 = allocations; req_size = 1 << 20;
        tex_t s1 = tex(40, sp[0], NVN_FORMAT_RGBA8), s2 = tex(41, sp[1], NVN_FORMAT_RGBA8), s3 = tex(42, sp[2], NVN_FORMAT_RGBA8);
        NvkImage *i1 = nvk_image_discard(&s1, NULL); VkDeviceMemory m1 = last_bind_mem; VkDeviceSize o1 = last_bind_off;
        NvkImage *i2 = nvk_image_discard(&s2, NULL); VkDeviceMemory m2 = last_bind_mem; VkDeviceSize o2 = last_bind_off;
        nvk_image_discard(&s3, NULL); VkDeviceSize o3 = last_bind_off;
        if (sub) {
            assert(allocations - al0 <= 1 && m1 == m2 && o1 != o2 && o2 != o3 && !(o1 % 256) && !(o2 % 256) && !(o3 % 256) && i1->sub && i2->sub);
            unsigned np0 = npending; nvk_image_release(41); assert(npending == np0 + 1);   /* dropped (sharing is off here): its range comes back only after the GPU finished with it */
            tex_t s4 = tex(43, sp[3], NVN_FORMAT_RGBA8); nvk_image_discard(&s4, NULL);
            assert(last_bind_off != o2);          /* not yet returned */
            release_pending();
            tex_t s5 = tex(44, sp[4], NVN_FORMAT_RGBA8); nvk_image_discard(&s5, NULL);
            assert(last_bind_off == o2 && last_bind_mem == m1);   /* returned: reused (first fit) */
            unsigned al1 = allocations; req_size = 16u << 20;
            tex_t big = tex(45, sp[5], NVN_FORMAT_RGBA8); NvkImage *ib = nvk_image_discard(&big, NULL);
            assert(allocations == al1 + 1 && last_bind_off == 0 && !ib->sub);   /* large: own memory */
        } else assert(allocations - al0 == 3 && !i1->sub);
        req_size = 1 << 20;
        printf("texture memory blocks: %s\n", sub ? "pass (small images share a block, ranges aligned and returned only after release, large images own memory)" : "off (TOTK_VK_TEX_SUBALLOC=0): one allocation per image");
    }
    printf("format lists: pass (%s)\n", format_list_on() ? "own format + sRGB partner, a view outside falls back to the own format" : "off: no list, views outside a list counted and allowed");
}
