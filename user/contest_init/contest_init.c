#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "test_runners.h"

#define MAX_TESTS 128
#define TEST_DIR "/test"
#define SUFFIX "_testcode.sh"
#define SUFFIX_LEN (sizeof(SUFFIX) - 1)
#define GLOBAL_TIMEOUT_SEC 1800

typedef struct {
    char path[512];
} test_entry_t;

typedef struct {
    char *test_name;
    int (*func)(const char *script_name, const char *script_dir);
} Test;

static test_entry_t g_tests[MAX_TESTS];
static Test test_table[] = {
    // {"glibc_basic",   run_glibc_basic_test},
    // {"glibc_busybox", run_glibc_busybox_test},
    // {"glibc_lua",     run_glibc_lua_test},
    // {"glibc_libctest", run_glibc_libctest_test},
    {"glibc_ltp", run_glibc_ltp_test}
    // {"musl_basic",    run_musl_basic_test},
    // {"musl_busybox",  run_musl_busybox_test},
    // {"musl_lua", run_musl_lua_test},
};

static int ends_with(const char *s, const char *suffix)
{
    size_t sl = strlen(s);
    size_t xl = strlen(suffix);
    if (xl > sl)
        return 0;
    return strcmp(s + sl - xl, suffix) == 0;
}

struct linux_dirent64_local {
    uint64_t d_ino;
    int64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
};

static void scan_tree(const char *dir, int depth, test_entry_t tests[], int *ntests)
{
    int fd = syscall(SYS_openat, AT_FDCWD, dir, O_RDONLY, 0);
    if (fd < 0) {
        printf("[CONTEST] Cannot open dir %s (errno=%d)\n", dir, errno);
        return;
    }

    char buf[2048];
    for (;;) {
        int nread = syscall(SYS_getdents64, fd, buf, sizeof(buf));
        if (nread <= 0)
            break;

        int bpos = 0;
        while (bpos < nread) {
            struct linux_dirent64_local *de =
                (struct linux_dirent64_local *)(buf + bpos);

            const char *name = de->d_name;
            if (strcmp(name, ".") != 0 && strcmp(name, "..") != 0) {
                char full[256];
                if (strcmp(dir, "/") == 0)
                    snprintf(full, sizeof(full), "/%s", name);
                else
                    snprintf(full, sizeof(full), "%s/%s", dir, name);

                int is_dir = (de->d_type == DT_DIR);

                if (!is_dir && ends_with(name, SUFFIX) && *ntests < MAX_TESTS) {
                    strncpy(tests[*ntests].path, full, sizeof(tests[*ntests].path) - 1);
                    tests[*ntests].path[sizeof(tests[*ntests].path) - 1] = '\0';
                    printf("[CONTEST][SCAN] found[%d] depth=%d path=%s\n",
                           *ntests, depth, tests[*ntests].path);
                    (*ntests)++;
                }

                if (is_dir)
                    scan_tree(full, depth + 1, tests, ntests);
            }

            bpos += de->d_reclen;
        }
    }

    close(fd);
}

static void extract_group(const char *path, char *out, size_t out_sz)
{
    const char *fname = strrchr(path, '/');
    fname = fname ? fname + 1 : path;

    size_t fl = strlen(fname);
    size_t copy = fl;
    if (fl > SUFFIX_LEN && ends_with(fname, SUFFIX))
        copy = fl - SUFFIX_LEN;
    if (copy >= out_sz)
        copy = out_sz - 1;

    memcpy(out, fname, copy);
    out[copy] = '\0';
}

static const char *path_basename(const char *path)
{
    const char *name = strrchr(path, '/');
    return name ? (name + 1) : path;
}

static void script_prefix(const char *script_name, char *out, size_t out_sz)
{
    size_t n = strlen(script_name);
    if (n > SUFFIX_LEN && ends_with(script_name, SUFFIX))
        n -= SUFFIX_LEN;
    if (n >= out_sz)
        n = out_sz - 1;
    memcpy(out, script_name, n);
    out[n] = '\0';
}

static void path_dirname(const char *path, char *out, size_t out_sz)
{
    const char *slash = strrchr(path, '/');
    if (!slash) {
        snprintf(out, out_sz, ".");
        return;
    }
    if (slash == path) {
        snprintf(out, out_sz, "/");
        return;
    }

    size_t n = (size_t)(slash - path);
    if (n >= out_sz)
        n = out_sz - 1;
    memcpy(out, path, n);
    out[n] = '\0';
}

static const char *runtime_from_path(const char *path)
{
    if (!path)
        return "unknown";
    if (strstr(path, "/glibc/"))
        return "glibc";
    if (strstr(path, "/musl/"))
        return "musl";
    return "unknown";
}

static int (*is_registered(const char *script, const char *runtime))(const char *, const char *)
{
    const char *script_name = path_basename(script);
    char prefix[64];
    char key[128];
    script_prefix(script_name, prefix, sizeof(prefix));
    snprintf(key, sizeof(key), "%s_%s", runtime, prefix);
    // printf("[CONTEST] script_name = %s  runtime = %s key = %s\n", script_name, runtime, key);
    int n = (int)(sizeof(test_table) / sizeof(test_table[0]));
    for (int i = 0; i < n; i++) {
        if (strcmp(test_table[i].test_name, key) == 0)
            return test_table[i].func;
    }
    return NULL;
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    printf("[CONTEST] Auto-test runner started\n");
    printf("[CONTEST] Scanning target image at %s\n", TEST_DIR);
    printf("[CONTEST] Global timeout: %d seconds\n", GLOBAL_TIMEOUT_SEC);
    {
        int wpid = fork();
        if (wpid == 0) {
            sleep(GLOBAL_TIMEOUT_SEC);
            printf("[CONTEST] Global timeout reached (%d s), powering off\n", GLOBAL_TIMEOUT_SEC);
            syscall(SYS_reboot, 0);
            while (1) {
            }
        } else if (wpid < 0) {
            printf("[CONTEST] watchdog fork failed errno=%d\n", errno);
        }
    }

    int ntests = 0;
    memset(g_tests, 0, sizeof(g_tests));

    scan_tree(TEST_DIR, 0, g_tests, &ntests);

    printf("[CONTEST] Found %d test script(s)\n", ntests);

    for (int i = 0; i < ntests; i++) {
        const char *script_name = path_basename(g_tests[i].path);
        const char *runtime = runtime_from_path(g_tests[i].path);
        int (*runner)(const char *, const char *) = is_registered(script_name, runtime);

        char group[128];
        extract_group(g_tests[i].path, group, sizeof(group));
        (void)group;
        // printf("[CONTEST][RUN] preparing #%d runtime=%s group=%s script=%s\n",
            //    i, runtime, group, g_tests[i].path);
        // printf("[CONTEST][REGISTER] script=%s registered=%d\n", script_name, runner != NULL);
        // printf("[CONTEST][DISPATCH] %s -> direct function call\n", script_name);

        // printf("[CONTEST] #### OS COMP TEST GROUP START %s ####\n", group);

        if (runner) {
            char script_dir[512];
            path_dirname(g_tests[i].path, script_dir, sizeof(script_dir));
            printf("[CONTEST] run %s\n", g_tests[i].path);
            runner(script_name, script_dir);
        }

        // printf("[CONTEST] #### OS COMP TEST GROUP END %s ####\n", group);
    }

    printf("[CONTEST] All tests done, powering off\n");
    syscall(SYS_reboot, 0);

    while (1) {
    }
    return 0;
}
