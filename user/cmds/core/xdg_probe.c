/* xdg_probe: replicate libxfce4util's xfce_mkdirhier() sequence against the
 * ext4 mount at /extra and report per-step errno values. */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

static void try_mkdir(const char *path, unsigned int mode)
{
    int r = mkdir(path, mode);
    printf("XDG_PROBE: mkdir(%s, %04o) = %d errno=%d (%s)\n",
           path, mode, r, errno, r < 0 ? strerror(errno) : "ok");
}

static void try_stat(const char *path)
{
    struct stat sb;
    int r = stat(path, &sb);
    if (r < 0)
        printf("XDG_PROBE: stat(%s) = %d errno=%d (%s)\n",
               path, r, errno, strerror(errno));
    else
        printf("XDG_PROBE: stat(%s) = 0 mode=%o dir=%d\n",
               path, sb.st_mode, S_ISDIR(sb.st_mode) ? 1 : 0);
}

int main(void)
{
    const char *base = "/extra/xdgprobe";

    unsigned int oumask = umask(0);
    printf("XDG_PROBE: umask(0) returned %04o\n", oumask);
    unsigned int numask = oumask & ~(S_IWUSR | S_IXUSR);
    printf("XDG_PROBE: umask(%04o) -> %04o\n", numask, umask(numask));

    printf("XDG_PROBE: --- post-chroot (distro session) ---\n");
    try_stat("/etc/passwd");
    try_stat("/root");
    try_mkdir("/root/.local", 0700);
    try_stat("/root/.local");
    try_mkdir("/root/.local/share", 0700);
    try_stat("/root/.local/share");

    printf("XDG_PROBE: done\n");
    return 0;
}
