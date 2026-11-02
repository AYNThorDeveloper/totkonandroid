#define _POSIX_C_SOURCE 200809L
#include "graphics_settings.h"
#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
TotkGraphicsSettings totk_graphics_defaults(void) {
    return (TotkGraphicsSettings){1280, 720, 1280, 720, 0};
}
static int render_valid(unsigned w, unsigned h) {
    return (w == 640 && h == 360) || (w == 960 && h == 540) || (w == 1280 && h == 720) ||
           (w == 1600 && h == 900) || (w == 1920 && h == 1080);   /* 1600x900 = the Switch's docked size */
}
static int output_valid(unsigned w, unsigned h) {
    return (w == 960 && h == 540) || (w == 1280 && h == 720) || (w == 1600 && h == 900) || (w == 1920 && h == 1080);
}
int totk_graphics_valid(const TotkGraphicsSettings *s) {
    return s && render_valid(s->render_width, s->render_height) && output_valid(s->output_width, s->output_height) &&
        (s->frame_cap == 0 || s->frame_cap == 30 || s->frame_cap == 40 || s->frame_cap == 60 || s->frame_cap == 120);
}
int totk_graphics_load(const char *path, TotkGraphicsSettings *out) {
    if (!out) return -1;
    *out = totk_graphics_defaults();
    if (!path || !*path) return 0;
    FILE *f = fopen(path, "r");
    if (!f) return errno == ENOENT ? 0 : -1;
    char text[256]; size_t n = fread(text, 1, sizeof text - 1, f);
    int error = ferror(f); int end = feof(f); fclose(f);
    if (error || !end || memchr(text, 0, n)) return -1;
    text[n] = 0;
    int v2 = !strncmp(text, "TOTK_GRAPHICS_V2", 16);
    const char *magic = v2 ? "TOTK_GRAPHICS_V2" : "TOTK_GRAPHICS_V1";
    size_t prefix = strlen(magic);
    if (strncmp(text, magic, prefix) || !isspace((unsigned char)text[prefix])) return -1;
    char *cursor = text + prefix;
    unsigned values[5] = {0};
    for (unsigned i = 0; i < (v2 ? 5u : 4u); ++i) {
        if (!isspace((unsigned char)*cursor)) return -1;
        while (isspace((unsigned char)*cursor)) ++cursor;
        if (!isdigit((unsigned char)*cursor)) return -1;
        errno = 0; char *next;
        unsigned long value = strtoul(cursor, &next, 10);
        if (errno || value > UINT_MAX) return -1;
        values[i] = (unsigned)value; cursor = next;
    }
    while (isspace((unsigned char)*cursor)) ++cursor;
    TotkGraphicsSettings s = {values[0], values[1], values[2], values[3], values[4]};
    if (*cursor || !totk_graphics_valid(&s)) return -1;
    *out = s; return 1;
}
int totk_graphics_save(const char *path, const TotkGraphicsSettings *s) {
    if (!path || !*path || !totk_graphics_valid(s)) return 0;
    size_t len = strlen(path);
    char *temp = malloc(len + 16);
    if (!temp) return 0;
    snprintf(temp, len + 16, "%s.XXXXXX", path);
    int fd = mkstemp(temp);
    if (fd < 0) { free(temp); return 0; }
    char data[128]; int n = snprintf(data, sizeof data, "TOTK_GRAPHICS_V2\n%u %u\n%u %u\n%u\n",
        s->render_width, s->render_height, s->output_width, s->output_height, s->frame_cap);
    size_t done = 0;
    while (done < (size_t)n) {
        ssize_t result = write(fd, data + done, (size_t)n - done);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) break;
        done += (size_t)result;
    }
    int ok = done == (size_t)n && fsync(fd) == 0;
    if (close(fd)) ok = 0;
    if (ok && rename(temp, path)) ok = 0;
    if (!ok) unlink(temp);
    free(temp); return ok;
}
int totk_graphics_patch_v143(uint8_t *base, size_t size, size_t file_size,
                             const TotkGraphicsSettings *s) {
    if (!base || ((uintptr_t)base & 4095) || !totk_graphics_valid(s) ||
        file_size > size || file_size > SIZE_MAX - 0x3fff) return 0;
    size_t data = (file_size + 0x3fff) & ~(size_t)0x3fff;
    if (data > size || size - data < 24 || file_size < 0x585e64) return 0;
    const size_t sites[] = {0x585e48, 0x585e50, 0x585e54, 0x585e60};
    const uint32_t original[] = {0x1e220160, 0xbd0cc660, 0x1e220140, 0xbd0cca60};
    for (unsigned i = 0; i < 4; ++i) {
        uint32_t word; memcpy(&word, base + sites[i], 4);
        if (word != original[i]) return 0;
    }
    int64_t pages = (int64_t)(((uintptr_t)base + data) >> 12) -
                    (int64_t)(((uintptr_t)base + sites[0]) >> 12);
    if (pages < -(1 << 20) || pages >= (1 << 20)) return 0;
    uint32_t imm = (uint32_t)pages & 0x1fffff;
    uint32_t patch[] = {0x90000003u | ((imm & 3) << 29) | ((imm >> 2) << 5),
                        0x2d420460, 0xbd0cc660, 0xbd0cca61};
    float resolution[2] = {(float)s->render_width, (float)s->render_height};
    memcpy(base + data + 16, resolution, sizeof resolution);
    for (unsigned i = 0; i < 4; ++i) memcpy(base + sites[i], &patch[i], 4);
    return 1;
}
