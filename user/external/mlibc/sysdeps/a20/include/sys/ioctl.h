#ifndef _SYS_IOCTL_H
#define _SYS_IOCTL_H

#include <abi-bits/ioctls.h>
#include <bits/winsize.h>

#ifdef __cplusplus
extern "C" {
#endif

int ioctl(int __fd, unsigned long __request, ...);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_IOCTL_H */
