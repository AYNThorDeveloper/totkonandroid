/* Android Vulkan texture and controller proof. No TOTK graphics are rendered. */
#include <android/input.h>
#include <android/log.h>
#include <android/asset_manager.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
#include <jni.h>
#include "../graphics_settings.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>
#include <sys/types.h>
#include "../thor_gamepad.h"
#include "../nvn_vk_format.h"
#include "../nvn_vk_sampler.h"
#include "../nvn_vk_texture_stage.h"
#include "../../games/totk/runtime/runtime.h"
#include "../../games/totk/third_party/d2r_gfx/gfx/nvn/include/nvn.h"
#include "../../games/totk/third_party/d2r_gfx/gfx/tex/nvn_deswizzle.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "TOTKVulkanProof", __VA_ARGS__)
#define FAIL(...) do { __android_log_print(ANDROID_LOG_ERROR, "TOTKVulkanProof", __VA_ARGS__); return 0; } while (0)

static pthread_mutex_t settings_lock = PTHREAD_MUTEX_INITIALIZER;
static TotkGraphicsSettings settings = {1280,720,1280,720};
static char settings_path[4096];
static unsigned settings_revision = 1, handled_revision;
static unsigned actual_width, actual_height;
static unsigned last_good_width = 1280, last_good_height = 720;
static int resize_failed, menu_open;
static ALooper *render_looper;

