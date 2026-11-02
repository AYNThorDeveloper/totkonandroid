/* NVN sampler state translation for Vulkan. */
#include "nvn_vk_sampler.h"

#include "../games/totk/third_party/d2r_gfx/gfx/nvn/include/nvn.h"

#include <math.h>
#include <string.h>

static int map_min_filter(uint8_t nvn, VkFilter *filter,
                          VkSamplerMipmapMode *mipmap_mode) {
    switch (nvn) {
        case NVN_MIN_FILTER_NEAREST:
            *filter = VK_FILTER_NEAREST;
            *mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            return 1;
        case NVN_MIN_FILTER_LINEAR:
            *filter = VK_FILTER_LINEAR;
            *mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            return 1;
        case NVN_MIN_FILTER_NEAREST_MIPMAP_NEAREST:
            *filter = VK_FILTER_NEAREST;
            *mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            return 1;
        case NVN_MIN_FILTER_LINEAR_MIPMAP_NEAREST:
            *filter = VK_FILTER_LINEAR;
            *mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            return 1;
        case NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR:
            *filter = VK_FILTER_NEAREST;
            *mipmap_mode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            return 1;
        case NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR:
            *filter = VK_FILTER_LINEAR;
            *mipmap_mode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            return 1;
        default:
            return 0;
    }
}

static int map_wrap(uint8_t nvn, VkSamplerAddressMode *out,
                    uint32_t *requirements) {
    switch (nvn) {
        case NVN_WRAP_MODE_CLAMP:
        case NVN_WRAP_MODE_CLAMP_TO_EDGE:
            *out = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            return 1;
        case NVN_WRAP_MODE_REPEAT:
            *out = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            return 1;
        case NVN_WRAP_MODE_MIRROR_CLAMP:
        case NVN_WRAP_MODE_MIRROR_CLAMP_TO_EDGE:
            *out = VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
            *requirements |= NVN_VK_SAMPLER_NEEDS_MIRROR_CLAMP;
            return 1;
        case NVN_WRAP_MODE_CLAMP_TO_BORDER:
            *out = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
            return 1;
        case NVN_WRAP_MODE_MIRRORED_REPEAT:
            *out = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
            return 1;
        case NVN_WRAP_MODE_MIRROR_CLAMP_TO_BORDER:
        default:
            return 0;
    }
}

static int near_value(float value, float expected) {
    float difference = value - expected;
    return difference > -0.0001f && difference < 0.0001f;
}

static int map_border(const float color[4], VkBorderColor *out) {
    if (near_value(color[0], 0.0f) && near_value(color[1], 0.0f) &&
        near_value(color[2], 0.0f) && near_value(color[3], 0.0f)) {
        *out = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        return 1;
    }
    if (near_value(color[0], 0.0f) && near_value(color[1], 0.0f) &&
        near_value(color[2], 0.0f) && near_value(color[3], 1.0f)) {
        *out = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        return 1;
    }
    if (near_value(color[0], 1.0f) && near_value(color[1], 1.0f) &&
        near_value(color[2], 1.0f) && near_value(color[3], 1.0f)) {
        *out = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        return 1;
    }
    return 0;
}

