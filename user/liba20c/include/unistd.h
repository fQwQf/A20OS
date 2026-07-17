#ifndef _UNISTD_H
#define _UNISTD_H

#include <sys/types.h>
#include <sys/stat.h>

struct timespec;

int     open(const char *path, int flags, ...);
int     close(int fd);
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
off_t   lseek(int fd, off_t offset, int whence);

int     stat(const char *path, struct stat *buf);
int     fstat(int fd, struct stat *buf);
int     lstat(const char *path, struct stat *buf);
int     mkdir(const char *path, mode_t mode);
int     rmdir(const char *path);
int     unlink(const char *path);
int     rename(const char *old, const char *new);
int     link(const char *old, const char *new);
int     symlink(const char *target, const char *linkpath);
ssize_t readlink(const char *path, char *buf, size_t bufsiz);
char   *getcwd(char *buf, size_t size);
int     chdir(const char *path);
int     dup(int oldfd);
int     dup2(int oldfd, int newfd);
int     pipe(int pipefd[2]);
int     access(const char *path, int amode);
unsigned int sleep(unsigned int seconds);
int     nanosleep(const struct timespec *req, struct timespec *rem);
pid_t   getpid(void);
uid_t   getuid(void);
gid_t   getgid(void);

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2

#define O_CREAT     0x0040
#define O_EXCL      0x0080
#define O_NOCTTY    0x0100
#define O_TRUNC     0x0200
#define O_APPEND    0x0400
#define O_NONBLOCK  0x0800
#define O_DIRECTORY 0x10000
#define O_NOFOLLOW  0x20000
#define O_CLOEXEC   0x80000
#define O_TMPFILE   0x400000

#define R_OK 4
#define W_OK 2
#define X_OK 1
#define F_OK 0

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define PATH_MAX 4096

#endif
