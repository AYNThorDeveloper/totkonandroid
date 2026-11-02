#include <stdlib.h>
#include <string.h>
/* NVN (Nintendo Switch) texture formats to Vulkan. */
#include "nvn_vk_format.h"
#ifdef VK_NO_PROTOTYPES
#include "nvk_vk_dispatch.h"   /* the game loader calls Vulkan through pointers */
#endif

#include "../games/totk/third_party/d2r_gfx/gfx/nvn/include/nvn.h"
#include "../games/totk/third_party/d2r_gfx/gfx/nvn/nvn_tex.h"

typedef struct {
    VkFormat format;
    uint32_t flags;
    uint8_t alpha_one;
} FormatEntry;

#define E(f)       {VK_FORMAT_##f, 0, 0}
#define A(f)       {VK_FORMAT_##f, 0, 1}
#define P(f)       {VK_FORMAT_##f, NVN_VK_FORMAT_PACKING_UNVERIFIED, 0}
#define PA(f)      {VK_FORMAT_##f, NVN_VK_FORMAT_PACKING_UNVERIFIED, 1}
#define S(f)       {VK_FORMAT_##f, NVN_VK_FORMAT_SHADER_READ_CONVERSION, 0}
#define SP(f)      {VK_FORMAT_##f, NVN_VK_FORMAT_SHADER_READ_CONVERSION | NVN_VK_FORMAT_PACKING_UNVERIFIED, 0}

