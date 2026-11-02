/* AArch64 Android host-call thunks. x0..x8 and v0..v7 pass arguments; x18 is
 * Android's platform register and is never touched. The guest thread owns a
 * host-stack slot; zero means this call is already on a host stack. */
.text
#ifdef ANDROID_LOADER_LIBRARY
    .globl android_thread_slots_descriptor
    .p2align 2
android_thread_slots_descriptor:
    adrp x0, :tlsdesc:android_thread_slots
    add x0, x0, #:tlsdesc_lo12:android_thread_slots
    ret
#endif

    .p2align 2
android_hle_switch:
#ifdef ANDROID_LOADER_LIBRARY
    stp x0, x1, [sp, #-32]!
    str x30, [sp, #16]
    adrp x0, :tlsdesc:android_thread_slots
    ldr x1, [x0, #:tlsdesc_lo12:android_thread_slots]
    add x0, x0, #:tlsdesc_lo12:android_thread_slots
    .tlsdesccall android_thread_slots
    blr x1
    mrs x9, tpidr_el0
    add x10, x9, x0
    ldr x30, [sp, #16]
    ldp x0, x1, [sp], #32
#else
    mrs x9, tpidr_el0
    add x10, x9, :tprel_hi12:android_thread_slots
    add x10, x10, :tprel_lo12_nc:android_thread_slots
#endif
    add x10, x10, #32
    ldr x11, [x10]
    cbz x11, 1f
    str xzr, [x10]
    mov x12, sp
    sub sp, x11, #32
    stp x29, x30, [sp]
    stp x12, x11, [sp, #16]
    mov x29, sp
    blr x16
    ldp x12, x11, [sp, #16]
    ldp x29, x30, [sp]
    mov sp, x12
#ifdef ANDROID_LOADER_LIBRARY
    stp x0, x1, [sp, #-32]!
    str x30, [sp, #16]
    adrp x0, :tlsdesc:android_thread_slots
    ldr x1, [x0, #:tlsdesc_lo12:android_thread_slots]
    add x0, x0, #:tlsdesc_lo12:android_thread_slots
    .tlsdesccall android_thread_slots
    blr x1
    mrs x9, tpidr_el0
    add x10, x9, x0
    ldr x30, [sp, #16]
    ldp x0, x1, [sp], #32
#else
    mrs x9, tpidr_el0
    add x10, x9, :tprel_hi12:android_thread_slots
    add x10, x10, :tprel_lo12_nc:android_thread_slots
#endif
    add x10, x10, #32
    str x11, [x10]
    ret
1:  br x16

    .p2align 2
android_hle_dispatch:
#ifdef ANDROID_LOADER_LIBRARY
    adrp x16, :got:android_hle_targets
    ldr x16, [x16, :got_lo12:android_hle_targets]
#else
    adrp x16, android_hle_targets
    add x16, x16, :lo12:android_hle_targets
#endif
    ldr x16, [x16, x17, lsl #3]
    b android_hle_switch

    .globl android_hle_thunks
    .p2align 4
android_hle_thunks:
.set hle_i, 0
.rept 4096
    mov x17, #hle_i
    b android_hle_dispatch
    brk #0xb5
    brk #0xb5
.set hle_i, hle_i+1
.endr

/* Probe helper: call a wrapped host function from a guarded guest stack. */
    .globl android_call_hle_on_guest_stack
    .p2align 2
android_call_hle_on_guest_stack:
    stp x19, x20, [sp, #-32]!
    str x30, [sp, #16]
    mov x19, sp
    mov x20, x2
    mov sp, x1
    blr x20
    mov sp, x19
    ldr x30, [sp, #16]
    ldp x19, x20, [sp], #32
    ret
