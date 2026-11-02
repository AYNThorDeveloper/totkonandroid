/* Presentation of NVN window textures on the Android surface.
 *
 * mtl_present (nvn_mtl.h) blits the game's window texture, cropped as the
 * game asked, onto a swapchain image of the activity's surface and presents
 * it. The blit runs on the same queue after the game's submissions, so the
 * texture holds the finished frame. Presents are serialized here; the ticket
 * is the present count, and mtl_present_wait returns once it is queued. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "nvn_vk_internal.h"
/* Live switches live in nvn_vk_draw.c; tests that link this file without it read the environment. */
__attribute__((weak)) unsigned nvk_live_gen;
__attribute__((weak)) const char *nvk_live_getenv(const char *name) { return getenv(name); }
#include "android_gfx.h"
#include "android_video_menu.h"
#include <android/native_window.h>
extern ANativeWindow *android_front_acquire_window(void);

#define FRAMES 3

static VkSwapchainKHR swapchain;
static VkFormat swap_format;
static VkExtent2D swap_extent;
static VkImage *swap_images;
static uint32_t swap_count;
static struct { VkCommandPool pool; VkCommandBuffer cb; VkFence fence; VkSemaphore acquired, done; } frames[FRAMES];
static unsigned frame_next;
static uint64_t presents;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static double last_present_s[64];

/* FPS counter (TOTK_FPS_OVERLAY=1): presents per second, updated twice a second, drawn top right into the swapchain
 * image after the game's frame. The digits are drawn on the CPU into a small host buffer per present slot (3x5 font,
 * scaled) and copied in; nothing touches the game's own images. */
#define OV_SCALE 6
#define OV_W (3 * (4 * OV_SCALE) + 2 * OV_SCALE)
#define OV_H (5 * OV_SCALE + 2 * OV_SCALE)
static struct { VkBuffer buf; VkDeviceMemory mem; uint8_t *map; } overlay[FRAMES];
static int overlay_on(void) {
    static int on = -1;
    static unsigned live_TOTK_FPS_OVERLAY; if (on < 0 || live_TOTK_FPS_OVERLAY != nvk_live_gen) { live_TOTK_FPS_OVERLAY = nvk_live_gen; const char *e = nvk_live_getenv("TOTK_FPS_OVERLAY"); on = e && !strcmp(e, "1"); }
    return on;
}
static unsigned overlay_fps(double now) {
    static double window_start; static unsigned count, shown;
    if (!window_start) window_start = now;
    count++;
    if (now - window_start >= 0.5) { shown = (unsigned)(count / (now - window_start) + 0.5); count = 0; window_start = now; }
    return shown;
}
static void overlay_draw(unsigned f, VkCommandBuffer cb, VkImage dst, double now) {
    int bgra = swap_format == VK_FORMAT_B8G8R8A8_UNORM || swap_format == VK_FORMAT_B8G8R8A8_SRGB;
    if (!overlay_on() || (!bgra && swap_format != VK_FORMAT_R8G8B8A8_UNORM && swap_format != VK_FORMAT_R8G8B8A8_SRGB)) return;
    if (swap_extent.width < OV_W + 32 || swap_extent.height < OV_H + 32) return;
    if (!overlay[f].map) {
        VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = OV_W * OV_H * 4, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
        VKCHECK(vkCreateBuffer(nvk_dev, &bci, NULL, &overlay[f].buf), "create FPS overlay buffer");
        VkMemoryRequirements req; vkGetBufferMemoryRequirements(nvk_dev, overlay[f].buf, &req);
        VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
            .memoryTypeIndex = nvk_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        VKCHECK(vkAllocateMemory(nvk_dev, &mai, NULL, &overlay[f].mem), "allocate FPS overlay memory");
        VKCHECK(vkBindBufferMemory(nvk_dev, overlay[f].buf, overlay[f].mem, 0), "bind FPS overlay memory");
        VKCHECK(vkMapMemory(nvk_dev, overlay[f].mem, 0, VK_WHOLE_SIZE, 0, (void **)&overlay[f].map), "map FPS overlay memory");
    }
    static const uint16_t font[10] = {0x7b6f, 0x2c97, 0x73e7, 0x73cf, 0x5bc9, 0x79cf, 0x79ef, 0x7249, 0x7bef, 0x7bcf};   /* 3x5, row-major, top bit first */
    unsigned fps = overlay_fps(now); if (fps > 999) fps = 999;
    char text[4]; snprintf(text, sizeof text, "%u", fps);
    uint8_t *px = overlay[f].map;
    for (int i = 0; i < OV_W * OV_H; ++i) { px[4 * i] = px[4 * i + 1] = px[4 * i + 2] = 0; px[4 * i + 3] = 255; }   /* black box */
    uint8_t ink[4] = {80, 255, 80, 255};   /* green, RGBA */
    if (bgra) { ink[0] = 80; ink[2] = 80; }
    for (int c = 0; text[c]; ++c) {
        uint16_t g = font[text[c] - '0'];
        for (int row = 0; row < 5; ++row) for (int col = 0; col < 3; ++col) {
            if (!(g >> (14 - (row * 3 + col)) & 1)) continue;
            for (int dy = 0; dy < OV_SCALE; ++dy) for (int dx = 0; dx < OV_SCALE; ++dx) {
                int x = OV_SCALE + c * 4 * OV_SCALE + col * OV_SCALE + dx, y = OV_SCALE + row * OV_SCALE + dy;
                memcpy(px + 4 * (y * OV_W + x), ink, 4);
            }
        }
    }
    VkBufferImageCopy r = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageOffset = {(int32_t)swap_extent.width - OV_W - 24, 24, 0}, .imageExtent = {OV_W, OV_H, 1}};
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);   /* after the blit */
    vkCmdCopyBufferToImage(cb, overlay[f].buf, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
}

