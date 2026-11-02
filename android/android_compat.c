#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <android/log.h>
#include <mach/thread_policy.h>

vm_size_t vm_page_size=4096;

/* ---------- Real-time requests (TOTK_ANDROID_RT=1, off by default) ----------
 * The shared NVN layer declares its frame threads (acquire/replay, translation) as Mach time-constraint threads; on the
 * Mac that kept the game's ~50 worker threads from preempting the replay (replay 6.6 -> 13 ms without it). On Android
 * that request always failed. With the switch: the thread gets nice TOTK_ANDROID_RT_NICE (default -10; apps may lower
 * their own threads' nice) and joins an Android performance hint session (API 33, looked up at run time) whose target
 * is half the frame period, so the reported per-frame CPU time (android_thread_work_report) asks for higher clocks. */
typedef void *(*hint_manager_fn)(void);
typedef void *(*hint_create_fn)(void *, const int32_t *, size_t, int64_t);
typedef int (*hint_update_fn)(void *, int64_t);
typedef int (*hint_report_fn)(void *, int64_t);
typedef void (*hint_close_fn)(void *);
static struct { int loaded; hint_manager_fn manager; hint_create_fn create; hint_update_fn update; hint_report_fn report; hint_close_fn close; } hint;
static pthread_mutex_t rt_lock=PTHREAD_MUTEX_INITIALIZER;
static void *rt_session; static int32_t rt_tids[8]; static size_t rt_ntids; static int64_t rt_target;
static int rt_on(void) {
    static int on=-1;
    if(on<0){const char *e=getenv("TOTK_ANDROID_RT");on=e&&!strcmp(e,"1");}
    return on;
}
static void hint_load(void) {
    if(hint.loaded)return;
    hint.loaded=1;
    void *lib=dlopen("libandroid.so",RTLD_NOW);
    if(!lib)return;
    hint.manager=(hint_manager_fn)dlsym(lib,"APerformanceHint_getManager");
    hint.create=(hint_create_fn)dlsym(lib,"APerformanceHint_createSession");
    hint.update=(hint_update_fn)dlsym(lib,"APerformanceHint_updateTargetWorkDuration");
    hint.report=(hint_report_fn)dlsym(lib,"APerformanceHint_reportActualWorkDuration");
    hint.close=(hint_close_fn)dlsym(lib,"APerformanceHint_closeSession");
}
kern_return_t android_thread_time_constraint(uint32_t period_ns,uint32_t computation_ns) {
    (void)computation_ns;   /* Mach's "computation" means something else for a hint session: the target is half the period */
    { static __thread int pinned; if(!pinned){pinned=1;android_thread_affinity("TOTK_AFFINITY_RENDER");} }
    if(!rt_on())return KERN_FAILURE;
    int32_t tid=(int32_t)gettid();
    const char *e=getenv("TOTK_ANDROID_RT_NICE");
    int nice=e&&*e?atoi(e):-10, pr=setpriority(PRIO_PROCESS,(id_t)tid,nice), perr=pr?errno:0;
    int64_t target=(int64_t)period_ns/2;
    pthread_mutex_lock(&rt_lock);
    hint_load();
    size_t i=0; while(i<rt_ntids&&rt_tids[i]!=tid)++i;
    int joined=i<rt_ntids;
    if(!joined&&rt_ntids<8)rt_tids[rt_ntids++]=tid;
    int hinted=0;
    if(hint.manager&&hint.create&&target>0){
        if(rt_session&&!joined&&hint.close){hint.close(rt_session);rt_session=NULL;}   /* a session's threads are fixed (API 33) */
        if(!rt_session){void *m=hint.manager();rt_session=m?hint.create(m,rt_tids,rt_ntids,target):NULL;rt_target=target;}
        else if(target!=rt_target&&hint.update){hint.update(rt_session,target);rt_target=target;}
        hinted=rt_session!=NULL;
    }
    pthread_mutex_unlock(&rt_lock);
    __android_log_print(ANDROID_LOG_INFO,"TOTKGameLoader","[android-rt] thread %d: nice %d %s%s; performance hint %s (target %.2f ms, %zu threads)",
        tid,nice,pr?"refused: ":"set",pr?strerror(perr):"",hinted?"on":hint.create?"refused":"unavailable (API < 33)",target/1e6,rt_ntids);
    return pr&&!hinted?KERN_FAILURE:KERN_SUCCESS;
}
/* Pinning (TOTK_AFFINITY_TRANSLATE, _RECORD, _RENDER; all off by default). The 25 Oct Thor profile of the translation thread showed
 * half its time on the prime core (cpu7, 3.19 GHz) and the rest spread over the four mid cores (2.7 GHz, narrower), as the
 * scheduler moved it around. A list such as "7", "3-6" or "3,4,7"; apps may set the affinity of their own threads. */
