/*
 * svcdemo — checkpoint/restore demo service.  Keeps a purely in-memory tick
 * counter and an append-only log file; a correct checkpoint/restore cycle
 * continues the counter and the log instead of restarting them.
 *
 * Writes its pid to /bin/svcdemo.pid so scripted supervisors can find it
 * without parsing ps output.
 */
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

static volatile unsigned long g_ticks;

int main(void)
{
    int pidfd = open("/bin/svcdemo.pid", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (pidfd >= 0) {
        dprintf(pidfd, "%d\n", getpid());
        close(pidfd);
    }
    int log = open("/bin/svcdemo.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    setvbuf(stdout, NULL, _IONBF, 0);
    for (;;) {
        g_ticks++;
        printf("svcdemo tick %lu pid=%d\n", g_ticks, getpid());
        if (log >= 0) {
            dprintf(log, "tick %lu pid=%d\n", g_ticks, getpid());
            fsync(log);
        }
        sleep(1);
    }
    return 0;
}
