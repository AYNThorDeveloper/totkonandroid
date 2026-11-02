/* NVN textures as Vulkan images for the Android execution layer.
 *
 * An image is created when a texture is first used by the GPU path (render
 * target now, sampling later) and starts with the texture's current pool
 * contents, de-swizzled by nvn_vk_texture_stage. Depth/stencil storage is
 * opaque on the console and starts undefined; games clear it before use.
 * All images stay in VK_IMAGE_LAYOUT_GENERAL. Images are looked up by the texture
 * serial (hle_nvn.c), because the game copies texture objects by value.
 *
 * Storage sharing (as TOTK_TEX_SHARE in nvn_mtl.m): TOTK re-initialises render
 * target objects at the same storage every frame and samples them through other,
 * persistent objects. On the console every descriptor of that memory reads the
 * same bytes; with one image per serial the new object started from stale pool
 * bytes and the sampling object never saw what was rendered. Serials with the
 * same storage, layout and Vulkan format now hold one image. When the last
 * holder goes, the image is kept for SHARE_IDLE_SUBMITS submissions (previous-
 * frame buffers come back with a new serial) and dropped with its pool.
 * TOTK_VK_TEX_SHARE=0: one image per serial, as before. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "nvn_vk_fmutex.h"
#include "nvn_vk_internal.h"
/* Live switches live in nvn_vk_draw.c; tests that link this file without it read the environment. */
__attribute__((weak)) unsigned nvk_live_gen;
__attribute__((weak)) const char *nvk_live_getenv(const char *name) { return getenv(name); }

int android_vk_2d_view_of_3d;   /* set at device creation (android_gfx_bridge.c) */
int android_vk_local_read;      /* colour attachments may be read as input attachments (framebuffer fetch) */
/* TOTK_VK_FETCH_DEPTH=1 (off by default): decals that sample the depth buffer at their own pixel read it as an input
   attachment instead (nvn_vk_draw.c fetch_choose), so the pass need not split. Depth images then get input-attachment
   usage, which only happens with the switch on: on Adreno that usage can cost depth compression (UBWC). */
int nvk_fetch_depth_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_VK_FETCH_DEPTH"); on = android_vk_local_read && e && !strcmp(e, "1"); }
    return on;
}
#include "nvn_vk_format.h"
#include "nvn_vk_texture_stage.h"
#include "nvn_dirty.h"
#include "nvn_deswizzle.h"

#define MAX_VIEWS 128
#define UPLOAD_CHUNK_MIN (4ull << 20)
#define UPLOAD_CHUNK_MAX (32ull << 20)
#define UPLOAD_CACHE_LIMIT (128ull << 20)

typedef struct {
    uint32_t base_level, levels, base_layer, layers;
    VkImageViewType type;
    VkFormat format;
    VkComponentMapping components;
    VkImageView view;
    uint8_t input;           /* depth-only input-attachment view (nvk_image_depth_input_view) */
} ViewEntry;

struct NvkImage {
    uint64_t serial;
    VkImage image;
    VkDeviceMemory memory;
    VkFormat format;
    VkImageAspectFlags aspect;
    VkImageViewType view_type;
    VkExtent3D extent;
    uint32_t levels, layers;
    ViewEntry views[MAX_VIEWS];
    unsigned nviews;
    uint64_t stamp;          /* nvn_dirty generation of the uploaded pool contents */
    uint64_t clean_at;       /* nvn_dirty_now() when the pool bytes were last found unchanged */
    uint64_t upload_fence;   /* the submission of its last upload (refresh_once) */
    int uploaded, ever_uploaded, gpu_owned;
    /* Storage sharing: serials holding the image; keyed images sit in the storage table. */
    unsigned refs;
    int keyed;
    struct { uint64_t mem, size; uint32_t w, h, d, levels, target, format; } key;
    uint64_t idle_fence;     /* pending fence when the last holder left */
    tex_t desc;              /* the description it was created with (GPU -> memory writeback) */
    uint64_t gpu_seq, wb_seq, gpu_dirty_at;   /* last GPU write (order, dirty generation); last write written back */
    VkDeviceSize bytes;
    NvkImage *snext;
    VkFormat flist[2]; uint8_t nflist;   /* TOTK_VK_FORMAT_LIST: the formats views may have (0: any, MUTABLE_FORMAT as before) */
    struct TexRange *sub;                /* memory from a shared block (memory is then VK_NULL_HANDLE) */
};
typedef struct SerialRef { uint64_t serial; NvkImage *img; struct SerialRef *next; } SerialRef;

/* Images were all created MUTABLE_FORMAT without a format list, so an NVN view could have any compatible format.
   Turnip then leaves out UBWC compression (also for render targets and depth buffers: more memory bandwidth).
   TOTK_VK_FORMAT_LIST (on by default since 23 Oct: the only view outside a list in a Thor run was a compressed
   texture, and compressed images keep no list; TOTK_VK_FORMAT_LIST=0 = as before) gives every image a format list of its own format and, for 8-bit colour and
   compressed formats, its sRGB/UNORM partner (the only reinterpretation seen in TOTK so far; Turnip keeps UBWC for such
   a pair). A view outside that list is logged and gets the image's own format instead (TOTK would then show wrong
   colours there; the log names the formats so the list can be widened). Without the switch every view format that a
   format list would refuse is logged too, so a run shows whether the list is safe before it is turned on. */
static int format_list_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_VK_FORMAT_LIST"); on = !(e && !strcmp(e, "0")); VKLOG("format lists on images: %s", on ? "on" : "off (TOTK_VK_FORMAT_LIST=0)"); }
    return on;
}
static int nvn_vk_format_compressed(VkFormat f) {   /* BC, ETC2/EAC and ASTC block formats */
    return (f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK);
}
static VkFormat srgb_partner(VkFormat f) {
    static const VkFormat pairs[][2] = {
        {VK_FORMAT_R8_UNORM, VK_FORMAT_R8_SRGB}, {VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_SRGB},
        {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB}, {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB},
        {VK_FORMAT_A8B8G8R8_UNORM_PACK32, VK_FORMAT_A8B8G8R8_SRGB_PACK32},
        {VK_FORMAT_BC1_RGB_UNORM_BLOCK, VK_FORMAT_BC1_RGB_SRGB_BLOCK}, {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_SRGB_BLOCK},
        {VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_BC2_SRGB_BLOCK}, {VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC3_SRGB_BLOCK},
        {VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC7_SRGB_BLOCK},
        {VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK}, {VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK, VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK},
        {VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK},
    };
    for (size_t i = 0; i < sizeof pairs / sizeof *pairs; ++i) { if (pairs[i][0] == f) return pairs[i][1]; if (pairs[i][1] == f) return pairs[i][0]; }
    if (f >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK)   /* UNORM, SRGB alternate */
        return (VkFormat)((f - VK_FORMAT_ASTC_4x4_UNORM_BLOCK) % 2 ? f - 1 : f + 1);
    return VK_FORMAT_UNDEFINED;
}
/* The format a view of img may have: the requested one, or (outside a format list) the image's own. */
static uint64_t flist_outside;
static VkFormat view_format(const NvkImage *img, VkFormat want) {
    if (want == img->format || nvn_vk_format_compressed(img->format)) return want;   /* compressed images keep no list */
    VkFormat partner = srgb_partner(img->format);
    if (img->nflist ? want == img->flist[0] || (img->nflist > 1 && want == img->flist[1]) : want == partner) return want;
    static struct { VkFormat a, b; } seen[32]; static unsigned nseen;
    unsigned k = 0;
    while (k < nseen && (seen[k].a != img->format || seen[k].b != want)) ++k;
    ++flist_outside;
    if (k == nseen && nseen < 32) {
        seen[nseen++] = (typeof(seen[0])){img->format, want};
        VKLOG("view format %d of an image in format %d is outside its format list (%llu so far)%s", want, img->format, (unsigned long long)flist_outside,
            img->nflist ? ": the image's own format is used" : " (TOTK_VK_FORMAT_LIST=1 would refuse it)");
    }
    return img->nflist ? img->format : want;
}

#define BUCKETS 4096
#define SHARE_IDLE_SUBMITS 600
#define SHARE_IDLE_BYTES (768ull << 20)   /* above this, idle images go after SHARE_IDLE_SHORT submissions */
#define SHARE_IDLE_SHORT 8
static SerialRef *buckets[BUCKETS];
static NvkImage *storage_buckets[BUCKETS];
static fmutex lock = FMUTEX_INIT;   /* nvn_vk_fmutex.h: the replay thread takes it several times per bound texture */
static int share_enabled = -1;
static uint64_t share_live, share_revived, share_dropped_untracked, share_swept, share_pool_dropped, share_keyed;
static uint64_t idle_images; static VkDeviceSize idle_bytes;

typedef struct UploadChunk {
    VkBuffer buffer;
    VkDeviceMemory memory;
    uint8_t *map;
    VkDeviceSize size, used;
    uint64_t fence;
    struct UploadChunk *next;
} UploadChunk;
static UploadChunk *upload_chunks;
static pthread_mutex_t upload_lock = PTHREAD_MUTEX_INITIALIZER;
static VkDeviceSize upload_cached_bytes, upload_alignment;
static int upload_pool_enabled = -1;
static _Atomic uint64_t upload_first, upload_reuploads, upload_bytes, upload_cpu_ns;
static _Atomic uint64_t upload_reused, upload_fallback, upload_copies;
static _Atomic uint64_t discarded_uploads, discarded_bytes;

void nvk_upload_stats(uint64_t *first, uint64_t *reuploads, uint64_t *bytes) {
    if (first) *first = atomic_load(&upload_first);
    if (reuploads) *reuploads = atomic_load(&upload_reuploads);
    if (bytes) *bytes = atomic_load(&upload_bytes);
}
uint64_t nvk_upload_cpu_ns_take(void) { return atomic_exchange(&upload_cpu_ns, 0); }

static VkDeviceSize upload_gcd(VkDeviceSize a, VkDeviceSize b) {
    while (b) { VkDeviceSize t = a % b; a = b; b = t; }
    return a;
}
static VkDeviceSize upload_align(VkDeviceSize value) {
    VkDeviceSize a = upload_alignment;
    if (value > UINT64_MAX - (a - 1)) return UINT64_MAX;
    return ((value + a - 1) / a) * a;
}

/* All copies recorded for one submission may share a chunk at disjoint
 * offsets. A completed timeline fence is the only condition for rewinding it.
 * The cache has a fixed memory bound; an overflow takes the transient path. */
