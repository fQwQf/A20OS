#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "common_runner.h"
#include "test_runners.h"

static int ensure_dir(const char *path) {
    struct stat st;

    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode))
            return 0;
        printf("[TEST][glibc] %s exists but is not a directory\n", path);
        return -1;
    }

    if (errno != ENOENT) {
        printf("[TEST][glibc] stat(%s) failed errno=%d\n", path, errno);
        return -1;
    }

    if (mkdir(path, 0755) != 0) {
        printf("[TEST][glibc] mkdir(%s) failed errno=%d\n", path, errno);
        return -1;
    }

    return 0;
}

struct linux_dirent64_local {
    uint64_t d_ino;
    int64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
};

static void cp_lib() {
    static const char *src = "/test/glibc/lib/ld-linux-riscv64-lp64d.so.1";
    static const char *dst = "/lib/ld-linux-riscv64-lp64d.so.1";

    if (ensure_dir("/lib") != 0)
        return;

    int src_fd = open(src, O_RDONLY);
    if (src_fd < 0) {
        printf("[TEST][glibc] open(%s) failed errno=%d\n", src, errno);
        return;
    }

    int dst_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (dst_fd < 0) {
        printf("[TEST][glibc] open(%s) failed errno=%d\n", dst, errno);
        close(src_fd);
        return;
    }

    char buf[16384];
    int ok = 1;
    for (;;) {
        ssize_t n = read(src_fd, buf, sizeof(buf));
        if (n == 0)
            break;
        if (n < 0) {
            printf("[TEST][glibc] read(%s) failed errno=%d\n", src, errno);
            ok = 0;
            break;
        }

        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(dst_fd, buf + off, (size_t)(n - off));
            if (w <= 0) {
                printf("[TEST][glibc] write(%s) failed errno=%d\n", dst, errno);
                close(src_fd);
                close(dst_fd);
                return;
            }
            off += w;
        }
    }

    close(src_fd);
    close(dst_fd);
    if (ok)
        printf("[TEST][glibc] copied %s\n", dst);
}

static void cp_lib_riscv64_linux_gnu() {
    static const char *src_dir = "/test/glibc/lib";
    static const char *dst_dir = "/lib/riscv64-linux-gnu";
    struct stat st;

    if (ensure_dir("/lib") != 0)
        return;
    if (ensure_dir(dst_dir) != 0)
        return;

    int dir_fd = syscall(SYS_openat, AT_FDCWD, src_dir, O_RDONLY, 0);
    if (dir_fd < 0) {
        printf("[TEST][glibc] open dir %s failed errno=%d\n", src_dir, errno);
        return;
    }

    char src[512];
    char dst[512];
    char buf[16384];
    char dir_buf[2048];
    for (;;) {
        int nread = syscall(SYS_getdents64, dir_fd, dir_buf, sizeof(dir_buf));
        if (nread == 0)
            break;
        if (nread < 0) {
            printf("[TEST][glibc] getdents64(%s) failed errno=%d\n", src_dir, errno);
            break;
        }

        int bpos = 0;
        while (bpos < nread) {
            struct linux_dirent64_local *ent =
                (struct linux_dirent64_local *)(dir_buf + bpos);
            const char *name = ent->d_name;

            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
                bpos += ent->d_reclen;
                continue;
            }

            int src_len = snprintf(src, sizeof(src), "%s/%s", src_dir, name);
            int dst_len = snprintf(dst, sizeof(dst), "%s/%s", dst_dir, name);
            if (src_len < 0 || dst_len < 0 ||
                src_len >= (int)sizeof(src) || dst_len >= (int)sizeof(dst)) {
                printf("[TEST][glibc] path too long: %s\n", name);
                bpos += ent->d_reclen;
                continue;
            }

            int src_fd = open(src, O_RDONLY);
            if (src_fd < 0) {
                printf("[TEST][glibc] open(%s) failed errno=%d\n", src, errno);
                bpos += ent->d_reclen;
                continue;
            }
            if (fstat(src_fd, &st) != 0 || !S_ISREG(st.st_mode)) {
                close(src_fd);
                bpos += ent->d_reclen;
                continue;
            }

            int dst_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777);
            if (dst_fd < 0) {
                printf("[TEST][glibc] open(%s) failed errno=%d\n", dst, errno);
                close(src_fd);
                bpos += ent->d_reclen;
                continue;
            }

            int ok = 1;
            for (;;) {
                ssize_t n = read(src_fd, buf, sizeof(buf));
                if (n == 0)
                    break;
                if (n < 0) {
                    printf("[TEST][glibc] read(%s) failed errno=%d\n", src, errno);
                    ok = 0;
                    break;
                }

                ssize_t off = 0;
                while (off < n) {
                    ssize_t w = write(dst_fd, buf + off, (size_t)(n - off));
                    if (w <= 0) {
                        printf("[TEST][glibc] write(%s) failed errno=%d\n", dst, errno);
                        close(src_fd);
                        close(dst_fd);
                        close(dir_fd);
                        return;
                    }
                    off += w;
                }
            }

            close(src_fd);
            close(dst_fd);
            if (ok)
                printf("[TEST][glibc] copied %s\n", dst);

            bpos += ent->d_reclen;
        }
    }

    close(dir_fd);
}

static void cp_busybox() {
    static const char *src = "/test/glibc/busybox";
    static const char *dst = "/bin/busybox";
    struct stat st;

    if (stat(dst, &st) == 0)
        return;

    if (ensure_dir("/bin") != 0)
        return;

    int src_fd = open(src, O_RDONLY);
    if (src_fd < 0) {
        printf("[TEST][glibc] open(%s) failed errno=%d\n", src, errno);
        return;
    }

    int dst_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (dst_fd < 0) {
        printf("[TEST][glibc] open(%s) failed errno=%d\n", dst, errno);
        close(src_fd);
        return;
    }

    char buf[16384];
    int ok = 1;
    for (;;) {
        ssize_t n = read(src_fd, buf, sizeof(buf));
        if (n == 0)
            break;
        if (n < 0) {
            printf("[TEST][glibc] read(%s) failed errno=%d\n", src, errno);
            ok = 0;
            break;
        }

        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(dst_fd, buf + off, (size_t)(n - off));
            if (w <= 0) {
                printf("[TEST][glibc] write(%s) failed errno=%d\n", dst, errno);
                close(src_fd);
                close(dst_fd);
                return;
            }
            off += w;
        }
    }

    close(src_fd);
    close(dst_fd);
    if (ok)
        printf("[TEST][glibc] copied %s\n", dst);
}

int run_glibc_basic_test(const char *script_name, const char *script_dir)
{
    cp_lib();
    return run_script_in_dir(script_name, script_dir, "TEST][glibc][basic");
}

int run_glibc_busybox_test(const char *script_name, const char *script_dir)
{
    // cp_lib();
    return run_script_in_dir(script_name, script_dir, "TEST][glibc][busybox");
}

int run_glibc_libctest_test(const char *script_name, const char *script_dir) {
    // cp_lib();
    return run_script_in_dir(script_name, script_dir, "TEST][glibc][libctest");
}

int run_glibc_lua_test(const char *script_name, const char *script_dir) {
    cp_busybox();
    return run_script_in_dir(script_name, script_dir, "TEST][glibc][lua");
}

int run_glibc_ltp_test(const char *script_name, const char *script_dir) {
    // cp_lib_riscv64_linux_gnu();
    // cp_lib();
    return run_script_in_dir(script_name ,script_dir, "TEST][glibc][ltp");
}
