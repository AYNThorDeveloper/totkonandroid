/* Real nvn_vk_shader.c: zero_inputs_reply on a cached reply (tests/zero_inputs_test.py compares it word for word
 * with shader_bridge.py). usage: zero_inputs_test <reply.vks> <mask> <out> */
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
    if (argc != 4) return 2;
    size_t len = 0, out_len = 0;
    uint8_t *data = read_file(argv[1], &len);
    if (!data) return 3;
    uint8_t *out = zero_inputs_reply(data, len, (uint32_t)strtoul(argv[2], NULL, 0), &out_len);
    if (!out) return 4;
    FILE *f = fopen(argv[3], "wb");
    if (!f || fwrite(out, 1, out_len, f) != out_len || fclose(f)) return 5;
    free(out); free(data);
    return 0;
}
