/* NativeActivity entry for the actual 1.4.3 loader. Rendering stops at the
 * first unimplemented NVN call; this app never substitutes synthetic pixels. */
#define _GNU_SOURCE
#include <android/log.h>
#include <android/choreographer.h>
#include <android/window.h>
#include <android_native_app_glue.h>
#include "../thor_gamepad.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <android/asset_manager.h>
#include "../android_video_menu.h"
#include "video_menu_assets.h"

extern int android_loader_run(int argc, char **argv);
extern void android_front_set_window(ANativeWindow *window);
extern void android_front_frame_tick(void);
extern void android_front_set_focus(int focused);
static struct android_app *video_app;
static int loader_started;
static _Atomic int loader_done;

static void *log_output(void *argument) {
    int fd=*(int *)argument;
    free(argument);
    char line[1024];
    size_t used=0;
    for (;;) {
        char c;
        ssize_t count=read(fd,&c,1);
        if (count<=0) break;
        if (c=='\n' || used==sizeof line-1) {
            line[used]=0;
            __android_log_print(ANDROID_LOG_INFO,"TOTKGameLoader","%s",line);
            used=0;
        } else line[used++]=c;
    }
    close(fd);
    return NULL;
}

static void redirect_output(void) {
    int fds[2];
    if (pipe(fds)) return;
    int *reader=malloc(sizeof *reader);
    if (!reader) { close(fds[0]);close(fds[1]);return; }
    *reader=fds[0];
    pthread_t thread;
    if (pthread_create(&thread,NULL,log_output,reader)) {
        free(reader);close(fds[0]);close(fds[1]);return;
    }
    pthread_detach(thread);
    dup2(fds[1],STDOUT_FILENO);
    dup2(fds[1],STDERR_FILENO);
    close(fds[1]);
    setvbuf(stdout,NULL,_IONBF,0);
    setvbuf(stderr,NULL,_IONBF,0);
}

/* Diagnostic switches without rebuilding: KEY=VALUE lines in <data>/env
 * (pushed with adb) are set before the loader starts. */
static void load_env_file(const char *directory) {
    char path[512];
    snprintf(path,sizeof path,"%s/env",directory);
    FILE *f=fopen(path,"r");
    if(!f) return;
    char line[512];
    while(fgets(line,sizeof line,f)) {
        line[strcspn(line,"\r\n")]=0;
        char *eq=strchr(line,'=');
        if(line[0]=='#'||!eq||eq==line) continue;
        *eq=0;
        setenv(line,eq+1,1);
        __android_log_print(ANDROID_LOG_INFO,"TOTKGameLoader","env %s=%s",line,eq+1);
    }
    fclose(f);
}

static void make_parents(char *path) {
    for(char *p=path+1;*p;p++)if(*p=='/') {*p=0;if(mkdir(path,0700)&&errno!=EEXIST)abort();*p='/';}
}
/* Not inlined: its 64 KB copy buffer must not stay in run_loader's frame,
   below which the game runs. */
__attribute__((noinline)) static void install_menu_assets(struct android_app *app) {
    char directory[4096];
    if(!app->activity->internalDataPath || snprintf(directory,sizeof directory,"%s/romfs-overlay",app->activity->internalDataPath)>=(int)sizeof directory)abort();
    for(size_t i=0;i<VIDEO_MENU_ASSET_COUNT;i++) {
        char source[4096],destination[4096],temp[4120];
        if(snprintf(source,sizeof source,"romfs-overlay/%s",video_menu_assets[i].path)>=(int)sizeof source ||
            snprintf(destination,sizeof destination,"%s/%s",directory,video_menu_assets[i].path)>=(int)sizeof destination)abort();
        make_parents(destination);snprintf(temp,sizeof temp,"%s.tmp",destination);
        AAsset *asset=AAssetManager_open(app->activity->assetManager,source,AASSET_MODE_STREAMING);
        if(!asset || (uint64_t)AAsset_getLength64(asset)!=video_menu_assets[i].size)abort();
        FILE *out=fopen(temp,"wb");if(!out)abort();
        char bytes[65536];int n;
        while((n=AAsset_read(asset,bytes,sizeof bytes))>0)if(fwrite(bytes,1,(size_t)n,out)!=(size_t)n)abort();
        AAsset_close(asset);if(n<0 || fflush(out)||fsync(fileno(out))||fclose(out)||rename(temp,destination))abort();
    }
    setenv("TOTK_ROMFS_OVERLAY",directory,1);
}
int android_video_menu_request(void) {
    struct android_app *app=video_app;if(!app)return 0;
    JNIEnv *env=NULL;JavaVM *vm=app->activity->vm;int detach=0;
    if((*vm)->GetEnv(vm,(void **)&env,JNI_VERSION_1_6)!=JNI_OK) {
        if((*vm)->AttachCurrentThread(vm,&env,NULL)!=JNI_OK)return 0;detach=1;
    }
    jclass cls=(*env)->GetObjectClass(env,app->activity->clazz);
    jmethodID show=cls?(*env)->GetMethodID(env,cls,"requestVideoSettingsFromGame","()V"):NULL;
    if(show)(*env)->CallVoidMethod(env,app->activity->clazz,show);
    int ok=show && !(*env)->ExceptionCheck(env);
    if((*env)->ExceptionCheck(env))(*env)->ExceptionClear(env);
    if(cls)(*env)->DeleteLocalRef(env,cls);
    if(detach)(*vm)->DetachCurrentThread(vm);
    return ok;
}

