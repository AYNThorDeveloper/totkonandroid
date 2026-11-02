/* Thor probe for nvn_vk_backend.c: can exported Vulkan memory (a dma-buf) be
 * mapped at an arbitrary fixed CPU address with coherent CPU<->GPU visibility?
 * Usage on device: vk_dmabuf_alias_probe <memory type index>. Prints PASS/FAIL
 * per step and CPU bandwidth through the alias. See VULKAN_BACKEND.md. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <vulkan/vulkan.h>
#define SZ (1u<<20)
#define CK(x) do{VkResult r_=(x);if(r_){printf("FAIL %s -> %d\n",#x,r_);return 1;}}while(0)
int main(int argc,char**argv){
 int want=argc>1?atoi(argv[1]):6;
 VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_3};
 VkInstanceCreateInfo ci={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
 VkInstance in;CK(vkCreateInstance(&ci,NULL,&in));
 uint32_t n=1;VkPhysicalDevice pd;vkEnumeratePhysicalDevices(in,&n,&pd);
 float pr=1;VkDeviceQueueCreateInfo q={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueCount=1,.pQueuePriorities=&pr};
 const char*ext[]={"VK_KHR_external_memory_fd"};
 VkDeviceCreateInfo dc={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&q,.enabledExtensionCount=1,.ppEnabledExtensionNames=ext};
 VkDevice d;CK(vkCreateDevice(pd,&dc,NULL,&d));VkQueue qu;vkGetDeviceQueue(d,0,0,&qu);
 VkExternalMemoryBufferCreateInfo eb={.sType=VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
 VkBufferCreateInfo bc={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.pNext=&eb,.size=SZ,.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
 VkBuffer b;CK(vkCreateBuffer(d,&bc,NULL,&b));
 VkMemoryRequirements mr;vkGetBufferMemoryRequirements(d,b,&mr);
 printf("buffer memoryTypeBits=%#x align=%llu\n",mr.memoryTypeBits,(unsigned long long)mr.alignment);
 if(!(mr.memoryTypeBits&(1u<<want))){printf("FAIL memtype %d not allowed\n",want);return 1;}
 VkMemoryDedicatedAllocateInfo ded={.sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,.buffer=b};
 VkExportMemoryAllocateInfo ex={.sType=VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,.pNext=getenv("NODED")?NULL:&ded,.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
 VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.pNext=&ex,.allocationSize=mr.size,.memoryTypeIndex=want};
 VkDeviceMemory m;CK(vkAllocateMemory(d,&ai,NULL,&m));CK(vkBindBufferMemory(d,b,m,0));
 PFN_vkGetMemoryFdKHR gfd=(PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(d,"vkGetMemoryFdKHR");
 VkMemoryGetFdInfoKHR gi={.sType=VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,.memory=m,.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
 int fd=-1;CK(gfd(d,&gi,&fd));
 char link[256]={0},path[64];snprintf(path,sizeof path,"/proc/self/fd/%d",fd);readlink(path,link,sizeof link-1);
 struct stat st;fstat(fd,&st);printf("exported fd=%d -> %s size=%lld\n",fd,link,(long long)st.st_size);
 uint8_t *vk=NULL;CK(vkMapMemory(d,m,0,SZ,0,(void**)&vk));
 /* "game" address: inside an anonymous reservation, replaced in place. */
 uint8_t *res=mmap(NULL,4*SZ,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 uint8_t *at=res+SZ;memset(at,0x5a,SZ);
 uint8_t *fx=mmap(at,SZ,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_FIXED,fd,0);
 if(fx==MAP_FAILED){printf("FAIL mmap exported fd at fixed address: %s\n",strerror(errno));return 1;}
 printf("PASS mmap at fixed %p\n",(void*)fx);
 for(unsigned i=0;i<SZ/4;i++)((uint32_t*)fx)[i]=i*2654435761u;
 int same=!memcmp(fx,vk,SZ);printf("%s CPU fixed-alias writes visible through vkMapMemory view\n",same?"PASS":"FAIL");
 VkCommandPoolCreateInfo pc={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};VkCommandPool cp;CK(vkCreateCommandPool(d,&pc,NULL,&cp));
 VkCommandBufferAllocateInfo ca={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=cp,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
 VkCommandBuffer cb;CK(vkAllocateCommandBuffers(d,&ca,&cb));
 VkCommandBufferBeginInfo bi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};CK(vkBeginCommandBuffer(cb,&bi));
 VkBufferCopy cpy={.srcOffset=0,.dstOffset=SZ/2,.size=4096};vkCmdCopyBuffer(cb,b,b,1,&cpy);
 vkCmdFillBuffer(cb,b,SZ-4096,4096,0xdeadbeefu);
 CK(vkEndCommandBuffer(cb));
 VkFenceCreateInfo fc={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};VkFence f;CK(vkCreateFence(d,&fc,NULL,&f));
 VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cb};CK(vkQueueSubmit(qu,1,&si,f));
 CK(vkWaitForFences(d,1,&f,VK_TRUE,UINT64_MAX));
 int copied=!memcmp(fx+SZ/2,fx,4096);int filled=1;for(unsigned i=0;i<1024;i++)if(((uint32_t*)(fx+SZ-4096))[i]!=0xdeadbeefu)filled=0;
 printf("%s GPU copy read CPU data and result visible at fixed address\n",copied?"PASS":"FAIL");
 printf("%s GPU fill visible at fixed address\n",filled?"PASS":"FAIL");

 { struct timespec t0,t1; static uint8_t tmp[SZ]; uint8_t *plain=malloc(SZ); memset(plain,1,SZ); volatile uint64_t sink=0;
   double r[4];
   for(int k=0;k<4;k++){ uint8_t *src=k<2?fx:plain; clock_gettime(CLOCK_MONOTONIC,&t0);
     for(int it=0;it<64;it++){ if(k%2==0){ uint64_t a=0; for(unsigned i=0;i<SZ/8;i++)a+=((uint64_t*)src)[i]; sink+=a;} else memcpy(src,tmp,SZ);} 
     clock_gettime(CLOCK_MONOTONIC,&t1); r[k]=64.0*SZ/((t1.tv_sec-t0.tv_sec)+(t1.tv_nsec-t0.tv_nsec)/1e9)/1e9; }
   printf("bandwidth GB/s: alias read %.2f write %.2f | malloc read %.2f write %.2f\n",r[0],r[1],r[2],r[3]); free(plain); }
 for(unsigned i=0;i<SZ/4;i++)((uint32_t*)fx)[i]=i*2654435761u;   /* the bandwidth test overwrote the pattern */
 /* Second alias of the same memory elsewhere (virtual pool style). */
 uint8_t *fx2=mmap(res+3*SZ,SZ,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_FIXED,fd,0);
 printf("%s second fixed alias of same memory\n",fx2!=MAP_FAILED&&!memcmp(fx2,fx,SZ)?"PASS":"FAIL");
 /* After the Vulkan memory is freed the CPU mapping must keep its pages (fd + mapping hold a reference). */
 vkUnmapMemory(d,m);vkDestroyBuffer(d,b,NULL);vkFreeMemory(d,m,NULL);close(fd);
 printf("%s mapping valid after vkFreeMemory\n",((uint32_t*)fx)[7]==7u*2654435761u?"PASS":"FAIL");
 return 0;}
