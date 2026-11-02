#pragma once
#include <stddef.h>
#include <stdint.h>

typedef uintptr_t mach_port_t;
typedef uintptr_t vm_map_t;
typedef uint64_t mach_vm_address_t;
typedef uint64_t mach_vm_size_t;
typedef uint64_t mach_vm_offset_t;
typedef uint64_t vm_address_t;
typedef uint64_t vm_size_t;
typedef uint64_t vm_offset_t;
typedef uint64_t memory_object_offset_t;
typedef uintptr_t memory_object_t;
typedef unsigned natural_t;
typedef unsigned mach_msg_type_number_t;
typedef int kern_return_t;
typedef int vm_prot_t;
typedef int vm_inherit_t;
typedef int vm_region_flavor_t;
typedef int *vm_region_info_t;
typedef int *vm_region_recurse_info_t;

typedef struct {
    vm_prot_t protection;
    vm_prot_t max_protection;
    unsigned user_tag;
    unsigned pages_resident;
    unsigned pages_dirtied;
    unsigned pages_swapped_out;
} vm_region_basic_info_data_64_t;

typedef struct {
    unsigned pages_resident;
    unsigned pages_dirtied;
    unsigned pages_swapped_out;
} vm_region_submap_info_data_64_t;

typedef struct { uintptr_t __x[31]; uintptr_t __pc, __lr, __fp; } arm_thread_state64_t;
typedef arm_thread_state64_t *thread_state_t;

#define KERN_SUCCESS 0
#define KERN_INVALID_ADDRESS 1
#define KERN_FAILURE 5
#define MACH_PORT_NULL ((mach_port_t)0)
#define FALSE 0
#define TRUE 1
#define MEMORY_OBJECT_NULL ((memory_object_t)0)
#define VM_PROT_NONE 0
#define VM_PROT_READ 1
#define VM_PROT_WRITE 2
#define VM_PROT_EXECUTE 4
#define VM_PROT_ALL 7
#define VM_FLAGS_ANYWHERE 0
#define VM_FLAGS_FIXED 1
#define VM_FLAGS_OVERWRITE 2
#define VM_INHERIT_DEFAULT 0
#define VM_INHERIT_NONE 2
#define VM_REGION_BASIC_INFO_64 9
#define VM_REGION_BASIC_INFO_COUNT_64 9
#define VM_REGION_SUBMAP_INFO_COUNT_64 18
#define ARM_THREAD_STATE64 6
#define ARM_THREAD_STATE64_COUNT 68

static inline mach_port_t mach_task_self(void) { return 0; }
static inline uintptr_t arm_thread_state64_get_pc(arm_thread_state64_t s) { return s.__pc; }
static inline uintptr_t arm_thread_state64_get_lr(arm_thread_state64_t s) { return s.__lr; }
static inline uintptr_t arm_thread_state64_get_fp(arm_thread_state64_t s) { return s.__fp; }

kern_return_t mach_port_deallocate(mach_port_t task, mach_port_t port);
kern_return_t mach_vm_allocate(vm_map_t task, mach_vm_address_t *address, mach_vm_size_t size, int flags);
kern_return_t mach_vm_deallocate(vm_map_t task, mach_vm_address_t address, mach_vm_size_t size);
kern_return_t mach_vm_map(vm_map_t task, mach_vm_address_t *address, mach_vm_size_t size,
                          mach_vm_offset_t mask, int flags, memory_object_t object,
                          memory_object_offset_t offset, int copy, vm_prot_t current,
                          vm_prot_t maximum, vm_inherit_t inherit);
kern_return_t mach_vm_region(vm_map_t task, mach_vm_address_t *address, mach_vm_size_t *size,
                             vm_region_flavor_t flavor, vm_region_info_t info,
                             mach_msg_type_number_t *count, mach_port_t *object);
kern_return_t mach_vm_read_overwrite(vm_map_t task, mach_vm_address_t address,
                                     mach_vm_size_t size, mach_vm_address_t data,
                                     mach_vm_size_t *out_size);
kern_return_t vm_region_64(vm_map_t task, vm_address_t *address, vm_size_t *size,
                           vm_region_flavor_t flavor, vm_region_info_t info,
                           mach_msg_type_number_t *count, mach_port_t *object);
kern_return_t vm_read_overwrite(vm_map_t task, vm_address_t address, vm_size_t size,
                                vm_address_t data, vm_size_t *out_size);
kern_return_t vm_region_recurse_64(vm_map_t task, vm_address_t *address, vm_size_t *size,
                                   natural_t *depth, vm_region_recurse_info_t info,
                                   mach_msg_type_number_t *count);
/* Sharing remap (copy=FALSE) only: aliases pages of a MAP_SHARED mapping.
 * Private anonymous sources cannot be aliased on Linux and fail. */
kern_return_t mach_vm_remap(vm_map_t task, mach_vm_address_t *address, mach_vm_size_t size,
                            mach_vm_offset_t mask, int flags, vm_map_t source_task,
                            mach_vm_address_t source, int copy, vm_prot_t *current,
                            vm_prot_t *maximum, vm_inherit_t inherit);
kern_return_t thread_suspend(mach_port_t thread);
kern_return_t thread_resume(mach_port_t thread);
kern_return_t thread_get_state(mach_port_t thread, int flavor, thread_state_t state,
                               mach_msg_type_number_t *count);
extern vm_size_t vm_page_size;
