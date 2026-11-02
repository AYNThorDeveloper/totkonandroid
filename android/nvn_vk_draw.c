/* NVN draws and dispatches on Vulkan.
 *
 * Follows the Metal replay (nvn_mtl.m draw_inner / encode_args): the bound
 * program's stages are translated per bound state (nvn_vk_shader.c), their
 * resources are bound by NVN slot from the replay state, and the draw uses
 * one pipeline per (shaders, attachment formats, blend/write masks, polygon
 * mode, depth clamp, topology class). Everything the Vulkan 1.3 core and
 * VK_EXT_vertex_input_dynamic_state allow is dynamic state.
 *
 * Window origin is upper-left (the game selects it): the viewport is flipped
 * (negative height) so clip-space y points up as on NVN, D3D and Metal. */
#define _GNU_SOURCE
#include <pthread.h>
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "nvn_vk_internal.h"
#include "nvn_vk_format.h"
#include "nvn_vk_driver_bank.h"

__attribute__((weak)) void nvk_segment_checkpoint(void) {}   /* backend absent in isolated host tests */

NvkState nvk_S;
static int state_ready;
static uint32_t dispatch_grid[3];
/* Per stage: bumped whenever its uniform, storage, texture, sampler or image bindings change (nvk_state_op, the
   only writer of those arrays besides state_reset). Lets per-draw work that depends only on them be reused. */
uint64_t nvk_bind_epoch[6], nvk_attr_epoch;
/* What a shader variant signature of stage st depends on besides constants (nvn_vk_shader.c nvk_shader_for): the
   stage's bindings, the vertex attribute formats (vertex), the render targets (fragment) and texture registrations.
   Equal epochs mean an equal signature. TOTK_VK_SIG_CACHE=0: recomputed every draw. */
uint64_t nvk_rt_epoch(void); uint64_t nvk_tex_info_gen(void);
__attribute__((weak)) void nvk_sig_stats(uint64_t *hits, uint64_t *misses) { *hits = *misses = 0; }   /* nvn_vk_shader.c */
int nvk_sig_epochs(int st, uint64_t e[4]) {
    static int on = -1;
    if (on < 0) { const char *v = getenv("TOTK_VK_SIG_CACHE"); on = !(v && !strcmp(v, "0")); }
    if (!on || st < 0 || st > 5) return 0;
    e[3] = nvk_tex_info_gen();   /* read first: a registration while the signature is computed only causes a recompute */
    e[0] = nvk_bind_epoch[st]; e[1] = st == 0 ? nvk_attr_epoch : 0; e[2] = st == 1 ? nvk_rt_epoch() : 0;
    return 1;
}

static void state_reset(void) {
    memset(&nvk_S, 0, sizeof nvk_S);
    for (int i = 0; i < 6; ++i) nvk_bind_epoch[i]++;
    nvk_attr_epoch++;
    for (int i = 0; i < 8; i++) nvk_S.blend[i] = (blend_st){(uint64_t)i, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD};
    nvk_S.color = (color_st){0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS}; nvk_S.chmask.mask = 0xffffffffu;
    nvk_S.poly = (poly_st){NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0};
    nvk_S.ds = (ds_st){0, 1, NVN_DEPTH_FUNC_LESS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS, 1, 1, 1, 1, 1, 1};
    nvk_S.st_vmask[0] = nvk_S.st_vmask[1] = nvk_S.st_wmask[0] = nvk_S.st_wmask[1] = 0xff; nvk_S.dr[1] = 1;
    state_ready = 1;
}

/* SetRenderEnableConditional: mode 0 renders when the two counter values match.
 * A report still on the GPU supplies the value it will write; only an unknown
 * one (samples passed) renders, as cond_eval in nvn_mtl.m. TOTK_VK_COND_LOG=1
 * prints the first 64 evaluations. */
static int cond_eval(const p_render_cond *c) {
    uint64_t a = 0, b = 0;
    int ka = nvk_counter_lookup(c->addr, &a), kb = nvk_counter_lookup(c->addr + 16, &b), enabled = 1;
    if (ka && kb) {
        if (ka < 0) memcpy(&a, (const void *)(uintptr_t)c->addr, 8);
        if (kb < 0) memcpy(&b, (const void *)(uintptr_t)(c->addr + 16), 8);
        enabled = c->mode == 0 ? a == b : a != b;
    }
    static int log = -1; static unsigned logged;
    if (log < 0) { const char *e = getenv("TOTK_VK_COND_LOG"); log = e && !strcmp(e, "1"); }
    /* After the first 64 evaluations only a change of outcome per address is logged (a menu opening flips one). */
    static struct { uint64_t addr; int state; } last[32]; static unsigned nlast, changed;
    int state = (enabled ? 1 : 0) | (ka > 0 ? 2 : 0) | (kb > 0 ? 4 : 0), li = 0;
    while (li < (int)nlast && last[li].addr != c->addr) ++li;
    if (li == (int)nlast && nlast < 32) { last[nlast].addr = c->addr; last[nlast++].state = -1; }
    int flip = li < 32 && last[li].state != state;
    if (li < 32) last[li].state = state;
    if (log && ((logged < 64 && ++logged) || (flip && changed++ < 400 && (logged = 64))))
        VKLOG("conditional render %#llx mode %d%s: %llu (%s) vs %llu (%s) -> %s", (unsigned long long)c->addr, c->mode, logged > 64 || changed ? " [changed]" : "",
            (unsigned long long)a, ka > 0 ? "pending" : ka ? "memory" : "unknown", (unsigned long long)b, kb > 0 ? "pending" : kb ? "memory" : "unknown",
            enabled ? "enabled" : "disabled");
    return enabled;
}

void nvk_state_op(unsigned op, const void *a) {
    if (!state_ready) state_reset();
    NvkState *S = &nvk_S;
    switch (op) {
    case OP_BIND_PROGRAM: S->prog = ((const p_bind_program *)a)->prog; break;
    case OP_BIND_STATE: {
        const p_bind_state *b = a;
        blend_st bs;
        switch (b->kind) {
        case 0: memcpy(&bs, b->bytes, sizeof bs); if (bs.target < 8) S->blend[bs.target] = bs; break;
        case 1: memcpy(&S->color, b->bytes, sizeof S->color); break;
        case 2: memcpy(&S->chmask, b->bytes, sizeof S->chmask); break;
        case 3: memcpy(&S->ms, b->bytes, sizeof S->ms); break;
        case 4: memcpy(&S->poly, b->bytes, sizeof S->poly); break;
        case 5: memcpy(&S->ds, b->bytes, sizeof S->ds); break;
        }
        break;
    }
    case OP_BIND_VATTR: { const p_bind_vattr *b = a; memcpy(S->attr, b->st, 4 * (size_t)b->n); S->nattr = b->n; nvk_attr_epoch++; break; }
    case OP_BIND_VSTREAM: { const p_bind_vstream *b = a; memcpy(S->stream, b->st, 8 * (size_t)b->n); S->nstream = b->n; break; }
    case OP_BIND_VBUF: { const p_bind_vbuf *b = a; if (b->first >= 0 && b->first + b->n <= 16) memcpy(&S->vbuf[b->first], b->r, sizeof *b->r * (size_t)b->n); break; }
    case OP_BIND_UBO: { const p_bind_buf *b = a; if (b->stage < 6 && b->index < 16) { S->ubo[b->stage][b->index] = *b; nvk_bind_epoch[b->stage]++; } break; }
    case OP_BIND_SSBO: { const p_bind_buf *b = a; if (b->stage < 6 && b->index < 16) { S->ssbo[b->stage][b->index] = *b; nvk_bind_epoch[b->stage]++; } break; }
    case OP_BIND_TEX: { const p_bind_handles *b = a; if (b->stage < 6 && b->first >= 0 && b->first + b->n <= 128) { memcpy(&S->tex[b->stage][b->first], b->h, 8 * (size_t)b->n); nvk_bind_epoch[b->stage]++; } break; }
    case OP_BIND_SAMP: { const p_bind_handles *b = a; if (b->stage < 6 && b->first >= 0 && b->first + b->n <= 128) { memcpy(&S->samp[b->stage][b->first], b->h, 8 * (size_t)b->n); nvk_bind_epoch[b->stage]++; } break; }
    case OP_BIND_IMG: { const p_bind_handles *b = a; if (b->stage < 6 && b->first >= 0 && b->first + b->n <= 32) { memcpy(&S->img[b->stage][b->first], b->h, 8 * (size_t)b->n); nvk_bind_epoch[b->stage]++; } break; }
    case OP_STENCIL: {
        const p_stencil *b = a;
        for (int f = 0; f < 2; f++) if (b->face & (1 << f)) {
            if (b->which == 0) S->st_vmask[f] = (uint32_t)b->value_mask; else if (b->which == 1) S->st_wmask[f] = (uint32_t)b->mask; else S->st_ref[f] = (uint32_t)b->ref;
        }
        break;
    }
    case OP_POLY_OFFSET: S->po = *(const p_poly_offset *)a; break;
    case OP_VIEWPORTS: { const p_viewports *b = a; for (int i = 0; i < b->n && b->first + i < 16; i++) memcpy(S->vp[b->first + i], b->v[i], 16); S->has_vp = 1; break; }
    case OP_DEPTH_RANGE: { const p_depth_range *b = a; S->dr[0] = b->n; S->dr[1] = b->f; break; }
    case OP_BLEND_COLOR: memcpy(S->blend_color, ((const p_blend_color *)a)->rgba, 16); break;
    case OP_DEPTH_CLAMP: S->depth_clamp = ((const p_flag *)a)->value; break;
    case OP_RENDER_ENABLE: S->render_disabled = !((const p_flag *)a)->value; break;
    case OP_RENDER_COND: S->render_disabled = !cond_eval(a); break;
    case OP_RASTER_DISCARD: S->raster_discard = ((const p_flag *)a)->value; break;
    }
}

/* ---------- NVN to Vulkan enums ---------- */
static VkBlendFactor blend_factor(unsigned f) {
    switch (f) {
    case 1: return VK_BLEND_FACTOR_ZERO; case 2: return VK_BLEND_FACTOR_ONE; case 3: return VK_BLEND_FACTOR_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR; case 5: return VK_BLEND_FACTOR_SRC_ALPHA; case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_DST_ALPHA; case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA; case 9: return VK_BLEND_FACTOR_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR; case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x10: return VK_BLEND_FACTOR_SRC1_COLOR; case 0x11: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case 0x12: return VK_BLEND_FACTOR_SRC1_ALPHA; case 0x13: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR; case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA; case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
    }
}
static VkBlendOp blend_op(unsigned e) {
    switch (e) { case 2: return VK_BLEND_OP_SUBTRACT; case 3: return VK_BLEND_OP_REVERSE_SUBTRACT; case 4: return VK_BLEND_OP_MIN; case 5: return VK_BLEND_OP_MAX; default: return VK_BLEND_OP_ADD; }
}
static VkCompareOp compare_op(unsigned f) { return f >= 1 && f <= 8 ? (VkCompareOp)(f - 1) : VK_COMPARE_OP_ALWAYS; }   /* GL order */
static VkStencilOp stencil_op(unsigned o) {
    switch (o) {
    case 2: return VK_STENCIL_OP_ZERO; case 3: return VK_STENCIL_OP_REPLACE; case 4: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 5: return VK_STENCIL_OP_DECREMENT_AND_CLAMP; case 6: return VK_STENCIL_OP_INVERT; case 7: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 8: return VK_STENCIL_OP_DECREMENT_AND_WRAP; default: return VK_STENCIL_OP_KEEP;
    }
}
static VkFormat vertex_format(int f) {
    switch (f) {
    case 0x01: return VK_FORMAT_R8_UNORM; case 0x02: return VK_FORMAT_R8_SNORM; case 0x03: return VK_FORMAT_R8_UINT; case 0x04: return VK_FORMAT_R8_SINT;
    case 0x05: return VK_FORMAT_R16_SFLOAT; case 0x06: return VK_FORMAT_R16_UNORM; case 0x07: return VK_FORMAT_R16_SNORM; case 0x08: return VK_FORMAT_R16_UINT;
    case 0x09: return VK_FORMAT_R16_SINT; case 0x0A: return VK_FORMAT_R32_SFLOAT; case 0x0B: return VK_FORMAT_R32_UINT; case 0x0C: return VK_FORMAT_R32_SINT;
    case 0x0D: return VK_FORMAT_R8G8_UNORM; case 0x0E: return VK_FORMAT_R8G8_SNORM; case 0x0F: return VK_FORMAT_R8G8_UINT; case 0x10: return VK_FORMAT_R8G8_SINT;
    case 0x11: return VK_FORMAT_R16G16_SFLOAT; case 0x12: return VK_FORMAT_R16G16_UNORM; case 0x13: return VK_FORMAT_R16G16_SNORM; case 0x14: return VK_FORMAT_R16G16_UINT;
    case 0x15: return VK_FORMAT_R16G16_SINT; case 0x16: return VK_FORMAT_R32G32_SFLOAT; case 0x17: return VK_FORMAT_R32G32_UINT; case 0x18: return VK_FORMAT_R32G32_SINT;
    case 0x19: return VK_FORMAT_R8G8B8_UNORM; case 0x1A: return VK_FORMAT_R8G8B8_SNORM; case 0x1B: return VK_FORMAT_R8G8B8_UINT; case 0x1C: return VK_FORMAT_R8G8B8_SINT;
    case 0x1D: return VK_FORMAT_R16G16B16_SFLOAT; case 0x1E: return VK_FORMAT_R16G16B16_UNORM; case 0x1F: return VK_FORMAT_R16G16B16_SNORM; case 0x20: return VK_FORMAT_R16G16B16_UINT;
    case 0x21: return VK_FORMAT_R16G16B16_SINT; case 0x22: return VK_FORMAT_R32G32B32_SFLOAT; case 0x23: return VK_FORMAT_R32G32B32_UINT; case 0x24: return VK_FORMAT_R32G32B32_SINT;
    case 0x25: return VK_FORMAT_R8G8B8A8_UNORM; case 0x26: return VK_FORMAT_R8G8B8A8_SNORM; case 0x27: return VK_FORMAT_R8G8B8A8_UINT; case 0x28: return VK_FORMAT_R8G8B8A8_SINT;
    case 0x29: return VK_FORMAT_R16G16B16A16_SFLOAT; case 0x2A: return VK_FORMAT_R16G16B16A16_UNORM; case 0x2B: return VK_FORMAT_R16G16B16A16_SNORM; case 0x2C: return VK_FORMAT_R16G16B16A16_UINT;
    case 0x2D: return VK_FORMAT_R16G16B16A16_SINT; case 0x2E: return VK_FORMAT_R32G32B32A32_SFLOAT; case 0x2F: return VK_FORMAT_R32G32B32A32_UINT; case 0x30: return VK_FORMAT_R32G32B32A32_SINT;
    case 0x3D: return VK_FORMAT_A2B10G10R10_UNORM_PACK32; case 0x69: return VK_FORMAT_A2B10G10R10_SNORM_PACK32; case 0x3F: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case 0x9A: return VK_FORMAT_B8G8R8A8_UNORM;
    /* *_UI2F / *_I2F: integers converted to float */
    case 0x51: return VK_FORMAT_R8_USCALED; case 0x52: return VK_FORMAT_R8_SSCALED; case 0x53: return VK_FORMAT_R16_USCALED; case 0x54: return VK_FORMAT_R16_SSCALED;
    case 0x57: return VK_FORMAT_R8G8_USCALED; case 0x58: return VK_FORMAT_R8G8_SSCALED; case 0x59: return VK_FORMAT_R16G16_USCALED; case 0x5A: return VK_FORMAT_R16G16_SSCALED;
    case 0x5D: return VK_FORMAT_R8G8B8_USCALED; case 0x5E: return VK_FORMAT_R8G8B8_SSCALED; case 0x5F: return VK_FORMAT_R16G16B16_USCALED; case 0x60: return VK_FORMAT_R16G16B16_SSCALED;
    case 0x63: return VK_FORMAT_R8G8B8A8_USCALED; case 0x64: return VK_FORMAT_R8G8B8A8_SSCALED; case 0x65: return VK_FORMAT_R16G16B16A16_USCALED; case 0x66: return VK_FORMAT_R16G16B16A16_SSCALED;
    default: return VK_FORMAT_UNDEFINED;
    }
}

/* ---------- Pipelines ---------- */
typedef struct {
    const NvkShader *vs, *ps, *cs;
    VkFormat color[8], depth, stencil;
    uint8_t ncolor, blend_en, polygon, depth_clamp, a2c, topo_class, logic, logic_op;
    blend_st blend[8];
    uint32_t chmask;
} PipeKey;
typedef struct Pipe {
    PipeKey k; VkPipeline pipe; VkPipelineLayout layout;
    pthread_cond_t ready_cv; int ready;
    struct Pipe *next, *next_job;
} Pipe;
static Pipe *pipes[4096];
static pthread_mutex_t pipe_lock = PTHREAD_MUTEX_INITIALIZER;
static VkDescriptorSetLayout empty_layout;
static pthread_mutex_t empty_layout_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t pipelines_made;
static _Atomic uint64_t warm_built, warm_waits;
_Atomic uint64_t nvk_pipeline_count, nvk_pipeline_ns;

static unsigned key_hash(const PipeKey *k) {
    unsigned h = 2166136261u;
    for (size_t i = 0; i < sizeof *k; ++i) h = (h ^ ((const uint8_t *)k)[i]) * 16777619u;
    return h % 4096;
}

static VkDescriptorSetLayout empty_set_layout(void) {
    pthread_mutex_lock(&empty_layout_lock);
    if (!empty_layout) {
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        VKCHECK(vkCreateDescriptorSetLayout(nvk_dev, &li, NULL, &empty_layout), "create empty set layout");
    }
    VkDescriptorSetLayout result = empty_layout;
    pthread_mutex_unlock(&empty_layout_lock);
    return result;
}

static VkPipelineLayout make_layout(const PipeKey *k) {
    VkDescriptorSetLayout sets[2] = {k->cs ? k->cs->layout : k->vs->layout, k->ps ? k->ps->layout : empty_set_layout()};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = k->cs ? 1 : 2, .pSetLayouts = sets};
    VkPipelineLayout layout;
    VKCHECK(vkCreatePipelineLayout(nvk_dev, &li, NULL, &layout), "create pipeline layout");
    return layout;
}

static void warm_record(const PipeKey *k);
static void warm_initialize(void);
enum { BUILD_DRAW, BUILD_WARM, BUILD_ASYNC };
static void pipeline_build(Pipe *p, int mode);
/* Caller holds pipe_lock. A new, not yet ready entry for k in the table. */
static Pipe *pipe_insert(const PipeKey *k, unsigned h) {
    Pipe *p = calloc(1, sizeof *p);
    if (!p) nvk_die("pipeline allocation");
    p->k = *k;
    pthread_cond_init(&p->ready_cv, NULL);
    p->next = pipes[h]; pipes[h] = p;
    return p;
}
static Pipe *pipeline_make(const PipeKey *k, int warming) {
    warm_initialize();
    unsigned h = key_hash(k);
    pthread_mutex_lock(&pipe_lock);
    Pipe *p = pipes[h];
    while (p && memcmp(&p->k, k, sizeof *k)) p = p->next;
    if (p) {
        if (!p->ready && !warming) atomic_fetch_add(&warm_waits, 1);
        while (!p->ready && !warming) pthread_cond_wait(&p->ready_cv, &pipe_lock);
        /* A prewarm request already in flight must not queue duplicate work. */
        Pipe *result = p->ready > 0 ? p : NULL;
        pthread_mutex_unlock(&pipe_lock);
        return result;
    }
    p = pipe_insert(k, h);
    pthread_mutex_unlock(&pipe_lock);
    pipeline_build(p, warming ? BUILD_WARM : BUILD_DRAW);
    return p;
}

/* ---------- Pipelines compiled in the background ----------
 * A graphics pipeline the draw needs and nobody has built yet used to be compiled on the replay thread, which is
 * the thread the frame waits on: 20-150 ms per new pipeline, the main cause of the dips in the open world (4-9 s of
 * them in 4 minutes on the Thor, 22 Oct). Now TOTK_VK_PIPELINE_THREADS workers (default 2; TOTK_VK_ASYNC_PIPELINES=0
 * = compile on the replay thread as before) build it, and the draws that need it are left out until it is ready,
 * usually a few frames: an object that appears a moment late instead of a frozen picture. Compute pipelines stay
 * synchronous (a skipped dispatch would leave game data unwritten). */
static Pipe *async_head, *async_tail;   /* queue, linked through Pipe.next_job; under pipe_lock */
static pthread_cond_t async_cv = PTHREAD_COND_INITIALIZER;
static _Atomic uint64_t async_built, async_skipped, async_ns;
static int async_threads = -1;
static void *async_worker(void *unused) {
    (void)unused;
    pthread_setname_np(pthread_self(), "totk-vk-pipe");
    nice(5);
    for (;;) {
        pthread_mutex_lock(&pipe_lock);
        while (!async_head) pthread_cond_wait(&async_cv, &pipe_lock);
        Pipe *p = async_head; async_head = p->next_job; if (!async_head) async_tail = NULL;
        pthread_mutex_unlock(&pipe_lock);
        pipeline_build(p, BUILD_ASYNC);
    }
    return NULL;
}
static void async_start(void) {
    const char *on = getenv("TOTK_VK_ASYNC_PIPELINES"), *n = getenv("TOTK_VK_PIPELINE_THREADS");
    int want = on && !strcmp(on, "0") ? 0 : n && *n ? atoi(n) : 2;
    if (want > 4) want = 4;
    int made = 0;
    for (; made < want; ++made) {
        pthread_t t;
        if (pthread_create(&t, NULL, async_worker, NULL)) break;
        pthread_detach(t);
    }
    async_threads = made;
    VKLOG("background pipeline compilation: %d threads (TOTK_VK_ASYNC_PIPELINES, TOTK_VK_PIPELINE_THREADS)", made);
}
static pthread_once_t async_once = PTHREAD_ONCE_INIT;
/* Only in gameplay. Menus and loading screens draw a few hundred times per frame and some of their pieces only once,
 * into an image that is reused afterwards: left out, such a piece stayed black for the whole loading screen. A frame
 * after one with fewer than TOTK_VK_ASYNC_MIN_DRAWS draws (default 1000; the open world draws 2000-5000) waits for
 * its pipelines as before. */
