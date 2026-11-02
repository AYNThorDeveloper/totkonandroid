/* Android loader milestone for the local TOTK 1.4.3 rewrite.
 * It maps the three original modules, checks the private rewrite digest,
 * applies RELA/RELR relocations, and reports imports. It does not call game
 * constructors or entry points. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "runtime.h"
#include "android_thread_slots.h"
#include "graphics_settings.h"
#include "game_version.h"

#include "android_runtime_tables.h"

#define MODULE_COUNT ANDROID_MODULE_COUNT
#define PAGE_SIZE 4096u
#define KEY_PAGE_SIZE 0x4000u

static Module *modules = android_modules;
static unsigned total_missing_strong;
static unsigned total_missing_weak;
static unsigned total_host_imports;
static unsigned total_cross_imports;
static unsigned module_missing_strong[MODULE_COUNT];
static unsigned module_missing_weak[MODULE_COUNT];
static unsigned module_host_imports[MODULE_COUNT];
static unsigned char *module_permissions[MODULE_COUNT];
static pthread_mutex_t module_permission_lock=PTHREAD_MUTEX_INITIALIZER;
static int booting;
static int startup_checkpoint_seen;

extern void android_gate_probe(void);
extern void android_runtime_services_init(void);
extern void android_dynamic_tls_rewrite(Module *module);

static void fail(const char *what) {
    fprintf(stderr, "[android-loader] %s: %s\n", what, strerror(errno));
    exit(1);
}

static void require(bool ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "[android-loader] invariant failed: %s\n", what);
        exit(1);
    }
}

_Noreturn void runtime_fail(const char *message) {
    fprintf(stderr,"[android-loader] FAIL: %s\n",message);
    exit(1);
}

static size_t align_up(size_t value, size_t align) {
    return (value + align - 1) & ~(align - 1);
}

#include "sha256.h"

static uint64_t hash_name(const char *s) { uint64_t h=1469598103934665603ULL; for(;*s;s++)h=(h^(uint8_t)*s)*1099511628211ULL; return h; }
static void make_hash(Module *m) {
    unsigned cap=16; while(cap<m->nsym*2u)cap<<=1;
    m->hash=calloc(cap,sizeof(*m->hash)); require(m->hash!=NULL,"symbol hash allocation"); m->hash_mask=cap-1;
    for(unsigned i=1;i<m->nsym;i++) if(m->symbols[i].defined) {
        unsigned p=(unsigned)hash_name(m->symbols[i].name)&m->hash_mask;
        while(m->hash[p]&&strcmp(m->symbols[i].name,m->symbols[m->hash[p]].name))p=(p+1)&m->hash_mask;
        if(!m->hash[p])m->hash[p]=i;
    }
}
static const Symbol *module_symbol(Module *m,const char *name) {
    unsigned p=(unsigned)hash_name(name)&m->hash_mask;
    while(m->hash[p]) { const Symbol *s=&m->symbols[m->hash[p]]; if(!strcmp(name,s->name))return s; p=(p+1)&m->hash_mask; }
    return NULL;
}
static uintptr_t module_symbol_address(Module *m,const Symbol *s) {
    return s&&s->value<m->key_off?(uintptr_t)m->base+s->value:0;
}
/* Host replacements take precedence even over a module's own exports, as in
 * the macOS loader: the SDK reaches many of its exported functions through
 * its own PLT (e.g. nn::hid::StartSixAxisSensor -> detail::StartSixAxisSensor),
 * and those internal calls must reach the host implementation too. */
static uintptr_t resolve_import(Module *owner,const Symbol *s,int *kind) {
    void *override=runtime_override(s->name);
    if(override) { *kind=4; return (uintptr_t)runtime_hle_wrap_named(override,s->name); }
    if(s->defined) { *kind=1; return module_symbol_address(owner,s); }
    for(unsigned i=0;i<MODULE_COUNT;i++) {
        const Symbol *candidate=module_symbol(&modules[i],s->name);
        uintptr_t addr=module_symbol_address(&modules[i],candidate);
        if(addr) { *kind=2; return addr; }
    }
    void *host=dlsym(RTLD_DEFAULT,s->name);
    if(host) { *kind=3; return (uintptr_t)runtime_hle_wrap_named(host,s->name); }
    *kind=0;
    return 0;
}

