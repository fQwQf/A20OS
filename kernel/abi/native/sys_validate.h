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

static inline int a20_validate_struct_header(const void *ptr, uint32_t expected_min_size)
{
    if (!ptr) return -A20_ERR_FAULT;
    uint32_t sz;
    if (copy_from_user(&sz, ptr, sizeof(uint32_t)) < 0)
        return -A20_ERR_FAULT;
    if (sz < expected_min_size) return -A20_ERR_INVALID_ARGUMENT;
    return A20_OK;
}
