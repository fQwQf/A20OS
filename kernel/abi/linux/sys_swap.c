#include "syscall_impl.h"
#include "core/errno.h"
#include "drivers/block/virtio_blk.h"

#ifdef CONFIG_SWAP

static block_dev_t *swap_path_to_block_dev(const char *path)
{
    static const char prefix[] = "/dev/vd";
    const char *p = path;

    if (strncmp(p, prefix, sizeof(prefix) - 1) != 0)
        return NULL;
    p += sizeof(prefix) - 1;

    int index = 0;
    if (*p >= 'a' && *p <= 'z' && p[1] == '\0') {
        index = *p - 'a';
    } else {
        if (*p < '0' || *p > '9')
            return NULL;
        while (*p >= '0' && *p <= '9') {
            if (index > 214748364)
                return NULL;
            index = index * 10 + (*p++ - '0');
        }
        if (*p != '\0')
            return NULL;
    }
    return virtio_blk_get_dev(index);
}

long sys_swapon(const char *path, int flags)
{
    char kpath[MAX_PATH_LEN];
    if (!path)
        return -EFAULT;
    if (user_strncpy(kpath, path, sizeof(kpath)) < 0)
        return -EFAULT;

    block_dev_t *bdev = swap_path_to_block_dev(kpath);
    if (!bdev)
        return -EINVAL;
    (void)flags;

    int type = swap_register_device(bdev, kpath);
    return type < 0 ? type : 0;
}

long sys_mkswap(const char *path, int flags)
{
    char kpath[MAX_PATH_LEN];
    if (!path)
        return -EFAULT;
    if (user_strncpy(kpath, path, sizeof(kpath)) < 0)
        return -EFAULT;

    block_dev_t *bdev = swap_path_to_block_dev(kpath);
    if (!bdev)
        return -EINVAL;
    return swap_format_device(bdev, kpath, flags & 0xFFFF, NULL);
}

long sys_swapoff(const char *path)
{
    char kpath[MAX_PATH_LEN];
    if (!path)
        return -EFAULT;
    if (user_strncpy(kpath, path, sizeof(kpath)) < 0)
        return -EFAULT;

    for (int type = 0; type < MAX_SWAPFILES; type++) {
        swap_info_struct *si = &swap_info[type];
        if (!si->active || !si->name || strcmp(si->name, kpath) != 0)
            continue;
        if (si->inuse_pages != 0)
            return -EBUSY;
        swap_unregister_device(type);
        return 0;
    }
    return -EINVAL;
}

#endif
