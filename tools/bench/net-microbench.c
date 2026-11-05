/* Identical Linux-ABI benchmark for native Linux and A20OS. No A20 headers.
 * TCP/UDP echo verifies every byte. Time excludes connection/fork setup.
 * bulk reports ONE direction payload bytes, although echo moves twice that.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void die(const char *s) { perror(s); exit(1); }
static double now(void) { struct timespec t; if(clock_gettime(CLOCK_MONOTONIC,&t)) die("clock"); return t.tv_sec+t.tv_nsec*1e-9; }
static int cmp_double(const void *a,const void *b) { double x=*(const double*)a,y=*(const double*)b;return (x>y)-(x<y); }
static void exact(int fd, void *p, size_t n, int writing) {
    char *b=p;
    while(n) { ssize_t r=writing?send(fd,b,n,MSG_NOSIGNAL):recv(fd,b,n,0);
        if(r<0 && errno==EINTR)continue;
        if(r<=0)die(writing?"send":"recv");
        b+=r;n-=r; }
}
static int sock(int udp) {
    int fd=socket(AF_INET,udp?SOCK_DGRAM:SOCK_STREAM,0); if(fd<0)die("socket");
    struct timeval tv={5,0}; int one=1;
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
    setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv));
    setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
    if(!udp)setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one));
    return fd;
}
static struct sockaddr_in addr(const char *ip,int port) {
    struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons(port)};
    if(inet_pton(AF_INET,ip,&a.sin_addr)!=1)die("address");
    return a;
}
static void server(int udp,int port,int ready) {
    int s=sock(udp);struct sockaddr_in a=addr("0.0.0.0",port);
    if(bind(s,(void*)&a,sizeof(a)))die("bind");
    if(!udp && listen(s,32))die("listen");
    if(ready>=0) { if(write(ready,"R",1)!=1)die("ready");close(ready); }
    else { puts("SERVER_READY");fflush(stdout); }
    char b[65536];
    if(udp) { for(;;) { socklen_t z=sizeof(a);ssize_t n=recvfrom(s,b,sizeof(b),0,(void*)&a,&z);
        if(n<0 && (errno==EINTR||errno==EAGAIN))continue;
        if(n<0)die("recvfrom");
        if(sendto(s,b,n,0,(void*)&a,z)!=n)die("sendto"); } }
    signal(SIGCHLD,SIG_IGN);
    for(;;) { int c=accept(s,0,0);if(c<0 && (errno==EINTR||errno==EAGAIN))continue;if(c<0)die("accept");
        pid_t p=fork();if(p<0)die("fork");if(p) {close(c);continue;}
        close(s);int one=1;setsockopt(c,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one));
        for(;;) { ssize_t n=recv(c,b,sizeof(b),0);if(n<0 && errno==EINTR)continue;if(n<0)die("server recv");if(!n)_exit(0);exact(c,b,n,1); }
    }
}
static void client(int udp,const char *ip,int port,size_t size,double seconds,
                   const char *source_ip,int source_port) {
    int s=sock(udp);
    if(source_ip) {
        struct sockaddr_in source=addr(source_ip,source_port);
        if(bind(s,(void*)&source,sizeof(source)))die("client bind");
    }
    struct sockaddr_in a=addr(ip,port);if(connect(s,(void*)&a,sizeof(a)))die("connect");
    unsigned char tx[65536],rx[65536];
    for(size_t j=0;j<size;j++)tx[j]=(unsigned char)(j*17+93);
    const size_t cap=1048576;
    double *lat=malloc(cap*sizeof(*lat));if(!lat)die("latency allocation");
    uint64_t count=0;double start=now(),end=start;
    do {
        memcpy(tx,&count,sizeof(count));
        if(udp) { if(send(s,tx,size,0)!=(ssize_t)size)die("udp send");if(recv(s,rx,sizeof(rx),0)!=(ssize_t)size)die("udp recv"); }
        else {exact(s,tx,size,1);exact(s,rx,size,0);}
        if(memcmp(tx,rx,size)) {fprintf(stderr,"payload mismatch\n");exit(2);}
        double t=now();lat[count%cap]=(t-end)*1e6;count++;end=t;
    } while(end-start<seconds);
    close(s);
    size_t n=count<cap?count:cap;qsort(lat,n,sizeof(*lat),cmp_double);
    printf("RESULT protocol=%s size=%zu count=%llu seconds=%.9f rps=%.3f MiBps=%.6f mean_us=%.3f p50_us=%.3f p99_us=%.3f latency_samples=%zu verified=1\n",
           udp?"udp":"tcp",size,(unsigned long long)count,end-start,count/(end-start),count*size/(end-start)/1048576,1e6*(end-start)/count,lat[n/2],lat[(n-1)*99/100],n);
    free(lat);
}
int main(int argc,char **argv) {
    if(argc<4) {fprintf(stderr,"server tcp|udp PORT; client/local tcp|udp PORT IP SIZE SECONDS\n");return 2;}
    signal(SIGPIPE,SIG_IGN);int udp=!strcmp(argv[2],"udp"),port=atoi(argv[3]);
    if(!strcmp(argv[1],"server"))server(udp,port,-1);
    if(argc!=7 && argc!=9)return 2;
    const char *source_ip=argc==9?argv[7]:NULL;
    int source_port=argc==9?atoi(argv[8]):0;
    if(source_ip && (source_port<1 || source_port>65535))return 2;
    size_t size=strtoul(argv[5],0,10);double seconds=atof(argv[6]);
    if(size<8||size>65536||seconds<=0)return 2;
    if(!strcmp(argv[1],"multi")) {
        if(source_ip)return 2; /* One explicit source tuple cannot own four flows. */
        pid_t children[4];double start=now();int bad=0;
        for(int i=0;i<4;i++) {
            children[i]=fork();if(children[i]<0)die("multi fork");
            if(!children[i]) {client(udp,argv[4],port,size,seconds,NULL,0);fflush(stdout);_exit(0);}
        }
        for(int i=0;i<4;i++) {int status;if(waitpid(children[i],&status,0)<0)die("wait");if(!WIFEXITED(status)||WEXITSTATUS(status))bad=1;}
        printf("PARALLEL clients=4 wall=%.9f verified=%d\n",now()-start,!bad);
        return bad;
    }
    pid_t child=0;
    if(!strcmp(argv[1],"local")) {
        int p[2];if(pipe(p))die("pipe");child=fork();if(child<0)die("fork");
        if(!child) {close(p[0]);server(udp,port,p[1]);_exit(0);}
        close(p[1]);char c;if(read(p[0],&c,1)!=1)die("server start");close(p[0]);
    }
    client(udp,argv[4],port,size,seconds,source_ip,source_port);
    if(child) {kill(child,SIGTERM);waitpid(child,0,0);}
    return 0;
}