static _Atomic uint64_t frame_draws_prev = 0, frame_draws_mark;
void nvk_draw_frame_end(void) {
    uint64_t now = nvk_draws_issued();
    atomic_store(&frame_draws_prev, now - atomic_exchange(&frame_draws_mark, now));
}
static int async_now(void) {
    static long min = -1;
    if (min < 0) { const char *e = getenv("TOTK_VK_ASYNC_MIN_DRAWS"); min = e && *e ? atol(e) : 1000; }
    return atomic_load(&frame_draws_prev) >= (uint64_t)min;
}
/* The pipeline for a draw, or NULL while it is being built in the background (the draw is then left out). */
static Pipe *pipeline_for_draw_uncached(const PipeKey *k) {
    pthread_once(&async_once, async_start);
    if (!async_threads || !async_now()) return pipeline_make(k, 0);
    warm_initialize();
    unsigned h = key_hash(k);
    pthread_mutex_lock(&pipe_lock);
    Pipe *p = pipes[h];
    while (p && memcmp(&p->k, k, sizeof *k)) p = p->next;
    if (!p) {
        p = pipe_insert(k, h);
        if (async_tail) async_tail->next_job = p; else async_head = p;
        async_tail = p;
        pthread_cond_signal(&async_cv);
    }
    Pipe *result = p->ready > 0 ? p : NULL;
    pthread_mutex_unlock(&pipe_lock);
    if (!result) atomic_fetch_add(&async_skipped, 1);
    return result;
}

/* Serial replay thread only. Pipes and their keys live for the device lifetime.
 * Cache only ready results obtained through pipe_lock (or built by this thread),
 * so no worker-owned partially initialized pipeline can escape via the memo.
 * Exact key equality includes shader variants and every static pipeline field;
 * dynamic state, resources and barriers still run on every draw as before.
 * TOTK_VK_PIPE_MEMO=0 uses the full hash/table lookup on every draw. */
#define PIPE_MEMO 256u
static Pipe *pipe_memo[PIPE_MEMO];
static uint64_t pipe_memo_hits, pipe_memo_misses;
static int pipe_memo_on(void) {
    static int on = -1; static unsigned generation;
    if (on < 0 || generation != nvk_live_gen) {
        generation = nvk_live_gen;
        const char *e = nvk_live_getenv("TOTK_VK_PIPE_MEMO"); on = !(e && !strcmp(e, "0"));
    }
    return on;
}
static Pipe *pipeline_for_draw(const PipeKey *k) {
    unsigned slot = (unsigned)(((uintptr_t)k->vs >> 4) ^ ((uintptr_t)k->ps >> 4)) & (PIPE_MEMO - 1);
    int memo = pipe_memo_on();
    if (memo && pipe_memo[slot] && !memcmp(&pipe_memo[slot]->k, k, sizeof *k)) {
        pipe_memo_hits++; return pipe_memo[slot];
    }
    pipe_memo_misses++;
    Pipe *p = pipeline_for_draw_uncached(k);
    if (memo && p) pipe_memo[slot] = p;
    return p;
}

static void pipeline_build(Pipe *p, int mode) {
    const PipeKey *k = &p->k;
    int warming = mode == BUILD_WARM;
    uint64_t started = nvk_now_ns();
    p->layout = make_layout(k);
    if (k->cs) {
        VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = k->cs->module, .pName = "main"},
            .layout = p->layout};
        VkPipelineCache pcache = nvk_pipeline_cache_begin();
        VkResult r = vkCreateComputePipelines(nvk_dev, pcache, 1, &ci, NULL, &p->pipe);
        nvk_pipeline_cache_end();
        if (r != VK_SUCCESS) { VKLOG("compute pipeline %s -> %d", k->cs->variant, r); nvk_die("create compute pipeline"); }
    } else {
        VkPipelineShaderStageCreateInfo stages[2] = {
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = k->vs->module, .pName = "main"},
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = k->ps ? k->ps->module : VK_NULL_HANDLE, .pName = "main"}};
        static const VkPrimitiveTopology topo[3] = {VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
        VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = topo[k->topo_class]};
        VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
        VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .depthClampEnable = k->depth_clamp, .polygonMode = k->polygon == NVN_POLYGON_MODE_LINE ? VK_POLYGON_MODE_LINE : k->polygon == NVN_POLYGON_MODE_POINT ? VK_POLYGON_MODE_POINT : VK_POLYGON_MODE_FILL,
            .lineWidth = 1.0f};
        VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT, .alphaToCoverageEnable = k->a2c};
        VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .maxDepthBounds = 1.0f};
        VkPipelineColorBlendAttachmentState att[8];
        for (int i = 0; i < k->ncolor; ++i) {
            const blend_st *b = &k->blend[i];
            att[i] = (VkPipelineColorBlendAttachmentState){.blendEnable = (k->blend_en >> i) & 1,
                .srcColorBlendFactor = blend_factor(b->src_rgb), .dstColorBlendFactor = blend_factor(b->dst_rgb), .colorBlendOp = blend_op(b->eq_rgb),
                .srcAlphaBlendFactor = blend_factor(b->src_a), .dstAlphaBlendFactor = blend_factor(b->dst_a), .alphaBlendOp = blend_op(b->eq_a),
                .colorWriteMask = (k->chmask >> (4 * i)) & 0xf};
        }
        VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .logicOpEnable = k->logic, .logicOp = (VkLogicOp)k->logic_op, .attachmentCount = k->ncolor, .pAttachments = att};
        static const VkDynamicState dyn[] = {
            VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
            VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
            VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
            VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
            VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP,
            VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
            VK_DYNAMIC_STATE_VERTEX_INPUT_EXT};
        VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = sizeof dyn / sizeof *dyn, .pDynamicStates = dyn};
        VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = k->ncolor,
            .pColorAttachmentFormats = k->color, .depthAttachmentFormat = k->depth, .stencilAttachmentFormat = k->stencil};
        /* Framebuffer fetch variants: the identity input attachment mapping spelled out. Leaving it out means the same
         * by the spec, but Turnip 26.2 then indexes its attachment table with VK_ATTACHMENT_UNUSED and crashes inside
         * vkCreateGraphicsPipelines (Thor, 22 Oct). Only these pipelines carry it, so the others keep their cache. */
        static const uint32_t identity[8] = {0, 1, 2, 3, 4, 5, 6, 7};
        VkRenderingInputAttachmentIndexInfoKHR iai = {.sType = VK_STRUCTURE_TYPE_RENDERING_INPUT_ATTACHMENT_INDEX_INFO_KHR,
            .colorAttachmentCount = k->ncolor, .pColorAttachmentInputIndices = identity};
        int fetches = 0;
        for (int i = 0; k->ps && i < k->ps->nb; ++i) fetches |= k->ps->b[i].kind == 14;
        if (fetches) ri.pNext = &iai;
        VkGraphicsPipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
            .stageCount = k->ps ? 2 : 1, .pStages = stages, .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
            .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb, .pDynamicState = &dy, .layout = p->layout};
        VkPipelineCache pcache = nvk_pipeline_cache_begin();
        VkResult r = vkCreateGraphicsPipelines(nvk_dev, pcache, 1, &ci, NULL, &p->pipe);
        nvk_pipeline_cache_end();
        if (r != VK_SUCCESS) {
            VKLOG("graphics pipeline vs %s ps %s -> %d", k->vs->variant, k->ps ? k->ps->variant : "-", r);
            nvk_die("create graphics pipeline");
        }
    }
    uint64_t elapsed = nvk_now_ns() - started;
    if (mode == BUILD_DRAW) { atomic_fetch_add(&nvk_pipeline_count, 1); atomic_fetch_add(&nvk_pipeline_ns, elapsed); }
    if (mode == BUILD_DRAW && elapsed > 20000000ull) VKLOG("pipeline compile stalled %.1f ms (vs %s ps %s cs %s)", elapsed / 1e6,
        k->vs ? k->vs->variant : "-", k->ps ? k->ps->variant : "-", k->cs ? k->cs->variant : "-");
    if (!warming) warm_record(k);
    else atomic_fetch_add(&warm_built, 1);
    pthread_mutex_lock(&pipe_lock);
    p->ready = 1;
    pthread_cond_broadcast(&p->ready_cv);
    uint64_t made = warming ? 0 : ++pipelines_made;
    pthread_mutex_unlock(&pipe_lock);
    if (mode == BUILD_ASYNC) {
        uint64_t n = atomic_fetch_add(&async_built, 1) + 1, ns = atomic_fetch_add(&async_ns, elapsed) + elapsed;
        if (n <= 4 || n % 100 == 0) VKLOG("background pipelines built %llu (mean %.1f ms, off the replay thread); draws left out while compiling %llu",
            (unsigned long long)n, ns / 1e6 / (double)n, (unsigned long long)atomic_load(&async_skipped));
    }
    if (made && (made <= 4 || made % 100 == 0)) VKLOG("pipelines created: %llu", (unsigned long long)made);
}
static Pipe *pipeline(const PipeKey *k) { return pipeline_make(k, 0); }

/* Pipeline handles and shader pointers do not survive a restart. Persist only
 * shader variant names and the value part of the key. Prewarm never asks the
 * bridge to translate: at start the worker loads the recorded variants from
 * the on-device shader cache (module and layout; the c1 constants follow at
 * the first draw) and builds their pipelines through the persistent pipeline
 * cache, before the game draws them. Variants missing from the cache are
 * built once a draw has loaded them. */
#ifndef WARM_LIMIT
#define WARM_LIMIT 65536u
#endif
#define WARM_BUCKETS (WARM_LIMIT * 2u)
#define WARM_MAGIC 0x31575054u  /* TPW1 */
typedef struct {
    PipeKey key;              /* vs/ps/cs are zero on disk */
    char vs[80], ps[80], cs[80];
} WarmDisk;
typedef struct { WarmDisk disk; uint64_t hash; uint32_t next, older, newer; uint8_t done; } WarmEntry;
typedef struct {
    uint32_t magic, version, record_size, count;
    uint32_t vendor, device, driver;
    uint8_t uuid[VK_UUID_SIZE];
    uint64_t checksum;
} WarmHeader;
static WarmEntry warm_entries[WARM_LIMIT];
static unsigned warm_count;
/* Index and LRU contain entry indices + 1; zero is the end of a chain. */
static uint32_t warm_buckets[WARM_BUCKETS], warm_oldest, warm_newest;
static uint64_t warm_revision;
static pthread_mutex_t warm_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t warm_cv = PTHREAD_COND_INITIALIZER;
static pthread_once_t warm_once = PTHREAD_ONCE_INIT;
static _Atomic int warm_started;
static int warm_scan_requested;
static char warm_path[768];
static VkPhysicalDeviceProperties warm_props;

static uint64_t warm_hash(const void *p, size_t n) {
    const uint8_t *b = p;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
static void warm_touch(unsigned i) {
    uint32_t id = i + 1; WarmEntry *e = &warm_entries[i];
    if (warm_newest == id) return;
    if (e->older) warm_entries[e->older - 1].newer = e->newer;
    else if (warm_oldest == id) warm_oldest = e->newer;
    if (e->newer) warm_entries[e->newer - 1].older = e->older;
    e->older = warm_newest; e->newer = 0;
    if (warm_newest) warm_entries[warm_newest - 1].newer = id;
    else warm_oldest = id;
    warm_newest = id;
}
static void warm_index_add(unsigned i) {
    unsigned b = (unsigned)warm_entries[i].hash & (WARM_BUCKETS - 1);
    warm_entries[i].next = warm_buckets[b]; warm_buckets[b] = i + 1;
    warm_touch(i);
}
static void warm_index_rebuild(void) {
    memset(warm_buckets, 0, sizeof warm_buckets); warm_oldest = warm_newest = 0;
    for (unsigned i = 0; i < warm_count; ++i) {
        warm_entries[i].older = warm_entries[i].newer = 0;
        warm_index_add(i);
    }
}
static unsigned warm_find(const WarmDisk *rec, uint64_t hash) {
    for (uint32_t id = warm_buckets[(unsigned)hash & (WARM_BUCKETS - 1)]; id; id = warm_entries[id - 1].next)
        if (warm_entries[id - 1].hash == hash && !memcmp(&warm_entries[id - 1].disk, rec, sizeof *rec)) return id;
    return 0;
}
static int warm_name(const char *s) {
    size_t n = strnlen(s, 80);
    if (!n) return 1;
    if (n >= 80) return 0;
    /* Keep the cache filename alphabet in step with nvn_vk_shader.c:
     * -i<mask>, -F<mask>[-Z<swizzle>], and depth-offset -zq / -zs.
     * Rejecting a generated suffix discards the entire prewarm history. */
    for (size_t i = 0; i < n; ++i)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f') ||
              s[i] == '-' || s[i] == 'i' || s[i] == 'x' || s[i] == 'F' ||
              s[i] == 'Z' || s[i] == 'z' || s[i] == 'q' || s[i] == 's')) return 0;
    return 1;
}
static int warm_valid(const WarmDisk *r) {
    if (r->key.vs || r->key.ps || r->key.cs || !warm_name(r->vs) || !warm_name(r->ps) || !warm_name(r->cs)) return 0;
    if (r->key.cs) return 0;
    if (r->cs[0]) return !r->vs[0] && !r->ps[0] && r->key.ncolor == 0;
    return r->vs[0] && r->key.ncolor <= 8 && r->key.topo_class < 3 && r->key.polygon <= NVN_POLYGON_MODE_FILL;
}
static void warm_save(WarmDisk *records, unsigned count) {
    WarmHeader hdr = {.magic = WARM_MAGIC, .version = 1, .record_size = sizeof *records,
        .count = count, .vendor = warm_props.vendorID, .device = warm_props.deviceID,
        .driver = warm_props.driverVersion,
        .checksum = warm_hash(records, (size_t)count * sizeof *records)};
    memcpy(hdr.uuid, warm_props.pipelineCacheUUID, VK_UUID_SIZE);
    char tmp[800]; snprintf(tmp, sizeof tmp, "%s.tmp", warm_path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    int ok = fwrite(&hdr, 1, sizeof hdr, f) == sizeof hdr &&
        fwrite(records, sizeof *records, count, f) == count && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f)) ok = 0;
    if (ok) ok = rename(tmp, warm_path) == 0;
    if (!ok) unlink(tmp);
}
/* A record is shader variant names plus driver-independent state, so the history outlives a driver: only the
 * VkPipelineCache blob is driver-bound. Keyed per driver, the switch to Turnip 27 (24 Oct) started from an empty
 * history (pipeline cache 0 bytes, 2 records): a 1.4 s freeze and ~29,500 draws left out while all of it compiled
 * again during play. Records of another driver are now rebuilt by the worker like any other, except framebuffer
 * fetch variants when this driver has no dynamic rendering local read. TOTK_VK_WARM_ANY_DRIVER=0: per driver. */
extern int android_vk_local_read;
static int warm_any_driver(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_VK_WARM_ANY_DRIVER"); on = !(e && !strcmp(e, "0")); }
    return on;
}
/* Appends the records of one history file not yet present; 0 when the file is missing or invalid. */
static int warm_load_file(const char *path, unsigned *foreign, unsigned *unusable) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    WarmHeader hdr;
    int ok = fread(&hdr, 1, sizeof hdr, f) == sizeof hdr && hdr.magic == WARM_MAGIC && hdr.version == 1 &&
        hdr.record_size == sizeof(WarmDisk) && hdr.count <= WARM_LIMIT &&
        hdr.vendor == warm_props.vendorID && hdr.device == warm_props.deviceID;
    int same = ok && hdr.driver == warm_props.driverVersion && !memcmp(hdr.uuid, warm_props.pipelineCacheUUID, VK_UUID_SIZE);
    if (!same && !warm_any_driver()) ok = 0;
    WarmDisk *r = ok && hdr.count ? malloc((size_t)hdr.count * sizeof *r) : NULL;
    if (ok && hdr.count && !r) ok = 0;
    if (ok && hdr.count) ok = fread(r, sizeof *r, hdr.count, f) == hdr.count;
    if (ok) ok = fgetc(f) == EOF && hdr.checksum == warm_hash(r, (size_t)hdr.count * sizeof *r);
    if (ok) for (unsigned i = 0; i < hdr.count; ++i) if (!warm_valid(&r[i])) { ok = 0; break; }
    if (ok) for (unsigned i = 0; i < hdr.count && warm_count < WARM_LIMIT; ++i) {
        if (!same && !android_vk_local_read && (strstr(r[i].vs, "-F") || strstr(r[i].ps, "-F"))) { ++*unusable; continue; }
        uint64_t hash = warm_hash(&r[i], sizeof r[i]);
        if (warm_find(&r[i], hash)) continue;
        warm_entries[warm_count] = (WarmEntry){.disk = r[i], .hash = hash};
        warm_index_add(warm_count++);
        if (!same) ++*foreign;
    }
    free(r); fclose(f);
    return ok;
}
/* Before the driver-independent file existed: pipeline-warm-v1-<vendor>-<device>-<driver>-<uuid>.bin, one per driver.
 * Only the newest is taken over: the driver played last. Older ones also hold variants of builds long gone (the Thor
 * kept a 6 MB history from 22 Oct, ~15,000 pipelines), which the worker would compile at every start. */
static unsigned warm_load_legacy(unsigned *foreign, unsigned *unusable) {
    char dir[768]; snprintf(dir, sizeof dir, "%s", warm_path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0; else snprintf(dir, sizeof dir, ".");
    char prefix[64]; snprintf(prefix, sizeof prefix, "pipeline-warm-v1-%08x-%08x-", warm_props.vendorID, warm_props.deviceID);
    struct { char name[256]; time_t mtime; } found[16]; unsigned n = 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    for (struct dirent *e; (e = readdir(d)) && n < 16; ) {
        size_t len = strlen(e->d_name);
        if (strncmp(e->d_name, prefix, strlen(prefix)) || len < 4 || len >= sizeof found[0].name || strcmp(e->d_name + len - 4, ".bin")) continue;
        char path[1100]; struct stat st;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (stat(path, &st)) continue;
        unsigned j = n++;
        while (j && found[j - 1].mtime > st.st_mtime) { found[j] = found[j - 1]; --j; }
        snprintf(found[j].name, sizeof found[j].name, "%s", e->d_name); found[j].mtime = st.st_mtime;
    }
    closedir(d);
    for (unsigned i = n; i-- > 0; ) {   /* newest first; an invalid one falls back to the next */
        char path[1100]; snprintf(path, sizeof path, "%s/%s", dir, found[i].name);
        if (warm_load_file(path, foreign, unusable)) return 1;
    }
    return 0;
}
static void warm_load(void) {
    warm_count = 0; warm_index_rebuild();
    unsigned foreign = 0, unusable = 0, files = 0;
    if (warm_load_file(warm_path, &foreign, &unusable)) files = 1;
    else if (warm_any_driver()) {
        if (!access(warm_path, F_OK)) VKLOG("pipeline prewarm: invalid history ignored");
        files = warm_load_legacy(&foreign, &unusable);
        if (files) VKLOG("pipeline prewarm: history taken over from %u per-driver file(s)", files);
    } else if (!access(warm_path, F_OK)) VKLOG("pipeline prewarm: invalid history ignored");
    if (!files) return;
    VKLOG("pipeline prewarm: %u recorded variants loaded (%u recorded with another driver, %u fetch variants this driver cannot build)",
        warm_count, foreign, unusable);
}
static void warm_preload(void) {
    /* Load (or miss) a shared shader once, rather than once per pipeline. */
    typedef struct { char name[80]; int stage; } Module;
    pthread_mutex_lock(&warm_lock); unsigned count = warm_count; pthread_mutex_unlock(&warm_lock);
    if (!count) return;
    unsigned capacity = count * 3, buckets = 1;
    while (buckets < capacity * 2) buckets *= 2;
    Module *modules = calloc(capacity, sizeof *modules);
    uint32_t *index = calloc(buckets, sizeof *index);
    if (!modules || !index) { free(modules); free(index); VKLOG("pipeline prewarm: preload allocation failed; draws load normally"); return; }
    unsigned loaded = 0, missing = 0, used = 0, refs = 0;
    for (unsigned i = 0; i < count; ++i) {
        pthread_mutex_lock(&warm_lock);
        WarmDisk rec = warm_entries[i].disk;
        pthread_mutex_unlock(&warm_lock);
        const char *names[3] = {rec.vs, rec.ps, rec.cs};
        for (int s = 0; s < 3; ++s) if (names[s][0]) {
            refs++;
            unsigned b = (unsigned)(warm_hash(names[s], strlen(names[s])) ^ (unsigned)s) & (buckets - 1);
            while (index[b] && (modules[index[b] - 1].stage != s || strcmp(modules[index[b] - 1].name, names[s]))) b = (b + 1) & (buckets - 1);
            if (index[b]) continue;
            snprintf(modules[used].name, sizeof modules[used].name, "%s", names[s]); modules[used].stage = s;
            index[b] = ++used;
            if (nvk_shader_preload(names[s], s == 0 ? 0 : s == 1 ? 1 : 5)) loaded++; else missing++;
        }
    }
    free(modules); free(index);
    VKLOG("pipeline prewarm: %u unique variant loads from the shader cache, %u not cached (%u references)", loaded, missing, refs);
}
/* Most recently used first: the history is stored oldest first, and the newest records are what the place the
 * save resumes in draws. Records added during the scan are built already (done). */
static void warm_scan(void) {
    pthread_mutex_lock(&warm_lock); unsigned n = warm_count; pthread_mutex_unlock(&warm_lock);
    for (unsigned i = n; i-- > 0; ) {
        pthread_mutex_lock(&warm_lock);
        int done = warm_entries[i].done;
        WarmDisk rec = warm_entries[i].disk;
        pthread_mutex_unlock(&warm_lock);
        if (done) continue;
        PipeKey k = rec.key;
        if (rec.cs[0]) {
            k.cs = nvk_shader_find_variant(rec.cs);
            if (!k.cs || k.cs->stage != 5) continue;
        } else {
            k.vs = nvk_shader_find_variant(rec.vs);
            if (!k.vs || k.vs->stage != 0) continue;
            if (rec.ps[0]) {
                k.ps = nvk_shader_find_variant(rec.ps);
                if (!k.ps || k.ps->stage != 1) continue;
            }
        }
        pthread_mutex_lock(&warm_lock);
        /* A full history may replace this entry while the shaders were looked up. */
        if (memcmp(&warm_entries[i].disk, &rec, sizeof rec)) { pthread_mutex_unlock(&warm_lock); continue; }
        warm_entries[i].done = 1;
        pthread_mutex_unlock(&warm_lock);
        pipeline_make(&k, 1);
        uint64_t n = atomic_load(&warm_built);
        if (n == 1 || !(n % 100)) VKLOG("pipeline prewarm built %llu variants (draws waited %llu)",
            (unsigned long long)n, (unsigned long long)atomic_load(&warm_waits));
    }
}
static void *warm_worker(void *unused) {
    (void)unused;
    pthread_setname_np(pthread_self(), "totk-vk-warm");
    nice(10);
    nvk_shader_seed_load();   /* during the loading screen, not at the first shader a draw misses */
    const char *pre = getenv("TOTK_VK_PRELOAD");
    if (!(pre && !strcmp(pre, "0"))) warm_preload();
    uint64_t saved = 0, last_save = 0;
    for (;;) {
        pthread_mutex_lock(&warm_lock);
        while (!warm_scan_requested && saved == warm_revision) pthread_cond_wait(&warm_cv, &warm_lock);
        uint64_t now = nvk_now_ns();
        if (!warm_scan_requested && saved != warm_revision && now - last_save < 2000000000ull) {
            struct timespec deadline; clock_gettime(CLOCK_REALTIME, &deadline);
            uint64_t wait_ns = 2000000000ull - (now - last_save);
            deadline.tv_sec += (time_t)(wait_ns / 1000000000ull);
            deadline.tv_nsec += (long)(wait_ns % 1000000000ull);
            if (deadline.tv_nsec >= 1000000000l) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000l; }
            pthread_cond_timedwait(&warm_cv, &warm_lock, &deadline);
        }
        int scan = warm_scan_requested; warm_scan_requested = 0;
        now = nvk_now_ns();
        unsigned count = 0; uint64_t rev = 0;
        WarmDisk *snapshot = NULL;
        if (saved != warm_revision && now - last_save >= 2000000000ull) {
            count = warm_count; rev = warm_revision;
            snapshot = malloc((size_t)count * sizeof *snapshot);
            if (snapshot) {
                unsigned i = 0;
                for (uint32_t id = warm_oldest; id; id = warm_entries[id - 1].newer) snapshot[i++] = warm_entries[id - 1].disk;
            }
        }
        pthread_mutex_unlock(&warm_lock);
        if (scan) warm_scan();
        if (snapshot) {
            warm_save(snapshot, count);
            free(snapshot);
            saved = rev; last_save = nvk_now_ns();
        }
    }
    return NULL;
}
static void warm_initialize_once(void) {
    const char *off = getenv("TOTK_VK_PREWARM");
    if (off && !strcmp(off, "0")) return;
    const char *dir = getenv("TOTK_SHADER_CACHE");
    if (!dir || !*dir) dir = "shader-cache";
    mkdir(dir, 0700);
    vkGetPhysicalDeviceProperties(nvk_phys, &warm_props);
    char uuid[VK_UUID_SIZE * 2 + 1];
    for (unsigned i = 0; i < VK_UUID_SIZE; ++i) snprintf(uuid + i * 2, 3, "%02x", warm_props.pipelineCacheUUID[i]);
    int n = warm_any_driver()
        ? snprintf(warm_path, sizeof warm_path, "%s/pipeline-warm-v2-%08x-%08x.bin", dir, warm_props.vendorID, warm_props.deviceID)
        : snprintf(warm_path, sizeof warm_path, "%s/pipeline-warm-v1-%08x-%08x-%08x-%s.bin", dir,
            warm_props.vendorID, warm_props.deviceID, warm_props.driverVersion, uuid);
    if (n < 0 || (size_t)n >= sizeof warm_path) return;
    warm_load();
    warm_scan_requested = 1;
    pthread_t thread;
    if (!pthread_create(&thread, NULL, warm_worker, NULL)) {
        pthread_detach(thread);
        atomic_store(&warm_started, 1);
        pthread_cond_signal(&warm_cv);
    } else VKLOG("pipeline prewarm: worker unavailable");
}
static void warm_initialize(void) { pthread_once(&warm_once, warm_initialize_once); }
void nvk_pipeline_warm_signal(void) {
    if (!atomic_load(&warm_started)) return;
    pthread_mutex_lock(&warm_lock);
    warm_scan_requested = 1;
    pthread_cond_signal(&warm_cv);
    pthread_mutex_unlock(&warm_lock);
}
static void warm_record(const PipeKey *k) {
    if (!atomic_load(&warm_started)) return;
    WarmDisk rec = {0}; rec.key = *k;
    rec.key.vs = rec.key.ps = rec.key.cs = NULL;
    if (k->vs) snprintf(rec.vs, sizeof rec.vs, "%s", k->vs->variant);
    if (k->ps) snprintf(rec.ps, sizeof rec.ps, "%s", k->ps->variant);
    if (k->cs) snprintf(rec.cs, sizeof rec.cs, "%s", k->cs->variant);
    uint64_t hash = warm_hash(&rec, sizeof rec);
    pthread_mutex_lock(&warm_lock);
    uint32_t hit = warm_find(&rec, hash);
    if (hit) { warm_touch(hit - 1); pthread_mutex_unlock(&warm_lock); return; }
    unsigned i;
    if (warm_count < WARM_LIMIT) i = warm_count++;
    else {
        i = warm_oldest - 1;
        WarmEntry *e = &warm_entries[i];
        unsigned b = (unsigned)e->hash & (WARM_BUCKETS - 1);
        uint32_t *link = &warm_buckets[b];
        while (*link && *link != i + 1) link = &warm_entries[*link - 1].next;
        if (*link) *link = e->next;
        warm_oldest = e->newer;
        if (warm_oldest) warm_entries[warm_oldest - 1].older = 0;
        else warm_newest = 0;
    }
    warm_entries[i] = (WarmEntry){.disk = rec, .hash = hash, .done = 1};
    warm_index_add(i);
    warm_revision++;
    pthread_cond_signal(&warm_cv);
    pthread_mutex_unlock(&warm_lock);
}

