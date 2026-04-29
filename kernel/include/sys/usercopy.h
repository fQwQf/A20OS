#ifndef _SYS_USERCOPY_H
#define _SYS_USERCOPY_H

#include "core/types.h"

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
int  user_buffer_segment(const void *user, size_t len, int write,
                         void **kaddr, size_t *chunk);

#endif /* _SYS_USERCOPY_H */
