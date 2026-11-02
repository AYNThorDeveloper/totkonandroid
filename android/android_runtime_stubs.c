#define _GNU_SOURCE
/* Platform hooks not needed before the game reaches renderer startup.
 * The Android host will replace these with filesystem, input, audio, and
 * Vulkan implementations as those subsystems are ported. */
#include "runtime.h"
#include "ipc.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/system_properties.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

uint64_t runtime_output_res_extra_bytes(void) { return 0; }

/* dm_guest_write_prepare lives in nvn_vk_host.c with the dirty-page tracker. */

void runtime_trace_guest(uint64_t fp,uint64_t lr) {
    fprintf(stderr,"[android-runtime] guest trace unavailable fp=%#llx lr=%#llx\n",
            (unsigned long long)fp,(unsigned long long)lr);
    extern void android_trace_guest_pc(uint64_t);
    android_trace_guest_pc(lr);
    for(unsigned i=0;i<12 && fp && !(fp&15);i++) {
        uint64_t frame[2];
        struct iovec local={frame,sizeof frame},remote={(void *)(uintptr_t)fp,sizeof frame};
        if(process_vm_readv(getpid(),&local,1,&remote,1,0)!=(ssize_t)sizeof frame)break;
        if(!frame[1])break;
        fprintf(stderr,"[android-runtime] frame %u fp=%#llx prev=%#llx lr=%#llx\n",i,
                (unsigned long long)fp,(unsigned long long)frame[0],(unsigned long long)frame[1]);
        android_trace_guest_pc(frame[1]);
        if(frame[0]<=fp || frame[0]-fp>(16u<<20))break;
        fp=frame[0];
    }
}

/* With libopus present, games/totk/runtime/opus_codec.c provides this. */
__attribute__((weak)) void *runtime_codec_override(const char *name) {(void)name;return NULL;}
/* TOTK_AUTOLOAD (scripted save loading for macOS test sessions) is not ported. */
void runtime_autoload_save_file_opened(const char *path) {(void)path;}
void *runtime_font_override(const char *name) {(void)name;return NULL;}
void *runtime_swkbd_override(const char *name) {(void)name;return NULL;}

static void android_lm_logger(IpcSession *session,const IpcRequest *request,IpcResponse *response) {
    (void)session;(void)request;(void)response;
}

static void android_lm_service(IpcSession *session,const IpcRequest *request,IpcResponse *response) {
    if(request->cmd==0) {
        if (request->domain) {
            uint32_t id=runtime_ipc_domain_new_object(session,"lm:logger",android_lm_logger,NULL);
            response->domain_objects[response->ndomain_objects++]=id;
        } else {
            response->move[response->nmove++]=runtime_ipc_new_session("lm:logger",android_lm_logger,NULL,NULL);
        }
        return;
    }
    response->result=0;
}

/* APM only configures the Switch's power profile. Android owns clock and thermal
 * policy, so this host reports handheld/normal mode and treats set requests as
 * successful no-ops. */
static void android_apm_session(IpcSession *session,const IpcRequest *request,IpcResponse *response) {
    (void)session;
    if (request->cmd == 1) {
        uint32_t configuration = 0x9222000a; /* Switch handheld profile */
        IPC_OUT(response, configuration);
    }
}

static void android_apm_service(IpcSession *session,const IpcRequest *request,IpcResponse *response) {
    (void)session;
    if (request->cmd == 0) {
        response->move[response->nmove++]=runtime_ipc_new_session("apm:session",android_apm_session,NULL,NULL);
    } else if (request->cmd == 1) {
        uint32_t mode = 0; /* Normal */
        IPC_OUT(response, mode);
    }
}

/* TOTK's 1.4.3 SDK opens aoc:u during SDK startup. This host has no Switch DLC
 * install database, so the service reports an empty add-on list. The game's
 * higher-level AOC count/list helpers are also overridden to the same result. */
