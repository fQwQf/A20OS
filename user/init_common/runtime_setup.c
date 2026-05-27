#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "runtime_setup.h"

#if defined(__riscv)
#define GLIBC_LOADER_NAME "ld-linux-riscv64-lp64d.so.1"
#define MUSL_LOADER_NAME "ld-musl-riscv64.so.1"
#elif defined(__aarch64__)
#define GLIBC_LOADER_NAME "ld-linux-aarch64.so.1"
#define MUSL_LOADER_NAME "ld-musl-aarch64.so.1"
#elif defined(__loongarch64)
#define GLIBC_LOADER_NAME "ld-linux-loongarch-lp64d.so.1"
#define MUSL_LOADER_NAME "ld-musl-loongarch-lp64d.so.1"
#else
#error Unsupported architecture
#endif

static int is_glibc_runtime(const char *runtime)
{
    return runtime && strcmp(runtime, "glibc") == 0;
}

static int is_musl_runtime(const char *runtime)
{
    return runtime && strcmp(runtime, "musl") == 0;
}

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

static int install_runtime_loader(const char *src_dir, const char *src_name,
                                  const char *dst_name)
{
    char src[256];
    char dst[256];
    struct stat st;

    snprintf(src, sizeof(src), "%s/%s", src_dir, src_name);
    if (stat(src, &st) != 0 || !S_ISREG(st.st_mode))
        return -1;
    int mode = st.st_mode & 0777;

    snprintf(dst, sizeof(dst), "/lib/%s", dst_name);
    if (stat(dst, &st) == 0 && S_ISREG(st.st_mode))
        goto try_lib64;
    unlink(dst);

    if (copy_file(src, dst, mode) != 0) {
        printf("[LIBLINK] copy %s -> %s failed errno=%d\n",
               src, dst, errno);
        return -1;
    }
    printf("[LIBLINK] copied %s\n", dst);

try_lib64:
    snprintf(dst, sizeof(dst), "/lib64/%s", dst_name);
    if (stat(dst, &st) == 0 && S_ISREG(st.st_mode))
        return 0;
    unlink(dst);
    if (copy_file(src, dst, mode) == 0) {
        printf("[LIBLINK] copied %s\n", dst);
    }
    return 0;
}

static int install_runtime_library_from_dirs(const char *const dirs[],
                                             const char *name)
{
    char src[256];
    char dst[256];
    struct stat st;

    snprintf(dst, sizeof(dst), "/lib/%s", name);
    if (stat(dst, &st) == 0 && S_ISREG(st.st_mode))
        return 0;

    for (int i = 0; dirs[i]; i++) {
        snprintf(src, sizeof(src), "%s/%s", dirs[i], name);
        if (stat(src, &st) != 0 || !S_ISREG(st.st_mode))
            continue;

        unlink(dst);
        if (copy_file(src, dst, st.st_mode & 0777) == 0) {
            printf("[LIBLINK] copied %s\n", dst);
            return 0;
        }
    }

    return -1;
}

static int ensure_runtime_dirs(void)
{
    if (ensure_dir("/lib") != 0) {
        printf("[LIBLINK] mkdir(/lib) failed errno=%d\n", errno);
        return -1;
    }
    if (ensure_dir("/lib64") != 0) {
        printf("[LIBLINK] mkdir(/lib64) failed errno=%d\n", errno);
        /* non-fatal: some systems don't need /lib64 */
    }
    return 0;
}

