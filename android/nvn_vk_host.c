/* Host services the shared NVN layer (d2r_gfx hle_nvn.c and helpers) expects
 * from the platform runtime. The macOS port provides them in
 * games/totk/runtime/gfx_bridge.c and loader.c; this is the Android side.
 * Diagnostics that rely on Darwin-only facilities report "unavailable". */
#define _GNU_SOURCE
#include <asm/sigcontext.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

extern _Noreturn void runtime_fail(const char *message);
extern uint64_t runtime_now_ns(void);
extern void android_trace_guest_pc(uint64_t pc);
extern void runtime_trace_guest(uint64_t fp, uint64_t lr);

/* ---------- ELF registration sections (Mach-O getsectiondata) ---------- */
const char android_no_mach_header;
typedef struct { const char *name; void *fn; } NamedEntry;
typedef struct { const char *name; void *fn; const char *why; } ForceEntry;
extern const NamedEntry __start_swnvn[] __attribute__((weak)), __stop_swnvn[] __attribute__((weak));
extern const NamedEntry __start_totknvnx[] __attribute__((weak)), __stop_totknvnx[] __attribute__((weak));
extern const ForceEntry __start_totkgfx[] __attribute__((weak)), __stop_totkgfx[] __attribute__((weak));

const void *android_getsectiondata(const char *section, unsigned long *size) {
    const char *lo = NULL, *hi = NULL;
    if (!strcmp(section, "swnvn")) { lo = (const char *)__start_swnvn; hi = (const char *)__stop_swnvn; }
    else if (!strcmp(section, "totknvnx")) { lo = (const char *)__start_totknvnx; hi = (const char *)__stop_totknvnx; }
    else if (!strcmp(section, "totkgfx")) { lo = (const char *)__start_totkgfx; hi = (const char *)__stop_totkgfx; }
    else runtime_fail("unknown registration section requested");
    *size = lo && hi ? (unsigned long)(hi - lo) : 0;
    return *size ? lo : NULL;
}

/* HLE_FORCE entries of the shared graphics layer (nvnBootstrapLoader). */
void *android_nvn_force_override(const char *name) {
    for (const ForceEntry *e = __start_totkgfx; e && e < __stop_totkgfx; ++e)
        if (!strcmp(e->name, name)) return e->fn;
    return NULL;
}

/* ---------- Time ---------- */
uint64_t g_t0_ns;
uint64_t now_ns(void) { return runtime_now_ns(); }
uint64_t guest_ns(void) { return runtime_now_ns(); }
int dm_detclock_enabled(void) { return 0; }
void dm_guest_frame_tick(void) {}

/* ---------- Guest-write and fault hooks (dirty-page tracking) ---------- */
static int (*fault_hook)(void *, int, void *);
static void (*prepare_hook)(void *, size_t);
extern uint64_t dm_host_write_pc;   /* nvn_dirty.c: writer of a host write (DM_NVN_TILELOG) */
void dm_fault_hook_register(int (*fn)(void *, int, void *)) {
    if (fault_hook) runtime_fail("second gfx fault hook");
    fault_hook = fn;
}
void dm_guest_write_prep_register(void (*fn)(void *, size_t)) {
    if (prepare_hook) runtime_fail("second gfx prepare hook");
    prepare_hook = fn;
}
void dm_res_report_register(void (*fn)(uint32_t, const char *)) { (void)fn; }
void dm_guest_write_prepare(void *addr, size_t n) {
    dm_host_write_pc = (uint64_t)__builtin_return_address(0);
    if (prepare_hook) prepare_hook(addr, n);
}
void crash_set_wait_dump(void (*fn)(void)) { (void)fn; }

__thread sigjmp_buf *runtime_fault_guard;
static struct sigaction previous_segv, previous_bus;

/* Linux AArch64 stores the fault's ESR in a record of uc_mcontext.__reserved;
 * bit 6 (WnR) of a data abort says whether the access was a write. */
static int fault_is_write(const ucontext_t *uc) {
    const uint8_t *p = (const uint8_t *)uc->uc_mcontext.__reserved;
    const uint8_t *end = p + sizeof uc->uc_mcontext.__reserved;
    while (p + sizeof(struct _aarch64_ctx) <= end) {
        const struct _aarch64_ctx *h = (const void *)p;
        if (!h->magic || h->size < sizeof *h) break;
        if (h->magic == ESR_MAGIC) return (int)((((const struct esr_context *)h)->esr >> 6) & 1);
        p += h->size;
    }
    return 1; /* unknown: a write fault is the only kind the dirty tracker resolves */
}

static void chain(int num, siginfo_t *info, void *context, const struct sigaction *old) {
    if (old->sa_flags & SA_SIGINFO) { if (old->sa_sigaction) { old->sa_sigaction(num, info, context); return; } }
    else if (old->sa_handler != SIG_DFL && old->sa_handler != SIG_IGN) { old->sa_handler(num); return; }
    /* Default action: restore it and return, so the fault repeats and the
       platform crash reporter records the original state. */
    signal(num, SIG_DFL);
}

static void gfx_fault(int num, siginfo_t *info, void *context) {
    if (runtime_fault_guard) { sigjmp_buf *g = runtime_fault_guard; runtime_fault_guard = NULL; siglongjmp(*g, 1); }
    if (fault_hook && fault_hook(info->si_addr, fault_is_write(context), context)) return;
    static _Atomic int reported;
    if (!reported++) {   /* name the guest code before the platform crash report */
        const ucontext_t *uc = context;
        fprintf(stderr, "[android-runtime] fatal signal %d at %p, pc=%#llx\n", num, info->si_addr, (unsigned long long)uc->uc_mcontext.pc);
        android_trace_guest_pc(uc->uc_mcontext.pc);
        runtime_trace_guest(uc->uc_mcontext.regs[29], uc->uc_mcontext.regs[30]);
    }
    chain(num, info, context, num == SIGBUS ? &previous_bus : &previous_segv);
}

void android_nvn_host_install(void) {
    static int installed;
    if (installed) return;
    installed = 1;
    g_t0_ns = runtime_now_ns();
    struct sigaction a = {0};
    a.sa_sigaction = gfx_fault; a.sa_flags = SA_SIGINFO | SA_ONSTACK; sigemptyset(&a.sa_mask);
    if (sigaction(SIGSEGV, &a, &previous_segv) || sigaction(SIGBUS, &a, &previous_bus))
        runtime_fail("install Android graphics fault handler");
}

/* ---------- Diagnostics without an Android equivalent ---------- */
void runtime_watch_set(uintptr_t addr, size_t len) {
    fprintf(stderr, "[android-nvn] write watch %#lx+%zu unavailable on Android\n", (unsigned long)addr, len);
}
void runtime_watch_tick(uint64_t frame) { (void)frame; }
_Atomic int runtime_survey_pending;
void runtime_survey_tick(uint64_t frame) { (void)frame; }
int thread_selfcounts(int type, void *buf, size_t nbytes) { (void)type; memset(buf, 0, nbytes); return -1; }
uint64_t g_draw_skip[16];
/* MetalFX frame interpolation (totk_mfx.m) does not exist on Android: off. */
int mfx_enabled(void) { return 0; }
int mfx_latch(void) { return 0; }
void mfx_set(int mode) { (void)mode; }
