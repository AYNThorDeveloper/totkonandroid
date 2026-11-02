/* Vulkan execution layer for the shared NVN interface (d2r_gfx hle_nvn.c),
 * implementing the C API that nvn_mtl.m provides on Apple platforms.
 *
 * Memory pools keep NVN's "GPU address = CPU address" rule without
 * VK_EXT_external_memory_host (absent from the Adreno 740 driver): each pool
 * gets exportable Vulkan memory whose dma-buf is mapped MAP_FIXED over the
 * game's own pool pages, after copying their contents. CPU and GPU then
 * share the same physical pages (verified on the Thor, see VULKAN_BACKEND.md).
 *
 * Submissions are ordered on one timeline semaphore. CPU-visible effects
 * (event values, counter records) are written by a completion thread only
 * after the GPU has finished the submission, as in the Metal layer.
 *
 * Replay currently covers command-list control, state recording, barriers,
 * buffer copies, uniform updates, events and counters. Every operation that
 * still needs a Vulkan implementation stops the run, naming the operation. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include "nvn_obj.h"
#include "nvn_mtl.h"
#include "android_gfx.h"
#include "nvn_deswizzle.h"
#include "nvn_dirty.h"
#include "totk_timeline.h"
/* totk_timeline.c is part of the game build; host tests of this file link without it: off. */
__attribute__((weak)) int totk_tl_on(void) { return 0; }
__attribute__((weak)) void totk_tl(int kind, uint64_t a, uint64_t b) { (void)kind; (void)a; (void)b; }
__attribute__((weak)) void totk_tl_at(int kind, uint64_t t, uint64_t a, uint64_t b) { (void)kind; (void)t; (void)a; (void)b; }
__attribute__((weak)) uint64_t totk_tl_commit_id(void) { return 0; }
__attribute__((weak)) uint64_t totk_tl_now(void) { return 0; }

extern _Noreturn void runtime_fail(const char *message);
extern int totk_vpool_backing(uintptr_t va, uint64_t len, uintptr_t *cpu);
extern int android_front_has_window(void);
extern int android_front_display_hz(void);
extern void dm_guest_write_prepare(void *addr, size_t n);

#include "nvn_vk_fmutex.h"
#include "nvn_vk_internal.h"
/* Live switches live in nvn_vk_draw.c; tests that link this file without it read the environment. */
__attribute__((weak)) unsigned nvk_live_gen;
__attribute__((weak)) const char *nvk_live_getenv(const char *name) { return getenv(name); }
#include "nvn_vk_sampler.h"

void nvk_draw(const p_draw *dw, const p_draw_idx *di);
void nvk_dispatch(const p_dispatch *d);
void nvk_state_op(unsigned op, const void *a);

_Noreturn void nvk_die(const char *what) { VKLOG("FAIL: %s", what); runtime_fail(what); }

VkDevice nvk_dev;
VkPhysicalDevice nvk_phys;
VkQueue nvk_queue;
uint32_t nvk_family;
static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
void nvk_queue_lock(void) { pthread_mutex_lock(&queue_mutex); }
void nvk_queue_unlock(void) { pthread_mutex_unlock(&queue_mutex); }

uint32_t nvk_memory_type(uint32_t allowed, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(nvk_phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((allowed & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    VKLOG("no memory type with flags %#x among %#x", want, allowed);
    nvk_die("no suitable Vulkan memory type");
}

static PFN_vkGetMemoryFdKHR get_memory_fd;
static uint32_t pool_memory_type;

/* ---------- Memory pools ---------- */
typedef struct {
    uintptr_t lo, hi;          /* page-aligned range in the game's address space */
    const void *key;           /* pointer the pool was registered with */
    VkDeviceMemory memory;     /* NULL for virtual pools (aliases of physical pools) */
    VkBuffer buffer;
} Pool;
static Pool *pools;
static size_t pool_count, pool_cap;
static pthread_mutex_t pool_lock = PTHREAD_MUTEX_INITIALIZER;
/* Every binding of every draw looks up its pool (profile 2026-10-21: lock + unlock ~18% of the replay thread).
   pool_gen changes under pool_lock whenever the table changes; a thread reuses its recent physical-pool
   hits without the lock while the generation is unchanged. */
static _Atomic uint64_t pool_gen = 1;
typedef struct { uintptr_t lo, hi; VkBuffer buffer; uint64_t gen; } PoolHit;
static __thread PoolHit pool_hits[8]; static __thread unsigned pool_hit_next;
/* The replay thread (the one that begins submissions, mtl_sub_begin) keeps its hits in plain statics: a __thread
   variable in this dlopen'ed library costs a TLS-descriptor call on every access (tlsdesc_resolver_dynamic in the
   profile), and the replay looks up pools for nearly every binding of every draw. Other threads keep the __thread
   cache. TOTK_VK_POOL_REPLAY_CACHE=0: every thread uses the __thread cache, as before. */
static PoolHit replay_hits[8]; static unsigned replay_hit_next;
static _Atomic uintptr_t replay_thread;
static _Atomic uintptr_t sampler_replay_thread;   /* independent of the pool-cache switch */
static int replay_cache_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_VK_POOL_REPLAY_CACHE"); on = !(e && !strcmp(e, "0")); }
    return on;
}
static inline int on_replay_thread(void) {
    return atomic_load_explicit(&replay_thread, memory_order_relaxed) == (uintptr_t)pthread_self();
}
static const PoolHit *pool_hit(uintptr_t addr, uint64_t len) {
    uint64_t g = atomic_load_explicit(&pool_gen, memory_order_acquire);
    const PoolHit *hits = on_replay_thread() ? replay_hits : pool_hits;
    for (int k = 0; k < 8; ++k) {
        const PoolHit *h = &hits[k];
        if (h->gen == g && addr >= h->lo && addr + len >= addr && addr + len <= h->hi) return h;
    }
    return NULL;
}
static void pool_hit_store(const Pool *p) {   /* pool_lock held, physical pool */
    PoolHit h = {p->lo, p->hi, p->buffer, atomic_load_explicit(&pool_gen, memory_order_relaxed)};
    if (on_replay_thread()) replay_hits[replay_hit_next++ & 7] = h;
    else pool_hits[pool_hit_next++ & 7] = h;
}
static uintptr_t virtual_lo[256], virtual_hi[256];
static unsigned virtual_count;

static uintptr_t page_size(void) { return (uintptr_t)getpagesize(); }

/* totk_vpool.c announces virtual pools before registering them: their pages
 * become aliases of physical pools, so they must not get memory of their own. */
void android_vk_mark_virtual(const void *mem, uint64_t size) {
    pthread_mutex_lock(&pool_lock);
    if (virtual_count == 256) nvk_die("virtual pool table full");
    virtual_lo[virtual_count] = (uintptr_t)mem; virtual_hi[virtual_count++] = (uintptr_t)mem + size;
    pthread_mutex_unlock(&pool_lock);
}
static int is_virtual(uintptr_t lo, uintptr_t hi) {
    for (unsigned i = 0; i < virtual_count; ++i) if (lo >= virtual_lo[i] && hi <= virtual_hi[i]) return 1;
    return 0;
}
void android_vk_unmark_virtual(const void *mem) {
    pthread_mutex_lock(&pool_lock);
    for (unsigned i = 0; i < virtual_count; ++i) if (virtual_lo[i] == (uintptr_t)mem) {
        virtual_lo[i] = virtual_lo[virtual_count - 1]; virtual_hi[i] = virtual_hi[--virtual_count]; break;
    }
    pthread_mutex_unlock(&pool_lock);
}

/* pools[] is sorted by lo and pools do not overlap: binary search. */
static Pool *pool_containing(uintptr_t addr, uint64_t len) { /* pool_lock held */
    size_t lo = 0, hi = pool_count;
    while (lo < hi) { size_t mid = (lo + hi) / 2; if (pools[mid].lo <= addr) lo = mid + 1; else hi = mid; }
    if (!lo) return NULL;
    Pool *p = &pools[lo - 1];
    return addr + len >= addr && addr + len <= p->hi ? p : NULL;
}

/* ENOMEM can mean VMA exhaustion with plenty of physical RAM left. */
static void mapping_diagnostics(void) {
    unsigned maps = 0; char line[512];
    FILE *f = fopen("/proc/self/maps", "r");
    if (f) { while (fgets(line, sizeof line, f)) if (strchr(line, '\n')) maps++; fclose(f); }
    unsigned limit = 0; f = fopen("/proc/sys/vm/max_map_count", "r");
    if (f) { (void)fscanf(f, "%u", &limit); fclose(f); }
    VKLOG("memory mappings %u / limit %u, registered NVN pools %zu", maps, limit, pool_count);
    f = fopen("/proc/self/status", "r");
    if (f) { while (fgets(line, sizeof line, f)) if (!strncmp(line, "Vm", 2)) { line[strcspn(line, "\n")] = 0; VKLOG("%s", line); } fclose(f); }
}

static void create_pool_memory(Pool *p) {
    uint64_t size = p->hi - p->lo;
    VkExternalMemoryBufferCreateInfo external = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &external, .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
                 VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT};
    VKCHECK(vkCreateBuffer(nvk_dev, &bci, NULL, &p->buffer), "create pool buffer");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(nvk_dev, p->buffer, &req);
    if (!(req.memoryTypeBits & (1u << pool_memory_type))) nvk_die("pool memory type not allowed for exportable buffers");
    VkMemoryDedicatedAllocateInfo dedicated = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .buffer = p->buffer};
    VkExportMemoryAllocateInfo export = {.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, .pNext = &dedicated,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &export,
        .allocationSize = req.size, .memoryTypeIndex = pool_memory_type};
    VKCHECK(vkAllocateMemory(nvk_dev, &mai, NULL, &p->memory), "allocate pool memory");
    VKCHECK(vkBindBufferMemory(nvk_dev, p->buffer, p->memory, 0), "bind pool memory");
    void *view;
    VKCHECK(vkMapMemory(nvk_dev, p->memory, 0, VK_WHOLE_SIZE, 0, &view), "map pool memory");
    memcpy(view, (const void *)p->lo, size);
    /* The game mapping below is the permanent CPU view. Keeping a second
     * vkMapMemory alias for every pool needlessly consumes thousands of VMAs. */
    vkUnmapMemory(nvk_dev, p->memory);
    VkMemoryGetFdInfoKHR gfi = {.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = p->memory,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
    int fd = -1;
    VKCHECK(get_memory_fd(nvk_dev, &gfi, &fd), "export pool memory");
    void *at = mmap((void *)p->lo, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
    int err = errno;
    close(fd);
    if (at != (void *)p->lo) { VKLOG("mmap pool %#lx+%#llx: %s", (unsigned long)p->lo, (unsigned long long)size, strerror(err)); mapping_diagnostics(); nvk_die("map pool memory at game address"); }
}

static void release_pool_memory(Pool *p) {
    if (!p->memory) return;
    uint64_t size = p->hi - p->lo;
    /* Preserve the dma-buf contents before replacing its only CPU mapping. */
    uint8_t *copy = malloc(size);
    if (!copy) nvk_die("pool release snapshot allocation");
    memcpy(copy, (const void *)p->lo, size);
    /* Return ordinary memory with the current contents to the game. */
    if (mmap((void *)p->lo, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != (void *)p->lo)
        nvk_die("restore game memory after pool");
    memcpy((void *)p->lo, copy, size);
    free(copy);
    vkDestroyBuffer(nvk_dev, p->buffer, NULL);
    vkFreeMemory(nvk_dev, p->memory, NULL);
}

void mtl_pool_add(const void *mem, uint64_t size) {
    mtl_init();
    uintptr_t pg = page_size(), lo = (uintptr_t)mem & ~(pg - 1), hi = ((uintptr_t)mem + size + pg - 1) & ~(pg - 1);
    if (lo != (uintptr_t)mem || hi != (uintptr_t)mem + size) nvk_die("NVN pool storage is not page aligned");
    pthread_mutex_lock(&pool_lock);
    for (size_t i = 0; i < pool_count; ++i) if (lo < pools[i].hi && pools[i].lo < hi) {
        VKLOG("pool %#lx+%#llx overlaps registered pool %#lx-%#lx", (unsigned long)lo, (unsigned long long)size,
            (unsigned long)pools[i].lo, (unsigned long)pools[i].hi);
        pthread_mutex_unlock(&pool_lock);
        nvk_die("overlapping NVN memory pools are not supported by the Vulkan layer yet");
    }
    if (pool_count == pool_cap) {
        pool_cap = pool_cap ? pool_cap * 2 : 256;
        pools = realloc(pools, pool_cap * sizeof *pools);
        if (!pools) nvk_die("pool table allocation");
    }
    Pool p = {.lo = lo, .hi = hi, .key = mem};
    if (!is_virtual(lo, hi)) create_pool_memory(&p);
    size_t at = 0;
    while (at < pool_count && pools[at].lo < p.lo) ++at;
    memmove(&pools[at + 1], &pools[at], (pool_count - at) * sizeof *pools);
    pools[at] = p; pool_count++;
    atomic_fetch_add_explicit(&pool_gen, 1, memory_order_release);
    pthread_mutex_unlock(&pool_lock);
}

void mtl_pool_del(const void *mem) {
    pthread_mutex_lock(&pool_lock);
    for (size_t i = 0; i < pool_count; ++i) if (pools[i].key == mem) {
        Pool p = pools[i];
        memmove(&pools[i], &pools[i + 1], (pool_count - i - 1) * sizeof *pools); pool_count--;
        atomic_fetch_add_explicit(&pool_gen, 1, memory_order_release);
        pthread_mutex_unlock(&pool_lock);
        nvk_image_drop_range(p.lo, p.hi);
        release_pool_memory(&p);
        return;
    }
    pthread_mutex_unlock(&pool_lock);
}

/* totk_vpool.c on Android: alias [src, src+size) of a physical pool at dst. */
int android_vk_pool_alias(void *dst, const void *src, uint64_t size) {
    pthread_mutex_lock(&pool_lock);
    Pool *p = pool_containing((uintptr_t)src, size);
    if (!p || !p->memory) {
        VKLOG("alias source %p+%#llx is not in a physical pool; pools touching it:", src, (unsigned long long)size);
        for (size_t i = 0; i < pool_count; ++i)
            if (pools[i].lo < (uintptr_t)src + size && (uintptr_t)src < pools[i].hi)
                VKLOG("  %#lx-%#lx %s", (unsigned long)pools[i].lo, (unsigned long)pools[i].hi, pools[i].memory ? "physical" : "virtual");
        pthread_mutex_unlock(&pool_lock);
        return 0;
    }
    VkMemoryGetFdInfoKHR gfi = {.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = p->memory,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
    int fd = -1;
    VkResult r = get_memory_fd(nvk_dev, &gfi, &fd);
    off_t offset = (off_t)((uintptr_t)src - p->lo);
    pthread_mutex_unlock(&pool_lock);
    if (r != VK_SUCCESS) return 0;
    void *at = mmap(dst, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, offset);
    close(fd);
    return at == dst;
}

/* NVN's InvalidateMappedRange is also the point where a GPU-rendered colour
 * target becomes visible to the CPU. Keep only targets in CPU-visible pools;
 * the large G-buffer/display targets never need this path. */
typedef struct Writeback {
    tex_t tex;
    uint64_t stamp;
    uint8_t *small_before; /* exact bytes avoid false dirtiness of a shared 128 KiB tracking unit */
    struct Writeback *next;
} Writeback;
static Writeback *writebacks;
static pthread_mutex_t writeback_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t wb_done, wb_cpu_skip, wb_missing;

static int writeback_target(const tex_t *t) {
    if (!t || !t->mem || !t->L.size || !t->pool || t->pool->host ||
        !(t->pool->flags & (NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_CPU_CACHED))) return 0;
    if (t->d.flags & (NVN_TEXTURE_FLAGS_DISPLAY | NVN_TEXTURE_FLAGS_COMPRESSIBLE |
        NVN_TEXTURE_FLAGS_ADAPTIVE_ZCULL | NVN_TEXTURE_FLAGS_DEPTH16_PREFER_FAST_CLEAR |
        NVN_TEXTURE_FLAGS_ZCULL_SUPPORT_STENCIL)) return 0;
    return t->d.samples <= 1 && t->d.target != 10;
}

static void writeback_note(const tex_t *t) {
    if (!writeback_target(t)) return;
    nvn_dirty_protect(t->mem, (size_t)t->L.size);
    pthread_mutex_lock(&writeback_lock);
    Writeback *e = writebacks;
    while (e && e->tex.serial != t->serial) e = e->next;
    if (!e) {
        e = calloc(1, sizeof *e);
        if (!e) nvk_die("writeback record allocation");
        e->next = writebacks; writebacks = e;
    }
    e->tex = *t;
    e->stamp = nvn_dirty_now();
    if (t->L.size <= 4096) {
        if (!e->small_before) e->small_before = malloc((size_t)t->L.size);
        if (!e->small_before) nvk_die("small writeback snapshot allocation");
        memcpy(e->small_before, t->mem, (size_t)t->L.size);
    }
    pthread_mutex_unlock(&writeback_lock);
}

void mtl_pool_invalidate(const pool_t *p, uint64_t off, uint64_t n) {
    if (!p || !p->mem || off >= p->size || !n) return;
    if (n > p->size - off) n = p->size - off;
    const uintptr_t lo = (uintptr_t)p->mem + off, hi = lo + n;
    pthread_mutex_lock(&writeback_lock);
    int any = 0;
    for (Writeback *e = writebacks; e; e = e->next)
        if ((uintptr_t)e->tex.mem < hi && (uintptr_t)e->tex.mem + e->tex.L.size > lo) { any = 1; break; }
    pthread_mutex_unlock(&writeback_lock);
    if (!any) return;
    extern void nvn_x_drain(void);
    nvn_x_drain();
    mtl_drain();
    pthread_mutex_lock(&writeback_lock);
    Writeback *selected = NULL, **tail = &selected, **link = &writebacks;
    while (*link) {
        Writeback *e = *link;
        if ((uintptr_t)e->tex.mem < hi && (uintptr_t)e->tex.mem + e->tex.L.size > lo) {
            *link = e->next; e->next = NULL; *tail = e; tail = &e->next;
        } else link = &e->next;
    }
    pthread_mutex_unlock(&writeback_lock);
    for (Writeback *e = selected; e;) {
        Writeback *next = e->next;
        int dirty = nvn_dirty_since(e->tex.mem, (size_t)e->tex.L.size, e->stamp);
        int cpu_changed = dirty && (!e->small_before ||
            memcmp(e->small_before, e->tex.mem, (size_t)e->tex.L.size));
        if (cpu_changed) atomic_fetch_add(&wb_cpu_skip, 1);
        else if (nvk_image_existing(e->tex.serial)) {
            nvk_image_readback(&e->tex);
            uint64_t count = atomic_fetch_add(&wb_done, 1) + 1;
            if (count <= 8 || count % 500 == 0) VKLOG("GPU texture writeback %llu: serial %llu, %ux%u, CPU skips %llu",
                (unsigned long long)count, (unsigned long long)e->tex.serial, e->tex.d.width, e->tex.d.height,
                (unsigned long long)atomic_load(&wb_cpu_skip));
        } else atomic_fetch_add(&wb_missing, 1);
        free(e->small_before); free(e); e = next;
    }
}

static int resolve(uint64_t addr, uint64_t len, VkBuffer *buffer, uint64_t *offset) {
    const PoolHit *h = pool_hit((uintptr_t)addr, len);
    if (h) { *buffer = h->buffer; *offset = addr - h->lo; return 1; }
    pthread_mutex_lock(&pool_lock);
    Pool *p = pool_containing((uintptr_t)addr, len);
    if (p && p->memory) pool_hit_store(p);
    if (p && !p->memory) {   /* virtual pool: use the physical backing */
        uintptr_t cpu = 0;
        pthread_mutex_unlock(&pool_lock);
        if (!totk_vpool_backing((uintptr_t)addr, len, &cpu) || !cpu) return 0;
        pthread_mutex_lock(&pool_lock);
        p = pool_containing(cpu, len); addr = cpu;
        if (p && !p->memory) p = NULL;
    }
    if (p) { *buffer = p->buffer; *offset = addr - p->lo; }
    pthread_mutex_unlock(&pool_lock);
    return p != NULL;
}

int nvk_resolve(uint64_t addr, uint64_t len, VkBuffer *buffer, uint64_t *offset) { return resolve(addr, len, buffer, offset); }
/* Bytes from addr to the end of its pool (0 when addr is not in a pool). */
uint64_t nvk_pool_remaining(uint64_t addr) {
    const PoolHit *h = pool_hit((uintptr_t)addr, 1);
    if (h) return h->hi - (uintptr_t)addr;
    pthread_mutex_lock(&pool_lock);
    Pool *p = pool_containing((uintptr_t)addr, 1);
    if (p && p->memory) pool_hit_store(p);
    uint64_t n = p ? p->hi - (uintptr_t)addr : 0;
    pthread_mutex_unlock(&pool_lock);
    return n;
}

int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off) {
    VkBuffer b;
    if (!resolve(addr, len, &b, off)) return 0;
    *buf = (void *)b;
    return 1;
}

void mtl_pool_res_stats(uint64_t v[4]) {
    pthread_mutex_lock(&pool_lock);
    uint64_t bytes = 0;
    for (size_t i = 0; i < pool_count; ++i) bytes += pools[i].hi - pools[i].lo;
    v[0] = pool_count; v[1] = bytes; v[2] = pool_count; v[3] = bytes;
    pthread_mutex_unlock(&pool_lock);
}

/* ---------- Submissions and completion ---------- */
/* A counter epoch survives resets until all GPU results/reports referencing
 * it complete. Only the completion thread reads/writes samples. */
typedef struct { _Atomic unsigned refs; uint64_t samples; } CounterEpoch;
/* kind 1 event, 2 counter report, 4 completed occlusion query, 5 debug dump, 6 tiny target write-back, 7 uniform diff check */
typedef struct { int kind; volatile uint32_t *val; uint32_t mode, v; uint64_t addr; uint32_t type; CounterEpoch *counter; int query; } PostOp;
static void post(PostOp o);
/* ---------- Tiny targets written back (TOTK_VK_TINY_WRITEBACK) ----------
 * The CPU reads some small GPU renderings straight from memory. writeback_note's path waits for InvalidateMappedRange,
 * and in the TOTK runs only three 1x1 targets ever went through it. BOTW, same engine family, reduces the scene's
 * depth bounds on the GPU down to 3x3 and fits its shadow cascades on the CPU from them; rendered into images only,
 * its Android port read stale memory there until it copied every tiny colour target back at the end of its pass
 * (BOTWRecompAndroid runtime/nvn/vulkan/exec.c, write-backs; MIT). Here likewise: a colour target in a CPU-visible
 * pool of at most 4 KiB with one level and layer is copied into a host slot at the end of each pass that wrote it,
 * and the completion thread swizzles it into the game's memory before the submission's fence signals, unless the CPU
 * wrote that memory since. No GPU sync. TOTK_VK_TINY_WRITEBACK=0: off. */
#define TINY_SLOTS 64u
#define TINY_SLOT_SIZE 65536u   /* the largest TOTK_VK_TINY_MAX */
static _Atomic int tiny_busy[TINY_SLOTS]; static void *tiny_state[TINY_SLOTS]; static uint64_t tiny_stamp[TINY_SLOTS];
static unsigned tiny_next;
static _Atomic uint64_t tiny_done, tiny_cpu_skip, tiny_no_slot, tiny_refused;
static uint64_t tiny_noted, tiny_unwritten, tiny_big, tiny_shape, tiny_view;   /* why candidates (CPU-visible colour targets) were or were not taken */
static struct { uint64_t mem, size; } tiny_known[64]; static unsigned tiny_known_n;
static tex_t pass_tiny_tex[8]; static uint8_t pass_tiny;   /* colour slots of the open pass that are tiny targets */
static int tiny_on(void) {
    static int on = -1;
    static unsigned live_TOTK_VK_TINY_WRITEBACK; if (on < 0 || live_TOTK_VK_TINY_WRITEBACK != nvk_live_gen) { live_TOTK_VK_TINY_WRITEBACK = nvk_live_gen; const char *e = nvk_live_getenv("TOTK_VK_TINY_WRITEBACK"); on = !(e && !strcmp(e, "0")); }
    return on;
}
/* 1: a tiny target. Counts why other CPU-visible colour targets are not, and logs the first ones up to 64 KiB. */
/* TOTK_VK_TINY_MAX (bytes, live, default 4096, at most 65536): the size limit. In TOTK's gameplay nothing is under
   4 KiB, but ~5 CPU-visible targets a frame are just above (96x96 R16, 32x32 BC4, 128x128 RGBA8: 25 Oct). */
static uint64_t tiny_max(void) {
    static uint64_t m; static unsigned g; static int init;
    if (!init || g != nvk_live_gen) { init = 1; g = nvk_live_gen; const char *e = nvk_live_getenv("TOTK_VK_TINY_MAX");
        m = e && *e ? strtoull(e, NULL, 0) : 4096; if (m > TINY_SLOT_SIZE) m = TINY_SLOT_SIZE; }
    return m;
}
static int tiny_target(const tex_t *t, int has_view, const view_t *v) {
    if (!writeback_target(t)) return 0;
    if (t->L.size <= 65536) { unsigned k = 0; while (k < tiny_known_n && tiny_known[k].mem != (uint64_t)(uintptr_t)t->mem) ++k;
        if (k == tiny_known_n && tiny_known_n < 64) tiny_known[tiny_known_n++] = (typeof(tiny_known[0])){(uint64_t)(uintptr_t)t->mem, t->L.size}; }
    int why = t->L.size > tiny_max() ? 1 : (t->d.depth > 1 || t->L.layers > 1 || t->d.target == 2) ? 2
        : has_view && ((v->set_levels && v->base_level) || (v->set_layers && v->min_layer)) ? 3 : 0;
    if (why == 1) tiny_big++; else if (why == 2) tiny_shape++; else if (why == 3) tiny_view++;
    static unsigned shown; static uint64_t seen[24];
    if (t->L.size <= 65536 && shown < 24) {
        int known = 0; for (unsigned k = 0; k < shown; ++k) known |= seen[k] == t->serial;
        if (!known) { seen[shown++] = t->serial;
            VKLOG("small CPU-visible colour target: serial %llu, %ux%u, NVN format %#x, %llu bytes, %u levels, %u layers, pool flags %#x: %s", (unsigned long long)t->serial,
                t->d.width, t->d.height, t->d.format, (unsigned long long)t->L.size, t->d.levels, t->L.layers, t->pool ? t->pool->flags : 0,
                why == 0 ? "written back after its passes" : why == 1 ? "over the size limit" : why == 2 ? "layered or 3D" : "view of another level or layer"); }
    }
    return !why;
}
/* At the end of a pass (cb outside rendering): copy the tiny targets it wrote. */
static void tiny_record(VkCommandBuffer c, uint16_t written) {
    uint8_t m = pass_tiny & (uint8_t)written;
    tiny_unwritten += (uint64_t)__builtin_popcount(pass_tiny & ~m & 0xff);
    if (!m || !tiny_on()) return;
    VkMemoryBarrier before = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT};
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before, 0, NULL, 0, NULL);
    int any = 0;
    for (int i = 0; i < 8; ++i) if (m >> i & 1) {
        unsigned slot = TINY_SLOTS;
        for (unsigned k = 0; k < TINY_SLOTS; ++k) { unsigned s = (tiny_next + k) % TINY_SLOTS; if (!atomic_load(&tiny_busy[s])) { slot = s; break; } }
        if (slot == TINY_SLOTS) { atomic_fetch_add(&tiny_no_slot, 1); continue; }
        void *st = nvk_tiny_wb_record(c, &pass_tiny_tex[i], slot, TINY_SLOT_SIZE, TINY_SLOTS);
        if (!st) { atomic_fetch_add(&tiny_refused, 1); continue; }
        tiny_next = (slot + 1) % TINY_SLOTS;
        atomic_store(&tiny_busy[slot], 1); tiny_state[slot] = st; tiny_stamp[slot] = nvn_dirty_now();
        post((PostOp){6, NULL, 0, 0, 0, 0, NULL, (int)slot});
        any = 1;
        static unsigned shown; static uint64_t seen[16];
        int known = 0; for (unsigned k = 0; k < shown; ++k) known |= seen[k] == pass_tiny_tex[i].serial;
        if (!known && shown < 16) { seen[shown++] = pass_tiny_tex[i].serial;
            VKLOG("tiny target written back after its pass: serial %llu, %ux%u, NVN format %#x, %llu bytes", (unsigned long long)pass_tiny_tex[i].serial,
                pass_tiny_tex[i].d.width, pass_tiny_tex[i].d.height, pass_tiny_tex[i].d.format, (unsigned long long)pass_tiny_tex[i].L.size); }
    }
    if (any) {
        VkMemoryBarrier after = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &after, 0, NULL, 0, NULL);
    }
}
static void tiny_finish(unsigned slot) {   /* completion thread */
    if (slot >= TINY_SLOTS || !tiny_state[slot]) return;
    if (nvk_tiny_wb_finish(tiny_state[slot], tiny_stamp[slot])) atomic_fetch_add(&tiny_done, 1);
    else atomic_fetch_add(&tiny_cpu_skip, 1);
    tiny_state[slot] = NULL;
    atomic_store(&tiny_busy[slot], 0);
}

