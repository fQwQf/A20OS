#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "busybox_setup.h"

static int ensure_dir(const char *path)
{
    struct stat st;

    if (stat(path, &st) == 0)
        return S_ISDIR(st.st_mode) ? 0 : -1;

    if (errno != ENOENT)
        return -1;

    if (mkdir(path, 0755) != 0 && errno != EEXIST)
        return -1;

    return 0;
}

static int copy_file(const char *src, const char *dst, int mode)
{
    int src_fd = open(src, O_RDONLY);
    if (src_fd < 0)
        return -1;

    int dst_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (dst_fd < 0) {
        close(src_fd);
        return -1;
    }

    char buf[16384];
    for (;;) {
        ssize_t n = read(src_fd, buf, sizeof(buf));
        if (n == 0)
            break;
        if (n < 0) {
            close(src_fd);
            close(dst_fd);
            return -1;
        }

        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(dst_fd, buf + off, (size_t)(n - off));
            if (w <= 0) {
                close(src_fd);
                close(dst_fd);
                return -1;
            }
            off += w;
        }
    }

    close(src_fd);
    close(dst_fd);
    return 0;
}

static const char *find_existing_dir(const char *const candidates[])
{
    struct stat st;

    for (int i = 0; candidates[i]; i++) {
        if (stat(candidates[i], &st) == 0 && S_ISDIR(st.st_mode))
            return candidates[i];
    }

    return NULL;
}

static void install_symlink(const char *target, const char *linkpath)
{
    struct stat st;

    if (stat(linkpath, &st) == 0)
        return;

    if (errno == ENOENT)
        unlink(linkpath);

    if (symlink(target, linkpath) != 0 && errno != EEXIST) {
        printf("[LIBLINK] symlink %s -> %s failed errno=%d\n",
               linkpath, target, errno);
    }
}

static void install_lib_file(const char *src_dir, const char *dst_dir,
                             const char *name)
{
    char src[256];
    char dst[256];
    struct stat st;

    snprintf(src, sizeof(src), "%s/%s", src_dir, name);
    if (stat(src, &st) != 0 || !S_ISREG(st.st_mode))
        return;
    int mode = st.st_mode & 0777;

    snprintf(dst, sizeof(dst), "%s/%s", dst_dir, name);
    if (stat(dst, &st) == 0)
        return;

    if (errno == ENOENT)
        unlink(dst);

    if (copy_file(src, dst, mode) == 0) {
        printf("[LIBLINK] copied %s\n", dst);
    } else {
        printf("[LIBLINK] copy %s -> %s failed errno=%d\n",
               src, dst, errno);
    }
}

static const char *find_busybox_source(void)
{
    static const char *candidates[] = {
        "/test/glibc/busybox",
        "/testrv/glibc/busybox",
        "/glibc/busybox",
        "/test/musl/busybox",
        "/testrv/musl/busybox",
        "/musl/busybox",
        NULL,
    };

    struct stat st;
    for (int i = 0; candidates[i]; i++) {
        if (stat(candidates[i], &st) == 0 && S_ISREG(st.st_mode))
            return candidates[i];
    }

    return NULL;
}

static void install_applet(const char *name)
{
    char path[128];
    struct stat st;

    snprintf(path, sizeof(path), "/bbin/%s", name);
    if (stat(path, &st) == 0)
        return;

    if (symlink("/bbin/busybox", path) != 0 && errno != EEXIST) {
        printf("[BBIN] symlink %s -> /bbin/busybox failed errno=%d\n",
               path, errno);
    }
}

