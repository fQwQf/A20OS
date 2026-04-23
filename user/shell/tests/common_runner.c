#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "common_runner.h"

static int split_dir_file(const char *path, char *dir, size_t dir_sz, char *file, size_t file_sz)
{
    const char *slash = strrchr(path, '/');
    if (!slash) {
        snprintf(dir, dir_sz, ".");
        snprintf(file, file_sz, "%s", path);
        return 0;
    }
    if (slash == path) {
        snprintf(dir, dir_sz, "/");
    } else {
        size_t n = (size_t)(slash - path);
        if (n >= dir_sz)
            n = dir_sz - 1;
        memcpy(dir, path, n);
        dir[n] = '\0';
    }
    snprintf(file, file_sz, "%s", slash + 1);
    return 0;
}

int run_script_via_mksh(const char *script_path, const char *tag, const char *path_env)
{
    char script_dir[512];
    char script_file[256];
    split_dir_file(script_path, script_dir, sizeof(script_dir),
                   script_file, sizeof(script_file));

    if (chdir(script_dir) != 0) {
        printf("[%s] chdir(%s) failed errno=%d\n", tag, script_dir, errno);
        return 127;
    }

    char *envp[] = {
        (char *)(path_env ? path_env : "PATH=.:/bin:/test:/test/glibc:/test/musl"),
        "HOME=/",
        NULL
    };

    char *mksh_argv[] = { "mksh", script_file, NULL };
    execve("/bin/mksh", mksh_argv, envp);

    printf("[%s] mksh exec failed: script=%s errno=%d\n", tag, script_path, errno);
    return 127;
}