static int upload_reserve(VkDeviceSize bytes, VkBuffer *buffer, VkDeviceSize *offset, uint8_t **map) {
    if (!bytes || bytes > UPLOAD_CHUNK_MAX) return 0;
    uint64_t fence = nvk_pending_fence(), done = nvk_completed_fence();
    pthread_mutex_lock(&upload_lock);
    if (upload_pool_enabled < 0) {
        const char *e = getenv("TOTK_VK_UPLOAD_POOL");
        upload_pool_enabled = !(e && !strcmp(e, "0"));
    }
    if (!upload_pool_enabled) { pthread_mutex_unlock(&upload_lock); return 0; }
    if (!upload_alignment) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(nvk_phys, &p);
        VkDeviceSize a = p.limits.optimalBufferCopyOffsetAlignment;
        if (!a) a = 1;
        /* 48 is divisible by the block sizes of the staged formats, including
         * 3/6/12-byte RGB texels and 8/16-byte compressed blocks. */
        VkDeviceSize factor = 48 / upload_gcd(a, 48);
        if (a > UINT64_MAX / factor) nvk_die("texture upload alignment overflow");
        upload_alignment = a * factor;
    }
    UploadChunk *choice = NULL;
    VkDeviceSize chosen_offset = 0;
    for (UploadChunk *c = upload_chunks; c; c = c->next) {
        if (c->fence != fence && c->fence > done) continue;
        VkDeviceSize base = upload_align(c->fence == fence ? c->used : 0);
        if (base > c->size || bytes > c->size - base) continue;
        if (!choice || (c->fence == fence && choice->fence != fence) ||
            ((c->fence == fence) == (choice->fence == fence) && c->size - base < choice->size - chosen_offset)) {
            choice = c; chosen_offset = base;
        }
    }
    if (!choice) {
        VkDeviceSize size = UPLOAD_CHUNK_MIN;
        while (size < bytes) size *= 2;
        if (upload_cached_bytes + size > UPLOAD_CACHE_LIMIT) { pthread_mutex_unlock(&upload_lock); return 0; }
        choice = calloc(1, sizeof *choice);
        if (!choice) nvk_die("texture upload chunk allocation");
        VkBufferCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
            .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
        VKCHECK(vkCreateBuffer(nvk_dev, &ci, NULL, &choice->buffer), "create pooled texture upload buffer");
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(nvk_dev, choice->buffer, &req);
        if (req.size > UPLOAD_CACHE_LIMIT - upload_cached_bytes) {
            vkDestroyBuffer(nvk_dev, choice->buffer, NULL);
            free(choice);
            pthread_mutex_unlock(&upload_lock);
            return 0;
        }
        VkMemoryAllocateInfo mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
            .memoryTypeIndex = nvk_memory_type(req.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        VKCHECK(vkAllocateMemory(nvk_dev, &mi, NULL, &choice->memory), "allocate pooled texture upload memory");
        VKCHECK(vkBindBufferMemory(nvk_dev, choice->buffer, choice->memory, 0), "bind pooled texture upload buffer");
        VKCHECK(vkMapMemory(nvk_dev, choice->memory, 0, VK_WHOLE_SIZE, 0, (void **)&choice->map), "map pooled texture upload memory");
        choice->size = size;
        upload_cached_bytes += req.size;
        choice->next = upload_chunks; upload_chunks = choice;
        chosen_offset = 0;
        VKLOG("texture upload pool: %llu MiB cached", (unsigned long long)(upload_cached_bytes >> 20));
    } else atomic_fetch_add(&upload_reused, 1);
    choice->fence = fence;
    choice->used = chosen_offset + bytes;
    *buffer = choice->buffer;
    *offset = chosen_offset;
    *map = choice->map + chosen_offset;
    pthread_mutex_unlock(&upload_lock);
    return 1;
}

static SerialRef **slot_of(uint64_t serial) {
    SerialRef **p = &buckets[(serial * 0x9e3779b97f4a7c15ull) >> 52];
    while (*p && (*p)->serial != serial) p = &(*p)->next;
    return p;
}
/* lock held */
static void serial_put(uint64_t serial, NvkImage *img) {
    SerialRef *r = malloc(sizeof *r);
    if (!r) nvk_die("image serial table allocation");
    *r = (SerialRef){serial, img, buckets[(serial * 0x9e3779b97f4a7c15ull) >> 52]};
    buckets[(serial * 0x9e3779b97f4a7c15ull) >> 52] = r;
    img->refs++;
}

static int share_on(void) {
    if (share_enabled < 0) { const char *e = getenv("TOTK_VK_TEX_SHARE"); share_enabled = !(e && !strcmp(e, "0")); }
    return share_enabled;
}
static int storage_key(const tex_t *t, VkFormat format, NvkImage *out) {
    if (!share_on() || !t->mem || !t->L.size || t->d.samples > 1 || t->d.target == 10) return 0;
    memset(&out->key, 0, sizeof out->key);
    out->key.mem = (uint64_t)(uintptr_t)t->mem; out->key.size = t->L.size;
    out->key.w = t->d.width ? t->d.width : 1; out->key.h = t->d.height ? t->d.height : 1; out->key.d = t->d.depth ? t->d.depth : 1;
    out->key.levels = t->d.levels ? t->d.levels : 1; out->key.target = (uint32_t)t->d.target; out->key.format = (uint32_t)format;
    return 1;
}
static NvkImage **storage_slot(const NvkImage *k) {
    uint64_t h = k->key.mem * 0x9e3779b97f4a7c15ull ^ k->key.size ^ ((uint64_t)k->key.w << 32 | k->key.h) ^ k->key.format;
    NvkImage **p = &storage_buckets[(h * 0xff51afd7ed558ccdull) >> 52];
    while (*p && memcmp(&(*p)->key, &k->key, sizeof k->key)) p = &(*p)->snext;
    return p;
}
/* lock held: the image leaves the storage table (it may still have holders). */
static void storage_unlink(NvkImage *img) {
    if (!img->keyed) return;
    NvkImage **p = storage_slot(img);
    if (*p == img) *p = img->snext;
    img->snext = NULL; img->keyed = 0;
    if (!img->refs) { idle_images--; idle_bytes -= img->bytes; }
}
/* ---------- Texture memory in shared blocks ----------
 * Every texture had its own vkAllocateMemory (and vkFreeMemory when dropped): entering an area creates hundreds at once,
 * ~0.24 ms each on the replay thread (freeze analysis 23 Oct: 100-150 ms frames). Images up to TEX_SUB_MAX bytes now
 * take a range of a TEX_BLOCK block of their memory type (first fit, aligned, freed ranges merged). A range is handed
 * back only after the image is destroyed, i.e. after the GPU finished with it (nvk_defer_image_release). An empty
 * block beyond the first of its type is freed. Images that require a dedicated allocation keep their own memory.
 * TOTK_VK_TEX_SUBALLOC=0: one allocation per texture, as before. */
#define TEX_BLOCK (64ull << 20)
#define TEX_SUB_MAX (8ull << 20)
typedef struct TexFree { VkDeviceSize off, size; struct TexFree *next; } TexFree;
typedef struct TexBlock { VkDeviceMemory memory; uint32_t type; VkDeviceSize used; TexFree *free; struct TexBlock *next; } TexBlock;
typedef struct TexRange { TexBlock *block; VkDeviceSize off, size; } TexRange;
static TexBlock *tex_blocks; static pthread_mutex_t tex_mem_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t tex_sub_allocs, tex_sub_blocks, tex_dedicated;
static int tex_suballoc_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_VK_TEX_SUBALLOC"); on = !(e && !strcmp(e, "0")); }
    return on;
}
static int tex_block_take(TexBlock *b, VkDeviceSize size, VkDeviceSize align, VkDeviceSize *off) {
    for (TexFree **pp = &b->free; *pp; pp = &(*pp)->next) {
        TexFree *f = *pp;
        VkDeviceSize start = (f->off + align - 1) / align * align;
        if (start + size > f->off + f->size) continue;
        VkDeviceSize head = start - f->off, tail = f->off + f->size - (start + size);
        if (head && tail) {   /* split in two */
            TexFree *t = malloc(sizeof *t); if (!t) nvk_die("texture memory free list");
            t->off = start + size; t->size = tail; t->next = f->next; f->size = head; f->next = t;
        } else if (head) f->size = head;
        else if (tail) { f->off = start + size; f->size = tail; }
        else { *pp = f->next; free(f); }
        b->used += size; *off = start;
        return 1;
    }
    return 0;
}
static TexRange *tex_mem_alloc(uint32_t type, VkDeviceSize size, VkDeviceSize align) {
    pthread_mutex_lock(&tex_mem_lock);
    VkDeviceSize off = 0; TexBlock *b = tex_blocks;
    for (; b; b = b->next) if (b->type == type && tex_block_take(b, size, align, &off)) break;
    if (!b) {
        b = calloc(1, sizeof *b); TexFree *f = calloc(1, sizeof *f);
        if (!b || !f) nvk_die("texture memory block");
        VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = TEX_BLOCK, .memoryTypeIndex = type};
        if (vkAllocateMemory(nvk_dev, &mai, NULL, &b->memory) != VK_SUCCESS) { free(b); free(f); pthread_mutex_unlock(&tex_mem_lock); return NULL; }
        b->type = type; f->size = TEX_BLOCK; b->free = f; b->next = tex_blocks; tex_blocks = b; tex_sub_blocks++;
        if (!tex_block_take(b, size, align, &off)) nvk_die("texture memory block too small");
        VKLOG("texture memory: block %llu of %llu MiB (type %u)", (unsigned long long)tex_sub_blocks, (unsigned long long)(TEX_BLOCK >> 20), type);
    }
    TexRange *r = malloc(sizeof *r); if (!r) nvk_die("texture memory range");
    r->block = b; r->off = off; r->size = size; tex_sub_allocs++;
    pthread_mutex_unlock(&tex_mem_lock);
    return r;
}
static void tex_mem_free(void *ctx) {   /* after the image was destroyed */
    TexRange *r = ctx; TexBlock *b = r->block;
    pthread_mutex_lock(&tex_mem_lock);
    TexFree **pp = &b->free;
    while (*pp && (*pp)->off < r->off) pp = &(*pp)->next;
    TexFree *n = malloc(sizeof *n); if (!n) nvk_die("texture memory free list");
    n->off = r->off; n->size = r->size; n->next = *pp; *pp = n;
    b->used -= r->size;
    for (TexFree *f = b->free; f && f->next; ) {   /* merge neighbours */
        if (f->off + f->size == f->next->off) { TexFree *d = f->next; f->size += d->size; f->next = d->next; free(d); }
        else f = f->next;
    }
    if (!b->used) {   /* empty: free it unless it is the only block of its type */
        unsigned same = 0; for (TexBlock *o = tex_blocks; o; o = o->next) same += o->type == b->type;
        if (same > 1) {
            TexBlock **bp = &tex_blocks; while (*bp != b) bp = &(*bp)->next; *bp = b->next;
            for (TexFree *f = b->free; f; ) { TexFree *d = f; f = f->next; free(d); }
            vkFreeMemory(nvk_dev, b->memory, NULL); free(b); tex_sub_blocks--;
        }
    }
    pthread_mutex_unlock(&tex_mem_lock);
    free(r);
}

static void destroy(NvkImage *img) {
    VkImageView *views = malloc((img->nviews ? img->nviews : 1) * sizeof *views);
    if (!views) nvk_die("deferred view list");
    for (unsigned i = 0; i < img->nviews; ++i) views[i] = img->views[i].view;
    if (img->sub) nvk_defer_image_release(img->image, VK_NULL_HANDLE, views, img->nviews, nvk_pending_fence(), tex_mem_free, img->sub);
    else nvk_defer_image(img->image, img->memory, views, img->nviews, nvk_pending_fence());
    free(views);
    free(img);
}
static void share_log(void) {
    static uint64_t said;
    uint64_t n = share_live + share_revived;
    if (n && (said < 6 || n >= said * 4)) {
        said = said < 6 ? said + 1 : n;
        VKLOG("texture storage sharing: %llu images keyed, %llu serials joined a live image, %llu took over a kept one; "
            "kept %llu (%.1f MiB), swept %llu, dropped with their pool %llu, untracked not reused %llu",
            (unsigned long long)share_keyed, (unsigned long long)share_live, (unsigned long long)share_revived,
            (unsigned long long)idle_images, (double)idle_bytes / 1048576.0, (unsigned long long)share_swept,
            (unsigned long long)share_pool_dropped, (unsigned long long)share_dropped_untracked);
    }
}

