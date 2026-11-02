#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#include "../graphics_settings.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
static const size_t sites[] = {0x585e48, 0x585e50, 0x585e54, 0x585e60};
static const uint32_t originals[] = {0x1e220160, 0xbd0cc660, 0x1e220140, 0xbd0cca60};
static void write_text(const char *path, const char *text) {
    FILE *f = fopen(path,"w"); assert(f); assert(fputs(text,f)>=0); assert(!fclose(f));
}
int main(int argc, char **argv) {
    assert(argc >= 2);
    TotkGraphicsSettings s = totk_graphics_defaults(), loaded;
    assert(totk_graphics_valid(&s));
    assert(totk_graphics_load(NULL, &loaded)==0 && loaded.render_width==1280);
    assert(totk_graphics_load(argv[1], &loaded)==0);
    s.render_width=960; s.render_height=540; s.output_width=1920; s.output_height=1080;
    assert(totk_graphics_save(argv[1], &s));
    assert(totk_graphics_load(argv[1], &loaded)==1 && !memcmp(&s,&loaded,sizeof s));
    struct stat st; assert(!stat(argv[1], &st) && (st.st_mode & 0777)==0600);
    /* Above the handheld size: the Switch's docked 1600x900 and 1080p (28 Oct 2026). */
    TotkGraphicsSettings hi = s; hi.render_width=1920; hi.render_height=1080;
    assert(totk_graphics_valid(&hi) && totk_graphics_save(argv[1], &hi));
    assert(totk_graphics_load(argv[1], &loaded)==1 && loaded.render_width==1920 && loaded.render_height==1080);
    hi.render_width=1600; hi.render_height=900; assert(totk_graphics_valid(&hi));
    hi.output_width=1600; hi.output_height=900; assert(totk_graphics_valid(&hi));
    hi.render_width=1920; hi.render_height=900; assert(!totk_graphics_valid(&hi));
    assert(totk_graphics_save(argv[1], &s));
    TotkGraphicsSettings invalid = {.render_width=1, .render_height=2, .output_width=3, .output_height=4};
    assert(!totk_graphics_save(argv[1], &invalid));
    assert(totk_graphics_load(argv[1], &loaded)==1 && !memcmp(&s,&loaded,sizeof s));
    const char *bad[] = {"", "V2 1280 720 1280 720", "TOTK_GRAPHICS_V1 960 540 1920", "TOTK_GRAPHICS_V1 960 540 1920 1080 junk", "TOTK_GRAPHICS_V1 800 600 1280 720", "TOTK_GRAPHICS_V1 -1280 720 1280 720", "TOTK_GRAPHICS_V1 4294968576 720 1280 720", "TOTK_GRAPHICS_V1 999999999999999999999999999999 720 1280 720"};
    for (size_t i=0;i<sizeof bad/sizeof *bad;++i) {
        write_text(argv[1],bad[i]);
        assert(totk_graphics_load(argv[1],&loaded)==-1 && loaded.render_width==1280 && loaded.output_height==720);
    }
    assert(!totk_graphics_save("/no-such-totk-directory/graphics.cfg", &s));
    size_t file_size=0x600001, data=(file_size+0x3fff)&~(size_t)0x3fff, size=data+0x4000;
    uint8_t *mem=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0); assert(mem!=MAP_FAILED);
    for(unsigned i=0;i<4;++i)memcpy(mem+sites[i],&originals[i],4);
    assert(!totk_graphics_patch_v143(mem,size,file_size,&invalid));
    assert(!totk_graphics_patch_v143(mem,file_size,file_size,&s));
    uint32_t wrong=0; memcpy(mem+sites[3],&wrong,4);
    assert(!totk_graphics_patch_v143(mem,size,file_size,&s));
    uint32_t word;memcpy(&word,mem+sites[0],4);assert(word==originals[0]);
    memcpy(mem+sites[3],&originals[3],4);
    assert(totk_graphics_patch_v143(mem,size,file_size,&s));
    float resolution[2];memcpy(resolution,mem+data+16,sizeof resolution);
    assert(resolution[0]==960 && resolution[1]==540);
    memcpy(&word,mem+sites[0],4);assert((word&0x9f00001f)==0x90000003);
    int32_t pages=(int32_t)((((word>>5)&0x7ffff)<<2)|((word>>29)&3));
    if(pages&(1<<20))pages|=(int32_t)0xffe00000;
    uintptr_t target=(((uintptr_t)mem+sites[0])&~(uintptr_t)4095)+(int64_t)pages*4096;
    assert(target==(uintptr_t)mem+data);
    assert(!totk_graphics_patch_v143(mem,size,file_size,&s)); // no double patch
    munmap(mem,size);
    if(argc==3) {
        int fd=open(argv[2],O_RDONLY);assert(fd>=0 && !fstat(fd,&st));
        file_size=(size_t)st.st_size;data=(file_size+0x3fff)&~(size_t)0x3fff;size=data+0x4000;
        mem=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);assert(mem!=MAP_FAILED);
        size_t done=0;
        while(done<file_size){ssize_t n=read(fd,mem+done,file_size-done);assert(n>0);done+=(size_t)n;}
        close(fd);
        assert(totk_graphics_patch_v143(mem,size,file_size,&s));
        memcpy(resolution,mem+data+16,sizeof resolution);assert(resolution[0]==960 && resolution[1]==540);
        munmap(mem,size);
        puts("PASS: local rewritten 1.4.3 instruction guard and patch bytes (no guest execution)");
    }
    puts("PASS: graphics settings persistence, malformed inputs, failure preservation and version-guarded resolution patch; ASan/UBSan");
    return 0;
}