/* ---------- Which uniform ranges change between the replay and the GPU's completion (TOTK_VK_UBO_DIFF=1, live) ----------
 * Diagnostic for the yellow flashes with TOTK_VK_UBO_COPY=1: a copy of a uniform range taken at the replay differs from
 * what the GPU read live. Each uniform range bound in a submission is copied once (never bound); when the submission has
 * completed, the copy is compared with the game's memory then. Ranges that an in-pass uniform update touched are
 * skipped (their intermediate values are legitimate). Reported: how many ranges differ, the first few with the offset
 * of the first differing 16-byte vector, and whether the range holds a tiny render target (the 1x1 light measurements). */
#define UDIFF_N 6000
typedef struct { uint64_t addr, size; uint8_t *copy; } UdiffEnt;
static UdiffEnt udiff_cur[UDIFF_N]; static unsigned udiff_n;
static _Atomic uint64_t udiff_ranges, udiff_differ, udiff_overlap_tiny, udiff_skipped_log, udiff_full;
static int udiff_on(void) {
    static int on = -1; static unsigned g;
    if (on < 0 || g != nvk_live_gen) { g = nvk_live_gen; const char *e = nvk_live_getenv("TOTK_VK_UBO_DIFF"); on = e && !strcmp(e, "1"); }
    return on;
}
int nvk_ubo_diff_on(void) { return udiff_on(); }
void nvk_ubo_diff_note(uint64_t addr, uint64_t size, const uint8_t *copy) {   /* replay thread */
    if (udiff_n == UDIFF_N) { atomic_fetch_add(&udiff_full, 1); return; }
    uint8_t *c = malloc((size_t)size);
    if (!c) return;
    memcpy(c, copy, (size_t)size);
    udiff_cur[udiff_n++] = (UdiffEnt){addr, size, c};
}
static void udiff_commit(void) {   /* before the submission leaves: its list becomes one post-op */
    if (!udiff_n) return;
    UdiffEnt *l = malloc(udiff_n * sizeof *l);
    if (!l) { for (unsigned i = 0; i < udiff_n; ++i) free(udiff_cur[i].copy); udiff_n = 0; return; }
    memcpy(l, udiff_cur, udiff_n * sizeof *l);
    post((PostOp){7, NULL, 0, udiff_n, (uint64_t)(uintptr_t)l, 0, NULL, 0});
    udiff_n = 0;
}
static void udiff_check(const PostOp *o) {   /* completion thread */
    UdiffEnt *l = (UdiffEnt *)(uintptr_t)o->addr;
    for (uint32_t i = 0; i < o->v; ++i) {
        const UdiffEnt *e = &l[i];
        atomic_fetch_add(&udiff_ranges, 1);
        const uint8_t *live = (const uint8_t *)(uintptr_t)e->addr;
        if (memcmp(e->copy, live, (size_t)e->size)) {
            uint64_t first = 0; while (e->copy[first] == live[first]) ++first;
            unsigned vec = 0; for (uint64_t k = 0; k < e->size; k += 16) { uint64_t n = e->size - k < 16 ? e->size - k : 16; vec += memcmp(e->copy + k, live + k, (size_t)n) != 0; }
            int tiny = 0;
            for (unsigned t = 0; t < tiny_known_n; ++t) if (tiny_known[t].mem < e->addr + e->size && e->addr < tiny_known[t].mem + tiny_known[t].size) tiny = 1;
            uint64_t d = atomic_fetch_add(&udiff_differ, 1); if (tiny) atomic_fetch_add(&udiff_overlap_tiny, 1);
            if (d < 60) VKLOG("uniform diff: range %#llx+%llu changed between the replay and completion: %u of %llu vectors differ, first at +%llu (vec %llu)%s",
                (unsigned long long)e->addr, (unsigned long long)e->size, vec, (unsigned long long)((e->size + 15) / 16), (unsigned long long)first,
                (unsigned long long)(first / 16), tiny ? ", HOLDS A TINY RENDER TARGET" : "");
        }
        free(e->copy);
    }
    free(l);
}
void nvk_ubo_diff_stats(uint64_t v[5]) { v[0] = udiff_ranges; v[1] = udiff_differ; v[2] = udiff_overlap_tiny; v[3] = udiff_skipped_log; v[4] = udiff_full; }
void nvk_ubo_diff_skip(void) { atomic_fetch_add(&udiff_skipped_log, 1); }

static void counter_retain(CounterEpoch *c) { if (c) atomic_fetch_add(&c->refs, 1); }
static void counter_release(CounterEpoch *c) { if (c && atomic_fetch_sub(&c->refs, 1) == 1) free(c); }
typedef struct { uint64_t fence; PostOp *ops; int n; int ts; uint64_t frame; } Pending;
#define RING 8
#define SEGMENTS 32
static struct { VkCommandPool pool; VkCommandBuffer cb, seg[SEGMENTS]; uint64_t fence; VkDescriptorPool dpool[16]; unsigned ndpool, cur, nseg; } ring[RING];
static unsigned ring_cur;
static unsigned ring_next;
static void descriptor_slot_begin(unsigned slot);
static VkCommandBuffer cb;   /* recording submission, NULL outside begin/commit */
static uint64_t cmd_epoch;   /* one per command buffer begun: state bound in it is valid for this value only */
uint64_t nvk_cmd_epoch(void) { return cmd_epoch; }
static VkSemaphore timeline;
static uint64_t fence_issued;
static uint64_t fence_done;
static pthread_mutex_t done_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t done_cv = PTHREAD_COND_INITIALIZER;
static Pending pending[256];
static unsigned pending_head, pending_tail;
static PostOp *post_ops;
static int post_n, post_cap;

static uint64_t switch_ticks(void) {   /* 19.2 MHz, the Switch system counter */
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    unsigned __int128 ns = (unsigned __int128)ts.tv_sec * 1000000000u + (unsigned)ts.tv_nsec;
    return (uint64_t)(ns * 12 / 625);
}

/* ---------- GPU time per frame ----------
 * Two timestamps per ring slot bracket each submission; the completion thread
 * adds their difference to the frame the submission was recorded in, as the
 * Metal layer does with GPUStartTime/GPUEndTime. TOTK_VK_GPU_TIME=0: off. */
static _Atomic uint64_t current_frame;
static VkQueryPool tspool;
static double ts_period;
/* ---------- GPU time in counter reports (TOTK_VK_COUNTER_GPU_TIME=1, off by default) ----------
 * Every NVN counter report carries a timestamp. They are written when the whole submission has completed, so all
 * reports of a frame show nearly the same time: GPU work looks free. A game that sizes its dynamic resolution by
 * such timestamps then never lowers it. With this on, a timestamp report also writes a GPU timestamp at its place
 * in the command stream; the completion thread converts it to Switch ticks with an offset (the smallest observed
 * CPU-minus-GPU time at completion, so a report is never in the future). Off by default: it can change what the
 * game does (lower resolution in heavy scenes), which still has to be confirmed on the Thor. */
#define CTS_N 1024
static VkQueryPool cts_pool; static uint32_t cts_free[CTS_N]; static unsigned cts_free_n;
static pthread_mutex_t cts_lock = PTHREAD_MUTEX_INITIALIZER;
static int64_t cts_offset_ns; static int cts_offset_set;
static uint64_t cts_reports, cts_exhausted;
static int counter_gpu_time_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_VK_COUNTER_GPU_TIME"); on = e && !strcmp(e, "1"); if (on) VKLOG("counter reports carry GPU timestamps (TOTK_VK_COUNTER_GPU_TIME=1)"); }
    return on;
}
/* A query slot for a timestamp report, or -1 (off, no timestamps, all in flight). */
static int cts_take(void) {
    if (!counter_gpu_time_on() || !tspool || ts_period <= 0) return -1;
    pthread_mutex_lock(&cts_lock);
    if (!cts_pool) {
        VkQueryPoolCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = CTS_N};
        VKCHECK(vkCreateQueryPool(nvk_dev, &qci, NULL, &cts_pool), "create counter timestamp pool");
        vkResetQueryPool(nvk_dev, cts_pool, 0, CTS_N);
        for (unsigned i = 0; i < CTS_N; ++i) cts_free[i] = CTS_N - 1 - i;
        cts_free_n = CTS_N;
    }
    int q = cts_free_n ? (int)cts_free[--cts_free_n] : -1;
    if (q < 0) cts_exhausted++;
    pthread_mutex_unlock(&cts_lock);
    return q;
}
/* Completion thread: the report's GPU time in Switch ticks (19.2 MHz), and the slot back. */
static uint64_t cts_ticks(int q) {
    uint64_t t = 0;
    VkResult r = vkGetQueryPoolResults(nvk_dev, cts_pool, (uint32_t)q, 1, sizeof t, &t, sizeof t, VK_QUERY_RESULT_64_BIT);
    vkResetQueryPool(nvk_dev, cts_pool, (uint32_t)q, 1);
    pthread_mutex_lock(&cts_lock); cts_free[cts_free_n++] = (uint32_t)q; pthread_mutex_unlock(&cts_lock);
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t cpu = (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
    if (r != VK_SUCCESS) return (uint64_t)((unsigned __int128)cpu * 12 / 625);
    int64_t gpu = (int64_t)((double)t * ts_period), d = cpu - gpu;
    if (!cts_offset_set || d < cts_offset_ns) { cts_offset_ns = d; cts_offset_set = 1; }
    cts_reports++;
    return (uint64_t)((unsigned __int128)(uint64_t)(gpu + cts_offset_ns) * 12 / 625);
}
static uint64_t ts_mask;
#define GRING 64
static struct { _Atomic uint64_t frame, ns; _Atomic int pending; } gpu_frames[GRING];
static void timestamps_init(void) {
    const char *e = getenv("TOTK_VK_GPU_TIME");
    if (e && !strcmp(e, "0")) return;
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(nvk_phys, &props);
    uint32_t n = 0; vkGetPhysicalDeviceQueueFamilyProperties(nvk_phys, &n, NULL);
    VkQueueFamilyProperties *qf = calloc(n ? n : 1, sizeof *qf);
    if (!qf) nvk_die("queue family list");
    vkGetPhysicalDeviceQueueFamilyProperties(nvk_phys, &n, qf);
    uint32_t bits = nvk_family < n ? qf[nvk_family].timestampValidBits : 0;
    free(qf);
    if (!bits || props.limits.timestampPeriod <= 0) { VKLOG("GPU timestamps unavailable: frame GPU time is not measured"); return; }
    VkQueryPoolCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2 * RING};
    VKCHECK(vkCreateQueryPool(nvk_dev, &qci, NULL, &tspool), "create timestamp query pool");
    vkResetQueryPool(nvk_dev, tspool, 0, 2 * RING);
    ts_period = props.limits.timestampPeriod;
    ts_mask = bits >= 64 ? ~0ull : (1ull << bits) - 1;
    VKLOG("GPU timestamps: %u valid bits, %.2f ns per tick", bits, ts_period);
}
static void gpu_tag_begin(uint64_t f) {
    unsigned k = f % GRING;
    if (atomic_load(&gpu_frames[k].frame) != f) { atomic_store(&gpu_frames[k].frame, f); atomic_store(&gpu_frames[k].ns, 0); atomic_store(&gpu_frames[k].pending, 0); }
    atomic_fetch_add(&gpu_frames[k].pending, 1);
}
static uint64_t gpu_tag_done(const Pending *job) {   /* the submission's GPU span in ns (0: not measured) */
    uint64_t t[2], ns = 0;
    VkResult r = vkGetQueryPoolResults(nvk_dev, tspool, 2 * (uint32_t)job->ts, 2, sizeof t, t, sizeof *t, VK_QUERY_RESULT_64_BIT);
    if (r == VK_SUCCESS) ns = (uint64_t)((double)((t[1] - t[0]) & ts_mask) * ts_period);
    unsigned k = job->frame % GRING;
    if (atomic_load(&gpu_frames[k].frame) != job->frame) return ns;
    if (r == VK_SUCCESS) atomic_fetch_add(&gpu_frames[k].ns, ns);
    atomic_fetch_sub(&gpu_frames[k].pending, 1);
    return ns;
}

/* ---------- GPU time per render pass (TOTK_VK_PASS_TIMING=1, off by default) ----------
 * A timestamp just before each pass begins and just after it ends (outside the pass, so a tiler's loads and stores
 * count), grouped by targets (size, colour count and format, depth format) and the first fragment shader drawn.
 * Every 300 frames the ten groups with the most GPU time per frame are logged. */
#define PT_PER 2048
typedef struct { uint32_t w, h; int ncolor, color0, depth; char ps[9]; } PassMeta;
typedef struct { PassMeta m; uint64_t ns, count; int used; } PassCost;
static VkQueryPool pt_pool; static PassMeta pt_meta[RING][PT_PER / 2]; static unsigned pt_n[RING]; static int pt_cur = -1;
static PassCost pt_cost[512]; static pthread_mutex_t pt_lock = PTHREAD_MUTEX_INITIALIZER;
/* Also with TOTK_VK_PASS_TIMING=1: a timestamp at the end of every segment and at the start of the next, so the time
   the GPU had nothing to do while the CPU still translated (gap between segments) is known; the frame GPU time
   (gpu_frames) includes it. And the sum over all passes, so "GPU outside passes" (transfers, barriers, idle) shows. */
static VkQueryPool seg_pool; static unsigned seg_n[RING];
static uint64_t pt_all_ns, pt_all_passes, seg_idle_ns, seg_idle_subs;
static int pass_timing_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_VK_PASS_TIMING"); on = e && !strcmp(e, "1"); }
    return on && tspool && ts_period > 0;
}
static void pt_slot_reset(unsigned ring_i) {   /* the slot's fence has passed */
    if (!pass_timing_on()) return;
    if (!pt_pool) {
        VkQueryPoolCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = RING * PT_PER};
        VKCHECK(vkCreateQueryPool(nvk_dev, &qci, NULL, &pt_pool), "create pass timing pool");
        VKLOG("GPU time per render pass: on (TOTK_VK_PASS_TIMING=1)");
    }
    vkResetQueryPool(nvk_dev, pt_pool, ring_i * PT_PER, PT_PER);
    pt_n[ring_i] = 0; pt_cur = -1;
    if (!seg_pool) {
        VkQueryPoolCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = RING * 2 * SEGMENTS};
        VKCHECK(vkCreateQueryPool(nvk_dev, &qci, NULL, &seg_pool), "create segment timing pool");
    }
    vkResetQueryPool(nvk_dev, seg_pool, ring_i * 2 * SEGMENTS, 2 * SEGMENTS);
    seg_n[ring_i] = 0;
}
/* Around a segment cut: bottom-of-pipe at the end of the old buffer, top-of-pipe at the start of the new one. */
static void seg_mark_end(VkCommandBuffer c, unsigned ring_i) {
    if (!seg_pool || !pt_pool || seg_n[ring_i] >= SEGMENTS) return;
    vkCmdWriteTimestamp(c, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, seg_pool, ring_i * 2 * SEGMENTS + 2 * seg_n[ring_i]);
}
static void seg_mark_start(VkCommandBuffer c, unsigned ring_i) {
    if (!seg_pool || !pt_pool || seg_n[ring_i] >= SEGMENTS) return;
    vkCmdWriteTimestamp(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, seg_pool, ring_i * 2 * SEGMENTS + 2 * seg_n[ring_i] + 1);
    seg_n[ring_i]++;
}
static void pt_begin(VkCommandBuffer c, unsigned ring_i, PassMeta m) {
    if (!pt_pool || pt_n[ring_i] >= PT_PER / 2) { pt_cur = -1; return; }
    pt_cur = (int)pt_n[ring_i]++;
    pt_meta[ring_i][pt_cur] = m;
    vkCmdWriteTimestamp(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pt_pool, ring_i * PT_PER + 2u * (unsigned)pt_cur);
}
static void pt_end(VkCommandBuffer c, unsigned ring_i) {
    if (pt_cur < 0) return;
    vkCmdWriteTimestamp(c, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pt_pool, ring_i * PT_PER + 2u * (unsigned)pt_cur + 1);
    pt_cur = -1;
}
static void pt_done(unsigned ring_i) {   /* completion thread */
    unsigned n = pt_n[ring_i];
    if (!pt_pool || !n) return;
    static uint64_t t[PT_PER];
    unsigned sn = seg_n[ring_i];
    uint64_t idle = 0; int idle_ok = 0;
    if (seg_pool && sn && vkGetQueryPoolResults(nvk_dev, seg_pool, ring_i * 2 * SEGMENTS, 2 * sn, sizeof(uint64_t) * 2 * sn, t, sizeof *t, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        for (unsigned k = 0; k < sn; ++k) {
            uint64_t d = (t[2 * k + 1] - t[2 * k]) & ts_mask;
            if (d < ts_mask / 2) idle += (uint64_t)((double)d * ts_period);   /* the next segment started after the previous one finished */
        }
        idle_ok = 1;
    }
    if (vkGetQueryPoolResults(nvk_dev, pt_pool, ring_i * PT_PER, 2 * n, sizeof(uint64_t) * 2 * n, t, sizeof *t, VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) return;
    pthread_mutex_lock(&pt_lock);
    if (idle_ok) { seg_idle_ns += idle; seg_idle_subs++; }
    for (unsigned k = 0; k < n; ++k) {
        const PassMeta *m = &pt_meta[ring_i][k];
        uint64_t ns = (uint64_t)((double)((t[2 * k + 1] - t[2 * k]) & ts_mask) * ts_period);
        unsigned h = (m->w * 31u + m->h * 17u + (unsigned)m->ncolor * 7u + (unsigned)m->color0 * 13u + (unsigned)m->depth * 5u + (unsigned)m->ps[0] * 3u + (unsigned)m->ps[1]) % 512;
        for (unsigned probe = 0; probe < 512; ++probe, h = (h + 1) % 512) {
            PassCost *c = &pt_cost[h];
            if (!c->used) { c->used = 1; c->m = *m; }
            else if (memcmp(&c->m, m, sizeof *m)) continue;
            c->ns += ns; c->count++;
            break;
        }
        pt_all_ns += ns; pt_all_passes++;
    }
    pthread_mutex_unlock(&pt_lock);
}
void nvk_pass_label(const char *ps_variant) {   /* the first fragment shader drawn in the open pass names it */
    if (pt_cur < 0 || !ps_variant) return;
    PassMeta *m = &pt_meta[ring_cur][pt_cur];
    if (!m->ps[0]) snprintf(m->ps, sizeof m->ps, "%.8s", ps_variant);
}
static int pt_by_ns(const void *a, const void *b) { uint64_t x = ((const PassCost *)a)->ns, y = ((const PassCost *)b)->ns; return (x < y) - (x > y); }
static void pt_report(uint64_t f0, uint64_t f, double nf) {
    if (!pt_pool) return;
    static PassCost c[512];
    pthread_mutex_lock(&pt_lock); memcpy(c, pt_cost, sizeof c); memset(pt_cost, 0, sizeof pt_cost);
    uint64_t all = pt_all_ns, nall = pt_all_passes, idle = seg_idle_ns, nidle = seg_idle_subs;
    pt_all_ns = pt_all_passes = seg_idle_ns = seg_idle_subs = 0; pthread_mutex_unlock(&pt_lock);
    qsort(c, 512, sizeof *c, pt_by_ns);
    uint64_t top = 0;
    for (int i = 0; i < 10 && c[i].used; ++i) top += c[i].ns;
    VKLOG("frames %llu-%llu: all passes %.2f ms GPU per frame (%.1f passes, top 10 groups %.2f ms); GPU idle between segments %.2f ms per submission",
        (unsigned long long)f0, (unsigned long long)f, all / 1e6 / nf, nall / nf, top / 1e6 / nf, nidle ? idle / 1e6 / (double)nidle : 0.0);
    for (int i = 0; i < 10 && c[i].used; ++i)
        VKLOG("frames %llu-%llu: pass #%d %ux%u, %d colour (fmt %d), depth fmt %d, ps %s: %.2f ms GPU per frame, %.1f passes per frame",
            (unsigned long long)f0, (unsigned long long)f, i + 1, c[i].m.w, c[i].m.h, c[i].m.ncolor, c[i].m.color0, c[i].m.depth,
            c[i].m.ps[0] ? c[i].m.ps : "-", c[i].ns / 1e6 / nf, c[i].count / nf);
}

/* ---------- Occlusion queries (NVN_COUNTER_TYPE_SAMPLES_PASSED) ----------
 * Resolve/recycle each query after its submission, rather than keeping every
 * slot since ResetCounter. Reports read the epoch's running total at their
 * exact position in completion order; resetting starts a fresh epoch without
 * invalidating earlier in-flight reports. No 4096-query history/list limit. */
#define QUERIES 16384
static VkQueryPool qpool;
static VkQueryControlFlags qflags;
static uint32_t qfree[QUERIES];
static unsigned qfree_n;
static pthread_mutex_t qlock = PTHREAD_MUTEX_INITIALIZER;
static CounterEpoch *qcounter;
static int q_active = -1;

static void query_pool_init(void) {
    VkQueryPoolCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_OCCLUSION, .queryCount = QUERIES};
    VKCHECK(vkCreateQueryPool(nvk_dev, &qci, NULL, &qpool), "create occlusion query pool");
    vkResetQueryPool(nvk_dev, qpool, 0, QUERIES);
    for (uint32_t i = 0; i < QUERIES; ++i) qfree[i] = QUERIES - 1 - i;
    qfree_n = QUERIES;
    VkPhysicalDeviceFeatures f;
    vkGetPhysicalDeviceFeatures(nvk_phys, &f);
    qflags = f.occlusionQueryPrecise ? VK_QUERY_CONTROL_PRECISE_BIT : 0;
    VKLOG("occlusion queries: %u, %s sample counts", QUERIES, qflags ? "precise" : "non-precise (only zero / non-zero)");
}
static void query_begin(VkCommandBuffer c) {
    if (!qcounter || q_active >= 0) return;
    pthread_mutex_lock(&qlock);
    int idx = qfree_n ? (int)qfree[--qfree_n] : -1;
    pthread_mutex_unlock(&qlock);
    /* No fabricated zero counts: that made geometry disappear. Slots from
     * completed submissions are recycled even when the guest never resets. */
    if (idx < 0) nvk_die("16384 simultaneous occlusion queries: submission must be split");
    vkCmdBeginQuery(c, qpool, (uint32_t)idx, qflags);
    q_active = idx;
}
static void query_end(VkCommandBuffer c) {
    if (q_active < 0) return;
    vkCmdEndQuery(c, qpool, (uint32_t)q_active);
    counter_retain(qcounter);
    post((PostOp){4, NULL, 0, 0, 0, 0, qcounter, q_active});
    q_active = -1;
}

/* Reports not yet written by the completion thread, with the counter value
 * they will write: 0 for timestamps and other types, unknown for samples
 * passed. A conditional render sees the values the GPU would see (Metal
 * cpend_lookup); only an unknown value makes it render. */
#define CPEND 4096
static struct { uint64_t addr, fence, val; int known; } cpend[CPEND];
static unsigned cpend_head;
static pthread_mutex_t cpend_lock = PTHREAD_MUTEX_INITIALIZER;
int nvk_counter_lookup(uint64_t addr, uint64_t *val) {
    uint64_t done = nvk_completed_fence(); int r = -1;
    pthread_mutex_lock(&cpend_lock);
    for (unsigned k = 0; k < CPEND && k < cpend_head; ++k) {
        unsigned i = (cpend_head - 1 - k) % CPEND;
        if (cpend[i].fence <= done) break;
        if (cpend[i].addr == addr) { r = cpend[i].known; *val = cpend[i].val; break; }
    }
    pthread_mutex_unlock(&cpend_lock);
    return r;
}

/* Debugging (TOTK_SHADER_DEBUG "dumptex"): an image copied into a host buffer, written to a file once its submission completed. */
static void end_rendering(int cause);
static VkBuffer host_buffer(VkBufferUsageFlags usage, uint32_t size, void **map);
static struct { void *map; uint32_t size, w, h; int vkformat; uint64_t serial; } dumps[8]; static unsigned ndumps;
void nvk_debug_dump_image(NvkImage *img, uint64_t serial) {
    if (ndumps == 8) return;
    end_rendering(NVK_END_TEXTURE);
    VkExtent2D e = nvk_image_level_extent(img, 0);
    uint32_t size = e.width * e.height * 16;   /* room for up to 16 bytes per texel */
    void *map; VkBuffer buf = host_buffer(VK_BUFFER_USAGE_TRANSFER_DST_BIT, size, &map);
    nvk_barrier(cb);
    VkBufferImageCopy r = {.imageSubresource = {nvk_image_aspect(img) & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : nvk_image_aspect(img), 0, 0, 1},
        .imageExtent = {e.width, e.height, 1}};
    vkCmdCopyImageToBuffer(cb, nvk_image_handle(img), VK_IMAGE_LAYOUT_GENERAL, buf, 1, &r);
    nvk_barrier(cb);
    unsigned k = ndumps++;
    dumps[k].map = map; dumps[k].size = size; dumps[k].w = e.width; dumps[k].h = e.height; dumps[k].vkformat = nvk_image_format(img); dumps[k].serial = serial;
    post((PostOp){5, NULL, 0, 0, 0, 0, NULL, (int)k});
    VKLOG("dumptex: serial %llu %ux%u vkformat %d copied (dump %u)", (unsigned long long)serial, e.width, e.height, nvk_image_format(img), k);
}

/* Debugging ("dumpbuf"): a buffer range as the GPU sees it at this point of the command stream. */
void nvk_debug_dump_buffer(uint64_t addr, uint64_t size) {
    if (ndumps == 8 || !size || size > UINT32_MAX) return;
    VkBuffer src; uint64_t off;
    if (!resolve(addr, size, &src, &off)) { VKLOG("dumpbuf: %#llx outside memory pools", (unsigned long long)addr); return; }
    end_rendering(NVK_END_TEXTURE);
    void *map; VkBuffer buf = host_buffer(VK_BUFFER_USAGE_TRANSFER_DST_BIT, (uint32_t)size, &map);
    nvk_barrier(cb);
    VkBufferCopy r = {.srcOffset = off, .dstOffset = 0, .size = size};
    vkCmdCopyBuffer(cb, src, buf, 1, &r);
    nvk_barrier(cb);
    unsigned k = ndumps++;
    dumps[k].map = map; dumps[k].size = (uint32_t)size; dumps[k].w = (uint32_t)size; dumps[k].h = 0; dumps[k].vkformat = -1; dumps[k].serial = addr;
    post((PostOp){5, NULL, 0, 0, 0, 0, NULL, (int)k});
    VKLOG("dumpbuf: %#llx size %llu copied on the GPU (dump %u)", (unsigned long long)addr, (unsigned long long)size, k);
}

/* After a submission completes, before its fence counts as done (the game may be waiting): the post-ops run, one
   vkGetQueryPoolResults per occlusion query. TOTK_VK_QUERY_BATCH=1 (off by default) reads all of the submission's
   queries in one call (with availability; one not available yet is read alone as before). Measured either way: the
   time from completion to the fence signal, and from the signal to the game's wait returning. */
static uint64_t qb_vals[2 * QUERIES]; static uint32_t qb_lo, qb_hi; static int qb_valid;
static uint64_t post_ns, post_done_ops, post_queries, post_subs, fence_done_at, wake_ns, wake_n;
static int query_batch_on(void) {
    static int on = -1;
    static unsigned live_TOTK_VK_QUERY_BATCH; if (on < 0 || live_TOTK_VK_QUERY_BATCH != nvk_live_gen) { live_TOTK_VK_QUERY_BATCH = nvk_live_gen; const char *e = nvk_live_getenv("TOTK_VK_QUERY_BATCH"); on = e && !strcmp(e, "1"); }
    return on;
}
static void query_batch_read(const Pending *job) {
    qb_valid = 0;
    if (!query_batch_on() || !qpool) return;
    uint32_t lo = UINT32_MAX, hi = 0; unsigned n = 0;
    for (int i = 0; i < job->n; ++i) if (job->ops[i].kind == 4) { uint32_t q = (uint32_t)job->ops[i].query; if (q < lo) lo = q; if (q > hi) hi = q; n++; }
    if (n < 2) return;
    VkResult r = vkGetQueryPoolResults(nvk_dev, qpool, lo, hi - lo + 1, sizeof(uint64_t) * 2 * (hi - lo + 1), qb_vals, 2 * sizeof(uint64_t),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if (r != VK_SUCCESS && r != VK_NOT_READY) return;   /* not ready: other queries in the range; ours carry availability */
    qb_lo = lo; qb_hi = hi; qb_valid = 1;
}
static void run_post(const PostOp *o) {
    if (o->kind == 5) {
        char path[160]; unsigned k = (unsigned)o->query;
        if (dumps[k].vkformat < 0) snprintf(path, sizeof path, "/data/local/tmp/totk-android-v143/gpubuf_%llx_%u.raw", (unsigned long long)dumps[k].serial, dumps[k].size);
        else snprintf(path, sizeof path, "/data/local/tmp/totk-android-v143/dump_%llu_%ux%u_f%d.raw", (unsigned long long)dumps[k].serial, dumps[k].w, dumps[k].h, dumps[k].vkformat);
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(dumps[k].map, 1, dumps[k].size, f); fclose(f); }
        VKLOG("dumptex: wrote %s (%s)", path, f ? "ok" : strerror(errno));
        return;
    }
    if (o->kind == 6) { tiny_finish((unsigned)o->query); return; }
    if (o->kind == 7) { udiff_check(o); return; }
    if (o->kind == 1) { if (o->mode == 1) *o->val += o->v; else *o->val = o->v; }
    else if (o->kind == 4) {
        /* The submission's fence has passed, so the result should be there. Turnip once waited 2 s and
           returned VK_TIMEOUT (2026-10-21, open world): then count the query as visible, so the game
           draws the object rather than culling it, and go on. */
        uint64_t v = 0;
        uint32_t qi = (uint32_t)o->query;
        VkResult r = qb_valid && qi >= qb_lo && qi <= qb_hi && qb_vals[2 * (qi - qb_lo) + 1] ? (v = qb_vals[2 * (qi - qb_lo)], VK_SUCCESS)
            : vkGetQueryPoolResults(nvk_dev, qpool, qi, 1, sizeof v, &v, sizeof v, VK_QUERY_RESULT_64_BIT);
        if (r != VK_SUCCESS) {
            static unsigned unavailable;
            if (r < 0) { VKLOG("read occlusion query %d -> VkResult %d", o->query, r); nvk_die("read occlusion query"); }
            if (unavailable++ < 16) VKLOG("occlusion query %d unavailable after its fence (VkResult %d, %u so far): counted as visible", o->query, r, unavailable);
            v = 1;
        }
        o->counter->samples += v;
        vkResetQueryPool(nvk_dev, qpool, (uint32_t)o->query, 1);
        pthread_mutex_lock(&qlock);
        qfree[qfree_n++] = (uint32_t)o->query;
        pthread_mutex_unlock(&qlock);
        counter_release(o->counter);
    } else {
        uint64_t rec[2] = {o->counter ? o->counter->samples : 0, o->query > 0 ? cts_ticks(o->query - 1) : switch_ticks()};
        dm_guest_write_prepare((void *)(uintptr_t)o->addr, sizeof rec);
        memcpy((void *)(uintptr_t)o->addr, rec, sizeof rec);
        counter_release(o->counter);
    }
}

static void *completion_thread(void *unused) {
    (void)unused;
    pthread_setname_np(pthread_self(), "totk-vk-done");
    for (;;) {
        pthread_mutex_lock(&done_lock);
        while (pending_tail == pending_head) pthread_cond_wait(&done_cv, &done_lock);
        Pending job = pending[pending_tail % 256];
        pthread_mutex_unlock(&done_lock);
        VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1,
            .pSemaphores = &timeline, .pValues = &job.fence};
        uint64_t wait_begin = nvk_now_ns();
        VKCHECK(vkWaitSemaphores(nvk_dev, &wi, UINT64_MAX), "wait for GPU submission");
        uint64_t t0 = nvk_now_ns();
        query_batch_read(&job);
        unsigned nq = 0;
        for (int i = 0; i < job.n; ++i) { nq += job.ops[i].kind == 4; run_post(&job.ops[i]); }
        qb_valid = 0;
        uint64_t t1 = nvk_now_ns();
        if (job.ts >= 0) {
            uint64_t gns = gpu_tag_done(&job); pt_done((unsigned)job.ts);
            if (gns && totk_tl_on()) { uint64_t end = totk_tl_now(); totk_tl_at(TL_GPU, end - gns, job.fence, end); }   /* TOTK_TIMELINE */
        }
        uint64_t timing_done = nvk_now_ns();
        free(job.ops);
        pthread_mutex_lock(&done_lock);
        post_ns += t1 - t0; post_done_ops += (uint64_t)job.n; post_queries += nq; post_subs++; fence_done_at = nvk_now_ns();
        __atomic_store_n(&fence_done, job.fence, __ATOMIC_RELEASE); pending_tail++;
        pthread_cond_broadcast(&done_cv);
        pthread_mutex_unlock(&done_lock);
        /* Report after waking the game: logging must not extend the fence's critical path.
           A semaphore wait includes scheduling/driver latency, so do not call it pure GPU execution time. */
        static unsigned slow_said;
        if (slow_said < 64 && (t0 - wait_begin >= 60000000ull || t1 - t0 >= 5000000ull || timing_done - t1 >= 5000000ull)) {
            slow_said++;
            VKLOG("slow completion: frame %llu fence %llu: semaphore wait %.2f ms, post-ops %.2f ms (%d ops, %u queries), timing reads %.2f ms",
                (unsigned long long)job.frame, (unsigned long long)job.fence, (t0 - wait_begin) / 1e6,
                (t1 - t0) / 1e6, job.n, nq, (timing_done - t1) / 1e6);
        }
    }
    return NULL;
}

