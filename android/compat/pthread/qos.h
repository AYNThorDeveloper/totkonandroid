#pragma once
#include <pthread.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
typedef int qos_class_t;
#define QOS_CLASS_UNSPECIFIED 0
#define QOS_CLASS_USER_INTERACTIVE 1
static inline int pthread_attr_set_qos_class_np(pthread_attr_t *attr, qos_class_t qos, int relative_priority) {
    (void)attr; (void)qos; (void)relative_priority; return 0;
}
static inline int pthread_set_qos_class_self_np(qos_class_t qos, int relative_priority) {
    (void)qos; (void)relative_priority; return 0;
}
static inline int pthread_get_qos_class_np(pthread_t thread, qos_class_t *qos, int *relative_priority) {
    (void)thread; if (qos) *qos=QOS_CLASS_UNSPECIFIED; if (relative_priority) *relative_priority=0; return 0;
}
static inline uintptr_t pthread_mach_thread_np(pthread_t thread) { (void)thread; return (uintptr_t)syscall(SYS_gettid); }
static inline int android_pthread_threadid(uint64_t *thread_id) {
    if (!thread_id) return -1; *thread_id=(uint64_t)syscall(SYS_gettid); return 0;
}
#ifndef pthread_threadid_np
#define pthread_threadid_np(thread, thread_id) android_pthread_threadid((thread_id))
#endif
static inline void *pthread_get_stackaddr_np(pthread_t thread) {
    pthread_attr_t attr;void *base=NULL;size_t size=0;
    if(pthread_getattr_np(thread,&attr)==0){pthread_attr_getstack(&attr,&base,&size);pthread_attr_destroy(&attr);}
    return (char *)base+size;
}
static inline size_t pthread_get_stacksize_np(pthread_t thread) {
    pthread_attr_t attr;void *base=NULL;size_t size=0;
    if(pthread_getattr_np(thread,&attr)==0){pthread_attr_getstack(&attr,&base,&size);pthread_attr_destroy(&attr);}
    return size;
}
