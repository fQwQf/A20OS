#include "fs/sync_file.h"

#include "core/errno.h"
#include "core/poll.h"
#include "core/string.h"
#include "core/timekeeping.h"
#include "fs/anonfd.h"
#include "fs/fdtable.h"
#include "fs/file.h"
#include "fs/vfs.h"
#include "mm/slab.h"
#include "sys/usercopy.h"

/* Completed fences for synchronous device backends. No active fence is
 * manufactured or signalled before the device has finished its operation. */
typedef struct {
    char name[32];
    uint64_t timestamp_ns;
} completed_fence_t;

struct sync_merge_data {
    char name[32];
    int32_t fd2, fence;
    uint32_t flags, pad;
};
struct sync_fence_info {
    char obj_name[32], driver_name[32];
    int32_t status;
    uint32_t flags;
    uint64_t timestamp_ns;
};
struct sync_file_info {
    char name[32];
    int32_t status;
    uint32_t flags, num_fences, pad;
    uint64_t sync_fence_info;
};
struct sync_set_deadline {
    uint64_t deadline_ns, pad;
};

static int completed_poll(vfile_t *vf, short events)
{
    (void)vf;
    return events & POLLIN;
}

static int completed_ioctl(vfile_t *vf, unsigned long req, void *arg)
{
    completed_fence_t *f = vf->priv;
    if (req == 0xc0383e04UL) { /* SYNC_IOC_FILE_INFO */
        struct sync_file_info info;
        if (copy_from_user(&info, arg, sizeof(info)) < 0)
            return -EFAULT;
        if (info.flags || info.pad)
            return -EINVAL;
        if (info.num_fences) {
            struct sync_fence_info detail = {0};
            memcpy(detail.obj_name, f->name, sizeof(detail.obj_name));
            strcpy(detail.driver_name, "a20-sync");
            detail.status = 1;
            detail.timestamp_ns = f->timestamp_ns;
            if (copy_to_user((void *)(uintptr_t)info.sync_fence_info,
                             &detail, sizeof(detail)) < 0)
                return -EFAULT;
        }
        memcpy(info.name, f->name, sizeof(info.name));
        info.status = 1;
        info.num_fences = 1;
        return copy_to_user(arg, &info, sizeof(info)) < 0 ? -EFAULT : 0;
    }
    if (req == 0xc0303e03UL) { /* SYNC_IOC_MERGE */
        struct sync_merge_data data;
        if (copy_from_user(&data, arg, sizeof(data)) < 0)
            return -EFAULT;
        if (data.flags || data.pad)
            return -EINVAL;
        int rc = sync_file_wait_completed(data.fd2);
        if (rc < 0)
            return rc;
        data.name[31] = '\0';
        data.fence = sync_file_create_completed(data.name);
        if (data.fence < 0)
            return data.fence;
        if (copy_to_user(arg, &data, sizeof(data)) < 0) {
            fdtable_close_current(data.fence);
            return -EFAULT;
        }
        return 0;
    }
    if (req == 0x40103e05UL) { /* SYNC_IOC_SET_DEADLINE */
        struct sync_set_deadline deadline;
        if (copy_from_user(&deadline, arg, sizeof(deadline)) < 0)
            return -EFAULT;
        /* A scheduling hint cannot change an already completed operation. */
        return deadline.pad ? -EINVAL : 0;
    }
    return -ENOTTY;
}

static vfile_ops_t completed_ops = {
    .poll = completed_poll,
    .ioctl = completed_ioctl,
    .close = anonfd_free_priv_close,
};

int sync_file_wait_completed(int fd)
{
    vfile_t *vf = fdtable_get_current_file_ref(fd);
    if (!vf)
        return -EBADF;
    int rc = vf->ops == &completed_ops ? 0 : -EINVAL;
    vfs_put_file(vf);
    return rc;
}

int sync_file_create_completed(const char *name)
{
    completed_fence_t *f = kcalloc(1, sizeof(*f));
    vfile_t *vf = vfile_alloc();
    if (!f || !vf) {
        kfree(f);
        if (vf)
            vfile_free(vf);
        return -ENOMEM;
    }
    strncpy(f->name, name, sizeof(f->name) - 1);
    uint64_t ts[2];
    timekeeping_get_monotonic(ts);
    f->timestamp_ns = ts[0] * 1000000000ULL + ts[1];
    vf->flags = O_RDONLY;
    vf->ops = &completed_ops;
    vf->priv = f;
    strcpy(vf->path, "anon_inode:[sync_file]");
    vfile_ref_init(vf, 1);
    return anonfd_install_vfile(vf, O_CLOEXEC);
}
