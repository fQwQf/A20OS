/* x86_64 diagnostic regression; run alone in a fresh guest with runner state
 * preservation. 950..999 are absent from this A20OS Linux syscall table. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#define CAP (1024 * 1024)
static char first[CAP], repeat[CAP], last[CAP];
static int fail(int line) { fprintf(stderr,"PERF_SNAPSHOT_FAIL line=%d errno=%d\n",line,errno);return 1; }
static size_t read_all(int fd,char *buf) {
    size_t n=0;
    while(n<CAP-1) {
        size_t want=CAP-1-n;
        if(want>37)want=37;
        ssize_t r=read(fd,buf+n,want);
        if(r<0) { perror("read"); exit(1); }
        if(!r) { buf[n]=0;return n; }
        n+=(size_t)r;
    }
    fprintf(stderr,"snapshot exceeded bound\n");exit(1);
}
static size_t snapshot(const char *path,char *buf) {
    int fd=open(path,O_RDONLY);if(fd<0) {perror("open snapshot");exit(1);}
    size_t n=read_all(fd,buf);if(close(fd))exit(1);return n;
}
static unsigned long long value(const char *buf,const char *key) {
    char needle[80];snprintf(needle,sizeof(needle),"%s: ",key);
    const char *p=strstr(buf,needle);
    return p?strtoull(p+strlen(needle),NULL,10):0;
}
static int state(const char *buf) {
    const char *p=strstr(buf,"collection_enabled_before_snapshot: ");
    return p?(int)value(buf,"collection_enabled_before_snapshot"):-1;
}
/* perf uses canonical generic syscall IDs; x86_64 getpid(39) maps to 172. */
int main(int argc,char **argv) {
    int strict=argc==1;
    if(!strict&&(argc!=2||strcmp(argv[1],"--observe")))return 2;
    struct stat st;
    if(stat("/proc/a20/perf",&st)) {perror("stat");return fail(__LINE__);}
    long long initial_stat_size=(long long)st.st_size;
    for(unsigned i=0;i<1000;i++)if(syscall(SYS_getpid)<=0)return fail(__LINE__);
    int fd=open("/proc/a20/perf",O_RDONLY);if(fd<0)return fail(__LINE__);
    size_t initial=read_all(fd,first);
    if(lseek(fd,0,SEEK_SET)!=0)return fail(__LINE__);
    size_t again=read_all(fd,repeat);
    if(close(fd)||initial!=again||memcmp(first,repeat,initial))return fail(__LINE__);
    int stat_no_arm=st.st_size==0&&state(first)==0&&value(first,"syscall_172_count")<1000;
    /* Verify these rejected numbers are still profiled without invoking any
     * registered operation; the source table has no entries in 950..999 (1000+ has aliases). */
    for(long nr=950;nr<1000;nr++) {
        errno=0;
        long rejected=syscall(nr,0,0,0,0,0,0);
        if(rejected!=-1||errno!=ENOSYS) {
            fprintf(stderr,"unexpected reserved syscall nr=%ld ret=%ld errno=%d\n",nr,rejected,errno);
            return fail(__LINE__);
        }
    }
    size_t complete=snapshot("/proc/a20/perf",last);
    int tail=complete>4096;
    for(unsigned nr=950;nr<1000;nr++) {
        char key[64];snprintf(key,sizeof(key),"syscall_%u_count",nr);
        if(value(last,key)!=1)tail=0;
    }
    unsigned long long before=value(last,"syscall_172_count");
    fd=open("/proc/a20/perf",O_WRONLY);
    if(fd<0||write(fd,"0",1)!=1||close(fd))return fail(__LINE__);
    /* A write-only open with no write must not restart collection. */
    fd=open("/proc/a20/perf",O_WRONLY);if(fd<0||close(fd))return fail(__LINE__);
    if(stat("/proc/a20/perf",&st))return fail(__LINE__);
    for(unsigned i=0;i<1000;i++)if(syscall(SYS_getpid)<=0)return fail(__LINE__);
    snapshot("/proc/a20/perf",last);
    int write_no_arm=state(last)==0&&value(last,"syscall_172_count")==before;
    struct stat locks;
    if(stat("/proc/a20/lock_contention",&locks))return fail(__LINE__);
    size_t lock_bytes=snapshot("/proc/a20/lock_contention",repeat);
    int footer=strstr(repeat,"lock_counters: registered=")!=NULL;
    printf("PERF_SNAPSHOT initial_bytes=%zu initial_stat_size=%lld initial_state=%d initial_getpid=%llu full_bytes=%zu tail_950_999=%d writeonly_no_arm=%d lock_bytes=%zu lock_stat_size=%lld lock_footer=%d strict=%d\n",
           initial,initial_stat_size,state(first),value(first,"syscall_172_count"),complete,
           tail,write_no_arm,lock_bytes,(long long)locks.st_size,footer,strict);
    if(strict&&(!stat_no_arm||!write_no_arm||!tail||!footer||locks.st_size!=0))return fail(__LINE__);
    if(strict)puts("PERF_SNAPSHOT: PASS chunked_read=1 seek_stable=1 stat_no_arm=1 writeonly_no_arm=1 complete_tail=1 lock_footer=1");
    return 0;
}