JNIEXPORT void JNICALL Java_org_macnativegames_totk_vulkanprobe_ThorActivity_nativeInit(
        JNIEnv *env, jclass cls, jstring path) {
    (void)cls;
    const char *text = (*env)->GetStringUTFChars(env, path, NULL);
    if (!text) return;
    pthread_mutex_lock(&settings_lock);
    if (strlen(text) < sizeof settings_path) {
        strcpy(settings_path, text);
        if (totk_graphics_load(settings_path, &settings) < 0) LOG("Invalid graphics config; using defaults");
        ++settings_revision;
    }
    if (render_looper) ALooper_wake(render_looper);
    pthread_mutex_unlock(&settings_lock);
    (*env)->ReleaseStringUTFChars(env, path, text);
}
JNIEXPORT jstring JNICALL Java_org_macnativegames_totk_vulkanprobe_ThorActivity_nativeRead(
        JNIEnv *env, jclass cls) {
    (void)cls; char text[256];
    pthread_mutex_lock(&settings_lock);
    snprintf(text, sizeof text, "{\"render\":[%u,%u],\"output\":[%u,%u],\"actual\":[%u,%u],\"failed\":%s}",
        settings.render_width, settings.render_height, settings.output_width, settings.output_height,
        actual_width, actual_height, resize_failed ? "true" : "false");
    pthread_mutex_unlock(&settings_lock);
    return (*env)->NewStringUTF(env, text);
}
JNIEXPORT jboolean JNICALL Java_org_macnativegames_totk_vulkanprobe_ThorActivity_nativeSave(
        JNIEnv *env, jclass cls, jint rw, jint rh, jint ow, jint oh) {
    (void)env; (void)cls;
    TotkGraphicsSettings requested = {(unsigned)rw,(unsigned)rh,(unsigned)ow,(unsigned)oh};
    pthread_mutex_lock(&settings_lock);
    int ok = totk_graphics_valid(&requested) && totk_graphics_save(settings_path, &requested);
    if (ok) { settings = requested; ++settings_revision; resize_failed = 0; }
    if (ok && render_looper) ALooper_wake(render_looper);
    pthread_mutex_unlock(&settings_lock);
    return ok ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT void JNICALL Java_org_macnativegames_totk_vulkanprobe_ThorActivity_nativeMenu(
        JNIEnv *env, jclass cls, jboolean open) {
    (void)env; (void)cls;
    pthread_mutex_lock(&settings_lock); menu_open = open; pthread_mutex_unlock(&settings_lock);
    runtime_gamepad_clear();
}

typedef struct {
    struct android_app *app;
    VkInstance instance;
    VkSurfaceKHR surface;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    VkSwapchainKHR swapchain;
    VkImage *images;
    uint32_t image_count;
    VkExtent2D extent;
    VkFormat swap_format;
    VkImageView *swap_views;
    VkFramebuffer *framebuffers;
    VkRenderPass render_pass;
    VkPipelineLayout pipeline_layout;
    VkPipeline graphics_pipeline;
    VkDescriptorSetLayout descriptor_layout;
    VkDescriptorPool descriptor_pool;
    VkDescriptorSet descriptor_set;
    VkImage texture_image;
    VkDeviceMemory texture_memory;
    VkImageView texture_view;
    VkSampler texture_sampler;
    VkCommandPool command_pool;
    VkCommandBuffer command;
    VkSemaphore acquired, rendered;
    VkFence fence;
    unsigned frames;
    int ready;
} Renderer;

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static void destroy(Renderer *r) {
    if (r->device) vkDeviceWaitIdle(r->device);
    if (r->graphics_pipeline) vkDestroyPipeline(r->device, r->graphics_pipeline, NULL);
    if (r->pipeline_layout) vkDestroyPipelineLayout(r->device, r->pipeline_layout, NULL);
    if (r->descriptor_pool) vkDestroyDescriptorPool(r->device, r->descriptor_pool, NULL);
    if (r->descriptor_layout) vkDestroyDescriptorSetLayout(r->device, r->descriptor_layout, NULL);
    if (r->texture_sampler) vkDestroySampler(r->device, r->texture_sampler, NULL);
    if (r->texture_view) vkDestroyImageView(r->device, r->texture_view, NULL);
    if (r->texture_image) vkDestroyImage(r->device, r->texture_image, NULL);
    if (r->texture_memory) vkFreeMemory(r->device, r->texture_memory, NULL);
    if (r->framebuffers)
        for (uint32_t i = 0; i < r->image_count; ++i)
            if (r->framebuffers[i]) vkDestroyFramebuffer(r->device, r->framebuffers[i], NULL);
    if (r->render_pass) vkDestroyRenderPass(r->device, r->render_pass, NULL);
    if (r->swap_views)
        for (uint32_t i = 0; i < r->image_count; ++i)
            if (r->swap_views[i]) vkDestroyImageView(r->device, r->swap_views[i], NULL);
    if (r->fence) vkDestroyFence(r->device, r->fence, NULL);
    if (r->rendered) vkDestroySemaphore(r->device, r->rendered, NULL);
    if (r->acquired) vkDestroySemaphore(r->device, r->acquired, NULL);
    if (r->command_pool) vkDestroyCommandPool(r->device, r->command_pool, NULL);
    free(r->framebuffers);
    free(r->swap_views);
    free(r->images);
    if (r->swapchain) vkDestroySwapchainKHR(r->device, r->swapchain, NULL);
    if (r->device) vkDestroyDevice(r->device, NULL);
    if (r->surface) vkDestroySurfaceKHR(r->instance, r->surface, NULL);
    if (r->instance) vkDestroyInstance(r->instance, NULL);
    struct android_app *app = r->app;
    memset(r, 0, sizeof *r);
    r->app = app;
}

static void log_nvn_format_support(VkPhysicalDevice physical) {
    uint32_t mapped_count = 0, sample_count = 0, color_count = 0;
    uint32_t depth_count = 0, storage_count = 0;
    for (uint32_t nvn = 0; nvn < 0x9d; ++nvn) {
        NvnVkFormat mapped;
        if (!nvn_vk_format_map(nvn, &mapped)) continue;
        VkFormatProperties properties;
        vkGetPhysicalDeviceFormatProperties(physical, mapped.format, &properties);
        VkFormatFeatureFlags features = properties.optimalTilingFeatures;
        int sampled = (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
        int color = (features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0;
        int depth = (features & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
        int storage = (features & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
        int transfer_src = (features & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) != 0;
        int transfer_dst = (features & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) != 0;
        ++mapped_count;
        sample_count += sampled;
        color_count += color;
        depth_count += depth;
        storage_count += storage;
        LOG("NVN 0x%02x -> VkFormat %d: sample=%d color=%d depth=%d storage=%d transfer=%d/%d linear=%d map=0x%x block=%ux%u/%uB",
            nvn, (int)mapped.format, sampled, color, depth, storage,
            transfer_src, transfer_dst,
            (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0,
            mapped.flags, (unsigned)mapped.block_width, (unsigned)mapped.block_height,
            (unsigned)mapped.bytes_per_block);
    }
    LOG("NVN format capability totals: mapped=%u sampled=%u color=%u depth=%u storage=%u",
        mapped_count, sample_count, color_count, depth_count, storage_count);
}

static int find_memory_type(Renderer *r, uint32_t type_bits,
                            VkMemoryPropertyFlags required,
                            VkMemoryPropertyFlags preferred,
                            uint32_t *index, VkMemoryPropertyFlags *selected_flags) {
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(r->physical, &properties);
    int fallback = -1;
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if (!(type_bits & (1u << i))) continue;
        VkMemoryPropertyFlags flags = properties.memoryTypes[i].propertyFlags;
        if ((flags & required) != required) continue;
        if (fallback < 0) fallback = (int)i;
        if ((flags & preferred) == preferred) {
            *index = i;
            *selected_flags = flags;
            return 1;
        }
    }
    if (fallback < 0) return 0;
    *index = (uint32_t)fallback;
    *selected_flags = properties.memoryTypes[fallback].propertyFlags;
    return 1;
}

static int create_test_texture(Renderer *r) {
    nvn_texdesc desc = {
        .target = NVN_TEXTURE_TARGET_2D,
        .format = NVN_FORMAT_RGBA8,
        .width = 4, .height = 4, .depth = 1, .levels = 1,
        .swizzle_r = NVN_TEXTURE_SWIZZLE_R, .swizzle_g = NVN_TEXTURE_SWIZZLE_G,
        .swizzle_b = NVN_TEXTURE_SWIZZLE_B, .swizzle_a = NVN_TEXTURE_SWIZZLE_A,
    };
    nvn_texlayout layout;
    nvn_tex_layout(&desc, &layout);
    if (!layout.size || layout.size > SIZE_MAX) FAIL("test texture layout is invalid");
    uint8_t *source = calloc(1, (size_t)layout.size);
    if (!source) FAIL("test texture source allocation failed");
    uint8_t pixels[4 * 4 * 4];
    for (unsigned y = 0; y < 4; ++y) {
        for (unsigned x = 0; x < 4; ++x) {
            uint8_t *pixel = &pixels[(y * 4 + x) * 4];
            int right = x >= 2, bottom = y >= 2;
            pixel[0] = right ? 36 : 220;
            pixel[1] = bottom ? 208 : 52;
            pixel[2] = right == bottom ? 44 : 224;
            pixel[3] = 255;
        }
    }
    if (nvn_swizzle(&desc, 0, 0, pixels, 4 * 4, source) != 0) {
        free(source);
        FAIL("could not create swizzled test texture");
    }
    NvnVkTextureStage stage = {0};
    NvnVkFormat source_format;
    if (!nvn_vk_format_map((uint32_t)desc.format, &source_format)) {
        free(source);
        FAIL("test texture format mapping failed");
    }
    int source_format_supported = nvn_vk_format_has_features(r->physical,
        source_format.format,
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
    NvnVkTextureStageResult staged = nvn_vk_texture_stage_with_bc_fallback(
        &desc, source, (size_t)layout.size, source_format_supported, &stage);
    free(source);
    if (staged != NVN_VK_STAGE_OK) FAIL("test texture staging failed: %d", staged);
    VkFormat texture_format = stage.format;
    VkComponentMapping texture_components = stage.components;
    VkImageViewType texture_view_type = stage.view_type;
    uint32_t texture_mip_levels = stage.mip_levels;
    uint32_t texture_array_layers = stage.array_layers;
    if (!nvn_vk_format_has_features(r->physical, stage.format,
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) {
        nvn_vk_texture_stage_destroy(&stage);
        FAIL("test texture format lacks sampled/transfer-destination support");
    }

    VkResult vr;
    VkImageCreateInfo image_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = stage.image_flags, .imageType = stage.image_type, .format = stage.format,
        .extent = stage.extent, .mipLevels = stage.mip_levels,
        .arrayLayers = stage.array_layers, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    vr = vkCreateImage(r->device, &image_info, NULL, &r->texture_image);
    if (vr != VK_SUCCESS) {
        nvn_vk_texture_stage_destroy(&stage);
        FAIL("test texture image creation failed: %d", vr);
    }
    VkMemoryRequirements image_requirements;
    vkGetImageMemoryRequirements(r->device, r->texture_image, &image_requirements);
    uint32_t image_memory_type;
    VkMemoryPropertyFlags image_memory_flags;
    if (!find_memory_type(r, image_requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &image_memory_type, &image_memory_flags)) {
        nvn_vk_texture_stage_destroy(&stage);
        FAIL("no device-local memory type for test image");
    }
    VkMemoryAllocateInfo image_memory_info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = image_requirements.size, .memoryTypeIndex = image_memory_type};
    vr = vkAllocateMemory(r->device, &image_memory_info, NULL, &r->texture_memory);
    if (vr != VK_SUCCESS || vkBindImageMemory(r->device, r->texture_image,
            r->texture_memory, 0) != VK_SUCCESS) {
        nvn_vk_texture_stage_destroy(&stage);
        FAIL("test texture image memory setup failed: %d", vr);
    }
    (void)image_memory_flags;

    VkBuffer staging_buffer = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    void *mapped = NULL;
    int ok = 0;
    VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = stage.data_size, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    vr = vkCreateBuffer(r->device, &buffer_info, NULL, &staging_buffer);
    if (vr != VK_SUCCESS) goto upload_cleanup;
    VkMemoryRequirements buffer_requirements;
    vkGetBufferMemoryRequirements(r->device, staging_buffer, &buffer_requirements);
    uint32_t buffer_memory_type;
    VkMemoryPropertyFlags buffer_memory_flags;
    if (!find_memory_type(r, buffer_requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            &buffer_memory_type, &buffer_memory_flags)) {
        vr = VK_ERROR_MEMORY_MAP_FAILED;
        goto upload_cleanup;
    }
    VkMemoryAllocateInfo buffer_memory_info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = buffer_requirements.size, .memoryTypeIndex = buffer_memory_type};
    vr = vkAllocateMemory(r->device, &buffer_memory_info, NULL, &staging_memory);
    if (vr != VK_SUCCESS) goto upload_cleanup;
    vr = vkBindBufferMemory(r->device, staging_buffer, staging_memory, 0);
    if (vr != VK_SUCCESS) goto upload_cleanup;
    vr = vkMapMemory(r->device, staging_memory, 0, VK_WHOLE_SIZE, 0, &mapped);
    if (vr != VK_SUCCESS) goto upload_cleanup;
    memcpy(mapped, stage.data, stage.data_size);
    if (!(buffer_memory_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        VkMappedMemoryRange flush = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = staging_memory, .offset = 0, .size = VK_WHOLE_SIZE};
        vr = vkFlushMappedMemoryRanges(r->device, 1, &flush);
        if (vr != VK_SUCCESS) goto upload_cleanup;
    }
    vkUnmapMemory(r->device, staging_memory);
    mapped = NULL;
    vr = vkResetCommandBuffer(r->command, 0);
    if (vr != VK_SUCCESS) goto upload_cleanup;
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vr = vkBeginCommandBuffer(r->command, &begin);
    if (vr != VK_SUCCESS) goto upload_cleanup;
    VkImageMemoryBarrier to_transfer = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = r->texture_image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, stage.mip_levels,
                             0, stage.array_layers}};
    vkCmdPipelineBarrier(r->command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_transfer);
    vkCmdCopyBufferToImage(r->command, staging_buffer, r->texture_image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, stage.region_count, stage.regions);
    VkImageMemoryBarrier to_sample = to_transfer;
    to_sample.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_sample.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    to_sample.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_sample.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(r->command, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &to_sample);
    vr = vkEndCommandBuffer(r->command);
    if (vr != VK_SUCCESS) goto upload_cleanup;
    VkSubmitInfo upload = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &r->command};
    vr = vkQueueSubmit(r->queue, 1, &upload, VK_NULL_HANDLE);
    if (vr != VK_SUCCESS) goto upload_cleanup;
    vr = vkQueueWaitIdle(r->queue);
    if (vr != VK_SUCCESS) goto upload_cleanup;
    ok = 1;

upload_cleanup:
    if (mapped) vkUnmapMemory(r->device, staging_memory);
    if (staging_buffer) vkDestroyBuffer(r->device, staging_buffer, NULL);
    if (staging_memory) vkFreeMemory(r->device, staging_memory, NULL);
    nvn_vk_texture_stage_destroy(&stage);
    if (!ok) FAIL("test texture buffer upload failed: %d", vr);

    VkImageViewCreateInfo view_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = r->texture_image, .viewType = texture_view_type, .format = texture_format,
        .components = texture_components,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, texture_mip_levels,
                             0, texture_array_layers}};
    vr = vkCreateImageView(r->device, &view_info, NULL, &r->texture_view);
    if (vr != VK_SUCCESS) FAIL("test texture image view creation failed: %d", vr);
    samp_par test_sampler = {.minf = NVN_MIN_FILTER_NEAREST,
        .magf = NVN_MAG_FILTER_NEAREST,
        .ws = NVN_WRAP_MODE_CLAMP_TO_EDGE, .wt = NVN_WRAP_MODE_CLAMP_TO_EDGE,
        .wr = NVN_WRAP_MODE_CLAMP_TO_EDGE, .reduction = NVN_SAMPLER_REDUCTION_AVERAGE,
        .lod_min = 0.0f, .lod_max = 0.0f, .aniso = 1.0f};
    VkSamplerCreateInfo sampler_info;
    VkSamplerReductionModeCreateInfo reduction_info;
    uint32_t sampler_requirements;
    NvnVkSamplerResult sampler_result = nvn_vk_sampler_map(
        &test_sampler, &sampler_info, &reduction_info, &sampler_requirements);
    if (sampler_result != NVN_VK_SAMPLER_OK || sampler_requirements)
        FAIL("synthetic NVN sampler mapping failed: %d, requirements=%#x",
             sampler_result, sampler_requirements);
    vr = vkCreateSampler(r->device, &sampler_info, NULL, &r->texture_sampler);
    if (vr != VK_SUCCESS) FAIL("test texture sampler creation failed: %d", vr);

    VkDescriptorSetLayoutBinding binding = {.binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo descriptor_layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &binding};
    vr = vkCreateDescriptorSetLayout(r->device, &descriptor_layout_info, NULL,
                                      &r->descriptor_layout);
    if (vr != VK_SUCCESS) FAIL("texture descriptor layout creation failed: %d", vr);
    VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &pool_size};
    vr = vkCreateDescriptorPool(r->device, &pool_info, NULL, &r->descriptor_pool);
    if (vr != VK_SUCCESS) FAIL("texture descriptor pool creation failed: %d", vr);
    VkDescriptorSetAllocateInfo set_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = r->descriptor_pool, .descriptorSetCount = 1,
        .pSetLayouts = &r->descriptor_layout};
    vr = vkAllocateDescriptorSets(r->device, &set_info, &r->descriptor_set);
    if (vr != VK_SUCCESS) FAIL("texture descriptor set allocation failed: %d", vr);
    VkDescriptorImageInfo descriptor_image = {.sampler = r->texture_sampler,
        .imageView = r->texture_view, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = r->descriptor_set, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &descriptor_image};
    vkUpdateDescriptorSets(r->device, 1, &write, 0, NULL);
    LOG("uploaded synthetic NVN RGBA8 texture through the Vulkan staging path");
    return 1;
}

