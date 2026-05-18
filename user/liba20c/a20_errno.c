/*
 * A20OS liba20c — A20 error code to POSIX errno mapping.
 */
#include <errno.h>

int __a20_to_errno(int a20_err)
{
    switch (a20_err) {
    case 0:  return 0;
    case 1:  return EPERM;
    case 2:  return ENOENT;
    case 3:  return EINTR;
    case 4:  return EIO;
    case 5:  return EBADF;
    case 6:  return ENOMEM;
    case 7:  return EACCES;
    case 8:  return EFAULT;
    case 9:  return EBUSY;
    case 10: return EEXIST;
    case 11: return ENOTSUP;
    case 12: return EINVAL;
    case 13: return ENOSPC;
    case 14: return ENOTDIR;
    case 15: return EISDIR;
    case 16: return ENOTEMPTY;
    case 17: return ENAMETOOLONG;
    case 18: return EWOULDBLOCK;
    case 19: return ETIMEDOUT;
    case 20: return ECANCELED;
    case 21: return EPROTO;
    case 22: return ERANGE;
    case 23: return EINVAL;
    case 24: return ENOENT;
    case 25: return ETIMEDOUT;
    default: return EINVAL;
    }
}
