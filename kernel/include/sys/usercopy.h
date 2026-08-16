#ifndef _SYS_USERCOPY_H
#define _SYS_USERCOPY_H

#include "core/types.h"
#include "core/errno.h"

/*
 * User pointer boundary helpers.
 *
 * Syscall ABI code must copy user memory through these functions before
 * handing data to core kernel subsystems. Internal fs/mm/proc/net code should
 * operate on kernel-owned buffers or already-validated scalar values.
 */
long copy_from_user(void *dst, const void *src, size_t n);
long copy_to_user(void *dst, const void *src, size_t n);
long user_strncpy(char *dst, const char *src, size_t max);
long user_strnlen(const char *src, size_t max);
int  user_buffer_segment(const void *user, size_t len, int write,
                         void **kaddr, size_t *chunk);

/*
 * Copy a user path and reject a kernel buffer that filled up without a NUL:
 * user_strncpy() silently truncates at max-1, so callers must check before
 * operating on a wrong path.  Returns 0 on success, -EFAULT on a bad user
 * pointer, or -ENAMETOOLONG when the source has no terminator within max-1.
 */
static inline long user_path_strncpy(char *dst, const char *src, size_t max)
{
    long r = user_strncpy(dst, src, max);
    if (r < 0)
        return r;
    if (r >= (long)(max - 1) && dst[max - 2] != '\0')
        return -ENAMETOOLONG;
    return 0;
}

#endif /* _SYS_USERCOPY_H */
