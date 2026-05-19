#include "core/defs.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/consts.h"
#include "core/lock.h"
#include "fs/vfs.h"
#include "fs/devfs.h"
#include "fs/fdtable.h"
#include "fs/file.h"
#include "abi/linux/errno.h"
#include "mm/mm.h"

#define MAX_LOOP_DEVS 8
#define LOOP_SECTOR_SIZE 512

#define LOOP_SET_FD       0x4C00
#define LOOP_CLR_FD       0x4C01
#define LOOP_SET_STATUS64 0x4C04
#define LOOP_GET_STATUS64 0x4C05

#define BLKGETSIZE64      0x80041272
#define BLKGETSIZE        0x1260
#define BLKSSZGET         0x1268

typedef struct {
    uint64_t lo_device;
    uint64_t lo_inode;
    uint64_t lo_rdevice;
    uint64_t lo_offset;
    uint64_t lo_sizelimit;
    uint32_t lo_number;
    uint32_t lo_encrypt_type;
    uint32_t lo_encrypt_key_size;
    uint32_t lo_flags;
    char     lo_file_name[64];
    char     lo_crypt_name[64];
    char     lo_encrypt_key[32];
    uint64_t lo_init[2];
} loop_info64_t;

typedef struct {
    int         in_use;
    vfile_t    *backing_vf;
    uint64_t    backing_size;
    spinlock_t  lock;
} loop_dev_t;

static loop_dev_t g_loop[MAX_LOOP_DEVS];

void loop_init(void) {
    for (int i = 0; i < MAX_LOOP_DEVS; i++) {
        memset(&g_loop[i], 0, sizeof(g_loop[i]));
        spin_init(&g_loop[i].lock);
        g_loop[i].backing_vf = NULL;
    }
}

static int loop_set_fd(int loop_idx, int user_fd) {
    if (loop_idx < 0 || loop_idx >= MAX_LOOP_DEVS) return -EINVAL;

    int64_t gfd = fdtable_get_current(user_fd);
    if (gfd < 0) return -EBADF;

    vfile_t *vf = vfs_get_file_ref((int)gfd);
    if (!vf) return -EBADF;

    long sz = 0;
    if (vf->ops && vf->ops->lseek) {
        long saved = vf->ops->lseek(vf, 0, SEEK_CUR);
        sz = vf->ops->lseek(vf, 0, SEEK_END);
        vf->ops->lseek(vf, saved, SEEK_SET);
    }
    vfs_put_file_ref((int)gfd, vf);

    if (sz <= 0) {
        return -EINVAL;
    }

    uint64_t flags = spin_lock_irqsave(&g_loop[loop_idx].lock);
    if (g_loop[loop_idx].in_use) {
        spin_unlock_irqrestore(&g_loop[loop_idx].lock, flags);
        return -EBUSY;
    }
    refcount_inc(&vf->ref_count);
    g_loop[loop_idx].backing_vf = vf;
    g_loop[loop_idx].backing_size = (uint64_t)sz;
    g_loop[loop_idx].in_use = 1;
    spin_unlock_irqrestore(&g_loop[loop_idx].lock, flags);
    return 0;
}

static int loop_clr_fd(int loop_idx) {
    if (loop_idx < 0 || loop_idx >= MAX_LOOP_DEVS) return -EINVAL;
    uint64_t flags = spin_lock_irqsave(&g_loop[loop_idx].lock);
    if (!g_loop[loop_idx].in_use) {
        spin_unlock_irqrestore(&g_loop[loop_idx].lock, flags);
        return -EINVAL;
    }
    vfile_t *vf = g_loop[loop_idx].backing_vf;
    g_loop[loop_idx].in_use = 0;
    g_loop[loop_idx].backing_size = 0;
    g_loop[loop_idx].backing_vf = NULL;
    spin_unlock_irqrestore(&g_loop[loop_idx].lock, flags);

    if (vf && refcount_dec_and_test(&vf->ref_count)) {
        if (vf->ops && vf->ops->close)
            vf->ops->close(vf);
        vfile_free(vf);
    }
    return 0;
}

static int loop_get_status64(int loop_idx, void *arg) {
    if (loop_idx < 0 || loop_idx >= MAX_LOOP_DEVS) return -EINVAL;
    loop_info64_t info;
    memset(&info, 0, sizeof(info));
    uint64_t flags = spin_lock_irqsave(&g_loop[loop_idx].lock);
    info.lo_number = (uint32_t)loop_idx;
    if (g_loop[loop_idx].in_use) {
        info.lo_sizelimit = g_loop[loop_idx].backing_size;
    }
    spin_unlock_irqrestore(&g_loop[loop_idx].lock, flags);
    if (copy_to_user(arg, &info, sizeof(info)) < 0)
        return -EFAULT;
    return 0;
}

