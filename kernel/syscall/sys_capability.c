#include "syscall_internal.h"

#define LINUX_CAPABILITY_VERSION_1 0x19980330U
#define LINUX_CAPABILITY_VERSION_2 0x20071026U
#define LINUX_CAPABILITY_VERSION_3 0x20080522U

typedef struct {
    uint32_t version;
    int pid;
} cap_user_header_t;

typedef struct {
    uint32_t effective;
    uint32_t permitted;
    uint32_t inheritable;
} cap_user_data_t;

#define CAP_SETPCAP 8

static int cap_version_words(uint32_t version)
{
    if (version == LINUX_CAPABILITY_VERSION_1) return 1;
    if (version == LINUX_CAPABILITY_VERSION_2 || version == LINUX_CAPABILITY_VERSION_3) return 2;
    return -EINVAL;
}

static uint64_t cap_data_effective(const cap_user_data_t data[2], int words)
{
    uint64_t value = data[0].effective;
    if (words > 1)
        value |= (uint64_t)data[1].effective << 32;
    return value;
}

static uint64_t cap_data_permitted(const cap_user_data_t data[2], int words)
{
    uint64_t value = data[0].permitted;
    if (words > 1)
        value |= (uint64_t)data[1].permitted << 32;
    return value;
}

static uint64_t cap_data_inheritable(const cap_user_data_t data[2], int words)
{
    uint64_t value = data[0].inheritable;
    if (words > 1)
        value |= (uint64_t)data[1].inheritable << 32;
    return value;
}

static void cap_put_words(cap_user_data_t data[2], int words,
                          uint64_t effective, uint64_t permitted,
                          uint64_t inheritable)
{
    memset(data, 0, sizeof(cap_user_data_t) * 2);
    data[0].effective = (uint32_t)effective;
    data[0].permitted = (uint32_t)permitted;
    data[0].inheritable = (uint32_t)inheritable;
    if (words > 1) {
        data[1].effective = (uint32_t)(effective >> 32);
        data[1].permitted = (uint32_t)(permitted >> 32);
        data[1].inheritable = (uint32_t)(inheritable >> 32);
    }
}

int64_t sys_capget(void *hdrp, void *datap)
{
    if (!hdrp) return -EFAULT;
    cap_user_header_t hdr;
    if (copy_from_user(&hdr, hdrp, sizeof(hdr)) < 0) return -EFAULT;
    int words = cap_version_words(hdr.version);
    if (words < 0) {
        uint32_t v = LINUX_CAPABILITY_VERSION_3;
        copy_to_user(hdrp, &v, sizeof(v));
        return -EINVAL;
    }
    task_t *cur = proc_current();
    if (hdr.pid < 0) return -EINVAL;
    task_t *target = cur;
    if (hdr.pid > 0) {
        target = proc_find(hdr.pid);
        if (!target) return -ESRCH;
    }
    if (!datap) return 0;

    cap_user_data_t data[2];
    uint64_t effective = target ? target->cap_effective : 0;
    uint64_t permitted = target ? target->cap_permitted : 0;
    uint64_t inheritable = target ? target->cap_inheritable : 0;
    cap_put_words(data, words, effective, permitted, inheritable);
    return copy_to_user(datap, data, sizeof(cap_user_data_t) * (size_t)words) < 0 ?
           -EFAULT : 0;
}

int64_t sys_capset(void *hdrp, const void *datap)
{
    if (!hdrp || !datap) return -EFAULT;
    cap_user_header_t hdr;
    if (copy_from_user(&hdr, hdrp, sizeof(hdr)) < 0) return -EFAULT;
    int words = cap_version_words(hdr.version);
    if (words < 0) {
        uint32_t v = LINUX_CAPABILITY_VERSION_3;
        copy_to_user(hdrp, &v, sizeof(v));
        return -EINVAL;
    }
    task_t *cur = proc_current();
    if (hdr.pid < 0) return -EINVAL;
    if (hdr.pid > 0) {
        task_t *target = proc_find(hdr.pid);
        if (!target) return -ESRCH;
        if (!cur || hdr.pid != cur->pid) return -EPERM;
    }
    cap_user_data_t data[2];
    if (copy_from_user(data, datap, sizeof(cap_user_data_t) * (size_t)words) < 0)
        return -EFAULT;
    if (!cur) return -ESRCH;

    uint64_t old_effective = cur->cap_effective;
    uint64_t old_permitted = cur->cap_permitted;
    uint64_t old_inheritable = cur->cap_inheritable;
    uint64_t new_effective = cap_data_effective(data, words);
    uint64_t new_permitted = cap_data_permitted(data, words);
    uint64_t new_inheritable = cap_data_inheritable(data, words);
    if (words == 1) {
        new_effective |= old_effective & 0xffffffff00000000ULL;
        new_permitted |= old_permitted & 0xffffffff00000000ULL;
        new_inheritable |= old_inheritable & 0xffffffff00000000ULL;
    }

    if ((new_effective & ~new_permitted) != 0)
        return -EPERM;
    if ((new_permitted & ~old_permitted) != 0)
        return -EPERM;
    uint64_t inheritable_allowed = old_inheritable | old_permitted;
    if (old_effective & (1ULL << CAP_SETPCAP))
        inheritable_allowed |= cur->cap_bounding;
    if ((new_inheritable & ~inheritable_allowed) != 0)
        return -EPERM;

    cur->cap_effective = new_effective;
    cur->cap_permitted = new_permitted;
    cur->cap_inheritable = new_inheritable;
    return 0;
}