static const FormatEntry formats[0x9D] = {
    [NVN_FORMAT_R8] = E(R8_UNORM),
    [NVN_FORMAT_R8SN] = E(R8_SNORM),
    [NVN_FORMAT_R8UI] = E(R8_UINT),
    [NVN_FORMAT_R8I] = E(R8_SINT),
    [NVN_FORMAT_R16F] = E(R16_SFLOAT),
    [NVN_FORMAT_R16] = E(R16_UNORM),
    [NVN_FORMAT_R16SN] = E(R16_SNORM),
    [NVN_FORMAT_R16UI] = E(R16_UINT),
    [NVN_FORMAT_R16I] = E(R16_SINT),
    [NVN_FORMAT_R32F] = E(R32_SFLOAT),
    [NVN_FORMAT_R32UI] = E(R32_UINT),
    [NVN_FORMAT_R32I] = E(R32_SINT),

    [NVN_FORMAT_RG8] = E(R8G8_UNORM),
    [NVN_FORMAT_RG8SN] = E(R8G8_SNORM),
    [NVN_FORMAT_RG8UI] = E(R8G8_UINT),
    [NVN_FORMAT_RG8I] = E(R8G8_SINT),
    [NVN_FORMAT_RG16F] = E(R16G16_SFLOAT),
    [NVN_FORMAT_RG16] = E(R16G16_UNORM),
    [NVN_FORMAT_RG16SN] = E(R16G16_SNORM),
    [NVN_FORMAT_RG16UI] = E(R16G16_UINT),
    [NVN_FORMAT_RG16I] = E(R16G16_SINT),
    [NVN_FORMAT_RG32F] = E(R32G32_SFLOAT),
    [NVN_FORMAT_RG32UI] = E(R32G32_UINT),
    [NVN_FORMAT_RG32I] = E(R32G32_SINT),

    [NVN_FORMAT_RGB8] = E(R8G8B8_UNORM),
    [NVN_FORMAT_RGB8SN] = E(R8G8B8_SNORM),
    [NVN_FORMAT_RGB8UI] = E(R8G8B8_UINT),
    [NVN_FORMAT_RGB8I] = E(R8G8B8_SINT),
    [NVN_FORMAT_RGB16F] = E(R16G16B16_SFLOAT),
    [NVN_FORMAT_RGB16] = E(R16G16B16_UNORM),
    [NVN_FORMAT_RGB16SN] = E(R16G16B16_SNORM),
    [NVN_FORMAT_RGB16UI] = E(R16G16B16_UINT),
    [NVN_FORMAT_RGB16I] = E(R16G16B16_SINT),
    [NVN_FORMAT_RGB32F] = E(R32G32B32_SFLOAT),
    [NVN_FORMAT_RGB32UI] = E(R32G32B32_UINT),
    [NVN_FORMAT_RGB32I] = E(R32G32B32_SINT),

    [NVN_FORMAT_RGBA8] = E(R8G8B8A8_UNORM),
    [NVN_FORMAT_RGBA8SN] = E(R8G8B8A8_SNORM),
    [NVN_FORMAT_RGBA8UI] = E(R8G8B8A8_UINT),
    [NVN_FORMAT_RGBA8I] = E(R8G8B8A8_SINT),
    [NVN_FORMAT_RGBA16F] = E(R16G16B16A16_SFLOAT),
    [NVN_FORMAT_RGBA16] = E(R16G16B16A16_UNORM),
    [NVN_FORMAT_RGBA16SN] = E(R16G16B16A16_SNORM),
    [NVN_FORMAT_RGBA16UI] = E(R16G16B16A16_UINT),
    [NVN_FORMAT_RGBA16I] = E(R16G16B16A16_SINT),
    [NVN_FORMAT_RGBA32F] = E(R32G32B32A32_SFLOAT),
    [NVN_FORMAT_RGBA32UI] = E(R32G32B32A32_UINT),
    [NVN_FORMAT_RGBA32I] = E(R32G32B32A32_SINT),

    [NVN_FORMAT_STENCIL8] = E(S8_UINT),
    [NVN_FORMAT_DEPTH16] = E(D16_UNORM),
    [NVN_FORMAT_DEPTH24] = E(X8_D24_UNORM_PACK32),
    [NVN_FORMAT_DEPTH32F] = E(D32_SFLOAT),
    [NVN_FORMAT_DEPTH24_STENCIL8] = E(D24_UNORM_S8_UINT),
    [NVN_FORMAT_DEPTH32F_STENCIL8] = E(D32_SFLOAT_S8_UINT),
    [NVN_FORMAT_RGBX8_SRGB] = A(R8G8B8A8_SRGB),
    [NVN_FORMAT_RGBA8_SRGB] = E(R8G8B8A8_SRGB),

    /* 16-bit legacy layouts remain provisional. The 32-bit layouts below
     * match the existing nvn_fmt_mtl.c path: R occupies the low bits. */
    [NVN_FORMAT_RGBA4] = P(R4G4B4A4_UNORM_PACK16),
    [NVN_FORMAT_RGB5] = PA(R5G5B5A1_UNORM_PACK16),
    [NVN_FORMAT_RGB5A1] = P(R5G5B5A1_UNORM_PACK16),
    [NVN_FORMAT_RGB565] = PA(R5G6B5_UNORM_PACK16),
    [NVN_FORMAT_RGB10A2] = E(A2B10G10R10_UNORM_PACK32),
    [NVN_FORMAT_RGB10A2UI] = E(A2B10G10R10_UINT_PACK32),
    [NVN_FORMAT_R11G11B10F] = E(B10G11R11_UFLOAT_PACK32),
    [NVN_FORMAT_RGB9E5F] = E(E5B9G9R9_UFLOAT_PACK32),

    [NVN_FORMAT_RGB_DXT1] = A(BC1_RGB_UNORM_BLOCK),
    [NVN_FORMAT_RGBA_DXT1] = E(BC1_RGBA_UNORM_BLOCK),
    [NVN_FORMAT_RGBA_DXT3] = E(BC2_UNORM_BLOCK),
    [NVN_FORMAT_RGBA_DXT5] = E(BC3_UNORM_BLOCK),
    [NVN_FORMAT_RGB_DXT1_SRGB] = A(BC1_RGB_SRGB_BLOCK),
    [NVN_FORMAT_RGBA_DXT1_SRGB] = E(BC1_RGBA_SRGB_BLOCK),
    [NVN_FORMAT_RGBA_DXT3_SRGB] = E(BC2_SRGB_BLOCK),
    [NVN_FORMAT_RGBA_DXT5_SRGB] = E(BC3_SRGB_BLOCK),
    [NVN_FORMAT_RGTC1_UNORM] = E(BC4_UNORM_BLOCK),
    [NVN_FORMAT_RGTC1_SNORM] = E(BC4_SNORM_BLOCK),
    [NVN_FORMAT_RGTC2_UNORM] = E(BC5_UNORM_BLOCK),
    [NVN_FORMAT_RGTC2_SNORM] = E(BC5_SNORM_BLOCK),
    [NVN_FORMAT_BPTC_UNORM] = E(BC7_UNORM_BLOCK),
    [NVN_FORMAT_BPTC_UNORM_SRGB] = E(BC7_SRGB_BLOCK),
    [NVN_FORMAT_BPTC_SFLOAT] = E(BC6H_SFLOAT_BLOCK),
    [NVN_FORMAT_BPTC_UFLOAT] = E(BC6H_UFLOAT_BLOCK),

    /* UI2F/I2F stores integer texels but samples them as floating point. */
    [NVN_FORMAT_R8_UI2F] = S(R8_UINT),
    [NVN_FORMAT_R8_I2F] = S(R8_SINT),
    [NVN_FORMAT_R16_UI2F] = S(R16_UINT),
    [NVN_FORMAT_R16_I2F] = S(R16_SINT),
    [NVN_FORMAT_R32_UI2F] = S(R32_UINT),
    [NVN_FORMAT_R32_I2F] = S(R32_SINT),
    [NVN_FORMAT_RG8_UI2F] = S(R8G8_UINT),
    [NVN_FORMAT_RG8_I2F] = S(R8G8_SINT),
    [NVN_FORMAT_RG16_UI2F] = S(R16G16_UINT),
    [NVN_FORMAT_RG16_I2F] = S(R16G16_SINT),
    [NVN_FORMAT_RG32_UI2F] = S(R32G32_UINT),
    [NVN_FORMAT_RG32_I2F] = S(R32G32_SINT),
    [NVN_FORMAT_RGB8_UI2F] = S(R8G8B8_UINT),
    [NVN_FORMAT_RGB8_I2F] = S(R8G8B8_SINT),
    [NVN_FORMAT_RGB16_UI2F] = S(R16G16B16_UINT),
    [NVN_FORMAT_RGB16_I2F] = S(R16G16B16_SINT),
    [NVN_FORMAT_RGB32_UI2F] = S(R32G32B32_UINT),
    [NVN_FORMAT_RGB32_I2F] = S(R32G32B32_SINT),
    [NVN_FORMAT_RGBA8_UI2F] = S(R8G8B8A8_UINT),
    [NVN_FORMAT_RGBA8_I2F] = S(R8G8B8A8_SINT),
    [NVN_FORMAT_RGBA16_UI2F] = S(R16G16B16A16_UINT),
    [NVN_FORMAT_RGBA16_I2F] = S(R16G16B16A16_SINT),
    [NVN_FORMAT_RGBA32_UI2F] = S(R32G32B32A32_UINT),
    [NVN_FORMAT_RGBA32_I2F] = S(R32G32B32A32_SINT),
    [NVN_FORMAT_RGB10A2SN] = P(A2B10G10R10_SNORM_PACK32),
    [NVN_FORMAT_RGB10A2I] = P(A2B10G10R10_SINT_PACK32),
    [NVN_FORMAT_RGB10A2_UI2F] = SP(A2B10G10R10_UINT_PACK32),
    [NVN_FORMAT_RGB10A2_I2F] = SP(A2B10G10R10_SINT_PACK32),

    [NVN_FORMAT_RGBX8] = A(R8G8B8A8_UNORM),
    [NVN_FORMAT_RGBX8SN] = A(R8G8B8A8_SNORM),
    [NVN_FORMAT_RGBX8UI] = A(R8G8B8A8_UINT),
    [NVN_FORMAT_RGBX8I] = A(R8G8B8A8_SINT),
    [NVN_FORMAT_RGBX16F] = A(R16G16B16A16_SFLOAT),
    [NVN_FORMAT_RGBX16] = A(R16G16B16A16_UNORM),
    [NVN_FORMAT_RGBX16SN] = A(R16G16B16A16_SNORM),
    [NVN_FORMAT_RGBX16UI] = A(R16G16B16A16_UINT),
    [NVN_FORMAT_RGBX16I] = A(R16G16B16A16_SINT),
    [NVN_FORMAT_RGBX32F] = A(R32G32B32A32_SFLOAT),
    [NVN_FORMAT_RGBX32UI] = A(R32G32B32A32_UINT),
    [NVN_FORMAT_RGBX32I] = A(R32G32B32A32_SINT),

    [NVN_FORMAT_BGR565] = P(B5G6R5_UNORM_PACK16),
    [NVN_FORMAT_BGR5] = PA(B5G5R5A1_UNORM_PACK16),
    [NVN_FORMAT_BGR5A1] = P(B5G5R5A1_UNORM_PACK16),
    [NVN_FORMAT_A1BGR5] = P(A1R5G5B5_UNORM_PACK16),
    [NVN_FORMAT_BGRX8] = A(B8G8R8A8_UNORM),
    [NVN_FORMAT_BGRA8] = E(B8G8R8A8_UNORM),
    [NVN_FORMAT_BGRX8_SRGB] = A(B8G8R8A8_SRGB),
    [NVN_FORMAT_BGRA8_SRGB] = E(B8G8R8A8_SRGB),
};

