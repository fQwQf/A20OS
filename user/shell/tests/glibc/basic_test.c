#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../common_runner.h"
#include "../test_runners.h"

static int ensure_dir(const char *path)
{
    if (mkdir(path, 0755) == 0 || errno == EEXIST)
        return 0;
    return -1;
}

static int dir_exists(const char *path)
{
    DIR *dp = opendir(path);
    if (!dp)
        return 0;
    closedir(dp);
    return 1;
}

static int copy_file(const char *src, const char *dst)
{
    int sfd = open(src, O_RDONLY, 0);
    if (sfd < 0)
        return -1;

    int dfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (dfd < 0) {
        close(sfd);
        return -1;
    }

    char buf[4096];
    for (;;) {
        int n = read(sfd, buf, sizeof(buf));
        if (n == 0)
            break;
        if (n < 0) {
            close(sfd);
            close(dfd);
            return -1;
        }
        int off = 0;
        while (off < n) {
            int w = write(dfd, buf + off, (size_t)(n - off));
            if (w < 0) {
                close(sfd);
                close(dfd);
                return -1;
            }
            off += w;
        }
    }

    close(sfd);
    close(dfd);
    return 0;
}

static void copy_libs_from_dir(const char *src_dir, const char *dst_dir)
{
    DIR *dp = opendir(src_dir);
    if (!dp)
        return;

    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (de->d_type == DT_DIR)
            continue;
        if (!(strstr(de->d_name, ".so") || strncmp(de->d_name, "ld-", 3) == 0))
            continue;

        char src[512];
        char dst[512];
        snprintf(src, sizeof(src), "%s/%s", src_dir, de->d_name);
        snprintf(dst, sizeof(dst), "%s/%s", dst_dir, de->d_name);
        (void)copy_file(src, dst);
    }
    closedir(dp);
}

static void remove_libs_from_dir_basename(const char *src_dir, const char *dst_dir)
{
    DIR *dp = opendir(src_dir);
    if (!dp)
        return;

    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (de->d_type == DT_DIR)
            continue;
        if (!(strstr(de->d_name, ".so") || strncmp(de->d_name, "ld-", 3) == 0))
            continue;

        char dst[512];
        snprintf(dst, sizeof(dst), "%s/%s", dst_dir, de->d_name);
        (void)unlink(dst);
    }
    closedir(dp);
}

static const char *select_glibc_src(void)
{
    if (dir_exists("/testrv/glibc/lib")) return "/testrv/glibc/lib";
    if (dir_exists("/test/glibc/lib")) return "/test/glibc/lib";
    if (dir_exists("/testrv/glibc")) return "/testrv/glibc";
    if (dir_exists("/test/glibc")) return "/test/glibc";
    return "/test/glibc/lib";
}

static void prepare_glibc_runtime(void)
{
    const char *src = select_glibc_src();
    (void)ensure_dir("/lib");
    (void)ensure_dir("/usr");
    (void)symlink("/lib", "/usr/lib");
    (void)symlink("/lib", "/lib64");

    copy_libs_from_dir(src, "/lib");

    {
        char ld_src[256];
        snprintf(ld_src, sizeof(ld_src), "%s/ld-linux-riscv64-lp64d.so.1", src);
        (void)copy_file(ld_src, "/lib/ld-linux-riscv64-lp64d.so.1");
    }
}

static void cleanup_glibc_runtime(void)
{
    const char *src = select_glibc_src();
    remove_libs_from_dir_basename(src, "/lib");
    (void)unlink("/lib/ld-linux-riscv64-lp64d.so.1");
}

int run_glibc_basic_test(const char *script_path)
{
    prepare_glibc_runtime();

    int pid = fork();
    if (pid < 0) {
        cleanup_glibc_runtime();
        return 127;
    }

    if (pid == 0) {
        int rc = run_script_via_mksh(script_path,
                                     "TEST][glibc][basic",
                                     "PATH=.:/bin:/test:/test/glibc:/test/musl");
        _exit((rc < 0) ? 127 : (rc & 0xFF));
    }

    int status = 0;
    int w = waitpid(pid, &status, 0);
    cleanup_glibc_runtime();
    if (w < 0)
        return 127;

    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return 127;
}