static uint32_t *read_spirv_asset(Renderer *r, const char *path, size_t *word_count) {
    AAsset *asset = AAssetManager_open(r->app->activity->assetManager, path, AASSET_MODE_BUFFER);
    if (!asset) { LOG("shader asset missing: %s", path); return NULL; }
    off_t length = AAsset_getLength(asset);
    if (length <= 0 || length > 1024 * 1024 || (length & 3)) {
        AAsset_close(asset); LOG("invalid SPIR-V asset length: %s", path); return NULL;
    }
    uint32_t *words = malloc((size_t)length);
    if (!words) { AAsset_close(asset); return NULL; }
    size_t read_bytes = 0;
    while (read_bytes < (size_t)length) {
        int amount = AAsset_read(asset, (uint8_t *)words + read_bytes,
                                 (size_t)length - read_bytes);
        if (amount <= 0) { free(words); AAsset_close(asset); return NULL; }
        read_bytes += (size_t)amount;
    }
    AAsset_close(asset);
    *word_count = (size_t)length / sizeof(uint32_t);
    return words;
}

static VkShaderModule create_shader_module(Renderer *r, const char *asset_path) {
    size_t word_count = 0;
    uint32_t *words = read_spirv_asset(r, asset_path, &word_count);
    if (!words) return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = word_count * sizeof(uint32_t), .pCode = words};
    VkShaderModule module = VK_NULL_HANDLE;
    VkResult result = vkCreateShaderModule(r->device, &info, NULL, &module);
    free(words);
    if (result != VK_SUCCESS) LOG("shader module creation failed (%s): %d", asset_path, result);
    return result == VK_SUCCESS ? module : VK_NULL_HANDLE;
}