static VkImageAspectFlags aspect_of(VkFormat f) {
    switch (f) {
    case VK_FORMAT_D16_UNORM: case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D32_SFLOAT: return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_S8_UINT: return VK_IMAGE_ASPECT_STENCIL_BIT;
    case VK_FORMAT_D16_UNORM_S8_UINT: case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    default: return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

/* ---------- Texel staging off the replay thread ----------
 * Uploads used to de-swizzle into a zeroed heap copy, then copy that into the upload buffer, all on the replay
 * thread (2-3.5 ms per frame in the open world). Now the texels go straight into the mapped upload memory, and a
 * texture of UPLOAD_SPLIT_MIN bytes or more is cut into row ranges of about UPLOAD_TASK_BYTES that
 * TOTK_VK_UPLOAD_THREADS helper threads (default 3; 0 = all on the replay thread) and the replay thread share.
 * The replay thread waits for all of them before it records the copy, so ordering is as before. */
#define UPLOAD_SPLIT_MIN (256u << 10)
#define UPLOAD_TASK_BYTES (64u << 10)
#define UPLOAD_TASKS 512
typedef struct { uint32_t region, r0, r1; } FillTask;
static struct {
    pthread_mutex_t lock; pthread_cond_t work, idle;
    const nvn_texdesc *d; const void *src; const NvnVkTextureStage *plan; uint8_t *dst;
    FillTask task[UPLOAD_TASKS]; _Atomic unsigned ntask;
    _Atomic unsigned next, done; _Atomic int failed;
    uint64_t generation; unsigned busy;   /* helpers still inside the current job */
} fill = {.lock = PTHREAD_MUTEX_INITIALIZER, .work = PTHREAD_COND_INITIALIZER, .idle = PTHREAD_COND_INITIALIZER};
static pthread_mutex_t fill_owner = PTHREAD_MUTEX_INITIALIZER;   /* one split job at a time */
static int fill_threads = -1;
static _Atomic uint64_t upload_split, upload_split_tasks;
static void fill_run(void) {
    for (unsigned k; (k = atomic_fetch_add(&fill.next, 1)) < fill.ntask; ) {
        const FillTask *t = &fill.task[k];
        if (nvn_vk_texture_stage_fill(fill.d, fill.src, fill.plan, t->region, t->r0, t->r1, fill.dst, NULL) != NVN_VK_STAGE_OK)
            atomic_store(&fill.failed, 1);
        atomic_fetch_add(&fill.done, 1);
    }
}
static void *fill_worker(void *unused) {
    (void)unused;
    uint64_t seen = 0;
    pthread_mutex_lock(&fill.lock);
    for (;;) {
        while (fill.generation == seen) pthread_cond_wait(&fill.work, &fill.lock);
        seen = fill.generation; fill.busy++;
        pthread_mutex_unlock(&fill.lock);
        fill_run();
        pthread_mutex_lock(&fill.lock);
        if (!--fill.busy) pthread_cond_broadcast(&fill.idle);
    }
    return NULL;
}
static void fill_start(void) {
    const char *e = getenv("TOTK_VK_UPLOAD_THREADS");
    fill_threads = e && *e ? atoi(e) : 3;
    if (fill_threads < 0) fill_threads = 0;
    if (fill_threads > 7) fill_threads = 7;
    for (int i = 0; i < fill_threads; ++i) {
        pthread_t th;
        if (pthread_create(&th, NULL, fill_worker, NULL)) { fill_threads = i; break; }
        pthread_detach(th);
    }
    VKLOG("texture staging: %d helper threads (TOTK_VK_UPLOAD_THREADS)", fill_threads);
}
static pthread_once_t fill_once = PTHREAD_ONCE_INIT;
/* All of plan's regions into dst; 0 on success. */
static int stage_fill_serial(const tex_t *t, const NvnVkTextureStage *plan, uint8_t *dst) {
    for (uint32_t i = 0; i < plan->region_count; ++i)
        if (nvn_vk_texture_stage_fill(&t->d, t->mem, plan, i, 0, UINT32_MAX, dst, NULL) != NVN_VK_STAGE_OK) return -1;
    return 0;
}
static _Atomic uint64_t upload_split_refused;
static int stage_fill(const tex_t *t, const NvnVkTextureStage *plan, uint8_t *dst) {
    pthread_once(&fill_once, fill_start);
    /* Every region is at least one task, so a texture with very many regions (long arrays x mips) stays on the
     * replay thread; with at most half the table in regions the size loop below always ends. */
    int split = fill_threads > 0 && plan->data_size >= UPLOAD_SPLIT_MIN && plan->region_count <= UPLOAD_TASKS / 2
        && !pthread_mutex_trylock(&fill_owner);
    if (!split) return stage_fill_serial(t, plan, dst);
    /* Row ranges of about UPLOAD_TASK_BYTES (more when the texture would need more than UPLOAD_TASKS of them; a
     * range can come out at half the target size, hence the factor 2). */
    uint64_t task_bytes = UPLOAD_TASK_BYTES;
    while (2 * (plan->data_size / task_bytes) + plan->region_count > UPLOAD_TASKS) task_bytes *= 2;
    unsigned n = 0;
    for (uint32_t i = 0; i < plan->region_count; ++i) {
        uint32_t rows = 0;
        if (nvn_vk_texture_stage_fill(&t->d, t->mem, plan, i, 0, 0, dst, &rows) != NVN_VK_STAGE_OK) { pthread_mutex_unlock(&fill_owner); return -1; }
        if (!rows) continue;
        uint64_t end = i + 1 < plan->region_count ? plan->regions[i + 1].bufferOffset : plan->data_size;
        uint64_t row_bytes = (end - plan->regions[i].bufferOffset) / rows + 1;
        uint32_t step = (uint32_t)(task_bytes / row_bytes); if (!step) step = 1;
        for (uint32_t r = 0; r < rows; r += step) {
            if (n == UPLOAD_TASKS) {   /* should not happen after the sizing above; never worth stopping for */
                pthread_mutex_unlock(&fill_owner);
                atomic_fetch_add(&upload_split_refused, 1);
                return stage_fill_serial(t, plan, dst);
            }
            fill.task[n++] = (FillTask){i, r, rows - r < step ? rows : r + step};
        }
    }
    pthread_mutex_lock(&fill.lock);
    fill.d = &t->d; fill.src = t->mem; fill.plan = plan; fill.dst = dst; fill.ntask = n;
    atomic_store(&fill.next, 0); atomic_store(&fill.done, 0); atomic_store(&fill.failed, 0);
    fill.generation++;
    pthread_cond_broadcast(&fill.work);
    pthread_mutex_unlock(&fill.lock);
    fill_run();
    pthread_mutex_lock(&fill.lock);   /* helpers still copying, or about to look at the next index, finish first */
    while (fill.busy || atomic_load(&fill.done) < n) pthread_cond_wait(&fill.idle, &fill.lock);
    pthread_mutex_unlock(&fill.lock);
    int failed = atomic_load(&fill.failed);
    pthread_mutex_unlock(&fill_owner);
    atomic_fetch_add(&upload_split, 1); atomic_fetch_add(&upload_split_tasks, n);
    return failed ? -1 : 0;
}

/* ---------- Re-uploads of only what changed ----------
 * TOTK streams textures in piece by piece (a mip level, an array layer) into storage it already uses, and every piece
 * made the whole texture stale: up to 38 re-uploads, 500 MB and 165 ms in one frame on the Thor (22 Oct). A
 * re-upload now stages only the regions (level x layer) whose source storage changed since the last upload; the
 * rest of the image keeps its contents. TOTK_VK_PARTIAL_UPLOAD=0 = always the whole texture. */
static _Atomic uint64_t partial_regions_kept, partial_regions_total, partial_bytes_saved, partial_none_dirty;
static int partial_on(void) {
    static int on = -1;
    static unsigned live_TOTK_VK_PARTIAL_UPLOAD; if (on < 0 || live_TOTK_VK_PARTIAL_UPLOAD != nvk_live_gen) { live_TOTK_VK_PARTIAL_UPLOAD = nvk_live_gen; const char *e = nvk_live_getenv("TOTK_VK_PARTIAL_UPLOAD"); on = !(e && !strcmp(e, "0")); }
    return on;
}
/* For 2D block-linear textures, a block row (8 << tileH block rows) occupies one contiguous source range.
   Check that range against the existing conservative dirty tracker and upload only dirty full-width strips.
   No hashes, skipped writes or delayed visibility. 3D/linear/unusual shapes keep the old whole-subresource path.
   TOTK_VK_UPLOAD_STRIPES=0 disables this; metadata is capped to avoid pathological command counts. */
#define UPLOAD_STRIP_LIMIT 4096
static _Atomic uint64_t strip_checks, strip_regions, strip_saved;
static int strips_on(void) {
    static int on = -1;
    static unsigned live_TOTK_VK_UPLOAD_STRIPES; if (on < 0 || live_TOTK_VK_UPLOAD_STRIPES != nvk_live_gen) { live_TOTK_VK_UPLOAD_STRIPES = nvk_live_gen; const char *e = nvk_live_getenv("TOTK_VK_UPLOAD_STRIPES"); on = !(e && !strcmp(e, "0")); }
    return on;
}
static int strip_geometry(const tex_t *t, const VkBufferImageCopy *r, uint32_t *row_bytes, uint32_t *rows,
                          uint64_t *base, uint32_t *step, uint64_t *source_step, uint32_t *block_height) {
    if (nvn_tex_is_linear(&t->d) || t->d.target == NVN_TEXTURE_TARGET_3D || r->imageExtent.depth != 1
        || r->imageOffset.x || r->imageOffset.y || r->imageOffset.z || r->imageSubresource.layerCount != 1) return 0;
    uint32_t slices, bpb; unsigned th, td; int bw, bh;
    if (!nvn_fmt_bpb(t->d.format, &bw, &bh) || bh < 1
        || nvn_deswizzle_geom(&t->d, r->imageSubresource.mipLevel, r->imageSubresource.baseArrayLayer,
                              row_bytes, rows, &slices, base, &th, &td, &bpb) || slices != 1 || td || th > 5) return 0;
    *step = 8u << th; *block_height = (uint32_t)bh;
    *source_step = ((uint64_t)*row_bytes + 63) / 64 * (512ull << th);
    uint64_t bands = ((uint64_t)*rows + *step - 1) / *step;
    return *base <= t->L.size && bands <= (t->L.size - *base) / *source_step;
}
static int restage_strips(NvnVkTextureStage *s, const tex_t *t, uint64_t since) {
    if (!strips_on() || s->data_size < UPLOAD_SPLIT_MIN || s->image_type != VK_IMAGE_TYPE_2D || nvn_tex_is_linear(&t->d)) return 0;
    uint64_t capacity = 0;
    for (uint32_t i = 0; i < s->region_count; ++i) {
        uint32_t row, rows, step, bh; uint64_t base, source_step;
        if (!strip_geometry(t, &s->regions[i], &row, &rows, &base, &step, &source_step, &bh)) return 0;
        capacity += ((uint64_t)rows + step - 1) / step;
        if (capacity > UPLOAD_STRIP_LIMIT) return 0;
    }
    VkBufferImageCopy *regions = calloc((size_t)capacity, sizeof *regions);
    if (!regions) return 0;   /* the existing plan is still valid */
    uint32_t n = 0; VkDeviceSize off = 0;
    int bw, bh; unsigned bpb = (unsigned)nvn_fmt_bpb(t->d.format, &bw, &bh);
    /* lcm(4,bpb), without assuming that RGB formats have a power-of-two block size. */
    uint64_t alignment = 4ull * bpb / upload_gcd(4, bpb);
    for (uint32_t i = 0; i < s->region_count; ++i) {
        const VkBufferImageCopy *r = &s->regions[i];
        uint32_t row, rows, step, block_height; uint64_t base, source_step;
        if (!strip_geometry(t, r, &row, &rows, &base, &step, &source_step, &block_height)) { free(regions); return 0; }
        for (uint32_t y = 0; y < rows; ) {
            uint32_t end = rows - y < step ? rows : y + step;
            atomic_fetch_add(&strip_checks, 1);
            if (!nvn_dirty_since((const uint8_t *)t->mem + base + (uint64_t)(y / step) * source_step,
                                  (size_t)source_step, since)) { y = end; continue; }
            /* Adjacent dirty bands can be submitted as a single rectangle. */
            while (end < rows) {
                atomic_fetch_add(&strip_checks, 1);
                if (!nvn_dirty_since((const uint8_t *)t->mem + base + (uint64_t)(end / step) * source_step,
                                      (size_t)source_step, since)) break;
                end = rows - end < step ? rows : end + step;
            }
            off += (alignment - off % alignment) % alignment;
            VkBufferImageCopy out = *r; out.bufferOffset = off;
            out.imageOffset.y = (int32_t)(y * block_height);
            uint32_t bottom = end * block_height;
            if (bottom > r->imageExtent.height) bottom = r->imageExtent.height;
            out.imageExtent.height = bottom - (uint32_t)out.imageOffset.y;
            regions[n++] = out; off += (uint64_t)row * (end - y); y = end;
        }
    }
    uint64_t saved = s->data_size > off ? s->data_size - off : 0;
    atomic_fetch_add(&strip_saved, saved); atomic_fetch_add(&strip_regions, n);
    atomic_fetch_add(&partial_regions_total, s->region_count); atomic_fetch_add(&partial_regions_kept, n);
    atomic_fetch_add(&partial_bytes_saved, saved);
    free(s->regions); s->regions = regions; s->region_count = n; s->data_size = (size_t)off;
    return 1;
}
/* Keeps the plan's regions whose source changed since `since`, packed from offset 0 (sizes as planned). */
static void restage_dirty(NvnVkTextureStage *s, const tex_t *t, uint64_t since) {
    if (restage_strips(s, t, since)) return;
    int is_3d = s->image_type == VK_IMAGE_TYPE_3D;
    uint32_t n = 0; VkDeviceSize off = 0;
    const uint8_t *mem = t->mem;
    for (uint32_t i = 0; i < s->region_count; ++i) {
        VkBufferImageCopy r = s->regions[i];
        VkDeviceSize size = (i + 1 < s->region_count ? s->regions[i + 1].bufferOffset : s->data_size) - r.bufferOffset;
        uint32_t level = r.imageSubresource.mipLevel, layer = is_3d ? 0 : r.imageSubresource.baseArrayLayer;
        uint64_t a = (uint64_t)layer * t->L.layer_size + nvn_tex_level_offset(&t->d, level);
        uint64_t b = level + 1 < s->mip_levels ? (uint64_t)layer * t->L.layer_size + nvn_tex_level_offset(&t->d, level + 1)
            : (uint64_t)(layer + 1) * t->L.layer_size;
        int dirty = a >= b || b > t->L.size || nvn_dirty_since(mem + a, (size_t)(b - a), since);   /* unknown shape: keep */
        if (!dirty) continue;
        r.bufferOffset = off; s->regions[n++] = r; off += size;
    }
    atomic_fetch_add(&partial_regions_total, s->region_count); atomic_fetch_add(&partial_regions_kept, n);
    atomic_fetch_add(&partial_bytes_saved, s->data_size - off);
    s->region_count = n; s->data_size = off;
}

/* Time and bytes of texture uploads (freeze logger, nvn_vk_backend.c mtl_set_frame). */
_Atomic uint64_t nvk_upload_ns, nvk_upload_n, nvk_upload_bytes;
static void upload_body(NvkImage *img, const tex_t *t, VkCommandBuffer cb);
static void upload(NvkImage *img, const tex_t *t, VkCommandBuffer cb) {
    uint64_t t0 = nvk_now_ns();
    upload_body(img, t, cb);
    img->upload_fence = nvk_pending_fence();
    atomic_fetch_add(&nvk_upload_ns, nvk_now_ns() - t0);
}
/* TOTK streams into some big textures while it draws with them: 23 Oct, Thor: one 5120x5120 BC4 texture (17 MiB)
 * was refreshed 18 times in one 192 ms frame, since every draw sampling it found its pool bytes changed again (54
 * refreshes, 417 MiB that frame). A texture of at least TOTK_VK_REFRESH_ONCE_MIN bytes (default 4 MiB) is refreshed
 * at most once per submission; later writes reach it with the next submission (its stamp stays at the upload, so
 * that refresh covers them). Smaller ones, which the game may rewrite between draws, refresh as before.
 * TOTK_VK_REFRESH_ONCE=0: every draw as before. */
static _Atomic uint64_t refresh_deferred;
static int refresh_wait(const NvkImage *img, const tex_t *t) {
    static int on = -1; static uint64_t min; static unsigned g;
    if (on < 0 || g != nvk_live_gen) {
        g = nvk_live_gen;
        const char *e = nvk_live_getenv("TOTK_VK_REFRESH_ONCE"); on = !(e && !strcmp(e, "0"));
        e = nvk_live_getenv("TOTK_VK_REFRESH_ONCE_MIN"); min = e && *e ? strtoull(e, NULL, 0) : 4u << 20;
    }
    if (!on || !img->uploaded || t->L.size < min || img->upload_fence != nvk_pending_fence()) return 0;
    uint64_t n = atomic_fetch_add(&refresh_deferred, 1) + 1;
    if (n == 1 || !(n % 256)) VKLOG("texture refresh: %llu deferred to the next submission (big textures written while in use, e.g. serial %llu %ux%u fmt %d)",
        (unsigned long long)n, (unsigned long long)t->serial, t->d.width, t->d.height, t->d.format);
    return 1;
}
static uint8_t *staging_transient(VkDeviceSize size, VkBuffer *buffer, VkDeviceMemory *memory) {
    atomic_fetch_add(&upload_fallback, 1);
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    VKCHECK(vkCreateBuffer(nvk_dev, &bci, NULL, buffer), "create texture staging buffer");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(nvk_dev, *buffer, &req);
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
        .memoryTypeIndex = nvk_memory_type(req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VKCHECK(vkAllocateMemory(nvk_dev, &mai, NULL, memory), "allocate texture staging memory");
    VKCHECK(vkBindBufferMemory(nvk_dev, *buffer, *memory, 0), "bind texture staging memory");
    uint8_t *map;
    VKCHECK(vkMapMemory(nvk_dev, *memory, 0, VK_WHOLE_SIZE, 0, (void **)&map), "map texture staging memory");
    return map;
}
/* Zeros for a texture whose pool bytes cannot be staged. vkCmdClearColorImage is not allowed on block-compressed
 * formats (VUID-vkCmdClearColorImage-image-00007): 23 Oct, Thor: a cleared 64x64 BC4 3D texture stalled the GPU for
 * ~340 ms in nearly every run, and that submission's fence then signalled with its occlusion queries never written.
 * Compressed images get zero blocks copied in instead. TOTK_VK_ZERO_COPY=0: cleared as before. */
static void zero_image(NvkImage *img, const tex_t *t, VkCommandBuffer cb) {
    static int on = -1;
    static unsigned live_TOTK_VK_ZERO_COPY; if (on < 0 || live_TOTK_VK_ZERO_COPY != nvk_live_gen) { live_TOTK_VK_ZERO_COPY = nvk_live_gen; const char *e = nvk_live_getenv("TOTK_VK_ZERO_COPY"); on = !(e && !strcmp(e, "0")); }
    int bw = 0, bh = 0, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    nvk_barrier(cb);
    if (!on || !nvn_vk_format_compressed(img->format) || bpb <= 0 || bw <= 0 || bh <= 0 || img->levels > 32) {
        VkClearColorValue zero = {{0}};
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, img->levels, 0, img->layers};
        vkCmdClearColorImage(cb, img->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
        nvk_barrier(cb);
        return;
    }
    VkBufferImageCopy regions[32];
    VkDeviceSize size = 0;
    for (uint32_t level = 0; level < img->levels; ++level) {
        VkExtent2D e = nvk_image_level_extent(img, level);
        uint32_t d = img->extent.depth >> level ? img->extent.depth >> level : 1;
        regions[level] = (VkBufferImageCopy){.bufferOffset = size,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, img->layers}, .imageExtent = {e.width, e.height, d}};
        size += (VkDeviceSize)((e.width + bw - 1) / bw) * ((e.height + bh - 1) / bh) * (uint32_t)bpb * d * img->layers;
        size = (size + 15) & ~(VkDeviceSize)15;
    }
    VkBuffer buffer; VkDeviceMemory memory = VK_NULL_HANDLE; VkDeviceSize base = 0; uint8_t *map;
    int pooled = upload_reserve(size, &buffer, &base, &map);
    if (!pooled) map = staging_transient(size, &buffer, &memory);
    memset(map, 0, (size_t)size);
    if (!pooled) vkUnmapMemory(nvk_dev, memory);
    for (uint32_t level = 0; level < img->levels; ++level) regions[level].bufferOffset += base;
    vkCmdCopyBufferToImage(cb, buffer, img->image, VK_IMAGE_LAYOUT_GENERAL, img->levels, regions);
    if (!pooled) nvk_defer_buffer(buffer, memory, nvk_pending_fence());
    nvk_barrier(cb);
}
static void upload_body(NvkImage *img, const tex_t *t, VkCommandBuffer cb) {
    if (img->aspect != VK_IMAGE_ASPECT_COLOR_BIT || !t->mem || !t->L.size) return;
    uint64_t started = nvk_now_ns();
    /* Write-protect before the stamp and the copy, not after. An unprotected page records no further writes: TOTK's
       streaming threads write the terrain texture cache (WriteTexels) while the replay uploads it, and what they wrote
       between the stamp and a later protect was neither copied nor seen as newer, so stale blocks stayed on the GPU
       for good (the "korrels"; 23 Oct, Thor: a forced whole re-upload removed them at once). Stamp, then protect,
       then copy: a write before the protect is in the copy, and one after it faults with a generation above the
       stamp, so the next use refreshes it. */
    uint64_t stamp = nvn_dirty_now();
    if (nvn_dirty_tracked(t->mem, (size_t)t->L.size)) nvn_dirty_protect(t->mem, (size_t)t->L.size);
    NvnVkTextureStage stage;
    NvnVkTextureStageResult r = nvn_vk_texture_stage_plan(&t->d, t->mem, (size_t)t->L.size, &stage);
    if (r != NVN_VK_STAGE_OK) {
        static int said;
        if (said++ < 16) VKLOG("texture %llu (%ux%ux%u levels %u fmt %#x target %d flags %#x packaged block %d) upload unsupported: staging result %d; initialized to zero",
            (unsigned long long)t->serial, t->d.width, t->d.height, t->d.depth, t->d.levels, t->d.format, t->d.target, t->d.flags, t->d.pkg_block_h, r);
        zero_image(img, t, cb);
        img->stamp = stamp; img->uploaded = 1;
        atomic_fetch_add(&upload_cpu_ns, nvk_now_ns() - started);
        return;
    }
    if (stage.format != img->format) {
        VKLOG("texture %llu staged as format %d but the image is %d", (unsigned long long)t->serial, stage.format, img->format);
        nvk_die("texture staging format mismatch");
    }
    if (!stage.data_size || !stage.region_count) nvk_die("empty staged texture upload");
    if (img->uploaded && partial_on()) {
        restage_dirty(&stage, t, img->stamp);
        if (!stage.region_count) {   /* only padding between the regions changed */
            atomic_fetch_add(&partial_none_dirty, 1);
            nvn_vk_texture_stage_destroy(&stage);
            img->stamp = stamp;
            if (nvn_dirty_tracked(t->mem, (size_t)t->L.size)) nvn_dirty_protect(t->mem, (size_t)t->L.size);
            atomic_fetch_add(&upload_cpu_ns, nvk_now_ns() - started);
            return;
        }
    }
    uint64_t plan_done = nvk_now_ns();
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize base = 0;
    uint8_t *map = NULL;
    int pooled = upload_reserve(stage.data_size, &buffer, &base, &map);
    if (!pooled) map = staging_transient(stage.data_size, &buffer, &memory);
    uint64_t reserve_done = nvk_now_ns();
    if (stage_fill(t, &stage, map)) {   /* as an unsupported texture: zeros (the staging space is simply not used) */
        static int said;
        if (said++ < 16) VKLOG("texture %llu (%ux%u fmt %#x target %d): de-swizzling failed; initialized to zero",
            (unsigned long long)t->serial, t->d.width, t->d.height, t->d.format, t->d.target);
        if (!pooled) { vkUnmapMemory(nvk_dev, memory); nvk_defer_buffer(buffer, memory, nvk_pending_fence()); }
        nvn_vk_texture_stage_destroy(&stage);
        zero_image(img, t, cb);
        img->stamp = stamp; img->uploaded = 1;
        atomic_fetch_add(&upload_cpu_ns, nvk_now_ns() - started);
        return;
    }
    uint64_t fill_done = nvk_now_ns();
    if (!pooled) vkUnmapMemory(nvk_dev, memory);
    uint32_t copied_regions = stage.region_count;
    int reupload = img->ever_uploaded;
    for (uint32_t i = 0; i < stage.region_count; ++i) {
        stage.regions[i].bufferOffset += base;
        stage.regions[i].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    }
    vkCmdCopyBufferToImage(cb, buffer, img->image, VK_IMAGE_LAYOUT_GENERAL, stage.region_count, stage.regions);
    size_t copied = stage.data_size;
    /* Count actual staged bytes, not the full NVN allocation when only some strips/mips were copied. */
    atomic_fetch_add(&nvk_upload_n, 1); atomic_fetch_add(&nvk_upload_bytes, copied);
    nvn_vk_texture_stage_destroy(&stage);
    if (!pooled) nvk_defer_buffer(buffer, memory, nvk_pending_fence());
    /* Later CPU writes to the storage make this upload stale (nvk_image_refresh). */
    img->stamp = stamp; img->uploaded = 1;
    if (img->ever_uploaded) atomic_fetch_add(&upload_reuploads, 1);
    else { img->ever_uploaded = 1; atomic_fetch_add(&upload_first, 1); }
    atomic_fetch_add(&upload_bytes, copied);
    uint64_t copy_done = nvk_now_ns();
    if (nvn_dirty_tracked(t->mem, (size_t)t->L.size)) nvn_dirty_protect(t->mem, (size_t)t->L.size);
    uint64_t ended = nvk_now_ns();
    atomic_fetch_add(&upload_cpu_ns, ended - started);
    static unsigned slow_said;
    if (ended - started >= 4000000ull && slow_said < 128) {
        slow_said++;
        VKLOG("slow texture upload: serial %llu %ux%u fmt %d, %s, source %.2f MiB, copied %.2f MiB in %u rectangles: %.2f ms (plan %.2f, reserve %.2f%s, convert %.2f, record %.2f, protect %.2f)",
            (unsigned long long)t->serial, t->d.width, t->d.height, t->d.format, reupload ? "refresh" : "first upload",
            t->L.size / 1048576.0, copied / 1048576.0, copied_regions, (ended - started) / 1e6,
            (plan_done - started) / 1e6, (reserve_done - plan_done) / 1e6, pooled ? " pooled" : " transient",
            (fill_done - reserve_done) / 1e6, (copy_done - fill_done) / 1e6, (ended - copy_done) / 1e6);
    }
    uint64_t n = atomic_fetch_add(&upload_copies, 1) + 1;
    if (!(n % 256) && strips_on()) VKLOG("texture upload strips: %llu dirty-range checks, %llu copy rectangles, %.1f MiB avoided",
        (unsigned long long)atomic_load(&strip_checks), (unsigned long long)atomic_load(&strip_regions),
        atomic_load(&strip_saved) / 1048576.0);
    if (n == 1 || !(n % 256)) VKLOG("texture uploads %llu: pool reservations reused %llu, transient %llu, copied %.1f MiB; split over threads %llu (%llu row ranges, %llu fell back); re-uploads emitted %llu copy regions from %llu source regions (%.1f MiB saved, %llu with nothing to stage)",
        (unsigned long long)n, (unsigned long long)atomic_load(&upload_reused),
        (unsigned long long)atomic_load(&upload_fallback), (double)atomic_load(&upload_bytes) / 1048576.0,
        (unsigned long long)atomic_load(&upload_split), (unsigned long long)atomic_load(&upload_split_tasks),
        (unsigned long long)atomic_load(&upload_split_refused), (unsigned long long)atomic_load(&partial_regions_kept),
        (unsigned long long)atomic_load(&partial_regions_total), (double)atomic_load(&partial_bytes_saved) / 1048576.0,
        (unsigned long long)atomic_load(&partial_none_dirty));
}

static NvkImage *create(const tex_t *t, VkCommandBuffer cb, int discard) {
    NvnVkFormat fmt;
    if (!nvn_vk_format_map((uint32_t)t->d.format, &fmt)) {
        VKLOG("texture %llu: NVN format %#x has no Vulkan format", (unsigned long long)t->serial, t->d.format);
        nvk_die("unmapped NVN texture format");
    }
    if (t->d.samples > 1) { VKLOG("texture %llu: %d samples", (unsigned long long)t->serial, t->d.samples); nvk_die("multisampled NVN textures are not implemented"); }
    NvkImage *img = calloc(1, sizeof *img);
    if (!img) nvk_die("image table allocation");
    img->desc = *t;
    VkImageType type; VkImageCreateFlags flags;
    if (!nvn_vk_texture_shape(&t->d, &type, &img->view_type, &flags, &img->extent, &img->layers, &img->levels)) {
        VKLOG("texture %llu: target %d %ux%ux%u has no Vulkan shape", (unsigned long long)t->serial, t->d.target, t->d.width, t->d.height, t->d.depth);
        nvk_die("unsupported NVN texture shape");
    }
    img->serial = t->serial;
    img->format = fmt.format;
    img->aspect = aspect_of(fmt.format);
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(nvk_phys, fmt.format, &fp);
    VkFormatFeatureFlags f = fp.optimalTilingFeatures;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (f & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (f & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (android_vk_local_read ? VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT : 0);
    if (f & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | (nvk_fetch_depth_on() ? VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT : 0);
    if (f & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (type == VK_IMAGE_TYPE_3D) flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;   /* render targets are its depth slices */
    if (type == VK_IMAGE_TYPE_3D && android_vk_2d_view_of_3d) flags |= VK_IMAGE_CREATE_2D_VIEW_COMPATIBLE_BIT_EXT;   /* sampled as one 2D slice */
    VkImageFormatListCreateInfo flist = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    if (format_list_on() && !nvn_vk_format_compressed(fmt.format)) {   /* compressed: no UBWC anyway, and TOTK copies them through uint views */
        img->flist[0] = fmt.format; img->nflist = 1;
        VkFormat partner = srgb_partner(fmt.format);
        if (partner != VK_FORMAT_UNDEFINED) img->flist[img->nflist++] = partner;
        flist.viewFormatCount = img->nflist; flist.pViewFormats = img->flist;
    }
    VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = img->nflist ? &flist : NULL, .flags = flags | VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
        .imageType = type, .format = fmt.format, .extent = img->extent, .mipLevels = img->levels, .arrayLayers = img->layers,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkResult r = vkCreateImage(nvk_dev, &ici, NULL, &img->image);
    if (r != VK_SUCCESS) {
        VKLOG("texture %llu: vkCreateImage %ux%ux%u levels %u layers %u format %d usage %#x -> %d", (unsigned long long)t->serial,
            img->extent.width, img->extent.height, img->extent.depth, img->levels, img->layers, fmt.format, usage, r);
        nvk_die("create Vulkan image for NVN texture");
    }
    VkMemoryDedicatedRequirements dedicated = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 req2 = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &dedicated};
    VkImageMemoryRequirementsInfo2 rinfo = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, .image = img->image};
    vkGetImageMemoryRequirements2(nvk_dev, &rinfo, &req2);
    VkMemoryRequirements req = req2.memoryRequirements;
    img->bytes = req.size;
    uint32_t mtype = nvk_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (tex_suballoc_on() && !dedicated.requiresDedicatedAllocation && req.size <= TEX_SUB_MAX) img->sub = tex_mem_alloc(mtype, req.size, req.alignment ? req.alignment : 1);
    if (img->sub) VKCHECK(vkBindImageMemory(nvk_dev, img->image, img->sub->block->memory, img->sub->off), "bind NVN texture image memory (shared block)");
    else {
        VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = mtype};
        VKCHECK(vkAllocateMemory(nvk_dev, &mai, NULL, &img->memory), "allocate NVN texture image memory");
        VKCHECK(vkBindImageMemory(nvk_dev, img->image, img->memory, 0), "bind NVN texture image memory");
        tex_dedicated++;
    }
    VkImageMemoryBarrier to_general = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img->image,
        .subresourceRange = {img->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &to_general);
    if (!discard) upload(img, t, cb);
    else if (img->aspect == VK_IMAGE_ASPECT_COLOR_BIT && t->mem && t->L.size) {
        uint64_t n = atomic_fetch_add(&discarded_uploads, 1) + 1;
        atomic_fetch_add(&discarded_bytes, t->L.size);
        if (n == 1 || !(n % 256)) VKLOG("fully cleared targets: skipped %llu initial uploads (%.1f MiB of pool storage)",
            (unsigned long long)n, (double)atomic_load(&discarded_bytes) / 1048576.0);
    }
    nvk_barrier(cb);
    return img;
}

/* Debugging (TOTK_VK_ALIAS_LOG=1): a texture about to be filled from CPU memory that overlaps an image the GPU has
   written under another description (format, size, levels, target). Its upload then carries stale CPU bytes instead
   of what the GPU rendered there. Lock held. */
static void alias_check(const tex_t *t, const NvkImage *probe) {
    static int on = -1; static unsigned said;
    /* TOTK_VK_ALIAS_LOG=2 (live: "set TOTK_VK_ALIAS_LOG=2"): only aliases with a depth texture on either side (shadow maps). */
    static unsigned live_TOTK_VK_ALIAS_LOG; if (on < 0 || live_TOTK_VK_ALIAS_LOG != nvk_live_gen) { live_TOTK_VK_ALIAS_LOG = nvk_live_gen; const char *e = nvk_live_getenv("TOTK_VK_ALIAS_LOG"); on = e ? atoi(e) : 0; said = 0; }
    if (!on || said >= 200 || !t->mem || !t->L.size) return;
    uintptr_t lo = (uintptr_t)t->mem, hi = lo + t->L.size;
    for (unsigned b = 0; b < BUCKETS; ++b)
        for (NvkImage *o = storage_buckets[b]; o; o = o->snext) {
            if (!o->gpu_owned || o->key.mem >= hi || o->key.mem + o->key.size <= lo) continue;
            if (probe && !memcmp(&o->key, &probe->key, sizeof o->key)) continue;
            if (on == 2 && !(t->d.format >= 0x32 && t->d.format <= 0x36) && !(nvk_image_aspect(o) & VK_IMAGE_ASPECT_DEPTH_BIT)) continue;
            said++;
            VKLOG("alias: serial %llu fmt %d %ux%ux%u levels %d target %d at %p+%llu overlaps GPU-written vkfmt %u %ux%ux%u levels %u target %u at %#llx+%llu",
                (unsigned long long)t->serial, (int)t->d.format, t->d.width, t->d.height, t->d.depth, t->d.levels, (int)t->d.target, t->mem, (unsigned long long)t->L.size,
                o->key.format, o->key.w, o->key.h, o->key.d, o->key.levels, o->key.target, (unsigned long long)o->key.mem, (unsigned long long)o->key.size);
            return;
        }
}
static NvkImage *image_get(const tex_t *t, VkCommandBuffer cb, int discard) {
    if (!t || !t->serial) nvk_die("NVN texture without serial");
    fm_lock(&lock);
    SerialRef *have = *slot_of(t->serial);
    if (have) { NvkImage *img = have->img; fm_unlock(&lock); return img; }
    NvnVkFormat fmt;
    NvkImage probe, *img = NULL;
    int keyed = nvn_vk_format_map((uint32_t)t->d.format, &fmt) && storage_key(t, fmt.format, &probe);
    if (keyed && (img = *storage_slot(&probe))) {
        if (img->refs) share_live++;
        else if (!img->gpu_owned && !nvn_dirty_tracked(t->mem, (size_t)t->L.size)) {
            /* A kept CPU-uploaded image of untracked storage: the bytes may have changed unseen. */
            storage_unlink(img); destroy(img); img = NULL; share_dropped_untracked++;
        } else { share_revived++; idle_images--; idle_bytes -= img->bytes; }
        if (img) share_log();
    }
    if (!img && !discard) alias_check(t, keyed ? &probe : NULL);
    if (!img) {
        img = create(t, cb, discard);
        if (keyed) { img->key = probe.key; img->keyed = 1; *storage_slot(img) = img; share_keyed++; }
    }
    serial_put(t->serial, img);
    fm_unlock(&lock);
    return img;
}
NvkImage *nvk_image(const tex_t *t, VkCommandBuffer cb) { return image_get(t, cb, 0); }
NvkImage *nvk_image_discard(const tex_t *t, VkCommandBuffer cb) { return image_get(t, cb, 1); }

NvkImage *nvk_image_existing(uint64_t serial) {
    fm_lock(&lock);
    SerialRef *r = *slot_of(serial);
    NvkImage *img = r ? r->img : NULL;
    fm_unlock(&lock);
    return img;
}

/* Debugging ("reupload" in the shader debug file): every image filled from pool memory is uploaded whole again at its
   next use. Stale uploads (a partial refresh that missed a change) then disappear at once. */
void nvk_image_reupload_all(void) {
    unsigned n = 0;
    fm_lock(&lock);
    for (unsigned b = 0; b < BUCKETS; ++b)
        for (SerialRef *r = buckets[b]; r; r = r->next)
            if (r->img && !r->img->gpu_owned && r->img->uploaded) { r->img->uploaded = 0; r->img->clean_at = 0; n++; }
    fm_unlock(&lock);
    VKLOG("shader debug: %u images will be uploaded whole again", n);
}
VkImage nvk_image_handle(const NvkImage *image) { return image->image; }
VkFormat nvk_image_format(const NvkImage *image) { return image->format; }
VkImageAspectFlags nvk_image_aspect(const NvkImage *image) { return image->aspect; }
VkExtent2D nvk_image_level_extent(const NvkImage *image, uint32_t level) {
    uint32_t w = image->extent.width >> level, h = image->extent.height >> level;
    return (VkExtent2D){w ? w : 1, h ? h : 1};
}

VkImageView nvk_image_attachment_view(NvkImage *img, const view_t *v, uint32_t *level_out, uint32_t *layers_out) {
    uint32_t level = v && v->set_levels ? v->base_level : 0;
    uint32_t base_layer = v && v->set_layers ? v->min_layer : 0;
    uint32_t layers = v && v->set_layers && v->nlayers ? v->nlayers : 1;
    int is3d = img->view_type == VK_IMAGE_VIEW_TYPE_3D;
    uint32_t avail = is3d ? (img->extent.depth >> level ? img->extent.depth >> level : 1) : img->layers;
    if (level >= img->levels || base_layer + layers > avail) {
        VKLOG("attachment view level %u layers %u+%u outside image (%u levels, %u layers, view type %d)", level, base_layer, layers,
            img->levels, avail, img->view_type);
        nvk_die("render-target view outside the texture");
    }
    VkFormat format = img->format;
    if (v && v->set_format) {   /* e.g. an sRGB view of an RGBA8 target; images are created MUTABLE_FORMAT */
        NvnVkFormat f;
        if (!nvn_vk_format_map((uint32_t)v->format, &f)) nvk_die("render-target view format has no Vulkan format");
        format = view_format(img, f.format);
    }
    VkImageViewType type = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    *level_out = level; *layers_out = layers;
    fm_lock(&lock);
    for (unsigned i = 0; i < img->nviews; ++i) {
        ViewEntry *e = &img->views[i];
        if (e->base_level == level && e->levels == 1 && e->base_layer == base_layer && e->layers == layers && e->type == type
            && e->format == format && !e->components.r && !e->input) {
            fm_unlock(&lock);
            return e->view;
        }
    }
    if (img->nviews == MAX_VIEWS) {   /* recycle the oldest view; destruction waits for the GPU */
        nvk_defer_image(VK_NULL_HANDLE, VK_NULL_HANDLE, &img->views[0].view, 1, nvk_pending_fence());
        memmove(img->views, img->views + 1, (MAX_VIEWS - 1) * sizeof *img->views);
        img->nviews--;
    }
    VkImageViewUsageCreateInfo usage = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
        .usage = img->aspect == VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT};
    VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .pNext = &usage, .image = img->image, .viewType = type,
        .format = format, .subresourceRange = {img->aspect, level, 1, base_layer, layers}};
    VkImageView view;
    VKCHECK(vkCreateImageView(nvk_dev, &vci, NULL, &view), "create render-target view");
    img->views[img->nviews++] = (ViewEntry){level, 1, base_layer, layers, type, format, {0}, view, 0};
    fm_unlock(&lock);
    return view;
}

/* Depth aspect of a depth/stencil attachment's level and layer, as an input attachment (TOTK_VK_FETCH_DEPTH). */
VkImageView nvk_image_depth_input_view(NvkImage *img, const view_t *v) {
    if (!(img->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) nvk_die("depth input attachment of an image without depth");
    uint32_t level = v && v->set_levels ? v->base_level : 0, base_layer = v && v->set_layers ? v->min_layer : 0;
    if (level >= img->levels || base_layer >= img->layers) nvk_die("depth input attachment outside the texture");
    fm_lock(&lock);
    for (unsigned i = 0; i < img->nviews; ++i) {
        ViewEntry *e = &img->views[i];
        if (e->input && e->base_level == level && e->base_layer == base_layer) { fm_unlock(&lock); return e->view; }
    }
    if (img->nviews == MAX_VIEWS) {
        nvk_defer_image(VK_NULL_HANDLE, VK_NULL_HANDLE, &img->views[0].view, 1, nvk_pending_fence());
        memmove(img->views, img->views + 1, (MAX_VIEWS - 1) * sizeof *img->views);
        img->nviews--;
    }
    VkImageViewUsageCreateInfo usage = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, .usage = VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT};
    VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .pNext = &usage, .image = img->image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = img->format, .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, level, 1, base_layer, 1}};
    VkImageView view;
    VKCHECK(vkCreateImageView(nvk_dev, &vci, NULL, &view), "create depth input-attachment view");
    img->views[img->nviews++] = (ViewEntry){level, 1, base_layer, 1, VK_IMAGE_VIEW_TYPE_2D, img->format, {0}, view, 1};
    fm_unlock(&lock);
    return view;
}

