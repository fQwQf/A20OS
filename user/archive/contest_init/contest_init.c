/* DEPRECATED — replaced by contest.sh + unified shell/init.c */
#if 0
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common_runner.h"

#define MAX_TESTS 128
#define TEST_DIR "/test"
#define SUFFIX "_testcode.sh"
#define SUFFIX_LEN (sizeof(SUFFIX) - 1)
#define GLOBAL_TIMEOUT_SEC 1800
#define MAX_BLACKLIST 256
#define MAX_BL_NAME 64

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
    /* LTP first — most important, longest running */
    {"glibc_ltp", "glibc", "TEST][glibc][ltp"},
    {"musl_ltp", "musl", "TEST][musl][ltp"},
    /* basic sanity */
    {"glibc_basic", "glibc", "TEST][glibc][basic"},
    {"musl_basic", "musl", "TEST][musl][basic"},
    /* busybox */
    {"glibc_busybox", "glibc", "TEST][glibc][busybox"},
    {"musl_busybox", "musl", "TEST][musl][busybox"},
    /* benchmarks */
    {"glibc_iozone", "glibc", "TEST][glibc][iozone"},
    {"musl_iozone", "musl", "TEST][musl][iozone"},
    {"glibc_netperf", "glibc", "TEST][glibc][netperf"},
    {"musl_netperf", "musl", "TEST][musl][netperf"},
    {"glibc_lua", "glibc", "TEST][glibc][lua"},
    {"musl_lua", "musl", "TEST][musl][lua"},
    {"glibc_libcbench", "glibc", "TEST][glibc][libcbench"},
    {"musl_libcbench", "musl", "TEST][musl][libcbench"},
    {"glibc_libctest", "glibc", "TEST][glibc][libctest"},
    {"musl_libctest", "musl", "TEST][musl][libctest"},
    {"glibc_cyclictest", "glibc", "TEST][glibc][cyclictest"},
    {"musl_cyclictest", "musl", "TEST][musl][cyclictest"},
    {"glibc_lmbench", "glibc", "TEST][glibc][lmbench"},
    {"musl_lmbench", "musl", "TEST][musl][lmbench"},
    {"glibc_iperf", "glibc", "TEST][glibc][iperf"},
    {"musl_iperf", "musl", "TEST][musl][iperf"},
};

static int compare_test_entry(const void *lhs, const void *rhs)
{
    const test_entry_t *a = (const test_entry_t *)lhs;
    const test_entry_t *b = (const test_entry_t *)rhs;
    return strcmp(a->path, b->path);
}

struct a20_dirent64_local {
    uint64_t d_ino;
    int64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
};

static char g_ltp_blacklist[MAX_BLACKLIST][MAX_BL_NAME];
static int g_ltp_blacklist_count;

static void load_ltp_blacklist(void)
{
    const char *paths[] = {
        "/etc/ltp_blacklist.txt",
        "/ltp_blacklist.txt",
    };
    g_ltp_blacklist_count = 0;

    for (int p = 0; p < (int)(sizeof(paths) / sizeof(paths[0])); p++) {
        int fd = open(paths[p], O_RDONLY);
        if (fd < 0)
            continue;
        printf("[CONTEST][LTP] loading blacklist from %s\n", paths[p]);

        char line[128];
        int pos = 0;
        for (;;) {
            char ch;
            ssize_t n = read(fd, &ch, 1);
            if (n <= 0) {
                if (pos > 0 && g_ltp_blacklist_count < MAX_BLACKLIST) {
                    line[pos] = '\0';
                    if (line[0] != '#' && line[0] != '\0')
                        strncpy(g_ltp_blacklist[g_ltp_blacklist_count++], line, MAX_BL_NAME - 1);
                }
                break;
            }
            if (ch == '\n') {
                if (pos > 0 && pos < (int)sizeof(line) && g_ltp_blacklist_count < MAX_BLACKLIST) {
                    line[pos] = '\0';
                    if (line[0] != '#' && line[0] != '\0')
                        strncpy(g_ltp_blacklist[g_ltp_blacklist_count++], line, MAX_BL_NAME - 1);
                }
                pos = 0;
            } else if (pos < (int)sizeof(line) - 1) {
                line[pos++] = ch;
            }
        }
        close(fd);
        if (g_ltp_blacklist_count > 0)
            return;
    }
}

static int is_ltp_blacklisted(const char *testcase)
{
    for (int i = 0; i < g_ltp_blacklist_count; i++) {
        if (strcmp(g_ltp_blacklist[i], testcase) == 0)
            return 1;
    }
    return 0;
}

static int is_ltp_group(const char *group)
{
    return strstr(group, "ltp") != NULL;
}