static int create_render_pipeline(Renderer *r) {
    VkAttachmentDescription attachment = {.format = r->swap_format,
        .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
    VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &color};
    VkSubpassDependency dependency = {.srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
    VkRenderPassCreateInfo render_pass_info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &attachment,
        .subpassCount = 1, .pSubpasses = &subpass,
        .dependencyCount = 1, .pDependencies = &dependency};
    VkResult vr = vkCreateRenderPass(r->device, &render_pass_info, NULL, &r->render_pass);
    if (vr != VK_SUCCESS) FAIL("render pass creation failed: %d", vr);

    r->swap_views = calloc(r->image_count, sizeof *r->swap_views);
    r->framebuffers = calloc(r->image_count, sizeof *r->framebuffers);
    if (!r->swap_views || !r->framebuffers) FAIL("swapchain view allocation failed");
    for (uint32_t i = 0; i < r->image_count; ++i) {
        VkImageViewCreateInfo view_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = r->images[i], .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = r->swap_format,
            .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                           VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vr = vkCreateImageView(r->device, &view_info, NULL, &r->swap_views[i]);
        if (vr != VK_SUCCESS) FAIL("swapchain image view creation failed: %d", vr);
        VkFramebufferCreateInfo framebuffer_info = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = r->render_pass, .attachmentCount = 1,
            .pAttachments = &r->swap_views[i], .width = r->extent.width,
            .height = r->extent.height, .layers = 1};
        vr = vkCreateFramebuffer(r->device, &framebuffer_info, NULL, &r->framebuffers[i]);
        if (vr != VK_SUCCESS) FAIL("swapchain framebuffer creation failed: %d", vr);
    }

    VkShaderModule vertex = create_shader_module(r, "shaders/textured.vert.spv");
    VkShaderModule fragment = create_shader_module(r, "shaders/textured.frag.spv");
    if (!vertex || !fragment) {
        if (vertex) vkDestroyShaderModule(r->device, vertex, NULL);
        if (fragment) vkDestroyShaderModule(r->device, fragment, NULL);
        FAIL("textured Vulkan shaders could not be loaded");
    }
    VkPipelineLayoutCreateInfo layout_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &r->descriptor_layout};
    vr = vkCreatePipelineLayout(r->device, &layout_info, NULL, &r->pipeline_layout);
    if (vr != VK_SUCCESS) {
        vkDestroyShaderModule(r->device, vertex, NULL);
        vkDestroyShaderModule(r->device, fragment, NULL);
        FAIL("graphics pipeline layout creation failed: %d", vr);
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vertex, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fragment, .pName = "main"},
    };
    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport viewport = {0, 0, (float)r->extent.width, (float)r->extent.height, 0, 1};
    VkRect2D scissor = {{0, 0}, r->extent};
    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1, .pScissors = &scissor};
    VkPipelineRasterizationStateCreateInfo rasterization = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState blend_attachment = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
    VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &blend_attachment};
    VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly, .pViewportState = &viewport_state,
        .pRasterizationState = &rasterization, .pMultisampleState = &multisample,
        .pColorBlendState = &blend, .layout = r->pipeline_layout,
        .renderPass = r->render_pass, .subpass = 0, .basePipelineIndex = -1};
    vr = vkCreateGraphicsPipelines(r->device, VK_NULL_HANDLE, 1, &pipeline_info,
                                    NULL, &r->graphics_pipeline);
    vkDestroyShaderModule(r->device, vertex, NULL);
    vkDestroyShaderModule(r->device, fragment, NULL);
    if (vr != VK_SUCCESS) FAIL("textured graphics pipeline creation failed: %d", vr);
    return 1;
}

