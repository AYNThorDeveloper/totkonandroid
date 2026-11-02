/* Real graphics pipeline lookup; fake Vulkan creation returns unique handles.
 * Changes in every static key field must keep selecting distinct pipelines. */
#include <assert.h>
#include <pthread.h>
static int test_setname(pthread_t t, const char *s) { (void)t; (void)s; return 0; }
#define pthread_setname_np test_setname
#include "../nvn_vk_draw.c"
VkDevice nvk_dev; VkPhysicalDevice nvk_phys; int android_vk_local_read;
_Noreturn void nvk_die(const char *s) { fprintf(stderr, "%s\n", s); abort(); }
VkPipelineCache nvk_pipeline_cache_begin(void) { return VK_NULL_HANDLE; }
void nvk_pipeline_cache_end(void) {}
NvkShader *nvk_shader_find_variant(const char *s) { (void)s; return NULL; }
int nvk_shader_preload(const char *s, int st) { (void)s; (void)st; return 0; }
void nvk_shader_seed_load(void) {}
static unsigned creates;
static VkResult fake_layout(VkDevice d, const VkPipelineLayoutCreateInfo *i, const VkAllocationCallbacks *a, VkPipelineLayout *out) {
    (void)d; (void)i; (void)a; *out = (VkPipelineLayout)(uintptr_t)1; return VK_SUCCESS;
}
static VkResult make_graphics(VkDevice d, VkPipelineCache pc, uint32_t n,
        const VkGraphicsPipelineCreateInfo *i, const VkAllocationCallbacks *a, VkPipeline *out) {
    (void)d; (void)pc; (void)a; assert(n == 1 && i->stageCount == 2);
    *out = (VkPipeline)(uintptr_t)++creates; return VK_SUCCESS;
}
PFN_vkCreatePipelineLayout vkCreatePipelineLayout = fake_layout;
PFN_vkCreateGraphicsPipelines vkCreateGraphicsPipelines = make_graphics;
PFN_vkCreateComputePipelines vkCreateComputePipelines;
PFN_vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout;
PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties;
static NvkShader vs = {.stage = 0, .layout = (VkDescriptorSetLayout)(uintptr_t)1},
    ps = {.stage = 1, .layout = (VkDescriptorSetLayout)(uintptr_t)2},
    ps_depth = {.stage = 1, .layout = (VkDescriptorSetLayout)(uintptr_t)3};
int main(void) {
    setenv("TOTK_VK_PREWARM", "0", 1); setenv("TOTK_VK_ASYNC_PIPELINES", "0", 1);
    PipeKey base = {.vs = &vs, .ps = &ps, .ncolor = 1, .topo_class = 2, .chmask = 15};
    base.color[0] = VK_FORMAT_R8G8B8A8_UNORM;
    Pipe *first = pipeline_for_draw(&base); assert(first && creates == 1);
    uint64_t hits = pipe_memo_hits;
    assert(pipeline_for_draw(&base) == first && creates == 1);
    assert(pipe_memo_hits == hits + (pipe_memo_on() ? 1 : 0));
    PipeKey changed[12]; for (unsigned i = 0; i < 12; ++i) changed[i] = base;
    changed[0].ps = &ps_depth;                  /* derived depth shader */
    changed[1].color[0] = VK_FORMAT_R16_SFLOAT;
    changed[2].depth = VK_FORMAT_D16_UNORM;
    changed[3].stencil = VK_FORMAT_S8_UINT;
    changed[4].chmask = 7;
    changed[5].polygon = NVN_POLYGON_MODE_LINE;
    changed[6].depth_clamp = 1;
    changed[7].a2c = 1;
    changed[8].topo_class = 1;
    changed[9].logic = 1; changed[9].logic_op = VK_LOGIC_OP_XOR;
    changed[10].blend_en = 1;
    changed[11].blend_en = 1; changed[11].blend[0].src_rgb = NVN_BLEND_FUNC_ONE;
    Pipe *seen[12];
    for (unsigned i = 0; i < 12; ++i) {
        seen[i] = pipeline_for_draw(&changed[i]); assert(seen[i] && seen[i] != first);
        for (unsigned j = 0; j < i; ++j) assert(seen[i] != seen[j]);
        assert(pipeline_for_draw(&changed[i]) == seen[i]);
        assert(pipeline_for_draw(&base) == first); /* same memo bucket: old key must never alias */
    }
    assert(creates == 13);
    for (unsigned i = 0; i < 10000; ++i) assert(pipeline_for_draw(&base) == first);
    assert(creates == 13);
    hits = pipe_memo_hits;
    setenv("TOTK_VK_PIPE_MEMO", "0", 1); nvk_live_gen++;
    assert(pipeline_for_draw(&base) == first && pipe_memo_hits == hits);
    setenv("TOTK_VK_PIPE_MEMO", "1", 1); nvk_live_gen++;
    assert(pipeline_for_draw(&base) == first);
    assert(pipeline_for_draw(&base) == first && pipe_memo_hits > hits);
    puts("graphics pipeline memo: pass (full static key, derived shader, bucket collisions, ready-only results, reuse without rebuilding; on/off supported)");
}