static pthread_once_t aoc_event_once=PTHREAD_ONCE_INIT;
static uint32_t aoc_list_changed_event;
static void android_aoc_event_init(void) { aoc_list_changed_event=runtime_event_new(); }
static void android_aoc_service(IpcSession *session,const IpcRequest *request,IpcResponse *response) {
    switch(request->cmd) {
    case 2: { uint32_t count=0; IPC_OUT(response,count); break; }
    case 3: { uint32_t count=0; IPC_OUT(response,count); break; }
    case 5: { uint64_t base_id=0x01007ef00011f000ULL; IPC_OUT(response,base_id); break; }
    case 7: break; /* PrepareAddOnContent: there are no installed entries to prepare. */
    case 8:
        pthread_once(&aoc_event_once,android_aoc_event_init);
        response->copy[response->ncopy++]=runtime_event_readable(aoc_list_changed_event);
        break;
    default: ipc_unimplemented(session,request);
    }
}

/* Error-context registration is used during SDK startup. Android has no Switch
 * error-context service, so retain the protocol shape and accept commits; the
 * context is diagnostic metadata and is not persisted by this bootstrap host. */
static void android_ectx_registrar(IpcSession *session,const IpcRequest *request,IpcResponse *response) {
    (void)response;
    if(request->cmd!=0) ipc_unimplemented(session,request); /* Complete */
}
static void android_ectx_writer(IpcSession *session,const IpcRequest *request,IpcResponse *response) {
    switch(request->cmd) {
    case 0: /* CreateContextRegistrar */
        if(request->domain) {
            if(response->ndomain_objects==sizeof response->domain_objects/sizeof response->domain_objects[0])
                runtime_fail("Android ectx returned too many domain objects");
            uint32_t id=runtime_ipc_domain_new_object(session,"ectx:registrar",android_ectx_registrar,NULL);
            response->domain_objects[response->ndomain_objects++]=id;
        } else {
            response->move[response->nmove++]=runtime_ipc_new_session("ectx:registrar",android_ectx_registrar,NULL,NULL);
        }
        break;
    case 1: /* CommitContext: Android does not persist Switch error metadata. */
        break;
    default:
        ipc_unimplemented(session,request);
    }
}

/* Android has no Switch parental-control account state. For now expose the
 * service object and model only initialization; later checks must be added from
 * the commands the game actually sends. */
static void android_pctl_service(IpcSession *session,const IpcRequest *request,IpcResponse *response) {
    (void)response;
    if(request->cmd==1) return; /* Initialize; no Switch-side policy applies here. */
    ipc_unimplemented(session,request);
}
static void android_pctl_factory(IpcSession *session,const IpcRequest *request,IpcResponse *response) {
    if(request->cmd!=0 && request->cmd!=1) ipc_unimplemented(session,request);
    if(request->domain) {
        if(response->ndomain_objects==sizeof response->domain_objects/sizeof response->domain_objects[0])
            runtime_fail("Android pctl returned too many domain objects");
        uint32_t id=runtime_ipc_domain_new_object(session,"pctl:service",android_pctl_service,NULL);
        response->domain_objects[response->ndomain_objects++]=id;
    } else {
        response->move[response->nmove++]=runtime_ipc_new_session("pctl:service",android_pctl_service,NULL,NULL);
    }
}

