#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "common_runner.h"

#define MAX_TESTS 128
#define TEST_DIR "/test"
#define SUFFIX "_testcode.sh"
#define SUFFIX_LEN (sizeof(SUFFIX) - 1)
#define GLOBAL_TIMEOUT_SEC 1800

typedef struct {
    char path[512];
} test_entry_t;

typedef struct {
    const char *test_name;
    const char *runtime;
    const char *tag;
} Test;

static test_entry_t g_tests[MAX_TESTS];
static Test test_table[] = {
    {"glibc_basic", "glibc", "TEST][glibc][basic"},
    {"glibc_busybox", "glibc", "TEST][glibc][busybox"},
    {"glibc_lua", "glibc", "TEST][glibc][lua"},
    {"glibc_libctest", "glibc", "TEST][glibc][libctest"},
    {"glibc_ltp", "glibc", "TEST][glibc][ltp"},
    {"musl_basic", "musl", "TEST][musl][basic"},
    {"musl_busybox", "musl", "TEST][musl][busybox"},
    {"musl_lua", "musl", "TEST][musl][lua"},
    {"musl_ltp", "musl", "TEST][musl][ltp"},
};

static int compare_test_entry(const void *lhs, const void *rhs)
{
    const test_entry_t *a = (const test_entry_t *)lhs;
    const test_entry_t *b = (const test_entry_t *)rhs;
    return strcmp(a->path, b->path);
}

static int ends_with(const char *s, const char *suffix)
{
    size_t sl = strlen(s);
    size_t xl = strlen(suffix);
    if (xl > sl)
        return 0;
    return strcmp(s + sl - xl, suffix) == 0;
}

struct a20_dirent64_local {
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
            struct a20_dirent64_local *de =
                (struct a20_dirent64_local *)(buf + bpos);

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

static const Test *find_test_entry(const char *script, const char *runtime)
{
    const char *script_name = path_basename(script);
    char prefix[64];
    char key[128];
    script_prefix(script_name, prefix, sizeof(prefix));
    snprintf(key, sizeof(key), "%s_%s", runtime, prefix);
    int n = (int)(sizeof(test_table) / sizeof(test_table[0]));
    for (int i = 0; i < n; i++) {
        if (strcmp(test_table[i].test_name, key) == 0)
            return &test_table[i];
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
    if (ntests > 1)
        qsort(g_tests, (size_t)ntests, sizeof(g_tests[0]), compare_test_entry);

    printf("[CONTEST] Found %d test script(s)\n", ntests);
    int executed = 0;
    int failed = 0;
    int skipped = 0;

    for (int i = 0; i < ntests; i++) {
        const char *script_name = path_basename(g_tests[i].path);
        const char *runtime = runtime_from_path(g_tests[i].path);
        const Test *test = find_test_entry(script_name, runtime);

        char group[128];
        extract_group(g_tests[i].path, group, sizeof(group));

        if (!test) {
            skipped++;
            printf("[CONTEST][SKIP] runtime=%s script=%s has no registered runner\n",
                   runtime, g_tests[i].path);
            continue;
        }

        char script_dir[512];
        path_dirname(g_tests[i].path, script_dir, sizeof(script_dir));
        printf("[CONTEST][RUN] runtime=%s group=%s script=%s\n",
               runtime, group, g_tests[i].path);
        int rc = run_script_in_dir(test->runtime, script_name, script_dir, test->tag);
        executed++;
        if (rc != 0) {
            failed++;
            printf("[CONTEST][FAIL] runtime=%s group=%s status=%d script=%s\n",
                   runtime, group, rc, g_tests[i].path);
        } else {
            printf("[CONTEST][PASS] runtime=%s group=%s script=%s\n",
                   runtime, group, g_tests[i].path);
        }
    }

    printf("[CONTEST] Summary: executed=%d failed=%d skipped=%d\n",
           executed, failed, skipped);
    printf("[CONTEST] All tests done, powering off\n");
    syscall(SYS_reboot, 0);

    while (1) {
    }
    return 0;
}
