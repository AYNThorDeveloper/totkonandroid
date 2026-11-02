/* TOTK 1.4.3 real-time frame step and 60 FPS frame controller on Android:
 * the macOS port's patch (games/totk/runtime/game_patches_v143.c,
 * patch_realtime and fps60) applied to the Android rewrite of main. Game time
 * then follows the measured present interval instead of fixed 1/30 s steps,
 * so the game keeps its real speed at any frame rate. The frame-step data
 * lives in main's writable key page (its first 64 bytes are loader keys);
 * the 16-byte veneer sits in the zero padding after the text, as on macOS.
 * Every patched site is checked against its original instruction first. */
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

extern _Noreturn void runtime_fail(const char *message);

#define FRAME_DATA_OFFSET 0x2000   /* within the 16 KiB key page, 4 KiB aligned for ADRP */

typedef struct { uint64_t off; uint32_t original, replacement; } Patch;
static const Patch fps60[] = {
    {0x2db204, 0x6b08015f, 0x7100055f}, {0x2db208, 0x1a89b10a, 0x1a9fa52a}, {0x2db22c, 0xb940468a, 0xd503201f},
    {0x2db360, 0x6b0801bf, 0x710005bf}, {0x2db36c, 0x1a88c1ac, 0x1a9fc5ac}, {0x2db340, 0xb9400288, 0x52800028},
    {0x2db34c, 0x2a0a03f5, 0x1a9fa555},
};

static _Atomic uint64_t *frame_step_data;
static _Atomic uint32_t *frame_interval_data;
static _Atomic uint32_t pinned_step_us;
static uint64_t last_present_ns;
static double frame_samples[5];
static unsigned frame_sample_count, frame_sample_next;

/* A host FPS limit is independent of the guest's 60-FPS controller. Turning
 * that controller off is not a host cap and can change the engine's step.
 * Used by hle_nvn's existing acquire pacing, never a second present sleep. */
static _Atomic unsigned frame_cap_override = UINT32_MAX;
int android_frame_cap_allowed(unsigned cap) {
    if (cap != 0 && cap != 30 && cap != 40 && cap != 60 && cap != 120) return 0;
    const char *rt = getenv("TOTK_REALTIME"), *fps = getenv("TOTK_FPS60");
    return !cap || !((rt && !strcmp(rt, "0")) || (fps && !strcmp(fps, "0")));
}
int android_frame_cap_set(unsigned cap) {
    if (!android_frame_cap_allowed(cap)) return 0;
    atomic_store(&frame_cap_override, cap); return 1;
}
uint64_t android_frame_pacing_period(void) {
    unsigned override = atomic_load(&frame_cap_override);
    if (override != UINT32_MAX) return override ? 1000000000ull / override : 0;
    static int initialized, cap;
    if (!initialized) {
        const char *e = getenv("TOTK_FRAME_CAP");
        if (e && *e) {
            char *end; errno = 0; long n = strtol(e, &end, 10);
            if (errno || *end || (n != 0 && (n < 30 || n > 120)))
                runtime_fail("TOTK_FRAME_CAP must be 0 (existing pacing) or 30..120");
            cap = (int)n;
        }
        if (cap) {
            const char *rt = getenv("TOTK_REALTIME"), *fps = getenv("TOTK_FPS60");
            if ((rt && !strcmp(rt, "0")) || (fps && !strcmp(fps, "0")))
                runtime_fail("TOTK_FRAME_CAP requires TOTK_REALTIME=1 and TOTK_FPS60=1");
            fprintf(stderr, "[android-timing] host cap %d FPS; real-time step and 60 FPS controller retained\n", cap);
        }
        initialized = 1;
    }
    return cap ? 1000000000ull / (unsigned)cap : 0;
}

static void frame_step_store(float seconds) {
    float values[3] = {seconds * 30.0f, seconds, seconds * 60.0f}; uint64_t bits; uint32_t interval;
    memcpy(&bits, values, 8); memcpy(&interval, values + 2, 4);
    atomic_store_explicit(frame_step_data, bits, memory_order_release);
    atomic_store_explicit(frame_interval_data, interval, memory_order_release);
}
void runtime_frame_step_pin(double ms) {
    uint32_t us = ms > 0 ? (uint32_t)(ms * 1000.0 + 0.5) : 0; atomic_store(&pinned_step_us, us);
    if (frame_step_data && us) frame_step_store((float)(us / 1e6));
    fprintf(stderr, "[android-timing] frame step %s, pin %.3f ms\n", us ? "pinned" : "real-time", us / 1000.0);
}
/* Called at every game present (hle_nvn.c nvnQueuePresentTexture). */
void runtime_frame_step_update(uint64_t now_ns) {
    if (!frame_step_data || !now_ns) return;
    if (atomic_load_explicit(&pinned_step_us, memory_order_relaxed)) { last_present_ns = now_ns; return; }
    if (!last_present_ns) { last_present_ns = now_ns; return; }
    if (now_ns <= last_present_ns) { last_present_ns = now_ns; frame_sample_count = frame_sample_next = 0; return; }
    double dt = (double)(now_ns - last_present_ns) * 1e-9; last_present_ns = now_ns;
    if (dt > 0.25 || dt < 0.001) { frame_sample_count = frame_sample_next = 0; return; }
    if (dt < 1.0 / 120.0) dt = 1.0 / 120.0;
    if (dt > 1.0 / 15.0) dt = 1.0 / 15.0;
    frame_samples[frame_sample_next++ % 5] = dt; if (frame_sample_count < 5) frame_sample_count++;
    double sum = 0; for (unsigned i = 0; i < frame_sample_count; i++) sum += frame_samples[i];
    frame_step_store((float)(sum / frame_sample_count));
    static unsigned logged_frames;
    if (++logged_frames % 120 == 0)
        fprintf(stderr, "[android-timing] present %.3f ms; game step %.3f ms (ratio30 %.4f); %u samples\n",
            dt * 1000.0, sum / frame_sample_count * 1000.0, sum / frame_sample_count * 30.0, frame_sample_count);
}

