/* Real nvn_vk_shader.c: fetch_reply on a cached fragment reply and its proofs (tests/fetch_variant_test.py checks the
   result with spirv-val). usage: fetch_variant_test <reply.vks> <proof.fetch> <attachment for every sample> <out> [zsw]
   (attachment 8 = the depth attachment, zsw its channel mapping, default 0x7650) */
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
    if (argc != 5 && argc != 6) return 2;
    size_t len = 0, out_len = 0;
    uint8_t *data = read_file(argv[1], &len);
    FILE *f = fopen(argv[2], "r");
    if (!data || !f) return 3;
    NvkShader ps; memset(&ps, 0, sizeof ps); ps.stage = 1;
    char line[64]; unsigned id, slot, loc;
    while (fgets(line, sizeof line, f)) if (sscanf(line, "S %u %u %u", &id, &slot, &loc) == 3 && ps.nfetch < 8)
        { ps.fetch[ps.nfetch].id = id; ps.fetch[ps.nfetch].slot = (uint8_t)slot; ps.fetch[ps.nfetch].loc = (uint8_t)loc; ps.nfetch++; }
    fclose(f);
    uint8_t att[8]; memset(att, (int)strtoul(argv[3], NULL, 0), sizeof att);
    uint16_t zsw = argc == 6 ? (uint16_t)strtoul(argv[5], NULL, 0) : 0x7650;
    uint8_t *out = fetch_reply(data, len, &ps, att, zsw, &out_len);
    if (!out) return 4;
    if (!shader_data_valid(out, out_len)) return 6;   /* what read_shader_cache checks before a cached variant is used */
    FILE *o = fopen(argv[4], "wb");
    if (!o || fwrite(out, 1, out_len, o) != out_len || fclose(o)) return 5;
    free(out); free(data);
    return 0;
}