/* ---------- Misaligned storage buffers ----------
 * Vulkan on the Adreno binds storage buffers at 64-byte offsets; NVN allows
 * 16. The translated shaders address a binding from its exact start, so such
 * a range is copied into an aligned scratch block before the draw and copied
 * back afterwards when the shader may write it. */
typedef struct { VkBuffer buffer; VkDeviceMemory memory; uint64_t size, used, fence; } Scratch;
static Scratch scratch[8];
static unsigned scratch_cur;
typedef struct { VkBuffer src; uint64_t src_off; VkBuffer dst; uint64_t dst_off, size; } CopyBack;
static CopyBack copy_back[64];
static int ncopy_back;
static uint64_t scratch_copies, scratch_bytes, scratch_reused;

static void scratch_alloc(uint64_t size, VkBuffer *buf, uint64_t *off) {
    uint64_t need = (size + 63) & ~63ull;
    Scratch *s = &scratch[scratch_cur];
    if (!s->buffer || s->used + need > s->size || (s->fence && s->fence != nvk_pending_fence())) {
        /* next block whose last use is complete */
        for (unsigned tries = 0; tries < 8; ++tries) {
            scratch_cur = (scratch_cur + 1) % 8;
            s = &scratch[scratch_cur];
            if (!s->buffer || s->fence <= nvk_completed_fence()) break;
        }
        if (s->buffer && s->fence > nvk_completed_fence()) nvk_die("storage scratch blocks exhausted");
        if (!s->buffer || s->size < need) {
            if (s->buffer) nvk_defer_buffer(s->buffer, s->memory, s->fence);
            s->size = need > (64u << 20) ? need : (64u << 20);
            VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = s->size,
                .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
            VKCHECK(vkCreateBuffer(nvk_dev, &bci, NULL, &s->buffer), "create storage scratch");
            VkMemoryRequirements req; vkGetBufferMemoryRequirements(nvk_dev, s->buffer, &req);
            VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                .memoryTypeIndex = nvk_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
            VKCHECK(vkAllocateMemory(nvk_dev, &mai, NULL, &s->memory), "allocate storage scratch");
            VKCHECK(vkBindBufferMemory(nvk_dev, s->buffer, s->memory, 0), "bind storage scratch");
        }
        s->used = 0;
    }
    *buf = s->buffer; *off = s->used;
    s->used += need; s->fence = nvk_pending_fence();
}

/* Barriers around the copies of misaligned storage ranges. They order buffer memory only: earlier shader and transfer
   writes before the copy reads, the copy before the shaders (and, for a copy back, before anything later). A full
   barrier (ALL_COMMANDS, MEMORY_WRITE) also made the driver flush the colour and depth caches, ~70 times a frame in
   TOTK's open world. TOTK_VK_COPY_BARRIER=0 (or "copybarrier 0" in the shader debug file): full barriers, as before. */
int nvk_copy_barrier_narrow = -1;
static void copy_barrier(VkCommandBuffer cb, int after, int back) {
    if (nvk_copy_barrier_narrow < 0) { const char *e = getenv("TOTK_VK_COPY_BARRIER"); nvk_copy_barrier_narrow = !(e && !strcmp(e, "0")); }
    if (!nvk_copy_barrier_narrow) { nvk_barrier(cb); return; }
    const VkPipelineStageFlags shaders = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    VkPipelineStageFlags src, dst;
    if (!after) {   /* earlier writes to the source (and earlier reads of the destination) before the copy */
        src = shaders | VK_PIPELINE_STAGE_TRANSFER_BIT; mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        dst = VK_PIPELINE_STAGE_TRANSFER_BIT; mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        if (back) src |= VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;   /* the game's memory may have been read as vertices */
    } else {        /* the copy before the shaders read the scratch copy, or before anything reads the game's memory */
        src = VK_PIPELINE_STAGE_TRANSFER_BIT; mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        dst = back ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : shaders;
        mb.dstAccessMask = back ? VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    }
    vkCmdPipelineBarrier(cb, src, dst, 0, 1, &mb, 0, NULL, 0, NULL);
}
static void finish_copy_back(VkCommandBuffer cb) {
    if (!ncopy_back) return;
    nvk_buffer_write_bump();
    nvk_rendering_end(NVK_END_BUFFER);
    copy_barrier(cb, 0, 1);
    for (int i = 0; i < ncopy_back; ++i) {
        VkBufferCopy r = {copy_back[i].src_off, copy_back[i].dst_off, copy_back[i].size};
        vkCmdCopyBuffer(cb, copy_back[i].src, copy_back[i].dst, 1, &r);
    }
    copy_barrier(cb, 1, 1);
    ncopy_back = 0;
}

/* ---------- Resources ---------- */
typedef struct {
    VkWriteDescriptorSet w[64];
    VkDescriptorBufferInfo bi[64];
    VkDescriptorImageInfo ii[64];
    int n;
    uint32_t dynoff[64];   /* dynamic uniform buffers (NvkShader.dyn): the offset that goes with the bind */
} Writes;

static int texture_of(uint64_t handle, tex_t *t, view_t *v, int *has_view) {
    int tid = (int)(handle & 0xfffff);
    return tid && nvk_registered_texture(tid, t, v, has_view);
}

/* A draw that samples an attachment of the open pass gets a new pass, so it
 * reads what earlier draws stored (draw_samples_rt in nvn_mtl.m); on a tiler
 * the open pass's writes are still in GMEM. TOTK_VK_FEEDBACK_SPLIT=0: off. */
int nvk_feedback_split_on(void) {
    static int on = -1;
    static unsigned live_TOTK_VK_FEEDBACK_SPLIT; if (on < 0 || live_TOTK_VK_FEEDBACK_SPLIT != nvk_live_gen) { live_TOTK_VK_FEEDBACK_SPLIT = nvk_live_gen; const char *e = nvk_live_getenv("TOTK_VK_FEEDBACK_SPLIT"); on = !(e && !strcmp(e, "0")); }
    return on;
}

/* ---------- Framebuffer fetch (decals) ----------
 * A fragment shader proven to sample texture slot T at its own pixel (tools/fetch_proof.py), paired with a vertex
 * shader proven to pass that screen position, may read colour attachment j directly when T holds j's image: same
 * image at level 0 / layer 0, same format, no swizzle, a 2D texture, and viewport 0 covering the whole target. The
 * draw then needs no feedback split (its slot is no longer sampled). Inside the pass a by-region barrier makes earlier
 * draws' writes to j visible to the read (nvk_pass_local_read_barrier). Needs VK_KHR_dynamic_rendering_local_read.
 * TOTK_VK_FETCH=0: off. */
extern int android_vk_local_read;
static uint64_t fetch_draws;
static int fetch_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_VK_FETCH"); on = android_vk_local_read && !(e && !strcmp(e, "0"));
        VKLOG("framebuffer fetch for own-pixel samples: %s", on ? "on" : android_vk_local_read ? "off (TOTK_VK_FETCH=0)" : "unavailable (no dynamic rendering local read)"); }
    return on;
}
static int texture_of(uint64_t handle, tex_t *t, view_t *v, int *has_view);
static int view_base(const view_t *v) {   /* level 0, layer 0, no swizzle */
    return !v || (!(v->set_levels && v->base_level) && !(v->set_layers && v->min_layer) && !v->set_swizzle);
}
/* Why a colour fetch was not used (measurement, nvk_split_report): counted per proven sample of an attached texture. */
enum { FC_OK, FC_VS, FC_VIEW, FC_FORMAT, FC_VIEWPORT, FC_STORES, FC_VARIANT, FC_N };
static const char *const fc_name[FC_N] = {"ok", "vertex shader not proven", "view not level 0 / layer 0 2D", "format differs", "viewport not the whole target", "shader stores", "variant refused"};
static uint64_t fc_count[FC_N];
static int fetch_target_why(const tex_t *t, const view_t *v, const p_set_rt *rt, int j) {
    if (t->d.target != 1 || (v && v->set_target && v->target != 1) || !view_base(v)) return FC_VIEW;
    const view_t *cv = rt->has_cview[j] ? &rt->cview[j] : NULL;
    if (!view_base(cv)) return FC_VIEW;
    int sf = v && v->set_format ? v->format : (int)t->d.format, af = cv && cv->set_format ? cv->format : (int)rt->color[j]->d.format;
    if (sf != af) return FC_FORMAT;
    if (!nvk_S.has_vp) return FC_VIEWPORT;
    const float *vp = nvk_S.vp[0];
    return vp[0] == 0 && vp[1] == 0 && vp[2] == (float)rt->color[j]->d.width && vp[3] == (float)rt->color[j]->d.height ? FC_OK : FC_VIEWPORT;
}
/* Turnip 26.2 dropped the buffer stores of a fragment shader drawn as a fetch variant: TOTK's water then never marked
   its screen tiles (value 2 in the tile-class buffer, measured on the GPU right after the draw), so the tile lighting
   skipped the water and the pond stayed black. A shader that stores keeps sampling its own pixel instead; the pass
   splits for it, which reads the same values. TOTK_VK_FETCH_STORES=1: fetch variants for them too. */
static int fetch_storing_ok(const NvkShader *ps) {
    static int on = -1;
    static unsigned live_TOTK_VK_FETCH_STORES; if (on < 0 || live_TOTK_VK_FETCH_STORES != nvk_live_gen) { live_TOTK_VK_FETCH_STORES = nvk_live_gen; const char *e = nvk_live_getenv("TOTK_VK_FETCH_STORES"); on = e && !strcmp(e, "1"); }
    if (on) return 1;
    for (int i = 0; i < ps->nb; ++i) if ((ps->b[i].kind == 4 || ps->b[i].kind == 6 || ps->b[i].kind == 13) && ps->b[i].rw == 1) return 0;
    return 1;
}
/* Depth fetch (TOTK_VK_FETCH_DEPTH=1): the proven sample's texture is the depth attachment of the pass. Allowed when
   it is that image at level 0 / layer 0 as a 2D texture in the attachment's format, viewport 0 covers the whole
   target and the draw does not write depth (it would read and write the same value). The sampled view's component
   mapping becomes the read's channel shuffle (*zsw, see nvk_shader_fetch_variant_z). 0 = ok, else the refusal. */
enum { FD_OK, FD_TARGET, FD_FORMAT, FD_VIEWPORT, FD_WRITES, FD_SWIZZLE, FD_MIXED, FD_VARIANT, FD_N };
static const char *const fd_name[FD_N] = {"ok", "not level 0 / layer 0 2D", "format", "viewport", "draw writes depth", "swizzle", "two mappings", "variant refused"};
static uint64_t fd_count[FD_N];
static int fetch_depth_ok(const tex_t *t, const view_t *v, const p_set_rt *rt, uint16_t *zsw) {
    const tex_t *d = rt->depth;
    const view_t *dv = rt->has_dview ? &rt->dview : NULL;
    if (t->d.target != 1 || (v && v->set_target && v->target != 1) || (v && ((v->set_levels && v->base_level) || (v->set_layers && v->min_layer)))
        || (dv && ((dv->set_levels && dv->base_level) || (dv->set_layers && dv->min_layer)))) return FD_TARGET;
    int sf = v && v->set_format ? v->format : (int)t->d.format, af = dv && dv->set_format ? dv->format : (int)d->d.format;
    if (sf != af) return FD_FORMAT;
    if (!nvk_S.has_vp) return FD_VIEWPORT;
    const float *vp = nvk_S.vp[0];
    if (vp[0] != 0 || vp[1] != 0 || vp[2] != (float)d->d.width || vp[3] != (float)d->d.height) return FD_VIEWPORT;
    if (nvk_S.ds.dt && nvk_S.ds.dw) return FD_WRITES;
    NvnVkFormat f; VkComponentMapping comp;
    uint32_t sel[4] = {t->d.swizzle_r, t->d.swizzle_g, t->d.swizzle_b, t->d.swizzle_a};
    if (v && v->set_swizzle) for (int i = 0; i < 4; ++i) sel[i] = v->swz[i];
    if (!nvn_vk_format_map((uint32_t)sf, &f) || !nvn_vk_format_swizzle(&f, sel, &comp)) return FD_SWIZZLE;
    const VkComponentSwizzle c[4] = {comp.r, comp.g, comp.b, comp.a};
    uint16_t z = 0;
    for (int i = 0; i < 4; ++i) {   /* sampling depth returns (D, 0, 0, 1): 0 = D, 5/6 = zero, 7 = one in the shuffle with (0, 0, 0, 1) */
        VkComponentSwizzle s = c[i] == VK_COMPONENT_SWIZZLE_IDENTITY ? (VkComponentSwizzle)(VK_COMPONENT_SWIZZLE_R + i) : c[i];
        unsigned idx = s == VK_COMPONENT_SWIZZLE_R ? 0 : s == VK_COMPONENT_SWIZZLE_G || s == VK_COMPONENT_SWIZZLE_B || s == VK_COMPONENT_SWIZZLE_ZERO ? 4
            : s == VK_COMPONENT_SWIZZLE_A || s == VK_COMPONENT_SWIZZLE_ONE ? 7 : 8;
        if (idx > 7) return FD_SWIZZLE;
        z |= (uint16_t)(idx << (4 * i));
    }
    *zsw = z;
    return FD_OK;
}
static uint64_t fetch_store_kept;
static NvkShader *fetch_choose(NvkShader *ps, const NvkShader *vs, const p_set_rt *rt) {
    int stores = !fetch_storing_ok(ps);
    if (stores) fetch_store_kept++;
    uint8_t att[8]; memset(att, 0xff, sizeof att);
    int any = 0, zset = 0, colour = 0; uint16_t zsw = 0x7650;
    for (int k = 0; k < ps->nfetch; ++k) {
        tex_t t; view_t v; int hv;
        uint64_t h = nvk_S.tex[1][ps->fetch[k].slot];
        if (!h || !texture_of(h, &t, &v, &hv)) continue;
        NvkImage *img = nvk_image_existing(t.serial);
        uint16_t all = img ? nvk_rt_slots(t.serial, img) : 0, slots = all & 0xff;
        if (slots) {   /* an attached colour target: the case that splits the pass when it is not fetched */
            int j = __builtin_ctz(slots);
            int why = !(vs->fetch_locs >> ps->fetch[k].loc & 1) ? FC_VS : stores ? FC_STORES
                : j >= rt->n || !rt->color[j] ? FC_VIEW : fetch_target_why(&t, hv ? &v : NULL, rt, j);
            if (why) { fc_count[why]++; continue; }
            att[k] = (uint8_t)j; any = 1; colour++;
            continue;
        }
        if (stores || !(vs->fetch_locs >> ps->fetch[k].loc & 1)) continue;
        if (!(all & 0x100) || !rt->depth || !nvk_fetch_depth_on()) continue;
        uint16_t z = 0; int why = fetch_depth_ok(&t, hv ? &v : NULL, rt, &z);
        if (!why && zset && z != zsw) why = FD_MIXED;
        if (why) { fd_count[why]++; continue; }
        zsw = z; zset = 1; att[k] = 8; any = 1;
    }
    if (!any) return NULL;
    NvkShader *f = zset ? nvk_shader_fetch_variant_z(ps, att, zsw) : nvk_shader_fetch_variant(ps, att);
    if (zset) fd_count[f ? FD_OK : FD_VARIANT]++;
    if (colour) fc_count[f ? FC_OK : FC_VARIANT] += (uint64_t)colour;
    return f;
}
static uint16_t fetch_attachments(const NvkShader *ps) {   /* bits 0-7: colour slots, bit 8: depth */
    uint16_t m = 0;
    for (int i = 0; ps && i < ps->nb; ++i) if (ps->b[i].kind == 14 && ps->b[i].reg >= 0 && ps->b[i].reg <= 8) m |= (uint16_t)(1u << ps->b[i].reg);
    return m;
}
uint64_t nvk_fetch_draws(void) { return fetch_draws; }

/* NVN leaves a colour target that the fragment shader has no output for untouched (Maxwell writes only the targets in
   the shader's output map); in Vulkan its contents become undefined, and the Adreno wrote black. TOTK's class-depth
   tile draws (ps 233dfb93: only gl_FragDepth, scene colour still bound, no blending) painted the lit water black and
   left it black on every frame. Such targets get a zero colour write mask. TOTK_VK_UNWRITTEN_MASK=0: written as before. */
static int unwritten_masked(void) {
    static int on = -1;
    static unsigned live_TOTK_VK_UNWRITTEN_MASK; if (on < 0 || live_TOTK_VK_UNWRITTEN_MASK != nvk_live_gen) { live_TOTK_VK_UNWRITTEN_MASK = nvk_live_gen; const char *e = nvk_live_getenv("TOTK_VK_UNWRITTEN_MASK"); on = !(e && !strcmp(e, "0")); }
    return on;
}
/* Attachment slots the current draw writes, and the ones it samples (nvk_pass_after_draw). */
static uint16_t draw_writes_now, draw_sampled_now;
/* The draw stores to buffers or images. Noted for its pass once that is open: write_set runs before the pass begins
   (a feedback split may just have ended it, as right before TOTK's water draw), and the note then got lost, leaving the
   pass-end barrier, game-barrier and feedback decisions blind to the store (the water's tile mask, read by the next
   pass's vertex shaders). */