static void init_once(void);
void mtl_init(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, init_once);
}

static void init_once(void) {
    const AndroidGfxVulkan *vk = android_gfx_vulkan();
    if (!vk) nvk_die("NVN Vulkan layer used before InitializeGraphics");
    nvk_dev = vk->device; nvk_phys = vk->physical; nvk_queue = vk->queue; nvk_family = vk->queue_family;
    get_memory_fd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(nvk_dev, "vkGetMemoryFdKHR");
    if (!get_memory_fd) nvk_die("vkGetMemoryFdKHR unavailable");
    /* Host-visible, coherent: CPU writes need no flush before submission. */
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(vk->physical, &mp);
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    pool_memory_type = UINT32_MAX;
    VkExternalMemoryBufferCreateInfo external = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &external, .size = 65536,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer probe;
    VKCHECK(vkCreateBuffer(nvk_dev, &bci, NULL, &probe), "probe exportable buffer");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(nvk_dev, probe, &req);
    vkDestroyBuffer(nvk_dev, probe, NULL);
    for (uint32_t i = 0; i < mp.memoryTypeCount && pool_memory_type == UINT32_MAX; ++i)
        if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) pool_memory_type = i;
    if (pool_memory_type == UINT32_MAX) nvk_die("no host-visible coherent exportable memory type");
    VkSemaphoreTypeCreateInfo sti = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0};
    VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &sti};
    VKCHECK(vkCreateSemaphore(nvk_dev, &sci, NULL, &timeline), "create timeline semaphore");
    nvk_record_install();   /* before anything is recorded */
    query_pool_init();
    timestamps_init();
    for (unsigned i = 0; i < RING; ++i) {
        VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, .queueFamilyIndex = nvk_family};
        VKCHECK(vkCreateCommandPool(nvk_dev, &pci, NULL, &ring[i].pool), "create command pool");
        VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = ring[i].pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        VKCHECK(vkAllocateCommandBuffers(nvk_dev, &cai, &ring[i].cb), "allocate command buffer");
    }
    pthread_t t;
    if (pthread_create(&t, NULL, completion_thread, NULL)) nvk_die("start completion thread");
    pthread_detach(t);
    VKLOG("ready: pools use memory type %u (flags %#x), dma-buf mapped at game addresses",
        pool_memory_type, mp.memoryTypes[pool_memory_type].propertyFlags);
}

static _Atomic uint64_t gpu_wait_ns;   /* time threads waited for the GPU (freeze logger) */
int mtl_wait(uint64_t fence, uint64_t timeout_ns) {
    struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl);
    uint64_t ns = (uint64_t)dl.tv_nsec + (timeout_ns > 3600000000000ull ? 3600000000000ull : timeout_ns);
    dl.tv_sec += (time_t)(ns / 1000000000ull); dl.tv_nsec = (long)(ns % 1000000000ull);
    uint64_t wait_t0 = nvk_now_ns();
    pthread_mutex_lock(&done_lock);
    int waited = fence_done < fence;
    while (fence_done < fence) if (pthread_cond_timedwait(&done_cv, &done_lock, &dl)) break;
    int ok = fence_done >= fence;
    if (waited && ok && fence_done_at) { wake_ns += nvk_now_ns() - fence_done_at; wake_n++; }
    pthread_mutex_unlock(&done_lock);
    if (waited) atomic_fetch_add(&gpu_wait_ns, nvk_now_ns() - wait_t0);
    return ok;
}
uint64_t mtl_last_fence(void) { return fence_issued; }
void mtl_finish(void) { if (fence_issued) mtl_wait(fence_issued, UINT64_MAX); }
void mtl_drain(void) {
    mtl_init(); mtl_finish();
    nvk_record_sync();   /* queued segment submission needs the queue lock: drain BEFORE taking it */
    nvk_queue_lock(); VkResult r = vkQueueWaitIdle(nvk_queue); nvk_queue_unlock();
    VKCHECK(r, "drain queue");
}

void nvk_barrier(VkCommandBuffer c) {
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}

/* ---------- Deferred destruction ---------- */
typedef struct { uint64_t fence; VkBuffer buffer; VkImage image; VkDeviceMemory memory; VkImageView *views; unsigned nviews;
                 void (*release)(void *); void *ctx; } Garbage;   /* release: called after the destruction (suballocated memory) */
static Garbage *garbage; static size_t garbage_n, garbage_cap;
static pthread_mutex_t garbage_lock = PTHREAD_MUTEX_INITIALIZER;
uint64_t nvk_pending_fence(void) { return fence_issued + 1; }
/* Lock-free: asked per uniform binding with the translation thread (the lock was taken ~5,500 times per frame); the one writer stores under done_lock. */
uint64_t nvk_completed_fence(void) { return __atomic_load_n(&fence_done, __ATOMIC_ACQUIRE); }
static void defer(Garbage g) {
    pthread_mutex_lock(&garbage_lock);
    if (garbage_n == garbage_cap) {
        garbage_cap = garbage_cap ? garbage_cap * 2 : 256;
        garbage = realloc(garbage, garbage_cap * sizeof *garbage);
        if (!garbage) nvk_die("deferred destruction list");
    }
    garbage[garbage_n++] = g;
    pthread_mutex_unlock(&garbage_lock);
}
void nvk_defer_buffer(VkBuffer buffer, VkDeviceMemory memory, uint64_t fence) { defer((Garbage){fence, buffer, VK_NULL_HANDLE, memory, NULL, 0, NULL, NULL}); }
void nvk_defer_image_release(VkImage image, VkDeviceMemory memory, VkImageView *views, unsigned nviews, uint64_t fence, void (*release)(void *), void *ctx) {
    VkImageView *copy = nviews ? malloc(nviews * sizeof *copy) : NULL;
    if (nviews && !copy) nvk_die("deferred view list");
    if (nviews) memcpy(copy, views, nviews * sizeof *copy);
    defer((Garbage){fence, VK_NULL_HANDLE, image, memory, copy, nviews, release, ctx});
}
void nvk_defer_image(VkImage image, VkDeviceMemory memory, VkImageView *views, unsigned nviews, uint64_t fence) {
    nvk_defer_image_release(image, memory, views, nviews, fence, NULL, NULL);
}
/* At most TOTK_VK_FREE_PER_SUBMIT (default 64; 0 = no limit) objects are destroyed per submission, oldest first: an
   area change released hundreds of textures at once, each vkFreeMemory on the replay thread (freeze analysis 23 Oct).
   The rest waits for the next submission. */
static uint64_t garbage_deferred;
static void collect_garbage(void) {
    static long cap = -1;
    if (cap < 0) { const char *e = getenv("TOTK_VK_FREE_PER_SUBMIT"); cap = e && *e ? atol(e) : 64; if (cap < 0) cap = 0; }
    uint64_t done = nvk_completed_fence();
    pthread_mutex_lock(&garbage_lock);
    size_t keep = 0; long freed = 0;
    for (size_t i = 0; i < garbage_n; ++i) {
        Garbage *g = &garbage[i];
        if (g->fence > done || (cap && freed >= cap)) { garbage[keep++] = *g; if (g->fence <= done) garbage_deferred++; continue; }
        for (unsigned v = 0; v < g->nviews; ++v) vkDestroyImageView(nvk_dev, g->views[v], NULL);
        free(g->views);
        if (g->buffer) vkDestroyBuffer(nvk_dev, g->buffer, NULL);
        if (g->image) vkDestroyImage(nvk_dev, g->image, NULL);
        if (g->memory) vkFreeMemory(nvk_dev, g->memory, NULL);
        if (g->release) g->release(g->ctx);
        freed++;
    }
    garbage_n = keep;
    pthread_mutex_unlock(&garbage_lock);
}

/* ---------- Render targets (dynamic rendering) ---------- */
static p_set_rt cur_rt;
static int have_rt, rendering;
static p_scissor scissor;
static int have_scissor;
static VkExtent2D render_extent;
static uint64_t pass_color[8], pass_depth;   /* serials attached to the open pass */
static const NvkImage *pass_img[9];           /* their images (a shared image may be sampled through another serial) */
static uint64_t pass_count, pass_ends[NVK_END_N], rt_continues, clears_skipped, clears_folded;
/* Full-area clears before the first draw of a pass become its load op
 * (LOAD_OP_CLEAR) instead of LOAD plus vkCmdClearAttachments: on a tiler the
 * attachment is then not read from memory first. A folded clear is applied
 * by the next begin_rendering; anything that ends the pass, reads a pending
 * target or changes targets begins and ends it first. TOTK_VK_CLEAR_LOAD=0: off. */
static struct { int any; uint8_t color[8], depth, stencil; VkClearColorValue cc[8]; float d; uint32_t s; } pend;
static void begin_rendering(void);
static void fold_targets(void);
/* B5: the deferred clear of the 1280x720 scene colour target, see scene_mode() below */
#define SCENE_SLOTS 4   /* one per 1280x720 image: TOTK clears more than one per frame */
static struct { uint64_t serial; NvkImage *img; uint32_t level, layer; VkClearColorValue cc; int valid; } scene_defer[SCENE_SLOTS];
static unsigned scene_pending, scene_next;
static uint64_t scene_deferred, scene_dropped, scene_materialized, scene_direct, scene_evicted, scene_read;
static int scene_find(uint64_t serial, const NvkImage *img);
static void scene_defer_run(int k);
static void scene_log(const char *what);
static int pass_storage_written, pass_image_stored;   /* any shader store in the open pass; one to an image */
static uint16_t pass_written, pass_sampled;   /* attachment slots written / sampled in the open pass (see nvk_rt_slots) */
static uint16_t pass_fetch_read;              /* depth (bit 8) read as an input attachment since the last write-after-read barrier */

/* Uniform updates the GPU has not executed yet, in command order (vkCmdUpdateBuffer cannot run inside a pass).
   An update while a pass is open is logged unflushed: draws after it bind a snapshot of the logical contents
   (pool memory plus every logged update, in order; nvk_ubo_read), and the pass end writes it to the pool.
   Entries leave the log once their submission completed: the coherent pool memory then holds them.
   TOTK_VK_UBO_INPASS=0: every update ends the pass, as before. */
typedef struct { uint64_t addr, n, fence; uint8_t *data; int flushed; uint64_t seq; } UboLog;
static UboLog *ubo_log; static size_t ubo_head, ubo_n, ubo_cap, ubo_unflushed;
static uint64_t ubo_unflushed_lo = UINT64_MAX, ubo_unflushed_hi;   /* span of the unflushed entries */
static uint64_t ubo_epoch, ubo_inpass_total;
/* Index of the log by 4 KiB page (hashed into UBO_PAGES counters): a read or check whose pages hold no entry skips
   walking the log (240 in-pass updates per frame plus those of the submission in flight; every uniform snapshot and
   storage-address read walked them all). Collisions only cost the walk. TOTK_VK_UBO_INDEX=0: always walk. */
#define UBO_PAGES 4096
static uint32_t ubo_page_all[UBO_PAGES], ubo_page_unfl[UBO_PAGES];
static uint64_t ubo_index_skips, ubo_index_walks;
static int ubo_index_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_VK_UBO_INDEX"); on = !(e && !strcmp(e, "0")); }
    return on;
}
static void ubo_pages(uint32_t *tab, uint64_t addr, uint64_t n, int delta) {
    uint64_t first = addr >> 12, last = (addr + n - 1) >> 12;
    if (last - first >= UBO_PAGES) last = first + UBO_PAGES - 1;
    for (uint64_t p = first; p <= last; ++p) tab[p & (UBO_PAGES - 1)] += (uint32_t)delta;
}
static int ubo_pages_any(const uint32_t *tab, uint64_t addr, uint64_t n) {
    if (!ubo_index_on() || !n) return 1;
    uint64_t first = addr >> 12, last = (addr + n - 1) >> 12;
    if (last - first >= UBO_PAGES) return 1;
    for (uint64_t p = first; p <= last; ++p) if (tab[p & (UBO_PAGES - 1)]) { ubo_index_walks++; return 1; }
    ubo_index_skips++;
    return 0;
}
/* Optional CPU-side storage reuse. Only pruned (flushed AND completed) entries return their bytes.
   Small updates use size classes; at most 2 MiB of idle storage is retained. The replay thread owns this pool,
   just like the uniform log. No ring-slot assumptions: an old submission can stay live across any number of frames.
   TOTK_VK_UBO_POOL=1 enables it; off until the separate Thor A/B test. */
#define UBO_POOL_CLASSES 11
#define UBO_POOL_LIMIT (2u * 1024u * 1024u)
typedef struct UboFree { struct UboFree *next; } UboFree;
static UboFree *ubo_free[UBO_POOL_CLASSES];
static size_t ubo_pool_bytes;
static uint64_t ubo_pool_hits, ubo_pool_allocs;
static int ubo_pool_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_VK_UBO_POOL"); on = e && !strcmp(e, "1"); }
    return on;
}
static unsigned ubo_pool_class(uint64_t n, size_t *bytes) {
    unsigned c = 0; *bytes = 64;
    while (n > *bytes && c < UBO_POOL_CLASSES) { *bytes *= 2; c++; }
    return c;
}
static uint8_t *ubo_data_alloc(uint64_t n) {
    if (!ubo_pool_on()) return malloc(n ? n : 1);
    size_t bytes; unsigned c = ubo_pool_class(n, &bytes);
    if (c < UBO_POOL_CLASSES && ubo_free[c]) {
        UboFree *p = ubo_free[c]; ubo_free[c] = p->next;
        ubo_pool_bytes -= bytes; ubo_pool_hits++; return (uint8_t *)p;
    }
    ubo_pool_allocs++;
    return malloc(c < UBO_POOL_CLASSES ? bytes : n);
}
static void ubo_data_free(uint8_t *data, uint64_t n) {
    if (ubo_pool_on()) {
        size_t bytes; unsigned c = ubo_pool_class(n, &bytes);
        if (c < UBO_POOL_CLASSES && ubo_pool_bytes <= UBO_POOL_LIMIT - bytes) {
            UboFree *p = (UboFree *)data; p->next = ubo_free[c]; ubo_free[c] = p;
            ubo_pool_bytes += bytes; return;
        }
    }
    free(data);
}
static void ubo_prune(void) {
    uint64_t done = nvk_completed_fence();
    while (ubo_head < ubo_n && ubo_log[ubo_head].flushed && ubo_log[ubo_head].fence <= done) {
        ubo_pages(ubo_page_all, ubo_log[ubo_head].addr, ubo_log[ubo_head].n, -1);
        ubo_data_free(ubo_log[ubo_head].data, ubo_log[ubo_head].n); ubo_head++;
    }
    if (ubo_head == ubo_n) ubo_head = ubo_n = 0;
}
static void ubo_append(uint64_t addr, const uint8_t *data, uint64_t n, int flushed) {
    ubo_prune();
    ubo_pages(ubo_page_all, addr, n, 1);
    if (!flushed) { if (!ubo_unflushed) memset(ubo_page_unfl, 0, sizeof ubo_page_unfl); ubo_pages(ubo_page_unfl, addr, n, 1); }
    if (ubo_n == ubo_cap) {
        if (ubo_head) { memmove(ubo_log, ubo_log + ubo_head, (ubo_n - ubo_head) * sizeof *ubo_log); ubo_n -= ubo_head; ubo_head = 0; }
        if (ubo_n == ubo_cap) { ubo_cap = ubo_cap ? ubo_cap * 2 : 256; ubo_log = realloc(ubo_log, ubo_cap * sizeof *ubo_log); if (!ubo_log) nvk_die("uniform log allocation"); }
    }
    uint8_t *copy = ubo_data_alloc(n); if (!copy) nvk_die("uniform log allocation");
    memcpy(copy, data, n);
    static uint64_t seq;
    ubo_log[ubo_n++] = (UboLog){addr, n, nvk_pending_fence(), copy, flushed, ++seq};
}
int nvk_ubo_unflushed(uint64_t addr, uint64_t size) {
    if (!ubo_unflushed || addr >= ubo_unflushed_hi || addr + size <= ubo_unflushed_lo) return 0;
    if (!ubo_pages_any(ubo_page_unfl, addr, size)) return 0;
    for (size_t i = ubo_head; i < ubo_n; ++i)
        if (!ubo_log[i].flushed && ubo_log[i].addr < addr + size && addr < ubo_log[i].addr + ubo_log[i].n) return 1;
    return 0;
}
uint64_t nvk_ubo_epoch(void) { return ubo_epoch; }
/* The newest logged update not yet flushed that overlaps the range (its sequence number), 0 when none: a snapshot of
   the range stays valid while this does not change (nvn_vk_draw.c ubo_snapshot). */
uint64_t nvk_ubo_unflushed_seq(uint64_t addr, uint64_t size) {
    if (!ubo_unflushed || addr >= ubo_unflushed_hi || addr + size <= ubo_unflushed_lo) return 0;
    if (!ubo_pages_any(ubo_page_unfl, addr, size)) return 0;
    uint64_t seq = 0;
    for (size_t i = ubo_head; i < ubo_n; ++i)
        if (!ubo_log[i].flushed && ubo_log[i].addr < addr + size && addr < ubo_log[i].addr + ubo_log[i].n && ubo_log[i].seq > seq) seq = ubo_log[i].seq;
    return seq;
}
static void ucap_base(uint64_t addr, uint64_t size, uint8_t *out);   /* below: uniforms as of the submit call */
/* A logged in-pass uniform update lies over the range (nvk_ubo_read would apply it on top of the base bytes). */
static int ubo_log_overlaps(uint64_t addr, uint64_t size) {
    if (!ubo_pages_any(ubo_page_all, addr, size)) return 0;
    for (size_t i = ubo_head; i < ubo_n; ++i) {
        const UboLog *e = &ubo_log[i];
        if (e->addr < addr + size && addr < e->addr + e->n) return 1;
    }
    return 0;
}
void nvk_ubo_read(uint64_t addr, uint64_t size, uint8_t *out) {
    ucap_base(addr, size, out);
    if (!ubo_pages_any(ubo_page_all, addr, size)) return;
    for (size_t i = ubo_head; i < ubo_n; ++i) {
        const UboLog *e = &ubo_log[i];
        uint64_t lo = e->addr > addr ? e->addr : addr, hi = e->addr + e->n < addr + size ? e->addr + e->n : addr + size;
        if (lo < hi) memcpy(out + (lo - addr), e->data + (lo - e->addr), hi - lo);
    }
}
/* In-pass draws consume immutable UBO snapshots; flush has no GPU consumers
 * between its updates. An update fully overwritten by a later update at the
 * same address need not reach the GPU. Keep every log entry for CPU overlays
 * and snapshot lifetimes. Hash collisions only miss an elimination. */
#define UBO_COVER_N 1024
static struct { uint64_t gen, addr, n, seq; } ubo_cover[UBO_COVER_N];
static uint64_t ubo_cover_gen, ubo_gpu_updates, ubo_gpu_elided;
static int ubo_coalesce_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_VK_UBO_COALESCE"); on = !(e && !strcmp(e, "0")); }
    return on;
}
static unsigned ubo_cover_slot(uint64_t addr) { return (unsigned)((addr >> 2) ^ (addr >> 12)) & (UBO_COVER_N - 1); }
static void ubo_flush_plan(void) {
    if (++ubo_cover_gen == 0) { memset(ubo_cover, 0, sizeof ubo_cover); ubo_cover_gen = 1; }
    for (size_t i = ubo_n; i > ubo_head;) {
        const UboLog *e = &ubo_log[--i];
        if (e->flushed) continue;
        typeof(ubo_cover[0]) *c = &ubo_cover[ubo_cover_slot(e->addr)];
        if (c->gen != ubo_cover_gen) *c = (typeof(*c)){ubo_cover_gen, e->addr, e->n, e->seq};
        else if (c->addr == e->addr && e->n > c->n) { c->n = e->n; c->seq = e->seq; }
    }
}
static int ubo_flush_shadowed(const UboLog *e) {
    const typeof(ubo_cover[0]) *c = &ubo_cover[ubo_cover_slot(e->addr)];
    return c->gen == ubo_cover_gen && c->addr == e->addr && c->n >= e->n && c->seq > e->seq;
}
static void ubo_flush(void) {   /* outside a pass: the logged in-pass updates reach the pool, in order */
    if (!ubo_unflushed) return;
    int coalesce = ubo_coalesce_on();
    if (coalesce) ubo_flush_plan();
    for (size_t i = ubo_head; i < ubo_n; ++i) {
        UboLog *e = &ubo_log[i]; VkBuffer b; uint64_t off;
        if (e->flushed) continue;
        if (coalesce && ubo_flush_shadowed(e)) { ubo_gpu_elided++; }
        else {
            if (!resolve(e->addr, e->n, &b, &off)) nvk_die("NVN uniform update outside memory pools");
            vkCmdUpdateBuffer(cb, b, off, e->n, e->data); ubo_gpu_updates++;
        }
        e->flushed = 1; e->fence = nvk_pending_fence(); nvk_buffer_write_bump();
    }
    memset(ubo_page_unfl, 0, sizeof ubo_page_unfl);
    ubo_unflushed = 0; ubo_unflushed_lo = UINT64_MAX; ubo_unflushed_hi = 0;
    nvk_barrier(cb);
}
static int ubo_inpass_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_VK_UBO_INPASS"); on = !(e && !strcmp(e, "0")); }
    return on;
}