void nvk_image_release(uint64_t serial) {
    fm_lock(&lock);
    SerialRef **p = slot_of(serial), *r = *p;
    if (!r) { fm_unlock(&lock); return; }
    *p = r->next;
    NvkImage *img = r->img;
    free(r);
    if (--img->refs) { fm_unlock(&lock); return; }
    if (img->keyed) {   /* kept for a later serial at the same storage */
        img->idle_fence = nvk_pending_fence();
        idle_images++; idle_bytes += img->bytes;
        fm_unlock(&lock);
        return;
    }
    destroy(img);
    fm_unlock(&lock);
}

/* Submission start: kept images nobody took over for a while go (sooner while many are kept). */
void nvk_image_sweep(void) {
    if (!idle_images) return;
    uint64_t now = nvk_pending_fence(), limit = idle_bytes > SHARE_IDLE_BYTES ? SHARE_IDLE_SHORT : SHARE_IDLE_SUBMITS;
    fm_lock(&lock);
    for (unsigned b = 0; b < BUCKETS && idle_images; ++b)
        for (NvkImage **p = &storage_buckets[b]; *p;) {
            NvkImage *img = *p;
            if (img->refs || now <= img->idle_fence + limit) { p = &img->snext; continue; }
            storage_unlink(img); destroy(img); share_swept++;
        }
    fm_unlock(&lock);
}

