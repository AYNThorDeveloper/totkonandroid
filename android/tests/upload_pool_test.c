#include <assert.h>
#include <stdio.h>
#include "../nvn_vk_image.c"

VkDevice nvk_dev;
VkPhysicalDevice nvk_phys;
static uint64_t pending = 1, completed;
static unsigned allocations;

uint64_t nvk_pending_fence(void) { return pending; }
uint64_t nvk_completed_fence(void) { return completed; }
uint32_t nvk_memory_type(uint32_t bits, VkMemoryPropertyFlags flags) {
    assert(bits & 1); assert(flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT); return 0;
}
_Noreturn void nvk_die(const char *message) { fprintf(stderr, "%s\n", message); abort(); }

static void properties(VkPhysicalDevice d, VkPhysicalDeviceProperties *p) {
    (void)d; memset(p, 0, sizeof *p); p->limits.optimalBufferCopyOffsetAlignment = 256;
}
static VkResult create_buffer(VkDevice d, const VkBufferCreateInfo *ci,
        const VkAllocationCallbacks *a, VkBuffer *out) {
    (void)d; (void)a; assert(ci->usage == VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    *out = (VkBuffer)(uintptr_t)++allocations; return VK_SUCCESS;
}
static void requirements(VkDevice d, VkBuffer b, VkMemoryRequirements *r) {
    (void)d; (void)b; r->size = UPLOAD_CHUNK_MIN; r->alignment = 256; r->memoryTypeBits = 1;
}
static VkResult alloc(VkDevice d, const VkMemoryAllocateInfo *i,
        const VkAllocationCallbacks *a, VkDeviceMemory *out) {
    (void)d; (void)a; *out = (VkDeviceMemory)calloc(1, (size_t)i->allocationSize);
    assert(*out); return VK_SUCCESS;
}
static VkResult bind(VkDevice d, VkBuffer b, VkDeviceMemory m, VkDeviceSize off) {
    (void)d; (void)b; (void)m; assert(off == 0); return VK_SUCCESS;
}
static VkResult map_memory(VkDevice d, VkDeviceMemory m, VkDeviceSize off,
        VkDeviceSize size, VkMemoryMapFlags flags, void **out) {
    (void)d; (void)size; (void)flags; *out = (uint8_t *)m + off; return VK_SUCCESS;
}
static void destroy_buffer(VkDevice d, VkBuffer b, const VkAllocationCallbacks *a) {
    (void)d; (void)b; (void)a;
}
PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties = properties;
PFN_vkCreateBuffer vkCreateBuffer = create_buffer;
PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements = requirements;
PFN_vkAllocateMemory vkAllocateMemory = alloc;
PFN_vkBindBufferMemory vkBindBufferMemory = bind;
PFN_vkMapMemory vkMapMemory = map_memory;
PFN_vkDestroyBuffer vkDestroyBuffer = destroy_buffer;

int main(void) {
    VkBuffer first, second, third, fourth;
    VkDeviceSize a, b, c, d;
    uint8_t *ma, *mb, *mc, *md;
    assert(upload_reserve(100, &first, &a, &ma));
    assert(upload_reserve(100, &second, &b, &mb));
    assert(first == second && a == 0 && b >= 100 && b % 768 == 0);
    assert(ma + b == mb);
    pending = 2;
    assert(upload_reserve(100, &third, &c, &mc));
    assert(third != first && c == 0 && allocations == 2);
    completed = 1; pending = 3;
    assert(upload_reserve(100, &fourth, &d, &md));
    assert(fourth == first && d == 0 && allocations == 2);
    assert(!upload_reserve(UPLOAD_CHUNK_MAX + 1, &fourth, &d, &md));
    for (UploadChunk *chunk = upload_chunks; chunk;) {
        UploadChunk *next = chunk->next;
        free((void *)chunk->memory); free(chunk); chunk = next;
    }
    puts("upload pool: pass (disjoint offsets, GPU fence, reuse, oversized fallback)");
}
