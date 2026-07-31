/*
 * A20OS Native ABI — Syscall input validation helpers.
 * Design reference: docs/native-abi/06-security.md §3, errors.md §2
 */
#include "core/types.h"
#include "core/defs.h"
#include "core/string.h"
#include "sys/usercopy.h"
#include "abi/native/types.h"
#include "abi/native/errno.h"

static inline int a20_validate_user_ptr(const void *ptr, uint64_t size)
{
    if (!ptr) return -A20_ERR_FAULT;
    if (size == 0) return A20_OK;
    uintptr_t end = (uintptr_t)ptr + size;
    if (end < (uintptr_t)ptr) return -A20_ERR_FAULT;
    return A20_OK;
}

/*
 * Struct evolution rules (docs/native-abi/01-types.md §2):
 *  - size must cover the complete layout for the presented version (short
 *    version-1 structures are rejected rather than zero-padding required
 *    fields);
 *  - version 0 is rejected; versions newer than the kernel supports are
 *    rejected, older/equal versions are accepted (E-APPEND only appends);
 *  - a larger user struct is truncated to the fields the kernel knows.
 */
static inline int a20_validate_struct_header(const void *ptr, uint32_t expected_size, uint32_t expected_version)
{
    if (!ptr) return -A20_ERR_FAULT;
    uint32_t hdr[2];
    if (copy_from_user(hdr, ptr, sizeof(hdr)) < 0)
        return -A20_ERR_FAULT;
    if (hdr[0] < expected_size) return -A20_ERR_INVALID_ARGUMENT;
    if (hdr[1] == 0 || hdr[1] > expected_version) return -A20_ERR_INVALID_ARGUMENT;
    return A20_OK;
}

/* Copy output fields back without overrunning an older, shorter userspace
 * structure. The user's original size header is authoritative; fields beyond
 * that size are not written. */
static inline int a20_copy_struct_to_user(void *ptr, const void *local,
                                          uint32_t kernel_size)
{
    uint32_t user_size;
    if (!ptr || copy_from_user(&user_size, ptr, sizeof(user_size)) < 0)
        return -A20_ERR_FAULT;
    if (user_size < sizeof(a20_abi_header_t))
        return -A20_ERR_INVALID_ARGUMENT;
    uint32_t copy_size = user_size < kernel_size ? user_size : kernel_size;
    if (copy_to_user(ptr, local, copy_size) < 0)
        return -A20_ERR_FAULT;
    return A20_OK;
}

#define A20_VALIDATE_AND_COPY(args_ptr, local_var) \
    do { \
        int _vr = a20_validate_struct_header((args_ptr), sizeof(local_var), 1); \
        if (_vr < 0) return _vr; \
        uint32_t _usz; \
        if (copy_from_user(&_usz, (args_ptr), sizeof(_usz)) < 0) \
            return -A20_ERR_FAULT; \
        memset(&(local_var), 0, sizeof(local_var)); \
        uint32_t _cpn = _usz < (uint32_t)sizeof(local_var) \
                        ? _usz : (uint32_t)sizeof(local_var); \
        if (copy_from_user(&(local_var), (args_ptr), _cpn) < 0) \
            return -A20_ERR_FAULT; \
    } while (0)