static int create(Renderer *r, unsigned width, unsigned height) {
    ANativeWindow *window = r->app->window;
    if (!window) FAIL("no Android window");
    int geometry = ANativeWindow_setBuffersGeometry(window, (int)width, (int)height, WINDOW_FORMAT_RGBA_8888);
    LOG("requested %ux%u buffers: result=%d, window=%dx%d", width, height, geometry,
        ANativeWindow_getWidth(window), ANativeWindow_getHeight(window));
    if (geometry != 0) FAIL("window geometry change failed");
    const char *instance_extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME,
                                          VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    VkApplicationInfo app_info = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                  .pApplicationName = "TOTK Vulkan Proof",
                                  .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app_info, .enabledExtensionCount = 2,
        .ppEnabledExtensionNames = instance_extensions};
    if (vkCreateInstance(&instance_info, NULL, &r->instance) != VK_SUCCESS)
        FAIL("vkCreateInstance failed");
    VkAndroidSurfaceCreateInfoKHR surface_info = {.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
                                                   .window = window};
    if (vkCreateAndroidSurfaceKHR(r->instance, &surface_info, NULL, &r->surface) != VK_SUCCESS)
        FAIL("vkCreateAndroidSurfaceKHR failed");
    uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(r->instance, &count, NULL) != VK_SUCCESS || !count)
        FAIL("no Vulkan physical device");
    VkPhysicalDevice devices[8];
    if (count > 8) count = 8;
    if (vkEnumeratePhysicalDevices(r->instance, &count, devices) != VK_SUCCESS)
        FAIL("physical-device enumeration failed");
    for (uint32_t i = 0; i < count && !r->physical; ++i) {
        uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &families, NULL);
        if (families > 32) families = 32;
        VkQueueFamilyProperties properties[32];
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &families, properties);
        for (uint32_t j = 0; j < families; ++j) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(devices[i], j, r->surface, &present);
            if ((properties[j].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                r->physical = devices[i]; r->queue_family = j; break;
            }
        }
    }
    if (!r->physical) FAIL("no graphics/present queue");
    VkPhysicalDeviceProperties gpu;
    vkGetPhysicalDeviceProperties(r->physical, &gpu);
    LOG("GPU %s, Vulkan %u.%u", gpu.deviceName,
        VK_API_VERSION_MAJOR(gpu.apiVersion), VK_API_VERSION_MINOR(gpu.apiVersion));
    log_nvn_format_support(r->physical);
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = r->queue_family, .queueCount = 1, .pQueuePriorities = &priority};
    const char *device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = device_extensions};
    if (vkCreateDevice(r->physical, &device_info, NULL, &r->device) != VK_SUCCESS)
        FAIL("vkCreateDevice failed");
    vkGetDeviceQueue(r->device, r->queue_family, 0, &r->queue);
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(r->physical, r->surface, &caps) != VK_SUCCESS)
        FAIL("surface capabilities failed");
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
        FAIL("swapchain images cannot be color attachments");
    uint32_t format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(r->physical, r->surface, &format_count, NULL);
    if (!format_count) FAIL("no surface formats");
    VkSurfaceFormatKHR formats[64];
    if (format_count > 64) format_count = 64;
    vkGetPhysicalDeviceSurfaceFormatsKHR(r->physical, r->surface, &format_count, formats);
    VkSurfaceFormatKHR format = formats[0];
    if (format_count == 1 && format.format == VK_FORMAT_UNDEFINED)
        format.format = VK_FORMAT_R8G8B8A8_UNORM;
    for (uint32_t i = 0; i < format_count; ++i)
        if (formats[i].format == VK_FORMAT_R8G8B8A8_UNORM ||
            formats[i].format == VK_FORMAT_B8G8R8A8_UNORM) { format = formats[i]; break; }
    r->swap_format = format.format;
    r->extent = caps.currentExtent;
    if (r->extent.width == UINT32_MAX) {
        r->extent.width = width; r->extent.height = height;
    }
    if (r->extent.width < caps.minImageExtent.width) r->extent.width = caps.minImageExtent.width;
    if (r->extent.height < caps.minImageExtent.height) r->extent.height = caps.minImageExtent.height;
    if (r->extent.width > caps.maxImageExtent.width) r->extent.width = caps.maxImageExtent.width;
    if (r->extent.height > caps.maxImageExtent.height) r->extent.height = caps.maxImageExtent.height;
    uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount && image_count > caps.maxImageCount) image_count = caps.maxImageCount;
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha)) {
        for (uint32_t bit = 1; bit <= VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR; bit <<= 1)
            if (caps.supportedCompositeAlpha & bit) { alpha = (VkCompositeAlphaFlagBitsKHR)bit; break; }
    }
    VkSwapchainCreateInfoKHR swap_info = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = r->surface, .minImageCount = image_count, .imageFormat = format.format,
        .imageColorSpace = format.colorSpace, .imageExtent = r->extent, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform, .compositeAlpha = alpha,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE};
    if (vkCreateSwapchainKHR(r->device, &swap_info, NULL, &r->swapchain) != VK_SUCCESS)
        FAIL("vkCreateSwapchainKHR failed: %ux%u", r->extent.width, r->extent.height);
    vkGetSwapchainImagesKHR(r->device, r->swapchain, &r->image_count, NULL);
    r->images = calloc(r->image_count, sizeof *r->images);
    if (!r->images || vkGetSwapchainImagesKHR(r->device, r->swapchain,
                                              &r->image_count, r->images) != VK_SUCCESS)
        FAIL("swapchain images failed");
    VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = r->queue_family};
    if (vkCreateCommandPool(r->device, &pool_info, NULL, &r->command_pool) != VK_SUCCESS)
        FAIL("command pool failed");
    VkCommandBufferAllocateInfo command_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = r->command_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    if (vkAllocateCommandBuffers(r->device, &command_info, &r->command) != VK_SUCCESS)
        FAIL("command buffer failed");
    VkSemaphoreCreateInfo semaphore_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (vkCreateSemaphore(r->device, &semaphore_info, NULL, &r->acquired) != VK_SUCCESS ||
        vkCreateSemaphore(r->device, &semaphore_info, NULL, &r->rendered) != VK_SUCCESS)
        FAIL("semaphore failed");
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                                    .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    if (vkCreateFence(r->device, &fence_info, NULL, &r->fence) != VK_SUCCESS)
        FAIL("fence failed");
    if (!create_test_texture(r) || !create_render_pipeline(r)) return 0;
    r->ready = 1;
    LOG("textured Vulkan swapchain ready: %ux%u, %u images, format %d",
        r->extent.width, r->extent.height, r->image_count, format.format);
    return 1;
}

