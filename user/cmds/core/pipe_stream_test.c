#define _GNU_SOURCE
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"PIPE_STREAM: FAIL line=%d errno=%d: %s\n",__LINE__,errno,#x); exit(1); } } while (0)
static void child_ok(pid_t pid) {
    int status; CHECK(waitpid(pid,&status,0)==pid); CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);
}
static void read_all(int fd, void *data, size_t count) {
    size_t done=0; while(done<count) { ssize_t n=read(fd,(char *)data+done,count-done); CHECK(n>0);done+=(size_t)n; }
}
static void wrap_resize(void) {
    int p[2]; char a[8192],b[8192]; memset(a,'a',sizeof(a));CHECK(pipe2(p,O_NONBLOCK)==0);
    CHECK(fcntl(p[1],F_SETPIPE_SZ,8192)==8192); CHECK(write(p[1],a,8192)==8192);
    CHECK(write(p[1],a,1)==-1&&errno==EAGAIN);CHECK(read(p[0],b,4096)==4096);
    memset(a,'b',3000);CHECK(write(p[1],a,3000)==3000);
    CHECK(fcntl(p[1],F_SETPIPE_SZ,4096)==-1&&errno==EBUSY);
    CHECK(read(p[0],b,sizeof(b))==7096);
    for(int i=0;i<7096;i++)CHECK(b[i]==(i<4096?'a':'b'));
    CHECK(fcntl(p[1],F_SETPIPE_SZ,4096)==4096);CHECK(write(p[1],a,4096)==4096);
    CHECK(read(p[0],b,1024)==1024);
    int grown=fcntl(p[1],F_SETPIPE_SZ,8192);
    CHECK(grown>=8192||(grown<0&&errno==EPERM));
    CHECK(read(p[0],b,sizeof(b))==3072);CHECK(!memcmp(b,a+1024,3072));
    CHECK(read(p[0],b,1)==-1&&errno==EAGAIN);close(p[1]);CHECK(read(p[0],b,1)==0);close(p[0]);
}
static void atomic_writers(void) {
    int p[2];pid_t ids[4];CHECK(pipe(p)==0);CHECK(fcntl(p[1],F_SETPIPE_SZ,4096)==4096);
    for(int i=0;i<4;i++) {CHECK((ids[i]=fork())>=0);if(!ids[i]) {
        close(p[0]);char record[256];memset(record,'A'+i,sizeof(record));
        for(int j=0;j<128;j++)if(write(p[1],record,sizeof(record))!=sizeof(record))_exit(1);
        close(p[1]);_exit(0);
    }}
    close(p[1]);unsigned counts[4]={0};char record[256];
    for(int i=0;i<512;i++) {read_all(p[0],record,sizeof(record));CHECK(record[0]>='A'&&record[0]<='D');
        counts[record[0]-'A']++;for(int j=1;j<256;j++)CHECK(record[j]==record[0]);}
    CHECK(read(p[0],record,1)==0);close(p[0]);for(int i=0;i<4;i++){CHECK(counts[i]==128);child_ok(ids[i]);}
}
static void resize_wakeup(void) {
    int p[2],done[2];char data[8192];memset(data,'a',4096);CHECK(pipe(p)==0);CHECK(pipe(done)==0);
    CHECK(fcntl(p[1],F_SETPIPE_SZ,4096)==4096);
    if(fcntl(p[1],F_SETPIPE_SZ,8192)<0&&errno==EPERM) {
        puts("PIPE_STREAM: SKIP resize-wakeup (host pipe quota)");
        close(p[0]);close(p[1]);close(done[0]);close(done[1]);return;
    }
    CHECK(fcntl(p[1],F_SETPIPE_SZ,4096)==4096);CHECK(write(p[1],data,4096)==4096);
    pid_t child=fork();CHECK(child>=0);if(!child){close(p[0]);close(done[0]);memset(data,'b',4096);
        if(write(done[1],"s",1)!=1||write(p[1],data,4096)!=4096||write(done[1],"d",1)!=1)_exit(1);
        _exit(0);}
    close(done[1]);char c;read_all(done[0],&c,1);usleep(50000);
    CHECK(fcntl(p[1],F_SETPIPE_SZ,8192)>=8192);
    struct pollfd pollfd={.fd=done[0],.events=POLLIN};CHECK(poll(&pollfd,1,2000)==1);
    read_all(done[0],&c,1);CHECK(c=='d');read_all(p[0],data,8192);
    for(int i=0;i<8192;i++)CHECK(data[i]==(i<4096?'a':'b'));
    close(done[0]);close(p[0]);close(p[1]);child_ok(child);
}
static void readiness(void) {
    int p[2],ep=epoll_create1(0);CHECK(ep>=0);CHECK(pipe2(p,O_NONBLOCK)==0);
    struct epoll_event event={.events=EPOLLIN|EPOLLET,.data.fd=p[0]},got;
    CHECK(epoll_ctl(ep,EPOLL_CTL_ADD,p[0],&event)==0);
    for(int i=0;i<128;i++) {char c;CHECK(write(p[1],"x",1)==1);CHECK(epoll_wait(ep,&got,1,1000)==1);
        CHECK(got.events&EPOLLIN);CHECK(read(p[0],&c,1)==1&&c=='x');CHECK(epoll_wait(ep,&got,1,0)==0);}
    close(p[1]);CHECK(epoll_wait(ep,&got,1,1000)==1);CHECK(got.events&EPOLLHUP);
    close(ep);close(p[0]);
}
static void broken_pipe(void) {
    int p[2];CHECK(pipe(p)==0);close(p[0]);signal(SIGPIPE,SIG_IGN);CHECK(write(p[1],"x",1)==-1&&errno==EPIPE);close(p[1]);
}
int main(void) {
    alarm(20);wrap_resize();atomic_writers();resize_wakeup();readiness();broken_pipe();
    puts("PIPE_STREAM: PASS wrap resize atomic-writers resize-wakeup epoll eof epipe");return 0;
}
