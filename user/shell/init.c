#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <errno.h>
#include <sys/stat.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include "tests/test_runners.h"

#ifdef CONTEST

/* ============================================================
 * Contest mode — auto test runner
 *
 * Recursively scans /test for *_testcode.sh and executes each
 * script serially. Output includes required START/END markers.
 * ============================================================ */

#define MAX_TESTS 128
#define TEST_DIR "/test"
#define SUFFIX "_testcode.sh"
#define SUFFIX_LEN 13
#define GLOBAL_TIMEOUT_SEC 300

typedef struct {
    char path[512];
} test_entry_t;

/* Avoid large stack frame in init: keep test table in static storage. */
static test_entry_t g_tests[MAX_TESTS];

static int ends_with(const char *s, const char *suffix)
{
    size_t sl = strlen(s);
    size_t xl = strlen(suffix);
    if (xl > sl)
        return 0;
    return strcmp(s + sl - xl, suffix) == 0;
}

struct linux_dirent64_local {
    uint64_t       d_ino;
    int64_t        d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[];
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

static int dispatch_test_script(const char *script_path)
{
    const char *script_name = path_basename(script_path);
    const char *runtime = runtime_from_path(script_path);

    if (strcmp(script_name, "basic_testcode.sh") == 0) {
        if (strcmp(runtime, "glibc") == 0)
            return run_glibc_basic_test(script_path);
        if (strcmp(runtime, "musl") == 0)
            return run_musl_basic_test(script_path);
    }

    return -1;
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
            while (1) { }
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
        if (strcmp(runtime, "unknown") == 0) {
            printf("[CONTEST][SKIP] unknown runtime class: %s\n", g_tests[i].path);
            continue;
        }

        if (strcmp(script_name, "basic_testcode.sh") != 0) {
            printf("[CONTEST][SKIP] unregistered %s test script: %s\n", runtime, g_tests[i].path);
            continue;
        }

        char group[128];
        extract_group(g_tests[i].path, group, sizeof(group));
        printf("[CONTEST][RUN] preparing #%d runtime=%s group=%s script=%s\n",
               i, runtime, group, g_tests[i].path);
        printf("[CONTEST][DISPATCH] %s -> dedicated C runner\n", script_name);

        printf("#### OS COMP TEST GROUP START %s ####\n", group);

        int rc = dispatch_test_script(g_tests[i].path);
        printf("[CONTEST][RUN] rc=%d script=%s\n", rc, g_tests[i].path);

        printf("#### OS COMP TEST GROUP END %s ####\n", group);
    }

    printf("[CONTEST] All tests done, powering off\n");
    syscall(SYS_reboot, 0);

    while (1)
    {
    }
    return 0;
}

#else

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

static int test_file_io(void)
{
    const char *path = "/testrv/testfile.txt";
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
    {
        printf("FAIL: open write: %d\n", errno);
        return 1;
    }
    const char *data = "Hello from A20OS test!\n";
    if (write(fd, data, strlen(data)) != (int)strlen(data))
    {
        printf("FAIL: write\n");
        close(fd);
        return 1;
    }
    close(fd);

    fd = open(path, O_RDONLY, 0);
    if (fd < 0)
    {
        printf("FAIL: open read\n");
        return 1;
    }
    char buf[256];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n < 0)
    {
        printf("FAIL: read\n");
        return 1;
    }
    buf[n] = '\0';
    if (strcmp(buf, data) != 0)
    {
        printf("FAIL: data mismatch: '%s' vs '%s'\n", buf, data);
        return 1;
    }

    if (unlink(path) < 0)
    {
        printf("FAIL: unlink\n");
        return 1;
    }
    printf("PASS: file I/O\n");
    return 0;
}

static int test_dir_ops(void)
{
    const char *dir = "/testrv/testdir";
    if (mkdir(dir, 0755) < 0 && errno != EEXIST)
    {
        printf("FAIL: mkdir: %d\n", errno);
        return 1;
    }
    char path[256];
    snprintf(path, sizeof(path), "%s/sub.txt", dir);
    int fd = open(path, O_WRONLY | O_CREAT, 0644);
    if (fd < 0)
    {
        printf("FAIL: open in dir\n");
        return 1;
    }
    close(fd);
    if (unlink(path) < 0)
    {
        printf("FAIL: unlink in dir\n");
        return 1;
    }
    if (rmdir(dir) < 0)
    {
        printf("FAIL: rmdir: %d\n", errno);
        return 1;
    }
    printf("PASS: directory ops\n");
    return 0;
}

static int test_pipe(void)
{
    int p[2];
    if (pipe(p) < 0)
    {
        printf("FAIL: pipe\n");
        return 1;
    }
    const char *msg = "pipe test";
    if (write(p[1], msg, strlen(msg)) != (int)strlen(msg))
    {
        printf("FAIL: pipe write\n");
        close(p[0]);
        close(p[1]);
        return 1;
    }
    close(p[1]);
    char buf[256];
    int n = read(p[0], buf, sizeof(buf) - 1);
    close(p[0]);
    if (n < 0)
    {
        printf("FAIL: pipe read\n");
        return 1;
    }
    buf[n] = '\0';
    if (strcmp(buf, msg) != 0)
    {
        printf("FAIL: pipe data mismatch\n");
        return 1;
    }
    printf("PASS: pipe\n");
    return 0;
}

