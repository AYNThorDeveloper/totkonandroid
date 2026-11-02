/* Real backend: uniforms captured at the submit call (TOTK_ASYNC_REPLAY). A recorded command stream binds uniform ranges
 * (also through OP_CALL); the capture copies them on the "game thread"; the game then rewrites its memory; the replay must
 * read the captured bytes through nvk_ubo_read, except where an earlier submission's GPU work, unfinished at the capture,
 * writes (that range is read from memory, the GPU timeline), and with nothing captured it reads memory as before. */
#include <pthread.h>
static int test_setname(pthread_t thread, const char *name) { (void)thread; (void)name; return 0; }
#define pthread_setname_np test_setname
#include "../nvn_vk_backend.c"
/* Tiny target write-backs live in nvn_vk_image.c, not linked here. */
void *nvk_tiny_wb_record(VkCommandBuffer c, const tex_t *t, unsigned slot, size_t cap, unsigned slots) { (void)c; (void)t; (void)slot; (void)cap; (void)slots; return NULL; }
int nvk_tiny_wb_finish(void *s, uint64_t st) { (void)s; (void)st; return 0; }
void dm_guest_write_prepare(void *p, size_t n) { (void)p; (void)n; }
_Noreturn void runtime_fail(const char *m) { fprintf(stderr, "%s\n", m); abort(); }
int totk_vpool_backing(uintptr_t va, uint64_t len, uintptr_t *cpu) { (void)va; (void)len; (void)cpu; return 0; }
#include <assert.h>

static uint8_t *op(uint8_t *p, uint32_t code, const void *arg, uint32_t n) {   /* one recorded op: header + argument */
    op_hdr h = {code, (uint32_t)(8 + ((n + 7) & ~7u))};
    memcpy(p, &h, sizeof h); memset(p + 8, 0, h.size - 8); if (n) memcpy(p + 8, arg, n);
    return p + h.size;
}
static uint8_t *bind_ubo(uint8_t *p, uint64_t addr, uint64_t size) { p_bind_buf b = {1, 0, addr, size}; return op(p, OP_BIND_UBO, &b, sizeof b); }