static void prepare_lmbench_helper_path(void)
{
    static int done;
    static const char script[] = "#!/bin/sh\nexec ./lmbench_all \"$@\"\n";
    const char *dirs[] = {
        "/code",
        "/code/lmbench_src",
        "/code/lmbench_src/bin",
        "/code/lmbench_src/bin/build",
    };
    const char *path = "/code/lmbench_src/bin/build/lmbench_all";
    struct stat st;

    if (done)
        return;

    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        if (ensure_dir(dirs[i]) != 0)
            return;
    }

    if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
        done = 1;
        return;
    }

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0)
        return;

    size_t off = 0;
    size_t len = sizeof(script) - 1;
    while (off < len) {
        ssize_t n = write(fd, script + off, len - off);
        if (n <= 0)
            break;
        off += (size_t)n;
    }
    close(fd);
    chmod(path, 0755);
    if (off == len)
        done = 1;
}

static int prepare_glibc_lib_links(void)
{
    static int done;
    static const char *const glibc_candidates[] = {
#if defined(__loongarch64)
        "/testla/glibc/lib",
        "/test/glibc/lib",
        "/testrv/glibc/lib",
#elif defined(__riscv)
        "/testrv/glibc/lib",
        "/test/glibc/lib",
        "/testla/glibc/lib",
#else
        "/test/glibc/lib",
        "/testrv/glibc/lib",
        "/testla/glibc/lib",
#endif
        "/glibc/lib",
        NULL,
    };
    static const char *const libgcc_candidates[] = {
#if defined(__loongarch64)
        "/testla/glibc/lib",
        "/test/glibc/lib",
        "/testrv/glibc/lib",
#elif defined(__riscv)
        "/testrv/glibc/lib",
        "/test/glibc/lib",
        "/testla/glibc/lib",
#else
        "/test/glibc/lib",
        "/testrv/glibc/lib",
        "/testla/glibc/lib",
#endif
        "/glibc/lib",
        "/bin/lib",
        NULL,
    };
    const char *glibc_dir;

    if (done)
        return 0;
    if (ensure_runtime_dirs() != 0)
        return -1;

    glibc_dir = find_existing_dir(glibc_candidates);
    if (!glibc_dir) {
        printf("[LIBLINK] glibc lib dir not found\n");
        return -1;
    }

    if (install_runtime_loader(glibc_dir, GLIBC_LOADER_NAME, GLIBC_LOADER_NAME) != 0)
        return -1;
    install_runtime_library_from_dirs(libgcc_candidates, "libgcc_s.so.1");
    done = 1;
    printf("[LIBLINK] glibc loader ready from %s\n", glibc_dir);
    return 0;
}

static int prepare_musl_lib_links(void)
{
    static int done;
    static const char *const musl_candidates[] = {
#if defined(__loongarch64)
        "/testla/musl/lib",
        "/test/musl/lib",
        "/testrv/musl/lib",
#elif defined(__riscv)
        "/testrv/musl/lib",
        "/test/musl/lib",
        "/testla/musl/lib",
#else
        "/test/musl/lib",
        "/testrv/musl/lib",
        "/testla/musl/lib",
#endif
        "/musl/lib",
        NULL,
    };
    const char *musl_dir;

    if (done)
        return 0;
    if (ensure_runtime_dirs() != 0)
        return -1;

    musl_dir = find_existing_dir(musl_candidates);
    if (!musl_dir) {
        printf("[LIBLINK] musl lib dir not found\n");
        return -1;
    }

    if (install_runtime_loader(musl_dir, "libc.so", MUSL_LOADER_NAME) != 0)
        return -1;
    done = 1;
    printf("[LIBLINK] musl loader ready from %s\n", musl_dir);
    return 0;
}

void setup_runtime_links(void)
{
    prepare_runtime_links(NULL);
}

int prepare_runtime_links(const char *runtime)
{
    int rc = 0;

    prepare_lmbench_helper_path();

    if (!runtime || (!is_glibc_runtime(runtime) && !is_musl_runtime(runtime))) {
        if (prepare_glibc_lib_links() != 0)
            rc = -1;
        if (prepare_musl_lib_links() != 0)
            rc = -1;
        return rc;
    }

    if (is_glibc_runtime(runtime))
        return prepare_glibc_lib_links();
    return prepare_musl_lib_links();
}
