/* Tegra NVN allocation -> Vulkan buffer/image copy regions. */
#include "nvn_vk_texture_stage.h"

#include "nvn_vk_format.h"
#include "../games/totk/third_party/d2r_gfx/gfx/nvn/include/nvn.h"
#include "../games/totk/third_party/d2r_gfx/gfx/tex/nvn_deswizzle.h"
#include "../games/totk/third_party/d2r_gfx/gfx/tex/nvn_bcdec.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define MAX_STAGED_DIMENSION (1u << 15)
#define MAX_STAGED_BYTES ((size_t)1 << 30)

static uint32_t mip_dim(uint32_t value, uint32_t level) {
    if (level >= 32) return 1;
    value >>= level;
    return value ? value : 1;
}

static uint32_t max_dim_for_target(const nvn_texdesc *desc) {
    uint32_t max_dim = desc->width;
    switch (desc->target) {
        case NVN_TEXTURE_TARGET_1D:
        case NVN_TEXTURE_TARGET_1D_ARRAY:
            break;
        case NVN_TEXTURE_TARGET_3D:
            if (desc->height > max_dim) max_dim = desc->height;
            if (desc->depth > max_dim) max_dim = desc->depth;
            break;
        default:
            if (desc->height > max_dim) max_dim = desc->height;
            break;
    }
    return max_dim;
}

static unsigned mip_count(uint32_t max_dim) {
    unsigned count = 1;
    while (max_dim > 1) {
        max_dim >>= 1;
        ++count;
    }
    return count;
}

static int supported_target(int target) {
    switch (target) {
        case NVN_TEXTURE_TARGET_1D:
        case NVN_TEXTURE_TARGET_2D:
        case NVN_TEXTURE_TARGET_3D:
        case NVN_TEXTURE_TARGET_1D_ARRAY:
        case NVN_TEXTURE_TARGET_2D_ARRAY:
        case NVN_TEXTURE_TARGET_RECTANGLE:
        case NVN_TEXTURE_TARGET_CUBEMAP:
        case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY:
            return 1;
        default:
            return 0;
    }
}

static int is_depth_stencil_format(int format) {
    return format >= NVN_FORMAT_STENCIL8 && format <= NVN_FORMAT_DEPTH32F_STENCIL8;
}

static size_t gcd_size(size_t a, size_t b) {
    while (b) {
        size_t next = a % b;
        a = b;
        b = next;
    }
    return a;
}

static int checked_lcm(size_t a, size_t b, size_t *out) {
    size_t divided = a / gcd_size(a, b);
    if (divided && b > SIZE_MAX / divided) return 0;
    *out = divided * b;
    return 1;
}

static int align_size(size_t value, size_t alignment, size_t *out) {
    size_t remainder = value % alignment;
    size_t padding = remainder ? alignment - remainder : 0;
    if (value > SIZE_MAX - padding) return 0;
    *out = value + padding;
    return 1;
}

static int subresource_bytes(uint32_t row_bytes, uint32_t rows, uint32_t slices,
                             size_t *out) {
    if (!row_bytes || !rows || !slices) return 0;
    size_t size = row_bytes;
    if (size > SIZE_MAX / rows) return 0;
    size *= rows;
    if (size > SIZE_MAX / slices) return 0;
    *out = size * slices;
    return 1;
}

