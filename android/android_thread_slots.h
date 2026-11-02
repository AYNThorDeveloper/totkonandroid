#pragma once
#include <stdint.h>

enum {
    ANDROID_KEY_K18 = 0,
    ANDROID_KEY_KTLS = 1,
    ANDROID_KEY_KTLR = 2,
    ANDROID_KEY_KGATE = 3,
    ANDROID_KEY_KRES0 = 4,
    ANDROID_KEY_KRES1 = 5,
    ANDROID_KEY_KASPECT = 6,
    ANDROID_KEY_KASPECTUI = 7,
    ANDROID_KEY_COUNT = 8
};

typedef struct {
    uintptr_t guest_x18;
    uintptr_t guest_tpidr_el0;
    uintptr_t guest_tpidrro_el0;
    uintptr_t host_gate;
    uintptr_t host_stack_top;
} AndroidThreadSlots;

extern __thread AndroidThreadSlots android_thread_slots;

/* The existing NSO rewrite addresses these per-thread fields from TPIDR_EL0. */
void android_thread_slots_offsets(uint64_t out[ANDROID_KEY_COUNT]);
void android_thread_slots_bind(uintptr_t x18, uintptr_t guest_tls,
                              uintptr_t guest_tlr, uintptr_t gate);
void android_module_key_page(uint64_t *page, uintptr_t gate,
                             uint32_t handheld_resolution,
                             uint32_t docked_resolution,
                             uint32_t aspect_bits,
                             uint32_t ui_aspect_bits);
