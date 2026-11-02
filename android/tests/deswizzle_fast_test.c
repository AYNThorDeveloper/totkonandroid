/* The row-wise deswizzle (nvn_deswizzle.c) against the per-16-byte reference it replaced (nvn_bl_offset for every
 * segment), byte for byte, for both directions and for row ranges (nvn_deswizzle_rows), over formats, mip chains,
 * arrays, cubes and 3D textures with odd sizes. Also times both on a 2048x2048 RGBA8 mip chain. */
#include "../../games/totk/third_party/d2r_gfx/gfx/nvn/include/nvn.h"
#include "../../games/totk/third_party/d2r_gfx/gfx/tex/nvn_deswizzle.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void reference(const nvn_texdesc *d, unsigned level, unsigned layer, const uint8_t *bl, uint8_t *lin, size_t pitch) {
    uint32_t row_bytes, rows, slices, bpb; uint64_t base; unsigned th, td;
    assert(!nvn_deswizzle_geom(d, level, layer, &row_bytes, &rows, &slices, &base, &th, &td, &bpb));
    for (uint32_t z = 0; z < slices; z++)
        for (uint32_t y = 0; y < rows; y++)
            for (uint32_t x = 0; x < row_bytes; x += 16) {
                uint32_t n = row_bytes - x < 16 ? row_bytes - x : 16;
                memcpy(lin + ((size_t)z * rows + y) * pitch + x, bl + base + nvn_bl_offset(x, y, z, row_bytes, rows, th, td), n);
            }
}

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint8_t next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint8_t)rng; }

static unsigned checked;
static void check(int target, int format, uint32_t w, uint32_t h, uint32_t depth, uint32_t levels) {
    nvn_texdesc d = {.target = target, .format = format, .width = w, .height = h, .depth = depth, .levels = levels};
    nvn_texlayout L; nvn_tex_layout(&d, &L);
    uint8_t *src = malloc(L.size), *back = calloc(1, L.size);
    assert(src && back);
    for (uint64_t i = 0; i < L.size; ++i) src[i] = next();
    for (unsigned layer = 0; layer < L.layers; ++layer)
        for (unsigned level = 0; level < levels; ++level) {
            uint32_t row_bytes, rows, slices, bpb; uint64_t base; unsigned th, td;
            assert(!nvn_deswizzle_geom(&d, level, layer, &row_bytes, &rows, &slices, &base, &th, &td, &bpb));
            size_t pitch = row_bytes + (level & 1) * 16, bytes = pitch * rows * slices;   /* odd levels: padded rows */
            uint8_t *want = calloc(1, bytes), *got = calloc(1, bytes), *part = calloc(1, bytes);
            assert(want && got && part);
            reference(&d, level, layer, src, want, pitch);
            assert(!nvn_deswizzle(&d, level, layer, src, got, pitch));
            if (memcmp(want, got, bytes)) { fprintf(stderr, "mismatch: target %d fmt %#x %ux%ux%u level %u layer %u\n", target, format, w, h, depth, level, layer); abort(); }
            uint32_t total = rows * slices, step = total / 3 + 1;   /* three uneven row ranges, as threads would take them */
            for (uint32_t r = 0; r < total; r += step) assert(!nvn_deswizzle_rows(&d, level, layer, src, part, pitch, r, r + step));
            assert(!memcmp(want, part, bytes));
            assert(!nvn_swizzle(&d, level, layer, got, pitch, back));   /* reverse: the same bytes land where they came from */
            free(want); free(got); free(part);
            checked++;
        }
    for (unsigned layer = 0; layer < L.layers; ++layer)
        for (unsigned level = 0; level < levels; ++level) {   /* every decoded byte went back to its place */
            uint32_t row_bytes, rows, slices, bpb; uint64_t base; unsigned th, td;
            nvn_deswizzle_geom(&d, level, layer, &row_bytes, &rows, &slices, &base, &th, &td, &bpb);
            for (uint32_t z = 0; z < slices; z++) for (uint32_t y = 0; y < rows; y++) for (uint32_t x = 0; x < row_bytes; x += 16) {
                uint64_t o = base + nvn_bl_offset(x, y, z, row_bytes, rows, th, td);
                uint32_t n = row_bytes - x < 16 ? row_bytes - x : 16;
                assert(!memcmp(src + o, back + o, n));
            }
        }
    free(src); free(back);
}

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

int main(void) {
    const int formats[] = {NVN_FORMAT_R8, NVN_FORMAT_RG16F, NVN_FORMAT_RGB8, NVN_FORMAT_RGBA8, NVN_FORMAT_RGBA32F, NVN_FORMAT_RGBA_DXT5, NVN_FORMAT_RGBA_ASTC_8x8};
    const uint32_t sizes[][2] = {{1, 1}, {3, 5}, {17, 9}, {64, 64}, {100, 37}, {256, 130}, {513, 260}};
    for (unsigned f = 0; f < sizeof formats / sizeof *formats; ++f)
        for (unsigned s = 0; s < sizeof sizes / sizeof *sizes; ++s) {
            uint32_t w = sizes[s][0], h = sizes[s][1], m = w > h ? w : h, levels = 1;
            while ((m >> levels) && levels < 6) levels++;
            check(NVN_TEXTURE_TARGET_2D, formats[f], w, h, 1, levels);
            check(NVN_TEXTURE_TARGET_2D_ARRAY, formats[f], w, h, 3, levels > 3 ? 3 : levels);
            if (w == h) check(NVN_TEXTURE_TARGET_CUBEMAP, formats[f], w, h, 1, 2);
        }
    check(NVN_TEXTURE_TARGET_3D, NVN_FORMAT_RGBA8, 33, 17, 9, 3);
    check(NVN_TEXTURE_TARGET_3D, NVN_FORMAT_R8, 64, 64, 32, 4);
    check(NVN_TEXTURE_TARGET_3D, NVN_FORMAT_RGBA_DXT5, 40, 24, 5, 2);
    check(NVN_TEXTURE_TARGET_1D, NVN_FORMAT_RGBA8, 300, 1, 1, 4);

    nvn_texdesc d = {.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8, .width = 2048, .height = 2048, .levels = 12};
    nvn_texlayout L; nvn_tex_layout(&d, &L);
    uint8_t *src = malloc(L.size), *dst = malloc((size_t)2048 * 2048 * 4);
    for (uint64_t i = 0; i < L.size; ++i) src[i] = (uint8_t)i;
    double t0 = now();
    for (int k = 0; k < 4; ++k) reference(&d, 0, 0, src, dst, 2048 * 4);
    double t1 = now();
    for (int k = 0; k < 4; ++k) nvn_deswizzle(&d, 0, 0, src, dst, 2048 * 4);
    double t2 = now();
    printf("deswizzle: %u subresources identical to the reference; 2048x2048 RGBA8 level 0: reference %.2f ms, row-wise %.2f ms\n",
        checked, (t1 - t0) / 4 * 1e3, (t2 - t1) / 4 * 1e3);
    free(src); free(dst);
    return 0;
}
