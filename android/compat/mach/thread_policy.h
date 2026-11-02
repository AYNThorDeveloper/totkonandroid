/* Mach scheduling and accounting queries used by shared diagnostics.
 * Android has no equivalent policies: the calls report KERN_FAILURE so
 * callers take their documented "not in effect / unavailable" paths.
 * Except with TOTK_ANDROID_RT=1: a time-constraint request then raises the calling thread's priority and joins it to an
 * Android performance hint session (android_compat.c android_thread_time_constraint). */
#pragma once
#include <mach/mach.h>

typedef int thread_flavor_t;
typedef int thread_policy_flavor_t;
typedef int *thread_policy_t;
typedef int *thread_info_t;
typedef int task_flavor_t;
typedef int *task_info_t;
typedef int boolean_t;
typedef struct { uint32_t period, computation, constraint; boolean_t preemptible; } thread_time_constraint_policy_data_t;
typedef struct { int seconds, microseconds; } time_value_t;
typedef struct { time_value_t user_time, system_time; int cpu_usage, policy, run_state, flags, suspend_count, sleep_time; } thread_basic_info_data_t;
typedef struct { uint64_t pth_user_time, pth_system_time; int pth_cpu_usage, pth_policy, pth_run_state, pth_flags,
    pth_sleep_time, pth_curpri, pth_priority, pth_maxpriority; char pth_name[64]; } thread_extended_info_data_t;
typedef struct { uint64_t virtual_size, phys_footprint; } task_vm_info_data_t;

#define THREAD_TIME_CONSTRAINT_POLICY 2
#define THREAD_TIME_CONSTRAINT_POLICY_COUNT 4
#define THREAD_BASIC_INFO 3
#define THREAD_BASIC_INFO_COUNT 10
#define THREAD_EXTENDED_INFO 5
#define THREAD_EXTENDED_INFO_COUNT 28
#define TASK_VM_INFO 22
#define TASK_VM_INFO_COUNT 4

static inline mach_port_t mach_thread_self(void) { return 0; }
kern_return_t android_thread_time_constraint(uint32_t period_ns, uint32_t computation_ns);
void android_thread_work_report(uint64_t cpu_ns);   /* once per frame per declared thread: its CPU time since the last */
void android_thread_affinity(const char *env_name);   /* pin the calling thread to the CPUs listed in that environment variable ("7", "3-6", "3,4,7"); nothing when unset */
static inline kern_return_t thread_policy_set(mach_port_t t, thread_policy_flavor_t f, thread_policy_t p,
    mach_msg_type_number_t n) {
    (void)t; (void)n;   /* the callers always mean the calling thread */
    if (f == THREAD_TIME_CONSTRAINT_POLICY && p) {
        const thread_time_constraint_policy_data_t *tc = (const thread_time_constraint_policy_data_t *)p;
        return android_thread_time_constraint(tc->period, tc->computation);
    }
    return KERN_FAILURE;
}
static inline kern_return_t thread_policy_get(mach_port_t t, thread_policy_flavor_t f, thread_policy_t p,
    mach_msg_type_number_t *n, boolean_t *def) { (void)t; (void)f; (void)p; (void)n; if (def) *def = 1; return KERN_FAILURE; }
static inline kern_return_t thread_info(mach_port_t t, thread_flavor_t f, thread_info_t i, mach_msg_type_number_t *n) {
    (void)t; (void)f; (void)i; (void)n; return KERN_FAILURE; }
static inline kern_return_t task_info(mach_port_t t, task_flavor_t f, task_info_t i, mach_msg_type_number_t *n) {
    (void)t; (void)f; (void)i; (void)n; return KERN_FAILURE; }
/* Absolute deadline in mach_absolute_time units (nanoseconds on Android). */
kern_return_t mach_wait_until(uint64_t deadline);