/* Black-frame detector (TOTK_BLACK_DETECT=1): every present also blits the frame down to 64x36 into a host buffer.
 * When the slot is reused (three presents later) the mean brightness and an 8x4 brightness grid are checked. A frame
 * that falls far below the recent average, the weapon/spell menu backdrop going black, is logged with the draw skip
 * counters of that frame, so the cause (skipped draws, missing shader, pipeline still compiling) shows up in the log. */
#define PR_W 64
#define PR_H 36
__attribute__((weak)) void nvk_draw_totals(uint64_t out[12]) { memset(out, 0, 12 * sizeof *out); }
static int probe_on(void) {
    static int on = -1;
    static unsigned live_TOTK_BLACK_DETECT; if (on < 0 || live_TOTK_BLACK_DETECT != nvk_live_gen) { live_TOTK_BLACK_DETECT = nvk_live_gen; const char *e = nvk_live_getenv("TOTK_BLACK_DETECT"); on = e && !strcmp(e, "1"); }
    return on;
}
static VkImage probe_image; static VkDeviceMemory probe_memory;
static struct { VkBuffer buf; VkDeviceMemory mem; uint8_t *map; uint64_t ticket, totals[12]; int armed; } probe[FRAMES];
static void probe_record(unsigned f, VkCommandBuffer cb, VkImage source, VkImageLayout source_layout, VkExtent2D src_extent, const int rect[4], uint64_t ticket) {
    if (!probe_image) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
            .extent = {PR_W, PR_H, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        VKCHECK(vkCreateImage(nvk_dev, &ci, NULL, &probe_image), "create black-frame probe image");
        VkMemoryRequirements req; vkGetImageMemoryRequirements(nvk_dev, probe_image, &req);
        VkMemoryAllocateInfo mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
            .memoryTypeIndex = nvk_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
        VKCHECK(vkAllocateMemory(nvk_dev, &mi, NULL, &probe_memory), "allocate black-frame probe image");
        VKCHECK(vkBindImageMemory(nvk_dev, probe_image, probe_memory, 0), "bind black-frame probe image");
        VKLOG("black-frame detector on: 64x36 probe per present, log lines start with 'BLACKFRAME'");
    }
    if (!probe[f].map) {
        VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = PR_W * PR_H * 4, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VKCHECK(vkCreateBuffer(nvk_dev, &bci, NULL, &probe[f].buf), "create black-frame probe buffer");
        VkMemoryRequirements req; vkGetBufferMemoryRequirements(nvk_dev, probe[f].buf, &req);
        VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
            .memoryTypeIndex = nvk_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        VKCHECK(vkAllocateMemory(nvk_dev, &mai, NULL, &probe[f].mem), "allocate black-frame probe memory");
        VKCHECK(vkBindBufferMemory(nvk_dev, probe[f].buf, probe[f].mem, 0), "bind black-frame probe memory");
        VKCHECK(vkMapMemory(nvk_dev, probe[f].mem, 0, VK_WHOLE_SIZE, 0, (void **)&probe[f].map), "map black-frame probe memory");
    }
    (void)src_extent;
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = probe_image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{rect[0], rect[1], 0}, {rect[2], rect[3], 1}},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {PR_W, PR_H, 1}}};
    vkCmdBlitImage(cb, source, source_layout, probe_image, VK_IMAGE_LAYOUT_GENERAL, 1, &blit, VK_FILTER_LINEAR);
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    VkBufferImageCopy r = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {PR_W, PR_H, 1}};
    vkCmdCopyImageToBuffer(cb, probe_image, VK_IMAGE_LAYOUT_GENERAL, probe[f].buf, 1, &r);
    probe[f].ticket = ticket; probe[f].armed = 1;
    nvk_draw_totals(probe[f].totals);
}
/* Called once the slot's fence has signalled: its probe holds the frame presented three presents ago. */
static void probe_check(unsigned f) {
    if (!probe[f].armed) return;
    probe[f].armed = 0;
    unsigned grid[32] = {0}, cells[32] = {0}; double sum = 0;
    for (int y = 0; y < PR_H; ++y) for (int x = 0; x < PR_W; ++x) {
        const uint8_t *p = probe[f].map + 4 * (y * PR_W + x);
        unsigned l = (p[0] * 54 + p[1] * 183 + p[2] * 19) >> 8;   /* RGB or BGR, near enough for a brightness test */
        sum += l; unsigned c = (y * 4 / PR_H) * 8 + x * 8 / PR_W; grid[c] += l; cells[c]++;
    }
    double mean = sum / (PR_W * PR_H);
    static double avg = -1; static uint64_t last_totals[12], last_ticket; static unsigned run, logged;
    uint64_t d[12]; for (int i = 0; i < 12; ++i) d[i] = probe[f].totals[i] - last_totals[i];
    memcpy(last_totals, probe[f].totals, sizeof last_totals);
    int dark = avg > 18 && mean < avg * 0.35;
    if (dark || (run && mean < avg * 0.6)) {
        ++run;
        if (logged < 3000 && (run == 1 || run == 10 || run == 40 || run == 150 || run % 600 == 0)) {
            ++logged;
            char g[32 * 4 + 1]; int n = 0;
            for (int i = 0; i < 32; ++i) n += snprintf(g + n, sizeof g - n, "%3u ", cells[i] ? grid[i] / cells[i] : 0);
            VKLOG("BLACKFRAME present %llu: brightness %.1f against recent average %.1f (run of %u); grid 8x4: %s| draws in this frame %llu, skipped: no program %llu, disabled %llu, culled %llu, index %llu, no shader %llu, pipeline compiling %llu; EQUAL draws on 16-bit depth without bias %llu, bias tracking cleared %llu, evicted %llu; presents since the previous probe %llu",
                (unsigned long long)probe[f].ticket, mean, avg, run, g, (unsigned long long)d[0], (unsigned long long)d[1], (unsigned long long)d[2],
                (unsigned long long)d[3], (unsigned long long)d[4], (unsigned long long)d[5], (unsigned long long)d[7], (unsigned long long)d[9], (unsigned long long)d[10], (unsigned long long)d[11], (unsigned long long)(probe[f].ticket - last_ticket));
        }
    } else {
        if (run > 1 && logged < 3000) VKLOG("BLACKFRAME over after %u frames at present %llu (brightness %.1f)", run, (unsigned long long)probe[f].ticket, mean);
        run = 0; avg = avg < 0 ? mean : avg * 0.9 + mean * 0.1;
    }
    last_ticket = probe[f].ticket;
}

