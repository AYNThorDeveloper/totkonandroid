// Tegra X1 块线性排布（公开描述见 deko3d / Ryujinx BlockLinearLayout）：
//   GOB = 64 字节 × 8 行 = 512 字节；GOB 内 (x 字节, y 行) 的偏移 =
//     (x%64/32)·256 + (y%8/2)·64 + (x%32/16)·32 + (y%2)·16 + x%16
//   块 = 1 个 GOB 宽 × 2^tileH 个 GOB 高 × 2^tileD 个 GOB 深，块内先排 y 再排 z；块按 x → y → z 行优先铺满本级。
// 级 / 层起点与本级实际块高块深取自 nvn_tex.c，这里只做级内寻址与搬运。
// Tegra X1 block-linear layout (public descriptions: deko3d / Ryujinx BlockLinearLayout):
//   GOB = 64 bytes × 8 rows = 512 bytes; offset of (x byte, y row) within a GOB =
//     (x%64/32)·256 + (y%8/2)·64 + (x%32/16)·32 + (y%2)·16 + x%16
//   block = 1 GOB wide × 2^tileH GOBs high × 2^tileD GOBs deep, ordered y first then z within a block; blocks fill the level in x → y → z row-major order.
// Level / layer start offsets and the actual block height and block depth of this level come from nvn_tex.c; here we only do intra-level addressing and copying.
#include "nvn_deswizzle.h"
#include <string.h>

static uint32_t gob_off(uint32_t x, uint32_t y) {
    return (x % 64 / 32) * 256 + (y % 8 / 2) * 64 + (x % 32 / 16) * 32 + (y % 2) * 16 + x % 16;
}

uint64_t nvn_bl_offset(uint32_t x, uint32_t y, uint32_t z, uint32_t row_bytes, uint32_t rows, unsigned th, unsigned td) {
    uint32_t bh = 1u << th, bd = 1u << td;
    uint64_t gobs_x = (row_bytes + 63) / 64, blocks_y = (rows + 8 * bh - 1) / (8 * bh);
    uint64_t block = 512ull * bh * bd;                                  // 一个块的字节 | bytes in one block
    return (z / bd) * gobs_x * blocks_y * block                        // 第几层块（深度方向） | which block layer (along depth)
         + (y / (8 * bh)) * gobs_x * block                             // 第几行块 | which block row
         + (x / 64) * block                                            // 第几列块 | which block column
         + ((z % bd) * bh + (y % (8 * bh)) / 8) * 512ull               // 块内第几个 GOB | which GOB within the block
         + gob_off(x, y);
}

static uint32_t mip(uint32_t s, unsigned l) { s >>= l; return s ? s : 1; }

// 本级几何：块宽字节数、块行数、切片数，以及本级本层在存储里的起点；不支持的贴图返回 -1
// Geometry of this level: width-in-blocks byte count, block row count, slice count, and where this level and layer starts in storage; returns -1 for unsupported textures
int nvn_deswizzle_geom(const nvn_texdesc *d, unsigned level, unsigned layer,
                       uint32_t *row_bytes, uint32_t *rows, uint32_t *slices, uint64_t *base, unsigned *th, unsigned *td, uint32_t *bpb) {
    if (d->target == 5 || d->target == 6 || d->target == 10 || nvn_tex_is_linear(d) || d->samples > 1) return -1;   // MS / BUFFER / LINEAR
    int bw, bh; *bpb = (uint32_t)nvn_fmt_bpb(d->format, &bw, &bh);
    if (!*bpb || level >= (d->levels ? d->levels : 1)) return -1;
    nvn_texlayout L; nvn_tex_layout(d, &L);
    if (layer >= L.layers) return -1;
    int one_d = d->target == 0 || d->target == 3;
    uint32_t w = mip(d->width, level), h = one_d ? 1 : mip(d->height, level);
    *slices = d->target == 2 ? mip(d->depth ? d->depth : 1, level) : 1;
    *row_bytes = (w + bw - 1) / bw * *bpb; *rows = (h + bh - 1) / bh;
    *base = (uint64_t)layer * L.layer_size + nvn_tex_level_offset(d, level);
    nvn_tex_level_tiles(d, level, th, td);
    return 0;
}