void android_thread_affinity(const char *env_name) {
    const char *e = getenv(env_name);
    if (!e || !*e) return;
    uint64_t mask = 0;
    for (const char *p = e; *p;) {
        char *end; long a = strtol(p, &end, 10);
        if (end == p) break;
        long b = a;
        if (*end == '-') { p = end + 1; b = strtol(p, &end, 10); }
        for (long c = a; c <= b && c < 64; ++c) if (c >= 0) mask |= 1ull << c;
        p = *end == ',' ? end + 1 : end;
        if (p == end && *end) break;
    }
    if (!mask) return;
    int tid = (int)gettid();
    long r = syscall(__NR_sched_setaffinity, tid, sizeof mask, &mask);
    __android_log_print(ANDROID_LOG_INFO, "TOTKGameLoader", "[android-rt] thread %d pinned to cpus %s (mask %#llx): %s", tid, e, (unsigned long long)mask, r ? strerror(errno) : "set");
}
void android_thread_work_report(uint64_t cpu_ns) {
    if(!rt_on()||!cpu_ns)return;
    pthread_mutex_lock(&rt_lock);
    if(rt_session&&hint.report)hint.report(rt_session,(int64_t)cpu_ns);
    pthread_mutex_unlock(&rt_lock);
}

static int host_prot(vm_prot_t p) {
    return (p&VM_PROT_READ?PROT_READ:0)|(p&VM_PROT_WRITE?PROT_WRITE:0)|(p&VM_PROT_EXECUTE?PROT_EXEC:0);
}

kern_return_t mach_port_deallocate(mach_port_t task,mach_port_t port) {
    (void)task;(void)port;return KERN_SUCCESS;
}

kern_return_t mach_vm_allocate(vm_map_t task,mach_vm_address_t *address,mach_vm_size_t size,int flags) {
    (void)task;
    int mf=MAP_PRIVATE|MAP_ANONYMOUS;
    if(flags&VM_FLAGS_FIXED)mf|=(flags&VM_FLAGS_OVERWRITE)?MAP_FIXED:MAP_FIXED_NOREPLACE;
    void *p=mmap((void *)(uintptr_t)*address,(size_t)size,PROT_READ|PROT_WRITE,mf,-1,0);
    if(p==MAP_FAILED)return KERN_FAILURE;
    *address=(uintptr_t)p;return KERN_SUCCESS;
}

kern_return_t mach_vm_deallocate(vm_map_t task,mach_vm_address_t address,mach_vm_size_t size) {
    (void)task;return munmap((void *)(uintptr_t)address,(size_t)size)==0?KERN_SUCCESS:KERN_FAILURE;
}

kern_return_t mach_vm_map(vm_map_t task,mach_vm_address_t *address,mach_vm_size_t size,
                          mach_vm_offset_t mask,int flags,memory_object_t object,
                          memory_object_offset_t offset,int copy,vm_prot_t current,
                          vm_prot_t maximum,vm_inherit_t inherit) {
    (void)task;(void)mask;(void)object;(void)offset;(void)copy;(void)maximum;(void)inherit;
    int mf=MAP_PRIVATE|MAP_ANONYMOUS;
    if(flags&VM_FLAGS_FIXED)mf|=(flags&VM_FLAGS_OVERWRITE)?MAP_FIXED:MAP_FIXED_NOREPLACE;
    else mf|=0;
    void *p=mmap((void *)(uintptr_t)*address,(size_t)size,host_prot(current),mf,-1,0);
    if(p==MAP_FAILED)return KERN_FAILURE;
    *address=(uintptr_t)p;return KERN_SUCCESS;
}

static int map_at_or_after(uintptr_t requested,uintptr_t *start,uintptr_t *end,int *prot) {
    FILE *maps=fopen("/proc/self/maps","r");if(!maps)return 0;
    char line[512];uintptr_t best_start=UINTPTR_MAX,best_end=0;int best_prot=0;
    while(fgets(line,sizeof line,maps)) {
        unsigned long long lo,hi;char perms[5]={0};
        if(sscanf(line,"%llx-%llx %4s",&lo,&hi,perms)!=3)continue;
        uintptr_t l=(uintptr_t)lo,h=(uintptr_t)hi;
        if(l<=requested&&requested<h){best_start=l;best_end=h;best_prot=(perms[0]=='r'?VM_PROT_READ:0)|(perms[1]=='w'?VM_PROT_WRITE:0)|(perms[2]=='x'?VM_PROT_EXECUTE:0);break;}
        if(l>=requested&&l<best_start){best_start=l;best_end=h;best_prot=(perms[0]=='r'?VM_PROT_READ:0)|(perms[1]=='w'?VM_PROT_WRITE:0)|(perms[2]=='x'?VM_PROT_EXECUTE:0);}
    }
    fclose(maps);if(best_start==UINTPTR_MAX)return 0;
    *start=best_start;*end=best_end;*prot=best_prot;return 1;
}

