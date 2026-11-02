/* Standalone translator exercise; no Vulkan device, app install or game launch. */
#include "../nvn_vk_translate.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
int main(int argc,char **argv) {
    if(argc!=4){fprintf(stderr,"usage: probe request.json cache-dir reply.vks\n");return 2;}
    FILE *f=fopen(argv[1],"rb");if(!f)return 3;
    if(fseek(f,0,SEEK_END))return 3;
    long n=ftell(f);if(n<=0||n>64*1024*1024)return 3;
    rewind(f);char *rq=calloc((size_t)n+1,1);if(!rq)return 3;
    if(fread(rq,1,(size_t)n,f)!=(size_t)n)return 3;fclose(f);
    mkdir(argv[2],0700);size_t len;unsigned char *reply=nvk_translate_local(rq,argv[2],&len);free(rq);
    if(!reply)return 4;f=fopen(argv[3],"wb");if(!f){free(reply);return 5;}
    int ok=fwrite(reply,1,len,f)==len;free(reply);if(fclose(f))ok=0;
    if(ok)printf("translated %zu bytes\n",len);return ok?0:5;
}
