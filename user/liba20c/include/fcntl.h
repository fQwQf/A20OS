#ifndef _FCNTL_H
#define _FCNTL_H

#include <unistd.h>

int fcntl(int fd, int cmd, ...);

#define F_DUPFD  0
#define F_GETFD  1
#define F_SETFD  2
#define F_GETFL  3
#define F_SETFL  4

#endif
