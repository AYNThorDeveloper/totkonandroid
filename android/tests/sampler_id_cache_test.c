/* Actual registry + sampler creation/cache code. Checks invalidation and concurrent registration, not a model. */
#include <pthread.h>
static int test_setname(pthread_t t, const char *n) { (void)t; (void)n; return 0; }
#define pthread_setname_np test_setname
#include "../nvn_vk_backend.c"
/* Tiny target write-backs live in nvn_vk_image.c, not linked here. */
void *nvk_tiny_wb_record(VkCommandBuffer c, const tex_t *t, unsigned slot, size_t cap, unsigned slots) { (void)c; (void)t; (void)slot; (void)cap; (void)slots; return NULL; }
int nvk_tiny_wb_finish(void *s, uint64_t st) { (void)s; (void)st; return 0; }
#include <assert.h>
_Noreturn void runtime_fail(const char *m) { fprintf(stderr, "%s\n", m); abort(); }
static unsigned created;
static int mutate_id = -1;
static samp_par changed;
static VkResult make_sampler(VkDevice d, const VkSamplerCreateInfo *ci, const VkAllocationCallbacks *a, VkSampler *s) {
    (void)d; (void)a; assert(ci->sType == VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
    *s = (VkSampler)(uintptr_t)(0x1000 + ++created);
    if (mutate_id >= 0) { int id = mutate_id; mutate_id = -1; mtl_reg_samp(id, &changed); }
    return VK_SUCCESS;
}
PFN_vkCreateSampler vkCreateSampler = make_sampler;
/* nvk_sampler logs the depth formats' filtering support once (shadow artefacts): no format features on the host. */
static void format_props(VkPhysicalDevice d, VkFormat f, VkFormatProperties *p) { (void)d; (void)f; memset(p, 0, sizeof *p); }
PFN_vkGetPhysicalDeviceFormatProperties vkGetPhysicalDeviceFormatProperties = format_props;
static samp_par p0, p1;
static VkSampler s0, s1;
static void *other_reader(void *unused) {
    (void)unused;
    for (unsigned i = 0; i < 1000; ++i) assert(nvk_sampler_for_id(7) == s0);
    return NULL;
}
static void *registrar(void *unused) {
    (void)unused;
    for (unsigned i = 0; i < 20000; ++i) mtl_reg_samp(7, i % 3 == 0 ? NULL : i % 3 == 1 ? &p0 : &p1);
    return NULL;
}
int main(void) {
    atomic_store(&sampler_replay_thread, (uintptr_t)pthread_self());
    int on = sampler_id_cache_on();
    p0 = (samp_par){.minf=NVN_MIN_FILTER_LINEAR, .magf=NVN_MAG_FILTER_LINEAR,
        .ws=NVN_WRAP_MODE_CLAMP_TO_EDGE, .wt=NVN_WRAP_MODE_CLAMP_TO_EDGE, .wr=NVN_WRAP_MODE_CLAMP_TO_EDGE,
        .lod_max=1000, .aniso=1};
    p1 = p0; p1.magf = NVN_MAG_FILTER_NEAREST;
    assert(!nvk_sampler_for_id(-1) && !nvk_sampler_for_id(7));
    mtl_reg_samp(7, &p0); s0 = nvk_sampler_for_id(7); assert(s0 && created == 1);
    uint64_t hits = sampler_id_hits;
    for (unsigned i = 0; i < 100; ++i) assert(nvk_sampler_for_id(7) == s0);
    assert(created == 1 && sampler_id_hits == hits + (on ? 100 : 0));
    mtl_reg_samp(7, &p1); s1 = nvk_sampler_for_id(7); assert(s1 && s1 != s0 && created == 2);
    mtl_reg_samp(7, NULL); assert(!nvk_sampler_for_id(7));
    mtl_reg_samp(7, &p0); assert(nvk_sampler_for_id(7) == s0);
    mtl_reg_samp(7 + SAMPLER_ID_CACHE, &p1);
    assert(nvk_sampler_for_id(7 + SAMPLER_ID_CACHE) == s1 && nvk_sampler_for_id(7) == s0);
    mtl_reg_samp(0, &p0); assert(nvk_sampler_for_id(0) == s0);
    /* If registration changes during a slow resolution, don't stamp the result with the newer epoch. */
    samp_par third = p0; third.lod_bias = 0.5f;
    changed = p1; mtl_reg_samp(9, &third); mutate_id = 9;
    VkSampler third_s = nvk_sampler_for_id(9); assert(third_s != s1 && third_s != s0);
    assert(nvk_sampler_for_id(9) == s1 && created == 3);
    /* A non-replay reader must never touch the replay-only front cache. */
    pthread_t t; hits = sampler_id_hits;
    assert(!pthread_create(&t, NULL, other_reader, NULL)); assert(!pthread_join(t, NULL));
    assert(sampler_id_hits == hits);
    assert(!pthread_create(&t, NULL, registrar, NULL));
    for (unsigned i = 0; i < 50000; ++i) { VkSampler s = nvk_sampler_for_id(7); assert(!s || s == s0 || s == s1); }
    assert(!pthread_join(t, NULL));
    mtl_reg_samp(7, &p0); assert(nvk_sampler_for_id(7) == s0);
    uint64_t before = nvk_now_ns();
    for (unsigned i = 0; i < 500000; ++i) assert(nvk_sampler_for_id(7) == s0);
    double ns = (nvk_now_ns() - before) / 500000.0;
    printf("sampler-id cache %s: pass (same id, replacement, unregister, collision, id zero, in-flight update, concurrent registry and non-replay readers); warm host lookup %.1f ns\n", on ? "on" : "off", ns);
    for (unsigned i = 0; i < 1024; ++i) while (samplers[i]) { SamplerEntry *e = samplers[i]; samplers[i] = e->next; free(e); }
    free(samp_slots); free(samp_used);
}
