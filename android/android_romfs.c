/* Read-only Android RomFS backend using the existing verified TOTKRFS1 format.
 * Writable save, temporary and cache storage come from the shared fs_host.c.
 * No split images, RomFS directory enumeration or overlays yet. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "romfs_path.h"
#include "sha256.h"
#if __has_include("romfs_config.h")
#include "romfs_config.h"
#else
#define ROMFS_IMAGE_SIZE 0ULL
#define ROMFS_IMAGE_SHA256 ""
#define ROMFS_INDEX_SHA256 ""
#endif
#ifdef __APPLE__
#define MTIME_NSEC(st) ((st).st_mtimensec)
#else
#define MTIME_NSEC(st) ((st).st_mtim.tv_nsec)
#endif
#define INVALID 0xe402u
#define NOT_FOUND 0x202u
#define UNSUPPORTED 0x6402u
#define NH 4096u
#define BASE 0x100000000ULL
extern _Noreturn void runtime_fail(const char *message);
extern void dm_guest_write_prepare(void *buffer, size_t size);
extern void *runtime_fs_host_override(const char *name);
void runtime_fs_log(const char *what, const char *path);
typedef struct { uint64_t offset, size; uint32_t name, len; } Entry;
_Static_assert(sizeof(Entry) == 24, "RomFS entry layout");
typedef struct { uint64_t file, generation; int overlay_fd_plus_one; uint64_t overlay_size; } Handle;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *index_data;
static uint64_t count, strings;
static int image_fd = -1;
static char mount_name[32];
static Handle handles[NH];
static void need(int ok, const char *why) { if (!ok) runtime_fail(why); }
static int read_at(int fd, void *buf, size_t size, uint64_t off) {
    size_t done = 0;
    while (done < size) {
        size_t chunk = size - done;
        if (chunk > (8u << 20)) chunk = 8u << 20;
        ssize_t n = pread(fd, (uint8_t *)buf + done, chunk, (off_t)(off + done));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        done += (size_t)n;
    }
    return 1;
}
static void verify(int fd, uint64_t size, const char *expected) {
    Sha256 sha; sha_init(&sha);
    uint8_t *buf = malloc(8u << 20); need(buf != NULL, "RomFS hash allocation");
    for (uint64_t off = 0; off < size;) {
        size_t n = size - off > (8u << 20) ? (8u << 20) : (size_t)(size - off);
        need(read_at(fd, buf, n, off), "RomFS hash read failed");
        sha_update(&sha, buf, n); off += n;
    }
    uint8_t digest[32]; char hex[65]; sha_final(&sha, digest); digest_hex(digest, hex);
    free(buf); need(!strcmp(hex, expected), "RomFS SHA-256 mismatch");
}
static Entry entry(uint64_t i) { Entry e; memcpy(&e, index_data + 24 + i * 24, 24); return e; }
static const char *name_of(Entry e) { return (const char *)index_data + strings + e.name; }
static uint64_t lower(const char *name) {
    uint64_t lo = 0, hi = count;
    while (lo < hi) { uint64_t mid = lo + (hi - lo) / 2; if (strcmp(name_of(entry(mid)), name) < 0) lo = mid + 1; else hi = mid; }
    return lo;
}
/* With lock held. Lazy initialization keeps clock/checkpoint runs data-free. */
static void init_locked(void) {
    if (image_fd >= 0) return;
    need(ROMFS_IMAGE_SIZE > 0, "Android RomFS metadata missing: prepare v143 RomFS and rebuild loader");
    const char *dir = getenv("TOTK_ROMFS");
    if (!dir || !*dir) dir = "extracted/v143/romfs";
    char path[4096]; struct stat st;
    need(snprintf(path, sizeof path, "%s/runtime-index.bin", dir) < (int)sizeof path, "RomFS path too long");
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    need(fd >= 0 && !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size >= 24 && st.st_size < (128 << 20), "RomFS index missing or invalid");
    size_t index_size = (size_t)st.st_size;
    index_data = malloc(index_size); need(index_data != NULL, "RomFS index allocation");
    need(read_at(fd, index_data, index_size, 0), "RomFS index read failed"); close(fd);
    /* Hash the actual private copy used for parsing. */
    Sha256 sha; uint8_t digest[32]; char hex[65]; sha_init(&sha);
    sha_update(&sha, index_data, index_size); sha_final(&sha, digest); digest_hex(digest, hex);
    need(!strcmp(hex, ROMFS_INDEX_SHA256), "RomFS index SHA-256 mismatch");
    need(!memcmp(index_data, "TOTKRFS1", 8), "RomFS index magic");
    memcpy(&count, index_data + 8, 8); memcpy(&strings, index_data + 16, 8);
    need(count < 1000000 && strings == 24 + count * 24 && strings <= index_size, "RomFS index layout");
    const char *previous = NULL;
    for (uint64_t i = 0; i < count; ++i) {
        Entry e = entry(i);
        need(e.offset <= ROMFS_IMAGE_SIZE && e.size <= ROMFS_IMAGE_SIZE - e.offset, "RomFS file bounds");
        need(e.name <= index_size - strings && e.len > 1 && e.len <= index_size - strings - e.name, "RomFS name bounds");
        const char *name = name_of(e);
        need(name[e.len - 1] == 0 && !memchr(name, 0, e.len - 1) && romfs_path_valid_relative(name) && name[e.len - 2] != '/' && !strchr(name, ':') && (!previous || strcmp(previous, name) < 0), "RomFS invalid or unsorted path");
        previous = name;
    }
    need(snprintf(path, sizeof path, "%s/romfs.bin", dir) < (int)sizeof path, "RomFS image path too long");
    fd = open(path, O_RDONLY | O_CLOEXEC);
    need(fd >= 0 && !fstat(fd, &st) && S_ISREG(st.st_mode) && (uint64_t)st.st_size == ROMFS_IMAGE_SIZE && st.st_size <= INT64_MAX, "RomFS image missing or wrong size (split images unsupported)");
    /* TOTK_ROMFS_VERIFIED_CACHE names a file outside the image directory that
       records the identity of an image that passed a full hash. A changed
       device, inode, size or modification time forces a new full hash. */
    const char *cache = getenv("TOTK_ROMFS_VERIFIED_CACHE");
    char stamp[256], seen[256] = "";
    snprintf(stamp, sizeof stamp, "%llu %llu %llu %lld.%09ld %s\n", (unsigned long long)st.st_dev,
        (unsigned long long)st.st_ino, (unsigned long long)st.st_size, (long long)st.st_mtime,
        (long)MTIME_NSEC(st), ROMFS_IMAGE_SHA256);
    FILE *cf = cache && *cache ? fopen(cache, "r") : NULL;
    if (cf) { if (!fgets(seen, sizeof seen, cf)) *seen = 0; fclose(cf); }
    if (*seen && !strcmp(seen, stamp)) {
        fprintf(stderr, "[android-romfs] image identity matches earlier full SHA-256 pass; skipping rehash\n");
    } else {
        fprintf(stderr, "[android-romfs] verifying game image SHA-256...\n");
        verify(fd, ROMFS_IMAGE_SIZE, ROMFS_IMAGE_SHA256);
        cf = cache && *cache ? fopen(cache, "w") : NULL;
        if (cf) { fputs(stamp, cf); fclose(cf); }
    }
    image_fd = fd;
    fprintf(stderr, "[android-romfs] read-only backend ready: %llu files\n", (unsigned long long)count);
}
/* Only generated, hash-verified menu overrides may replace existing files.
 * The base image/index still undergo their original verification. */
