/* Descriptor set reuse (nvn_vk_draw.c set_for / bind_sets): identical layout and contents within one submission
 * share a set; any difference in buffer, offset, range, view, sampler, layout or binding, or a new submission, gets a
 * new one; dynamic offsets come out in binding order; a bind is skipped only when the same command buffer already
 * holds the same sets and offsets; TOTK_VK_DESC_CACHE=0 writes a set per call (second run of this binary). */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
static int test_setname(pthread_t t, const char *name) { (void)t; (void)name; return 0; }
#define pthread_setname_np test_setname
#include "../nvn_vk_draw.c"

VkDevice nvk_dev;
VkPhysicalDevice nvk_phys;
_Noreturn void nvk_die(const char *s) { fprintf(stderr, "%s\n", s); abort(); }
static uint64_t fence = 1, epoch = 1;
static unsigned allocated, updates, binds;
uint64_t nvk_pending_fence(void) { return fence; }
uint64_t nvk_cmd_epoch(void) { return epoch; }
VkDescriptorSet nvk_alloc_set(VkDescriptorSetLayout layout) { (void)layout; return (VkDescriptorSet)(uintptr_t)(0x1000 + ++allocated); }
static void update(VkDevice d, uint32_t n, const VkWriteDescriptorSet *w, uint32_t c, const VkCopyDescriptorSet *cp) {
    (void)d; (void)c; (void)cp;
    for (uint32_t i = 0; i < n; ++i) assert(w[i].dstSet);
    updates++;
}
static uint32_t last_dyn[8], last_ndyn;
static void bind(VkCommandBuffer cb, VkPipelineBindPoint p, VkPipelineLayout l, uint32_t first, uint32_t n, const VkDescriptorSet *s,
                 uint32_t nd, const uint32_t *d) {
    (void)cb; (void)p; (void)l; (void)first; (void)n; (void)s;
    last_ndyn = nd; memcpy(last_dyn, d, nd * sizeof *d); binds++;
}
PFN_vkUpdateDescriptorSets vkUpdateDescriptorSets = update;
/* Descriptor update templates (TOTK_VK_DESC_TEMPLATE): one per layout and write pattern; the packed block must hold
   exactly what the plain writes would have. */
static unsigned templates, template_writes; static VkDescriptorUpdateTemplateCreateInfo last_tci; static VkDescriptorUpdateTemplateEntry last_ent[8];
static Writes *expect;
static VkResult make_template(VkDevice d, const VkDescriptorUpdateTemplateCreateInfo *ci, const VkAllocationCallbacks *a, VkDescriptorUpdateTemplate *t) {
    (void)d; (void)a; last_tci = *ci; memcpy(last_ent, ci->pDescriptorUpdateEntries, ci->descriptorUpdateEntryCount * sizeof *last_ent);
    assert(ci->templateType == VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET && ci->descriptorSetLayout);
    *t = (VkDescriptorUpdateTemplate)(uintptr_t)(0x9000 + ++templates); return VK_SUCCESS;
}
static void update_template(VkDevice d, VkDescriptorSet set, VkDescriptorUpdateTemplate t, const void *data) {
    (void)d; (void)t; assert(set && expect);
    for (int i = 0; i < expect->n; ++i) {
        const uint8_t *slot = (const uint8_t *)data + last_ent[i].offset;
        if (expect->w[i].pBufferInfo) assert(!memcmp(slot, expect->w[i].pBufferInfo, sizeof(VkDescriptorBufferInfo)));
        else assert(!memcmp(slot, expect->w[i].pImageInfo, sizeof(VkDescriptorImageInfo)));
    }
    updates++; template_writes++;
}
PFN_vkCreateDescriptorUpdateTemplate vkCreateDescriptorUpdateTemplate = make_template;
PFN_vkUpdateDescriptorSetWithTemplate vkUpdateDescriptorSetWithTemplate = update_template;
PFN_vkCmdBindDescriptorSets vkCmdBindDescriptorSets = bind;