kern_return_t mach_vm_region(vm_map_t task,mach_vm_address_t *address,mach_vm_size_t *size,
                             vm_region_flavor_t flavor,vm_region_info_t info,
                             mach_msg_type_number_t *count,mach_port_t *object) {
    (void)task;(void)flavor;(void)count;
    uintptr_t lo,hi;int prot;
    if(!map_at_or_after((uintptr_t)*address,&lo,&hi,&prot))return KERN_INVALID_ADDRESS;
    *address=lo;*size=hi-lo;if(info){memset(info,0,sizeof(vm_region_basic_info_data_64_t));info[0]=prot;info[1]=VM_PROT_ALL;}if(object)*object=MACH_PORT_NULL;return KERN_SUCCESS;
}

kern_return_t mach_vm_read_overwrite(vm_map_t task,mach_vm_address_t address,mach_vm_size_t size,
                                     mach_vm_address_t data,mach_vm_size_t *out_size) {
    (void)task;
    struct iovec local={(void *)(uintptr_t)data,(size_t)size},remote={(void *)(uintptr_t)address,(size_t)size};
    ssize_t n=process_vm_readv(getpid(),&local,1,&remote,1,0);
    if(n<0){*out_size=0;return KERN_INVALID_ADDRESS;}*out_size=(mach_vm_size_t)n;return KERN_SUCCESS;
}

kern_return_t vm_region_64(vm_map_t task,vm_address_t *address,vm_size_t *size,
                           vm_region_flavor_t flavor,vm_region_info_t info,
                           mach_msg_type_number_t *count,mach_port_t *object) {
    mach_vm_address_t a=*address;mach_vm_size_t n=0;kern_return_t r=mach_vm_region(task,&a,&n,flavor,info,count,object);*address=a;*size=n;return r;
}

kern_return_t vm_read_overwrite(vm_map_t task,vm_address_t address,vm_size_t size,
                                vm_address_t data,vm_size_t *out_size) {
    mach_vm_size_t n=0;kern_return_t r=mach_vm_read_overwrite(task,address,size,data,&n);*out_size=n;return r;
}

kern_return_t vm_region_recurse_64(vm_map_t task,vm_address_t *address,vm_size_t *size,
                                   natural_t *depth,vm_region_recurse_info_t info,
                                   mach_msg_type_number_t *count) {
    (void)depth;mach_port_t object;return vm_region_64(task,address,size,VM_REGION_BASIC_INFO_64,info,count,&object);
}

kern_return_t mach_vm_remap(vm_map_t task,mach_vm_address_t *address,mach_vm_size_t size,
                            mach_vm_offset_t mask,int flags,vm_map_t source_task,
                            mach_vm_address_t source,int copy,vm_prot_t *current,
                            vm_prot_t *maximum,vm_inherit_t inherit) {
    (void)task;(void)mask;(void)source_task;(void)inherit;
    if(copy||!(flags&VM_FLAGS_FIXED)||!(flags&VM_FLAGS_OVERWRITE))return KERN_FAILURE;
    /* old_size 0 duplicates a shared mapping at the fixed destination. */
    void *p=mremap((void *)(uintptr_t)source,0,(size_t)size,MREMAP_MAYMOVE|MREMAP_FIXED,(void *)(uintptr_t)*address);
    if(p==MAP_FAILED)return KERN_FAILURE;
    if(current)*current=VM_PROT_READ|VM_PROT_WRITE;
    if(maximum)*maximum=VM_PROT_ALL;
    *address=(uintptr_t)p;return KERN_SUCCESS;
}

kern_return_t thread_suspend(mach_port_t thread) {(void)thread;return KERN_FAILURE;}
kern_return_t thread_resume(mach_port_t thread) {(void)thread;return KERN_FAILURE;}
kern_return_t thread_get_state(mach_port_t thread,int flavor,thread_state_t state,mach_msg_type_number_t *count) {
    (void)thread;(void)flavor;(void)state;(void)count;return KERN_FAILURE;
}

int mach_timebase_info(mach_timebase_info_t info) {if(!info)return 1;info->numer=1;info->denom=1;return 0;}
uint64_t mach_absolute_time(void) {struct timespec ts;if(clock_gettime(CLOCK_MONOTONIC,&ts))return 0;return (uint64_t)ts.tv_sec*1000000000ULL+(uint64_t)ts.tv_nsec;}
kern_return_t mach_wait_until(uint64_t deadline) {
    struct timespec ts={.tv_sec=(time_t)(deadline/1000000000ULL),.tv_nsec=(long)(deadline%1000000000ULL)};
    while(clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&ts,NULL)==EINTR){}
    return KERN_SUCCESS;
}
uint64_t mach_continuous_time(void) {struct timespec ts;if(clock_gettime(CLOCK_BOOTTIME,&ts))return 0;return (uint64_t)ts.tv_sec*1000000000ULL+(uint64_t)ts.tv_nsec;}
