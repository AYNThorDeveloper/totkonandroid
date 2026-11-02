/* Real nvk_image_needs_prepare: the per-draw pool check is skipped while no
 * tracked guest write happened since the image was last found clean. */
#include <assert.h>
#include <stdio.h>
#include "../nvn_vk_image.c"
VkDevice nvk_dev;
VkPhysicalDevice nvk_phys;
_Noreturn void nvk_die(const char *message) { fprintf(stderr, "%s\n", message); abort(); }
static uint64_t now = 10, writes_after = 0; static unsigned lookups;
uint64_t nvn_dirty_now(void) { return now; }
int nvn_dirty_since(const void *addr, size_t len, uint64_t stamp) { (void)addr; (void)len; lookups++; return writes_after > stamp; }
static uint64_t pending = 5;
uint64_t nvk_pending_fence(void) { return pending; }
int main(void) {
    static uint8_t pool[256];
    NvkImage *img = calloc(1, sizeof *img);
    img->serial = 42; img->uploaded = 1; img->stamp = 9;
    serial_put(42, img);
    tex_t t; memset(&t, 0, sizeof t); t.serial = 42; t.mem = pool; t.L.size = sizeof pool;
    assert(!nvk_image_needs_prepare(42, &t) && lookups == 1);
    assert(!nvk_image_needs_prepare(42, &t) && lookups == 1);   /* nothing written anywhere */
    now = 11;                                                    /* a write elsewhere */
    assert(!nvk_image_needs_prepare(42, &t) && lookups == 2);
    now = 12; writes_after = 12;                                 /* a write to this texture */
    assert(nvk_image_needs_prepare(42, &t) && lookups == 3);
    assert(nvk_image_needs_prepare(42, &t) && lookups == 4);     /* dirty results are not cached */
    nvk_image_cpu_written(&t);
    assert(nvk_image_needs_prepare(42, &t) && lookups == 4);     /* not uploaded: no lookup needed */
    img->gpu_owned = 1; img->uploaded = 1;
    assert(!nvk_image_needs_prepare(42, &t));
    /* refresh_once: a big texture uploaded in this submission and written again waits for the next one */
    img->gpu_owned = 0; img->uploaded = 1; img->stamp = 12; img->upload_fence = pending;
    now = 13; writes_after = 13; t.L.size = 8u << 20;
    assert(!nvk_image_needs_prepare(42, &t));
    pending++;
    assert(nvk_image_needs_prepare(42, &t));
    t.L.size = sizeof pool; img->upload_fence = pending;                 /* small ones refresh as before */
    assert(nvk_image_needs_prepare(42, &t));
    free(img);
    puts("image dirty epoch: pass (clean result reused until the next tracked write; big textures refresh once per submission)");
}