static int texture_geometry(const nvn_texdesc *desc, const nvn_texlayout *layout,
                            VkImageType *image_type, VkImageViewType *view_type,
                            VkImageCreateFlags *image_flags,
                            VkExtent3D *extent, uint32_t *array_layers) {
    if (desc->target == NVN_TEXTURE_TARGET_1D) {
        *image_type = VK_IMAGE_TYPE_1D;
        *view_type = VK_IMAGE_VIEW_TYPE_1D;
        *extent = (VkExtent3D){desc->width, 1, 1};
        *array_layers = 1;
    } else if (desc->target == NVN_TEXTURE_TARGET_1D_ARRAY) {
        *image_type = VK_IMAGE_TYPE_1D;
        *view_type = VK_IMAGE_VIEW_TYPE_1D_ARRAY;
        *extent = (VkExtent3D){desc->width, 1, 1};
        *array_layers = desc->height;
    } else if (desc->target == NVN_TEXTURE_TARGET_3D) {
        *image_type = VK_IMAGE_TYPE_3D;
        *view_type = VK_IMAGE_VIEW_TYPE_3D;
        *extent = (VkExtent3D){desc->width, desc->height, desc->depth};
        *array_layers = 1;
    } else {
        *image_type = VK_IMAGE_TYPE_2D;
        *extent = (VkExtent3D){desc->width, desc->height, 1};
        *array_layers = layout->layers;
        if (desc->target == NVN_TEXTURE_TARGET_2D_ARRAY)
            *view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        else if (desc->target == NVN_TEXTURE_TARGET_CUBEMAP) {
            *view_type = VK_IMAGE_VIEW_TYPE_CUBE;
            *image_flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        } else if (desc->target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY) {
            *view_type = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
            *image_flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        } else
            *view_type = VK_IMAGE_VIEW_TYPE_2D;
    }
    return *array_layers != 0;
}

int nvn_vk_texture_shape(const nvn_texdesc *desc, VkImageType *image_type, VkImageViewType *view_type,
                         VkImageCreateFlags *image_flags, VkExtent3D *extent, uint32_t *array_layers,
                         uint32_t *mip_levels) {
    if (!desc || !supported_target(desc->target) || !desc->width) return 0;
    nvn_texlayout layout;
    nvn_tex_layout(desc, &layout);
    *image_flags = 0;
    if (!texture_geometry(desc, &layout, image_type, view_type, image_flags, extent, array_layers)) return 0;
    *mip_levels = desc->levels ? desc->levels : 1;
    return extent->width && extent->height && extent->depth;
}

static void texture_mip_extent(const nvn_texdesc *desc, uint32_t level,
                               VkExtent3D *extent) {
    extent->width = mip_dim(desc->width, level);
    extent->height = (desc->target == NVN_TEXTURE_TARGET_1D ||
                      desc->target == NVN_TEXTURE_TARGET_1D_ARRAY)
        ? 1 : mip_dim(desc->height, level);
    extent->depth = desc->target == NVN_TEXTURE_TARGET_3D
        ? mip_dim(desc->depth, level) : 1;
}

static NvnVkTextureStageResult get_geometry(const nvn_texdesc *desc,
                                            uint32_t levels,
                                            uint32_t layers,
                                            uint32_t level,
                                            uint32_t layer,
                                            uint32_t *row_bytes,
                                            uint32_t *rows,
                                            uint32_t *slices,
                                            uint64_t *base) {
    if (nvn_tex_is_linear(desc)) {
        if (level != 0 || layer != 0 || levels != 1 || layers != 1 ||
            (desc->target != NVN_TEXTURE_TARGET_1D &&
             desc->target != NVN_TEXTURE_TARGET_2D &&
             desc->target != NVN_TEXTURE_TARGET_RECTANGLE))
            return NVN_VK_STAGE_UNSUPPORTED_TARGET;
        int block_width = 0, block_height = 0;
        int bytes_per_block = nvn_fmt_bpb(desc->format, &block_width, &block_height);
        if (bytes_per_block <= 0 || block_width <= 0 || block_height <= 0)
            return NVN_VK_STAGE_UNSUPPORTED_FORMAT;
        uint64_t blocks_wide = ((uint64_t)desc->width + block_width - 1) / block_width;
        uint64_t block_rows = desc->target == NVN_TEXTURE_TARGET_1D ? 1
            : ((uint64_t)desc->height + block_height - 1) / block_height;
        uint64_t row_size = blocks_wide * (uint32_t)bytes_per_block;
        if (!row_size || row_size > UINT32_MAX || !block_rows || block_rows > UINT32_MAX)
            return NVN_VK_STAGE_SIZE_OVERFLOW;
        *row_bytes = (uint32_t)row_size;
        *rows = (uint32_t)block_rows;
        *slices = 1;
        *base = 0;
        return NVN_VK_STAGE_OK;
    }
    unsigned tile_h, tile_d;
    uint32_t bytes_per_block;
    if (level >= levels || layer >= layers) return NVN_VK_STAGE_INVALID_ARGUMENT;
    if (nvn_deswizzle_geom(desc, level, layer, row_bytes, rows, slices, base,
                           &tile_h, &tile_d, &bytes_per_block) != 0)
        return NVN_VK_STAGE_UNSUPPORTED_TARGET;
    if (bytes_per_block == 0) return NVN_VK_STAGE_UNSUPPORTED_FORMAT;
    if (!*row_bytes || !*rows || !*slices ||
        *base > UINT64_MAX - (uint64_t)*row_bytes * *rows * *slices)
        return NVN_VK_STAGE_SIZE_OVERFLOW;
    return NVN_VK_STAGE_OK;
}