/* A memory pool is finalised: no later texture may take over images of its storage. */
void nvk_image_drop_range(uintptr_t lo, uintptr_t hi) {
    fm_lock(&lock);
    for (unsigned b = 0; b < BUCKETS; ++b)
        for (NvkImage **p = &storage_buckets[b]; *p;) {
            NvkImage *img = *p;
            if (img->key.mem < lo || img->key.mem >= hi) { p = &img->snext; continue; }
            int idle = !img->refs;
            storage_unlink(img);
            if (idle) { destroy(img); share_pool_dropped++; }
        }
    fm_unlock(&lock);
}

static _Atomic uint64_t gpu_write_seq;
void nvk_image_written(NvkImage *image) { image->gpu_owned = 1; image->gpu_seq = atomic_fetch_add(&gpu_write_seq, 1) + 1; image->gpu_dirty_at = nvn_dirty_now(); }
int nvk_image_gpu_owned(const NvkImage *image) { return image->gpu_owned; }

void nvk_image_refresh(NvkImage *img, const tex_t *t, VkCommandBuffer cb) {
    if (img->gpu_owned || !t->mem) return;
    if (img->uploaded && !nvn_dirty_since(t->mem, (size_t)t->L.size, img->stamp)) return;
    if (refresh_wait(img, t)) return;
    nvk_barrier(cb);
    upload(img, t, cb);
    nvk_barrier(cb);
}