static int frame(Renderer *r) {
    if (vkWaitForFences(r->device, 1, &r->fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
        FAIL("wait for frame fence failed");
    uint32_t index;
    VkResult result = vkAcquireNextImageKHR(r->device, r->swapchain, UINT64_MAX,
                                             r->acquired, VK_NULL_HANDLE, &index);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) return 0;
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        FAIL("acquire image failed: %d", result);
    vkResetFences(r->device, 1, &r->fence);
    vkResetCommandBuffer(r->command, 0);
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    if (vkBeginCommandBuffer(r->command, &begin) != VK_SUCCESS)
        FAIL("begin command buffer failed");
    VkClearValue clear = {.color = {.float32 = {0.035f, 0.055f, 0.075f, 1.0f}}};
    VkRenderPassBeginInfo render_begin = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = r->render_pass, .framebuffer = r->framebuffers[index],
        .renderArea = {{0, 0}, r->extent}, .clearValueCount = 1, .pClearValues = &clear};
    vkCmdBeginRenderPass(r->command, &render_begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(r->command, VK_PIPELINE_BIND_POINT_GRAPHICS, r->graphics_pipeline);
    vkCmdBindDescriptorSets(r->command, VK_PIPELINE_BIND_POINT_GRAPHICS,
        r->pipeline_layout, 0, 1, &r->descriptor_set, 0, NULL);
    vkCmdDraw(r->command, 3, 1, 0, 0);
    vkCmdEndRenderPass(r->command);
    if (vkEndCommandBuffer(r->command) != VK_SUCCESS) FAIL("end command buffer failed");
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &r->acquired, .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1, .pCommandBuffers = &r->command,
        .signalSemaphoreCount = 1, .pSignalSemaphores = &r->rendered};
    if (vkQueueSubmit(r->queue, 1, &submit, r->fence) != VK_SUCCESS)
        FAIL("queue submit failed");
    VkPresentInfoKHR present = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &r->rendered,
        .swapchainCount = 1, .pSwapchains = &r->swapchain, .pImageIndices = &index};
    result = vkQueuePresentKHR(r->queue, &present);
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        FAIL("present failed: %d", result);
    r->frames++;
    if (r->frames == 1 || r->frames % 300 == 0) LOG("presented frame %u", r->frames);
    return 1;
}