static int region_count_for(uint32_t levels, uint32_t layers, int is_3d,
                            uint32_t *out) {
    uint64_t count = (uint64_t)levels * (is_3d ? 1u : layers);
    if (!count || count > UINT32_MAX || count > SIZE_MAX / sizeof(VkBufferImageCopy))
        return 0;
    *out = (uint32_t)count;
    return 1;
}

void nvn_vk_texture_stage_destroy(NvnVkTextureStage *stage) {
    if (!stage) return;
    free(stage->data);
    free(stage->regions);
    memset(stage, 0, sizeof *stage);
}

NvnVkTextureStageResult nvn_vk_texture_stage_plan(const nvn_texdesc *desc,
                                                  const void *source,
                                                  size_t source_size,
                                                  NvnVkTextureStage *out) {
    if (!out) return NVN_VK_STAGE_INVALID_ARGUMENT;
    memset(out, 0, sizeof *out);
    if (!desc || !source || !source_size) return NVN_VK_STAGE_INVALID_ARGUMENT;
    if (!supported_target(desc->target)) return NVN_VK_STAGE_UNSUPPORTED_TARGET;
    /* The packaged block height only shapes 2D-style layouts (nvn_tex_layout ignores it for 3D, whose first layout
       byte also exceeds 5: a 64x64 BC4 3D texture was refused here on every Thor run, 23 Oct). */
    if (!desc->width || desc->width > MAX_STAGED_DIMENSION ||
        desc->samples < 0 || desc->samples > 1 ||
        (desc->target != NVN_TEXTURE_TARGET_3D && (desc->pkg_block_h < 0 || desc->pkg_block_h > 5)))
        return NVN_VK_STAGE_INVALID_ARGUMENT;

    if (desc->target != NVN_TEXTURE_TARGET_1D &&
        desc->target != NVN_TEXTURE_TARGET_1D_ARRAY &&
        (!desc->height || desc->height > MAX_STAGED_DIMENSION))
        return NVN_VK_STAGE_INVALID_ARGUMENT;
    if (desc->target == NVN_TEXTURE_TARGET_3D &&
        (!desc->depth || desc->depth > MAX_STAGED_DIMENSION))
        return NVN_VK_STAGE_INVALID_ARGUMENT;
    if (desc->target == NVN_TEXTURE_TARGET_1D_ARRAY &&
        (!desc->height || desc->height > MAX_STAGED_DIMENSION))
        return NVN_VK_STAGE_INVALID_ARGUMENT;
    if (desc->target == NVN_TEXTURE_TARGET_2D_ARRAY &&
        (!desc->depth || desc->depth > MAX_STAGED_DIMENSION))
        return NVN_VK_STAGE_INVALID_ARGUMENT;
    if (desc->target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY &&
        (!desc->depth || desc->depth > MAX_STAGED_DIMENSION / 6))
        return NVN_VK_STAGE_INVALID_ARGUMENT;
    if ((desc->target == NVN_TEXTURE_TARGET_CUBEMAP ||
         desc->target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY) &&
        desc->width != desc->height)
        return NVN_VK_STAGE_INVALID_ARGUMENT;
    int linear = nvn_tex_is_linear(desc);
    if (linear && ((desc->levels > 1) ||
        (desc->target != NVN_TEXTURE_TARGET_1D &&
         desc->target != NVN_TEXTURE_TARGET_2D &&
         desc->target != NVN_TEXTURE_TARGET_RECTANGLE)))
        return NVN_VK_STAGE_UNSUPPORTED_TARGET;

    uint32_t levels = desc->levels ? desc->levels : 1;
    if (levels > 32 || levels > mip_count(max_dim_for_target(desc)))
        return NVN_VK_STAGE_INVALID_ARGUMENT;

    NvnVkFormat mapped;
    if (!nvn_vk_format_map((uint32_t)desc->format, &mapped) ||
        (mapped.flags & NVN_VK_FORMAT_PACKING_UNVERIFIED) ||
        is_depth_stencil_format(desc->format))
        return NVN_VK_STAGE_UNSUPPORTED_FORMAT;
    uint32_t selectors[4] = {
        desc->swizzle_r, desc->swizzle_g, desc->swizzle_b, desc->swizzle_a,
    };
    VkComponentMapping components;
    if (!nvn_vk_format_swizzle(&mapped, selectors, &components))
        return NVN_VK_STAGE_INVALID_ARGUMENT;

    nvn_texlayout layout;
    nvn_tex_layout(desc, &layout);
    if (!layout.size || !layout.layers || layout.size > SIZE_MAX)
        return NVN_VK_STAGE_SIZE_OVERFLOW;
    if (layout.layer_size > UINT64_MAX / layout.layers ||
        layout.layer_size * layout.layers != layout.size)
        return NVN_VK_STAGE_SIZE_OVERFLOW;
    if (layout.size > source_size) return NVN_VK_STAGE_SOURCE_TOO_SMALL;

    VkImageType image_type;
    VkImageViewType view_type;
    VkImageCreateFlags image_flags = 0;
    VkExtent3D extent;
    uint32_t array_layers;
    if (!texture_geometry(desc, &layout, &image_type, &view_type, &image_flags, &extent,
                          &array_layers))
        return NVN_VK_STAGE_INVALID_ARGUMENT;
    if (array_layers != layout.layers || array_layers > MAX_STAGED_DIMENSION ||
        (image_type == VK_IMAGE_TYPE_2D && extent.height > MAX_STAGED_DIMENSION) ||
        (image_type == VK_IMAGE_TYPE_3D && extent.depth > MAX_STAGED_DIMENSION))
        return NVN_VK_STAGE_INVALID_ARGUMENT;

    int is_3d = image_type == VK_IMAGE_TYPE_3D;
    uint32_t subresource_layers = is_3d ? 1 : array_layers;
    uint32_t region_count;
    if (!region_count_for(levels, subresource_layers, is_3d, &region_count))
        return NVN_VK_STAGE_SIZE_OVERFLOW;

    size_t offset = 0;
    for (uint32_t level = 0; level < levels; ++level) {
        for (uint32_t layer = 0; layer < subresource_layers; ++layer) {
            uint32_t row_bytes, rows, slices;
            uint64_t base;
            NvnVkTextureStageResult result = get_geometry(
                desc, levels, subresource_layers, level, layer,
                &row_bytes, &rows, &slices, &base);
            if (result != NVN_VK_STAGE_OK) return result;
            if (slices != (is_3d ? mip_dim(desc->depth, level) : 1))
                return NVN_VK_STAGE_UNSUPPORTED_TARGET;
            size_t alignment;
            if (!checked_lcm(4, mapped.bytes_per_block, &alignment))
                return NVN_VK_STAGE_SIZE_OVERFLOW;
            size_t bytes, start;
            if (!subresource_bytes(row_bytes, rows, slices, &bytes) ||
                !align_size(offset, alignment, &start) || bytes > SIZE_MAX - start)
                return NVN_VK_STAGE_SIZE_OVERFLOW;
            offset = start + bytes;
            if (base > layout.size || bytes > layout.size - (size_t)base)
                return NVN_VK_STAGE_SOURCE_TOO_SMALL;
        }
    }
    if (!offset || offset > MAX_STAGED_BYTES)
        return NVN_VK_STAGE_SIZE_OVERFLOW;

    NvnVkTextureStage staged = {
        .regions = calloc(region_count, sizeof(VkBufferImageCopy)),
        .data_size = offset,
        .region_count = region_count,
        .format = mapped.format,
        .components = components,
        .format_flags = mapped.flags,
        .image_type = image_type,
        .view_type = view_type,
        .image_flags = image_flags,
        .extent = extent,
        .mip_levels = levels,
        .array_layers = array_layers,
    };
    if (!staged.regions) return NVN_VK_STAGE_OUT_OF_MEMORY;

    offset = 0;
    uint32_t region_index = 0;
    for (uint32_t level = 0; level < levels; ++level) {
        for (uint32_t layer = 0; layer < subresource_layers; ++layer) {
            uint32_t row_bytes, rows, slices;
            uint64_t base;
            NvnVkTextureStageResult result = get_geometry(
                desc, levels, subresource_layers, level, layer,
                &row_bytes, &rows, &slices, &base);
            if (result != NVN_VK_STAGE_OK) {
                nvn_vk_texture_stage_destroy(&staged);
                return result;
            }
            size_t alignment, bytes, start;
            if (!checked_lcm(4, mapped.bytes_per_block, &alignment) ||
                !subresource_bytes(row_bytes, rows, slices, &bytes) ||
                !align_size(offset, alignment, &start) ||
                start > staged.data_size ||
                bytes > staged.data_size - start) {
                nvn_vk_texture_stage_destroy(&staged);
                return NVN_VK_STAGE_SIZE_OVERFLOW;
            }
            VkExtent3D mip_extent;
            texture_mip_extent(desc, level, &mip_extent);
            staged.regions[region_index++] = (VkBufferImageCopy){
                .bufferOffset = (VkDeviceSize)start,
                .bufferRowLength = 0,
                .bufferImageHeight = 0,
                .imageSubresource = {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .mipLevel = level,
                    .baseArrayLayer = is_3d ? 0 : layer,
                    .layerCount = 1,
                },
                .imageOffset = {0, 0, 0},
                .imageExtent = mip_extent,
            };
            offset = start + bytes;
        }
    }
    *out = staged;
    return NVN_VK_STAGE_OK;
}