int main(void) {
    static uint8_t mem[65536] __attribute__((aligned(4096)));   /* one "pool" of guest memory */
    for (unsigned i = 0; i < sizeof mem; ++i) mem[i] = (uint8_t)i;
    pools = calloc(1, sizeof *pools); pool_count = pool_cap = 1;
    pools[0] = (Pool){(uintptr_t)mem, (uintptr_t)mem + sizeof mem, mem, (VkDeviceMemory)(uintptr_t)1, (VkBuffer)(uintptr_t)2};
    const uint64_t base = (uint64_t)(uintptr_t)mem;

    /* Stream: main buffer binds [0x100, 0x200) and calls a second buffer binding [0x180, 0x300) (overlapping: merged)
       and [0x1000, 0x1100); an out-of-pool binding is dropped. */
    static uint8_t sub[256] __attribute__((aligned(8))), main_cb[512] __attribute__((aligned(8)));
    uint8_t *q = sub;
    q = bind_ubo(q, base + 0x180, 0x180);
    q = bind_ubo(q, base + 0x1000, 0x100);
    q = op(q, OP_END, NULL, 0);
    uint64_t target = (uint64_t)(uintptr_t)sub;
    q = main_cb;
    q = bind_ubo(q, base + 0x100, 0x100);
    q = bind_ubo(q, 0x10, 0x40);                          /* not in any pool */
    q = op(q, OP_CALL, &target, sizeof target);
    q = op(q, OP_END, NULL, 0);
    uint64_t h[1] = {(uint64_t)(uintptr_t)main_cb};

    void *cap = mtl_sub_capture_ubos(h, 1);
    assert(cap);
    Ucap *c = cap;
    assert(c->n == 2 && c->r[0].lo == base + 0x100 && c->r[0].hi == base + 0x300 && c->r[1].lo == base + 0x1000 && c->r[1].hi == base + 0x1100);

    uint8_t before[0x400]; memcpy(before, mem, sizeof before);
    memset(mem, 0xee, 0x2000);                            /* the game rebuilds its constants right after the submit */

    uint8_t out[0x300];
    nvk_ubo_read(base + 0x100, 0x200, out);               /* no capture active yet: memory, as before */
    for (unsigned i = 0; i < 0x200; ++i) assert(out[i] == 0xee);

    mtl_sub_snapshot_ubos_cap(h, 1, cap);                 /* replay start */
    assert(nvk_ucap_active());
    nvk_ubo_read(base + 0x100, 0x200, out);
    assert(!memcmp(out, before + 0x100, 0x200));          /* the bytes of the submit call */
    nvk_ubo_read(base + 0xf0, 0x40, out);                 /* straddles the start: memory before 0x100, captured after */
    for (unsigned i = 0; i < 0x10; ++i) assert(out[i] == 0xee);
    assert(!memcmp(out + 0x10, before + 0x100, 0x30));
    nvk_ubo_read(base + 0x400, 0x10, out);                /* not captured: memory */
    for (unsigned i = 0; i < 0x10; ++i) assert(out[i] == 0xee);

    /* An earlier submission's GPU write into [0x1000, 0x1100), unfinished at the capture: memory (GPU timeline). */
    fence_issued = 5;                                     /* pending fence 6, completed 0 at the capture above */
    nvk_note_gpu_write(base + 0x1000, 0x100);
    nvk_ubo_read(base + 0x1000, 0x10, out);
    for (unsigned i = 0; i < 0x10; ++i) assert(out[i] == 0xee);
    nvk_ubo_read(base + 0x100, 0x10, out);                /* other captured ranges are unaffected */
    assert(!memcmp(out, before + 0x100, 0x10));

    /* nvk_ucap_cover: a range inside one captured range, unchanged since, is named with its bytes (the replay binds one arena
       copy of the whole captured range at an offset); anything else is refused and keeps the per-binding copy. */
    {   uint64_t serial, rlo, rlen; uint32_t idx; const uint8_t *cb;
        assert(nvk_ucap_cover(base + 0x180, 0x40, &serial, &idx, &cb, &rlo, &rlen));
        assert(serial == 1 && idx == 0 && rlo == base + 0x100 && rlen == 0x200 && !memcmp(cb, before + 0x100, 0x200));
        assert(nvk_ucap_cover(base + 0x1000, 0x10, &serial, &idx, &cb, &rlo, &rlen) == 0);       /* unfinished GPU write over it */
        assert(nvk_ucap_cover(base + 0x1100, 0x10, &serial, &idx, &cb, &rlo, &rlen) == 0);       /* just outside the second range */
        assert(nvk_ucap_cover(base + 0x2f0, 0x20, &serial, &idx, &cb, &rlo, &rlen) == 0);        /* leaves the first range */
        assert(nvk_ucap_cover(base + 0x400, 0x10, &serial, &idx, &cb, &rlo, &rlen) == 0);        /* not captured */
        assert(nvk_ucap_cover(base + 0x180, 0, &serial, &idx, &cb, &rlo, &rlen) == 0);
        /* an in-pass uniform update over part of the range: nvk_ubo_read applies it, so the plain capture is not the answer */
        uint8_t upd[16]; memset(upd, 0x5a, sizeof upd);
        ubo_append(base + 0x1c0, upd, sizeof upd, 0);
        assert(nvk_ucap_cover(base + 0x1a0, 0x40, &serial, &idx, &cb, &rlo, &rlen) == 0);        /* overlaps the update */
        assert(nvk_ucap_cover(base + 0x120, 0x20, &serial, &idx, &cb, &rlo, &rlen) == 1);        /* clear of it */
        uint8_t o2[0x40]; nvk_ubo_read(base + 0x1a0, 0x40, o2);
        assert(o2[0x20] == 0x5a && o2[0x2f] == 0x5a && o2[0x1f] != 0x5a && o2[0x30] != 0x5a);    /* the update is what the GPU sees */
    }

    /* The next submission without a capture (synchronous replay): memory again. */
    mtl_sub_snapshot_ubos_cap(h, 1, NULL);
    assert(!nvk_ucap_active());
    nvk_ubo_read(base + 0x100, 0x10, out);
    for (unsigned i = 0; i < 0x10; ++i) assert(out[i] == 0xee);
    uint64_t subs, bytes, ns; nvk_ucap_stats(&subs, &bytes, &ns);
    assert(subs == 1 && bytes == 0x300);
    printf("uniform capture: pass (bindings through calls collected, overlaps merged, out-of-pool dropped, captured bytes read after the game rewrote memory, unfinished GPU writes and uncaptured ranges read from memory)\n");
    return 0;
}