#if __has_include("video_menu_assets.h")
#include "video_menu_assets.h"
static int menu_overlay(const char *name, uint64_t *size) {
    const char *dir=getenv("TOTK_ROMFS_OVERLAY");
    if(!dir || !*dir)return -1;
    for(size_t i=0;i<VIDEO_MENU_ASSET_COUNT;i++)if(!strcmp(name,video_menu_assets[i].path)) {
        char path[4096];
        need(snprintf(path,sizeof path,"%s/%s",dir,name)<(int)sizeof path,"menu overlay path too long");
        int fd=open(path,O_RDONLY|O_CLOEXEC);struct stat st;
        need(fd>=0 && !fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_size>=0 &&
            (uint64_t)st.st_size==video_menu_assets[i].size,"menu overlay missing or wrong size");
        verify(fd,(uint64_t)st.st_size,video_menu_assets[i].sha256);
        *size=(uint64_t)st.st_size;
        fprintf(stderr,"[android-romfs] verified original-menu override: %s\n",name);
        return fd;
    }
    return -1;
}
#else
static int menu_overlay(const char *name,uint64_t *size) {(void)name;(void)size;return -1;}
#endif

/* SDK initialization precedes the data-free checkpoint. MountRom does the I/O. */
void runtime_fs_init(void) {}
static unsigned mount_rom(const char *name, void *cache, uint64_t size) {
    (void)cache; (void)size;
    if (!name || !romfs_path_valid_relative(name) || strlen(name) >= sizeof mount_name || strchr(name, '/') || strchr(name, ':')) return INVALID;
    pthread_mutex_lock(&lock);
    if (*mount_name && strcmp(name, mount_name)) { pthread_mutex_unlock(&lock); return UNSUPPORTED; }
    init_locked(); strcpy(mount_name, name); pthread_mutex_unlock(&lock); return 0;
}
static unsigned cache_size(uint64_t *out) { if (!out) return INVALID; *out = 0; return 0; }
static unsigned open_file(uint64_t *out, const char *path, int mode) {
    if (!out || !path) return INVALID;
    if (mode != 1) return UNSUPPORTED;
    pthread_mutex_lock(&lock); const char *p = romfs_path_relative(mount_name, path);
    if (!p) { pthread_mutex_unlock(&lock); return NOT_FOUND; }
    uint64_t i = lower(p);
    if (i >= count || strcmp(name_of(entry(i)), p)) { pthread_mutex_unlock(&lock); return NOT_FOUND; }
    for (unsigned j = 1; j < NH; ++j) if (!handles[j].file) {
        Handle *h = &handles[j];
        /* Reject stale handles even when their table slot has been reused.
           Handles stay below fs_host.c's FILE_BASE (0x200000000). */
        if (h->generation >= BASE / NH - 1) h->generation = 0;
        ++h->generation; h->file = i + 1;
        int fd=menu_overlay(p,&h->overlay_size);h->overlay_fd_plus_one=fd+1;
        *out = BASE + h->generation * NH + j;
        pthread_mutex_unlock(&lock); return 0;
    }
    pthread_mutex_unlock(&lock); return UNSUPPORTED;
}
static Handle *handle_of(uint64_t h) {
    if (h <= BASE) return NULL;
    uint64_t value = h - BASE; unsigned slot = value % NH; Handle *p = &handles[slot];
    return slot && p->file && p->generation == value / NH ? p : NULL;
}
static unsigned file_size(int64_t *out, uint64_t h) {
    if (!out) return INVALID;
    pthread_mutex_lock(&lock); Handle *p = handle_of(h);
    if (p) *out = (int64_t)(p->overlay_fd_plus_one?p->overlay_size:entry(p->file - 1).size);
    pthread_mutex_unlock(&lock); return p ? 0 : INVALID;
}
/* Checked under lock. The verified image descriptor and index live until process exit; a read can use their
   captured offset/size after its logical handle closes. Per-handle overlay descriptors still require the old lock.
   Holding this global lock across pread and guest write preparation serialised every streaming worker.
   TOTK_ROMFS_PARALLEL=0 keeps that original behaviour for A/B. */