static Writes W;
static void writes(uint64_t buffer, uint64_t offset, uint64_t view) {   /* one uniform buffer + one texture */
    memset(&W, 0, sizeof W); expect = &W;
    W.n = 2;
    W.w[0] = (VkWriteDescriptorSet){.dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &W.bi[0]};
    W.bi[0] = (VkDescriptorBufferInfo){(VkBuffer)(uintptr_t)buffer, offset, 256};
    W.w[1] = (VkWriteDescriptorSet){.dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &W.ii[1]};
    W.ii[1] = (VkDescriptorImageInfo){(VkSampler)(uintptr_t)7, (VkImageView)(uintptr_t)view, VK_IMAGE_LAYOUT_GENERAL};
}

int main(void) {
    int on = dc_on();
    VkDescriptorSetLayout la = (VkDescriptorSetLayout)(uintptr_t)0x10, lb = (VkDescriptorSetLayout)(uintptr_t)0x20;
    writes(1, 0, 100); VkDescriptorSet a = set_for(&W, la);
    uint64_t hashed_before = dc_key_builds;
    writes(1, 0, 100); VkDescriptorSet b = set_for(&W, la);
    if (on && dc_recent_on()) assert(dc_key_builds == hashed_before && dc_recent_hits == 1);
    else if (on) assert(dc_key_builds == hashed_before + 1);
    assert(on ? a == b && allocated == 1 && updates == 1 : a != b && allocated == 2);
    if (on) {
        writes(1, 256, 100); assert(set_for(&W, la) != a);          /* other offset */
        writes(2, 0, 100);   assert(set_for(&W, la) != a);          /* other buffer */
        writes(1, 0, 101);   assert(set_for(&W, la) != a);          /* other view */
        writes(1, 0, 100);   assert(set_for(&W, lb) != a);          /* other layout */
        writes(1, 0, 100); W.ii[1].sampler = (VkSampler)(uintptr_t)8; assert(set_for(&W, la) != a);
        writes(1, 0, 100); W.w[1].dstBinding = 2; assert(set_for(&W, la) != a);
        writes(1, 0, 100); assert(set_for(&W, la) == a);            /* still there */
        fence++;
        writes(1, 0, 100); VkDescriptorSet c = set_for(&W, la); assert(c != a);   /* next submission: its own pools */
        writes(1, 0, 100); assert(set_for(&W, la) == c);
        W.n = 0; VkDescriptorSet e = set_for(&W, lb); assert(set_for(&W, lb) == e);   /* empty fragment set: one per submission */
    }
    /* Dynamic offsets in binding order, whatever the index order of the bindings. */
    NvkShader s = {.ndyn = 2, .dyn_order = {3, 1}};
    W.dynoff[1] = 512; W.dynoff[3] = 64;
    uint32_t dyn[4]; assert(dyn_offsets(&W, &s, dyn) == 2 && dyn[0] == 64 && dyn[1] == 512);
    /* Bind skipping: same sets and offsets in the same command buffer only. */
    VkDescriptorSetLayout layouts[2] = {la, lb};
    VkDescriptorSet sets[2] = {a, b};
    unsigned before = binds;
    bind_sets(NULL, VK_PIPELINE_BIND_POINT_GRAPHICS, VK_NULL_HANDLE, layouts, sets, 2, dyn, 2);
    bind_sets(NULL, VK_PIPELINE_BIND_POINT_GRAPHICS, VK_NULL_HANDLE, layouts, sets, 2, dyn, 2);
    assert(binds == before + (on ? 1 : 2) && last_ndyn == 2 && last_dyn[0] == 64);
    dyn[0] = 128; bind_sets(NULL, VK_PIPELINE_BIND_POINT_GRAPHICS, VK_NULL_HANDLE, layouts, sets, 2, dyn, 2);
    assert(binds == before + (on ? 2 : 3) && last_dyn[0] == 128);                     /* other offset */
    bind_sets(NULL, VK_PIPELINE_BIND_POINT_COMPUTE, VK_NULL_HANDLE, layouts, sets, 1, dyn, 1);
    bind_sets(NULL, VK_PIPELINE_BIND_POINT_GRAPHICS, VK_NULL_HANDLE, layouts, sets, 2, dyn, 2);
    assert(binds == before + (on ? 4 : 5));                                           /* compute in between: bound again */
    epoch++; bind_sets(NULL, VK_PIPELINE_BIND_POINT_GRAPHICS, VK_NULL_HANDLE, layouts, sets, 2, dyn, 2);
    assert(binds == before + (on ? 5 : 6));                                           /* new command buffer (segment) */
    if (dt_on()) assert(template_writes == updates && templates == (on ? 3u : 1u));   /* (la, A), (lb, A), (la, binding 2) */
    else assert(!template_writes && !templates);
    if (on) {
        /* Four genuinely different descriptor contents with the same hash bucket must coexist.
           A fifth may evict one; it must never return a set for different contents. */
        uint64_t values[5], rec[DC_WRITES][4]; unsigned found = 0, bucket = 0;
        for (uint64_t v = 1000; found < 5; ++v) {
            writes(v, 0, 100);
            unsigned b = (unsigned)dc_key(&W, la, rec) & (DC_N - 1);
            if (dc_ways() == 4) b &= ~3u;
            if (!found) bucket = b;
            if (b == bucket) values[found++] = v;
        }
        fence++;
        VkDescriptorSet saved[4];
        for (unsigned i = 0; i < 4; ++i) { writes(values[i], 0, 100); saved[i] = set_for(&W, la); }
        if (dc_ways() == 4) {
            unsigned count = allocated;
            for (unsigned i = 0; i < 4; ++i) { writes(values[i], 0, 100); assert(set_for(&W, la) == saved[i]); }
            assert(allocated == count);
        } else { writes(values[0], 0, 100); assert(set_for(&W, la) != saved[0]); }
        writes(values[4], 0, 100); VkDescriptorSet fifth = set_for(&W, la);
        for (unsigned i = 0; i < 4; ++i) assert(fifth != saved[i]);
        assert(set_for(&W, la) == fifth);
        fence++; assert(set_for(&W, la) != fifth);
        printf("descriptor bucket collisions: pass (%u way, exact contents, eviction and submission lifetime)\n", dc_ways());
    }
    if (on) {
        /* The shortcut must invalidate on fields other than the common buffer/view changes. */
        fence++;
        writes(1, 0, 100); VkDescriptorSet original = set_for(&W, la);
        W.bi[0].range = 512; assert(set_for(&W, la) != original);
        writes(1, 0, 100); W.ii[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        assert(set_for(&W, la) != original);
        writes(1, 0, 100); W.w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        assert(set_for(&W, la) != original);
        writes(1, 0, 100); W.n = 1; assert(set_for(&W, la) != original);
        /* Distinct layouts that share a recent slot cannot reuse one another's set. */
        VkDescriptorSetLayout collision = (VkDescriptorSetLayout)((uintptr_t)la + 0x400);
        assert(dc_recent_slot(collision) == dc_recent_slot(la));
        writes(1, 0, 100); VkDescriptorSet other = set_for(&W, collision);
        assert(other != original);
        VkDescriptorSet back = set_for(&W, la); assert(back != other);
        uint64_t keys = dc_key_builds;
        assert(set_for(&W, la) == back);
        if (dc_recent_on()) assert(dc_key_builds == keys);
        fence++; assert(set_for(&W, la) != back);
        printf("descriptor recent shortcut: pass (full fields, layout collision, count, lifetime; %llu key builds bypassed)\n",
            (unsigned long long)dc_recent_hits);
    }
    printf("descriptor update templates: %s\n", dt_on() ? "pass (one per layout and write pattern, packed block equals the plain writes)" : "off (TOTK_VK_DESC_TEMPLATE=0)");
    printf("descriptor set reuse%s: pass (same contents shared, every field compared, per submission, dynamic offsets in binding order, binds skipped only within one command buffer)\n",
        on ? "" : " (TOTK_VK_DESC_CACHE=0)");
    return 0;
}
