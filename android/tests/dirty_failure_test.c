#include <sys/mman.h>
#include <sys/wait.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>
static int fail_mode;
static int injected_mprotect(void *a, size_t n, int p) {
    if ((fail_mode==1 && p==PROT_READ) || (fail_mode==2 && (p&PROT_WRITE))) { errno=ENOMEM; return -1; }
    return mprotect(a,n,p);
}
#define mprotect injected_mprotect
#include "../../games/totk/third_party/d2r_gfx/gfx/nvn/nvn_dirty.c"
void dm_fault_hook_register(int (*f)(void *,int,void *)) { (void)f; }
void dm_guest_write_prep_register(void (*f)(void *,size_t)) { (void)f; }
int main(void) {
    size_t n=(size_t)getpagesize();
    void *mem=mmap(NULL,n,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0); assert(mem!=MAP_FAILED);
    nvn_dirty_add_range(mem,n); uint64_t stamp=nvn_dirty_now();
    nvn_dirty_protect(mem,n); assert(find((uintptr_t)mem)->state[0]==PG_PROT);
    assert(on_fault(mem,1,NULL)); *(volatile char *)mem=1;
    assert(nvn_dirty_since(mem,n,stamp));
    stamp=nvn_dirty_now(); fail_mode=1; nvn_dirty_protect(mem,n);
    assert(find((uintptr_t)mem)->state[0]==PG_NONE); *(volatile char *)mem=2;
    assert(nvn_dirty_since(mem,n,stamp)); /* never pretend failed protection is clean */
    nvn_dirty_remove_range(mem); nvn_dirty_add_range(mem,n); fail_mode=0;
    nvn_dirty_protect(mem,n);
    pid_t child=fork(); assert(child>=0);
    if(!child) { fail_mode=2; on_fault(mem,1,NULL); _exit(0); }
    int status; assert(waitpid(child,&status,0)==child); assert(WIFEXITED(status)&&WEXITSTATUS(status)==127);
    nvn_dirty_remove_range(mem); munmap(mem,n);
    puts("dirty tracking: pass (normal fault, failed protection stays dirty, failed unprotect exits without loop)");
}
