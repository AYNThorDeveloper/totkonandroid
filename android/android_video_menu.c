/* 1.4.3 original Options-menu integration. The row is in the game's own
 * OptionSetTable, not an Android launcher button. The original layout engine
 * creates/selects/scrolls it; only its Decide callback opens the video panel. */
#define _GNU_SOURCE
#include "android_video_menu.h"
#include "graphics_settings.h"
#include <errno.h>
#include <jni.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern _Noreturn void runtime_fail(const char *message);
extern void *runtime_hle_wrap(void *function);
extern void runtime_input_clear(void);
extern int android_frame_cap_allowed(unsigned cap);
extern int android_frame_cap_set(unsigned cap);
extern uint64_t android_frame_pacing_period(void);
extern void android_menu_count_hook(void), android_menu_setup_hook(void), android_menu_activate_hook(void);
void *android_menu_count_original, *android_menu_setup_original, *android_menu_activate_original;
void *android_menu_get_button, *android_menu_normalize_host, *android_menu_register_host, *android_menu_activate_host;
static _Atomic uintptr_t video_button;
static _Atomic int blocked;
static pthread_mutex_t settings_mutex=PTHREAD_MUTEX_INITIALIZER;
static char settings_path[4096];
static TotkGraphicsSettings saved, active;
static unsigned output_width,output_height;
static int settings_ready;
__attribute__((weak)) int android_video_menu_request(void) { return 0; }

/* Runtime OptionSet = {OptionType, ButtonType, GameDataHash, pad,
 * ChoiceCount, pad, Choices*, ChoiceMask, DisplayConditionHash}. */
