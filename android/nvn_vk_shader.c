#include "nvn_vk_translate.h"
/* Shader variants for the Vulkan execution layer.
 *
 * As in the macOS port (totk_shader_rt.m), a shader is identified by
 * SHA-256(control 0x880 + code area) and translated per "answers": the
 * texture/image slot types, vertex input and colour output classes of the
 * bound state, plus the NVN depth mode. Translations come from the bridge
 * (tools/shader_bridge.py over adb reverse) and are cached on the device in
 * TOTK_SHADER_CACHE. Each reply holds SPIR-V and a binding table. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include "nvn_vk_internal.h"
/* Live switches live in nvn_vk_draw.c; tests that link this file without it read the environment. */
__attribute__((weak)) unsigned nvk_live_gen;
__attribute__((weak)) const char *nvk_live_getenv(const char *name) { return getenv(name); }
#include "sha256.h"

extern int totk_depth_near_minus_w(void);
_Atomic uint64_t nvk_shader_count, nvk_shader_ns;

/* ---------- Shader identity per program stage ---------- */
typedef struct { uint64_t serial; const void *control; char key[65]; } KeyEntry;
static KeyEntry keys[4096];

static const char *shader_key(const prog_t *p, unsigned i) {
    const uint8_t *control = p->sd[i].control;
    unsigned slot = (unsigned)((p->serial * 31 + i) % 4096);
    KeyEntry *e = &keys[slot];
    if (e->serial == p->serial && e->control == control && e->key[0]) return e->key;
    uint32_t length;
    memcpy(&length, control + 0x704, 4);
    if (!length || length > 16u << 20) nvk_die("invalid NVN shader code length");
    Sha256 s; uint8_t d[32];
    sha_init(&s); sha_update(&s, control, 0x880); sha_update(&s, (const void *)(uintptr_t)p->sd[i].data, length); sha_final(&s, d);
    e->serial = p->serial; e->control = control; digest_hex(d, e->key);
    return e->key;
}

/* ---------- Answers from the bound state (nvn_mtl.m rt_answers) ---------- */
static const char *target_name(int t) {
    switch (t) { case 0: return "1d"; case 2: return "3d"; case 3: return "1darray"; case 4: return "2darray"; case 5: return "2dms";
    case 6: return "2dmsarray"; case 8: return "cube"; case 9: return "cubearray"; case 10: return "buffer"; default: return "2d"; }
}
static const char *image_format(int f) {
    switch (f) {
    case 0x01: return "R8Unorm"; case 0x0D: return "R8G8Unorm"; case 0x25: return "R8G8B8A8Unorm"; case 0x27: return "R8G8B8A8Uint";
    case 0x05: return "R16Float"; case 0x11: return "R16G16Float"; case 0x29: return "R16G16B16A16Float"; case 0x08: return "R16Uint"; case 0x2C: return "R16G16B16A16Uint";
    case 0x0A: return "R32Float"; case 0x16: return "R32G32Float"; case 0x2E: return "R32G32B32A32Float"; case 0x0B: return "R32Uint"; case 0x0C: return "R32Sint";
    case 0x17: return "R32G32Uint"; case 0x2F: return "R32G32B32A32Uint"; case 0x3D: return "R10G10B10A2Unorm"; case 0x3F: return "R11G11B10Float";
    default: return NULL;
    }
}
static int fmt_class_id(int f) {   /* 0 float, 1 uint, 2 sint */
    switch (f) {
    case 0x03: case 0x08: case 0x0B: case 0x0F: case 0x14: case 0x17: case 0x1B: case 0x20: case 0x23: case 0x27: case 0x2C: case 0x2F: return 1;
    case 0x04: case 0x09: case 0x0C: case 0x10: case 0x15: case 0x18: case 0x1C: case 0x21: case 0x24: case 0x28: case 0x2D: case 0x30: return 2;
    default: return 0;
    }
}
static const char *fmt_class(int f) { static const char *const n[3] = {"float", "uint", "sint"}; return n[fmt_class_id(f)]; }