static void create_swapchain(void) {
    const AndroidGfxVulkan *vk = android_gfx_vulkan();
    if (!vk || !vk->surface) nvk_die("present without an Android surface");
    const char *output=getenv("TOTK_VIDEO_OUTPUT");
    unsigned requested_w=0,requested_h=0;
    if(output && sscanf(output,"%ux%u",&requested_w,&requested_h)==2) {
        ANativeWindow *window=android_front_acquire_window();
        if(window) {
            int result=ANativeWindow_setBuffersGeometry(window,(int)requested_w,(int)requested_h,0);
            VKLOG("video settings output request %ux%u -> %d",requested_w,requested_h,result);
            ANativeWindow_release(window);
        }
    }
    VkSurfaceCapabilitiesKHR caps;
    VKCHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(nvk_phys, vk->surface, &caps), "query surface capabilities");
    uint32_t n = 0;
    VKCHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(nvk_phys, vk->surface, &n, NULL), "count surface formats");
    VkSurfaceFormatKHR *formats = calloc(n ? n : 1, sizeof *formats);
    VKCHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(nvk_phys, vk->surface, &n, formats), "query surface formats");
    VkSurfaceFormatKHR chosen = formats[0];
    for (uint32_t i = 0; i < n; ++i)
        if (formats[i].format == VK_FORMAT_R8G8B8A8_UNORM || formats[i].format == VK_FORMAT_B8G8R8A8_UNORM) { chosen = formats[i]; break; }
    free(formats);
    swap_format = chosen.format;
    swap_extent = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : (VkExtent2D){1920, 1080};
    /* The Thor's panel is natively portrait: the surface reports a 90/270 degree
       current transform. The blit does not pre-rotate, so the compositor rotates
       (identity pre-transform) and the swapchain uses the landscape extent. */
    int rotated = caps.currentTransform & (VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR | VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR);
    if (rotated && swap_extent.height > swap_extent.width) swap_extent = (VkExtent2D){swap_extent.height, swap_extent.width};
    android_video_menu_active_output(swap_extent.width,swap_extent.height);
    VKLOG("surface current transform %#x, extent %ux%u", caps.currentTransform, swap_extent.width, swap_extent.height);
    uint32_t images = caps.minImageCount + 1;
    if (caps.maxImageCount && images > caps.maxImageCount) images = caps.maxImageCount;
    VkSwapchainCreateInfoKHR sci = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = vk->surface,
        .minImageCount = images, .imageFormat = chosen.format, .imageColorSpace = chosen.colorSpace, .imageExtent = swap_extent,
        .imageArrayLayers = 1, .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE};
    if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)) sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    VKCHECK(vkCreateSwapchainKHR(nvk_dev, &sci, NULL, &swapchain), "create Android swapchain");
    VKCHECK(vkGetSwapchainImagesKHR(nvk_dev, swapchain, &swap_count, NULL), "count swapchain images");
    swap_images = calloc(swap_count, sizeof *swap_images);
    VKCHECK(vkGetSwapchainImagesKHR(nvk_dev, swapchain, &swap_count, swap_images), "get swapchain images");
    for (unsigned i = 0; i < FRAMES; ++i) {
        VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, .queueFamilyIndex = nvk_family};
        VKCHECK(vkCreateCommandPool(nvk_dev, &pci, NULL, &frames[i].pool), "create present command pool");
        VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = frames[i].pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        VKCHECK(vkAllocateCommandBuffers(nvk_dev, &cai, &frames[i].cb), "allocate present command buffer");
        VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
        VKCHECK(vkCreateFence(nvk_dev, &fci, NULL, &frames[i].fence), "create present fence");
        VkSemaphoreCreateInfo semi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VKCHECK(vkCreateSemaphore(nvk_dev, &semi, NULL, &frames[i].acquired), "create acquire semaphore");
        VKCHECK(vkCreateSemaphore(nvk_dev, &semi, NULL, &frames[i].done), "create present semaphore");
    }
    VKLOG("swapchain %ux%u format %d, %u images, FIFO", swap_extent.width, swap_extent.height, swap_format, swap_count);
}

