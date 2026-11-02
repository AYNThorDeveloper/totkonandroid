/* nvn_swizzle_rows_compact: writing a region's block rows only gives the same storage as the whole-level
 * deswizzle / patch / swizzle that WriteTexels did before (BC4 and RGBA8, odd offsets, several block heights). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvn_deswizzle.h"
#include "nvn_tex.h"
int main(void) {
    const int formats[2] = {0x49, 0x25};   /* RGTC1 (BC4), RGBA8 */
    for (int f = 0; f < 2; ++f) for (unsigned w = 200; w <= 1100; w += 450) {
        nvn_texdesc d = {.target = 1, .format = formats[f], .width = w, .height = w / 2 + 37, .depth = 1, .levels = 1};
        nvn_texlayout L; nvn_tex_layout(&d, &L);
        int bw, bh, bpb = nvn_fmt_bpb(d.format, &bw, &bh);
        size_t pitch = (size_t)((d.width + bw - 1) / bw) * bpb, rows = (d.height + bh - 1) / bh;
        uint8_t *a = malloc(L.size), *b = malloc(L.size), *lin = malloc(pitch * rows);
        for (size_t i = 0; i < L.size; ++i) a[i] = (uint8_t)(i * 2654435761u >> 13);
        memcpy(b, a, L.size);
        size_t y0 = rows / 3 + 1, hb = rows / 5 + 2, x0 = (size_t)bpb * 3, wb = pitch / 2;
        uint8_t *patch = malloc(wb * hb); for (size_t i = 0; i < wb * hb; ++i) patch[i] = (uint8_t)(i * 7 + 1);
        /* before: whole level */
        assert(!nvn_deswizzle(&d, 0, 0, a, lin, pitch));
        for (size_t y = 0; y < hb; ++y) memcpy(lin + (y0 + y) * pitch + x0, patch + y * wb, wb);
        assert(!nvn_swizzle(&d, 0, 0, lin, pitch, a));
        /* now: the region's rows */
        assert(!nvn_deswizzle_rows_compact(&d, 0, 0, b, lin, pitch, (uint32_t)y0, (uint32_t)y0, (uint32_t)(y0 + hb)));
        for (size_t y = 0; y < hb; ++y) memcpy(lin + y * pitch + x0, patch + y * wb, wb);
        assert(!nvn_swizzle_rows_compact(&d, 0, 0, lin, pitch, b, (uint32_t)y0, (uint32_t)y0, (uint32_t)(y0 + hb)));
        assert(!memcmp(a, b, L.size));
        free(a); free(b); free(lin); free(patch);
    }
    puts("swizzle rows: pass (region rows only = whole-level round trip; BC4 and RGBA8)");
    return 0;
}