typedef struct { char *p; size_t n, cap; } Str;
static void put(Str *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(Str *s, const char *fmt, ...) {
    for (;;) {
        va_list ap; va_start(ap, fmt);
        int k = vsnprintf(s->p + s->n, s->cap - s->n, fmt, ap);
        va_end(ap);
        if (k < 0) nvk_die("shader request formatting");
        if ((size_t)k < s->cap - s->n) { s->n += (size_t)k; return; }
        s->cap = s->cap * 2 + (size_t)k + 64;
        s->p = realloc(s->p, s->cap);
        if (!s->p) nvk_die("shader request allocation");
    }
}

/* JSON members (without braces) describing the bound state one stage depends on. */
static void answers(Str *out, int st) {
    int first = 1;
    put(out, "\"attr\": {");
    if (st == 0) for (int i = 0; i < nvk_S.nattr && i < 16; ++i) {
        put(out, "%s\"%d\": \"%s\"", first ? "" : ", ", i, fmt_class(nvk_S.attr[i].fmt)); first = 0;
    }
    put(out, "}, \"depth_minus_w\": %s, \"fout\": {", totk_depth_near_minus_w() ? "true" : "false");
    first = 1;
    const p_set_rt *rt = nvk_render_targets();
    if (st == 1 && rt) for (int i = 0; i < rt->n && i < 8; ++i) if (rt->color[i]) {
        int f = rt->has_cview[i] && rt->cview[i].set_format ? rt->cview[i].format : rt->color[i]->d.format;
        put(out, "%s\"%d\": \"%s\"", first ? "" : ", ", i, fmt_class(f)); first = 0;
    }
    put(out, "}, \"img\": {");
    first = 1;
    for (int r = 0; r < 32; ++r) {
        if (!nvk_S.img[st][r]) continue;
        tex_t t; view_t v; int hv;
        if (!nvk_registered_texture((int)(nvk_S.img[st][r] & 0xfffff), &t, &v, &hv)) continue;
        int f = hv && v.set_format ? v.format : t.d.format, tg = hv && v.set_target ? v.target : t.d.target;
        const char *fn = image_format(f);
        if (fn) put(out, "%s\"%d\": {\"fmt\": \"%s\", \"type\": \"%s\"}", first ? "" : ", ", r, fn, target_name(tg));
        else put(out, "%s\"%d\": {\"type\": \"%s\"}", first ? "" : ", ", r, target_name(tg));
        first = 0;
    }
    put(out, "}, \"tex\": {");
    first = 1;
    for (int r = 0; r < 128; ++r) {
        if (!nvk_S.tex[st][r]) continue;
        tex_t t; view_t v; int hv;
        if (!nvk_registered_texture((int)(nvk_S.tex[st][r] & 0xfffff), &t, &v, &hv)) continue;
        int f = hv && v.set_format ? v.format : t.d.format, tg = hv && v.set_target ? v.target : t.d.target;
        put(out, "%s\"%d\": \"%s%s\"", first ? "" : ", ", r, target_name(tg), f >= 0x32 && f <= 0x36 ? "_depth" : "");
        first = 0;
    }
    put(out, "}");
}

/* ---------- Bridge connection ----------
 * Optional: the device plays from seeded and cached variants alone (tools/android_shader_seed.py). Without a bridge
 * (none listening, or TOTK_SHADER_BRIDGE=0) a variant neither covers is skipped, never fatal; a connection attempt
 * is repeated at most every 5 s. */
static int bridge_fd = -1, bridge_said;
static uint64_t bridge_retry_ns;
static char rbuf[65536]; static size_t rlen, rpos;
static int bridge_disabled(void) {
    static int off = -1;
    if (off < 0) { const char *e = getenv("TOTK_SHADER_BRIDGE"); off = e && !strcmp(e, "0"); if (off) VKLOG("TOTK_SHADER_BRIDGE=0: seeded and cached shader variants only"); }
    return off;
}
static void bridge_lost(const char *why) {
    if (bridge_fd >= 0) close(bridge_fd);
    bridge_fd = -1; bridge_retry_ns = nvk_now_ns() + 5000000000ull;
    VKLOG("shader bridge %s: seeded and cached variants only, other variants are skipped", why);
}

/* 1 when a bridge is connected (TOTK_SHADER_BRIDGE: port, default 27760; 0 = no bridge). */
static int bridge_connect(void) {
    if (bridge_fd >= 0) return 1;
    if (bridge_disabled() || nvk_now_ns() < bridge_retry_ns) return 0;
    const char *e = getenv("TOTK_SHADER_BRIDGE");
    int port = e && *e ? atoi(e) : 27760;
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof a)) {
        if (!bridge_said++)
            VKLOG("shader bridge 127.0.0.1:%d unreachable (%s): seeded and cached variants only (a Mac can serve the rest: tools/shader_bridge.py and adb reverse tcp:%d tcp:%d)", port, strerror(errno), port, port);
        if (fd >= 0) close(fd);
        bridge_retry_ns = nvk_now_ns() + 5000000000ull;
        return 0;
    }
    struct timeval timeout = {.tv_sec = 30};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout) ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout)) nvk_die("shader bridge timeout setup");
    bridge_fd = fd; rlen = rpos = 0;
    VKLOG("connected to shader bridge on port %d", port);
    return 1;
}
static int bridge_byte(void) {
    if (rpos == rlen) {
        ssize_t k;
        do k = read(bridge_fd, rbuf, sizeof rbuf); while (k < 0 && errno == EINTR);
        if (k <= 0) { VKLOG("shader bridge read: %s", k ? strerror(errno) : "connection closed"); return -1; }
        rlen = (size_t)k; rpos = 0;
    }
    return (unsigned char)rbuf[rpos++];
}
static int send_all(const char *p, size_t n) {
    while (n) {
        ssize_t k = write(bridge_fd, p, n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return 0;
        p += k; n -= (size_t)k;
    }
    return 1;
}

/* Reply bytes: header and table lines, then the SPIR-V (length from the header). NULL without a bridge, or when
 * it disconnects or stalls (30 s) during the request. */
static uint8_t *bridge_request(const char *json, size_t *out_len) {
    if (!bridge_connect()) return NULL;
    if (!send_all(json, strlen(json))) { bridge_lost("write failed"); return NULL; }
    Str reply = {0};
    char line[512]; size_t ll = 0;
    long spv = -1, lines = -1, seen = -1;
    for (;;) {
        int c = bridge_byte();
        if (c < 0) { free(reply.p); bridge_lost("disconnected or stalled (30 s)"); return NULL; }
        put(&reply, "%c", c);
        if (c != '\n') { if (ll < sizeof line - 1) line[ll++] = (char)c; continue; }
        line[ll] = 0; ll = 0;
        if (seen < 0) {
            if (!strncmp(line, "ERR ", 4)) { VKLOG("shader translation failed: %s", line + 4); nvk_die("shader translation failed"); }
            long nb, ni, no;
            if (sscanf(line, "OK %ld %ld %ld %ld", &spv, &nb, &ni, &no) != 4) { VKLOG("bad bridge reply: %s", line); nvk_die("bad shader bridge reply"); }
            lines = nb + ni + no; seen = 0;
        } else ++seen;
        if (seen == lines) break;
    }
    for (long i = 0; i < spv; ++i) {
        int c = bridge_byte();
        if (c < 0) { free(reply.p); bridge_lost("disconnected or stalled (30 s)"); return NULL; }
        put(&reply, "%c", c);
    }
    *out_len = reply.n;
    return (uint8_t *)reply.p;
}

/* ---------- Variant table ---------- */
typedef struct Variant { char name[80]; NvkShader *shader; struct Variant *next; } Variant;
static Variant *variants[4096];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

/* ---------- Parsing and Vulkan objects ---------- */
/* c[1]: the shader package's own constants, as a uniform buffer. Created at
 * the first draw for variants preloaded from the cache. */
static void attach_c1(NvkShader *s, const uint8_t *c1, uint32_t c1_len) {
    s->c1_size = (c1_len + 255u) & ~255u;
    if (!s->c1_size) s->c1_size = 256;
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = s->c1_size, .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
    VKCHECK(vkCreateBuffer(nvk_dev, &bci, NULL, &s->c1), "create c1 buffer");
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(nvk_dev, s->c1, &req);
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
        .memoryTypeIndex = nvk_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VKCHECK(vkAllocateMemory(nvk_dev, &mai, NULL, &s->c1_memory), "allocate c1 buffer");
    VKCHECK(vkBindBufferMemory(nvk_dev, s->c1, s->c1_memory, 0), "bind c1 buffer");
    void *m; VKCHECK(vkMapMemory(nvk_dev, s->c1_memory, 0, VK_WHOLE_SIZE, 0, &m), "map c1 buffer");
    memset(m, 0, s->c1_size); if (c1_len) memcpy(m, c1, c1_len);
    vkUnmapMemory(nvk_dev, s->c1_memory);
}
static void load_fetch(NvkShader *s);
/* Game uniform buffers as dynamic uniform buffers (nvn_vk_draw.c, descriptor reuse): the offset of a binding then goes
   with vkCmdBindDescriptorSets instead of into the set, so draws that differ only in where their constants sit (most
   of them: per-object blocks in one buffer) can share a set. Up to the device's maxDescriptorSetUniformBuffersDynamic
   per pipeline layout: half each for the vertex and fragment set, all of it for compute; lowest binding numbers first.
   nvk_dyn_ubo_limit is set at device creation (android_gfx_bridge.c); 0 (host tests, TOTK_VK_UBO_DYNAMIC=0): none. */
int nvk_dyn_ubo_limit;
static void dynamic_ubos(NvkShader *s) {
    unsigned budget = s->stage == 5 ? (unsigned)nvk_dyn_ubo_limit : (unsigned)nvk_dyn_ubo_limit / 2;
    if (budget > 16) budget = 16;
    s->dyn = 0; s->ndyn = 0;
    while (s->ndyn < budget) {   /* binding numbers in rising order: the order of the offsets */
        int best = -1;
        for (int i = 0; i < s->nb; ++i)
            if (s->b[i].kind == 1 && s->b[i].dtype == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && s->b[i].count == 1 && !(s->dyn >> i & 1) &&
                (best < 0 || s->b[i].binding < s->b[best].binding)) best = i;
        if (best < 0) break;
        s->dyn |= 1ull << best; s->dyn_order[s->ndyn++] = (uint8_t)best;
    }
}
/* ---------- Exact vertex positions (TOTK_VK_EXACT_POSITION) ----------
 * TOTK draws geometry again over its own depth: EQUAL or LEQUAL tests without depth writes (27 and 42 of the traced
 * draws, 22 Oct), decals, terrain blend layers, class lighting. That needs bit-identical positions from different
 * vertex shaders. Maxwell runs each FMUL / FADD as written; the translated SPIR-V had neither Invariant on the position
 * nor NoContraction, so the Adreno compiler may fuse multiply-adds differently per shader and per pipeline: a second
 * pass then lands a rounding step off its depth and drops pixels (BOTW's Android port saw black slashes and flicker on
 * foliage drawn twice until it made gl_Position invariant and precise: runtime/nvn/maxwell/translate.c, MIT).
 * Every vertex shader gets Invariant on its position and NoContraction on its float arithmetic. NULL: not recognised. */
static uint32_t *spv_exact_position(const uint32_t *w, size_t n, size_t *out_n, unsigned *exact) {
    if (n < 5 || w[0] != 0x07230203) return NULL;
    size_t insert = 0, nops = 0; uint32_t pos_struct = 0, pos_member = 0, pos_var = 0; int invariant = 0;
    for (size_t i = 5; i < n; ) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) return NULL;
        if (op == 54) break;   /* first function: the annotations are behind us */
        if (op == 71 || op == 72 || op == 73 || op == 74 || op == 75 || op == 332 || op == 5632 || op == 5633) insert = i + len;
        if (op == 72 && len == 5 && w[i + 3] == 11 && w[i + 4] == 0) { pos_struct = w[i + 1]; pos_member = w[i + 2]; }
        if (op == 71 && len == 4 && w[i + 2] == 11 && w[i + 3] == 0) pos_var = w[i + 1];
        if ((op == 72 && len == 4 && w[i + 3] == 18) || (op == 71 && len == 3 && w[i + 2] == 18)) invariant = 1;
        i += len;
    }
    if (!insert || (!pos_struct && !pos_var)) return NULL;
    for (size_t i = 5; i < n; i += w[i] >> 16) {
        uint32_t op = w[i] & 0xffff;
        if (op == 129 || op == 131 || op == 133 || (op >= 142 && op <= 148)) nops++;   /* FAdd FSub FMul, vector/matrix products, Dot */
    }
    uint32_t *out = malloc((n + 4 + 3 * nops) * 4);
    if (!out) return NULL;
    memcpy(out, w, insert * 4); size_t o = insert;
    if (!invariant) {
        if (pos_struct) { out[o++] = 4u << 16 | 72; out[o++] = pos_struct; out[o++] = pos_member; out[o++] = 18; }
        else { out[o++] = 3u << 16 | 71; out[o++] = pos_var; out[o++] = 18; }
    }
    for (size_t i = 5; i < n; i += w[i] >> 16) {
        uint32_t op = w[i] & 0xffff;
        if (op == 129 || op == 131 || op == 133 || (op >= 142 && op <= 148)) { out[o++] = 3u << 16 | 71; out[o++] = w[i + 2]; out[o++] = 42; }
    }
    memcpy(out + o, w + insert, (n - insert) * 4); o += n - insert;
    *out_n = o; *exact = (unsigned)nops;
    return out;
}
static int exact_position_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_VK_EXACT_POSITION"); on = !(e && !strcmp(e, "0"));
        VKLOG("exact vertex positions (Invariant + NoContraction): %s", on ? "on" : "off (TOTK_VK_EXACT_POSITION=0)"); }
    return on;
}
static NvkShader *parse_module(const char *variant, int stage, const uint8_t *data, size_t len) {
    NvkShader *s = calloc(1, sizeof *s);
    if (!s) nvk_die("shader allocation");
    snprintf(s->variant, sizeof s->variant, "%s", variant);
    s->stage = stage;
    const char *p = (const char *)data, *end = p + len;
    long spv, nb, ni, no;
    if (sscanf(p, "OK %ld %ld %ld %ld %u %u %u", &spv, &nb, &ni, &no, &s->local[0], &s->local[1], &s->local[2]) != 7) nvk_die("corrupt shader cache entry");
    s->b = calloc((size_t)(nb ? nb : 1), sizeof *s->b); s->nb = (int)nb;
    p = memchr(p, '\n', (size_t)(end - p)) + 1;
    for (long i = 0; i < nb + ni + no; ++i) {
        if (p[0] == 'B') {
            int set, binding, kind, reg, sreg, off, rw, dtype, count, view;
            if (sscanf(p, "B %d %d %d %d %d %d %d %d %d %d", &set, &binding, &kind, &reg, &sreg, &off, &rw, &dtype, &count, &view) != 10) nvk_die("corrupt shader binding line");
            NvkBinding *b = &s->b[i];
            *b = (NvkBinding){(uint8_t)set, (uint8_t)kind, (uint8_t)rw, (uint8_t)dtype, (int16_t)reg, (int16_t)sreg, (int16_t)view, (uint16_t)off, (uint16_t)binding, (uint16_t)count};
        } else if (p[0] == 'I') {
            int loc; char cls[8];
            if (sscanf(p, "I %d %7s", &loc, cls) != 2 || s->nin == 32) nvk_die("corrupt shader input line");
            s->in[s->nin++] = (typeof(s->in[0])){(uint8_t)loc, (uint8_t)(!strcmp(cls, "uint") ? 1 : !strcmp(cls, "sint") ? 2 : 0)};
            if (loc < 32) s->in_mask |= 1u << loc;
        } else if (p[0] == 'O') {
            int loc; char cls[8];
            if (sscanf(p, "O %d %7s", &loc, cls) != 2) nvk_die("corrupt shader output line");
            if (loc < 32) s->out_locs |= 1u << loc;
            if (stage == 1 && loc < 8) s->out_mask |= (uint8_t)(1u << loc);
        } else nvk_die("corrupt shader cache line");
        p = memchr(p, '\n', (size_t)(end - p)) + 1;
    }
    if (end - p != spv || spv % 4) nvk_die("shader cache SPIR-V length mismatch");
    VkShaderModuleCreateInfo mci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = (size_t)spv, .pCode = (const uint32_t *)p};
    uint32_t *aligned = malloc((size_t)spv);
    memcpy(aligned, p, (size_t)spv); mci.pCode = aligned;
    for (size_t w = 5; stage == 1 && w + 4 <= (size_t)spv / 4;) {   /* OpDecorate %x BuiltIn FragDepth */
        uint32_t op = aligned[w], n = op >> 16;
        if (op == (4u << 16 | 71u) && aligned[w + 2] == 11 && aligned[w + 3] == 22) { s->frag_depth = 1; break; }
        w += n ? n : 1;
    }
    uint32_t *exact_words = NULL;
    if (stage == 0 && exact_position_on()) {
        size_t en; unsigned ops;
        exact_words = spv_exact_position(aligned, (size_t)spv / 4, &en, &ops);
        if (exact_words) { mci.pCode = exact_words; mci.codeSize = en * 4; }
        else { static unsigned said; if (said++ < 8) VKLOG("vertex shader %s: position not found, compiled as translated", variant); }
    }
    VkResult r = vkCreateShaderModule(nvk_dev, &mci, NULL, &s->module);
    free(aligned); free(exact_words);
    if (r != VK_SUCCESS) { VKLOG("shader %s: vkCreateShaderModule -> %d", variant, r); nvk_die("create shader module"); }
    VkShaderStageFlags flags = stage == 0 ? VK_SHADER_STAGE_VERTEX_BIT : stage == 1 ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutBinding lb[64];
    if (s->nb > 64) nvk_die("too many shader resources");
    dynamic_ubos(s);
    for (int i = 0; i < s->nb; ++i)
        lb[i] = (VkDescriptorSetLayoutBinding){s->b[i].binding, s->dyn >> i & 1 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : (VkDescriptorType)s->b[i].dtype,
            s->b[i].count, flags, NULL};
    VkDescriptorSetLayoutCreateInfo lci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = (uint32_t)s->nb, .pBindings = lb};
    VKCHECK(vkCreateDescriptorSetLayout(nvk_dev, &lci, NULL, &s->layout), "create shader descriptor set layout");
    load_fetch(s);
    return s;
}
static const char *cache_dir(void);
/* <variant>.fetch next to the cached reply (tools/android_shader_seed.py): "S <sample id> <slot> <location>" for a
   fragment shader, "V <location>" for a vertex shader. A variant with stripped inputs ("-i<mask>") uses its parent's
   proofs, without samples whose location it stripped (they read the default now, not the screen position). */
static void load_fetch(NvkShader *s) {
    char base[80]; uint32_t stripped = 0;
    snprintf(base, sizeof base, "%s", s->variant);
    char *i = strstr(base + 33, "-i");
    if (i) { stripped = (uint32_t)strtoul(i + 2, NULL, 16); *i = 0; }
    if (strstr(base + 33, "-F")) return;   /* a fetch variant itself */
    char path[640]; snprintf(path, sizeof path, "%s/%s.fetch", cache_dir(), base);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[64]; unsigned id, slot, loc;
    while (fgets(line, sizeof line, f)) {
        if (s->stage == 1 && sscanf(line, "S %u %u %u", &id, &slot, &loc) == 3 && slot < 128 && loc < 32 && !(stripped >> loc & 1) && s->nfetch < 8)
            { s->fetch[s->nfetch].id = id; s->fetch[s->nfetch].slot = (uint8_t)slot; s->fetch[s->nfetch].loc = (uint8_t)loc; s->nfetch++; }
        else if (s->stage == 0 && sscanf(line, "V %u", &loc) == 1 && loc < 32) s->fetch_locs |= 1u << loc;
    }
    fclose(f);
}
static NvkShader *parse(const char *variant, int stage, const uint8_t *data, size_t len, const uint8_t *c1, uint32_t c1_len) {
    NvkShader *s = parse_module(variant, stage, data, len);
    attach_c1(s, c1, c1_len);
    return s;
}


