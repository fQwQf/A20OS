/* race_probe: N concurrent processes replicate the distro's first-boot race,
 * each running the xfce_mkdirhier sequence against the same paths on ext4.
 * Any outcome other than success or EEXIST-with-dir is a kernel bug. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static const char *DIRS[] = {
    "/extra/race",
    "/extra/race/.local",
    "/extra/race/.local/share",
    "/extra/race/.config",
    "/extra/race/.config/xfce4",
    "/extra/race/.config/xfce4/xfconf",
};
static const int NDIRS = (int)(sizeof(DIRS) / sizeof(DIRS[0]));

/* returns 0 ok, 1 inconsistent */
static int check_stat(const char *path)
{
    struct stat sb;
    int tries = 0;
retry:
    if (stat(path, &sb) == 0)
        return S_ISDIR(sb.st_mode) ? 0 : 1;
    if (errno == ENOENT && tries++ < 50) {
        struct timespec ts = { 0, 20000000 };
        nanosleep(&ts, NULL);
        goto retry;
    }
    printf("RACE_PROBE[%d]: stat(%s) errno=%d after %d tries\n",
           getpid(), path, errno, tries);
    return 1;
}

static int child_run(int id)
{
    int bad = 0;
    /* jitter so processes interleave differently */
    struct timespec ts = { 0, (long)(id % 7) * 3000000 };
    nanosleep(&ts, NULL);

    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < NDIRS; i++) {
            int r = mkdir(DIRS[i], 0700);
            if (r < 0 && errno != EEXIST) {
                printf("RACE_PROBE[%d]: mkdir(%s) errno=%d\n",
                       getpid(), DIRS[i], errno);
                bad = 1;
            }
            if (!check_stat(DIRS[i]))
                continue;
            printf("RACE_PROBE[%d]: stat-inconsistent(%s)\n",
                   getpid(), DIRS[i]);
            bad = 1;
        }
        ts.tv_nsec = (long)round * 1000000;
        nanosleep(&ts, NULL);
    }
    return bad;
}

int main(void)
{
    const int N = 8;
    int bad = 0;

    /* start clean */
    for (int i = NDIRS - 1; i >= 0; i--)
        rmdir(DIRS[i]);

    for (int i = 0; i < N; i++) {
        pid_t p = fork();
        if (p == 0)
            _exit(child_run(i));
    }
    for (int i = 0; i < N; i++) {
        int st = 0;
        wait(&st);
        if (WEXITSTATUS(st))
            bad = 1;
    }
    printf("RACE_PROBE: %s\n", bad ? "FAIL" : "PASS");
    return bad;
}