static VkImageViewType view_type_of_target(int target) {
    switch (target) {
    case 0: return VK_IMAGE_VIEW_TYPE_1D; case 2: return VK_IMAGE_VIEW_TYPE_3D; case 3: return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    case 4: case 6: return VK_IMAGE_VIEW_TYPE_2D_ARRAY; case 8: return VK_IMAGE_VIEW_TYPE_CUBE; case 9: return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    default: return VK_IMAGE_VIEW_TYPE_2D;
    }
}

VkImageView nvk_image_sample_view(NvkImage *img, const tex_t *t, const view_t *v, VkImageViewType type, int storage) {
    uint32_t base_level = v && v->set_levels ? v->base_level : 0;
    uint32_t levels = v && v->set_levels && v->nlevels ? v->nlevels : img->levels - base_level;
    uint32_t base_layer = v && v->set_layers ? v->min_layer : 0;
    uint32_t layers = v && v->set_layers && v->nlayers ? v->nlayers : img->layers - base_layer;
    if ((int)type < 0) type = view_type_of_target(v && v->set_target ? v->target : t->d.target);
    if (storage) levels = 1;
    switch (type) {
    case VK_IMAGE_VIEW_TYPE_1D: case VK_IMAGE_VIEW_TYPE_2D: case VK_IMAGE_VIEW_TYPE_3D: layers = 1; break;
    case VK_IMAGE_VIEW_TYPE_CUBE: layers = 6; break;
    case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY: layers -= layers % 6; break;
    default: break;
    }
    /* A 2D view of a 3D image reads one depth slice (VK_EXT_image_2d_view_of_3d): the layer range addresses slices. */
    int slice_of_3d = type == VK_IMAGE_VIEW_TYPE_2D && img->view_type == VK_IMAGE_VIEW_TYPE_3D && android_vk_2d_view_of_3d && levels == 1;
    uint32_t slices = slice_of_3d ? (img->extent.depth >> base_level ? img->extent.depth >> base_level : 1) : img->layers;
    if (((type == VK_IMAGE_VIEW_TYPE_3D) != (img->view_type == VK_IMAGE_VIEW_TYPE_3D) && !slice_of_3d)
        || ((type == VK_IMAGE_VIEW_TYPE_CUBE || type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) && img->view_type != VK_IMAGE_VIEW_TYPE_CUBE && img->view_type != VK_IMAGE_VIEW_TYPE_CUBE_ARRAY)
        || !levels || !layers || base_level + levels > img->levels || base_layer + layers > slices) {
        static unsigned said;
        if (++said <= 20 || said % 1000 == 0)
            VKLOG("texture %llu: view type %d levels %u+%u layers %u+%u not possible on image type %d (%u levels, %u layers); zeros sampled (%u so far)",
                (unsigned long long)img->serial, type, base_level, levels, base_layer, layers, img->view_type, img->levels, img->layers, said);
        /* The game bound a texture the shader cannot view this way: a 2D slice of a 3D texture without
           VK_EXT_image_2d_view_of_3d (Qualcomm driver), or a plain 2D texture in a cube slot (texture 5, 28 Oct 2026,
           ended the game). The Switch samples something undefined there; zeros instead of stopping. */
        if (nvk_dummy_view_ready(type, storage)) return nvk_dummy_view(type, storage, VK_NULL_HANDLE);
        nvk_die("incompatible NVN texture view");
    }
    VkFormat format = img->format;
    NvnVkFormat f;
    int nvn_format = v && v->set_format ? v->format : t->d.format;
    if (!nvn_vk_format_map((uint32_t)nvn_format, &f)) nvk_die("texture view format has no Vulkan format");
    if (v && v->set_format) format = view_format(img, f.format);
    uint32_t sel[4] = {t->d.swizzle_r, t->d.swizzle_g, t->d.swizzle_b, t->d.swizzle_a};
    if (v && v->set_swizzle) for (int i = 0; i < 4; ++i) sel[i] = v->swz[i];
    VkComponentMapping comp = {0};
    if (!storage && !nvn_vk_format_swizzle(&f, sel, &comp)) nvk_die("invalid NVN texture swizzle");
    VkImageAspectFlags aspect = img->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : img->aspect;
    fm_lock(&lock);
    for (unsigned i = 0; i < img->nviews; ++i) {
        ViewEntry *e = &img->views[i];
        if (e->base_level == base_level && e->levels == levels && e->base_layer == base_layer && e->layers == layers && e->type == type
            && e->format == format && !memcmp(&e->components, &comp, sizeof comp) && !e->input) { fm_unlock(&lock); return e->view; }
    }
    if (img->nviews == MAX_VIEWS) {   /* recycle: views of old bindings are rarely reused */
        nvk_defer_image(VK_NULL_HANDLE, VK_NULL_HANDLE, &img->views[0].view, 1, nvk_pending_fence());
        memmove(img->views, img->views + 1, (MAX_VIEWS - 1) * sizeof *img->views);
        img->nviews--;
    }
    VkImageViewUsageCreateInfo usage = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
        .usage = storage ? VK_IMAGE_USAGE_STORAGE_BIT : VK_IMAGE_USAGE_SAMPLED_BIT};
    VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .pNext = &usage, .image = img->image, .viewType = type,
        .format = format, .components = comp, .subresourceRange = {aspect, base_level, levels, base_layer, layers}};
    VkImageView view;
    VkResult r = vkCreateImageView(nvk_dev, &vci, NULL, &view);
    if (r != VK_SUCCESS) {
        fm_unlock(&lock);
        VKLOG("texture %llu: sampling view type %d format %d (image %d) -> %d", (unsigned long long)img->serial, type, format, img->format, r);
        nvk_die("create NVN texture sampling view");
    }
    img->views[img->nviews++] = (ViewEntry){base_level, levels, base_layer, layers, type, format, comp, view, 0};
    fm_unlock(&lock);
    return view;
}

