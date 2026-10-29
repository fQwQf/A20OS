/* Same static Linux ELF on Linux KVM and A20OS. Exercise a real L3 interface,
 * not backend functions: UDP output, injected input, controls and lifetime. */
#define main socket_probe_main
#include "net-socket-abi-probe.c"
#undef main
#include <fcntl.h>
/* Linux UAPI, independent of the target kernel's headers. */
#define IFF_TUN 0x0001
#define IFF_NO_PI 0x1000
#define TUNSETIFF _IOW('T', 202, int)
#define TUNSETPERSIST _IOW('T', 203, int)
#include <sys/mman.h>

static unsigned short ip_checksum(const unsigned char *b, size_t length) {
    unsigned sum = 0;
    for (size_t i = 0; i < length; ++i) sum += (unsigned)b[i] << (i % 2 ? 0 : 8);
    while (sum >> 16) sum = (sum & 65535) + (sum >> 16);
    return (unsigned short)~sum;
}
static int create_tun(const char *name) {
    int fd = open("/dev/net/tun", O_RDWR | O_NONBLOCK);
    require(fd >= 0, "open tun");
    struct ifreq request = {0};
    snprintf(request.ifr_name, sizeof(request.ifr_name), "%s", name);
    request.ifr_flags = IFF_TUN | IFF_NO_PI;
    require(ioctl(fd, TUNSETIFF, &request) == 0, "TUNSETIFF");
    return fd;
}
static void set_address(int sock, const char *name, unsigned long operation, const char *ip) {
    struct ifreq request = {0};
    snprintf(request.ifr_name, sizeof(request.ifr_name), "%s", name);
    struct sockaddr_in *a = (void *)&request.ifr_addr;
    a->sin_family = AF_INET;
    require(inet_pton(AF_INET, ip, &a->sin_addr) == 1, "address parse");
    require(ioctl(sock, operation, &request) == 0, "set TUN address/netmask");
}
static void test_tun(void) {
    const char *name = "a20probe0";
    int tun = create_tun(name);
    unsigned index = if_nametoindex(name);
    require(index != 0, "TUN public interface index");
    int udp = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    require(udp >= 0, "TUN UDP socket");
    struct ifreq request = {0};
    snprintf(request.ifr_name, sizeof(request.ifr_name), "%s", name);
    request.ifr_flags = 0;
    require(ioctl(udp, SIOCSIFFLAGS, &request) == 0, "TUN down");
    request.ifr_mtu = 0;
    require(ioctl(udp, SIOCGIFMTU, &request) == 0 && request.ifr_mtu == 1500, "TUN MTU getter");
    require(ioctl(udp, SIOCGIFFLAGS, &request) == 0 && !(request.ifr_flags & IFF_UP),
            "MTU getter preserves DOWN");
    request.ifr_mtu = 1400;
    require(ioctl(udp, SIOCSIFMTU, &request) == 0, "TUN MTU setter");
    require(ioctl(udp, SIOCGIFMTU, &request) == 0 && request.ifr_mtu == 1400, "TUN MTU readback");
    set_address(udp, name, SIOCSIFADDR, "198.18.0.1");
    set_address(udp, name, SIOCSIFNETMASK, "255.255.255.0");
    request.ifr_flags = IFF_UP;
    require(ioctl(udp, SIOCSIFFLAGS, &request) == 0, "TUN up");
    struct sockaddr_in local = address("198.18.0.1", 49153), peer = address("198.18.0.2", 49154);
    require(bind(udp, (void *)&local, sizeof(local)) == 0, "bind TUN UDP");
    require(sendto(udp, "tundata", 7, 0, (void *)&peer, sizeof(peer)) == 7, "send via TUN");
    struct pollfd ready = { .fd = tun, .events = POLLIN };
    unsigned char packet[4096];
    int n=0, found=0; size_t ihl=0;
    for(int attempts=0;attempts<8 && !found;++attempts) {
        if(poll(&ready,1,1000)!=1) continue;
        n=read(tun,packet,sizeof(packet));
        if(n>=35 && packet[0]>>4==4 && packet[9]==17) {
            ihl=(packet[0]&15)*4;
            found=ihl>=20 && n>=(int)ihl+15 && !memcmp(packet+ihl+8,"tundata",7);
        }
    }
    require(found && ip_checksum(packet,ihl)==0 && n==(int)ihl+15,"TUN wire checksum/payload");
    /* A zero UDP checksum is valid IPv4. Deliver an independent incoming
     * datagram through the character device into the bound Linux socket. */
    unsigned char input[35] = {0x45, 0, 0, 35, 0x12, 0x34, 0, 0, 64, 17};
    memcpy(input + 12, &peer.sin_addr, 4); memcpy(input + 16, &local.sin_addr, 4);
    memcpy(input + 20, &peer.sin_port, 2); memcpy(input + 22, &local.sin_port, 2);
    input[25] = 15; memcpy(input + 28, "tunback", 7);
    unsigned short check = htons(ip_checksum(input, 20)); memcpy(input + 10, &check, 2);
    require(write(tun, input, sizeof(input)) == sizeof(input), "inject TUN packet");
    ready = (struct pollfd){ .fd = udp, .events = POLLIN };
    require(poll(&ready, 1, 3000) == 1, "TUN injected UDP ready");
    n = recv(udp, packet, sizeof(packet), 0);
    require(n == 7 && !memcmp(packet, "tunback", 7), "TUN injected UDP bytes");
    for (int wrap=0;wrap<64;++wrap) {
        unsigned char token[7]={'w','r','a','p',0,0,0}; token[6]=(unsigned char)wrap;
        require(sendto(udp,token,sizeof(token),0,(void*)&peer,sizeof(peer))==sizeof(token),"TUN ring wrap send");
        ready=(struct pollfd){.fd=tun,.events=POLLIN};
        int matched=0;
        for(int attempts=0;attempts<8 && !matched;++attempts) {
            if(poll(&ready,1,1000)!=1) continue;
            n=read(tun,packet,sizeof(packet));
            if(n>=35 && packet[0]>>4==4 && packet[9]==17) {
                size_t header=(packet[0]&15)*4;
                matched=header>=20 && n>=(int)header+15 && !memcmp(packet+header+8,token,7);
            }
        }
        require(matched,"TUN FIFO repeated wrap payload");
    }
    close(udp);
    require(ioctl(tun, TUNSETPERSIST, 1) == 0, "persist scalar argument");
    close(tun);
    require(if_nametoindex(name) == index, "persistent interface survives close");
    tun = create_tun(name);
    require(if_nametoindex(name) == index, "persistent reattach keeps identity");
    require(ioctl(tun, TUNSETPERSIST, 0) == 0, "unpersist");
    close(tun);
    require(if_nametoindex(name) == 0, "unpersisted close removes interface");
    /* Both backend slots persistent and detached: reopening must use control
     * credits, rather than allocating a third protocol interface. */
    for (int i=0;i<2;++i) {
        char persistent[16]; snprintf(persistent,sizeof(persistent),"a20persist%d",i);
        tun=create_tun(persistent);
        require(ioctl(tun,TUNSETPERSIST,1)==0,"fill persistent interface slots"); close(tun);
    }
    for (int i=0;i<2;++i) {
        char persistent[16]; snprintf(persistent,sizeof(persistent),"a20persist%d",i);
        tun=create_tun(persistent);
        require(ioctl(tun,TUNSETPERSIST,0)==0,"reattach full persistent set"); close(tun);
    }
    for (int i = 0; i < 24; ++i) { tun = create_tun(name); close(tun); }
    /* Read-only ifreq: input copy succeeds, output copy fails. Closing must
     * leave neither the backend slot nor public name reserved. */
    void *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    require(page != MAP_FAILED, "fault page");
    struct ifreq *bad = page;
    memset(bad, 0, sizeof(*bad)); strcpy(bad->ifr_name, name); bad->ifr_flags = IFF_TUN | IFF_NO_PI;
    require(mprotect(page, 4096, PROT_READ) == 0, "readonly ifreq");
    tun = open("/dev/net/tun", O_RDWR | O_NONBLOCK);
    require(tun >= 0 && ioctl(tun, TUNSETIFF, bad) < 0 && errno == EFAULT, "TUNSETIFF copyback fault");
    close(tun); munmap(page, 4096);
    tun = create_tun(name); close(tun);
    puts("NET_TUN_ABI_PASS");
}
static void test_tun_ipv6(void) {
    int tun=create_tun("a20v6");
    require(system("ip -6 addr add fd42:99::1/64 dev a20v6 && ip link set dev a20v6 up")==0,
            "configure TUN IPv6");
    int udp=socket(AF_INET6,SOCK_DGRAM|SOCK_NONBLOCK,0);
    require(udp>=0,"TUN IPv6 socket");
    struct sockaddr_in6 local={.sin6_family=AF_INET6,.sin6_port=htons(49153)};
    struct sockaddr_in6 peer={.sin6_family=AF_INET6,.sin6_port=htons(49154)};
    inet_pton(AF_INET6,"fd42:99::1",&local.sin6_addr);
    inet_pton(AF_INET6,"fd42:99::2",&peer.sin6_addr);
    int bound=0;
    /* Linux performs DAD; A20OS currently promotes administrative addresses
     * directly to VALID. Wait for actual bind readiness on either kernel. */
    for(int attempt=0;attempt<8 && !bound;++attempt) {
        if(bind(udp,(void*)&local,sizeof(local))==0) bound=1;
        else if(errno==EADDRNOTAVAIL) usleep(500000);
        else break;
    }
    require(bound,"TUN IPv6 bind");
    require(sendto(udp,"tunipv6",7,0,(void*)&peer,sizeof(peer))==7,"TUN IPv6 send");
    struct pollfd ready={.fd=tun,.events=POLLIN};
    unsigned char packet[4096]; int found=0;
    /* Linux may first emit IPv6 DAD/RS traffic when the link comes up. */
    for (int attempts=0;attempts<8 && !found;++attempts) {
        if (poll(&ready,1,1000)!=1) continue;
        int n=read(tun,packet,sizeof(packet));
        found=n==55 && packet[0]>>4==6 && packet[6]==17 && !memcmp(packet+48,"tunipv6",7);
    }
    require(found,"TUN IPv6 output bytes");
    require(connect(udp,(void*)&peer,sizeof(peer))==0 && send(udp,"conip6!",7,0)==7,
            "TUN IPv6 connected send");
    found=0;
    for (int attempts=0;attempts<8 && !found;++attempts) {
        if (poll(&ready,1,1000)!=1) continue;
        int n=read(tun,packet,sizeof(packet));
        found=n==55 && packet[6]==17 && !memcmp(packet+48,"conip6!",7);
    }
    require(found,"TUN IPv6 connected wire bytes");
    int lazy=socket(AF_INET6,SOCK_DGRAM|SOCK_NONBLOCK,0);
    require(lazy>=0 && sendto(lazy,"lazyip6",7,0,(void*)&peer,sizeof(peer))==7,
            "TUN IPv6 lazy bind/send");
    found=0;
    for (int attempts=0;attempts<8 && !found;++attempts) {
        if (poll(&ready,1,1000)!=1) continue;
        int n=read(tun,packet,sizeof(packet));
        found=n==55 && packet[6]==17 && !memcmp(packet+48,"lazyip6",7);
    }
    require(found,"TUN IPv6 lazy wire bytes");
    struct sockaddr_in6 actual={0}; socklen_t actual_size=sizeof(actual);
    require(getsockname(lazy,(void*)&actual,&actual_size)==0 && actual.sin6_port!=0 &&
            !memcmp(packet+40,&actual.sin6_port,2),"IPv6 lazy frontend/backend port agree");
    close(lazy);
    int automatic=socket(AF_INET6,SOCK_DGRAM|SOCK_NONBLOCK,0);
    require(automatic>=0 && connect(automatic,(void*)&peer,sizeof(peer))==0 &&
            send(automatic,"connnew",7,0)==7,"TUN IPv6 unbound connect/send");
    found=0;
    for(int attempts=0;attempts<8 && !found;++attempts) {
        if(poll(&ready,1,1000)!=1) continue;
        int n=read(tun,packet,sizeof(packet));
        found=n==55 && packet[6]==17 && !memcmp(packet+48,"connnew",7);
    }
    actual_size=sizeof(actual);
    require(found && getsockname(automatic,(void*)&actual,&actual_size)==0 && actual.sin6_port!=0 &&
            !memcmp(packet+40,&actual.sin6_port,2) && !memcmp(&actual.sin6_addr,&local.sin6_addr,16),
            "IPv6 connect concrete source/port agree with wire");
    close(automatic);


    unsigned char input[55]={0x60,0,0,0,0,15,17,64};
    memcpy(input+8,&peer.sin6_addr,16); memcpy(input+24,&local.sin6_addr,16);
    memcpy(input+40,&peer.sin6_port,2); memcpy(input+42,&local.sin6_port,2);
    input[45]=15; memcpy(input+48,"backip6",7);
    unsigned char check_input[55+8];
    memcpy(check_input,input+8,32);
    memset(check_input+32,0,8); check_input[35]=15; check_input[39]=17;
    memcpy(check_input+40,input+40,15);
    unsigned short check=htons(ip_checksum(check_input,55));
    memcpy(input+46,&check,2);
    require(write(tun,input,55)==55,"TUN IPv6 injection");
    ready=(struct pollfd){.fd=udp,.events=POLLIN};
    require(poll(&ready,1,3000)==1,"TUN IPv6 injected UDP ready");
    require(recv(udp,packet,sizeof(packet),0)==7 && !memcmp(packet,"backip6",7),
            "TUN IPv6 injected bytes");
    close(udp); close(tun);
    puts("NET_TUN_IPV6_PASS");
}
int main(int argc, char **argv) {
    if (socket_probe_main(argc, argv)) return 1;
    test_tun();
    test_tun_ipv6();
    return 0;
}
