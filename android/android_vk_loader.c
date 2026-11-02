/* Which Vulkan driver the game uses.
 * Default: the system driver through the platform loader (libvulkan.so).
 * TOTK_VK_DRIVER=<file> (e.g. vulkan.freedreno.so, a Turnip build in the
 * adrenotools format) loads that driver from TOTK_VK_DRIVER_DIR instead,
 * through libadrenotools; its hook libraries ship in the APK's lib folder. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <adrenotools/driver.h>
#include <vulkan/vulkan.h>

extern _Noreturn void runtime_fail(const char *message);

static void own_library_dir(char *out, size_t n) {
    Dl_info info;
    if (!dladdr((void *)own_library_dir, &info) || !info.dli_fname) runtime_fail("locate the game library directory");
    snprintf(out, n, "%s", info.dli_fname);
    char *slash = strrchr(out, '/');
    if (!slash) runtime_fail("game library path without directory");
    slash[1] = 0;
}

PFN_vkGetInstanceProcAddr android_vk_loader(void) {
    const char *driver = getenv("TOTK_VK_DRIVER");
    void *lib;
    if (driver && *driver && strcmp(driver, "system")) {
        const char *dir = getenv("TOTK_VK_DRIVER_DIR");
        if (!dir || !*dir) runtime_fail("TOTK_VK_DRIVER needs TOTK_VK_DRIVER_DIR");
        char hooks[PATH_MAX], drivers[PATH_MAX];
        own_library_dir(hooks, sizeof hooks);
        snprintf(drivers, sizeof drivers, "%s%s", dir, dir[strlen(dir) - 1] == '/' ? "" : "/");
        lib = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, NULL, hooks, drivers, driver, NULL, NULL);
        if (!lib) { fprintf(stderr, "[android-gfx] custom driver %s%s could not be opened\n", drivers, driver); runtime_fail("open custom Vulkan driver"); }
        fprintf(stderr, "[android-gfx] Vulkan driver: custom %s%s (libadrenotools, hooks in %s)\n", drivers, driver, hooks);
    } else {
        lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        if (!lib) runtime_fail("open libvulkan.so");
        fprintf(stderr, "[android-gfx] Vulkan driver: system\n");
    }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    if (!gipa) runtime_fail("Vulkan loader without vkGetInstanceProcAddr");
    return gipa;
}