#undef E
#undef A
#undef P
#undef PA
#undef S
#undef SP

static const VkFormat astc_ldr[14] = {
    VK_FORMAT_ASTC_4x4_UNORM_BLOCK, VK_FORMAT_ASTC_5x4_UNORM_BLOCK,
    VK_FORMAT_ASTC_5x5_UNORM_BLOCK, VK_FORMAT_ASTC_6x5_UNORM_BLOCK,
    VK_FORMAT_ASTC_6x6_UNORM_BLOCK, VK_FORMAT_ASTC_8x5_UNORM_BLOCK,
    VK_FORMAT_ASTC_8x6_UNORM_BLOCK, VK_FORMAT_ASTC_8x8_UNORM_BLOCK,
    VK_FORMAT_ASTC_10x5_UNORM_BLOCK, VK_FORMAT_ASTC_10x6_UNORM_BLOCK,
    VK_FORMAT_ASTC_10x8_UNORM_BLOCK, VK_FORMAT_ASTC_10x10_UNORM_BLOCK,
    VK_FORMAT_ASTC_12x10_UNORM_BLOCK, VK_FORMAT_ASTC_12x12_UNORM_BLOCK,
};

static const VkFormat astc_srgb[14] = {
    VK_FORMAT_ASTC_4x4_SRGB_BLOCK, VK_FORMAT_ASTC_5x4_SRGB_BLOCK,
    VK_FORMAT_ASTC_5x5_SRGB_BLOCK, VK_FORMAT_ASTC_6x5_SRGB_BLOCK,
    VK_FORMAT_ASTC_6x6_SRGB_BLOCK, VK_FORMAT_ASTC_8x5_SRGB_BLOCK,
    VK_FORMAT_ASTC_8x6_SRGB_BLOCK, VK_FORMAT_ASTC_8x8_SRGB_BLOCK,
    VK_FORMAT_ASTC_10x5_SRGB_BLOCK, VK_FORMAT_ASTC_10x6_SRGB_BLOCK,
    VK_FORMAT_ASTC_10x8_SRGB_BLOCK, VK_FORMAT_ASTC_10x10_SRGB_BLOCK,
    VK_FORMAT_ASTC_12x10_SRGB_BLOCK, VK_FORMAT_ASTC_12x12_SRGB_BLOCK,
};