static int romfs_parallel_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_ROMFS_PARALLEL"); on = !(e && !strcmp(e, "0")); }
    return on;
}
static unsigned read_file(uint64_t *out, uint64_t h, int64_t off, void *buf, uint64_t size) {
    if (!out || off < 0 || (!buf && size) || size > SIZE_MAX) return INVALID;
    pthread_mutex_lock(&lock); Handle *p = handle_of(h);
    if (!p) { pthread_mutex_unlock(&lock); return INVALID; }
    Entry e = entry(p->file - 1);
    int source=image_fd;
    if(p->overlay_fd_plus_one){source=p->overlay_fd_plus_one-1;e.offset=0;e.size=p->overlay_size;}
    if ((uint64_t)off > e.size) { pthread_mutex_unlock(&lock); return INVALID; }
    uint64_t n = size < e.size - (uint64_t)off ? size : e.size - (uint64_t)off;
    int unlocked = !p->overlay_fd_plus_one && romfs_parallel_on();
    if (unlocked) pthread_mutex_unlock(&lock);
    if (n) dm_guest_write_prepare(buf, (size_t)n);
    int ok = read_at(source, buf, (size_t)n, e.offset + (uint64_t)off);
    if (ok) *out = n;
    if (!unlocked) pthread_mutex_unlock(&lock);
    need(ok, "RomFS image read failure"); return 0;
}
static void close_file(uint64_t h) {
    pthread_mutex_lock(&lock); Handle *p = handle_of(h);
    if (p) {if(p->overlay_fd_plus_one)close(p->overlay_fd_plus_one-1);p->overlay_fd_plus_one=0;p->file=0;}
    pthread_mutex_unlock(&lock); need(p != NULL, "RomFS close invalid handle");
}
static unsigned entry_type(int *out, const char *path) {
    if (!out || !path) return INVALID;
    pthread_mutex_lock(&lock); size_t len = strlen(mount_name);
    if (len && !strncmp(path, mount_name, len) && path[len] == ':' && path[len + 1] == '/') {
        const char *tail = path + len + 2; while (*tail == '/') ++tail;
        if (!*tail) { *out = 0; pthread_mutex_unlock(&lock); return 0; }
    }
    const char *p = romfs_path_relative(mount_name, path); unsigned result = NOT_FOUND;
    if (p) {
        uint64_t i = lower(p);
        if (i < count && !strcmp(name_of(entry(i)), p)) { *out = 1; result = 0; }
        else {
            char dir[4096];
            if (snprintf(dir, sizeof dir, "%s/", p) < (int)sizeof dir) {
                i = lower(dir);
                if (i < count && !strncmp(name_of(entry(i)), dir, strlen(dir))) { *out = 0; result = 0; }
            }
        }
    }
    pthread_mutex_unlock(&lock); return result;
}
/* The RomFS side of the unified nn::fs host (games/totk/runtime/fs_host.h):
 * fs_host.c owns the SDK entry points and routes RomFS paths and handles here. */