static int draw_storage_now, draw_image_store_now;
static struct { uint64_t a, n; } draw_wr[8]; static unsigned draw_nwr;   /* buffer ranges the draw stores to */
static void draw_wr_note(uint64_t a, uint64_t n) { if (draw_nwr < 8) { draw_wr[draw_nwr].a = a; draw_wr[draw_nwr++].n = n; } else draw_wr[7].n = UINT64_MAX / 2; }
static uint16_t draw_writes(const NvkShader *ps, const p_set_rt *rt) {
    if (nvk_S.raster_discard) return 0;
    uint16_t w = 0;
    if (ps) for (int i = 0; i < rt->n && i < 8; ++i)
        if (rt->color[i] && (ps->out_mask >> i & 1) && (nvk_S.chmask.mask >> (4 * i) & 0xf)) w |= (uint16_t)(1u << i);
    const ds_st *d = &nvk_S.ds;
    if (rt->depth && ((d->dt && d->dw) || (d->st && (nvk_S.st_wmask[0] | nvk_S.st_wmask[1])))) w |= 0x100;
    return w;
}

/* Why feedback splits happen (every 300 frames, nvk_split_report): the sampled slot the open pass wrote (depth, colour,
   both, or neither: a store in the pass or the draw writing it itself), per shader that sampled it. */
enum { SPL_DEPTH, SPL_COLOUR, SPL_BOTH, SPL_OTHER, SPL_N };
static struct { char name[9]; uint8_t stage, proof, fetch; uint64_t n[SPL_N]; } spl_tab[128];
static uint64_t spl_total[SPL_N], spl_lost;
static void split_note(const NvkShader *s, int st, uint16_t slots) {
    uint16_t w = slots & nvk_pass_written_slots();
    int c = (w & 0x100) ? ((w & 0xff) ? SPL_BOTH : SPL_DEPTH) : (w & 0xff) ? SPL_COLOUR : SPL_OTHER;
    spl_total[c]++;
    unsigned h = 0;
    for (int i = 0; i < 8 && s->variant[i]; ++i) h = h * 31u + (unsigned char)s->variant[i];
    for (unsigned probe = 0; probe < 128; ++probe) {
        typeof(spl_tab[0]) *e = &spl_tab[(h + probe) & 127];
        if (!e->name[0]) { snprintf(e->name, sizeof e->name, "%.8s", s->variant); e->stage = (uint8_t)st; e->proof = s->nfetch > 0 || strstr(s->variant, "-F"); e->fetch = strstr(s->variant, "-F") != NULL; }
        else if (strncmp(e->name, s->variant, 8) || e->stage != st) continue;
        e->n[c]++;
        return;
    }
    spl_lost++;
}

/* The registry lookup of each texture binding, done in prepare_textures, is reused by write_set for the same draw
   (same stage, binding and handle): one lock and copy fewer per texture. write_set still asks the image table, so a
   texture a game thread releases in between is still caught (dummy view). TOTK_VK_TEX_LOOKUP_ONCE=0: looked up twice. */
static int tc_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_VK_TEX_LOOKUP_ONCE"); on = !(e && !strcmp(e, "0")); }
    return on;
}
static uint64_t tc_token = 1, tc_hits, tc_misses;   /* tc_token: one per draw or dispatch */
static struct { uint64_t token, h; tex_t t; view_t v; int hv; } tc_tab[6][64];
static void tc_put(int st, int i, uint64_t h, const tex_t *t, const view_t *v, int hv) {
    if (!tc_on() || st < 0 || st > 5 || i < 0 || i >= 64) return;
    tc_tab[st][i].token = tc_token; tc_tab[st][i].h = h; tc_tab[st][i].t = *t; tc_tab[st][i].v = *v; tc_tab[st][i].hv = hv;
}
static int tc_get(int st, int i, uint64_t h, tex_t *t, view_t *v, int *hv) {
    if (!tc_on() || st < 0 || st > 5 || i < 0 || i >= 64 || tc_tab[st][i].token != tc_token || tc_tab[st][i].h != h) { tc_misses++; return 0; }
    *t = tc_tab[st][i].t; *v = tc_tab[st][i].v; *hv = tc_tab[st][i].hv; tc_hits++;
    return 1;
}

/* A tiler runs every vertex shader of a render pass (binning) before the pass's fragment shaders. A vertex shader
 * reading a storage buffer that fragment shaders of the open pass wrote therefore read it before those writes. TOTK
 * classifies its screen tiles that way: ~260 G-buffer fragment shaders mark their tile in a tile-class buffer, and
 * the tile pass of the same render pass (vs fbb7c2ff, 240 instances; at 640x360 vs f6686edf / 78b29311 / 0faec1b4)
 * reads it in its vertex shader: tiles read as empty were left unlit, the tile-sized black blocks (23 Oct). Such a
 * draw now ends the pass first (with its shader-write barrier). TOTK_VK_VS_STORAGE_SPLIT=0: kept in the pass. */
static uint64_t vs_storage_splits;
static void vertex_storage_split(const NvkShader *vs) {
    static int on = -1;
    static unsigned live_TOTK_VK_VS_STORAGE_SPLIT; if (on < 0 || live_TOTK_VK_VS_STORAGE_SPLIT != nvk_live_gen) { live_TOTK_VK_VS_STORAGE_SPLIT = nvk_live_gen; const char *e = nvk_live_getenv("TOTK_VK_VS_STORAGE_SPLIT"); on = !(e && !strcmp(e, "0")); }
    if (!on || !vs || !nvk_pass_storage_written()) return;
    for (int i = 0; i < vs->nb; ++i) {
        const NvkBinding *b = &vs->b[i];
        uint64_t addr = 0;
        if (b->kind == 6 && b->reg >= 0 && b->reg < 16) addr = nvk_S.ssbo[0][b->reg].addr;
        else if (b->kind == 13 && b->reg >= 0 && b->reg < 16) {
            const p_bind_buf *u = &nvk_S.ubo[0][b->reg];
            if (u->addr && u->size >= (uint64_t)b->off + 8) nvk_ubo_read(u->addr + b->off, 8, (uint8_t *)&addr);
        } else continue;
        uint64_t n = b->kind == 6 && nvk_S.ssbo[0][b->reg].size ? nvk_S.ssbo[0][b->reg].size : addr ? nvk_pool_remaining(addr) : 0;
        if (!addr || !nvk_pass_storage_overlaps(addr, n)) continue;   /* only a buffer this pass stored to */
        if (!(vs_storage_splits++ % 1000)) VKLOG("vertex shader %.12s reads storage written in the open pass: pass ended (%llu so far)", vs->variant, (unsigned long long)vs_storage_splits);
        nvk_rendering_end(NVK_END_FEEDBACK);
        return;
    }
}

/* Sampled and storage textures need their images (creation, stale uploads)
 * before the rendering instance starts. */
static void prepare_textures(const NvkShader *s, int st) {
    if (!s) return;
    for (int i = 0; i < s->nb; ++i) {
        const NvkBinding *b = &s->b[i];
        uint64_t h = b->kind == 3 && b->reg >= 0 && b->reg < 128 ? nvk_S.tex[st][b->reg] : b->kind == 4 && b->reg >= 0 && b->reg < 32 ? nvk_S.img[st][b->reg] : 0;
        tex_t t; view_t v; int hv;
        if (b->kind == 3 || b->kind == 4) {
            VkImageViewType type = (VkImageViewType)(b->view & 0xff);
            if (!h || !texture_of(h, &t, &v, &hv)) { nvk_rendering_end(NVK_END_TEXTURE); nvk_dummy_view(type, b->kind == 4, nvk_cmd()); continue; }
            tc_put(st, i, h, &t, &v, hv);
            NvkImage *existing = nvk_image_existing(t.serial);
            if (b->kind == 4) nvk_scene_defer_write(existing, t.serial);   /* a store into a scene target runs its deferred clear first (B5) */
            else nvk_scene_defer_read(existing, t.serial);                  /* a sampled one keeps its previous contents */
            int attached = nvk_rendering_attached_image(t.serial, existing);   /* 2: a folded clear of it is still pending */
            uint16_t slots = st == 5 ? 0 : nvk_rt_slots(t.serial, existing);
            if (attached == 2) nvk_rendering_end(NVK_END_FEEDBACK);
            else if (attached && nvk_feedback_split_on() && nvk_feedback_split_needed(slots, draw_writes_now)) { split_note(s, st, slots); nvk_rendering_end(NVK_END_FEEDBACK); }
            draw_sampled_now |= slots;
            /* The write below falls back to a dummy if the game releases the texture meanwhile;
               the dummy must already exist, since creating it needs a transfer outside the pass. */
            if (!nvk_dummy_view_ready(type, b->kind == 4)) { nvk_rendering_end(NVK_END_TEXTURE); nvk_dummy_view(type, b->kind == 4, nvk_cmd()); }
            if (nvk_image_needs_prepare_existing(existing, &t)) {
                nvk_rendering_end(NVK_END_TEXTURE);
                if (!existing) nvk_alias_writeback(&t);   /* may submit: the command buffer changes */
                NvkImage *img = nvk_image(&t, nvk_cmd());
                nvk_image_refresh(img, &t, nvk_cmd());
            }
        }
    }
}

static int buffer_info(uint64_t addr, uint64_t size, VkDescriptorBufferInfo *out, uint64_t align, uint64_t max_range, int storage_rw) {
    VkBuffer buf; uint64_t off;
    if (!addr || !nvk_resolve(addr, 1, &buf, &off)) return 0;
    uint64_t avail = nvk_pool_remaining(addr);
    uint64_t range = size ? size : avail;
    if (range > avail) range = avail;
    if (range > max_range) range = max_range;
    if (off % align) {
        if (!size && storage_rw >= 0) {
            /* Address read by the shader from a uniform block: extent unknown.
               Copy up to TOTK_VK_SBUF_COPY_MAX bytes (default 4 MiB). */
            static uint64_t cap; static int said;
            if (!cap) { const char *e = getenv("TOTK_VK_SBUF_COPY_MAX"); cap = e ? strtoull(e, NULL, 0) : 4u << 20; }
            if (range > cap) range = cap;
            if (said++ < 8) VKLOG("storage buffer %#llx of unknown size (%llu bytes to its pool end): %llu bytes copied",
                (unsigned long long)addr, (unsigned long long)avail, (unsigned long long)range);
            size = range;
        }
        if (storage_rw < 0 || !size) {
            VKLOG("%s binding %#llx (size %llu) is not %llu-byte aligned in its pool", storage_rw < 0 ? "uniform" : "storage",
                (unsigned long long)addr, (unsigned long long)size, (unsigned long long)align);
            nvk_die("misaligned NVN buffer binding");
        }
        /* A read-only range copied earlier in this submission, with no GPU write into game buffers since, is bound
           from that copy again: no copy, no barriers, no pass end (dispatches bind the same ranges many times).
           TOTK_VK_SBUF_COPY_REUSE=0: copied every time. */
        static int reuse = -1;
        if (reuse < 0) { const char *e = getenv("TOTK_VK_SBUF_COPY_REUSE"); reuse = !(e && !strcmp(e, "0")); }
        static struct { VkBuffer src, dst; uint64_t src_off, range, dst_off, fence, epoch; } sc[64];
        unsigned k = (unsigned)(((uintptr_t)buf >> 4) ^ (off >> 4) ^ (range << 3)) & 63;
        uint64_t fence = nvk_pending_fence(), epoch = nvk_buffer_write_epoch();
        if (reuse && !storage_rw && sc[k].src == buf && sc[k].src_off == off && sc[k].range == range && sc[k].fence == fence && sc[k].epoch == epoch) {
            scratch_reused++;
            *out = (VkDescriptorBufferInfo){sc[k].dst, sc[k].dst_off, range};
            return 1;
        }
        VkCommandBuffer cb = nvk_cmd();
        VkBuffer sb; uint64_t so;
        scratch_alloc(range, &sb, &so);
        if (reuse && !storage_rw) { sc[k].src = buf; sc[k].src_off = off; sc[k].range = range; sc[k].dst = sb; sc[k].dst_off = so; sc[k].fence = fence; sc[k].epoch = epoch; }
        nvk_rendering_end(NVK_END_BUFFER);
        copy_barrier(cb, 0, 0);
        VkBufferCopy r = {off, so, range};
        vkCmdCopyBuffer(cb, buf, sb, 1, &r);
        copy_barrier(cb, 1, 0);
        if (storage_rw) {
            if (ncopy_back == 64) nvk_die("too many written misaligned storage buffers in one draw");
            copy_back[ncopy_back++] = (CopyBack){sb, so, buf, off, range};
        }
        if (!(scratch_copies++ & 1023)) VKLOG("misaligned storage buffers copied: %llu (%llu MiB)", (unsigned long long)scratch_copies,
            (unsigned long long)((scratch_bytes += range) >> 20));
        else scratch_bytes += range;
        buf = sb; off = so;
    }
    *out = (VkDescriptorBufferInfo){buf, off, range};
    return 1;
}

/* Immutable uniform snapshots. Reusing a bank after changing its bytes would
 * retroactively change earlier draws in the same command buffer. */
typedef struct { VkBuffer buffer; VkDeviceMemory memory; uint8_t *map; uint64_t size, used, fence; } DriverArena;
static DriverArena driver_arena[8];
static struct { uint8_t bytes[0xA00]; uint64_t fence; VkDescriptorBufferInfo binding; } driver_last[6][2];
static uint64_t arena_alloc(uint64_t size, VkBuffer *buffer, uint8_t **map);
/* The bank's bytes depend only on the stage's bindings (and the dispatch grid for compute): unchanged since the last
   fill in this submission, the previous copy is bound without filling and comparing 2.5 KiB again (every draw did,
   the largest memcpy share of the replay profile). TOTK_VK_DRIVER_BANK_FAST=0: fill and compare every time. */
static uint64_t driver_epoch[6][2]; static uint32_t driver_grid[6][2][3];
static uint64_t driver_fast_hits, driver_fills;
static int driver_fast_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_VK_DRIVER_BANK_FAST"); on = !(e && !strcmp(e, "0")); }
    return on;
}
static VkDescriptorBufferInfo driver_bank(int bank, int stage) {
    uint64_t fence = nvk_pending_fence();
    unsigned b = bank == 2;
    if (driver_fast_on() && stage >= 0 && stage < 6 && driver_last[stage][b].fence == fence && driver_epoch[stage][b] == nvk_bind_epoch[stage]
        && (stage != 5 || !memcmp(driver_grid[stage][b], dispatch_grid, sizeof dispatch_grid))) { driver_fast_hits++; return driver_last[stage][b].binding; }
    driver_fills++;
    if (stage >= 0 && stage < 6) { driver_epoch[stage][b] = nvk_bind_epoch[stage]; memcpy(driver_grid[stage][b], dispatch_grid, sizeof dispatch_grid); }
    uint8_t bytes[0xA00]; nvk_driver_bank_fill(bytes, bank, stage, dispatch_grid);
    if (driver_last[stage][b].fence == fence && !memcmp(bytes, driver_last[stage][b].bytes, sizeof bytes))
        return driver_last[stage][b].binding;
    VkBuffer buffer; uint8_t *map; uint64_t offset = arena_alloc(sizeof bytes, &buffer, &map);
    memcpy(map, bytes, sizeof bytes);
    memcpy(driver_last[stage][b].bytes, bytes, sizeof bytes);
    driver_last[stage][b].fence = fence;
    driver_last[stage][b].binding = (VkDescriptorBufferInfo){buffer, offset, sizeof bytes};
    return driver_last[stage][b].binding;
}
static uint64_t arena_alignment(void) {   /* copies are bound as uniform or storage buffers */
    static uint64_t alignment;
    if (!alignment) { VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(nvk_phys, &p);
        alignment = p.limits.minUniformBufferOffsetAlignment > p.limits.minStorageBufferOffsetAlignment ? p.limits.minUniformBufferOffsetAlignment : p.limits.minStorageBufferOffsetAlignment;
        if (!alignment) alignment = 1; }
    return alignment;
}
/* Space in this submission's arena: returns the offset, *map points at it. */
static uint64_t arena_alloc(uint64_t size, VkBuffer *buffer, uint8_t **map) {
    uint64_t fence = nvk_pending_fence();
    DriverArena *a = &driver_arena[fence % 8];
    if (a->fence != fence) {
        if (a->fence > nvk_completed_fence()) nvk_die("driver bank arena reused before completion");
        a->used = 0; a->fence = fence;
    }
    uint64_t alignment = arena_alignment();
    uint64_t offset = (a->used + alignment - 1) / alignment * alignment;
    if (!a->buffer || offset + size > a->size) {
        if (a->buffer) { vkUnmapMemory(nvk_dev, a->memory); nvk_defer_buffer(a->buffer, a->memory, fence); }
        a->size = a->size ? a->size * 2 : 1u << 20; offset = 0;
        while (a->size < size) a->size *= 2;
        VkBufferCreateInfo ci = {.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size=a->size, .usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
        VKCHECK(vkCreateBuffer(nvk_dev, &ci, NULL, &a->buffer), "create driver bank arena");
        VkMemoryRequirements req; vkGetBufferMemoryRequirements(nvk_dev, a->buffer, &req);
        VkMemoryAllocateInfo mi = {.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize=req.size,
            .memoryTypeIndex=nvk_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        VKCHECK(vkAllocateMemory(nvk_dev, &mi, NULL, &a->memory), "allocate driver bank arena");
        VKCHECK(vkBindBufferMemory(nvk_dev, a->buffer, a->memory, 0), "bind driver bank arena");
        VKCHECK(vkMapMemory(nvk_dev, a->memory, 0, VK_WHOLE_SIZE, 0, (void **)&a->map), "map driver bank arena");
    }
    a->used = offset + size; *buffer = a->buffer; *map = a->map + offset;
    return offset;
}

/* A uniform binding whose range a uniform update of the open pass covers: bind a snapshot of the contents
   the GPU will see at this point. Snapshots are reused until the next in-pass update or submission. */
static uint64_t ubo_early_skips;
static int ubo_early_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_VK_UBO_EARLY_CHECK"); on = !(e && !strcmp(e, "0")); }
    return on;
}
static int ubo_snapshot(uint64_t addr, uint64_t size, VkDescriptorBufferInfo *out) {
    /* A snapshot only exists for a pending uniform update. The candidate interval contains the eventual pool-clamped
       interval, so a negative result can skip the pool lookup entirely. A positive result is checked again after
       clamping; unrelated updates beyond a pool boundary must not create a snapshot. */
    uint64_t candidate = !size || size > 65536 ? 65536 : size;
    if (ubo_early_on() && addr <= UINT64_MAX - candidate && !nvk_ubo_unflushed(addr, candidate)) {
        ubo_early_skips++; return 0;
    }
    uint64_t avail = nvk_pool_remaining(addr), range = size ? size : avail;
    if (range > avail) range = avail;
    if (range > 65536) range = 65536;
    /* Keyed by the newest overlapping update rather than by any in-pass update anywhere (the global epoch): a draw's
       own per-draw uniform update no longer makes every other uniform block of later draws copy itself again
       (23 Oct, Thor profile: memcpy in write_set ~8% of the replay thread). TOTK_VK_UBO_SNAP_RANGE=0: global epoch. */
    static int by_range = -1; static unsigned g;
    if (by_range < 0 || g != nvk_live_gen) { g = nvk_live_gen; const char *e = nvk_live_getenv("TOTK_VK_UBO_SNAP_RANGE"); by_range = !(e && !strcmp(e, "0")); }
    uint64_t seq = range ? nvk_ubo_unflushed_seq(addr, range) : 0;
    if (!seq) return 0;
    static struct { uint64_t addr, range, epoch, fence; VkDescriptorBufferInfo bi; } cache[64];
    uint64_t epoch = by_range ? seq : nvk_ubo_epoch(), fence = nvk_pending_fence();
    unsigned k = (unsigned)((addr >> 4) ^ (addr >> 12) ^ range) & 63;
    if (cache[k].addr == addr && cache[k].range == range && cache[k].epoch == epoch && cache[k].fence == fence) { *out = cache[k].bi; return 1; }
    VkBuffer buffer; uint8_t *map; uint64_t offset = arena_alloc(range, &buffer, &map);
    nvk_ubo_read(addr, range, map);
    *out = (VkDescriptorBufferInfo){buffer, offset, range};
    cache[k].addr = addr; cache[k].range = range; cache[k].epoch = epoch; cache[k].fence = fence; cache[k].bi = *out;
    return 1;
}

/* Uniform ranges (<= 64 KiB) bound as copies of their contents as of the submission (the in-pass uniform log included),
   unless GPU work not completed yet writes them (then the game's memory: the GPU timeline). Reused within a submission.
   - TOTK_FENCE_LAG (hle_nvn.c): the game may rewrite memory while the previous frame's GPU work still reads it.
     TOTK_FENCE_LAG_COPY=0: bind the game's memory as before.
   - TOTK_ASYNC_REPLAY: the submission's uniforms were captured at its submit call (nvk_ucap_active); the game has
     rebuilt its constant pools since, so every uniform binding must be a copy of the captured bytes.
   - TOTK_VK_UBO_COPY=1 (live, off by default): always, as the Metal layer does with its submit-time snapshots. Our GPU
     runs 25-30 ms behind a frame, longer than the console's; if TOTK rewrites constants for the next frame while this
     one is still drawn, the shadow casters and the lit pass could read different cascade matrices (the open "trembling
     shadows" lead, 24 Oct). This is the A/B for that. */
__attribute__((weak)) int nvn_fence_lag_active(void) { return 0; }   /* hle_nvn.c; host tests link without it */
__attribute__((weak)) int nvk_ubo_diff_on(void) { return 0; }
__attribute__((weak)) void nvk_ubo_diff_note(uint64_t a, uint64_t n, const uint8_t *c) { (void)a; (void)n; (void)c; }
__attribute__((weak)) void nvk_ubo_diff_skip(void) {}
__attribute__((weak)) int nvk_ucap_active(void) { return 0; }
__attribute__((weak)) int nvk_ucap_cover(uint64_t addr, uint64_t size, uint64_t *serial, uint32_t *idx, const uint8_t **bytes, uint64_t *rlo, uint64_t *rlen) { (void)addr; (void)size; (void)serial; (void)idx; (void)bytes; (void)rlo; (void)rlen; return 0; }       /* nvn_vk_backend.c; host tests of this file link without it */
static uint64_t lag_copies, lag_copy_bytes, lag_copy_skipped;
static int ubo_copy_all(void) {
    static int on = -1; static unsigned g;
    if (on < 0 || g != nvk_live_gen) { g = nvk_live_gen; const char *e = nvk_live_getenv("TOTK_VK_UBO_COPY"); on = e && !strcmp(e, "1"); }
    return on;
}
int nvk_ubo_copying(void) { return nvn_fence_lag_active() || nvk_ucap_active() || ubo_copy_all(); }
/* One arena copy per captured uniform range (nvk_ucap_cover), keyed by capture, range and submission. */
#define CAP_ARENA_N 4096
#define CAP_ARENA_MAX (2u << 20)
typedef struct { uint64_t serial; uint32_t idx; uint64_t fence, rlo; VkBuffer buffer; uint64_t offset; } CapArena;
static CapArena cap_arena[CAP_ARENA_N];
static uint64_t cap_arena_binds, cap_arena_copies, cap_arena_bytes;
static int cap_arena_on(void) {
    static int on = -1; static unsigned g;
    if (on < 0 || g != nvk_live_gen) { g = nvk_live_gen; const char *e = nvk_live_getenv("TOTK_VK_UBO_CAP_ARENA"); on = !(e && !strcmp(e, "0")); }
    return on;
}
void nvk_cap_arena_stats(uint64_t *binds, uint64_t *copies, uint64_t *bytes) { *binds = cap_arena_binds; *copies = cap_arena_copies; *bytes = cap_arena_bytes; }
static int lag_copy(uint64_t addr, uint64_t size, VkDescriptorBufferInfo *out) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_FENCE_LAG_COPY"); on = !(e && !strcmp(e, "0")); }
    if (!addr) return 0;
    int lag = on && nvn_fence_lag_active();
    if (!lag && !nvk_ucap_active() && !ubo_copy_all()) return 0;
    uint64_t avail = nvk_pool_remaining(addr), range = size ? size : avail;
    if (range > avail) range = avail;
    if (!range || range > 65536) return 0;
    if (nvk_gpu_written_live(addr, range)) { lag_copy_skipped++; return 0; }
    static struct { uint64_t addr, range, epoch, fence; VkDescriptorBufferInfo bi; } cache[256];
    uint64_t epoch = nvk_ubo_epoch(), fence = nvk_pending_fence();
    unsigned k = (unsigned)((addr >> 4) ^ (addr >> 14) ^ range) & 255;
    if (cache[k].addr == addr && cache[k].range == range && cache[k].epoch == epoch && cache[k].fence == fence) { *out = cache[k].bi; return 1; }
    if (nvk_ucap_active() && cap_arena_on()) {   /* inside a captured range: one arena copy of that range, bound at an offset */
        uint64_t serial, rlo, rlen; uint32_t idx; const uint8_t *bytes;
        if (nvk_ucap_cover(addr, range, &serial, &idx, &bytes, &rlo, &rlen) && rlen <= CAP_ARENA_MAX && !((addr - rlo) & (arena_alignment() - 1))) {
            CapArena *e = &cap_arena[((uint64_t)idx * 0x9E3779B1u + serial * 0x85EBCA6Bu) % CAP_ARENA_N];
            if (e->serial != serial || e->idx != idx || e->fence != fence || e->rlo != rlo) {
                VkBuffer cb; uint8_t *cm; uint64_t co = arena_alloc(rlen, &cb, &cm);
                memcpy(cm, bytes, (size_t)rlen);
                *e = (CapArena){serial, idx, fence, rlo, cb, co};
                cap_arena_copies++; cap_arena_bytes += rlen;
            }
            *out = (VkDescriptorBufferInfo){e->buffer, e->offset + (addr - rlo), range};
            cache[k].addr = addr; cache[k].range = range; cache[k].epoch = epoch; cache[k].fence = fence; cache[k].bi = *out;
            cap_arena_binds++;
            return 1;
        }
    }
    VkBuffer buffer; uint8_t *map; uint64_t offset = arena_alloc(range, &buffer, &map);
    nvk_ubo_read(addr, range, map);
    *out = (VkDescriptorBufferInfo){buffer, offset, range};
    cache[k].addr = addr; cache[k].range = range; cache[k].epoch = epoch; cache[k].fence = fence; cache[k].bi = *out;
    lag_copies++; lag_copy_bytes += range;
    return 1;
}
void nvk_lag_copy_stats(uint64_t *copies, uint64_t *bytes, uint64_t *skipped) { *copies = lag_copies; *bytes = lag_copy_bytes; *skipped = lag_copy_skipped; }

