#include "../lib/libc.h"

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    int pid = fork();
    if (pid < 0)
    {
        printf("[INIT] fork failed, shutting down.\n");
        syscall1(SYS_reboot, 0);
    }
    if (pid == 0)
    {
        char *sh_argv[] = {"sh", NULL};
        execve("/bin/sh", sh_argv, NULL);
        printf("[INIT] execve failed.\n");
        _exit(127);
    }

    int status = 0;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
    {
        printf("[INIT] Shell exited cleanly. Powering off.\n");
        syscall1(SYS_reboot, 0);
    }

    while (1)
    {
    }
    return 0;
}