static const char *cache_dir(void) {
    static char dir[512];
    if (!dir[0]) {
        const char *e = getenv("TOTK_SHADER_CACHE");
        snprintf(dir, sizeof dir, "%s", e && *e ? e : "shader-cache");
        mkdir(dir, 0700);
    }
    return dir;
}
static uint8_t *read_file(const char *path, size_t *len) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    uint8_t *buf = NULL;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size <= (32 << 20) &&
        (buf = malloc((size_t)st.st_size))) {
        size_t done = 0;
        while (done < (size_t)st.st_size) {
            ssize_t n = read(fd, buf + done, (size_t)st.st_size - done);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            done += (size_t)n;
        }
        if (done == (size_t)st.st_size) *len = done;
        else { free(buf); buf = NULL; }
    } else { free(buf); buf = NULL; }
    close(fd);
    return buf;
}

/* A torn or hand-edited .vks file must be treated as a cache miss. parse()
 * consumes the line table and SPIR-V directly, so validate every boundary
 * before allowing cached bytes to reach it. */
static int shader_data_valid(const uint8_t *data, size_t len) {
    if (!data || len < 8 || len > (32u << 20)) return 0;
    const uint8_t *end = data + len;
    const uint8_t *nl = memchr(data, '\n', len);
    if (!nl || (size_t)(nl - data) >= 256) return 0;
    char line[256]; size_t line_len = (size_t)(nl - data);
    memcpy(line, data, line_len); line[line_len] = 0;
    long spv, nb, ni, no; unsigned lx, ly, lz; int used = 0;
    if (sscanf(line, "OK %ld %ld %ld %ld %u %u %u%n", &spv, &nb, &ni, &no, &lx, &ly, &lz, &used) != 7 ||
        line[used] || spv <= 0 || (spv & 3) || nb < 0 || nb > 64 || ni < 0 || ni > 32 || no < 0 || no > 32 ||
        (uint64_t)spv > len || ((lx || ly || lz) && (!lx || !ly || !lz || lx > 1024 || ly > 1024 || lz > 1024 ||
            (uint64_t)lx * ly * lz > 1024))) return 0;

    const uint8_t *p = nl + 1;
    for (long i = 0; i < nb + ni + no; ++i) {
        nl = memchr(p, '\n', (size_t)(end - p));
        if (!nl || (size_t)(nl - p) >= sizeof line - 1) return 0;
        line_len = (size_t)(nl - p); memcpy(line, p, line_len); line[line_len] = 0;
        used = 0;
        if (i < nb) {
            int set, binding, kind, reg, sreg, off, rw, dtype, count, view;
            if (sscanf(line, "B %d %d %d %d %d %d %d %d %d %d%n", &set, &binding, &kind, &reg,
                    &sreg, &off, &rw, &dtype, &count, &view, &used) != 10 || line[used] ||
                set < 0 || set > 1 || binding < 0 || binding > 72 || kind < 0 || kind > 15 ||   /* 64+, 14, 15: fetch variants (72: depth) */
                reg < -1 || reg > 127 || sreg < -1 || sreg > 127 || off < 0 || off > UINT16_MAX ||
                (rw != 0 && rw != 1) || dtype < 0 || dtype > 10 || (dtype > 7 && kind != 14) || count != 1 ||
                view < -1 || view > 262) return 0;
        } else if (i < nb + ni) {
            int loc; char cls[8];
            if (sscanf(line, "I %d %7s%n", &loc, cls, &used) != 2 || line[used] || loc < 0 || loc >= 32 ||
                (strcmp(cls, "float") && strcmp(cls, "uint") && strcmp(cls, "sint"))) return 0;
        } else {
            int loc; char cls[8];
            if (sscanf(line, "O %d %7s%n", &loc, cls, &used) != 2 || line[used] || loc < 0 || loc >= 32 ||
                (strcmp(cls, "float") && strcmp(cls, "uint") && strcmp(cls, "sint"))) return 0;
        }
        p = nl + 1;
    }
    return (size_t)(end - p) == (size_t)spv;
}

static _Atomic uint64_t shader_cache_hits, shader_cache_misses;
static uint8_t *read_shader_cache(const char *path, size_t *len) {
    uint8_t *data = read_file(path, len);
    if (data && !shader_data_valid(data, *len)) {
        free(data); data = NULL; unlink(path);
        VKLOG("discarded invalid shader cache entry: %s", path);
    }
    uint64_t count = data ? atomic_fetch_add(&shader_cache_hits, 1) + 1 : atomic_fetch_add(&shader_cache_misses, 1) + 1;
    if (count == 1 || !(count % 128)) VKLOG("shader cache %s: %llu %s", data ? "hits" : "misses",
        (unsigned long long)count, data ? "loaded" : "so far");
    return data;
}

static void write_file(const char *path, const uint8_t *data, size_t len) {
    char tmp[600]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return;
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, data + done, len - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        done += (size_t)n;
    }
    int ok = done == len;
    if (close(fd)) ok = 0;
    if (!ok || rename(tmp, path)) unlink(tmp);
}

static void hex_into(Str *out, const uint8_t *p, size_t n) {
    static const char x[] = "0123456789abcdef";
    if (out->cap - out->n < 2 * n + 1) { out->cap = out->n + 2 * n + 64; out->p = realloc(out->p, out->cap); if (!out->p) nvk_die("shader request allocation"); }
    for (size_t i = 0; i < n; ++i) { out->p[out->n++] = x[p[i] >> 4]; out->p[out->n++] = x[p[i] & 15]; }
    out->p[out->n] = 0;
}

static Variant **slot_for(const char *variant) {
    unsigned h = 0; for (const char *c = variant; *c; ++c) h = h * 131 + (unsigned char)*c;
    Variant **v = &variants[h % 4096];
    while (*v && strcmp((*v)->name, variant)) v = &(*v)->next;
    return v;
}

NvkShader *nvk_shader_find_variant(const char *name) {
    if (!name || !*name) return NULL;
    pthread_mutex_lock(&lock);
    Variant *v = *slot_for(name);
    NvkShader *shader = v ? v->shader : NULL;
    pthread_mutex_unlock(&lock);
    return shader;
}

/* ---------- Unwritten fragment inputs (port of shader_bridge.py Spirv.zero_inputs) ----------
 * The Adreno driver refuses a fragment input the vertex stage does not write, which NVN allows. The Input variables
 * at the mask's locations become Private variables holding the Maxwell default ((0, 0, 0, 1) for a float vec4, else
 * null) and leave the entry point interface; access chains into them get Private pointer types. Word-for-word the
 * bridge's output (tests/zero_inputs_test.py), so the device needs no bridge for it. NULL on malformed SPIR-V. */
static uint32_t *spv_zero_inputs(const uint32_t *w, size_t n, uint32_t mask, size_t *out_n) {
    if (n < 5 || w[0] != 0x07230203 || w[3] > (1u << 22)) return NULL;
    uint32_t bound = w[3];
    uint32_t *tpos = calloc(bound, 4), *vtype = calloc(bound, 4), *vstor = calloc(bound, 4), *new_ptr = calloc(bound, 4), *nulls = calloc(bound, 4);
    int32_t *loc = malloc((size_t)bound * 4);
    uint8_t *flags = calloc(bound, 1);   /* 1 built-in, 2 variable, 4 target, 8 derived, 16 pointee needing a Private pointer */
    uint32_t *out = NULL, *decls = NULL; size_t nd = 0, no = 0;
    if (!tpos || !vtype || !vstor || !new_ptr || !nulls || !loc || !flags) goto fail;
    for (uint32_t i = 0; i < bound; ++i) loc[i] = -1;
    for (size_t i = 5; i < n; ) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) goto fail;
        if (op == 71 && len >= 3 && w[i + 1] < bound) {
            if (w[i + 2] == 30 && len > 3) loc[w[i + 1]] = (int32_t)w[i + 3];
            if (w[i + 2] == 11) flags[w[i + 1]] |= 1;
        } else if (((op >= 19 && op <= 30) || op == 32) && len >= 2 && w[i + 1] < bound) tpos[w[i + 1]] = (uint32_t)i;
        else if (op == 59 && len >= 4 && w[i + 2] < bound) { vtype[w[i + 2]] = w[i + 1]; vstor[w[i + 2]] = w[i + 3]; flags[w[i + 2]] |= 2; }
        i += len;
    }
#define POINTEE(ptr) ((ptr) < bound && tpos[ptr] && (w[tpos[ptr]] & 0xffff) == 32 ? w[tpos[ptr] + 3] : UINT32_MAX)
    int any = 0;
    for (uint32_t v = 0; v < bound; ++v)
        if ((flags[v] & 3) == 2 && vstor[v] == 1 && loc[v] >= 0 && loc[v] < 32 && (mask >> loc[v] & 1)) {
            uint32_t t = POINTEE(vtype[v]);
            if (t >= bound) goto fail;
            flags[v] |= 4 | 8; flags[t] |= 16; any = 1;
        }
    if (any) for (size_t i = 5; i < n; i += w[i] >> 16) {
        uint32_t op = w[i] & 0xffff;
        if ((op == 65 || op == 66) && (w[i] >> 16) >= 4 && w[i + 3] < bound && (flags[w[i + 3]] & 8)) {
            uint32_t t = POINTEE(w[i + 1]);
            if (w[i + 2] >= bound || t >= bound) goto fail;
            flags[w[i + 2]] |= 8; flags[t] |= 16;
        }
    }
    decls = malloc(((size_t)bound * 4 + 16) * 4);
    if (!decls) goto fail;
    uint32_t next = bound;
    for (uint32_t t = 0; t < bound; ++t) if (flags[t] & 16) new_ptr[t] = next++;
    for (uint32_t t = 0; t < bound; ++t) if (flags[t] & 16) { decls[nd++] = 4u << 16 | 32; decls[nd++] = new_ptr[t]; decls[nd++] = 6; decls[nd++] = t; }
    for (uint32_t v = 0; v < bound; ++v) {
        if (!(flags[v] & 4)) continue;
        uint32_t t = POINTEE(vtype[v]);
        if (nulls[t]) continue;
        uint32_t f = tpos[t] && (w[tpos[t]] & 0xffff) == 23 && w[tpos[t] + 3] == 4 ? w[tpos[t] + 2] : UINT32_MAX;
        if (f < bound && tpos[f] && (w[tpos[f]] & 0xffff) == 22 && w[tpos[f] + 2] == 32) {   /* Maxwell default of an unwritten attribute */
            uint32_t zero = next, one = next + 1, vec = next + 2;
            uint32_t d[] = {4u << 16 | 43, f, zero, 0, 4u << 16 | 43, f, one, 0x3f800000u, 7u << 16 | 44, t, vec, zero, zero, zero, one};
            memcpy(decls + nd, d, sizeof d); nd += sizeof d / 4;
            nulls[t] = vec; next += 3;
        } else {
            decls[nd++] = 3u << 16 | 46; decls[nd++] = t; decls[nd++] = next; nulls[t] = next++;
        }
    }
    out = malloc((n + nd + 8) * 4);
    if (!out) goto fail;
    memcpy(out, w, 5 * 4); no = 5;
    int inserted = 0;
    for (size_t i = 5; i < n; ) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (op == 15 && len >= 4) {   /* OpEntryPoint: drop the targets from the interface */
            size_t k = 3, start = no;
            while (k < len) { uint32_t word = w[i + k++]; if (!(word & 0xff) || !(word & 0xff00) || !(word & 0xff0000) || !(word & 0xff000000)) break; }
            memcpy(out + no, w + i, k * 4); no += k;
            for (; k < len; ++k) if (!(w[i + k] < bound && (flags[w[i + k]] & 4))) out[no++] = w[i + k];
            out[start] = (uint32_t)(no - start) << 16 | op;
        } else if (op == 71 && w[i + 1] < bound && (flags[w[i + 1]] & 4)) {
        } else if (op == 59 && w[i + 2] < bound && (flags[w[i + 2]] & 4)) {
            if (!inserted) { memcpy(out + no, decls, nd * 4); no += nd; inserted = 1; }
            uint32_t t = POINTEE(w[i + 1]);
            out[no++] = 5u << 16 | 59; out[no++] = new_ptr[t]; out[no++] = w[i + 2]; out[no++] = 6; out[no++] = nulls[t];
        } else if ((op == 65 || op == 66) && w[i + 3] < bound && (flags[w[i + 3]] & 8)) {
            memcpy(out + no, w + i, len * 4); out[no + 1] = new_ptr[POINTEE(w[i + 1])]; no += len;
        } else { memcpy(out + no, w + i, len * 4); no += len; }
        i += len;
    }
