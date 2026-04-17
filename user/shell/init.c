#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/wait.h>
#include <sys/syscall.h>

#ifdef CONTEST

/* ============================================================
 * Contest mode — auto-test runner
 *
 * Scans /test (competition EXT4 disk) for *_testcode.sh scripts,
 * runs each one sequentially via /bin/sh, prints the required
 * markers, then shuts down.
 * ============================================================ */

#define MAX_TESTS   64
#define TEST_DIR    "/test"
#define SUFFIX      "_testcode.sh"
#define SUFFIX_LEN  13

static int ends_with(const char *s, const char *suffix)
{
    size_t sl = strlen(s);
    size_t xl = strlen(suffix);
    if (xl > sl) return 0;
    return strcmp(s + sl - xl, suffix) == 0;
}

static void extract_group(const char *filename, char *out, size_t out_sz)
{
    size_t fl = strlen(filename);
    size_t copy = fl - SUFFIX_LEN;
    if (copy >= out_sz) copy = out_sz - 1;
    memcpy(out, filename, copy);
    out[copy] = '\0';
}

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    printf("[CONTEST] Auto-test runner started\n");

    chdir(TEST_DIR);

    DIR *d = opendir(TEST_DIR);
    if (!d) {
        printf("[CONTEST] Cannot open %s, shutting down\n", TEST_DIR);
        syscall(SYS_reboot, 0);
    }

    char *tests[MAX_TESTS];
    int ntests = 0;

    struct dirent *de;
    while ((de = readdir(d)) != NULL && ntests < MAX_TESTS) {
        if (ends_with(de->d_name, SUFFIX)) {
            char *path = malloc(256);
            snprintf(path, 256, "%s/%s", TEST_DIR, de->d_name);
            tests[ntests++] = path;
        }
    }
    closedir(d);

    printf("[CONTEST] Found %d test(s)\n", ntests);

    for (int i = 0; i < ntests; i++) {
        const char *slash = strrchr(tests[i], '/');
        const char *fname = slash ? slash + 1 : tests[i];

        char group[128];
        extract_group(fname, group, sizeof(group));

        printf("#### OS COMP TEST GROUP START %s ####\n", group);

        int pid = fork();
        if (pid < 0) {
            printf("[CONTEST] fork failed, skipping %s\n", fname);
        } else if (pid == 0) {
            char *sh_argv[] = { "mksh", tests[i], NULL };
            char *envp[] = { "PATH=/bin:" TEST_DIR, "HOME=/", NULL };
            execve("/bin/mksh", sh_argv, envp);
            printf("[CONTEST] execve failed: %s\n", tests[i]);
            _exit(127);
        } else {
            int status = 0;
            waitpid(pid, &status, 0);
        }

        printf("#### OS COMP TEST GROUP END %s ####\n", group);
        free(tests[i]);
    }

    printf("[CONTEST] All tests done, powering off\n");
    syscall(SYS_reboot, 0);

    while (1) {}
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

static int test_file_io(void) {
    const char *path = "/testrv/testfile.txt";
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { printf("FAIL: open write: %d\n", errno); return 1; }
    const char *data = "Hello from A20OS test!\n";
    if (write(fd, data, strlen(data)) != (int)strlen(data)) {
        printf("FAIL: write\n"); close(fd); return 1;
    }
    close(fd);

    fd = open(path, O_RDONLY, 0);
    if (fd < 0) { printf("FAIL: open read\n"); return 1; }
    char buf[256];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n < 0) { printf("FAIL: read\n"); return 1; }
    buf[n] = '\0';
    if (strcmp(buf, data) != 0) {
        printf("FAIL: data mismatch: '%s' vs '%s'\n", buf, data);
        return 1;
    }

    if (unlink(path) < 0) { printf("FAIL: unlink\n"); return 1; }
    printf("PASS: file I/O\n");
    return 0;
}

static int test_dir_ops(void) {
    const char *dir = "/testrv/testdir";
    if (mkdir(dir, 0755) < 0 && errno != EEXIST) {
        printf("FAIL: mkdir: %d\n", errno); return 1;
    }
    char path[256];
    snprintf(path, sizeof(path), "%s/sub.txt", dir);
    int fd = open(path, O_WRONLY | O_CREAT, 0644);
    if (fd < 0) { printf("FAIL: open in dir\n"); return 1; }
    close(fd);
    if (unlink(path) < 0) { printf("FAIL: unlink in dir\n"); return 1; }
    if (rmdir(dir) < 0) { printf("FAIL: rmdir: %d\n", errno); return 1; }
    printf("PASS: directory ops\n");
    return 0;
}

static int test_pipe(void) {
    int p[2];
    if (pipe(p) < 0) { printf("FAIL: pipe\n"); return 1; }
    const char *msg = "pipe test";
    if (write(p[1], msg, strlen(msg)) != (int)strlen(msg)) {
        printf("FAIL: pipe write\n"); close(p[0]); close(p[1]); return 1;
    }
    close(p[1]);
    char buf[256];
    int n = read(p[0], buf, sizeof(buf) - 1);
    close(p[0]);
    if (n < 0) { printf("FAIL: pipe read\n"); return 1; }
    buf[n] = '\0';
    if (strcmp(buf, msg) != 0) {
        printf("FAIL: pipe data mismatch\n"); return 1;
    }
    printf("PASS: pipe\n");
    return 0;
}