/* After a pass: only what it wrote must reach later reads. A pass that wrote nothing (only sampled, tested
 * depth or counted samples) needs no barrier; otherwise the source scope is its attachment writes (and shader
 * stages when it stored to memory) instead of all commands and all memory writes, so the driver flushes only the
 * caches that hold them. TOTK_VK_PASS_BARRIER=0: the full barrier, as before. */
static uint64_t pass_barriers_skipped, pass_barriers_narrow;
static void pass_end_barrier(void) {
    static int on = -1;
    static unsigned live_TOTK_VK_PASS_BARRIER; if (on < 0 || live_TOTK_VK_PASS_BARRIER != nvk_live_gen) { live_TOTK_VK_PASS_BARRIER = nvk_live_gen; const char *e = nvk_live_getenv("TOTK_VK_PASS_BARRIER"); on = !(e && !strcmp(e, "0")); }
    if (!on) { nvk_barrier(cb); return; }
    if (!pass_written && !pass_storage_written) { pass_barriers_skipped++; return; }
    VkPipelineStageFlags src = 0; VkAccessFlags access = 0;
    if (pass_written & 0xff) { src |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; access |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; }
    if (pass_written & 0x100) { src |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT; access |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT; }
    if (pass_storage_written) { src |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT; access |= VK_ACCESS_SHADER_WRITE_BIT; }
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = access,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(cb, src, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    pass_barriers_narrow++;
}

static void predict_end(int cause);
static void end_rendering(int cause) {
    if (!rendering && pend.any) begin_rendering();
    if (!rendering) return;
    pass_ends[cause]++;
    query_end(cb);
    vkCmdEndRendering(cb);
    pt_end(cb, ring_cur);
    rendering = 0;
    predict_end(cause);
    pass_end_barrier();
    tiny_record(cb, pass_written);
    ubo_flush();
}
/* Why transfers end passes (measurement): copy buffer<->texture, texture copy, texture clear, buffer copy,
   uniform update, game barrier; and the NVN barrier bits seen while a pass was open. */
enum { XF_B2T, XF_T2T, XF_CLEAR, XF_COPY, XF_UBO, XF_BARRIER, XF_N };
static uint64_t xfer_ends[XF_N], barrier_bits_open[16], barriers_open, barriers_total, barriers_kept;
/* An NVN barrier inside an open pass mostly says "what was drawn will be sampled next"
   (ORDER_FRAGMENTS | INVALIDATE_TEXTURE, ~115 per frame in the open world). This layer already orders that:
   a target change ends the pass with a barrier, and sampling an attachment of the open pass splits it
   (feedback split). Only storage writes inside the pass, indirect data and L2 (CPU visibility) need the
   pass to end. TOTK_VK_BARRIER_KEEP=0: every barrier ends the pass, as before. */
void nvk_pass_storage_write(int image) { if (rendering) { pass_storage_written = 1; pass_image_stored |= image; } }
int nvk_pass_storage_written(void) { return rendering && pass_storage_written; }
/* Buffer ranges stored to in the open pass (vertex_storage_split, nvn_vk_draw.c); past 32 any range counts. */
static uint64_t pwr_lo[32], pwr_hi[32]; static unsigned pwr_n; static int pwr_all;
void nvk_pass_storage_range(uint64_t addr, uint64_t size) {
    if (!rendering || !addr || !size) return;
    for (unsigned i = 0; i < pwr_n; ++i) if (pwr_lo[i] == addr && pwr_hi[i] == addr + size) return;
    if (pwr_n == 32) { pwr_all = 1; return; }
    pwr_lo[pwr_n] = addr; pwr_hi[pwr_n++] = addr + size;
}
int nvk_pass_storage_overlaps(uint64_t addr, uint64_t size) {
    if (!rendering || !pass_storage_written) return 0;
    if (pwr_all) return 1;
    for (unsigned i = 0; i < pwr_n; ++i) if (addr < pwr_hi[i] && pwr_lo[i] < addr + size) return 1;
    return 0;
}
/* Sampling an attachment after a store in the pass: only an image store can have written the attachment. Buffer
   stores (TOTK: ~260 G-buffer shaders mark their screen tile in a tile-class buffer) cannot, so they no longer split
   the pass when a decal then samples depth or normals. TOTK_VK_FEEDBACK_BUFFER_STORES=0: any store splits, as before. */
static int buffer_stores_split(void) {
    static int on = -1;
    static unsigned live_TOTK_VK_FEEDBACK_BUFFER_STORES; if (on < 0 || live_TOTK_VK_FEEDBACK_BUFFER_STORES != nvk_live_gen) { live_TOTK_VK_FEEDBACK_BUFFER_STORES = nvk_live_gen; const char *e = nvk_live_getenv("TOTK_VK_FEEDBACK_BUFFER_STORES"); on = e && !strcmp(e, "0"); }
    return on;
}
static int barrier_keeps_pass(int32_t bits) {
    static int on = -1;
    static unsigned live_TOTK_VK_BARRIER_KEEP; if (on < 0 || live_TOTK_VK_BARRIER_KEEP != nvk_live_gen) { live_TOTK_VK_BARRIER_KEEP = nvk_live_gen; const char *e = nvk_live_getenv("TOTK_VK_BARRIER_KEEP"); on = !(e && !strcmp(e, "0")); }
    return on && rendering && !pass_storage_written && !(bits & (8 | 256)) && nvk_feedback_split_on();
}
static void transfer_barrier_why(int why) {
    if (rendering) xfer_ends[why]++;
    end_rendering(NVK_END_TRANSFER); nvk_barrier(cb);
}
static void transfer_barrier(void) { transfer_barrier_why(XF_BARRIER); }

/* TOTK re-sets identical targets many times per frame (nvn_mtl.m keeps the
 * Metal pass open then). Every end stores and reloads all attachments, which
 * costs a full GMEM round trip on Adreno. TOTK_VK_RT_CONTINUE=0: off. */
static int rt_continue_on(void) {
    static int on = -1;
    static unsigned live_TOTK_VK_RT_CONTINUE; if (on < 0 || live_TOTK_VK_RT_CONTINUE != nvk_live_gen) { live_TOTK_VK_RT_CONTINUE = nvk_live_gen; const char *e = nvk_live_getenv("TOTK_VK_RT_CONTINUE"); on = !(e && !strcmp(e, "0")); }
    return on;
}
/* A re-initialised target object at the same storage holds the same (shared) image. */
static int same_target(const tex_t *t, uint64_t serial, int slot) {
    if (t->serial == serial) return 1;
    const NvkImage *img = pass_img[slot] ? nvk_image_existing(t->serial) : NULL;
    return img && img == pass_img[slot];
}
/* The attached serials and their images (images may not exist yet for a folded clear). */
static void pass_targets_note(void) {
    for (int i = 0; i < 8; ++i) {
        pass_color[i] = i < cur_rt.n && cur_rt.color[i] ? cur_rt.color[i]->serial : 0;
        pass_img[i] = pass_color[i] ? nvk_image_existing(pass_color[i]) : NULL;
    }
    pass_depth = cur_rt.depth ? cur_rt.depth->serial : 0;
    pass_img[8] = pass_depth ? nvk_image_existing(pass_depth) : NULL;
}
static int view_same(int ha, const view_t *a, int hb, const view_t *b) { return ha == hb && (!ha || !memcmp(a, b, sizeof *a)); }
static int rt_same_as_pass(const p_set_rt *r) {
    if (r->n != cur_rt.n || (r->depth != NULL) != (pass_depth != 0) || (r->depth && !same_target(r->depth, pass_depth, 8))) return 0;
    if (!r->n && !r->depth) return 0;   /* the viewport sizes a pass without attachments */
    if (!view_same(r->has_dview, &r->dview, cur_rt.has_dview, &cur_rt.dview)) return 0;
    for (int i = 0; i < r->n; ++i) {
        if ((r->color[i] != NULL) != (pass_color[i] != 0) || (r->color[i] && !same_target(r->color[i], pass_color[i], i))) return 0;
        if (!view_same(r->has_cview[i], &r->cview[i], cur_rt.has_cview[i], &cur_rt.cview[i])) return 0;
    }
    return 1;
}

/* ---------- Segmented submission ----------
 * A NVN submission reached the GPU as one command buffer after its whole replay, so the GPU idled while the CPU
 * translated and the game then waited for the GPU: frame time = replay + GPU (Test/20261022_snelheidsmodel).
 * Now every TOTK_VK_SEGMENT_DRAWS draws (default 200; 0 = off) the replay hands the GPU what it has recorded, at
 * a target change, where no pass is open and no caller holds the command buffer. The first segment of a submission
 * goes after TOTK_VK_SEGMENT_FIRST draws (default 30) so the GPU starts early (the macOS port: 60, then 75).
 * 23 Oct, Thor: with 100/400 the GPU waited 4-5 ms per frame between segments for the CPU, hence smaller ones. Segments go to the same queue
 * in order and without a signal: barriers order across them as within one buffer, and only the last one signals
 * the submission's timeline value, so fences, post-ops, arenas and slot reuse keep meaning "all of it done".
 * State bound in a command buffer is not carried over (nvk_cmd_epoch). The GPU-time timestamps bracket the whole
 * submission, so with segments they include the GPU waiting for the next segment. */
static uint64_t seg_mark, segments_total, submits_total;
static unsigned segment_every(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("TOTK_VK_SEGMENT_DRAWS"); v = e && *e ? atoi(e) : 200; if (v < 0) v = 0;
        VKLOG("segmented submission: %s", v ? "on" : "off (TOTK_VK_SEGMENT_DRAWS=0)"); if (v) VKLOG("segmented submission: every %d draws, at a target change", v); }
    return (unsigned)v;
}
static unsigned segment_first(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("TOTK_VK_SEGMENT_FIRST"); v = e && *e ? atoi(e) : 30; if (v < 1) v = 1; }
    return (unsigned)v;
}
static void segment_cut(void);
static void segment_maybe(void) {
    unsigned every = segment_every(), i = ring_cur;
    if (every && !ring[i].nseg && segment_first() < every) every = segment_first();
    if (!every || !cb || rendering || pend.any || nvk_draws_issued() - seg_mark < every) return;
    if (ring[i].nseg + 1 >= SEGMENTS) return;
    segment_cut();
}
/* The target-change hook alone misses pass ends caused by feedback or texture preparation: in the heavy Thor
   scene it only hands over ~7 segments despite ~6000 draws. At explicit draw/dispatch checkpoints the caller is
   about to refresh its local command-buffer handle, so a cut cannot strand a stale handle on its stack.
   Never end a rendering instance here: only reuse an already safe boundary. Opt-in pending device A/B. */
static uint64_t checkpoint_cuts;
void nvk_segment_checkpoint(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_VK_SEGMENT_CHECKPOINTS"); on = e && !strcmp(e, "1"); }
    if (!on) return;
    uint64_t before = segments_total;
    segment_maybe();
    checkpoint_cuts += segments_total - before;
}
/* Everything recorded so far reaches the GPU and completes (GPU -> memory writeback of aliased images). 0 when the
   open submission has no segment left; then nothing was submitted. */
int nvk_submit_wait(void) {
    end_rendering(NVK_END_TEXTURE);
    if (cb) {
        if (ring[ring_cur].nseg + 1 >= SEGMENTS) return 0;
        segment_cut();
    }
    nvk_record_sync();   /* the recorder may still be submitting the segment; do not hold its queue lock */
    nvk_queue_lock(); VkResult r = vkQueueWaitIdle(nvk_queue); nvk_queue_unlock();
    VKCHECK(r, "wait for the GPU before a writeback");
    return 1;
}
static void segment_buffer_ready(unsigned i, unsigned n) {
    if (!ring[i].seg[n]) {
        VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = ring[i].pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        VKCHECK(vkAllocateCommandBuffers(nvk_dev, &cai, &ring[i].seg[n]), "allocate segment command buffer");
    }
}
static void segment_cut(void) {
    unsigned i = ring_cur, n = ring[i].nseg;
    int async = nvk_record_segments_on();
    /* Allocation touches the command pool and synchronizes the recorder. Do it before handing that pool's old
       buffer to the worker. In warmed ring slots these buffers are already allocated, so there is no wait. */
    if (async) segment_buffer_ready(i, n);
    seg_mark_end(cb, i);
    if (totk_tl_on()) totk_tl(TL_COMMIT, totk_tl_commit_id(), 0);   /* TOTK_TIMELINE: a segment leaves the replay */
    if (!async || !nvk_record_segment(nvk_queue, cb, ring[i].seg[n])) {
        VKCHECK(vkEndCommandBuffer(cb), "end command buffer segment");
        VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
        nvk_queue_lock(); VkResult sr = vkQueueSubmit(nvk_queue, 1, &si, VK_NULL_HANDLE); nvk_queue_unlock();
        VKCHECK(sr, "submit NVN segment");
        segment_buffer_ready(i, n);
        VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        VKCHECK(vkBeginCommandBuffer(ring[i].seg[n], &bi), "begin segment command buffer");
    }
    ring[i].nseg++;
    cb = ring[i].seg[n]; ++cmd_epoch;
    seg_mark_start(cb, i);
    seg_mark = nvk_draws_issued(); segments_total++;
}

/* ---------- Discards ----------
 * NVN DiscardColor / DiscardDepthStencil: the attachment's contents become undefined. When it comes before the pass
 * begins (begin_rendering is lazy: at the first draw or clear), that pass need not load the attachment from memory
 * (LOAD_OP_DONT_CARE, and no stale upload of it). A discard after drawing would want STORE_OP_DONT_CARE, but the store
 * op is fixed when the pass begins, so those are only counted. Target changes forget pending discards.
 * TOTK_VK_DISCARD=0: ignored, as before. */
static uint64_t discard_serial[9];   /* per slot (8: depth/stencil): the image a discard applies to at the next begin */
static uint64_t discards_total, discards_open, discards_load;
static int discard_on(void) {
    static int on = -1;
    static unsigned live_TOTK_VK_DISCARD; if (on < 0 || live_TOTK_VK_DISCARD != nvk_live_gen) { live_TOTK_VK_DISCARD = nvk_live_gen; const char *e = nvk_live_getenv("TOTK_VK_DISCARD"); on = !(e && !strcmp(e, "0")); }
    return on;
}
static void discard_op(int slot) {
    discards_total++;
    if (!discard_on() || !have_rt || slot < 0 || slot > 8) return;
    const tex_t *t = slot == 8 ? cur_rt.depth : slot < cur_rt.n ? cur_rt.color[slot] : NULL;
    if (!t) return;
    if (rendering) { discards_open++; return; }   /* the pass already loaded it */
    discard_serial[slot] = t->serial;
}
static int discarded(int slot, const tex_t *t) { return discard_on() && t && discard_serial[slot] == t->serial; }

/* ---------- Attachment use prediction ----------
 * Every pass stored all its attachments back to memory and loaded them (Adreno: a GMEM resolve and restore per
 * attachment when the driver renders in tiles), also those its draws only read (depth tested, not written: decals,
 * transparents) or never touched (a depth buffer bound to a post-processing pass). Which slots a pass writes or uses
 * is known only once it ended, so it is predicted from the same pass in earlier frames: the same images (not serials:
 * TOTK re-creates target objects every frame) as the n-th pass on them in the frame. After UP_STREAK consecutive
 * frames in which it never wrote a slot, that slot gets STORE_OP_NONE; never used at all (and VK_*_load_store_op_none
 * enabled): LOAD_OP_NONE too. The prediction is enforced, not trusted: a draw or clear about to write such a slot (or
 * use an unloaded one) first ends the pass (NVK_END_PREDICT), with nothing written, so memory still holds the slot's
 * contents, and the pass continues normally; that frame's observation stops the prediction. A wrong guess therefore
 * costs one pass split, never pixels. TOTK_VK_ATTACH_PREDICT=0: every slot loaded and stored, as before. */
int android_vk_load_op_none;   /* android_gfx_bridge.c */
#define UP_N 4096
#define UP_STREAK 3
typedef struct { uint64_t key, frame, acc_frame; uint16_t obs_w, obs_u, acc_w, acc_u; uint8_t streak; } UsePred;
static UsePred *up_tab;
static struct { uint64_t sig, frame; unsigned count; } up_ord[1024];   /* passes on the same images this frame */
static UsePred *pass_pred;              /* the open pass's entry */
static int pass_resume;                 /* the next begin continues a pass a misprediction ended */
static uint16_t pass_read, pass_store_none, pass_load_none, next_w, next_r;
static uint64_t up_predicted, up_store_none, up_load_none, up_mispredicted;
static int predict_on(void) {
    static int on = -1;
    static unsigned live_TOTK_VK_ATTACH_PREDICT; if (on < 0 || live_TOTK_VK_ATTACH_PREDICT != nvk_live_gen) { live_TOTK_VK_ATTACH_PREDICT = nvk_live_gen; const char *e = nvk_live_getenv("TOTK_VK_ATTACH_PREDICT"); on = !(e && !strcmp(e, "0")); if (on && !(up_tab = calloc(UP_N, sizeof *up_tab))) on = 0; }
    return on;
}
static uint64_t up_mix(uint64_t h, uint64_t v) { h ^= v; h *= 0xff51afd7ed558ccdull; return h ^ h >> 29; }
static uint64_t up_view(uint64_t h, int has, const view_t *v) {
    if (!has) return up_mix(h, 0);
    const uint8_t *b = (const uint8_t *)v;
    for (size_t i = 0; i < sizeof *v; ++i) h = up_mix(h, b[i]);
    return h;
}
/* At begin_rendering, images attached: slots to store with STORE_OP_NONE (*store_none) and to leave unloaded too. */
static void predict(uint16_t attached, uint16_t cleared, uint16_t *store_none, uint16_t *load_none) {
    *store_none = *load_none = 0;
    uint16_t nw = next_w, nr = next_r; next_w = next_r = 0;
    if (!predict_on()) { pass_pred = NULL; return; }
    if (pass_resume) { pass_resume = 0; return; }   /* same entry, nothing elided */
    uint64_t frame = atomic_load(&current_frame), sig = up_mix(0x51ed27ull, (uint64_t)cur_rt.n);
    for (int i = 0; i < cur_rt.n && i < 8; ++i) {
        sig = up_mix(sig, cur_rt.color[i] ? (uint64_t)(uintptr_t)nvk_image_existing(cur_rt.color[i]->serial) : 0);
        sig = up_view(sig, cur_rt.color[i] && cur_rt.has_cview[i], &cur_rt.cview[i]);
    }
    sig = up_mix(sig, cur_rt.depth ? (uint64_t)(uintptr_t)nvk_image_existing(cur_rt.depth->serial) : 0);
    sig = up_view(sig, cur_rt.depth && cur_rt.has_dview, &cur_rt.dview);
    typeof(up_ord[0]) *o = &up_ord[sig & 1023];
    if (o->sig != sig || o->frame != frame) { o->sig = sig; o->frame = frame; o->count = 0; }
    uint64_t key = up_mix(sig, o->count++) | 1;
    UsePred *e = &up_tab[key & (UP_N - 1)];
    pass_pred = e;
    if (e->key != key) { memset(e, 0, sizeof *e); e->key = key; e->frame = frame; return; }
    if (e->frame != frame) {   /* fold the previous frame's observation */
        if (e->streak && e->acc_frame + 1 == e->frame) { if (e->streak < 255) e->streak++; e->acc_w |= e->obs_w; e->acc_u |= e->obs_u; }
        else { e->streak = 1; e->acc_w = e->obs_w; e->acc_u = e->obs_u; }
        e->acc_frame = e->frame; e->frame = frame; e->obs_w = e->obs_u = 0;
    }
    if (e->streak < UP_STREAK || e->acc_frame + 1 != frame) return;
    *store_none = attached & ~cleared & ~e->acc_w & ~nw;
    if (android_vk_load_op_none) *load_none = *store_none & ~e->acc_u & ~nr;
    if (*store_none) up_predicted++;
}
static void predict_end(int cause) {
    if (pass_pred) { pass_pred->obs_w |= pass_written; pass_pred->obs_u |= pass_written | pass_read; }
    if (cause == NVK_END_PREDICT) pass_resume = 1; else pass_pred = NULL;
    pass_store_none = pass_load_none = 0;
}
/* Before an attachment access (draw, clear): slots it writes and slots it reads (depth/stencil test, framebuffer fetch).
   An open pass that elided one of them ends first; otherwise the reads count for the open or the next pass. */
void nvk_pass_access(uint16_t writes, uint16_t reads) {
    if (rendering && ((writes & pass_store_none) || ((writes | reads) & pass_load_none))) { up_mispredicted++; end_rendering(NVK_END_PREDICT); }
    if (rendering) pass_read |= reads;
    else { next_w |= writes; next_r |= reads; }
}

static uint64_t rt_epoch;   /* every target change: shader variant signatures depend on the target formats */
uint64_t nvk_rt_epoch(void) { return rt_epoch; }
static void set_render_targets(const p_set_rt *r) {
    rt_epoch++;
    if ((rendering || pend.any) && rt_continue_on() && rt_same_as_pass(r)) { cur_rt = *r; rt_continues++; return; }
    end_rendering(NVK_END_RT);
    pass_resume = 0; pass_pred = NULL; next_w = next_r = 0;
    memset(discard_serial, 0, sizeof discard_serial);
    segment_maybe();
    cur_rt = *r; have_rt = 1;
}

static void attach(VkRenderingAttachmentInfo *out, const tex_t *t, const view_t *v, int discard, uint32_t *w, uint32_t *h) {
    NvkImage *img = discard ? nvk_image_discard(t, cb) : nvk_image(t, cb);
    nvk_image_written(img);
    if (nvk_image_aspect(img) == VK_IMAGE_ASPECT_COLOR_BIT) writeback_note(t);
    uint32_t level, layers;
    out->sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    out->imageView = nvk_image_attachment_view(img, v, &level, &layers);
    out->imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    out->loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    out->storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkExtent2D e = nvk_image_level_extent(img, level);
    if (e.width < *w) *w = e.width;
    if (e.height < *h) *h = e.height;
}