int runtime_romfs_owns(const char *path) {
    pthread_mutex_lock(&lock); size_t n = strlen(mount_name);
    int owns = n && path && !strncmp(path, mount_name, n) && path[n] == ':';
    pthread_mutex_unlock(&lock); return owns;
}
int runtime_romfs_handle(uint64_t h) { return h > BASE && h < 2 * BASE; }
unsigned runtime_romfs_open(uint64_t *out, const char *path, int mode) { runtime_fs_log("rom", path ? path : "(null)"); return open_file(out, path, mode); }
unsigned runtime_romfs_read(uint64_t *out, uint64_t h, int64_t offset, void *buffer, uint64_t size) { return read_file(out, h, offset, buffer, size); }
unsigned runtime_romfs_size(int64_t *out, uint64_t h) { return file_size(out, h); }
void runtime_romfs_close(uint64_t h) { close_file(h); }
unsigned runtime_romfs_entry_type(int *out, const char *path) { return entry_type(out, path); }
/* TOTK_FS_LOG=1: log every file open with the time since start. */
void runtime_fs_log(const char *what, const char *path) {
    static int on = -1; static struct timespec t0;
    if (on < 0) { on = getenv("TOTK_FS_LOG") != NULL; clock_gettime(CLOCK_MONOTONIC, &t0); }
    if (!on) return;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    fprintf(stderr, "[totk-fs] %8.3f %s %s\n", (double)(t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9, what, path);
}
void *runtime_fs_override(const char *name) {
    static const struct { const char *name; void *fn; } map[] = {
        {"_ZN2nn2fs8MountRomEPKcPvm", mount_rom},
        {"_ZN2nn2fs22QueryMountRomCacheSizeEPm", cache_size},
        {"_ZN2nn2fs22QueryMountRomCacheSizeEPmi", cache_size},
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; ++i) if (!strcmp(name, map[i].name)) return map[i].fn;
    return runtime_fs_host_override(name);
}
