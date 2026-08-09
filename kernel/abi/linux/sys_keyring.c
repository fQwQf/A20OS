#include "syscall_impl.h"

#include "ipc/keyring.h"

/*
 * Linux keyring syscalls.  This file only translates the Linux wire shapes
 * (user pointers, string bounds, cmd/arg dispatch) and delegates to the
 * kernel keyring subsystem in kernel/ipc/keyring.c.
 */

int64_t sys_add_key(const char *type, const char *description,
                    const void *payload, size_t plen, int32_t ringid)
{
    if (plen > KEYRING_PAYLOAD_MAX)
        return -E2BIG;
    if (!type || !description)
        return -EFAULT;

    char type_buf[KEYRING_TYPE_MAX];
    char desc_buf[KEYRING_DESC_MAX];
    if (user_strncpy(type_buf, type, sizeof(type_buf)) < 0 ||
        type_buf[0] == '\0')
        return -EFAULT;
    if (user_strncpy(desc_buf, description, sizeof(desc_buf)) < 0 ||
        desc_buf[0] == '\0')
        return -EFAULT;

    void *payload_k = NULL;
    if (plen > 0) {
        if (!payload)
            return -EFAULT;
        payload_k = proc_scratch_buffer(plen);
        if (!payload_k)
            return -ENOMEM;
        if (copy_from_user(payload_k, payload, plen) < 0)
            return -EFAULT;
    }

    return (int64_t)keyring_add_key(type_buf, desc_buf, payload_k, plen,
                                    ringid);
}

int64_t sys_request_key(const char *type, const char *description,
                        const char *callout_info, int32_t dest_ringid)
{
    (void)callout_info;
    if (!type || !description)
        return -EFAULT;

    char type_buf[KEYRING_TYPE_MAX];
    char desc_buf[KEYRING_DESC_MAX];
    if (user_strncpy(type_buf, type, sizeof(type_buf)) < 0 ||
        type_buf[0] == '\0')
        return -EFAULT;
    if (user_strncpy(desc_buf, description, sizeof(desc_buf)) < 0 ||
        desc_buf[0] == '\0')
        return -EFAULT;

    return (int64_t)keyring_request_key(type_buf, desc_buf, dest_ringid);
}

int64_t sys_keyctl(int cmd, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a5;
    switch (cmd) {
    case KEYCTL_GET_KEYRING_ID:
        return (int64_t)keyring_get_keyring_id((key_serial_t)a2, (int)a3);

    case KEYCTL_JOIN_SESSION_KEYRING: {
        if (a2 == 0)
            return (int64_t)keyring_join_session(NULL);
        char name[KEYRING_NAME_MAX];
        if (user_strncpy(name, (const char *)(uintptr_t)a2, sizeof(name)) < 0)
            return -EFAULT;
        return (int64_t)keyring_join_session(name);
    }

    case KEYCTL_CHOWN:
        return (int64_t)keyring_chown((key_serial_t)a2, (int32_t)a3,
                                      (int32_t)a4);

    case KEYCTL_SETPERM:
        return (int64_t)keyring_setperm((key_serial_t)a2, (uint32_t)a3);

    case KEYCTL_SET_TIMEOUT:
        return (int64_t)keyring_set_timeout((key_serial_t)a2, a3);

    case KEYCTL_LINK:
        return (int64_t)keyring_link((key_serial_t)a2, (key_serial_t)a3);

    case KEYCTL_UNLINK:
        return (int64_t)keyring_unlink((key_serial_t)a2, (key_serial_t)a3);

    case KEYCTL_SEARCH: {
        if (!a3 || !a4)
            return -EFAULT;
        char type_buf[KEYRING_TYPE_MAX];
        char desc_buf[KEYRING_DESC_MAX];
        if (user_strncpy(type_buf, (const char *)(uintptr_t)a3,
                         sizeof(type_buf)) < 0)
            return -EFAULT;
        if (user_strncpy(desc_buf, (const char *)(uintptr_t)a4,
                         sizeof(desc_buf)) < 0)
            return -EFAULT;
        return (int64_t)keyring_search((key_serial_t)a2, type_buf, desc_buf);
    }

    case KEYCTL_DESCRIBE: {
        /* Query the full description length first, then render into a
         * kernel scratch buffer bounded by its own size (never the caller's
         * a4, which could exceed the scratch capacity).  A NULL buffer with
         * a4 == 0 is a length-only query (Linux semantics). */
        long need = keyring_describe((key_serial_t)a2, NULL, 0);
        if (need < 0)
            return need;
        if (!a3)
            return need + 1;
        void *buf = proc_scratch_buffer((size_t)need + 1);
        if (!buf)
            return -ENOMEM;
        long len = keyring_describe((key_serial_t)a2, buf, (size_t)need + 1);
        if (len < 0)
            return len;
        size_t copy = (size_t)len < a4 ? (size_t)len : (size_t)a4;
        if (copy > 0 && copy_to_user((void *)(uintptr_t)a3, buf, copy) < 0)
            return -EFAULT;
        return len + 1;
    }

    case KEYCTL_READ: {
        if (!a3)
            return -EFAULT;
        long len = keyring_read((key_serial_t)a2, NULL, 0);
        if (len < 0)
            return len;
        void *buf = proc_scratch_buffer((size_t)len + 1);
        if (!buf)
            return -ENOMEM;
        len = keyring_read((key_serial_t)a2, buf, (size_t)len + 1);
        if (len < 0)
            return len;
        size_t copy = (size_t)len < a4 ? (size_t)len : (size_t)a4;
        if (copy > 0 && copy_to_user((void *)(uintptr_t)a3, buf, copy) < 0)
            return -EFAULT;
        return len;
    }

    case KEYCTL_CLEAR:
    case KEYCTL_INVALIDATE:
        /* keyctl(KEYCTL_CLEAR/INVALIDATE): empty or invalidate the keyring. */
        return -EOPNOTSUPP;

    default:
        return -EOPNOTSUPP;
    }
}