/* TOTK_VK_UBO_DIFF (nvn_vk_backend.c): each uniform range of a submission, once, copied for the completion-time comparison. */
static void ubo_diff_probe(uint64_t addr, uint64_t size) {
    uint64_t avail = nvk_pool_remaining(addr), range = size ? size : avail;
    if (range > avail) range = avail;
    if (!range || range > 65536) return;
    static struct { uint64_t addr, range, fence; } seen[512];
    uint64_t fence = nvk_pending_fence();
    unsigned k = (unsigned)((addr >> 4) ^ (addr >> 13) ^ range) & 511;
    if (seen[k].addr == addr && seen[k].range == range && seen[k].fence == fence) return;
    seen[k].addr = addr; seen[k].range = range; seen[k].fence = fence;
    if (nvk_ubo_unflushed_seq(addr, range)) { nvk_ubo_diff_skip(); return; }   /* in-pass updates: intermediate values are right */
    uint8_t *tmp = malloc((size_t)range);
    if (!tmp) return;
    nvk_ubo_read(addr, range, tmp);
    nvk_ubo_diff_note(addr, range, tmp);
    free(tmp);
}
static int dbg_tracing;   /* the current draw is traced (nvk_shader_debug_frame) */
static int dbg_dumpbuf_slot = -1, dbg_dumpafter_slot = -1;   /* "dumpbufafter <slot>": the same, right after that draw */
static uint64_t dbg_after_addr, dbg_after_size;
static int dbg_dumpdepth;   /* "dumpdepth 1": the traced draw's depth attachment before it, "dumpdepth 2": after it */   /* "dumpbuf <slot>": write the CPU view (shared with the GPU) of that storage buffer of the next traced draw */
static int dbg_dumptex_binding = -1;   /* "dumptex <binding>": dump that texture of the next traced draw */
static struct { int stage; unsigned binding; } dbg_dumpubo[8]; static unsigned dbg_ndumpubo;   /* "dumpubo <stage> <binding>": log the full effective UBO on the next traced draw */
static int dbg_writer_fmt; static unsigned dbg_writer_w, dbg_writer_h, dbg_writer_left;
static char dbg_writer_after[33];   /* "writerafter <prefix>": the writer trace starts after a draw with that fragment shader */   /* "writer <fmt> <w> <h>": trace draws rendering to such a target */
static struct { char hash[33]; int binding; } dbg_white[8]; static unsigned dbg_nwhite;   /* "white <hash prefix> <binding>" */
/* Debugging ("bufwatch <hex address>"): every draw, dispatch, copy and update that uses the buffer holding that address
   during the next whole frame is logged in command order, with what it does to it. */
static uint64_t dbg_watch_addr; static int dbg_watch_state; static unsigned dbg_watch_seq;
static char dbg_watch_what[200];
int nvk_bufwatch_hit(uint64_t addr, uint64_t n) {
    return dbg_watch_state == 2 && dbg_watch_seq < 6000 && addr && addr <= dbg_watch_addr && dbg_watch_addr < addr + (n ? n : 1);
}
void nvk_bufwatch_note(const char *what, uint64_t addr, uint64_t n) {
    VKLOG("bufwatch %u: %s %#llx+%llu (watched at %+lld)", dbg_watch_seq++, what, (unsigned long long)addr, (unsigned long long)n, (long long)(dbg_watch_addr - addr));
}
static void watch_binding(int st, const p_bind_buf *u, int reg, int rw, const char *kind) {
    if (!u || !nvk_bufwatch_hit(u->addr, u->size ? u->size : nvk_pool_remaining(u->addr))) return;
    VKLOG("bufwatch %u: %s | stage %d %s slot %d %s %#llx+%llu (watched at %+lld)", dbg_watch_seq++, dbg_watch_what, st, kind, reg,
        rw > 0 ? "WRITE" : "read", (unsigned long long)u->addr, (unsigned long long)u->size, (long long)(dbg_watch_addr - u->addr));
}
static void dump_effective_ubo(const NvkShader *s, int st, unsigned binding, const p_bind_buf *u) {
    if (!u || !u->addr || !u->size || u->size > 65536) {
        VKLOG("ubo dump skipped: shader %s stage %d binding %u address %#llx size %llu (valid range 1..65536)",
            s->variant, st, binding, (unsigned long long)(u ? u->addr : 0), (unsigned long long)(u ? u->size : 0));
        return;
    }
    uint8_t *bytes = malloc((size_t)u->size);
    if (!bytes) { VKLOG("ubo dump allocation failed: shader %s stage %d binding %u size %llu", s->variant, st, binding, (unsigned long long)u->size); return; }
    nvk_ubo_read(u->addr, u->size, bytes);   /* pool bytes plus queued uniform updates, in command order */
    VKLOG("ubo dump begin: shader %s stage %d binding %u address %#llx size %llu; rows are raw uint32 and float32 vec4s",
        s->variant, st, binding, (unsigned long long)u->addr, (unsigned long long)u->size);
    for (uint64_t off = 0; off + 16 <= u->size; off += 16) {
        uint32_t raw[4]; float f[4];
        memcpy(raw, bytes + off, sizeof raw); memcpy(f, bytes + off, sizeof f);
        VKLOG("ubo dump: stage %d binding %u vec4 %llu: %08x %08x %08x %08x | %.9g %.9g %.9g %.9g",
            st, binding, (unsigned long long)(off / 16), raw[0], raw[1], raw[2], raw[3], f[0], f[1], f[2], f[3]);
    }
    if (u->size & 15) VKLOG("ubo dump: stage %d binding %u trailing bytes %llu", st, binding, (unsigned long long)(u->size & 15));
    VKLOG("ubo dump end: shader %s stage %d binding %u", s->variant, st, binding);
    free(bytes);
}
static void write_set(Writes *W, const NvkShader *s, int st) {
    W->n = 0;
    VkBuffer zero; uint32_t zero_size = nvk_zero_buffer(&zero);
    for (int i = 0; i < s->nb; ++i) {
        const NvkBinding *b = &s->b[i];
        if (b->count != 1) { VKLOG("shader %s binding %u is an array of %u", s->variant, b->binding, b->count); nvk_die("descriptor arrays are not implemented"); }
        VkWriteDescriptorSet *w = &W->w[W->n];
        *w = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = b->binding,
            .descriptorCount = 1, .descriptorType = (VkDescriptorType)b->dtype};
        VkDescriptorBufferInfo *bi = &W->bi[W->n];
        VkDescriptorImageInfo *ii = &W->ii[W->n];
        *bi = (VkDescriptorBufferInfo){zero, 0, zero_size};
        switch (b->kind) {
        case 1: {   /* user uniform buffer */
            const p_bind_buf *u = b->reg >= 0 && b->reg < 16 ? &nvk_S.ubo[st][b->reg] : NULL;
            if (u && u->addr && nvk_ubo_diff_on()) ubo_diff_probe(u->addr, u->size);
            if (u && !(u->addr && (ubo_snapshot(u->addr, u->size, bi) || lag_copy(u->addr, u->size, bi)))) buffer_info(u->addr, u->size, bi, 64, 65536, -1);
            if (dbg_watch_state == 2) watch_binding(st, u, b->reg, 0, "uniform");
            if (dbg_tracing && u && u->addr) {
                for (unsigned q = 0; q < dbg_ndumpubo; ++q) if (dbg_dumpubo[q].stage == st && dbg_dumpubo[q].binding == b->binding) {
                    dump_effective_ubo(s, st, b->binding, u); dbg_dumpubo[q].stage = -1;
                }
                float f[12] = {0}; uint64_t n = u->size && u->size < sizeof f ? u->size : sizeof f;
                nvk_ubo_read(u->addr, n, (uint8_t *)f);
                VKLOG("trace:   stage %d uniform c%d (binding %u) %#llx size %llu: %g %g %g %g | %g %g %g %g | %g %g %g %g", st, b->reg + 3, b->binding,
                    (unsigned long long)u->addr, (unsigned long long)u->size, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11]);
                if (u->size <= 256) {   /* small blocks in full, as raw words (masks and counts are integers) */
                    uint32_t wv[64] = {0}; nvk_ubo_read(u->addr, u->size, (uint8_t *)wv);
                    for (unsigned q = 0; q * 16 < u->size; ++q)
                        VKLOG("trace:   stage %d uniform c%d vec4 %u: %#x %#x %#x %#x (%g %g %g %g)", st, b->reg + 3, q, wv[4*q], wv[4*q+1], wv[4*q+2], wv[4*q+3],
                            *(float *)&wv[4*q], *(float *)&wv[4*q+1], *(float *)&wv[4*q+2], *(float *)&wv[4*q+3]);
                }
                if (u->size >= 148 * 16) {
                    uint32_t w7[4]; nvk_ubo_read(u->addr + 147 * 16, sizeof w7, (uint8_t *)w7);
                    VKLOG("trace:   stage %d uniform c%d vec4 147: %#x %#x %#x %#x (%g %g %g %g)", st, b->reg + 3, w7[0], w7[1], w7[2], w7[3],
                        *(float *)&w7[0], *(float *)&w7[1], *(float *)&w7[2], *(float *)&w7[3]);
                }
                if (u->size >= 26 * 16) {   /* TOTK tile lighting: c11[25].x is the tile class a draw shades */
                    float c25[4]; nvk_ubo_read(u->addr + 25 * 16, sizeof c25, (uint8_t *)c25);
                    VKLOG("trace:   stage %d uniform c%d vec4 25: %g %g %g %g", st, b->reg + 3, c25[0], c25[1], c25[2], c25[3]);
                }
                if (u->size >= 147 * 16) {   /* TOTK G-buffer shaders: c4[146] = tile scale x, y and the packed tile row width */
                    uint32_t c146[4]; nvk_ubo_read(u->addr + 146 * 16, sizeof c146, (uint8_t *)c146);
                    float fx, fy; memcpy(&fx, &c146[0], 4); memcpy(&fy, &c146[1], 4);
                    VKLOG("trace:   stage %d uniform c%d vec4 146: %g %g | %#x %#x", st, b->reg + 3, fx, fy, c146[2], c146[3]);
                }
            }
            if (s->dyn >> i & 1) {   /* the offset goes with the bind; dynamic offset + range stays within the buffer */
                if (bi->offset > UINT32_MAX) nvk_die("dynamic uniform buffer offset above 4 GiB");
                W->dynoff[i] = (uint32_t)bi->offset; bi->offset = 0;
                w->descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            }
            w->pBufferInfo = bi; break;
        }
        case 7: *bi = (VkDescriptorBufferInfo){nvk_support_buffer(), 0, 65536}; w->pBufferInfo = bi; break;
        case 8: *bi = (VkDescriptorBufferInfo){s->c1, 0, s->c1_size}; w->pBufferInfo = bi; break;
        case 9: case 10: {
            *bi = driver_bank(b->kind == 9 ? 0 : 2, st);
            w->pBufferInfo = bi; break;
        }
        case 6: case 12: {   /* storage buffer slot / compute uniform as pointer */
            const p_bind_buf *u = b->reg >= 0 && b->reg < 16 ? (b->kind == 12 ? &nvk_S.ubo[st][b->reg] : &nvk_S.ssbo[st][b->reg]) : NULL;
            if (u && !(b->rw == 0 && u->size && u->size <= 65536 && lag_copy(u->addr, u->size, bi))) buffer_info(u->addr, u->size, bi, 64, UINT32_MAX, b->rw);
            if (dbg_watch_state == 2) watch_binding(st, u, b->reg, b->rw, b->kind == 12 ? "uniform-as-storage" : "storage");
            if (dbg_tracing && u) {   /* e.g. TOTK's tile mask: written by G-buffer fragment shaders, read by tile-lighting vertex shaders */
                uint32_t q[4] = {0}; if (u->addr && nvk_pool_remaining(u->addr) >= 448 + sizeof q) memcpy(q, (const void *)(uintptr_t)(u->addr + 448), sizeof q);
                VKLOG("trace:   stage %d storage slot %d (binding %u, %s) %#llx size %llu; CPU view of bytes 448..463: %u %u %u %u", st, b->reg, b->binding,
                    b->rw ? "written" : "read", (unsigned long long)u->addr, (unsigned long long)u->size, q[0], q[1], q[2], q[3]);
                if (dbg_dumpbuf_slot == b->reg && u->addr) {
                    uint64_t n = u->size ? u->size : nvk_pool_remaining(u->addr), rem = nvk_pool_remaining(u->addr); if (n > rem) n = rem;
                    nvk_debug_dump_buffer(u->addr, n); dbg_dumpbuf_slot = -1;
                }
                if (dbg_dumpafter_slot == b->reg && u->addr) {
                    uint64_t rem = nvk_pool_remaining(u->addr);
                    dbg_after_addr = u->addr; dbg_after_size = u->size && u->size < rem ? u->size : rem; dbg_dumpafter_slot = -1;
                }
            }
            if (b->rw > 0) { draw_storage_now = 1; if (u) { uint64_t n = u->size ? u->size : nvk_pool_remaining(u->addr); nvk_note_gpu_write(u->addr, n); draw_wr_note(u->addr, n); } }
            w->pBufferInfo = bi; break;
        }
        case 13: {   /* storage buffer address read from a user uniform block */
            const p_bind_buf *u = b->reg >= 0 && b->reg < 16 ? &nvk_S.ubo[st][b->reg] : NULL;
            uint64_t va = 0;
            if (u && u->addr && u->size >= (uint64_t)b->off + 8) nvk_ubo_read(u->addr + b->off, 8, (uint8_t *)&va);   /* with updates not yet executed */
            if (va) buffer_info(va, 0, bi, 64, UINT32_MAX, b->rw);
            if (dbg_watch_state == 2 && va) { p_bind_buf pb = {0}; pb.addr = va; watch_binding(st, &pb, b->reg, b->rw, "storage-from-uniform"); }
            if (b->rw > 0) { draw_storage_now = 1; nvk_note_gpu_write(va, nvk_pool_remaining(va)); draw_wr_note(va, nvk_pool_remaining(va)); }
            w->pBufferInfo = bi; break;
        }
        case 14: {   /* framebuffer fetch: colour attachment reg (8: the depth attachment) of the pass, read at the fragment's own pixel */
            ii->sampler = VK_NULL_HANDLE; ii->imageLayout = VK_IMAGE_LAYOUT_GENERAL; ii->imageView = b->reg == 8 ? nvk_rt_depth_input_view() : nvk_rt_color_view(b->reg);
            if (!ii->imageView) nvk_die("framebuffer fetch without its attachment");
            w->pImageInfo = ii; break;
        }
        case 3: case 4: case 15: {   /* 15: a texture the fetch variant no longer samples, bound to a dummy (still declared) */
            uint64_t h = b->kind == 3 ? (b->reg >= 0 && b->reg < 128 ? nvk_S.tex[st][b->reg] : 0) : b->kind == 4 ? (b->reg >= 0 && b->reg < 32 ? nvk_S.img[st][b->reg] : 0) : 0;
            VkImageViewType type = (VkImageViewType)(b->view & 0xff);
            if (b->view & 256) nvk_die("multisampled texture bindings are not implemented");
            tex_t t; view_t v; int hv;
            ii->imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            int known = h && (tc_get(st, i, h, &t, &v, &hv) || texture_of(h, &t, &v, &hv));
            NvkImage *img = known ? nvk_image_existing(t.serial) : NULL;
            if (b->kind == 4 && dbg_writer_left && img && (int)t.d.format == dbg_writer_fmt && t.d.width == dbg_writer_w && t.d.height == dbg_writer_h) {
                dbg_writer_left--;
                VKLOG("trace: image store to serial %llu mem %p (fmt %d %ux%u) by shader %s stage %d binding %u", (unsigned long long)t.serial, t.mem, (int)t.d.format,
                    t.d.width, t.d.height, s->variant, st, b->binding);
            }
            if (dbg_tracing && dbg_dumptex_binding == (int)b->binding && img && st == 1) {
                nvk_debug_dump_image(img, t.serial); dbg_dumptex_binding = -1;
            }
            int white = 0;
            for (unsigned k = 0; k < dbg_nwhite && st == 1; ++k)
                if (dbg_white[k].binding == (int)b->binding && !strncmp(s->variant, dbg_white[k].hash, strlen(dbg_white[k].hash))) white = 1;
            if (dbg_tracing) {
                if (!h || !texture_of(h, &t, &v, &hv)) VKLOG("trace:   stage %d binding %u slot %d handle %#llx: no texture (dummy)", st, b->binding, b->reg, (unsigned long long)h);
                else VKLOG("trace:   stage %d binding %u slot %d: serial %llu mem %p fmt %d%s %ux%ux%u target %d levels %d, view type %d, image %s%s%s",
                    st, b->binding, b->reg, (unsigned long long)t.serial, t.mem, (int)t.d.format, hv && v.set_format ? " (view fmt)" : "", t.d.width, t.d.height, t.d.depth,
                    (int)t.d.target, (int)t.d.levels, (int)(b->view & 0xff), img ? "present" : "MISSING", img && nvk_image_gpu_owned(img) ? ", GPU-written" : ", CPU upload",
                    nvk_rendering_attached(t.serial) ? ", ATTACHED to the open pass" : "");
            }
            if (white && nvk_white_view_ready()) {
                ii->imageView = nvk_white_view(nvk_cmd());
            } else if (h && !img && known) {
                /* Released (or re-registered) by a game thread after prepare_textures: NVN allows
                   finalizing once commands are recorded. One draw samples zeros instead. */
                static unsigned released;
                if (released++ < 16) VKLOG("texture %llu released between preparation and binding (shader %s slot %d) -> dummy image",
                    (unsigned long long)t.serial, s->variant, b->reg);
                ii->imageView = nvk_dummy_view(type, b->kind == 4, nvk_cmd());
            } else if (img) {
                ii->imageView = nvk_image_sample_view(img, &t, hv ? &v : NULL, type, b->kind == 4);
                if (b->kind == 4) { nvk_image_written(img); draw_storage_now = draw_image_store_now = 1; }
            } else {
                static unsigned missing;
                if (h && missing++ < 24) VKLOG("missing bound texture: shader %s stage %d slot %d handle %#llx -> dummy image",
                    s->variant, st, b->reg, (unsigned long long)h);
                ii->imageView = nvk_dummy_view(type, b->kind == 4, nvk_cmd());
            }
            if (b->dtype == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
                int sid = b->sreg >= 0 ? (int)(nvk_S.samp[st][b->sreg & 127] >> 20) : (int)(h >> 20);
                ii->sampler = nvk_sampler_for_id(sid);
                if (!ii->sampler) {
                    samp_par sp;
                    static unsigned missing_sampler;
                    if (h && missing_sampler++ < 24) VKLOG("missing sampler: shader %s stage %d slot %d sampler %d -> nearest fallback",
                        s->variant, st, b->reg, sid);
                    memset(&sp, 0, sizeof sp);
                    sp.minf = NVN_MIN_FILTER_NEAREST; sp.magf = NVN_MAG_FILTER_NEAREST;
                    sp.ws = sp.wt = sp.wr = NVN_WRAP_MODE_CLAMP_TO_EDGE; sp.lod_max = 1000.0f; sp.aniso = 1.0f;
                    ii->sampler = nvk_sampler(&sp);
                }
            }
            w->pImageInfo = ii; break;
        }
        default:
            VKLOG("shader %s binding %u kind %u descriptor %u", s->variant, b->binding, b->kind, b->dtype);
            nvk_die("NVN resource kind not implemented on Vulkan");
        }
        W->n++;
    }
}

