/* In-process Android NativeAOT translator; keeps the existing .vks cache wire format. */
#include "nvn_vk_translate.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#ifdef __ANDROID__
#include <android/log.h>
#define LOG(...) __android_log_print(ANDROID_LOG_INFO,"TOTKShader",__VA_ARGS__)
#else
#define LOG(...) do { fprintf(stderr,__VA_ARGS__); fputc('\n',stderr); } while (0)
#endif
static pthread_once_t once=PTHREAD_ONCE_INIT;
static void *library;
static uint8_t *(*translate)(const char *,size_t,const char *,size_t *);
static void (*release)(void *);
static void load(void) {
    const char *override=getenv("TOTK_SHADER_TRANSLATOR_LIBRARY");
    library=dlopen(override&&*override?override:"libtotkshader.so",RTLD_NOW|RTLD_LOCAL);
    if(!library){LOG("local shader translator unavailable: %s",dlerror());return;}
    *(void **)(&translate)=dlsym(library,"totk_shader_translate");
    *(void **)(&release)=dlsym(library,"totk_shader_free");
    if(!translate||!release){LOG("local shader translator has incompatible exports");translate=NULL;release=NULL;return;}
    LOG("local shader translator ready; new variants are compiled on this device");
}
uint8_t *nvk_translate_local(const char *request,const char *cache,size_t *length) {
    *length=0;
    const char *setting=getenv("TOTK_SHADER_LOCAL");
    if(setting&&!strcmp(setting,"0"))return NULL;
    pthread_once(&once,load);if(!translate)return NULL;
    char work[768];int n=snprintf(work,sizeof work,"%s/translate-work",cache);
    if(n<0||(size_t)n>=sizeof work){LOG("shader work directory is too long");return NULL;}
    mkdir(work,0700);
    size_t bytes=0;uint8_t *reply=translate(request,strlen(request),work,&bytes),*copy=NULL;
    if(reply&&bytes>=3&&bytes<=32u*1024u*1024u&&!memcmp(reply,"OK ",3)) {
        copy=malloc(bytes);if(copy){memcpy(copy,reply,bytes);*length=bytes;}
    }else if(reply&&bytes)LOG("local shader translation failed: %.*s",(int)(bytes<400?bytes:400),(const char *)reply);
    else LOG("local shader translation returned no reply");
    if(reply)release(reply);return copy;
}
