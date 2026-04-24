#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common_runner.h"
#include "test_runners.h"

static void cp_lib() {
    static const char *src = "/test/glibc/lib/ld-linux-riscv64-lp64d.so.1";
    static const char *dst = "/lib/ld-linux-riscv64-lp64d.so.1";
    struct stat st;
    if (stat("/lib", &st) == 0 && S_ISDIR(st.st_mode))
        return;

    if (mkdir("/lib", 0755) != 0 && errno != EEXIST) {
        printf("[TEST][glibc] mkdir(/lib) failed errno=%d\n", errno);
        return;
    }

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
    for (;;) {
        ssize_t n = read(src_fd, buf, sizeof(buf));
        if (n == 0)
            break;
        if (n < 0) {
            printf("[TEST][glibc] read(%s) failed errno=%d\n", src, errno);
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
