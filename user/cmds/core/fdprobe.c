/* fdprobe: which fd-family syscalls does an envelope block?
 * Run under envwrap; prints per-call errno so policy gaps show up. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

static void probe(const char *name, int rc)
{
    printf("FDPROBE: %s %s errno=%d (%s)\n", name, rc >= 0 ? "ok" : "FAIL",
           errno, strerror(errno));
    if (rc >= 0)
        close(rc);
}

int main(void)
{
    printf("FDPROBE: start\n");
    probe("epoll_create1", epoll_create1(0));
    probe("eventfd", eventfd(0, 0));
    probe("timerfd", timerfd_create(1, 0));
    probe("inotify_init1", inotify_init1(0));
    int sv[2] = { -1, -1 };
    int sr = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    printf("FDPROBE: socketpair %s errno=%d (%s)\n", sr == 0 ? "ok" : "FAIL",
           errno, strerror(errno));
    if (sr == 0) {
        close(sv[0]);
        close(sv[1]);
    }
    int pfd[2] = { -1, -1 };
    int pr = pipe(pfd);
    printf("FDPROBE: pipe %s errno=%d (%s)\n", pr == 0 ? "ok" : "FAIL",
           errno, strerror(errno));
    if (pr == 0) {
        close(pfd[0]);
        close(pfd[1]);
    }
    probe("socket_INET", socket(AF_INET, SOCK_STREAM, 0));
    printf("FDPROBE: done\n");
    return 0;
}