#undef POINTEE
    out[3] = next;
    *out_n = no;
    free(tpos); free(vtype); free(vstor); free(new_ptr); free(nulls); free(loc); free(flags); free(decls);
    return out;
fail:
    free(tpos); free(vtype); free(vstor); free(new_ptr); free(nulls); free(loc); free(flags); free(decls); free(out);
    return NULL;
}
/* A cached reply with the inputs at mask stripped: SPIR-V rewritten, their I lines dropped, header counts updated. */
static uint8_t *zero_inputs_reply(const uint8_t *data, size_t len, uint32_t mask, size_t *out_len) {
    if (!shader_data_valid(data, len)) return NULL;
    long spv, nb, ni, no; unsigned lx, ly, lz;
    if (sscanf((const char *)data, "OK %ld %ld %ld %ld %u %u %u", &spv, &nb, &ni, &no, &lx, &ly, &lz) != 7) return NULL;
    const uint8_t *p = memchr(data, '\n', len) + 1, *spv_at = data + len - spv;
    uint32_t *words = malloc((size_t)spv), *rewritten; size_t nw;
    if (!words) return NULL;
    memcpy(words, spv_at, (size_t)spv);
    rewritten = spv_zero_inputs(words, (size_t)spv / 4, mask, &nw);
    free(words);
    if (!rewritten) return NULL;
    Str table = {0}; long kept_in = 0;
    while (p < spv_at) {
        const uint8_t *e = memchr(p, '\n', (size_t)(spv_at - p));
        int l;
        if (!(p[0] == 'I' && sscanf((const char *)p, "I %d", &l) == 1 && l >= 0 && l < 32 && (mask >> l & 1))) {
            if (p[0] == 'I') ++kept_in;
            put(&table, "%.*s\n", (int)(e - p), (const char *)p);
        }
        p = e + 1;
    }
    Str r = {0};
    put(&r, "OK %zu %ld %ld %ld %u %u %u\n%s", nw * 4, nb, kept_in, no, lx, ly, lz, table.p ? table.p : "");
    size_t head = r.n;
    r.p = realloc(r.p, head + nw * 4);
    if (!r.p) nvk_die("shader reply allocation");
    memcpy(r.p + head, rewritten, nw * 4);
    free(rewritten); free(table.p);
    *out_len = head + nw * 4;
    return (uint8_t *)r.p;
}

NvkShader *nvk_shader_zero_inputs(NvkShader *ps, uint32_t mask) {
    char name[80];
    snprintf(name, sizeof name, "%.60s-i%x", ps->variant, mask);
    pthread_mutex_lock(&lock);
    Variant **v = slot_for(name);
    if (*v) {
        NvkShader *sh = (*v)->shader;
        if (!sh->c1) {   /* preloaded: the parent's constants */
            void *m; VKCHECK(vkMapMemory(nvk_dev, ps->c1_memory, 0, VK_WHOLE_SIZE, 0, &m), "map c1 buffer");
            attach_c1(sh, m, ps->c1_size); vkUnmapMemory(nvk_dev, ps->c1_memory);
        }
        pthread_mutex_unlock(&lock);
        return sh;
    }
    char path[640];
    snprintf(path, sizeof path, "%s/%s.vks", cache_dir(), name);
    uint64_t started = nvk_now_ns();
    size_t len = 0;
    uint8_t *data = read_shader_cache(path, &len);
    if (!data) {
        char parent[640];
        snprintf(parent, sizeof parent, "%s/%s.vks", cache_dir(), ps->variant);
        size_t plen = 0;
        uint8_t *pdata = read_file(parent, &plen);
        int had = pdata != NULL;
        data = had ? zero_inputs_reply(pdata, plen, mask, &len) : NULL;
        free(pdata);
        if (!data) {
            pthread_mutex_unlock(&lock);
            static unsigned said; if (said++ < 8) VKLOG("fragment variant %s: inputs %#x cannot be stripped (%s): draw skipped", ps->variant, mask, had ? "malformed" : "not cached");
            return NULL;
        }
        write_file(path, data, len);
        VKLOG("fragment variant %s: inputs %#x unwritten by the vertex stage read zero", ps->variant, mask);
    }
    void *c1 = calloc(1, ps->c1_size);
    void *m; VKCHECK(vkMapMemory(nvk_dev, ps->c1_memory, 0, VK_WHOLE_SIZE, 0, &m), "map c1 buffer");
    memcpy(c1, m, ps->c1_size); vkUnmapMemory(nvk_dev, ps->c1_memory);
    NvkShader *sh = parse(name, 1, data, len, c1, ps->c1_size);
    atomic_fetch_add(&nvk_shader_count, 1); atomic_fetch_add(&nvk_shader_ns, nvk_now_ns() - started);
    free(c1); free(data);
    Variant *n = calloc(1, sizeof *n);
    snprintf(n->name, sizeof n->name, "%s", name); n->shader = sh; *v = n;
    pthread_mutex_unlock(&lock);
    nvk_pipeline_warm_signal();
    return sh;
}

/* ---------- Class depth written off the tie (TOTK_VK_FRAGDEPTH_EQUAL_BIAS, nvn_vk_draw.c) ----------
 * TOTK writes its class depth as gl_FragDepth = 0.5 + class / 510: in a 16-bit buffer that is 32767.5 + 128.5 * class,
 * exactly half a code for every even class (water: 39192.5). Which way the writer and the EQUAL-testing light pass then
 * round depends on float noise, differently per class (23 Oct, Thor: one bias lit the water and left Link unlit, the
 * other way round without it). The writer's variant adds `delta` to every gl_FragDepth store; the light passes get the
 * same shift as a depth bias, so both sit a quarter code off the tie and round alike. NULL on SPIR-V not recognised. */
static uint32_t *spv_depth_offset(const uint32_t *w, size_t n, float delta, size_t *out_n) {
    if (n < 5 || w[0] != 0x07230203 || w[3] > (1u << 22)) return NULL;
    uint32_t bound = w[3], fd = 0, f32 = 0, stores = 0;
    for (size_t i = 5; i < n; ) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) return NULL;
        if (op == 71 && len == 4 && w[i + 2] == 11 && w[i + 3] == 22) fd = w[i + 1];
        if (op == 22 && len == 3 && w[i + 2] == 32) f32 = w[i + 1];
        i += len;
    }
    if (!fd || !f32) return NULL;
    for (size_t i = 5; i < n; i += w[i] >> 16) if ((w[i] & 0xffff) == 62 && (w[i] >> 16) >= 3 && w[i + 1] == fd) stores++;
    if (!stores) return NULL;
    uint32_t *out = malloc((n + 4 + (size_t)stores * 5) * 4);
    if (!out) return NULL;
    uint32_t c = bound, next = bound + 1, bits; memcpy(&bits, &delta, 4);
    memcpy(out, w, 5 * 4); size_t no = 5; int placed = 0;
    for (size_t i = 5; i < n; ) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (op == 54 && !placed) { out[no++] = 4u << 16 | 43; out[no++] = f32; out[no++] = c; out[no++] = bits; placed = 1; }   /* OpConstant before the first function */
        if (op == 62 && len >= 3 && w[i + 1] == fd) {
            uint32_t sum = next++;
            out[no++] = 5u << 16 | 129; out[no++] = f32; out[no++] = sum; out[no++] = w[i + 2]; out[no++] = c;   /* OpFAdd */
            memcpy(out + no, w + i, len * 4); out[no + 2] = sum; no += len;
        } else { memcpy(out + no, w + i, len * 4); no += len; }
        i += len;
    }
    if (!placed) { free(out); return NULL; }
    out[3] = next; *out_n = no;
    return out;
}
/* The class-depth writers at half resolution (ps b117e55c, 5218c5e5) read the material ID with texture() at an
 * interpolated coordinate that falls exactly on the corner of four full-resolution texels; x255 and trunc then make the
 * class. Interpolation noise on the Adreno picks one of the four per pixel (or blends them), so on material borders
 * single half-resolution pixels got another class: unlit 2x2 blocks after the upscale that move with the camera (the
 * "korrels", 24 Oct). In the writer variant every 2D texture() sample reads the texel floor(uv * size + 0.25) at its
 * centre, at level 0: one fixed texel, as exact arithmetic picks it. Samples with image operands are left alone. */
static uint32_t *spv_snap_samples(const uint32_t *w, size_t n, size_t *out_n) {
    if (n < 5 || w[0] != 0x07230203 || w[3] > (1u << 22)) return NULL;
    uint32_t bound = w[3];
    uint32_t *rtype = calloc(bound, 4), *ty_op = calloc(bound, 4), *ty_a = calloc(bound, 4), *ty_b = calloc(bound, 4);
    uint32_t *out = NULL;
    if (!rtype || !ty_op || !ty_a || !ty_b) goto fail;
    uint32_t glsl = 0, f32 = 0, i32 = 0, v2f = 0, v2i = 0, nsamples = 0; int has_cap = 0;
    for (size_t i = 5; i < n; ) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) goto fail;
        if (op == 17 && len == 2 && w[i + 1] == 50) has_cap = 1;
        else if (op == 11 && len >= 3 && !strncmp((const char *)(w + i + 2), "GLSL.std.450", 12)) glsl = w[i + 1];
        else if (op == 22 && len == 3 && w[i + 2] == 32) f32 = w[i + 1];
        else if (op == 21 && len == 4 && w[i + 2] == 32 && w[i + 3] == 1) i32 = w[i + 1];
        else if (op == 23 && len == 4 && w[i + 3] == 2 && w[i + 2] < bound) { ty_op[w[i + 1]] = 23; ty_a[w[i + 1]] = w[i + 2]; }
        else if (op == 25 && len >= 9 && w[i + 1] < bound) { ty_op[w[i + 1]] = 25; ty_a[w[i + 1]] = w[i + 3] | w[i + 5] << 8 | w[i + 6] << 16; }   /* dim, arrayed, ms */
        else if (op == 27 && len == 3 && w[i + 1] < bound) { ty_op[w[i + 1]] = 27; ty_b[w[i + 1]] = w[i + 2]; }
        else if ((op == 61 || op == 169 || op == 80 || op == 79 || op == 129 || op == 133 || op == 136 || op == 12) && len >= 3 && w[i + 2] < bound) rtype[w[i + 2]] = w[i + 1];
        i += len;
    }
    for (uint32_t t = 0; t < bound; ++t) if (ty_op[t] == 23 && ty_a[t] == f32 && f32) v2f = t; else if (ty_op[t] == 23 && ty_a[t] == i32 && i32) v2i = t;
    if (!glsl || !f32 || !v2f) goto fail;
    for (size_t i = 5; i < n; i += w[i] >> 16) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (op != 87 || !(len == 5 || (len == 6 && !w[i + 5])) || w[i + 3] >= bound || w[i + 4] >= bound) continue;   /* no image operands (or an empty mask) */
        uint32_t st = rtype[w[i + 3]], it = st < bound && ty_op[st] == 27 ? ty_b[st] : 0;
        if (it && ty_op[it] == 25 && ty_a[it] == 1 && rtype[w[i + 4]] == v2f) nsamples++;   /* 2D, not arrayed, not multisampled */
    }
    if (!nsamples) goto fail;
    out = malloc((n + 40 + (size_t)nsamples * 40) * 4);
    if (!out) goto fail;
    uint32_t next = bound;
    uint32_t I32 = i32 ? i32 : next++, V2I = v2i ? v2i : next++;
    uint32_t ci0 = next++, cf0 = next++, cf025 = next++, cf05 = next++, cv025 = next++, cv05 = next++;
    float f0 = 0, f025 = 0.25f, f05 = 0.5f; uint32_t b0, b025, b05; memcpy(&b0, &f0, 4); memcpy(&b025, &f025, 4); memcpy(&b05, &f05, 4);
    memcpy(out, w, 5 * 4); size_t no = 5; int placed = 0, capped = has_cap;
    for (size_t i = 5; i < n; ) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (op == 54 && !placed) {   /* types and constants before the first function */
            if (!i32) { out[no++] = 4u << 16 | 21; out[no++] = I32; out[no++] = 32; out[no++] = 1; }
            if (!v2i) { out[no++] = 4u << 16 | 23; out[no++] = V2I; out[no++] = I32; out[no++] = 2; }
            uint32_t d[] = {4u << 16 | 43, I32, ci0, 0, 4u << 16 | 43, f32, cf0, b0, 4u << 16 | 43, f32, cf025, b025, 4u << 16 | 43, f32, cf05, b05,
                            5u << 16 | 44, v2f, cv025, cf025, cf025, 5u << 16 | 44, v2f, cv05, cf05, cf05};
            memcpy(out + no, d, sizeof d); no += sizeof d / 4; placed = 1;
        }
        uint32_t st = 0, it = 0;
        if (op == 87 && (len == 5 || (len == 6 && !w[i + 5])) && w[i + 3] < bound && w[i + 4] < bound) { st = rtype[w[i + 3]]; it = st < bound && ty_op[st] == 27 ? ty_b[st] : 0; }
        if (it && ty_op[it] == 25 && ty_a[it] == 1 && rtype[w[i + 4]] == v2f) {
            uint32_t img = next++, sz = next++, szf = next++, t = next++, t2 = next++, fl = next++, t3 = next++, uv = next++;
            uint32_t d[] = {4u << 16 | 100, it, img, w[i + 3],            /* OpImage */
                            5u << 16 | 103, V2I, sz, img, ci0,              /* OpImageQuerySizeLod */
                            4u << 16 | 111, v2f, szf, sz,                   /* OpConvertSToF */
                            5u << 16 | 133, v2f, t, w[i + 4], szf,          /* uv * size */
                            5u << 16 | 129, v2f, t2, t, cv025,              /* + 0.25 */
                            6u << 16 | 12, v2f, fl, glsl, 8, t2,            /* Floor */
                            5u << 16 | 129, v2f, t3, fl, cv05,              /* + 0.5 */
                            5u << 16 | 136, v2f, uv, t3, szf,               /* / size */
                            7u << 16 | 88, w[i + 1], w[i + 2], w[i + 3], uv, 2, cf0};   /* OpImageSampleExplicitLod, Lod 0 */
            memcpy(out + no, d, sizeof d); no += sizeof d / 4;
        } else {
            memcpy(out + no, w + i, len * 4); no += len;
            if (op == 17 && !capped) { out[no++] = 2u << 16 | 17; out[no++] = 50; capped = 1; }   /* OpCapability ImageQuery */
        }
        i += len;
    }
    if (!placed) goto fail;
    out[3] = next; *out_n = no;
    free(rtype); free(ty_op); free(ty_a); free(ty_b);
    return out;