/* Recreate only on the render thread, after all old work has completed.
 * Keep an operational previous extent when a requested size is rejected. */
static void reconfigure(Renderer *r) {
    pthread_mutex_lock(&settings_lock);
    TotkGraphicsSettings desired = settings;
    unsigned revision = settings_revision;
    pthread_mutex_unlock(&settings_lock);
    destroy(r);
    int ok = create(r, desired.output_width, desired.output_height);
    if (ok) { last_good_width = r->extent.width; last_good_height = r->extent.height; }
    else {
        destroy(r);
        if (!create(r, last_good_width, last_good_height)) destroy(r);
    }
    pthread_mutex_lock(&settings_lock);
    handled_revision = revision;
    resize_failed = !ok;
    actual_width = r->ready ? r->extent.width : 0;
    actual_height = r->ready ? r->extent.height : 0;
    pthread_mutex_unlock(&settings_lock);
}

static int32_t input(struct android_app *app, AInputEvent *event) {
    (void)app;
    pthread_mutex_lock(&settings_lock); int blocked = menu_open; pthread_mutex_unlock(&settings_lock);
    // Return unhandled so Android dispatches overlay touch/keys to the WebView.
    // Never feed those events into the guest controller state.
    if (blocked) return 0;
    if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY &&
        AKeyEvent_getKeyCode(event) == AKEYCODE_BUTTON_START) {
        if (AKeyEvent_getAction(event) == AKEY_EVENT_ACTION_DOWN && !AKeyEvent_getRepeatCount(event)) {
            // NativeActivity's InputQueue can receive Start before Java dispatch.
            JNIEnv *env = NULL; JavaVM *vm = app->activity->vm;
            int attached = (*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) == JNI_OK;
            if (attached || (*vm)->AttachCurrentThread(vm, &env, NULL) == JNI_OK) {
                jclass cls = (*env)->GetObjectClass(env, app->activity->clazz);
                jmethodID method = (*env)->GetMethodID(env, cls, "requestMenuFromNative", "()V");
                if (method) (*env)->CallVoidMethod(env, app->activity->clazz, method);
                if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); }
                (*env)->DeleteLocalRef(env, cls);
                if (!attached) (*vm)->DetachCurrentThread(vm);
            }
        }
        return 1;
    }
    int type = AInputEvent_getType(event);
    int source = AInputEvent_getSource(event);
    int handled = android_gamepad_event(event);
    if (type == AINPUT_EVENT_TYPE_KEY)
        LOG("key=%d action=%d source=%#x device=%d", AKeyEvent_getKeyCode(event),
            AKeyEvent_getAction(event), source, AInputEvent_getDeviceId(event));
    if (type == AINPUT_EVENT_TYPE_KEY && (source & (AINPUT_SOURCE_GAMEPAD | AINPUT_SOURCE_JOYSTICK))) {
        RuntimeGamepadState pad;
        runtime_gamepad_read(&pad);
        LOG("Switch buttons=%#llx sticks=(%d,%d,%d,%d)",
            (unsigned long long)pad.buttons, pad.lx, pad.ly, pad.rx, pad.ry);
        return 1;
    }
    if (type == AINPUT_EVENT_TYPE_MOTION && (source & AINPUT_SOURCE_JOYSTICK)) {
        const int axes[] = {AMOTION_EVENT_AXIS_X, AMOTION_EVENT_AXIS_Y,
                            AMOTION_EVENT_AXIS_Z, AMOTION_EVENT_AXIS_RZ,
                            AMOTION_EVENT_AXIS_GAS, AMOTION_EVENT_AXIS_BRAKE,
                            AMOTION_EVENT_AXIS_HAT_X, AMOTION_EVENT_AXIS_HAT_Y};
        float values[8];
        static float previous[8];
        static int first = 1;
        int changed = first;
        for (unsigned i = 0; i < 8; ++i) {
            values[i] = AMotionEvent_getAxisValue(event, axes[i], 0);
            float difference = values[i] - previous[i];
            if (difference > 0.15f || difference < -0.15f) changed = 1;
        }
        if (changed) {
            LOG("axes device=%d L(%.2f,%.2f) R(%.2f,%.2f) triggers(%.2f,%.2f) hat(%.2f,%.2f)",
                AInputEvent_getDeviceId(event), values[0], values[1], values[2], values[3],
                values[4], values[5], values[6], values[7]);
            memcpy(previous, values, sizeof previous);
            first = 0;
        }
        return 1;
    }
    return handled;
}