static int loop_read_locked(vfile_t *vf, char *buf, size_t count, size_t offset) {
    if (!vf->ops || !vf->ops->lseek || !vf->ops->read)
        return -EIO;
    long saved = vf->ops->lseek(vf, 0, SEEK_CUR);
    vf->ops->lseek(vf, (long)offset, SEEK_SET);
    int r = vf->ops->read(vf, buf, count);
    vf->ops->lseek(vf, saved, SEEK_SET);
    return r;
}

static int loop_write_locked(vfile_t *vf, const char *buf, size_t count, size_t offset) {
    if (!vf->ops || !vf->ops->lseek || !vf->ops->write)
        return -EIO;
    long saved = vf->ops->lseek(vf, 0, SEEK_CUR);
    vf->ops->lseek(vf, (long)offset, SEEK_SET);
    int r = vf->ops->write(vf, buf, count);
    vf->ops->lseek(vf, saved, SEEK_SET);
    return r;
}

int loop_dev_read(int loop_idx, char *buf, size_t count, size_t offset) {
    if (loop_idx < 0 || loop_idx >= MAX_LOOP_DEVS) return -EINVAL;
    uint64_t flags = spin_lock_irqsave(&g_loop[loop_idx].lock);
    if (!g_loop[loop_idx].in_use || !g_loop[loop_idx].backing_vf) {
        spin_unlock_irqrestore(&g_loop[loop_idx].lock, flags);
        return -ENXIO;
    }
    vfile_t *vf = g_loop[loop_idx].backing_vf;
    uint64_t bsz = g_loop[loop_idx].backing_size;
    spin_unlock_irqrestore(&g_loop[loop_idx].lock, flags);

    if (offset >= bsz) return 0;
    if (offset + count > bsz) count = (size_t)(bsz - offset);
    return loop_read_locked(vf, buf, count, offset);
}

int loop_dev_write(int loop_idx, const char *buf, size_t count, size_t offset) {
    if (loop_idx < 0 || loop_idx >= MAX_LOOP_DEVS) return -EINVAL;
    uint64_t flags = spin_lock_irqsave(&g_loop[loop_idx].lock);
    if (!g_loop[loop_idx].in_use || !g_loop[loop_idx].backing_vf) {
        spin_unlock_irqrestore(&g_loop[loop_idx].lock, flags);
        return -ENXIO;
    }
    vfile_t *vf = g_loop[loop_idx].backing_vf;
    uint64_t bsz = g_loop[loop_idx].backing_size;
    spin_unlock_irqrestore(&g_loop[loop_idx].lock, flags);

    if (offset >= bsz) return -ENOSPC;
    if (offset + count > bsz) count = (size_t)(bsz - offset);
    return loop_write_locked(vf, buf, count, offset);
}

int loop_dev_ioctl(vfile_t *vf, unsigned long req, void *arg) {
    int idx = (int)(intptr_t)vf->priv - 100;
    if (idx < 0 || idx >= MAX_LOOP_DEVS) return -EINVAL;

    if (req == LOOP_SET_FD) {
        int user_fd;
        if (copy_from_user(&user_fd, arg, sizeof(user_fd)) < 0) return -EFAULT;
        return loop_set_fd(idx, user_fd);
    }
    if (req == LOOP_CLR_FD) {
        return loop_clr_fd(idx);
    }
    if (req == LOOP_GET_STATUS64) {
        return loop_get_status64(idx, arg);
    }
    if (req == LOOP_SET_STATUS64) {
        return 0;
    }
    if (req == BLKGETSIZE64) {
        uint64_t flags = spin_lock_irqsave(&g_loop[idx].lock);
        uint64_t sz = g_loop[idx].in_use ? g_loop[idx].backing_size : 0;
        spin_unlock_irqrestore(&g_loop[idx].lock, flags);
        if (copy_to_user(arg, &sz, sizeof(sz)) < 0) return -EFAULT;
        return 0;
    }
    if (req == BLKGETSIZE) {
        uint64_t flags = spin_lock_irqsave(&g_loop[idx].lock);
        uint64_t sz = g_loop[idx].in_use ? (g_loop[idx].backing_size / LOOP_SECTOR_SIZE) : 0;
        spin_unlock_irqrestore(&g_loop[idx].lock, flags);
        uint32_t sectors = (uint32_t)sz;
        if (copy_to_user(arg, &sectors, sizeof(sectors)) < 0) return -EFAULT;
        return 0;
    }
    if (req == BLKSSZGET) {
        int ssz = LOOP_SECTOR_SIZE;
        if (copy_to_user(arg, &ssz, sizeof(ssz)) < 0) return -EFAULT;
        return 0;
    }
    return -ENOTTY;
}

int loop_dev_count(void) {
    return MAX_LOOP_DEVS;
}