static void begin_rendering(void) {
    if (rendering) return;
    if (!have_rt) nvk_die("NVN rendering without render targets");
    if (scene_pending)   /* the first draw to a scene target: its deferred clear runs now, as the load op */
        for (int i = 0; i < cur_rt.n && i < 8; ++i) {
            int k = cur_rt.color[i] ? scene_find(cur_rt.color[i]->serial, nvk_image_existing(cur_rt.color[i]->serial)) : -1;
            if (k < 0) continue;
            if (!pend.color[i]) { pend.color[i] = 1; pend.cc[i] = scene_defer[k].cc; fold_targets(); }
            scene_defer[k].valid = 0; scene_pending--;
            if (++scene_materialized == 1 || scene_materialized % 5000 == 0) scene_log("run as load op");
        }
    VkRenderingAttachmentInfo colors[8] = {{0}}, depth = {0}, stencil = {0};
    uint32_t w = UINT32_MAX, h = UINT32_MAX;
    pass_tiny = 0;
    for (int i = 0; i < cur_rt.n; ++i) {
        colors[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        int gone = i < 8 && !pend.color[i] && discarded(i, cur_rt.color[i]);
        if (cur_rt.color[i]) attach(&colors[i], cur_rt.color[i], cur_rt.has_cview[i] ? &cur_rt.cview[i] : NULL, pend.color[i] || gone, &w, &h);
        if (i < 8 && cur_rt.color[i] && tiny_target(cur_rt.color[i], cur_rt.has_cview[i], &cur_rt.cview[i])) { pass_tiny |= (uint8_t)(1u << i); pass_tiny_tex[i] = *cur_rt.color[i]; tiny_noted++; }
        if (cur_rt.color[i] && pend.color[i]) { colors[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; colors[i].clearValue.color = pend.cc[i]; }
        else if (gone) { colors[i].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; discards_load++; }
    }
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .layerCount = 1,
        .colorAttachmentCount = (uint32_t)cur_rt.n, .pColorAttachments = colors};
    if (cur_rt.depth) {
        attach(&depth, cur_rt.depth, cur_rt.has_dview ? &cur_rt.dview : NULL, discarded(8, cur_rt.depth), &w, &h);
        VkImageAspectFlags aspect = nvk_image_aspect(nvk_image(cur_rt.depth, cb));
        if (discarded(8, cur_rt.depth)) {   /* both aspects; a pending clear below still wins */
            depth.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; discards_load++;
        }
        stencil = depth;
        if (pend.depth) { depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; depth.clearValue.depthStencil.depth = pend.d; }
        if (pend.stencil) { stencil.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; stencil.clearValue.depthStencil.stencil = pend.s; }
        if (aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ri.pStencilAttachment = &stencil;
        if (aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ri.pDepthAttachment = &depth;
    }
    {   uint16_t attached = cur_rt.depth ? 0x100 : 0, cleared = (pend.depth || pend.stencil) ? 0x100 : 0, sn, ln;
        for (int i = 0; i < cur_rt.n && i < 8; ++i) if (cur_rt.color[i]) { attached |= (uint16_t)(1u << i); if (pend.color[i]) cleared |= (uint16_t)(1u << i); }
        uint16_t reads = next_r;
        predict(attached, cleared, &sn, &ln);
        for (int i = 0; i < cur_rt.n && i < 8; ++i) {
            if (sn >> i & 1) { colors[i].storeOp = VK_ATTACHMENT_STORE_OP_NONE; up_store_none++; }
            if (ln >> i & 1) { colors[i].loadOp = VK_ATTACHMENT_LOAD_OP_NONE; up_load_none++; }
        }
        if (sn & 0x100) { depth.storeOp = stencil.storeOp = VK_ATTACHMENT_STORE_OP_NONE; up_store_none++; }
        if (ln & 0x100) { depth.loadOp = stencil.loadOp = VK_ATTACHMENT_LOAD_OP_NONE; up_load_none++; }
        pass_store_none = sn; pass_load_none = ln; pass_read = reads; }
    pass_written = (uint16_t)((pend.depth || pend.stencil) && cur_rt.depth ? 0x100 : 0); pass_sampled = 0; pass_fetch_read = 0;   /* clear load ops write */
    memset(discard_serial, 0, sizeof discard_serial);   /* consumed by this pass */
    for (int i = 0; i < cur_rt.n && i < 8; ++i) if (cur_rt.color[i] && pend.color[i]) pass_written |= (uint16_t)(1u << i);
    memset(&pend, 0, sizeof pend);
    if (w == UINT32_MAX) {   /* no attachments: the draws only write storage; the viewport bounds the area */
        float vw = nvk_S.has_vp ? nvk_S.vp[0][0] + nvk_S.vp[0][2] : 1, vh = nvk_S.has_vp ? nvk_S.vp[0][1] + nvk_S.vp[0][3] : 1;
        w = vw < 1 ? 1 : vw > 16384 ? 16384 : (uint32_t)(vw + 0.5f);
        h = vh < 1 ? 1 : vh > 16384 ? 16384 : (uint32_t)(vh + 0.5f);
    }
    ri.renderArea = (VkRect2D){{0, 0}, {w, h}};
    if (pt_pool) pt_begin(cb, ring_cur, (PassMeta){w, h, cur_rt.n, cur_rt.n && cur_rt.color[0] ? (int)cur_rt.color[0]->d.format : -1,
        cur_rt.depth ? (int)cur_rt.depth->d.format : -1, {0}});
    vkCmdBeginRendering(cb, &ri);
    rendering = 1;
    pass_count++;
    pass_targets_note();
    query_begin(cb);
    pass_storage_written = pass_image_stored = 0; pwr_n = 0; pwr_all = 0;
    render_extent = (VkExtent2D){w, h};
}

void nvk_rendering_begin(void) { begin_rendering(); }
void nvk_rendering_end(int cause) { end_rendering(cause); }

/* ---------- Attachments sampled in the open pass ----------
 * Every draw sampling an attachment of the open pass used to end it (feedback split, ~283 pass ends per frame in
 * the open world): after the split the next draw sampling it split again. A split is needed only when the pass
 * holds something memory does not, or will: the attachment was written in this pass (a draw, a clear, a clear
 * load op), the draw itself writes it, or a later draw writes one sampled earlier in the pass (nvk_pass_before_
 * draw). Otherwise the sampled memory is what the pass loaded and no command of the pass writes that attachment,
 * which Vulkan allows (VUID-vkCmdDraw-None-06538). Bits 0-7: colour slots, bit 8: depth/stencil.
 * TOTK_VK_FEEDBACK_TRACK=0: every sampled attachment splits, as before. */
static uint64_t fb_written, fb_self, fb_kept;
static int feedback_track_on(void) {
    static int on = -1;
    static unsigned live_TOTK_VK_FEEDBACK_TRACK; if (on < 0 || live_TOTK_VK_FEEDBACK_TRACK != nvk_live_gen) { live_TOTK_VK_FEEDBACK_TRACK = nvk_live_gen; const char *e = nvk_live_getenv("TOTK_VK_FEEDBACK_TRACK"); on = !(e && !strcmp(e, "0")); }
    return on;
}
/* The slots of the current targets (open pass or the next one) that hold this texture's image. */
uint16_t nvk_rt_slots(uint64_t serial, const NvkImage *img) {
    if (!have_rt || !serial) return 0;
    uint16_t m = 0;
    for (int i = 0; i < cur_rt.n && i < 8; ++i) {
        const tex_t *c = cur_rt.color[i];
        if (c && (c->serial == serial || (img && (rendering ? pass_img[i] : nvk_image_existing(c->serial)) == img))) m |= (uint16_t)(1u << i);
    }
    const tex_t *d = cur_rt.depth;
    if (d && (d->serial == serial || (img && (rendering ? pass_img[8] : nvk_image_existing(d->serial)) == img))) m |= 0x100;
    return m;
}
/* A draw samples attachments (slots) of the open pass: 1 when the pass must end first. */
int nvk_feedback_split_needed(uint16_t slots, uint16_t draw_writes) {
    if (!feedback_track_on()) return 1;
    if ((slots & pass_written) || pass_image_stored || (pass_storage_written && buffer_stores_split())) { fb_written++; return 1; }
    if (slots & draw_writes) { fb_self++; return 1; }
    fb_kept++;
    return 0;
}
/* Before a draw that writes these slots: one sampled earlier in this pass ends it (write after read). Depth read
   as an input attachment (TOTK_VK_FETCH_DEPTH) instead gets an execution dependency inside the pass: the earlier
   fragment shaders' reads before this draw's depth tests overwrite the value. */
static uint64_t fb_war, war_barriers;
void nvk_pass_before_draw(uint16_t writes) {
    if (rendering && feedback_track_on() && (writes & pass_sampled)) { fb_war++; end_rendering(NVK_END_FEEDBACK); }
    if (rendering && (writes & pass_fetch_read & 0x100)) {
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            VK_DEPENDENCY_BY_REGION_BIT, 0, NULL, 0, NULL, 0, NULL);
        pass_fetch_read &= (uint16_t)~0x100; war_barriers++;
    }
}
void nvk_pass_after_draw(uint16_t writes, uint16_t sampled) { pass_written |= writes; pass_sampled |= sampled; }
/* Framebuffer fetch: earlier draws of this pass wrote colour attachments the next draw reads as input attachments. */
static uint64_t local_read_barriers;
void nvk_pass_local_read_barrier(uint16_t attachments) {
    if (!rendering) return;
    if (attachments & 0x100) pass_fetch_read |= 0x100;
    if (!(attachments & pass_written)) return;
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT};
    VkPipelineStageFlags src = 0;
    if (attachments & pass_written & 0xff) { src |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; mb.srcAccessMask |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; }
    if (attachments & pass_written & 0x100) {   /* depth (TOTK_VK_FETCH_DEPTH) */
        src |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT; mb.srcAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }
    vkCmdPipelineBarrier(cb, src, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_DEPENDENCY_BY_REGION_BIT, 1, &mb, 0, NULL, 0, NULL);
    local_read_barriers++;
}
/* The view colour attachment j has (or will have) in the pass: the one begin_rendering attaches. */
VkImageView nvk_rt_color_view(int j) {
    if (!have_rt || j < 0 || j >= cur_rt.n || !cur_rt.color[j]) return VK_NULL_HANDLE;
    uint32_t level, layers;
    return nvk_image_attachment_view(nvk_image(cur_rt.color[j], cb), cur_rt.has_cview[j] ? &cur_rt.cview[j] : NULL, &level, &layers);
}
/* The depth attachment as an input attachment (TOTK_VK_FETCH_DEPTH): depth aspect only, same level and layer. */
VkImageView nvk_rt_depth_input_view(void) {
    if (!have_rt || !cur_rt.depth) return VK_NULL_HANDLE;
    return nvk_image_depth_input_view(nvk_image(cur_rt.depth, cb), cur_rt.has_dview ? &cur_rt.dview : NULL);
}
/* Nonzero (and unique) while a pass is open: what draws may key per-pass caches on. */
uint64_t nvk_open_pass_serial(void) { return rendering ? pass_count : 0; }
uint16_t nvk_pass_written_slots(void) { return rendering ? pass_written : 0; }   /* split diagnostics */
int nvk_rendering_attached_image(uint64_t serial, const NvkImage *img) {
    if ((!rendering && !pend.any) || !serial) return 0;
    int hit = serial == pass_depth || (img && img == pass_img[8]);
    for (int i = 0; i < 8 && !hit; ++i) hit = pass_color[i] == serial || (img && img == pass_img[i]);
    return hit ? (rendering ? 1 : 2) : 0;
}
int nvk_rendering_attached(uint64_t serial) { return nvk_rendering_attached_image(serial, nvk_image_existing(serial)); }
const p_set_rt *nvk_render_targets(void) { return have_rt ? &cur_rt : NULL; }
VkExtent2D nvk_render_extent(void) { return render_extent; }
int nvk_scissor(p_scissor *out) { if (have_scissor) *out = scissor; return have_scissor; }

/* NVN clears are limited by the scissor rectangle. */
static int clear_rect(VkClearRect *out) {
    int32_t x0 = 0, y0 = 0, x1 = (int32_t)render_extent.width, y1 = (int32_t)render_extent.height;
    if (have_scissor) {
        if (scissor.x > x0) x0 = scissor.x;
        if (scissor.y > y0) y0 = scissor.y;
        if (scissor.x + scissor.w < x1) x1 = scissor.x + scissor.w;
        if (scissor.y + scissor.h < y1) y1 = scissor.y + scissor.h;
    }
    if (x1 <= x0 || y1 <= y0) return 0;
    *out = (VkClearRect){{{x0, y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}}, 0, 1};
    return 1;
}

static int clear_load_on(void) {
    static int on = -1;
    static unsigned live_TOTK_VK_CLEAR_LOAD; if (on < 0 || live_TOTK_VK_CLEAR_LOAD != nvk_live_gen) { live_TOTK_VK_CLEAR_LOAD = nvk_live_gen; const char *e = nvk_live_getenv("TOTK_VK_CLEAR_LOAD"); on = !(e && !strcmp(e, "0")); }
    return on;
}
/* A clear can become the load op when no pass is open and the scissor covers
 * the render area the next pass will have. */
static int clear_foldable(void) {
    if (rendering || !have_rt || !clear_load_on()) return 0;
    uint32_t w = UINT32_MAX, h = UINT32_MAX;
    for (int i = 0; i <= cur_rt.n; ++i) {
        const tex_t *t = i < cur_rt.n ? cur_rt.color[i] : cur_rt.depth;
        if (!t) continue;
        const view_t *v = i < cur_rt.n ? (cur_rt.has_cview[i] ? &cur_rt.cview[i] : NULL) : (cur_rt.has_dview ? &cur_rt.dview : NULL);
        uint32_t level = v && v->set_levels ? v->base_level : 0;
        if (level >= 32) return 0;
        uint32_t tw = t->d.width >> level, th = t->d.height >> level;
        if (tw && tw < w) w = tw;
        if (th && th < h) h = th;
    }
    if (w == UINT32_MAX) return 0;
    return !have_scissor || (scissor.x <= 0 && scissor.y <= 0 && (int64_t)scissor.x + scissor.w >= (int64_t)w && (int64_t)scissor.y + scissor.h >= (int64_t)h);
}
static void fold_targets(void) {
    pend.any = 1; clears_folded++;
    pass_targets_note();
}

/* TOTK gates most clears with SetRenderEnableConditional so each runs once
 * (hle_nvn.c); a disabled clear must not wipe a persistent target. */
/* B5 (black weapon/spell menu backdrop). While a menu is open TOTK stops drawing the world (~110-300 draws a frame instead of
 * 1500+) and builds the blurred backdrop from the 1280x720 scene colour target the last world frame left behind. Every frame
 * still starts with a clear of that target. In world frames the clear is needed (the tiled lighting only draws lit tiles and
 * relies on the cleared rest; skipping it always showed stale green tiles, 26 Oct), but in a menu frame the blur samples the
 * target before anything is drawn to it, so the clear wiped the scene first: black backdrop.
 * So the clear of a 1280x720 colour target is deferred: it runs just before the first write to the image, through whichever
 * serial: as the load op when a pass attaches it (begin_rendering), else as a transfer clear before a storage-image binding
 * (nvk_scene_defer_write), a copy or upload into it or a texture clear. It is dropped when the image is sampled or copied
 * first (nvk_scene_defer_read): the menu frame reads the scene for the blur and later still draws into it, so a clear run
 * at that draw blacked out the next frame's backdrop (28 Oct, fix 4). Each 1280x720 image has its own pending clear:
 * TOTK clears about five a frame; with one slot the others were lost or run early (tiles in the world, black menu).
 * The first version (26 Oct) matched serials only and dropped the clear on the first read: the image was also written
 * through a second serial of the shared image, the clear then ran after those writes and wiped them (dark staircase tiles
 * at rock/sky edges, 26 Oct recording).
 * TOTK_VK_KEEP_SCENE (live): 2 deferred (default), 1 skipped for good (experiment), 0 plain clears as before. */
/* The scene colour target has the configured render size (TOTK_SCENE_SIZE, set by the loader from graphics.cfg; the
   game's handheld 1280x720 when the render-resolution patch is off). */
static int scene_size(uint32_t w, uint32_t h) {
    static uint32_t sw, sh;
    if (!sw) {
        const char *e = getenv("TOTK_SCENE_SIZE");
        if (!e || sscanf(e, "%ux%u", &sw, &sh) != 2 || !sw || !sh) sw = 1280, sh = 720;
        VKLOG("scene colour target size %ux%u", sw, sh);
    }
    return w == sw && h == sh;
}
static int scene_mode(void) {
    static int mode = -1; static unsigned g;
    if (mode < 0 || g != nvk_live_gen) { g = nvk_live_gen; const char *e = nvk_live_getenv("TOTK_VK_KEEP_SCENE"); mode = e && *e ? atoi(e) : 2; if (mode < 0 || mode > 2) mode = 2;
        VKLOG("scene colour clear (1280x720): %s (TOTK_VK_KEEP_SCENE)", mode == 2 ? "deferred per image, dropped when read first" : mode == 1 ? "always skipped" : "plain"); }
    return mode;
}
static void scene_log(const char *what) {
    VKLOG("scene clear %s: %llu deferred, %llu run as load op, %llu run before another write, %llu dropped (read first), %llu superseded, %llu run when slots were full; draws so far %llu", what,
        (unsigned long long)scene_deferred, (unsigned long long)scene_materialized, (unsigned long long)scene_direct, (unsigned long long)scene_read,
        (unsigned long long)scene_dropped, (unsigned long long)scene_evicted, (unsigned long long)nvk_draws_issued());
}
static int scene_find(uint64_t serial, const NvkImage *img) {
    if (!scene_pending) return -1;
    for (int k = 0; k < SCENE_SLOTS; ++k)
        if (scene_defer[k].valid && (scene_defer[k].serial == serial || (img && scene_defer[k].img == img))) return k;
    return -1;
}
/* The pending clear as a transfer clear, outside rendering. */
static void scene_defer_run(int k) {
    if (!scene_defer[k].valid) return;
    scene_defer[k].valid = 0; scene_pending--;   /* first: end_rendering may begin a pass for folded clears */
    transfer_barrier_why(XF_CLEAR);
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, scene_defer[k].level, 1, scene_defer[k].layer, 1};
    vkCmdClearColorImage(cb, nvk_image_handle(scene_defer[k].img), VK_IMAGE_LAYOUT_GENERAL, &scene_defer[k].cc, 1, &range);
    nvk_image_written(scene_defer[k].img);
    nvk_barrier(cb);
}
void nvk_scene_defer_read(const NvkImage *img, uint64_t serial) {
    int k = scene_find(serial, img);
    if (k < 0) return;
    scene_defer[k].valid = 0; scene_pending--;
    if (++scene_read <= 5 || scene_read % 200 == 0) scene_log("dropped, target read first (menu frame)");
}
void nvk_scene_defer_write(const NvkImage *img, uint64_t serial) {
    int k = scene_find(serial, img);
    if (k < 0) return;
    scene_defer_run(k);
    if (++scene_direct <= 5 || scene_direct % 1000 == 0) scene_log("run before a storage/copy/upload write");
}
static void clear_color(const p_clear_color *c) {
    if (nvk_dump_frame_active() && have_rt && c->index >= 0 && c->index < cur_rt.n && cur_rt.color[c->index])
        VKLOG("frame clear: %ux%u serial %llu: %s", cur_rt.color[c->index]->d.width, cur_rt.color[c->index]->d.height, (unsigned long long)cur_rt.color[c->index]->serial, nvk_S.render_disabled ? "skipped" : "executed");
    if (have_rt && c->index >= 0 && c->index < cur_rt.n && cur_rt.color[c->index] && cur_rt.color[c->index]->d.width >= 1280 && cur_rt.color[c->index]->serial < 4000) {
        static unsigned said; if (said++ < 6000) VKLOG("clear of the %ux%u target serial %llu (rgba %g %g %g %g): %s", cur_rt.color[c->index]->d.width, cur_rt.color[c->index]->d.height,
            (unsigned long long)cur_rt.color[c->index]->serial, c->rgba[0], c->rgba[1], c->rgba[2], c->rgba[3], nvk_S.render_disabled ? "skipped, rendering disabled" : "executed");
    }
    if (nvk_S.render_disabled) { clears_skipped++; return; }
    if (!have_rt || c->index < 0 || c->index >= cur_rt.n || !cur_rt.color[c->index] || !(c->mask & 0xf)) return;
    if (scene_size(cur_rt.color[c->index]->d.width, cur_rt.color[c->index]->d.height) && (c->mask & 0xf) == 0xf) {
        int m = scene_mode();
        if (m == 1) { clears_skipped++; return; }
        const tex_t *t = cur_rt.color[c->index];
        NvkImage *img = m == 2 || scene_pending ? nvk_image_existing(t->serial) : NULL;
        int k = scene_find(t->serial, img);
        if (k >= 0) {   /* neither read nor written since its last clear */
            scene_defer[k].valid = 0; scene_pending--;
            if (++scene_dropped <= 5 || scene_dropped % 200 == 0) scene_log("superseded, target not written since");
        }
        if (m == 2 && img && clear_foldable()) {   /* an image not created yet has nothing to keep */
            if (scene_pending == SCENE_SLOTS) {   /* full: the oldest runs now */
                scene_defer_run((int)scene_next);
                if (++scene_evicted <= 5 || scene_evicted % 1000 == 0) scene_log("run, all slots pending");
            }
            k = 0;
            while (scene_defer[k].valid) ++k;
            scene_defer[k].serial = t->serial; scene_defer[k].img = img;
            scene_defer[k].level = cur_rt.has_cview[c->index] && cur_rt.cview[c->index].set_levels ? cur_rt.cview[c->index].base_level : 0;
            scene_defer[k].layer = cur_rt.has_cview[c->index] && cur_rt.cview[c->index].set_layers ? cur_rt.cview[c->index].min_layer : 0;
            memcpy(scene_defer[k].cc.float32, c->rgba, sizeof c->rgba); scene_defer[k].valid = 1; scene_pending++; scene_deferred++;
            scene_next = (scene_next + 1) % SCENE_SLOTS;
            return;
        }
    }
    if ((c->mask & 0xf) != 0xf) { VKLOG("color clear with channel mask %#x", c->mask); nvk_die("masked NVN color clears are not implemented"); }
    if (clear_foldable()) {
        pend.color[c->index] = 1; memcpy(pend.cc[c->index].float32, c->rgba, sizeof c->rgba);
        fold_targets();
        return;
    }
    nvk_pass_access(c->index < 8 ? (uint16_t)(1u << c->index) : 0, 0);
    begin_rendering();
    VkClearRect rect;
    if (!clear_rect(&rect)) return;
    VkClearAttachment ca = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = (uint32_t)c->index};
    memcpy(ca.clearValue.color.float32, c->rgba, sizeof c->rgba);
    vkCmdClearAttachments(cb, 1, &ca, 1, &rect);
    if (c->index < 8) pass_written |= (uint16_t)(1u << c->index);
}

static void clear_depth_stencil(const p_clear_ds *c) {
    if (nvk_S.render_disabled) { clears_skipped++; return; }
    if (!have_rt || !cur_rt.depth) return;
    VkImageAspectFlags have = nvk_image_aspect(nvk_image(cur_rt.depth, cb)), aspect = 0;
    if (c->depth_mask && (have & VK_IMAGE_ASPECT_DEPTH_BIT)) aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if ((c->stencil_mask & 0xff) && (have & VK_IMAGE_ASPECT_STENCIL_BIT)) {
        if ((c->stencil_mask & 0xff) != 0xff) { VKLOG("stencil clear with mask %#x", c->stencil_mask); nvk_die("masked NVN stencil clears are not implemented"); }
        aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    if (!aspect) return;
    if (clear_foldable()) {
        if (aspect & VK_IMAGE_ASPECT_DEPTH_BIT) { pend.depth = 1; pend.d = c->depth; }
        if (aspect & VK_IMAGE_ASPECT_STENCIL_BIT) { pend.stencil = 1; pend.s = (uint32_t)c->stencil; }
        fold_targets();
        return;
    }
    nvk_pass_access(0x100, 0);
    begin_rendering();
    VkClearRect rect;
    if (!clear_rect(&rect)) return;
    VkClearAttachment ca = {.aspectMask = aspect, .clearValue.depthStencil = {c->depth, (uint32_t)c->stencil}};
    vkCmdClearAttachments(cb, 1, &ca, 1, &rect);
    pass_written |= 0x100;
}

/* ---------- Texture copies and clears ---------- */
static int64_t copy_stride;   /* nvnCommandBufferSetCopyRowStride; 0 = tightly packed rows */

static unsigned view_level(int has, const view_t *v) { return has && v->set_levels ? v->base_level : 0; }
static unsigned view_layer(int has, const view_t *v) { return has && v->set_layers ? v->min_layer : 0; }
static VkImageAspectFlags copy_aspect(NvkImage *img) {
    VkImageAspectFlags a = nvk_image_aspect(img);
    return a & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : a;   /* stencil is copied separately in NVN */
}

/* Buffer <-> texture: rows are copy_stride apart (or packed), layers/slices packed. */
static void copy_buffer_texture(const p_copy_b2t *c, int to_texture) {
    if (!c->tex) return;
    if (to_texture) nvk_scene_defer_write(nvk_image_existing(c->tex->serial), c->tex->serial);
    transfer_barrier_why(XF_B2T);
    NvkImage *img = nvk_image(c->tex, cb);
    if (to_texture) nvk_image_written(img);
    int bw, bh, bpb = nvn_fmt_bpb(c->tex->d.format, &bw, &bh);
    if (!bpb) nvk_die("buffer/texture copy with unknown format");
    size_t row = copy_stride ? (size_t)copy_stride : (size_t)((c->r.width + bw - 1) / bw) * (size_t)bpb;
    size_t rows = (size_t)((c->r.height + bh - 1) / bh), img_bytes = row * rows;
    int is3d = c->tex->d.target == 2, nz = c->r.depth ? c->r.depth : 1;
    if (row % (size_t)bpb) { VKLOG("copy row stride %zu not a multiple of the %d-byte block", row, bpb); nvk_die("unaligned NVN copy row stride"); }
    VkBuffer buffer; uint64_t offset;
    if (!to_texture) nvk_note_gpu_write(c->src, img_bytes * (size_t)nz);
    if (!resolve(c->src, img_bytes * (size_t)nz, &buffer, &offset)) {
        VKLOG("%s at %#llx (%zu bytes) outside memory pools", to_texture ? "buffer->texture source" : "texture->buffer destination",
            (unsigned long long)c->src, img_bytes * (size_t)nz);
        nvk_die("NVN texture copy buffer outside memory pools");
    }
    unsigned level = view_level(c->has_view, &c->view), layer0 = view_layer(c->has_view, &c->view);
    VkBufferImageCopy regions[64];
    uint32_t n = 0;
    for (int z = 0; z < (is3d ? 1 : nz); ++z) {
        if (n == 64) nvk_die("too many layers in one NVN texture copy");
        regions[n++] = (VkBufferImageCopy){.bufferOffset = offset + (uint64_t)z * img_bytes,
            .bufferRowLength = (uint32_t)(row / (size_t)bpb) * (uint32_t)bw, .bufferImageHeight = (uint32_t)rows * (uint32_t)bh,
            .imageSubresource = {copy_aspect(img), level, is3d ? 0 : layer0 + (uint32_t)c->r.zoffset + (uint32_t)z, 1},
            .imageOffset = {c->r.xoffset, c->r.yoffset, is3d ? c->r.zoffset : 0},
            .imageExtent = {(uint32_t)c->r.width, (uint32_t)c->r.height, is3d ? (uint32_t)nz : 1}};
    }
    if (to_texture) vkCmdCopyBufferToImage(cb, buffer, nvk_image_handle(img), VK_IMAGE_LAYOUT_GENERAL, n, regions);
    else vkCmdCopyImageToBuffer(cb, nvk_image_handle(img), VK_IMAGE_LAYOUT_GENERAL, buffer, n, regions);
    nvk_barrier(cb);
}

static void copy_texture_texture(const p_copy_t2t *c) {
    if (!c->src || !c->dst) return;
    nvk_scene_defer_read(nvk_image_existing(c->src->serial), c->src->serial);
    nvk_scene_defer_write(nvk_image_existing(c->dst->serial), c->dst->serial);
    transfer_barrier_why(XF_T2T);
    NvkImage *s = nvk_image(c->src, cb), *d = nvk_image(c->dst, cb);
    nvk_image_written(d);
    unsigned sl = view_level(c->has_sv, &c->sv), dl = view_level(c->has_dv, &c->dv);
    unsigned s0 = view_layer(c->has_sv, &c->sv), d0 = view_layer(c->has_dv, &c->dv);
    int s3 = c->src->d.target == 2, d3 = c->dst->d.target == 2, nz = c->sr.depth ? c->sr.depth : 1;
    int same_size = c->sr.width == c->dr.width && c->sr.height == c->dr.height && (c->sr.depth ? c->sr.depth : 1) == (c->dr.depth ? c->dr.depth : 1);
    int flags = c->flags & ~NVN_COPY_FLAGS_ENGINE_2D_BIT & ~NVN_COPY_FLAGS_LINEAR_FILTER_BIT;
    if (flags || s3 != d3) {
        VKLOG("texture copy %dx%dx%d -> %dx%dx%d flags %#x 3D %d->%d", c->sr.width, c->sr.height, c->sr.depth, c->dr.width, c->dr.height, c->dr.depth, c->flags, s3, d3);
        nvk_die("NVN texture copy form not implemented");
    }
    if (same_size && nvk_image_format(s) == nvk_image_format(d)) {
        VkImageCopy r = {.srcSubresource = {copy_aspect(s), sl, s3 ? 0 : s0 + (uint32_t)c->sr.zoffset, s3 ? 1 : (uint32_t)nz},
            .srcOffset = {c->sr.xoffset, c->sr.yoffset, s3 ? c->sr.zoffset : 0},
            .dstSubresource = {copy_aspect(d), dl, d3 ? 0 : d0 + (uint32_t)c->dr.zoffset, d3 ? 1 : (uint32_t)nz},
            .dstOffset = {c->dr.xoffset, c->dr.yoffset, d3 ? c->dr.zoffset : 0},
            .extent = {(uint32_t)c->sr.width, (uint32_t)c->sr.height, s3 ? (uint32_t)nz : 1}};
        vkCmdCopyImage(cb, nvk_image_handle(s), VK_IMAGE_LAYOUT_GENERAL, nvk_image_handle(d), VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    } else {
        /* Scaling or format conversion: the 2D engine path, as a filtered blit. */
        if (s3 || nz != 1) nvk_die("scaled NVN copies of 3D or layered textures are not implemented");
        VkImageBlit b = {.srcSubresource = {copy_aspect(s), sl, s0 + (uint32_t)c->sr.zoffset, 1},
            .srcOffsets = {{c->sr.xoffset, c->sr.yoffset, 0}, {c->sr.xoffset + c->sr.width, c->sr.yoffset + c->sr.height, 1}},
            .dstSubresource = {copy_aspect(d), dl, d0 + (uint32_t)c->dr.zoffset, 1},
            .dstOffsets = {{c->dr.xoffset, c->dr.yoffset, 0}, {c->dr.xoffset + c->dr.width, c->dr.yoffset + c->dr.height, 1}}};
        int linear = (c->flags & NVN_COPY_FLAGS_LINEAR_FILTER_BIT) && copy_aspect(s) == VK_IMAGE_ASPECT_COLOR_BIT;
        vkCmdBlitImage(cb, nvk_image_handle(s), VK_IMAGE_LAYOUT_GENERAL, nvk_image_handle(d), VK_IMAGE_LAYOUT_GENERAL, 1, &b,
            linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    }
    nvk_barrier(cb);
}

static int integer_format(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT: case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SINT: case VK_FORMAT_R32_UINT:
    case VK_FORMAT_R32_SINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16_SINT:
    case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R8G8B8A8_SINT:
    case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT:
    case VK_FORMAT_A2B10G10R10_UINT_PACK32: case VK_FORMAT_A2B10G10R10_SINT_PACK32:
        return 1;
    default: return 0;
    }
}

/* Whole-level clears only (the Metal layer has the same limit). */
static void clear_texture(const p_clear_tex *c) {
    if (!c->tex) return;
    nvk_scene_defer_write(nvk_image_existing(c->tex->serial), c->tex->serial);
    transfer_barrier_why(XF_CLEAR);
    NvkImage *img = nvk_image(c->tex, cb);
    nvk_image_written(img);
    unsigned level = view_level(c->has_view, &c->view), layer0 = view_layer(c->has_view, &c->view);
    VkExtent2D e = nvk_image_level_extent(img, level);
    int nz = c->r.depth ? c->r.depth : 1, is3d = c->tex->d.target == 2;
    if (c->r.xoffset || c->r.yoffset || (uint32_t)c->r.width != e.width || (uint32_t)c->r.height != e.height || (c->mask & 0xf) != 0xf
        || is3d || (c->has_view && c->view.set_format)) {
        VKLOG("texture clear region %d,%d %dx%d of %ux%u mask %#x 3D %d view format %d", c->r.xoffset, c->r.yoffset, c->r.width, c->r.height,
            e.width, e.height, c->mask, is3d, c->has_view && c->view.set_format ? c->view.format : -1);
        nvk_die("NVN texture clear form not implemented");
    }
    VkImageAspectFlags aspect = nvk_image_aspect(img);
    VkImageSubresourceRange range = {aspect, level, 1, layer0 + (uint32_t)c->r.zoffset, (uint32_t)nz};
    if (aspect == VK_IMAGE_ASPECT_COLOR_BIT) {
        VkClearColorValue v;
        if (c->is_uint != integer_format(nvk_image_format(img))) nvk_die("NVN texture clear value type does not match the format");
        memcpy(&v, &c->c, sizeof v);
        vkCmdClearColorImage(cb, nvk_image_handle(img), VK_IMAGE_LAYOUT_GENERAL, &v, 1, &range);
    } else {
        VkClearDepthStencilValue v = {aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? c->c.f[0] : 0,
            aspect == VK_IMAGE_ASPECT_STENCIL_BIT ? c->c.u[0] : c->c.u[1]};
        vkCmdClearDepthStencilImage(cb, nvk_image_handle(img), VK_IMAGE_LAYOUT_GENERAL, &v, 1, &range);
    }
    nvk_barrier(cb);
}

/* TOTK_VK_SKIP_DRAWS=1 (diagnostic): draws and dispatches without a Vulkan
 * implementation are skipped and counted instead of stopping the run, so the
 * implemented passes (render targets, clears, copies, present) can be seen. */
static int skip_draws(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("TOTK_VK_SKIP_DRAWS"); v = e && !strcmp(e, "1"); if (v) VKLOG("TOTK_VK_SKIP_DRAWS=1: unimplemented draws are skipped (diagnostic)"); }
    return v;
}
static uint64_t skipped_draws;

void mtl_sub_begin(void) {
    mtl_init();
    nvk_record_front();
    atomic_store_explicit(&sampler_replay_thread, (uintptr_t)pthread_self(), memory_order_relaxed);
    if (replay_cache_on() && !on_replay_thread()) {   /* the replay thread (rarely changes; the old thread's hits are dropped) */
        memset(replay_hits, 0, sizeof replay_hits);
        atomic_store_explicit(&replay_thread, (uintptr_t)pthread_self(), memory_order_relaxed);
    }
    if (cb) nvk_die("re-entrant NVN submission");
    collect_garbage();
    nvk_image_sweep();
    unsigned i = ring_next++ % RING;
    if (ring[i].fence) mtl_wait(ring[i].fence, UINT64_MAX);
    pt_slot_reset(i);
    VKCHECK(vkResetCommandPool(nvk_dev, ring[i].pool, 0), "reset command pool");
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VKCHECK(vkBeginCommandBuffer(ring[i].cb, &bi), "begin command buffer");
    descriptor_slot_begin(i);   /* previous slot fence was waited above, command pool is now empty */
    ring[i].cur = 0; ring_cur = i; ring[i].nseg = 0;
    cb = ring[i].cb; ++cmd_epoch;
    seg_mark = nvk_draws_issued();
    ring[i].fence = fence_issued + 1;
    post_n = 0;
    if (tspool) {   /* the slot's previous results were read before its fence completed */
        vkResetQueryPool(nvk_dev, tspool, 2 * i, 2);
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, tspool, 2 * i);
    }
}

