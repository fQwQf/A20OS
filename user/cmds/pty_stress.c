#include <pty.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define PTY_STRESS_ROUNDS 16

static int run_round(int round)
{
    int master_fd = -1;
    char name[32];
    struct winsize size = {
        .ws_row = 24,
        .ws_col = 80,
    };

    pid_t pid = forkpty(&master_fd, name, NULL, &size);
    if (pid < 0) {
        perror("forkpty");
        return 1;
    }
    if (pid == 0) {
        char *argv[] = {"mksh", "-c", "printf PTY_OK", NULL};
        char *envp[] = {
            "PATH=/bin:/usr/bin",
            "HOME=/",
            "SHELL=/bin/mksh",
            "TERM=xterm",
            NULL,
        };
        execve("/bin/mksh", argv, envp);
        _exit(127);
    }

    char output[128];
    size_t used = 0;
    for (;;) {
        ssize_t count = read(master_fd, output + used, sizeof(output) - 1 - used);
        if (count < 0) {
            perror("pty read");
            close(master_fd);
            waitpid(pid, NULL, 0);
            return 1;
        }
        if (count == 0)
            break;
        used += (size_t)count;
        if (used == sizeof(output) - 1)
            break;
    }
    output[used] = '\0';
    close(master_fd);

    int status = 0;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0 || strstr(output, "PTY_OK") == NULL) {
        printf("pty_stress: round %d failed on %s, status=%d, output=%s\n",
               round, name, status, output);
        return 1;
    }
    return 0;
}

int main(void)
{
    for (int round = 1; round <= PTY_STRESS_ROUNDS; round++) {
        if (run_round(round) != 0)
            return 1;
    }
    printf("pty_stress: PASS (%d sessions)\n", PTY_STRESS_ROUNDS);
    return 0;
}
