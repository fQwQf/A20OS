/*
 * A20OS Native ABI — Syscall input validation helpers.
 * Design reference: docs/native-abi/06-security.md §3, errors.md §2
 */
#include "core/types.h"
#include "core/defs.h"
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

static inline int a20_validate_struct_header(const void *ptr, uint32_t expected_size, uint32_t expected_version)
{
    if (!ptr) return -A20_ERR_FAULT;
    uint32_t hdr[2];
    if (copy_from_user(hdr, ptr, sizeof(hdr)) < 0)
        return -A20_ERR_FAULT;
    if (hdr[0] != expected_size) return -A20_ERR_INVALID_ARGUMENT;
    if (hdr[1] != expected_version) return -A20_ERR_INVALID_ARGUMENT;
    return A20_OK;
}

#define A20_VALIDATE_AND_COPY(args_ptr, local_var) \
    do { \
        int _vr = a20_validate_struct_header((args_ptr), sizeof(local_var), 1); \
        if (_vr < 0) return _vr; \
        if (copy_from_user(&(local_var), (args_ptr), sizeof(local_var)) < 0) \
            return -A20_ERR_FAULT; \
    } while (0)