void setup_bbin_busybox(void)
{
    static int done;
    if (done)
        return;
    done = 1;

    if (ensure_dir("/bbin") != 0) {
        printf("[BBIN] mkdir(/bbin) failed errno=%d\n", errno);
        return;
    }

    struct stat st;
    if (stat("/bbin/busybox", &st) != 0) {
        const char *src = find_busybox_source();
        if (!src) {
            printf("[BBIN] busybox source not found\n");
            return;
        }
        if (copy_file(src, "/bbin/busybox", 0755) != 0) {
            printf("[BBIN] copy %s -> /bbin/busybox failed errno=%d\n",
                   src, errno);
            return;
        }
        printf("[BBIN] copied /bbin/busybox from %s\n", src);
    }

    static const char *applets[] = {
        "[",
        "[[",
        "awk",
        "basename",
        "cat",
        "chmod",
        "chown",
        "cp",
        "cut",
        "date",
        "dd",
        "df",
        "dirname",
        "dmesg",
        "echo",
        "env",
        "expr",
        "false",
        "find",
        "free",
        "grep",
        "head",
        "hexdump",
        "hostname",
        "id",
        "kill",
        "ln",
        "ls",
        "mkdir",
        "mktemp",
        "more",
        "mount",
        "mv",
        "od",
        "printf",
        "ps",
        "pwd",
        "readlink",
        "realpath",
        "rm",
        "rmdir",
        "sed",
        "seq",
        "sh",
        "sleep",
        "sort",
        "stat",
        "sync",
        "tail",
        "tar",
        "tee",
        "test",
        "touch",
        "tr",
        "true",
        "uname",
        "uniq",
        "usleep",
        "wc",
        "which",
        "whoami",
        "xargs",
        "yes",
        NULL,
    };

    for (int i = 0; applets[i]; i++)
        install_applet(applets[i]);

    printf("[BBIN] busybox applets ready under /bbin\n");
}

void setup_dev_runtime_lib_links(void)
{
    static int done;
    if (done)
        return;
    done = 1;

    static const char *const glibc_candidates[] = {
        "/test/glibc/lib",
        "/testrv/glibc/lib",
        "/glibc/lib",
        NULL,
    };
    static const char *const musl_candidates[] = {
        "/test/musl/lib",
        "/testrv/musl/lib",
        "/musl/lib",
        NULL,
    };
    static const char *const glibc_libs[] = {
        "ld-linux-riscv64-lp64d.so.1",
        "libc.so",
        "libc.so.6",
        "libm.so",
        "libm.so.6",
        "dlopen_dso.so",
        "tls_align_dso.so",
        "tls_init_dso.so",
        "tls_get_new-dtv_dso.so",
        NULL,
    };
    static const char *const musl_libs[] = {
        "libc.so",
        "dlopen_dso.so",
        "tls_align_dso.so",
        "tls_init_dso.so",
        "tls_get_new-dtv_dso.so",
        NULL,
    };

    if (ensure_dir("/lib") != 0) {
        printf("[LIBLINK] mkdir(/lib) failed errno=%d\n", errno);
        return;
    }
    ensure_dir("/lib/glibc");
    ensure_dir("/lib/musl");
    ensure_dir("/lib/riscv64-linux-gnu");

    const char *glibc_dir = find_existing_dir(glibc_candidates);
    if (glibc_dir) {
        for (int i = 0; glibc_libs[i]; i++) {
            install_lib_file(glibc_dir, "/lib/glibc", glibc_libs[i]);
            install_lib_file(glibc_dir, "/lib/riscv64-linux-gnu", glibc_libs[i]);
        }
        install_symlink("/lib/glibc/ld-linux-riscv64-lp64d.so.1",
                        "/lib/ld-linux-riscv64-lp64d.so.1");
        printf("[LIBLINK] glibc links ready from %s\n", glibc_dir);
    } else {
        printf("[LIBLINK] glibc lib dir not found\n");
    }

    const char *musl_dir = find_existing_dir(musl_candidates);
    if (musl_dir) {
        for (int i = 0; musl_libs[i]; i++)
            install_lib_file(musl_dir, "/lib/musl", musl_libs[i]);
        install_symlink("/lib/musl/libc.so", "/lib/ld-musl-riscv64.so.1");
        printf("[LIBLINK] musl links ready from %s\n", musl_dir);
    } else {
        printf("[LIBLINK] musl lib dir not found\n");
    }
}