void android_gate_dispatch(uintptr_t return_address,uint64_t *registers) {
    for(unsigned i=0;i<MODULE_COUNT;i++) {
        Module *m=&modules[i];
        for(unsigned j=0;j<m->ngate;j++) {
            const Gate *g=&m->gates[j];
            if((uintptr_t)m->base+g->ret!=return_address)continue;
            if(g->timer) {
                require(g->arg<31&&g->arg!=18,"invalid timer destination register");
                struct timespec now;
                require(clock_gettime(CLOCK_MONOTONIC,&now)==0,"read Android monotonic clock");
                uint64_t ns=(uint64_t)now.tv_sec*1000000000ULL+(uint64_t)now.tv_nsec;
                registers[g->arg]=(ns*12ULL)/625ULL; /* Switch CNTFRQ_EL0 is 19.2 MHz. */
            } else {
                runtime_svc(g->arg,registers,m->name,g->original);
            }
            return;
        }
    }
    fprintf(stderr,"[android-loader] unknown gate return address %#" PRIxPTR "\n",return_address);
    exit(1);
}

void *runtime_symbol(const char *name) {
    for(unsigned i=0;i<MODULE_COUNT;i++) {
        const Symbol *s=module_symbol(&modules[i],name);
        uintptr_t p=module_symbol_address(&modules[i],s);
        if(p)return (void *)p;
    }
    return NULL;
}

uint8_t *runtime_module_base(const char *name) {
    for(unsigned i=0;i<MODULE_COUNT;i++)if(!strcmp(modules[i].name,name))return modules[i].base;
    return NULL;
}

void android_trace_guest_pc(uint64_t pc) {
    for(unsigned i=0;i<MODULE_COUNT;i++) {
        Module *m=&modules[i]; uintptr_t base=(uintptr_t)m->base;
        if(pc<base||pc-base>=m->size)continue;
        uint64_t off=pc-base; const Symbol *best=NULL;
        for(unsigned j=0;j<m->nsym;j++) {
            const Symbol *s=&m->symbols[j];
            if(s->defined&&s->value<=off&&(!best||s->value>best->value))best=s;
        }
        if(best)fprintf(stderr,"[android-runtime] PC %s+%#llx nearest %s+%#llx\n",m->name,
                        (unsigned long long)off,best->name,(unsigned long long)(off-best->value));
        else fprintf(stderr,"[android-runtime] PC %s+%#llx (no preceding symbol)\n",m->name,(unsigned long long)off);
        return;
    }
    fprintf(stderr,"[android-runtime] PC %#llx is outside the mapped game modules\n",(unsigned long long)pc);
}

static unsigned module_page_permission(unsigned i,uint64_t off) {
    Module *m=&modules[i];
    if(off<m->ro_off)return 5;
    if(off<m->data_off)return 1;
    if(off<m->key_off) {
        if(!module_permissions[i])return 3;
        return module_permissions[i][(off-m->data_off)/PAGE_SIZE]&7;
    }
    if(off<m->key_off+KEY_PAGE_SIZE)return 1;
    return off>=m->patch_off?5:3;
}

/* Address naming for the shared graphics layer's diagnostics. */
int runtime_module_offset(uint64_t pc,const char **name,uint64_t *off) {
    for(unsigned i=0;i<MODULE_COUNT;i++)if(pc>=(uintptr_t)modules[i].base&&pc<(uintptr_t)modules[i].base+modules[i].size){
        *name=modules[i].name;*off=pc-(uintptr_t)modules[i].base;return 1;}
    return 0;
}
void runtime_describe_address(uint64_t a,char *out,size_t n) {
    const char *name;uint64_t off;
    if(runtime_module_offset(a,&name,&off))snprintf(out,n,"%s+0x%llx",name,(unsigned long long)off);
    else snprintf(out,n,"host:0x%llx",(unsigned long long)a);
}
/* The graphics diagnostic reads the pre-1.4 controller layout (see loader.c). */
uintptr_t runtime_frame_controller_return_pc(void) { return 0; }

