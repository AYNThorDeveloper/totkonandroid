/* Test the Android host-call gate on a guarded 4 KiB guest stack. */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include "android_thread_slots.h"

#if !defined(__ANDROID__) || !defined(__aarch64__)
#error Android AArch64 required
#endif

static uintptr_t host_low, host_high;
static int depth, dispatches, nested_ok, hle_calls, hle_nested;

extern uintptr_t android_guest_gate_call(uintptr_t value);
extern uintptr_t android_call_on_guest_stack(uintptr_t value, uintptr_t stack_top);
extern uintptr_t android_call_hle_on_guest_stack(uintptr_t value, uintptr_t stack_top,
                                                 void *function);
extern void *runtime_hle_wrap_named(void *function, const char *name);

_Noreturn void runtime_fail(const char *message) {
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
}

static void need(int condition, const char *message) {
    if (!condition) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}

static uintptr_t nested_host_function(uintptr_t value) {
    need(android_thread_slots.host_stack_top == 0, "nested host call stack slot");
    ++hle_nested;
    return value + 1;
}

static uintptr_t large_host_function(uintptr_t value) {
    volatile char frame[8192];
    uintptr_t here = (uintptr_t)&frame[0];
    need(here > host_low && here + sizeof frame < host_high,
         "wrapped host call did not switch to host stack");
    need(android_thread_slots.host_stack_top == 0, "wrapped host stack slot");
    frame[0] = 1;
    frame[sizeof frame - 1] = 2;
    ++hle_calls;
    uintptr_t (*nested)(uintptr_t) = runtime_hle_wrap_named((void *)nested_host_function, "nested");
    return nested(value) + frame[0] + frame[sizeof frame - 1];
}

void android_gate_dispatch(uintptr_t return_address, uint64_t *registers) {
    uintptr_t here = (uintptr_t)&return_address;
    need(here > host_low && here < host_high, "dispatcher did not switch to host stack");
    need(android_thread_slots.host_stack_top == 0, "host stack slot not cleared during dispatch");
    need(return_address != 0 && registers[19] == 123, "gate register frame");
    ++dispatches;
    if (depth == 0) {
        depth = 1;
        nested_ok = android_guest_gate_call(3) == 15;
        depth = 0;
    }
    registers[0] += 5;
    registers[19] = 7;
}

int main(void) {
    long page = sysconf(_SC_PAGESIZE);
    need(page == 4096, "this Thor probe expects 4 KiB pages");
    size_t host_size = 64 * 1024;
    void *host = mmap(NULL, host_size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    need(host != MAP_FAILED, "host stack allocation");
    host_low = (uintptr_t)host;
    host_high = host_low + host_size;
    void *guest = mmap(NULL, (size_t)page * 2, PROT_NONE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    need(guest != MAP_FAILED, "guest stack allocation");
    need(mprotect((char *)guest + page, (size_t)page,
                  PROT_READ | PROT_WRITE) == 0, "guest stack guard");
    uintptr_t guest_top = (uintptr_t)guest + (uintptr_t)page * 2;
    android_thread_slots.host_stack_top = host_high;
    uintptr_t result = android_call_on_guest_stack(10, guest_top);
    need(result == 22, "dispatcher changes to x0 and x19");
    need(dispatches == 2 && nested_ok, "nested host-to-guest-to-host call");
    need(android_thread_slots.host_stack_top == host_high, "host stack slot not restored");
    void *wrapped = runtime_hle_wrap_named((void *)large_host_function, "large");
    need(wrapped == runtime_hle_wrap_named((void *)large_host_function, "large"),
         "stable host-call wrapper pointer");
    uintptr_t hle_result = android_call_hle_on_guest_stack(10, guest_top, wrapped);
    need(hle_result == 14 && hle_calls == 1 && hle_nested == 1,
         "wrapped host call and nested call result");
    need(android_thread_slots.host_stack_top == host_high,
         "host stack slot restored after wrapped call");
    printf("{\"status\":\"pass\",\"scope\":\"Android host-call gate, separate guarded guest stack\","
           "\"guest_stack_bytes\":%ld,\"host_stack_bytes\":%zu,\"dispatches\":%d,"
           "\"nested_call\":true,\"wrapped_host_call\":true,\"result\":%" PRIuPTR "}\n",
           page, host_size, dispatches, result);
    munmap(guest, (size_t)page * 2);
    munmap(host, host_size);
    return 0;
}
