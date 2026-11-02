#define _GNU_SOURCE
#include "runtime.h"
#include "android_thread_slots.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

pthread_key_t runtime_keys[8];
static pthread_key_t environment_key;
static pthread_once_t keys_once=PTHREAD_ONCE_INIT;

typedef struct { void *tls,*tlr,*dummy; } AndroidThreadEnvironment;

extern void android_gate_probe(void);

static void environment_destroy(void *value) {
    AndroidThreadEnvironment *e=value;
    if(!e)return;
    free(e->dummy);free(e->tlr);free(e->tls);free(e);
}

static void keys_init(void) {
    for(unsigned i=0;i<8;i++)if(pthread_key_create(&runtime_keys[i],NULL))runtime_fail("create Android runtime TLS key");
    if(pthread_key_create(&environment_key,environment_destroy))runtime_fail("create Android thread environment key");
}

void runtime_thread_init(void) {
    if(pthread_once(&keys_once,keys_init))runtime_fail("initialize Android thread TLS keys");
    if(pthread_getspecific(environment_key))runtime_fail("duplicate Android guest thread environment");
    AndroidThreadEnvironment *e=calloc(1,sizeof(*e));
    if(!e)runtime_fail("allocate Android guest thread environment");
    e->tls=calloc(1,8192);e->tlr=calloc(1,0x200);e->dummy=calloc(1,0x400);
    if(!e->tls||!e->tlr||!e->dummy)runtime_fail("allocate Android guest TLS blocks");
    memcpy((uint8_t *)e->tlr+0x1f8,&e->dummy,sizeof(e->dummy));
    if(pthread_setspecific(environment_key,e)||
       pthread_setspecific(runtime_keys[0],NULL)||
       pthread_setspecific(runtime_keys[1],e->tls)||
       pthread_setspecific(runtime_keys[2],e->tlr)||
       pthread_setspecific(runtime_keys[3],(void *)android_gate_probe))
        runtime_fail("bind Android guest TLS keys");
    android_thread_slots_bind(0,(uintptr_t)e->tls,(uintptr_t)e->tlr,(uintptr_t)android_gate_probe);
#ifdef ANDROID_LOADER_LIBRARY
    uint64_t offsets[ANDROID_KEY_COUNT]={0};
    android_thread_slots_offsets(offsets);
    fprintf(stderr,"[android-tls] thread guest-slot offsets K18=%#llx KTLS=%#llx KTLR=%#llx KGATE=%#llx\n",
        (unsigned long long)offsets[0],(unsigned long long)offsets[1],
        (unsigned long long)offsets[2],(unsigned long long)offsets[3]);
#endif
}

void runtime_hle_stack_set(uintptr_t top) {
    android_thread_slots.host_stack_top=top&~(uintptr_t)15;
}