static int test_mmap(void) {
    size_t len = 4096 * 4;
    void *addr = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (addr == MAP_FAILED) { printf("FAIL: mmap\n"); return 1; }
    volatile char *p = (volatile char *)addr;
    for (size_t i = 0; i < len; i++) p[i] = (char)(i & 0xFF);
    for (size_t i = 0; i < len; i++) {
        if (p[i] != (char)(i & 0xFF)) {
            printf("FAIL: mmap data mismatch at %zu\n", i);
            munmap(addr, len);
            return 1;
        }
    }
    if (munmap(addr, len) < 0) { printf("FAIL: munmap\n"); return 1; }
    printf("PASS: mmap/munmap\n");
    return 0;
}

static int test_brk(void) {
    void *orig = sbrk(0);
    if (orig == (void *)-1) { printf("FAIL: sbrk(0)\n"); return 1; }
    void *new_brk = (char *)orig + 8192;
    if (brk(new_brk) < 0) { printf("FAIL: brk\n"); return 1; }
    volatile char *p = (volatile char *)orig;
    for (int i = 0; i < 4096; i++) p[i] = (char)i;
    for (int i = 0; i < 4096; i++) {
        if (p[i] != (char)i) { printf("FAIL: brk data mismatch\n"); return 1; }
    }
    if (brk(orig) < 0) { printf("FAIL: brk restore\n"); return 1; }
    printf("PASS: brk\n");
    return 0;
}

static int test_fork_exec(void) {
    int pid = fork();
    if (pid < 0) { printf("FAIL: fork\n"); return 1; }
    if (pid == 0) {
        char *argv[] = {"echo", "fork_exec_ok", NULL};
        execve("/bin/echo", argv, NULL);
        _exit(1);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) { printf("FAIL: waitpid\n"); return 1; }
    if (status != 0) { printf("FAIL: child exit status %d\n", status); return 1; }
    printf("PASS: fork/exec\n");
    return 0;
}

static int test_fork_pipe(void) {
    int p[2];
    if (pipe(p) < 0) { printf("FAIL: pipe in fork_pipe\n"); return 1; }
    int pid = fork();
    if (pid < 0) { printf("FAIL: fork in fork_pipe\n"); close(p[0]); close(p[1]); return 1; }
    if (pid == 0) {
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
    if (n < 0) { printf("FAIL: read in fork_pipe\n"); return 1; }
    buf[n] = '\0';
    if (strcmp(buf, "child_to_parent") != 0) {
        printf("FAIL: fork_pipe data mismatch: '%s'\n", buf);
        return 1;
    }
    printf("PASS: fork+pipe\n");
    return 0;
}

static int test_dup(void) {
    int fd = open("/testrv/testdup.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { printf("FAIL: open dup\n"); return 1; }
    int fd2 = dup(fd);
    if (fd2 < 0) { printf("FAIL: dup\n"); close(fd); return 1; }
    const char *msg = "dup_test";
    if (write(fd2, msg, strlen(msg)) != (int)strlen(msg)) {
        printf("FAIL: write dup\n"); close(fd); close(fd2); return 1;
    }
    close(fd2);
    close(fd);
    fd = open("/testrv/testdup.txt", O_RDONLY, 0);
    if (fd < 0) { printf("FAIL: open dup verify\n"); return 1; }
    char buf[256];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    buf[n] = '\0';
    if (strcmp(buf, msg) != 0) { printf("FAIL: dup data mismatch\n"); return 1; }
    unlink("/testrv/testdup.txt");
    printf("PASS: dup\n");
    return 0;
}

static int test_stress(void) {
    for (int round = 0; round < 10; round++) {
        int pid = fork();
        if (pid < 0) { printf("FAIL: stress fork round %d\n", round); return 1; }
        if (pid == 0) {
            char cmd[256];
            snprintf(cmd, sizeof(cmd),
                "i=1; while [ $i -le %d ]; do mksh -c 'echo 1' & i=$((i+1)); done; wait",
                10);
            char *mksh_argv[] = {"mksh", "-c", cmd, NULL};
            char *envp[] = { "PATH=/bin", "HOME=/", NULL };
            execve("/bin/mksh", mksh_argv, envp);
            _exit(1);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        if (status != 0) { printf("FAIL: stress round %d status=%d\n", round, status); return 1; }
    }
    printf("PASS: fork/exec stress\n");
    return 0;
}

int main(int argc, char *argv[])
{
    (void)argc;
    char *sh_argv[] = {"mksh", NULL};
    char *envp[] = { "PATH=/bin", "HOME=/", NULL };
    execve("/bin/mksh", sh_argv, envp);
    printf("[INIT] Failed to start shell, powering off.\n");
    syscall(SYS_reboot, 0);
    while (1) {}
    return 0;
}

#endif /* CONTEST */
