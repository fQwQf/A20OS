#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/perf.h"
#include "sys/syscall.h"
typedef int pf_type_t;
syscall_prof_t sys_prof[SYSCALL_PROFILE_MAX];
static size_t live,peak,allocs,frees,fail_at,required;
static int mode;
static void *kmalloc(size_t n) {
    allocs++;
    if(allocs==fail_at)return NULL;
    size_t *p=malloc(n+sizeof(size_t));assert(p);*p=n;
    live+=n;if(live>peak)peak=live;return p+1;
}
static void kfree(void *p) {
    if(!p)return;
    size_t *h=(size_t *)p-1;live-=*h;frees++;free(h);
}
static int generate_content(pf_type_t type,int pid,char *buf,size_t cap) {
    (void)type;(void)pid;
    if(mode==0)return (int)a20_perf_format(buf,cap);
    if(mode==2)return -EIO;
    size_t n=required<cap-1?required:cap-1;
    memset(buf,'x',n);buf[n]=0;return (int)n;
}
/* PRODUCTION_GROW */
static uint64_t mock_ticks=100;
static uint64_t timer_get_ticks(void) { return mock_ticks; }
/* PRODUCTION_PROFILE */
static void reset_allocator(void) {
    assert(live==0);peak=allocs=frees=fail_at=0;
}
int main(void) {
    for(unsigned c=0;c<A20_PERF_COUNTER_COUNT;c++)g_a20_perf_percpu[0].values[c]=UINT64_MAX;
    for(unsigned i=0;i<SYSCALL_PROFILE_MAX;i++)sys_prof[i]=(syscall_prof_t){UINT64_MAX,UINT64_MAX};
    char *out=NULL;size_t len=0;
    g_a20_perf_enabled=0;
    char tiny[8];memset(tiny,'g',sizeof(tiny));
    assert(a20_perf_format(tiny,0)==0&&tiny[0]=='g'&&g_a20_perf_enabled==0);
    assert(a20_perf_format(tiny,4)==3&&tiny[3]==0&&tiny[4]=='g'&&g_a20_perf_enabled==0);
    assert(procfs_diag_content_alloc(0,0,4096,&out,&len)==0);
    assert(len>65536&&len<131072&&out[len-1]=='\n'&&out[len]==0);
    assert(strstr(out,"collection_enabled_before_snapshot: 0\n"));
    assert(strstr(out,"syscall_1023_ticks: 18446744073709551615\n"));
    assert(g_a20_perf_enabled==1);
    printf("DIAG_MAX: length=%zu allocated_peak=%zu attempts=%zu complete=1\n",len,peak,allocs);
    kfree(out);assert(live==0&&allocs==frees);
    mode=1;
    const size_t lengths[]={0,1,4094,4095,4096,8190,8191,8192,100000,1048574};
    for(unsigned i=0;i<sizeof(lengths)/sizeof(lengths[0]);i++) {
        reset_allocator();required=lengths[i];
        assert(procfs_diag_content_alloc(0,0,4096,&out,&len)==0);
        assert(len==required&&out[len]==0);kfree(out);
        assert(allocs==frees&&live==0&&peak<=PROCFS_DIAG_MAX_CONTENT);
    }
    for(unsigned i=0;i<2;i++) {
        reset_allocator();required=i?2*PROCFS_DIAG_MAX_CONTENT:PROCFS_DIAG_MAX_CONTENT-1;
        assert(procfs_diag_content_alloc(0,0,4096,&out,&len)==-EOVERFLOW);
        assert(out==NULL&&len==0&&allocs==frees&&live==0);
    }
    for(unsigned fail=1;fail<=6;fail++) {
        reset_allocator();required=100000;fail_at=fail;
        assert(procfs_diag_content_alloc(0,0,4096,&out,&len)==-ENOMEM);
        assert(out==NULL&&len==0&&allocs==frees+1&&live==0);
    }
    reset_allocator();mode=2;
    assert(procfs_diag_content_alloc(0,0,4096,&out,&len)==-EIO);
    assert(out==NULL&&len==0&&allocs==frees&&live==0);
    assert(procfs_diag_content_alloc(0,0,1,&out,&len)==-EINVAL);
    assert(procfs_diag_content_alloc(0,0,PROCFS_DIAG_MAX_CONTENT+1,&out,&len)==-EINVAL);
    memset(sys_prof,0,sizeof(sys_prof));g_a20_perf_enabled=0;
    assert(syscall_profile_now()==0);
    g_a20_perf_enabled=1;
    syscall_profile_record(39,0);
    assert(sys_prof[39].count==0&&sys_prof[39].cycles==0);
    assert(syscall_profile_now()==100);
    mock_ticks=125;
    syscall_profile_record(39,100);
    assert(sys_prof[39].count==1&&sys_prof[39].cycles==25);
    syscall_profile_record(SYSCALL_PROFILE_MAX,100);
    g_a20_perf_enabled=0;syscall_profile_record(39,100);
    assert(sys_prof[39].count==1&&sys_prof[39].cycles==25);
    puts("DIAG_GROW: saturation/bounds/ENOMEM/error cleanup + disabled-entry timestamp PASS");
    return 0;
}