uint64_t runtime_module_permission(uint64_t address,uint64_t size,unsigned permission) {
    if(address&(PAGE_SIZE-1))return 0xcc01;
    if(!size||(size&(PAGE_SIZE-1)))return 0xca01;
    if(permission!=0&&permission!=1&&permission!=3)return 0xd801;
    for(unsigned i=0;i<MODULE_COUNT;i++) {
        Module *m=&modules[i];uintptr_t start=(uintptr_t)m->base;
        if(address<start||address-start<m->data_off||address-start>=m->key_off||size>m->key_off-(address-start))continue;
        pthread_mutex_lock(&module_permission_lock);
        if(!module_permissions[i]) {
            size_t count=(m->key_off-m->data_off)/PAGE_SIZE;
            module_permissions[i]=malloc(count);
            if(!module_permissions[i]){pthread_mutex_unlock(&module_permission_lock);runtime_fail("allocate Android permission map");}
            memset(module_permissions[i],3,count);
        }
        size_t first=(address-start-m->data_off)/PAGE_SIZE;
        for(size_t p=0;p<size/PAGE_SIZE;p++)module_permissions[i][first+p]=(unsigned char)permission;
        int prot=(permission&1?PROT_READ:0)|(permission&2?PROT_WRITE:0);
        int ok=mprotect((void *)(uintptr_t)address,(size_t)size,prot)==0;
        pthread_mutex_unlock(&module_permission_lock);
        return ok?0:0xd401;
    }
    return 0xd401;
}

uint64_t runtime_module_lock_permission(uint64_t address,uint64_t size,unsigned mask,unsigned attribute) {
    if(address&(PAGE_SIZE-1))return 0xcc01;
    if(!size||(size&(PAGE_SIZE-1)))return 0xca01;
    if(mask!=16||attribute!=16)return 0xd801;
    for(unsigned i=0;i<MODULE_COUNT;i++) {
        Module *m=&modules[i];uintptr_t start=(uintptr_t)m->base;
        if(address<start||address-start<m->data_off||address-start>=m->key_off||size>m->key_off-(address-start))continue;
        pthread_mutex_lock(&module_permission_lock);
        if(!module_permissions[i]) {
            size_t count=(m->key_off-m->data_off)/PAGE_SIZE;
            module_permissions[i]=calloc(count,1);
            if(!module_permissions[i]){pthread_mutex_unlock(&module_permission_lock);runtime_fail("allocate Android RELRO map");}
            memset(module_permissions[i],3,count);
        }
        size_t first=(address-start-m->data_off)/PAGE_SIZE;
        for(size_t p=0;p<size/PAGE_SIZE;p++)module_permissions[i][first+p]|=16;
        pthread_mutex_unlock(&module_permission_lock);return 0;
    }
    return 0xd401;
}

int runtime_module_query(uint64_t address,uint64_t *base,uint64_t *size,
                         unsigned *state,unsigned *permission,unsigned *attribute) {
    for(unsigned i=0;i<MODULE_COUNT;i++) {
        Module *m=&modules[i];uintptr_t start=(uintptr_t)m->base;
        if(address<start||address-start>=m->size)continue;
        uint64_t off=(address-start)&~(uint64_t)(PAGE_SIZE-1),end=off+PAGE_SIZE;
        pthread_mutex_lock(&module_permission_lock);
        unsigned perm=module_page_permission(i,off),attr=off>=m->data_off&&off<m->key_off&&module_permissions[i]?
            module_permissions[i][(off-m->data_off)/PAGE_SIZE]&16:0;
        while(off>0&&module_page_permission(i,off-PAGE_SIZE)==perm)off-=PAGE_SIZE;
        while(end<m->size&&module_page_permission(i,end)==perm)end+=PAGE_SIZE;
        pthread_mutex_unlock(&module_permission_lock);
        *base=start+off;*size=end-off;*state=off>=m->data_off?4:3;*permission=perm;*attribute=attr;return 1;
    }
    /* mmap may place the modules apart. Gaps must not look like executable
       code to nn::ro's module-header search. */
    uint64_t lower=0,upper=1ULL<<39;
    for(unsigned i=0;i<MODULE_COUNT;i++) {
        uint64_t start=(uintptr_t)modules[i].base,end=start+modules[i].size;
        if(end<=address&&end>lower)lower=end;
        if(start>address&&start<upper)upper=start;
    }
    if(lower && upper<(1ULL<<39) && address>=lower && address<upper) {
        *base=lower;*size=upper-lower;*state=0x10;*permission=0;*attribute=0;return 1;
    }
    return 0;
}