fail:
    free(rtype); free(ty_op); free(ty_a); free(ty_b); free(out);
    return NULL;
}
static int class_snap_on(void) {
    static int on = -1; static unsigned g;
    if (on < 0 || g != nvk_live_gen) { g = nvk_live_gen; const char *e = nvk_live_getenv("TOTK_VK_CLASS_SNAP"); on = !(e && !strcmp(e, "0")); }
    return on;
}
static uint8_t *depth_offset_reply(const uint8_t *data, size_t len, float delta, size_t *out_len) {
    if (!shader_data_valid(data, len)) return NULL;
    long spv, nb, ni, no; unsigned lx, ly, lz;
    if (sscanf((const char *)data, "OK %ld %ld %ld %ld %u %u %u", &spv, &nb, &ni, &no, &lx, &ly, &lz) != 7) return NULL;
    const uint8_t *table = memchr(data, '\n', len) + 1, *spv_at = data + len - spv;
    uint32_t *words = malloc((size_t)spv), *rewritten; size_t nw;
    if (!words) return NULL;
    memcpy(words, spv_at, (size_t)spv);
    rewritten = spv_depth_offset(words, (size_t)spv / 4, delta, &nw);
    free(words);
    if (!rewritten) return NULL;
    if (class_snap_on()) {   /* a writer without a 2D texture() keeps the depth offset alone */
        size_t ns; uint32_t *snapped = spv_snap_samples(rewritten, nw, &ns);
        if (snapped) { free(rewritten); rewritten = snapped; nw = ns; }
    }
    Str r = {0};
    put(&r, "OK %zu %ld %ld %ld %u %u %u\n%.*s", nw * 4, nb, ni, no, lx, ly, lz, (int)(spv_at - table), (const char *)table);
    size_t head = r.n;
    r.p = realloc(r.p, head + nw * 4);
    if (!r.p) nvk_die("shader reply allocation");
    memcpy(r.p + head, rewritten, nw * 4);
    free(rewritten);
    *out_len = head + nw * 4;
    return (uint8_t *)r.p;
}
static void depth_variant_name(char name[80], const char *parent, float delta, int snap) {
    uint32_t bits; memcpy(&bits, &delta, sizeof bits);
    Sha256 hash; uint8_t digest[32]; char key[65];
    sha_init(&hash); sha_update(&hash, parent, strlen(parent)); sha_final(&hash, digest); digest_hex(digest, key);
    /* Keep room for downstream -i / -F variants, while hashing the entire parent name. */
    snprintf(name, 80, "%.32s-%s%08x", key, snap ? "zs" : "zq", bits);
}
static void inherit_c1(NvkShader *sh, const NvkShader *parent) {
    if (sh->c1) return;
    if (!parent->c1_memory || !parent->c1_size) { attach_c1(sh, NULL, 0); return; }
    void *m; VKCHECK(vkMapMemory(nvk_dev, parent->c1_memory, 0, VK_WHOLE_SIZE, 0, &m), "map parent c1 buffer");
    attach_c1(sh, m, parent->c1_size); vkUnmapMemory(nvk_dev, parent->c1_memory);
}
NvkShader *nvk_shader_depth_offset(NvkShader *ps, float delta) {
    char name[80];
    depth_variant_name(name, ps->variant, delta, class_snap_on());
    pthread_mutex_lock(&lock);
    Variant **v = slot_for(name);
    if (*v) { NvkShader *sh = (*v)->shader; inherit_c1(sh, ps); pthread_mutex_unlock(&lock); return sh; }
    char parent[640], path[640];
    snprintf(path, sizeof path, "%s/%s.vks", cache_dir(), name);
    size_t plen = 0, len = 0;
    uint8_t *data = read_shader_cache(path, &len);
    if (!data) {
        snprintf(parent, sizeof parent, "%s/%s.vks", cache_dir(), ps->variant);
        uint8_t *pdata = read_file(parent, &plen);
        data = pdata ? depth_offset_reply(pdata, plen, delta, &len) : NULL;
        free(pdata);
        if (data) write_file(path, data, len);
    }
    if (!data) {
        pthread_mutex_unlock(&lock);
        static unsigned said; if (said++ < 8) VKLOG("fragment variant %s: gl_FragDepth offset not built (%s)", ps->variant, plen ? "malformed" : "not cached");
        return NULL;
    }
    NvkShader *sh = parse_module(name, 1, data, len);
    inherit_c1(sh, ps); free(data);
    Variant *n = calloc(1, sizeof *n);
    snprintf(n->name, sizeof n->name, "%s", name); n->shader = sh; *v = n;
    pthread_mutex_unlock(&lock);
    VKLOG("fragment variant %s: gl_FragDepth stores shifted by %g", name, delta);
    nvk_pipeline_warm_signal();
    return sh;
}

/* ---------- Framebuffer fetch (decals reading the target they render into) ----------
 * The proven samples (load_fetch) become reads of an input attachment at the fragment's own pixel: OpImageSample*
 * -> OpLoad of a SubpassData image + OpImageRead at (0, 0), with InputAttachmentIndex = the colour attachment, set 1,
 * binding 64 + index. With VK_KHR_dynamic_rendering_local_read that reads the pass's own contents: no pass split, no
 * copy (nvn_vk_draw.c chooses it per draw). NULL on SPIR-V this does not recognise.
 * att 8 = the depth attachment (TOTK_VK_FETCH_DEPTH): binding 72 without InputAttachmentIndex, which local read maps
 * to the depth attachment (pDepthInputAttachmentIndex NULL). Its read goes through OpVectorShuffle with (0, 0, 0, 1),
 * channel c taking index zsw >> 4c & 7 (0: depth, 4/5/6: zero, 7: one), so it returns what the sampled view's
 * component mapping returned, independent of what the driver puts in the other three channels. */
