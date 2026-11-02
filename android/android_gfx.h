/* Vulkan device facts the NVN dispatcher needs when answering the game. */
#pragma once
#include <stdint.h>
#include <vulkan/vulkan.h>

typedef struct {
    int min_max_filtering, astc, bc, stencil8, geometry_shader, tessellation, multi_viewport;
    float max_anisotropy;
} AndroidGfxCaps;

int android_gfx_device_ready(void);
const AndroidGfxCaps *android_gfx_caps(void); /* NULL before InitializeGraphics */

typedef struct {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    VkSurfaceKHR surface;
} AndroidGfxVulkan;
const AndroidGfxVulkan *android_gfx_vulkan(void); /* NULL before InitializeGraphics */
