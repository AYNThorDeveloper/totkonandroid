/* Exercise the real variant cache through construction, reuse and a simulated process restart. */
#include <pthread.h>
#include <sys/socket.h>
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif
#include "../nvn_vk_shader.c"
#include <assert.h>
VkDevice nvk_dev;
NvkState nvk_S;
uint32_t nvk_texture_info(int id){(void)id;return 0;}
const p_set_rt *nvk_render_targets(void){return NULL;}
int totk_depth_near_minus_w(void){return 1;}
void nvk_pipeline_warm_signal(void){}
uint32_t nvk_memory_type(uint32_t bits,VkMemoryPropertyFlags flags){(void)bits;(void)flags;return 0;}
_Noreturn void nvk_die(const char *m){fprintf(stderr,"%s\n",m);abort();}
static unsigned made;
static VkResult module(VkDevice d,const VkShaderModuleCreateInfo *i,const VkAllocationCallbacks *a,VkShaderModule *o){(void)d;(void)i;(void)a;*o=(VkShaderModule)(uintptr_t)++made;return VK_SUCCESS;}
static VkResult layout(VkDevice d,const VkDescriptorSetLayoutCreateInfo *i,const VkAllocationCallbacks *a,VkDescriptorSetLayout *o){(void)d;(void)i;(void)a;*o=(VkDescriptorSetLayout)(uintptr_t)1;return VK_SUCCESS;}
static VkResult buffer(VkDevice d,const VkBufferCreateInfo *i,const VkAllocationCallbacks *a,VkBuffer *o){(void)d;(void)i;(void)a;*o=(VkBuffer)(uintptr_t)1;return VK_SUCCESS;}
static void requirements(VkDevice d,VkBuffer b,VkMemoryRequirements *r){(void)d;(void)b;*r=(VkMemoryRequirements){.size=256,.memoryTypeBits=1};}
static VkResult allocate(VkDevice d,const VkMemoryAllocateInfo *i,const VkAllocationCallbacks *a,VkDeviceMemory *o){(void)d;(void)a;*o=(VkDeviceMemory)(uintptr_t)calloc(1,i->allocationSize);return *o?VK_SUCCESS:VK_ERROR_OUT_OF_HOST_MEMORY;}
static VkResult bind_memory(VkDevice d,VkBuffer b,VkDeviceMemory m,VkDeviceSize s){(void)d;(void)b;(void)m;(void)s;return VK_SUCCESS;}
static VkResult map(VkDevice d,VkDeviceMemory m,VkDeviceSize o,VkDeviceSize s,VkMemoryMapFlags f,void **p){(void)d;(void)s;(void)f;*p=(uint8_t *)(uintptr_t)m+o;return VK_SUCCESS;}
static void unmap(VkDevice d,VkDeviceMemory m){(void)d;(void)m;}
static void destroy_module(VkDevice d,VkShaderModule m,const VkAllocationCallbacks *a){(void)d;(void)m;(void)a;}
static void destroy_layout(VkDevice d,VkDescriptorSetLayout l,const VkAllocationCallbacks *a){(void)d;(void)l;(void)a;}
PFN_vkCreateShaderModule vkCreateShaderModule=module;
PFN_vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout=layout;
PFN_vkCreateBuffer vkCreateBuffer=buffer;
PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements=requirements;
PFN_vkAllocateMemory vkAllocateMemory=allocate;
PFN_vkBindBufferMemory vkBindBufferMemory=bind_memory;
PFN_vkMapMemory vkMapMemory=map;
PFN_vkUnmapMemory vkUnmapMemory=unmap;
PFN_vkDestroyShaderModule vkDestroyShaderModule=destroy_module;
PFN_vkDestroyDescriptorSetLayout vkDestroyDescriptorSetLayout=destroy_layout;
static void forget(void){
    for(unsigned i=0;i<4096;++i){Variant *v=variants[i];while(v){Variant *next=v->next;
        free((void *)(uintptr_t)v->shader->c1_memory);free(v->shader->b);free(v->shader);free(v);v=next;}variants[i]=NULL;}
}
int main(int argc,char **argv){
    assert(argc==3);setenv("TOTK_SHADER_CACHE",argv[2],1);
    size_t n=0;uint8_t *data=read_file(argv[1],&n);assert(shader_data_valid(data,n));
    uint8_t constants[64];memset(constants,0x5a,sizeof constants);
    NvkShader parent={.stage=1,.c1=(VkBuffer)(uintptr_t)1,.c1_memory=(VkDeviceMemory)(uintptr_t)constants,.c1_size=sizeof constants};
    strcpy(parent.variant,"0123456789abcdef0123456789abcdef-0123456789ab");
    char path[640];snprintf(path,sizeof path,"%s/%s.vks",argv[2],parent.variant);write_file(path,data,n);free(data);
    float d1=0.25f/65535.0f,d2=0.5f/65535.0f;
    NvkShader *a=nvk_shader_depth_offset(&parent,d1),*b=nvk_shader_depth_offset(&parent,d2);
    assert(a&&b&&a!=b&&strcmp(a->variant,b->variant));assert(made==2);
    assert(nvk_shader_depth_offset(&parent,d1)==a&&made==2);
    char saved[80];strcpy(saved,a->variant);forget();assert(!unlink(path));
    assert(nvk_shader_preload(saved,1));NvkShader *pre=nvk_shader_find_variant(saved);assert(pre&&!pre->c1);
    assert(nvk_shader_depth_offset(&parent,d1)==pre&&pre->c1);
    assert(!memcmp((void *)(uintptr_t)pre->c1_memory,constants,sizeof constants));
    /* Long parent identities differing beyond the old truncation point must stay distinct. */
    char p1[80],p2[80],k1[80],k2[80];memset(p1,'a',79);p1[79]=0;strcpy(p2,p1);p2[78]='b';
    depth_variant_name(k1,p1,d1,1);depth_variant_name(k2,p2,d1,1);assert(strcmp(k1,k2));
    depth_variant_name(k2,p1,d1,0);assert(strcmp(k1,k2));
    forget();puts("depth variant cache: pass (bias identity, disk persistence without parent, preload constants, long parents, snap mode)");
    /* A variant met without a bridge is noted once, as the bridge request plus the full key (tools/seed_missing.py). */
    const char *key="0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    Str rq={0};put(&rq,"{\"variant\": \"%.32s-0123456789ab\", \"stage\": \"ps\", \"code_hex\": \"00\", \"c1\": \"\", \"attr\": {}}\n",key);
    missing_note("0123456789abcdef0123456789abcdef-0123456789ab",key,&rq);
    snprintf(path,sizeof path,"%s/missing/0123456789abcdef0123456789abcdef-0123456789ab.json",argv[2]);
    size_t nn=0;uint8_t *note=read_file(path,&nn);assert(note);
    char want[512];snprintf(want,sizeof want,"%.*s, \"key\": \"%s\"}\n",(int)(rq.n-2),rq.p,key);
    assert(nn==strlen(want)&&!memcmp(note,want,nn));free(note);
    rq.p[10]='X';missing_note("0123456789abcdef0123456789abcdef-0123456789ab",key,&rq);   /* noted once: not rewritten */
    note=read_file(path,&nn);assert(note&&!memcmp(note,want,nn));free(note);free(rq.p);
    puts("missing variant note: pass (request plus key, written once)");
}
