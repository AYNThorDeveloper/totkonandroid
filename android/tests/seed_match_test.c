/* Real nvn_vk_shader.c: seed.idx matching. A seeded variant is taken when every slot it uses has the answer it was
 * translated for (unbound = the translator's default); otherwise only as the nearest stand-in. No Vulkan use. */
#include <pthread.h>
#include <sys/socket.h>
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0   /* macOS host test: the bridge socket is never opened */
#endif
#include "../nvn_vk_shader.c"
#include <assert.h>
VkDevice nvk_dev;
NvkState nvk_S;
static uint32_t info[64];
static p_set_rt targets; static int have_targets, dmw = 1;
uint32_t nvk_texture_info(int id) { return id > 0 && id < 64 ? info[id] : 0; }
const p_set_rt *nvk_render_targets(void) { return have_targets ? &targets : NULL; }
int totk_depth_near_minus_w(void) { return dmw; }
_Noreturn void nvk_die(const char *m) { fprintf(stderr, "%s\n", m); abort(); }

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    setenv("TOTK_SHADER_CACHE", argv[1], 1);
    char path[600]; snprintf(path, sizeof path, "%s/seed.idx", argv[1]);
    FILE *f = fopen(path, "w");
    const char *K = "0123456789abcdef0123456789abcdef";
    fprintf(f, "%s %s-aaaaaaaaaaaa 1 o0:1 t2:2 t9:34\n", K, K);     /* ps: 2d at 2, 2d depth at 9, float output 0 */
    fprintf(f, "%s %s-bbbbbbbbbbbb 1 o0:2 t2:2 t9:34\n", K, K);     /* the same with a uint output */
    fprintf(f, "%s %s-cccccccccccc 0 a0:1 a1:3 i4\n", K, K);        /* vs: float 0, sint 1, image 4 unbound */
    fprintf(f, "malformed line\n");
    fclose(f);
    info[5] = 0x80000000u | 0x25u << 8 | 1;   /* 2D RGBA8 */
    info[6] = 0x80000000u | 0x33u << 8 | 1;   /* 2D depth */
    info[7] = 0x80000000u | 0x25u << 8 | 4;   /* 2D array */
    tex_t rgba = {0}; rgba.d.format = 0x25; tex_t r32ui = {0}; r32ui.d.format = 0x0B;
    targets.n = 1; targets.color[0] = &rgba; have_targets = 1;
    pthread_mutex_lock(&lock);
    nvk_S.tex[1][2] = 5; nvk_S.tex[1][9] = 6;
    const SeedEntry *e = seed_find(K, 1, 0);
    assert(e && !strcmp(e->variant + 33, "aaaaaaaaaaaa") && seed_count == 3);
    nvk_S.tex[1][40] = 7;                      /* an unused slot does not matter */
    assert(seed_find(K, 1, 0) == e);
    nvk_S.tex[1][2] = 0;                       /* unbound reads as the translator's 2d default */
    assert(seed_find(K, 1, 0) == e);
    targets.color[0] = &r32ui;                 /* uint output: the other variant */
    e = seed_find(K, 1, 0); assert(e && !strcmp(e->variant + 33, "bbbbbbbbbbbb"));
    targets.color[0] = &rgba; nvk_S.tex[1][9] = 5;   /* not depth any more: no exact variant */
    assert(!seed_find(K, 1, 0));
    e = seed_find(K, 1, 1); assert(e && !strcmp(e->variant + 33, "aaaaaaaaaaaa"));   /* nearest: 2 of 3 agree */
    assert(!seed_find("fedcba9876543210fedcba9876543210", 1, 1));                    /* other shader: nothing */
    nvk_S.nattr = 2; nvk_S.attr[0].fmt = 0x2E; nvk_S.attr[1].fmt = 0x30;   /* float, sint */
    e = seed_find(K, 0, 0); assert(e && !strcmp(e->variant + 33, "cccccccccccc"));
    nvk_S.img[0][4] = 5;                       /* an image bound where the seed had none */
    assert(!seed_find(K, 0, 0));
    nvk_S.img[0][4] = 0; dmw = 0;              /* another depth mode: seeds do not apply */
    assert(!seed_find(K, 0, 1));
    pthread_mutex_unlock(&lock);
    puts("seed matching: pass (used slots decide, unbound = default, nearest only on request, depth mode guarded)");
    /* Dynamic uniform buffers: game uniform buffers only, lowest binding numbers first, half the device limit for a
       graphics stage (both sets share one pipeline layout), none when the limit is 0. */
    NvkBinding b[5] = {
        {.kind = 3, .dtype = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .binding = 0, .count = 1},
        {.kind = 1, .dtype = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .binding = 9, .count = 1},
        {.kind = 9, .dtype = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .binding = 1, .count = 1},   /* driver bank: stays */
        {.kind = 1, .dtype = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .binding = 4, .count = 1},
        {.kind = 1, .dtype = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .binding = 6, .count = 1}};
    NvkShader d = {.stage = 1, .b = b, .nb = 5};
    nvk_dyn_ubo_limit = 0; dynamic_ubos(&d); assert(!d.dyn && !d.ndyn);
    nvk_dyn_ubo_limit = 4; dynamic_ubos(&d);
    assert(d.ndyn == 2 && d.dyn_order[0] == 3 && d.dyn_order[1] == 4 && d.dyn == ((1u << 3) | (1u << 4)));
    d.stage = 5; dynamic_ubos(&d);
    assert(d.ndyn == 3 && d.dyn_order[0] == 3 && d.dyn_order[1] == 4 && d.dyn_order[2] == 1);
    nvk_dyn_ubo_limit = 0;
    puts("dynamic uniform buffers: pass (game uniform buffers only, binding order, half the limit per graphics stage, off at 0)");
}