static int test_mmap(void)
{
    size_t len = 4096 * 4;
    void *addr = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (addr == MAP_FAILED)
    {
        printf("FAIL: mmap\n");
        return 1;
    }
    volatile char *p = (volatile char *)addr;
    for (size_t i = 0; i < len; i++)
        p[i] = (char)(i & 0xFF);
    for (size_t i = 0; i < len; i++)
    {
        if (p[i] != (char)(i & 0xFF))
        {
            printf("FAIL: mmap data mismatch at %zu\n", i);
            munmap(addr, len);
            return 1;
        }
    }
    if (munmap(addr, len) < 0)
    {
        printf("FAIL: munmap\n");
        return 1;
    }
    printf("PASS: mmap/munmap\n");
    return 0;
}

static int test_brk(void)
{
    void *orig = sbrk(0);
    if (orig == (void *)-1)
    {
        printf("FAIL: sbrk(0)\n");
        return 1;
    }
    void *new_brk = (char *)orig + 8192;
    if (brk(new_brk) < 0)
    {
        printf("FAIL: brk\n");
        return 1;
    }
    volatile char *p = (volatile char *)orig;
    for (int i = 0; i < 4096; i++)
        p[i] = (char)i;
    for (int i = 0; i < 4096; i++)
    {
        if (p[i] != (char)i)
        {
            printf("FAIL: brk data mismatch\n");
            return 1;
        }
    }
    if (brk(orig) < 0)
    {
        printf("FAIL: brk restore\n");
        return 1;
    }
    printf("PASS: brk\n");
    return 0;
}

static int test_fork_exec(void)
{
    int pid = fork();
    if (pid < 0)
    {
        printf("FAIL: fork\n");
        return 1;
    }
    if (pid == 0)
    {
        char *argv[] = {"echo", "fork_exec_ok", NULL};
        execve("/bin/echo", argv, NULL);
        _exit(1);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
    {
        printf("FAIL: waitpid\n");
        return 1;
    }
    if (status != 0)
    {
        printf("FAIL: child exit status %d\n", status);
        return 1;
    }
    printf("PASS: fork/exec\n");
    return 0;
}

static int test_fork_pipe(void)
{
    int p[2];
    if (pipe(p) < 0)
    {
        printf("FAIL: pipe in fork_pipe\n");
        return 1;
    }
    int pid = fork();
    if (pid < 0)
    {
        printf("FAIL: fork in fork_pipe\n");
        close(p[0]);
        close(p[1]);
        return 1;
    }
    if (pid == 0)
    {
        close(p[0]);
        const char *msg = "child_to_parent";
        write(p[1], msg, strlen(msg));
        close(p[1]);
        _exit(0);
    }
    close(p[1]);
    char buf[256];
    int n = read(p[0], buf, sizeof(buf) - 1);
    close(p[0]);
    waitpid(pid, NULL, 0);
    if (n < 0)
    {
        printf("FAIL: read in fork_pipe\n");
        return 1;
    }
    buf[n] = '\0';
    if (strcmp(buf, "child_to_parent") != 0)
    {
        printf("FAIL: fork_pipe data mismatch: '%s'\n", buf);
        return 1;
    }
    printf("PASS: fork+pipe\n");
    return 0;
}

static int test_dup(void)
{
    int fd = open("/testrv/testdup.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
    {
        printf("FAIL: open dup\n");
        return 1;
    }
    int fd2 = dup(fd);
    if (fd2 < 0)
    {
        printf("FAIL: dup\n");
        close(fd);
        return 1;
    }
    const char *msg = "dup_test";
    if (write(fd2, msg, strlen(msg)) != (int)strlen(msg))
    {
        printf("FAIL: write dup\n");
        close(fd);
        close(fd2);
        return 1;
    }
    close(fd2);
    close(fd);
    fd = open("/testrv/testdup.txt", O_RDONLY, 0);
    if (fd < 0)
    {
        printf("FAIL: open dup verify\n");
        return 1;
    }
    char buf[256];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    buf[n] = '\0';
    if (strcmp(buf, msg) != 0)
    {
        printf("FAIL: dup data mismatch\n");
        return 1;
    }
    unlink("/testrv/testdup.txt");
    printf("PASS: dup\n");
    return 0;
}

static int test_stress(void)
{
    for (int round = 0; round < 10; round++)
    {
        int pid = fork();
        if (pid < 0)
        {
            printf("FAIL: stress fork round %d\n", round);
            return 1;
        }
        if (pid == 0)
        {
            char cmd[256];
            snprintf(cmd, sizeof(cmd),
                     "i=1; while [ $i -le %d ]; do mksh -c 'echo 1' & i=$((i+1)); done; wait",
                     10);
            char *mksh_argv[] = {"mksh", "-c", cmd, NULL};
            char *envp[] = {"PATH=/bin", "HOME=/", NULL};
            execve("/bin/mksh", mksh_argv, envp);
            _exit(1);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        if (status != 0)
        {
            printf("FAIL: stress round %d status=%d\n", round, status);
            return 1;
        }
    }
    printf("PASS: fork/exec stress\n");
    return 0;
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    int pid = fork();
    if (pid < 0)
    {
        printf("[INIT] fork failed, shutting down.\n");
        syscall(SYS_reboot, 0);
    }
    if (pid == 0)
    {
        char *sh_argv[] = {"mksh", NULL};
        execve("/bin/mksh", sh_argv, NULL);
        printf("[INIT] execve failed.\n");
        _exit(127);
    }

    int status = 0;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
    {
        printf("[INIT] Shell exited cleanly. Powering off.\n");
        syscall(SYS_reboot, 0);
    }

    while (1)
    {
    }
    return 0;
}

#endif /* CONTEST */
