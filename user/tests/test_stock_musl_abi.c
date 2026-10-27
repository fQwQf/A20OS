#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/times.h>
#include <time.h>
#include <unistd.h>
#include "../cmds/linux_sched.h"

#define REQUIRE(expr) do { if (!(expr)) { \
    printf("STOCK_MUSL: FAIL line=%d errno=%d %s\n", __LINE__, errno, #expr); return 1; \
} } while (0)

int main(void) {
    struct ifaddrs *list;
    REQUIRE(getifaddrs(&list) == 0);
    int found = 0;
    for (struct ifaddrs *p = list; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        struct sockaddr_in *sin = (struct sockaddr_in *)p->ifa_addr;
        char addr[INET_ADDRSTRLEN];
        REQUIRE(inet_ntop(AF_INET, &sin->sin_addr, addr, sizeof(addr)) != NULL);
        if (!strcmp(addr, "10.0.2.15")) {
            REQUIRE(p->ifa_name && p->ifa_netmask);
            REQUIRE(!(p->ifa_flags & IFF_LOOPBACK));
            found = 1;
        }
    }
    freeifaddrs(list);
    REQUIRE(found);
    puts("STOCK_MUSL: rtnetlink PASS");

    errno = 0;
    REQUIRE(sched_getscheduler(0) == -1 && errno == ENOSYS);
    struct sched_param sp = {0};
    REQUIRE(a20_linux_sched_setscheduler(0, SCHED_OTHER, &sp) == 0);
    REQUIRE(a20_linux_sched_getscheduler(0) == SCHED_OTHER);
    REQUIRE(a20_linux_sched_getparam(0, &sp) == 0);
    REQUIRE(a20_linux_sched_setparam(0, &sp) == 0);
    puts("STOCK_MUSL: libc-stubs-and-raw-scheduling PASS");

    struct timespec now, zero = {0};
    REQUIRE(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    REQUIRE(now.tv_nsec >= 0 && now.tv_nsec < 1000000000L);
    sigset_t mask;
    sigemptyset(&mask);
    REQUIRE(pselect(0, NULL, NULL, NULL, &zero, &mask) == 0);
    REQUIRE(ppoll(NULL, 0, &zero, &mask) == 0);
    struct { struct tms value; unsigned long guard[2]; } ticks;
    memset(&ticks, 0, sizeof(ticks));
    ticks.guard[0] = 0x12345678UL; ticks.guard[1] = 0x76543210UL;
    REQUIRE(times(&ticks.value) != (clock_t)-1);
    REQUIRE(ticks.guard[0] == 0x12345678UL && ticks.guard[1] == 0x76543210UL);
    struct itimerval it = {0}, old;
    REQUIRE(setitimer(ITIMER_REAL, &it, &old) == 0);
    REQUIRE(getitimer(ITIMER_REAL, &old) == 0);
    timer_t timer;
    struct sigevent sev = { .sigev_notify = SIGEV_NONE };
    REQUIRE(timer_create(CLOCK_MONOTONIC, &sev, &timer) == 0);
    struct itimerspec spec = {0}, observed;
    spec.it_value.tv_sec = 2;
    REQUIRE(timer_settime(timer, 0, &spec, NULL) == 0);
    REQUIRE(timer_gettime(timer, &observed) == 0);
    REQUIRE(observed.it_value.tv_sec <= 2);
    REQUIRE(timer_delete(timer) == 0);
    puts("STOCK_MUSL: time64-pointer-width-and-timers PASS");
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    REQUIRE(sock >= 0);
    struct timeval timeout = { .tv_sec=1, .tv_usec=500000 }, out_timeout;
    socklen_t timeout_size = sizeof(out_timeout);
    REQUIRE(setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    REQUIRE(getsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &out_timeout, &timeout_size) == 0);
    REQUIRE(timeout_size == sizeof(out_timeout));
    REQUIRE(out_timeout.tv_sec == 1 && out_timeout.tv_usec == 500000);
    if (sizeof(long) == 4) {
        long old_timeout[2] = {2, 500000}, observed_old[2] = {0};
        socklen_t old_size = sizeof(observed_old);
        REQUIRE(setsockopt(sock, SOL_SOCKET, 20, old_timeout, sizeof(old_timeout)) == 0);
        REQUIRE(getsockopt(sock, SOL_SOCKET, 20, observed_old, &old_size) == 0);
        REQUIRE(old_size == sizeof(observed_old));
        REQUIRE(observed_old[0] == 2 && observed_old[1] == 500000);
    }
    close(sock);
    puts("STOCK_MUSL: socket-timeout-wire-layouts PASS");


    int fd = open("/tmp/stock-musl-offset", O_CREAT | O_TRUNC | O_RDWR, 0600);
    REQUIRE(fd >= 0);
    REQUIRE(ftruncate(fd, 8192) == 0);
    REQUIRE(pwrite(fd, "second", 7, 4096) == 7);
    char data[8] = {0};
    REQUIRE(pread(fd, data, 7, 4096) == 7 && !strcmp(data, "second"));
    REQUIRE(lseek(fd, 4096, SEEK_SET) == 4096);
    REQUIRE(lseek(fd, 0, SEEK_CUR) == 4096);
    char *mapping = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd, 4096);
    REQUIRE(mapping != MAP_FAILED);
    REQUIRE(!memcmp(mapping, "second", 7));
    REQUIRE(munmap(mapping, 4096) == 0);
    /* The native-width VFS currently cannot represent large RV32 positions;
     * the ABI boundary must refuse them, never silently access offset zero. */
    if (sizeof(long) == 4) {
        errno = 0;
        REQUIRE(lseek(fd, (off_t)1 << 32, SEEK_SET) == (off_t)-1 && errno == EOVERFLOW);
        REQUIRE(lseek(fd, 0, SEEK_CUR) == 4096);
    }
    close(fd);
    unlink("/tmp/stock-musl-offset");
    puts("STOCK_MUSL: split-offsets-llseek-and-mmap2 PASS");
    puts("STOCK_MUSL: PASS");
    return 0;
}