static void normalize_options(void *table) {
    uint32_t count; uintptr_t data;
    memcpy(&count,table,4);memcpy(&data,(uint8_t *)table+8,8);
    if(count>128 || (count&&!data)) runtime_fail("unexpected original Options-table layout");
    for(unsigned i=0;i<count;i++) {
        uint8_t *p=(uint8_t *)data+i*40;int32_t type;uint32_t button,choices;
        memcpy(&type,p,4);memcpy(&button,p+4,4);memcpy(&choices,p+16,4);
        /* None+Selection never exists in the original table. The private
         * Bootup override adds precisely this one marker row. */
        if(type==-1 && button==1 && choices==1) {
            int32_t video=16;memcpy(p,&video,4);
            fprintf(stderr,"[android-video-menu] original Options row registered (id 16)\n");
        }
    }
}
static void register_button(void *control,void *button) {
    int32_t id;memcpy(&id,(uint8_t *)control+0x48,4);
    if(id==16 && button) atomic_store(&video_button,(uintptr_t)button);
}
static int activate_video(void *screen,void *button) {
    (void)screen;
    if(!button || (uintptr_t)button!=atomic_load(&video_button))return 0;
    if(!atomic_load(&blocked) && !android_video_menu_request())
        fprintf(stderr,"[android-video-menu] video panel unavailable\n");
    return 1; /* Never send this custom ID into TOTK's normal option actions. */
}
void android_video_menu_block_input(int open) {
    atomic_store(&blocked,open!=0);runtime_input_clear();
}
int android_video_menu_input_blocked(void) {return atomic_load(&blocked);}
static void jump(uint8_t *at,uintptr_t target) {
    uint32_t code[]={0x58000050,0xd61f0200}; /* ldr x16,+8; br x16 */
    memcpy(at,code,8);memcpy(at+8,&target,8);
}
int android_video_menu_patch(uint8_t *base,uint64_t text_end,uint64_t ro_off) {
    const char *overlay=getenv("TOTK_ROMFS_OVERLAY"),*enabled=getenv("TOTK_VIDEO_MENU");
    if(!overlay || !*overlay || (enabled&&!strcmp(enabled,"0")))return 0;
    static const struct {uint64_t off;uint32_t original[4];} sites[]={
        {0x2089c14,{0xd10143ff,0xa9017bfd,0xa9025ff8,0xa90357f6}},
        {0x212bf04,{0xd10283ff,0xa9057bfd,0xa90667fa,0xa9075ff8}},
        {0x218d274,{0xa9bc7bfd,0xa9015ff8,0xa90257f6,0xa9034ff4}},
    };
    uint64_t stub=(text_end+0x100+15)&~15ull;
    if(stub+96>ro_off)runtime_fail("video menu trampolines do not fit after text");
    for(unsigned i=0;i<sizeof sites/sizeof *sites;i++) {
        if(sites[i].off+16>text_end || memcmp(base+sites[i].off,sites[i].original,16))
            runtime_fail("1.4.3 original Options instruction guard mismatch");
    }
    for(unsigned i=0;i<96;i++)if(base[stub+i])runtime_fail("video menu trampoline padding not empty");
    void **trampolines[]={&android_menu_count_original,&android_menu_setup_original,&android_menu_activate_original};
    void *hooks[]={(void *)android_menu_count_hook,(void *)android_menu_setup_hook,(void *)android_menu_activate_hook};
    for(unsigned i=0;i<3;i++) {
        uint8_t *trampoline=base+stub+i*32;
        memcpy(trampoline,base+sites[i].off,16);jump(trampoline+16,(uintptr_t)base+sites[i].off+16);
        *trampolines[i]=trampoline;
    }
    android_menu_get_button=base+0x212c1d0;
    android_menu_normalize_host=runtime_hle_wrap((void *)normalize_options);
    android_menu_register_host=runtime_hle_wrap((void *)register_button);
    android_menu_activate_host=runtime_hle_wrap((void *)activate_video);
    for(unsigned i=0;i<3;i++)jump(base+sites[i].off,(uintptr_t)hooks[i]);
    fprintf(stderr,"[android-video-menu] 1.4.3 original Options hooks installed (3 guarded entries)\n");
    return 1;
}
void android_video_menu_settings_path(const char *path) {
    pthread_mutex_lock(&settings_mutex);
    if(!settings_ready) {
        if(!path || strlen(path)>=sizeof settings_path)runtime_fail("video settings path invalid");
        strcpy(settings_path,path);
        int loaded=totk_graphics_load(path,&saved);
        if(loaded<0)fprintf(stderr,"[android-video-menu] invalid settings file; using defaults\n");
        if(!loaded) saved.output_width=1920,saved.output_height=1080; /* preserve earlier game output */
        active=saved;settings_ready=1;
    }
    pthread_mutex_unlock(&settings_mutex);
}
void android_video_menu_startup(void) {
    pthread_mutex_lock(&settings_mutex);
    if(!settings_ready || !totk_graphics_save(settings_path,&saved))runtime_fail("video settings startup persistence failed");
    setenv("TOTK_GRAPHICS_CONFIG",settings_path,1);
    char output[40];snprintf(output,sizeof output,"%ux%u",saved.output_width,saved.output_height);
    setenv("TOTK_VIDEO_OUTPUT",output,1);
    if(!getenv("TOTK_FRAME_CAP") && !android_frame_cap_set(saved.frame_cap))runtime_fail("saved FPS cap requires realtime/60 FPS controller");
    pthread_mutex_unlock(&settings_mutex);
}
void android_video_menu_active_output(unsigned w,unsigned h) {pthread_mutex_lock(&settings_mutex);output_width=w;output_height=h;pthread_mutex_unlock(&settings_mutex);}
JNIEXPORT void JNICALL Java_org_macnativegames_totk_gameprobe_VideoSettingsActivity_nativeInit(JNIEnv *e,jclass cls,jstring path) {
    (void)cls;const char *s=(*e)->GetStringUTFChars(e,path,NULL);if(!s)return;
    android_video_menu_settings_path(s);(*e)->ReleaseStringUTFChars(e,path,s);
}
JNIEXPORT jstring JNICALL Java_org_macnativegames_totk_gameprobe_VideoSettingsActivity_nativeRead(JNIEnv *e,jclass cls) {
    (void)cls;char json[512];pthread_mutex_lock(&settings_mutex);
    uint64_t period=android_frame_pacing_period();unsigned cap=period?(unsigned)((1000000000ull+period/2)/period):0;
    snprintf(json,sizeof json,"{\"render\":[%u,%u],\"output\":[%u,%u],\"fps\":%u,\"activeRender\":[%u,%u],\"activeOutput\":[%u,%u],\"activeFps\":%u}",
        saved.render_width,saved.render_height,saved.output_width,saved.output_height,saved.frame_cap,
        active.render_width,active.render_height,output_width,output_height,cap);
    pthread_mutex_unlock(&settings_mutex);return (*e)->NewStringUTF(e,json);
}
JNIEXPORT jboolean JNICALL Java_org_macnativegames_totk_gameprobe_VideoSettingsActivity_nativeSave(JNIEnv *e,jclass cls,jint rw,jint rh,jint ow,jint oh,jint fps) {
    (void)e;(void)cls;TotkGraphicsSettings s={(unsigned)rw,(unsigned)rh,(unsigned)ow,(unsigned)oh,(unsigned)fps};
    pthread_mutex_lock(&settings_mutex);
    int ok=totk_graphics_valid(&s)&&android_frame_cap_allowed(s.frame_cap)&&totk_graphics_save(settings_path,&s);
    if(ok){saved=s;android_frame_cap_set(s.frame_cap);}
    pthread_mutex_unlock(&settings_mutex);return ok?JNI_TRUE:JNI_FALSE;
}
JNIEXPORT void JNICALL Java_org_macnativegames_totk_gameprobe_VideoSettingsActivity_nativeMenu(JNIEnv *e,jclass cls,jboolean open) {
    (void)e;(void)cls;android_video_menu_block_input(open);
}