/* ---------- Descriptor set reuse ----------
 * Every draw allocated and wrote two descriptor sets (write_set was ~31% of the replay in the open world). Within a
 * submission (its sets live until its ring slot is reused) a set with exactly the same layout and contents is now
 * reused: buffers, offsets, ranges, views, samplers and layouts compared in full, not just hashed. Views, buffers and
 * samplers are destroyed only after the submissions using them completed (nvk_defer_*), so a handle cannot come back
 * for something else within one submission. With dynamic uniform buffers (nvn_vk_shader.c) the per-draw constant
 * offsets are left out of the set, so most draws of one material share it. A bind is skipped when the command buffer
 * already holds the same sets with the same offsets (same set layouts, no push constants: compatible layouts).
 * TOTK_VK_DESC_CACHE=0: a new set per draw, as before. */
#define DC_N 2048
#define DC_WRITES 32
typedef struct { uint64_t fence, hash; VkDescriptorSetLayout layout; VkDescriptorSet set; uint32_t n; uint64_t rec[DC_WRITES][4]; } DescEntry;
static DescEntry *dc;
static uint64_t dc_hits, dc_misses, dc_uncached, binds_skipped, dc_evictions;
/* A material commonly reuses the same sets on neighbouring draws. Check its last entry
 * before constructing/hashing the full key. This is still exact descriptor equality;
 * write_set and all resource preparation/invalidation run normally. Pointers refer only
 * to the fixed dc table, and are revalidated after eviction and on every submission. */
static DescEntry *dc_recent[64];
static uint64_t dc_recent_hits, dc_key_builds;
static int dc_recent_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_VK_DESC_RECENT"); on = !(e && !strcmp(e, "0")); }
    return on;
}
static unsigned dc_recent_slot(VkDescriptorSetLayout layout) {
    uintptr_t v = (uintptr_t)layout;
    return (unsigned)((v >> 4) ^ (v >> 12)) & 63u;
}
static int dc_matches(const DescEntry *e, const Writes *W) {
    for (int i = 0; i < W->n; ++i) {
        const VkWriteDescriptorSet *w = &W->w[i];
        if (e->rec[i][0] != (w->dstBinding | (uint64_t)w->descriptorType << 32)) return 0;
        if (w->pBufferInfo) {
            const VkDescriptorBufferInfo *b = w->pBufferInfo;
            if (e->rec[i][1] != (uint64_t)(uintptr_t)b->buffer || e->rec[i][2] != b->offset || e->rec[i][3] != b->range) return 0;
        } else {
            const VkDescriptorImageInfo *b = w->pImageInfo;
            if (e->rec[i][1] != (uint64_t)(uintptr_t)b->imageView || e->rec[i][2] != (uint64_t)(uintptr_t)b->sampler || e->rec[i][3] != (uint64_t)b->imageLayout) return 0;
        }
    }
    return 1;
}
/* Same total storage, four candidates per hash bucket instead of one. Optional until Thor A/B:
   TOTK_VK_DESC_WAYS=4. Full descriptor equality and submission lifetime checks remain mandatory. */
static uint8_t dc_victim[DC_N / 4];
static unsigned dc_ways(void) {
    static unsigned ways;
    if (!ways) { const char *e = getenv("TOTK_VK_DESC_WAYS"); ways = e && !strcmp(e, "4") ? 4 : 1; }
    return ways;
}
static int dc_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_VK_DESC_CACHE"); on = !(e && !strcmp(e, "0")); if (on) dc = calloc(DC_N, sizeof *dc); if (!dc) on = 0; }
    return on;
}
/* New sets are written through descriptor update templates (as yuzu and DXVK do): one template per set layout and
   write pattern (bindings and types in order), the descriptors packed into one block, so the driver walks a fixed
   table instead of parsing every VkWriteDescriptorSet. Same contents as before. TOTK_VK_DESC_TEMPLATE=0: off. */
#define DT_N 1024
typedef union { VkDescriptorImageInfo image; VkDescriptorBufferInfo buffer; } DtSlot;
typedef struct { VkDescriptorSetLayout layout; uint32_t n, sig[DC_WRITES]; VkDescriptorUpdateTemplate tmpl; } DtEntry;
static DtEntry *dt;
static uint64_t dt_writes, dt_created, dt_fallback;
static int dt_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_VK_DESC_TEMPLATE"); on = !(e && !strcmp(e, "0")); if (on && !(dt = calloc(DT_N, sizeof *dt))) on = 0; }
    return on;
}
/* The template for this layout and write pattern (made on first use), or VK_NULL_HANDLE to write the plain way. */
static VkDescriptorUpdateTemplate dt_template(const Writes *W, VkDescriptorSetLayout layout) {
    if (!dt_on() || !W->n || W->n > DC_WRITES) return VK_NULL_HANDLE;
    uint32_t sig[DC_WRITES]; uint64_t h = (uint64_t)(uintptr_t)layout * 0x9e3779b97f4a7c15ull ^ (uint64_t)W->n;
    for (int i = 0; i < W->n; ++i) {
        const VkWriteDescriptorSet *w = &W->w[i];
        if (w->descriptorCount != 1 || w->dstArrayElement || (!w->pBufferInfo && !w->pImageInfo)) return VK_NULL_HANDLE;
        sig[i] = w->dstBinding << 8 | (uint32_t)w->descriptorType;
        h ^= sig[i]; h *= 0xff51afd7ed558ccdull; h ^= h >> 29;
    }
    for (unsigned probe = 0; probe < 8; ++probe) {
        DtEntry *e = &dt[(h + probe) & (DT_N - 1)];
        if (e->tmpl && e->layout == layout && e->n == (uint32_t)W->n && !memcmp(e->sig, sig, (size_t)W->n * sizeof *sig)) return e->tmpl;
        if (e->tmpl) continue;
        VkDescriptorUpdateTemplateEntry ent[DC_WRITES];
        for (int i = 0; i < W->n; ++i)
            ent[i] = (VkDescriptorUpdateTemplateEntry){.dstBinding = W->w[i].dstBinding, .dstArrayElement = 0, .descriptorCount = 1,
                .descriptorType = W->w[i].descriptorType, .offset = (size_t)i * sizeof(DtSlot), .stride = sizeof(DtSlot)};
        VkDescriptorUpdateTemplateCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO,
            .descriptorUpdateEntryCount = (uint32_t)W->n, .pDescriptorUpdateEntries = ent,
            .templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET, .descriptorSetLayout = layout};
        if (vkCreateDescriptorUpdateTemplate(nvk_dev, &ci, NULL, &e->tmpl) != VK_SUCCESS) { e->tmpl = VK_NULL_HANDLE; return VK_NULL_HANDLE; }
        e->layout = layout; e->n = (uint32_t)W->n; memcpy(e->sig, sig, (size_t)W->n * sizeof *sig); dt_created++;
        return e->tmpl;
    }
    return VK_NULL_HANDLE;   /* neighbourhood full: plain writes */
}
static void write_descriptors(Writes *W, VkDescriptorSet set, VkDescriptorSetLayout layout) {
    VkDescriptorUpdateTemplate t = dt_template(W, layout);
    if (t) {
        DtSlot slot[DC_WRITES];
        for (int i = 0; i < W->n; ++i) {
            if (W->w[i].pBufferInfo) slot[i].buffer = *W->w[i].pBufferInfo; else slot[i].image = *W->w[i].pImageInfo;
        }
        nvk_record_template_bytes((size_t)W->n * sizeof(DtSlot));
        vkUpdateDescriptorSetWithTemplate(nvk_dev, set, t, slot);
        dt_writes++;
        return;
    }
    for (int i = 0; i < W->n; ++i) W->w[i].dstSet = set;
    if (W->n) { vkUpdateDescriptorSets(nvk_dev, (uint32_t)W->n, W->w, 0, NULL); if (dt_on()) dt_fallback++; }
}
static uint64_t dc_key(const Writes *W, VkDescriptorSetLayout layout, uint64_t rec[DC_WRITES][4]) {
    dc_key_builds++;
    uint64_t h = (uint64_t)(uintptr_t)layout * 0x9e3779b97f4a7c15ull;
    for (int i = 0; i < W->n; ++i) {
        const VkWriteDescriptorSet *w = &W->w[i];
        rec[i][0] = w->dstBinding | (uint64_t)w->descriptorType << 32;
        if (w->pBufferInfo) { rec[i][1] = (uint64_t)(uintptr_t)w->pBufferInfo->buffer; rec[i][2] = w->pBufferInfo->offset; rec[i][3] = w->pBufferInfo->range; }
        else { rec[i][1] = (uint64_t)(uintptr_t)w->pImageInfo->imageView; rec[i][2] = (uint64_t)(uintptr_t)w->pImageInfo->sampler; rec[i][3] = w->pImageInfo->imageLayout; }
        for (int k = 0; k < 4; ++k) { h ^= rec[i][k]; h *= 0xff51afd7ed558ccdull; h ^= h >> 29; }
    }
    return h;
}
static VkDescriptorSet set_for(Writes *W, VkDescriptorSetLayout layout) {
    uint64_t fence = nvk_pending_fence(), rec[DC_WRITES][4], h = 0;
    int cache = dc_on() && W->n <= DC_WRITES;
    DescEntry *e = NULL;
    if (cache) {
        if (dc_recent_on()) {
            DescEntry *last = dc_recent[dc_recent_slot(layout)];
            if (last && last->fence == fence && last->layout == layout && last->n == (uint32_t)W->n && dc_matches(last, W)) {
                dc_hits++; dc_recent_hits++; return last->set;
            }
        }
        h = dc_key(W, layout, rec);
        unsigned ways = dc_ways(), base = (unsigned)h & (DC_N - 1);
        if (ways == 4) base &= ~3u;
        for (unsigned i = 0; i < ways; ++i) {
            DescEntry *candidate = &dc[base + i];
            if (candidate->fence == fence && candidate->hash == h && candidate->layout == layout && candidate->n == (uint32_t)W->n
                && !memcmp(candidate->rec, rec, (size_t)W->n * sizeof rec[0])) {
                if (dc_recent_on()) dc_recent[dc_recent_slot(layout)] = candidate;
                dc_hits++; return candidate->set;
            }
            if (!e && candidate->fence != fence) e = candidate;
        }
        if (!e) {
            e = &dc[base + (ways == 4 ? dc_victim[base / 4]++ & 3u : 0)];
            dc_evictions++;
        }
    }
    VkDescriptorSet set = nvk_alloc_set(layout);
    write_descriptors(W, set, layout);
    if (cache) { e->fence = fence; e->hash = h; e->layout = layout; e->set = set; e->n = (uint32_t)W->n; memcpy(e->rec, rec, (size_t)W->n * sizeof rec[0]); dc_misses++; }
    else dc_uncached++;
    if (cache && dc_recent_on()) dc_recent[dc_recent_slot(layout)] = e;
    return set;
}
/* The dynamic offsets of a set, in binding order. */
static uint32_t dyn_offsets(const Writes *W, const NvkShader *s, uint32_t *out) {
    for (unsigned k = 0; k < s->ndyn; ++k) out[k] = W->dynoff[s->dyn_order[k]];
    return s->ndyn;
}
static struct { uint64_t epoch; VkPipelineBindPoint point; VkDescriptorSetLayout layouts[2]; VkDescriptorSet sets[2]; uint32_t nsets, ndyn, dyn[32]; } bound;
static void bind_sets(VkCommandBuffer cb, VkPipelineBindPoint point, VkPipelineLayout pl, const VkDescriptorSetLayout *layouts,
                      const VkDescriptorSet *sets, uint32_t nsets, const uint32_t *dyn, uint32_t ndyn) {
    uint64_t epoch = nvk_cmd_epoch();
    if (dc_on() && bound.epoch == epoch && bound.point == point && bound.nsets == nsets && bound.ndyn == ndyn &&
        !memcmp(bound.layouts, layouts, nsets * sizeof *layouts) && !memcmp(bound.sets, sets, nsets * sizeof *sets) &&
        !memcmp(bound.dyn, dyn, ndyn * sizeof *dyn)) { binds_skipped++; return; }
    vkCmdBindDescriptorSets(cb, point, pl, 0, nsets, sets, ndyn, dyn);
    bound.epoch = epoch; bound.point = point; bound.nsets = nsets; bound.ndyn = ndyn;
    memcpy(bound.layouts, layouts, nsets * sizeof *layouts); memcpy(bound.sets, sets, nsets * sizeof *sets); memcpy(bound.dyn, dyn, ndyn * sizeof *dyn);
}
void nvk_desc_cache_stats(uint64_t v[4]) { v[0] = dc_hits; v[1] = dc_misses; v[2] = dc_uncached; v[3] = binds_skipped; }

/* ---------- Draws ---------- */
static VkPrimitiveTopology topology(int prim, int *cls) {
    switch (prim) {
    case NVN_DRAW_PRIMITIVE_POINTS: *cls = 0; return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case NVN_DRAW_PRIMITIVE_LINES: *cls = 1; return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case NVN_DRAW_PRIMITIVE_LINE_STRIP: *cls = 1; return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case NVN_DRAW_PRIMITIVE_TRIANGLES: case NVN_DRAW_PRIMITIVE_QUADS: *cls = 2; return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP: *cls = 2; return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_FAN: *cls = 2; return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    default: VKLOG("NVN primitive %d", prim); nvk_die("NVN primitive type not implemented");
    }
}

static VkBuffer quad_indices(void) {
    static VkBuffer b;
    if (!b) {
        VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 65536 * 24, .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT};
        VKCHECK(vkCreateBuffer(nvk_dev, &bci, NULL, &b), "create quad index buffer");
        VkMemoryRequirements req; vkGetBufferMemoryRequirements(nvk_dev, b, &req);
        VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
            .memoryTypeIndex = nvk_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        VkDeviceMemory m; VKCHECK(vkAllocateMemory(nvk_dev, &mai, NULL, &m), "allocate quad index buffer");
        VKCHECK(vkBindBufferMemory(nvk_dev, b, m, 0), "bind quad index buffer");
        uint32_t *ix; VKCHECK(vkMapMemory(nvk_dev, m, 0, VK_WHOLE_SIZE, 0, (void **)&ix), "map quad index buffer");
        for (uint32_t q = 0; q < 65536; ++q) { uint32_t v = q * 4; uint32_t six[6] = {v, v + 1, v + 2, v, v + 2, v + 3}; memcpy(ix + q * 6, six, 24); }
        vkUnmapMemory(nvk_dev, m);
    }
    return b;
}

static PFN_vkCmdSetVertexInputEXT set_vertex_input;
__attribute__((weak)) PFN_vkCmdSetVertexInputEXT nvk_record_vertex_input(PFN_vkCmdSetVertexInputEXT driver) { return driver; }   /* nvn_vk_record.c */
__attribute__((weak)) void nvk_record_template_bytes(size_t bytes) { (void)bytes; }

/* Per-submission shadow of what the command buffer already holds: identical
 * dynamic state, graphics pipeline, vertex buffers and vertex input layout
 * are not re-emitted (the driver re-emits every set state at the next draw).
 * Valid only within one command buffer. TOTK_VK_STATE_SHADOW=0: off. */
static int shadow_valid(uint64_t *fence) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_VK_STATE_SHADOW"); on = !(e && !strcmp(e, "0")); }
    uint64_t now = nvk_cmd_epoch();   /* per command buffer: a submission may span several (segments) */
    int valid = on && *fence == now;
    *fence = now;
    return valid;
}
static struct {
    uint64_t fence;
    uint32_t nb, na;
    struct { uint32_t binding, stride; VkVertexInputRate rate; } b[17];
    struct { uint32_t location, binding, offset; VkFormat format; } a[32];
    VkBuffer buf[17]; VkDeviceSize off[17];
} vi_last;
static uint64_t state_skipped;

static void vertex_input(VkCommandBuffer cb, const NvkShader *vs) {
    if (!set_vertex_input) set_vertex_input = nvk_record_vertex_input((PFN_vkCmdSetVertexInputEXT)vkGetDeviceProcAddr(nvk_dev, "vkCmdSetVertexInputEXT"));
    if (!set_vertex_input) nvk_die("vkCmdSetVertexInputEXT unavailable");
    VkVertexInputBindingDescription2EXT bd[17]; VkVertexInputAttributeDescription2EXT ad[32];
    uint32_t nb = 0, na = 0, used = 0;
    int valid = shadow_valid(&vi_last.fence);
    VkBuffer zero; nvk_zero_buffer(&zero);
    for (int i = 0; i < vs->nin; ++i) {
        unsigned loc = vs->in[i].loc;
        const va_st *a = loc < 16 && (int)loc < nvk_S.nattr ? &nvk_S.attr[loc] : NULL;
        VkFormat f = a && a->fmt ? vertex_format((int)a->fmt) : VK_FORMAT_UNDEFINED;
        if (a && a->fmt && f == VK_FORMAT_UNDEFINED) { VKLOG("NVN vertex format %#x", a->fmt); nvk_die("NVN vertex format not mapped"); }
        if (f == VK_FORMAT_UNDEFINED) {   /* disabled attribute: zeros from a stride-0 binding */
            static const VkFormat zf[3] = {VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_SINT};
            ad[na++] = (VkVertexInputAttributeDescription2EXT){.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT,
                .location = loc, .binding = 16, .format = zf[vs->in[i].cls], .offset = 0};
            used |= 1u << 16;
            continue;
        }
        ad[na++] = (VkVertexInputAttributeDescription2EXT){.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT,
            .location = loc, .binding = a->stream, .format = f, .offset = a->offset};
        used |= 1u << a->stream;
    }
    for (uint32_t s = 0; s < 17; ++s) if (used & (1u << s)) {
        if (s == 16) {
            bd[nb++] = (VkVertexInputBindingDescription2EXT){.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT,
                .binding = 16, .stride = 0, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX, .divisor = 1};
            if (!valid || vi_last.buf[16] != zero || vi_last.off[16]) {
                VkDeviceSize zo = 0; vkCmdBindVertexBuffers(cb, 16, 1, &zero, &zo);
                vi_last.buf[16] = zero; vi_last.off[16] = 0;
            }
            continue;
        }
        const vs_st *st = &nvk_S.stream[s];
        if (st->divisor > 1) { VKLOG("vertex stream %u divisor %u", s, st->divisor); nvk_die("instance divisors above 1 are not implemented"); }
        bd[nb++] = (VkVertexInputBindingDescription2EXT){.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT,
            .binding = s, .stride = st->stride, .inputRate = st->divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX, .divisor = 1};
        VkBuffer buf; uint64_t off;
        if (!nvk_S.vbuf[s].address || !nvk_resolve(nvk_S.vbuf[s].address, 1, &buf, &off)) { buf = zero; off = 0; }
        VkDeviceSize o = off;
        if (!valid || vi_last.buf[s] != buf || vi_last.off[s] != o) {
            vkCmdBindVertexBuffers(cb, s, 1, &buf, &o);
            vi_last.buf[s] = buf; vi_last.off[s] = o;
        }
    }
    int same = valid && vi_last.nb == nb && vi_last.na == na;
    for (uint32_t i = 0; same && i < nb; ++i)
        same = vi_last.b[i].binding == bd[i].binding && vi_last.b[i].stride == bd[i].stride && vi_last.b[i].rate == bd[i].inputRate;
    for (uint32_t i = 0; same && i < na; ++i)
        same = vi_last.a[i].location == ad[i].location && vi_last.a[i].binding == ad[i].binding && vi_last.a[i].offset == ad[i].offset && vi_last.a[i].format == ad[i].format;
    if (same) { state_skipped++; return; }
    set_vertex_input(cb, nb, bd, na, ad);
    vi_last.nb = nb; vi_last.na = na;
    for (uint32_t i = 0; i < nb; ++i) { vi_last.b[i].binding = bd[i].binding; vi_last.b[i].stride = bd[i].stride; vi_last.b[i].rate = bd[i].inputRate; }
    for (uint32_t i = 0; i < na; ++i) { vi_last.a[i].location = ad[i].location; vi_last.a[i].binding = ad[i].binding; vi_last.a[i].offset = ad[i].offset; vi_last.a[i].format = ad[i].format; }
}

typedef struct {
    VkViewport vp; VkRect2D sc; VkPrimitiveTopology topo; VkCullModeFlags cull; VkFrontFace front;
    VkBool32 bias_en; float bias[3]; float blend[4];
    VkBool32 dt, dw; VkCompareOp df; VkBool32 st;
    VkStencilOp sop[2][3]; VkCompareOp scmp[2]; uint32_t cmask[2], wmask[2], ref[2];
    VkBool32 restart;
} DynState;
static DynState dyn_last;
static uint64_t dyn_fence;
/* TOTK lights each material class with a full-screen pass that tests EQUAL against a 16-bit class depth written as
 * gl_FragDepth = 0.5 + class / 510 with depth test ALWAYS (ps 233dfb93, b117e55c, 5218c5e5). In 16-bit codes that is
 * 32767.5 + 128.5 * class: every even class (water 50, class 0) lies exactly on half a code, and whether the writer and
 * the light pass round it the same way is float noise (23 Oct, Thor, live: depth bias +0.5 on the light passes lit the
 * water but left Link unlit; without it Link was lit and the water black; even +0.1 flipped Link). Both sides are moved
 * a quarter code off the tie: the writer through a fragment variant adding TOTK_VK_FRAGDEPTH_EQUAL_BIAS (default 0.25)
 * codes to its gl_FragDepth (nvk_shader_depth_offset), the EQUAL draws on that buffer through the same depth bias.
 * 0 = off. Live "eqbias <constant>" (debugging) overrides the bias for all EQUAL draws on 16-bit depth, "eqbiaswriter
 * <prefix>" limits it to buffers from those writers. */
