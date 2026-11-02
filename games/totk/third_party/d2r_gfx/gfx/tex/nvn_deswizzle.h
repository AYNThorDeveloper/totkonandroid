// Tegra 块线性 ↔ 线性。级 / 层偏移与逐级块高块深一律取 nvn/nvn_tex.c（nvn_tex_level_offset / nvn_tex_layout / nvn_tex_level_tiles）。
// Tegra block-linear ↔ linear. Level / layer offsets and per-level block height and block depth always come from nvn/nvn_tex.c (nvn_tex_level_offset / nvn_tex_layout / nvn_tex_level_tiles).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../nvn/nvn_tex.h"

// 把第 level 级、第 layer 层（数组 / 立方体的层；非分层贴图取 0）从整张贴图存储 src_storage 解到线性 dst_linear。
// dst 按块行排：每行 dst_pitch 字节（≥ 本级块宽 × 每块字节），压缩格式一行 = 一行块。
// 3D 贴图：一次解出本级全部深度切片，切片 k 位于 dst_linear + k × dst_pitch × 本级块行数。
// 线性 / 线性渲染目标按行距复制；不支持多重采样、BUFFER，返回 -1。成功返回 0。
// Decode level number level, layer number layer (layer of an array / cube; 0 for non-layered textures) from the whole-texture storage src_storage into linear dst_linear.
// dst is laid out by block rows: dst_pitch bytes per row (≥ width in blocks of this level × bytes per block); for compressed formats one row = one row of blocks.
// 3D textures: all depth slices of this level are decoded at once; slice k is at dst_linear + k × dst_pitch × block row count of this level.
// Linear / linear-render-target storage is copied by pitch. Multisampling and BUFFER return -1; success returns 0.
int nvn_deswizzle(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src_storage, void *dst_linear, size_t dst_pitch);
// 只解线性行 [r0, r1)（3D：切片 k 的块行 y 为第 k × 本级块行数 + y 行），仍按 dst_linear + 行号 × dst_pitch 落位；可分给多个线程并行。
// Decodes only linear rows [r0, r1) (3D: block row y of slice k is row k × block rows of this level + y), still placed at
// dst_linear + row × dst_pitch; lets several threads share one level.
int nvn_deswizzle_rows(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src_storage, void *dst_linear, size_t dst_pitch, uint32_t r0, uint32_t r1);
// 反向（单测往返用）：线性 src_linear → 写进整张贴图存储 dst_storage 的对应级 / 层
// Reverse (for unit test round trips): linear src_linear → written into the corresponding level / layer of the whole-texture storage dst_storage
int nvn_swizzle(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src_linear, size_t src_pitch, void *dst_storage);
// 单点地址：本级本层内，线性 (x 字节, y 块行, z 切片) → 相对本级本层起点的块线性字节偏移（单测按手算核）
// Single-point address: within this level and layer, linear (x byte, y block row, z slice) → block-linear byte offset relative to the start of this level and layer (unit tests verify against hand calculation)
uint64_t nvn_bl_offset(uint32_t x, uint32_t y, uint32_t z, uint32_t row_bytes, uint32_t rows, unsigned tileH, unsigned tileD);

int nvn_deswizzle_geom(const nvn_texdesc *d, unsigned level, unsigned layer,
                       uint32_t *row_bytes, uint32_t *rows, uint32_t *slices,
                       uint64_t *base, unsigned *tileH, unsigned *tileD, uint32_t *bpb);

/* Decode absolute source rows [r0,r1), writing source row first_row at destination row zero.
 * first_row <= r0 <= r1; used to pack a full-width strip without allocating earlier rows. */
int nvn_deswizzle_rows_compact(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src_storage,
                               void *dst_linear, size_t dst_pitch, uint32_t first_row, uint32_t r0, uint32_t r1);
/* Reverse of nvn_deswizzle_rows_compact: linear rows (source row zero = absolute row first_row) into rows [r0,r1) of the
 * storage. Only the GOB rows of those block rows are written (TOTK's WriteTexels into a 5120x5120 texture). */
int nvn_swizzle_rows_compact(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src_linear,
                             size_t src_pitch, void *dst_storage, uint32_t first_row, uint32_t r0, uint32_t r1);
