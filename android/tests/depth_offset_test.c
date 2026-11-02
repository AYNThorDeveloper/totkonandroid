/* Real nvn_vk_shader.c: depth_offset_reply on a cached gl_FragDepth writer (TOTK's class depth, ps 233dfb93).
 * usage: depth_offset_test <reply.vks> <out.spv>: writes the rewritten SPIR-V for spirv-val and checks that every
 * FragDepth store now stores an OpFAdd of the original value and the offset constant. */
#include <pthread.h>
#include <sys/socket.h>
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0   /* macOS host test: the bridge socket is never opened */
#endif
#include "../nvn_vk_shader.c"
VkDevice nvk_dev;
NvkState nvk_S;
uint32_t nvk_texture_info(int id) { (void)id; return 0; }
const p_set_rt *nvk_render_targets(void) { return NULL; }
int totk_depth_near_minus_w(void) { return 1; }
_Noreturn void nvk_die(const char *m) { fprintf(stderr, "%s\n", m); abort(); }

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    size_t len = 0, out_len = 0;
    uint8_t *data = read_file(argv[1], &len);
    if (!data) return 3;
    const float delta = 0.25f / 65535.0f;
    uint8_t *out = depth_offset_reply(data, len, delta, &out_len);
    if (!out) return 4;
    long spv; if (sscanf((const char *)out, "OK %ld", &spv) != 1 || (size_t)spv > out_len) return 5;
    size_t n = (size_t)spv / 4; uint32_t *w = malloc((size_t)spv);   /* the reply's SPIR-V follows a text header: copy aligned */
    if (!w) return 5;
    memcpy(w, out + out_len - spv, (size_t)spv);
    uint32_t fd = 0, c = 0, bits; memcpy(&bits, &delta, 4);
    unsigned stores = 0, adds = 0;
    for (size_t i = 5; i < n; i += w[i] >> 16) if ((w[i] & 0xffff) == 71 && w[i + 2] == 11 && w[i + 3] == 22) fd = w[i + 1];
    for (size_t i = 5; i < n; i += w[i] >> 16) if ((w[i] & 0xffff) == 43 && (w[i] >> 16) == 4 && w[i + 3] == bits) c = w[i + 2];
    if (!fd || !c) return 6;
    for (size_t i = 5, prev = 0; i < n; prev = i, i += w[i] >> 16)
        if ((w[i] & 0xffff) == 62 && w[i + 1] == fd) {
            stores++;
            if (prev && (w[prev] & 0xffff) == 129 && w[prev + 2] == w[i + 2] && w[prev + 4] == c) adds++;
        }
    if (!stores || adds != stores) return 7;
    unsigned implicit = 0, explicit_ = 0;   /* the texel-centre snap (TOTK_VK_CLASS_SNAP, on by default) */
    for (size_t i = 5; i < n; i += w[i] >> 16) { implicit += (w[i] & 0xffff) == 87; explicit_ += (w[i] & 0xffff) == 88; }
    if (implicit) return 9;
    FILE *f = fopen(argv[2], "wb");
    if (!f || fwrite(w, 4, n, f) != n || fclose(f)) return 8;
    printf("gl_FragDepth offset: pass (%u stores shifted, %u samples snapped to a texel centre)\n", stores, explicit_);
    free(w); free(out); free(data);
    return 0;
}