/* The video panel's language row (language.cfg): the text language nn::oe::GetDesiredLanguage reports,
   in place of the system's. Read once at start, as the game reads it once. Empty or absent: the system. */
static void apply_language_choice(const char *data) {
    static const char *const known[]={"en-US","en-GB","fr","fr-CA","de","it","es","es-419","nl","pt-BR","ru","ja","ko","zh-Hans","zh-Hant"};
    char path[4096],code[16]={0};
    if(!data || snprintf(path,sizeof path,"%s/language.cfg",data)>=(int)sizeof path)return;
    FILE *f=fopen(path,"r");if(!f)return;
    size_t n=fread(code,1,sizeof code-1,f);fclose(f);
    while(n && (code[n-1]=='\n'||code[n-1]==' '))code[--n]=0;
    for(size_t i=0;i<sizeof known/sizeof *known;i++)if(!strcmp(code,known[i])) {
        setenv("TOTK_LANG",code,1);
        __android_log_print(ANDROID_LOG_INFO,"TOTKGameLoader","text language from the video panel: %s",code);
        return;
    }
    if(n)__android_log_print(ANDROID_LOG_WARN,"TOTKGameLoader","language.cfg holds an unknown language '%s'; following the system",code);
}

static void *run_loader(void *argument) {
    struct android_app *app=argument;
    const char *directory="/data/local/tmp/totk-android-v143";
    load_env_file(directory);
    char graphics[4096];
    if(!app->activity->internalDataPath || snprintf(graphics,sizeof graphics,"%s/graphics.cfg",app->activity->internalDataPath)>=(int)sizeof graphics)abort();
    android_video_menu_settings_path(graphics);
    android_video_menu_startup();
    apply_language_choice(app->activity->internalDataPath);
    const char *video=getenv("TOTK_VIDEO_MENU");
    /* TOTK_VIDEO_MENU=2: only the L3+R3 shortcut; the Options row (Bootup pack override) crashed the game, so no overlay. */
    if(!video || (strcmp(video,"0") && strcmp(video,"2")))install_menu_assets(app);
    char romfs[256];
    snprintf(romfs,sizeof romfs,"%s/romfs",directory);
    setenv("TOTK_ROMFS",romfs,1);
    /* The Thor is a handheld: the game's handheld mode renders at 720p. */
    setenv("TOTK_OPERATION_MODE","handheld",0);
    char verified[512];
    if (app->activity->internalDataPath &&
        snprintf(verified,sizeof verified,"%s/romfs-verified",app->activity->internalDataPath)<(int)sizeof verified)
        setenv("TOTK_ROMFS_VERIFIED_CACHE",verified,1);
    char profile[512];
    if (app->activity->internalDataPath &&
        snprintf(profile,sizeof profile,"%s/user-data",app->activity->internalDataPath)<(int)sizeof profile)
        setenv("TOTK_PROFILE_DIR",profile,1);
    char shaders[512];
    if (app->activity->internalDataPath &&
        snprintf(shaders,sizeof shaders,"%s/shaders",app->activity->internalDataPath)<(int)sizeof shaders)
        setenv("TOTK_SHADER_CACHE",shaders,0);
    char drivers[512];
    if (app->activity->internalDataPath &&
        snprintf(drivers,sizeof drivers,"%s/gpu_drivers/",app->activity->internalDataPath)<(int)sizeof drivers)
        setenv("TOTK_VK_DRIVER_DIR",drivers,0);
    char *arguments[]={"totk-game-activity",(char *)directory,"boot",NULL};
    int result=android_loader_run(3,arguments);
    __android_log_print(ANDROID_LOG_ERROR,"TOTKGameLoader","loader returned %d",result);
    atomic_store(&loader_done,1);
    ALooper_wake(app->looper);
    return NULL;
}