uint64_t mtl_sub_commit(void) {
    if (!cb) nvk_die("NVN commit without submission");
    end_rendering(NVK_END_COMMIT);
    /* GPU writes become visible to the CPU after completion (coherent memory). */
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    if (tspool) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, tspool, 2 * ring_cur + 1);
    VKCHECK(vkEndCommandBuffer(cb), "end command buffer");
    uint64_t fence = ++fence_issued;
    submits_total++;
    VkTimelineSemaphoreSubmitInfo ts = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &fence};
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &ts, .commandBufferCount = 1,
        .pCommandBuffers = &cb, .signalSemaphoreCount = 1, .pSignalSemaphores = &timeline};
    udiff_commit();
    if (totk_tl_on()) { uint64_t id = totk_tl_commit_id(); totk_tl(TL_COMMIT, id, 1); totk_tl(TL_FENCE, id, fence); }   /* TOTK_TIMELINE */
    nvk_queue_lock(); VkResult sr = vkQueueSubmit(nvk_queue, 1, &si, VK_NULL_HANDLE); nvk_queue_unlock();
    VKCHECK(sr, "submit NVN commands");
    cb = NULL;
    Pending job = {fence, NULL, post_n, tspool ? (int)ring_cur : -1, atomic_load(&current_frame)};
    if (job.ts >= 0) gpu_tag_begin(job.frame);
    if (post_n) {
        job.ops = malloc((size_t)post_n * sizeof *job.ops);
        if (!job.ops) nvk_die("post-op allocation");
        memcpy(job.ops, post_ops, (size_t)post_n * sizeof *job.ops);
    }
    pthread_mutex_lock(&done_lock);
    if (pending_head - pending_tail == 256) nvk_die("too many unfinished submissions");
    pending[pending_head++ % 256] = job;
    pthread_cond_broadcast(&done_cv);
    pthread_mutex_unlock(&done_lock);
    return fence;
}

static VkDescriptorPool new_descriptor_pool(void) {
    VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 32768}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16384},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32768}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4096},
        {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 2048}, {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 2048},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2048}, {VK_DESCRIPTOR_TYPE_SAMPLER, 2048},
        {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 2048},   /* framebuffer fetch (nvn_vk_draw.c) */
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 32768}};   /* game uniform buffers (nvn_vk_shader.c dynamic_ubos) */
    VkDescriptorPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 8192,
        .poolSizeCount = sizeof sizes / sizeof *sizes, .pPoolSizes = sizes};
    VkDescriptorPool pool;
    VKCHECK(vkCreateDescriptorPool(nvk_dev, &pci, NULL, &pool), "create descriptor pool");
    return pool;
}
/* Replay thread only. Reuse allocations, never old descriptor contents: set_for
 * writes every returned set before binding it. A set is handed out once per
 * submission and becomes available again only after its slot fence completed.
 * Bound the retained stock; uncommon layout churn falls back to pool reset.
 * TOTK_VK_DESC_RECYCLE=0 keeps the former reset/allocate path. */
#define DS_LAYOUTS 1024u
#define DS_SETS 8192u
typedef struct { VkDescriptorSetLayout layout; VkDescriptorSet *sets; unsigned count, used, cap; } DescStock;
typedef struct { DescStock *stock; unsigned layouts, total; int untracked; } DescSlot;
static DescSlot desc_slots[RING];
static uint64_t desc_fresh, desc_recycled, desc_pool_resets;
static uint64_t desc_reset_reasons[5]; /* off, untracked, set limit, layout limit, multiple pools */
static int desc_recycle_on(void) {
    static int on = -1; static unsigned generation;
    if (on < 0 || generation != nvk_live_gen) {
        generation = nvk_live_gen;
        const char *e = nvk_live_getenv("TOTK_VK_DESC_RECYCLE"); on = !(e && !strcmp(e, "0"));
    }
    return on;
}
static void descriptor_slot_begin(unsigned slot) {
    typeof(ring[0]) *r = &ring[slot];
    DescSlot *s = &desc_slots[slot];
    if (r->fence && r->fence > nvk_completed_fence()) nvk_die("descriptor slot reused before completion");
    int reset = !desc_recycle_on() || s->untracked || s->total >= DS_SETS ||
        s->layouts > DS_LAYOUTS * 3 / 4 || r->cur > 0;
    if (reset) {
        desc_reset_reasons[0] += !desc_recycle_on();
        desc_reset_reasons[1] += s->untracked != 0;
        desc_reset_reasons[2] += s->total >= DS_SETS;
        desc_reset_reasons[3] += s->layouts > DS_LAYOUTS * 3 / 4;
        desc_reset_reasons[4] += r->cur > 0;
        for (unsigned d = 0; d < r->ndpool; ++d) {
            VKCHECK(vkResetDescriptorPool(nvk_dev, r->dpool[d], 0), "reset descriptor pool");
            desc_pool_resets++;
        }
        if (s->stock) for (unsigned j = 0; j < DS_LAYOUTS; ++j) {
            free(s->stock[j].sets);
            s->stock[j] = (DescStock){0};
        }
        s->layouts = s->total = 0; s->untracked = 0; r->cur = 0;
    } else if (s->stock) {
        for (unsigned j = 0; j < DS_LAYOUTS; ++j) s->stock[j].used = 0;
    }
}
static DescStock *descriptor_stock(VkDescriptorSetLayout layout) {
    if (!desc_recycle_on()) return NULL;
    DescSlot *s = &desc_slots[ring_cur];
    if (!s->stock) s->stock = calloc(DS_LAYOUTS, sizeof *s->stock);
    if (!s->stock) { s->untracked = 1; return NULL; }
    uint64_t h = (uint64_t)(uintptr_t)layout * 0x9e3779b97f4a7c15ull;
    for (unsigned j = 0; j < DS_LAYOUTS; ++j) {
        DescStock *e = &s->stock[((unsigned)(h >> 32) + j) & (DS_LAYOUTS - 1)];
        if (e->layout == layout) return e;
        if (!e->layout) { e->layout = layout; s->layouts++; return e; }
    }
    s->untracked = 1;
    return NULL;
}
static VkDescriptorSet allocate_set(VkDescriptorSetLayout layout) {
    typeof(ring[0]) *r = &ring[ring_cur];
    for (;;) {
        if (r->cur == r->ndpool) {
            if (r->ndpool == 16) nvk_die("descriptor pools exhausted in one submission");
            r->dpool[r->ndpool++] = new_descriptor_pool();
        }
        VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = r->dpool[r->cur],
            .descriptorSetCount = 1, .pSetLayouts = &layout};
        VkDescriptorSet set;
        VkResult res = vkAllocateDescriptorSets(nvk_dev, &ai, &set);
        if (res == VK_SUCCESS) { desc_fresh++; return set; }
        if (res != VK_ERROR_OUT_OF_POOL_MEMORY && res != VK_ERROR_FRAGMENTED_POOL) { VKLOG("allocate descriptor set -> %d", res); nvk_die("allocate descriptor set"); }
        r->cur++;
    }
}
VkDescriptorSet nvk_alloc_set(VkDescriptorSetLayout layout) {
    if (!cb || !layout) nvk_die("descriptor set without submission or layout");
    DescSlot *s = &desc_slots[ring_cur];
    DescStock *e = descriptor_stock(layout);
    if (e && e->used < e->count) { desc_recycled++; return e->sets[e->used++]; }
    VkDescriptorSet set = allocate_set(layout);
    if (e && s->total < DS_SETS) {
        if (e->count == e->cap) {
            unsigned cap = e->cap ? e->cap * 2 : 8;
            VkDescriptorSet *sets = realloc(e->sets, (size_t)cap * sizeof *sets);
            if (!sets) { s->untracked = 1; return set; }
            e->sets = sets; e->cap = cap;
        }
        e->sets[e->count++] = set; e->used++; s->total++;
    } else s->untracked = 1;
    return set;
}
VkDescriptorPool nvk_descriptor_pool(void) { return ring[ring_cur].ndpool ? ring[ring_cur].dpool[ring[ring_cur].cur] : VK_NULL_HANDLE; }
VkCommandBuffer nvk_cmd(void) { if (!cb) nvk_die("NVN draw outside a submission"); return cb; }

static void post(PostOp o) {
    if (post_n == post_cap) {
        post_cap = post_cap ? post_cap * 2 : 64;
        post_ops = realloc(post_ops, (size_t)post_cap * sizeof *post_ops);
        if (!post_ops) nvk_die("post-op table allocation");
    }
    post_ops[post_n++] = o;
}
void mtl_sub_event(volatile uint32_t *val, uint32_t mode, uint32_t v) { post((PostOp){1, val, mode, v, 0, 0, NULL, 0}); }
void mtl_sub_counter(uint64_t addr, uint32_t type) {
    PostOp o = {2, NULL, 0, 0, addr, type, NULL, 0};
    if (type == NVN_COUNTER_TYPE_TIMESTAMP) {
        int q = cts_take();
        if (q >= 0) { vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, cts_pool, (uint32_t)q); o.query = q + 1; }
    }
    if (type == NVN_COUNTER_TYPE_SAMPLES_PASSED) {
        query_end(cb);   /* later draws are not part of this value */
        o.counter = qcounter; counter_retain(o.counter);
        if (rendering) query_begin(cb);
    }
    pthread_mutex_lock(&cpend_lock);   /* run_post writes 0 for every type but samples passed */
    unsigned slot = cpend_head++ % CPEND;
    cpend[slot].addr = addr; cpend[slot].fence = ring[ring_cur].fence;
    cpend[slot].val = 0; cpend[slot].known = type != NVN_COUNTER_TYPE_SAMPLES_PASSED;
    pthread_mutex_unlock(&cpend_lock);
    post(o);
}
static void counter_reset(uint32_t type) {
    if (type != NVN_COUNTER_TYPE_SAMPLES_PASSED) return;
    query_end(cb);
    counter_release(qcounter);
    qcounter = calloc(1, sizeof *qcounter);
    if (!qcounter) nvk_die("occlusion counter allocation");
    atomic_init(&qcounter->refs, 1);
    if (rendering) query_begin(cb);
}

/* ---------- Uniforms as of the submit call (TOTK_ASYNC_REPLAY=1) ----------
 * With the translation thread nvnQueueSubmitCommands returns before the replay, as on the console, and other game threads
 * rebuild the constant pools right away (nvn_mtl.m mtl_sub_capture_ubos, whose design this follows). So the submit call
 * (game thread) copies every uniform range the submission binds; the replay reads uniform bytes through nvk_ubo_read, which
 * takes captured bytes over memory, and binds copies of all uniform ranges (nvn_vk_draw.c ubo_copy) rather than the
 * game's memory. A range an earlier submission's GPU work writes and had not finished at the capture is read from memory
 * (GPU timeline), as the Metal layer does. Without the translation thread nothing is captured: the replay runs inside the
 * submit call and memory is current. TOTK_UBO_CAPTURE=0: no capture (A/B only, wrong uniforms expected). */
typedef struct { uint64_t lo, hi; } UcapRange;
typedef struct { uint64_t done; int n; UcapRange *r; size_t *off; uint8_t *bytes; } Ucap;
static Ucap *ucap_cur;   /* replay thread: the capture of the submission being replayed */
static _Atomic uint64_t ucap_subs, ucap_bytes, ucap_ns;
static void ucap_collect(UcapRange **r, int *n, int *cap, const uint8_t *p, int depth) {
    for (unsigned guard = 0; guard < 4000000; ++guard) {
        op_hdr h; memcpy(&h, p, sizeof h);
        if (h.size < 8 || (h.size & 7) || h.op >= OP_COUNT_) { VKLOG("uniform capture: corrupt recording at %p (op %u size %u)", (const void *)p, h.op, h.size); return; }
        if (h.op == OP_END) return;
        if (h.op == OP_JUMP) { memcpy(&p, p + 8, 8); continue; }
        if (h.op == OP_CALL) { uint64_t t; memcpy(&t, p + 8, 8); if (depth < 16) ucap_collect(r, n, cap, (const uint8_t *)(uintptr_t)t, depth + 1); }
        else if (h.op == OP_BIND_UBO) {
            p_bind_buf b; memcpy(&b, p + 8, sizeof b);
            if (b.addr && b.size && b.size <= 65536) {
                if (*n == *cap) { *cap = *cap ? *cap * 2 : 4096; UcapRange *g = realloc(*r, (size_t)*cap * sizeof *g); if (!g) nvk_die("uniform capture ranges"); *r = g; }
                (*r)[(*n)++] = (UcapRange){b.addr, b.addr + b.size};
            }
        }
        p += h.size;
    }
}
static int ucap_cmp(const void *x, const void *y) { const UcapRange *a = x, *b = y; return a->lo < b->lo ? -1 : a->lo > b->lo; }
static int ucap_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_UBO_CAPTURE"); on = !(e && !strcmp(e, "0")); }
    return on;
}
void *mtl_sub_capture_ubos(const uint64_t *h, int n) {   /* game thread, in the submit call */
    if (!ucap_on()) return NULL;
    uint64_t t0 = nvk_now_ns();
    Ucap *c = calloc(1, sizeof *c);
    if (!c) nvk_die("uniform capture");
    c->done = nvk_completed_fence();   /* before the bytes: a write completing meanwhile only makes this conservative */
    UcapRange *r = NULL; int nr = 0, cap = 0;
    for (int i = 0; i < n; ++i) ucap_collect(&r, &nr, &cap, (const uint8_t *)(uintptr_t)h[i], 0);
    if (nr) {
        qsort(r, (size_t)nr, sizeof *r, ucap_cmp);
        int k = -1;   /* merge overlapping or touching ranges inside one pool; drop ranges outside the pools */
        for (int i = 0; i < nr; ++i) {
            VkBuffer b; uint64_t o;
            if (k >= 0 && r[i].lo <= r[k].hi) {
                if (r[i].hi <= r[k].hi) continue;
                if (resolve(r[k].lo, r[i].hi - r[k].lo, &b, &o)) { r[k].hi = r[i].hi; continue; }
            }
            if (!resolve(r[i].lo, r[i].hi - r[i].lo, &b, &o)) continue;
            r[++k] = r[i];
        }
        c->n = k + 1;
        size_t total = 0;
        c->off = malloc(sizeof *c->off * (size_t)(c->n ? c->n : 1));
        if (!c->off) nvk_die("uniform capture offsets");
        for (int i = 0; i < c->n; ++i) { c->off[i] = total; total += (size_t)(r[i].hi - r[i].lo); }
        c->bytes = malloc(total ? total : 1);
        if (!c->bytes) nvk_die("uniform capture bytes");
        for (int i = 0; i < c->n; ++i) memcpy(c->bytes + c->off[i], (const void *)(uintptr_t)r[i].lo, (size_t)(r[i].hi - r[i].lo));
        c->r = r; r = NULL;
        atomic_fetch_add(&ucap_bytes, total);
    }
    free(r);
    atomic_fetch_add(&ucap_subs, 1); atomic_fetch_add(&ucap_ns, nvk_now_ns() - t0);
    return c;
}
static void ucap_free(Ucap *c) { if (c) { free(c->r); free(c->off); free(c->bytes); free(c); } }
static uint64_t ucap_serial;   /* which capture is current: bumped at every hand-over (replay thread) */
void mtl_sub_snapshot_ubos_cap(const uint64_t *h, int n, void *cap) {   /* replay start: this submission's capture */
    (void)h; (void)n;
    ucap_free(ucap_cur); ucap_cur = cap; ucap_serial++;
}
int nvk_ucap_active(void) { return ucap_cur != NULL; }
/* Lowest `done` any later gpu_written_after query can use: the replayed submission's capture (later captures and live queries are newer). */
static uint64_t nvk_ucap_floor(void) { return ucap_cur ? ucap_cur->done : nvk_completed_fence(); }
void nvk_ucap_stats(uint64_t *subs, uint64_t *bytes, uint64_t *ns) { *subs = ucap_subs; *bytes = ucap_bytes; *ns = ucap_ns; }
static int gpu_written_after(uint64_t addr, uint64_t size, uint64_t fence);
/* [addr, addr+size) as of the submit call: memory, with captured bytes where a captured range covers them and no GPU
   write unfinished at the capture touches them. */
static void ucap_base(uint64_t addr, uint64_t size, uint8_t *out) {
    const Ucap *c = ucap_cur;
    if (!c || !c->n) { memcpy(out, (const void *)(uintptr_t)addr, size); return; }
    int lo = 0, hi = c->n;   /* first captured range ending above addr (disjoint and sorted) */
    while (lo < hi) { int m = (lo + hi) / 2; if (c->r[m].hi <= addr) lo = m + 1; else hi = m; }
    /* One captured range holds all of it (the usual uniform binding): read only the capture, not the memory the game is
       rewriting meanwhile (and not twice), unless a write of unfinished GPU work touches it. */
    if (lo < c->n && c->r[lo].lo <= addr && addr + size <= c->r[lo].hi && !gpu_written_after(addr, size, c->done)) {
        memcpy(out, c->bytes + c->off[lo] + (addr - c->r[lo].lo), (size_t)size);
        return;
    }
    memcpy(out, (const void *)(uintptr_t)addr, size);
    for (int i = lo; i < c->n && c->r[i].lo < addr + size; ++i) {
        uint64_t s0 = c->r[i].lo > addr ? c->r[i].lo : addr, e0 = c->r[i].hi < addr + size ? c->r[i].hi : addr + size;
        if (gpu_written_after(s0, e0 - s0, c->done)) continue;
        memcpy(out + (s0 - addr), c->bytes + c->off[i] + (s0 - c->r[i].lo), (size_t)(e0 - s0));
    }
}
/* [addr, addr+size) lies inside one captured range and nvk_ubo_read would return exactly the captured bytes for it now (no GPU
   write unfinished at the capture touches it, no in-pass uniform update lies over it). The replay then binds one arena copy
   of that range (made once per capture, TOTK_VK_UBO_CAP_ARENA=0 off) at an offset instead of copying every binding on its
   own: ~5,600 small copies, each with lookups, were 12% of the translation thread. (serial, idx) names the range. */
int nvk_ucap_cover(uint64_t addr, uint64_t size, uint64_t *serial, uint32_t *idx, const uint8_t **bytes, uint64_t *rlo, uint64_t *rlen) {
    const Ucap *c = ucap_cur;
    if (!c || !c->n || !size) return 0;
    int lo = 0, hi = c->n;
    while (lo < hi) { int m = (lo + hi) / 2; if (c->r[m].hi <= addr) lo = m + 1; else hi = m; }
    if (lo >= c->n || c->r[lo].lo > addr || addr + size > c->r[lo].hi) return 0;
    if (gpu_written_after(addr, size, c->done) || ubo_log_overlaps(addr, size)) return 0;
    *serial = ucap_serial; *idx = (uint32_t)lo; *bytes = c->bytes + c->off[lo]; *rlo = c->r[lo].lo; *rlen = c->r[lo].hi - c->r[lo].lo;
    return 1;
}
void mtl_sub_index(const uint64_t *h, int n) { (void)h; (void)n; }
void mtl_exec_rest(const uint64_t *h, int n) { (void)h; (void)n; }

/* ---------- Buffer ranges the GPU writes ----------
 * Storage writes and copies, with the submission that does them. TOTK_FENCE_LAG copies (nvn_vk_draw.c) must not copy a
 * range such work may still change: the copy would hold the contents from before it. */
#define GW_N 4096
static struct { uint64_t lo, hi, fence; } gw[GW_N]; static unsigned gw_head;
/* Bumped by every GPU write into game memory buffers (storage stores, buffer copies, texture-to-buffer copies, uniform
   updates, copy-backs): a read-only copy of a buffer range made since the last bump still holds what memory holds. */
static uint64_t buffer_write_epoch = 1;
uint64_t nvk_buffer_write_epoch(void) { return buffer_write_epoch; }
void nvk_buffer_write_bump(void) { buffer_write_epoch++; }
/* Fast "no write overlaps" answer for gpu_written_after (TOTK_VK_GW_INDEX=0: always scan the ring). The ring is scanned from
   the newest entry down to the first one at or below `done`: with the translation thread every uniform binding asks twice
   and `done` lags a frame or two behind, so that was ~5,500 scans of hundreds of entries per frame (the replay thread went
   from 18.6 to 27.7 ms). Writes up to GWP_PAGES pages long mark the newest fence on each of their 4 KiB pages (hashed;
   a collision only costs a scan); longer ones (a storage buffer is noted with the rest of its pool) are kept as ranges.
   An answer of "none" is exact: every ring entry that overlaps the query and is newer than `done` marked one of the query's
   pages or sits in the range list; the ring wrap and a dropped range fall back to the scan. */
#define GWP_N 16384
#define GWP_PAGES 64
#define GWB_N 256
static uint64_t gwp_fence[GWP_N];
static struct { uint64_t lo, hi, fence; } gwb[GWB_N]; static unsigned gwb_n;
static uint64_t gwb_lost;   /* newest fence of a long range that did not fit the list */
static uint64_t gw_scans, gw_fast, gw_ring_steps;
static int gw_index_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_VK_GW_INDEX"); on = !(e && !strcmp(e, "0")); }
    return on;
}
static inline unsigned gwp_slot(uint64_t page) { return (unsigned)((page * 0x9E3779B97F4A7C15ull) >> 50); }
static void gw_index_add(uint64_t addr, uint64_t size, uint64_t fence) {
    uint64_t p0 = addr >> 12, p1 = (addr + size - 1) >> 12;
    if (p1 - p0 < GWP_PAGES) {
        for (uint64_t p = p0; p <= p1; ++p) { uint64_t *f = &gwp_fence[gwp_slot(p)]; if (*f < fence) *f = fence; }
        return;
    }
    for (unsigned i = 0; i < gwb_n; ++i)
        if (gwb[i].lo == addr && gwb[i].hi == addr + size) { if (gwb[i].fence < fence) gwb[i].fence = fence; return; }
    if (gwb_n == GWB_N) {   /* full: drop ranges nothing can still ask about (older than the replayed submission's capture) */
        uint64_t keep = nvk_ucap_floor(), k = 0;
        for (unsigned i = 0; i < gwb_n; ++i) if (gwb[i].fence > keep) gwb[k++] = gwb[i];
        gwb_n = (unsigned)k;
        if (gwb_n == GWB_N) { if (gwb_lost < fence) gwb_lost = fence; return; }
    }
    gwb[gwb_n++] = (typeof(gwb[0])){addr, addr + size, fence};
}
void nvk_note_gpu_write(uint64_t addr, uint64_t size) {
    buffer_write_epoch++;
    if (!addr || !size) return;
    uint64_t fence = nvk_pending_fence();
    gw[gw_head++ % GW_N] = (typeof(gw[0])){addr, addr + size, fence};
    gw_index_add(addr, size, fence);
}
int nvk_gpu_written_live(uint64_t addr, uint64_t size) { return gpu_written_after(addr, size, nvk_completed_fence()); }
/* A recorded GPU write with a fence above `done` overlaps the range. */
static int gpu_written_scan(uint64_t addr, uint64_t size, uint64_t done) {
    for (unsigned k = 0; k < GW_N && k < gw_head; ++k) {
        unsigned i = (gw_head - 1 - k) % GW_N;
        if (gw[i].fence <= done) break;   /* older entries completed too (recorded in submission order) */
        gw_ring_steps++;
        if (gw[i].lo < addr + size && addr < gw[i].hi) return 1;
    }
    return gw_head > GW_N && gw[gw_head % GW_N].fence > done;   /* the ring wrapped over live entries: assume written */
}
static int gpu_written_after(uint64_t addr, uint64_t size, uint64_t done) {
    if (gw_index_on() && size && !(gw_head > GW_N && gw[gw_head % GW_N].fence > done) && gwb_lost <= done) {
        uint64_t p0 = addr >> 12, p1 = (addr + size - 1) >> 12;
        if (p1 - p0 < GWP_PAGES) {
            int maybe = 0;
            for (uint64_t p = p0; p <= p1 && !maybe; ++p) maybe = gwp_fence[gwp_slot(p)] > done;
            for (unsigned i = 0; i < gwb_n && !maybe; ++i) maybe = gwb[i].fence > done && gwb[i].lo < addr + size && addr < gwb[i].hi;
            if (!maybe) { gw_fast++; return 0; }
        }
    }
    gw_scans++;
    return gpu_written_scan(addr, size, done);
}
void nvk_gw_stats(uint64_t *fast, uint64_t *scans, uint64_t *steps) { *fast = gw_fast; *scans = gw_scans; *steps = gw_ring_steps; }

void mtl_sub_copy(uint64_t src, uint64_t dst, uint64_t n) {
    VkBuffer sb, db; uint64_t so, dofs;
    if (!n) return;
    if (!resolve(src, n, &sb, &so) || !resolve(dst, n, &db, &dofs)) {
        VKLOG("copy %#llx -> %#llx (%llu bytes) outside registered pools", (unsigned long long)src, (unsigned long long)dst, (unsigned long long)n);
        nvk_die("NVN buffer copy outside memory pools");
    }
    if (nvk_bufwatch_hit(dst, n)) nvk_bufwatch_note("buffer copy WRITE", dst, n);
    if (nvk_bufwatch_hit(src, n)) nvk_bufwatch_note("buffer copy read", src, n);
    transfer_barrier_why(XF_COPY);
    VkBufferCopy r = {.srcOffset = so, .dstOffset = dofs, .size = n};
    vkCmdCopyBuffer(cb, sb, db, 1, &r);
    nvk_note_gpu_write(dst, n);
    nvk_barrier(cb);
}

