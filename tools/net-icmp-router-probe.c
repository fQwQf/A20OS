/* Run as the Linux LAN client behind the router. Validate actual ICMP
 * delivery, quoted socket tuple and advertised next-hop MTU through Linux's
 * error queue. No A20OS-specific syscall or header. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#define SO_EE_ORIGIN_ICMP 2
struct error_info {
    uint32_t error; uint8_t origin, type, code, pad; uint32_t info, data;
};
static void require(int ok, const char *why) { if (!ok) { perror(why); exit(1); } }
static void probe(int ttl, unsigned size, int type, int code, unsigned mtu) {
    int fd=socket(AF_INET,SOCK_DGRAM|SOCK_NONBLOCK,0), one=1, do_pmtu=2;
    require(fd>=0,"UDP socket");
    require(setsockopt(fd,IPPROTO_IP,IP_RECVERR,&one,sizeof(one))==0,"IP_RECVERR");
    require(setsockopt(fd,IPPROTO_IP,IP_TTL,&ttl,sizeof(ttl))==0,"TTL");
    require(setsockopt(fd,IPPROTO_IP,IP_MTU_DISCOVER,&do_pmtu,sizeof(do_pmtu))==0,"DF");
    struct sockaddr_in peer={.sin_family=AF_INET,.sin_port=htons(39029)};
    inet_pton(AF_INET,"10.20.0.2",&peer.sin_addr);
    require(connect(fd,(void*)&peer,sizeof(peer))==0,"connect");
    unsigned char payload[1400]; memset(payload,0x5b,sizeof(payload));
    require(send(fd,payload,size,0)==(ssize_t)size,"send DF UDP");
    struct pollfd p={.fd=fd,.events=POLLERR};
    require(poll(&p,1,4000)==1 && (p.revents&POLLERR),"ICMP error readiness");
    unsigned char data[1600],control[512];
    struct iovec iov={data,sizeof(data)};
    struct sockaddr_in destination={0};
    struct msghdr msg={.msg_name=&destination,.msg_namelen=sizeof(destination),
        .msg_iov=&iov,.msg_iovlen=1,.msg_control=control,.msg_controllen=sizeof(control)};
    ssize_t n=recvmsg(fd,&msg,MSG_ERRQUEUE);
    require(n>=0 && (n==0 || !memcmp(data,payload,n<8?(size_t)n:8)),"ICMP quoted original payload");
    require(destination.sin_addr.s_addr==peer.sin_addr.s_addr && destination.sin_port==peer.sin_port,
            "ICMP original socket tuple");
    struct error_info error={0}; int found=0;
    for (struct cmsghdr *c=CMSG_FIRSTHDR(&msg);c;c=CMSG_NXTHDR(&msg,c))
        if(c->cmsg_level==IPPROTO_IP && c->cmsg_type==IP_RECVERR && c->cmsg_len>=CMSG_LEN(sizeof(error))) {
            memcpy(&error,CMSG_DATA(c),sizeof(error)); found=1;
        }
    require(found && error.origin==SO_EE_ORIGIN_ICMP && error.type==type && error.code==code,
            "ICMP type/code/origin");
    if(mtu) require(error.info==mtu && error.error==EMSGSIZE,"ICMP next-hop MTU");
    printf("ICMP_ERROR_PASS type=%u code=%u info=%u errno=%u\n",error.type,error.code,error.info,error.error);
    close(fd);
}
static void probe6(int hops, unsigned size, int type, int code, unsigned mtu) {
    int fd=socket(AF_INET6,SOCK_DGRAM|SOCK_NONBLOCK,0), one=1, do_pmtu=2;
    require(fd>=0,"IPv6 UDP socket");
    require(setsockopt(fd,IPPROTO_IPV6,IPV6_RECVERR,&one,sizeof(one))==0,"IPV6_RECVERR");
    require(setsockopt(fd,IPPROTO_IPV6,IPV6_UNICAST_HOPS,&hops,sizeof(hops))==0,"hop limit");
    require(setsockopt(fd,IPPROTO_IPV6,IPV6_MTU_DISCOVER,&do_pmtu,sizeof(do_pmtu))==0,"IPv6 DF");
    /* Wait for the configured global LAN address to finish Linux DAD. Without
     * this, Linux can choose a link-local source for the first global probe,
     * which a conforming router must not forward. */
    struct sockaddr_in6 local={.sin6_family=AF_INET6};
    inet_pton(AF_INET6,"fd42:10::2",&local.sin6_addr);
    int bound=0;
    for(int attempt=0;attempt<8 && !bound;++attempt) {
        if(bind(fd,(void*)&local,sizeof(local))==0) bound=1;
        else if(errno==EADDRNOTAVAIL) usleep(500000); else break;
    }
    require(bound,"IPv6 global LAN source ready");

    struct sockaddr_in6 peer={.sin6_family=AF_INET6,.sin6_port=htons(39029)};
    inet_pton(AF_INET6,"fd42:20::2",&peer.sin6_addr);
    require(connect(fd,(void*)&peer,sizeof(peer))==0,"IPv6 connect");
    unsigned char payload[1400]; memset(payload,0x6c,sizeof(payload));
    require(send(fd,payload,size,0)==(ssize_t)size,"send IPv6 UDP");
    struct pollfd p={.fd=fd,.events=POLLERR};
    require(poll(&p,1,4000)==1 && (p.revents&POLLERR),"ICMPv6 error readiness");
    unsigned char data[1600],control[512];
    struct iovec iov={data,sizeof(data)};
    struct sockaddr_in6 destination={0};
    struct msghdr msg={.msg_name=&destination,.msg_namelen=sizeof(destination),
        .msg_iov=&iov,.msg_iovlen=1,.msg_control=control,.msg_controllen=sizeof(control)};
    ssize_t n=recvmsg(fd,&msg,MSG_ERRQUEUE);
    require(n>=0 && (n==0 || !memcmp(data,payload,n<8?(size_t)n:8)),"ICMPv6 original payload");
    require(!memcmp(&destination.sin6_addr,&peer.sin6_addr,16) && destination.sin6_port==peer.sin6_port,
            "ICMPv6 original socket tuple");
    struct error_info error={0}; int found=0;
    for (struct cmsghdr *c=CMSG_FIRSTHDR(&msg);c;c=CMSG_NXTHDR(&msg,c))
        if(c->cmsg_level==IPPROTO_IPV6 && c->cmsg_type==IPV6_RECVERR && c->cmsg_len>=CMSG_LEN(sizeof(error))) {
            memcpy(&error,CMSG_DATA(c),sizeof(error)); found=1;
        }
    require(found && error.origin==3 && error.type==type && error.code==code,"ICMPv6 type/code/origin");
    if(mtu) require(error.info==mtu && error.error==EMSGSIZE,"ICMPv6 next-hop MTU");
    printf("ICMP6_ERROR_PASS type=%u code=%u info=%u errno=%u\n",error.type,error.code,error.info,error.error);
    close(fd);
}
int main(void) {
    probe(64,64,3,3,0); /* External UDP port-unreachable quotes the NAPT tuple. */
    probe(1,64,11,0,0); /* Router must preserve a valid quoted IP header. */
    probe(64,1400,3,4,1280); /* Configured router WAN MTU is 1280. */
    probe6(64,64,1,4,0);
    probe6(1,64,3,0,0);
    probe6(64,1400,2,0,1280);
    puts("NET_ICMP_ROUTER_PASS"); return 0;
}