static int find_ltp_bin_dir(const char *script_dir, const char *runtime, char *out, size_t out_sz)
{
    const char *arch_primary =
#if defined(__loongarch64)
        "la";
#else
        "rv";
#endif

    const char *candidates[] = {
        NULL, NULL, NULL, NULL, NULL, NULL,
    };
    char buf[256];
    int n = 0;

    snprintf(buf, sizeof(buf), "%s/ltp/testcases/bin", script_dir);
    candidates[n++] = strdup(buf);
    snprintf(buf, sizeof(buf), "/test/%s/ltp/testcases/bin", runtime);
    candidates[n++] = strdup(buf);
    snprintf(buf, sizeof(buf), "/test%s/%s/ltp/testcases/bin", arch_primary, runtime);
    candidates[n++] = strdup(buf);
    snprintf(buf, sizeof(buf), "/%s/ltp/testcases/bin", runtime);
    candidates[n++] = strdup(buf);

    struct stat st;
    for (int i = 0; i < n; i++) {
        if (candidates[i] && stat(candidates[i], &st) == 0 && S_ISDIR(st.st_mode)) {
            snprintf(out, out_sz, "%s", candidates[i]);
            for (int j = 0; j < n; j++) free((void *)candidates[j]);
    return 0;
}
#endif
    }
    for (int j = 0; j < n; j++) free((void *)candidates[j]);
    return -1;
}

static int run_ltp_inline(const char *script_dir, const char *runtime, const char *group)
{
    char bin_dir[512];
    if (find_ltp_bin_dir(script_dir, runtime, bin_dir, sizeof(bin_dir)) != 0) {
        printf("#### OS COMP TEST GROUP START %s ####\n", group);
        printf("[CONTEST][LTP] cannot find ltp/testcases/bin for %s\n", runtime);
        printf("#### OS COMP TEST GROUP END %s ####\n", group);
        return 127;
    }

    printf("#### OS COMP TEST GROUP START %s ####\n", group);
    printf("[CONTEST][LTP] using bin_dir=%s blacklist=%d entries\n",
           bin_dir, g_ltp_blacklist_count);

    int total = 0, passed = 0, skipped = 0;

    int dfd = open(bin_dir, O_RDONLY);
    if (dfd < 0) {
        printf("[CONTEST][LTP] cannot open %s errno=%d\n", bin_dir, errno);
        printf("#### OS COMP TEST GROUP END %s ####\n", group);
        return 127;
    }

    char dentbuf[2048];
    for (;;) {
        int nread = syscall(SYS_getdents64, dfd, dentbuf, sizeof(dentbuf));
        if (nread <= 0)
            break;

        int bpos = 0;
        while (bpos < nread) {
            struct a20_dirent64_local *de = (struct a20_dirent64_local *)(dentbuf + bpos);
            const char *name = de->d_name;

            if (strcmp(name, ".") != 0 && strcmp(name, "..") != 0 &&
                de->d_type != DT_DIR && !strstr(name, ".sh")) {
                if (is_ltp_blacklisted(name)) {
                    skipped++;
                    printf("[CONTEST][LTP][SKIP] %s (blacklisted)\n", name);
                } else {
                    total++;
                    char test_path[512];
                    snprintf(test_path, sizeof(test_path), "%s/%s", bin_dir, name);

                    printf("RUN LTP CASE %s\n", name);

                    int pid = fork();
                    if (pid == 0) {
                        char *argv[] = {"sh", "-c", test_path, NULL};
                        char path_env[256];
                        snprintf(path_env, sizeof(path_env), "PATH=/bin:/test/%s:/test", runtime);
                        char *envp[] = {path_env, "HOME=/", NULL};
                        execve("/bin/mksh", argv, envp);
                        _exit(127);
                    }

                    int status = 0;
                    waitpid(pid, &status, 0);
                    int ret = WIFEXITED(status) ? WEXITSTATUS(status) :
                             (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 127);

                    if (ret == 0) {
                        printf("END LTP CASE %s : 0\n", name);
                        passed++;
                    } else {
                        printf("FAIL LTP CASE %s : %d\n", name, ret);
                    }
                }
            }
            bpos += de->d_reclen;
        }
    }
    close(dfd);

    printf("\nSummary:\npassed   %d\nfailed   %d\nbroken   0\nskipped  %d\nwarnings 0\n",
           passed, total - passed, skipped);
    printf("#### OS COMP TEST GROUP END %s ####\n", group);

    printf("[CONTEST][LTP] total=%d passed=%d skipped(blacklisted)=%d\n",
           total, passed, skipped);
    return (total > 0 && passed == total) ? 0 : 1;
}

static int ends_with(const char *s, const char *suffix)
{
    size_t sl = strlen(s);
    size_t xl = strlen(suffix);
    if (xl > sl)
        return 0;
    return strcmp(s + sl - xl, suffix) == 0;
}

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

    load_ltp_blacklist();
    printf("[CONTEST] LTP blacklist: %d entries\n", g_ltp_blacklist_count);

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

        int rc;
        if (is_ltp_group(group)) {
            printf("[CONTEST][RUN] runtime=%s group=%s (inline LTP runner)\n",
                   runtime, group);
            rc = run_ltp_inline(script_dir, runtime, group);
        } else {
            printf("[CONTEST][RUN] runtime=%s group=%s script=%s\n",
                   runtime, group, g_tests[i].path);
            rc = run_script_in_dir(test->runtime, script_name, script_dir, test->tag);
        }
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