static void sub_update(const uint8_t *data, uint64_t dst, uint64_t n) {
    VkBuffer b; uint64_t off;
    if (!n) return;
    if (!resolve(dst, n, &b, &off)) nvk_die("NVN uniform update outside memory pools");
    if (nvk_bufwatch_hit(dst, n)) nvk_bufwatch_note("buffer update WRITE", dst, n);
    if ((off & 3) || (n & 3) || n > 65536) {
        VKLOG("uniform update %#llx size %llu not expressible with vkCmdUpdateBuffer", (unsigned long long)dst, (unsigned long long)n);
        nvk_die("unaligned NVN uniform update needs a staging path");
    }
    if (rendering && ubo_inpass_on()) {
        ubo_append(dst, data, n, 0); ubo_unflushed++; ubo_epoch++; ubo_inpass_total++;
        if (dst < ubo_unflushed_lo) ubo_unflushed_lo = dst;
        if (dst + n > ubo_unflushed_hi) ubo_unflushed_hi = dst + n;
        return;
    }
    transfer_barrier_why(XF_UBO);
    vkCmdUpdateBuffer(cb, b, off, n, data);
    buffer_write_epoch++;
    ubo_epoch++;   /* uniform copies cached by epoch (lag_copy) must not survive an update outside a pass either */
    nvk_barrier(cb);
    ubo_append(dst, data, n, 1);
}

/* ---------- Command replay ---------- */
static const char *const op_names[OP_COUNT_] = {
    "END", "JUMP", "BEGIN", "CALL", "SIGNAL_EVENT", "REPORT_COUNTER", "COPY_B2B", "UPDATE_UB",
    "SET_RT", "CLEAR_COLOR", "CLEAR_DS", "VIEWPORTS", "SCISSOR", "DEPTH_RANGE",
    "BIND_PROGRAM", "BIND_STATE", "BIND_VATTR", "BIND_VSTREAM", "BIND_VBUF",
    "BIND_UBO", "BIND_SSBO", "BIND_TEX", "BIND_SAMP",
    "STENCIL", "POLY_OFFSET", "DRAW", "DRAW_IDX", "DISPATCH", "BARRIER",
    "SET_TEX_POOL", "SET_SAMP_POOL", "COPY_B2T", "COPY_STRIDE", "BLEND_COLOR", "COPY_T2B", "BIND_IMG",
    "COPY_T2T", "RESET_COUNTER", "CLEAR_TEX", "DEPTH_CLAMP", "RENDER_ENABLE", "RASTER_DISCARD", "RENDER_COND", "DISCARD",
};
static uint64_t op_seen[OP_COUNT_];

static _Noreturn void missing_op(unsigned op) {
    VKLOG("command op %s (%u) has no Vulkan replay yet", op < OP_COUNT_ && op_names[op] ? op_names[op] : "?", op);
    char line[512]; size_t used = 0;
    for (unsigned i = 0; i < OP_COUNT_ && used < sizeof line - 32; ++i)
        if (op_seen[i]) used += (size_t)snprintf(line + used, sizeof line - used, " %s=%llu", op_names[i], (unsigned long long)op_seen[i]);
    VKLOG("ops replayed before it:%s", used ? line : " none");
    nvk_die("NVN command replay needs a Vulkan implementation");
}

void mtl_exec(const uint8_t *p) {
    if (!cb) nvk_die("NVN replay outside a submission");
    for (;;) {
        op_hdr h; memcpy(&h, p, sizeof h);
        if (h.size < 8 || (h.size & 7) || h.op >= OP_COUNT_) {
            VKLOG("corrupt command record at %p: op %u size %u", (const void *)p, h.op, h.size);
            nvk_die("corrupt NVN command record");
        }
        const uint8_t *a = p + 8;
        switch (h.op) {
        case OP_END: return;
        case OP_BEGIN: break;
        case OP_JUMP: memcpy(&p, a, 8); continue;
        case OP_CALL: { uint64_t t; memcpy(&t, a, 8); mtl_exec((const uint8_t *)(uintptr_t)t); break; }
        case OP_SIGNAL_EVENT: { volatile uint32_t *val; uint32_t m, v; memcpy(&val, a, 8); memcpy(&m, a + 8, 4); memcpy(&v, a + 12, 4);
            mtl_sub_event(val, m, v); break; }
        case OP_REPORT_COUNTER: { uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, a + 8, 8); mtl_sub_counter(ad, (uint32_t)t); break; }
        case OP_RESET_COUNTER: { uint64_t t; memcpy(&t, a, 8); counter_reset((uint32_t)t); break; }
        case OP_COPY_B2B: { uint64_t s, d, n; memcpy(&s, a, 8); memcpy(&d, a + 8, 8); memcpy(&n, a + 16, 8); mtl_sub_copy(s, d, n); break; }
        case OP_UPDATE_UB: { uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, a + 8, 8); sub_update(a + 16, d, n); break; }
        case OP_BARRIER: { int32_t bits; memcpy(&bits, a, 4); barriers_total++;
            if (rendering) { barriers_open++; for (int k = 0; k < 16; ++k) if (bits & (1 << k)) barrier_bits_open[k]++; }
            if (barrier_keeps_pass(bits)) { barriers_kept++; break; }
            transfer_barrier(); break; }
        case OP_SET_RT: set_render_targets((const p_set_rt *)a); break;
        case OP_COPY_STRIDE: copy_stride = ((const p_copy_stride *)a)->stride; break;
        case OP_COPY_B2T: copy_buffer_texture((const p_copy_b2t *)a, 1); break;
        case OP_COPY_T2B: copy_buffer_texture((const p_copy_b2t *)a, 0); break;
        case OP_COPY_T2T: copy_texture_texture((const p_copy_t2t *)a); break;
        case OP_CLEAR_TEX: clear_texture((const p_clear_tex *)a); break;
        case OP_SCISSOR: memcpy(&scissor, a, sizeof scissor); have_scissor = 1; break;
        case OP_CLEAR_COLOR: clear_color((const p_clear_color *)a); break;
        case OP_CLEAR_DS: clear_depth_stencil((const p_clear_ds *)a); break;
        case OP_DRAW: case OP_DRAW_IDX: case OP_DISPATCH:
            if (skip_draws()) {
                ++skipped_draws;
                if (!(skipped_draws & (skipped_draws - 1))) VKLOG("skipped %llu draws/dispatches", (unsigned long long)skipped_draws);
            } else if (h.op == OP_DISPATCH) nvk_dispatch((const p_dispatch *)a);
            else if (h.op == OP_DRAW) nvk_draw((const p_draw *)a, NULL);
            else nvk_draw(NULL, (const p_draw_idx *)a);
            break;
        /* State without GPU work of its own; it takes effect at draws (nvn_vk_draw.c). */
        case OP_VIEWPORTS: case OP_DEPTH_RANGE: case OP_BIND_PROGRAM: case OP_BIND_STATE:
        case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF: case OP_BIND_UBO: case OP_BIND_SSBO:
        case OP_BIND_TEX: case OP_BIND_SAMP: case OP_BIND_IMG: case OP_STENCIL: case OP_POLY_OFFSET:
        case OP_BLEND_COLOR: case OP_DEPTH_CLAMP: case OP_RENDER_ENABLE: case OP_RASTER_DISCARD: case OP_RENDER_COND:
            nvk_state_op(h.op, a);
            break;
        case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL:
            break;
        case OP_DISCARD: discard_op(((const p_flag *)a)->value); break;
        default: missing_op(h.op);
        }
        op_seen[h.op]++;
        p += h.size;
    }
}

/* ---------- Texture and sampler registration ---------- */
typedef struct { tex_t tex; view_t view; int used, has_view; uint64_t gen; } TexSlot;
static TexSlot *tex_slots; static int tex_slot_cap, tex_pool_n;
static samp_par *samp_slots; static uint8_t *samp_used; static int samp_slot_cap;
static fmutex reg_lock = FMUTEX_INIT;   /* nvn_vk_fmutex.h: texture/sampler registry, read per bound texture by the replay thread */

/* TOTK re-initialises transient textures (G-buffer etc.) every frame at the
 * same pool memory without re-registering their descriptors; on hardware the
 * descriptor refers to the memory, so it stays valid. A registration whose
 * serial was finalised follows its storage to the texture living there now
 * (nvn_tex_live_at, as tex_by_id in nvn_mtl.m), instead of rebuilding a dead
 * serial from CPU pool bytes. Finalised serials are kept (grow-only, as on
 * Metal); dead_gen lets a slot skip the check until another texture dies. */
int nvn_tex_live_at(const void *mem, tex_t *out);   /* hle_nvn.c */
static uint64_t *dead_set; static size_t dead_cap, dead_n; static uint64_t dead_gen = 1;
static uint64_t tex_redirects, tex_dead_unresolved;
static size_t dead_slot(uint64_t serial, size_t cap) { return (size_t)((serial * 0x9e3779b97f4a7c15ull) >> 24) & (cap - 1); }
static int dead_has_locked(uint64_t serial) {
    if (!dead_cap) return 0;
    for (size_t i = dead_slot(serial, dead_cap);; i = (i + 1) & (dead_cap - 1)) {
        if (!dead_set[i]) return 0;
        if (dead_set[i] == serial) return 1;
    }
}
static void dead_add_locked(uint64_t serial) {
    if (!serial || dead_has_locked(serial)) return;
    if (2 * (dead_n + 1) > dead_cap) {
        size_t cap = dead_cap ? dead_cap * 2 : 4096;
        uint64_t *set = calloc(cap, sizeof *set);
        if (!set) nvk_die("finalised texture set allocation");
        for (size_t j = 0; j < dead_cap; ++j) if (dead_set[j]) {
            size_t i = dead_slot(dead_set[j], cap);
            while (set[i]) i = (i + 1) & (cap - 1);
            set[i] = dead_set[j];
        }
        free(dead_set); dead_set = set; dead_cap = cap;
    }
    size_t i = dead_slot(serial, dead_cap);
    while (dead_set[i]) i = (i + 1) & (dead_cap - 1);
    dead_set[i] = serial; dead_n++; dead_gen++;
}
static void follow_storage_locked(int id, TexSlot *s) {
    if (!s->tex.mem || !dead_has_locked(s->tex.serial)) { s->gen = dead_gen; return; }
    tex_t live;
    if (nvn_tex_live_at(s->tex.mem, &live) && live.serial != s->tex.serial && live.L.size == s->tex.L.size &&
        live.d.format == s->tex.d.format && live.d.width == s->tex.d.width && live.d.height == s->tex.d.height &&
        live.d.depth == s->tex.d.depth && live.d.target == s->tex.d.target && live.d.levels == s->tex.d.levels) {
        if (tex_redirects++ < 4 || tex_redirects % 100000 == 0)
            VKLOG("texture id %d: registration followed its storage from finalised serial %llu to live serial %llu (%llu redirects so far)",
                id, (unsigned long long)s->tex.serial, (unsigned long long)live.serial, (unsigned long long)tex_redirects);
        s->tex = live; s->gen = dead_gen;
    } else {
        tex_dead_unresolved++;   /* checked again at the next use */
        /* Diagnosis: why the storage has no matching live texture (first 40 distinct serials). */
        static uint64_t seen[40]; static unsigned nseen;
        int known = 0; for (unsigned k = 0; k < nseen; ++k) if (seen[k] == s->tex.serial) known = 1;
        if (!known && nseen < 40) {
            seen[nseen++] = s->tex.serial;
            const tex_t *d = &s->tex; int found = nvn_tex_live_at(d->mem, &live);
            NvkImage *img = nvk_image_existing(d->serial);
            VKLOG("texture id %d: finalised serial %llu at %p (fmt %d %ux%ux%u target %d levels %d size %llu, image %s%s) unresolved: %s",
                id, (unsigned long long)d->serial, d->mem, (int)d->d.format, d->d.width, d->d.height, d->d.depth, (int)d->d.target, (int)d->d.levels,
                (unsigned long long)d->L.size, img ? "kept" : "gone", img && nvk_image_gpu_owned(img) ? ", GPU-written" : "",
                !found ? "no live texture at that storage" : live.serial == d->serial ? "live entry is the same serial" : "live texture has another shape");
            if (found && live.serial != d->serial)
                VKLOG("  live serial %llu: fmt %d %ux%ux%u target %d levels %d size %llu", (unsigned long long)live.serial, (int)live.d.format,
                    live.d.width, live.d.height, live.d.depth, (int)live.d.target, (int)live.d.levels, (unsigned long long)live.L.size);
        }
    }
}

void mtl_set_tex_pool_n(int n) { fm_lock(&reg_lock); tex_pool_n = n; fm_unlock(&reg_lock); }
/* Per texture id: what shader translation depends on (target, format), read
 * without the lock on every draw; 0 = unregistered. NVN pools hold at most
 * 2^20 textures (device property 0x24). */
static _Atomic uint32_t tex_info[1u << 20];
static _Atomic uint64_t tex_info_gen = 1;   /* changes whenever a registration changes a texture's info (any thread) */
uint64_t nvk_tex_info_gen(void) { return atomic_load_explicit(&tex_info_gen, memory_order_acquire); }
uint32_t nvk_texture_info(int id) { return id > 0 && id < (1 << 20) ? atomic_load_explicit(&tex_info[id], memory_order_relaxed) : 0; }

void mtl_reg_tex(int id, const tex_t *t, const view_t *v) {
    if (id < 0) return;
    if (id < (1 << 20)) {
        uint32_t info = 0;
        if (t) {
            int f = v && v->set_format ? v->format : t->d.format, tg = v && v->set_target ? v->target : t->d.target;
            info = 0x80000000u | (uint32_t)(f & 0xffff) << 8 | (uint32_t)(tg & 0xff);
        }
        if (atomic_exchange_explicit(&tex_info[id], info, memory_order_relaxed) != info) atomic_fetch_add_explicit(&tex_info_gen, 1, memory_order_release);
    }
    fm_lock(&reg_lock);
    if (id >= tex_slot_cap) {
        int cap = tex_slot_cap ? tex_slot_cap : 1024; while (cap <= id) cap *= 2;
        tex_slots = realloc(tex_slots, (size_t)cap * sizeof *tex_slots);
        if (!tex_slots) nvk_die("texture registry allocation");
        memset(tex_slots + tex_slot_cap, 0, (size_t)(cap - tex_slot_cap) * sizeof *tex_slots);
        tex_slot_cap = cap;
    }
    TexSlot *s = &tex_slots[id];
    if (t) { s->tex = *t; s->used = 1; s->has_view = v != NULL; s->gen = 0; if (v) s->view = *v; else memset(&s->view, 0, sizeof s->view); }
    else s->used = 0;
    fm_unlock(&reg_lock);
}
/* Registration epoch for the replay's sampler-id cache. Published only once the protected data was updated. */
static _Atomic uint64_t sampler_reg_epoch = 1;
void mtl_reg_samp(int id, const samp_par *p) {
    if (id < 0) return;
    fm_lock(&reg_lock);
    if (id >= samp_slot_cap) {
        int cap = samp_slot_cap ? samp_slot_cap : 256; while (cap <= id) cap *= 2;
        samp_slots = realloc(samp_slots, (size_t)cap * sizeof *samp_slots);
        samp_used = realloc(samp_used, (size_t)cap);
        if (!samp_slots || !samp_used) nvk_die("sampler registry allocation");
        memset(samp_used + samp_slot_cap, 0, (size_t)(cap - samp_slot_cap));
        samp_slot_cap = cap;
    }
    if (p) { samp_slots[id] = *p; samp_used[id] = 1; } else samp_used[id] = 0;
    atomic_fetch_add_explicit(&sampler_reg_epoch, 1, memory_order_release);
    fm_unlock(&reg_lock);
}
int nvk_registered_texture(int id, tex_t *t, view_t *v, int *has_view) {
    fm_lock(&reg_lock);
    int ok = id > 0 && id < tex_slot_cap && tex_slots[id].used;
    if (ok && tex_slots[id].gen != dead_gen) follow_storage_locked(id, &tex_slots[id]);
    if (ok) { *t = tex_slots[id].tex; *v = tex_slots[id].view; *has_view = tex_slots[id].has_view; }
    fm_unlock(&reg_lock);
    return ok;
}
int nvk_registered_sampler(int id, samp_par *p) {
    fm_lock(&reg_lock);
    int ok = id >= 0 && id < samp_slot_cap && samp_used[id];
    if (ok) *p = samp_slots[id];
    fm_unlock(&reg_lock);
    return ok;
}

/* ---------- Samplers (cached by NVN parameters) ---------- */
typedef struct SamplerEntry { samp_par p; VkSampler s; struct SamplerEntry *next; } SamplerEntry;
static SamplerEntry *samplers[1024];
static pthread_mutex_t sampler_lock = PTHREAD_MUTEX_INITIALIZER;
VkSampler nvk_sampler(const samp_par *p) {
    unsigned h = 2166136261u;
    for (size_t i = 0; i < sizeof *p; ++i) h = (h ^ ((const uint8_t *)p)[i]) * 16777619u;
    pthread_mutex_lock(&sampler_lock);
    SamplerEntry *e = samplers[h % 1024];
    while (e && memcmp(&e->p, p, sizeof *p)) e = e->next;
    if (e) { pthread_mutex_unlock(&sampler_lock); return e->s; }
    VkSamplerCreateInfo ci; VkSamplerReductionModeCreateInfo red; uint32_t needs;
    samp_par q = *p;
    VkSamplerCustomBorderColorCreateInfoEXT custom = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    NvnVkSamplerResult r = nvn_vk_sampler_map(&q, &ci, &red, &needs);
    if (r == NVN_VK_SAMPLER_UNSUPPORTED_BORDER) {   /* any other colour: custom border colour */
        memset(q.border, 0, sizeof q.border);
        r = nvn_vk_sampler_map(&q, &ci, &red, &needs);
        memcpy(custom.customBorderColor.float32, p->border, sizeof p->border);
        custom.pNext = ci.pNext; ci.pNext = &custom; ci.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
    }
    if (r != NVN_VK_SAMPLER_OK) { pthread_mutex_unlock(&sampler_lock); VKLOG("NVN sampler mapping failed: %d", r); nvk_die("unsupported NVN sampler"); }
    e = calloc(1, sizeof *e);
    e->p = *p;
    {   /* Shadow samplers (depth compare): what the game asks for and what Vulkan gets, once each (shadow artefacts, 24 Oct). */
        static int said; static int formats_said;
        if (!formats_said) {
            formats_said = 1;
            static const struct { VkFormat f; const char *n; } df[] = {{VK_FORMAT_D16_UNORM, "D16"}, {VK_FORMAT_X8_D24_UNORM_PACK32, "X8D24"},
                {VK_FORMAT_D24_UNORM_S8_UINT, "D24S8"}, {VK_FORMAT_D32_SFLOAT, "D32F"}, {VK_FORMAT_D32_SFLOAT_S8_UINT, "D32FS8"}};
            for (unsigned i = 0; i < sizeof df / sizeof *df; ++i) {
                VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(nvk_phys, df[i].f, &fp);
                VKLOG("depth format %s: sampled %d, linear filtering %d, attachment %d", df[i].n,
                    !!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT), !!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT),
                    !!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT));
            }
        }
        if (ci.compareEnable && said++ < 32)
            VKLOG("shadow sampler: NVN min %u mag %u wrap %u/%u/%u cmp func %u lod %g..%g bias %g aniso %g -> Vulkan min %d mag %d mip %d compare op %d lod %g..%g",
                p->minf, p->magf, p->ws, p->wt, p->wr, p->cmp_func, p->lod_min, p->lod_max, p->lod_bias, p->aniso,
                ci.minFilter, ci.magFilter, ci.mipmapMode, ci.compareOp, ci.minLod, ci.maxLod);
    }
    VKCHECK(vkCreateSampler(nvk_dev, &ci, NULL, &e->s), "create sampler");
    e->next = samplers[h % 1024]; samplers[h % 1024] = e;
    pthread_mutex_unlock(&sampler_lock);
    return e->s;
}

/* The samplers themselves are immutable and live for the device lifetime. Repeated use of an unchanged registered
   id can bypass both the registry lock/copy and the parameter hash/second lock. Only the serial replay thread uses
   this front cache; all other threads take the original locked path. A registration concurrent with a lookup leaves
   an OLD epoch in that cache entry, forcing a fresh lookup next time. No pointers into reallocatable registries.
   TOTK_VK_SAMPLER_ID_CACHE=0 returns to the original path. */
#define SAMPLER_ID_CACHE 256
static struct { uint64_t epoch; int id; VkSampler sampler; } sampler_ids[SAMPLER_ID_CACHE];
static uint64_t sampler_id_hits, sampler_id_misses;
void nvk_sampler_id_stats(uint64_t *hits, uint64_t *misses) { *hits = sampler_id_hits; *misses = sampler_id_misses; }
static int sampler_id_cache_on(void) {   /* replay thread only */
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_VK_SAMPLER_ID_CACHE"); on = !(e && !strcmp(e, "0")); }
    return on;
}
VkSampler nvk_sampler_for_id(int id) {
    if (id < 0) return VK_NULL_HANDLE;
    int cache = atomic_load_explicit(&sampler_replay_thread, memory_order_relaxed) == (uintptr_t)pthread_self() && sampler_id_cache_on();
    uint64_t epoch = cache ? atomic_load_explicit(&sampler_reg_epoch, memory_order_acquire) : 0;
    unsigned k = (unsigned)id & (SAMPLER_ID_CACHE - 1);
    if (cache && sampler_ids[k].epoch == epoch && sampler_ids[k].id == id) {
        sampler_id_hits++; return sampler_ids[k].sampler;
    }
    samp_par p;
    if (!nvk_registered_sampler(id, &p)) return VK_NULL_HANDLE;
    VkSampler sampler = nvk_sampler(&p);
    if (cache) {
        sampler_id_misses++; sampler_ids[k].id = id; sampler_ids[k].sampler = sampler;
        sampler_ids[k].epoch = epoch;
    }
    return sampler;
}

/* ---------- Constant buffers the replay supplies ---------- */
static VkBuffer host_buffer(VkBufferUsageFlags usage, uint32_t size, void **map) {
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
    VkBuffer b; VKCHECK(vkCreateBuffer(nvk_dev, &bci, NULL, &b), "create host buffer");
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(nvk_dev, b, &req);
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
        .memoryTypeIndex = nvk_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VkDeviceMemory m; VKCHECK(vkAllocateMemory(nvk_dev, &mai, NULL, &m), "allocate host buffer");
    VKCHECK(vkBindBufferMemory(nvk_dev, b, m, 0), "bind host buffer");
    VKCHECK(vkMapMemory(nvk_dev, m, 0, VK_WHOLE_SIZE, 0, map), "map host buffer");
    memset(*map, 0, size);
    return b;
}
uint32_t nvk_zero_buffer(VkBuffer *buffer) {
    static VkBuffer b;
    if (!b) { void *m; b = host_buffer(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT, 65536, &m); }
    *buffer = b;
    return 65536;
}
/* Ryujinx support buffer: render scale 1.0 for every slot (offset 192, vec4
 * stride), no BGRA swaps, viewport transform enabled. Same as the Metal layer. */
VkBuffer nvk_support_buffer(void) {
    static VkBuffer b;
    if (!b) {
        void *m; b = host_buffer(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 65536, &m);
        for (int i = 0; i < 73; ++i) *(float *)((uint8_t *)m + 192 + 16 * i) = 1.0f;
    }
    return b;
}

void mtl_tex_release(tex_t *t) {
    if (!t || !t->serial) return;
    pthread_mutex_lock(&writeback_lock);
    Writeback **link = &writebacks;
    while (*link) {
        Writeback *e = *link;
        if (e->tex.serial == t->serial) {
            *link = e->next;
            free(e->small_before); free(e);
            break;
        }
        link = &e->next;
    }
    pthread_mutex_unlock(&writeback_lock);
    fm_lock(&reg_lock); dead_add_locked(t->serial); fm_unlock(&reg_lock);
    nvk_image_release(t->serial);
}
void mtl_prog_release(prog_t *p) { (void)p; }
void mtl_dead_forget(const uint64_t *serials, size_t n) { (void)serials; (void)n; }

/* Texture storage is pool memory in the NVN block-linear layout, as on the
 * console. No GPU pass writes textures yet, so the pool bytes are the whole
 * truth and CPU texel access converts between them and linear data. */
static void texel_region(const tex_t *t, unsigned level, const NVNcopyRegion *r, const char *what,
                         int *bpb, size_t *wb, size_t *hb, size_t *x0, size_t *y0, size_t *lp, size_t *lr, uint32_t *lz) {
    int bw, bh;
    *bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!*bpb) { VKLOG("%s: unknown NVN format %#x", what, t->d.format); nvk_die("texel access with unknown format"); }
    int nz = r->depth ? r->depth : 1, is3d = t->d.target == 2;
    if (r->xoffset < 0 || r->yoffset < 0 || r->zoffset < 0 || r->width < 0 || r->height < 0 || r->xoffset % bw || r->yoffset % bh) {
        VKLOG("%s: invalid region %d,%d,%d %dx%dx%d", what, r->xoffset, r->yoffset, r->zoffset, r->width, r->height, nz);
        nvk_die("invalid texel region");
    }
    uint32_t lw = t->d.width >> level ? t->d.width >> level : 1, lh = t->d.height >> level ? t->d.height >> level : 1;
    *lz = is3d ? (t->d.depth >> level ? t->d.depth >> level : 1) : 1;
    if ((uint32_t)(r->xoffset + r->width) > lw || (uint32_t)(r->yoffset + r->height) > lh || (is3d && (uint32_t)(r->zoffset + nz) > *lz))
        nvk_die("texel region outside the texture level");
    *wb = (size_t)((r->width + bw - 1) / bw) * (size_t)*bpb; *hb = (size_t)((r->height + bh - 1) / bh);
    *x0 = (size_t)(r->xoffset / bw) * (size_t)*bpb; *y0 = (size_t)(r->yoffset / bh);
    *lp = (size_t)((lw + bw - 1) / bw) * (size_t)*bpb; *lr = (lh + bh - 1) / bh;
}

void mtl_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) {
    int bpb; size_t wb, hb, x0, y0, lp, lr; uint32_t lz;
    if (!r->width || !r->height) return;
    nvk_image_readback(t);
    texel_region(t, level, r, "ReadTexels", &bpb, &wb, &hb, &x0, &y0, &lp, &lr, &lz);
    int nz = r->depth ? r->depth : 1, is3d = t->d.target == 2;
    uint8_t *lin = malloc(lp * lr * lz);
    if (!lin) nvk_die("ReadTexels allocation");
    for (int z = 0; z < nz; z++) {
        unsigned layer = is3d ? 0 : layer0 + (unsigned)r->zoffset + (unsigned)z;
        if ((!is3d || z == 0) && nvn_deswizzle(&t->d, level, layer, t->mem, lin, lp)) nvk_die("ReadTexels: unsupported texture layout");
        const uint8_t *src = lin + (is3d ? (size_t)(r->zoffset + z) * lp * lr : 0);
        for (size_t y = 0; y < hb; y++) memcpy(dst + (size_t)z * img + y * row, src + (y0 + y) * lp + x0, wb);
    }
    free(lin);
}

/* WriteTexels into a block-linear 2D texture or array layer: only the block rows of the region are converted, and only
 * their block rows of storage are marked written. 23 Oct, Thor: TOTK writes small regions into a 5120x5120 BC4 texture
 * (17 MiB); converting the whole level both ways per call took a game thread ~70 ms, the frame waiting on it (most of
 * the 60-170 ms stalls), and marking all of it written made the next upload the whole texture. TOTK_VK_TEXELS_ROWS=0:
 * whole level, as before. Returns 0 when the caller must take the whole-level path. */
static int write_texels_rows(const tex_t *t, unsigned level, unsigned layer, const uint8_t *src, size_t row,
                             size_t wb, size_t hb, size_t x0, size_t y0, size_t lp) {
    static int on = -1;
    static unsigned live_TOTK_VK_TEXELS_ROWS; if (on < 0 || live_TOTK_VK_TEXELS_ROWS != nvk_live_gen) { live_TOTK_VK_TEXELS_ROWS = nvk_live_gen; const char *e = nvk_live_getenv("TOTK_VK_TEXELS_ROWS"); on = !(e && !strcmp(e, "0")); }
    uint32_t row_bytes, rows, slices, bpb; uint64_t base; unsigned th, td;
    if (!on || nvn_deswizzle_geom(&t->d, level, layer, &row_bytes, &rows, &slices, &base, &th, &td, &bpb) || slices != 1 || row_bytes != lp)
        return 0;
    uint32_t r0 = (uint32_t)y0, r1 = (uint32_t)(y0 + hb);
    const uint64_t block = 512ull << th, span = (uint64_t)((row_bytes + 63) / 64) * block;
    uint64_t lo = base + (uint64_t)(r0 >> (3 + th)) * span, hi = base + (uint64_t)(((r1 - 1) >> (3 + th)) + 1) * span;
    if (hi > t->L.size) hi = t->L.size;
    if (lo >= hi) return 0;
    uint8_t *lin = malloc(lp * hb);
    if (!lin) nvk_die("WriteTexels allocation");
    dm_guest_write_prepare((uint8_t *)t->mem + lo, (size_t)(hi - lo));
    if (nvn_deswizzle_rows_compact(&t->d, level, layer, t->mem, lin, lp, r0, r0, r1)) nvk_die("WriteTexels: unsupported texture layout");
    for (size_t y = 0; y < hb; y++) memcpy(lin + y * lp + x0, src + y * row, wb);
    if (nvn_swizzle_rows_compact(&t->d, level, layer, lin, lp, t->mem, r0, r0, r1)) nvk_die("WriteTexels: unsupported texture layout");
    free(lin);
    return 1;
}