static float dbg_eqbias; static int dbg_depth16;
static float fragdepth_bias_now;   /* the bias for the current draw (0: none) */
static float fragdepth_equal_bias(void) {
    static float v = -1; static unsigned g;
    if (v < 0 || g != nvk_live_gen) { g = nvk_live_gen; const char *e = nvk_live_getenv("TOTK_VK_FRAGDEPTH_EQUAL_BIAS"); v = e && *e ? (float)atof(e) : 0.25f; if (v < 0) v = 0;
        VKLOG("EQUAL tests against gl_FragDepth-written 16-bit depth: bias %g%s", v, v ? "" : " (off)"); }
    return v;
}
static uint64_t eq_unbiased, fd_cleared, fd_evicted;   /* EQUAL draws on 16-bit depth without the bias; tracking entries cleared / evicted */
static const void *fd_mem[8]; static unsigned fd_next;   /* 16-bit depth buffers last written by a gl_FragDepth shader */
static char fd_writer[8][13];                             /* its fragment shader (log) */
/* Debugging ("eqbiaswriter <prefix>", several lines allowed): only depth from these writers gets the bias. */
static char dbg_eqw[8][33]; static unsigned dbg_neqw;
static int eqbias_writer_ok(const char *writer) {
    if (!dbg_neqw) return 1;
    for (unsigned k = 0; k < dbg_neqw; ++k) if (!strncmp(writer, dbg_eqw[k], strlen(dbg_eqw[k]))) return 1;
    return 0;
}
static uint64_t fragdepth_biased;
static void fragdepth_note(const NvkShader *ps, const p_set_rt *rt) {
    fragdepth_bias_now = 0;
    if (!rt->depth || rt->depth->d.format != NVN_FORMAT_DEPTH16 || !rt->depth->mem || !nvk_S.ds.dt) return;
    const void *m = rt->depth->mem; int i = 0;
    while (i < 8 && fd_mem[i] != m) ++i;
    if (nvk_S.ds.dw) {
        /* Only values written regardless of what is there (ALWAYS) are a computed class depth. A gl_FragDepth writer
           with an ordinary test (Link's pass, 23 Oct) passes its own depth on, and EQUAL against it already matches:
           there any bias, even +0.1, left Link unlit. */
        if (ps && ps->frag_depth && compare_op(nvk_S.ds.df) == VK_COMPARE_OP_ALWAYS) {
            if (i == 8) { i = fd_next++ % 8; if (fd_mem[i]) fd_evicted++; }
            fd_mem[i] = m; snprintf(fd_writer[i], sizeof fd_writer[i], "%.12s", ps->variant);
        } else if (i < 8) { fd_mem[i] = NULL; fd_cleared++; }
        return;
    }
    if (i == 8 && compare_op(nvk_S.ds.df) == VK_COMPARE_OP_EQUAL) eq_unbiased++;
    if (i < 8 && compare_op(nvk_S.ds.df) == VK_COMPARE_OP_EQUAL) {
        static struct { char ps[13], writer[13]; } seen[32]; static unsigned nseen;
        unsigned k = 0;
        while (k < nseen && (strncmp(seen[k].ps, ps ? ps->variant : "-", 12) || strcmp(seen[k].writer, fd_writer[i]))) ++k;
        if (k == nseen && nseen < 32) {
            snprintf(seen[k].ps, sizeof seen[k].ps, "%.12s", ps ? ps->variant : "-"); snprintf(seen[k].writer, sizeof seen[k].writer, "%s", fd_writer[i]); nseen++;
            VKLOG("EQUAL against gl_FragDepth-written 16-bit depth: ps %s on depth from ps %s (%ux%u)", seen[k].ps, seen[k].writer, rt->depth->d.width, rt->depth->d.height);
        }
        if (!eqbias_writer_ok(fd_writer[i])) return;
        fragdepth_bias_now = fragdepth_equal_bias();
        if (fragdepth_bias_now != 0 && !(fragdepth_biased++ % 100000))
            VKLOG("EQUAL tests against gl_FragDepth-written 16-bit depth: %llu draws biased (ps %.12s)", (unsigned long long)fragdepth_biased, ps ? ps->variant : "-");
    }
}

static void dynamic_state(VkCommandBuffer cb, VkPrimitiveTopology topo) {
    NvkState *S = &nvk_S;
    DynState n; memset(&n, 0, sizeof n);
    VkExtent2D e = nvk_render_extent();
    float x = 0, y = 0, w = (float)e.width, h = (float)e.height;
    if (S->has_vp) { x = S->vp[0][0]; y = S->vp[0][1]; w = S->vp[0][2]; h = S->vp[0][3]; }
    n.vp = (VkViewport){x, y + h, w, -h, S->dr[0], S->dr[1]};
    n.sc = (VkRect2D){{0, 0}, e};
    p_scissor ps;
    if (nvk_scissor(&ps)) {
        int32_t x0 = ps.x < 0 ? 0 : ps.x, y0 = ps.y < 0 ? 0 : ps.y, x1 = ps.x + ps.w, y1 = ps.y + ps.h;
        if (x1 > (int32_t)e.width) x1 = (int32_t)e.width;
        if (y1 > (int32_t)e.height) y1 = (int32_t)e.height;
        n.sc = (VkRect2D){{x0, y0}, {(uint32_t)(x1 > x0 ? x1 - x0 : 0), (uint32_t)(y1 > y0 ? y1 - y0 : 0)}};
    }
    n.topo = topo;
    n.cull = S->poly.cull == NVN_FACE_FRONT ? VK_CULL_MODE_FRONT_BIT : S->poly.cull == NVN_FACE_BACK ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    static int flip = -1;
    if (flip < 0) { const char *f = getenv("TOTK_VK_WINDING_FLIP"); flip = f && atoi(f); }
    int ccw = (S->poly.front == NVN_FRONT_FACE_CCW) != (flip != 0);
    n.front = ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    n.bias_en = (S->poly.offset_en & 4) != 0;
    n.bias[0] = S->po.units; n.bias[1] = S->po.clamp; n.bias[2] = S->po.factor;
    if (dbg_eqbias != 0 && dbg_depth16 && !n.bias_en && S->ds.dt && compare_op(S->ds.df) == VK_COMPARE_OP_EQUAL) {
        n.bias_en = VK_TRUE; n.bias[0] = dbg_eqbias; n.bias[1] = 0; n.bias[2] = 0;
    } else if (fragdepth_bias_now != 0 && !n.bias_en) { n.bias_en = VK_TRUE; n.bias[0] = fragdepth_bias_now; n.bias[1] = 0; n.bias[2] = 0; }
    memcpy(n.blend, S->blend_color, sizeof n.blend);
    n.dt = S->ds.dt; n.dw = S->ds.dw; n.df = compare_op(S->ds.df); n.st = S->ds.st;
    n.sop[0][0] = stencil_op(S->ds.fail_f); n.sop[0][1] = stencil_op(S->ds.zpass_f); n.sop[0][2] = stencil_op(S->ds.zfail_f); n.scmp[0] = compare_op(S->ds.sf_f);
    n.sop[1][0] = stencil_op(S->ds.fail_b); n.sop[1][1] = stencil_op(S->ds.zpass_b); n.sop[1][2] = stencil_op(S->ds.zfail_b); n.scmp[1] = compare_op(S->ds.sf_b);
    for (int f = 0; f < 2; ++f) { n.cmask[f] = S->st_vmask[f]; n.wmask[f] = S->st_wmask[f]; n.ref[f] = S->st_ref[f]; }
    n.restart = topo == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP || topo == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP || topo == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    int valid = shadow_valid(&dyn_fence);
    if (valid && !memcmp(&n, &dyn_last, sizeof n)) { state_skipped++; return; }
#define CHANGED(f) (!valid || memcmp(&n.f, &dyn_last.f, sizeof n.f))
    if (CHANGED(vp)) vkCmdSetViewport(cb, 0, 1, &n.vp);
    if (CHANGED(sc)) vkCmdSetScissor(cb, 0, 1, &n.sc);
    if (CHANGED(topo)) vkCmdSetPrimitiveTopology(cb, n.topo);
    if (CHANGED(cull)) vkCmdSetCullMode(cb, n.cull);
    if (CHANGED(front)) vkCmdSetFrontFace(cb, n.front);
    if (CHANGED(bias_en)) vkCmdSetDepthBiasEnable(cb, n.bias_en);
    if (CHANGED(bias)) vkCmdSetDepthBias(cb, n.bias[0], n.bias[1], n.bias[2]);
    if (CHANGED(blend)) vkCmdSetBlendConstants(cb, n.blend);
    if (CHANGED(dt)) vkCmdSetDepthTestEnable(cb, n.dt);
    if (CHANGED(dw)) vkCmdSetDepthWriteEnable(cb, n.dw);
    if (CHANGED(df)) vkCmdSetDepthCompareOp(cb, n.df);
    if (!valid) { vkCmdSetDepthBoundsTestEnable(cb, VK_FALSE); vkCmdSetRasterizerDiscardEnable(cb, VK_FALSE); }
    if (CHANGED(st)) vkCmdSetStencilTestEnable(cb, n.st);
    if (CHANGED(sop) || CHANGED(scmp)) {
        vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_BIT, n.sop[0][0], n.sop[0][1], n.sop[0][2], n.scmp[0]);
        vkCmdSetStencilOp(cb, VK_STENCIL_FACE_BACK_BIT, n.sop[1][0], n.sop[1][1], n.sop[1][2], n.scmp[1]);
    }
    if (CHANGED(cmask)) { vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_BIT, n.cmask[0]); vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_BACK_BIT, n.cmask[1]); }
    if (CHANGED(wmask)) { vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_BIT, n.wmask[0]); vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_BACK_BIT, n.wmask[1]); }
    if (CHANGED(ref)) { vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_BIT, n.ref[0]); vkCmdSetStencilReference(cb, VK_STENCIL_FACE_BACK_BIT, n.ref[1]); }
    if (CHANGED(restart)) vkCmdSetPrimitiveRestartEnable(cb, n.restart);
#undef CHANGED
    dyn_last = n;
}

static uint64_t draws, skipped[8];
/* Draw and skip totals for the black-frame detector in nvn_vk_present.c: out = total draws, then skipped[0..7]. */
void nvk_draw_totals(uint64_t out[12]) { out[0] = draws; for (int i = 0; i < 8; ++i) out[i + 1] = skipped[i]; out[9] = eq_unbiased; out[10] = fd_cleared; out[11] = fd_evicted; }
uint64_t nvk_draws_issued(void) { return draws; }
uint64_t nvk_state_skipped(void) { return state_skipped; }

/* Live shader bisection (debugging): TOTK_SHADER_DEBUG names a control file read every 30 frames.
   "dump" logs the fragment shaders (hash, draws) of the next frame; each "skip <hash prefix>" line skips the draws
   whose fragment shader starts with that prefix. Rewrite the file to change it while the game runs. */
static char dbg_skip[512][33]; static unsigned dbg_nskip; static int dbg_dump;
/* Live switches: up to 32 "set NAME=VALUE" overrides, swapped in whole (other threads read them). */
typedef struct { unsigned n; char name[32][48], value[32][24]; } LiveSet;
static LiveSet *_Atomic live_set;
unsigned nvk_live_gen;
const char *nvk_live_getenv(const char *name) {
    const LiveSet *l = atomic_load_explicit(&live_set, memory_order_acquire);
    for (unsigned i = 0; l && i < l->n; ++i) if (!strcmp(l->name[i], name)) return l->value[i];
    return getenv(name);
}
static char dbg_trace[33], dbg_trace2[33]; static unsigned dbg_trace_left;   /* "trace2 <prefix>": a second prefix traced alongside */
/* "trace <hash prefix>": log the bindings of the next 6 draws whose fragment or vertex shader has it */
static struct { char hash[33]; unsigned draws; } dbg_seen[1024]; static unsigned dbg_nseen;
void nvk_shader_debug_frame(uint64_t frame) {
    static const char *path; static int init; static char last[4096];
    if (!init) { init = 1; path = getenv("TOTK_SHADER_DEBUG"); }
    if (dbg_dump == 2) {   /* the dumped frame ended */
        VKLOG("shader debug: %u fragment shaders in frame %llu", dbg_nseen, (unsigned long long)frame);
        for (unsigned i = 0; i < dbg_nseen; ++i) VKLOG("shader debug: ps %s draws %u", dbg_seen[i].hash, dbg_seen[i].draws);
        dbg_dump = 0; dbg_nseen = 0;
    } else if (dbg_dump == 1) dbg_dump = 2;
    if (dbg_watch_state == 2) { VKLOG("bufwatch: frame %llu ended (%u uses)", (unsigned long long)frame, dbg_watch_seq); dbg_watch_state = 0; }
    else if (dbg_watch_state == 1) { dbg_watch_state = 2; dbg_watch_seq = 0; VKLOG("bufwatch: watching %#llx during frame %llu", (unsigned long long)dbg_watch_addr, (unsigned long long)frame); }
    if (!path || frame % 30) return;
    FILE *f = fopen(path, "r"); if (!f) return;
    char text[4096]; size_t len = fread(text, 1, sizeof text - 1, f); fclose(f); text[len] = 0;
    if (!strcmp(text, last)) return;   /* unchanged since the last read */
    memcpy(last, text, len + 1);
    char line[128]; unsigned n = 0, nw = 0; int dump = 0; float eqbias = 0; char eqw[8][33]; unsigned neqw = 0;
    LiveSet *ls = calloc(1, sizeof *ls);
    dbg_ndumpubo = 0;
    for (char *p = text, *nl; *p; p = nl ? nl + 1 : p + strlen(p)) {
        nl = strchr(p, '\n'); size_t l = nl ? (size_t)(nl - p) : strlen(p); if (l >= sizeof line) l = sizeof line - 1;
        memcpy(line, p, l); line[l] = 0;
        char h[33];
        if (!strncmp(line, "dumptex", 7)) sscanf(line, "dumptex %d", &dbg_dumptex_binding);
        else if (!strncmp(line, "dumpubo", 7)) { int stage; unsigned binding; if (sscanf(line, "dumpubo %d %u", &stage, &binding) == 2 && stage >= 0 && stage < 2 && dbg_ndumpubo < 8) dbg_dumpubo[dbg_ndumpubo++] = (typeof(dbg_dumpubo[0])){stage, binding}; }
        else if (!strncmp(line, "bufwatch", 8)) { unsigned long long a; if (sscanf(line, "bufwatch %llx", &a) == 1) { dbg_watch_addr = a; dbg_watch_state = 1; } }
        else if (!strncmp(line, "dumpdepth", 9)) sscanf(line, "dumpdepth %d", &dbg_dumpdepth);
        else if (!strncmp(line, "reupload", 8)) nvk_image_reupload_all();
        else if (!strncmp(line, "set ", 4) && ls && ls->n < 32) {
            char *eq = strchr(line + 4, '=');
            if (eq && eq - (line + 4) < 48) { snprintf(ls->name[ls->n], 48, "%.*s", (int)(eq - (line + 4)), line + 4); snprintf(ls->value[ls->n], 24, "%s", eq + 1); ls->n++; }
        }
        else if (sscanf(line, "eqbiaswriter %32s", h) == 1 && neqw < 8) snprintf(eqw[neqw++], 33, "%s", h);
        else if (!strncmp(line, "eqbias", 6)) { float v; if (sscanf(line, "eqbias %f", &v) == 1) { eqbias = v; VKLOG("shader debug: EQUAL draws on 16-bit depth get depth bias %g", v); } }
        else if (!strncmp(line, "copybarrier", 11)) { int v; if (sscanf(line, "copybarrier %d", &v) == 1) { nvk_copy_barrier_narrow = v != 0; VKLOG("copy barriers: %s", v ? "narrow" : "full"); } }
        else if (!strncmp(line, "dumpbufafter", 12)) sscanf(line, "dumpbufafter %d", &dbg_dumpafter_slot);
        else if (!strncmp(line, "dumpbuf", 7)) sscanf(line, "dumpbuf %d", &dbg_dumpbuf_slot);
        else if (!strncmp(line, "dump", 4)) dump = 1;
        else if (sscanf(line, "skip %32s", h) == 1 && n < 512) snprintf(dbg_skip[n++], 33, "%s", h);
        else if (sscanf(line, "trace2 %32s", h) == 1) snprintf(dbg_trace2, 33, "%s", h);
        else if (sscanf(line, "trace %32s", h) == 1) { snprintf(dbg_trace, 33, "%s", h); dbg_trace2[0] = 0; dbg_trace_left = 6; }
        else if (sscanf(line, "writerafter %32s", h) == 1) snprintf(dbg_writer_after, 33, "%s", h);
        else if (sscanf(line, "writer %d %u %u", &dbg_writer_fmt, &dbg_writer_w, &dbg_writer_h) == 3) dbg_writer_left = 400;
        else { int bnd; if (sscanf(line, "white %32s %d", h, &bnd) == 2 && nw < 8) { snprintf(dbg_white[nw].hash, 33, "%s", h); dbg_white[nw++].binding = bnd; } }
        if (!nl) break;
    }
    dbg_nskip = n; dbg_nwhite = nw; dbg_eqbias = eqbias; memcpy(dbg_eqw, eqw, sizeof eqw); dbg_neqw = neqw;
    {   /* the previous set stays allocated: another thread may still read it (a few hundred bytes per change) */
        const LiveSet *old = atomic_load_explicit(&live_set, memory_order_relaxed);
        if (ls && (old ? old->n : 0) + ls->n) {
            atomic_store_explicit(&live_set, ls, memory_order_release); __atomic_add_fetch(&nvk_live_gen, 1, __ATOMIC_RELEASE);
            for (unsigned i = 0; i < ls->n; ++i) VKLOG("shader debug: live switch %s=%s", ls->name[i], ls->value[i]);
            if (!ls->n) VKLOG("shader debug: live switches cleared (environment again)");
        } else free(ls);
    }
    if (dump) { dbg_dump = 1; dbg_nseen = 0; }
    VKLOG("shader debug: %u skip prefixes, %u white bindings%s", n, nw, dump ? ", dumping the next frame" : "");
}
int nvk_dump_frame_active(void) { return dbg_dump == 2; }
static int dbg_skip_draw(const NvkShader *ps) {
    if (!ps) return 0;
    if (dbg_dump == 2) {
        unsigned i = 0; while (i < dbg_nseen && strncmp(dbg_seen[i].hash, ps->variant, 32)) ++i;
        if (i == dbg_nseen && dbg_nseen < 1024) { snprintf(dbg_seen[i].hash, 33, "%.32s", ps->variant); dbg_seen[i].draws = 0; dbg_nseen++; }
        if (i < dbg_nseen) dbg_seen[i].draws++;
    }
    for (unsigned i = 0; i < dbg_nskip; ++i) if (!strncmp(ps->variant, dbg_skip[i], strlen(dbg_skip[i]))) return 1;
    return 0;
}

/* CPU time per draw by phase (TOTK_VK_CPU_PHASES=1, off by default; costs ~6 clock reads per draw): shader lookup,
   textures (fetch choice, prepare, splits), pipeline (key and lookup), descriptors (write, cache, sets) and recording.
   Reported per frame with the split diagnostics. */
enum { PH_SHADERS, PH_TEXTURES, PH_PIPELINE, PH_DESCRIPTORS, PH_RECORD, PH_N };
static int ph_on = -1; static uint64_t ph_last, ph_ns[PH_N], ph_total, ph_draws;
static inline void ph(int k) { if (ph_on > 0) { uint64_t t = nvk_now_ns(); ph_ns[k] += t - ph_last; ph_last = t; } }
static void draw_impl(const p_draw *dw, const p_draw_idx *di);
void nvk_draw(const p_draw *dw, const p_draw_idx *di) {
    if (ph_on < 0) { const char *e = getenv("TOTK_VK_CPU_PHASES"); ph_on = e && !strcmp(e, "1"); if (ph_on) VKLOG("CPU time per draw phase: on (TOTK_VK_CPU_PHASES=1)"); }
    if (ph_on <= 0) { draw_impl(dw, di); return; }
    uint64_t t0 = nvk_now_ns(); ph_last = t0;
    draw_impl(dw, di);
    ph_total += nvk_now_ns() - t0; ph_draws++;
}
/* Pipeline key formats of the render targets: the same for every draw of an open pass on the same target serials
   (nvk_image of an attached target is a lookup only), so they are kept per pass. TOTK_VK_KEY_CACHE=0: looked up per draw. */