static uint32_t adrp(unsigned reg, uint64_t pc, uint64_t target) {
    int64_t pages = (int64_t)(target >> 12) - (int64_t)(pc >> 12);
    if (pages < -(1 << 20) || pages >= (1 << 20)) runtime_fail("1.4.3 frame-step data outside ADRP range");
    uint32_t imm = (uint32_t)pages & 0x1fffff;
    return 0x90000000 | ((imm & 3) << 29) | ((imm >> 2) << 5) | reg;
}
static uint32_t branch(uint64_t pc, uint64_t target) {
    int64_t delta = (int64_t)target - (int64_t)pc;
    if ((delta & 3) || delta < -(1 << 27) || delta >= (1 << 27)) runtime_fail("1.4.3 frame-step branch outside range");
    return 0x14000000 | (((uint32_t)(delta >> 2)) & 0x3ffffff);
}
static uint32_t word_at(const uint8_t *base, uint64_t off) { uint32_t w; memcpy(&w, base + off, 4); return w; }

/* base: main's mapping; text_end / ro_off: end of code and start of read-only
 * data; key_off: main's key page (writable). Returns 1 when applied. */
int android_frame_step_patch(uint8_t *base, uint64_t text_end, uint64_t ro_off, uint64_t key_off) {
    const char *enabled = getenv("TOTK_REALTIME");
    if (enabled && !strcmp(enabled, "0")) { fprintf(stderr, "[android-loader] real-time frame step off (TOTK_REALTIME=0)\n"); return 0; }
    static const Patch guards[] = {
        {0x3e5658, 0x1e210808, 0}, {0x2daa54, 0xbd425260, 0}, {0x2daa58, 0x52a84e08, 0},
        {0x2daa6c, 0x5e21d800, 0}, {0x2daa70, 0x1e201820, 0}, {0x2daa74, 0x1e2c1001, 0}, {0x2daa78, 0x1e210800, 0}};
    for (unsigned i = 0; i < sizeof guards / sizeof *guards; i++) if (word_at(base, guards[i].off) != guards[i].original) {
        fprintf(stderr, "[android-loader] frame-step site %#llx holds %#x, expected %#x\n", (unsigned long long)guards[i].off,
            word_at(base, guards[i].off), guards[i].original);
        runtime_fail("1.4.3 real-time original instruction mismatch");
    }
    for (unsigned i = 0; i < sizeof fps60 / sizeof *fps60; i++) if (word_at(base, fps60[i].off) != fps60[i].original)
        runtime_fail("1.4.3 FPS patch original instruction mismatch");
    uint64_t stub = (text_end + 15) & ~15ULL, data = key_off + FRAME_DATA_OFFSET;
    if (stub + 16 > ro_off) runtime_fail("1.4.3 frame-step veneer does not fit after the text");
    for (unsigned i = 0; i < 16; i++) if (base[stub + i]) runtime_fail("1.4.3 frame-step veneer padding not empty");
    for (unsigned i = 0; i < 16; i++) if (base[data + i]) runtime_fail("1.4.3 frame-step data slot in the key page not empty");
    /* Replace the nominal 60/FPS interval before the original producer applies its
       0.5/1.0 selector; x8 is free here, s0 is dead until the call sites reload it. */
    uint32_t veneer[] = {adrp(8, (uintptr_t)base + stub, (uintptr_t)base + data), 0xbd400900, 0x1e210808, branch(stub + 12, 0x3e565c)};
    memcpy(base + stub, veneer, sizeof veneer);
    uint32_t words[] = {branch(0x3e5658, stub), adrp(9, (uintptr_t)base + 0x2daa54, (uintptr_t)base + data), 0xbd400120,
        0xd503201f, 0xd503201f, 0xd503201f, 0xd503201f};
    for (unsigned i = 0; i < sizeof guards / sizeof *guards; i++) memcpy(base + guards[i].off, &words[i], 4);
    frame_step_data = (_Atomic uint64_t *)(void *)(base + data);
    frame_interval_data = (_Atomic uint32_t *)(void *)(base + data + 8);
    uint64_t initial_period = android_frame_pacing_period();
    frame_step_store(initial_period ? (float)(initial_period * 1e-9) : 1.0f / 60.0f);
    const char *f60 = getenv("TOTK_FPS60");
    if (!(f60 && !strcmp(f60, "0"))) for (unsigned i = 0; i < sizeof fps60 / sizeof *fps60; i++) memcpy(base + fps60[i].off, &fps60[i].replacement, 4);
    fprintf(stderr, "[android-loader] 1.4.3 real-time frame step (7 sites, veneer +%#llx, data key+%#x)%s\n",
        (unsigned long long)stub, FRAME_DATA_OFFSET, f60 && !strcmp(f60, "0") ? "" : " and 60 FPS frame controller (7 sites)");
    return 1;
}
