/* NVN sampler descriptors mapped to Vulkan sampler state. */
#pragma once

#include <stdint.h>
#include <vulkan/vulkan.h>

#include "../games/totk/third_party/d2r_gfx/gfx/nvn/nvn_obj.h"

enum {
    NVN_VK_SAMPLER_NEEDS_ANISOTROPY = 1u << 0,
    NVN_VK_SAMPLER_NEEDS_MIRROR_CLAMP = 1u << 1,
    NVN_VK_SAMPLER_NEEDS_MINMAX_REDUCTION = 1u << 2,
};

typedef enum {
    NVN_VK_SAMPLER_OK = 0,
    NVN_VK_SAMPLER_INVALID_ARGUMENT = -1,
    NVN_VK_SAMPLER_UNSUPPORTED_FILTER = -2,
    NVN_VK_SAMPLER_UNSUPPORTED_WRAP = -3,
    NVN_VK_SAMPLER_UNSUPPORTED_BORDER = -4,
    NVN_VK_SAMPLER_UNSUPPORTED_COMPARE = -5,
    NVN_VK_SAMPLER_UNSUPPORTED_REDUCTION = -6,
} NvnVkSamplerResult;

/*
 * Maps the NVN sampler fields represented by samp_par. Optional Vulkan
 * features/extensions are reported in requirements and are never silently
 * enabled by this mapper. `reduction` remains caller-owned for the lifetime of
 * `out->pNext` when NVN min/max reduction is selected.
 */
NvnVkSamplerResult nvn_vk_sampler_map(const samp_par *source,
                                      VkSamplerCreateInfo *out,
                                      VkSamplerReductionModeCreateInfo *reduction,
                                      uint32_t *requirements);
