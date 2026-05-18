#ifndef _UNISTD_H
#define _UNISTD_H

#include <stddef.h>

typedef long ssize_t;
typedef long off_t;

int     open(const char *path, int flags, ...);
int     close(int fd);
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
off_t   lseek(int fd, off_t offset, int whence);

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  4
#define O_TRUNC  8
#define O_APPEND 16

#endif
