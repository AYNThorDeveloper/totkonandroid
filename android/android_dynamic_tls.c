/* Adapt verified direct-slot rewrites to NativeActivity's dynamic ELF TLS.
 * Each recognized slot-address sequence branches to an appended TLSDESC
 * trampoline. The resolver preserves SIMD and all GPRs except x0/x1/x30;
 * those registers and NZCV are saved below the rewrite's 128-byte red zone.
 * Original files and their digest metadata remain unchanged. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#ifdef ANDROID_LOADER_LIBRARY
extern void *android_thread_slots_descriptor(void);

enum { PAGE=4096, THUNK_BYTES=88, FRAME_BYTES=192 };

static uint32_t word_at(const Module *m,size_t offset) {
    uint32_t word;memcpy(&word,m->base+offset,4);return word;
}

static int key_reference(const Module *m,size_t offset,unsigned *reg,unsigned *slot) {
    uint32_t adrp=word_at(m,offset),load=word_at(m,offset+4);
    if ((adrp&0x9f000000u)!=0x90000000u) return 0;
    unsigned r=adrp&31;
    uint64_t immediate=((adrp>>29)&3)|(((adrp>>5)&0x7ffffu)<<2);
    int64_t pages=(int64_t)(immediate^0x100000u)-0x100000;
    int64_t address=(int64_t)(offset&~(size_t)4095)+pages*4096;
    if (address!=(int64_t)m->key_off ||
        (load&0xffc003ffu)!=(0xf9400000u|(r<<5)|r)) return 0;
    unsigned index=(load>>10)&4095;
    if (index>=4) return 0;
    *reg=r;*slot=index;return 1;
}

static uint32_t branch(size_t from,size_t to) {
    int64_t delta=(int64_t)to-(int64_t)from;
    if ((delta&3)||delta<-(1LL<<27)||delta>=(1LL<<27))
        runtime_fail("dynamic TLS trampoline branch out of range");
    return 0x14000000u|((uint32_t)(delta/4)&0x3ffffffu);
}

static void emit_thunk(Module *m,size_t offset,size_t resume,unsigned base,unsigned slot) {
    uint32_t words[]={
        0xd10003ffu|(FRAME_BYTES<<10), /* sub sp,sp,#192 */
        0xa90007e0u,                  /* stp x0,x1,[sp] */
        0xa9017be2u,                  /* stp x2,x30,[sp,#16] */
        0xd53b4202u,                  /* mrs x2,nzcv */
        0xf90013e2u,                  /* str x2,[sp,#32] */
        0x58000000u|((80-20)/4<<5),   /* ldr x0, descriptor literal */
        0xf9400001u,                  /* ldr x1,[x0] */
        0xd63f0020u,                  /* blr x1: TLSDESC resolver */
        0xd53bd041u,                  /* mrs x1,tpidr_el0 */
        0x8b000020u,                  /* add x0,x1,x0 */
        0x91000000u|((slot*8)<<10),    /* add x0,x0,#field offset */
        0xf90017e0u,                  /* str x0,[sp,#40] */
        0xf94013e2u,                  /* ldr x2,[sp,#32] */
        0xd51b4202u,                  /* msr nzcv,x2 */
        0xa94007e0u,                  /* ldp x0,x1,[sp] */
        0xa9417be2u,                  /* ldp x2,x30,[sp,#16] */
        0xf94017e0u|base,             /* ldr xBase,[sp,#40] */
        0x910003ffu|(FRAME_BYTES<<10), /* add sp,sp,#192 */
        branch(offset+72,resume),
        0xd503201fu,
    };
    memcpy(m->base+offset,words,sizeof words);
    uintptr_t descriptor=(uintptr_t)android_thread_slots_descriptor();
    memcpy(m->base+offset+80,&descriptor,sizeof descriptor);
}

void android_dynamic_tls_rewrite(Module *m) {
    size_t count=0;
    for (size_t at=m->patch_off;at+12<=m->file_size;at+=4) {
        unsigned reg,slot;
        if (!key_reference(m,at,&reg,&slot)) continue;
        uint32_t previous=at>=4?word_at(m,at-4):0,add=word_at(m,at+8);
        unsigned base=previous&31;
        if ((previous&~31u)!=0xd53bd040u && (previous&~31u)!=0xd53bd060u)
            runtime_fail("dynamic TLS key reference lacks recognized thread-pointer instruction");
        if (base==18||base==31||reg==18||reg==31||base==reg||
            add!=(0x8b000000u|(reg<<16)|(base<<5)|base))
            runtime_fail("dynamic TLS key reference lacks recognized slot addition");
        ++count;
    }
    if (!count) runtime_fail("no dynamic TLS slot references found");
    size_t tail=m->size,new_size=(tail+count*THUNK_BYTES+PAGE-1)&~(size_t)(PAGE-1);
    void *mapping=mremap(m->base,m->size,new_size,MREMAP_MAYMOVE);
    if (mapping==MAP_FAILED) runtime_fail("extend module for dynamic TLS trampolines");
    m->base=mapping;m->size=new_size;
    size_t written=0;
    for (size_t at=m->patch_off;at+12<=m->file_size;at+=4) {
        unsigned reg,slot;
        if (!key_reference(m,at,&reg,&slot)) continue;
        unsigned base=word_at(m,at-4)&31;
        size_t thunk=tail+written*THUNK_BYTES;
        emit_thunk(m,thunk,at+12,base,slot);
        uint32_t replacement[]={branch(at,thunk),0xd503201fu,0xd503201fu};
        memcpy(m->base+at,replacement,sizeof replacement);
        ++written;
    }
    if (written!=count) runtime_fail("dynamic TLS rewrite count changed");
    fprintf(stderr,"[android-tls] %s: %zu thread-slot references adapted for dynamic TLS (%zu bytes)\n",
        m->name,count,count*THUNK_BYTES);
}
#else
void android_dynamic_tls_rewrite(Module *m) { (void)m; }
#endif