typedef struct { uint64_t time_point; uint8_t source_id[16]; } AndroidSteadyTimePoint;
typedef struct { int64_t posix_time; AndroidSteadyTimePoint steady_time_point; } AndroidSystemClockContext;
typedef struct {
    int32_t gmtoff;
    uint8_t is_dst, padding0[3];
    int32_t abbr_index;
    uint8_t is_std, is_gmt, padding1[2];
} AndroidTimeTypeInfo;
typedef struct {
    int32_t time_count, type_count, char_count;
    uint8_t go_back, go_ahead, padding[2];
    int64_t transition_times[1000];
    uint8_t transition_types[1000];
    AndroidTimeTypeInfo types[128];
    char abbreviations[512];
    int32_t default_type;
    uint8_t reserved[0x12c4];
} AndroidTimeZoneRule;
typedef struct { char name[0x24]; } AndroidLocationName;
typedef struct { uint64_t version[2]; } AndroidTimeZoneRuleVersion;
_Static_assert(sizeof(AndroidSteadyTimePoint)==0x18,"Switch steady-clock point layout");
_Static_assert(sizeof(AndroidSystemClockContext)==0x20,"Switch system-clock context layout");
_Static_assert(sizeof(AndroidTimeTypeInfo)==0x10,"Switch timezone type layout");
_Static_assert(sizeof(AndroidTimeZoneRule)==0x4000,"Switch timezone rule layout");
static pthread_once_t time_event_once=PTHREAD_ONCE_INIT;
static uint32_t time_update_event;
static void android_time_event_init(void) { time_update_event=runtime_event_new(); }
static uint64_t android_steady_ns(void) {
    struct timespec ts;
    if(clock_gettime(CLOCK_MONOTONIC,&ts)) return 0;
    return (uint64_t)ts.tv_sec*1000000000ull+(uint64_t)ts.tv_nsec;
}
static int64_t android_posix_time(void) { return (int64_t)time(NULL); }
static uint64_t android_steady_seconds(void) { return android_steady_ns()/1000000000ull; }
static AndroidSystemClockContext android_clock_context(void) {
    AndroidSystemClockContext context={0};
    context.steady_time_point.time_point=android_steady_seconds();
    context.posix_time=android_posix_time()-(int64_t)context.steady_time_point.time_point;
    return context;
}
static void android_time_write_seqlock(uint8_t *page,size_t offset,const void *value,size_t size) {
    uint32_t sequence=0;
    memcpy(page+offset,&sequence,sizeof sequence);
    memcpy(page+offset+8,value,size);
    memcpy(page+offset+8+size,value,size);
}
static void android_time_system_clock(IpcSession *session,const IpcRequest *request,IpcResponse *response) {
    switch(request->cmd) {
    case 0: { int64_t now=android_posix_time(); IPC_OUT(response,now); break; }
    case 2: { AndroidSystemClockContext context=android_clock_context(); IPC_OUT(response,context); break; }
    case 4:
        pthread_once(&time_event_once,android_time_event_init);
        response->copy[response->ncopy++]=runtime_event_readable(time_update_event);
        break;
    default: ipc_unimplemented(session,request);
    }
}
static void android_time_steady_clock(IpcSession *session,const IpcRequest *request,IpcResponse *response) {
    switch(request->cmd) {
    case 0: { AndroidSteadyTimePoint point={.time_point=android_steady_seconds()}; IPC_OUT(response,point); break; }
    case 2: case 200: { int64_t offset=0; IPC_OUT(response,offset); break; }
    case 100: { int64_t rtc=android_posix_time(); IPC_OUT(response,rtc); break; }
    case 101: { uint8_t reset=0; IPC_OUT(response,reset); break; }
    case 102: { uint32_t result=0; IPC_OUT(response,result); break; }
    default: ipc_unimplemented(session,request);
    }
}
static int android_timezone_name(AndroidLocationName *name) {
    memset(name,0,sizeof *name);
    char timezone[0x24]={0};
    if(__system_property_get("persist.sys.timezone",timezone)<=0 || !timezone[0])
        memcpy(timezone,"UTC",4);
    memcpy(name->name,timezone,strnlen(timezone,sizeof name->name));
    return 0;
}
static int android_time_type(const struct tm *tm) {
    extern AndroidTimeZoneRule android_time_rule;
    int32_t offset=(int32_t)tm->tm_gmtoff;
    const char *abbr=tm->tm_zone?tm->tm_zone:"UTC";
    for(int i=0;i<android_time_rule.type_count;i++) {
        AndroidTimeTypeInfo *type=&android_time_rule.types[i];
        if(type->gmtoff==offset && type->is_dst==(uint8_t)(tm->tm_isdst>0) &&
           !strcmp(android_time_rule.abbreviations+type->abbr_index,abbr)) return i;
    }
    if(android_time_rule.type_count>=128 || android_time_rule.char_count+strlen(abbr)+1>sizeof android_time_rule.abbreviations)
        return android_time_rule.default_type;
    int i=android_time_rule.type_count++;
    AndroidTimeTypeInfo *type=&android_time_rule.types[i];
    type->gmtoff=offset;
    type->is_dst=(uint8_t)(tm->tm_isdst>0);
    type->abbr_index=android_time_rule.char_count;
    size_t n=strlen(abbr)+1;
    memcpy(android_time_rule.abbreviations+android_time_rule.char_count,abbr,n);
    android_time_rule.char_count+=(int32_t)n;
    return i;
}
AndroidTimeZoneRule android_time_rule;
static int android_type_at(int64_t value) {
    time_t t=(time_t)value; struct tm local;
    if(!localtime_r(&t,&local)) return 0;
    return android_time_type(&local);
}
static int64_t android_utc_month(int year,int month) {
    struct tm tm={.tm_year=year-1900,.tm_mon=month-1,.tm_mday=1,.tm_hour=12};
    return (int64_t)timegm(&tm);
}
static pthread_once_t timezone_rule_once=PTHREAD_ONCE_INIT;
static void android_timezone_rule_init(void) {
    memset(&android_time_rule,0,sizeof android_time_rule);
    tzset();
    int64_t previous=android_utc_month(1900,1);
    int previous_type=android_type_at(previous);
    android_time_rule.default_type=previous_type;
    for(int year=1900;year<=2101;year++) for(int month=1;month<=12;month++) {
        if(year==1900 && month==1) continue;
        int64_t current=android_utc_month(year,month);
        int current_type=android_type_at(current);
        if(current_type!=previous_type && android_time_rule.time_count<1000) {
            int64_t lo=previous+1,hi=current;
            while(lo<hi) {
                int64_t mid=lo+(hi-lo)/2;
                if(android_type_at(mid)==previous_type) lo=mid+1;
                else hi=mid;
            }
            android_time_rule.transition_times[android_time_rule.time_count]=lo;
            android_time_rule.transition_types[android_time_rule.time_count]=(uint8_t)current_type;
            android_time_rule.time_count++;
        }
        previous=current;
        previous_type=current_type;
    }
}
static void android_time_timezone_service(IpcSession *session,const IpcRequest *request,IpcResponse *response) {
    pthread_once(&timezone_rule_once,android_timezone_rule_init);
    switch(request->cmd) {
    case 0: { AndroidLocationName name; android_timezone_name(&name); IPC_OUT(response,name); break; }
    case 1: break; /* Android's system timezone is managed by Android settings. */
    case 2: { int32_t count=1; IPC_OUT(response,count); break; }
    case 3: {
        int32_t start=IPC_ARG(request,int32_t,0),count=0;
        if(request->nc && start<=0 && request->c[0].size>=sizeof(AndroidLocationName)) {
            AndroidLocationName name; android_timezone_name(&name);
            memcpy(ipc_guest(request->c[0].addr,sizeof name),&name,sizeof name);
            count=1;
        }
        IPC_OUT(response,count); break;
    }
    case 4: {
        if(request->nb && request->b[0].size>=sizeof android_time_rule)
            memcpy(ipc_guest(request->b[0].addr,sizeof android_time_rule),&android_time_rule,sizeof android_time_rule);
        else if(request->nc && request->c[0].size>=sizeof android_time_rule)
            memcpy(ipc_guest(request->c[0].addr,sizeof android_time_rule),&android_time_rule,sizeof android_time_rule);
        else runtime_fail("Android timezone rule output buffer missing or too small");
        break;
    }
    case 5: { AndroidTimeZoneRuleVersion version={{0,0}}; IPC_OUT(response,version); break; }
    case 6: {
        AndroidLocationName name; AndroidSteadyTimePoint updated={.time_point=android_steady_seconds()};
        android_timezone_name(&name); IPC_OUT(response,name); IPC_OUT(response,updated); break;
    }
    case 20:
        pthread_once(&time_event_once,android_time_event_init);
        response->copy[response->ncopy++]=runtime_event_readable(time_update_event);
        break;
    default: ipc_unimplemented(session,request);
    }
}
static void android_time_add_object(IpcSession *session,const IpcRequest *request,IpcResponse *response,
                                   const char *name,IpcHandler handler) {
    if(request->domain) {
        if(response->ndomain_objects==sizeof response->domain_objects/sizeof response->domain_objects[0])
            runtime_fail("Android time service returned too many domain objects");
        uint32_t id=runtime_ipc_domain_new_object(session,name,handler,NULL);
        response->domain_objects[response->ndomain_objects++]=id;
    } else {
        response->move[response->nmove++]=runtime_ipc_new_session(name,handler,NULL,NULL);
    }
}
static void android_time_static_service(IpcSession *session,const IpcRequest *request,IpcResponse *response) {
    switch(request->cmd) {
    case 0: case 1: case 4:
        android_time_add_object(session,request,response,"time:system-clock",android_time_system_clock);
        break;
    case 2:
        android_time_add_object(session,request,response,"time:steady-clock",android_time_steady_clock);
        break;
    case 3:
        android_time_add_object(session,request,response,"time:timezone",android_time_timezone_service);
        break;
    case 20:
    {
        uint32_t handle=runtime_shared_memory_new(0x1000);
        uint8_t *page=runtime_shared_memory_data(handle);
        AndroidSteadyTimePoint steady={0};
        AndroidSystemClockContext user_clock={0},network_clock={0};
        uint64_t steady_now=android_steady_seconds();
        user_clock.posix_time=android_posix_time()-(int64_t)steady_now;
        user_clock.steady_time_point.time_point=steady_now;
        network_clock=user_clock;
        android_time_write_seqlock(page,0x00,&steady,sizeof steady);
        android_time_write_seqlock(page,0x38,&user_clock,sizeof user_clock);
        android_time_write_seqlock(page,0x80,&network_clock,sizeof network_clock);
        response->copy[response->ncopy++]=handle;
        break;
    }
    default: ipc_unimplemented(session,request);
    }
}

