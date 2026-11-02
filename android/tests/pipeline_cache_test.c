#include <pthread.h>
static int test_setname(pthread_t t,const char *n) { (void)t;(void)n;return 0; }
#define pthread_setname_np test_setname
#include "../nvn_vk_pipeline_cache.c"
#include <assert.h>
VkDevice nvk_dev;
VkPhysicalDevice nvk_phys;
_Noreturn void nvk_die(const char *m) { fprintf(stderr,"%s\n",m);abort(); }
static void props(VkPhysicalDevice d,VkPhysicalDeviceProperties *p) {
    (void)d; memset(p,0,sizeof *p); p->vendorID=0x5143; p->deviceID=740; p->driverVersion=123;
    memset(p->pipelineCacheUUID,0x5a,VK_UUID_SIZE);
}
static uint8_t blob[64];
static VkResult create(VkDevice d,const VkPipelineCacheCreateInfo *ci,const VkAllocationCallbacks *a,VkPipelineCache *p) {
    (void)d;(void)a;
    if(ci->initialDataSize) assert(valid_header(ci->pInitialData,ci->initialDataSize));
    *p=(VkPipelineCache)(uintptr_t)1;return VK_SUCCESS;
}
static VkResult get_data(VkDevice d,VkPipelineCache p,size_t *size,void *out) {
    (void)d;(void)p;
    if(out) { assert(*size>=sizeof blob); memcpy(out,blob,sizeof blob); }
    *size=sizeof blob;return VK_SUCCESS;
}
PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties=props;
PFN_vkCreatePipelineCache vkCreatePipelineCache=create;
PFN_vkGetPipelineCacheData vkGetPipelineCacheData=get_data;
int main(int argc,char **argv) {
    assert(argc==2);setenv("TOTK_SHADER_CACHE",argv[1],1);
    props(VK_NULL_HANDLE,&properties);
    uint32_t header[]={32,VK_PIPELINE_CACHE_HEADER_VERSION_ONE,properties.vendorID,properties.deviceID};
    memcpy(blob,header,sizeof header);memcpy(blob+16,properties.pipelineCacheUUID,VK_UUID_SIZE);
    assert(valid_header(blob,sizeof blob));blob[16]^=1;assert(!valid_header(blob,sizeof blob));blob[16]^=1;
    assert(!valid_header(blob,16));
    assert(nvk_pipeline_cache_begin()!=VK_NULL_HANDLE);nvk_pipeline_cache_end();
    struct stat st; int written=0;
    for(unsigned i=0;i<60;i++) { struct timespec delay={0,50000000};nanosleep(&delay,NULL);if(!stat(path,&st)&&st.st_size==sizeof blob) {written=1;break;} }
    assert(written);FILE *f=fopen(path,"rb");assert(f);uint8_t saved[64];assert(fread(saved,1,sizeof saved,f)==sizeof saved);fclose(f);
    assert(!memcmp(saved,blob,sizeof blob));
    puts("pipeline cache: pass (device/UUID rejection, load, asynchronous atomic persistence)");
}