NvnVkTextureStageResult nvn_vk_texture_stage_fill(const nvn_texdesc *desc,
                                                  const void *source,
                                                  const NvnVkTextureStage *plan,
                                                  uint32_t region, uint32_t r0,
                                                  uint32_t r1, uint8_t *dst,
                                                  uint32_t *total_rows) {
    if (!desc || !plan || region >= plan->region_count) return NVN_VK_STAGE_INVALID_ARGUMENT;
    const VkBufferImageCopy *r = &plan->regions[region];
    int is_3d = plan->image_type == VK_IMAGE_TYPE_3D;
    uint32_t level = r->imageSubresource.mipLevel, layer = is_3d ? 0 : r->imageSubresource.baseArrayLayer;
    uint32_t row_bytes, rows, slices;
    uint64_t base;
    NvnVkTextureStageResult result = get_geometry(desc, plan->mip_levels, is_3d ? 1 : plan->array_layers,
                                                  level, layer, &row_bytes, &rows, &slices, &base);
    if (result != NVN_VK_STAGE_OK) return result;
    /* A 2D region can be a full-width row strip. r0/r1 are relative to that strip, including on helper threads.
       Compressed formats count block rows; the final strip may end at a non-block-aligned image edge. */
    int bw, bh;
    if (!nvn_fmt_bpb(desc->format, &bw, &bh) || bh <= 0 || r->imageOffset.x || r->imageOffset.z || r->imageOffset.y < 0
        || (uint32_t)r->imageOffset.y % (uint32_t)bh || r->bufferRowLength || r->bufferImageHeight)
        return NVN_VK_STAGE_INVALID_ARGUMENT;
    uint32_t first = (uint32_t)r->imageOffset.y / (uint32_t)bh;
    uint32_t count = is_3d ? rows * slices : (r->imageExtent.height + (uint32_t)bh - 1) / (uint32_t)bh;
    if (first > rows || (!is_3d && count > rows - first) || (is_3d && first)) return NVN_VK_STAGE_INVALID_ARGUMENT;
    if (total_rows) *total_rows = count;
    if (r1 > count) r1 = count;
    if (r0 >= r1) return NVN_VK_STAGE_OK;
    if (nvn_deswizzle_rows_compact(desc, level, layer, source, dst + r->bufferOffset, row_bytes,
                                   first, first + r0, first + r1) != 0)
        return NVN_VK_STAGE_DESWIZZLE_FAILED;
    return NVN_VK_STAGE_OK;
}