void mtl_write_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, const uint8_t *src, size_t row, size_t img) {
    int bpb; size_t wb, hb, x0, y0, lp, lr; uint32_t lz;
    if (!r->width || !r->height) return;
    int was_gpu = nvk_image_gpu_owned_serial(t->serial);
    nvk_image_readback(t);
    texel_region(t, level, r, "WriteTexels", &bpb, &wb, &hb, &x0, &y0, &lp, &lr, &lz);
    int nz = r->depth ? r->depth : 1, is3d = t->d.target == 2;
    if (!is3d && !nvn_tex_is_linear(&t->d)) {
        int done = 1;
        for (int z = 0; z < nz && done; z++)
            done = write_texels_rows(t, level, layer0 + (unsigned)r->zoffset + (unsigned)z, src + (size_t)z * img, row, wb, hb, x0, y0, lp);
        if (done) { nvk_image_cpu_written_rows(t, was_gpu); return; }
    }
    uint8_t *lin = malloc(lp * lr * lz);
    if (!lin) nvk_die("WriteTexels allocation");
    dm_guest_write_prepare(t->mem, t->L.size);
    for (int z = 0; z < nz; z++) {
        unsigned layer = is3d ? 0 : layer0 + (unsigned)r->zoffset + (unsigned)z;
        if ((!is3d || z == 0) && nvn_deswizzle(&t->d, level, layer, t->mem, lin, lp)) nvk_die("WriteTexels: unsupported texture layout");
        uint8_t *d = lin + (is3d ? (size_t)(r->zoffset + z) * lp * lr : 0);
        for (size_t y = 0; y < hb; y++) memcpy(d + (y0 + y) * lp + x0, src + (size_t)z * img + y * row, wb);
        if ((!is3d || z == nz - 1) && nvn_swizzle(&t->d, level, layer, lin, lp, t->mem)) nvk_die("WriteTexels: unsupported texture layout");
    }
    free(lin);
    nvk_image_cpu_written(t);
}

/* ---------- Presentation (nvn_vk_present.c draws; window facts here) ---------- */
int mtl_has_window(void) { return android_front_has_window(); }
int mtl_display_hz(void) { return android_front_display_hz(); }
uint32_t mtl_present_qos(void) { return 0; }
int mtl_link_active(void) { return 0; }   /* no display link: hle_nvn paces by its own clock */
int mtl_link_wait_tick(uint64_t timeout_ns, unsigned frame_ticks) { (void)timeout_ns; (void)frame_ticks; return 0; }
int mtl_warm_wait(int timeout_ms) { (void)timeout_ms; return 0; }   /* no pipeline warm-up to wait for */
void mtl_note_world(void) {}
void mtl_xv_window_tex(uint64_t serial) { (void)serial; }
void mtl_shot_at_present(const tex_t *t) { (void)t; }
void mtl_probe_pin_rt(const tex_t *t) { (void)t; }
void mtl_pin_refs(void (*mark)(const void *)) { (void)mark; }
/* Defaults for host tests that link the backend alone; the game build has the real ones (hle_nvn.c, nvn_vk_draw.c). */
__attribute__((weak)) int nvn_fence_lag_active(void) { return 0; }
__attribute__((weak)) void nvk_lag_copy_stats(uint64_t *copies, uint64_t *bytes, uint64_t *skipped) { *copies = *bytes = *skipped = 0; }
__attribute__((weak)) void nvk_cap_arena_stats(uint64_t *binds, uint64_t *copies, uint64_t *bytes) { *binds = *copies = *bytes = 0; }
__attribute__((weak)) uint64_t nvk_fetch_draws(void) { return 0; }
__attribute__((weak)) int nvk_bufwatch_hit(uint64_t addr, uint64_t n) { (void)addr; (void)n; return 0; }
__attribute__((weak)) void nvk_bufwatch_note(const char *what, uint64_t addr, uint64_t n) { (void)what; (void)addr; (void)n; }
__attribute__((weak)) void nvk_desc_cache_stats(uint64_t v[4]) { memset(v, 0, 4 * sizeof *v); }
__attribute__((weak)) void nvk_split_report(uint64_t f0, uint64_t f, double nf) { (void)f0; (void)f; (void)nf; }
/* nvn_vk_record.c; host tests that link the backend without it record directly. */
__attribute__((weak)) void nvk_record_install(void) {}
__attribute__((weak)) void nvk_record_front(void) {}
__attribute__((weak)) void nvk_record_sync(void) {}
__attribute__((weak)) void nvk_record_stats(uint64_t v[4]) { memset(v, 0, 4 * sizeof *v); }
/* Every 300 frames: render passes per frame and why they ended, plus the
 * conditional-clear and texture-redirect counts. */
static void pass_stats(uint64_t f) {
    static uint64_t f0, p0, d0, e0[NVK_END_N], c0, s0, g0, n0, fo, k0;
    if (!f0) { f0 = f ? f : 1; return; }
    if (f < f0 + 300) return;
    double nf = (double)(f - f0), e[NVK_END_N];
    {   static uint64_t fresh0, reused0, resets0;
        VKLOG("frames %llu-%llu: descriptor allocations per frame fresh %.1f, recycled %.1f, pool resets %.1f",
            (unsigned long long)f0, (unsigned long long)f, (desc_fresh - fresh0) / nf,
            (desc_recycled - reused0) / nf, (desc_pool_resets - resets0) / nf);
        fresh0 = desc_fresh; reused0 = desc_recycled; resets0 = desc_pool_resets;
    }
    {   static uint64_t old[5];
        VKLOG("frames %llu-%llu: descriptor slot reset reasons per frame: off %.2f, untracked %.2f, set limit %.2f, layout limit %.2f, multiple pools %.2f",
            (unsigned long long)f0, (unsigned long long)f, (desc_reset_reasons[0] - old[0]) / nf,
            (desc_reset_reasons[1] - old[1]) / nf, (desc_reset_reasons[2] - old[2]) / nf,
            (desc_reset_reasons[3] - old[3]) / nf, (desc_reset_reasons[4] - old[4]) / nf);
        memcpy(old, desc_reset_reasons, sizeof old);
    }
    for (int i = 0; i < NVK_END_N; ++i) { e[i] = (pass_ends[i] - e0[i]) / nf; e0[i] = pass_ends[i]; }
    uint64_t dr = nvk_draws_issued(), gsum = 0, gn = 0;
    for (unsigned k = 0; k < GRING; ++k) {
        uint64_t fr = atomic_load(&gpu_frames[k].frame);
        if (fr && fr >= f0 && fr < f && !atomic_load(&gpu_frames[k].pending)) { gsum += atomic_load(&gpu_frames[k].ns); gn++; }
    }
    VKLOG("frames %llu-%llu: %.1f passes, %.0f draws per frame; pass ends: targets %.1f, transfers %.1f, textures %.1f, buffers %.1f, dispatch %.1f, feedback %.1f, commit %.1f; same-target continues %.1f; clears as load op %.1f, skipped (rendering disabled) %llu; texture redirects %llu (unresolved %llu); state blocks not re-emitted %.0f",
        (unsigned long long)f0, (unsigned long long)f, (pass_count - p0) / nf, (dr - d0) / nf,
        e[NVK_END_RT], e[NVK_END_TRANSFER], e[NVK_END_TEXTURE], e[NVK_END_BUFFER], e[NVK_END_DISPATCH], e[NVK_END_FEEDBACK], e[NVK_END_COMMIT],
        (rt_continues - c0) / nf, (clears_folded - fo) / nf, (unsigned long long)(clears_skipped - s0), (unsigned long long)(tex_redirects - g0),
        (unsigned long long)(tex_dead_unresolved - n0), (nvk_state_skipped() - k0) / nf);
    {   static uint64_t x0[XF_N], b0[16], bo0, bt0, bk0, ui0;
        VKLOG("frames %llu-%llu: transfer pass ends per frame: buffer<->texture %.1f, texture copy %.1f, texture clear %.1f, buffer copy %.1f, uniform update %.1f, barrier %.1f; uniform updates kept in pass %.1f; barriers %.1f per frame (%.1f in an open pass, %.1f kept it open), bits in open pass: 1:%.1f 2:%.1f 4:%.1f 8:%.1f 16:%.1f 32:%.1f 64:%.1f 128:%.1f 256:%.1f 512:%.1f",
            (unsigned long long)f0, (unsigned long long)f, (xfer_ends[XF_B2T] - x0[XF_B2T]) / nf, (xfer_ends[XF_T2T] - x0[XF_T2T]) / nf,
            (xfer_ends[XF_CLEAR] - x0[XF_CLEAR]) / nf, (xfer_ends[XF_COPY] - x0[XF_COPY]) / nf, (xfer_ends[XF_UBO] - x0[XF_UBO]) / nf,
            (xfer_ends[XF_BARRIER] - x0[XF_BARRIER]) / nf, (ubo_inpass_total - ui0) / nf, (barriers_total - bt0) / nf, (barriers_open - bo0) / nf, (barriers_kept - bk0) / nf,
            (barrier_bits_open[0] - b0[0]) / nf, (barrier_bits_open[1] - b0[1]) / nf, (barrier_bits_open[2] - b0[2]) / nf, (barrier_bits_open[3] - b0[3]) / nf,
            (barrier_bits_open[4] - b0[4]) / nf, (barrier_bits_open[5] - b0[5]) / nf, (barrier_bits_open[6] - b0[6]) / nf, (barrier_bits_open[7] - b0[7]) / nf,
            (barrier_bits_open[8] - b0[8]) / nf, (barrier_bits_open[9] - b0[9]) / nf);
        memcpy(x0, xfer_ends, sizeof x0); memcpy(b0, barrier_bits_open, sizeof b0); bo0 = barriers_open; bt0 = barriers_total; bk0 = barriers_kept; ui0 = ubo_inpass_total; }
    {   static uint64_t r0[4]; uint64_t r[4]; nvk_record_stats(r);
        if (r[0] != r0[0]) VKLOG("frames %llu-%llu: recording thread: %.0f calls queued per frame, %.0f waits for it to catch up (%.0f of them actually waited)",
            (unsigned long long)f0, (unsigned long long)f, (r[0] - r0[0]) / nf, (r[1] - r0[1]) / nf, (r[2] - r0[2]) / nf);
        memcpy(r0, r, sizeof r0); }
    {
        static uint64_t old[3]; uint64_t now[3]; nvk_record_overlap_stats(now);
        if (now[0] != old[0]) VKLOG("frames %llu-%llu: asynchronous segment handovers %.1f per frame; replay waits for recorder %.3f ms/frame; segment queue lock + submit %.3f ms/frame",
            (unsigned long long)f0, (unsigned long long)f, (now[0] - old[0]) / nf,
            (now[1] - old[1]) / nf / 1e6, (now[2] - old[2]) / nf / 1e6);
        memcpy(old, now, sizeof old);
    }
    {
        static uint64_t before;
        if (checkpoint_cuts != before) VKLOG("frames %llu-%llu: safe draw checkpoints submitted %.1f extra segments per frame without ending a pass",
            (unsigned long long)f0, (unsigned long long)f, (checkpoint_cuts - before) / nf);
        before = checkpoint_cuts;
    }
    {   static uint64_t pn0, po0, pq0, ps0, wk0, wn0;
        pthread_mutex_lock(&done_lock);
        uint64_t pn = post_ns, po = post_done_ops, pq = post_queries, ps = post_subs, wk = wake_ns, wn = wake_n;
        pthread_mutex_unlock(&done_lock);
        if (ps != ps0) VKLOG("frames %llu-%llu: after GPU completion: %.0f post-ops (%.0f occlusion queries) per submission take %.3f ms before the fence signal%s; the waiting game wakes %.3f ms after it",
            (unsigned long long)f0, (unsigned long long)f, (double)(po - po0) / (double)(ps - ps0), (double)(pq - pq0) / (double)(ps - ps0),
            (double)(pn - pn0) / 1e6 / (double)(ps - ps0), query_batch_on() ? " (queries read in one call)" : "", wn != wn0 ? (double)(wk - wk0) / 1e6 / (double)(wn - wn0) : 0.0);
        pn0 = pn; po0 = po; pq0 = pq; ps0 = ps; wk0 = wk; wn0 = wn; }
    {   static uint64_t s0u, w0u;
        if (ubo_index_skips + ubo_index_walks != s0u + w0u) VKLOG("frames %llu-%llu: uniform log reads per frame: answered by the page index %.0f, walked %.0f",
            (unsigned long long)f0, (unsigned long long)f, (ubo_index_skips - s0u) / nf, (ubo_index_walks - w0u) / nf);
        s0u = ubo_index_skips; w0u = ubo_index_walks; }
    {
        static uint64_t sh0, sm0; uint64_t sh, sm; nvk_sampler_id_stats(&sh, &sm);
        if (sh + sm != sh0 + sm0) VKLOG("frames %llu-%llu: sampler id cache per frame: reused %.0f, resolved %.0f",
            (unsigned long long)f0, (unsigned long long)f, (sh - sh0) / nf, (sm - sm0) / nf);
        sh0 = sh; sm0 = sm;
    }
    if (ubo_pool_on()) {
        static uint64_t ph0, pa0;
        VKLOG("frames %llu-%llu: uniform log storage per frame: reused %.0f, allocated %.0f; idle pool %.1f KiB",
            (unsigned long long)f0, (unsigned long long)f, (ubo_pool_hits - ph0) / nf,
            (ubo_pool_allocs - pa0) / nf, ubo_pool_bytes / 1024.0);
        ph0 = ubo_pool_hits; pa0 = ubo_pool_allocs;
    }
    if (gn) VKLOG("frames %llu-%llu: GPU %.2f ms per frame (mean of %llu recent frames)", (unsigned long long)f0, (unsigned long long)f,
        gsum / 1e6 / (double)gn, (unsigned long long)gn);
    {   static uint64_t w0, s0f, r0, k0f;
        VKLOG("frames %llu-%llu: sampled attachments per frame: split because written in the pass %.1f, the draw writes it %.1f, written after sampling %.1f; kept the pass %.1f",
            (unsigned long long)f0, (unsigned long long)f, (fb_written - w0) / nf, (fb_self - s0f) / nf, (fb_war - r0) / nf, (fb_kept - k0f) / nf);
        w0 = fb_written; s0f = fb_self; r0 = fb_war; k0f = fb_kept; }
    pt_report(f0, f, nf);
    if (udiff_on()) { uint64_t v[5]; nvk_ubo_diff_stats(v);
        VKLOG("frames %llu-%llu: uniform diff (totals): %llu ranges compared, %llu changed before completion (%llu of them hold a tiny render target), %llu skipped for in-pass updates, %llu dropped (list full)",
            (unsigned long long)f0, (unsigned long long)f, (unsigned long long)v[0], (unsigned long long)v[1], (unsigned long long)v[2], (unsigned long long)v[3], (unsigned long long)v[4]); }
    {   static uint64_t us0, ub0, un0; uint64_t us, ub, un; nvk_ucap_stats(&us, &ub, &un);
        if (us != us0) VKLOG("frames %llu-%llu: uniforms captured at the submit call: %.1f submissions per frame, %.1f KiB and %.3f ms (game thread) each",
            (unsigned long long)f0, (unsigned long long)f, (us - us0) / nf, (ub - ub0) / 1024.0 / (double)(us - us0), (un - un0) / 1e6 / (double)(us - us0));
        us0 = us; ub0 = ub; un0 = un; }
    VKLOG("frames %llu-%llu: tiny targets (totals): noted %llu, written back %llu, CPU wrote first %llu, pass without writes %llu, no slot %llu, not copyable %llu; "
        "other CPU-visible colour targets: over the limit (%llu bytes) %llu, layered/3D %llu, other level/layer view %llu", (unsigned long long)f0, (unsigned long long)f,
        (unsigned long long)tiny_noted, (unsigned long long)atomic_load(&tiny_done), (unsigned long long)atomic_load(&tiny_cpu_skip), (unsigned long long)tiny_unwritten,
        (unsigned long long)atomic_load(&tiny_no_slot), (unsigned long long)atomic_load(&tiny_refused), (unsigned long long)tiny_max(), (unsigned long long)tiny_big, (unsigned long long)tiny_shape,
        (unsigned long long)tiny_view);
    {   static uint64_t lc0, lb0, ls0; uint64_t lc, lb, ls; nvk_lag_copy_stats(&lc, &lb, &ls);
        extern int nvk_ubo_copying(void);
        if (nvk_ubo_copying() || lc != lc0) VKLOG("frames %llu-%llu: uniform copies per frame %.0f (%.2f MiB), not copied because GPU work still writes them %.1f",
            (unsigned long long)f0, (unsigned long long)f, (lc - lc0) / nf, (lb - lb0) / nf / 1048576.0, (ls - ls0) / nf);
        lc0 = lc; lb0 = lb; ls0 = ls; }
    {   static uint64_t cb0, cc0, cy0; uint64_t cb, cc, cy; nvk_cap_arena_stats(&cb, &cc, &cy);
        if (cb != cb0) VKLOG("frames %llu-%llu: uniforms bound from the capture arena per frame %.0f (%.1f range copies, %.2f MiB copied)",
            (unsigned long long)f0, (unsigned long long)f, (cb - cb0) / nf, (cc - cc0) / nf, (cy - cy0) / nf / 1048576.0);
        cb0 = cb; cc0 = cc; cy0 = cy; }
    {   static uint64_t gf0, gs0, gr0; uint64_t gf, gs, gr; nvk_gw_stats(&gf, &gs, &gr);
        if (gf != gf0 || gs != gs0) VKLOG("frames %llu-%llu: GPU-write checks per frame: %.0f answered by the page index, %.0f ring scans (%.0f ring entries stepped)",
            (unsigned long long)f0, (unsigned long long)f, (gf - gf0) / nf, (gs - gs0) / nf, (gr - gr0) / nf);
        gf0 = gf; gs0 = gs; gr0 = gr; }
    if (counter_gpu_time_on()) { static uint64_t cr0, ce0;
        VKLOG("frames %llu-%llu: timestamp reports with GPU time %.1f per frame (%llu without a free slot); CPU-GPU offset %.3f ms",
            (unsigned long long)f0, (unsigned long long)f, (cts_reports - cr0) / nf, (unsigned long long)(cts_exhausted - ce0), cts_offset_ns / 1e6);
        cr0 = cts_reports; ce0 = cts_exhausted; }
    {   static uint64_t fd0, lb0, wb0;
        if (nvk_fetch_draws() != fd0) VKLOG("frames %llu-%llu: framebuffer fetch draws per frame %.1f (local-read barriers %.1f, depth write-after-read barriers %.1f)",
            (unsigned long long)f0, (unsigned long long)f, (nvk_fetch_draws() - fd0) / nf, (local_read_barriers - lb0) / nf, (war_barriers - wb0) / nf);
        fd0 = nvk_fetch_draws(); lb0 = local_read_barriers; wb0 = war_barriers; }
    nvk_split_report(f0, f, nf);
    {   static uint64_t ps0, pn0;
        VKLOG("frames %llu-%llu: pass-end barriers per frame: none needed %.1f, attachment-scoped %.1f",
            (unsigned long long)f0, (unsigned long long)f, (pass_barriers_skipped - ps0) / nf, (pass_barriers_narrow - pn0) / nf);
        ps0 = pass_barriers_skipped; pn0 = pass_barriers_narrow; }
    {   static uint64_t dt0, do0, dl0;
        VKLOG("frames %llu-%llu: discards per frame %.1f: attachments not loaded %.1f, after the pass began (kept) %.1f",
            (unsigned long long)f0, (unsigned long long)f, (discards_total - dt0) / nf, (discards_load - dl0) / nf, (discards_open - do0) / nf);
        dt0 = discards_total; dl0 = discards_load; do0 = discards_open; }
    {   static uint64_t sb0, sg0;
        VKLOG("frames %llu-%llu: %.2f NVN submissions and %.2f extra segments per frame", (unsigned long long)f0, (unsigned long long)f,
            (submits_total - sb0) / nf, (segments_total - sg0) / nf);
        sb0 = submits_total; sg0 = segments_total; }
    {   static uint64_t a0, b0, c0p, m0;
        VKLOG("frames %llu-%llu: attachment use prediction per frame: passes %.1f, attachments not stored %.1f, not loaded either %.1f; mispredictions (pass split) %.2f",
            (unsigned long long)f0, (unsigned long long)f, (up_predicted - a0) / nf, (up_store_none - b0) / nf, (up_load_none - c0p) / nf, (up_mispredicted - m0) / nf);
        a0 = up_predicted; b0 = up_store_none; c0p = up_load_none; m0 = up_mispredicted; }
    {   static uint64_t v0[4]; uint64_t v[4]; nvk_desc_cache_stats(v);
        double hit = (double)(v[0] - v0[0]), miss = (double)(v[1] - v0[1]) + (double)(v[2] - v0[2]);
        VKLOG("frames %llu-%llu: descriptor sets per frame: reused %.0f, written %.0f (%.0f%% reused; %.0f too large to cache); binds skipped %.0f",
            (unsigned long long)f0, (unsigned long long)f, hit / nf, miss / nf, hit + miss > 0 ? 100.0 * hit / (hit + miss) : 0.0,
            (v[2] - v0[2]) / nf, (v[3] - v0[3]) / nf);
        memcpy(v0, v, sizeof v0); }
    { static uint64_t emitted0, elided0;
        if (ubo_gpu_updates != emitted0 || ubo_gpu_elided != elided0)
            VKLOG("frames %llu-%llu: GPU uniform updates %.1f emitted, %.1f fully overwritten updates eliminated per frame",
                (unsigned long long)f0, (unsigned long long)f, (ubo_gpu_updates - emitted0) / nf, (ubo_gpu_elided - elided0) / nf);
        emitted0 = ubo_gpu_updates; elided0 = ubo_gpu_elided;
    }
    f0 = f; p0 = pass_count; d0 = dr; c0 = rt_continues; s0 = clears_skipped; g0 = tex_redirects; n0 = tex_dead_unresolved; fo = clears_folded; k0 = nvk_state_skipped();
}
__attribute__((weak)) void nvk_draw_frame_end(void) {}
/* Freeze logger: a frame longer than TOTK_HITCH_MS (default 60) logs what happened during it, so each freeze has a
   named cause: shaders loaded or translated, pipelines built while the replay waited (compute, loading screens),
   texture uploads, waiting for the GPU, and the draws it had. At most 400 lines. TOTK_HITCH_MS=0: off. */
static void hitch_check(uint64_t f) {
    static int64_t limit = -1; static uint64_t last, s0, sn0, p0, pn0, u0, un0, ub0, w0, d0; static unsigned said;
    if (limit < 0) { const char *e = getenv("TOTK_HITCH_MS"); limit = e && *e ? atoll(e) : 60; }
    uint64_t now = nvk_now_ns(), s = atomic_load(&nvk_shader_ns), sn = atomic_load(&nvk_shader_count), p = atomic_load(&nvk_pipeline_ns),
             pn = atomic_load(&nvk_pipeline_count), u = atomic_load(&nvk_upload_ns), un = atomic_load(&nvk_upload_n), ub = atomic_load(&nvk_upload_bytes),
             w = atomic_load(&gpu_wait_ns), d = nvk_draws_issued();
    if (last && limit > 0 && now - last > (uint64_t)limit * 1000000ull && said < 400) {
        said++;
        double ms = (now - last) / 1e6, sm = (s - s0) / 1e6, pm = (p - p0) / 1e6, um = (u - u0) / 1e6, wm = (w - w0) / 1e6;
        VKLOG("freeze: frame %llu took %.0f ms: shaders loaded %.1f ms (%llu), pipelines built while waiting %.1f ms (%llu), texture uploads %.1f ms (%llu, %.1f MiB), waiting for the GPU %.1f ms, %llu draws, rest %.1f ms",
            (unsigned long long)f, ms, sm, (unsigned long long)(sn - sn0), pm, (unsigned long long)(pn - pn0), um, (unsigned long long)(un - un0),
            (ub - ub0) / 1048576.0, wm, (unsigned long long)(d - d0), ms - sm - pm - um - wm);
    }
    last = now; s0 = s; sn0 = sn; p0 = p; pn0 = pn; u0 = u; un0 = un; ub0 = ub; w0 = w; d0 = d;
}
void mtl_set_frame(uint64_t f) { atomic_store(&current_frame, f); hitch_check(f); pass_stats(f); nvk_shader_debug_frame(f); nvk_draw_frame_end(); }

/* ---------- Statistics ---------- */
void mtl_alloc_stats(uint64_t *waits, uint64_t *grown, int *n) { *waits = 0; *grown = 0; *n = RING; }
void mtl_args_stats(uint64_t *enc, uint64_t *reuse) { *enc = 0; *reuse = 0; }
void mtl_blit_stats(uint64_t v[6]) { memset(v, 0, 6 * sizeof *v); }
void mtl_bw(uint64_t *load, uint64_t *store, uint64_t *full) { *load = 0; *store = 0; *full = 0; }
uint64_t mtl_chunk_commits(void) { return 0; }
void mtl_create_ns(uint64_t *lib_ns, uint64_t *pso_ns) { *lib_ns = atomic_load(&nvk_shader_ns); *pso_ns = atomic_load(&nvk_pipeline_ns); }
void mtl_create_stats(uint64_t *lib, uint64_t *pso) { *lib = atomic_load(&nvk_shader_count); *pso = atomic_load(&nvk_pipeline_count); }
uint64_t mtl_drawable_ns_take(void) { return 0; }
uint64_t mtl_ds_omit(void) { return 0; }
double mtl_gpu_ms(uint64_t f) {
    unsigned k = f % GRING;
    if (atomic_load(&gpu_frames[k].frame) != f || atomic_load(&gpu_frames[k].pending)) return -1;
    return atomic_load(&gpu_frames[k].ns) / 1e6;
}
int mtl_gpu_span(uint64_t f, uint64_t *t0, uint64_t *t1) { (void)f; (void)t0; (void)t1; return 0; }
void mtl_keepalive_stats(uint64_t v[3]) { memset(v, 0, 3 * sizeof *v); }
void mtl_layer_stats(uint64_t v[4]) { memset(v, 0, 4 * sizeof *v); }
uint64_t mtl_pkg_miss(void) { return 0; }
void mtl_pkg_stats(uint64_t *uploads, uint64_t *reuploads) { nvk_upload_stats(uploads, reuploads, NULL); }
void mtl_ren_stats(uint64_t v[6]) { memset(v, 0, 6 * sizeof *v); }
void mtl_stats(uint64_t *passes, uint64_t *draws) { *passes = pass_count; *draws = nvk_draws_issued(); }
void mtl_ub_present(double iv) { (void)iv; }
uint64_t mtl_udc_saved_bytes(void) { return 0; }
uint64_t mtl_upload_bytes(void) { uint64_t bytes; nvk_upload_stats(NULL, NULL, &bytes); return bytes; }
uint64_t mtl_upload_gpu_ns_take(void) { return 0; }
uint64_t mtl_upload_ns_take(void) { return nvk_upload_cpu_ns_take(); }
void mtl_upload_wait_stats(uint64_t *n, uint64_t *ns) { *n = 0; *ns = 0; }
void mtl_use_stats(uint64_t *call, uint64_t *skip) { *call = 0; *skip = 0; }