static void image_barrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to,
                          VkAccessFlags src, VkAccessFlags dst, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = src, .dstAccessMask = dst,
        .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

/* Blitting SRGB directly into the UNORM Android surface decodes gamma and
 * writes linear values as display bytes, making midtones too dark. A raw
 * compatible-format copy preserves the game's encoded bytes before scaling.
 * Only used for the two SRGB window formats; UNORM output is unchanged. */
static VkImage encoded_image;
static VkDeviceMemory encoded_memory;
static VkExtent2D encoded_extent;
static VkFormat encoded_format;
static VkImage presentation_source(NvkImage *src, VkCommandBuffer cb) {
    VkFormat format = nvk_image_format(src), raw;
    if (format == VK_FORMAT_R8G8B8A8_SRGB) raw = VK_FORMAT_R8G8B8A8_UNORM;
    else if (format == VK_FORMAT_B8G8R8A8_SRGB) raw = VK_FORMAT_B8G8R8A8_UNORM;
    else return nvk_image_handle(src);
    VkExtent2D extent=nvk_image_level_extent(src,0);
    if (!encoded_image || encoded_extent.width!=extent.width || encoded_extent.height!=extent.height || encoded_format!=raw) {
        /* Resize is rare. Wait for prior present copies before replacing the
         * shared intermediate, whose users are on the same Vulkan queue. */
        if (encoded_image) {
            nvk_record_sync();   /* queued segment submits must run before taking the shared queue lock */
            nvk_queue_lock(); VkResult r=vkQueueWaitIdle(nvk_queue); nvk_queue_unlock();
            VKCHECK(r,"resize encoded present image");
            vkDestroyImage(nvk_dev,encoded_image,NULL); vkFreeMemory(nvk_dev,encoded_memory,NULL);
        }
        encoded_extent=extent; encoded_format=raw;
        VkImageCreateInfo ci={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=raw,
            .extent={extent.width,extent.height,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,
            .tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .sharingMode=VK_SHARING_MODE_EXCLUSIVE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
        VKCHECK(vkCreateImage(nvk_dev,&ci,NULL,&encoded_image),"create encoded present image");
        VkMemoryRequirements req; vkGetImageMemoryRequirements(nvk_dev,encoded_image,&req);
        VkMemoryAllocateInfo mi={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,
            .memoryTypeIndex=nvk_memory_type(req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
        VKCHECK(vkAllocateMemory(nvk_dev,&mi,NULL,&encoded_memory),"allocate encoded present image");
        VKCHECK(vkBindImageMemory(nvk_dev,encoded_image,encoded_memory,0),"bind encoded present image");
        image_barrier(cb,encoded_image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,0,
            VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VKLOG("present SRGB %d via raw UNORM %d: preserve encoded display colours",format,raw);
    }
    nvk_barrier(cb);
    VkImageCopy region={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.extent={extent.width,extent.height,1}};
    vkCmdCopyImage(cb,nvk_image_handle(src),VK_IMAGE_LAYOUT_GENERAL,encoded_image,VK_IMAGE_LAYOUT_GENERAL,1,&region);
    nvk_barrier(cb);
    return encoded_image;
}

uint64_t mtl_present(const tex_t *t, double period, const int crop[4]) {
    (void)period;
    if (!t) nvk_die("present without a texture");
    NvkImage *src = nvk_image_existing(t->serial);
    if (!src) {
        /* Never rendered by the GPU path: nothing the game drew can be shown. */
        static int said;
        if (said++ < 4) VKLOG("present of texture %llu that no GPU pass has written; skipped", (unsigned long long)t->serial);
        pthread_mutex_lock(&lock); uint64_t ticket = ++presents; pthread_mutex_unlock(&lock);
        return ticket;
    }
    pthread_mutex_lock(&lock);
    if (!swapchain) create_swapchain();
    unsigned f = frame_next++ % FRAMES;
    VKCHECK(vkWaitForFences(nvk_dev, 1, &frames[f].fence, VK_TRUE, UINT64_MAX), "wait for present slot");
    probe_check(f);
    uint32_t index;
    VkResult r = vkAcquireNextImageKHR(nvk_dev, swapchain, UINT64_MAX, frames[f].acquired, VK_NULL_HANDLE, &index);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { VKLOG("acquire swapchain image -> %d", r); pthread_mutex_unlock(&lock); nvk_die("acquire swapchain image"); }
    VKCHECK(vkResetFences(nvk_dev, 1, &frames[f].fence), "reset present fence");
    VKCHECK(vkResetCommandPool(nvk_dev, frames[f].pool, 0), "reset present pool");
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VkCommandBuffer cb = frames[f].cb;
    VKCHECK(vkBeginCommandBuffer(cb, &bi), "begin present commands");
    nvk_barrier(cb);   /* the game's rendering into the window texture */
    VkImage dst = swap_images[index];
    image_barrier(cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkExtent2D se = nvk_image_level_extent(src, 0);
    int x0 = 0, y0 = 0, x1 = (int)se.width, y1 = (int)se.height;
    if (crop && crop[2] > 0 && crop[3] > 0) { x0 = crop[0]; y0 = crop[1]; x1 = crop[0] + crop[2]; y1 = crop[1] + crop[3]; }
    VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{x0, y0, 0}, {x1, y1, 1}},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {(int)swap_extent.width, (int)swap_extent.height, 1}}};
    VkImage source = presentation_source(src, cb);
    vkCmdBlitImage(cb, source, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    if (probe_on()) { int rect[4] = {x0, y0, x1, y1}; probe_record(f, cb, source, VK_IMAGE_LAYOUT_GENERAL, se, rect, presents + 1); }
    { struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now); overlay_draw(f, cb, dst, (double)now.tv_sec + now.tv_nsec / 1e9); }
    image_barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    VKCHECK(vkEndCommandBuffer(cb), "end present commands");
    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &frames[f].acquired,
        .pWaitDstStageMask = &wait, .commandBufferCount = 1, .pCommandBuffers = &cb, .signalSemaphoreCount = 1, .pSignalSemaphores = &frames[f].done};
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &frames[f].done,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &index};
    nvk_queue_lock();
    VkResult sr = vkQueueSubmit(nvk_queue, 1, &si, frames[f].fence);
    VkResult pr = sr == VK_SUCCESS ? vkQueuePresentKHR(nvk_queue, &pi) : sr;
    nvk_queue_unlock();
    if (sr != VK_SUCCESS || (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR)) {
        VKLOG("present submit %d present %d", sr, pr); pthread_mutex_unlock(&lock); nvk_die("present to the Android surface");
    }
    uint64_t ticket = ++presents;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    last_present_s[ticket % 64] = (double)ts.tv_sec + ts.tv_nsec / 1e9;
    if (ticket == 1 || ticket % 600 == 0) VKLOG("presented frame %llu (texture %llu, %dx%d crop, source format %d, surface %d)", (unsigned long long)ticket,
        (unsigned long long)t->serial, x1 - x0, y1 - y0, nvk_image_format(src), swap_format);
    pthread_mutex_unlock(&lock);
    return ticket;
}

void mtl_present_wait(uint64_t ticket) { (void)ticket; }   /* mtl_present queues synchronously */
void mtl_present_flush(void) {}

/* Time the n-th present was queued (seconds, monotonic); 0 = not yet. */
double mtl_presented_time(uint64_t n) {
    pthread_mutex_lock(&lock);
    double t = n && n <= presents && presents - n < 64 ? last_present_s[n % 64] : 0;
    pthread_mutex_unlock(&lock);
    return t;
}
