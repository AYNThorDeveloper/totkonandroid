/* Driver-specific persistent pipeline cache. Disk writes run off the replay
 * thread. A VkPipelineCache synchronizes itself (it is not created with
 * EXTERNALLY_SYNCHRONIZED), so pipeline creation on the replay thread and the
 * prewarm worker never waits for the other or for a snapshot; the mutex only
 * guards the revision count. Snapshots are taken after creation has been
 * quiet for one second. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "nvn_vk_internal.h"

#define CACHE_LIMIT (256u << 20)
static VkPipelineCache cache;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static uint64_t revision;
static char path[768];
static VkPhysicalDeviceProperties properties;

static int valid_header(const uint8_t *data, size_t size) {
    uint32_t header[4];
    if (size < 32) return 0;
    memcpy(header, data, sizeof header);
    return header[0] >= 32 && header[0] <= size && header[1] == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
        header[2] == properties.vendorID && header[3] == properties.deviceID &&
        !memcmp(data + 16, properties.pipelineCacheUUID, VK_UUID_SIZE);
}
static void *save_worker(void *unused) {
    (void)unused;
    pthread_setname_np(pthread_self(), "totk-vk-cache");
    uint64_t saved = 0;
    for (;;) {
        pthread_mutex_lock(&mutex);
        while (saved == revision) pthread_cond_wait(&changed, &mutex);
        /* Debounce compilations. Even continuous creation gets a checkpoint
         * after five seconds; an idle cache is saved after one second. */
        uint64_t seen = revision;
        for (unsigned seconds = 0; seconds < 5; seconds++) {
            struct timespec deadline; clock_gettime(CLOCK_REALTIME, &deadline); deadline.tv_sec++;
            int r = 0;
            while (r != ETIMEDOUT) r = pthread_cond_timedwait(&changed, &mutex, &deadline);
            if (seen == revision) break;
            seen = revision;
        }
        uint64_t snapshot = revision;
        pthread_mutex_unlock(&mutex);
        size_t size = 0;
        VkResult r = vkGetPipelineCacheData(nvk_dev, cache, &size, NULL);
        uint8_t *data = r == VK_SUCCESS && size && size <= CACHE_LIMIT ? malloc(size) : NULL;
        if (data) r = vkGetPipelineCacheData(nvk_dev, cache, &size, data);
        if (r == VK_INCOMPLETE) { free(data); data = NULL; }   /* grew in between: the next revision saves it */
        int ok = 0;
        if (data && r == VK_SUCCESS && valid_header(data, size)) {
            char tmp[800]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
            FILE *f = fopen(tmp, "wb");
            if (f) {
                ok = fwrite(data, 1, size, f) == size;
                if (ok) ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
                if (fclose(f)) ok = 0;
                if (ok) ok = rename(tmp, path) == 0;
                if (!ok) unlink(tmp);
            }
        }
        free(data);
        if (ok) VKLOG("pipeline cache saved: %zu bytes, revision %llu", size, (unsigned long long)snapshot);
        else VKLOG("pipeline cache snapshot/write failed; rendering continues");
        /* Retry on the next creation, not in a tight loop on unwritable storage. */
        saved = snapshot;
    }
    return NULL;
}
static void initialize(void) {
    const char *e = getenv("TOTK_VK_PIPELINE_CACHE");
    if (e && !strcmp(e, "0")) return;
    const char *dir = getenv("TOTK_SHADER_CACHE");
    if (!dir || !*dir) dir = "shader-cache";
    mkdir(dir, 0700);
    vkGetPhysicalDeviceProperties(nvk_phys, &properties);
    char uuid[VK_UUID_SIZE * 2 + 1];
    for (unsigned i = 0; i < VK_UUID_SIZE; i++) snprintf(uuid + 2 * i, 3, "%02x", properties.pipelineCacheUUID[i]);
    int n = snprintf(path, sizeof path, "%s/pipeline-%08x-%08x-%08x-%s.bin", dir,
        properties.vendorID, properties.deviceID, properties.driverVersion, uuid);
    if (n < 0 || (size_t)n >= sizeof path) { path[0] = 0; return; }
    uint8_t *data = NULL; size_t size = 0;
    FILE *f = fopen(path, "rb");
    if (f) {
        struct stat st;
        if (!fstat(fileno(f), &st) && st.st_size >= 32 && st.st_size <= CACHE_LIMIT) {
            size = (size_t)st.st_size; data = malloc(size);
            if (data && (fread(data, 1, size, f) != size || !valid_header(data, size))) { free(data); data = NULL; }
        }
        fclose(f);
    }
    VkPipelineCacheCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
        .initialDataSize = data ? size : 0, .pInitialData = data};
    VkResult r = vkCreatePipelineCache(nvk_dev, &ci, NULL, &cache);
    if (r != VK_SUCCESS && data) { ci.initialDataSize = 0; ci.pInitialData = NULL; r = vkCreatePipelineCache(nvk_dev, &ci, NULL, &cache); }
    free(data);
    if (r != VK_SUCCESS) { cache = VK_NULL_HANDLE; VKLOG("pipeline cache unavailable (%d)", r); return; }
    VKLOG("pipeline cache ready: initial %zu bytes, %s", ci.initialDataSize, path);
    pthread_t thread;
    int err = pthread_create(&thread, NULL, save_worker, NULL);
    if (!err) pthread_detach(thread);
    else VKLOG("pipeline cache saver unavailable (%d); cache remains in memory", err);
}
VkPipelineCache nvk_pipeline_cache_begin(void) {
    pthread_once(&once, initialize);
    return cache;
}
void nvk_pipeline_cache_end(void) {
    if (!cache) return;
    pthread_mutex_lock(&mutex);
    revision++; pthread_cond_signal(&changed);
    pthread_mutex_unlock(&mutex);
}
