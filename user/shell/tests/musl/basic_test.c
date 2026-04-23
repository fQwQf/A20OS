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

static void copy_named_file(const char *src_dir, const char *dst_dir, const char *name)
{
    char src[512];
    char dst[512];
    snprintf(src, sizeof(src), "%s/%s", src_dir, name);
    snprintf(dst, sizeof(dst), "%s/%s", dst_dir, name);
    (void)copy_file(src, dst);
}

static void remove_named_file(const char *dst_dir, const char *name)
{
    char dst[512];
    snprintf(dst, sizeof(dst), "%s/%s", dst_dir, name);
    (void)unlink(dst);
}

static const char *select_musl_src(void)
{
    if (dir_exists("/testrv/musl/lib")) return "/testrv/musl/lib";
    if (dir_exists("/test/musl/lib")) return "/test/musl/lib";
    if (dir_exists("/testrv/musl")) return "/testrv/musl";
    if (dir_exists("/test/musl")) return "/test/musl";
    return "/test/musl/lib";
}

static void prepare_musl_runtime(void)
{
    const char *src = select_musl_src();
    (void)ensure_dir("/lib");
    (void)ensure_dir("/usr");
    (void)symlink("/lib", "/usr/lib");
    (void)symlink("/lib", "/lib64");

    copy_named_file(src, "/lib", "dlopen_dso.so");
    copy_named_file(src, "/lib", "tls_align_dso.so");
    copy_named_file(src, "/lib", "tls_init_dso.so");
    copy_named_file(src, "/lib", "tls_get_new-dtv_dso.so");
    copy_named_file(src, "/lib", "libc.so");
}

static void cleanup_musl_runtime(void)
{
    remove_named_file("/lib", "dlopen_dso.so");
    remove_named_file("/lib", "tls_align_dso.so");
    remove_named_file("/lib", "tls_init_dso.so");
    remove_named_file("/lib", "tls_get_new-dtv_dso.so");
    remove_named_file("/lib", "libc.so");
}

static void build_sibling_basic_dir(const char *script_path, char *out, size_t out_sz)
{
    const char *slash = strrchr(script_path, '/');
    if (!slash) {
        snprintf(out, out_sz, "./basic");
        return;
    }

    size_t n = (size_t)(slash - script_path);
    if (n >= out_sz)
        n = out_sz - 1;
    memcpy(out, script_path, n);
    out[n] = '\0';
    strncat(out, "/basic", out_sz - strlen(out) - 1);
}

static int path_exists(const char *path)
{
    return access(path, F_OK) == 0;
}

static int run_musl_script_with_fallback(const char *script_path)
{
    char basic_dir[512];
    build_sibling_basic_dir(script_path, basic_dir, sizeof(basic_dir));
    if (path_exists(basic_dir)) {
        return run_script_via_mksh(script_path,
                                   "TEST][musl][basic",
                                   "PATH=.:/bin:/test:/test/musl:/test/glibc");
    }

    char alt_script[512];
    if (strncmp(script_path, "/test/musl/", 11) == 0) {
        snprintf(alt_script, sizeof(alt_script), "/testrv/musl/%s", script_path + 11);
    } else if (strncmp(script_path, "/testrv/musl/", 13) == 0) {
        snprintf(alt_script, sizeof(alt_script), "/test/musl/%s", script_path + 13);
    } else {
        snprintf(alt_script, sizeof(alt_script), "%s", script_path);
    }

    build_sibling_basic_dir(alt_script, basic_dir, sizeof(basic_dir));
    if (path_exists(alt_script) && path_exists(basic_dir)) {
        printf("[TEST][musl][basic] remap script path: %s -> %s\n", script_path, alt_script);
        return run_script_via_mksh(alt_script,
                                   "TEST][musl][basic",
                                   "PATH=.:/bin:/test:/test/musl:/test/glibc");
    }

    printf("[TEST][musl][basic] script/basic path missing: script=%s alt=%s\n",
           script_path, alt_script);
    return 127;
}

int run_musl_basic_test(const char *script_path)
{
    prepare_musl_runtime();

    int pid = fork();
    if (pid < 0) {
        cleanup_musl_runtime();
        return 127;
    }

    if (pid == 0) {
        int rc = run_musl_script_with_fallback(script_path);
        _exit((rc < 0) ? 127 : (rc & 0xFF));
    }

    int status = 0;
    int w = waitpid(pid, &status, 0);
    cleanup_musl_runtime();
    if (w < 0)
        return 127;

    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return 127;
}
