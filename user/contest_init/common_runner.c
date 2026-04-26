#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common_runner.h"
#include "runtime_setup.h"

static int is_musl_runtime(const char *runtime)
{
    return runtime && strcmp(runtime, "musl") == 0;
}

static int dir_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int append_path_component(char *buf, size_t buf_size, const char *path)
{
    size_t len;
    int n;

    if (!buf || buf_size == 0 || !path || !*path)
        return -1;

    len = strlen(buf);
    n = snprintf(buf + len, buf_size - len, "%s%s", len == 0 ? "" : ":", path);
    if (n < 0 || (size_t)n >= buf_size - len)
        return -1;
    return 0;
}

static void append_existing_dir(char *buf, size_t buf_size, const char *path)
{
    if (dir_exists(path))
        append_path_component(buf, buf_size, path);
}

static int build_test_exec_path(const char *runtime, char *buf, size_t buf_size)
{
    if (!buf || buf_size == 0)
        return -1;

    buf[0] = '\0';
    if (append_path_component(buf, buf_size, ".") != 0)
        return -1;
    if (append_path_component(buf, buf_size, "/bin") != 0)
        return -1;
    append_existing_dir(buf, buf_size, "/test");

    if (is_musl_runtime(runtime)) {
        append_existing_dir(buf, buf_size, "/test/musl");
        append_existing_dir(buf, buf_size, "/testrv/musl");
        append_existing_dir(buf, buf_size, "/testla/musl");
        append_existing_dir(buf, buf_size, "/test/glibc");
        append_existing_dir(buf, buf_size, "/testrv/glibc");
        append_existing_dir(buf, buf_size, "/testla/glibc");
    } else {
        append_existing_dir(buf, buf_size, "/test/glibc");
        append_existing_dir(buf, buf_size, "/testrv/glibc");
        append_existing_dir(buf, buf_size, "/testla/glibc");
        append_existing_dir(buf, buf_size, "/test/musl");
        append_existing_dir(buf, buf_size, "/testrv/musl");
        append_existing_dir(buf, buf_size, "/testla/musl");
    }

    append_existing_dir(buf, buf_size, "/testrv");
    append_existing_dir(buf, buf_size, "/testla");
    return 0;
}

static int build_test_ld_library_path(const char *runtime, char *buf, size_t buf_size)
{
    if (!buf || buf_size == 0)
        return -1;

    buf[0] = '\0';
    if (is_musl_runtime(runtime)) {
        append_existing_dir(buf, buf_size, "/test/musl/lib");
        append_existing_dir(buf, buf_size, "/testrv/musl/lib");
        append_existing_dir(buf, buf_size, "/testla/musl/lib");
        append_existing_dir(buf, buf_size, "/musl/lib");
        append_existing_dir(buf, buf_size, "/test/glibc/lib");
        append_existing_dir(buf, buf_size, "/testrv/glibc/lib");
        append_existing_dir(buf, buf_size, "/testla/glibc/lib");
    } else {
        append_existing_dir(buf, buf_size, "/test/glibc/lib");
        append_existing_dir(buf, buf_size, "/testrv/glibc/lib");
        append_existing_dir(buf, buf_size, "/testla/glibc/lib");
        append_existing_dir(buf, buf_size, "/glibc/lib");
        append_existing_dir(buf, buf_size, "/test/musl/lib");
        append_existing_dir(buf, buf_size, "/testrv/musl/lib");
        append_existing_dir(buf, buf_size, "/testla/musl/lib");
    }

    return buf[0] ? 0 : -1;
}

int run_script_in_dir(const char *runtime, const char *script_name,
                      const char *script_dir, const char *tag)
{
    int pid = fork();
    if (pid < 0)
        return 127;

    if (pid == 0) {
        if (chdir(script_dir) != 0) {
            printf("[%s] chdir(%s) failed errno=%d\n", tag, script_dir, errno);
            _exit(127);
        }

        if (prepare_runtime_links(runtime) != 0) {
            printf("[%s] runtime lib prep for %s was incomplete\n",
                   tag, runtime ? runtime : "default");
        }

        char path_value[512];
        char ld_library_path_value[512];
        char path_env[576];
        char ld_library_path_env[576];

        if (build_test_exec_path(runtime, path_value, sizeof(path_value)) != 0)
            snprintf(path_value, sizeof(path_value), ".:/bin:/test");
        if (build_test_ld_library_path(runtime, ld_library_path_value,
                                       sizeof(ld_library_path_value)) != 0)
            snprintf(ld_library_path_value, sizeof(ld_library_path_value), "/lib");

        snprintf(path_env, sizeof(path_env), "PATH=%s", path_value);
        snprintf(ld_library_path_env, sizeof(ld_library_path_env),
                 "LD_LIBRARY_PATH=%s", ld_library_path_value);

        char *envp[] = {
            path_env,
            ld_library_path_env,
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
