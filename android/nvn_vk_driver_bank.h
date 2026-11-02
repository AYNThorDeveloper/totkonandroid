/* Byte layout shared with nvn_mtl.m driver_bank_fill. */
#pragma once
#include <string.h>
#include "nvn_vk_internal.h"
static inline void nvk_driver_bank_fill(uint8_t d[0xA00], int bank, int stage, const uint32_t grid[3]) {
    memset(d, 0, 0xA00);
    if (!bank) {
        int base = stage == 0 ? 0x110 : stage == 5 ? 0x310 : stage == 1 ? 0x510 : -1;
        if (base >= 0) for (int r = 0; r < 16; r++) {
            uint64_t a = nvk_S.ssbo[stage][r].addr; uint32_t n = (uint32_t)nvk_S.ssbo[stage][r].size;
            memcpy(d + base + 16 * r, &a, 8); memcpy(d + base + 16 * r + 8, &n, 4);
        }
        if (stage == 5) {
            memcpy(d + 0x200, grid, 12);
            for (int r = 0; r < 16; r++) {
                uint64_t a = nvk_S.ubo[5][r].addr; uint32_t n = (uint32_t)nvk_S.ubo[5][r].size;
                memcpy(d + 0x210 + 16 * r, &a, 8); memcpy(d + 0x210 + 16 * r + 8, &n, 4);
            }
        } else if (!stage) { float limit = 2047.9375f; memcpy(d + 0x610, &limit, 4); }
    } else {
        for (int r = 0; r < 128; r++) {
            uint32_t t = (uint32_t)nvk_S.tex[stage][r], s = (uint32_t)nvk_S.samp[stage][r];
            memcpy(d + 0x168 + 8 * r, &t, 4); memcpy(d + 0x568 + 8 * r, &s, 4);
        }
        for (int r = 0; r < 32; r++) { uint32_t t = (uint32_t)nvk_S.tex[stage][r]; memcpy(d + 0x20 + 8 * r, &t, 4); }
    }
}
