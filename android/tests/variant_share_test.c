/* Real shader-variant code: answers per used slot decide whether a variant
 * translated for other bound state can be reused. No Vulkan or bridge use. */
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
static p_set_rt targets; static int have_targets;
uint32_t nvk_texture_info(int id) { return id > 0 && id < 64 ? info[id] : 0; }
const p_set_rt *nvk_render_targets(void) { return have_targets ? &targets : NULL; }
int totk_depth_near_minus_w(void) { return 0; }
_Noreturn void nvk_die(const char *m) { fprintf(stderr, "%s\n", m); abort(); }

int main(void) {
    /* Fragment shader sampling texture slot 2 and writing colour output 0. */
    NvkBinding bind = {.kind = 3, .reg = 2};
    NvkShader ps = {.stage = 1, .b = &bind, .nb = 1, .out_mask = 1};
    info[5] = 0x80000000u | 0x25u << 8 | 1;   /* 2D RGBA8 */
    info[6] = 0x80000000u | 0x25u << 8 | 4;   /* 2D array */
    info[7] = 0x80000000u | 0x33u << 8 | 1;   /* 2D depth */
    tex_t rgba = {0}; rgba.d.format = 0x25; tex_t r32ui = {0}; r32ui.d.format = 0x0B;
    targets.n = 1; targets.color[0] = &rgba; have_targets = 1;
    nvk_S.tex[1][2] = 5; nvk_S.tex[1][9] = 6;
    Answers was; answers_now(&was, 1);
    pthread_mutex_lock(&lock);
    shared_add("0123456789abcdef0123456789abcdef", &ps, &was);
    /* Another texture in an unused slot: same translation. */
    nvk_S.tex[1][9] = 7; nvk_S.tex[1][40] = 5;
    Answers now; answers_now(&now, 1);
    assert(shared_find("0123456789abcdef0123456789abcdef", &now) == &ps);
    assert(!shared_find("fedcba9876543210fedcba9876543210", &now));   /* other shader */
    /* The used slot changes target or depth flag: new translation. */
    nvk_S.tex[1][2] = 6; answers_now(&now, 1); assert(!shared_find("0123456789abcdef0123456789abcdef", &now));
    nvk_S.tex[1][2] = 7; answers_now(&now, 1); assert(!shared_find("0123456789abcdef0123456789abcdef", &now));
    nvk_S.tex[1][2] = 0; answers_now(&now, 1); assert(!shared_find("0123456789abcdef0123456789abcdef", &now));
    /* The written output's class changes (float -> uint): new translation. */
    nvk_S.tex[1][2] = 5; targets.color[0] = &r32ui; answers_now(&now, 1);
    assert(!shared_find("0123456789abcdef0123456789abcdef", &now));
    /* An unwritten output target changes: same translation. */
    targets.color[0] = &rgba; targets.n = 2; targets.color[1] = &r32ui; answers_now(&now, 1);
    assert(shared_find("0123456789abcdef0123456789abcdef", &now) == &ps);
    /* Vertex shader: only the attributes it reads count. */
    NvkShader vs = {.stage = 0, .nin = 1}; vs.in[0].loc = 1;
    nvk_S.nattr = 3; nvk_S.attr[0].fmt = 0x2E; nvk_S.attr[1].fmt = 0x2E; nvk_S.attr[2].fmt = 0x2E;
    answers_now(&was, 0); shared_add("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", &vs, &was);
    nvk_S.attr[2].fmt = 0x2F; answers_now(&now, 0);
    assert(shared_find("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", &now) == &vs);
    nvk_S.attr[1].fmt = 0x2F; answers_now(&now, 0);
    assert(!shared_find("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", &now));
    pthread_mutex_unlock(&lock);
    puts("variant sharing: pass (unused slots/outputs/attributes ignored; used target, depth, class changes translate)");
}
