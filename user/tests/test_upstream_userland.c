#define _GNU_SOURCE
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sched.h>
#include "../cmds/linux_sched.h"
#include <stdio.h>
#include <errno.h>
#include <string.h>
int main(void) {
    struct ifaddrs *list = 0;
    int rc = getifaddrs(&list), found = 0;
    printf("UPSTREAM_PROBE: getifaddrs rc=%d errno=%d\n", rc, errno);
    if (rc) list = 0;
    for (struct ifaddrs *p=list; p; p=p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        char address[INET_ADDRSTRLEN];
        struct sockaddr_in *sin=(struct sockaddr_in *)p->ifa_addr;
        if (inet_ntop(AF_INET, &sin->sin_addr, address, sizeof address)) {
            printf("UPSTREAM_PROBE: interface=%s address=%s\n",p->ifa_name,address);
            if (!strcmp(address,"10.0.2.15")) found=1;
        }
    }
    freeifaddrs(list);
    if (!found) { puts("UPSTREAM_PROBE: FAIL missing configured interface"); }
    struct sched_param param = { .sched_priority = 0 };
    errno=0; rc=a20_linux_sched_setscheduler(0,SCHED_OTHER,&param);
    printf("UPSTREAM_PROBE: sched_setscheduler rc=%d errno=%d\n",rc,errno);
    if (rc || a20_linux_sched_getscheduler(0)!=SCHED_OTHER || a20_linux_sched_getparam(0,&param)) return 3;
    if (!found) return 2;
    struct protoent *protocol = getprotobyname("ip");
    if (!protocol || protocol->p_proto != 0) return 4;
    puts("UPSTREAM_PROBE: PASS");
    return 0;
}