/* Small appletOE host for startup. The Android activity owns focus and the
 * Vulkan surface; these Switch-facing objects report an active handheld app. */
static pthread_once_t applet_events_once=PTHREAD_ONCE_INIT;
static uint32_t applet_message_event, applet_suspended_event, applet_gpu_error_event;
static pthread_mutex_t applet_message_lock=PTHREAD_MUTEX_INITIALIZER;
static uint32_t applet_messages[32];
static unsigned applet_message_head,applet_message_tail;
static void android_applet_events_init(void) {
    applet_message_event=runtime_event_new();
    applet_suspended_event=runtime_event_new();
    applet_gpu_error_event=runtime_event_new();
    applet_messages[applet_message_tail++]=15; /* Initial FocusStateChanged from the Android activity. */
    runtime_event_signal(applet_message_event);
}
/* nn::oe notification messages (focus changes) use the shared applet host
 * queue (games/totk/runtime/applet_host.c), fed by android_front_set_focus. */
static void android_applet_add_object(IpcSession *session,IpcResponse *response,
                                      const char *name,IpcHandler handler) {
    if (response->ndomain_objects == sizeof response->domain_objects / sizeof response->domain_objects[0])
        runtime_fail("Android applet returned too many domain objects");
    uint32_t id=runtime_ipc_domain_new_object(session,name,handler,NULL);
    response->domain_objects[response->ndomain_objects++]=id;
}
static void android_applet_common(IpcSession *session,const IpcRequest *request,IpcResponse *response) {
    (void)session;
    switch(request->cmd) {
    case 0: case 13: {
        pthread_once(&applet_events_once,android_applet_events_init);
        uint32_t event=runtime_event_readable(request->cmd==0?applet_message_event:applet_suspended_event);
        response->copy[response->ncopy++]=event;
        break;
    }
    case 1: {
        pthread_once(&applet_events_once,android_applet_events_init);
        pthread_mutex_lock(&applet_message_lock);
        uint32_t message=applet_message_head!=applet_message_tail?
            applet_messages[applet_message_head++%32]:0;
        if(applet_message_head==applet_message_tail)runtime_event_clear(applet_message_event);
        pthread_mutex_unlock(&applet_message_lock);
        IPC_OUT(response,message); break;
    }
    case 5: { uint8_t mode=0; IPC_OUT(response,mode); break; } /* handheld */
    case 6: { uint32_t mode=0; IPC_OUT(response,mode); break; } /* normal */
    case 9: { uint8_t focus=1; IPC_OUT(response,focus); break; } /* in focus */
    default: break;
    }
}
static void android_applet_self(IpcSession *session,const IpcRequest *request,IpcResponse *response) {
    (void)session;
    if(request->cmd==91) {
        pthread_once(&applet_events_once,android_applet_events_init);
        response->copy[response->ncopy++]=runtime_event_readable(applet_suspended_event);
    }
}
static void android_applet_functions(IpcSession *session,const IpcRequest *request,IpcResponse *response) {
    (void)session;
    switch(request->cmd) {
    case 123: { int32_t index=-1; IPC_OUT(response,index); break; } /* GetPreviousProgramIndex: launched directly */
    case 130:
        pthread_once(&applet_events_once,android_applet_events_init);
        response->copy[response->ncopy++]=runtime_event_readable(applet_gpu_error_event);
        break;
    default:
        fprintf(stderr,"[android-applet] IApplicationFunctions command %u not modeled\n",request->cmd);
    }
}
static void android_applet_window(IpcSession *session,const IpcRequest *request,IpcResponse *response) {
    (void)session;
    if(request->cmd==1) { uint64_t user_id=1; IPC_OUT(response,user_id); }
}
static void android_applet_audio(IpcSession *session,const IpcRequest *request,IpcResponse *response) {
    (void)session;
    if(request->cmd==1||request->cmd==2) { float volume=1.0f; IPC_OUT(response,volume); }
}
static void android_applet_noop(IpcSession *session,const IpcRequest *request,IpcResponse *response) {
    (void)session;(void)request;(void)response;
}
static void android_applet_proxy(IpcSession *session,const IpcRequest *request,IpcResponse *response) {
    const char *name=NULL; IpcHandler handler=android_applet_noop;
    switch(request->cmd) {
    case 0: name="applet:common-state"; handler=android_applet_common; break;
    case 1: name="applet:self"; handler=android_applet_self; break;
    case 2: name="applet:window"; handler=android_applet_window; break;
    case 3: name="applet:audio"; handler=android_applet_audio; break;
    case 4: name="applet:display"; break;
    case 11: name="applet:library-creator"; break;
    case 20: name="applet:functions"; handler=android_applet_functions; break;
    case 1000: name="applet:debug"; break;
    default:
        fprintf(stderr,"[android-applet] proxy command %u not modeled\n",request->cmd);
        return;
    }
    android_applet_add_object(session,response,name,handler);
}
static void android_applet_service(IpcSession *session,const IpcRequest *request,IpcResponse *response) {
    if(request->cmd==0 && request->domain) {
        android_applet_add_object(session,response,"applet:application-proxy",android_applet_proxy);
    } else if(request->cmd==0) {
        response->move[response->nmove++]=runtime_ipc_new_session("applet:application-proxy",android_applet_proxy,NULL,NULL);
    } else {
        fprintf(stderr,"[android-applet] appletOE command %u not modeled\n",request->cmd);
    }
}

void android_runtime_services_init(void) {
    runtime_ipc_register("lm",android_lm_service,NULL,NULL);
    runtime_ipc_register("apm",android_apm_service,NULL,NULL);
    runtime_ipc_register("appletOE",android_applet_service,NULL,NULL);
    runtime_ipc_register("aoc:u",android_aoc_service,NULL,NULL);
    runtime_ipc_register("ectx:aw",android_ectx_writer,NULL,NULL);
    runtime_ipc_register("pctl",android_pctl_factory,NULL,NULL);
    runtime_ipc_register("pctl:s",android_pctl_factory,NULL,NULL);
    runtime_ipc_register("pctl:a",android_pctl_factory,NULL,NULL);
    runtime_ipc_register("pctl:r",android_pctl_factory,NULL,NULL);
    runtime_ipc_register("time:u",android_time_static_service,NULL,NULL);
}