static VkImageView dummy_views[2][7];
/* Debugging (TOTK_SHADER_DEBUG "white"): a 2D texture of ones; created outside a pass. */
static VkImageView white_view;
int nvk_white_view_ready(void) { return white_view != VK_NULL_HANDLE; }
VkImageView nvk_white_view(VkCommandBuffer cb) {
    if (white_view) return white_view;
    static VkImage image; static VkDeviceMemory memory;
    VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {1, 1, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VKCHECK(vkCreateImage(nvk_dev, &ici, NULL, &image), "create white image");
    VkMemoryRequirements req; vkGetImageMemoryRequirements(nvk_dev, image, &req);
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
        .memoryTypeIndex = nvk_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VKCHECK(vkAllocateMemory(nvk_dev, &mai, NULL, &memory), "allocate white image");
    VKCHECK(vkBindImageMemory(nvk_dev, image, memory, 0), "bind white image");
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkClearColorValue one = {{1.0f, 1.0f, 1.0f, 1.0f}};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, image, VK_IMAGE_LAYOUT_GENERAL, &one, 1, &range);
    nvk_barrier(cb);
    VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = range};
    VKCHECK(vkCreateImageView(nvk_dev, &vci, NULL, &white_view), "create white view");
    return white_view;
}
int nvk_dummy_view_ready(VkImageViewType type, int storage) { return (unsigned)type <= 6 && dummy_views[storage != 0][type]; }
VkImageView nvk_dummy_view(VkImageViewType type, int storage, VkCommandBuffer cb) {
    static VkImage images[2][7]; static VkDeviceMemory memory[2][7]; VkImageView (*views)[7] = dummy_views;
    if ((unsigned)type > 6) nvk_die("dummy view type");
    if (views[storage][type]) return views[storage][type];
    int cube = type == VK_IMAGE_VIEW_TYPE_CUBE || type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    int is3d = type == VK_IMAGE_VIEW_TYPE_3D, is1d = type == VK_IMAGE_VIEW_TYPE_1D || type == VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
        .imageType = is3d ? VK_IMAGE_TYPE_3D : is1d ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {1, 1, 1}, .mipLevels = 1, .arrayLayers = cube ? 6 : 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VKCHECK(vkCreateImage(nvk_dev, &ici, NULL, &images[storage][type]), "create dummy image");
    VkMemoryRequirements req; vkGetImageMemoryRequirements(nvk_dev, images[storage][type], &req);
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
        .memoryTypeIndex = nvk_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VKCHECK(vkAllocateMemory(nvk_dev, &mai, NULL, &memory[storage][type]), "allocate dummy image");
    VKCHECK(vkBindImageMemory(nvk_dev, images[storage][type], memory[storage][type], 0), "bind dummy image");
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = images[storage][type], .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, ici.arrayLayers}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkClearColorValue zero = {{0}};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, ici.arrayLayers};
    vkCmdClearColorImage(cb, images[storage][type], VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
    nvk_barrier(cb);
    VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = images[storage][type], .viewType = type,
        .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, cube ? 6 : 1}};
    VKCHECK(vkCreateImageView(nvk_dev, &vci, NULL, &views[storage][type]), "create dummy view");
    return views[storage][type];
}

/* Called for every bound texture of every draw. nvn_dirty_now() advances on
 * every tracked guest write, so while it equals the value of the last clean
 * check nothing can have changed and the pool lookup is skipped. */
int nvk_image_needs_prepare(uint64_t serial, const tex_t *t) { return nvk_image_needs_prepare_existing(nvk_image_existing(serial), t); }
int nvk_image_needs_prepare_existing(NvkImage *img, const tex_t *t) {
    if (!img) return 1;
    if (img->gpu_owned || !t->mem) return 0;
    if (!img->uploaded) return 1;
    uint64_t now = nvn_dirty_now();
    if (img->clean_at == now) return 0;
    if (nvn_dirty_since(t->mem, (size_t)t->L.size, img->stamp)) return !refresh_wait(img, t);
    img->clean_at = now;
    return 0;
}

/* Synchronize a GPU-written colour texture back to NVN block-linear storage
 * before CPU texel access. Dedicated command resources avoid touching the
 * replay ring, and the shared queue orders this copy after submitted writes. */