static void frame_callback(int64_t frame_time,void *argument) {
    (void)frame_time;
    struct android_app *app=argument;
    android_front_frame_tick();
    if (!app->destroyRequested && !atomic_load(&loader_done))
        AChoreographer_postFrameCallback64(AChoreographer_getInstance(),frame_callback,app);
}

static void command(struct android_app *app,int32_t cmd) {
    if (cmd==APP_CMD_INIT_WINDOW && app->window) {
        android_front_set_window(app->window);
        if (!loader_started) {
            __android_log_print(ANDROID_LOG_INFO,"TOTKGameLoader","native window %dx%d; starting game loader worker",
                ANativeWindow_getWidth(app->window),ANativeWindow_getHeight(app->window));
            /* The game's main thread runs on this host stack. TOTK's loading
               path alone nears the 1 MB bionic default (one frame is 44 KB). */
            pthread_t thread;pthread_attr_t attr;
            int failed=pthread_attr_init(&attr)||pthread_attr_setstacksize(&attr,8u<<20)||
                pthread_create(&thread,&attr,run_loader,app);
            pthread_attr_destroy(&attr);
            if (failed) {
                __android_log_print(ANDROID_LOG_ERROR,"TOTKGameLoader","game loader worker creation failed");
                atomic_store(&loader_done,1);return;
            }
            pthread_detach(thread);loader_started=1;
        }
    } else if (cmd==APP_CMD_TERM_WINDOW) {
        android_front_set_window(NULL);
        /* A diagnostic loader cannot yet suspend/recreate NVN resources. */
        if(loader_started) _exit(0);
    } else if (cmd==APP_CMD_GAINED_FOCUS) android_front_set_focus(1);
    else if (cmd==APP_CMD_LOST_FOCUS) android_front_set_focus(0);
}

static int32_t input(struct android_app *app,AInputEvent *event) {
    (void)app;
    if(android_video_menu_input_blocked())return 0;
    if(AInputEvent_getType(event)==AINPUT_EVENT_TYPE_KEY) {
        /* L3 + R3 pressed together (both downs within 300 ms, no key repeats) open the video settings panel
         * (unless TOTK_VIDEO_MENU=0). Held buttons repeat on this controller, so repeats must not count. */
        static int64_t l3_down,r3_down,last_open;
        int code=AKeyEvent_getKeyCode(event),action=AKeyEvent_getAction(event);
        int64_t now=AKeyEvent_getEventTime(event);
        if(code==AKEYCODE_BUTTON_THUMBL||code==AKEYCODE_BUTTON_THUMBR) {
            int64_t *mine=code==AKEYCODE_BUTTON_THUMBL?&l3_down:&r3_down,*other=code==AKEYCODE_BUTTON_THUMBL?&r3_down:&l3_down;
            if(action==AKEY_EVENT_ACTION_UP)*mine=0;
            else if(action==AKEY_EVENT_ACTION_DOWN&&AKeyEvent_getRepeatCount(event)==0) {
                *mine=now;
                const char *video=getenv("TOTK_VIDEO_MENU");
                if(*other&&now-*other<=300000000ll&&now-last_open>1500000000ll&&(!video||strcmp(video,"0"))) {
                    l3_down=r3_down=0;last_open=now;android_video_menu_request();return 1;
                }
            }
        }
    }
    return android_gamepad_event(event);
}

void android_main(struct android_app *app) {
    video_app=app;
    redirect_output();
    ANativeActivity_setWindowFlags(app->activity,AWINDOW_FLAG_KEEP_SCREEN_ON,0);
    app->onAppCmd=command;
    app->onInputEvent=input;
    AChoreographer_postFrameCallback64(AChoreographer_getInstance(),frame_callback,app);
    __android_log_print(ANDROID_LOG_INFO,"TOTKGameLoader","waiting for Android native window");
    while (!app->destroyRequested && !atomic_load(&loader_done)) {
        int events;
        struct android_poll_source *source=NULL;
        if (ALooper_pollOnce(-1,NULL,&events,(void **)&source)>=0 && source)
            source->process(app,source);
    }
    if(loader_started) _exit(0);
}