NvnVkTextureStageResult nvn_vk_texture_stage(const nvn_texdesc *desc,
                                             const void *source,
                                             size_t source_size,
                                             NvnVkTextureStage *out) {
    NvnVkTextureStageResult result = nvn_vk_texture_stage_plan(desc, source, source_size, out);
    if (result != NVN_VK_STAGE_OK) return result;
    out->data = calloc(1, out->data_size);
    if (!out->data) {
        nvn_vk_texture_stage_destroy(out);
        return NVN_VK_STAGE_OUT_OF_MEMORY;
    }
    for (uint32_t i = 0; i < out->region_count; ++i) {
        result = nvn_vk_texture_stage_fill(desc, source, out, i, 0, UINT32_MAX, out->data, NULL);
        if (result != NVN_VK_STAGE_OK) {
            nvn_vk_texture_stage_destroy(out);
            return result;
        }
    }
    return NVN_VK_STAGE_OK;
}

static VkFormat bc_fallback_format(int format, uint32_t *bytes_per_pixel) {
    switch (format) {
        case NVN_FORMAT_RGB_DXT1:
        case NVN_FORMAT_RGBA_DXT1:
        case NVN_FORMAT_RGBA_DXT3:
        case NVN_FORMAT_RGBA_DXT5:
        case NVN_FORMAT_BPTC_UNORM:
            *bytes_per_pixel = 4;
            return VK_FORMAT_R8G8B8A8_UNORM;
        case NVN_FORMAT_RGB_DXT1_SRGB:
        case NVN_FORMAT_RGBA_DXT1_SRGB:
        case NVN_FORMAT_RGBA_DXT3_SRGB:
        case NVN_FORMAT_RGBA_DXT5_SRGB:
        case NVN_FORMAT_BPTC_UNORM_SRGB:
            *bytes_per_pixel = 4;
            return VK_FORMAT_R8G8B8A8_SRGB;
        case NVN_FORMAT_RGTC1_UNORM:
            *bytes_per_pixel = 1;
            return VK_FORMAT_R8_UNORM;
        case NVN_FORMAT_RGTC1_SNORM:
            *bytes_per_pixel = 1;
            return VK_FORMAT_R8_SNORM;
        case NVN_FORMAT_RGTC2_UNORM:
            *bytes_per_pixel = 2;
            return VK_FORMAT_R8G8_UNORM;
        case NVN_FORMAT_RGTC2_SNORM:
            *bytes_per_pixel = 2;
            return VK_FORMAT_R8G8_SNORM;
        case NVN_FORMAT_BPTC_SFLOAT:
        case NVN_FORMAT_BPTC_UFLOAT:
            *bytes_per_pixel = 8;
            return VK_FORMAT_R16G16B16A16_SFLOAT;
        default:
            return VK_FORMAT_UNDEFINED;
    }
}

