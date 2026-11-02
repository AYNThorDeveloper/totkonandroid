/* NVN texture formats mapped to Vulkan formats for the Android backend. */
#pragma once

#include <stdint.h>
#include <vulkan/vulkan.h>

enum {
    /* Integer texels must be sampled as integers and converted in shader code. */
    NVN_VK_FORMAT_SHADER_READ_CONVERSION = 1u << 0,
    /* A candidate Vulkan packed format exists, but its NVN bit layout is unverified. */
    NVN_VK_FORMAT_PACKING_UNVERIFIED = 1u << 1,
};

typedef struct {
    VkFormat format;
    VkComponentMapping components;
    uint32_t flags;
    uint8_t block_width;
    uint8_t block_height;
    uint8_t bytes_per_block;
} NvnVkFormat;

/* Returns 1 when a Vulkan candidate exists; inspect flags before using it. */
int nvn_vk_format_map(uint32_t nvn_format, NvnVkFormat *out);

/* Compose NVN texture/view selectors (ZERO, ONE, R, G, B, A) with the
 * component mapping required by the chosen Vulkan format. */
int nvn_vk_format_swizzle(const NvnVkFormat *format, const uint32_t selectors[4],
                          VkComponentMapping *out);

/* Tests optimal-tiled format features on the selected physical device. */
int nvn_vk_format_has_features(VkPhysicalDevice device, VkFormat format,
                               VkFormatFeatureFlags required_features);
