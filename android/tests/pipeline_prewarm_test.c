#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <fcntl.h>
static int test_setname(pthread_t t, const char *name) { (void)t; (void)name; return 0; }
#define pthread_setname_np test_setname
#define WARM_LIMIT 64u   /* Exercise production saturation/replacement with a small history. */
#include "../nvn_vk_draw.c"

VkDevice nvk_dev;
VkPhysicalDevice nvk_phys;
int android_vk_local_read;
static NvkShader shader = {.stage = 5, .module = (VkShaderModule)(uintptr_t)1,
    .layout = (VkDescriptorSetLayout)(uintptr_t)2};
static _Atomic unsigned compute_creates;
static _Atomic int finish_build, draw_done;
_Noreturn void nvk_die(const char *s) { fprintf(stderr, "%s\n", s); abort(); }
NvkShader *nvk_shader_find_variant(const char *name) {
    return strcmp(name, shader.variant) ? NULL : &shader;
}
static _Atomic unsigned preloads;
void nvk_shader_seed_load(void) {}   /* no seed in this test */
int nvk_shader_preload(const char *name, int stage) {   /* the recorded compute variant is "cached" */
    atomic_fetch_add(&preloads, 1); return !strcmp(name, shader.variant) && stage == 5;
}
VkPipelineCache nvk_pipeline_cache_begin(void) { return VK_NULL_HANDLE; }
void nvk_pipeline_cache_end(void) {}
static void properties(VkPhysicalDevice d, VkPhysicalDeviceProperties *p) {
    (void)d; memset(p, 0, sizeof *p); p->vendorID = 0x5143; p->deviceID = 740;
    p->driverVersion = 123; memset(p->pipelineCacheUUID, 0x5a, VK_UUID_SIZE);
}
static VkResult create_layout(VkDevice d, const VkPipelineLayoutCreateInfo *ci,
        const VkAllocationCallbacks *a, VkPipelineLayout *out) {
    (void)d; (void)ci; (void)a; *out = (VkPipelineLayout)(uintptr_t)3; return VK_SUCCESS;
}
static VkResult create_compute(VkDevice d, VkPipelineCache cache, uint32_t n,
        const VkComputePipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *out) {
    (void)d; (void)a; assert(n == 1 && cache == VK_NULL_HANDLE);
    assert(ci->stage.module == shader.module);
    atomic_fetch_add(&compute_creates, 1);
    while (!atomic_load(&finish_build)) {
        struct timespec delay = {0, 1000000}; nanosleep(&delay, NULL);
    }
    *out = (VkPipeline)(uintptr_t)4; return VK_SUCCESS;
}
static _Atomic unsigned graphics_creates;
static _Atomic int graphics_release;
static pthread_t graphics_thread;
static VkResult create_graphics(VkDevice d, VkPipelineCache cache, uint32_t n,
        const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *out) {
    (void)d; (void)cache; (void)a; assert(n == 1 && ci->stageCount == 2);
    graphics_thread = pthread_self();
    atomic_fetch_add(&graphics_creates, 1);
    while (!atomic_load(&graphics_release)) { struct timespec delay = {0, 1000000}; nanosleep(&delay, NULL); }
    *out = (VkPipeline)(uintptr_t)6; return VK_SUCCESS;
}
static VkResult create_set_layout(VkDevice d, const VkDescriptorSetLayoutCreateInfo *ci,
        const VkAllocationCallbacks *a, VkDescriptorSetLayout *out) {
    (void)d; (void)ci; (void)a; *out = (VkDescriptorSetLayout)(uintptr_t)5; return VK_SUCCESS;
}
PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties = properties;
PFN_vkCreatePipelineLayout vkCreatePipelineLayout = create_layout;
PFN_vkCreateComputePipelines vkCreateComputePipelines = create_compute;
PFN_vkCreateGraphicsPipelines vkCreateGraphicsPipelines = create_graphics;
PFN_vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout = create_set_layout;
static void *draw_thread(void *key) {
    assert(pipeline((PipeKey *)key));
    atomic_store(&draw_done, 1);
    return NULL;
}