static int readback_image(NvkImage *img, const tex_t *t, int fatal);
void nvk_image_readback(const tex_t *t) {
    NvkImage *img = nvk_image_existing(t->serial);
    if (!img || !img->gpu_owned) return;
    readback_image(img, t, 1);
}
static int readback_image(NvkImage *img, const tex_t *t, int fatal) {
    int ok = 1;
    if (img->aspect != VK_IMAGE_ASPECT_COLOR_BIT) { if (fatal) nvk_die("CPU readback of GPU depth/stencil texture is not implemented"); return 0; }
    NvnVkTextureStage stage;
    NvnVkTextureStageResult staged = nvn_vk_texture_stage(&t->d, t->mem, (size_t)t->L.size, &stage);
    if (staged != NVN_VK_STAGE_OK) { if (fatal) nvk_die("GPU readback texture layout unsupported"); return 0; }
    if (stage.format != img->format) { nvn_vk_texture_stage_destroy(&stage); if (fatal) nvk_die("GPU readback format conversion unsupported"); return 0; }
    VkBuffer buffer; VkDeviceMemory memory;
    VkBufferCreateInfo bi = {.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size=stage.data_size, .usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VKCHECK(vkCreateBuffer(nvk_dev, &bi, NULL, &buffer), "create readback buffer");
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(nvk_dev, buffer, &req);
    VkMemoryAllocateInfo mi = {.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize=req.size,
        .memoryTypeIndex=nvk_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VKCHECK(vkAllocateMemory(nvk_dev, &mi, NULL, &memory), "allocate readback memory");
    VKCHECK(vkBindBufferMemory(nvk_dev, buffer, memory, 0), "bind readback memory");
    VkCommandPool pool; VkCommandBuffer command; VkFence fence;
    VkCommandPoolCreateInfo pi = {.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags=VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, .queueFamilyIndex=nvk_family};
    VKCHECK(vkCreateCommandPool(nvk_dev, &pi, NULL, &pool), "create readback commands");
    VkCommandBufferAllocateInfo ai = {.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool=pool, .level=VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount=1};
    VKCHECK(vkAllocateCommandBuffers(nvk_dev, &ai, &command), "allocate readback commands");
    VkCommandBufferBeginInfo begin = {.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VKCHECK(vkBeginCommandBuffer(command, &begin), "begin readback commands");
    nvk_barrier(command);
    vkCmdCopyImageToBuffer(command, img->image, VK_IMAGE_LAYOUT_GENERAL, buffer, stage.region_count, stage.regions);
    VkMemoryBarrier barrier = {.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask=VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
    VKCHECK(vkEndCommandBuffer(command), "end readback commands");
    VkFenceCreateInfo fi = {.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VKCHECK(vkCreateFence(nvk_dev, &fi, NULL, &fence), "create readback fence");
    VkSubmitInfo si = {.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount=1, .pCommandBuffers=&command};
    nvk_queue_lock(); VkResult submitted=vkQueueSubmit(nvk_queue, 1, &si, fence); nvk_queue_unlock();
    VKCHECK(submitted, "submit readback");
    VKCHECK(vkWaitForFences(nvk_dev, 1, &fence, VK_TRUE, UINT64_MAX), "wait for readback");
    void *map; VKCHECK(vkMapMemory(nvk_dev, memory, 0, VK_WHOLE_SIZE, 0, &map), "map readback");
    void *token=nvn_dirty_gpu_write_begin(t->mem, (size_t)t->L.size);
    int bw; int bpb=nvn_fmt_bpb(t->d.format, &bw, NULL);
    for (unsigned i=0;i<stage.region_count;i++) {
        VkBufferImageCopy *r=&stage.regions[i];
        size_t pitch=(size_t)((r->imageExtent.width + (unsigned)bw - 1) / (unsigned)bw) * (unsigned)bpb;
        if (nvn_swizzle(&t->d, r->imageSubresource.mipLevel, r->imageSubresource.baseArrayLayer,
            (uint8_t *)map+r->bufferOffset, pitch, t->mem)) { if (fatal) nvk_die("GPU readback swizzle failed"); ok = 0; break; }
    }
    nvn_dirty_gpu_write_end(token);
    vkUnmapMemory(nvk_dev, memory);
    vkDestroyFence(nvk_dev, fence, NULL); vkDestroyCommandPool(nvk_dev, pool, NULL);
    vkDestroyBuffer(nvk_dev, buffer, NULL); vkFreeMemory(nvk_dev, memory, NULL);
    nvn_vk_texture_stage_destroy(&stage);
    return ok;
}

/* Tiny targets written back without a GPU sync (TOTK_VK_TINY_WRITEBACK, nvn_vk_backend.c): the copy into a host slot
   is recorded at the end of the pass, the swizzle into the game's memory runs once the GPU is done. NULL: an image or
   layout this cannot copy (nothing recorded). */
typedef struct { tex_t tex; NvnVkTextureStage stage; } TinyWb;
static VkBuffer tiny_buf; static VkDeviceMemory tiny_mem; static uint8_t *tiny_map;
void *nvk_tiny_wb_record(VkCommandBuffer cmd, const tex_t *t, unsigned slot, size_t cap, unsigned slots) {
    NvkImage *img = nvk_image_existing(t->serial);
    if (!img || !img->gpu_owned || img->aspect != VK_IMAGE_ASPECT_COLOR_BIT) return NULL;
    if (!tiny_buf) {   /* host slots, one per write-back in flight */
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)cap * slots, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VKCHECK(vkCreateBuffer(nvk_dev, &bi, NULL, &tiny_buf), "create tiny write-back buffer");
        VkMemoryRequirements req; vkGetBufferMemoryRequirements(nvk_dev, tiny_buf, &req);
        VkMemoryAllocateInfo mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
            .memoryTypeIndex = nvk_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        VKCHECK(vkAllocateMemory(nvk_dev, &mi, NULL, &tiny_mem), "allocate tiny write-back memory");
        VKCHECK(vkBindBufferMemory(nvk_dev, tiny_buf, tiny_mem, 0), "bind tiny write-back memory");
        void *map; VKCHECK(vkMapMemory(nvk_dev, tiny_mem, 0, VK_WHOLE_SIZE, 0, &map), "map tiny write-back memory"); tiny_map = map;
    }
    VkBuffer buf = tiny_buf; size_t off = (size_t)slot * cap;
    TinyWb *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    if (nvn_vk_texture_stage(&t->d, t->mem, (size_t)t->L.size, &w->stage) != NVN_VK_STAGE_OK) { free(w); return NULL; }
    if (w->stage.format != img->format || w->stage.data_size > cap) { nvn_vk_texture_stage_destroy(&w->stage); free(w); return NULL; }
    for (unsigned i = 0; i < w->stage.region_count; ++i) w->stage.regions[i].bufferOffset += off;
    vkCmdCopyImageToBuffer(cmd, img->image, VK_IMAGE_LAYOUT_GENERAL, buf, w->stage.region_count, w->stage.regions);
    w->tex = *t;
    return w;
}
/* Completion thread. 1: written back; 0: the CPU wrote that memory since `stamp` (its bytes are newer) or the
   swizzle failed. Frees the record either way. */
int nvk_tiny_wb_finish(void *state, uint64_t stamp) {
    TinyWb *w = state; const tex_t *t = &w->tex; int ok = 0; const uint8_t *map = tiny_map;
    if (!nvn_dirty_since(t->mem, (size_t)t->L.size, stamp)) {
        void *token = nvn_dirty_gpu_write_begin(t->mem, (size_t)t->L.size);
        int bw; int bpb = nvn_fmt_bpb(t->d.format, &bw, NULL);
        ok = bpb > 0;
        for (unsigned i = 0; ok && i < w->stage.region_count; ++i) {
            const VkBufferImageCopy *r = &w->stage.regions[i];
            size_t pitch = (size_t)((r->imageExtent.width + (unsigned)bw - 1) / (unsigned)bw) * (unsigned)bpb;
            if (nvn_swizzle(&t->d, r->imageSubresource.mipLevel, r->imageSubresource.baseArrayLayer, map + r->bufferOffset, pitch, t->mem)) ok = 0;
        }
        nvn_dirty_gpu_write_end(token);
    }
    nvn_vk_texture_stage_destroy(&w->stage); free(w);
    return ok;
}

/* GPU -> memory writeback for aliased storage (TOTK_VK_ALIAS_WRITEBACK=1; off by default: on the Thor, 22 Oct 2026,
   4800 write-backs in a few minutes left the water black and the grain in place, and each costs a GPU sync). On the Switch a texture and
   the render target the GPU drew into the same memory are one thing; here each description has its own image, and a
   texture first seen under another description than the image the GPU wrote there (TOTK re-uses transient memory
   for other formats and sizes every frame) was filled with stale CPU bytes. Before such a texture is created, the
   overlapping GPU-written colour images (oldest write first) are copied back into the game's memory, as the Metal
   layer's writeback does (upstream issue #78: the water colour). Each costs a GPU sync, so only images with a write
   not yet written back, and not overwritten by the CPU since, take part. */
static uint64_t awb_done, awb_skipped, awb_depth;
static int alias_wb_on(void) {
    static int on = -1;
    static unsigned live_TOTK_VK_ALIAS_WRITEBACK; if (on < 0 || live_TOTK_VK_ALIAS_WRITEBACK != nvk_live_gen) { live_TOTK_VK_ALIAS_WRITEBACK = nvk_live_gen; const char *e = nvk_live_getenv("TOTK_VK_ALIAS_WRITEBACK"); on = e && !strcmp(e, "1"); }
    return on;
}
extern int nvk_submit_wait(void);
void nvk_alias_writeback(const tex_t *t) {
    if (!alias_wb_on() || !t->mem || !t->L.size || nvk_image_existing(t->serial)) return;
    enum { MAXW = 8 };
    NvkImage *hit[MAXW]; tex_t desc[MAXW]; uint64_t seq[MAXW]; int n = 0;
    NvnVkFormat fmt; NvkImage probe; int keyed = nvn_vk_format_map((uint32_t)t->d.format, &fmt) && storage_key(t, fmt.format, &probe);
    uintptr_t lo = (uintptr_t)t->mem, hi = lo + t->L.size;
    fm_lock(&lock);
    for (unsigned b = 0; b < BUCKETS; ++b)
        for (NvkImage *o = storage_buckets[b]; o; o = o->snext) {
            if (!o->gpu_owned || o->gpu_seq == o->wb_seq || o->key.mem >= hi || o->key.mem + o->key.size <= lo) continue;
            if (keyed && !memcmp(&o->key, &probe.key, sizeof o->key)) continue;
            if (o->aspect != VK_IMAGE_ASPECT_COLOR_BIT) { awb_depth++; continue; }
            if (nvn_dirty_since(o->desc.mem, (size_t)o->desc.L.size, o->gpu_dirty_at)) { o->wb_seq = o->gpu_seq; awb_skipped++; continue; }
            if (n == MAXW) continue;
            int k = n++;
            while (k && seq[k - 1] > o->gpu_seq) { hit[k] = hit[k - 1]; desc[k] = desc[k - 1]; seq[k] = seq[k - 1]; --k; }
            hit[k] = o; desc[k] = o->desc; seq[k] = o->gpu_seq;
        }
    fm_unlock(&lock);
    if (!n) return;
    if (!nvk_submit_wait()) { awb_skipped += (uint64_t)n; return; }
    for (int i = 0; i < n; ++i) {
        fm_lock(&lock);
        int live = 0;
        for (unsigned b = 0; b < BUCKETS && !live; ++b) for (NvkImage *o = storage_buckets[b]; o; o = o->snext) if (o == hit[i]) { live = 1; break; }
        if (live && hit[i]->gpu_seq == seq[i]) {
            if (readback_image(hit[i], &desc[i], 0)) { hit[i]->wb_seq = seq[i]; hit[i]->gpu_dirty_at = nvn_dirty_now(); awb_done++; }
            else { hit[i]->wb_seq = seq[i]; awb_skipped++; }
        }
        fm_unlock(&lock);
    }
    if (awb_done <= 8 || !(awb_done % 200)) VKLOG("aliased GPU writeback: %llu images written back (last for serial %llu fmt %d %ux%u), %llu skipped, %llu depth not supported",
        (unsigned long long)awb_done, (unsigned long long)t->serial, (int)t->d.format, t->d.width, t->d.height, (unsigned long long)awb_skipped, (unsigned long long)awb_depth);
}

void nvk_image_cpu_written(const tex_t *t) {
    NvkImage *img = nvk_image_existing(t->serial);
    if (img) { img->gpu_owned = 0; img->uploaded = 0; img->clean_at = 0; }
}
/* After a CPU write of some rows (mtl_write_texels): an image the GPU had not written keeps its upload, and the dirty
   pages of those rows alone are refreshed (partial upload); one the GPU had written is uploaded whole, as before. */
void nvk_image_cpu_written_rows(const tex_t *t, int was_gpu) {
    NvkImage *img = nvk_image_existing(t->serial);
    if (!img) return;
    if (was_gpu) { img->gpu_owned = 0; img->uploaded = 0; }
    img->clean_at = 0;
}
int nvk_image_gpu_owned_serial(uint64_t serial) { NvkImage *img = nvk_image_existing(serial); return img && img->gpu_owned; }