static uint32_t *spv_fetch(const uint32_t *w, size_t n, const uint32_t *ids, const uint8_t *att, int count, uint16_t zsw, size_t *out_n) {
    if (n < 5 || w[0] != 0x07230203 || w[3] > (1u << 22)) return NULL;
    uint32_t bound = w[3], F = 0, I = 0, V2 = 0, V4 = 0;
    uint8_t *vec4f = calloc(bound, 1);   /* float vec4 types: what an input attachment read returns */
    if (!vec4f) return NULL;
    int has_cap = 0; size_t first_fn = 0, first_type = 0, first_cap_end = 0;
    for (size_t i = 5; i < n; ) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) { free(vec4f); return NULL; }
        if (op == 17) { if (w[i + 1] == 40) has_cap = 1; first_cap_end = i + len; }
        if (op == 22 && len >= 3 && w[i + 2] == 32 && !F) F = w[i + 1];
        if (op == 21 && len >= 4 && w[i + 2] == 32 && w[i + 3] == 1 && !I) I = w[i + 1];
        if (!first_type && ((op >= 19 && op <= 39) || (op >= 41 && op <= 52) || op == 59)) first_type = i;
        if (op == 54 && !first_fn) first_fn = i;
        i += len;
    }
    for (size_t i = 5; i < n; i += w[i] >> 16) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (op == 23 && len == 4 && w[i + 2] == F && w[i + 3] == 4 && w[i + 1] < bound) { vec4f[w[i + 1]] = 1; if (!V4) V4 = w[i + 1]; }
        if (op == 23 && len == 4 && I && w[i + 2] == I && w[i + 3] == 2 && !V2) V2 = w[i + 1];
    }
    if (!F || !first_fn || !first_type || !first_cap_end) { free(vec4f); return NULL; }
    uint32_t next = bound;
    uint32_t newI = I ? 0 : next++, newV2 = V2 ? 0 : next++;
    if (!I) I = newI;
    if (!V2) V2 = newV2;
    uint32_t imgT = next++, ptrT = next++, nullC = next++, var[9] = {0};
    for (int k = 0; k < count; ++k) if (att[k] < 9 && !var[att[k]]) var[att[k]] = next++;
    uint32_t f0 = 0, f1 = 0, K = 0;   /* depth: constants 0.0, 1.0 and (0, 0, 0, 1) */
    if (var[8]) { if (!V4) { free(vec4f); return NULL; } f0 = next++; f1 = next++; K = next++; }
    uint32_t *out = malloc((n + 96 + 24 * (size_t)count) * 4);
    if (!out) { free(vec4f); return NULL; }
    size_t o = 0, replaced = 0;
    memcpy(out, w, 5 * 4); o = 5;
    for (size_t i = 5; i < n; ) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (i == first_type)   /* annotations end here: decorate the new variables */
            for (uint32_t j = 0; j < 9; ++j) if (var[j]) {
                uint32_t d[] = {4u << 16 | 71, var[j], 34, 1, 4u << 16 | 71, var[j], 33, 64 + j, 4u << 16 | 71, var[j], 43, j};
                size_t nd = j == 8 ? 8 : sizeof d / 4;   /* depth: no InputAttachmentIndex */
                memcpy(out + o, d, nd * 4); o += nd;
            }
        if (i == first_fn) {   /* types, the null coordinate and the variables, before the first function */
            if (newI) { out[o++] = 4u << 16 | 21; out[o++] = newI; out[o++] = 32; out[o++] = 1; }
            if (newV2) { out[o++] = 4u << 16 | 23; out[o++] = newV2; out[o++] = I; out[o++] = 2; }
            uint32_t d[] = {9u << 16 | 25, imgT, F, 6, 0, 0, 0, 2, 0, 4u << 16 | 32, ptrT, 0, imgT, 3u << 16 | 46, V2, nullC};
            memcpy(out + o, d, sizeof d); o += sizeof d / 4;
            if (K) {
                uint32_t c[] = {4u << 16 | 43, F, f0, 0, 4u << 16 | 43, F, f1, 0x3f800000u, 7u << 16 | 44, V4, K, f0, f0, f0, f1};
                memcpy(out + o, c, sizeof c); o += sizeof c / 4;
            }
            for (uint32_t j = 0; j < 9; ++j) if (var[j]) { out[o++] = 4u << 16 | 59; out[o++] = ptrT; out[o++] = var[j]; out[o++] = 0; }
        }
        int k = -1;
        if ((op == 87 || op == 88) && len >= 5)
            for (int q = 0; q < count; ++q) if (ids[q] == w[i + 2] && att[q] < 9) k = q;
        if (k >= 0) {
            if (!vec4f[w[i + 1] < bound ? w[i + 1] : 0]) { free(out); free(vec4f); return NULL; }
            uint32_t tmp = next++;
            if (att[k] == 8) {
                uint32_t raw = next++;
                uint32_t d[] = {4u << 16 | 61, imgT, tmp, var[8], 5u << 16 | 98, w[i + 1], raw, tmp, nullC,
                    9u << 16 | 79, w[i + 1], w[i + 2], raw, K, zsw & 7u, zsw >> 4 & 7u, zsw >> 8 & 7u, zsw >> 12 & 7u};
                memcpy(out + o, d, sizeof d); o += sizeof d / 4;
            } else {
                uint32_t d[] = {4u << 16 | 61, imgT, tmp, var[att[k]], 5u << 16 | 98, w[i + 1], w[i + 2], tmp, nullC};
                memcpy(out + o, d, sizeof d); o += sizeof d / 4;
            }
            replaced++;
        } else { memcpy(out + o, w + i, len * 4); o += len; }
        if (!has_cap && i + len == first_cap_end) { out[o++] = 2u << 16 | 17; out[o++] = 40; has_cap = 1; }   /* InputAttachment */
        i += len;
    }
    free(vec4f);
    if (!replaced) { free(out); return NULL; }
    out[3] = next;
    *out_n = o;
    return out;
}
static uint8_t *fetch_reply(const uint8_t *data, size_t len, const NvkShader *ps, const uint8_t att[8], uint16_t zsw, size_t *out_len) {
    if (!shader_data_valid(data, len)) return NULL;
    long spv, nb, ni, no; unsigned lx, ly, lz;
    if (sscanf((const char *)data, "OK %ld %ld %ld %ld %u %u %u", &spv, &nb, &ni, &no, &lx, &ly, &lz) != 7) return NULL;
    const uint8_t *p = memchr(data, '\n', len) + 1, *spv_at = data + len - spv;
    uint32_t ids[8]; uint8_t a[8]; int count = 0; uint16_t used = 0; uint8_t slot_att[128]; memset(slot_att, 0xff, sizeof slot_att);
    for (int k = 0; k < ps->nfetch; ++k) { ids[count] = ps->fetch[k].id; a[count++] = att[k]; if (att[k] < 9) { used |= (uint16_t)(1u << att[k]); slot_att[ps->fetch[k].slot] = att[k]; } }
    for (int k = 0; k < ps->nfetch; ++k) if (att[k] >= 9 && slot_att[ps->fetch[k].slot] < 9) return NULL;   /* a slot is fetched wholly or not at all */
    uint32_t *words = malloc((size_t)spv), *rewritten; size_t nw;
    if (!words) return NULL;
    memcpy(words, spv_at, (size_t)spv);
    rewritten = spv_fetch(words, (size_t)spv / 4, ids, a, count, zsw, &nw);
    free(words);
    if (!rewritten) return NULL;
    /* The input attachment rows join the binding rows: the parser reads the header's count of B lines first, then
     * the I/O lines (appended after those, the first Thor run with fetch read an empty binding and stopped). */
    Str table = {0}, tail = {0}; long rows = 0;
    while (p < spv_at) {
        const uint8_t *e = memchr(p, '\n', (size_t)(spv_at - p));
        int set, binding, kind, reg, rest;
        if (p[0] == 'B' && sscanf((const char *)p, "B %d %d %d %d%n", &set, &binding, &kind, &reg, &rest) == 4 && kind == 3 && reg >= 0 && reg < 128 && slot_att[reg] < 9) {
            put(&table, "B %d %d 15 %d%.*s\n", set, binding, reg, (int)(e - p - rest), (const char *)p + rest);   /* sampled no more */
        } else put(p[0] == 'B' ? &table : &tail, "%.*s\n", (int)(e - p), (const char *)p);
        if (p[0] == 'B') ++rows;
        p = e + 1;
    }
    for (int j = 0; j < 9; ++j) if (used >> j & 1) { put(&table, "B 1 %d 14 %d -1 0 0 10 1 -1\n", 64 + j, j); ++rows; }
    if (tail.p) put(&table, "%s", tail.p);
    free(tail.p);
    Str r = {0};
    put(&r, "OK %zu %ld %ld %ld %u %u %u\n%s", nw * 4, rows, ni, no, lx, ly, lz, table.p ? table.p : "");
    size_t head = r.n;
    r.p = realloc(r.p, head + nw * 4);
    if (!r.p) nvk_die("shader reply allocation");
    memcpy(r.p + head, rewritten, nw * 4);
    free(rewritten); free(table.p);
    *out_len = head + nw * 4;
    return (uint8_t *)r.p;
}
static uint64_t fetch_built, fetch_refused;
NvkShader *nvk_shader_fetch_variant(NvkShader *ps, const uint8_t att[8]) { return nvk_shader_fetch_variant_z(ps, att, 0x7650); }
NvkShader *nvk_shader_fetch_variant_z(NvkShader *ps, const uint8_t att[8], uint16_t zsw) {
    char name[80]; uint32_t code = 0; int depth = 0;
    for (int k = 0; k < 8; ++k) { code |= (uint32_t)((k < ps->nfetch && att[k] < 9 ? att[k] : 0xf)) << (4 * k); depth |= k < ps->nfetch && att[k] == 8; }
    if (depth) snprintf(name, sizeof name, "%.56s-F%08x-Z%04x", ps->variant, code, zsw);
    else snprintf(name, sizeof name, "%.60s-F%08x", ps->variant, code);
    pthread_mutex_lock(&lock);
    Variant **v = slot_for(name);
    if (*v) {
        NvkShader *sh = (*v)->shader;
        if (sh) inherit_c1(sh, ps);   /* persisted fetch variants are preloaded without constants */
        pthread_mutex_unlock(&lock);
        return sh;   /* NULL: refused before */
    }
    char path[640]; snprintf(path, sizeof path, "%s/%s.vks", cache_dir(), name);
    size_t len = 0;
    uint8_t *data = read_shader_cache(path, &len);
    if (!data) {
        char parent[640]; snprintf(parent, sizeof parent, "%s/%s.vks", cache_dir(), ps->variant);
        size_t plen = 0; uint8_t *pdata = read_file(parent, &plen);
        data = pdata ? fetch_reply(pdata, plen, ps, att, zsw, &len) : NULL;
        free(pdata);
        if (data) write_file(path, data, len);
    }
    NvkShader *sh = NULL;
    if (data) {
        void *c1 = calloc(1, ps->c1_size ? ps->c1_size : 256);
        if (ps->c1_memory) { void *m; VKCHECK(vkMapMemory(nvk_dev, ps->c1_memory, 0, VK_WHOLE_SIZE, 0, &m), "map c1 buffer"); memcpy(c1, m, ps->c1_size); vkUnmapMemory(nvk_dev, ps->c1_memory); }
        sh = parse(name, 1, data, len, c1, ps->c1_size);
        free(c1); free(data);
        if (++fetch_built <= 8 || fetch_built % 100 == 0) VKLOG("framebuffer fetch variant %s (%llu built)", name, (unsigned long long)fetch_built);
    } else if (++fetch_refused <= 8 || fetch_refused % 100 == 0) VKLOG("framebuffer fetch: variant of %s refused (%llu so far): sampled as before", ps->variant, (unsigned long long)fetch_refused);
    Variant *n = calloc(1, sizeof *n);
    if (!n) nvk_die("shader variant allocation");
    snprintf(n->name, sizeof n->name, "%s", name); n->shader = sh; *v = n;   /* a refusal is remembered too */
    pthread_mutex_unlock(&lock);
    if (sh) nvk_pipeline_warm_signal();
    return sh;
}

/* ---------- Variants shared across unused bound state ----------
 * A variant name hashes every bound slot (answers), but a translation only
 * depends on the texture/image slots in its binding table, the vertex inputs
 * it reads and the colour outputs it writes; totk_shader_rt.m keys on the
 * translator's deps the same way. Before loading or translating a new name,
 * a variant of the same shader whose used slots had the same answers is
 * reused, so stale bindings do not multiply modules and pipelines.
 * TOTK_VK_VARIANT_SHARE=0: off. */
typedef struct { uint32_t tex[128], img[32]; uint8_t attr[16], fout[8], dmw; } Answers;
typedef struct Shared { char key[33]; NvkShader *shader; Answers at; struct Shared *next; } Shared;
static Shared *shared[1024];
static uint64_t shared_reuses;
static int share_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_VK_VARIANT_SHARE"); on = !(e && !strcmp(e, "0")); }
    return on;
}
/* The numeric form of answers(): equal codes mean equal JSON members. */
static void answers_now(Answers *a, int st) {
    memset(a, 0, sizeof *a);
    a->dmw = (uint8_t)totk_depth_near_minus_w();
    if (st == 0) for (int i = 0; i < nvk_S.nattr && i < 16; ++i) a->attr[i] = (uint8_t)(1 + fmt_class_id(nvk_S.attr[i].fmt));
    const p_set_rt *rt = nvk_render_targets();
    if (st == 1 && rt) for (int i = 0; i < rt->n && i < 8; ++i) if (rt->color[i])
        a->fout[i] = (uint8_t)(1 + fmt_class_id(rt->has_cview[i] && rt->cview[i].set_format ? rt->cview[i].format : rt->color[i]->d.format));
    for (int r = 0; r < 32; ++r) if (nvk_S.img[st][r]) a->img[r] = nvk_texture_info((int)(nvk_S.img[st][r] & 0xfffff));
    for (int r = 0; r < 128; ++r) if (nvk_S.tex[st][r]) {
        uint32_t info = nvk_texture_info((int)(nvk_S.tex[st][r] & 0xfffff));
        int f = (int)((info >> 8) & 0xffff);
        a->tex[r] = info ? (info & 0xff) | (f >= 0x32 && f <= 0x36 ? 0x100u : 0) | 0x200u : 0;
    }
}
static int answers_match(const NvkShader *s, const Answers *was, const Answers *now) {
    if (was->dmw != now->dmw) return 0;
    for (int i = 0; i < s->nb; ++i) {
        const NvkBinding *b = &s->b[i];
        if ((b->kind == 3 || b->kind == 5) && b->reg >= 0 && b->reg < 128 && was->tex[b->reg] != now->tex[b->reg]) return 0;
        if (b->kind == 4 && b->reg >= 0 && b->reg < 32 && was->img[b->reg] != now->img[b->reg]) return 0;
    }
    if (s->stage == 0) for (int i = 0; i < s->nin; ++i) if (s->in[i].loc < 16 && was->attr[s->in[i].loc] != now->attr[s->in[i].loc]) return 0;
    if (s->stage == 1) for (int l = 0; l < 8; ++l) if ((s->out_mask >> l & 1) && was->fout[l] != now->fout[l]) return 0;
    return 1;
}
static unsigned shared_slot(const char *key) { unsigned h = 0; for (int i = 0; i < 32 && key[i]; ++i) h = h * 131 + (unsigned char)key[i]; return h % 1024; }
/* Caller holds lock. */
static NvkShader *shared_find(const char *key, const Answers *now) {
    for (Shared *e = shared[shared_slot(key)]; e; e = e->next)
        if (!strncmp(e->key, key, 32) && answers_match(e->shader, &e->at, now)) return e->shader;
    return NULL;
}
static void shared_add(const char *key, NvkShader *sh, const Answers *at) {
    if (sh->shared) return;
    Shared *e = calloc(1, sizeof *e);
    if (!e) nvk_die("shared variant allocation");
    snprintf(e->key, sizeof e->key, "%.32s", key); e->shader = sh; e->at = *at; sh->shared = 1;
    unsigned h = shared_slot(key); e->next = shared[h]; shared[h] = e;
}

