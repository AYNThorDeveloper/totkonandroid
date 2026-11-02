/* Exercise the production patch emitter and measured-step state, with no GPU. */
#include <assert.h>
#include <math.h>
#include <setjmp.h>
#include "../android_frame_step.c"
_Noreturn void runtime_fail(const char *message) { fprintf(stderr, "%s\n", message); exit(2); }
static float seconds(void) {
    uint64_t bits = atomic_load(frame_step_data); float values[2];
    memcpy(values, &bits, sizeof values); return values[1];
}
static void rate(uint64_t *now, unsigned hz) {
    for (unsigned i = 0; i < 20; i++) { *now += 1000000000ull / hz; runtime_frame_step_update(*now); }
    assert(fabs(seconds() - 1.0 / hz) < 1e-6);
    assert(fabs(seconds() * hz - 1.0) < 2e-5); /* one wall second => one game second */
}
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    const size_t size = 0x430000, text_end = 0x400000, ro = 0x404000, key = 0x410000;
    uint8_t *base = calloc(1, size); assert(base);
    const Patch guards[] = {{0x3e5658,0x1e210808,0}, {0x2daa54,0xbd425260,0}, {0x2daa58,0x52a84e08,0},
        {0x2daa6c,0x5e21d800,0}, {0x2daa70,0x1e201820,0}, {0x2daa74,0x1e2c1001,0}, {0x2daa78,0x1e210800,0}};
    for (unsigned i=0;i<sizeof guards/sizeof *guards;i++) memcpy(base+guards[i].off,&guards[i].original,4);
    for (unsigned i=0;i<sizeof fps60/sizeof *fps60;i++) memcpy(base+fps60[i].off,&fps60[i].original,4);
    assert(android_frame_step_patch(base,text_end,ro,key));
    for (unsigned i=0;i<sizeof fps60/sizeof *fps60;i++) assert(word_at(base,fps60[i].off)==fps60[i].replacement);
    uint64_t now=1000000000; runtime_frame_step_update(now);
    rate(&now,60); rate(&now,30); rate(&now,40); rate(&now,120); rate(&now,30);
    now += 3000000000ull; runtime_frame_step_update(now); /* shader stall: no giant physics step */
    assert(seconds() < 0.067f); rate(&now,30);
    runtime_frame_step_update(now-1); rate(&now,60); /* reset/backward clock */
    runtime_frame_step_pin(33.333); rate(&now,30); runtime_frame_step_pin(0); rate(&now,60);
    uint64_t period=android_frame_pacing_period();
    const char *cap=getenv("TOTK_FRAME_CAP");
    assert(period==(cap&&atoi(cap)?1000000000ull/(unsigned)atoi(cap):0));
    free(base); puts("frame step: pass (30/40/60/120 Hz, transitions, stalls, pin, controller patch)");
}
