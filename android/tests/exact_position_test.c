/* Real nvn_vk_shader.c: spv_exact_position on cached vertex shaders (TOTK_VK_EXACT_POSITION).
 * usage: exact_position_test <out dir> <reply.vks>...: writes <out dir>/<n>.spv for spirv-val and checks that the
 * position is decorated Invariant once and every float add / sub / mul / product / dot NoContraction, the rest of
 * the module unchanged. */
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

static int arith(uint32_t op) { return op == 129 || op == 131 || op == 133 || (op >= 142 && op <= 148); }

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    unsigned files = 0, total_ops = 0;
    for (int a = 2; a < argc; ++a) {
        size_t len = 0;
        uint8_t *data = read_file(argv[a], &len);
        long spv; if (!data || sscanf((const char *)data, "OK %ld", &spv) != 1 || (size_t)spv > len) return 3;
        size_t n = (size_t)spv / 4; uint32_t *w = malloc((size_t)spv);
        if (!w) return 3;
        memcpy(w, data + len - spv, (size_t)spv);
        size_t on; unsigned ops = 0;
        uint32_t *o = spv_exact_position(w, n, &on, &ops);
        if (!o) { fprintf(stderr, "%s: not rewritten\n", argv[a]); return 4; }
        unsigned want = 0, nocontract = 0, invariant = 0; size_t kept = 0;
        size_t had = 0;   /* the translator already marks some positions Invariant: kept as they are */
        for (size_t i = 5; i < n; i += w[i] >> 16) {
            want += arith(w[i] & 0xffff);
            uint32_t op = w[i] & 0xffff, l = w[i] >> 16;
            if ((op == 72 && l == 4 && w[i + 3] == 18) || (op == 71 && l == 3 && w[i + 2] == 18) || (op == 71 && l == 3 && w[i + 2] == 42)) had += l;
        }
        for (size_t i = 5; i < on; i += o[i] >> 16) {
            uint32_t op = o[i] & 0xffff, l = o[i] >> 16;
            if (op == 71 && l == 3 && o[i + 2] == 42) nocontract++;
            else if ((op == 72 && l == 4 && o[i + 3] == 18) || (op == 71 && l == 3 && o[i + 2] == 18)) invariant++;
            else kept += l;
        }
        if (ops != want || nocontract != want || invariant != 1 || kept != n - 5 - had || memcmp(o, w, 5 * 4)) {
            fprintf(stderr, "%s: ops %u/%u, NoContraction %u, Invariant %u, kept %zu of %zu\n", argv[a], ops, want, nocontract, invariant, kept, n - 5);
            return 5;
        }
        char path[1024]; snprintf(path, sizeof path, "%s/%u.spv", argv[1], files);
        FILE *f = fopen(path, "wb");
        if (!f || fwrite(o, 4, on, f) != on || fclose(f)) return 6;
        total_ops += ops; files++;
        free(o); free(w); free(data);
    }
    printf("exact vertex positions: pass (%u vertex shaders, %u operations NoContraction, position Invariant once each)\n", files, total_ops);
    return 0;
}