static void command(struct android_app *app, int32_t cmd) {
    Renderer *r = app->userData;
    if (cmd == APP_CMD_INIT_WINDOW && app->window) {
        reconfigure(r);
    } else if (cmd == APP_CMD_TERM_WINDOW) {
        destroy(r);
        runtime_gamepad_clear();
        pthread_mutex_lock(&settings_lock); actual_width = actual_height = 0; pthread_mutex_unlock(&settings_lock);
    }
}

void android_main(struct android_app *app) {
    Renderer r = {.app = app};
    runtime_gamepad_start();
    pthread_mutex_lock(&settings_lock); render_looper = app->looper; pthread_mutex_unlock(&settings_lock);
    app->userData = &r;
    app->onAppCmd = command;
    app->onInputEvent = input;
    uint64_t next_frame = 0;
    while (!app->destroyRequested) {
        int events;
        struct android_poll_source *source;
        int timeout = r.ready ? 0 : -1;
        while (ALooper_pollOnce(timeout, NULL, &events, (void **)&source) >= 0) {
            if (source) source->process(app, source);
            if (app->destroyRequested) break;
            timeout = 0;
        }
        if (app->destroyRequested) break;
        pthread_mutex_lock(&settings_lock);
        int pending = handled_revision != settings_revision;
        pthread_mutex_unlock(&settings_lock);
        if (app->window && pending) reconfigure(&r);
        if (!r.ready) continue;
        uint64_t now = now_ns();
        if (next_frame > now) {
            struct timespec delay = {(time_t)((next_frame - now) / 1000000000ull),
                                     (long)((next_frame - now) % 1000000000ull)};
            nanosleep(&delay, NULL);
        }
        if (!frame(&r)) { destroy(&r); if (app->window) reconfigure(&r); }
        next_frame = now_ns() + 33333333ull; /* Probe cap only; production will use Swappy. */
    }
    pthread_mutex_lock(&settings_lock); render_looper = NULL; pthread_mutex_unlock(&settings_lock);
    destroy(&r);
}