NvnVkSamplerResult nvn_vk_sampler_map(const samp_par *source,
                                      VkSamplerCreateInfo *out,
                                      VkSamplerReductionModeCreateInfo *reduction,
                                      uint32_t *requirements) {
    if (!source || !out || !reduction || !requirements)
        return NVN_VK_SAMPLER_INVALID_ARGUMENT;
    *requirements = 0;
    memset(out, 0, sizeof *out);
    memset(reduction, 0, sizeof *reduction);

    VkFilter min_filter;
    VkSamplerMipmapMode mipmap_mode;
    if (!map_min_filter(source->minf, &min_filter, &mipmap_mode) ||
        source->magf > NVN_MAG_FILTER_LINEAR)
        return NVN_VK_SAMPLER_UNSUPPORTED_FILTER;
    if (!isfinite(source->lod_min) || !isfinite(source->lod_max) ||
        !isfinite(source->lod_bias) || !isfinite(source->aniso) ||
        source->lod_min > source->lod_max)
        return NVN_VK_SAMPLER_INVALID_ARGUMENT;

    VkSamplerAddressMode address_u, address_v, address_w;
    if (!map_wrap(source->ws, &address_u, requirements) ||
        !map_wrap(source->wt, &address_v, requirements) ||
        !map_wrap(source->wr, &address_w, requirements))
        return NVN_VK_SAMPLER_UNSUPPORTED_WRAP;

    if (source->cmp_mode != NVN_COMPARE_MODE_NONE &&
        source->cmp_mode != NVN_COMPARE_MODE_COMPARE_R_TO_TEXTURE)
        return NVN_VK_SAMPLER_UNSUPPORTED_COMPARE;
    VkCompareOp compare_op = VK_COMPARE_OP_ALWAYS;
    if (source->cmp_mode == NVN_COMPARE_MODE_COMPARE_R_TO_TEXTURE) {
        if (source->cmp_func < NVN_COMPARE_FUNC_NEVER ||
            source->cmp_func > NVN_COMPARE_FUNC_ALWAYS)
            return NVN_VK_SAMPLER_UNSUPPORTED_COMPARE;
        compare_op = (VkCompareOp)(source->cmp_func - NVN_COMPARE_FUNC_NEVER);
    }

    VkSamplerReductionMode reduction_mode;
    switch (source->reduction) {
        case NVN_SAMPLER_REDUCTION_AVERAGE:
            reduction_mode = VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE;
            break;
        case NVN_SAMPLER_REDUCTION_MIN:
            reduction_mode = VK_SAMPLER_REDUCTION_MODE_MIN;
            *requirements |= NVN_VK_SAMPLER_NEEDS_MINMAX_REDUCTION;
            break;
        case NVN_SAMPLER_REDUCTION_MAX:
            reduction_mode = VK_SAMPLER_REDUCTION_MODE_MAX;
            *requirements |= NVN_VK_SAMPLER_NEEDS_MINMAX_REDUCTION;
            break;
        default:
            return NVN_VK_SAMPLER_UNSUPPORTED_REDUCTION;
    }

    VkBorderColor border = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if (address_u == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
        address_v == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
        address_w == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER) {
        if (!map_border(source->border, &border))
            return NVN_VK_SAMPLER_UNSUPPORTED_BORDER;
    }

    out->sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    out->magFilter = source->magf == NVN_MAG_FILTER_LINEAR
        ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    out->minFilter = min_filter;
    out->mipmapMode = mipmap_mode;
    out->addressModeU = address_u;
    out->addressModeV = address_v;
    out->addressModeW = address_w;
    out->mipLodBias = source->lod_bias;
    out->anisotropyEnable = source->aniso > 1.0f ? VK_TRUE : VK_FALSE;
    out->maxAnisotropy = source->aniso < 1.0f ? 1.0f
        : source->aniso > 16.0f ? 16.0f : source->aniso;
    if (out->anisotropyEnable) *requirements |= NVN_VK_SAMPLER_NEEDS_ANISOTROPY;
    out->compareEnable = source->cmp_mode != NVN_COMPARE_MODE_NONE;
    out->compareOp = compare_op;
    out->minLod = source->lod_min < 0.0f ? 0.0f : source->lod_min;
    out->maxLod = source->lod_max < 0.0f ? 0.0f : source->lod_max;
    if (source->minf == NVN_MIN_FILTER_NEAREST || source->minf == NVN_MIN_FILTER_LINEAR) {
        /* NVN's non-mipmapped GL filters: Vulkan needs an explicit LOD clamp.
         * 0.25 retains the minification filter while rounding to mip zero. */
        out->minLod = 0.0f;
        out->maxLod = 0.25f;
    }
    out->borderColor = border;
    out->unnormalizedCoordinates = VK_FALSE;

    if (reduction_mode != VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE) {
        reduction->sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO;
        reduction->reductionMode = reduction_mode;
        out->pNext = reduction;
    }
    return NVN_VK_SAMPLER_OK;
}