/* Prewarm: module and layout of a recorded variant from the on-device cache,
 * so its pipelines can be built before the first draw. Never asks the bridge. */
int nvk_shader_preload(const char *name, int stage) {
    if (!name || !*name) return 0;
    if (nvk_shader_find_variant(name)) return 1;
    char path[640];
    snprintf(path, sizeof path, "%s/%s.vks", cache_dir(), name);
    size_t len = 0;
    uint8_t *data = read_file(path, &len);
    if (!data) return 0;
    if (!shader_data_valid(data, len)) { free(data); return 0; }
    NvkShader *sh = parse_module(name, stage, data, len);
    free(data);
    pthread_mutex_lock(&lock);
    Variant **v = slot_for(name);
    if (*v) {   /* a draw loaded it meanwhile */
        pthread_mutex_unlock(&lock);
        vkDestroyShaderModule(nvk_dev, sh->module, NULL); vkDestroyDescriptorSetLayout(nvk_dev, sh->layout, NULL);
        free(sh->b); free(sh);
        return 1;
    }
    Variant *n = calloc(1, sizeof *n);
    if (!n) nvk_die("shader variant allocation");
    snprintf(n->name, sizeof n->name, "%s", name); n->shader = sh; *v = n;
    pthread_mutex_unlock(&lock);
    return 1;
}

/* ---------- Seeded variants (tools/android_shader_seed.py) ----------
 * seed.idx in the shader cache names every variant translated ahead of play, with the answer each slot it uses was
 * translated for. A draw whose answers agree on all of them takes that variant: no bridge for anything the seed
 * covers. A slot the answers leave out reads the translator's default (2d texture, float input or output), so an
 * unbound slot here compares as that default, as the translator saw it. */
typedef struct SeedEntry { char key[33], variant[48]; uint8_t stage, n; uint32_t at; struct SeedEntry *next; } SeedEntry;
enum { SEED_TEX, SEED_IMG, SEED_ATTR, SEED_FOUT };
#define SEED_SLOT(kind, reg, val) ((uint16_t)((kind) << 14 | ((reg) & 0x7f) << 7 | ((val) & 0x7f)))
static SeedEntry *seed_buckets[8192];
static uint16_t *seed_slots; static size_t seed_nslots;
static uint64_t seed_count, seed_hits, seed_near, seed_missing;
static unsigned seed_bucket(const char *key) { unsigned h = 0; for (int i = 0; i < 32 && key[i]; ++i) h = h * 131 + (unsigned char)key[i]; return h % 8192; }
/* Caller holds lock. */
static void seed_load(void) {
    static int done;
    if (done) return;
    done = 1;
    char path[640];
    snprintf(path, sizeof path, "%s/seed.idx", cache_dir());
    FILE *f = fopen(path, "r");
    if (!f) { VKLOG("no shader seed (%s): variants come from the cache and the bridge", path); return; }
    size_t cap = 1 << 16; seed_slots = malloc(cap * sizeof *seed_slots);
    char line[1024];
    while (seed_slots && fgets(line, sizeof line, f)) {
        char key[40], variant[64]; int stage, used;
        if (sscanf(line, "%39s %63s %d%n", key, variant, &stage, &used) != 3 || strlen(key) != 32 || strlen(variant) >= 48) continue;
        SeedEntry *e = calloc(1, sizeof *e);
        if (!e) break;
        memcpy(e->key, key, 33); snprintf(e->variant, sizeof e->variant, "%s", variant); e->stage = (uint8_t)stage; e->at = (uint32_t)seed_nslots;
        char *save = NULL;
        for (char *t = strtok_r(line + used, " \n", &save); t; t = strtok_r(NULL, " \n", &save)) {
            int reg = 0, val = 0, kind = t[0] == 't' ? SEED_TEX : t[0] == 'i' ? SEED_IMG : t[0] == 'a' ? SEED_ATTR : t[0] == 'o' ? SEED_FOUT : -1;
            if (kind < 0 || sscanf(t + 1, "%d:%d", &reg, &val) < 1 || reg < 0 || reg > 127) continue;
            if (seed_nslots == cap) { uint16_t *g = realloc(seed_slots, (cap *= 2) * sizeof *g); if (!g) break; seed_slots = g; }
            seed_slots[seed_nslots++] = SEED_SLOT(kind, reg, val);
            if (++e->n == 255) break;
        }
        unsigned h = seed_bucket(key); e->next = seed_buckets[h]; seed_buckets[h] = e; ++seed_count;
    }
    fclose(f);
    VKLOG("shader seed: %llu variants (%zu slot answers) from %s", (unsigned long long)seed_count, seed_nslots, path);
}
void nvk_shader_seed_load(void) { pthread_mutex_lock(&lock); seed_load(); pthread_mutex_unlock(&lock); }
/* The value a seeded slot answer is compared with, from the bound state now. */
static int seed_value_now(int st, int kind, int reg) {
    switch (kind) {
    case SEED_TEX: {
        uint32_t info = nvk_S.tex[st][reg] ? nvk_texture_info((int)(nvk_S.tex[st][reg] & 0xfffff)) : 0;
        if (!info) return 2;
        static const uint8_t ids[11] = {1, 2, 3, 4, 5, 6, 7, 2, 8, 9, 10};   /* target_name() */
        int t = (int)(info & 0xff), f = (int)((info >> 8) & 0xffff);
        return (t < 11 ? ids[t] : 2) + (f >= 0x32 && f <= 0x36 ? 32 : 0);
    }
    case SEED_IMG: return reg < 32 && nvk_S.img[st][reg] && nvk_texture_info((int)(nvk_S.img[st][reg] & 0xfffff)) ? 1 : 0;
    case SEED_ATTR: return st == 0 && reg < nvk_S.nattr && reg < 16 ? 1 + fmt_class_id(nvk_S.attr[reg].fmt) : 1;
    default: {
        const p_set_rt *rt = nvk_render_targets();
        if (!rt || reg >= rt->n || reg >= 8 || !rt->color[reg]) return 1;
        return 1 + fmt_class_id(rt->has_cview[reg] && rt->cview[reg].set_format ? rt->cview[reg].format : rt->color[reg]->d.format);
    }
    }
}
/* Caller holds lock. The seeded variant of this shader whose used slots all agree with the bound state; with
 * nearest, otherwise the one agreeing on most of them (only when no bridge can translate the exact one). */
static const SeedEntry *seed_find(const char *key, int st, int nearest) {
    seed_load();
    if (!seed_count || !totk_depth_near_minus_w()) return NULL;   /* seeds are translated for TOTK's depth mode */
    const SeedEntry *best = NULL; int best_score = -1;
    for (const SeedEntry *e = seed_buckets[seed_bucket(key)]; e; e = e->next) {
        if (e->stage != st || strncmp(e->key, key, 32)) continue;
        int score = 0;
        for (unsigned i = 0; i < e->n; ++i) {
            uint16_t s = seed_slots[e->at + i];
            score += seed_value_now(st, s >> 14, s >> 7 & 0x7f) == (s & 0x7f);
        }
        if (score == e->n) return e;
        if (nearest && score > best_score) { best = e; best_score = score; }
    }
    return best;
}
/* Caller holds lock. The seeded variant's shader, loaded once under its own name (zero_inputs reads its file). */
static NvkShader *seed_take(const SeedEntry *e, int st, const uint8_t *c1, uint32_t c1_len) {
    Variant **v = slot_for(e->variant);
    if (*v) { if (!(*v)->shader->c1) attach_c1((*v)->shader, c1, c1_len); return (*v)->shader; }
    char path[640];
    snprintf(path, sizeof path, "%s/%s.vks", cache_dir(), e->variant);
    size_t len = 0;
    uint8_t *data = read_shader_cache(path, &len);
    if (!data) return NULL;
    NvkShader *sh = parse(e->variant, st, data, len, c1, c1_len);
    free(data);
    Variant *n = calloc(1, sizeof *n);
    if (!n) nvk_die("shader variant allocation");
    snprintf(n->name, sizeof n->name, "%s", e->variant); n->shader = sh; *v = n;
    return sh;
}

/* ---------- Fast path: (program, stage, bound-state signature) -> variant ----------
 * The signature covers exactly what answers() describes, as numbers. */
static uint64_t mix(uint64_t h, uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); return h * 0x100000001b3ull; }
static uint64_t signature(int st) {
    uint64_t h = mix(0x51ed270b27ull, (uint64_t)st);
    h = mix(h, (uint64_t)totk_depth_near_minus_w());
    if (st == 0) { h = mix(h, (uint64_t)nvk_S.nattr); for (int i = 0; i < nvk_S.nattr && i < 16; ++i) h = mix(h, nvk_S.attr[i].fmt); }
    const p_set_rt *rt = nvk_render_targets();
    if (st == 1 && rt) for (int i = 0; i < rt->n && i < 8; ++i) if (rt->color[i])
        h = mix(h, (uint64_t)i << 32 | (uint32_t)(rt->has_cview[i] && rt->cview[i].set_format ? rt->cview[i].format : rt->color[i]->d.format));
    for (int r = 0; r < 32; ++r) if (nvk_S.img[st][r]) h = mix(h, (uint64_t)(r + 256) << 32 | nvk_texture_info((int)(nvk_S.img[st][r] & 0xfffff)));
    for (int r = 0; r < 128; ++r) if (nvk_S.tex[st][r]) {
        uint32_t info = nvk_texture_info((int)(nvk_S.tex[st][r] & 0xfffff));
        int f = (int)((info >> 8) & 0xffff);
        /* only the target and the depth flag reach the translator for textures */
        h = mix(h, (uint64_t)r << 32 | (info ? (info & 0xff) | (f >= 0x32 && f <= 0x36 ? 0x100u : 0) | 0x200u : 0));
    }
    return h;
}
/* The texture and image registers a program stage's code uses (from the bindings of its first translated variant):
   the translation depends only on those (shared variants, share_on), so once known the signature hashes them alone
   instead of all 128 + 32 registers with a registration lookup each. The full signature was recomputed for ~3500
   draws a frame, at every binding change (23 Oct, Thor). TOTK_VK_SIG_USED=0: all registers, as before. */
