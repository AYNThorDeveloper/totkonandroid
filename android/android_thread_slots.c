#include "android_thread_slots.h"

__thread AndroidThreadSlots android_thread_slots;

static uintptr_t host_thread_pointer(void) {
    uintptr_t tp;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    return tp;
}

void android_thread_slots_offsets(uint64_t out[ANDROID_KEY_COUNT]) {
    uintptr_t tp = host_thread_pointer();
    out[ANDROID_KEY_K18] = (uintptr_t)&android_thread_slots.guest_x18 - tp;
    out[ANDROID_KEY_KTLS] = (uintptr_t)&android_thread_slots.guest_tpidr_el0 - tp;
    out[ANDROID_KEY_KTLR] = (uintptr_t)&android_thread_slots.guest_tpidrro_el0 - tp;
    out[ANDROID_KEY_KGATE] = (uintptr_t)&android_thread_slots.host_gate - tp;
}

void android_thread_slots_bind(uintptr_t x18, uintptr_t guest_tls,
                              uintptr_t guest_tlr, uintptr_t gate) {
    android_thread_slots.guest_x18 = x18;
    android_thread_slots.guest_tpidr_el0 = guest_tls;
    android_thread_slots.guest_tpidrro_el0 = guest_tlr;
    android_thread_slots.host_gate = gate;
}

void android_module_key_page(uint64_t *page, uintptr_t gate,
                             uint32_t handheld_resolution,
                             uint32_t docked_resolution,
                             uint32_t aspect_bits,
                             uint32_t ui_aspect_bits) {
    uint64_t offsets[ANDROID_KEY_COUNT] = {0};
    android_thread_slots_offsets(offsets);
    for (unsigned i = 0; i < 4; ++i) page[i] = offsets[i];
    page[ANDROID_KEY_KRES0] = handheld_resolution;
    page[ANDROID_KEY_KRES1] = docked_resolution;
    page[ANDROID_KEY_KASPECT] = aspect_bits;
    page[ANDROID_KEY_KASPECTUI] = ui_aspect_bits;
    android_thread_slots.host_gate = gate;
}
