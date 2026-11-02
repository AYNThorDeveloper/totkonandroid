/* Host-side staging data for NVN textures copied into Vulkan images. */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#include "../games/totk/third_party/d2r_gfx/gfx/nvn/nvn_tex.h"

typedef enum {
    NVN_VK_STAGE_OK = 0,
    NVN_VK_STAGE_INVALID_ARGUMENT = -1,
    NVN_VK_STAGE_UNSUPPORTED_TARGET = -2,
    NVN_VK_STAGE_UNSUPPORTED_FORMAT = -3,
    NVN_VK_STAGE_SOURCE_TOO_SMALL = -4,
    NVN_VK_STAGE_SIZE_OVERFLOW = -5,
    NVN_VK_STAGE_OUT_OF_MEMORY = -6,
    NVN_VK_STAGE_DESWIZZLE_FAILED = -7,
} NvnVkTextureStageResult;

typedef struct {
    void *data;
    size_t data_size;
    VkBufferImageCopy *regions;
    uint32_t region_count;

    VkFormat format;
    /* NVN texture swizzle composed with format-specific fallback channels. */
    VkComponentMapping components;
    uint32_t format_flags;
    VkImageType image_type;
    VkImageViewType view_type;
    VkImageCreateFlags image_flags;
    VkExtent3D extent;
    uint32_t mip_levels;
    uint32_t array_layers;
} NvnVkTextureStage;

/*
 * Unpacks a block-linear or supported pitch/linear NVN allocation into tightly
 * packed color subresources suitable for VkBufferImageCopy (bufferRowLength
 * and bufferImageHeight are zero). The destination image must be created with
 * VK_IMAGE_USAGE_TRANSFER_DST_BIT.
 * Packed formats whose NVN bit order is still unverified are rejected.
 */
NvnVkTextureStageResult nvn_vk_texture_stage(const nvn_texdesc *desc,
                                             const void *source,
                                             size_t source_size,
                                             NvnVkTextureStage *out);

/* Pass bc_supported=0 when the device cannot sample/copy the mapped BC
 * format. The existing decoder then expands BC1-7 to an uncompressed image;
 * mip/layer regions and the NVN view swizzle are retained. */
NvnVkTextureStageResult nvn_vk_texture_stage_with_bc_fallback(
    const nvn_texdesc *desc, const void *source, size_t source_size,
    int bc_supported, NvnVkTextureStage *out);

/* nvn_vk_texture_stage in two steps, so the texels can go straight into mapped upload memory and several threads can
 * share one texture: _plan checks the texture and sets everything but data (NULL); _fill writes rows [r0, r1) of
 * region `region` (3D: slice k's block row y is row k * rows + y) to dst + regions[region].bufferOffset, and returns
 * the region's row count in *total_rows when that is not NULL (r0 >= r1 only asks for it). */
NvnVkTextureStageResult nvn_vk_texture_stage_plan(const nvn_texdesc *desc,
                                                  const void *source,
                                                  size_t source_size,
                                                  NvnVkTextureStage *out);
/* Regions may also be full-width 2D row strips. imageOffset.y identifies their source row;
 * r0/r1 and total_rows are relative to the strip (compressed formats count block rows). */
NvnVkTextureStageResult nvn_vk_texture_stage_fill(const nvn_texdesc *desc,
                                                  const void *source,
                                                  const NvnVkTextureStage *plan,
                                                  uint32_t region, uint32_t r0,
                                                  uint32_t r1, uint8_t *dst,
                                                  uint32_t *total_rows);

void nvn_vk_texture_stage_destroy(NvnVkTextureStage *stage);

/* Vulkan image shape of an NVN texture (any supported format, including
 * depth/stencil, which staging rejects). Returns 0 for unsupported targets. */
int nvn_vk_texture_shape(const nvn_texdesc *desc, VkImageType *image_type, VkImageViewType *view_type,
                         VkImageCreateFlags *image_flags, VkExtent3D *extent, uint32_t *array_layers,
                         uint32_t *mip_levels);