static struct { uint64_t serial; unsigned stage; uint64_t tex[2]; uint32_t img; uint8_t known; } used_regs[8192];
static uint64_t sig_used_hits;
static int sig_used_on(void) {
    static int on = -1;
    static unsigned live_TOTK_VK_SIG_USED; if (on < 0 || live_TOTK_VK_SIG_USED != nvk_live_gen) { live_TOTK_VK_SIG_USED = nvk_live_gen; const char *e = nvk_live_getenv("TOTK_VK_SIG_USED"); on = !(e && !strcmp(e, "0")); }
    return on;
}
static uint64_t signature_used(int st, const typeof(used_regs[0]) *u) {
    uint64_t h = mix(0x51ed270b27ull, (uint64_t)st);
    h = mix(h, (uint64_t)totk_depth_near_minus_w());
    if (st == 0) { h = mix(h, (uint64_t)nvk_S.nattr); for (int i = 0; i < nvk_S.nattr && i < 16; ++i) h = mix(h, nvk_S.attr[i].fmt); }
    const p_set_rt *rt = nvk_render_targets();
    if (st == 1 && rt) for (int i = 0; i < rt->n && i < 8; ++i) if (rt->color[i])
        h = mix(h, (uint64_t)i << 32 | (uint32_t)(rt->has_cview[i] && rt->cview[i].set_format ? rt->cview[i].format : rt->color[i]->d.format));
    for (uint32_t m = u->img; m; m &= m - 1) {
        int r = __builtin_ctz(m);
        if (nvk_S.img[st][r]) h = mix(h, (uint64_t)(r + 256) << 32 | nvk_texture_info((int)(nvk_S.img[st][r] & 0xfffff)));
    }
    for (int w = 0; w < 2; ++w) for (uint64_t m = u->tex[w]; m; m &= m - 1) {
        int r = w * 64 + __builtin_ctzll(m);
        if (!nvk_S.tex[st][r]) continue;
        uint32_t info = nvk_texture_info((int)(nvk_S.tex[st][r] & 0xfffff));
        int f = (int)((info >> 8) & 0xffff);
        h = mix(h, (uint64_t)r << 32 | (info ? (info & 0xff) | (f >= 0x32 && f <= 0x36 ? 0x100u : 0) | 0x200u : 0));
    }
    return mix(h, 0x75736564ull);   /* never equal to a full signature */
}
typedef struct Fast { uint64_t serial, sig; unsigned stage; NvkShader *shader; int nvn_stage; } Fast;
static Fast fast[32768];   /* two ways per slot (slot and slot^1): direct-mapped, 16384 slots let ~3% of the draws fall through to shader_slow (answer text, SHA-256, lock) in the 25 Oct Thor profile */

static NvkShader *shader_slow(const prog_t *p, unsigned i, int *nvn_stage);
/* nvn_vk_draw.c: the epochs a stage's signature depends on; 0 = not cacheable (host tests without the draw layer). */
__attribute__((weak)) int nvk_sig_epochs(int st, uint64_t e[4]) { (void)st; (void)e; return 0; }
static struct { uint64_t e[4], sig; int valid; } sig_cache[6];
static uint64_t sig_hits, sig_misses;
void nvk_sig_stats(uint64_t *hits, uint64_t *misses) { *hits = sig_hits + sig_used_hits; *misses = sig_misses; }
NvkShader *nvk_shader_for(const prog_t *p, unsigned i, int *nvn_stage) {
    uint32_t st;
    memcpy(&st, (const uint8_t *)p->sd[i].control + 0x714, 4);
    int sst = (int)(st <= 5 ? st : 0);
    uint64_t e[4], sig;
    typeof(used_regs[0]) *u = &used_regs[mix(p->serial, i) % 8192];
    int by_used = sig_used_on() && share_on();
    if (by_used && u->known && u->serial == p->serial && u->stage == i) { sig = signature_used(sst, u); sig_used_hits++; }
    else {
        int cacheable = nvk_sig_epochs(sst, e);
        if (cacheable && sig_cache[sst].valid && !memcmp(sig_cache[sst].e, e, sizeof e)) { sig = sig_cache[sst].sig; sig_hits++; }
        else {
            sig = signature(sst); sig_misses++;
            if (cacheable) { memcpy(sig_cache[sst].e, e, sizeof e); sig_cache[sst].sig = sig; sig_cache[sst].valid = 1; }
        }
    }
    Fast *f = &fast[(mix(mix(p->serial, i), sig) >> 17) % 32768], *f2 = &fast[(unsigned)(f - fast) ^ 1u];
    if (f->shader && f->serial == p->serial && f->stage == i && f->sig == sig) { *nvn_stage = f->nvn_stage; return f->shader; }
    if (f2->shader && f2->serial == p->serial && f2->stage == i && f2->sig == sig) { *nvn_stage = f2->nvn_stage; return f2->shader; }
    NvkShader *s = shader_slow(p, i, nvn_stage);
    *f2 = *f;   /* the newest entry takes the first way, the one it displaces the second */
    *f = (Fast){p->serial, sig, i, s, *nvn_stage};
    if (by_used && s && !(u->known && u->serial == p->serial && u->stage == i)) {   /* learn the used registers */
        u->serial = p->serial; u->stage = i; u->tex[0] = u->tex[1] = 0; u->img = 0;
        for (int k = 0; k < s->nb; ++k) {
            const NvkBinding *b = &s->b[k];
            if ((b->kind == 3 || b->kind == 15) && b->reg >= 0 && b->reg < 128) u->tex[b->reg >> 6] |= 1ull << (b->reg & 63);
            else if (b->kind == 4 && b->reg >= 0 && b->reg < 32) u->img |= 1u << b->reg;
        }
        u->known = 1;
    }
    return s;
}

/* Caller holds lock. A variant neither the seed nor the cache covers, met without a bridge: its request, exactly as
 * the bridge would have received it plus the full shader key, goes to <cache>/missing/<variant>.json once.
 * tools/seed_missing.py pulls these, translates them on the Mac and pushes the replies into the cache, so the next
 * start loads the exact variant instead of a stand-in (4 shaders in the 24 Oct runs). */
static void missing_note(const char *variant, const char *key, Str *rq) {
    char path[700];
    snprintf(path, sizeof path, "%s/missing", cache_dir());
    mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/missing/%s.json", cache_dir(), variant);
    if (!access(path, F_OK) || rq->n < 2) return;
    Str note = {0};
    put(&note, "%.*s, \"key\": \"%s\"}\n", (int)(rq->n - 2), rq->p, key);   /* the request ends in "}\n" */
    write_file(path, (const uint8_t *)note.p, note.n);
    free(note.p);
    static unsigned noted;
    if (++noted <= 8 || noted % 50 == 0) VKLOG("shader variant %s noted for the seed: %s (%u so far)", variant, path, noted);
}

static NvkShader *shader_slow(const prog_t *p, unsigned i, int *nvn_stage) {
    const uint8_t *control = p->sd[i].control, *code = (const uint8_t *)(uintptr_t)p->sd[i].data;
    if (!control || !code) nvk_die("NVN program stage without code");
    uint32_t st, c1o, c1t;
    memcpy(&st, control + 0x714, 4); memcpy(&c1o, control + 0x700, 4); memcpy(&c1t, control + 0x704, 4);
    if (st != 0 && st != 1 && st != 5) { VKLOG("NVN shader stage %u (tessellation/geometry)", st); nvk_die("NVN shader stage without a translation chain"); }
    *nvn_stage = (int)st;
    const char *key = shader_key(p, i);
    Str ans = {0};
    answers(&ans, (int)st);
    uint32_t local[3] = {0, 0, 0};
    if (st == 5) {
        memcpy(local, control + 0x748, 12);
        if (!local[0] || !local[1] || !local[2] || (uint64_t)local[0] * local[1] * local[2] > 1024) nvk_die("implausible compute thread group size");
        put(&ans, ", \"local\": [%u, %u, %u]", local[0], local[1], local[2]);
    }
    Sha256 s; uint8_t d[32]; char dh[65];
    sha_init(&s); sha_update(&s, ans.p, ans.n); sha_final(&s, d); digest_hex(d, dh);
    char variant[80];
    snprintf(variant, sizeof variant, "%.32s-%.12s", key, dh);
    unsigned h = 0; for (const char *c = variant; *c; ++c) h = h * 131 + (unsigned char)*c;
    if (!c1t || c1t > 16u << 20 || c1o > c1t || (st != 5 && c1o < 0x30)) nvk_die("NVN shader code area / c1 bounds");
    Answers now; answers_now(&now, (int)st);
    pthread_mutex_lock(&lock);
    Variant *v = variants[h % 4096];
    while (v && strcmp(v->name, variant)) v = v->next;
    if (v) {
        if (!v->shader->c1) attach_c1(v->shader, code + c1o, c1t - c1o);   /* preloaded */
        if (share_on()) shared_add(key, v->shader, &now);
        pthread_mutex_unlock(&lock); free(ans.p); return v->shader;
    }
    NvkShader *same = share_on() ? shared_find(key, &now) : NULL;
    if (same) {   /* this name now resolves to the shared variant */
        v = calloc(1, sizeof *v);
        if (!v) nvk_die("shader variant allocation");
        snprintf(v->name, sizeof v->name, "%s", variant); v->shader = same; v->next = variants[h % 4096]; variants[h % 4096] = v;
        if (++shared_reuses <= 4 || shared_reuses % 100 == 0)
            VKLOG("shader variant %s reuses %s: same answers for its used slots (%llu reuses)", variant, same->variant, (unsigned long long)shared_reuses);
        pthread_mutex_unlock(&lock); free(ans.p); return same;
    }
    char path[640];
    snprintf(path, sizeof path, "%s/%s.vks", cache_dir(), variant);
    uint64_t started = nvk_now_ns();
    size_t len = 0;
    uint8_t *data = read_shader_cache(path, &len);
    NvkShader *sh = NULL;
    int exact = 1;
    if (!data) {   /* translated ahead of play (seed.idx) */
        const SeedEntry *e = seed_find(key, (int)st, 0);
        if (e && (sh = seed_take(e, (int)st, code + c1o, c1t - c1o)) && (++seed_hits <= 4 || seed_hits % 500 == 0))
            VKLOG("shader variant %s: seeded %s (%llu seed hits)", variant, e->variant, (unsigned long long)seed_hits);
    }
    if (!data && !sh) {
        size_t skip = st == 5 ? 0 : 0x30;   /* graphics code drops the 0x30-byte NVN header */
        Str rq = {0};
        put(&rq, "{\"variant\": \"%s\", \"stage\": \"%s\", \"code_hex\": \"", variant, st == 0 ? "vs" : st == 1 ? "ps" : "cs");
        hex_into(&rq, code + skip, c1t - skip);
        put(&rq, "\", \"c1\": \"");
        hex_into(&rq, code + c1o, c1t - c1o);
        put(&rq, "\", %s", ans.p);
        put(&rq, "}\n");
        data = nvk_translate_local(rq.p, cache_dir(), &len);
        if (!data) data = bridge_request(rq.p, &len);
        if (!data) missing_note(variant, key, &rq);
        free(rq.p);
        if (data) {
            if (!shader_data_valid(data, len)) nvk_die("invalid shader translation reply");
            write_file(path, data, len);
            static unsigned translated;
            if (++translated <= 8 || translated % 50 == 0) VKLOG("translated %u: %s %s", translated, st == 0 ? "vs" : st == 1 ? "ps" : "cs", variant);
        } else {   /* no bridge: the seeded variant of this shader closest to the bound state, else nothing */
            const SeedEntry *e = seed_find(key, (int)st, 1);
            if (e && (sh = seed_take(e, (int)st, code + c1o, c1t - c1o))) {
                exact = 0;
                if (++seed_near <= 8 || seed_near % 100 == 0)
                    VKLOG("shader variant %s not seeded, no bridge: nearest seeded %s stands in (%llu so far)", variant, e->variant, (unsigned long long)seed_near);
            } else {
                if (++seed_missing <= 8 || seed_missing % 100 == 0)
                    VKLOG("shader variant %s not seeded, no bridge: its draws are skipped (%llu so far)", variant, (unsigned long long)seed_missing);
                pthread_mutex_unlock(&lock); free(ans.p);
                return NULL;
            }
        }
    }
    if (!sh) { sh = parse(variant, (int)st, data, len, code + c1o, c1t - c1o); free(data); }
    uint64_t elapsed = nvk_now_ns() - started;
    atomic_fetch_add(&nvk_shader_count, 1); atomic_fetch_add(&nvk_shader_ns, elapsed);
    if (elapsed > 20000000ull) VKLOG("shader load/translation stalled %.1f ms: %s", elapsed / 1e6, variant);
    if (exact) {   /* a stand-in stays out of the tables: a bridge that returns translates the real one */
        v = calloc(1, sizeof *v);
        if (!v) nvk_die("shader variant allocation");
        snprintf(v->name, sizeof v->name, "%s", variant); v->shader = sh; v->next = variants[h % 4096]; variants[h % 4096] = v;
        if (share_on()) shared_add(key, sh, &now);
    }
    pthread_mutex_unlock(&lock);
    nvk_pipeline_warm_signal();
    free(ans.p);
    return sh;
}
