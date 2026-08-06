typedef unsigned long size_t;

extern int open(const char *path, int flags, ...);
extern int close(int fd);
extern long write(int fd, const void *buffer, size_t count);
extern int fork(void);
extern int execve(const char *path, char *const argv[], char *const envp[]);
extern int waitpid(int pid, int *status, int options);
extern int unlink(const char *path);
extern void _exit(int status);

#define O_WRONLY 01
#define O_CREAT  0100
#define O_TRUNC  01000

static size_t string_length(const char *text)
{
    size_t length = 0;
    while (text[length] != '\0')
        length++;
    return length;
}

static void write_text(const char *text)
{
    (void)write(1, text, string_length(text));
}

int probe_main(void)
{
    static const char path[] = "/work/a20-stage7-shebang-probe.sh";
    static const char script[] = "#!/usr/bin/env bash\nexit 37\n";
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0)
        return 1;
    if (write(fd, script, sizeof(script) - 1) != (long)(sizeof(script) - 1) ||
        close(fd) != 0) {
        (void)unlink(path);
        return 2;
    }

    int pid = fork();
    if (pid < 0) {
        (void)unlink(path);
        return 3;
    }
    if (pid == 0) {
        char *argv[] = {(char *)path, (char *)0};
        char *envp[] = {
            "PATH=/root/.cargo/bin:/usr/local/bin:/usr/bin:/bin:/sbin:/usr/sbin",
            (char *)0,
        };
        execve(path, argv, envp);
        _exit(127);
    }

    int status = 0;
    int waited = waitpid(pid, &status, 0);
    (void)unlink(path);
    if (waited != pid || (status & 0x7f) != 0 ||
        ((status >> 8) & 0xff) != 37)
        return 4;

    write_text("BUILDSTORM_STAGE7_SHEBANG_EXEC ok interpreter=/usr/bin/env arg=bash\n");
    return 0;
}
