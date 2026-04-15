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

#else /* Development mode — launch interactive shell */

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