int nvn_vk_format_map(uint32_t nvn_format, NvnVkFormat *out) {
    if (!out) return 0;
    *out = (NvnVkFormat){
        .format = VK_FORMAT_UNDEFINED,
        .components = {
            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
        },
    };

    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t flags = 0;
    int alpha_one = 0;
    if (nvn_format >= NVN_FORMAT_RGBA_ASTC_4x4 &&
        nvn_format <= NVN_FORMAT_RGBA_ASTC_12x12_SRGB) {
        unsigned index = (nvn_format - NVN_FORMAT_RGBA_ASTC_4x4) % 14;
        format = nvn_format < NVN_FORMAT_RGBA_ASTC_4x4_SRGB
            ? astc_ldr[index] : astc_srgb[index];
    } else if (nvn_format < sizeof formats / sizeof formats[0]) {
        const FormatEntry entry = formats[nvn_format];
        format = entry.format;
        flags = entry.flags;
        alpha_one = entry.alpha_one;
    }
    if (format == VK_FORMAT_UNDEFINED) return 0;
    if (nvn_format == NVN_FORMAT_DEPTH16) {   /* experiment: TOTK_VK_D16_AS_D32F=1 renders 16-bit depth as 32-bit float */
        static int f32 = -1;
        if (f32 < 0) { const char *e = getenv("TOTK_VK_D16_AS_D32F"); f32 = e && !strcmp(e, "1"); }
        if (f32) format = VK_FORMAT_D32_SFLOAT;
    }

    int block_width = 0, block_height = 0;
    int bytes_per_block = nvn_fmt_bpb((int)nvn_format, &block_width, &block_height);
    if (bytes_per_block <= 0 || block_width <= 0 || block_height <= 0 ||
        bytes_per_block > UINT8_MAX || block_width > UINT8_MAX || block_height > UINT8_MAX)
        return 0;

    out->format = format;
    out->flags = flags;
    out->block_width = (uint8_t)block_width;
    out->block_height = (uint8_t)block_height;
    out->bytes_per_block = (uint8_t)bytes_per_block;
    if (alpha_one) out->components.a = VK_COMPONENT_SWIZZLE_ONE;
    return 1;
}

