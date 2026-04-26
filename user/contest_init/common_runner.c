#include <errno.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common_runner.h"

int run_script_in_dir(const char *script_name, const char *script_dir, const char *tag)
{
    int pid = fork();
    if (pid < 0)
        return 127;

    if (pid == 0) {
        if (chdir(script_dir) != 0) {
            printf("[%s] chdir(%s) failed errno=%d\n", tag, script_dir, errno);
            _exit(127);
        }

        char *envp[] = {
            "PATH=.:/bin:/test:/test/glibc:/test/musl:/testrv/glibc:/testrv/musl",
            "HOME=/",
            NULL,
        };

        char *argv[] = {"mksh", (char *)script_name, NULL};
        execve("/bin/mksh", argv, envp);

        printf("[%s] mksh exec failed: dir=%s script=%s errno=%d\n",
               tag, script_dir, script_name, errno);
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return 127;

    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return 127;
}