NvnVkTextureStageResult nvn_vk_texture_stage_with_bc_fallback(
    const nvn_texdesc *desc, const void *source, size_t source_size,
    int bc_supported, NvnVkTextureStage *out) {
    if (!out) return NVN_VK_STAGE_INVALID_ARGUMENT;
    if (bc_supported || !desc) return nvn_vk_texture_stage(desc, source, source_size, out);

    uint32_t bytes_per_pixel = 0;
    VkFormat fallback = bc_fallback_format(desc->format, &bytes_per_pixel);
    if (fallback == VK_FORMAT_UNDEFINED)
        return nvn_vk_texture_stage(desc, source, source_size, out);

    NvnVkTextureStage compressed;
    NvnVkTextureStageResult result = nvn_vk_texture_stage(
        desc, source, source_size, &compressed);
    if (result != NVN_VK_STAGE_OK) return result;

    NvnVkFormat original;
    if (!nvn_vk_format_map((uint32_t)desc->format, &original)) {
        nvn_vk_texture_stage_destroy(&compressed);
        return NVN_VK_STAGE_UNSUPPORTED_FORMAT;
    }

    size_t size = 0;
    VkBufferImageCopy *regions = calloc(compressed.region_count, sizeof *regions);
    if (!regions) {
        nvn_vk_texture_stage_destroy(&compressed);
        return NVN_VK_STAGE_OUT_OF_MEMORY;
    }
    for (uint32_t i = 0; i < compressed.region_count; ++i) {
        const VkBufferImageCopy *src = &compressed.regions[i];
        size_t pixels, bytes, aligned;
        if (!subresource_bytes(src->imageExtent.width, src->imageExtent.height,
                               src->imageExtent.depth, &pixels) ||
            pixels > SIZE_MAX / bytes_per_pixel ||
            !align_size(size, 4, &aligned) ||
            aligned > MAX_STAGED_BYTES ||
            pixels * bytes_per_pixel > MAX_STAGED_BYTES - aligned) {
            free(regions);
            nvn_vk_texture_stage_destroy(&compressed);
            return NVN_VK_STAGE_SIZE_OVERFLOW;
        }
        bytes = pixels * bytes_per_pixel;
        regions[i] = *src;
        regions[i].bufferOffset = (VkDeviceSize)aligned;
        size = aligned + bytes;
    }

    uint8_t *data = calloc(1, size);
    if (!data) {
        free(regions);
        nvn_vk_texture_stage_destroy(&compressed);
        return NVN_VK_STAGE_OUT_OF_MEMORY;
    }
    for (uint32_t i = 0; i < compressed.region_count; ++i) {
        const VkExtent3D extent = compressed.regions[i].imageExtent;
        size_t block_rows = ((size_t)extent.height + 3) / 4;
        size_t block_pitch = ((size_t)extent.width + 3) / 4 * original.bytes_per_block;
        size_t source_slice = block_pitch * block_rows;
        size_t dest_pitch = (size_t)extent.width * bytes_per_pixel;
        size_t dest_slice = dest_pitch * extent.height;
        size_t old_offset = (size_t)compressed.regions[i].bufferOffset;
        size_t new_offset = (size_t)regions[i].bufferOffset;
        if (old_offset > compressed.data_size ||
            source_slice > (compressed.data_size - old_offset) / extent.depth ||
            new_offset > size || dest_slice > (size - new_offset) / extent.depth) {
            free(data);
            free(regions);
            nvn_vk_texture_stage_destroy(&compressed);
            return NVN_VK_STAGE_SIZE_OVERFLOW;
        }
        for (uint32_t z = 0; z < extent.depth; ++z) {
            if (nvn_bc_decode(desc->format,
                              (const uint8_t *)compressed.data + old_offset + z * source_slice,
                              block_pitch, extent.width, extent.height,
                              data + new_offset + z * dest_slice, dest_pitch) != 0) {
                free(data);
                free(regions);
                nvn_vk_texture_stage_destroy(&compressed);
                return NVN_VK_STAGE_UNSUPPORTED_FORMAT;
            }
        }
    }

    free(compressed.data);
    free(compressed.regions);
    compressed.data = data;
    compressed.data_size = size;
    compressed.regions = regions;
    compressed.format = fallback;
    *out = compressed;
    return NVN_VK_STAGE_OK;
}