static VkComponentSwizzle format_component(VkComponentMapping mapping, unsigned channel) {
    switch (channel) {
        case 0: return mapping.r;
        case 1: return mapping.g;
        case 2: return mapping.b;
        default: return mapping.a;
    }
}

static VkComponentSwizzle channel_swizzle(unsigned channel) {
    static const VkComponentSwizzle channels[4] = {
        VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G,
        VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A,
    };
    return channels[channel];
}

int nvn_vk_format_swizzle(const NvnVkFormat *format, const uint32_t selectors[4],
                          VkComponentMapping *out) {
    if (!format || !selectors || !out) return 0;
    VkComponentMapping composed;
    VkComponentSwizzle *channels[4] = {
        &composed.r, &composed.g, &composed.b, &composed.a,
    };
    for (unsigned i = 0; i < 4; ++i) {
        uint32_t selector = selectors[i];
        if (selector == NVN_TEXTURE_SWIZZLE_ZERO) {
            *channels[i] = VK_COMPONENT_SWIZZLE_ZERO;
        } else if (selector == NVN_TEXTURE_SWIZZLE_ONE) {
            *channels[i] = VK_COMPONENT_SWIZZLE_ONE;
        } else if (selector >= NVN_TEXTURE_SWIZZLE_R &&
                   selector <= NVN_TEXTURE_SWIZZLE_A) {
            unsigned source = selector - NVN_TEXTURE_SWIZZLE_R;
            VkComponentSwizzle mapped = format_component(format->components, source);
            *channels[i] = mapped == VK_COMPONENT_SWIZZLE_IDENTITY
                ? channel_swizzle(source) : mapped;
        } else {
            return 0;
        }
    }
    *out = composed;
    return 1;
}

int nvn_vk_format_has_features(VkPhysicalDevice device, VkFormat format,
                               VkFormatFeatureFlags required_features) {
    if (device == VK_NULL_HANDLE || format == VK_FORMAT_UNDEFINED) return 0;
    VkFormatProperties properties;
    vkGetPhysicalDeviceFormatProperties(device, format, &properties);
    return (properties.optimalTilingFeatures & required_features) == required_features;
}
