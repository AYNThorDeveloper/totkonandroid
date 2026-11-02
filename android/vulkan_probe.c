/* Read-only Vulkan capability probe for the connected Android device. */
#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan.h>

int main(void) {
    VkApplicationInfo app = {0};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "TOTK Android capability probe";
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo info = {0};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;
    VkInstance instance;
    VkResult result = vkCreateInstance(&info, NULL, &instance);
    if (result != VK_SUCCESS) { fprintf(stderr, "vkCreateInstance: %d\n", result); return 1; }
    uint32_t count = 0;
    result = vkEnumeratePhysicalDevices(instance, &count, NULL);
    if (result != VK_SUCCESS || !count) { fprintf(stderr, "no Vulkan GPU: %d\n", result); vkDestroyInstance(instance, NULL); return 1; }
    VkPhysicalDevice devices[8];
    if (count > 8) count = 8;
    result = vkEnumeratePhysicalDevices(instance, &count, devices);
    if (result != VK_SUCCESS) { fprintf(stderr, "enumerate GPUs: %d\n", result); vkDestroyInstance(instance, NULL); return 1; }
    for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceProperties p;
        VkPhysicalDeviceMemoryProperties m;
        vkGetPhysicalDeviceProperties(devices[i], &p);
        vkGetPhysicalDeviceMemoryProperties(devices[i], &m);
        uint32_t qcount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &qcount, NULL);
        VkQueueFamilyProperties q[32];
        if (qcount > 32) qcount = 32;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &qcount, q);
        uint32_t graphics = 0;
        for (uint32_t j = 0; j < qcount; ++j) if (q[j].queueFlags & VK_QUEUE_GRAPHICS_BIT) graphics++;
        uint32_t ecount = 0;
        vkEnumerateDeviceExtensionProperties(devices[i], NULL, &ecount, NULL);
        VkExtensionProperties ext[512];
        if (ecount > 512) ecount = 512;
        vkEnumerateDeviceExtensionProperties(devices[i], NULL, &ecount, ext);
        int swapchain = 0, timeline = 0, memory_budget = 0;
        for (uint32_t j = 0; j < ecount; ++j) {
            swapchain |= !strcmp(ext[j].extensionName, "VK_KHR_swapchain");
            timeline |= !strcmp(ext[j].extensionName, "VK_KHR_timeline_semaphore");
            memory_budget |= !strcmp(ext[j].extensionName, "VK_EXT_memory_budget");
        }
        printf("gpu=%s\napi=%u.%u.%u\nvendor=0x%x device=0x%x\nmax_image_2d=%u\n"
               "graphics_queue_families=%u\nswapchain=%d timeline_semaphore_extension=%d memory_budget_extension=%d\n"
               "memory_heaps=%u\n",
               p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion),
               VK_API_VERSION_PATCH(p.apiVersion), p.vendorID, p.deviceID,
               p.limits.maxImageDimension2D, graphics, swapchain, timeline, memory_budget, m.memoryHeapCount);
    }
    vkDestroyInstance(instance, NULL);
    return 0;
}
