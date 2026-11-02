#pragma once
#include <stddef.h>
#include <stdint.h>
typedef struct {
    unsigned render_width, render_height;
    unsigned output_width, output_height;
    unsigned frame_cap; /* 0 = original host pacing; otherwise 30/40/60/120 */
} TotkGraphicsSettings;
TotkGraphicsSettings totk_graphics_defaults(void);
int totk_graphics_valid(const TotkGraphicsSettings *s);
/* load: 1 loaded, 0 absent/defaults, -1 invalid or I/O failure (defaults retained). */
int totk_graphics_load(const char *path, TotkGraphicsSettings *out);
int totk_graphics_save(const char *path, const TotkGraphicsSettings *s);
/* Exact version/instruction guard; writes nothing on failure. Before RX sealing. */
int totk_graphics_patch_v143(uint8_t *base, size_t size, size_t file_size,
                             const TotkGraphicsSettings *s);