int main(int argc, char **argv) {
    assert(argc == 2);
    snprintf(warm_path, sizeof warm_path, "%s/pipeline-warm.bin", argv[1]);
    strcpy(shader.variant,"abcdef0123456789abcdef0123456789-abcdef012345");
    atomic_store(&warm_started,1);
    PipeKey saturation={.cs=&shader};
    for(unsigned i=0;i<WARM_LIMIT;++i){saturation.chmask=i;warm_record(&saturation);}
    assert(warm_count==WARM_LIMIT);
    WarmDisk first=warm_entries[0].disk,second=warm_entries[1].disk;
    saturation.chmask=0;warm_record(&saturation); /* Preserve a recently used old entry. */
    saturation.chmask=WARM_LIMIT;warm_record(&saturation);
    assert(warm_count==WARM_LIMIT);
    assert(warm_find(&first,warm_hash(&first,sizeof first)));
    assert(!warm_find(&second,warm_hash(&second,sizeof second)));
    warm_preload();assert(atomic_load(&preloads)==1); /* 64 references, one module load. */
    WarmDisk history[WARM_LIMIT];unsigned nh=0;
    for(uint32_t id=warm_oldest;id;id=warm_entries[id-1].newer)history[nh++]=warm_entries[id-1].disk;
    assert(nh==WARM_LIMIT);warm_save(history,nh);warm_count=0;warm_load();
    assert(warm_count==WARM_LIMIT && warm_find(&first,warm_hash(&first,sizeof first)));
    puts("warm history saturation: pass (LRU replacement, deduplication, indexed reload, unique preloads)");
    warm_count=0;warm_index_rebuild();warm_revision=0;
    atomic_store(&warm_started,0);atomic_store(&preloads,0);
    warm_props.vendorID = 0x5143; warm_props.deviceID = 740;
    warm_props.driverVersion = 123;
    memset(warm_props.pipelineCacheUUID, 0x5a, VK_UUID_SIZE);
    WarmDisk record = {0};
    strcpy(record.vs, "0123456789abcdef0123456789abcdef-0123456789ab");
    strcpy(record.ps, "abcdef0123456789abcdef0123456789-abcdef012345");
    record.key.ncolor = 1;
    record.key.color[0] = VK_FORMAT_R8G8B8A8_UNORM;
    record.key.topo_class = 2;
    record.key.polygon = NVN_POLYGON_MODE_FILL;
    assert(warm_valid(&record));
    WarmDisk fetch = record;   /* framebuffer fetch variant names end in -F<attachment mask> */
    strcpy(fetch.ps, "abcdef0123456789abcdef0123456789-abcdef012345-Ffffffff0");
    assert(warm_valid(&fetch));
    strcpy(fetch.ps, "abcdef0123456789abcdef0123456789-abcdef012345-G");
    assert(!warm_valid(&fetch));
    /* Renderer-generated depth variants must survive a restart too. One rejected
     * name used to discard the entire otherwise valid prewarm history. */
    const char *suffixes[] = {"-zq", "-zs", "-F00000008-Z3210", "-i3-zq", "-i3-zs"};
    WarmDisk derived[5];
    for (unsigned j = 0; j < 5; ++j) {
        derived[j] = record;
        snprintf(derived[j].ps, sizeof derived[j].ps, "%.45s%s", record.ps, suffixes[j]);
        assert(warm_valid(&derived[j]));
    }
    warm_save(derived, 5);
    warm_count = 0;
    warm_load();
    assert(warm_count == 5);
    for (unsigned j = 0; j < 5; ++j)
        assert(!memcmp(&warm_entries[j].disk, &derived[j], sizeof derived[j]));
    strcpy(fetch.ps, "../shader-zq"); assert(!warm_valid(&fetch));
    strcpy(fetch.ps, "/shader-zs"); assert(!warm_valid(&fetch));
    memset(fetch.ps, 'a', sizeof fetch.ps); assert(!warm_valid(&fetch));
    record.key.vs = (NvkShader *)(uintptr_t)1;
    assert(!warm_valid(&record));
    record.key.vs = NULL;
    warm_save(&record, 1);
    warm_load();
    assert(warm_count == 1 && !memcmp(&warm_entries[0].disk, &record, sizeof record));
    FILE *f = fopen(warm_path, "r+b"); assert(f);
    assert(fseek(f, (long)sizeof(WarmHeader), SEEK_SET) == 0);
    int c = fgetc(f); assert(c != EOF);
    assert(fseek(f, -1, SEEK_CUR) == 0);
    assert(fputc(c ^ 1, f) != EOF); fclose(f);
    warm_count = 0;
    warm_load();
    assert(warm_count == 0);
    /* The history outlives a driver switch; fetch variants only where this driver can build them. */
    snprintf(warm_path, sizeof warm_path, "%s/cross.bin", argv[1]);
    WarmDisk two[2] = {record, record};
    strcpy(two[1].ps, "abcdef0123456789abcdef0123456789-abcdef012345-F00000000");
    warm_save(two, 2);
    warm_props.driverVersion = 999;
    android_vk_local_read = 0; warm_load();
    assert(warm_count == 1 && !memcmp(&warm_entries[0].disk, &record, sizeof record));
    android_vk_local_read = 1; warm_load();
    assert(warm_count == 2 && !memcmp(&warm_entries[1].disk, &two[1], sizeof two[1]));
    warm_save(two, 2); android_vk_local_read = 0; warm_load();
    assert(warm_count == 2);   /* this driver's own fetch records are kept */
    /* Of the per-driver files of before, the newest is taken over. */
    warm_props.driverVersion = 123;
    char legacy[2][900];
    for (int j = 0; j < 2; ++j) {
        snprintf(legacy[j], sizeof legacy[j], "%s/pipeline-warm-v1-%08x-%08x-%08x-%02d.bin", argv[1],
            warm_props.vendorID, warm_props.deviceID, 200u + j, j);
        snprintf(warm_path, sizeof warm_path, "%s", legacy[j]);
        WarmDisk recs[2] = {record, record}; recs[1].key.chmask = 7u + j;
        warm_save(recs, 2);
        struct timespec times[2] = {{.tv_sec = 1000 + j}, {.tv_sec = 1000 + j}};
        assert(!utimensat(AT_FDCWD, legacy[j], times, 0));
    }
    snprintf(warm_path, sizeof warm_path, "%s/pipeline-warm-v2-%08x-%08x.bin", argv[1], warm_props.vendorID, warm_props.deviceID);
    warm_load();   /* only the newest per-driver file */
    assert(warm_count == 2 && warm_entries[1].disk.key.chmask == 8u);
    assert(!unlink(legacy[0]) && !unlink(legacy[1]));
    android_vk_local_read = 1;
    puts("history across drivers: pass (other driver loaded, fetch filtered without local read, newest per-driver file taken over)");
    WarmDisk compute = {0};
    strcpy(compute.cs, "abcdef0123456789abcdef0123456789-abcdef012345");
    strcpy(shader.variant, compute.cs);
    assert(warm_valid(&compute));
    char uuid[VK_UUID_SIZE * 2 + 1];
    for (unsigned i = 0; i < VK_UUID_SIZE; ++i)
        snprintf(uuid + i * 2, 3, "%02x", warm_props.pipelineCacheUUID[i]);
    snprintf(warm_path, sizeof warm_path, "%s/pipeline-warm-v1-%08x-%08x-%08x-%s.bin",
        argv[1], warm_props.vendorID, warm_props.deviceID, warm_props.driverVersion, uuid);
    warm_save(&compute, 1);
    warm_count = 0;
    setenv("TOTK_SHADER_CACHE", argv[1], 1);
    warm_initialize();
    for (unsigned i = 0; i < 200 && !atomic_load(&compute_creates); ++i) {
        struct timespec delay = {0, 10000000}; nanosleep(&delay, NULL);
    }
    assert(atomic_load(&compute_creates) == 1 && atomic_load(&preloads) == 1);   /* loaded before the scan */
    PipeKey key = {.cs = &shader};
    pthread_t draw; assert(!pthread_create(&draw, NULL, draw_thread, &key));
    struct timespec delay = {0, 20000000}; nanosleep(&delay, NULL);
    assert(!atomic_load(&draw_done) && atomic_load(&compute_creates) == 1);
    atomic_store(&finish_build, 1);
    pthread_join(draw, NULL);
    for (unsigned i = 0; i < 200 && !atomic_load(&warm_built); ++i) {
        struct timespec delay2 = {0, 10000000}; nanosleep(&delay2, NULL);
    }
    assert(atomic_load(&warm_built) == 1 && atomic_load(&warm_waits) == 1);
    assert(pipeline(&key) && atomic_load(&compute_creates) == 1);
    /* Background compilation: a draw never waits; the key is queued once; the draw gets it when built. */
    static NvkShader vs = {.stage = 0, .module = (VkShaderModule)(uintptr_t)7, .layout = (VkDescriptorSetLayout)(uintptr_t)8},
        ps = {.stage = 1, .module = (VkShaderModule)(uintptr_t)9, .layout = (VkDescriptorSetLayout)(uintptr_t)10};
    PipeKey gk = {.vs = &vs, .ps = &ps, .ncolor = 1, .topo_class = 2};
    gk.color[0] = VK_FORMAT_R8G8B8A8_UNORM;
    atomic_store(&frame_draws_prev, 5000);   /* gameplay */
    assert(!pipeline_for_draw(&gk) && !pipeline_for_draw(&gk));   /* still compiling: left out, no wait */
    for (unsigned i = 0; i < 200 && !atomic_load(&graphics_creates); ++i) { struct timespec d2 = {0, 10000000}; nanosleep(&d2, NULL); }
    assert(atomic_load(&graphics_creates) == 1 && !pthread_equal(graphics_thread, pthread_self()));
    assert(!pipeline_for_draw(&gk) && atomic_load(&async_skipped) == 3);
    atomic_store(&graphics_release, 1);
    Pipe *gp = NULL;
    for (unsigned i = 0; i < 200 && !(gp = pipeline_for_draw(&gk)); ++i) { struct timespec d3 = {0, 10000000}; nanosleep(&d3, NULL); }
    assert(gp && gp->pipe == (VkPipeline)(uintptr_t)6 && atomic_load(&graphics_creates) == 1 && atomic_load(&async_built) == 1);
    atomic_store(&frame_draws_prev, 200);   /* a loading screen: built before the draw returns, never left out */
    PipeKey lk = gk; lk.topo_class = 1;
    uint64_t skipped_before = atomic_load(&async_skipped);
    assert(pipeline_for_draw(&lk) && atomic_load(&graphics_creates) == 2 && atomic_load(&async_skipped) == skipped_before);
    puts("background pipelines: pass (gameplay draw never waits, one build per key, built off the calling thread, ready afterwards; loading screens wait)");
    puts("pipeline prewarm: pass (pointerless records, fetch variant names, corruption rejection, preload before scan, in-flight draw wait and reuse)");
}