uint64_t runtime_image_bytes(void) {
    uint64_t total=0;for(unsigned i=0;i<MODULE_COUNT;i++)total+=modules[i].key_off;
    return (total+PAGE_SIZE-1)&~(uint64_t)(PAGE_SIZE-1);
}

uint64_t runtime_now_ns(void) {
    struct timespec now;require(clock_gettime(CLOCK_MONOTONIC,&now)==0,"read Android monotonic clock");
    return (uint64_t)now.tv_sec*1000000000ULL+(uint64_t)now.tv_nsec;
}

void runtime_startup_checkpoint(uintptr_t caller) {
    if(!booting||startup_checkpoint_seen)return;
    require(caller==(uintptr_t)modules[0].base+GAME_ADDR(0xe995c4),"startup checkpoint is not the v1.4.3 nnMain focus site");
    for(unsigned i=0;i<MODULE_COUNT;i++) {
        uint32_t state;memcpy(&state,modules[i].base+modules[i].ctor_flag,sizeof(state));
        require(state==2,"SDK constructor checkpoint is incomplete");
    }
    startup_checkpoint_seen=1;
    const char *stop=getenv("TOTK_STOP_AT_STARTUP_CHECKPOINT");
    if(stop&&*stop&&strcmp(stop,"0")&&strcmp(stop,"false")) {
        printf("{\"status\":\"pass\",\"version\":\"1.4.3\",\"phase\":\"SDK startup and module constructors completed\",\"scope\":\"stopped at the original nnMain focus setup before graphics\"}\n");
        fflush(stdout);_exit(0);
    }
    fprintf(stderr,"[android-loader] passed SDK constructor checkpoint; continuing original nnMain\n");
}

static void loader_ready(void) { fprintf(stderr,"[android-loader] SDK linked the modules\n"); }
static void module_inits(void) {
    for(unsigned i=0;i<MODULE_COUNT;i++) {
        Module *m=&modules[i];
        fprintf(stderr,"[android-loader] enter %s DT_INIT\n",m->name);
        ((void(*)(void))(m->base+m->init))();
        uint32_t state;memcpy(&state,m->base+m->ctor_flag,sizeof(state));
        require(state==2,"SDK skipped module constructors");
        fprintf(stderr,"[android-loader] return %s DT_INIT\n",m->name);
    }
}
static void module_finis(void) { runtime_fail("guest module finalizers are not available in this Android milestone"); }

static void boot_until_renderer(void) {
    booting=1;
    android_runtime_services_init();
    runtime_thread_init();
    uint32_t main_handle=svc_thread_main_init();
    void *start=runtime_symbol("_ZN2nn4init5StartEmmPFvvES2_S2_");
    require(start!=NULL,"nn::init::Start import is present");
    fprintf(stderr,"[android-loader] enter original nn::init::Start\n");
    ((void(*)(uint64_t,uint64_t,void(*)(void),void(*)(void),void(*)(void)))start)(main_handle,0,loader_ready,module_inits,module_finis);
    runtime_fail("nn::init::Start returned unexpectedly");
}