// 线性行 [r0, r1)（3D：切片 z 的块行 y 为第 z × 本级块行数 + y 行）在 lin 里位于 lin + 行号 × pitch。
// 按线性输出顺序逐行搬：一行里每个 GOB 的四段 16 字节在 GOB 内偏移 0 / 32 / 256 / 288（gob_off），块与 GOB 起点逐行算一次，不做除法。
// Linear rows [r0, r1) (3D: block row y of slice z is row z × block rows of this level + y) lie at lin + row × pitch.
// Copied row by row in linear output order: the four 16-byte segments of each GOB in a row sit at GOB offsets 0 / 32 / 256 / 288
// (gob_off); block and GOB starts are computed once per row, without divisions.
static int move(const nvn_texdesc *d, unsigned level, unsigned layer, uint8_t *bl, uint8_t *lin, size_t pitch, int to_linear, uint32_t r0, uint32_t r1, uint32_t dst_first_row) {
    if (nvn_tex_is_linear(d)) {
        if (level || layer || d->samples > 1 || (d->target != 0 && d->target != 1 && d->target != 7)) return -1;
        int bw, bh, bpb = nvn_fmt_bpb(d->format, &bw, &bh);
        if (!bpb) return -1;
        size_t row = (size_t)((d->width + bw - 1) / bw) * bpb;
        uint32_t rows = d->target == 0 ? 1 : (d->height + bh - 1) / bh, stride = nvn_tex_linear_stride(d);
        if (pitch < row || stride < row) return -1;
        if (r1 > rows) r1 = rows;
        for (uint32_t y = r0; y < r1; y++) {
            if (to_linear) memcpy(lin + (y - dst_first_row) * pitch, bl + (size_t)y * stride, row);
            else memcpy(bl + (size_t)y * stride, lin + (y - dst_first_row) * pitch, row);
        }
        return 0;
    }
    uint32_t row_bytes, rows, slices, bpb; uint64_t base; unsigned th, td;
    if (nvn_deswizzle_geom(d, level, layer, &row_bytes, &rows, &slices, &base, &th, &td, &bpb)) return -1;
    if (pitch < row_bytes) return -1;
    bl += base;
    const uint32_t bh = 1u << th, bd = 1u << td, ymask = 8 * bh - 1;
    const uint64_t gobs_x = (row_bytes + 63) / 64, blocks_y = (rows + 8 * bh - 1) >> (3 + th), block = 512ull * bh * bd;
    const uint32_t full = row_bytes / 64, tail = row_bytes % 64;
    static const uint16_t seg[4] = {0, 32, 256, 288};
    uint64_t total = (uint64_t)rows * slices;
    if (r1 > total) r1 = (uint32_t)total;
    uint32_t z = rows ? r0 / rows : 0, y = rows ? r0 % rows : 0;
    for (uint32_t r = r0; r < r1; r++) {
        uint8_t *row = lin + (size_t)(r - dst_first_row) * pitch;
        uint8_t *p = bl + (z >> td) * gobs_x * blocks_y * block + (uint64_t)(y >> (3 + th)) * gobs_x * block
                   + ((uint64_t)(z & (bd - 1)) * bh + ((y & ymask) >> 3)) * 512 + ((y & 7) >> 1) * 64 + (y & 1) * 16;
        uint32_t g = 0;
        if (to_linear) {
            for (; g < full; g++, p += block, row += 64) {
                memcpy(row, p, 16); memcpy(row + 16, p + 32, 16); memcpy(row + 32, p + 256, 16); memcpy(row + 48, p + 288, 16);
            }
        } else {
            for (; g < full; g++, p += block, row += 64) {
                memcpy(p, row, 16); memcpy(p + 32, row + 16, 16); memcpy(p + 256, row + 32, 16); memcpy(p + 288, row + 48, 16);
            }
        }
        for (uint32_t x = 0; x < tail; x += 16) {   // 本行最后一个不满的 GOB | the row's last, partial GOB
            uint32_t n = tail - x < 16 ? tail - x : 16;
            if (to_linear) memcpy(row + x, p + seg[x / 16], n); else memcpy(p + seg[x / 16], row + x, n);
        }
        if (++y == rows) { y = 0; z++; }
    }
    return 0;
}

int nvn_deswizzle_rows(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src, void *dst, size_t dst_pitch, uint32_t r0, uint32_t r1) {
    return move(d, level, layer, (uint8_t *)src, (uint8_t *)dst, dst_pitch, 1, r0, r1, 0);
}
int nvn_deswizzle(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src, void *dst, size_t dst_pitch) {
    return move(d, level, layer, (uint8_t *)src, (uint8_t *)dst, dst_pitch, 1, 0, UINT32_MAX, 0);
}
int nvn_swizzle(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src, size_t src_pitch, void *dst) {
    return move(d, level, layer, (uint8_t *)dst, (uint8_t *)src, src_pitch, 0, 0, UINT32_MAX, 0);
}

/* Compact row windows avoid an out-of-bounds "dst - first_row * pitch" pointer for partial image uploads. */
int nvn_deswizzle_rows_compact(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src, void *dst,
                               size_t pitch, uint32_t first_row, uint32_t r0, uint32_t r1) {
    if (r0 < first_row || r1 < r0) return -1;
    return move(d, level, layer, (uint8_t *)src, (uint8_t *)dst, pitch, 1, r0, r1, first_row);
}
int nvn_swizzle_rows_compact(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src, size_t pitch,
                             void *dst, uint32_t first_row, uint32_t r0, uint32_t r1) {
    if (r0 < first_row || r1 < r0) return -1;
    return move(d, level, layer, (uint8_t *)dst, (uint8_t *)src, pitch, 0, r0, r1, first_row);
}