static int key_cache_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_VK_KEY_CACHE"); on = !(e && !strcmp(e, "0")); }
    return on;
}
static struct { uint64_t pass, serial[9]; VkFormat color[8], depth, stencil; } kc;
static uint64_t kc_hits, kc_misses;
static void key_formats(PipeKey *k, const p_set_rt *rt, VkCommandBuffer cb) {
    uint64_t pass = key_cache_on() ? nvk_open_pass_serial() : 0, ser[9] = {0};
    for (int i = 0; i < rt->n && i < 8; ++i) ser[i] = rt->color[i] ? rt->color[i]->serial : 0;
    ser[8] = rt->depth ? rt->depth->serial : 0;
    if (pass && kc.pass == pass && !memcmp(kc.serial, ser, sizeof ser)) {
        memcpy(k->color, kc.color, sizeof k->color); k->depth = kc.depth; k->stencil = kc.stencil; kc_hits++;
        return;
    }
    for (int i = 0; i < rt->n; ++i) if (rt->color[i]) {
        NvkImage *img = nvk_image(rt->color[i], cb);
        k->color[i] = nvk_image_format(img);
        if (rt->has_cview[i] && rt->cview[i].set_format) {
            NvnVkFormat f;
            if (!nvn_vk_format_map((uint32_t)rt->cview[i].format, &f)) nvk_die("render-target view format has no Vulkan format");
            k->color[i] = f.format;
        }
    }
    if (rt->depth) {
        NvkImage *img = nvk_image(rt->depth, cb);
        VkImageAspectFlags asp = nvk_image_aspect(img);
        if (asp & VK_IMAGE_ASPECT_DEPTH_BIT) k->depth = nvk_image_format(img);
        if (asp & VK_IMAGE_ASPECT_STENCIL_BIT) k->stencil = nvk_image_format(img);
    }
    kc_misses++;
    if (pass) { kc.pass = pass; memcpy(kc.serial, ser, sizeof ser); memcpy(kc.color, k->color, sizeof kc.color); kc.depth = k->depth; kc.stencil = k->stencil; }
}
static void draw_impl(const p_draw *dw, const p_draw_idx *di) {
    if (!state_ready) state_reset();
    NvkState *S = &nvk_S;
    if (!S->prog) { skipped[0]++; return; }
    if (S->render_disabled || S->raster_discard) { skipped[1]++; return; }
    if (S->poly.cull == NVN_FACE_FRONT_AND_BACK) { skipped[2]++; return; }
    int count = dw ? dw->count : di->count;
    if (count <= 0) return;
    const p_set_rt *rt = nvk_render_targets();
    if (!rt) nvk_die("NVN draw without render targets");
    if (S->color.alpha_func && S->color.alpha_func != NVN_ALPHA_FUNC_ALWAYS) {
        static int said; if (!said++) VKLOG("alpha test %u is not implemented (drawn without it, as on Metal)", S->color.alpha_func);
    }
    NvkShader *vs = NULL, *ps = NULL; int missing = 0;
    for (unsigned i = 0; i < S->prog->nstage; ++i) {
        int st; NvkShader *s = nvk_shader_for(S->prog, i, &st);
        if (!s) missing = 1;   /* no seeded, cached or bridge translation (nvn_vk_shader.c logs it) */
        if (st == 0) vs = s; else if (st == 1) ps = s;
    }
    if (missing) { skipped[4]++; return; }
    if (!vs) nvk_die("NVN draw without a vertex shader");
    ph(PH_SHADERS);
    if (dbg_dump == 2) {   /* "dump": the draw list of that frame, in order */
        static unsigned n; if (!n || dbg_nseen == 0) n = 0;
        const tex_t *c0 = rt->n > 0 ? rt->color[0] : NULL;
        VKLOG("frame draw %u: ps %.8s vs %.8s count %d -> %ux%u serial %llu fmt %d%s", n++, ps ? ps->variant : "-", vs->variant, count, c0 ? c0->d.width : 0, c0 ? c0->d.height : 0,
            c0 ? (unsigned long long)c0->serial : 0ull, c0 ? (int)c0->d.format : -1, rt->depth ? " +depth" : "");
    }
    if ((dbg_nskip || dbg_dump) && dbg_skip_draw(ps)) return;
    dbg_tracing = dbg_trace_left && ((ps && !strncmp(ps->variant, dbg_trace, strlen(dbg_trace))) || !strncmp(vs->variant, dbg_trace, strlen(dbg_trace)) ||
        (dbg_trace2[0] && ((ps && !strncmp(ps->variant, dbg_trace2, strlen(dbg_trace2))) || !strncmp(vs->variant, dbg_trace2, strlen(dbg_trace2)))));
    int writer = 0;
    if (dbg_writer_left && dbg_writer_after[0] && ps && !strncmp(ps->variant, dbg_writer_after, strlen(dbg_writer_after))) { dbg_writer_after[0] = 0; VKLOG("trace: writer armed after ps %.12s", ps->variant); }
    if (dbg_writer_left && !dbg_writer_after[0]) for (int i = 0; i < rt->n; ++i) if (rt->color[i] && (int)rt->color[i]->d.format == dbg_writer_fmt &&
        rt->color[i]->d.width == dbg_writer_w && rt->color[i]->d.height == dbg_writer_h) writer = 1;
    if (writer) {
        dbg_writer_left--;
        for (int i = 0; i < rt->n; ++i) if (rt->color[i] && (int)rt->color[i]->d.format == dbg_writer_fmt && rt->color[i]->d.width == dbg_writer_w && rt->color[i]->d.height == dbg_writer_h)
            VKLOG("trace: writer draw ps %.44s vs %.12s target %d serial %llu (%d targets) count %d inst %d, depth fmt %d test %d func %d write %d, blend %d chmask %#x",
                ps ? ps->variant : "-", vs->variant, i, (unsigned long long)rt->color[i]->serial, rt->n, count, dw ? (int)dw->instances : (int)di->instances,
                rt->depth ? (int)rt->depth->d.format : -1, S->ds.dt, S->ds.df, S->ds.dw, (S->color.blend_en >> i) & 1, (S->chmask.mask >> (4 * i)) & 0xf);
    }
    if (dbg_tracing) dbg_trace_left--;
    if (dbg_tracing) {
        VKLOG("trace: draw ps %s vs %s, %d color targets%s, blend %#x", ps ? ps->variant : "-", vs->variant, rt->n, rt->depth ? " + depth" : "", (unsigned)S->color.blend_en);
        for (int i = 0; i < rt->n; ++i) if (rt->color[i])
            VKLOG("trace:   target %d serial %llu fmt %d %ux%u, view fmt %d, channel mask %#x, blend %d (out %d)", i, (unsigned long long)rt->color[i]->serial, (int)rt->color[i]->d.format,
                rt->color[i]->d.width, rt->color[i]->d.height, rt->has_cview[i] && rt->cview[i].set_format ? (int)rt->cview[i].format : -1, (S->chmask.mask >> (4 * i)) & 0xf,
                (S->color.blend_en >> i) & 1, ps ? (ps->out_mask >> i) & 1 : -1);
        if (rt->depth) VKLOG("trace:   depth serial %llu fmt %d %ux%u mem %p image %p (draw %llu)", (unsigned long long)rt->depth->serial, (int)rt->depth->d.format,
            rt->depth->d.width, rt->depth->d.height, rt->depth->mem, (void *)nvk_image_existing(rt->depth->serial), (unsigned long long)draws);
        if (dbg_dumpdepth == 1 && rt->depth) { NvkImage *di_ = nvk_image_existing(rt->depth->serial); if (di_) nvk_debug_dump_image(di_, rt->depth->serial); dbg_dumpdepth = 0; }
        VKLOG("trace:   depth test %d write %d func %d, stencil test %d func %d ref %d; depth range %g..%g", S->ds.dt, S->ds.dw, S->ds.df, S->ds.st, S->ds.sf_f,
            (int)S->st_ref[0], S->dr[0], S->dr[1]);
    }
    if (ps && (ps->in_mask & ~vs->out_locs) && !(ps = nvk_shader_zero_inputs(ps, ps->in_mask & ~vs->out_locs))) { skipped[4]++; return; }
    if (ps && ps->nfetch && fetch_on()) { NvkShader *f = fetch_choose(ps, vs, rt);   /* also without a proven vertex shader: counted */ if (f) { ps = f; fetch_draws++; } }
    VkCommandBuffer cb = nvk_cmd();
    draw_writes_now = draw_writes(ps, rt); draw_sampled_now = 0; draw_storage_now = draw_image_store_now = 0; draw_nwr = 0;
    tc_token++;
    vertex_storage_split(vs);
    prepare_textures(vs, 0);
    prepare_textures(ps, 1);
    nvk_pass_before_draw(draw_writes_now);
    nvk_segment_checkpoint();   /* can only cut an already closed pass; discard the old local cb */
    cb = nvk_cmd();   /* preparation, prediction or a checkpoint may have started a new command buffer */
    ph(PH_TEXTURES);
    if (dbg_nwhite && !nvk_white_view_ready()) { nvk_rendering_end(NVK_END_TEXTURE); nvk_white_view(cb); }
    int cls; VkPrimitiveTopology topo = topology(dw ? dw->prim : di->prim, &cls);
    /* Pipeline key from the render targets and bound state */
    PipeKey k; memset(&k, 0, sizeof k);
    if (ps && ps->frag_depth && S->ds.dt && S->ds.dw && compare_op(S->ds.df) == VK_COMPARE_OP_ALWAYS && rt->depth &&
        rt->depth->d.format == NVN_FORMAT_DEPTH16 && fragdepth_equal_bias() != 0) {
        NvkShader *z = nvk_shader_depth_offset(ps, fragdepth_equal_bias() / 65535.0f);
        if (z) ps = z;
    }
    k.vs = vs; k.ps = ps; k.topo_class = (uint8_t)cls;
    k.ncolor = (uint8_t)rt->n;
    key_formats(&k, rt, cb);
    k.blend_en = (uint8_t)S->color.blend_en;
    for (int i = 0; i < k.ncolor; ++i) if ((k.blend_en >> i) & 1) { k.blend[i] = S->blend[i]; k.blend[i].target = 0; }
    k.chmask = S->chmask.mask;
    for (int i = k.ncolor; i < 8; ++i) k.chmask &= ~(0xfu << (4 * i));
    if (unwritten_masked()) for (int i = 0; i < k.ncolor; ++i) if (!ps || !(ps->out_mask >> i & 1)) k.chmask &= ~(0xfu << (4 * i));
    k.polygon = (uint8_t)S->poly.mode; k.depth_clamp = (uint8_t)(S->depth_clamp != 0); k.a2c = S->ms.a2c;
    if (S->color.logic_op != NVN_LOGIC_OP_COPY) { k.logic = 1; k.logic_op = (uint8_t)S->color.logic_op; }
    Pipe *p = pipeline_for_draw(&k);
    ph(PH_PIPELINE);
    if (!p) { skipped[6]++; return; }   /* compiling in the background */
    VkDescriptorSet sets[2];
    /* vkUpdateDescriptorSets consumes these arrays before it returns, so keep
     * the scratch on this thread's stack instead of allocating it per draw. */
    Writes W;
    VkDescriptorSetLayout layouts[2] = {vs->layout, ps ? ps->layout : empty_set_layout()};
    uint32_t dyn[32], ndyn;
    if (dbg_watch_state == 2) snprintf(dbg_watch_what, sizeof dbg_watch_what, "draw %llu vs %.12s ps %.12s count %d inst %u, %d targets %ux%u", (unsigned long long)draws,
        vs->variant, ps ? ps->variant : "-", count, dw ? (unsigned)dw->instances : (unsigned)di->instances, rt->n, rt->n && rt->color[0] ? rt->color[0]->d.width : 0, rt->n && rt->color[0] ? rt->color[0]->d.height : 0);
    write_set(&W, vs, 0); sets[0] = set_for(&W, layouts[0]); ndyn = dyn_offsets(&W, vs, dyn);
    if (ps) { write_set(&W, ps, 1); sets[1] = set_for(&W, layouts[1]); ndyn += dyn_offsets(&W, ps, dyn + ndyn); }
    else { W.n = 0; sets[1] = set_for(&W, layouts[1]); }
    ph(PH_DESCRIPTORS);
    nvk_pass_access(draw_writes_now, (uint16_t)((rt->depth && (S->ds.dt || S->ds.st) ? 0x100 : 0) | fetch_attachments(ps)));
    nvk_rendering_begin();
    if (draw_storage_now) { nvk_pass_storage_write(draw_image_store_now); for (unsigned i = 0; i < draw_nwr; ++i) nvk_pass_storage_range(draw_wr[i].a, draw_wr[i].n); }
    nvk_pass_local_read_barrier(fetch_attachments(ps));
    nvk_pass_label(ps ? ps->variant : NULL);
    static uint64_t pipe_fence; static VkPipeline pipe_last;
    if (!shadow_valid(&pipe_fence) || pipe_last != p->pipe) { vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipe); pipe_last = p->pipe; }
    bind_sets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, layouts, sets, 2, dyn, ndyn);
    dbg_depth16 = rt->depth && rt->depth->d.format == NVN_FORMAT_DEPTH16;
    fragdepth_note(ps, rt);
    dynamic_state(cb, topo);
    vertex_input(cb, vs);
    if (dw) {
        uint32_t inst = dw->instances > 1 ? (uint32_t)dw->instances : 1;
        if (dw->prim == NVN_DRAW_PRIMITIVE_QUADS) {
            if (count < 4) { finish_copy_back(cb); return; }
            vkCmdBindIndexBuffer(cb, quad_indices(), 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cb, (uint32_t)(count / 4) * 6, inst, 0, dw->first, (uint32_t)dw->base_instance);
        } else vkCmdDraw(cb, (uint32_t)count, inst, (uint32_t)dw->first, (uint32_t)dw->base_instance);
    } else {
        if (di->prim == NVN_DRAW_PRIMITIVE_QUADS) nvk_die("indexed NVN quads are not implemented");
        VkBuffer ib; uint64_t off;
        if (!nvk_resolve(di->ibuf, 1, &ib, &off)) { static int said; if (said++ < 4) VKLOG("index buffer %#llx outside memory pools: draw skipped", (unsigned long long)di->ibuf); skipped[3]++; finish_copy_back(cb); return; }
        VkIndexType it = di->itype == 0 ? VK_INDEX_TYPE_UINT8_EXT : di->itype == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
        vkCmdBindIndexBuffer(cb, ib, off, it);
        vkCmdDrawIndexed(cb, (uint32_t)count, di->instances ? (uint32_t)di->instances : 1, 0, di->base_vertex, (uint32_t)di->base_instance);
    }
    finish_copy_back(cb);
    nvk_pass_after_draw(draw_writes_now, draw_sampled_now);
    ph(PH_RECORD);
    if (dbg_after_addr) { nvk_debug_dump_buffer(dbg_after_addr, dbg_after_size); dbg_after_addr = 0; }
    if (dbg_tracing && dbg_dumpdepth == 2 && rt->depth) { NvkImage *di_ = nvk_image_existing(rt->depth->serial); if (di_) nvk_debug_dump_image(di_, rt->depth->serial); dbg_dumpdepth = 0; }
    if (++draws == 1 || draws % 100000 == 0)
        VKLOG("draws %llu (skipped: no program %llu, disabled %llu, culled %llu, index %llu, no shader %llu, pipeline compiling %llu)", (unsigned long long)draws,
            (unsigned long long)skipped[0], (unsigned long long)skipped[1], (unsigned long long)skipped[2], (unsigned long long)skipped[3],
            (unsigned long long)skipped[4], (unsigned long long)skipped[6]);
}

void nvk_dispatch(const p_dispatch *d) {
    dispatch_grid[0] = d->x; dispatch_grid[1] = d->y; dispatch_grid[2] = d->z;
    if (!state_ready) state_reset();
    NvkState *S = &nvk_S;
    if (!S->prog) return;
    NvkShader *cs = NULL;
    int missing = 0;
    for (unsigned i = 0; i < S->prog->nstage; ++i) { int st; NvkShader *s = nvk_shader_for(S->prog, i, &st); if (!s) missing = 1; if (st == 5) cs = s; }
    if (missing) { skipped[5]++; return; }   /* untranslatable without a bridge: logged by nvn_vk_shader.c */
    if (!cs) nvk_die("NVN dispatch without a compute shader");
    nvk_rendering_end(NVK_END_DISPATCH);
    tc_token++;
    prepare_textures(cs, 5);
    nvk_segment_checkpoint();
    VkCommandBuffer cb = nvk_cmd();
    PipeKey k; memset(&k, 0, sizeof k); k.cs = cs;
    Pipe *p = pipeline(&k);
    Writes W;
    if (dbg_watch_state == 2) snprintf(dbg_watch_what, sizeof dbg_watch_what, "dispatch cs %.12s grid %u %u %u", cs->variant, d->x, d->y, d->z);
    write_set(&W, cs, 5);
    VkDescriptorSet set = set_for(&W, cs->layout);
    uint32_t dyn[16], ndyn = dyn_offsets(&W, cs, dyn);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p->pipe);
    bind_sets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p->layout, &cs->layout, &set, 1, dyn, ndyn);
    if (d->x > 0 && d->y > 0 && d->z > 0) vkCmdDispatch(cb, (uint32_t)d->x, (uint32_t)d->y, (uint32_t)d->z);
    nvk_barrier(cb);
    finish_copy_back(cb);
}

/* ---------- Every 300 frames: why passes split, depth fetch, key cache, CPU phases (nvn_vk_backend.c pass_stats) ---------- */
static int spl_by_total(const void *a, const void *b) {
    const typeof(spl_tab[0]) *x = a, *y = b;
    uint64_t tx = x->n[0] + x->n[1] + x->n[2] + x->n[3], ty = y->n[0] + y->n[1] + y->n[2] + y->n[3];
    return (tx < ty) - (tx > ty);
}
void nvk_split_report(uint64_t f0, uint64_t f, double nf) {
    static uint64_t t0[SPL_N];
    double tot = 0;
    for (int c = 0; c < SPL_N; ++c) tot += (double)(spl_total[c] - t0[c]);
    if (tot > 0) {
        static typeof(spl_tab[0]) c[128];
        memcpy(c, spl_tab, sizeof c); memset(spl_tab, 0, sizeof spl_tab);
        qsort(c, 128, sizeof *c, spl_by_total);
        char top[640]; size_t o = 0; top[0] = 0;
        for (int i = 0; i < 8 && c[i].name[0] && o < sizeof top - 96; ++i) {
            uint64_t n = c[i].n[0] + c[i].n[1] + c[i].n[2] + c[i].n[3];
            const char *why = c[i].n[SPL_DEPTH] * 2 >= n ? "depth" : c[i].n[SPL_COLOUR] * 2 >= n ? "colour" : c[i].n[SPL_BOTH] * 2 >= n ? "both" : "mixed/other";
            o += (size_t)snprintf(top + o, sizeof top - o, "%s%s %s %.1f (%s%s)", i ? ", " : "", c[i].name, c[i].stage == 1 ? "ps" : c[i].stage == 0 ? "vs" : "cs",
                n / nf, why, c[i].fetch ? ", fetch variant" : c[i].proof ? ", has fetch proof" : ", no fetch proof");
        }
        VKLOG("frames %llu-%llu: feedback splits per frame by what the pass wrote of the sampled slot: depth %.1f, colour %.1f, both %.1f, neither (store or self) %.1f; top: %s%s",
            (unsigned long long)f0, (unsigned long long)f, (spl_total[SPL_DEPTH] - t0[SPL_DEPTH]) / nf, (spl_total[SPL_COLOUR] - t0[SPL_COLOUR]) / nf,
            (spl_total[SPL_BOTH] - t0[SPL_BOTH]) / nf, (spl_total[SPL_OTHER] - t0[SPL_OTHER]) / nf, top, spl_lost ? " (table full, some shaders not listed)" : "");
        memcpy(t0, spl_total, sizeof t0); spl_lost = 0;
    }
    {   static uint64_t c0[FC_N];
        char why[400]; size_t o = 0; why[0] = 0; uint64_t tot = 0;
        for (int i = 0; i < FC_N; ++i) tot += fc_count[i] - c0[i];
        for (int i = 1; i < FC_N; ++i) o += (size_t)snprintf(why + o, sizeof why - o, "%s%s %.1f", i > 1 ? ", " : "", fc_name[i], (fc_count[i] - c0[i]) / nf);
        if (tot) VKLOG("frames %llu-%llu: colour fetch per frame (proven samples of an attached target): used %.1f; not used: %s",
            (unsigned long long)f0, (unsigned long long)f, (fc_count[FC_OK] - c0[FC_OK]) / nf, why);
        memcpy(c0, fc_count, sizeof c0); }
    if (nvk_fetch_depth_on()) {
        static uint64_t d0[FD_N];
        char why[400]; size_t o = 0; why[0] = 0;
        for (int i = 1; i < FD_N; ++i) o += (size_t)snprintf(why + o, sizeof why - o, "%s%s %.1f", i > 1 ? ", " : "", fd_name[i], (fd_count[i] - d0[i]) / nf);
        VKLOG("frames %llu-%llu: depth fetch draws per frame %.1f; not used: %s", (unsigned long long)f0, (unsigned long long)f, (fd_count[FD_OK] - d0[FD_OK]) / nf, why);
        memcpy(d0, fd_count, sizeof d0);
    }
    {   static uint64_t sc0, sr0;
        if (scratch_copies + scratch_reused != sc0 + sr0) VKLOG("frames %llu-%llu: misaligned storage ranges per frame copied %.1f, bound from an earlier copy %.1f",
            (unsigned long long)f0, (unsigned long long)f, (scratch_copies - sc0) / nf, (scratch_reused - sr0) / nf);
        sc0 = scratch_copies; sr0 = scratch_reused; }
    {
        static uint64_t ue0;
        if (ubo_early_skips != ue0) VKLOG("frames %llu-%llu: uniform snapshots ruled out before pool lookup %.0f per frame",
            (unsigned long long)f0, (unsigned long long)f, (ubo_early_skips - ue0) / nf);
        ue0 = ubo_early_skips;
    }
    {   static uint64_t sh0, sm0; uint64_t sh, sm; nvk_sig_stats(&sh, &sm);
        if (sh + sm != sh0 + sm0) VKLOG("frames %llu-%llu: shader variant signatures per frame reused %.0f, computed %.0f",
            (unsigned long long)f0, (unsigned long long)f, (sh - sh0) / nf, (sm - sm0) / nf);
        sh0 = sh; sm0 = sm; }
    {   static uint64_t hits0, misses0;
        VKLOG("frames %llu-%llu: graphics pipeline lookups per frame memo %.0f, full hash/table %.0f",
            (unsigned long long)f0, (unsigned long long)f, (pipe_memo_hits - hits0) / nf, (pipe_memo_misses - misses0) / nf);
        hits0 = pipe_memo_hits; misses0 = pipe_memo_misses;
    }
    {   static uint64_t dh0, dfl0;
        if (driver_fast_hits + driver_fills != dh0 + dfl0) VKLOG("frames %llu-%llu: driver banks per frame reused unchanged %.0f, filled %.0f",
            (unsigned long long)f0, (unsigned long long)f, (driver_fast_hits - dh0) / nf, (driver_fills - dfl0) / nf);
        dh0 = driver_fast_hits; dfl0 = driver_fills; }
    if (dc_ways() == 4 && dc_on()) {
        static uint64_t ev0;
        VKLOG("frames %llu-%llu: four-way descriptor cache: %.1f live entries displaced per frame",
            (unsigned long long)f0, (unsigned long long)f, (dc_evictions - ev0) / nf);
        ev0 = dc_evictions;
    }
    {   static uint64_t dw0, dc0, df0;
        if (dt_writes + dt_fallback != dw0 + df0) VKLOG("frames %llu-%llu: descriptor sets written per frame through templates %.0f, plainly %.0f (%llu templates)",
            (unsigned long long)f0, (unsigned long long)f, (dt_writes - dw0) / nf, (dt_fallback - df0) / nf, (unsigned long long)dt_created);
        dw0 = dt_writes; dc0 = dt_created; df0 = dt_fallback; (void)dc0; }
    {   static uint64_t th0, tm0;
        if (tc_hits + tc_misses != th0 + tm0) VKLOG("frames %llu-%llu: texture registry lookups per frame reused from preparation %.0f, looked up again %.0f",
            (unsigned long long)f0, (unsigned long long)f, (tc_hits - th0) / nf, (tc_misses - tm0) / nf);
        th0 = tc_hits; tm0 = tc_misses; }
    {   static uint64_t h0, m0;
        if (kc_hits + kc_misses != h0 + m0) VKLOG("frames %llu-%llu: pipeline key target formats per frame: kept from the pass %.0f, looked up %.0f",
            (unsigned long long)f0, (unsigned long long)f, (kc_hits - h0) / nf, (kc_misses - m0) / nf);
        h0 = kc_hits; m0 = kc_misses; }
    if (ph_on > 0) {
        static uint64_t p0[PH_N], pt0, pd0;
        double sum = 0, v[PH_N];
        for (int i = 0; i < PH_N; ++i) { v[i] = (double)(ph_ns[i] - p0[i]) / 1e6 / nf; sum += v[i]; }
        double total = (double)(ph_total - pt0) / 1e6 / nf, draws = (double)(ph_draws - pd0) / nf;
        VKLOG("frames %llu-%llu: CPU in draws %.2f ms per frame (%.0f draws, %.2f us each): shaders %.2f, textures %.2f, pipeline %.2f, descriptors %.2f, recording %.2f, early returns %.2f",
            (unsigned long long)f0, (unsigned long long)f, total, draws, draws > 0 ? total * 1000.0 / draws : 0.0,
            v[PH_SHADERS], v[PH_TEXTURES], v[PH_PIPELINE], v[PH_DESCRIPTORS], v[PH_RECORD], total - sum);
        memcpy(p0, ph_ns, sizeof p0); pt0 = ph_total; pd0 = ph_draws;
    }
}