static void map_module(Module *m,const char *directory) {
    char path[4096];
    int n=snprintf(path,sizeof(path),"%s/%s.patched.bin",directory,m->name);
    require(n>0&&(size_t)n<sizeof(path),"module path length");
    int fd=open(path,O_RDONLY|O_CLOEXEC); if(fd<0)fail("open patched module");
    struct stat st; if(fstat(fd,&st))fail("stat patched module");
    require((uint64_t)st.st_size==m->file_size,"patched module size differs from generated metadata");
    long page=sysconf(_SC_PAGESIZE); require(page==PAGE_SIZE,"Thor uses unexpected page size");
    m->size=align_up(m->file_size,PAGE_SIZE)+(!strcmp(m->name,"main")?KEY_PAGE_SIZE:0);
    m->base=mmap(NULL,m->size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(m->base==MAP_FAILED)fail("mmap module");
    size_t done=0;
    while(done<m->file_size){ssize_t got=pread(fd,m->base+done,m->file_size-done,(off_t)done);if(got<0){if(errno==EINTR)continue;fail("read module");}if(!got){errno=EIO;fail("short module read");}done+=(size_t)got;}
    close(fd);
    Sha256 sha;uint8_t digest[32];char hex[65];sha_init(&sha);sha_update(&sha,m->base,m->file_size);sha_final(&sha,digest);digest_hex(digest,hex);
    require(!strcmp(hex,m->digest),"patched module digest differs from generated metadata");
    android_dynamic_tls_rewrite(m);
    require(m->text_end&&m->text_end<m->ro_off&&m->ro_off<=m->data_off&&m->data_off<m->key_off,"invalid module layout");
    require(m->ro_off==align_up(m->text_end,PAGE_SIZE)&&!(m->ro_off&(PAGE_SIZE-1))&&!(m->data_off&(PAGE_SIZE-1))&&!(m->key_off&(KEY_PAGE_SIZE-1))&&!(m->patch_off&(KEY_PAGE_SIZE-1)),"module boundaries are not Android page aligned");
    require(m->key_off+KEY_PAGE_SIZE<=m->patch_off&&m->patch_off<m->file_size,"key/patch range outside module");
    uint64_t keys[ANDROID_KEY_COUNT]={0};
    android_thread_slots_offsets(keys);
#ifdef ANDROID_LOADER_LIBRARY
    if (!strcmp(m->name,"main")) fprintf(stderr,
        "[android-tls] module key-slot offsets K18=%#llx KTLS=%#llx KTLR=%#llx KGATE=%#llx\n",
        (unsigned long long)keys[0],(unsigned long long)keys[1],
        (unsigned long long)keys[2],(unsigned long long)keys[3]);
#endif
    for(unsigned i=0;i<4;i++)((uint64_t *)(m->base+m->key_off))[i]=keys[i];
    ((uint64_t *)(m->base+m->key_off))[ANDROID_KEY_KRES0]=0x02d00500ULL; /* 1280 x 720 */
    ((uint64_t *)(m->base+m->key_off))[ANDROID_KEY_KRES1]=0x03840640ULL; /* 1600 x 900 */
    ((uint64_t *)(m->base+m->key_off))[ANDROID_KEY_KASPECT]=0x40071c72ULL;
    ((uint64_t *)(m->base+m->key_off))[ANDROID_KEY_KASPECTUI]=0x40071c72ULL;
    make_hash(m);
    fprintf(stderr,"[android-loader] mapped %s at %p size=%zu symbols=%u relocations=%u\n",m->name,m->base,m->size,m->nsym,m->nreloc);
}

static void relocate_module(Module *m,unsigned module_index) {
    for(unsigned i=0;i<m->nreloc;i++) {
        const Reloc *r=&m->relocs[i];
        require(r->where>=m->data_off&&r->where+8<=m->key_off&&r->symbol<m->nsym,"relocation outside writable image");
        uintptr_t value=0;
        if(r->kind==1027) {
            require(r->addend>=0&&(uint64_t)r->addend<m->key_off,"relative relocation addend outside image");
            value=(uintptr_t)m->base+(uintptr_t)r->addend;m->relative++;
        } else {
            require(r->kind==257||r->kind==1025||r->kind==1026,"unsupported AArch64 relocation");
            const Symbol *s=&m->symbols[r->symbol];int resolution=0;
            value=resolve_import(m,s,&resolution);
            if(!value) {
                if(s->binding==2){m->weak++;module_missing_weak[module_index]++;total_missing_weak++;}
                else {module_missing_strong[module_index]++;total_missing_strong++;if(total_missing_strong<=32)fprintf(stderr,"[android-loader] unresolved strong import %s: %s\n",m->name,s->name);}
            } else {
                if(resolution==2){m->cross++;total_cross_imports++;}
                else if(resolution==3||resolution==4){module_host_imports[module_index]++;total_host_imports++;}
            }
            if(r->kind!=1026)value+=(uintptr_t)r->addend;
            m->symbolic++;
        }
        memcpy(m->base+r->where,&value,sizeof(value));
    }
}

static int main_key_writable;   /* the real-time frame step keeps its data in main's key page */
static void protect_module(Module *m) {
    size_t text=align_up(m->text_end,PAGE_SIZE);
    size_t ro_end=m->data_off;
    require(mprotect(m->base,text,PROT_READ|PROT_EXEC)==0,"protect module text RX");
    if(ro_end>m->ro_off)require(mprotect(m->base+m->ro_off,ro_end-m->ro_off,PROT_READ)==0,"protect module rodata R");
    int key_rw=main_key_writable&&!strcmp(m->name,"main");
    require(mprotect(m->base+m->key_off,KEY_PAGE_SIZE,key_rw?PROT_READ|PROT_WRITE:PROT_READ)==0,"protect module key page");
    size_t patch_end=m->size;
    require(mprotect(m->base+m->patch_off,patch_end-m->patch_off,PROT_READ|PROT_EXEC)==0,"protect module patch text RX");
    __builtin___clear_cache((char *)m->base,(char *)m->base+text);
    __builtin___clear_cache((char *)m->base+m->patch_off,(char *)m->base+patch_end);
}

typedef struct { uintptr_t function; uint64_t last_tick; unsigned calls; } ClockProbe;

static void *clock_probe_thread(void *argument) {
    ClockProbe *probe=argument;
    void *guest_tls=calloc(1,8192),*guest_tlr=calloc(1,0x200);
    require(guest_tls&&guest_tlr,"allocate per-thread guest TLS");
    android_thread_slots_bind(0,(uintptr_t)guest_tls,(uintptr_t)guest_tlr,(uintptr_t)android_gate_probe);
    uint64_t previous=0;
    for(unsigned i=0;i<1000;i++) {
        uint64_t current=((uint64_t(*)(void))probe->function)();
        require(current>previous,"original GetSystemTick returned a monotonic value");
        previous=current;
    }
    probe->last_tick=previous;probe->calls=1000;
    free(guest_tlr);free(guest_tls);
    return NULL;
}

static void call_original_clock(void) {
    const Symbol *tick=NULL;
    Module *owner=NULL;
    for(unsigned i=0;i<MODULE_COUNT;i++) {
        const Symbol *candidate=module_symbol(&modules[i],"_ZN2nn2os13GetSystemTickEv");
        if(candidate){tick=candidate;owner=&modules[i];break;}
    }
    require(tick&&owner,"TOTK GetSystemTick symbol is present");
    uintptr_t fn=module_symbol_address(owner,tick);
    require(fn!=0,"TOTK GetSystemTick has an executable address");
    pthread_t threads[4];ClockProbe probes[4]={0};
    for(unsigned i=0;i<4;i++){probes[i].function=fn;require(pthread_create(&threads[i],NULL,clock_probe_thread,&probes[i])==0,"start clock probe thread");}
    uint64_t last=0;unsigned calls=0;
    for(unsigned i=0;i<4;i++){require(pthread_join(threads[i],NULL)==0,"join clock probe thread");calls+=probes[i].calls;if(probes[i].last_tick>last)last=probes[i].last_tick;}
    require(calls==4000,"all four clock probe threads completed");
    printf("{\"status\":\"pass\",\"version\":\"1.4.3\",\"execution\":\"original TOTK SDK GetSystemTick -> per-thread Android TLS -> host gate -> monotonic clock\",\"threads\":4,\"calls\":%u,\"last_tick\":%" PRIu64 ",\"scope\":\"one original timer leaf called; constructors and game entry not run\"}\n",calls,last);
}

int android_loader_run(int argc,char **argv) {
    if(argc<2||argc>3){fprintf(stderr,"usage: %s /path/to/rewritten/android-v143 [load|clock|boot]\n",argv[0]);return 2;}
    const char *mode=argc==3?argv[2]:"load";
    if(strcmp(mode,"load")&&strcmp(mode,"clock")&&strcmp(mode,"boot")){fprintf(stderr,"mode must be load, clock, or boot\n");return 2;}
    for(unsigned i=0;i<MODULE_COUNT;i++)map_module(&modules[i],argv[1]);
    for(unsigned i=0;i<MODULE_COUNT;i++)relocate_module(&modules[i],i);
    const char *graphics_path=getenv("TOTK_GRAPHICS_CONFIG"),*render_patch=getenv("TOTK_RENDER_PATCH");
    if(render_patch && !strcmp(render_patch,"0"))
        fprintf(stderr,"[android-loader] render-resolution patch off (TOTK_RENDER_PATCH=0)\n");
    else if(graphics_path && *graphics_path) {
        TotkGraphicsSettings graphics;
        require(totk_graphics_load(graphics_path,&graphics)==1,"graphics config missing or invalid");
        for(unsigned i=0;i<MODULE_COUNT;i++)if(!strcmp(modules[i].name,"main")) {
            Module *m=&modules[i];
            require(totk_graphics_patch_v143(m->base,m->size,m->file_size,&graphics),
                    "1.4.3 render-resolution instruction guard failed");
            fprintf(stderr,"[android-loader] configured scene render size %ux%u; configured output %ux%u\n",
                    graphics.render_width,graphics.render_height,graphics.output_width,graphics.output_height);
            /* The renderer recognises the scene colour target by this size (nvn_vk_backend.c, scene_size) */
            char scene[32];snprintf(scene,sizeof scene,"%ux%u",graphics.render_width,graphics.render_height);
            setenv("TOTK_SCENE_SIZE",scene,1);
        }
    }
    for(unsigned i=0;i<MODULE_COUNT;i++)if(!strcmp(modules[i].name,"main")&&!strcmp(mode,"boot")) {
        Module *m=&modules[i];
        extern int android_frame_step_patch(uint8_t *base,uint64_t text_end,uint64_t ro_off,uint64_t key_off);
        main_key_writable=android_frame_step_patch(m->base,m->text_end,m->ro_off,m->key_off);
        extern int android_video_menu_patch(uint8_t *,uint64_t,uint64_t);
        android_video_menu_patch(m->base,m->text_end,m->ro_off);
    }
    for(unsigned i=0;i<MODULE_COUNT;i++)protect_module(&modules[i]);
    if(!strcmp(mode,"clock"))call_original_clock();
    if(!strcmp(mode,"boot"))boot_until_renderer();
    printf("{\"status\":\"%s\",\"version\":\"1.4.3\",\"modules\":[",total_missing_strong?"loaded_with_unresolved_imports":"loaded");
    for(unsigned i=0;i<MODULE_COUNT;i++) {
        Module *m=&modules[i];
        printf("%s{\"name\":\"%s\",\"symbols\":%u,\"relocations\":%u,\"relative\":%u,\"symbolic\":%u,\"cross_module\":%u,\"host\":%u,\"missing_strong\":%u,\"missing_weak\":%u}",i?",":"",m->name,m->nsym,m->nreloc,m->relative,m->symbolic,m->cross,module_host_imports[i],module_missing_strong[i],module_missing_weak[i]);
    }
    printf("],\"totals\":{\"cross_module\":%u,\"host\":%u,\"missing_strong\":%u,\"missing_weak\":%u},\"scope\":\"modules mapped and relocated; no TOTK constructor, service, or game entry executed\"}\n",total_cross_imports,total_host_imports,total_missing_strong,total_missing_weak);
    return total_missing_strong?3:0;
}

#ifndef ANDROID_LOADER_LIBRARY
int main(int argc,char **argv) { return android_loader_run(argc,argv); }
#endif
