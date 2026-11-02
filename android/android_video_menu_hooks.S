/* These entry shims run on the guest stack/TLS. All C callbacks go through
 * the same host-stack/TLS gate as SDK imports; guest functions stay guest. */
.text
.p2align 2
.macro load_target reg, name
    adrp \reg, :got:\name
    ldr \reg, [\reg, :got_lo12:\name]
    ldr \reg, [\reg]
.endm
.global android_menu_count_hook
.type android_menu_count_hook,%function
android_menu_count_hook:
    stp x29,x30,[sp,#-32]!
    mov x29,sp
    str x0,[sp,#16]
    load_target x16,android_menu_normalize_host
    blr x16
    ldr x0,[sp,#16]
    ldp x29,x30,[sp],#32
    load_target x16,android_menu_count_original
    br x16
.global android_menu_setup_hook
.type android_menu_setup_hook,%function
android_menu_setup_hook:
    stp x29,x30,[sp,#-48]!
    stp x19,x20,[sp,#16]
    mov x29,sp
    mov x19,x0
    mov x20,x1
    load_target x16,android_menu_setup_original
    blr x16
    ldr w8,[x19,#0x48]
    cmp w8,#16
    b.ne 1f
    mov x0,x19
    load_target x16,android_menu_get_button
    blr x16
    mov x1,x0
    mov x0,x19
    load_target x16,android_menu_register_host
    blr x16
1:  ldp x19,x20,[sp,#16]
    ldp x29,x30,[sp],#48
    ret
.global android_menu_activate_hook
.type android_menu_activate_hook,%function
android_menu_activate_hook:
    stp x29,x30,[sp,#-32]!
    mov x29,sp
    stp x0,x1,[sp,#16]
    load_target x16,android_menu_activate_host
    blr x16
    mov w16,w0
    ldp x0,x1,[sp,#16]
    ldp x29,x30,[sp],#32
    cbnz w16,2f
    load_target x16,android_menu_activate_original
    br x16
2:  ret
.section .note.GNU-stack,"",%progbits
